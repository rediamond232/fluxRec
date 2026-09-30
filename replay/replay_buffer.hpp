#pragma once

#include "mux/packet.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace fluxrec::replay {

using Packet = media::EncodedPacket;
using PacketPtr = std::shared_ptr<const Packet>;
using AudioPacket = media::EncodedAudioPacket;
using AudioPacketPtr = std::shared_ptr<const AudioPacket>;
using Nanoseconds = std::chrono::nanoseconds;

struct ReplayBufferConfig {
  static constexpr std::size_t minimum_payload_budget_bytes = 512U * 1024U * 1024U;
  static constexpr std::size_t maximum_payload_budget_bytes = 2U * 1024U * 1024U * 1024U;

  Nanoseconds history_duration{std::chrono::seconds{30}};
  // Extra retention permits a requested window to start at the preceding GOP
  // keyframe. The maximum storage duration is history_duration + this margin.
  Nanoseconds max_keyframe_preroll{std::chrono::seconds{2}};
  // Compressed packet payload budget for the retained ring. Snapshot leases
  // share packet payloads with the ring and have their own bounded budget.
  std::size_t max_bytes{512U * 1024U * 1024U};
  std::size_t max_packets{65'536};
  std::size_t max_live_snapshots{4};
  // Aggregate selected payload bytes held by asynchronous snapshots. This is
  // counted conservatively across snapshots, including packets still in the
  // ring, so total payload storage is bounded by max_bytes + this value.
  std::size_t max_snapshot_bytes{512U * 1024U * 1024U};
};

// Builds bounded ring and snapshot payload limits from replay history and
// requested video bitrate. The estimate uses at least 120 Mbps, or twice the
// requested rate, and includes keyframe preroll. The ring is capped at 2 GiB.
[[nodiscard]] ReplayBufferConfig make_duration_aware_replay_buffer_config(
    std::chrono::nanoseconds history_duration, std::uint64_t target_bitrate_bps);

enum class AppendResult {
  stored,
  invalid_packet,
  incompatible_stream,
  packet_too_large,
  packet_capacity,
  outside_retention_window,
  snapshot_memory_budget,
  inactive,
};

enum class SnapshotStatus {
  ready,
  invalid_duration,
  insufficient_history,
  no_keyframe,
  keyframe_preroll_exceeded,
  snapshot_memory_budget,
  snapshot_capacity,
};

namespace detail {
struct PacketBlock;
struct SnapshotLease;
struct SharedBudget;
}  // namespace detail

// An immutable view over a keyframe-aligned packet sequence. It retains only
// shared packet/block ownership; packet payload bytes are never copied here.
// Keep the snapshot alive while an asynchronous mux worker consumes its
// packets so the buffer can account for bytes pinned outside the live ring.
class ReplaySnapshot {
 public:
  ReplaySnapshot() = default;

  [[nodiscard]] SnapshotStatus status() const noexcept { return status_; }
  [[nodiscard]] explicit operator bool() const noexcept {
    return status_ == SnapshotStatus::ready;
  }
  [[nodiscard]] std::size_t packet_count() const noexcept { return packet_count_; }
  [[nodiscard]] std::size_t audio_packet_count() const noexcept {
    return audio_packets_.size();
  }
  [[nodiscard]] std::size_t total_packet_count() const noexcept {
    return packet_count_ + audio_packets_.size();
  }
  [[nodiscard]] std::size_t payload_bytes() const noexcept { return payload_bytes_; }
  [[nodiscard]] Nanoseconds requested_duration() const noexcept { return requested_duration_; }
  [[nodiscard]] Nanoseconds keyframe_preroll() const noexcept { return keyframe_preroll_; }
  [[nodiscard]] Nanoseconds start_time() const noexcept { return start_time_; }
  [[nodiscard]] Nanoseconds end_time() const noexcept { return end_time_; }

  // Visits packets in their original decode order. A mux worker can enqueue
  // each shared packet without copying compressed payloads or holding the
  // replay-buffer lock.
  void for_each_packet(const std::function<void(const PacketPtr&)>& visitor) const;
  // Audio payloads remain on their absolute monotonic sample clock for muxing;
  // this snapshot returns packets whose video-relative capture times fall in
  // [start_time(), end_time()]. It skips packets before the selected keyframe,
  // since compressed packets cannot be trimmed without decoding.
  void for_each_audio_packet(const std::function<void(const AudioPacketPtr&)>& visitor) const;

 private:
  SnapshotStatus status_{SnapshotStatus::no_keyframe};
  // Snapshots copy packet references, not compressed payloads. Keeping direct
  // references lets the ring release evicted block slots promptly.
  std::vector<PacketPtr> video_packets_;
  std::vector<AudioPacketPtr> audio_packets_;
  std::shared_ptr<detail::SnapshotLease> lease_;
  std::size_t packet_count_{};
  std::size_t payload_bytes_{};
  Nanoseconds requested_duration_{};
  Nanoseconds keyframe_preroll_{};
  Nanoseconds start_time_{};
  Nanoseconds end_time_{};

  friend class ReplayBuffer;
};

struct ReplayBufferStats {
  bool active{};
  // packet_count is the combined video and audio history count.
  std::size_t packet_count{};
  std::size_t video_packet_count{};
  std::size_t audio_packet_count{};
  std::size_t payload_bytes{};
  // Aggregate payload bytes selected by live snapshots. Packets shared with
  // the ring are counted again here for conservative export-budget checks.
  std::size_t snapshot_payload_bytes{};
  std::size_t live_snapshots{};
  Nanoseconds retained_duration{};
  std::uint64_t appended_packets{};
  std::uint64_t evicted_packets{};
  std::uint64_t rejected_packets{};
  std::uint64_t inactive_packets{};
  std::uint64_t audio_appended_packets{};
  std::uint64_t audio_evicted_packets{};
  std::uint64_t audio_rejected_packets{};
  std::uint64_t inactive_audio_packets{};
};

// Thread-safe bounded storage for compressed video and optional audio packets.
// Both packet types share one byte and packet budget. It starts active. Append
// does no I/O and never waits for a snapshot consumer; snapshots hold immutable
// shared packet references for asynchronous muxing.
class ReplayBuffer {
 public:
  explicit ReplayBuffer(ReplayBufferConfig config = {});
  ~ReplayBuffer();

  ReplayBuffer(const ReplayBuffer&) = delete;
  ReplayBuffer& operator=(const ReplayBuffer&) = delete;
  ReplayBuffer(ReplayBuffer&&) = delete;
  ReplayBuffer& operator=(ReplayBuffer&&) = delete;

  [[nodiscard]] AppendResult append(PacketPtr packet);
  [[nodiscard]] AppendResult append(Packet packet);
  // `video_relative_time` is CLOCK_MONOTONIC capture time minus the first
  // captured video's timestamp; it may be negative for audio captured first.
  // Packet PTS must stay absolute on the encoder's sample clock for MuxWriter.
  [[nodiscard]] AppendResult append_audio(AudioPacketPtr packet,
                                          Nanoseconds video_relative_time);
  [[nodiscard]] AppendResult append_audio(AudioPacket packet,
                                          Nanoseconds video_relative_time);

  // stop() serializes with append(), gates future appends, and preserves
  // retained packets so snapshot() remains usable for saving. An append that
  // acquired the ring lock before stop() may complete before it returns;
  // later appends return AppendResult::inactive without taking the lock.
  // start() clears frozen ring history and begins a fresh session. Existing
  // snapshots remain valid. Both operations are idempotent and return true
  // only when state changed.
  [[nodiscard]] bool start();
  [[nodiscard]] bool stop();
  [[nodiscard]] bool active() const noexcept;

  // The requested history must not exceed the configured history duration.
  // A ready snapshot begins at the latest keyframe no later than the requested
  // start, with its lead-in bounded by max_keyframe_preroll. It may therefore
  // cover up to that much extra time while remaining within the configured
  // retention duration.
  [[nodiscard]] ReplaySnapshot snapshot(Nanoseconds requested_duration) const;
  [[nodiscard]] ReplaySnapshot snapshot() const;
  [[nodiscard]] ReplayBufferStats stats() const;

  [[nodiscard]] const ReplayBufferConfig& config() const noexcept { return config_; }

 private:
  struct Impl;
  [[nodiscard]] AppendResult append_for_generation(PacketPtr packet,
                                                   std::uint64_t generation);
  ReplayBufferConfig config_;
  std::unique_ptr<Impl> impl_;
};

}  // namespace fluxrec::replay
