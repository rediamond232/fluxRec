#include "capture/capture.hpp"
#include "mux/mux_writer.hpp"
#include "mux/packet.hpp"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <ffnvcodec/nvEncodeAPI.h>
#include <gbm.h>
#include <drm_fourcc.h>
#include <linux/dma-buf.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <dlfcn.h>
#include <expected>
#include <filesystem>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <poll.h>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using fluxrec::capture::CaptureRequirements;
using fluxrec::capture::CapturedFrame;
using fluxrec::capture::DmabufImage;
using fluxrec::capture::UniqueFd;
using fluxrec::capture::WaylandCapture;

// Minimal CUDA Driver API declarations for the EGL interop calls used below.
// The driver API is loaded dynamically, so CUDA toolkit headers/libraries are
// not required to build this diagnostic. CUeglFrame follows cudaEGL.h's public
// ABI (CUDA Driver API 13.4); keep these offsets checked.
using CUresult = int;
using CUdevice = int;
struct CUctx_st;
struct CUarray_st;
struct CUgraphicsResource_st;
using CUcontext = CUctx_st*;
using CUarray = CUarray_st*;
using CUgraphicsResource = CUgraphicsResource_st*;
enum CUeglFrameType : int {
    CU_EGL_FRAME_TYPE_ARRAY = 0,
    CU_EGL_FRAME_TYPE_PITCH = 1,
};
struct CUeglFrame {
    union {
        CUarray pArray[3];
        void* pPitch[3];
    } frame;
    unsigned int width;
    unsigned int height;
    unsigned int depth;
    unsigned int pitch;
    unsigned int planeCount;
    unsigned int numChannels;
    CUeglFrameType frameType;
    int eglColorFormat;
    int cuFormat;
};
static_assert(offsetof(CUeglFrame, width) == sizeof(void*) * 3U);
static_assert(offsetof(CUeglFrame, frameType) == sizeof(void*) * 3U + 6U * sizeof(unsigned int));
static_assert(sizeof(CUeglFrame) == (sizeof(void*) == 8U ? 64U : 48U));

// NVIDIA's CUDA for Tegra EGL interop reference says this flags argument is
// reserved and must be zero. Keep that value even though the local desktop
// driver accepted an earlier experimental build with 1U.
constexpr unsigned int kCudaEglRegisterFlags = 0U;
constexpr unsigned int kCudaContextFlagsDefault = 0U;
constexpr unsigned int kDefaultPoolSize = 8U;
constexpr unsigned int kMaximumPoolSize = 16U;
constexpr int kMaximumDurationMs = 60000;
constexpr std::uint32_t kMaximumFrames = 21600U;
constexpr int kFenceWaitMs = 2000;
constexpr int kDefaultCaptureWaitMs = 3000;
constexpr std::int64_t kNanosecondsPerSecond = 1'000'000'000LL;
constexpr std::int64_t kNominalFrameDurationNs = kNanosecondsPerSecond / 360LL;

enum class ResourceStage : unsigned int {
    none = 0U,
    egl = 1U,
    cuda = 2U,
    nvenc = 3U,
};

enum class ColorMetadata {
    bt709_limited,
};

struct Options {
    std::string output_name = "DP-1";
    std::string output_file;
    std::string output_mkv;
    int capture_wait_ms = kDefaultCaptureWaitMs;
    int duration_ms = 0;
    std::uint32_t max_frames = 1U;
    unsigned int pool_slots = kDefaultPoolSize;
    unsigned int pipeline_depth = 1U;
    NV_ENC_TUNING_INFO nvenc_tuning = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
    std::optional<std::uint32_t> bitrate_mbps;
    std::optional<NV_ENC_H264_ENTROPY_CODING_MODE> h264_coder;
    std::optional<ColorMetadata> color_metadata;
    std::optional<ResourceStage> rearm_check_stage;
    bool retire_before_wait = false;
    bool register_only = false;
};

void print_usage(const char* executable) {
    std::fprintf(stderr,
        "usage: %s [--output NAME] [--capture-wait-ms N] [--duration-ms N] [--max-frames N] [--pool-slots N] [--pipeline-depth N] [--nvenc-tuning ull|ll] [--bitrate-mbps N] [--h264-coder auto|cabac|vlc] [--color-metadata bt709-limited] [--rearm-check-stage none|egl|cuda|nvenc] [--retire-before-wait] [--register-only] (--output-file PATH | --output-mkv PATH)\n"
        "Default mode captures and encodes one real DMA-BUF frame. --duration-ms enables a bounded multi-frame H.264 run (maximum 60 seconds / 21600 frames).\n"
        "NVENC controls are independent opt-ins; defaults retain the probe's P1/ULL preset configuration. --bitrate-mbps selects VBR and sets averageBitRate.\n"
        "--color-metadata bt709-limited writes BT.709 primaries/transfer/matrix and limited-range VUI metadata; this signals interpretation and does not guarantee NVENC uses BT.709 for RGB conversion.\n"
        "--output-mkv requires --duration-ms and --pipeline-depth greater than 1; it muxes H.264 packets on a separate worker using source presentation timestamps.\n"
        "--register-only stops after EGL import, CUDA array inspection, NVENC initialization, and input map.\n",
        executable);
}

std::optional<Options> parse_options(int argc, char** argv) {
    Options options;
    bool max_frames_set = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--help") {
            print_usage(argv[0]);
            return std::nullopt;
        }
        if ((arg == "--output" || arg == "--capture-wait-ms" || arg == "--duration-ms" ||
             arg == "--max-frames" || arg == "--pool-slots" || arg == "--pipeline-depth" ||
             arg == "--output-file" || arg == "--output-mkv" ||
             arg == "--nvenc-tuning" || arg == "--bitrate-mbps" || arg == "--h264-coder" ||
             arg == "--color-metadata" ||
             arg == "--rearm-check-stage") &&
            i + 1 < argc) {
            const std::string_view value{argv[++i]};
            try {
                if (arg == "--output") {
                    options.output_name = value;
                } else if (arg == "--output-file") {
                    options.output_file = value;
                } else if (arg == "--output-mkv") {
                    options.output_mkv = value;
                } else if (arg == "--rearm-check-stage") {
                    if (value == "none") options.rearm_check_stage = ResourceStage::none;
                    else if (value == "egl") options.rearm_check_stage = ResourceStage::egl;
                    else if (value == "cuda") options.rearm_check_stage = ResourceStage::cuda;
                    else if (value == "nvenc") options.rearm_check_stage = ResourceStage::nvenc;
                    else throw std::invalid_argument("stage must be none, egl, cuda, or nvenc");
                } else if (arg == "--capture-wait-ms") {
                    options.capture_wait_ms = std::stoi(std::string(value));
                } else if (arg == "--nvenc-tuning") {
                    if (value == "ull") options.nvenc_tuning = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
                    else if (value == "ll") options.nvenc_tuning = NV_ENC_TUNING_INFO_LOW_LATENCY;
                    else throw std::invalid_argument("tuning must be ull or ll");
                } else if (arg == "--bitrate-mbps") {
                    const unsigned long parsed = std::stoul(std::string(value));
                    if (parsed > std::numeric_limits<std::uint32_t>::max()) {
                        throw std::out_of_range("bitrate");
                    }
                    options.bitrate_mbps = static_cast<std::uint32_t>(parsed);
                } else if (arg == "--h264-coder") {
                    if (value == "auto") {
                        options.h264_coder = NV_ENC_H264_ENTROPY_CODING_MODE_AUTOSELECT;
                    } else if (value == "cabac") {
                        options.h264_coder = NV_ENC_H264_ENTROPY_CODING_MODE_CABAC;
                    } else if (value == "vlc") {
                        options.h264_coder = NV_ENC_H264_ENTROPY_CODING_MODE_CAVLC;
                    } else {
                        throw std::invalid_argument("coder must be auto, cabac, or vlc");
                    }
                } else if (arg == "--color-metadata") {
                    if (value == "bt709-limited") {
                        options.color_metadata = ColorMetadata::bt709_limited;
                    } else {
                        throw std::invalid_argument("metadata must be bt709-limited");
                    }
                } else if (arg == "--duration-ms") {
                    options.duration_ms = std::stoi(std::string(value));
                } else if (arg == "--max-frames") {
                    const unsigned long parsed = std::stoul(std::string(value));
                    if (parsed > std::numeric_limits<std::uint32_t>::max()) {
                        throw std::out_of_range("max frames");
                    }
                    options.max_frames = static_cast<std::uint32_t>(parsed);
                    max_frames_set = true;
                } else if (arg == "--pool-slots") {
                    const unsigned long parsed = std::stoul(std::string(value));
                    if (parsed > std::numeric_limits<unsigned int>::max()) {
                        throw std::out_of_range("pool slots");
                    }
                    options.pool_slots = static_cast<unsigned int>(parsed);
                } else {
                    const unsigned long parsed = std::stoul(std::string(value));
                    if (parsed > std::numeric_limits<unsigned int>::max()) {
                        throw std::out_of_range("pipeline depth");
                    }
                    options.pipeline_depth = static_cast<unsigned int>(parsed);
                }
            } catch (...) {
                std::fprintf(stderr, "invalid value for %.*s\n",
                             static_cast<int>(arg.size()), arg.data());
                    return std::nullopt;
            }
        } else if (arg == "--register-only") {
            options.register_only = true;
        } else if (arg == "--retire-before-wait") {
            options.retire_before_wait = true;
        } else {
            print_usage(argv[0]);
            return std::nullopt;
        }
    }
    if (!options.output_file.empty() && !options.output_mkv.empty()) {
        std::fprintf(stderr, "--output-file and --output-mkv are mutually exclusive\n");
        return std::nullopt;
    }
    if (options.output_file.empty() && options.output_mkv.empty() &&
        !options.register_only && !options.rearm_check_stage) {
        std::fprintf(stderr, "--output-file or --output-mkv is required unless --register-only is selected\n");
        return std::nullopt;
    }
    if (options.capture_wait_ms < 1 || options.capture_wait_ms > 60000) {
        std::fprintf(stderr, "--capture-wait-ms must be between 1 and 60000\n");
        return std::nullopt;
    }
    if (options.bitrate_mbps && (*options.bitrate_mbps < 1U || *options.bitrate_mbps > 500U)) {
        std::fprintf(stderr, "--bitrate-mbps must be between 1 and 500\n");
        return std::nullopt;
    }
    if (options.duration_ms != 0 &&
        (options.duration_ms < 100 || options.duration_ms > kMaximumDurationMs)) {
        std::fprintf(stderr, "--duration-ms must be between 100 and %d\n", kMaximumDurationMs);
        return std::nullopt;
    }
    if (options.duration_ms != 0 && !max_frames_set) {
        if (options.output_mkv.empty()) {
            options.max_frames = 3600U;
        } else {
            const std::uint64_t timed_frames =
                (static_cast<std::uint64_t>(options.duration_ms) * 360U + 999U) / 1000U + 1U;
            options.max_frames = static_cast<std::uint32_t>(
                std::min<std::uint64_t>(timed_frames, kMaximumFrames));
        }
    }
    if (options.max_frames == 0U || options.max_frames > kMaximumFrames) {
        std::fprintf(stderr, "--max-frames must be between 1 and %u\n", kMaximumFrames);
        return std::nullopt;
    }
    if (options.pool_slots < 3U || options.pool_slots > kMaximumPoolSize) {
        std::fprintf(stderr, "--pool-slots must be between 3 and %u\n", kMaximumPoolSize);
        return std::nullopt;
    }
    if (options.pipeline_depth < 1U || options.pipeline_depth > kMaximumPoolSize) {
        std::fprintf(stderr, "--pipeline-depth must be between 1 and %u\n", kMaximumPoolSize);
        return std::nullopt;
    }
    if (options.register_only && (options.duration_ms != 0 || max_frames_set ||
                                  !options.output_mkv.empty())) {
        std::fprintf(stderr, "--register-only cannot be combined with --duration-ms, --max-frames, or --output-mkv\n");
        return std::nullopt;
    }
    if (options.rearm_check_stage) {
        if (options.duration_ms != 0 || max_frames_set || options.register_only ||
            !options.output_file.empty() || !options.output_mkv.empty() ||
            options.capture_wait_ms > 5000 ||
            (options.retire_before_wait && *options.rearm_check_stage == ResourceStage::none)) {
            std::fprintf(stderr, "--rearm-check-stage cannot be combined with encode modes; wait is capped at 5000ms and --retire-before-wait requires egl/cuda/nvenc\n");
            return std::nullopt;
        }
    } else if (options.retire_before_wait) {
        std::fprintf(stderr, "--retire-before-wait requires --rearm-check-stage\n");
        return std::nullopt;
    }
    if (options.duration_ms == 0 && max_frames_set && options.max_frames != 1U) {
        std::fprintf(stderr, "--max-frames greater than 1 requires --duration-ms\n");
        return std::nullopt;
    }
    if (options.pipeline_depth > 1U && options.duration_ms == 0) {
        std::fprintf(stderr, "--pipeline-depth greater than 1 requires --duration-ms\n");
        return std::nullopt;
    }
    if (!options.output_mkv.empty() && options.duration_ms == 0) {
        std::fprintf(stderr, "--output-mkv requires --duration-ms\n");
        return std::nullopt;
    }
    if (!options.output_mkv.empty() && options.pipeline_depth < 2U) {
        std::fprintf(stderr, "--output-mkv requires --pipeline-depth greater than 1\n");
        return std::nullopt;
    }
    return options;
}

class DynamicLibrary {
public:
    explicit DynamicLibrary(const char* soname) : handle_(::dlopen(soname, RTLD_NOW | RTLD_LOCAL)) {}
    ~DynamicLibrary() { if (handle_) ::dlclose(handle_); }
    DynamicLibrary(const DynamicLibrary&) = delete;
    DynamicLibrary& operator=(const DynamicLibrary&) = delete;
    [[nodiscard]] explicit operator bool() const noexcept { return handle_ != nullptr; }
    [[nodiscard]] void* symbol(const char* name) const noexcept {
        return handle_ ? ::dlsym(handle_, name) : nullptr;
    }
private:
    void* handle_ = nullptr;
};

template<class T>
T load_symbol(const DynamicLibrary& library, const char* name) {
    void* raw = library.symbol(name);
    T result{};
    static_assert(sizeof(result) == sizeof(raw));
    std::memcpy(&result, &raw, sizeof(result));
    return result;
}

struct PoolSlot {
    std::unique_ptr<gbm_bo, decltype(&gbm_bo_destroy)> bo{nullptr, gbm_bo_destroy};
    UniqueFd plane_fd;
    DmabufImage image;
};

std::optional<std::string> render_node_for_device(std::uint64_t expected_device) {
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator("/dev/dri", error)) {
        struct stat metadata {};
        if (::stat(entry.path().c_str(), &metadata) == 0 && S_ISCHR(metadata.st_mode) &&
            static_cast<std::uint64_t>(metadata.st_rdev) == expected_device) {
            return entry.path().string();
        }
    }
    return std::nullopt;
}

std::optional<PoolSlot> make_pool_slot(gbm_device* device,
                                       const CaptureRequirements& requirements,
                                       std::uint32_t fourcc,
                                       std::uint64_t modifier) {
    PoolSlot slot;
    gbm_bo* raw = gbm_bo_create_with_modifiers2(
        device, requirements.width, requirements.height, fourcc, &modifier, 1U,
        GBM_BO_USE_RENDERING);
    if (!raw) return std::nullopt;
    slot.bo.reset(raw);
    if (gbm_bo_get_modifier(raw) != modifier || gbm_bo_get_format(raw) != fourcc ||
        gbm_bo_get_plane_count(raw) != 1) {
        return std::nullopt;
    }
    slot.plane_fd.reset(gbm_bo_get_fd_for_plane(raw, 0));
    if (!slot.plane_fd) return std::nullopt;
    slot.image.width = requirements.width;
    slot.image.height = requirements.height;
    slot.image.fourcc = fourcc;
    slot.image.modifier = modifier;
    slot.image.planes.push_back({slot.plane_fd.get(), gbm_bo_get_offset(raw, 0),
                                 gbm_bo_get_stride_for_plane(raw, 0)});
    return slot;
}

bool allocate_pool(gbm_device* device, const CaptureRequirements& requirements,
                   std::vector<PoolSlot>& slots, std::uint32_t& selected_fourcc,
                   std::uint64_t& selected_modifier, unsigned int requested_slots) {
    const std::array<std::uint32_t, 2> preferred{DRM_FORMAT_ARGB8888, DRM_FORMAT_XRGB8888};
    for (const std::uint32_t wanted : preferred) {
        for (const auto& pair : requirements.dmabuf_formats) {
            if (pair.fourcc != wanted) continue;
            std::vector<PoolSlot> candidate;
            candidate.reserve(requested_slots);
            for (unsigned int i = 0; i < requested_slots; ++i) {
                auto slot = make_pool_slot(device, requirements, pair.fourcc, pair.modifier);
                if (!slot) break;
                candidate.push_back(std::move(*slot));
            }
            if (candidate.size() != requested_slots) continue;
            slots = std::move(candidate);
            selected_fourcc = pair.fourcc;
            selected_modifier = pair.modifier;
            return true;
        }
    }
    return false;
}

std::string fourcc_string(std::uint32_t value) {
    std::string result(4, ' ');
    for (unsigned int i = 0; i < 4U; ++i) {
        result[i] = static_cast<char>((value >> (i * 8U)) & 0xffU);
    }
    return result;
}

bool wait_for_dma_buf_read_fence(int dma_buf_fd, bool report_success = true) {
    dma_buf_export_sync_file export_info{};
    export_info.flags = DMA_BUF_SYNC_READ;
    export_info.fd = -1;
    if (::ioctl(dma_buf_fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &export_info) < 0) {
        std::fprintf(stderr, "stage=fence_export failed errno=%d (%s)\n", errno,
                     std::strerror(errno));
        return false;
    }
    UniqueFd sync_fd{export_info.fd};
    if (!sync_fd) {
        std::fprintf(stderr, "stage=fence_export returned an invalid sync-file fd\n");
        return false;
    }
    pollfd descriptor{.fd = sync_fd.get(), .events = POLLIN, .revents = 0};
    int result = 0;
    do {
        result = ::poll(&descriptor, 1, kFenceWaitMs);
    } while (result < 0 && errno == EINTR);
    if (result <= 0 || (descriptor.revents & POLLIN) == 0) {
        std::fprintf(stderr, "stage=fence_wait %s%s\n",
                     result == 0 ? "timed out" : "failed",
                     result < 0 ? std::strerror(errno) : "");
        return false;
    }
    if (report_success) std::printf("stage=fence_wait passed flags=DMA_BUF_SYNC_READ\n");
    return true;
}

struct EglApi {
    PFNEGLQUERYDEVICESEXTPROC query_devices = nullptr;
    PFNEGLQUERYDEVICESTRINGEXTPROC query_device_string = nullptr;
    PFNEGLQUERYDEVICEATTRIBEXTPROC query_device_attrib = nullptr;
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display = nullptr;
    PFNEGLQUERYDMABUFFORMATSEXTPROC query_dmabuf_formats = nullptr;
    PFNEGLQUERYDMABUFMODIFIERSEXTPROC query_dmabuf_modifiers = nullptr;
    PFNEGLCREATEIMAGEKHRPROC create_image = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC destroy_image = nullptr;
};

template<class T>
T egl_proc(const char* name) {
    const auto raw = eglGetProcAddress(name);
    T result{};
    static_assert(sizeof(result) == sizeof(raw));
    std::memcpy(&result, &raw, sizeof(result));
    return result;
}

struct EglState {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLDeviceEXT device = EGL_NO_DEVICE_EXT;
    std::vector<EGLImageKHR> images;
    EglApi api;
    int cuda_ordinal = -1;
    bool initialized = false;

    ~EglState() {
        if (display != EGL_NO_DISPLAY && api.destroy_image) {
            for (const EGLImageKHR image : images) {
                if (image != EGL_NO_IMAGE_KHR) (void)api.destroy_image(display, image);
            }
        }
        if (display != EGL_NO_DISPLAY && initialized) (void)eglTerminate(display);
    }
};

bool contains_extension(const char* extensions, std::string_view wanted) {
    if (!extensions) return false;
    std::string_view all{extensions};
    std::size_t at = 0;
    while (at < all.size()) {
        while (at < all.size() && all[at] == ' ') ++at;
        const std::size_t end = all.find(' ', at);
        const std::string_view item = all.substr(at, end == std::string_view::npos
            ? all.size() - at : end - at);
        if (item == wanted) return true;
        if (end == std::string_view::npos) break;
        at = end + 1U;
    }
    return false;
}

std::expected<std::unique_ptr<EglState>, std::string> initialize_egl_for_capture_device(
    std::string_view render_node, const DmabufImage& image) {
    auto state = std::make_unique<EglState>();
    state->api.query_devices = egl_proc<PFNEGLQUERYDEVICESEXTPROC>("eglQueryDevicesEXT");
    state->api.query_device_string = egl_proc<PFNEGLQUERYDEVICESTRINGEXTPROC>("eglQueryDeviceStringEXT");
    state->api.query_device_attrib = egl_proc<PFNEGLQUERYDEVICEATTRIBEXTPROC>("eglQueryDeviceAttribEXT");
    state->api.get_platform_display = egl_proc<PFNEGLGETPLATFORMDISPLAYEXTPROC>("eglGetPlatformDisplayEXT");
    state->api.query_dmabuf_formats = egl_proc<PFNEGLQUERYDMABUFFORMATSEXTPROC>("eglQueryDmaBufFormatsEXT");
    state->api.query_dmabuf_modifiers = egl_proc<PFNEGLQUERYDMABUFMODIFIERSEXTPROC>("eglQueryDmaBufModifiersEXT");
    state->api.create_image = egl_proc<PFNEGLCREATEIMAGEKHRPROC>("eglCreateImageKHR");
    state->api.destroy_image = egl_proc<PFNEGLDESTROYIMAGEKHRPROC>("eglDestroyImageKHR");
    if (!state->api.query_devices || !state->api.query_device_string ||
        !state->api.get_platform_display || !state->api.query_dmabuf_formats ||
        !state->api.query_dmabuf_modifiers || !state->api.create_image ||
        !state->api.destroy_image) {
        return std::unexpected("required EGL device/modifier/image entry point is absent");
    }

    EGLint device_count = 0;
    if (state->api.query_devices(0, nullptr, &device_count) != EGL_TRUE || device_count <= 0) {
        return std::unexpected("eglQueryDevicesEXT returned no EGL devices");
    }
    std::vector<EGLDeviceEXT> devices(static_cast<std::size_t>(device_count));
    if (state->api.query_devices(device_count, devices.data(), &device_count) != EGL_TRUE) {
        return std::unexpected("eglQueryDevicesEXT failed to enumerate EGL devices");
    }

    for (const EGLDeviceEXT device : devices) {
        const char* path = state->api.query_device_string(device, EGL_DRM_RENDER_NODE_FILE_EXT);
        if (path && render_node == path) {
            state->device = device;
            break;
        }
    }
    if (state->device == EGL_NO_DEVICE_EXT) {
        return std::unexpected("no EGL device matched the capture DMA-BUF render node");
    }
    const char* device_extensions = state->api.query_device_string(state->device, EGL_EXTENSIONS);
    if (!contains_extension(device_extensions, "EGL_NV_device_cuda")) {
        return std::unexpected("matched EGL device does not advertise EGL_NV_device_cuda");
    }
    if (!state->api.query_device_attrib) {
        state->api.query_device_attrib = egl_proc<PFNEGLQUERYDEVICEATTRIBEXTPROC>("eglQueryDeviceAttribEXT");
    }
    if (!state->api.query_device_attrib) {
        return std::unexpected("eglQueryDeviceAttribEXT is absent");
    }
    EGLAttrib cuda_ordinal = -1;
    if (state->api.query_device_attrib(state->device, EGL_CUDA_DEVICE_NV, &cuda_ordinal) != EGL_TRUE ||
        cuda_ordinal < 0 || cuda_ordinal > std::numeric_limits<int>::max()) {
        return std::unexpected("EGL_NV_device_cuda did not return a valid CUDA device ordinal");
    }
    state->cuda_ordinal = static_cast<int>(cuda_ordinal);

    state->display = state->api.get_platform_display(EGL_PLATFORM_DEVICE_EXT, state->device, nullptr);
    if (state->display == EGL_NO_DISPLAY) {
        return std::unexpected("eglGetPlatformDisplayEXT(EGL_PLATFORM_DEVICE_EXT) failed");
    }
    EGLint major = 0;
    EGLint minor = 0;
    if (eglInitialize(state->display, &major, &minor) != EGL_TRUE) {
        return std::unexpected("eglInitialize for the matched EGL device failed");
    }
    state->initialized = true;
    const char* display_extensions = eglQueryString(state->display, EGL_EXTENSIONS);
    if (!contains_extension(display_extensions, "EGL_EXT_image_dma_buf_import") ||
        !contains_extension(display_extensions, "EGL_EXT_image_dma_buf_import_modifiers")) {
        return std::unexpected("matched EGL display lacks DMA-BUF import/modifier extensions");
    }
    std::printf("stage=egl_device passed render_node=%.*s egl=%d.%d cuda_ordinal=%d\n",
                static_cast<int>(render_node.size()), render_node.data(), major, minor,
                state->cuda_ordinal);

    const std::uint32_t fourcc = image.fourcc;
    EGLint format_count = 0;
    if (state->api.query_dmabuf_formats(state->display, 0, nullptr, &format_count) != EGL_TRUE ||
        format_count < 0) {
        return std::unexpected("eglQueryDmaBufFormatsEXT(count) failed");
    }
    std::vector<EGLint> formats(static_cast<std::size_t>(format_count));
    if (state->api.query_dmabuf_formats(state->display, format_count, formats.data(), &format_count) != EGL_TRUE) {
        return std::unexpected("eglQueryDmaBufFormatsEXT(list) failed");
    }
    if (format_count > static_cast<EGLint>(formats.size())) {
        return std::unexpected("eglQueryDmaBufFormatsEXT returned more formats than the supplied capacity");
    }
    if (std::find(formats.begin(), formats.end(), static_cast<EGLint>(fourcc)) == formats.end()) {
        return std::unexpected("EGL does not advertise the captured DRM fourcc");
    }
    EGLint modifier_count = 0;
    if (state->api.query_dmabuf_modifiers(state->display, static_cast<EGLint>(fourcc),
            0, nullptr, nullptr, &modifier_count) != EGL_TRUE || modifier_count < 0) {
        return std::unexpected("eglQueryDmaBufModifiersEXT(count) failed");
    }
    std::vector<EGLuint64KHR> modifiers(static_cast<std::size_t>(modifier_count));
    std::vector<EGLBoolean> external_only(static_cast<std::size_t>(modifier_count));
    if (state->api.query_dmabuf_modifiers(state->display, static_cast<EGLint>(fourcc),
            modifier_count, modifiers.data(), external_only.data(), &modifier_count) != EGL_TRUE) {
        return std::unexpected("eglQueryDmaBufModifiersEXT(list) failed");
    }
    if (modifier_count > static_cast<EGLint>(modifiers.size())) {
        return std::unexpected("eglQueryDmaBufModifiersEXT returned more modifiers than the supplied capacity");
    }
    const auto modifier = std::find(modifiers.begin(), modifiers.end(), image.modifier);
    if (modifier == modifiers.end()) {
        return std::unexpected("EGL does not advertise the capture modifier for this fourcc");
    }
    const std::size_t modifier_index = static_cast<std::size_t>(modifier - modifiers.begin());
    std::printf("stage=egl_modifier passed fourcc=0x%08x modifier=0x%016llx external_only=%s\n",
                fourcc, static_cast<unsigned long long>(image.modifier),
                external_only[modifier_index] == EGL_TRUE ? "yes" : "no");
    return state;
}

std::expected<EGLImageKHR, std::string> import_egl_image(EglState& egl,
                                                         const DmabufImage& image,
                                                         int fd) {
    if (image.planes.size() != 1U) {
        return std::unexpected("probe currently requires one-plane AR24/XR24 capture buffers");
    }
    const auto& plane = image.planes[0];
    if (image.width > static_cast<std::uint32_t>(std::numeric_limits<EGLint>::max()) ||
        image.height > static_cast<std::uint32_t>(std::numeric_limits<EGLint>::max()) ||
        plane.offset > static_cast<std::uint32_t>(std::numeric_limits<EGLint>::max()) ||
        plane.stride > static_cast<std::uint32_t>(std::numeric_limits<EGLint>::max())) {
        return std::unexpected("DMA-BUF dimensions/plane metadata exceed EGLint range");
    }
    const EGLint attributes[] = {
        EGL_WIDTH, static_cast<EGLint>(image.width),
        EGL_HEIGHT, static_cast<EGLint>(image.height),
        EGL_LINUX_DRM_FOURCC_EXT, static_cast<EGLint>(image.fourcc),
        EGL_DMA_BUF_PLANE0_FD_EXT, fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, static_cast<EGLint>(plane.offset),
        EGL_DMA_BUF_PLANE0_PITCH_EXT, static_cast<EGLint>(plane.stride),
        EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
            static_cast<EGLint>(image.modifier & 0xffffffffULL),
        EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
            static_cast<EGLint>((image.modifier >> 32U) & 0xffffffffULL),
        EGL_NONE,
    };
    const EGLImageKHR imported = egl.api.create_image(egl.display, EGL_NO_CONTEXT,
                                                       EGL_LINUX_DMA_BUF_EXT, nullptr, attributes);
    if (imported == EGL_NO_IMAGE_KHR) {
        const EGLint error = eglGetError();
        return std::unexpected("eglCreateImageKHR(DMA-BUF) failed with EGL error " +
                               std::to_string(error));
    }
    std::printf("stage=egl_import passed width=%u height=%u fourcc=0x%08x pitch=%u offset=%u modifier=0x%016llx\n",
                image.width, image.height, image.fourcc, plane.stride, plane.offset,
                static_cast<unsigned long long>(image.modifier));
    egl.images.push_back(imported);
    return imported;
}

struct CudaApi {
    DynamicLibrary library{"libcuda.so.1"};
    CUresult (*init)(unsigned int) = nullptr;
    CUresult (*device_get)(CUdevice*, int) = nullptr;
    CUresult (*context_create)(CUcontext*, unsigned int, CUdevice) = nullptr;
    CUresult (*context_destroy)(CUcontext) = nullptr;
    CUresult (*graphics_egl_register_image)(CUgraphicsResource*, EGLImageKHR, unsigned int) = nullptr;
    CUresult (*graphics_resource_get_mapped_egl_frame)(CUeglFrame*, CUgraphicsResource,
                                                        unsigned int, unsigned int) = nullptr;
    CUresult (*graphics_unregister_resource)(CUgraphicsResource) = nullptr;
    CUresult (*context_synchronize)() = nullptr;
    CUresult (*get_error_name)(CUresult, const char**) = nullptr;
    CUresult (*get_error_string)(CUresult, const char**) = nullptr;

    CudaApi() {
        init = load_symbol<decltype(init)>(library, "cuInit");
        device_get = load_symbol<decltype(device_get)>(library, "cuDeviceGet");
        context_create = load_symbol<decltype(context_create)>(library, "cuCtxCreate_v2");
        context_destroy = load_symbol<decltype(context_destroy)>(library, "cuCtxDestroy_v2");
        graphics_egl_register_image = load_symbol<decltype(graphics_egl_register_image)>(
            library, "cuGraphicsEGLRegisterImage");
        graphics_resource_get_mapped_egl_frame = load_symbol<
            decltype(graphics_resource_get_mapped_egl_frame)>(
                library, "cuGraphicsResourceGetMappedEglFrame");
        graphics_unregister_resource = load_symbol<decltype(graphics_unregister_resource)>(
            library, "cuGraphicsUnregisterResource");
        context_synchronize = load_symbol<decltype(context_synchronize)>(library, "cuCtxSynchronize");
        get_error_name = load_symbol<decltype(get_error_name)>(library, "cuGetErrorName");
        get_error_string = load_symbol<decltype(get_error_string)>(library, "cuGetErrorString");
    }

    [[nodiscard]] bool available() const noexcept {
        return library && init && device_get && context_create && context_destroy &&
            graphics_egl_register_image && graphics_resource_get_mapped_egl_frame &&
            graphics_unregister_resource && context_synchronize;
    }

    [[nodiscard]] std::string describe(CUresult result) const {
        const char* name = nullptr;
        const char* message = nullptr;
        if (get_error_name) (void)get_error_name(result, &name);
        if (get_error_string) (void)get_error_string(result, &message);
        std::string out = "CUDA result " + std::to_string(result);
        if (name) out += " (" + std::string(name) + ")";
        if (message) out += ": " + std::string(message);
        return out;
    }
};

struct CudaState {
    CudaApi api;
    CUcontext context = nullptr;
    std::vector<CUgraphicsResource> resources;
    bool owns_context = false;

    ~CudaState() {
        for (const CUgraphicsResource resource : resources) {
            if (resource) (void)api.graphics_unregister_resource(resource);
        }
        if (context && owns_context) (void)api.context_destroy(context);
    }
};

struct CudaSlot {
    CUgraphicsResource resource = nullptr;
    CUeglFrame frame{};
};

std::expected<std::unique_ptr<CudaState>, std::string> initialize_cuda(int ordinal) {
    auto cuda = std::make_unique<CudaState>();
    if (!cuda->api.available()) {
        return std::unexpected("libcuda.so.1 lacks required CUDA Driver API functions");
    }
    CUresult result = cuda->api.init(0U);
    if (result != 0) return std::unexpected("cuInit failed: " + cuda->api.describe(result));
    CUdevice device = 0;
    result = cuda->api.device_get(&device, ordinal);
    if (result != 0) return std::unexpected("cuDeviceGet failed: " + cuda->api.describe(result));
    result = cuda->api.context_create(&cuda->context, kCudaContextFlagsDefault, device);
    if (result != 0) return std::unexpected("cuCtxCreate_v2 failed: " + cuda->api.describe(result));
    cuda->owns_context = true;
    return cuda;
}

std::expected<CudaSlot, std::string> register_cuda_egl_image(CudaState& cuda,
                                                             EGLImageKHR image) {
    CudaSlot slot;
    CUresult result = cuda.api.graphics_egl_register_image(
        &slot.resource, image, kCudaEglRegisterFlags);
    if (result != 0) {
        return std::unexpected("cuGraphicsEGLRegisterImage failed: " + cuda.api.describe(result));
    }
    cuda.resources.push_back(slot.resource);
    result = cuda.api.graphics_resource_get_mapped_egl_frame(&slot.frame, slot.resource, 0U, 0U);
    if (result != 0) {
        return std::unexpected("cuGraphicsResourceGetMappedEglFrame failed: " +
                               cuda.api.describe(result));
    }
    if (slot.frame.frameType != CU_EGL_FRAME_TYPE_ARRAY) {
        return std::unexpected("CUDA EGL frame is pitch-linear; NVENC CUDAARRAY needs array input");
    }
    if (slot.frame.planeCount != 1U || !slot.frame.frame.pArray[0]) {
        return std::unexpected("CUDA EGL frame did not expose one non-null array plane");
    }
    if (slot.frame.width == 0U || slot.frame.height == 0U) {
        return std::unexpected("CUDA EGL array reported empty dimensions");
    }
    std::printf("stage=cuda_egl_import passed frame_type=array width=%u height=%u depth=%u planes=%u channels=%u pitch=%u format=%d color=%d array=%p\n",
                slot.frame.width, slot.frame.height, slot.frame.depth,
                slot.frame.planeCount, slot.frame.numChannels, slot.frame.pitch,
                slot.frame.cuFormat, slot.frame.eglColorFormat,
                static_cast<void*>(slot.frame.frame.pArray[0]));
    return slot;
}

struct NvencState {
    DynamicLibrary library{"libnvidia-encode.so.1"};
    NV_ENCODE_API_FUNCTION_LIST functions{};
    void* encoder = nullptr;
    std::vector<NV_ENC_REGISTERED_PTR> registered;
    std::vector<NV_ENC_INPUT_PTR> mapped_resources;
    std::mutex mapped_mutex;
    std::vector<NV_ENC_OUTPUT_PTR> bitstreams;
    std::vector<NV_ENC_OUTPUT_PTR> locked_bitstreams;
    // Compatibility aliases used only by the serial one-frame/depth-1 path.
    NV_ENC_INPUT_PTR mapped = nullptr;
    NV_ENC_OUTPUT_PTR bitstream = nullptr;

    ~NvencState() {
        if (!encoder) return;
        if (functions.nvEncUnmapInputResource) {
            std::lock_guard lock(mapped_mutex);
            for (const NV_ENC_INPUT_PTR resource : mapped_resources) {
                if (resource) (void)functions.nvEncUnmapInputResource(encoder, resource);
            }
        }
        if (functions.nvEncUnlockBitstream) {
            for (const NV_ENC_OUTPUT_PTR output : locked_bitstreams) {
                if (output) (void)functions.nvEncUnlockBitstream(encoder, output);
            }
        }
        if (functions.nvEncUnregisterResource) {
            for (const NV_ENC_REGISTERED_PTR resource : registered) {
                if (resource) (void)functions.nvEncUnregisterResource(encoder, resource);
            }
        }
        if (functions.nvEncDestroyBitstreamBuffer) {
            for (const NV_ENC_OUTPUT_PTR output : bitstreams) {
                if (output) (void)functions.nvEncDestroyBitstreamBuffer(encoder, output);
            }
        }
        if (functions.nvEncDestroyEncoder) (void)functions.nvEncDestroyEncoder(encoder);
    }
};

const char* nvenc_format_name(NV_ENC_BUFFER_FORMAT format) {
    switch (format) {
        case NV_ENC_BUFFER_FORMAT_NV12: return "NV12";
        case NV_ENC_BUFFER_FORMAT_ARGB: return "ARGB";
        case NV_ENC_BUFFER_FORMAT_ABGR: return "ABGR";
        case NV_ENC_BUFFER_FORMAT_YV12: return "YV12";
        case NV_ENC_BUFFER_FORMAT_IYUV: return "IYUV";
        case NV_ENC_BUFFER_FORMAT_YUV444: return "YUV444";
        default: return "other";
    }
}

const char* nvenc_tuning_name(NV_ENC_TUNING_INFO tuning) {
    switch (tuning) {
        case NV_ENC_TUNING_INFO_LOW_LATENCY: return "LL";
        case NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY: return "ULL";
        default: return "other";
    }
}

const char* nvenc_rc_name(NV_ENC_PARAMS_RC_MODE mode) {
    switch (mode) {
        case NV_ENC_PARAMS_RC_CONSTQP: return "CONSTQP";
        case NV_ENC_PARAMS_RC_VBR: return "VBR";
        case NV_ENC_PARAMS_RC_CBR: return "CBR";
        default: return "other";
    }
}

const char* nvenc_h264_coder_name(NV_ENC_H264_ENTROPY_CODING_MODE coder) {
    switch (coder) {
        case NV_ENC_H264_ENTROPY_CODING_MODE_AUTOSELECT: return "AUTO";
        case NV_ENC_H264_ENTROPY_CODING_MODE_CABAC: return "CABAC";
        case NV_ENC_H264_ENTROPY_CODING_MODE_CAVLC: return "CAVLC";
        default: return "other";
    }
}

std::expected<std::unique_ptr<NvencState>, std::string> initialize_nvenc(CUcontext cuda_context,
                                                       unsigned int width,
                                                       unsigned int height,
                                                       NV_ENC_BUFFER_FORMAT wanted_format,
                                                       const Options& options) {
    auto state = std::make_unique<NvencState>();
    if (!state->library) return std::unexpected("dlopen(libnvidia-encode.so.1) failed");
    using CreateInstanceFn = NVENCSTATUS (NVENCAPI *)(NV_ENCODE_API_FUNCTION_LIST*);
    const auto create_instance = load_symbol<CreateInstanceFn>(state->library,
                                                                "NvEncodeAPICreateInstance");
    if (!create_instance) return std::unexpected("NvEncodeAPICreateInstance is absent");
    state->functions.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    NVENCSTATUS status = create_instance(&state->functions);
    if (status != NV_ENC_SUCCESS) {
        return std::unexpected("NvEncodeAPICreateInstance failed with status " +
                               std::to_string(status));
    }

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS open_params{};
    open_params.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    open_params.deviceType = NV_ENC_DEVICE_TYPE_CUDA;
    open_params.device = cuda_context;
    open_params.apiVersion = NVENCAPI_VERSION;
    status = state->functions.nvEncOpenEncodeSessionEx(&open_params, &state->encoder);
    if (status != NV_ENC_SUCCESS || !state->encoder) {
        return std::unexpected("nvEncOpenEncodeSessionEx failed with status " +
                               std::to_string(status));
    }

    uint32_t format_count = 0;
    status = state->functions.nvEncGetInputFormatCount(
        state->encoder, NV_ENC_CODEC_H264_GUID, &format_count);
    if (status != NV_ENC_SUCCESS || format_count == 0U || format_count > 256U) {
        return std::unexpected("NvEncGetInputFormatCount(H.264) failed or returned an invalid count");
    }
    std::vector<NV_ENC_BUFFER_FORMAT> formats(format_count);
    uint32_t returned_count = 0;
    status = state->functions.nvEncGetInputFormats(
        state->encoder, NV_ENC_CODEC_H264_GUID, formats.data(), format_count, &returned_count);
    if (status != NV_ENC_SUCCESS) {
        return std::unexpected("NvEncGetInputFormats(H.264) failed with status " +
                               std::to_string(status));
    }
    if (returned_count > format_count) {
        return std::unexpected("NvEncGetInputFormats returned more formats than the supplied capacity");
    }
    std::printf("stage=nvenc_input_formats passed H264 formats=");
    for (uint32_t i = 0; i < returned_count; ++i) {
        std::printf("%s%s", i == 0U ? "" : ",", nvenc_format_name(formats[i]));
    }
    std::putchar('\n');
    if (std::find(formats.begin(), formats.begin() + returned_count, wanted_format) ==
        formats.begin() + returned_count) {
        return std::unexpected(std::string("NVENC H.264 does not report ") +
                               nvenc_format_name(wanted_format) + " input support");
    }

    NV_ENC_PRESET_CONFIG preset{};
    preset.version = NV_ENC_PRESET_CONFIG_VER;
    preset.presetCfg.version = NV_ENC_CONFIG_VER;
    status = state->functions.nvEncGetEncodePresetConfigEx(
        state->encoder, NV_ENC_CODEC_H264_GUID, NV_ENC_PRESET_P1_GUID,
        options.nvenc_tuning, &preset);
    if (status != NV_ENC_SUCCESS) {
        return std::unexpected(std::string("NvEncGetEncodePresetConfigEx(P1/") +
                               nvenc_tuning_name(options.nvenc_tuning) + ") failed with status " +
                               std::to_string(status));
    }
    preset.presetCfg.frameIntervalP = 1U;
    if (options.bitrate_mbps) {
        preset.presetCfg.rcParams.rateControlMode = NV_ENC_PARAMS_RC_VBR;
        preset.presetCfg.rcParams.averageBitRate = *options.bitrate_mbps * 1000000U;
    }
    if (options.h264_coder) {
        preset.presetCfg.encodeCodecConfig.h264Config.entropyCodingMode = *options.h264_coder;
    }
    if (options.color_metadata == ColorMetadata::bt709_limited) {
        NV_ENC_CONFIG_H264_VUI_PARAMETERS& vui =
            preset.presetCfg.encodeCodecConfig.h264Config.h264VUIParameters;
        vui.videoSignalTypePresentFlag = 1U;
        vui.videoFormat = NV_ENC_VUI_VIDEO_FORMAT_COMPONENT;
        vui.videoFullRangeFlag = 0U;
        vui.colourDescriptionPresentFlag = 1U;
        vui.colourPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT709;
        vui.transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_BT709;
        vui.colourMatrix = NV_ENC_VUI_MATRIX_COEFFS_BT709;
    }

    NV_ENC_INITIALIZE_PARAMS initialize{};
    initialize.version = NV_ENC_INITIALIZE_PARAMS_VER;
    initialize.encodeGUID = NV_ENC_CODEC_H264_GUID;
    initialize.presetGUID = NV_ENC_PRESET_P1_GUID;
    initialize.encodeWidth = width;
    initialize.encodeHeight = height;
    initialize.darWidth = width;
    initialize.darHeight = height;
    initialize.frameRateNum = 360U;
    initialize.frameRateDen = 1U;
    initialize.enableEncodeAsync = 0U;
    initialize.enablePTD = 1U;
    initialize.encodeConfig = &preset.presetCfg;
    initialize.tuningInfo = options.nvenc_tuning;
    const NV_ENC_RC_PARAMS& rc = preset.presetCfg.rcParams;
    const NV_ENC_H264_ENTROPY_CODING_MODE coder =
        preset.presetCfg.encodeCodecConfig.h264Config.entropyCodingMode;
    const std::string requested_bitrate = options.bitrate_mbps
        ? std::to_string(*options.bitrate_mbps) : "unset";
    std::printf("stage=nvenc_config passed source=preset_with_opt_ins tuning=%s rc_mode=%s average_bitrate_bps=%u max_bitrate_bps=%u vbv_buffer_bits=%u entropy_coder=%s frame_interval_p=%u gop_length=%u bitrate_option_mbps=%s coder_option=%s color_metadata=%s",
        nvenc_tuning_name(options.nvenc_tuning), nvenc_rc_name(rc.rateControlMode),
        rc.averageBitRate, rc.maxBitRate, rc.vbvBufferSize, nvenc_h264_coder_name(coder),
        preset.presetCfg.frameIntervalP, preset.presetCfg.gopLength,
        requested_bitrate.c_str(),
        options.h264_coder ? nvenc_h264_coder_name(*options.h264_coder) : "unset",
        options.color_metadata == ColorMetadata::bt709_limited ? "bt709-limited" : "unset");
    if (options.color_metadata == ColorMetadata::bt709_limited) {
        std::printf(" video_format=component full_range=0 primaries=bt709 transfer=bt709 matrix=bt709");
    }
    std::putchar('\n');
    status = state->functions.nvEncInitializeEncoder(state->encoder, &initialize);
    if (status != NV_ENC_SUCCESS) {
        return std::unexpected(std::string("nvEncInitializeEncoder(H.264 P1/") +
                               nvenc_tuning_name(options.nvenc_tuning) + ") failed with status " +
                               std::to_string(status));
    }
    std::printf("stage=nvenc_initialize passed codec=H264 preset=P1 tuning=%s dimensions=%ux%u\n",
                nvenc_tuning_name(options.nvenc_tuning), width, height);
    return state;
}

std::expected<std::vector<std::uint8_t>, std::string> get_h264_sequence_params(
    NvencState& nvenc) {
    std::array<std::uint8_t, NV_MAX_SEQ_HDR_LEN> buffer{};
    std::uint32_t payload_size = 0U;
    NV_ENC_SEQUENCE_PARAM_PAYLOAD payload{};
    payload.version = NV_ENC_SEQUENCE_PARAM_PAYLOAD_VER;
    payload.inBufferSize = static_cast<std::uint32_t>(buffer.size());
    payload.spsppsBuffer = buffer.data();
    payload.outSPSPPSPayloadSize = &payload_size;
    const NVENCSTATUS status = nvenc.functions.nvEncGetSequenceParams(
        nvenc.encoder, &payload);
    if (status != NV_ENC_SUCCESS) {
        return std::unexpected("nvEncGetSequenceParams failed with status " +
                               std::to_string(status));
    }
    if (payload_size == 0U || payload_size > buffer.size()) {
        return std::unexpected("nvEncGetSequenceParams returned an empty or oversized SPS/PPS payload");
    }
    return std::vector<std::uint8_t>(buffer.begin(), buffer.begin() + payload_size);
}

std::expected<NV_ENC_REGISTERED_PTR, std::string> register_array(
    NvencState& nvenc, const CUeglFrame& frame, NV_ENC_BUFFER_FORMAT format) {
    NV_ENC_REGISTER_RESOURCE registration{};
    registration.version = NV_ENC_REGISTER_RESOURCE_VER;
    registration.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_CUDAARRAY;
    registration.width = frame.width;
    registration.height = frame.height;
    registration.pitch = frame.width * frame.numChannels;
    registration.resourceToRegister = frame.frame.pArray[0];
    registration.bufferFormat = format;
    registration.bufferUsage = NV_ENC_INPUT_IMAGE;
    const NVENCSTATUS status = nvenc.functions.nvEncRegisterResource(
        nvenc.encoder, &registration);
    if (status != NV_ENC_SUCCESS) {
        return std::unexpected("nvEncRegisterResource failed with status " + std::to_string(status));
    }
    nvenc.registered.push_back(registration.registeredResource);
    std::printf("stage=nvenc_register_cudaarray passed resource_type=CUDAARRAY format=%s pitch=%u\n",
                nvenc_format_name(format), registration.pitch);
    return registration.registeredResource;
}

bool create_bitstream(NvencState& nvenc) {
    NV_ENC_CREATE_BITSTREAM_BUFFER create{};
    create.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
    const NVENCSTATUS status = nvenc.functions.nvEncCreateBitstreamBuffer(nvenc.encoder, &create);
    if (status != NV_ENC_SUCCESS || !create.bitstreamBuffer) {
        std::fprintf(stderr, "stage=nvenc_output_buffer failed status=%u\n", status);
        return false;
    }
    nvenc.bitstreams.push_back(create.bitstreamBuffer);
    if (!nvenc.bitstream) nvenc.bitstream = create.bitstreamBuffer;
    return true;
}

bool ensure_bitstream_count(NvencState& nvenc, std::size_t count) {
    while (nvenc.bitstreams.size() < count) {
        if (!create_bitstream(nvenc)) return false;
    }
    return true;
}

bool map_registered_array_handle(NvencState& nvenc, NV_ENC_REGISTERED_PTR registered,
                                 NV_ENC_BUFFER_FORMAT format, NV_ENC_INPUT_PTR& mapped) {
    NV_ENC_MAP_INPUT_RESOURCE map{};
    map.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
    map.registeredResource = registered;
    const NVENCSTATUS status = nvenc.functions.nvEncMapInputResource(nvenc.encoder, &map);
    if (status != NV_ENC_SUCCESS || !map.mappedResource) {
        std::fprintf(stderr, "stage=nvenc_map_cudaarray failed status=%u\n", status);
        return false;
    }
    if (map.mappedBufferFmt != format) {
        std::fprintf(stderr, "stage=nvenc_map_cudaarray returned unexpected format=%u\n",
                     static_cast<unsigned int>(map.mappedBufferFmt));
        (void)nvenc.functions.nvEncUnmapInputResource(nvenc.encoder, map.mappedResource);
        return false;
    }
    mapped = map.mappedResource;
    {
        std::lock_guard lock(nvenc.mapped_mutex);
        nvenc.mapped_resources.push_back(mapped);
    }
    std::printf("stage=nvenc_map_cudaarray passed format=%s\n", nvenc_format_name(format));
    return true;
}

bool map_registered_array(NvencState& nvenc, NV_ENC_REGISTERED_PTR registered,
                          NV_ENC_BUFFER_FORMAT format) {
    return map_registered_array_handle(nvenc, registered, format, nvenc.mapped);
}

bool unmap_array(NvencState& nvenc, NV_ENC_INPUT_PTR& mapped) {
    if (!mapped) return true;
    const NV_ENC_INPUT_PTR old_mapped = mapped;
    const NVENCSTATUS status = nvenc.functions.nvEncUnmapInputResource(nvenc.encoder, old_mapped);
    if (status != NV_ENC_SUCCESS) {
        std::fprintf(stderr, "stage=nvenc_unmap_cudaarray failed status=%u\n", status);
        return false;
    }
    {
        std::lock_guard lock(nvenc.mapped_mutex);
        std::erase(nvenc.mapped_resources, old_mapped);
    }
    if (nvenc.mapped == old_mapped) nvenc.mapped = nullptr;
    mapped = nullptr;
    return true;
}

bool unmap_array(NvencState& nvenc) {
    return unmap_array(nvenc, nvenc.mapped);
}

struct StageTiming {
    std::uint64_t count = 0;
    std::uint64_t total_ns = 0;
    std::uint64_t max_ns = 0;

    void add(std::chrono::steady_clock::duration duration) noexcept {
        const auto value = std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
        if (value <= 0) return;
        const auto ns = static_cast<std::uint64_t>(value);
        ++count;
        total_ns += ns;
        max_ns = std::max(max_ns, ns);
    }
};

struct SlotInterop {
    EGLImageKHR egl_image = EGL_NO_IMAGE_KHR;
    CudaSlot cuda{};
    NV_ENC_REGISTERED_PTR nvenc_registered = nullptr;
    bool egl_ready = false;
    bool cuda_ready = false;
    bool nvenc_ready = false;
};

struct RunMetrics {
    std::uint64_t warmup_frames = 0;
    std::uint64_t window_dequeued = 0;
    std::uint64_t submissions = 0;
    std::uint64_t successful_submissions = 0;
    std::uint64_t delayed_submissions = 0;
    std::uint64_t encoder_busy_retries = 0;
    std::uint64_t eos_submissions = 0;
    std::uint64_t encoded = 0;
    std::uint64_t bytes = 0;
    std::uint64_t first_source_sequence = 0;
    std::uint64_t last_source_sequence = 0;
    std::uint64_t source_sequence_gaps = 0;
    std::uint64_t timestamp_mismatches = 0;
    std::size_t queue_high_water = 0;
    std::size_t busy_high_water = 0;
    std::size_t pipeline_high_water = 0;
    StageTiming egl_setup;
    StageTiming cuda_context;
    StageTiming nvenc_setup;
    StageTiming warmup_capture_wait;
    StageTiming capture_wait;
    StageTiming fence_wait;
    StageTiming egl_import;
    StageTiming cuda_register;
    StageTiming nvenc_register;
    StageTiming nvenc_map;
    StageTiming nvenc_unmap;
    StageTiming encode_call;
    StageTiming bitstream_lock;
    StageTiming bitstream_write;
    StageTiming bitstream_unlock;
};

class CaptureStopGuard {
public:
    explicit CaptureStopGuard(WaylandCapture& capture) noexcept : capture_(capture) {}
    CaptureStopGuard(const CaptureStopGuard&) = delete;
    CaptureStopGuard& operator=(const CaptureStopGuard&) = delete;
    ~CaptureStopGuard() { stop_and_drain(); }

    void activate() noexcept { active_ = true; }

    void stop_and_drain() noexcept {
        if (!active_) return;
        capture_.stop();
        while (auto queued = capture_.try_pop_frame()) queued->lease.release_after_gpu();
        active_ = false;
    }

private:
    WaylandCapture& capture_;
    bool active_ = false;
};

struct EncodeResult {
    std::uint32_t bytes = 0;
    std::uint64_t output_timestamp = 0;
    std::uint32_t picture_type = 0;
    StageTiming encode_call;
    StageTiming lock_wait;
    StageTiming bitstream_write;
    StageTiming unlock_call;
};

struct EncodeSubmitResult {
    NVENCSTATUS status = NV_ENC_SUCCESS;
    StageTiming call;
};

EncodeSubmitResult submit_mapped_frame(NvencState& nvenc, const CUeglFrame& frame,
                                       NV_ENC_BUFFER_FORMAT format,
                                       NV_ENC_INPUT_PTR mapped,
                                       NV_ENC_OUTPUT_PTR bitstream,
                                       std::uint64_t source_sequence) {
    NV_ENC_PIC_PARAMS picture{};
    picture.version = NV_ENC_PIC_PARAMS_VER;
    picture.inputWidth = frame.width;
    picture.inputHeight = frame.height;
    picture.inputPitch = frame.width * frame.numChannels;
    picture.inputTimeStamp = source_sequence;
    picture.inputDuration = 1U;
    picture.inputBuffer = mapped;
    picture.outputBitstream = bitstream;
    picture.bufferFmt = format;
    picture.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    EncodeSubmitResult submitted;
    const auto begin = std::chrono::steady_clock::now();
    submitted.status = nvenc.functions.nvEncEncodePicture(nvenc.encoder, &picture);
    submitted.call.add(std::chrono::steady_clock::now() - begin);
    return submitted;
}

bool write_all(int fd, const void* source, std::size_t length, std::string_view stage) {
    const auto* bytes = static_cast<const std::uint8_t*>(source);
    std::size_t remaining = length;
    while (remaining != 0U) {
        const ssize_t written = ::write(fd, bytes, remaining);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) {
            std::fprintf(stderr, "stage=%.*s failed errno=%d (%s)\n",
                         static_cast<int>(stage.size()), stage.data(), errno, std::strerror(errno));
            return false;
        }
        const auto count = static_cast<std::size_t>(written);
        bytes += count;
        remaining -= count;
    }
    return true;
}

class OutputFiles {
public:
    explicit OutputFiles(const std::string& stream_path) : stream_path_(stream_path), csv_path_(stream_path + ".frames.csv") {
        csv_.reset(::open(csv_path_.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600));
        if (!csv_) {
            error_ = "could not exclusively create frame CSV " + csv_path_ + ": " + std::strerror(errno);
            return;
        }
        created_csv_ = true;
        stream_.reset(::open(stream_path_.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600));
        if (!stream_) {
            error_ = "could not exclusively create H.264 stream " + stream_path_ + ": " + std::strerror(errno);
            return;
        }
        created_stream_ = true;
        constexpr std::string_view header =
            "encode_index,source_sequence,has_presentation_time,presentation_time_ns,slot_index,"
            "nvenc_output_timestamp,bytes,picture_type\n";
        if (!write_all(csv_.get(), header.data(), header.size(), "csv_header")) {
            error_ = "could not write CSV header";
        }
    }

    ~OutputFiles() {
        if (!complete_) {
            if (created_stream_) (void)::unlink(stream_path_.c_str());
            if (created_csv_) (void)::unlink(csv_path_.c_str());
        }
    }

    [[nodiscard]] bool good() const noexcept { return stream_ && csv_ && error_.empty(); }
    [[nodiscard]] const std::string& error() const noexcept { return error_; }
    [[nodiscard]] int stream_fd() const noexcept { return stream_.get(); }
    [[nodiscard]] int csv_fd() const noexcept { return csv_.get(); }
    void mark_complete() noexcept { complete_ = true; }

    bool flush() {
        if (::fsync(stream_.get()) != 0 || ::fsync(csv_.get()) != 0) {
            error_ = std::string("fsync failed: ") + std::strerror(errno);
            return false;
        }
        return true;
    }

private:
    std::string stream_path_;
    std::string csv_path_;
    UniqueFd stream_;
    UniqueFd csv_;
    std::string error_;
    bool complete_ = false;
    bool created_stream_ = false;
    bool created_csv_ = false;
};

// H.264 Matroska output is isolated behind MuxWriter's compressed-packet queue.
// This object is called only by the NVENC bitstream worker, after copying bytes
// out of NVENC's locked output buffer; it never sees a raw image.
class MatroskaPacketSink {
public:
    MatroskaPacketSink(const std::string& path, unsigned int width,
                       unsigned int height, std::uint32_t bitrate_mbps,
                       std::vector<std::uint8_t> codec_extradata) {
        fluxrec::mux::MuxConfig config;
        config.output_path = path;
        config.codec = fluxrec::media::VideoCodec::h264;
        config.width = static_cast<int>(width);
        config.height = static_cast<int>(height);
        config.stream_time_base = {1, static_cast<int>(kNanosecondsPerSecond)};
        config.frame_rate = {360, 1};
        config.bit_rate = static_cast<std::int64_t>(bitrate_mbps) * 1'000'000LL;
        config.codec_extradata = std::move(codec_extradata);
        config.queue_capacity_packets = 1024U;
        config.queue_capacity_bytes = 128U * 1024U * 1024U;
        writer_ = std::make_unique<fluxrec::mux::MuxWriter>(std::move(config));
        if (!writer_->wait_until_ready(std::chrono::seconds(5))) {
            error_ = writer_->error_message();
            if (error_.empty()) error_ = "MuxWriter did not become ready within five seconds";
            writer_->close();
            throw std::runtime_error(error_);
        }
    }

    MatroskaPacketSink(const MatroskaPacketSink&) = delete;
    MatroskaPacketSink& operator=(const MatroskaPacketSink&) = delete;

    ~MatroskaPacketSink() {
        if (writer_) writer_->close();
    }

    [[nodiscard]] bool enqueue_access_unit(std::vector<std::uint8_t>&& data,
                                           const CapturedFrame& captured,
                                           std::uint64_t output_timestamp,
                                           NV_ENC_PIC_TYPE picture_type) {
        if (!error_.empty()) return false;
        if (!captured.has_presentation_time) {
            return fail("Wayland capture did not provide a source presentation timestamp");
        }
        if (output_timestamp != captured.sequence) {
            return fail("NVENC output timestamp did not match the captured source sequence");
        }
        if (captured.presentation_time_ns >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return fail("source presentation timestamp exceeds the signed packet timestamp range");
        }

        fluxrec::media::EncodedPacket packet;
        packet.data = std::move(data);
        packet.codec = fluxrec::media::VideoCodec::h264;
        packet.pts = static_cast<std::int64_t>(captured.presentation_time_ns);
        // The probe configures frameIntervalP=1, so it emits no B-frames and
        // source presentation time is also the decode timestamp.
        packet.dts = packet.pts;
        packet.duration = 0;
        packet.keyframe = picture_type == NV_ENC_PIC_TYPE_IDR;
        packet.time_base = {1, static_cast<int>(kNanosecondsPerSecond)};

        if (pending_) {
            if (packet.pts <= pending_->pts) {
                return fail("source presentation timestamps are not strictly increasing");
            }
            const std::int64_t delta = packet.pts - pending_->pts;
            pending_->duration = delta;
            last_duration_ns_ = delta;
            if (!submit_pending()) return false;
        }
        pending_.emplace(std::move(packet));
        return true;
    }

    [[nodiscard]] bool finish_packets() {
        if (!error_.empty()) return false;
        if (!pending_) return true;
        pending_->duration = last_duration_ns_ > 0
            ? last_duration_ns_ : kNominalFrameDurationNs;
        return submit_pending();
    }

    [[nodiscard]] bool close_and_check() {
        if (!writer_) return error_.empty();
        writer_->close();
        if (writer_->state() == fluxrec::mux::WriterState::failed && error_.empty()) {
            error_ = writer_->error_message();
            if (error_.empty()) error_ = "MuxWriter failed while writing Matroska output";
        }
        if (writer_->state() == fluxrec::mux::WriterState::closed && error_.empty()) {
            const auto result = writer_->stats();
            if (result.enqueued_packets != result.muxed_video_packets) {
                error_ = "Matroska writer closed without muxing every accepted video packet";
            }
        }
        return error_.empty();
    }

    [[nodiscard]] const std::string& error() const noexcept { return error_; }
    [[nodiscard]] fluxrec::mux::MuxStats stats() const noexcept {
        return writer_ ? writer_->stats() : fluxrec::mux::MuxStats{};
    }

private:
    [[nodiscard]] bool fail(std::string message) {
        if (error_.empty()) error_ = std::move(message);
        return false;
    }

    [[nodiscard]] bool submit_pending() {
        if (!pending_) return true;
        for (unsigned int attempt = 0U; attempt < 64U; ++attempt) {
            const auto result = writer_->try_enqueue(std::move(*pending_));
            if (result == fluxrec::mux::EnqueueResult::accepted) {
                pending_.reset();
                return true;
            }
            if (result == fluxrec::mux::EnqueueResult::queue_busy) {
                std::this_thread::yield();
                continue;
            }
            const char* reason = "unknown enqueue error";
            switch (result) {
                case fluxrec::mux::EnqueueResult::accepted: return true;
                case fluxrec::mux::EnqueueResult::queue_full: reason = "packet queue is full"; break;
                case fluxrec::mux::EnqueueResult::queue_busy: reason = "packet queue is busy"; break;
                case fluxrec::mux::EnqueueResult::packet_too_large: reason = "packet exceeds the queue byte limit"; break;
                case fluxrec::mux::EnqueueResult::invalid_packet: reason = "MuxWriter rejected the packet metadata"; break;
                case fluxrec::mux::EnqueueResult::not_ready: reason = "MuxWriter is not ready"; break;
                case fluxrec::mux::EnqueueResult::closed: reason = "MuxWriter is closed"; break;
            }
            return fail(reason);
        }
        return fail("MuxWriter queue remained contended after bounded enqueue retries");
    }

    std::unique_ptr<fluxrec::mux::MuxWriter> writer_;
    std::optional<fluxrec::media::EncodedPacket> pending_;
    std::int64_t last_duration_ns_{0};
    std::string error_;
};

bool write_csv_row(OutputFiles& output, std::uint64_t encode_index,
                   const fluxrec::capture::CapturedFrame& captured, std::uint32_t bytes,
                   std::uint64_t output_timestamp, std::uint32_t picture_type) {
    char line[256]{};
    const int length = std::snprintf(line, sizeof(line), "%llu,%llu,%u,%llu,%zu,%llu,%u,%u\n",
        static_cast<unsigned long long>(encode_index),
        static_cast<unsigned long long>(captured.sequence),
        captured.has_presentation_time ? 1U : 0U,
        static_cast<unsigned long long>(captured.presentation_time_ns),
        captured.lease.slot_index(),
        static_cast<unsigned long long>(output_timestamp), bytes, picture_type);
    return length > 0 && static_cast<std::size_t>(length) < sizeof(line) &&
        write_all(output.csv_fd(), line, static_cast<std::size_t>(length), "csv_write");
}

struct PendingEncode {
    CapturedFrame captured;
    NV_ENC_INPUT_PTR mapped_input = nullptr;
    NV_ENC_OUTPUT_PTR output_bitstream = nullptr;
    std::uint64_t encode_index = 0;
    bool ready = false;
};

struct PipelineWorkerMetrics {
    std::uint64_t encoded = 0;
    std::uint64_t bytes = 0;
    std::uint64_t locks = 0;
    std::uint64_t lock_successes = 0;
    std::uint64_t unlocks = 0;
    std::uint64_t unlock_successes = 0;
    std::uint64_t unmaps = 0;
    std::uint64_t leases_released = 0;
    std::uint64_t timestamp_mismatches = 0;
    std::uint64_t source_sequence_gaps = 0;
    std::uint64_t first_source_sequence = 0;
    std::uint64_t last_source_sequence = 0;
    StageTiming lock_wait;
    StageTiming bitstream_write;
    StageTiming unlock_call;
    StageTiming unmap_call;
};

struct PipelineState {
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<PendingEncode> queue;
    std::vector<CapturedFrame> deferred_release;
    PipelineWorkerMetrics worker_metrics;
    std::size_t in_flight = 0;
    std::size_t unready = 0;
    std::size_t in_flight_high_water = 0;
    bool eos_flushed = false;
    bool producer_done = false;
    std::string error;
};

void set_pipeline_error(PipelineState& state, std::string error) {
    std::lock_guard lock(state.mutex);
    if (state.error.empty()) state.error = std::move(error);
    state.changed.notify_all();
}

void pipeline_output_worker(PipelineState& state, NvencState& nvenc,
                            OutputFiles* output, MatroskaPacketSink* mux_output) {
    while (true) {
        std::optional<PendingEncode> pending_job;
        bool producer_finished = false;
        {
            std::unique_lock lock(state.mutex);
            state.changed.wait(lock, [&] {
                return state.producer_done || (!state.queue.empty() && state.queue.front().ready);
            });
            if (state.queue.empty() && state.producer_done) {
                producer_finished = true;
            } else if (state.queue.empty() || !state.queue.front().ready) {
                // EOS failed. Keep unready inputs and their leases alive until
                // the encoder object is destroyed by the caller.
                return;
            } else {
                pending_job.emplace(std::move(state.queue.front()));
                state.queue.pop_front();
            }
        }
        if (producer_finished) {
            if (mux_output && !mux_output->finish_packets()) {
                set_pipeline_error(state, "final Matroska packet enqueue failed: " + mux_output->error());
            }
            return;
        }
        PendingEncode& job = *pending_job;

        NV_ENC_LOCK_BITSTREAM lock{};
        lock.version = NV_ENC_LOCK_BITSTREAM_VER;
        lock.outputBitstream = job.output_bitstream;
        auto begin = std::chrono::steady_clock::now();
        NVENCSTATUS lock_status = nvenc.functions.nvEncLockBitstream(nvenc.encoder, &lock);
        state.worker_metrics.lock_wait.add(std::chrono::steady_clock::now() - begin);
        ++state.worker_metrics.locks;
        if (lock_status != NV_ENC_SUCCESS) {
            set_pipeline_error(state, "nvEncLockBitstream failed with status " +
                std::to_string(lock_status) + "; waiting for EOS flush before cleanup");
            std::unique_lock lock_state(state.mutex);
            state.changed.wait(lock_state, [&] { return state.eos_flushed || state.producer_done; });
            if (!state.eos_flushed) {
                state.queue.push_front(std::move(job));
                state.changed.notify_all();
                return;
            }
            lock = {};
            lock.version = NV_ENC_LOCK_BITSTREAM_VER;
            lock.outputBitstream = job.output_bitstream;
            begin = std::chrono::steady_clock::now();
            lock_status = nvenc.functions.nvEncLockBitstream(nvenc.encoder, &lock);
            state.worker_metrics.lock_wait.add(std::chrono::steady_clock::now() - begin);
            ++state.worker_metrics.locks;
        }

        bool output_valid = false;
        bool output_ready = false;
        std::vector<std::uint8_t> mux_data;
        if (lock_status == NV_ENC_SUCCESS) {
            ++state.worker_metrics.lock_successes;
            nvenc.locked_bitstreams.push_back(job.output_bitstream);
            output_valid = lock.bitstreamBufferPtr != nullptr && lock.bitstreamSizeInBytes != 0U;
            if (!output_valid) {
                set_pipeline_error(state, "NVENC locked a bitstream with an empty pointer or size");
            } else if (mux_output != nullptr) {
                begin = std::chrono::steady_clock::now();
                const auto* bytes = static_cast<const std::uint8_t*>(lock.bitstreamBufferPtr);
                mux_data.assign(bytes, bytes + lock.bitstreamSizeInBytes);
                state.worker_metrics.bitstream_write.add(std::chrono::steady_clock::now() - begin);
                output_ready = true;
            } else {
                begin = std::chrono::steady_clock::now();
                output_ready = write_all(output->stream_fd(), lock.bitstreamBufferPtr,
                    lock.bitstreamSizeInBytes, "output_write");
                state.worker_metrics.bitstream_write.add(std::chrono::steady_clock::now() - begin);
                if (!output_ready) set_pipeline_error(state, "writing the encoded H.264 bitstream failed");
            }
            // These identity counters describe NVENC output and apply equally
            // to the raw H.264 writer and the Matroska packet sink. Keep them
            // outside the sink-specific path so muxing cannot silently report
            // zero source IDs for successfully encoded frames.
            if (output_valid) {
                if (lock.outputTimeStamp != job.captured.sequence) {
                    ++state.worker_metrics.timestamp_mismatches;
                }
                if (state.worker_metrics.last_source_sequence != 0U &&
                    job.captured.sequence > state.worker_metrics.last_source_sequence + 1U) {
                    state.worker_metrics.source_sequence_gaps += job.captured.sequence -
                        state.worker_metrics.last_source_sequence - 1U;
                }
                if (state.worker_metrics.first_source_sequence == 0U) {
                    state.worker_metrics.first_source_sequence = job.captured.sequence;
                }
                state.worker_metrics.last_source_sequence = job.captured.sequence;
            }
            const auto unlock_begin = std::chrono::steady_clock::now();
            const NVENCSTATUS unlock_status = nvenc.functions.nvEncUnlockBitstream(
                nvenc.encoder, job.output_bitstream);
            state.worker_metrics.unlock_call.add(std::chrono::steady_clock::now() - unlock_begin);
            ++state.worker_metrics.unlocks;
            if (unlock_status != NV_ENC_SUCCESS) {
                set_pipeline_error(state, "nvEncUnlockBitstream failed with status " +
                    std::to_string(unlock_status));
            } else {
                ++state.worker_metrics.unlock_successes;
                std::erase(nvenc.locked_bitstreams, job.output_bitstream);
            }
            if (output_valid && output_ready && unlock_status == NV_ENC_SUCCESS) {
                bool accepted = false;
                if (mux_output != nullptr) {
                    accepted = mux_output->enqueue_access_unit(
                        std::move(mux_data), job.captured, lock.outputTimeStamp, lock.pictureType);
                    if (!accepted) {
                        set_pipeline_error(state, "enqueueing an H.264 packet to Matroska failed: " +
                            mux_output->error());
                    }
                } else if (write_csv_row(*output, job.encode_index, job.captured,
                        lock.bitstreamSizeInBytes, lock.outputTimeStamp,
                        static_cast<std::uint32_t>(lock.pictureType))) {
                    accepted = true;
                } else {
                    set_pipeline_error(state, "writing the frame correlation CSV failed");
                }
                if (accepted) {
                    ++state.worker_metrics.encoded;
                    state.worker_metrics.bytes += lock.bitstreamSizeInBytes;
                }
            }
        } else {
            set_pipeline_error(state, "nvEncLockBitstream still failed after EOS flush with status " +
                std::to_string(lock_status));
        }

        if (lock_status != NV_ENC_SUCCESS) {
            // A failed post-flush lock is not proof that the encoder stopped
            // reading this input. Keep both its mapping and capture lease alive
            // until NvencState tears down the encoder and unmaps tracked inputs.
            state.deferred_release.push_back(std::move(job.captured));
        } else {
            const auto unmap_begin = std::chrono::steady_clock::now();
            const bool unmapped = unmap_array(nvenc, job.mapped_input);
            state.worker_metrics.unmap_call.add(std::chrono::steady_clock::now() - unmap_begin);
            if (unmapped) {
                ++state.worker_metrics.unmaps;
                job.captured.lease.release_after_gpu();
                ++state.worker_metrics.leases_released;
            } else {
                set_pipeline_error(state, "unmapping a completed NVENC input failed; deferring its lease until encoder teardown");
                state.deferred_release.push_back(std::move(job.captured));
            }
        }

        {
            std::lock_guard lock_state(state.mutex);
            if (state.in_flight > 0U) --state.in_flight;
            state.changed.notify_all();
        }
    }
}

std::expected<EncodeResult, std::string> encode_mapped_frame(
    NvencState& nvenc, const CUeglFrame& frame, NV_ENC_BUFFER_FORMAT format,
    std::uint64_t source_sequence, OutputFiles& output) {
    if (!nvenc.mapped || !nvenc.bitstream) {
        return std::unexpected("NVENC input or output buffer is not mapped/created");
    }
    NV_ENC_PIC_PARAMS picture{};
    picture.version = NV_ENC_PIC_PARAMS_VER;
    picture.inputWidth = frame.width;
    picture.inputHeight = frame.height;
    picture.inputPitch = frame.width * frame.numChannels;
    picture.inputTimeStamp = source_sequence;
    picture.inputDuration = 1U;
    picture.inputBuffer = nvenc.mapped;
    picture.outputBitstream = nvenc.bitstream;
    picture.bufferFmt = format;
    picture.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;

    EncodeResult encoded;
    auto begin = std::chrono::steady_clock::now();
    NVENCSTATUS status = nvenc.functions.nvEncEncodePicture(nvenc.encoder, &picture);
    encoded.encode_call.add(std::chrono::steady_clock::now() - begin);
    if (status != NV_ENC_SUCCESS) {
        return std::unexpected("nvEncEncodePicture failed with status " + std::to_string(status));
    }

    NV_ENC_LOCK_BITSTREAM lock{};
    lock.version = NV_ENC_LOCK_BITSTREAM_VER;
    lock.outputBitstream = nvenc.bitstream;
    begin = std::chrono::steady_clock::now();
    status = nvenc.functions.nvEncLockBitstream(nvenc.encoder, &lock);
    encoded.lock_wait.add(std::chrono::steady_clock::now() - begin);
    if (status != NV_ENC_SUCCESS) {
        return std::unexpected("nvEncLockBitstream failed status=" + std::to_string(status));
    }
    if (!lock.bitstreamBufferPtr || lock.bitstreamSizeInBytes == 0U) {
        const NVENCSTATUS unlock_status = nvenc.functions.nvEncUnlockBitstream(
            nvenc.encoder, nvenc.bitstream);
        if (unlock_status != NV_ENC_SUCCESS) {
            return std::unexpected("nvEncLockBitstream returned an empty output and unlock failed status=" +
                                   std::to_string(unlock_status));
        }
        return std::unexpected("nvEncLockBitstream returned an empty output buffer");
    }
    encoded.bytes = lock.bitstreamSizeInBytes;
    encoded.output_timestamp = lock.outputTimeStamp;
    encoded.picture_type = static_cast<std::uint32_t>(lock.pictureType);
    begin = std::chrono::steady_clock::now();
    const bool wrote = write_all(output.stream_fd(), lock.bitstreamBufferPtr,
                                 lock.bitstreamSizeInBytes, "output_write");
    encoded.bitstream_write.add(std::chrono::steady_clock::now() - begin);
    begin = std::chrono::steady_clock::now();
    const NVENCSTATUS unlock_status = nvenc.functions.nvEncUnlockBitstream(nvenc.encoder, nvenc.bitstream);
    encoded.unlock_call.add(std::chrono::steady_clock::now() - begin);
    if (unlock_status != NV_ENC_SUCCESS) {
        return std::unexpected("nvEncUnlockBitstream failed status=" + std::to_string(unlock_status));
    }
    if (!wrote) return std::unexpected("writing the encoded H.264 bitstream failed");
    return encoded;
}

std::expected<void, std::string> prepare_slot_resources(
    std::size_t slot_index, CapturedFrame& captured, std::string_view render_node,
    unsigned int width, unsigned int height, std::uint32_t expected_fourcc,
    std::uint64_t expected_modifier, std::vector<SlotInterop>& slots,
    std::unique_ptr<EglState>& egl, std::unique_ptr<CudaState>& cuda,
    std::unique_ptr<NvencState>& nvenc, RunMetrics& metrics,
    const Options& options, ResourceStage target = ResourceStage::nvenc) {
    if (slot_index >= slots.size()) {
        return std::unexpected("lease slot index exceeds the configured pool");
    }
    const DmabufImage& source = captured.lease.image();
    if (source.fourcc != expected_fourcc || source.modifier != expected_modifier ||
        source.width != width || source.height != height || source.planes.size() != 1U) {
        return std::unexpected("leased image metadata differs from the selected one-plane pool format");
    }
    SlotInterop& slot = slots[slot_index];

    if (!egl) {
        const auto begin = std::chrono::steady_clock::now();
        auto result = initialize_egl_for_capture_device(render_node, source);
        metrics.egl_setup.add(std::chrono::steady_clock::now() - begin);
        if (!result) return std::unexpected("EGL setup: " + result.error());
        egl = std::move(*result);
    }
    if (!slot.egl_ready) {
        auto source_fd_result = captured.lease.duplicate_plane_fd(0U);
        if (!source_fd_result) return std::unexpected("DMA-BUF duplicate: " + source_fd_result.error().message);
        UniqueFd source_fd = std::move(*source_fd_result);
        const auto egl_begin = std::chrono::steady_clock::now();
        auto egl_image = import_egl_image(*egl, source, source_fd.get());
        metrics.egl_import.add(std::chrono::steady_clock::now() - egl_begin);
        if (!egl_image) return std::unexpected("EGLImage import: " + egl_image.error());
        slot.egl_image = *egl_image;
        slot.egl_ready = true;
    }
    if (target == ResourceStage::egl) {
        std::printf("stage=slot_resources passed slot=%zu retained_stage=egl\n", slot_index);
        return {};
    }

    if (!cuda) {
        const auto begin = std::chrono::steady_clock::now();
        auto result = initialize_cuda(egl->cuda_ordinal);
        metrics.cuda_context.add(std::chrono::steady_clock::now() - begin);
        if (!result) return std::unexpected("CUDA context setup: " + result.error());
        cuda = std::move(*result);
    }
    if (!slot.cuda_ready) {
        const auto cuda_begin = std::chrono::steady_clock::now();
        auto cuda_slot = register_cuda_egl_image(*cuda, slot.egl_image);
        metrics.cuda_register.add(std::chrono::steady_clock::now() - cuda_begin);
        if (!cuda_slot) return std::unexpected("CUDA EGL registration: " + cuda_slot.error());
        slot.cuda = *cuda_slot;
        if (slot.cuda.frame.width != width || slot.cuda.frame.height != height ||
            slot.cuda.frame.numChannels != 4U) {
            return std::unexpected("CUDA EGL array dimensions or packed four-channel layout differs from capture");
        }
        slot.cuda_ready = true;
    }
    if (target == ResourceStage::cuda) {
        std::printf("stage=slot_resources passed slot=%zu retained_stage=cuda\n", slot_index);
        return {};
    }

    if (!nvenc) {
        const auto begin = std::chrono::steady_clock::now();
        auto result = initialize_nvenc(cuda->context, width, height,
                                       NV_ENC_BUFFER_FORMAT_ARGB, options);
        metrics.nvenc_setup.add(std::chrono::steady_clock::now() - begin);
        if (!result) return std::unexpected("NVENC setup: " + result.error());
        nvenc = std::move(*result);
    }
    if (!slot.nvenc_ready) {
        const auto nvenc_begin = std::chrono::steady_clock::now();
        auto registered = register_array(*nvenc, slot.cuda.frame, NV_ENC_BUFFER_FORMAT_ARGB);
        metrics.nvenc_register.add(std::chrono::steady_clock::now() - nvenc_begin);
        if (!registered) return std::unexpected("NVENC CUDAARRAY registration: " + registered.error());
        slot.nvenc_registered = *registered;
        slot.nvenc_ready = true;
    }
    std::printf("stage=slot_resources passed slot=%zu strategy=cached_per_pool_slot\n", slot_index);
    return {};
}

bool retire_slot_resources(SlotInterop& slot, EglState* egl, CudaState* cuda,
                           NvencState* nvenc, std::string& error) {
    if (slot.nvenc_ready) {
        if (!nvenc || !nvenc->encoder || !slot.nvenc_registered) {
            error = "NVENC state is missing while retiring a registered input";
            return false;
        }
        const NVENCSTATUS status = nvenc->functions.nvEncUnregisterResource(
            nvenc->encoder, slot.nvenc_registered);
        if (status != NV_ENC_SUCCESS) {
            error = "nvEncUnregisterResource failed with status " + std::to_string(status);
            return false;
        }
        std::erase(nvenc->registered, slot.nvenc_registered);
        slot.nvenc_registered = nullptr;
        slot.nvenc_ready = false;
    }
    if (slot.cuda_ready) {
        if (!cuda || !slot.cuda.resource) {
            error = "CUDA state is missing while retiring an EGL registration";
            return false;
        }
        const CUresult result = cuda->api.graphics_unregister_resource(slot.cuda.resource);
        if (result != 0) {
            error = "cuGraphicsUnregisterResource failed: " + cuda->api.describe(result);
            return false;
        }
        std::erase(cuda->resources, slot.cuda.resource);
        slot.cuda = {};
        slot.cuda_ready = false;
    }
    if (slot.egl_ready) {
        if (!egl || !egl->api.destroy_image ||
            egl->api.destroy_image(egl->display, slot.egl_image) != EGL_TRUE) {
            error = "eglDestroyImageKHR failed while retiring the slot EGLImage";
            return false;
        }
        std::erase(egl->images, slot.egl_image);
        slot.egl_image = EGL_NO_IMAGE_KHR;
        slot.egl_ready = false;
    }
    std::printf("stage=slot_resources_retired passed\n");
    return true;
}

void update_pool_high_water(RunMetrics& metrics, const fluxrec::capture::CaptureStats& stats) {
    metrics.queue_high_water = std::max(metrics.queue_high_water, stats.frame_queue_occupancy);
    metrics.busy_high_water = std::max(metrics.busy_high_water, stats.pool_busy_slots);
}

const char* capture_status_name(fluxrec::capture::CaptureStatus status) {
    switch (status) {
        case fluxrec::capture::CaptureStatus::connected: return "connected";
        case fluxrec::capture::CaptureStatus::running: return "running";
        case fluxrec::capture::CaptureStatus::stopped: return "stopped";
        case fluxrec::capture::CaptureStatus::failed: return "failed";
    }
    return "unknown";
}

const char* resource_stage_name(ResourceStage stage) {
    switch (stage) {
        case ResourceStage::none: return "none";
        case ResourceStage::egl: return "egl";
        case ResourceStage::cuda: return "cuda";
        case ResourceStage::nvenc: return "nvenc";
    }
    return "unknown";
}

void print_capture_snapshot(std::string_view stage, WaylandCapture& capture) {
    const auto stats = capture.stats();
    const auto error = capture.runtime_error();
    const std::uint64_t settled = stats.completed + stats.failures + stats.cancelled_at_stop;
    const std::uint64_t unsettled_estimate = stats.requests > settled ? stats.requests - settled : 0U;
    std::fprintf(stderr,
        "capture_snapshot stage=%.*s status=%s requests=%llu completed=%llu admitted=%llu "
        "dropped=%llu rate_limited=%llu failures=%llu cancelled=%llu unsettled_request_estimate=%llu "
        "pool_size=%zu free=%zu capturing=%zu leased=%zu busy=%zu queue_occupancy=%zu runtime_error=%s\n",
        static_cast<int>(stage.size()), stage.data(), capture_status_name(capture.status()),
        static_cast<unsigned long long>(stats.requests),
        static_cast<unsigned long long>(stats.completed),
        static_cast<unsigned long long>(stats.admitted),
        static_cast<unsigned long long>(stats.dropped),
        static_cast<unsigned long long>(stats.rate_limited),
        static_cast<unsigned long long>(stats.failures),
        static_cast<unsigned long long>(stats.cancelled_at_stop),
        static_cast<unsigned long long>(unsettled_estimate), stats.pool_size,
        stats.pool_free_slots, stats.pool_capturing_slots, stats.pool_leased_slots,
        stats.pool_busy_slots, stats.frame_queue_occupancy,
        error ? error->message.c_str() : "none");
}

void print_timing(std::string_view stage, const StageTiming& timing) {
    const std::uint64_t average = timing.count == 0U ? 0U : timing.total_ns / timing.count;
    std::printf("timing stage=%.*s count=%llu total_ns=%llu average_ns=%llu max_ns=%llu\n",
        static_cast<int>(stage.size()), stage.data(),
        static_cast<unsigned long long>(timing.count),
        static_cast<unsigned long long>(timing.total_ns),
        static_cast<unsigned long long>(average),
        static_cast<unsigned long long>(timing.max_ns));
}

} // namespace

int main(int argc, char** argv) {
    const auto parsed = parse_options(argc, argv);
    if (!parsed) return argc == 2 && std::string_view(argv[1]) == "--help" ? 0 : 2;
    const Options& options = *parsed;

    fluxrec::capture::CaptureConfig capture_config;
    capture_config.output_name = options.output_name;
    capture_config.frame_queue_capacity = options.pool_slots;
    // Rearm isolation tests need a quiet capture queue so the frame returned
    // after the registration baseline must come from a new Wayland request.
    capture_config.max_fps = options.rearm_check_stage ? 1U : 0U;
    auto capture_result = fluxrec::capture::WaylandCapture::connect(capture_config);
    if (!capture_result) {
        std::fprintf(stderr, "stage=capture_connect failed: %s\n",
                     capture_result.error().message.c_str());
        return 1;
    }
    auto capture = std::move(*capture_result);
    const CaptureRequirements& requirements = capture.requirements();
    if (!requirements.dmabuf_device) {
        std::fprintf(stderr, "stage=capture_negotiation failed: no DMA-BUF device\n");
        return 1;
    }
    const auto render_node = render_node_for_device(*requirements.dmabuf_device);
    if (!render_node) {
        std::fprintf(stderr, "stage=capture_negotiation failed: DMA-BUF device is not in /dev/dri\n");
        return 1;
    }
    UniqueFd render_fd{::open(render_node->c_str(), O_RDWR | O_CLOEXEC)};
    if (!render_fd) {
        std::fprintf(stderr, "stage=gbm_open failed path=%s error=%s\n",
                     render_node->c_str(), std::strerror(errno));
        return 1;
    }
    std::unique_ptr<gbm_device, decltype(&gbm_device_destroy)> gbm{
        gbm_create_device(render_fd.get()), gbm_device_destroy};
    if (!gbm) {
        std::fprintf(stderr, "stage=gbm_device failed path=%s\n", render_node->c_str());
        return 1;
    }

    std::vector<PoolSlot> pool;
    std::uint32_t selected_fourcc = 0;
    std::uint64_t selected_modifier = 0;
    if (!allocate_pool(gbm.get(), requirements, pool, selected_fourcc, selected_modifier,
                       options.pool_slots)) {
        std::fprintf(stderr, "stage=gbm_pool failed: could not allocate %u exact-modifier slots\n",
                     options.pool_slots);
        return 1;
    }
    std::vector<DmabufImage> images;
    images.reserve(pool.size());
    for (const PoolSlot& slot : pool) images.push_back(slot.image);
    auto set_pool = capture.set_buffer_pool(images);
    if (!set_pool) {
        std::fprintf(stderr, "stage=capture_pool failed: %s\n", set_pool.error().message.c_str());
        return 1;
    }
    std::printf("stage=capture_pool passed output=%s dimensions=%ux%u render_node=%s format=%s modifier=0x%016llx slots=%zu\n",
                options.output_name.c_str(), requirements.width, requirements.height,
                render_node->c_str(), fourcc_string(selected_fourcc).c_str(),
                static_cast<unsigned long long>(selected_modifier), pool.size());
    if (options.rearm_check_stage) {
        std::printf("stage=rearm_capture_pacing passed max_fps=1 queue_capacity=%u\n",
                    options.pool_slots);
    }
    std::optional<CapturedFrame> active_frame;
    // Kept before NvencState so any lease whose input could not be safely
    // unmapped is destroyed only after the NVENC session teardown.
    std::vector<CapturedFrame> deferred_release_frames;
    std::unique_ptr<EglState> egl;
    std::unique_ptr<CudaState> cuda;
    std::unique_ptr<NvencState> nvenc;
    std::unique_ptr<OutputFiles> output;
    std::unique_ptr<MatroskaPacketSink> mux_output;
    std::vector<SlotInterop> slot_interop(pool.size());
    RunMetrics metrics;
    CaptureStopGuard stop_capture{capture};
    bool active_fence_ready = false;

    if (!options.register_only && !options.rearm_check_stage && options.output_mkv.empty()) {
        output = std::make_unique<OutputFiles>(options.output_file);
        if (!output->good()) {
            std::fprintf(stderr, "stage=output_create failed: %s\n", output->error().c_str());
            return 1;
        }
    }
    auto started = capture.start();
    if (!started) {
        std::fprintf(stderr, "stage=capture_start failed: %s\n", started.error().message.c_str());
        return 1;
    }
    stop_capture.activate();
    auto fetch_frame = [&](int timeout_ms, StageTiming& timing, bool log_frame) -> bool {
        const auto begin = std::chrono::steady_clock::now();
        active_frame = capture.wait_pop_frame(std::chrono::milliseconds(timeout_ms));
        timing.add(std::chrono::steady_clock::now() - begin);
        if (active_frame && active_frame->lease) {
            active_fence_ready = false;
            if (log_frame) {
                std::printf("stage=capture_frame passed sequence=%llu slot=%zu\n",
                    static_cast<unsigned long long>(active_frame->sequence),
                    active_frame->lease.slot_index());
            }
            update_pool_high_water(metrics, capture.stats());
            return true;
        }
        return false;
    };

    auto prepare_current = [&](bool log_fence, ResourceStage target) -> bool {
        if (!active_frame || !active_frame->lease) return false;
        const std::size_t slot_index = active_frame->lease.slot_index();
        auto source_fd = active_frame->lease.duplicate_plane_fd(0U);
        if (!source_fd) {
            std::fprintf(stderr, "stage=dmabuf_dup failed: %s\n", source_fd.error().message.c_str());
            return false;
        }
        const auto fence_begin = std::chrono::steady_clock::now();
        const bool fence_ready = wait_for_dma_buf_read_fence(source_fd->get(), log_fence);
        metrics.fence_wait.add(std::chrono::steady_clock::now() - fence_begin);
        if (!fence_ready) return false;
        auto result = prepare_slot_resources(slot_index, *active_frame, *render_node,
            requirements.width, requirements.height, selected_fourcc, selected_modifier,
            slot_interop, egl, cuda, nvenc, metrics, options, target);
        if (!result) {
            std::fprintf(stderr, "stage=slot_resources failed: %s\n", result.error().c_str());
            return false;
        }
        active_fence_ready = true;
        return true;
    };

    if (options.rearm_check_stage) {
        if (!fetch_frame(options.capture_wait_ms, metrics.warmup_capture_wait, true)) {
            print_capture_snapshot("rearm_first_frame_timeout", capture);
            const auto runtime_error = capture.runtime_error();
            std::fprintf(stderr, "stage=rearm_check_first_frame failed: %s\n",
                runtime_error ? runtime_error->message.c_str() : "timed out waiting for first ready frame");
            return 1;
        }
        ++metrics.warmup_frames;
        const ResourceStage stage = *options.rearm_check_stage;
        if (stage != ResourceStage::none && !prepare_current(true, stage)) return 1;
        print_capture_snapshot("rearm_before_lease_release", capture);
        if (options.retire_before_wait) {
            std::string retire_error;
            SlotInterop& slot = slot_interop[active_frame->lease.slot_index()];
            if (!retire_slot_resources(slot, egl.get(), cuda.get(), nvenc.get(), retire_error)) {
                std::fprintf(stderr, "stage=rearm_resource_retirement failed: %s\n", retire_error.c_str());
                return 1;
            }
        }
        std::vector<CapturedFrame> held_prebaseline_frames;
        held_prebaseline_frames.reserve(options.pool_slots);
        while (auto queued = capture.try_pop_frame()) {
            held_prebaseline_frames.push_back(std::move(*queued));
        }
        const std::size_t drained_prebaseline = held_prebaseline_frames.size();
        const std::size_t registered_slot = active_frame->lease.slot_index();
        const auto baseline_stats = capture.stats();
        print_capture_snapshot("rearm_post_registration_baseline", capture);
        if (baseline_stats.frame_queue_occupancy != 0U ||
            baseline_stats.pool_capturing_slots != 0U ||
            baseline_stats.requests != baseline_stats.completed + baseline_stats.failures) {
            std::fprintf(stderr,
                "stage=rearm_baseline failed: request still in flight or queue not quiet "
                "after draining %zu pre-baseline frames\n", drained_prebaseline);
            return 1;
        }
        const std::uint64_t baseline_requests = baseline_stats.requests;
        const std::uint64_t baseline_completed = baseline_stats.completed;
        std::printf("stage=rearm_baseline passed requests=%llu completed=%llu held_prebaseline=%zu retained_slot=%zu\n",
            static_cast<unsigned long long>(baseline_requests),
            static_cast<unsigned long long>(baseline_completed), drained_prebaseline,
            registered_slot);
        active_frame->lease.release_after_gpu();
        active_frame.reset();
        active_fence_ready = false;
        print_capture_snapshot(options.retire_before_wait
            ? "rearm_after_resource_retirement" : "rearm_after_lease_release", capture);

        if (!fetch_frame(options.capture_wait_ms, metrics.warmup_capture_wait, true)) {
            print_capture_snapshot("rearm_second_frame_timeout", capture);
            std::fprintf(stderr, "stage=rearm_check_second_frame failed stage=%s resources=%s\n",
                resource_stage_name(stage), options.retire_before_wait ? "retired" : "retained");
            return 1;
        }
        const std::uint64_t second_sequence = active_frame->sequence;
        const std::size_t second_slot = active_frame->lease.slot_index();
        const auto after_second_stats = capture.stats();
        if (second_slot != registered_slot ||
            second_sequence <= baseline_completed ||
            after_second_stats.completed <= baseline_completed ||
            after_second_stats.requests <= baseline_requests) {
            print_capture_snapshot("rearm_nonfresh_frame", capture);
            std::fprintf(stderr,
                "stage=rearm_check_second_frame failed: frame did not follow the quiet baseline "
                "or did not reuse retained slot %zu (got slot %zu)\n",
                registered_slot, second_slot);
            return 1;
        }
        active_frame->lease.release_after_gpu();
        active_frame.reset();
        for (CapturedFrame& held : held_prebaseline_frames) {
            held.lease.release_after_gpu();
        }
        held_prebaseline_frames.clear();
        stop_capture.stop_and_drain();
        std::printf("probe_result=rearm_check_passed stage=%s resources=%s baseline_requests=%llu baseline_completed=%llu held_prebaseline=%zu registered_slot=%zu second_sequence=%llu second_slot=%zu\n",
            resource_stage_name(stage), options.retire_before_wait ? "retired_before_wait" : "retained",
            static_cast<unsigned long long>(baseline_requests),
            static_cast<unsigned long long>(baseline_completed),
            drained_prebaseline, registered_slot,
            static_cast<unsigned long long>(second_sequence), second_slot);
        print_capture_snapshot("rearm_check_complete", capture);
        return 0;
    }

    if (options.register_only) {
        if (!fetch_frame(options.capture_wait_ms, metrics.warmup_capture_wait, true)) {
            print_capture_snapshot("register_only_wait_timeout", capture);
            const auto runtime_error = capture.runtime_error();
            std::fprintf(stderr, "stage=capture_frame failed%s%s\n",
                runtime_error ? ": " : "",
                runtime_error ? runtime_error->message.c_str() : "timed out waiting for ready frame");
            return 1;
        }
        if (!prepare_current(true, ResourceStage::nvenc)) return 1;
        SlotInterop& slot = slot_interop[active_frame->lease.slot_index()];
        const auto map_begin = std::chrono::steady_clock::now();
        const bool mapped = map_registered_array(*nvenc, slot.nvenc_registered,
                                                  NV_ENC_BUFFER_FORMAT_ARGB);
        metrics.nvenc_map.add(std::chrono::steady_clock::now() - map_begin);
        if (!mapped) return 1;
        stop_capture.stop_and_drain();
        const auto unmap_begin = std::chrono::steady_clock::now();
        const bool unmapped = unmap_array(*nvenc);
        metrics.nvenc_unmap.add(std::chrono::steady_clock::now() - unmap_begin);
        if (!unmapped) return 1;
        active_frame->lease.release_after_gpu();
        active_frame.reset();
        std::printf("probe_result=registered_and_mapped passed; registrations=cached_per_pool_slot\n");
        print_timing("egl_import", metrics.egl_import);
        print_timing("cuda_egl_register", metrics.cuda_register);
        print_timing("nvenc_register", metrics.nvenc_register);
        print_timing("nvenc_map", metrics.nvenc_map);
        print_timing("nvenc_unmap", metrics.nvenc_unmap);
        return 0;
    }

    if (options.duration_ms != 0) {
        if (!fetch_frame(options.capture_wait_ms, metrics.warmup_capture_wait, true)) {
            print_capture_snapshot("warmup_wait_timeout", capture);
            const auto runtime_error = capture.runtime_error();
            std::fprintf(stderr, "stage=capture_warmup failed%s%s\n",
                runtime_error ? ": " : "",
                runtime_error ? runtime_error->message.c_str() : "timed out waiting for ready frame");
            return 1;
        }
        ++metrics.warmup_frames;
        if (!prepare_current(false, ResourceStage::nvenc)) return 1;
        const std::size_t warmup_slot = active_frame->lease.slot_index();
        print_capture_snapshot("warmup_before_lease_release", capture);
        active_frame->lease.release_after_gpu();
        active_frame.reset();
        active_fence_ready = false;
        while (auto stale = capture.try_pop_frame()) {
            ++metrics.warmup_frames;
            stale->lease.release_after_gpu();
        }
        std::printf("stage=warmup_complete passed captured=%llu registered_slots=%zu pool_slots=%zu\n",
            static_cast<unsigned long long>(metrics.warmup_frames),
            static_cast<std::size_t>(std::count_if(slot_interop.begin(), slot_interop.end(),
                [](const SlotInterop& slot) { return slot.nvenc_ready; })), slot_interop.size());
        std::printf("stage=warmup_resource_slot slot=%zu registration_policy=lazy_on_first_use\n", warmup_slot);
        print_capture_snapshot("warmup_after_release_and_drain", capture);
    }

    if (options.duration_ms != 0 &&
        (!nvenc || !ensure_bitstream_count(*nvenc, options.pipeline_depth))) return 1;
    if (!options.output_mkv.empty()) {
        auto extradata = get_h264_sequence_params(*nvenc);
        if (!extradata) {
            std::fprintf(stderr, "stage=mkv_sequence_params failed: %s\n", extradata.error().c_str());
            return 1;
        }
        try {
            mux_output = std::make_unique<MatroskaPacketSink>(options.output_mkv,
                requirements.width, requirements.height,
                options.bitrate_mbps.value_or(0U), std::move(*extradata));
        } catch (const std::exception& error) {
            std::fprintf(stderr, "stage=mkv_output_create failed: %s\n", error.what());
            return 1;
        }
        std::printf("stage=mkv_output_ready passed path=%s codec=H264 source_clock=presentation_time_ns packet_time_base=1/1000000000 queue_packets=1024 queue_bytes=%u dts_policy=pts_no_b_frames\n",
                    options.output_mkv.c_str(), 128U * 1024U * 1024U);
    }
    const auto measurement_start = std::chrono::steady_clock::now();
    std::uint64_t encode_index = 0;
    if (options.duration_ms != 0 && options.pipeline_depth > 1U) {
        PipelineState pipeline;
        std::thread output_worker([&] {
            pipeline_output_worker(pipeline, *nvenc, output.get(), mux_output.get());
        });
        std::string run_error;
        bool stopped_for_delayed_input = false;
        std::size_t output_ring_index = 0;

        auto set_run_error = [&](std::string message) {
            if (run_error.empty()) run_error = std::move(message);
        };
        auto wait_for_capacity = [&]() {
            std::unique_lock lock(pipeline.mutex);
            const bool woke = pipeline.changed.wait_for(lock,
                std::chrono::milliseconds(options.capture_wait_ms), [&] {
                    return !pipeline.error.empty() || pipeline.in_flight < options.pipeline_depth ||
                        (pipeline.in_flight != 0U && pipeline.unready >= pipeline.in_flight);
                });
            if (!pipeline.error.empty()) return -1;
            if (pipeline.in_flight < options.pipeline_depth) return 1;
            if (pipeline.in_flight != 0U && pipeline.unready >= pipeline.in_flight) return 2;
            return woke ? -1 : 0;
        };

        while (metrics.submissions < options.max_frames) {
            const int capacity = wait_for_capacity();
            if (capacity == 2) {
                stopped_for_delayed_input = true;
                std::printf("stage=pipeline_capacity stop=all_in_flight_waiting_for_more_input\n");
                break;
            }
            if (capacity <= 0) {
                std::lock_guard lock(pipeline.mutex);
                set_run_error(capacity == 0 ?
                    "output worker did not free a pipeline slot before timeout" : pipeline.error);
                break;
            }

            const auto elapsed = std::chrono::steady_clock::now() - measurement_start;
            if (elapsed >= std::chrono::milliseconds(options.duration_ms)) break;
            const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(
                std::chrono::milliseconds(options.duration_ms) - elapsed);
            const int wait_ms = std::min(options.capture_wait_ms,
                static_cast<int>(std::max<std::int64_t>(1, remaining.count())));
            if (!fetch_frame(wait_ms, metrics.capture_wait, metrics.window_dequeued == 0U)) {
                if (std::chrono::steady_clock::now() - measurement_start >=
                    std::chrono::milliseconds(options.duration_ms)) break;
                print_capture_snapshot("pipeline_capture_wait_failed", capture);
                const auto runtime_error = capture.runtime_error();
                set_run_error(runtime_error ? runtime_error->message :
                    "timed out waiting for a ready frame");
                break;
            }
            if (std::chrono::steady_clock::now() - measurement_start >=
                std::chrono::milliseconds(options.duration_ms)) {
                active_frame->lease.release_after_gpu();
                active_frame.reset();
                break;
            }

            ++metrics.window_dequeued;
            const std::size_t slot_index = active_frame->lease.slot_index();
            if (slot_index >= slot_interop.size()) {
                set_run_error("captured frame slot exceeds configured pool");
                active_frame->lease.release_after_gpu();
                active_frame.reset();
                break;
            }
            if (!slot_interop[slot_index].nvenc_ready) {
                if (!prepare_current(false, ResourceStage::nvenc)) {
                    set_run_error("failed preparing a captured slot for NVENC");
                    break;
                }
            } else if (!active_fence_ready) {
                auto source_fd = active_frame->lease.duplicate_plane_fd(0U);
                if (!source_fd) {
                    set_run_error("DMA-BUF duplicate failed: " + source_fd.error().message);
                    break;
                }
                const auto fence_begin = std::chrono::steady_clock::now();
                if (!wait_for_dma_buf_read_fence(source_fd->get(), false)) {
                    set_run_error("DMA-BUF read fence wait failed");
                    break;
                }
                metrics.fence_wait.add(std::chrono::steady_clock::now() - fence_begin);
                active_fence_ready = true;
            }

            SlotInterop& slot = slot_interop[slot_index];
            NV_ENC_INPUT_PTR mapped_input = nullptr;
            const auto map_begin = std::chrono::steady_clock::now();
            if (!map_registered_array_handle(*nvenc, slot.nvenc_registered,
                    NV_ENC_BUFFER_FORMAT_ARGB, mapped_input)) {
                set_run_error("NVENC input map failed");
                break;
            }
            metrics.nvenc_map.add(std::chrono::steady_clock::now() - map_begin);

            NV_ENC_OUTPUT_PTR output_bitstream =
                nvenc->bitstreams[output_ring_index % options.pipeline_depth];
            EncodeSubmitResult submitted;
            constexpr unsigned int kEncoderBusyRetryLimit = 8U;
            for (unsigned int retry = 0; retry <= kEncoderBusyRetryLimit; ++retry) {
                submitted = submit_mapped_frame(*nvenc, slot.cuda.frame,
                    NV_ENC_BUFFER_FORMAT_ARGB, mapped_input, output_bitstream,
                    active_frame->sequence);
                metrics.encode_call.add(std::chrono::nanoseconds(
                    static_cast<std::chrono::nanoseconds::rep>(submitted.call.total_ns)));
                if (submitted.status != NV_ENC_ERR_ENCODER_BUSY) break;
                if (retry == kEncoderBusyRetryLimit) break;
                ++metrics.encoder_busy_retries;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (submitted.status != NV_ENC_SUCCESS &&
                submitted.status != NV_ENC_ERR_NEED_MORE_INPUT) {
                set_run_error("nvEncEncodePicture failed with status " +
                    std::to_string(submitted.status));
                if (unmap_array(*nvenc, mapped_input)) {
                    active_frame->lease.release_after_gpu();
                } else {
                    deferred_release_frames.push_back(std::move(*active_frame));
                }
                active_frame.reset();
                break;
            }

            const bool ready = submitted.status == NV_ENC_SUCCESS;
            PendingEncode pending{
                .captured = std::move(*active_frame),
                .mapped_input = mapped_input,
                .output_bitstream = output_bitstream,
                .encode_index = metrics.submissions + 1U,
                .ready = ready,
            };
            active_frame.reset();
            active_fence_ready = false;
            ++metrics.submissions;
            ++output_ring_index;
            if (ready) ++metrics.successful_submissions;
            else ++metrics.delayed_submissions;

            {
                std::lock_guard lock(pipeline.mutex);
                if (ready) {
                    for (PendingEncode& earlier : pipeline.queue) {
                        if (!earlier.ready) {
                            earlier.ready = true;
                            if (pipeline.unready > 0U) --pipeline.unready;
                        }
                    }
                }
                pipeline.queue.push_back(std::move(pending));
                ++pipeline.in_flight;
                if (!ready) ++pipeline.unready;
                pipeline.in_flight_high_water = std::max(
                    pipeline.in_flight_high_water, pipeline.in_flight);
            }
            pipeline.changed.notify_all();
            metrics.pipeline_high_water = std::max(metrics.pipeline_high_water,
                pipeline.in_flight_high_water);
            update_pool_high_water(metrics, capture.stats());
        }

        stop_capture.stop_and_drain();
        // SDK 13.1 §5.1 requires a zeroed picture packet with only the structure
        // version and EOS flag set; no input buffer is supplied.
        NV_ENC_PIC_PARAMS eos_picture{};
        eos_picture.version = NV_ENC_PIC_PARAMS_VER;
        eos_picture.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
        const NVENCSTATUS eos_status = nvenc->functions.nvEncEncodePicture(
            nvenc->encoder, &eos_picture);
        ++metrics.eos_submissions;
        if (eos_status != NV_ENC_SUCCESS) {
            set_run_error("NVENC EOS flush failed with status " + std::to_string(eos_status));
        }
        {
            std::lock_guard lock(pipeline.mutex);
            pipeline.eos_flushed = eos_status == NV_ENC_SUCCESS;
            if (pipeline.eos_flushed) {
                for (PendingEncode& pending : pipeline.queue) {
                    if (!pending.ready) {
                        pending.ready = true;
                        if (pipeline.unready > 0U) --pipeline.unready;
                    }
                }
            }
            pipeline.producer_done = true;
        }
        pipeline.changed.notify_all();
        output_worker.join();

        std::string worker_error;
        std::size_t pending_at_eos = 0;
        std::size_t unready_at_eos = 0;
        {
            std::lock_guard lock(pipeline.mutex);
            worker_error = pipeline.error;
            pending_at_eos = pipeline.in_flight;
            unready_at_eos = pipeline.unready;
            while (!pipeline.queue.empty()) {
                deferred_release_frames.push_back(std::move(pipeline.queue.front().captured));
                pipeline.queue.pop_front();
            }
            metrics.pipeline_high_water = std::max(metrics.pipeline_high_water,
                pipeline.in_flight_high_water);
        }
        if (!worker_error.empty()) set_run_error(std::move(worker_error));
        for (CapturedFrame& held : pipeline.deferred_release) {
            deferred_release_frames.push_back(std::move(held));
        }

        const PipelineWorkerMetrics& worker_metrics = pipeline.worker_metrics;
        metrics.encoded = worker_metrics.encoded;
        metrics.bytes = worker_metrics.bytes;
        metrics.source_sequence_gaps = worker_metrics.source_sequence_gaps;
        metrics.timestamp_mismatches = worker_metrics.timestamp_mismatches;
        metrics.first_source_sequence = worker_metrics.first_source_sequence;
        metrics.last_source_sequence = worker_metrics.last_source_sequence;
        if (run_error.empty() && (metrics.submissions != metrics.encoded ||
            worker_metrics.lock_successes != worker_metrics.unlock_successes ||
            metrics.encoded != worker_metrics.unmaps ||
            metrics.encoded != worker_metrics.leases_released)) {
            set_run_error("pipeline EOS counts did not reconcile submissions, output locks, unmaps, and lease releases");
        }
        update_pool_high_water(metrics, capture.stats());
        if (mux_output && !mux_output->close_and_check()) {
            set_run_error("Matroska writer shutdown failed: " + mux_output->error());
        }
        if (mux_output && run_error.empty()) {
            const auto mux_stats = mux_output->stats();
            if (mux_stats.enqueued_packets != worker_metrics.encoded) {
                set_run_error("Matroska enqueue count did not match completed NVENC bitstreams");
            }
        }
        const auto measurement_end = std::chrono::steady_clock::now();
        const auto capture_stats = capture.stats();
        if (output && !output->flush()) {
            set_run_error("flushing output files failed: " + output->error());
        }

        std::printf("pipeline_eos status=%u submissions=%llu encoded=%llu lock_successes=%llu unlock_successes=%llu unmaps=%llu leases_released=%llu pending_after_worker=%zu unready_after_worker=%zu\n",
            static_cast<unsigned int>(eos_status),
            static_cast<unsigned long long>(metrics.submissions),
            static_cast<unsigned long long>(worker_metrics.encoded),
            static_cast<unsigned long long>(worker_metrics.lock_successes),
            static_cast<unsigned long long>(worker_metrics.unlock_successes),
            static_cast<unsigned long long>(worker_metrics.unmaps),
            static_cast<unsigned long long>(worker_metrics.leases_released),
            pending_at_eos, unready_at_eos);

        if (!run_error.empty() || metrics.encoded == 0U) {
            std::fprintf(stderr, "stage=pipeline_run failed: %s%s\n",
                run_error.empty() ? "no bitstreams encoded" : run_error.c_str(),
                stopped_for_delayed_input ? " (stopped after delayed inputs filled the ring; EOS attempted)" : "");
            return 1;
        }
        if (output) output->mark_complete();
        const auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            measurement_end - measurement_start).count();
        const double elapsed_seconds = static_cast<double>(elapsed_ns) / 1'000'000'000.0;
        const double encode_fps = elapsed_seconds > 0.0
            ? static_cast<double>(metrics.encoded) / elapsed_seconds : 0.0;
        const double bitrate_mbps = elapsed_seconds > 0.0
            ? static_cast<double>(metrics.bytes) * 8.0 / elapsed_seconds / 1'000'000.0 : 0.0;
        std::printf("probe_result=pipelined_multi_frame_encoded passed duration_requested_ms=%d pipeline_depth=%u max_frames=%u elapsed_ns=%lld window_dequeued=%llu submissions=%llu success_status=%llu need_more_input=%llu busy_retries=%llu encoded=%llu bytes=%llu source_sequence_first=%llu source_sequence_last=%llu source_sequence_gaps=%llu timestamp_mismatches=%llu fps=%.3f bitrate_mbps=%.3f stopped_for_delayed_input=%u\n",
            options.duration_ms, options.pipeline_depth, options.max_frames,
            static_cast<long long>(elapsed_ns),
            static_cast<unsigned long long>(metrics.window_dequeued),
            static_cast<unsigned long long>(metrics.submissions),
            static_cast<unsigned long long>(metrics.successful_submissions),
            static_cast<unsigned long long>(metrics.delayed_submissions),
            static_cast<unsigned long long>(metrics.encoder_busy_retries),
            static_cast<unsigned long long>(metrics.encoded),
            static_cast<unsigned long long>(metrics.bytes),
            static_cast<unsigned long long>(metrics.first_source_sequence),
            static_cast<unsigned long long>(metrics.last_source_sequence),
            static_cast<unsigned long long>(metrics.source_sequence_gaps),
            static_cast<unsigned long long>(metrics.timestamp_mismatches),
            encode_fps, bitrate_mbps, stopped_for_delayed_input ? 1U : 0U);
        std::printf("pipeline_counts in_flight_high_water=%zu lock_attempts=%llu lock_successes=%llu unlock_attempts=%llu unlock_successes=%llu unmaps=%llu leases_released=%llu eos_submissions=%llu capture_completed=%llu capture_admitted=%llu capture_dropped=%llu capture_failures=%llu capture_cancelled_at_stop=%llu queue_high_water=%zu busy_high_water=%zu pool_starvation_periods=%llu pool_starvation_total_ns=%llu\n",
            metrics.pipeline_high_water,
            static_cast<unsigned long long>(worker_metrics.locks),
            static_cast<unsigned long long>(worker_metrics.lock_successes),
            static_cast<unsigned long long>(worker_metrics.unlocks),
            static_cast<unsigned long long>(worker_metrics.unlock_successes),
            static_cast<unsigned long long>(worker_metrics.unmaps),
            static_cast<unsigned long long>(worker_metrics.leases_released),
            static_cast<unsigned long long>(metrics.eos_submissions),
            static_cast<unsigned long long>(capture_stats.completed),
            static_cast<unsigned long long>(capture_stats.admitted),
            static_cast<unsigned long long>(capture_stats.dropped),
            static_cast<unsigned long long>(capture_stats.failures),
            static_cast<unsigned long long>(capture_stats.cancelled_at_stop),
            metrics.queue_high_water, metrics.busy_high_water,
            static_cast<unsigned long long>(capture_stats.pool_starvation_periods),
            static_cast<unsigned long long>(capture_stats.pool_starvation_total_ns));
        std::printf("resource_strategy=cached_per_pool_slot pool_slots=%zu registered_slots=%zu pipeline_depth=%u output_buffers=%zu lock_worker=blocking_sync_NVENC stream=%s frame_csv=%s output_mode=%s\n",
            slot_interop.size(), static_cast<std::size_t>(std::count_if(slot_interop.begin(), slot_interop.end(),
                [](const SlotInterop& slot) { return slot.nvenc_ready; })),
            options.pipeline_depth, nvenc->bitstreams.size(),
            options.output_mkv.empty() ? options.output_file.c_str() : options.output_mkv.c_str(),
            options.output_mkv.empty() ? (options.output_file + ".frames.csv").c_str() : "none",
            options.output_mkv.empty() ? "raw-h264" : "matroska");
        if (mux_output) {
            const auto mux_stats = mux_output->stats();
            std::printf("mkv_mux_stats enqueued=%llu muxed_video=%llu queue_full=%llu queue_busy=%llu invalid=%llu bytes=%llu queue_high_water_packets=%zu queue_high_water_bytes=%zu output_file_bytes=%llu\n",
                static_cast<unsigned long long>(mux_stats.enqueued_packets),
                static_cast<unsigned long long>(mux_stats.muxed_video_packets),
                static_cast<unsigned long long>(mux_stats.queue_full_drops),
                static_cast<unsigned long long>(mux_stats.queue_busy_drops),
                static_cast<unsigned long long>(mux_stats.invalid_packets),
                static_cast<unsigned long long>(mux_stats.packet_payload_bytes),
                mux_stats.queue_high_water_packets, mux_stats.queue_high_water_bytes,
                static_cast<unsigned long long>(mux_stats.output_file_bytes));
        }
        print_timing("egl_import_per_slot", metrics.egl_import);
        print_timing("cuda_egl_register_per_slot", metrics.cuda_register);
        print_timing("nvenc_register_per_slot", metrics.nvenc_register);
        print_timing("capture_wait", metrics.capture_wait);
        print_timing("dma_buf_read_fence", metrics.fence_wait);
        print_timing("nvenc_map_per_frame", metrics.nvenc_map);
        print_timing("nvenc_encode_picture_submit", metrics.encode_call);
        print_timing("nvenc_lock_bitstream_worker", worker_metrics.lock_wait);
        print_timing(mux_output ? "compressed_packet_copy_worker"
                                : "encoded_bitstream_write_worker",
                     worker_metrics.bitstream_write);
        print_timing("nvenc_unlock_bitstream_worker", worker_metrics.unlock_call);
        print_timing("nvenc_unmap_worker", worker_metrics.unmap_call);
        return 0;
    }
    while (metrics.encoded < options.max_frames) {
        update_pool_high_water(metrics, capture.stats());
        int wait_ms = options.capture_wait_ms;
        if (options.duration_ms != 0) {
            const auto duration = std::chrono::milliseconds(options.duration_ms);
            const auto elapsed = std::chrono::steady_clock::now() - measurement_start;
            if (elapsed >= duration) break;
            const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(duration - elapsed);
            wait_ms = std::min(wait_ms, static_cast<int>(std::max<std::int64_t>(1, remaining.count())));
        }
        if (!fetch_frame(wait_ms, metrics.capture_wait, metrics.window_dequeued == 0U)) {
            if (options.duration_ms != 0 &&
                std::chrono::steady_clock::now() - measurement_start >=
                    std::chrono::milliseconds(options.duration_ms)) break;
            print_capture_snapshot("measurement_wait_no_frame", capture);
            const auto runtime_error = capture.runtime_error();
            std::fprintf(stderr, "stage=capture_frame failed%s%s\n",
                runtime_error ? ": " : "",
                runtime_error ? runtime_error->message.c_str() : "timed out waiting for ready frame");
            return 1;
        }
        if (options.duration_ms != 0 && std::chrono::steady_clock::now() - measurement_start >=
                std::chrono::milliseconds(options.duration_ms)) {
            active_frame->lease.release_after_gpu();
            active_frame.reset();
            break;
        }
        ++metrics.window_dequeued;
        const std::size_t slot_index = active_frame->lease.slot_index();
        if (slot_index >= slot_interop.size()) {
            std::fprintf(stderr, "stage=nvenc_register_cudaarray failed: frame slot %zu exceeds pool\n", slot_index);
            return 1;
        }
        if (!slot_interop[slot_index].nvenc_ready &&
            !prepare_current(false, ResourceStage::nvenc)) return 1;
        if (!nvenc || (!nvenc->bitstream && !create_bitstream(*nvenc))) return 1;
        if (!active_fence_ready) {
            auto source_fd = active_frame->lease.duplicate_plane_fd(0U);
            if (!source_fd) {
                std::fprintf(stderr, "stage=dmabuf_dup failed: %s\n", source_fd.error().message.c_str());
                return 1;
            }
            const auto fence_begin = std::chrono::steady_clock::now();
            if (!wait_for_dma_buf_read_fence(source_fd->get(), false)) return 1;
            metrics.fence_wait.add(std::chrono::steady_clock::now() - fence_begin);
            active_fence_ready = true;
        }

        SlotInterop& slot = slot_interop[slot_index];
        const auto map_begin = std::chrono::steady_clock::now();
        if (!map_registered_array(*nvenc, slot.nvenc_registered, NV_ENC_BUFFER_FORMAT_ARGB)) return 1;
        metrics.nvenc_map.add(std::chrono::steady_clock::now() - map_begin);

        auto encoded = encode_mapped_frame(*nvenc, slot.cuda.frame,
            NV_ENC_BUFFER_FORMAT_ARGB, active_frame->sequence, *output);
        if (!encoded) {
            std::fprintf(stderr, "stage=nvenc_encode failed: %s\n", encoded.error().c_str());
            return 1;
        }
        metrics.encode_call.total_ns += encoded->encode_call.total_ns;
        metrics.encode_call.max_ns = std::max(metrics.encode_call.max_ns, encoded->encode_call.max_ns);
        ++metrics.encode_call.count;
        metrics.bitstream_lock.total_ns += encoded->lock_wait.total_ns;
        metrics.bitstream_lock.max_ns = std::max(metrics.bitstream_lock.max_ns, encoded->lock_wait.max_ns);
        ++metrics.bitstream_lock.count;
        metrics.bitstream_write.total_ns += encoded->bitstream_write.total_ns;
        metrics.bitstream_write.max_ns = std::max(metrics.bitstream_write.max_ns, encoded->bitstream_write.max_ns);
        ++metrics.bitstream_write.count;
        metrics.bitstream_unlock.total_ns += encoded->unlock_call.total_ns;
        metrics.bitstream_unlock.max_ns = std::max(metrics.bitstream_unlock.max_ns, encoded->unlock_call.max_ns);
        ++metrics.bitstream_unlock.count;

        const auto unmap_begin = std::chrono::steady_clock::now();
        if (!unmap_array(*nvenc)) return 1;
        metrics.nvenc_unmap.add(std::chrono::steady_clock::now() - unmap_begin);
        ++encode_index;
        if (!write_csv_row(*output, encode_index, *active_frame, encoded->bytes,
                           encoded->output_timestamp, encoded->picture_type)) {
            std::fprintf(stderr, "stage=csv_write failed while recording frame correlation\n");
            return 1;
        }
        if (encoded->output_timestamp != active_frame->sequence) ++metrics.timestamp_mismatches;
        if (metrics.last_source_sequence != 0U && active_frame->sequence > metrics.last_source_sequence + 1U) {
            metrics.source_sequence_gaps += active_frame->sequence - metrics.last_source_sequence - 1U;
        }
        if (metrics.first_source_sequence == 0U) metrics.first_source_sequence = active_frame->sequence;
        metrics.last_source_sequence = active_frame->sequence;
        ++metrics.encoded;
        metrics.bytes += encoded->bytes;
        active_frame->lease.release_after_gpu();
        active_frame.reset();
        active_fence_ready = false;
    }

    const auto measurement_end = std::chrono::steady_clock::now();
    update_pool_high_water(metrics, capture.stats());
    stop_capture.stop_and_drain();
    const auto capture_stats = capture.stats();
    if (metrics.encoded == 0U) {
        std::fprintf(stderr, "stage=nvenc_encode failed: no frames encoded in the measurement window\n");
        return 1;
    }
    if (!output->flush()) {
        std::fprintf(stderr, "stage=output_flush failed: %s\n", output->error().c_str());
        return 1;
    }
    output->mark_complete();

    const auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        measurement_end - measurement_start).count();
    const double elapsed_seconds = static_cast<double>(elapsed_ns) / 1'000'000'000.0;
    const double encode_fps = elapsed_seconds > 0.0
        ? static_cast<double>(metrics.encoded) / elapsed_seconds : 0.0;
    const double bitrate_mbps = elapsed_seconds > 0.0
        ? static_cast<double>(metrics.bytes) * 8.0 / elapsed_seconds / 1'000'000.0 : 0.0;
    std::printf("probe_result=multi_frame_encoded passed duration_requested_ms=%d max_frames=%u elapsed_ns=%lld warmup_captured=%llu window_dequeued=%llu encoded=%llu bytes=%llu source_sequence_first=%llu source_sequence_last=%llu source_sequence_gaps=%llu timestamp_mismatches=%llu fps=%.3f bitrate_mbps=%.3f\n",
        options.duration_ms, options.max_frames, static_cast<long long>(elapsed_ns),
        static_cast<unsigned long long>(metrics.warmup_frames),
        static_cast<unsigned long long>(metrics.window_dequeued),
        static_cast<unsigned long long>(metrics.encoded),
        static_cast<unsigned long long>(metrics.bytes),
        static_cast<unsigned long long>(metrics.first_source_sequence),
        static_cast<unsigned long long>(metrics.last_source_sequence),
        static_cast<unsigned long long>(metrics.source_sequence_gaps),
        static_cast<unsigned long long>(metrics.timestamp_mismatches), encode_fps, bitrate_mbps);
    std::printf("capture_stats completed=%llu admitted=%llu dropped=%llu rate_limited=%llu failures=%llu cancelled_at_stop=%llu queue_high_water=%zu busy_high_water=%zu pool_starvation_periods=%llu pool_starvation_total_ns=%llu request_to_ready_avg_ns=%llu request_to_ready_p95_ns=%llu\n",
        static_cast<unsigned long long>(capture_stats.completed),
        static_cast<unsigned long long>(capture_stats.admitted),
        static_cast<unsigned long long>(capture_stats.dropped),
        static_cast<unsigned long long>(capture_stats.rate_limited),
        static_cast<unsigned long long>(capture_stats.failures),
        static_cast<unsigned long long>(capture_stats.cancelled_at_stop),
        metrics.queue_high_water, metrics.busy_high_water,
        static_cast<unsigned long long>(capture_stats.pool_starvation_periods),
        static_cast<unsigned long long>(capture_stats.pool_starvation_total_ns),
        static_cast<unsigned long long>(capture_stats.request_to_ready_average_ns),
        static_cast<unsigned long long>(capture_stats.request_to_ready_p95_ns));
    std::printf("resource_strategy=cached_per_pool_slot pool_slots=%zu registered_slots=%zu map_policy=map_encode_lock_unlock_per_frame lease_release=after_bitstream_unlock_and_nvenc_unmap stream=%s frame_csv=%s\n",
        slot_interop.size(), static_cast<std::size_t>(std::count_if(slot_interop.begin(), slot_interop.end(),
            [](const SlotInterop& slot) { return slot.nvenc_ready; })), options.output_file.c_str(),
        (options.output_file + ".frames.csv").c_str());
    print_timing("egl_setup", metrics.egl_setup);
    print_timing("egl_import_per_slot", metrics.egl_import);
    print_timing("cuda_context_setup", metrics.cuda_context);
    print_timing("cuda_egl_register_per_slot", metrics.cuda_register);
    print_timing("nvenc_setup", metrics.nvenc_setup);
    print_timing("nvenc_register_per_slot", metrics.nvenc_register);
    print_timing("warmup_capture_wait", metrics.warmup_capture_wait);
    print_timing("capture_wait", metrics.capture_wait);
    print_timing("dma_buf_read_fence", metrics.fence_wait);
    print_timing("nvenc_map_per_frame", metrics.nvenc_map);
    print_timing("nvenc_encode_picture", metrics.encode_call);
    print_timing("nvenc_lock_bitstream", metrics.bitstream_lock);
    print_timing("encoded_bitstream_write", metrics.bitstream_write);
    print_timing("nvenc_unlock_bitstream", metrics.bitstream_unlock);
    print_timing("nvenc_unmap_per_frame", metrics.nvenc_unmap);
    return 0;
}
