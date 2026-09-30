#pragma once

#include "mux/mux_writer.hpp"
#include "replay/replay_buffer.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace fluxrec::engine {

struct ReplayExporterConfig {
    // Counts the active export and queued exports together. Each job pins one
    // ReplaySnapshot, so this is also the maximum number of live export leases.
    std::size_t max_in_flight_jobs{2};
    // Completed metadata is retained until popped, then oldest entries are
    // discarded when this fixed-size result queue fills.
    std::size_t max_completed_results{32};
    std::chrono::milliseconds writer_ready_timeout{5000};
    // A packet enqueue that remains blocked by MuxWriter's bounded queue for
    // longer than this fails the export. Time spent writing accepted packets
    // does not count against this stall timeout.
    std::chrono::milliseconds writer_stall_timeout{30000};
    std::chrono::microseconds initial_retry_delay{200};
    std::chrono::milliseconds max_retry_delay{5};
};

enum class ReplayExportSubmitStatus {
    queued,
    invalid_config,
    queue_full,
    snapshot_unavailable,
    snapshot_codec_mismatch,
    stopping,
};

struct ReplayExportSubmission {
    ReplayExportSubmitStatus status{ReplayExportSubmitStatus::snapshot_unavailable};
    std::uint64_t job_id{};
    replay::SnapshotStatus snapshot_status{replay::SnapshotStatus::no_keyframe};

    [[nodiscard]] explicit operator bool() const noexcept {
        return status == ReplayExportSubmitStatus::queued;
    }
};

struct ReplayExportResult {
    std::uint64_t job_id{};
    std::string output_path;
  bool succeeded{};
  std::size_t snapshot_video_packets{};
  std::size_t snapshot_audio_packets{};
  // Combined packet count submitted to MuxWriter (audio is zero when the
  // caller supplies a video-only MuxConfig).
  std::size_t snapshot_packets{};
    std::size_t snapshot_payload_bytes{};
    std::uint64_t enqueued_packets{};
    std::uint64_t muxed_packets{};
    std::uint64_t enqueue_retries{};
    std::uint64_t output_file_bytes{};
    std::string error;
};

struct ReplayExporterStats {
    std::uint64_t submitted_jobs{};
    std::uint64_t completed_jobs{};
    std::uint64_t failed_jobs{};
    std::uint64_t rejected_capacity{};
    std::uint64_t rejected_snapshot{};
    std::uint64_t rejected_codec_mismatch{};
    std::uint64_t enqueue_retries{};
    std::uint64_t completed_results_discarded{};
    std::size_t in_flight_jobs{};
    std::size_t queued_jobs{};
};

// Exports keyframe-aligned compressed snapshots on its own worker. For A/V,
// selected video and audio packets are merged by their absolute DTS before
// entering MuxWriter, avoiding track-at-a-time interleaver buffering. submit()
// is intended for a UI/control path: it acquires the snapshot lease, but it
// never waits for disk or MuxWriter queue space. All queue-full/busy retries
// occur on the exporter worker while live recording can continue independently.
class ReplayExporter final {
public:
    ReplayExporter(replay::ReplayBuffer& buffer, ReplayExporterConfig config = {});
    ~ReplayExporter();

    ReplayExporter(const ReplayExporter&) = delete;
    ReplayExporter& operator=(const ReplayExporter&) = delete;
    ReplayExporter(ReplayExporter&&) = delete;
    ReplayExporter& operator=(ReplayExporter&&) = delete;

    // MuxConfig must describe the active compressed streams, including codec
    // dimensions, stream time bases, audio formats, and encoder extradata. For
    // audio, video_origin_time_ns remains the original recording origin.
    // MuxWriter applies the first selected video's DTS offset once so audio and
    // reordered video share the same presentation origin. This exporter
    // supplies no encoding path and copies no raw frames.
    [[nodiscard]] ReplayExportSubmission submit(
        mux::MuxConfig mux_config, replay::Nanoseconds requested_duration);

    // Returns the oldest completed result without waiting.
    [[nodiscard]] bool try_pop_result(ReplayExportResult& result);
    [[nodiscard]] ReplayExporterStats stats() const noexcept;

    // Stops new submissions, finishes already accepted exports, and joins the
    // worker. Safe to call more than once.
    void close() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace fluxrec::engine
