#pragma once

#include "telemetry/system_metrics.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace fluxrec::telemetry {

enum class DropStage : std::size_t {
    capture,
    conversion,
    encoder,
    mux,
    replay,
    count,
};

enum class Queue : std::size_t {
    gpu_frame_pool,
    capture,
    encode,
    // PacketRouter ingress ring: occupancy counts waiting entries and excludes
    // the worker's in-flight packet. Capacity is entries; queued payload bytes
    // have a separate 64 MiB limit.
    packet,
    mux,
    count,
};

enum class Timing : std::size_t {
    frame_time,
    // Steady-clock time from capture request to the Wayland ready callback.
    capture,
    // Steady-clock wall time from the Wayland ready callback to encoder submit entry.
    // Includes host queue/scheduling delay; this is not GPU execution time.
    capture_ready_to_submit_wall,
    // Hardware execution time; only populated from GPU timestamp queries.
    gpu_conversion,
    // Hardware encoder queue measurements; only populated from a hardware timing source.
    nvenc_submission,
    nvenc_completion,
    // Host monotonic wall time around the named NVENC API calls. These include
    // driver waits and do not measure GPU/NVENC hardware execution time.
    nvenc_encode_picture_api_wall,
    nvenc_lock_bitstream_api_wall,
    // Host wall time from accepted NVENC submission return to successful
    // bitstream-lock return. This is a PTS-matched availability proxy; it
    // includes queueing and host/driver delay, not hardware execution time.
    nvenc_submit_to_bitstream_wall,
    // CPU elapsed time in the application around the named FFmpeg call path.
    dmabuf_import_cpu,
    // CPU steady-clock wall time spent around Vulkan-to-CUDA frame transfer.
    // This can include driver work or waiting; it is not GPU execution time.
    cuda_transfer_cpu,
    filter_submit_cpu,
    filter_pull_cpu,
    encoder_send_cpu,
    encoder_receive_cpu,
    // Steady-clock wall time from frame submit to its matching encoded packet.
    submit_to_packet_wall,
    frame_identity_gpu,
    // Enqueue acceptance to the mux worker removing the packet from its queue.
    mux_queue_wait,
    // Mux packet enqueue acceptance to successful libavformat write completion.
    mux,
    // Duration of each underlying write(2) syscall; does not imply durable fsync.
    disk_write,
    count,
};

enum class GpuConversionPass : std::uint8_t {
    first,
    second,
};

enum class NvencApiCall : std::uint8_t {
    encode_picture,
    lock_bitstream,
};

struct NvencApiTimingSample {
    std::int64_t pts{};
    std::uint64_t encode_picture_wall_ns{};
    std::uint64_t lock_bitstream_wall_ns{};
    std::optional<std::uint64_t> submit_to_bitstream_wall_ns;
};

struct FrameCounts {
    std::uint64_t captured{};
    // These values are absent until their source is connected. A reported
    // zero means a source reported zero frames, rather than "not measured".
    std::optional<std::uint64_t> compositor;
    std::optional<std::uint64_t> unique;
    std::uint64_t submitted{};
    std::uint64_t encoded{};
    // Absent until a primary mux reports packet counts. Replay-only mode has
    // no primary mux, so its muxed count stays unknown rather than zero.
    std::optional<std::uint64_t> muxed;
    std::uint64_t dropped{};
    std::array<std::uint64_t, static_cast<std::size_t>(DropStage::count)> dropped_by_stage{};
    std::optional<std::uint64_t> duplicated;
    // These diagnostic counters stay absent unless GPU frame digests are on.
    std::optional<std::uint64_t> identity_checked;
    std::optional<std::uint64_t> identity_skipped;
    std::optional<std::uint64_t> identity_unclassified;
};

struct CaptureCounts {
    // Exact ext-image-copy-capture ready completions reported by Wayland. This
    // includes frames later refused by the output-rate cap or internal queue.
    std::optional<std::uint64_t> wayland_completed;
    // Completed frames admitted to the capture queue for process_frame.
    std::optional<std::uint64_t> admitted;
    // Completed frames intentionally refused by the configured output-rate cap.
    std::optional<std::uint64_t> rate_limited;
};

struct QueueSnapshot {
    std::optional<std::uint64_t> occupancy;
    std::optional<std::uint64_t> high_water;
    std::optional<std::uint64_t> capacity;
};

struct StorageSnapshot {
    // Byte counters are absent until the corresponding subsystem is sampled.
    std::optional<std::uint64_t> output_file_bytes;
    std::optional<std::uint64_t> encoded_payload_bytes;
    // Replay payload accounting excludes packet metadata and allocator overhead.
    std::optional<std::uint64_t> replay_payload_bytes;
    std::optional<std::uint64_t> replay_snapshot_payload_bytes;
    std::optional<std::uint64_t> replay_budget_bytes;
    // Bits per second, derived from the change in output_file_bytes over the reporter interval.
    std::optional<double> output_file_bitrate_bps;
};

struct ReplayModeSnapshot {
    // These remain absent until the engine reports whether a replay ring is
    // configured and whether it is currently accepting packets.
    std::optional<bool> enabled;
    std::optional<bool> active;
    // Cumulative PacketRouter ingress capacity/byte-limit rejects. This covers
    // packets routed toward mux and/or replay, excludes close rejects and
    // downstream mux/replay drops, and is reported whenever replay is configured
    // (even when its ring is currently inactive).
    std::optional<std::uint64_t> packet_queue_drops;
};

struct DistributionSnapshot {
    std::uint64_t samples{};
    std::uint64_t window_capacity{};
    std::optional<double> average_ns;
    std::optional<double> p50_ns;
    std::optional<double> p95_ns;
    std::optional<double> p99_ns;
    std::optional<double> p99_9_ns;
    std::optional<double> max_ns;
};

struct FrameRates {
    std::optional<double> captured_fps;
    std::optional<double> compositor_fps;
    std::optional<double> unique_fps;
    std::optional<double> submitted_fps;
    std::optional<double> encoded_fps;
    std::optional<double> muxed_fps;
};

struct Snapshot {
    std::chrono::system_clock::time_point timestamp;
    std::chrono::steady_clock::time_point monotonic_timestamp;
    FrameCounts frames;
    CaptureCounts capture;
    std::array<QueueSnapshot, static_cast<std::size_t>(Queue::count)> queues{};
    StorageSnapshot storage;
    ReplayModeSnapshot replay;
    std::array<DistributionSnapshot, static_cast<std::size_t>(Timing::count)> timings{};
    // Recent per-frame host API-call wall spans joined by the NVENC input/output
    // PTS. This preserves frame correlation across B-frame reordering.
    std::vector<NvencApiTimingSample> nvenc_api_timing_samples;
    std::optional<FrameRates> frame_rates;
    // True only for the no-primary-output replay-buffer-only operation.
    std::optional<bool> replay_only;
    // Absent until a system sampler has supplied its first sample. Individual
    // measurements remain absent when the corresponding source is unavailable.
    std::optional<SystemMetrics> system;
};

// Calculate rates over the monotonic time between two snapshots. A rate is
// absent if the counter went backwards or the interval is not positive.
[[nodiscard]] FrameRates calculateFrameRates(const Snapshot& previous,
                                            const Snapshot& current) noexcept;

// Convert samples of a cumulative counter to a non-underflowing delta. When a
// source resets, the caller should still advance its previous sample to the
// current value so later increments are counted from the new baseline.
[[nodiscard]] constexpr std::uint64_t monotonicCounterDelta(
    std::uint64_t previous, std::uint64_t current) noexcept {
    return current >= previous ? current - previous : 0;
}

// Thread-safe, bounded-memory instrumentation. Timing percentiles and the
// associated average/max describe the most recent `timing_window_capacity`
// observations for each timing metric.
class Telemetry final {
public:
    explicit Telemetry(std::size_t timing_window_capacity = 8192);

    Telemetry(const Telemetry&) = delete;
    Telemetry& operator=(const Telemetry&) = delete;
    Telemetry(Telemetry&&) = delete;
    Telemetry& operator=(Telemetry&&) = delete;

    // Counts are independent observations supplied by the corresponding
    // pipeline stage. `captured` is a delivered capture buffer; `compositor`
    // is the count of distinct presentation timestamps on delivered frames.
    // It does not count all source presentations made by the compositor.
    void addCaptured(std::uint64_t count = 1) noexcept;
    void addCaptureWaylandCompleted(std::uint64_t count = 1) noexcept;
    void addCaptureAdmitted(std::uint64_t count = 1) noexcept;
    void addCaptureRateLimited(std::uint64_t count = 1) noexcept;
    void addCompositorFrames(std::uint64_t count = 1) noexcept;
    void addUniqueFrames(std::uint64_t count = 1) noexcept;
    void addSubmittedFrames(std::uint64_t count = 1) noexcept;
    void addEncodedFrames(std::uint64_t count = 1) noexcept;
    void addMuxedFrames(std::uint64_t count = 1) noexcept;
    void addDuplicatedFrames(std::uint64_t count = 1) noexcept;
    void addIdentityCheckedFrames(std::uint64_t count = 1) noexcept;
    void addIdentitySkippedFrames(std::uint64_t count = 1) noexcept;
    void addIdentityUnclassifiedFrames(std::uint64_t count = 1) noexcept;
    void addDroppedFrames(DropStage stage, std::uint64_t count = 1) noexcept;

    // Call after a queue mutation, with the current size read from the queue.
    // High-water is monotonic for the lifetime of this Telemetry instance.
    void observeQueue(Queue queue, std::uint64_t occupancy) noexcept;
    // Use these when multiple producers/consumers mutate queue occupancy
    // concurrently and can report each enqueue/dequeue exactly once.
    void addQueueEntries(Queue queue, std::uint64_t count = 1) noexcept;
    void removeQueueEntries(Queue queue, std::uint64_t count = 1) noexcept;
    void setQueueCapacity(Queue queue, std::uint64_t capacity) noexcept;

    // Feed counters from the mux writer and replay buffer. Replay values count
    // compressed payload bytes; they do not include metadata or allocator overhead.
    void observeOutputStats(std::uint64_t file_bytes,
                            std::uint64_t encoded_payload_bytes) noexcept;
    void observeReplayStats(std::uint64_t replay_payload_bytes,
                            std::uint64_t snapshot_payload_bytes,
                            std::uint64_t payload_budget_bytes) noexcept;
    // `enabled` reports whether a replay ring is configured; `active` reports
    // whether it is currently accepting packets. Call with (false, false)
    // when replay is not configured so clients can distinguish no ring from
    // an as-yet-unobserved mode.
    // `packet_queue_drops` counts cumulative ingress capacity/byte-limit rejects.
    void observeReplayMode(bool enabled, bool active,
                           std::uint64_t packet_queue_drops = 0,
                           bool replay_only = false) noexcept;
    // Publish a low-rate system sample from the stats thread. Missing values
    // are preserved as unknown and serialize as JSON null.
    void observeSystemMetrics(const SystemMetrics& metrics) noexcept;

    // Negative durations are ignored. The templated overload accepts any
    // chrono duration and normalizes it to nanoseconds.
    void observe(Timing timing, std::chrono::nanoseconds duration) noexcept;
    template<class Rep, class Period>
    void observe(Timing timing, std::chrono::duration<Rep, Period> duration) noexcept {
        observe(timing, std::chrono::duration_cast<std::chrono::nanoseconds>(duration));
    }
    void observeNvencApiTiming(const NvencApiTimingSample& sample) noexcept;

    [[nodiscard]] Snapshot snapshot() const;
    [[nodiscard]] std::string toJson() const;
    [[nodiscard]] static std::string toJson(const Snapshot& snapshot);
    [[nodiscard]] std::size_t timingWindowCapacity() const noexcept { return timing_window_capacity_; }

private:
    struct QueueState {
        std::atomic<std::uint64_t> occupancy{0};
        std::atomic<std::uint64_t> high_water{0};
        std::atomic<std::uint64_t> capacity{0};
        std::atomic<bool> occupancy_observed{false};
        std::atomic<bool> capacity_observed{false};
    };

    struct TimingState {
        explicit TimingState(std::size_t capacity) : samples(capacity) {}
        mutable std::mutex mutex;
        std::vector<std::uint64_t> samples;
        std::size_t next{};
        std::size_t size{};
    };

    static constexpr std::size_t counter_count = 10;
    static void updateHighWater(QueueState& state, std::uint64_t occupancy) noexcept;
    std::array<std::atomic<std::uint64_t>, counter_count> counters_{};
    std::array<std::atomic<bool>, counter_count> counter_observed_{};
    std::array<std::atomic<std::uint64_t>, 3> capture_counters_{};
    std::array<std::atomic<bool>, 3> capture_counter_observed_{};
    std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(DropStage::count)> drops_{};
    std::array<QueueState, static_cast<std::size_t>(Queue::count)> queues_{};
    std::array<TimingState, static_cast<std::size_t>(Timing::count)> timings_;
    const std::size_t timing_window_capacity_;
    static constexpr std::size_t nvenc_api_timing_capacity = 1024;
    mutable std::mutex nvenc_api_timing_mutex_;
    std::array<NvencApiTimingSample, nvenc_api_timing_capacity> nvenc_api_timing_samples_{};
    std::size_t nvenc_api_timing_next_{};
    std::size_t nvenc_api_timing_size_{};
    std::atomic<std::uint64_t> output_file_bytes_{0};
    std::atomic<std::uint64_t> encoded_payload_bytes_{0};
    std::atomic<std::uint64_t> replay_payload_bytes_{0};
    std::atomic<std::uint64_t> replay_snapshot_payload_bytes_{0};
    std::atomic<std::uint64_t> replay_budget_bytes_{0};
    std::atomic<bool> output_stats_observed_{false};
    std::atomic<bool> replay_stats_observed_{false};
    std::atomic<bool> replay_enabled_{false};
    std::atomic<bool> replay_active_{false};
    std::atomic<std::uint64_t> replay_packet_queue_drops_{0};
    std::atomic<bool> replay_only_{false};
    std::atomic<bool> replay_mode_observed_{false};
    mutable std::mutex system_metrics_mutex_;
    std::optional<SystemMetrics> system_metrics_;
};

// Joins the two asynchronous scale_vulkan timestamp results by input-frame
// PTS and records one sum per frame in Timing::gpu_conversion. The bounded
// pending set handles the readback delay without retaining frame data.
class GpuConversionTimingBridge final {
public:
    explicit GpuConversionTimingBridge(Telemetry& telemetry) noexcept;

    void observePass(GpuConversionPass pass, std::int64_t frame_pts,
                     std::uint64_t elapsed_ns) noexcept;

private:
    struct PendingFrame {
        std::int64_t pts{};
        std::uint64_t first_ns{};
        std::uint64_t second_ns{};
        bool used{};
        bool has_first{};
        bool has_second{};
    };

    static constexpr std::size_t pending_capacity = 64;
    Telemetry& telemetry_;
    std::mutex pending_mutex_;
    std::array<PendingFrame, pending_capacity> pending_{};
};

// Joins host-observed NVENC API call spans by PTS. The encode call reports the
// input PTS; the blocking bitstream lock reports NVENC's output PTS. The bounded
// pending set accommodates API output delay and B-frame reordering.
class NvencApiTimingBridge final {
public:
    explicit NvencApiTimingBridge(Telemetry& telemetry) noexcept;

    void observe(NvencApiCall call, std::int64_t frame_pts,
                 std::uint64_t elapsed_ns,
                 std::uint64_t event_monotonic_ns) noexcept;

private:
    struct PendingFrame {
        std::int64_t pts{};
        std::uint64_t encode_picture_wall_ns{};
        std::uint64_t lock_bitstream_wall_ns{};
        std::uint64_t encode_picture_return_monotonic_ns{};
        std::uint64_t lock_bitstream_return_monotonic_ns{};
        bool used{};
        bool has_encode_picture{};
        bool has_lock_bitstream{};
    };

    static constexpr std::size_t pending_capacity = 128;
    Telemetry& telemetry_;
    std::mutex pending_mutex_;
    std::array<PendingFrame, pending_capacity> pending_{};
};

// Emits a JSON snapshot on a dedicated thread. The callback runs on that
// thread and should return promptly; a slow sink never blocks instrumentation.
class PeriodicJsonReporter final {
public:
    using Sink = std::function<void(std::string)>;

    PeriodicJsonReporter(const Telemetry& telemetry,
                         std::chrono::milliseconds interval,
                         Sink sink);
    ~PeriodicJsonReporter();

    PeriodicJsonReporter(const PeriodicJsonReporter&) = delete;
    PeriodicJsonReporter& operator=(const PeriodicJsonReporter&) = delete;
    PeriodicJsonReporter(PeriodicJsonReporter&&) = delete;
    PeriodicJsonReporter& operator=(PeriodicJsonReporter&&) = delete;

private:
    std::mutex wait_mutex_;
    std::condition_variable_any wake_;
    std::jthread worker_;
};

} // namespace fluxrec::telemetry
