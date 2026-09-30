#include "capture/capture.hpp"

#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"
#include "linux-dmabuf-v1-client-protocol.h"
#include "capture_lifecycle.hpp"
#include "capture_rate_limit.hpp"
#include "capture_pts_rate_limit.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <cstdio>
#include <deque>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <mutex>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/types.h>
#include <thread>
#include <tuple>
#include <unistd.h>
#include <utility>

namespace fluxrec::capture {

UniqueFd::UniqueFd(int fd) noexcept : fd_(fd) {}

UniqueFd::~UniqueFd() {
    reset();
}

UniqueFd::UniqueFd(UniqueFd&& other) noexcept : fd_(other.release()) {}

UniqueFd& UniqueFd::operator=(UniqueFd&& other) noexcept {
    if (this != &other) {
        reset(other.release());
    }
    return *this;
}

int UniqueFd::get() const noexcept {
    return fd_;
}

UniqueFd::operator bool() const noexcept {
    return fd_ >= 0;
}

int UniqueFd::release() noexcept {
    const int result = fd_;
    fd_ = -1;
    return result;
}

void UniqueFd::reset(int fd) noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
    }
    fd_ = fd;
}

namespace {

constexpr std::uint32_t fourcc(char first, char second, char third, char fourth) noexcept {
    return static_cast<std::uint32_t>(first) |
           (static_cast<std::uint32_t>(second) << 8U) |
           (static_cast<std::uint32_t>(third) << 16U) |
           (static_cast<std::uint32_t>(fourth) << 24U);
}

struct PlaneLayout {
    std::size_t count = 0;
    std::array<std::uint64_t, 4> minimum_stride{};
    std::array<std::uint64_t, 4> rows{};
};

std::optional<PlaneLayout> describe_format(std::uint32_t format,
                                          std::uint32_t width,
                                          std::uint32_t height) {
    const std::uint64_t w = width;
    const std::uint64_t h = height;
    const std::uint64_t half_width = (w + 1U) / 2U;
    const std::uint64_t half_height = (h + 1U) / 2U;
    PlaneLayout layout;

    const auto packed = [&layout, h](std::uint64_t bytes_per_pixel) {
        layout.count = 1;
        layout.minimum_stride[0] = bytes_per_pixel;
        layout.rows[0] = h;
    };
    const auto planar_420 = [&layout, w, h, half_width, half_height] {
        layout.count = 3;
        layout.minimum_stride = {w, half_width, half_width, 0};
        layout.rows = {h, half_height, half_height, 0};
    };
    const auto semiplanar = [&layout, h](
                                std::uint64_t luma_bytes,
                                std::uint64_t chroma_bytes,
                                std::uint64_t chroma_rows) {
        layout.count = 2;
        layout.minimum_stride = {luma_bytes, chroma_bytes, 0, 0};
        layout.rows = {h, chroma_rows, 0, 0};
    };

    switch (format) {
    case fourcc('X', 'R', '2', '4'): case fourcc('A', 'R', '2', '4'):
    case fourcc('X', 'B', '2', '4'): case fourcc('A', 'B', '2', '4'):
    case fourcc('R', 'X', '2', '4'): case fourcc('R', 'A', '2', '4'):
    case fourcc('B', 'X', '2', '4'): case fourcc('B', 'A', '2', '4'):
    case fourcc('X', 'R', '3', '0'): case fourcc('A', 'R', '3', '0'):
    case fourcc('X', 'B', '3', '0'): case fourcc('A', 'B', '3', '0'):
    case fourcc('A', 'Y', 'U', 'V'): case fourcc('A', 'V', 'U', 'Y'):
    case fourcc('X', 'Y', 'U', 'V'): case fourcc('X', 'V', 'U', 'Y'):
        packed(4U);
        break;
    case fourcc('R', 'G', '2', '4'): case fourcc('B', 'G', '2', '4'):
        packed(3U);
        break;
    case fourcc('R', 'G', '1', '6'): case fourcc('B', 'G', '1', '6'):
    case fourcc('R', 'G', '8', '8'): case fourcc('G', 'R', '8', '8'):
    case fourcc('R', '1', '6', ' '):
    case fourcc('Y', 'U', 'Y', 'V'): case fourcc('Y', 'V', 'Y', 'U'):
    case fourcc('U', 'Y', 'V', 'Y'): case fourcc('V', 'Y', 'U', 'Y'):
        packed(2U);
        break;
    case fourcc('R', '8', ' ', ' '): case fourcc('C', '8', ' ', ' '):
    case fourcc('D', '8', ' ', ' '):
        packed(1U);
        break;
    case fourcc('R', 'G', '3', '2'): case fourcc('G', 'R', '3', '2'):
        packed(4U);
        break;
    case fourcc('N', 'V', '1', '2'): case fourcc('N', 'V', '2', '1'):
        semiplanar(w, half_width * 2U, half_height);
        break;
    case fourcc('N', 'V', '1', '6'): case fourcc('N', 'V', '6', '1'):
        semiplanar(w, half_width * 2U, h);
        break;
    case fourcc('N', 'V', '2', '4'): case fourcc('N', 'V', '4', '2'):
        semiplanar(w, w * 2U, h);
        break;
    case fourcc('P', '0', '1', '0'): case fourcc('P', '0', '1', '2'):
    case fourcc('P', '0', '1', '6'):
        semiplanar(w * 2U, w * 2U, half_height);
        break;
    case fourcc('P', '2', '1', '0'):
        semiplanar(w * 2U, w * 2U, h);
        break;
    case fourcc('Y', 'U', '1', '2'): case fourcc('Y', 'V', '1', '2'):
        planar_420();
        break;
    case fourcc('Y', 'U', '1', '6'): case fourcc('Y', 'V', '1', '6'):
        layout.count = 3;
        layout.minimum_stride = {w, half_width, half_width, 0};
        layout.rows = {h, h, h, 0};
        break;
    case fourcc('Y', 'U', '2', '4'): case fourcc('Y', 'V', '2', '4'):
        layout.count = 3;
        layout.minimum_stride = {w, w, w, 0};
        layout.rows = {h, h, h, 0};
        break;
    default:
        return std::nullopt;
    }

    if (layout.count == 1U && layout.minimum_stride[0] != 0U) {
        layout.minimum_stride[0] *= w;
    }
    return layout;
}

} // namespace

namespace detail {

class CaptureCore;

struct BufferStorage {
    DmabufImage image;
    std::vector<UniqueFd> fds;
};

struct FrameLeaseState {
    std::weak_ptr<CaptureCore> owner;
    std::shared_ptr<BufferStorage> storage;
    std::size_t slot = 0;
    std::uint64_t sequence = 0;
    std::atomic<bool> released{false};

    void release() noexcept;
};

class CaptureCore : public std::enable_shared_from_this<CaptureCore> {
public:
    explicit CaptureCore(CaptureConfig config)
        : config_(std::move(config)), rate_limit_(config_.max_fps),
          pts_rate_limit_(config_.max_fps) {
        const char* trace_path = std::getenv("FLUXREC_CAPTURE_SLOT_TRACE");
        if (trace_path != nullptr && trace_path[0] != '\0') {
            slot_trace_.open(trace_path, std::ios::out | std::ios::trunc);
            if (!slot_trace_) {
                std::fprintf(stderr, "capture slot trace: could not open '%s': %s\n",
                             trace_path, std::strerror(errno));
            }
        }
    }
    ~CaptureCore() { shutdown(); }

    [[nodiscard]] std::expected<void, CaptureError> initialize();
    [[nodiscard]] std::expected<void, CaptureError> set_buffer_pool(
        std::span<const DmabufImage> images);
    [[nodiscard]] std::expected<void, CaptureError> start();
    void stop() noexcept;

    [[nodiscard]] const CaptureRequirements& requirements() const noexcept { return requirements_; }
    [[nodiscard]] std::optional<CapturedFrame> try_pop_frame();
    [[nodiscard]] std::optional<CapturedFrame> wait_pop_frame(std::chrono::milliseconds timeout);
    [[nodiscard]] CaptureStatus status() const noexcept { return status_.load(); }
    [[nodiscard]] std::optional<CaptureError> runtime_error() const;
    [[nodiscard]] std::uint64_t captured_frames() const noexcept { return captured_frames_.load(); }
    [[nodiscard]] std::uint64_t dropped_frames() const noexcept { return dropped_frames_.load(); }
    [[nodiscard]] std::uint64_t capture_failures() const noexcept { return capture_failures_.load(); }
    [[nodiscard]] CaptureStats stats() const;
    [[nodiscard]] std::size_t pool_size() const noexcept { return pool_size_count_.load(); }

    void release_slot(std::size_t slot, std::uint64_t sequence) noexcept;

private:
    enum class SlotState : std::uint8_t { free, capturing, leased };

    struct OutputRecord {
        CaptureCore* owner = nullptr;
        std::uint32_t global_name = 0;
        std::uint32_t version = 0;
        wl_output* proxy = nullptr;
        std::string name;
        bool removed = false;
    };

    struct BufferSlot {
        std::shared_ptr<BufferStorage> storage;
        wl_buffer* proxy = nullptr;
        SlotState state = SlotState::free;
        // Sequence of the lease that currently owns this slot. A release may
        // free the slot only when this still matches, so a stale release cannot
        // reclaim a slot that has already been leased again.
        std::uint64_t lease_sequence = 0;
    };

    struct ReleasedSlot {
        std::size_t slot = 0;
        std::uint64_t sequence = 0;
    };

    struct PendingBuffer {
        zwp_linux_buffer_params_v1* params = nullptr;
        wl_buffer* buffer = nullptr;
        bool failed = false;
    };

    struct InFlightFrame {
        ext_image_copy_capture_session_v1* session = nullptr;
        ext_image_copy_capture_frame_v1* proxy = nullptr;
        std::size_t slot = 0;
        std::chrono::steady_clock::time_point request_started_at;
        std::optional<std::uint64_t> presentation_time_ns;
        std::optional<std::uint32_t> transform;
        std::vector<DamageRect> damage;
        // Hyprland starts the DMA-BUF copy on output commit and ignores later
        // commits for that frame until the copy finishes. presentation_time
        // arrives at copy start, before ready. Once this is set, another
        // session may be armed so the next commit is not dropped.
        bool copy_started = false;
    };

    static void registry_global(void* data, wl_registry* registry, std::uint32_t name,
                                const char* interface, std::uint32_t version);
    static void registry_global_remove(void* data, wl_registry* registry, std::uint32_t name);
    static void output_geometry(void*, wl_output*, std::int32_t, std::int32_t,
                                std::int32_t, std::int32_t, std::int32_t,
                                const char*, const char*, std::int32_t);
    static void output_mode(void*, wl_output*, std::uint32_t, std::int32_t,
                            std::int32_t, std::int32_t);
    static void output_done(void*, wl_output*);
    static void output_scale(void*, wl_output*, std::int32_t);
    static void output_name(void* data, wl_output*, const char* name);
    static void output_description(void*, wl_output*, const char*);
    static void session_buffer_size(void* data, ext_image_copy_capture_session_v1*,
                                    std::uint32_t width, std::uint32_t height);
    static void session_shm_format(void*, ext_image_copy_capture_session_v1*, std::uint32_t);
    static void session_dmabuf_device(void* data, ext_image_copy_capture_session_v1*, wl_array* device);
    static void session_dmabuf_format(void* data, ext_image_copy_capture_session_v1*,
                                      std::uint32_t format, wl_array* modifiers);
    static void session_done(void* data, ext_image_copy_capture_session_v1*);
    static void session_stopped(void* data, ext_image_copy_capture_session_v1*);
    static void frame_transform(void* data, ext_image_copy_capture_frame_v1*, std::uint32_t transform);
    static void frame_damage(void* data, ext_image_copy_capture_frame_v1*, std::int32_t x,
                             std::int32_t y, std::int32_t width, std::int32_t height);
    static void frame_presentation_time(void* data, ext_image_copy_capture_frame_v1*,
                                       std::uint32_t sec_hi, std::uint32_t sec_lo,
                                       std::uint32_t nsec);
    static void frame_ready(void* data, ext_image_copy_capture_frame_v1*);
    static void frame_failed(void* data, ext_image_copy_capture_frame_v1*, std::uint32_t reason);
    static void buffer_created(void* data, zwp_linux_buffer_params_v1*, wl_buffer* buffer);
    static void buffer_failed(void* data, zwp_linux_buffer_params_v1*);

    static const wl_registry_listener registry_listener_;
    static const wl_output_listener output_listener_;
    static const ext_image_copy_capture_session_v1_listener session_listener_;
    static const ext_image_copy_capture_frame_v1_listener frame_listener_;
    static const zwp_linux_buffer_params_v1_listener buffer_listener_;

    [[nodiscard]] CaptureError make_error(std::string message, int system_error = 0) const;
    [[nodiscard]] bool roundtrip(std::string_view operation, CaptureError& error);
    [[nodiscard]] std::expected<std::shared_ptr<BufferStorage>, CaptureError> own_image(
        const DmabufImage& image) const;
    [[nodiscard]] bool supported(const DmabufImage& image) const noexcept;
    [[nodiscard]] bool validate_constraints(const CaptureRequirements& value) const;
    void begin_constraint_batch();
    void on_constraints_done();
    void on_frame_ready(ext_image_copy_capture_frame_v1* proxy);
    void on_frame_failed(ext_image_copy_capture_frame_v1* proxy, std::uint32_t reason);
    [[nodiscard]] InFlightFrame* find_in_flight(ext_image_copy_capture_frame_v1* proxy) noexcept;
    void erase_in_flight(ext_image_copy_capture_frame_v1* proxy) noexcept;
    void destroy_in_flight_frames(bool count_cancelled) noexcept;
    void record_request_to_ready(std::uint64_t nanoseconds) noexcept;
    void begin_pool_starvation() noexcept;
    void end_pool_starvation() noexcept;
    void transition_slot(BufferSlot& slot, SlotState next) noexcept;
    void set_failure(std::string message, int system_error = 0) noexcept;
    [[nodiscard]] bool check_display(std::string_view operation) noexcept;
    void wake_worker() noexcept;
    void wake_worker_locked() noexcept;
    void process_released_slots();
    void trace_ready(std::uint64_t sequence, std::size_t slot,
                     bool has_presentation_time, std::uint64_t presentation_time_ns) noexcept;
    void trace_lease_release(std::uint64_t sequence, std::size_t slot) noexcept;
    void trace_slot_free(std::uint64_t sequence, std::size_t slot) noexcept;
    void schedule_capture(bool overlap_arm = false);
    void run_worker() noexcept;
    void cleanup_wayland() noexcept;
    void shutdown() noexcept;
    [[nodiscard]] std::optional<std::chrono::steady_clock::duration>
    timeout_until_next_action() const noexcept;
    void drain_wake_fd() noexcept;

    CaptureConfig config_;
    CaptureRateLimit rate_limit_;
    CapturePtsRateLimit pts_rate_limit_;
    wl_display* display_ = nullptr;
    wl_registry* registry_ = nullptr;
    zwp_linux_dmabuf_v1* linux_dmabuf_ = nullptr;
    ext_image_capture_source_v1* source_ = nullptr;
    ext_output_image_capture_source_manager_v1* source_manager_ = nullptr;
    ext_image_copy_capture_manager_v1* capture_manager_ = nullptr;
    // ext-image-copy-capture allows one frame object per session. Hyprland
    // copies that frame on output commit and, while the copy is still in
    // flight, drops later commits. Overlapping sessions keep one request
    // waiting for the next commit while another copy finishes. Only one
    // session is waiting at a time, so one commit is not copied twice.
    static constexpr std::size_t kOverlappedCaptureSessions = 3;
    std::vector<ext_image_copy_capture_session_v1*> sessions_;
    std::size_t constraints_completed_ = 0;
    std::vector<std::unique_ptr<OutputRecord>> outputs_;
    std::uint32_t linux_dmabuf_global_ = 0;
    std::uint32_t source_manager_global_ = 0;
    std::uint32_t capture_manager_global_ = 0;
    std::uint32_t selected_output_global_ = 0;
    wl_output* selected_output_ = nullptr;

    CaptureRequirements requirements_;
    CaptureRequirements pending_requirements_;
    bool constraint_batch_open_ = false;
    bool pending_buffer_size_seen_ = false;
    bool initial_constraints_received_ = false;
    std::vector<BufferSlot> slots_;
    std::size_t queue_capacity_ = 0;
    std::vector<std::unique_ptr<InFlightFrame>> in_flight_;
    std::uint64_t next_sequence_ = 1;
    unsigned int consecutive_failures_ = 0;
    std::optional<std::chrono::steady_clock::time_point> retry_at_;

    mutable std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<CapturedFrame> frame_queue_;
    mutable std::mutex error_mutex_;
    std::optional<CaptureError> error_;
    mutable std::mutex release_mutex_;
    std::vector<ReleasedSlot> released_slots_;
    std::mutex slot_trace_mutex_;
    std::ofstream slot_trace_;
    UniqueFd wake_fd_;
    std::thread worker_;
    std::atomic<CaptureStatus> status_{CaptureStatus::connected};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> worker_active_{false};
    std::atomic<std::uint64_t> captured_frames_{0};
    std::atomic<std::uint64_t> admitted_frames_{0};
    std::atomic<std::uint64_t> rate_limited_frames_{0};
    std::atomic<std::uint64_t> dropped_frames_{0};
    std::atomic<std::uint64_t> overlap_arm_failures_{0};
    std::atomic<std::uint64_t> capture_failures_{0};
    std::atomic<std::uint64_t> capture_requests_{0};
    std::atomic<std::uint64_t> cancelled_at_stop_{0};
    std::atomic<std::size_t> pool_size_count_{0};
    mutable std::mutex pool_metrics_mutex_;
    std::size_t pool_free_slots_ = 0;
    std::size_t pool_capturing_slots_ = 0;
    std::size_t pool_leased_slots_ = 0;
    mutable std::mutex latency_mutex_;
    std::array<std::uint64_t, 512> latency_history_{};
    std::size_t latency_history_count_ = 0;
    std::size_t latency_history_next_ = 0;
    std::uint64_t latency_sample_count_ = 0;
    std::uint64_t latency_total_ns_ = 0;
    std::uint64_t latency_last_ns_ = 0;
    std::uint64_t latency_max_ns_ = 0;
    std::atomic<std::uint64_t> pool_starvation_periods_{0};
    std::atomic<std::uint64_t> pool_starvation_total_ns_{0};
    std::atomic<std::int64_t> pool_starvation_started_ns_{-1};
};

const wl_registry_listener CaptureCore::registry_listener_{
    .global = CaptureCore::registry_global,
    .global_remove = CaptureCore::registry_global_remove,
};

const wl_output_listener CaptureCore::output_listener_{
    .geometry = CaptureCore::output_geometry,
    .mode = CaptureCore::output_mode,
    .done = CaptureCore::output_done,
    .scale = CaptureCore::output_scale,
    .name = CaptureCore::output_name,
    .description = CaptureCore::output_description,
};

const ext_image_copy_capture_session_v1_listener CaptureCore::session_listener_{
    .buffer_size = CaptureCore::session_buffer_size,
    .shm_format = CaptureCore::session_shm_format,
    .dmabuf_device = CaptureCore::session_dmabuf_device,
    .dmabuf_format = CaptureCore::session_dmabuf_format,
    .done = CaptureCore::session_done,
    .stopped = CaptureCore::session_stopped,
};

const ext_image_copy_capture_frame_v1_listener CaptureCore::frame_listener_{
    .transform = CaptureCore::frame_transform,
    .damage = CaptureCore::frame_damage,
    .presentation_time = CaptureCore::frame_presentation_time,
    .ready = CaptureCore::frame_ready,
    .failed = CaptureCore::frame_failed,
};

const zwp_linux_buffer_params_v1_listener CaptureCore::buffer_listener_{
    .created = CaptureCore::buffer_created,
    .failed = CaptureCore::buffer_failed,
};

void FrameLeaseState::release() noexcept {
    if (released.exchange(true)) {
        return;
    }
    if (auto capture = owner.lock()) {
        capture->release_slot(slot, sequence);
    }
    storage.reset();
}

CaptureError CaptureCore::make_error(std::string message, int system_error) const {
    if (system_error != 0) {
        message.append(": ");
        message.append(std::strerror(system_error));
    }
    return CaptureError{.message = std::move(message), .system_error = system_error};
}

bool CaptureCore::roundtrip(std::string_view operation, CaptureError& error) {
    if (wl_display_roundtrip(display_) >= 0) {
        return true;
    }
    const int display_error = wl_display_get_error(display_);
    error = make_error(std::string(operation) + " failed: Wayland roundtrip", display_error != 0
                                                                            ? display_error
                                                                            : errno);
    return false;
}

bool CaptureCore::validate_constraints(const CaptureRequirements& value) const {
    return value.width > 0 && value.height > 0 && !value.dmabuf_formats.empty();
}

std::expected<void, CaptureError> CaptureCore::initialize() {
    if (config_.max_fps > 1'000'000'000U) {
        return std::unexpected(make_error(
            "max_fps exceeds the nanosecond scheduling clock's supported precision"));
    }

    display_ = wl_display_connect(nullptr);
    if (display_ == nullptr) {
        return std::unexpected(make_error("could not connect to the Wayland display", errno));
    }

    registry_ = wl_display_get_registry(display_);
    if (registry_ == nullptr || wl_registry_add_listener(registry_, &registry_listener_, this) < 0) {
        return std::unexpected(make_error("could not create the Wayland registry", errno));
    }

    CaptureError error;
    if (!roundtrip("Wayland registry discovery", error)) {
        return std::unexpected(std::move(error));
    }

    if (!roundtrip("Wayland output name discovery", error)) {
        return std::unexpected(std::move(error));
    }

    if (linux_dmabuf_ == nullptr || source_manager_ == nullptr || capture_manager_ == nullptr) {
        return std::unexpected(make_error(
            "compositor must advertise linux-dmabuf-v1, ext-image-capture-source-v1, and ext-image-copy-capture-v1"));
    }

    for (const auto& output : outputs_) {
        if (!output->removed && output->name == config_.output_name) {
            selected_output_ = output->proxy;
            selected_output_global_ = output->global_name;
            break;
        }
    }
    if (selected_output_ == nullptr) {
        const auto unnamed_output = std::find_if(outputs_.begin(), outputs_.end(), [](const auto& output) {
            return !output->removed && output->name.empty() && output->version < 4U;
        });
        if (unnamed_output != outputs_.end()) {
            return std::unexpected(make_error("wl_output v4 is required to select an output by name"));
        }
        return std::unexpected(make_error("requested Wayland output was not found: " + config_.output_name));
    }

    source_ = ext_output_image_capture_source_manager_v1_create_source(
        source_manager_, selected_output_);
    if (source_ == nullptr) {
        return std::unexpected(make_error("could not create an image capture source for output " + config_.output_name));
    }

    const std::uint32_t options = config_.paint_cursors
        ? static_cast<std::uint32_t>(EXT_IMAGE_COPY_CAPTURE_MANAGER_V1_OPTIONS_PAINT_CURSORS)
        : 0U;
    sessions_.reserve(kOverlappedCaptureSessions);
    for (std::size_t index = 0; index < kOverlappedCaptureSessions; ++index) {
        ext_image_copy_capture_session_v1* session =
            ext_image_copy_capture_manager_v1_create_session(capture_manager_, source_, options);
        if (session == nullptr || ext_image_copy_capture_session_v1_add_listener(
                                      session, &session_listener_, this) < 0) {
            return std::unexpected(make_error("could not create an ext-image-copy-capture session"));
        }
        sessions_.push_back(session);
        if (!roundtrip("capture session negotiation", error)) {
            return std::unexpected(std::move(error));
        }
        if (status_.load() == CaptureStatus::failed) {
            return std::unexpected(runtime_error().value_or(make_error("capture session negotiation failed")));
        }
        if (constraints_completed_ < sessions_.size()) {
            return std::unexpected(make_error(
                "capture session did not finish advertising DMA-BUF constraints"));
        }
    }
    if (!initial_constraints_received_ || !validate_constraints(requirements_)) {
        return std::unexpected(make_error(
            "capture session did not advertise a buffer size and at least one DMA-BUF format/modifier"));
    }

    return {};
}

std::expected<std::shared_ptr<BufferStorage>, CaptureError> CaptureCore::own_image(
    const DmabufImage& image) const {
    if (image.width != requirements_.width || image.height != requirements_.height) {
        return std::unexpected(make_error("DMA-BUF dimensions do not match the capture session"));
    }
    if (!supported(image)) {
        return std::unexpected(make_error("DMA-BUF fourcc/modifier was not advertised by the capture session"));
    }
    if (image.width > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()) ||
        image.height > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())) {
        return std::unexpected(make_error("DMA-BUF dimensions exceed the Wayland protocol range"));
    }
    const std::optional<PlaneLayout> layout = describe_format(image.fourcc, image.width, image.height);
    if (!layout) {
        return std::unexpected(make_error("DMA-BUF fourcc has no validated plane layout in this capture module"));
    }
    if (image.planes.size() != layout->count) {
        return std::unexpected(make_error("DMA-BUF plane count does not match its DRM fourcc"));
    }

    auto storage = std::make_shared<BufferStorage>();
    storage->image = image;
    storage->fds.reserve(image.planes.size());
    std::array<std::uint64_t, 4> plane_end{};
    for (std::size_t index = 0; index < image.planes.size(); ++index) {
        const DmabufPlane& plane = image.planes[index];
        if (plane.fd < 0 || plane.stride == 0) {
            return std::unexpected(make_error("DMA-BUF plane has an invalid fd or zero stride"));
        }
        if (plane.stride < layout->minimum_stride[index]) {
            return std::unexpected(make_error("DMA-BUF plane stride is too small for its fourcc and dimensions"));
        }
        const std::uint64_t offset = plane.offset;
        const std::uint64_t rows = layout->rows[index];
        if (rows == 0U ||
            static_cast<std::uint64_t>(plane.stride) >
                (std::numeric_limits<std::uint64_t>::max() - offset) / rows) {
            return std::unexpected(make_error("DMA-BUF plane offset/stride extent overflows"));
        }
        plane_end[index] = offset + static_cast<std::uint64_t>(plane.stride) * rows;
        const off_t saved_offset = ::lseek(plane.fd, 0, SEEK_CUR);
        const off_t allocation_size = ::lseek(plane.fd, 0, SEEK_END);
        if (saved_offset >= 0) {
            (void)::lseek(plane.fd, saved_offset, SEEK_SET);
        }
        if (allocation_size >= 0 &&
            plane_end[index] > static_cast<std::uint64_t>(allocation_size)) {
            return std::unexpected(make_error("DMA-BUF plane extent exceeds the exported allocation size"));
        }
        for (std::size_t previous = 0; previous < index; ++previous) {
            if (image.planes[previous].fd == plane.fd &&
                offset < plane_end[previous] &&
                static_cast<std::uint64_t>(image.planes[previous].offset) < plane_end[index]) {
                return std::unexpected(make_error("DMA-BUF planes in the same fd overlap"));
            }
        }
        const int copied_fd = ::fcntl(plane.fd, F_DUPFD_CLOEXEC, 0);
        if (copied_fd < 0) {
            return std::unexpected(make_error("could not duplicate DMA-BUF plane fd", errno));
        }
        storage->fds.emplace_back(copied_fd);
        storage->image.planes[index].fd = copied_fd;
    }
    return storage;
}

bool CaptureCore::supported(const DmabufImage& image) const noexcept {
    return std::any_of(requirements_.dmabuf_formats.begin(), requirements_.dmabuf_formats.end(),
                       [&image](const FormatModifier& entry) {
                           return entry.fourcc == image.fourcc && entry.modifier == image.modifier;
                       });
}

std::expected<void, CaptureError> CaptureCore::set_buffer_pool(
    std::span<const DmabufImage> images) {
    if (status_.load() != CaptureStatus::connected || worker_.joinable()) {
        return std::unexpected(make_error("the DMA-BUF pool can only be set before capture starts"));
    }
    if (!slots_.empty()) {
        return std::unexpected(make_error("the DMA-BUF pool has already been set"));
    }
    if (images.empty()) {
        return std::unexpected(make_error("the DMA-BUF pool must contain at least one image"));
    }

    std::vector<std::shared_ptr<BufferStorage>> storages;
    storages.reserve(images.size());
    for (const DmabufImage& image : images) {
        auto storage = own_image(image);
        if (!storage) {
            return std::unexpected(storage.error());
        }
        storages.push_back(std::move(*storage));
    }

    std::vector<PendingBuffer> pending(images.size());
    for (std::size_t index = 0; index < storages.size(); ++index) {
        PendingBuffer& item = pending[index];
        item.params = zwp_linux_dmabuf_v1_create_params(linux_dmabuf_);
        if (item.params == nullptr) {
            for (PendingBuffer& cleanup : pending) {
                if (cleanup.params != nullptr) {
                    zwp_linux_buffer_params_v1_destroy(cleanup.params);
                }
                if (cleanup.buffer != nullptr) {
                    wl_buffer_destroy(cleanup.buffer);
                }
            }
            return std::unexpected(make_error("could not create linux-dmabuf buffer parameters"));
        }
        if (zwp_linux_buffer_params_v1_add_listener(item.params, &buffer_listener_, &item) < 0) {
            for (PendingBuffer& cleanup : pending) {
                if (cleanup.params != nullptr) {
                    zwp_linux_buffer_params_v1_destroy(cleanup.params);
                }
            }
            return std::unexpected(make_error("could not listen for linux-dmabuf buffer import"));
        }

        const DmabufImage& image = storages[index]->image;
        const std::uint32_t modifier_hi = static_cast<std::uint32_t>(image.modifier >> 32U);
        const std::uint32_t modifier_lo = static_cast<std::uint32_t>(image.modifier & 0xffffffffULL);
        for (std::size_t plane_index = 0; plane_index < image.planes.size(); ++plane_index) {
            const DmabufPlane& plane = image.planes[plane_index];
            zwp_linux_buffer_params_v1_add(
                item.params, plane.fd, static_cast<std::uint32_t>(plane_index), plane.offset,
                plane.stride, modifier_hi, modifier_lo);
        }
    }
    for (std::size_t index = 0; index < storages.size(); ++index) {
        PendingBuffer& item = pending[index];
        const DmabufImage& image = storages[index]->image;
        zwp_linux_buffer_params_v1_create(
            item.params, static_cast<std::int32_t>(image.width),
            static_cast<std::int32_t>(image.height), image.fourcc, 0);
    }

    CaptureError error;
    if (!roundtrip("DMA-BUF wl_buffer import", error)) {
        for (PendingBuffer& item : pending) {
            if (item.params != nullptr) {
                zwp_linux_buffer_params_v1_destroy(item.params);
            }
            if (item.buffer != nullptr) {
                wl_buffer_destroy(item.buffer);
            }
        }
        set_failure(error.message, error.system_error);
        return std::unexpected(std::move(error));
    }
    if (status_.load() == CaptureStatus::failed) {
        for (PendingBuffer& item : pending) {
            if (item.params != nullptr) {
                zwp_linux_buffer_params_v1_destroy(item.params);
            }
            if (item.buffer != nullptr) {
                wl_buffer_destroy(item.buffer);
            }
        }
        return std::unexpected(runtime_error().value_or(
            make_error("capture constraints changed while importing the DMA-BUF pool")));
    }

    bool any_failed = false;
    for (PendingBuffer& item : pending) {
        if (item.params != nullptr) {
            zwp_linux_buffer_params_v1_destroy(item.params);
            item.params = nullptr;
        }
        any_failed = any_failed || item.failed || item.buffer == nullptr;
    }
    if (any_failed) {
        for (PendingBuffer& item : pending) {
            if (item.buffer != nullptr) {
                wl_buffer_destroy(item.buffer);
            }
        }
        return std::unexpected(make_error(
            "compositor rejected one or more DMA-BUFs while importing the capture pool"));
    }

    slots_.reserve(storages.size());
    for (std::size_t index = 0; index < storages.size(); ++index) {
        slots_.push_back(BufferSlot{
            .storage = std::move(storages[index]),
            .proxy = pending[index].buffer,
            .state = SlotState::free,
        });
    }
    queue_capacity_ = config_.frame_queue_capacity == 0
        ? slots_.size()
        : std::min(config_.frame_queue_capacity, slots_.size());
    released_slots_.reserve(slots_.size());
    {
        std::lock_guard lock(pool_metrics_mutex_);
        pool_size_count_.store(slots_.size());
        pool_free_slots_ = slots_.size();
        pool_capturing_slots_ = 0;
        pool_leased_slots_ = 0;
    }
    return {};
}

std::expected<void, CaptureError> CaptureCore::start() {
    if (status_.load() != CaptureStatus::connected) {
        return std::unexpected(make_error("capture is not in the connected state"));
    }
    if (slots_.empty()) {
        return std::unexpected(make_error("set a DMA-BUF pool before starting capture"));
    }

    // GPU setup can take long enough for the selected monitor or compositor to
    // disappear after pool import. Process queued Wayland events before the
    // worker can submit its first capture request against a stale source.
    CaptureError error;
    if (!roundtrip("capture start validation", error)) {
        set_failure(error.message, error.system_error);
        return std::unexpected(std::move(error));
    }
    if (status_.load() != CaptureStatus::connected) {
        return std::unexpected(runtime_error().value_or(
            make_error("capture source became unavailable before capture start")));
    }

    const int wake = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (wake < 0) {
        return std::unexpected(make_error("could not create the capture wake eventfd", errno));
    }
    wake_fd_.reset(wake);
    stop_requested_.store(false);
    status_.store(CaptureStatus::running);
    worker_active_.store(true);
    try {
        const std::shared_ptr<CaptureCore> self = shared_from_this();
        worker_ = std::thread([self] { self->run_worker(); });
    } catch (const std::exception& exception) {
        {
            std::lock_guard lock(release_mutex_);
            worker_active_.store(false);
            wake_fd_.reset();
        }
        status_.store(CaptureStatus::connected);
        return std::unexpected(make_error(std::string("could not start capture event thread: ") + exception.what()));
    }
    return {};
}

void CaptureCore::release_slot(std::size_t slot, std::uint64_t sequence) noexcept {
    trace_lease_release(sequence, slot);
    std::lock_guard lock(release_mutex_);
    if (!worker_active_.load()) {
        if (slot < pool_size_count_.load()) {
            std::lock_guard metrics_lock(pool_metrics_mutex_);
            if (pool_leased_slots_ != 0U) {
                --pool_leased_slots_;
            }
        }
        return;
    }
    released_slots_.push_back(ReleasedSlot{.slot = slot, .sequence = sequence});
    wake_worker_locked();
}

void CaptureCore::wake_worker() noexcept {
    std::lock_guard lock(release_mutex_);
    if (worker_active_.load()) {
        wake_worker_locked();
    }
}

void CaptureCore::wake_worker_locked() noexcept {
    if (!wake_fd_) {
        return;
    }
    const std::uint64_t one = 1;
    const ssize_t result = ::write(wake_fd_.get(), &one, sizeof(one));
    (void)result;
}

void CaptureCore::transition_slot(BufferSlot& slot, SlotState next) noexcept {
    std::lock_guard lock(pool_metrics_mutex_);
    std::size_t* previous_count = nullptr;
    switch (slot.state) {
    case SlotState::free:
        previous_count = &pool_free_slots_;
        break;
    case SlotState::capturing:
        previous_count = &pool_capturing_slots_;
        break;
    case SlotState::leased:
        previous_count = &pool_leased_slots_;
        break;
    }
    std::size_t* next_count = nullptr;
    switch (next) {
    case SlotState::free:
        next_count = &pool_free_slots_;
        break;
    case SlotState::capturing:
        next_count = &pool_capturing_slots_;
        break;
    case SlotState::leased:
        next_count = &pool_leased_slots_;
        break;
    }
    if (slot.state == next) {
        return;
    }
    if (previous_count != nullptr && *previous_count != 0U) {
        --*previous_count;
    }
    if (next_count != nullptr) {
        ++*next_count;
    }
    slot.state = next;
}

void CaptureCore::process_released_slots() {
    std::vector<ReleasedSlot> released;
    {
        std::lock_guard lock(release_mutex_);
        released.swap(released_slots_);
    }
    for (const ReleasedSlot released_slot : released) {
        if (released_slot.slot < slots_.size() &&
            slots_[released_slot.slot].state == SlotState::leased &&
            slots_[released_slot.slot].lease_sequence == released_slot.sequence) {
            transition_slot(slots_[released_slot.slot], SlotState::free);
            trace_slot_free(released_slot.sequence, released_slot.slot);
        }
    }
}

void CaptureCore::trace_ready(std::uint64_t sequence, std::size_t slot,
                              bool has_presentation_time,
                              std::uint64_t presentation_time_ns) noexcept {
    if (!slot_trace_.is_open()) return;
    std::lock_guard lock(slot_trace_mutex_);
    slot_trace_ << "{\"event\":\"ready\",\"sequence\":" << sequence
                << ",\"slot_index\":" << slot << ",\"presentation_time_ns\":";
    if (has_presentation_time) {
        slot_trace_ << presentation_time_ns;
    } else {
        slot_trace_ << "null";
    }
    slot_trace_ << "}\n";
}

void CaptureCore::trace_lease_release(std::uint64_t sequence, std::size_t slot) noexcept {
    if (!slot_trace_.is_open()) return;
    std::lock_guard lock(slot_trace_mutex_);
    slot_trace_ << "{\"event\":\"lease_release\",\"sequence\":" << sequence
                << ",\"slot_index\":" << slot << "}\n";
}

void CaptureCore::trace_slot_free(std::uint64_t sequence, std::size_t slot) noexcept {
    if (!slot_trace_.is_open()) return;
    std::lock_guard lock(slot_trace_mutex_);
    slot_trace_ << "{\"event\":\"slot_free\",\"sequence\":" << sequence
                << ",\"slot_index\":" << slot << "}\n";
}

CaptureCore::InFlightFrame* CaptureCore::find_in_flight(
    ext_image_copy_capture_frame_v1* proxy) noexcept {
    for (const auto& frame : in_flight_) {
        if (frame && frame->proxy == proxy) {
            return frame.get();
        }
    }
    return nullptr;
}

void CaptureCore::erase_in_flight(ext_image_copy_capture_frame_v1* proxy) noexcept {
    std::erase_if(in_flight_, [&](const std::unique_ptr<InFlightFrame>& frame) {
        return frame && frame->proxy == proxy;
    });
}

void CaptureCore::destroy_in_flight_frames(bool count_cancelled) noexcept {
    for (auto& frame : in_flight_) {
        if (!frame) continue;
        if (frame->proxy != nullptr) {
            ext_image_copy_capture_frame_v1_destroy(frame->proxy);
            frame->proxy = nullptr;
            if (count_cancelled && stop_requested_.load() &&
                status_.load() == CaptureStatus::running) {
                cancelled_at_stop_.fetch_add(1);
            }
        }
        if (frame->slot < slots_.size() && slots_[frame->slot].state == SlotState::capturing) {
            transition_slot(slots_[frame->slot], SlotState::free);
        }
    }
    in_flight_.clear();
}

void CaptureCore::schedule_capture(bool overlap_arm) {
    if (status_.load() != CaptureStatus::running || stop_requested_.load()) {
        return;
    }
    const bool waiting_for_copy = std::any_of(in_flight_.begin(), in_flight_.end(),
        [](const std::unique_ptr<InFlightFrame>& frame) {
            return frame && !frame->copy_started;
        });
    if (waiting_for_copy) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (retry_at_ && now < *retry_at_) {
        return;
    }
    retry_at_.reset();

    const auto free_count = static_cast<std::size_t>(std::count_if(
        slots_.begin(), slots_.end(), [](const BufferSlot& slot) {
            return slot.state == SlotState::free;
        }));
    // A normal request must leave one free slot so the copy-start path can
    // arm its successor. The overlap arm is allowed to take the last slot.
    // Without that spare slot, Hyprland drops the next commit while this
    // copy is in flight.
    if (free_count < (overlap_arm ? 1U : 2U)) {
        begin_pool_starvation();
        return;
    }
    end_pool_starvation();
    // At and above the native target cadence, request immediately and let the
    // presentation-time admission bucket enforce the configured output cap.
    // This avoids sending requests just after a source presentation and
    // missing that frame while waiting for a timer deadline.
    const bool request_pacing_enabled = config_.max_fps > 0U && config_.max_fps < 360U;
    if (request_pacing_enabled && !rate_limit_.can_start(now)) {
        return;
    }
    const auto available = std::find_if(slots_.begin(), slots_.end(), [](const BufferSlot& slot) {
        return slot.state == SlotState::free;
    });
    if (available == slots_.end()) {
        begin_pool_starvation();
        return;
    }
    ext_image_copy_capture_session_v1* session = nullptr;
    for (ext_image_copy_capture_session_v1* candidate : sessions_) {
        const bool busy = std::any_of(in_flight_.begin(), in_flight_.end(),
            [&](const std::unique_ptr<InFlightFrame>& frame) {
                return frame && frame->session == candidate;
            });
        if (!busy) {
            session = candidate;
            break;
        }
    }
    if (session == nullptr) {
        return;
    }
    const std::size_t slot_index = static_cast<std::size_t>(std::distance(slots_.begin(), available));
    auto frame = std::make_unique<InFlightFrame>();
    frame->session = session;
    frame->slot = slot_index;
    frame->proxy = ext_image_copy_capture_session_v1_create_frame(session);
    if (frame->proxy == nullptr || ext_image_copy_capture_frame_v1_add_listener(
                                       frame->proxy, &frame_listener_, this) < 0) {
        set_failure("could not create an ext-image-copy-capture frame");
        return;
    }

    transition_slot(*available, SlotState::capturing);
    ext_image_copy_capture_frame_v1_attach_buffer(frame->proxy, slots_[slot_index].proxy);
    ext_image_copy_capture_frame_v1_damage_buffer(
        frame->proxy, 0, 0, static_cast<std::int32_t>(requirements_.width),
        static_cast<std::int32_t>(requirements_.height));
    frame->request_started_at = std::chrono::steady_clock::now();
    capture_requests_.fetch_add(1);
    ext_image_copy_capture_frame_v1_capture(frame->proxy);
    in_flight_.push_back(std::move(frame));
    rate_limit_.request_started(std::chrono::steady_clock::now());
}

std::optional<std::chrono::steady_clock::duration>
CaptureCore::timeout_until_next_action() const noexcept {
    std::optional<std::chrono::steady_clock::time_point> deadline = retry_at_;
    const bool request_pacing_enabled = config_.max_fps > 0U && config_.max_fps < 360U;
    if (const auto rate_deadline = rate_limit_.deadline();
        request_pacing_enabled && rate_deadline && (!deadline || *rate_deadline > *deadline)) {
        deadline = rate_deadline;
    }
    if (!deadline || !in_flight_.empty() || status_.load() != CaptureStatus::running ||
        stop_requested_.load() || std::none_of(slots_.begin(), slots_.end(), [](const BufferSlot& slot) {
            return slot.state == SlotState::free;
        })) {
        return std::nullopt;
    }
    const auto remaining = *deadline - std::chrono::steady_clock::now();
    if (remaining <= std::chrono::steady_clock::duration::zero()) {
        return std::chrono::steady_clock::duration::zero();
    }
    return remaining;
}

void CaptureCore::drain_wake_fd() noexcept {
    std::uint64_t value = 0;
    while (::read(wake_fd_.get(), &value, sizeof(value)) == static_cast<ssize_t>(sizeof(value))) {
    }
}

void CaptureCore::run_worker() noexcept {
    try {
        while (!stop_requested_.load() && status_.load() == CaptureStatus::running) {
            process_released_slots();
            if (status_.load() != CaptureStatus::running) {
                break;
            }
            schedule_capture();
            if (status_.load() != CaptureStatus::running) {
                break;
            }

            while (wl_display_prepare_read(display_) != 0) {
                if (wl_display_dispatch_pending(display_) < 0) {
                    set_failure("Wayland event dispatch failed", wl_display_get_error(display_));
                    break;
                }
            }
            if (status_.load() != CaptureStatus::running) {
                break;
            }

            bool want_write = false;
            if (wl_display_flush(display_) < 0) {
                if (errno == EAGAIN) {
                    want_write = true;
                } else {
                    wl_display_cancel_read(display_);
                    set_failure("Wayland request flush failed", errno);
                    break;
                }
            }

            std::array<pollfd, 2> descriptors{};
            descriptors[0].fd = wl_display_get_fd(display_);
            descriptors[0].events = static_cast<short>(POLLIN | (want_write ? POLLOUT : 0));
            descriptors[1].fd = wake_fd_.get();
            descriptors[1].events = POLLIN;
            const auto wait_duration = timeout_until_next_action();
            const ::timespec wait_timeout = wait_duration
                ? CaptureRateLimit::ppoll_timeout(*wait_duration)
                : ::timespec{};
            const int poll_result = ::ppoll(
                descriptors.data(), descriptors.size(),
                wait_duration ? &wait_timeout : nullptr, nullptr);
            if (poll_result < 0) {
                const int poll_error = errno;
                wl_display_cancel_read(display_);
                if (poll_error == EINTR) {
                    continue;
                }
                set_failure("capture poll failed", poll_error);
                break;
            }

            const bool readable = (descriptors[0].revents & POLLIN) != 0;
            if (readable) {
                if (wl_display_read_events(display_) < 0) {
                    set_failure("Wayland event read failed", wl_display_get_error(display_));
                    break;
                }
                if (wl_display_dispatch_pending(display_) < 0) {
                    set_failure("Wayland event dispatch failed", wl_display_get_error(display_));
                    break;
                }
            } else {
                wl_display_cancel_read(display_);
            }

            if ((descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                set_failure("Wayland display connection was closed");
                break;
            }
            if ((descriptors[1].revents & POLLIN) != 0) {
                drain_wake_fd();
            }
            if (status_.load() != CaptureStatus::running) {
                break;
            }
        }
    } catch (const std::exception& exception) {
        set_failure(std::string("capture event thread threw: ") + exception.what());
    } catch (...) {
        set_failure("capture event thread threw an unknown exception");
    }

    destroy_in_flight_frames(true);
    end_pool_starvation();
    std::vector<ReleasedSlot> released_at_exit;
    {
        std::lock_guard lock(release_mutex_);
        worker_active_.store(false);
        wake_fd_.reset();
        released_at_exit.swap(released_slots_);
    }
    for (const ReleasedSlot released_slot : released_at_exit) {
        if (released_slot.slot < slots_.size() &&
            slots_[released_slot.slot].state == SlotState::leased &&
            slots_[released_slot.slot].lease_sequence == released_slot.sequence) {
            transition_slot(slots_[released_slot.slot], SlotState::free);
            trace_slot_free(released_slot.sequence, released_slot.slot);
        }
    }
    cleanup_wayland();
    if (status_.load() != CaptureStatus::failed) {
        status_.store(CaptureStatus::stopped);
    }
    queue_cv_.notify_all();
}

void CaptureCore::cleanup_wayland() noexcept {
    destroy_in_flight_frames(false);
    for (ext_image_copy_capture_session_v1* session : sessions_) {
        if (session != nullptr) {
            ext_image_copy_capture_session_v1_destroy(session);
        }
    }
    sessions_.clear();
    for (BufferSlot& slot : slots_) {
        if (slot.proxy != nullptr) {
            wl_buffer_destroy(slot.proxy);
            slot.proxy = nullptr;
        }
    }
    if (source_ != nullptr) {
        ext_image_capture_source_v1_destroy(source_);
        source_ = nullptr;
    }
    if (capture_manager_ != nullptr) {
        ext_image_copy_capture_manager_v1_destroy(capture_manager_);
        capture_manager_ = nullptr;
    }
    if (source_manager_ != nullptr) {
        ext_output_image_capture_source_manager_v1_destroy(source_manager_);
        source_manager_ = nullptr;
    }
    for (const auto& output : outputs_) {
        if (output->proxy != nullptr) {
            wl_output_destroy(output->proxy);
            output->proxy = nullptr;
        }
    }
    selected_output_ = nullptr;
    if (linux_dmabuf_ != nullptr) {
        zwp_linux_dmabuf_v1_destroy(linux_dmabuf_);
        linux_dmabuf_ = nullptr;
    }
    if (registry_ != nullptr) {
        wl_registry_destroy(registry_);
        registry_ = nullptr;
    }
    if (display_ != nullptr) {
        (void)wl_display_flush(display_);
        wl_display_disconnect(display_);
        display_ = nullptr;
    }
    slots_.clear();
    {
        std::lock_guard lock(pool_metrics_mutex_);
        pool_free_slots_ = 0;
        pool_capturing_slots_ = 0;
    }
}

void CaptureCore::shutdown() noexcept {
    stop();
    if (display_ != nullptr) {
        cleanup_wayland();
    }
}

void CaptureCore::stop() noexcept {
    stop_requested_.store(true);
    wake_worker();
    if (worker_.joinable()) {
        if (worker_.get_id() == std::this_thread::get_id()) {
            worker_.detach();
            return;
        }
        worker_.join();
    } else if (display_ != nullptr) {
        {
            std::lock_guard lock(release_mutex_);
            worker_active_.store(false);
            wake_fd_.reset();
            released_slots_.clear();
        }
        cleanup_wayland();
        if (status_.load() != CaptureStatus::failed) {
            status_.store(CaptureStatus::stopped);
        }
        queue_cv_.notify_all();
    }
}

std::optional<CapturedFrame> CaptureCore::try_pop_frame() {
    std::lock_guard lock(queue_mutex_);
    if (frame_queue_.empty()) {
        return std::nullopt;
    }
    CapturedFrame frame = std::move(frame_queue_.front());
    frame_queue_.pop_front();
    return frame;
}

std::optional<CapturedFrame> CaptureCore::wait_pop_frame(std::chrono::milliseconds timeout) {
    std::unique_lock lock(queue_mutex_);
    queue_cv_.wait_for(lock, timeout, [this] {
        return !frame_queue_.empty() || status_.load() != CaptureStatus::running;
    });
    if (frame_queue_.empty()) {
        return std::nullopt;
    }
    CapturedFrame frame = std::move(frame_queue_.front());
    frame_queue_.pop_front();
    return frame;
}

std::optional<CaptureError> CaptureCore::runtime_error() const {
    std::lock_guard lock(error_mutex_);
    return error_;
}

void CaptureCore::set_failure(std::string message, int system_error) noexcept {
    {
        std::lock_guard lock(error_mutex_);
        if (!error_) {
            error_ = make_error(std::move(message), system_error);
        }
    }
    status_.store(CaptureStatus::failed);
    queue_cv_.notify_all();
    wake_worker();
}

bool CaptureCore::check_display(std::string_view operation) noexcept {
    if (display_ != nullptr && wl_display_get_error(display_) == 0) {
        return true;
    }
    const int error = display_ != nullptr ? wl_display_get_error(display_) : 0;
    set_failure(std::string(operation) + " failed: Wayland display error", error);
    return false;
}

void CaptureCore::begin_constraint_batch() {
    if (constraint_batch_open_) {
        return;
    }
    pending_requirements_ = CaptureRequirements{};
    pending_buffer_size_seen_ = false;
    constraint_batch_open_ = true;
}

void CaptureCore::on_constraints_done() {
    if (!constraint_batch_open_ || !pending_buffer_size_seen_ ||
        !validate_constraints(pending_requirements_)) {
        set_failure("compositor sent incomplete DMA-BUF capture constraints");
        return;
    }

    const auto sort_formats = [](std::vector<FormatModifier>& formats) {
        std::sort(formats.begin(), formats.end(), [](const FormatModifier& left, const FormatModifier& right) {
            return std::tie(left.fourcc, left.modifier) < std::tie(right.fourcc, right.modifier);
        });
    };
    sort_formats(pending_requirements_.dmabuf_formats);

    if (!initial_constraints_received_) {
        requirements_ = std::move(pending_requirements_);
        initial_constraints_received_ = true;
    } else if (requirements_.width != pending_requirements_.width ||
               requirements_.height != pending_requirements_.height ||
               requirements_.dmabuf_device != pending_requirements_.dmabuf_device ||
               requirements_.dmabuf_formats != pending_requirements_.dmabuf_formats) {
        set_failure("capture DMA-BUF constraints changed; stop and rebuild the pool before capturing again");
        return;
    }
    constraint_batch_open_ = false;
    ++constraints_completed_;
}

void CaptureCore::on_frame_ready(ext_image_copy_capture_frame_v1* proxy) {
    InFlightFrame* live = find_in_flight(proxy);
    if (live == nullptr) {
        set_failure("compositor sent a capture ready event without a live frame");
        return;
    }
    const auto ready_at = std::chrono::steady_clock::now();
    std::unique_ptr<InFlightFrame> completed;
    for (auto& frame : in_flight_) {
        if (frame && frame->proxy == proxy) {
            completed = std::move(frame);
            break;
        }
    }
    std::erase_if(in_flight_, [](const std::unique_ptr<InFlightFrame>& frame) {
        return !frame;
    });
    if (!completed) {
        set_failure("compositor sent a capture ready event without a live frame");
        return;
    }
    if (completed->proxy != nullptr) {
        ext_image_copy_capture_frame_v1_destroy(completed->proxy);
        completed->proxy = nullptr;
    }
    if (completed->slot >= slots_.size()) {
        set_failure("capture frame referenced an invalid DMA-BUF pool slot");
        return;
    }
    if (status_.load() == CaptureStatus::failed) {
        transition_slot(slots_[completed->slot], SlotState::free);
        return;
    }

    const auto elapsed = ready_at - completed->request_started_at;
    const auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
    const std::uint64_t request_to_ready_ns = elapsed_ns > 0
        ? static_cast<std::uint64_t>(elapsed_ns)
        : 0U;
    record_request_to_ready(request_to_ready_ns);

    consecutive_failures_ = 0;
    retry_at_.reset();
    const std::uint64_t sequence = next_sequence_++;
    captured_frames_.fetch_add(1);

    trace_ready(sequence, completed->slot, completed->presentation_time_ns.has_value(),
                completed->presentation_time_ns.value_or(0));
    if (!pts_rate_limit_.admit(completed->presentation_time_ns, ready_at)) {
        rate_limited_frames_.fetch_add(1);
        transition_slot(slots_[completed->slot], SlotState::free);
        schedule_capture();
        return;
    }

    slots_[completed->slot].lease_sequence = sequence;
    transition_slot(slots_[completed->slot], SlotState::leased);

    CapturedFrame result;
    result.sequence = sequence;
    result.request_to_ready_ns = request_to_ready_ns;
    result.ready_at = ready_at;
    if (completed->presentation_time_ns) {
        result.presentation_time_ns = *completed->presentation_time_ns;
        result.has_presentation_time = true;
    }
    if (completed->transform) {
        result.transform = *completed->transform;
        result.has_transform = true;
    }
    result.damage = std::move(completed->damage);
    auto lease_state = std::make_unique<FrameLeaseState>();
    lease_state->owner = weak_from_this();
    lease_state->storage = slots_[completed->slot].storage;
    lease_state->slot = completed->slot;
    lease_state->sequence = sequence;
    result.lease = FrameLease(std::move(lease_state));

    {
        std::lock_guard lock(queue_mutex_);
        if (frame_queue_.size() >= queue_capacity_) {
            // Leave the slot leased. Destroying result releases that sequence;
            // freeing it here would let a newer capture reuse the buffer before
            // the lease destructor runs.
            dropped_frames_.fetch_add(1);
        } else {
            frame_queue_.push_back(std::move(result));
            admitted_frames_.fetch_add(1);
        }
    }
    queue_cv_.notify_all();
    schedule_capture();
}

void CaptureCore::record_request_to_ready(std::uint64_t nanoseconds) noexcept {
    std::lock_guard lock(latency_mutex_);
    ++latency_sample_count_;
    latency_total_ns_ += nanoseconds;
    latency_last_ns_ = nanoseconds;
    latency_max_ns_ = std::max(latency_max_ns_, nanoseconds);
    latency_history_[latency_history_next_] = nanoseconds;
    latency_history_next_ = (latency_history_next_ + 1U) % latency_history_.size();
    latency_history_count_ = std::min(latency_history_count_ + 1U, latency_history_.size());
}

void CaptureCore::begin_pool_starvation() noexcept {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    std::int64_t inactive = -1;
    if (pool_starvation_started_ns_.compare_exchange_strong(inactive, now_ns)) {
        pool_starvation_periods_.fetch_add(1);
    }
}

void CaptureCore::end_pool_starvation() noexcept {
    const std::int64_t started = pool_starvation_started_ns_.exchange(-1);
    if (started < 0) {
        return;
    }
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    if (now_ns > started) {
        pool_starvation_total_ns_.fetch_add(static_cast<std::uint64_t>(now_ns - started));
    }
}

CaptureStats CaptureCore::stats() const {
    CaptureStats result;
    result.requests = capture_requests_.load();
    result.completed = captured_frames_.load();
    result.admitted = admitted_frames_.load();
    result.rate_limited = rate_limited_frames_.load();
    result.dropped = dropped_frames_.load();
    result.overlap_arm_failures = overlap_arm_failures_.load();
    result.failures = capture_failures_.load();
    result.cancelled_at_stop = cancelled_at_stop_.load();
    {
        std::lock_guard lock(queue_mutex_);
        result.frame_queue_occupancy = frame_queue_.size();
    }
    {
        std::lock_guard lock(pool_metrics_mutex_);
        result.pool_size = pool_size_count_.load();
        result.pool_free_slots = pool_free_slots_;
        result.pool_capturing_slots = pool_capturing_slots_;
        result.pool_leased_slots = pool_leased_slots_;
    }
    result.pool_busy_slots = result.pool_capturing_slots + result.pool_leased_slots;
    result.pool_starvation_periods = pool_starvation_periods_.load();
    result.pool_starvation_total_ns = pool_starvation_total_ns_.load();
    const std::int64_t starvation_started = pool_starvation_started_ns_.load();
    if (starvation_started >= 0) {
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
        if (now_ns > starvation_started) {
            result.pool_starvation_current_ns = static_cast<std::uint64_t>(now_ns - starvation_started);
        }
    }

    std::array<std::uint64_t, 512> samples{};
    {
        std::lock_guard lock(latency_mutex_);
        result.request_to_ready_samples = latency_sample_count_;
        result.request_to_ready_last_ns = latency_last_ns_;
        result.request_to_ready_average_ns = latency_sample_count_ != 0U
            ? latency_total_ns_ / latency_sample_count_
            : 0U;
        result.request_to_ready_max_ns = latency_max_ns_;
        result.request_to_ready_window_size = latency_history_count_;
        std::copy_n(latency_history_.begin(), latency_history_count_, samples.begin());
    }

    if (result.request_to_ready_window_size != 0U) {
        auto sample_end = samples.begin() + static_cast<std::ptrdiff_t>(result.request_to_ready_window_size);
        std::sort(samples.begin(), sample_end);
        const auto percentile = [&samples, &result](std::uint64_t percent) {
            const std::uint64_t count = static_cast<std::uint64_t>(result.request_to_ready_window_size);
            const std::uint64_t index = ((count * percent + 99U) / 100U) - 1U;
            return samples[static_cast<std::size_t>(index)];
        };
        result.request_to_ready_p50_ns = percentile(50U);
        result.request_to_ready_p95_ns = percentile(95U);
        result.request_to_ready_p99_ns = percentile(99U);
    }
    return result;
}

void CaptureCore::on_frame_failed(ext_image_copy_capture_frame_v1* proxy, std::uint32_t reason) {
    std::unique_ptr<InFlightFrame> failed;
    for (auto& frame : in_flight_) {
        if (frame && frame->proxy == proxy) {
            failed = std::move(frame);
            break;
        }
    }
    std::erase_if(in_flight_, [](const std::unique_ptr<InFlightFrame>& frame) {
        return !frame;
    });
    if (!failed) {
        set_failure("compositor sent a capture failure event without a live frame");
        return;
    }
    const std::size_t slot = failed->slot;
    if (failed->proxy != nullptr) {
        ext_image_copy_capture_frame_v1_destroy(failed->proxy);
        failed->proxy = nullptr;
    }
    if (slot < slots_.size()) {
        transition_slot(slots_[slot], SlotState::free);
    }
    capture_failures_.fetch_add(1);

    if (reason == EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_UNKNOWN) {
        ++consecutive_failures_;
        if (consecutive_failures_ < 3U) {
            retry_at_ = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(5U * consecutive_failures_);
            return;
        }
        set_failure("compositor repeatedly failed capture frames (unknown failure reason)");
    } else if (reason == EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS) {
        set_failure("compositor rejected the capture buffer constraints; rebuild the DMA-BUF pool");
    } else {
        set_failure("compositor stopped the capture session");
    }
}

void CaptureCore::registry_global(void* data, wl_registry* registry, std::uint32_t name,
                                  const char* interface, std::uint32_t version) {
    auto* self = static_cast<CaptureCore*>(data);
    if (std::strcmp(interface, wl_output_interface.name) == 0) {
        auto output = std::make_unique<OutputRecord>();
        output->owner = self;
        output->global_name = name;
        output->version = version;
        const std::uint32_t bind_version = std::min(version, 4U);
        output->proxy = static_cast<wl_output*>(wl_registry_bind(
            registry, name, &wl_output_interface, bind_version));
        if (output->proxy != nullptr) {
            (void)wl_output_add_listener(output->proxy, &output_listener_, output.get());
            self->outputs_.push_back(std::move(output));
        }
    } else if (std::strcmp(interface, zwp_linux_dmabuf_v1_interface.name) == 0 &&
               self->linux_dmabuf_ == nullptr && version >= 1U) {
        self->linux_dmabuf_global_ = name;
        self->linux_dmabuf_ = static_cast<zwp_linux_dmabuf_v1*>(wl_registry_bind(
            registry, name, &zwp_linux_dmabuf_v1_interface, std::min(version, 6U)));
    } else if (std::strcmp(interface, ext_output_image_capture_source_manager_v1_interface.name) == 0 &&
               self->source_manager_ == nullptr) {
        self->source_manager_global_ = name;
        self->source_manager_ = static_cast<ext_output_image_capture_source_manager_v1*>(
            wl_registry_bind(registry, name, &ext_output_image_capture_source_manager_v1_interface,
                            std::min(version, 1U)));
    } else if (std::strcmp(interface, ext_image_copy_capture_manager_v1_interface.name) == 0 &&
               self->capture_manager_ == nullptr) {
        self->capture_manager_global_ = name;
        self->capture_manager_ = static_cast<ext_image_copy_capture_manager_v1*>(
            wl_registry_bind(registry, name, &ext_image_copy_capture_manager_v1_interface,
                            std::min(version, 1U)));
    }
}

void CaptureCore::registry_global_remove(void* data, wl_registry*, std::uint32_t name) {
    auto* self = static_cast<CaptureCore*>(data);
    const auto output = std::find_if(self->outputs_.begin(), self->outputs_.end(), [name](const auto& item) {
        return item->global_name == name;
    });
    if (output != self->outputs_.end()) {
        (*output)->removed = true;
    }
    const CaptureStatus status = self->status_.load();
    if (should_fail_selected_output_removal(status, name, self->selected_output_global_)) {
        self->set_failure(status == CaptureStatus::running
                              ? "selected Wayland output was removed during capture"
                              : "selected Wayland output was removed before capture started");
    }
}

void CaptureCore::output_geometry(void*, wl_output*, std::int32_t, std::int32_t,
                                  std::int32_t, std::int32_t, std::int32_t,
                                  const char*, const char*, std::int32_t) {}
void CaptureCore::output_mode(void*, wl_output*, std::uint32_t, std::int32_t,
                              std::int32_t, std::int32_t) {}
void CaptureCore::output_done(void*, wl_output*) {}
void CaptureCore::output_scale(void*, wl_output*, std::int32_t) {}
void CaptureCore::output_name(void* data, wl_output*, const char* name) {
    static_cast<OutputRecord*>(data)->name = name != nullptr ? name : "";
}
void CaptureCore::output_description(void*, wl_output*, const char*) {}

void CaptureCore::session_buffer_size(void* data, ext_image_copy_capture_session_v1*,
                                      std::uint32_t width, std::uint32_t height) {
    auto* self = static_cast<CaptureCore*>(data);
    self->begin_constraint_batch();
    if (self->pending_buffer_size_seen_) {
        self->set_failure("compositor sent more than one buffer_size event in a constraint batch");
        return;
    }
    self->pending_requirements_.width = width;
    self->pending_requirements_.height = height;
    self->pending_buffer_size_seen_ = true;
}

void CaptureCore::session_shm_format(void*, ext_image_copy_capture_session_v1*, std::uint32_t) {
    // The capture module deliberately has no SHM or CPU-pixel path.
}

void CaptureCore::session_dmabuf_device(void* data, ext_image_copy_capture_session_v1*, wl_array* device) {
    auto* self = static_cast<CaptureCore*>(data);
    self->begin_constraint_batch();
    if (self->pending_requirements_.dmabuf_device) {
        self->set_failure("compositor sent more than one dmabuf_device event in a constraint batch");
        return;
    }
    if (device == nullptr || device->size != sizeof(std::uint64_t)) {
        self->set_failure("compositor sent a DMA-BUF device value with an unexpected size");
        return;
    }
    std::uint64_t value = 0;
    std::memcpy(&value, device->data, sizeof(value));
    self->pending_requirements_.dmabuf_device = value;
}

void CaptureCore::session_dmabuf_format(void* data, ext_image_copy_capture_session_v1*,
                                        std::uint32_t format, wl_array* modifiers) {
    auto* self = static_cast<CaptureCore*>(data);
    self->begin_constraint_batch();
    if (modifiers == nullptr || (modifiers->size % sizeof(std::uint64_t)) != 0U) {
        self->set_failure("compositor sent a malformed DMA-BUF modifier list");
        return;
    }
    const auto* bytes = static_cast<const std::byte*>(modifiers->data);
    for (std::size_t offset = 0; offset < modifiers->size; offset += sizeof(std::uint64_t)) {
        std::uint64_t modifier = 0;
        std::memcpy(&modifier, bytes + offset, sizeof(modifier));
        const FormatModifier pair{.fourcc = format, .modifier = modifier};
        if (std::find(self->pending_requirements_.dmabuf_formats.begin(),
                      self->pending_requirements_.dmabuf_formats.end(), pair) ==
            self->pending_requirements_.dmabuf_formats.end()) {
            self->pending_requirements_.dmabuf_formats.push_back(pair);
        }
    }
}

void CaptureCore::session_done(void* data, ext_image_copy_capture_session_v1*) {
    static_cast<CaptureCore*>(data)->on_constraints_done();
}

void CaptureCore::session_stopped(void* data, ext_image_copy_capture_session_v1*) {
    static_cast<CaptureCore*>(data)->set_failure("compositor stopped the image capture session");
}

void CaptureCore::frame_transform(void* data, ext_image_copy_capture_frame_v1* proxy, std::uint32_t transform) {
    auto* self = static_cast<CaptureCore*>(data);
    if (InFlightFrame* frame = self->find_in_flight(proxy)) {
        frame->transform = transform;
    }
}

void CaptureCore::frame_damage(void* data, ext_image_copy_capture_frame_v1* proxy, std::int32_t x,
                               std::int32_t y, std::int32_t width, std::int32_t height) {
    auto* self = static_cast<CaptureCore*>(data);
    if (InFlightFrame* frame = self->find_in_flight(proxy)) {
        frame->damage.push_back(DamageRect{.x = x, .y = y, .width = width, .height = height});
    }
}

void CaptureCore::frame_presentation_time(void* data, ext_image_copy_capture_frame_v1* proxy,
                                          std::uint32_t sec_hi, std::uint32_t sec_lo,
                                          std::uint32_t nsec) {
    auto* self = static_cast<CaptureCore*>(data);
    InFlightFrame* frame = self->find_in_flight(proxy);
    if (frame == nullptr) {
        return;
    }
    if (nsec >= 1'000'000'000U) {
        self->set_failure("compositor sent an invalid capture presentation timestamp");
        return;
    }
    const std::uint64_t seconds = (static_cast<std::uint64_t>(sec_hi) << 32U) |
                                  static_cast<std::uint64_t>(sec_lo);
    if (seconds > (std::numeric_limits<std::uint64_t>::max() - nsec) / 1'000'000'000ULL) {
        self->set_failure("capture presentation timestamp exceeds nanosecond range");
        return;
    }
    frame->presentation_time_ns = seconds * 1'000'000'000ULL + nsec;
    if (!frame->copy_started) {
        frame->copy_started = true;
        // The compositor has begun this copy. Arm another session before the
        // next output commit so that commit is not dropped while this copy
        // is still in flight.
        self->schedule_capture(true);
        const bool successor_armed = std::any_of(
            self->in_flight_.begin(), self->in_flight_.end(),
            [](const std::unique_ptr<InFlightFrame>& pending) {
                return pending && !pending->copy_started;
            });
        if (!successor_armed) {
            self->overlap_arm_failures_.fetch_add(1);
        }
    }
}

void CaptureCore::frame_ready(void* data, ext_image_copy_capture_frame_v1* proxy) {
    static_cast<CaptureCore*>(data)->on_frame_ready(proxy);
}

void CaptureCore::frame_failed(void* data, ext_image_copy_capture_frame_v1* proxy, std::uint32_t reason) {
    static_cast<CaptureCore*>(data)->on_frame_failed(proxy, reason);
}

void CaptureCore::buffer_created(void* data, zwp_linux_buffer_params_v1*, wl_buffer* buffer) {
    static_cast<PendingBuffer*>(data)->buffer = buffer;
}

void CaptureCore::buffer_failed(void* data, zwp_linux_buffer_params_v1*) {
    static_cast<PendingBuffer*>(data)->failed = true;
}

} // namespace detail

FrameLease::FrameLease(std::unique_ptr<detail::FrameLeaseState> state) noexcept
    : state_(std::move(state)) {}

FrameLease::~FrameLease() {
    release_after_gpu();
}

FrameLease::FrameLease(FrameLease&& other) noexcept = default;

FrameLease& FrameLease::operator=(FrameLease&& other) noexcept {
    if (this != &other) {
        release_after_gpu();
        state_ = std::move(other.state_);
    }
    return *this;
}

FrameLease::operator bool() const noexcept {
    return state_ != nullptr && !state_->released.load();
}

std::size_t FrameLease::slot_index() const noexcept {
    return state_ != nullptr ? state_->slot : 0U;
}

const DmabufImage& FrameLease::image() const {
    if (!state_ || state_->released.load() || !state_->storage) {
        throw std::logic_error("capture frame lease is no longer valid");
    }
    return state_->storage->image;
}

std::expected<UniqueFd, CaptureError> FrameLease::duplicate_plane_fd(std::size_t plane) const {
    if (!state_ || state_->released.load() || !state_->storage) {
        return std::unexpected(CaptureError{.message = "capture frame lease is no longer valid"});
    }
    if (plane >= state_->storage->image.planes.size()) {
        return std::unexpected(CaptureError{.message = "DMA-BUF plane index is out of range"});
    }
    const int copied_fd = ::fcntl(state_->storage->image.planes[plane].fd, F_DUPFD_CLOEXEC, 0);
    if (copied_fd < 0) {
        const int error = errno;
        return std::unexpected(CaptureError{
            .message = std::string("could not duplicate DMA-BUF plane fd: ") + std::strerror(error),
            .system_error = error,
        });
    }
    return UniqueFd(copied_fd);
}

void FrameLease::release_after_gpu() noexcept {
    if (state_) {
        state_->release();
        state_.reset();
    }
}

WaylandCapture::WaylandCapture(std::shared_ptr<detail::CaptureCore> core) noexcept
    : core_(std::move(core)) {}

WaylandCapture::~WaylandCapture() {
    stop();
}

WaylandCapture::WaylandCapture(WaylandCapture&& other) noexcept = default;

WaylandCapture& WaylandCapture::operator=(WaylandCapture&& other) noexcept {
    if (this != &other) {
        stop();
        core_ = std::move(other.core_);
    }
    return *this;
}

std::expected<WaylandCapture, CaptureError> WaylandCapture::connect(CaptureConfig config) {
    auto core = std::make_shared<detail::CaptureCore>(std::move(config));
    auto result = core->initialize();
    if (!result) {
        core->stop();
        return std::unexpected(result.error());
    }
    return WaylandCapture(std::move(core));
}

const CaptureRequirements& WaylandCapture::requirements() const {
    if (!core_) {
        throw std::logic_error("WaylandCapture is empty");
    }
    return core_->requirements();
}

std::expected<void, CaptureError> WaylandCapture::set_buffer_pool(
    std::span<const DmabufImage> images) {
    if (!core_) {
        return std::unexpected(CaptureError{.message = "WaylandCapture is empty"});
    }
    return core_->set_buffer_pool(images);
}

std::expected<void, CaptureError> WaylandCapture::start() {
    if (!core_) {
        return std::unexpected(CaptureError{.message = "WaylandCapture is empty"});
    }
    return core_->start();
}

void WaylandCapture::stop() noexcept {
    if (core_) {
        core_->stop();
    }
}

std::optional<CapturedFrame> WaylandCapture::try_pop_frame() {
    return core_ ? core_->try_pop_frame() : std::nullopt;
}

std::optional<CapturedFrame> WaylandCapture::wait_pop_frame(std::chrono::milliseconds timeout) {
    return core_ ? core_->wait_pop_frame(timeout) : std::nullopt;
}

CaptureStatus WaylandCapture::status() const noexcept {
    return core_ ? core_->status() : CaptureStatus::stopped;
}

std::optional<CaptureError> WaylandCapture::runtime_error() const {
    return core_ ? core_->runtime_error() : std::nullopt;
}

std::uint64_t WaylandCapture::captured_frames() const noexcept {
    return core_ ? core_->captured_frames() : 0U;
}

std::uint64_t WaylandCapture::dropped_frames() const noexcept {
    return core_ ? core_->dropped_frames() : 0U;
}

std::uint64_t WaylandCapture::capture_failures() const noexcept {
    return core_ ? core_->capture_failures() : 0U;
}

CaptureStats WaylandCapture::stats() const {
    return core_ ? core_->stats() : CaptureStats{};
}

std::size_t WaylandCapture::pool_size() const noexcept {
    return core_ ? core_->pool_size() : 0U;
}

} // namespace fluxrec::capture
