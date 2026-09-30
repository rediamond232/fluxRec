#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace fluxrec::capture {

struct CaptureError {
    std::string message;
    int system_error = 0;
};

class UniqueFd {
public:
    UniqueFd() noexcept = default;
    explicit UniqueFd(int fd) noexcept;
    ~UniqueFd();

    UniqueFd(UniqueFd&& other) noexcept;
    UniqueFd& operator=(UniqueFd&& other) noexcept;

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    [[nodiscard]] int get() const noexcept;
    [[nodiscard]] explicit operator bool() const noexcept;
    [[nodiscard]] int release() noexcept;
    void reset(int fd = -1) noexcept;

private:
    int fd_ = -1;
};

struct DmabufPlane {
    // Input fds passed to set_buffer_pool are borrowed for that call. Fds
    // exposed by FrameLease::image() are borrowed for the lease lifetime.
    int fd = -1;
    std::uint32_t offset = 0;
    std::uint32_t stride = 0;
};

struct DmabufImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t fourcc = 0;
    std::uint64_t modifier = 0;
    std::vector<DmabufPlane> planes;
};

struct FormatModifier {
    std::uint32_t fourcc = 0;
    std::uint64_t modifier = 0;

    friend bool operator==(const FormatModifier&, const FormatModifier&) = default;
};

struct CaptureRequirements {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    // Raw dev_t value from ext_image_copy_capture_session_v1.dmabuf_device.
    // Its meaning is compositor-defined; retain it when selecting the GPU
    // device and allocator for the supplied pool.
    std::optional<std::uint64_t> dmabuf_device;
    std::vector<FormatModifier> dmabuf_formats;
};

struct CaptureConfig {
    std::string output_name = "DP-1";
    bool paint_cursors = false;
    // Maximum output-frame rate. Zero leaves capture uncapped. Positive values
    // use a bounded-burst presentation-time token bucket (at most max_fps*T+2
    // admissions over a PTS interval T); this is a long-run ceiling, not a
    // strict per-frame or per-one-second limit. Below 360 FPS, requests are
    // also paced at the configured interval. At 360 FPS and above, requests
    // stay immediate and the compositor PTS bucket enforces the output ceiling.
    std::uint32_t max_fps = 0;
    // Zero means one queue entry per pool slot. A smaller value drops frames
    // when the consumer falls behind, without stalling Wayland dispatch.
    std::size_t frame_queue_capacity = 0;
};

struct DamageRect {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;
};

namespace detail {
class CaptureCore;
struct FrameLeaseState;
} // namespace detail

class FrameLease {
public:
    FrameLease() noexcept = default;
    ~FrameLease();

    FrameLease(FrameLease&& other) noexcept;
    FrameLease& operator=(FrameLease&& other) noexcept;

    FrameLease(const FrameLease&) = delete;
    FrameLease& operator=(const FrameLease&) = delete;

    [[nodiscard]] explicit operator bool() const noexcept;
    [[nodiscard]] std::size_t slot_index() const noexcept;
    [[nodiscard]] const DmabufImage& image() const;
    [[nodiscard]] std::expected<UniqueFd, CaptureError> duplicate_plane_fd(std::size_t plane) const;

    // Call after the GPU has finished reading this slot. Destroying the lease
    // has the same effect, so it can be moved into an asynchronous GPU job.
    void release_after_gpu() noexcept;

private:
    explicit FrameLease(std::unique_ptr<detail::FrameLeaseState> state) noexcept;
    std::unique_ptr<detail::FrameLeaseState> state_;

    friend class detail::CaptureCore;
};

struct CapturedFrame {
    std::uint64_t sequence = 0;
    std::uint64_t presentation_time_ns = 0;
    std::uint64_t request_to_ready_ns = 0;
    // Host steady-clock instant when the Wayland ready callback arrived.
    // Used to measure ready-to-encoder-submit handoff delay without touching pixels.
    std::optional<std::chrono::steady_clock::time_point> ready_at;
    bool has_presentation_time = false;
    std::uint32_t transform = 0;
    bool has_transform = false;
    std::vector<DamageRect> damage;
    FrameLease lease;
};

enum class CaptureStatus {
    connected,
    running,
    stopped,
    failed,
};

struct CaptureStats {
    std::uint64_t requests = 0;
    // Every successful Wayland ready completion, including frames refused by
    // the configured output-rate token bucket.
    std::uint64_t completed = 0;
    // Frames admitted to the internal queue for the engine. This excludes
    // rate-limited completions and queue-full drops.
    std::uint64_t admitted = 0;
    // Successful completions intentionally refused by the output-rate token
    // bucket. These are not internal queue drops or capture failures.
    std::uint64_t rate_limited = 0;
    std::uint64_t dropped = 0;
    // Copy had started, but no free DMA-BUF slot or idle session was available
    // to arm the next request. Hyprland can drop the next output commit in
    // that window. This is not included in `dropped`.
    std::uint64_t overlap_arm_failures = 0;
    std::uint64_t failures = 0;
    // Capture requests canceled by an explicit stop while still in flight.
    // These are neither completed frames nor compositor/source drops.
    std::uint64_t cancelled_at_stop = 0;
    std::uint64_t request_to_ready_samples = 0;
    std::uint64_t request_to_ready_last_ns = 0;
    std::uint64_t request_to_ready_average_ns = 0;
    std::uint64_t request_to_ready_max_ns = 0;
    // Percentiles use the most recent up to 512 successful captures.
    std::size_t request_to_ready_window_size = 0;
    std::uint64_t request_to_ready_p50_ns = 0;
    std::uint64_t request_to_ready_p95_ns = 0;
    std::uint64_t request_to_ready_p99_ns = 0;
    // A starvation period starts when every slot is unavailable and ends when
    // a lease returns a slot. It measures capture backpressure time, not an
    // inferred count of compositor frames missed during that interval.
    std::uint64_t pool_starvation_periods = 0;
    std::uint64_t pool_starvation_total_ns = 0;
    std::uint64_t pool_starvation_current_ns = 0;

    // Occupancy snapshot: frame_queue_occupancy is the number of ready frames
    // waiting in the internal queue. pool_size is the configured pool size.
    // While running, free + capturing + leased equals pool_size, and queued
    // frames are a subset of leased slots. pool_busy_slots is capturing plus
    // leased. After stop the Wayland pool is inactive, so free/capturing are
    // zero; outstanding FrameLeases (queued or returned to the caller) remain
    // counted until released.
    std::size_t frame_queue_occupancy = 0;
    std::size_t pool_size = 0;
    std::size_t pool_free_slots = 0;
    std::size_t pool_capturing_slots = 0;
    std::size_t pool_leased_slots = 0;
    std::size_t pool_busy_slots = 0;
};

class WaylandCapture {
public:
    WaylandCapture() noexcept = default;
    ~WaylandCapture();

    WaylandCapture(WaylandCapture&& other) noexcept;
    WaylandCapture& operator=(WaylandCapture&& other) noexcept;

    WaylandCapture(const WaylandCapture&) = delete;
    WaylandCapture& operator=(const WaylandCapture&) = delete;

    // Connects to the Wayland compositor, selects output_name, creates an
    // ext-image-copy-capture session, and collects its DMA-BUF constraints.
    // This does not allocate any image storage.
    [[nodiscard]] static std::expected<WaylandCapture, CaptureError> connect(
        CaptureConfig config = {});

    [[nodiscard]] const CaptureRequirements& requirements() const;

    // Imports caller-allocated GPU DMA-BUFs as wl_buffers. The pool must match
    // the negotiated dimensions and one advertised fourcc/modifier pair.
    // Known DRM format plane counts, minimum strides and per-fd plane overlap
    // are checked here. If lseek(SEEK_END) reports an allocation size, each
    // plane extent is checked against it; otherwise the allocator must supply
    // valid in-bounds extents because some DMA-BUF exporters do not report size.
    // FDs are duplicated and owned by the capture module until its wl_buffers
    // are destroyed; captured pixels are never mapped or copied by this API.
    [[nodiscard]] std::expected<void, CaptureError> set_buffer_pool(
        std::span<const DmabufImage> images);

    [[nodiscard]] std::expected<void, CaptureError> start();
    void stop() noexcept;

    [[nodiscard]] std::optional<CapturedFrame> try_pop_frame();
    [[nodiscard]] std::optional<CapturedFrame> wait_pop_frame(std::chrono::milliseconds timeout);

    [[nodiscard]] CaptureStatus status() const noexcept;
    [[nodiscard]] std::optional<CaptureError> runtime_error() const;
    [[nodiscard]] std::uint64_t captured_frames() const noexcept;
    [[nodiscard]] std::uint64_t dropped_frames() const noexcept;
    [[nodiscard]] std::uint64_t capture_failures() const noexcept;
    [[nodiscard]] CaptureStats stats() const;
    [[nodiscard]] std::size_t pool_size() const noexcept;

private:
    explicit WaylandCapture(std::shared_ptr<detail::CaptureCore> core) noexcept;
    std::shared_ptr<detail::CaptureCore> core_;
};

} // namespace fluxrec::capture
