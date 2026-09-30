#include "replay/replay_buffer.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;
using fluxrec::media::AudioCodec;
using fluxrec::media::EncodedAudioPacket;
using fluxrec::media::EncodedPacket;
using fluxrec::replay::AppendResult;
using fluxrec::replay::ReplayBuffer;
using fluxrec::replay::ReplayBufferConfig;
using fluxrec::replay::SnapshotStatus;

namespace {

void check(bool condition, const char* expression, int line) {
  if (!condition) {
    throw std::runtime_error("check failed at line " + std::to_string(line) + ": " + expression);
  }
}

#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

EncodedPacket packet(std::int64_t pts, bool keyframe, std::size_t bytes = 1,
                     fluxrec::media::Rational time_base = {1, 1000}) {
  EncodedPacket result;
  result.data.assign(bytes, static_cast<std::uint8_t>(pts));
  result.pts = pts;
  result.dts = pts;
  result.duration = 1;
  result.keyframe = keyframe;
  result.time_base = time_base;
  return result;
}

EncodedAudioPacket audio_packet(std::size_t stream_index,
                                std::uint64_t capture_time_ns,
                                std::size_t bytes = 2) {
  EncodedAudioPacket result;
  result.stream_index = stream_index;
  result.data.assign(bytes, static_cast<std::uint8_t>(stream_index + 1));
  result.codec = AudioCodec::aac;
  result.time_base = {1, 48'000};
  result.pts = static_cast<std::int64_t>(
      (static_cast<__int128>(capture_time_ns) * 48'000 + 500'000'000) /
      1'000'000'000);
  result.duration = 960;
  result.capture_time_ns = capture_time_ns;
  result.keyframe = true;
  return result;
}

void test_duration_eviction_and_preceding_keyframe() {
  ReplayBuffer buffer({3s, 1s, 1024});
  for (std::int64_t pts = 0; pts <= 6000; pts += 1000) {
    const bool keyframe = pts % 2000 == 0;
    CHECK(buffer.append(packet(pts, keyframe)) == AppendResult::stored);
  }

  const auto stats = buffer.stats();
  CHECK(stats.packet_count == 5);
  CHECK(stats.retained_duration == 4s);
  CHECK(stats.evicted_packets == 2);

  const auto snapshot = buffer.snapshot(3s);
  CHECK(snapshot.status() == SnapshotStatus::ready);
  CHECK(snapshot.packet_count() == 5);
  CHECK(snapshot.start_time() == 2s);
  CHECK(snapshot.end_time() == 6s);
  CHECK(snapshot.keyframe_preroll() == 1s);

  std::vector<std::int64_t> pts;
  std::vector<bool> keyframes;
  snapshot.for_each_packet([&](const auto& p) {
    pts.push_back(p->pts);
    keyframes.push_back(p->keyframe);
  });
  CHECK((pts == std::vector<std::int64_t>{2000, 3000, 4000, 5000, 6000}));
  CHECK(keyframes.front());
}

void test_runtime_start_stop_freezes_and_resets_ring() {
  ReplayBuffer buffer({2s, 1s, 1024});
  CHECK(buffer.active());
  CHECK(buffer.append(packet(0, true)) == AppendResult::stored);
  CHECK(buffer.append(packet(1000, true)) == AppendResult::stored);
  CHECK(buffer.append(packet(2000, false)) == AppendResult::stored);

  auto before_stop = buffer.snapshot(1s);
  CHECK(before_stop.status() == SnapshotStatus::ready);
  CHECK(buffer.stop());
  CHECK(!buffer.active());
  CHECK(!buffer.stop());
  CHECK(buffer.append(packet(3000, false)) == AppendResult::inactive);
  CHECK(buffer.stats().inactive_packets == 1);
  CHECK(buffer.stats().packet_count == 3);

  auto frozen = buffer.snapshot(1s);
  CHECK(frozen.status() == SnapshotStatus::ready);
  std::vector<std::int64_t> frozen_pts;
  frozen.for_each_packet([&](const auto& p) { frozen_pts.push_back(p->pts); });
  CHECK((frozen_pts == std::vector<std::int64_t>{1000, 2000}));

  CHECK(buffer.start());
  CHECK(buffer.active());
  CHECK(!buffer.start());
  CHECK(buffer.stats().packet_count == 0);
  CHECK(buffer.append(packet(0, true)) == AppendResult::stored);
  CHECK(buffer.snapshot(1s).status() == SnapshotStatus::insufficient_history);

  std::vector<std::int64_t> old_snapshot_pts;
  before_stop.for_each_packet([&](const auto& p) { old_snapshot_pts.push_back(p->pts); });
  CHECK((old_snapshot_pts == std::vector<std::int64_t>{1000, 2000}));
  CHECK(buffer.stats().live_snapshots == 2);
}

void test_byte_eviction_and_missing_keyframe_status() {
  ReplayBuffer buffer({10s, 2s, 10});
  for (std::int64_t pts = 0; pts < 5; ++pts) {
    CHECK(buffer.append(packet(pts * 1000, pts == 0, 3)) == AppendResult::stored);
  }

  auto stats = buffer.stats();
  CHECK(stats.payload_bytes == 9);
  CHECK(stats.packet_count == 3);
  CHECK(stats.evicted_packets == 2);
  CHECK(buffer.snapshot(1s).status() == SnapshotStatus::no_keyframe);

  CHECK(buffer.append(packet(5000, false, 11)) == AppendResult::packet_too_large);
  CHECK(buffer.stats().payload_bytes == 9);
}

void test_packet_count_bound() {
  ReplayBuffer buffer({10s, 2s, 100, 2, 2});
  CHECK(buffer.append(packet(0, true, 1)) == AppendResult::stored);
  CHECK(buffer.append(packet(1000, false, 1)) == AppendResult::stored);
  CHECK(buffer.append(packet(2000, false, 1)) == AppendResult::stored);
  const auto stats = buffer.stats();
  CHECK(stats.packet_count == 2);
  CHECK(stats.evicted_packets == 1);
}

void test_snapshot_budget_tracks_async_lifetime() {
  ReplayBuffer buffer({2s, 1s, 30, 65'536, 4, 20});
  CHECK(buffer.append(packet(0, true, 3)) == AppendResult::stored);
  CHECK(buffer.append(packet(1000, false, 3)) == AppendResult::stored);
  CHECK(buffer.append(packet(2000, true, 3)) == AppendResult::stored);
  CHECK(buffer.append(packet(3000, false, 3)) == AppendResult::stored);

  auto snapshot = buffer.snapshot(2s);
  CHECK(snapshot.status() == SnapshotStatus::ready);
  CHECK(snapshot.payload_bytes() == 12);
  CHECK(buffer.stats().snapshot_payload_bytes == 12);
  CHECK(buffer.snapshot(2s).status() == SnapshotStatus::snapshot_memory_budget);

  snapshot = {};
  CHECK(buffer.stats().snapshot_payload_bytes == 0);
  CHECK(buffer.snapshot(2s).status() == SnapshotStatus::ready);
}

void test_snapshot_shares_ring_payload_budget_without_shrinking_history() {
  // The full ring has 11 bytes while the selected snapshot holds 9 of those
  // same bytes. Their sum exceeds either individual budget, but no payload is
  // copied, so the export must be accepted and the ring must keep appending.
  ReplayBuffer buffer({8s, 2s, 11, 65'536, 4, 11});
  for (std::int64_t pts = 0; pts <= 10'000; pts += 1000) {
    CHECK(buffer.append(packet(pts, pts % 2000 == 0)) == AppendResult::stored);
  }

  auto snapshot = buffer.snapshot(8s);
  CHECK(snapshot.status() == SnapshotStatus::ready);
  CHECK(snapshot.packet_count() == 9);
  CHECK(snapshot.payload_bytes() == 9);
  CHECK(buffer.stats().payload_bytes == 11);
  CHECK(buffer.stats().snapshot_payload_bytes == 9);

  // A live save does not consume the retained-history budget or cause replay
  // packets to be rejected as capture continues.
  CHECK(buffer.append(packet(11'000, false)) == AppendResult::stored);
  CHECK(buffer.append(packet(12'000, true)) == AppendResult::stored);
  CHECK(buffer.stats().payload_bytes == 11);
  CHECK(buffer.stats().rejected_packets == 0);
  std::vector<std::int64_t> retained_snapshot_pts;
  snapshot.for_each_packet([&](const auto& p) { retained_snapshot_pts.push_back(p->pts); });
  CHECK((retained_snapshot_pts == std::vector<std::int64_t>{2000, 3000, 4000, 5000,
      6000, 7000, 8000, 9000, 10'000}));

  CHECK(buffer.snapshot(8s).status() == SnapshotStatus::snapshot_memory_budget);
  snapshot = {};
  CHECK(buffer.stats().snapshot_payload_bytes == 0);
  CHECK(buffer.snapshot(8s).status() == SnapshotStatus::ready);
}

void test_byte_limited_ring_reports_insufficient_history() {
  ReplayBuffer buffer({8s, 2s, 5, 65'536, 4, 5});
  for (std::int64_t pts = 0; pts <= 10'000; pts += 1000) {
    CHECK(buffer.append(packet(pts, pts % 2000 == 0)) == AppendResult::stored);
  }

  const auto stats = buffer.stats();
  CHECK(stats.video_packet_count == 5);
  CHECK(stats.retained_duration == 4s);
  // The session has run for ten seconds, but byte eviction retained only four.
  // Report the actual retained window instead of searching an impossible old GOP.
  CHECK(buffer.snapshot(8s).status() == SnapshotStatus::insufficient_history);
}

void test_duration_aware_budgets_cover_presets_and_clamp_custom_history() {
  using fluxrec::replay::make_duration_aware_replay_buffer_config;
  constexpr auto minimum = ReplayBufferConfig::minimum_payload_budget_bytes;
  constexpr auto maximum = ReplayBufferConfig::maximum_payload_budget_bytes;

  const auto fifteen = make_duration_aware_replay_buffer_config(15s, 50'000'000);
  const auto thirty = make_duration_aware_replay_buffer_config(30s, 50'000'000);
  const auto sixty = make_duration_aware_replay_buffer_config(60s, 50'000'000);
  const auto one_twenty = make_duration_aware_replay_buffer_config(120s, 50'000'000);
  CHECK(fifteen.max_bytes == minimum);
  CHECK(thirty.max_bytes == minimum);
  CHECK(sixty.max_bytes == 930'000'000);
  CHECK(one_twenty.max_bytes == 1'830'000'000);
  CHECK(fifteen.max_snapshot_bytes == fifteen.max_bytes);
  CHECK(thirty.max_snapshot_bytes == thirty.max_bytes);
  CHECK(sixty.max_snapshot_bytes == sixty.max_bytes);
  CHECK(one_twenty.max_snapshot_bytes == one_twenty.max_bytes);

  const auto custom = make_duration_aware_replay_buffer_config(3600s, 50'000'000);
  CHECK(custom.max_bytes == maximum);
  CHECK(custom.max_snapshot_bytes == maximum);
  CHECK(custom.max_bytes + custom.max_snapshot_bytes == 4ULL * 1024ULL * 1024ULL * 1024ULL);
}

void test_live_snapshot_count_bound_and_copy_lifetime() {
  ReplayBuffer buffer({2s, 1s, 1024, 4096, 1});
  CHECK(buffer.append(packet(0, true)) == AppendResult::stored);
  CHECK(buffer.append(packet(1000, true)) == AppendResult::stored);
  CHECK(buffer.append(packet(2000, false)) == AppendResult::stored);

  auto snapshot = buffer.snapshot(1s);
  CHECK(snapshot.status() == SnapshotStatus::ready);
  auto copy = snapshot;
  CHECK(buffer.stats().live_snapshots == 1);
  CHECK(buffer.snapshot(1s).status() == SnapshotStatus::snapshot_capacity);

  snapshot = {};
  CHECK(buffer.stats().live_snapshots == 1);
  copy = {};
  CHECK(buffer.stats().live_snapshots == 0);
  CHECK(buffer.snapshot(1s).status() == SnapshotStatus::ready);
}

void test_timestamp_conversion_and_stream_validation() {
  ReplayBuffer buffer({2s, 1s, 1024});
  CHECK(buffer.append(packet(0, true, 1, {1, 30})) == AppendResult::stored);
  CHECK(buffer.snapshot(1s).status() == SnapshotStatus::insufficient_history);
  CHECK(buffer.append(packet(30, true, 1, {1, 30})) == AppendResult::stored);
  CHECK(buffer.append(packet(60, false, 1, {1, 30})) == AppendResult::stored);

  const auto snapshot = buffer.snapshot(1s);
  CHECK(snapshot.status() == SnapshotStatus::ready);
  CHECK(snapshot.start_time() == 1s);
  CHECK(snapshot.end_time() == 2s);
  CHECK(snapshot.keyframe_preroll() == 0s);

  const auto invalid_duration = buffer.snapshot(1001s);
  CHECK(invalid_duration.status() == SnapshotStatus::invalid_duration);
  auto compatible = packet(90, false, 1, {1, 30});
  CHECK(buffer.append(std::move(compatible)) == AppendResult::stored);
  CHECK(buffer.append(packet(90, false, 1, {2, 60})) == AppendResult::incompatible_stream);

  auto invalid = packet(1, false, 1, {1, 0});
  CHECK(buffer.append(std::move(invalid)) == AppendResult::invalid_packet);

  auto missing_dts = packet(90, false);
  missing_dts.dts = std::numeric_limits<std::int64_t>::min();
  CHECK(buffer.append(std::move(missing_dts)) == AppendResult::invalid_packet);

  auto missing_pts = packet(90, false);
  missing_pts.pts = std::numeric_limits<std::int64_t>::min();
  CHECK(buffer.append(std::move(missing_pts)) == AppendResult::invalid_packet);
}

void test_audio_tracks_follow_video_keyframe_window_and_arrival_order() {
  ReplayBuffer buffer({4s, 1s, 4096, 128, 2});
  constexpr std::uint64_t origin_ns = 10'000'000'000ULL;

  // Audio may arrive before the first video packet and before the video origin.
  CHECK(buffer.append_audio(audio_packet(0, origin_ns - 100'000'000ULL), -100ms) ==
        AppendResult::stored);
  for (std::int64_t pts = 0; pts <= 4000; pts += 1000) {
    CHECK(buffer.append(packet(pts, pts % 2000 == 0)) == AppendResult::stored);
  }

  // Appends are intentionally out of timestamp order. The two independent
  // audio sources share one video-relative timeline and sort on snapshot.
  CHECK(buffer.append_audio(audio_packet(1, origin_ns + 2'500'000'000ULL), 2500ms) ==
        AppendResult::stored);
  CHECK(buffer.append_audio(audio_packet(0, origin_ns + 2'000'000'000ULL), 2000ms) ==
        AppendResult::stored);
  CHECK(buffer.append_audio(audio_packet(1, origin_ns + 1'500'000'000ULL), 1500ms) ==
        AppendResult::stored);
  CHECK(buffer.append_audio(audio_packet(0, origin_ns + 4'100'000'000ULL), 4100ms) ==
        AppendResult::stored);

  const auto snapshot = buffer.snapshot(2s);
  CHECK(snapshot.status() == SnapshotStatus::ready);
  CHECK(snapshot.start_time() == 2s);
  CHECK(snapshot.end_time() == 4s);
  CHECK(snapshot.packet_count() == 3);
  CHECK(snapshot.audio_packet_count() == 2);
  CHECK(snapshot.total_packet_count() == 5);
  CHECK(snapshot.payload_bytes() == 7);

  std::vector<std::uint64_t> audio_capture_times;
  std::vector<std::size_t> audio_streams;
  snapshot.for_each_audio_packet([&](const auto& p) {
    audio_capture_times.push_back(p->capture_time_ns);
    audio_streams.push_back(p->stream_index);
  });
  CHECK((audio_capture_times == std::vector<std::uint64_t>{
      origin_ns + 2'000'000'000ULL, origin_ns + 2'500'000'000ULL}));
  CHECK((audio_streams == std::vector<std::size_t>{0, 1}));

  const auto stats = buffer.stats();
  CHECK(stats.packet_count == 10);
  CHECK(stats.video_packet_count == 5);
  CHECK(stats.audio_packet_count == 5);
  CHECK(stats.audio_appended_packets == 5);
  CHECK(stats.audio_rejected_packets == 0);
}

void test_audio_packets_share_replay_capacity_and_validate_absolute_pts() {
  ReplayBuffer buffer({4s, 1s, 128, 4, 2});
  constexpr std::uint64_t origin_ns = 5'000'000'000ULL;
  CHECK(buffer.append(packet(0, true, 4)) == AppendResult::stored);
  CHECK(buffer.append(packet(1000, false, 4)) == AppendResult::stored);
  CHECK(buffer.append_audio(audio_packet(0, origin_ns + 1'000'000'000ULL, 4), 1s) ==
        AppendResult::stored);
  CHECK(buffer.stats().packet_count == 3);

  // The combined packet budget evicts oldest audio first, preserving the
  // video GOP and its decode path when audio history can be shed independently.
  CHECK(buffer.append_audio(audio_packet(1, origin_ns + 2'000'000'000ULL, 4), 2s) ==
        AppendResult::stored);
  auto stats = buffer.stats();
  CHECK(stats.packet_count == 4);
  CHECK(stats.video_packet_count == 2);
  CHECK(stats.audio_packet_count == 2);
  CHECK(stats.audio_evicted_packets == 0);

  // A new video packet first sheds the oldest audio packet to make room.
  CHECK(buffer.append(packet(2000, false, 4)) == AppendResult::stored);
  stats = buffer.stats();
  CHECK(stats.packet_count == 4);
  CHECK(stats.video_packet_count == 3);
  CHECK(stats.audio_packet_count == 1);
  CHECK(stats.audio_evicted_packets == 1);

  auto relative_pts = audio_packet(0, origin_ns + 3'000'000'000ULL, 1);
  relative_pts.pts = 1;  // Not on the packet's absolute monotonic sample clock.
  CHECK(buffer.append_audio(std::move(relative_pts), 3s) == AppendResult::invalid_packet);
  stats = buffer.stats();
  CHECK(stats.audio_rejected_packets == 1);
}

void test_concurrent_append_and_snapshot() {
  ReplayBuffer buffer({1s, 100ms, 4U * 1024U * 1024U});
  std::atomic<bool> done{false};
  std::atomic<std::uint64_t> valid_snapshots{0};
  std::atomic<bool> bad_snapshot{false};

  std::thread reader([&] {
    while (!done.load(std::memory_order_acquire)) {
      auto snapshot = buffer.snapshot(500ms);
      if (snapshot.status() != SnapshotStatus::ready) {
        std::this_thread::yield();
        continue;
      }
      std::size_t visited = 0;
      bool first_was_keyframe = false;
      std::int64_t previous_pts = std::numeric_limits<std::int64_t>::min();
      snapshot.for_each_packet([&](const auto& p) {
        if (!p) {
          bad_snapshot.store(true, std::memory_order_relaxed);
          return;
        }
        if (visited == 0) {
          first_was_keyframe = p->keyframe;
        }
        if (p->pts < previous_pts) {
          bad_snapshot.store(true, std::memory_order_relaxed);
        }
        previous_pts = p->pts;
        ++visited;
      });
      if (!first_was_keyframe || visited != snapshot.packet_count()) {
        bad_snapshot.store(true, std::memory_order_relaxed);
      }
      ++valid_snapshots;
      std::this_thread::yield();
    }
  });

  for (std::int64_t frame = 0; frame < 5000; ++frame) {
    const auto result = buffer.append(packet(frame, frame % 30 == 0, 32, {1, 360}));
    CHECK(result == AppendResult::stored);
    std::this_thread::yield();
  }
  done.store(true, std::memory_order_release);
  reader.join();

  CHECK(valid_snapshots.load() > 0);
  CHECK(!bad_snapshot.load());
  const auto stats = buffer.stats();
  CHECK(stats.payload_bytes <= buffer.config().max_bytes);
  CHECK(stats.retained_duration <= 1100ms);
}

#undef CHECK

}  // namespace

int main() {
  test_duration_eviction_and_preceding_keyframe();
  test_runtime_start_stop_freezes_and_resets_ring();
  test_byte_eviction_and_missing_keyframe_status();
  test_packet_count_bound();
  test_snapshot_budget_tracks_async_lifetime();
  test_snapshot_shares_ring_payload_budget_without_shrinking_history();
  test_byte_limited_ring_reports_insufficient_history();
  test_duration_aware_budgets_cover_presets_and_clamp_custom_history();
  test_live_snapshot_count_bound_and_copy_lifetime();
  test_timestamp_conversion_and_stream_validation();
  test_audio_tracks_follow_video_keyframe_window_and_arrival_order();
  test_audio_packets_share_replay_capacity_and_validate_absolute_pts();
  test_concurrent_append_and_snapshot();
  std::cout << "replay buffer tests passed\n";
}
