#include "replay/replay_buffer.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace fluxrec::replay::detail {

constexpr std::size_t kPacketsPerBlock = 256;

struct PacketBlock {
  std::array<PacketPtr, kPacketsPerBlock> packets{};
  std::array<std::uint64_t, kPacketsPerBlock> sequence{};
  std::size_t count{};
};

struct SharedBudget {
  std::atomic<std::size_t> snapshot_payload_bytes{0};
  std::atomic<std::size_t> live_snapshots{0};
};

struct SnapshotLease {
  std::shared_ptr<SharedBudget> budget;
  std::size_t bytes{};

  ~SnapshotLease() {
    if (budget && bytes != 0) {
      budget->snapshot_payload_bytes.fetch_sub(bytes, std::memory_order_relaxed);
      budget->live_snapshots.fetch_sub(1, std::memory_order_relaxed);
    }
  }
};

}  // namespace fluxrec::replay::detail

namespace fluxrec::replay {
namespace {

using TimestampIndex = std::multimap<std::int64_t, std::uint64_t>;
using AudioKey = std::pair<std::int64_t, std::uint64_t>;
using AudioIndex = std::map<AudioKey, AudioPacketPtr>;

constexpr std::size_t kAudioStreamCount = 2;

struct AudioStreamState {
  bool configured{};
  media::AudioCodec codec{};
  media::Rational time_base{};
};

struct Segment {
  std::shared_ptr<detail::PacketBlock> block;
  std::size_t begin{};
  std::size_t end{};
};

struct PacketLocation {
  std::shared_ptr<detail::PacketBlock> block;
  std::size_t slot{};
  TimestampIndex::iterator timestamp;
  TimestampIndex::iterator keyframe;
  bool has_keyframe_index{};
};

std::int64_t to_nanoseconds(const Packet& packet, bool& valid) {
  valid = packet.time_base.num > 0 && packet.time_base.den > 0 &&
          packet.pts >= 0 &&
          packet.dts != std::numeric_limits<std::int64_t>::min();
  if (!valid) {
    return 0;
  }

  // Packet PTS values and time bases are signed 64-bit / signed 32-bit. This
  // intermediate has enough range for pts * num * 1e9 before division.
  __int128 value = static_cast<__int128>(packet.pts) * packet.time_base.num;
  value *= 1'000'000'000;
  value /= packet.time_base.den;
  if (value < std::numeric_limits<std::int64_t>::min() ||
      value > std::numeric_limits<std::int64_t>::max()) {
    valid = false;
    return 0;
  }
  return static_cast<std::int64_t>(value);
}

bool audio_clock_matches(const AudioPacket& packet) {
  if (!packet.time_base.valid() || packet.pts < 0 || packet.capture_time_ns == 0 ||
      packet.capture_time_ns > static_cast<std::uint64_t>(
          std::numeric_limits<std::int64_t>::max())) {
    return false;
  }
  const __int128 numerator = static_cast<__int128>(packet.pts) *
      packet.time_base.num * 1'000'000'000;
  const __int128 pts_ns = (numerator + packet.time_base.den / 2) /
                          packet.time_base.den;
  const __int128 capture_ns = packet.capture_time_ns;
  const __int128 difference = pts_ns > capture_ns ? pts_ns - capture_ns
                                                  : capture_ns - pts_ns;
  const __int128 tick_ns =
      (static_cast<__int128>(packet.time_base.num) * 1'000'000'000 +
       packet.time_base.den - 1) / packet.time_base.den;
  return difference <= tick_ns + 1;
}

std::int64_t subtract_duration_saturated(std::int64_t timestamp, Nanoseconds duration) {
  const auto amount = duration.count();
  if (amount > 0 && timestamp < std::numeric_limits<std::int64_t>::min() + amount) {
    return std::numeric_limits<std::int64_t>::min();
  }
  return timestamp - amount;
}

}  // namespace

ReplayBufferConfig make_duration_aware_replay_buffer_config(
    std::chrono::nanoseconds history_duration, std::uint64_t target_bitrate_bps) {
  if (history_duration <= Nanoseconds::zero()) {
    throw std::invalid_argument("replay history duration must be positive");
  }

  constexpr std::uint64_t minimum_estimated_bitrate_bps = 120'000'000;
  constexpr std::uint64_t maximum_requested_bitrate_bps = 500'000'000;
  constexpr __int128 bits_per_byte_nanos = 8'000'000'000;

  ReplayBufferConfig config;
  config.history_duration = history_duration;
  const auto bounded_target_bps = std::min(target_bitrate_bps,
                                           maximum_requested_bitrate_bps);
  const auto estimated_bitrate_bps = std::max(
      minimum_estimated_bitrate_bps, bounded_target_bps * 2);
  const __int128 retained_duration_ns =
      static_cast<__int128>(history_duration.count()) +
      config.max_keyframe_preroll.count();
  const __int128 estimated_bytes =
      (retained_duration_ns * estimated_bitrate_bps + bits_per_byte_nanos - 1) /
      bits_per_byte_nanos;
  const __int128 bounded_bytes = std::min(
      static_cast<__int128>(ReplayBufferConfig::maximum_payload_budget_bytes),
      std::max(static_cast<__int128>(ReplayBufferConfig::minimum_payload_budget_bytes),
               estimated_bytes));
  config.max_bytes = static_cast<std::size_t>(bounded_bytes);
  config.max_snapshot_bytes = config.max_bytes;
  return config;
}

struct ReplayBuffer::Impl {
  Impl() : shared_budget(std::make_shared<detail::SharedBudget>()) {}

  mutable std::mutex mutex;
  std::vector<Segment> segments;
  TimestampIndex timestamps;
  TimestampIndex keyframes;
  AudioIndex audio_packets;
  std::unordered_map<std::uint64_t, PacketLocation> locations;
  std::shared_ptr<detail::SharedBudget> shared_budget;
  std::size_t payload_bytes{};
  std::size_t packet_count{};
  std::size_t video_packet_count{};
  std::size_t audio_packet_count{};
  std::uint64_t next_sequence{};
  std::uint64_t appended_packets{};
  std::uint64_t evicted_packets{};
  std::uint64_t rejected_packets{};
  std::atomic<std::uint64_t> inactive_packets{};
  std::uint64_t audio_appended_packets{};
  std::uint64_t audio_evicted_packets{};
  std::uint64_t audio_rejected_packets{};
  std::atomic<std::uint64_t> inactive_audio_packets{};
  std::int64_t latest_pts_ns{};
  std::atomic<std::uint64_t> generation{};
  std::atomic<bool> active{true};
  bool has_timestamps{};
  bool has_stream{};
  media::VideoCodec codec{};
  media::Rational time_base{};
  std::array<AudioStreamState, kAudioStreamCount> audio_streams{};

  void erase_location(std::uint64_t sequence) {
    const auto found = locations.find(sequence);
    if (found == locations.end()) {
      return;
    }
    timestamps.erase(found->second.timestamp);
    if (found->second.has_keyframe_index) {
      keyframes.erase(found->second.keyframe);
    }
    locations.erase(found);
  }

  void remove_front_through(std::uint64_t last_sequence) {
    while (!segments.empty()) {
      auto& segment = segments.front();
      while (segment.begin < segment.end &&
             segment.block->sequence[segment.begin] <= last_sequence) {
        const auto sequence = segment.block->sequence[segment.begin];
        const auto packet_size = segment.block->packets[segment.begin]->data.size();
        erase_location(sequence);
        payload_bytes -= packet_size;
        --packet_count;
        --video_packet_count;
        ++evicted_packets;
        // Blocks are shared by the ring, but snapshots hold direct packet
        // references. Clearing this slot releases evicted payloads even when
        // the rest of the block remains active in the ring.
        segment.block->packets[segment.begin].reset();
        ++segment.begin;
      }
      if (segment.begin == segment.end) {
        segments.erase(segments.begin());
      } else {
        break;
      }
    }
  }

  void remove_oldest_one() {
    if (segments.empty()) {
      return;
    }
    const auto& segment = segments.front();
    remove_front_through(segment.block->sequence[segment.begin]);
  }

  void remove_oldest_audio() {
    if (audio_packets.empty()) return;
    const auto first = audio_packets.begin();
    payload_bytes -= first->second->data.size();
    --packet_count;
    --audio_packet_count;
    ++evicted_packets;
    ++audio_evicted_packets;
    audio_packets.erase(first);
  }

  void make_room(std::size_t packet_bytes, std::size_t available_bytes,
                 std::size_t max_packets) {
    while (packet_count >= max_packets ||
           payload_bytes > available_bytes - packet_bytes) {
      // Preserve the video GOP index when possible; audio packets can be
      // evicted independently without making video snapshots undecodable.
      if (!audio_packets.empty()) {
        remove_oldest_audio();
      } else if (!segments.empty()) {
        remove_oldest_one();
      } else {
        break;
      }
    }
  }

  void prune_audio_before(std::int64_t cutoff) {
    while (!audio_packets.empty() && audio_packets.begin()->first.first < cutoff) {
      remove_oldest_audio();
    }
  }

  void prune_before(std::int64_t cutoff) {
    const auto expired_end = timestamps.lower_bound(cutoff);
    if (expired_end == timestamps.begin()) {
      prune_audio_before(cutoff);
      return;
    }
    auto last = timestamps.begin();
    for (auto it = timestamps.begin(); it != expired_end; ++it) {
      if (it->second > last->second) {
        last = it;
      }
    }
    remove_front_through(last->second);
    prune_audio_before(cutoff);
  }
};

void ReplaySnapshot::for_each_packet(
    const std::function<void(const PacketPtr&)>& visitor) const {
  if (!visitor || status_ != SnapshotStatus::ready) {
    return;
  }
  for (const auto& packet : video_packets_) visitor(packet);
}

void ReplaySnapshot::for_each_audio_packet(
    const std::function<void(const AudioPacketPtr&)>& visitor) const {
  if (!visitor || status_ != SnapshotStatus::ready) return;
  for (const auto& packet : audio_packets_) visitor(packet);
}

ReplayBuffer::ReplayBuffer(ReplayBufferConfig config) : config_(config) {
  if (config_.history_duration <= Nanoseconds::zero() ||
      config_.max_keyframe_preroll < Nanoseconds::zero() || config_.max_bytes == 0 ||
      config_.max_packets == 0 || config_.max_live_snapshots == 0 ||
      config_.max_snapshot_bytes == 0) {
    throw std::invalid_argument("replay buffer limits must be positive");
  }
  if (config_.max_keyframe_preroll.count() >
      std::numeric_limits<std::int64_t>::max() - config_.history_duration.count()) {
    throw std::invalid_argument("replay buffer duration limit overflows");
  }
  impl_ = std::make_unique<Impl>();
}

ReplayBuffer::~ReplayBuffer() = default;

AppendResult ReplayBuffer::append(Packet packet) {
  const auto generation = impl_->generation.load(std::memory_order_acquire);
  if (!impl_->active.load(std::memory_order_acquire)) {
    impl_->inactive_packets.fetch_add(1, std::memory_order_relaxed);
    return AppendResult::inactive;
  }
  return append_for_generation(
      std::make_shared<const Packet>(std::move(packet)), generation);
}

AppendResult ReplayBuffer::append(PacketPtr packet) {
  const auto generation = impl_->generation.load(std::memory_order_acquire);
  if (!impl_->active.load(std::memory_order_acquire)) {
    impl_->inactive_packets.fetch_add(1, std::memory_order_relaxed);
    return AppendResult::inactive;
  }
  return append_for_generation(std::move(packet), generation);
}

AppendResult ReplayBuffer::append_for_generation(PacketPtr packet,
                                                 std::uint64_t generation) {
  if (!impl_->active.load(std::memory_order_acquire) ||
      impl_->generation.load(std::memory_order_acquire) != generation) {
    impl_->inactive_packets.fetch_add(1, std::memory_order_relaxed);
    return AppendResult::inactive;
  }
  if (!packet || packet->data.empty()) {
    std::lock_guard lock(impl_->mutex);
    ++impl_->rejected_packets;
    return AppendResult::invalid_packet;
  }
  bool timestamp_valid = false;
  const auto timestamp_ns = to_nanoseconds(*packet, timestamp_valid);
  if (!timestamp_valid) {
    std::lock_guard lock(impl_->mutex);
    ++impl_->rejected_packets;
    return AppendResult::invalid_packet;
  }
  if (packet->data.size() > config_.max_bytes) {
    std::lock_guard lock(impl_->mutex);
    ++impl_->rejected_packets;
    return AppendResult::packet_too_large;
  }

  std::lock_guard lock(impl_->mutex);
  auto& state = *impl_;
  if (!state.active.load(std::memory_order_relaxed) ||
      state.generation.load(std::memory_order_relaxed) != generation) {
    state.inactive_packets.fetch_add(1, std::memory_order_relaxed);
    return AppendResult::inactive;
  }
  if (state.has_stream &&
      (packet->codec != state.codec || packet->time_base.num != state.time_base.num ||
       packet->time_base.den != state.time_base.den)) {
    ++state.rejected_packets;
    return AppendResult::incompatible_stream;
  }

  if (!state.has_stream) {
    state.has_stream = true;
    state.codec = packet->codec;
    state.time_base = packet->time_base;
    state.latest_pts_ns = timestamp_ns;
    state.has_timestamps = true;
  } else if (timestamp_ns > state.latest_pts_ns) {
    state.latest_pts_ns = timestamp_ns;
  }
  const auto storage_duration = config_.history_duration + config_.max_keyframe_preroll;
  const auto cutoff = subtract_duration_saturated(state.latest_pts_ns, storage_duration);
  if (timestamp_ns < cutoff) {
    ++state.rejected_packets;
    return AppendResult::outside_retention_window;
  }
  state.prune_before(cutoff);

  const auto available_bytes = config_.max_bytes;
  if (packet->data.size() > available_bytes) {
    ++state.rejected_packets;
    return AppendResult::snapshot_memory_budget;
  }
  state.make_room(packet->data.size(), available_bytes, config_.max_packets);
  if (state.packet_count >= config_.max_packets ||
      state.payload_bytes > available_bytes - packet->data.size()) {
    ++state.rejected_packets;
    return AppendResult::snapshot_memory_budget;
  }

  std::shared_ptr<detail::PacketBlock> block;
  if (state.segments.empty() ||
      state.segments.back().end == detail::kPacketsPerBlock) {
    block = std::make_shared<detail::PacketBlock>();
    state.segments.push_back(Segment{block, 0, 0});
  } else {
    block = state.segments.back().block;
  }

  const auto slot = block->count;
  const auto sequence = state.next_sequence++;
  const auto timestamp_it = state.timestamps.emplace(timestamp_ns, sequence);
  TimestampIndex::iterator keyframe_it{};
  if (packet->keyframe) {
    keyframe_it = state.keyframes.emplace(timestamp_ns, sequence);
  }
  block->packets[slot] = std::move(packet);
  block->sequence[slot] = sequence;
  ++block->count;
  state.segments.back().end = block->count;
  state.locations.emplace(sequence,
                          PacketLocation{block, slot, timestamp_it, keyframe_it,
                                         block->packets[slot]->keyframe});
  state.payload_bytes += block->packets[slot]->data.size();
  ++state.packet_count;
  ++state.video_packet_count;
  ++state.appended_packets;
  return AppendResult::stored;
}

AppendResult ReplayBuffer::append_audio(AudioPacket packet,
                                        Nanoseconds video_relative_time) {
  return append_audio(std::make_shared<const AudioPacket>(std::move(packet)),
                      video_relative_time);
}

AppendResult ReplayBuffer::append_audio(AudioPacketPtr packet,
                                        Nanoseconds video_relative_time) {
  const auto generation = impl_->generation.load(std::memory_order_acquire);
  if (!impl_->active.load(std::memory_order_acquire)) {
    impl_->inactive_packets.fetch_add(1, std::memory_order_relaxed);
    impl_->inactive_audio_packets.fetch_add(1, std::memory_order_relaxed);
    return AppendResult::inactive;
  }
  if (!packet || packet->stream_index >= kAudioStreamCount || packet->data.empty() ||
      packet->duration < 0 || !audio_clock_matches(*packet)) {
    std::lock_guard lock(impl_->mutex);
    ++impl_->rejected_packets;
    ++impl_->audio_rejected_packets;
    return AppendResult::invalid_packet;
  }
  if (packet->data.size() > config_.max_bytes) {
    std::lock_guard lock(impl_->mutex);
    ++impl_->rejected_packets;
    ++impl_->audio_rejected_packets;
    return AppendResult::packet_too_large;
  }

  std::lock_guard lock(impl_->mutex);
  auto& state = *impl_;
  if (!state.active.load(std::memory_order_relaxed) ||
      state.generation.load(std::memory_order_relaxed) != generation) {
    state.inactive_packets.fetch_add(1, std::memory_order_relaxed);
    state.inactive_audio_packets.fetch_add(1, std::memory_order_relaxed);
    return AppendResult::inactive;
  }

  auto& stream = state.audio_streams[packet->stream_index];
  if (stream.configured &&
      (packet->codec != stream.codec || packet->time_base.num != stream.time_base.num ||
       packet->time_base.den != stream.time_base.den)) {
    ++state.rejected_packets;
    ++state.audio_rejected_packets;
    return AppendResult::incompatible_stream;
  }
  if (!stream.configured) {
    stream.configured = true;
    stream.codec = packet->codec;
    stream.time_base = packet->time_base;
  }

  const auto timestamp_ns = video_relative_time.count();
  if (state.has_timestamps) {
    const auto storage_duration = config_.history_duration + config_.max_keyframe_preroll;
    const auto cutoff = subtract_duration_saturated(state.latest_pts_ns, storage_duration);
    if (timestamp_ns < cutoff) {
      ++state.rejected_packets;
      ++state.audio_rejected_packets;
      return AppendResult::outside_retention_window;
    }
    state.prune_audio_before(cutoff);
  }

  const auto available_bytes = config_.max_bytes;
  if (packet->data.size() > available_bytes) {
    ++state.rejected_packets;
    ++state.audio_rejected_packets;
    return AppendResult::snapshot_memory_budget;
  }
  state.make_room(packet->data.size(), available_bytes, config_.max_packets);
  if (state.packet_count >= config_.max_packets ||
      state.payload_bytes > available_bytes - packet->data.size()) {
    ++state.rejected_packets;
    ++state.audio_rejected_packets;
    return AppendResult::snapshot_memory_budget;
  }

  const auto sequence = state.next_sequence++;
  const auto payload_size = packet->data.size();
  state.audio_packets.emplace(AudioKey{timestamp_ns, sequence}, std::move(packet));
  state.payload_bytes += payload_size;
  ++state.packet_count;
  ++state.audio_packet_count;
  ++state.appended_packets;
  ++state.audio_appended_packets;
  return AppendResult::stored;
}

bool ReplayBuffer::start() {
  std::vector<Segment> old_segments;
  TimestampIndex old_timestamps;
  TimestampIndex old_keyframes;
  AudioIndex old_audio_packets;
  std::unordered_map<std::uint64_t, PacketLocation> old_locations;
  {
    std::lock_guard lock(impl_->mutex);
    auto& state = *impl_;
    if (state.active.load(std::memory_order_relaxed)) {
      return false;
    }

    // Swap old storage out under the lock, then release packet references after
    // unlocking. This keeps a fresh start from making the packet router wait
    // for a large batch of compressed-buffer deallocations.
    state.segments.swap(old_segments);
    state.timestamps.swap(old_timestamps);
    state.keyframes.swap(old_keyframes);
    state.audio_packets.swap(old_audio_packets);
    state.locations.swap(old_locations);
    state.packet_count = 0;
    state.video_packet_count = 0;
    state.audio_packet_count = 0;
    state.payload_bytes = 0;
    state.has_stream = false;
    state.has_timestamps = false;
    state.latest_pts_ns = 0;
    state.audio_streams = {};
    state.generation.fetch_add(1, std::memory_order_release);
    state.active.store(true, std::memory_order_release);
  }
  return true;
}

bool ReplayBuffer::stop() {
  std::lock_guard lock(impl_->mutex);
  auto& state = *impl_;
  if (!state.active.load(std::memory_order_relaxed)) {
    return false;
  }
  state.generation.fetch_add(1, std::memory_order_release);
  state.active.store(false, std::memory_order_release);
  return true;
}

bool ReplayBuffer::active() const noexcept {
  return impl_->active.load(std::memory_order_acquire);
}

ReplaySnapshot ReplayBuffer::snapshot(Nanoseconds requested_duration) const {
  ReplaySnapshot result;
  result.requested_duration_ = requested_duration;
  if (requested_duration <= Nanoseconds::zero() || requested_duration > config_.history_duration) {
    result.status_ = SnapshotStatus::invalid_duration;
    return result;
  }

  std::lock_guard lock(impl_->mutex);
  const auto& state = *impl_;
  if (!state.has_timestamps || state.video_packet_count == 0 ||
      state.timestamps.empty()) {
    result.status_ = SnapshotStatus::no_keyframe;
    return result;
  }

  const auto earliest_retained_timestamp = state.timestamps.begin()->first;
  if (static_cast<__int128>(state.latest_pts_ns) - earliest_retained_timestamp <
      requested_duration.count()) {
    result.status_ = SnapshotStatus::insufficient_history;
    return result;
  }

  const auto target_start = subtract_duration_saturated(state.latest_pts_ns, requested_duration);
  auto keyframe = state.keyframes.upper_bound(target_start);
  if (keyframe == state.keyframes.begin()) {
    result.status_ = state.keyframes.empty()
        ? SnapshotStatus::no_keyframe : SnapshotStatus::insufficient_history;
    return result;
  }
  --keyframe;
  const auto preroll_count = static_cast<__int128>(target_start) - keyframe->first;
  if (preroll_count < 0 || preroll_count > config_.max_keyframe_preroll.count()) {
    result.status_ = SnapshotStatus::keyframe_preroll_exceeded;
    return result;
  }

  const auto location = state.locations.find(keyframe->second);
  if (location == state.locations.end()) {
    result.status_ = SnapshotStatus::no_keyframe;
    return result;
  }

  std::size_t selected_count = 0;
  bool after_start = false;
  for (const auto& segment : state.segments) {
    const auto begin = after_start ? segment.begin : std::max(segment.begin, location->second.slot);
    if (!after_start && segment.block.get() != location->second.block.get()) {
      continue;
    }
    after_start = true;
    for (auto slot = begin; slot < segment.end; ++slot) {
      const auto& packet = segment.block->packets[slot];
      if (!packet) {
        result.video_packets_.clear();
        result.status_ = SnapshotStatus::no_keyframe;
        return result;
      }
      result.video_packets_.push_back(packet);
      result.payload_bytes_ += packet->data.size();
      ++selected_count;
    }
  }
  if (result.video_packets_.empty() || selected_count == 0) {
    result.status_ = SnapshotStatus::no_keyframe;
    return result;
  }

  const auto clip_start_ns = keyframe->first;
  const auto clip_end_ns = state.latest_pts_ns;
  for (const auto& [audio_key, audio_packet] : state.audio_packets) {
    const auto audio_time_ns = audio_key.first;
    if (audio_time_ns > clip_end_ns) break;
    // Compressed audio packets cannot be trimmed at a keyframe boundary. Start
    // with the first packet on or after the selected video keyframe so exported
    // audio has no negative DTS relative to its adjusted video origin.
    if (audio_time_ns < clip_start_ns) continue;
    result.audio_packets_.push_back(audio_packet);
    result.payload_bytes_ += audio_packet->data.size();
  }

  if (state.shared_budget->live_snapshots.load(std::memory_order_relaxed) >=
      config_.max_live_snapshots) {
    result.video_packets_.clear();
    result.audio_packets_.clear();
    result.status_ = SnapshotStatus::snapshot_capacity;
    return result;
  }
  const auto pinned_bytes = state.shared_budget->snapshot_payload_bytes.load(std::memory_order_relaxed);
  if (pinned_bytes > config_.max_snapshot_bytes ||
      result.payload_bytes_ > config_.max_snapshot_bytes - pinned_bytes) {
    result.video_packets_.clear();
    result.audio_packets_.clear();
    result.status_ = SnapshotStatus::snapshot_memory_budget;
    return result;
  }

  auto lease = std::make_shared<detail::SnapshotLease>();
  lease->budget = state.shared_budget;
  lease->bytes = result.payload_bytes_;
  state.shared_budget->snapshot_payload_bytes.fetch_add(result.payload_bytes_, std::memory_order_relaxed);
  state.shared_budget->live_snapshots.fetch_add(1, std::memory_order_relaxed);
  result.lease_ = std::move(lease);
  result.packet_count_ = selected_count;
  result.start_time_ = Nanoseconds{keyframe->first};
  result.end_time_ = Nanoseconds{state.latest_pts_ns};
  result.keyframe_preroll_ = Nanoseconds{static_cast<std::int64_t>(preroll_count)};
  result.status_ = SnapshotStatus::ready;
  return result;
}

ReplaySnapshot ReplayBuffer::snapshot() const {
  return snapshot(config_.history_duration);
}

ReplayBufferStats ReplayBuffer::stats() const {
  std::lock_guard lock(impl_->mutex);
  const auto& state = *impl_;
  ReplayBufferStats result;
  result.active = state.active.load(std::memory_order_acquire);
  result.packet_count = state.packet_count;
  result.video_packet_count = state.video_packet_count;
  result.audio_packet_count = state.audio_packet_count;
  result.payload_bytes = state.payload_bytes;
  result.snapshot_payload_bytes =
      state.shared_budget->snapshot_payload_bytes.load(std::memory_order_relaxed);
  result.live_snapshots = state.shared_budget->live_snapshots.load(std::memory_order_relaxed);
  result.appended_packets = state.appended_packets;
  result.evicted_packets = state.evicted_packets;
  result.rejected_packets = state.rejected_packets;
  result.inactive_packets = state.inactive_packets.load(std::memory_order_relaxed);
  result.audio_appended_packets = state.audio_appended_packets;
  result.audio_evicted_packets = state.audio_evicted_packets;
  result.audio_rejected_packets = state.audio_rejected_packets;
  result.inactive_audio_packets =
      state.inactive_audio_packets.load(std::memory_order_relaxed);
  if (state.timestamps.size() > 1) {
    const auto duration = static_cast<__int128>(state.timestamps.rbegin()->first) -
                          state.timestamps.begin()->first;
    result.retained_duration = Nanoseconds{static_cast<std::int64_t>(std::min<__int128>(
        duration, std::numeric_limits<std::int64_t>::max()))};
  }
  return result;
}

}  // namespace fluxrec::replay
