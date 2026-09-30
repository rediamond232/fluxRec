#include "gpu/direct_cuda_egl_encoder.hpp"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <drm_fourcc.h>
#include <ffnvcodec/nvEncodeAPI.h>
#include <linux/dma-buf.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <dlfcn.h>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <poll.h>
#include <string_view>
#include <system_error>
#include <sys/ioctl.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace fluxrec::gpu {
namespace {

class DynamicLibrary final {
public:
    explicit DynamicLibrary(const char* name) : handle_(::dlopen(name, RTLD_NOW | RTLD_LOCAL)) {}
    ~DynamicLibrary() { if (handle_) (void)::dlclose(handle_); }
    DynamicLibrary(const DynamicLibrary&) = delete;
    DynamicLibrary& operator=(const DynamicLibrary&) = delete;
    [[nodiscard]] explicit operator bool() const noexcept { return handle_ != nullptr; }

    template<class T>
    [[nodiscard]] T symbol(const char* name) const noexcept {
        void* raw = handle_ ? ::dlsym(handle_, name) : nullptr;
        T result{};
        static_assert(sizeof(result) == sizeof(raw));
        std::memcpy(&result, &raw, sizeof(result));
        return result;
    }

private:
    void* handle_ = nullptr;
};

bool extension_present(const char* list, std::string_view wanted) {
    if (!list) return false;
    const std::string_view all{list};
    std::size_t offset = 0;
    while (offset < all.size()) {
        while (offset < all.size() && all[offset] == ' ') ++offset;
        const std::size_t end = all.find(' ', offset);
        if (all.substr(offset, end == std::string_view::npos
                ? all.size() - offset : end - offset) == wanted) return true;
        if (end == std::string_view::npos) break;
        offset = end + 1U;
    }
    return false;
}

template<class T>
T egl_proc(const char* name) noexcept {
    const auto raw = eglGetProcAddress(name);
    T result{};
    static_assert(sizeof(result) == sizeof(raw));
    std::memcpy(&result, &raw, sizeof(result));
    return result;
}

using CUresult = int;
using CUdevice = int;
struct CUctx_st;
struct CUarray_st;
struct CUgraphicsResource_st;
using CUcontext = CUctx_st*;
using CUarray = CUarray_st*;
using CUgraphicsResource = CUgraphicsResource_st*;
enum CUeglFrameType : int { CU_EGL_FRAME_TYPE_ARRAY = 0, CU_EGL_FRAME_TYPE_PITCH = 1 };
struct CUeglFrame {
    union { CUarray pArray[3]; void* pPitch[3]; } frame;
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
static_assert(sizeof(CUeglFrame) == (sizeof(void*) == 8U ? 64U : 48U));

constexpr std::int64_t kPacketTimeBaseDen = 1'000'000;
constexpr int kFenceWaitMs = 2000;
constexpr unsigned int kCudaEglRegisterFlags = 0U;

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

struct EglState {
    EglApi api;
    EGLDeviceEXT device = EGL_NO_DEVICE_EXT;
    EGLDisplay display = EGL_NO_DISPLAY;
    int cuda_ordinal = -1;
    bool initialized = false;
    std::vector<EGLImageKHR> images;

    ~EglState() {
        if (display != EGL_NO_DISPLAY && api.destroy_image) {
            for (const EGLImageKHR image : images) {
                if (image != EGL_NO_IMAGE_KHR) (void)api.destroy_image(display, image);
            }
        }
        if (initialized) (void)eglTerminate(display);
    }
};

struct CudaApi {
    DynamicLibrary library{"libcuda.so.1"};
    CUresult (*init)(unsigned int) = nullptr;
    CUresult (*device_get)(CUdevice*, int) = nullptr;
    CUresult (*device_get_name)(char*, int, CUdevice) = nullptr;
    CUresult (*context_create)(CUcontext*, unsigned int, CUdevice) = nullptr;
    CUresult (*context_set_current)(CUcontext) = nullptr;
    CUresult (*context_destroy)(CUcontext) = nullptr;
    CUresult (*graphics_egl_register_image)(CUgraphicsResource*, EGLImageKHR, unsigned int) = nullptr;
    CUresult (*graphics_resource_get_mapped_egl_frame)(CUeglFrame*, CUgraphicsResource,
                                                        unsigned int, unsigned int) = nullptr;
    CUresult (*graphics_unregister_resource)(CUgraphicsResource) = nullptr;
    CUresult (*get_error_name)(CUresult, const char**) = nullptr;
    CUresult (*get_error_string)(CUresult, const char**) = nullptr;

    CudaApi() {
        init = library.symbol<decltype(init)>("cuInit");
        device_get = library.symbol<decltype(device_get)>("cuDeviceGet");
        device_get_name = library.symbol<decltype(device_get_name)>("cuDeviceGetName");
        context_create = library.symbol<decltype(context_create)>("cuCtxCreate_v2");
        context_set_current = library.symbol<decltype(context_set_current)>("cuCtxSetCurrent");
        context_destroy = library.symbol<decltype(context_destroy)>("cuCtxDestroy_v2");
        graphics_egl_register_image = library.symbol<decltype(graphics_egl_register_image)>(
            "cuGraphicsEGLRegisterImage");
        graphics_resource_get_mapped_egl_frame =
            library.symbol<decltype(graphics_resource_get_mapped_egl_frame)>(
                "cuGraphicsResourceGetMappedEglFrame");
        graphics_unregister_resource = library.symbol<decltype(graphics_unregister_resource)>(
            "cuGraphicsUnregisterResource");
        get_error_name = library.symbol<decltype(get_error_name)>("cuGetErrorName");
        get_error_string = library.symbol<decltype(get_error_string)>("cuGetErrorString");
    }

    [[nodiscard]] bool available() const noexcept {
        return library && init && device_get && context_create && context_set_current &&
            context_destroy && graphics_egl_register_image &&
            graphics_resource_get_mapped_egl_frame && graphics_unregister_resource;
    }

    [[nodiscard]] std::string describe(CUresult code) const {
        const char* name = nullptr;
        const char* message = nullptr;
        if (get_error_name) (void)get_error_name(code, &name);
        if (get_error_string) (void)get_error_string(code, &message);
        std::string out = "CUDA result " + std::to_string(code);
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

struct NvencState {
    DynamicLibrary library{"libnvidia-encode.so.1"};
    NV_ENCODE_API_FUNCTION_LIST functions{};
    void* encoder = nullptr;
    bool initialized = false;
    std::vector<NV_ENC_REGISTERED_PTR> registered;
    std::vector<NV_ENC_INPUT_PTR> mapped;
    std::vector<NV_ENC_OUTPUT_PTR> bitstreams;
    std::vector<NV_ENC_OUTPUT_PTR> locked;

    ~NvencState() {
        if (!encoder) return;
        for (const NV_ENC_INPUT_PTR input : mapped) {
            if (input && functions.nvEncUnmapInputResource) {
                (void)functions.nvEncUnmapInputResource(encoder, input);
            }
        }
        for (const NV_ENC_OUTPUT_PTR output : locked) {
            if (output && functions.nvEncUnlockBitstream) {
                (void)functions.nvEncUnlockBitstream(encoder, output);
            }
        }
        for (const NV_ENC_REGISTERED_PTR resource : registered) {
            if (resource && functions.nvEncUnregisterResource) {
                (void)functions.nvEncUnregisterResource(encoder, resource);
            }
        }
        for (const NV_ENC_OUTPUT_PTR output : bitstreams) {
            if (output && functions.nvEncDestroyBitstreamBuffer) {
                (void)functions.nvEncDestroyBitstreamBuffer(encoder, output);
            }
        }
        if (functions.nvEncDestroyEncoder) (void)functions.nvEncDestroyEncoder(encoder);
    }
};

struct SlotInterop {
    capture::UniqueFd dma_buf_fd;
    std::uint32_t offset = 0;
    std::uint32_t stride = 0;
    std::uint64_t modifier = 0;
    EGLImageKHR egl_image = EGL_NO_IMAGE_KHR;
    CUgraphicsResource cuda_resource = nullptr;
    CUeglFrame cuda_frame{};
    NV_ENC_REGISTERED_PTR nvenc_resource = nullptr;
};

struct PendingEncode {
    capture::CapturedFrame captured;
    NV_ENC_INPUT_PTR mapped_input = nullptr;
    NV_ENC_OUTPUT_PTR output = nullptr;
    std::int64_t pts = 0;
    std::chrono::steady_clock::time_point submitted_at{};
    bool ready = false;
};

bool wait_for_read_fence(int fd, std::string& why) {
    dma_buf_export_sync_file exported{};
    exported.flags = DMA_BUF_SYNC_READ;
    exported.fd = -1;
    int result;
    do { result = ::ioctl(fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &exported); }
    while (result < 0 && errno == EINTR);
    if (result < 0 || exported.fd < 0) {
        why = "DMA-BUF read sync-file export failed: " + std::string(std::strerror(errno));
        return false;
    }
    capture::UniqueFd sync_fd{exported.fd};
    pollfd descriptor{.fd = sync_fd.get(), .events = POLLIN, .revents = 0};
    do { result = ::poll(&descriptor, 1, kFenceWaitMs); }
    while (result < 0 && errno == EINTR);
    if (result <= 0 || (descriptor.revents & POLLIN) == 0) {
        why = result == 0 ? "DMA-BUF read sync-file timed out" :
            "DMA-BUF read sync-file poll failed: " + std::string(std::strerror(errno));
        return false;
    }
    return true;
}

} // namespace

struct DirectCudaEglEncoder::Impl {
    DirectCudaEglConfig config;
    std::string cuda_device_name;
    EGLint egl_major = 0;
    EGLint egl_minor = 0;
    std::unique_ptr<EglState> egl;
    std::unique_ptr<CudaState> cuda;
    std::unique_ptr<NvencState> nvenc;
    std::vector<SlotInterop> slots;
    std::vector<std::uint8_t> extradata;
    std::vector<PendingEncode> deferred;
    std::size_t pipeline_depth = 1;
    std::size_t next_output = 0;

    mutable std::mutex mutex;
    std::condition_variable changed;
    std::deque<PendingEncode> queue;
    std::deque<media::EncodedPacket> completed;
    EncoderStats counters;
    std::string worker_error;
    std::thread worker;
    std::thread lock_watchdog;
    std::atomic<bool> lock_watchdog_stop{false};
    // Nonzero while nvEncLockBitstream is in progress. Steady-clock tick count
    // of the deadline. Zero means no lock is in progress.
    std::atomic<std::int64_t> bitstream_lock_deadline_ticks{0};
    std::size_t in_flight = 0;
    std::size_t unready = 0;
    std::size_t deferred_count = 0;
    bool eos_submitted = false;
    bool eos_flushed = false;
    bool producer_done = false;
    bool drained = false;

    ~Impl() {
        if (!drained) (void)drain();
        lock_watchdog_stop.store(true, std::memory_order_release);
        if (lock_watchdog.joinable()) lock_watchdog.join();
        {
            std::lock_guard lock(mutex);
            producer_done = true;
        }
        changed.notify_all();
        if (worker.joinable()) worker.join();

        // Keep the capture leases, CUDA EGL resources, EGLImages, and backing
        // DMA-BUF fds alive until NVENC has stopped and its registrations close.
        nvenc.reset();
        for (PendingEncode& pending : deferred) {
            pending.captured.lease.release_after_gpu();
        }
        deferred.clear();
        if (cuda) {
            if (cuda->context) (void)cuda->api.context_set_current(cuda->context);
            for (const CUgraphicsResource resource : cuda->resources) {
                if (resource) (void)cuda->api.graphics_unregister_resource(resource);
            }
            cuda->resources.clear();
            for (SlotInterop& slot : slots) slot.cuda_resource = nullptr;
        }
        egl.reset();
        slots.clear();
        cuda.reset();
    }

    static std::expected<std::unique_ptr<Impl>, GpuError> create(
        std::string render_node, std::span<const capture::DmabufImage> images,
        DirectCudaEglConfig requested) {
        auto state = std::unique_ptr<Impl>(new (std::nothrow) Impl);
        if (!state) return std::unexpected(GpuError{"allocating direct EGL encoder failed", 0});
        state->config = requested;
        const bool rate_ok = requested.qp
            ? *requested.qp <= 51U
            : requested.bitrate > 0;
        if (requested.width != 2560U || requested.height != 1440U ||
            requested.pool_size < 3U || requested.pool_size > 16U ||
            requested.pool_size != images.size() ||
            requested.fps_num == 0U || requested.fps_den == 0U ||
            !rate_ok || requested.gop_size == 0U ||
            requested.async_depth < 1) {
            return std::unexpected(GpuError{
                "experimental CUDA EGL encoder requires 2560x1440, a matching pool of at least three images, positive rate or constant QP, GOP, and async depth >= 1", 0});
        }
        state->pipeline_depth = std::min<std::size_t>(
            images.size(), static_cast<std::size_t>(requested.async_depth));
        if (state->pipeline_depth == 0U || state->pipeline_depth > 16U) {
            return std::unexpected(GpuError{
                "experimental CUDA EGL encoder supports pipeline depth 1 through 16", 0});
        }
        if (render_node.empty() || images.empty()) {
            return std::unexpected(GpuError{"capture render node or DMA-BUF pool is empty", 0});
        }
        const capture::DmabufImage& first = images.front();
        if (first.width != requested.width || first.height != requested.height ||
            first.fourcc != DRM_FORMAT_ARGB8888 || first.planes.size() != 1U ||
            first.planes[0].fd < 0 || first.planes[0].stride == 0U ||
            first.planes[0].offset > static_cast<std::uint32_t>(std::numeric_limits<EGLint>::max()) ||
            first.planes[0].stride > static_cast<std::uint32_t>(std::numeric_limits<EGLint>::max())) {
            return std::unexpected(GpuError{
                "experimental CUDA EGL path only accepts one-plane 2560x1440 AR24 DMA-BUF images", 0});
        }
        for (const capture::DmabufImage& image : images) {
            if (image.width != first.width || image.height != first.height ||
                image.fourcc != first.fourcc || image.modifier != first.modifier ||
                image.planes.size() != 1U || image.planes[0].fd < 0 ||
                image.planes[0].stride != first.planes[0].stride ||
                image.planes[0].offset > static_cast<std::uint32_t>(std::numeric_limits<EGLint>::max())) {
                return std::unexpected(GpuError{
                    "experimental CUDA EGL pool contains inconsistent DMA-BUF descriptors", 0});
            }
        }
        auto initialized = state->initialize_egl(render_node, first);
        if (!initialized) return std::unexpected(initialized.error());
        auto cuda_initialized = state->initialize_cuda();
        if (!cuda_initialized) return std::unexpected(cuda_initialized.error());
        auto nvenc_initialized = state->initialize_nvenc();
        if (!nvenc_initialized) return std::unexpected(nvenc_initialized.error());
        auto imported = state->prepare_slots(images);
        if (!imported) return std::unexpected(imported.error());
        try {
            state->lock_watchdog = std::thread([raw = state.get()] { raw->watch_bitstream_lock(); });
            state->worker = std::thread([raw = state.get()] { raw->output_worker(); });
        } catch (const std::system_error& exception) {
            state->lock_watchdog_stop.store(true, std::memory_order_release);
            if (state->lock_watchdog.joinable()) state->lock_watchdog.join();
            if (state->worker.joinable()) state->worker.join();
            return std::unexpected(GpuError{
                "starting NVENC bitstream worker failed: " + std::string(exception.what()),
                exception.code().value()});
        }
        return state;
    }

    std::expected<void, GpuError> initialize_egl(std::string_view render_node,
                                                  const capture::DmabufImage& image) {
        egl = std::make_unique<EglState>();
        egl->api.query_devices = egl_proc<PFNEGLQUERYDEVICESEXTPROC>("eglQueryDevicesEXT");
        egl->api.query_device_string = egl_proc<PFNEGLQUERYDEVICESTRINGEXTPROC>("eglQueryDeviceStringEXT");
        egl->api.query_device_attrib = egl_proc<PFNEGLQUERYDEVICEATTRIBEXTPROC>("eglQueryDeviceAttribEXT");
        egl->api.get_platform_display = egl_proc<PFNEGLGETPLATFORMDISPLAYEXTPROC>("eglGetPlatformDisplayEXT");
        egl->api.query_dmabuf_formats = egl_proc<PFNEGLQUERYDMABUFFORMATSEXTPROC>("eglQueryDmaBufFormatsEXT");
        egl->api.query_dmabuf_modifiers = egl_proc<PFNEGLQUERYDMABUFMODIFIERSEXTPROC>("eglQueryDmaBufModifiersEXT");
        egl->api.create_image = egl_proc<PFNEGLCREATEIMAGEKHRPROC>("eglCreateImageKHR");
        egl->api.destroy_image = egl_proc<PFNEGLDESTROYIMAGEKHRPROC>("eglDestroyImageKHR");
        if (!egl->api.query_devices || !egl->api.query_device_string ||
            !egl->api.query_device_attrib || !egl->api.get_platform_display ||
            !egl->api.query_dmabuf_formats || !egl->api.query_dmabuf_modifiers ||
            !egl->api.create_image || !egl->api.destroy_image) {
            return std::unexpected(GpuError{"required EGL device/DMA-BUF import entry point is absent", 0});
        }

        EGLint count = 0;
        if (egl->api.query_devices(0, nullptr, &count) != EGL_TRUE || count <= 0) {
            return std::unexpected(GpuError{"eglQueryDevicesEXT found no EGL devices", 0});
        }
        std::vector<EGLDeviceEXT> devices(static_cast<std::size_t>(count));
        if (egl->api.query_devices(count, devices.data(), &count) != EGL_TRUE) {
            return std::unexpected(GpuError{"eglQueryDevicesEXT enumeration failed", 0});
        }
        for (const EGLDeviceEXT device : devices) {
            const char* path = egl->api.query_device_string(device, EGL_DRM_RENDER_NODE_FILE_EXT);
            if (path && render_node == path) { egl->device = device; break; }
        }
        if (egl->device == EGL_NO_DEVICE_EXT) {
            return std::unexpected(GpuError{"no EGL device matches the capture render node", 0});
        }
        const char* device_extensions = egl->api.query_device_string(egl->device, EGL_EXTENSIONS);
        if (!extension_present(device_extensions, "EGL_NV_device_cuda")) {
            return std::unexpected(GpuError{"matched EGL device lacks EGL_NV_device_cuda", 0});
        }
        EGLAttrib ordinal = -1;
        if (egl->api.query_device_attrib(egl->device, EGL_CUDA_DEVICE_NV, &ordinal) != EGL_TRUE ||
            ordinal < 0 || ordinal > std::numeric_limits<int>::max()) {
            return std::unexpected(GpuError{"EGL_NV_device_cuda returned no valid CUDA ordinal", 0});
        }
        egl->cuda_ordinal = static_cast<int>(ordinal);
        egl->display = egl->api.get_platform_display(EGL_PLATFORM_DEVICE_EXT, egl->device, nullptr);
        if (egl->display == EGL_NO_DISPLAY ||
            eglInitialize(egl->display, &egl_major, &egl_minor) != EGL_TRUE) {
            if (egl->display != EGL_NO_DISPLAY) {
                (void)eglTerminate(egl->display);
                egl->display = EGL_NO_DISPLAY;
            }
            return std::unexpected(GpuError{"initializing the matched EGL device failed", 0});
        }
        egl->initialized = true;
        const char* display_extensions = eglQueryString(egl->display, EGL_EXTENSIONS);
        if (!extension_present(display_extensions, "EGL_EXT_image_dma_buf_import") ||
            !extension_present(display_extensions, "EGL_EXT_image_dma_buf_import_modifiers")) {
            return std::unexpected(GpuError{"matched EGL display lacks DMA-BUF import/modifier extensions", 0});
        }
        EGLint format_count = 0;
        if (egl->api.query_dmabuf_formats(egl->display, 0, nullptr, &format_count) != EGL_TRUE ||
            format_count <= 0) {
            return std::unexpected(GpuError{"EGL DMA-BUF format enumeration failed", 0});
        }
        std::vector<EGLint> formats(static_cast<std::size_t>(format_count));
        if (egl->api.query_dmabuf_formats(egl->display, format_count,
                                           formats.data(), &format_count) != EGL_TRUE ||
            std::find(formats.begin(), formats.end(), static_cast<EGLint>(image.fourcc)) == formats.end()) {
            return std::unexpected(GpuError{"EGL does not advertise the captured AR24 fourcc", 0});
        }
        EGLint modifier_count = 0;
        if (egl->api.query_dmabuf_modifiers(egl->display, static_cast<EGLint>(image.fourcc),
                0, nullptr, nullptr, &modifier_count) != EGL_TRUE || modifier_count <= 0) {
            return std::unexpected(GpuError{"EGL DMA-BUF modifier enumeration failed", 0});
        }
        std::vector<EGLuint64KHR> modifiers(static_cast<std::size_t>(modifier_count));
        if (egl->api.query_dmabuf_modifiers(egl->display, static_cast<EGLint>(image.fourcc),
                modifier_count, modifiers.data(), nullptr, &modifier_count) != EGL_TRUE ||
            std::find(modifiers.begin(), modifiers.end(), image.modifier) == modifiers.end()) {
            return std::unexpected(GpuError{"EGL does not advertise the exact captured modifier", 0});
        }
        return {};
    }

    std::expected<void, GpuError> initialize_cuda() {
        cuda = std::make_unique<CudaState>();
        if (!cuda->api.available()) {
            return std::unexpected(GpuError{"libcuda.so.1 lacks required CUDA EGL interop functions", 0});
        }
        CUresult status = cuda->api.init(0U);
        if (status != 0) return std::unexpected(GpuError{"cuInit failed: " + cuda->api.describe(status), status});
        CUdevice device = 0;
        status = cuda->api.device_get(&device, egl->cuda_ordinal);
        if (status != 0) return std::unexpected(GpuError{"cuDeviceGet failed: " + cuda->api.describe(status), status});
        if (cuda->api.device_get_name) {
            char name[256] = {};
            if (cuda->api.device_get_name(name, static_cast<int>(sizeof(name)), device) == 0) {
                cuda_device_name = name;
            }
        }
        status = cuda->api.context_create(&cuda->context, 0U, device);
        if (status != 0)
            return std::unexpected(GpuError{"cuCtxCreate_v2 failed: " + cuda->api.describe(status), status});
        if (!cuda->context)
            return std::unexpected(GpuError{"cuCtxCreate_v2 returned a null context", 0});
        cuda->owns_context = true;
        return {};
    }

    std::expected<void, GpuError> initialize_nvenc();
    std::expected<void, GpuError> prepare_slots(std::span<const capture::DmabufImage> images);
    void output_worker() noexcept;
    void watch_bitstream_lock() noexcept;
    void arm_bitstream_lock_deadline() noexcept;
    void clear_bitstream_lock_deadline() noexcept;
    void set_worker_error(std::string message);
    void defer_job(PendingEncode&& job, bool was_in_flight, std::string message);
    std::vector<media::EncodedPacket> collect_completed_locked();
    std::expected<std::vector<media::EncodedPacket>, GpuError> submit(
        capture::CapturedFrame&& frame, std::int64_t pts,
        std::chrono::steady_clock::time_point submitted_at);
    std::expected<std::vector<media::EncodedPacket>, GpuError> drain();
    void add_timing(std::uint64_t EncoderStats::*last, std::uint64_t EncoderStats::*total,
                    std::chrono::steady_clock::duration duration) noexcept;
};

std::expected<void, GpuError> DirectCudaEglEncoder::Impl::initialize_nvenc() {
    nvenc = std::make_unique<NvencState>();
    if (!nvenc->library) {
        return std::unexpected(GpuError{"dlopen(libnvidia-encode.so.1) failed", errno});
    }
    using CreateInstance = NVENCSTATUS (NVENCAPI *)(NV_ENCODE_API_FUNCTION_LIST*);
    const CreateInstance create_instance = nvenc->library.symbol<CreateInstance>("NvEncodeAPICreateInstance");
    if (!create_instance) {
        return std::unexpected(GpuError{"NvEncodeAPICreateInstance is absent", 0});
    }
    nvenc->functions.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    NVENCSTATUS status = create_instance(&nvenc->functions);
    if (status != NV_ENC_SUCCESS) {
        return std::unexpected(GpuError{"NvEncodeAPICreateInstance failed: " + std::to_string(status), static_cast<int>(status)});
    }
    const auto& f = nvenc->functions;
    if (!f.nvEncOpenEncodeSessionEx || !f.nvEncGetInputFormatCount ||
        !f.nvEncGetInputFormats || !f.nvEncGetEncodePresetConfigEx ||
        !f.nvEncInitializeEncoder || !f.nvEncGetSequenceParams ||
        !f.nvEncCreateBitstreamBuffer || !f.nvEncDestroyBitstreamBuffer ||
        !f.nvEncRegisterResource || !f.nvEncUnregisterResource ||
        !f.nvEncMapInputResource || !f.nvEncUnmapInputResource ||
        !f.nvEncEncodePicture || !f.nvEncLockBitstream ||
        !f.nvEncUnlockBitstream || !f.nvEncDestroyEncoder) {
        return std::unexpected(GpuError{"NVENC driver omitted a required function-table entry", 0});
    }
    const CUresult context_status = cuda->api.context_set_current(cuda->context);
    if (context_status != 0) {
        return std::unexpected(GpuError{"cuCtxSetCurrent before NVENC initialization failed: " + cuda->api.describe(context_status), context_status});
    }
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS open{};
    open.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    open.deviceType = NV_ENC_DEVICE_TYPE_CUDA;
    open.device = cuda->context;
    open.apiVersion = NVENCAPI_VERSION;
    status = nvenc->functions.nvEncOpenEncodeSessionEx(&open, &nvenc->encoder);
    if (status != NV_ENC_SUCCESS || !nvenc->encoder) {
        return std::unexpected(GpuError{"nvEncOpenEncodeSessionEx failed: " + std::to_string(status), static_cast<int>(status)});
    }

    const bool hevc = config.codec == Codec::hevc;
    const GUID codec_guid = hevc ? NV_ENC_CODEC_HEVC_GUID : NV_ENC_CODEC_H264_GUID;
    const char* codec_name = hevc ? "HEVC" : "H.264";
    uint32_t format_count = 0;
    status = nvenc->functions.nvEncGetInputFormatCount(
        nvenc->encoder, codec_guid, &format_count);
    if (status != NV_ENC_SUCCESS || format_count == 0U || format_count > 256U) {
        return std::unexpected(GpuError{std::string("NVENC ") + codec_name + " input format query failed", static_cast<int>(status)});
    }
    std::vector<NV_ENC_BUFFER_FORMAT> formats(format_count);
    uint32_t returned = 0;
    status = nvenc->functions.nvEncGetInputFormats(
        nvenc->encoder, codec_guid, formats.data(), format_count, &returned);
    if (status != NV_ENC_SUCCESS || returned > format_count ||
        std::find(formats.begin(), formats.begin() + returned,
                  NV_ENC_BUFFER_FORMAT_ARGB) == formats.begin() + returned) {
        return std::unexpected(GpuError{"NVENC does not advertise CUDAARRAY ARGB input", static_cast<int>(status)});
    }

    NV_ENC_PRESET_CONFIG preset{};
    preset.version = NV_ENC_PRESET_CONFIG_VER;
    preset.presetCfg.version = NV_ENC_CONFIG_VER;
    // Constant QP has to use the ultra-low-latency preset. P1/LL at QP 17
    // spent long enough in motion search that capture fell to about 120 FPS.
    const NV_ENC_TUNING_INFO tuning = config.qp
        ? NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY
        : NV_ENC_TUNING_INFO_LOW_LATENCY;
    const char* tuning_name = config.qp ? "ULL" : "LL";
    status = nvenc->functions.nvEncGetEncodePresetConfigEx(
        nvenc->encoder, codec_guid, NV_ENC_PRESET_P1_GUID, tuning, &preset);
    if (status != NV_ENC_SUCCESS) {
        return std::unexpected(GpuError{std::string("NVENC P1/") + tuning_name + " preset query failed: " + std::to_string(status), static_cast<int>(status)});
    }
    NV_ENC_CONFIG& encode = preset.presetCfg;
    encode.frameIntervalP = 1U;
    if (hevc) encode.profileGUID = NV_ENC_HEVC_PROFILE_MAIN_GUID;
    encode.gopLength = config.gop_size;
    if (config.qp) {
        if (*config.qp > 51U) {
            return std::unexpected(GpuError{
                "direct CUDA EGL constant QP must be between 0 and 51", 0});
        }
        // Same QP for every picture type. Adaptive quantization and VBV
        // targets would let the driver move away from that value.
        encode.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CONSTQP;
        encode.rcParams.constQP.qpIntra = *config.qp;
        encode.rcParams.constQP.qpInterP = *config.qp;
        encode.rcParams.constQP.qpInterB = *config.qp;
        encode.rcParams.averageBitRate = 0;
        encode.rcParams.maxBitRate = 0;
        encode.rcParams.vbvBufferSize = 0;
        encode.rcParams.vbvInitialDelay = 0;
        encode.rcParams.enableAQ = 0;
        encode.rcParams.enableTemporalAQ = 0;
        encode.rcParams.enableLookahead = 0;
        encode.rcParams.enableMinQP = 0;
        encode.rcParams.enableMaxQP = 0;
        encode.rcParams.enableInitialRCQP = 0;
        encode.rcParams.multiPass = NV_ENC_MULTI_PASS_DISABLED;
        encode.rcParams.enableLookahead = 0;
        encode.rcParams.lookaheadDepth = 0;
    } else {
        encode.rcParams.rateControlMode = NV_ENC_PARAMS_RC_VBR;
        encode.rcParams.averageBitRate = static_cast<uint32_t>(std::min<std::int64_t>(
            config.bitrate, std::numeric_limits<uint32_t>::max()));
    }
    if (hevc) {
        NV_ENC_CONFIG_HEVC& hevc_config = encode.encodeCodecConfig.hevcConfig;
        // 2560x1440 at 360 Hz is within HEVC level 6.1 sample rate. Main
        // profile, 8-bit 4:2:0. IDR period matches the GOP so replay can
        // enter on a clean picture, and each IDR repeats VPS/SPS/PPS.
        hevc_config.level = config.qp ? NV_ENC_LEVEL_HEVC_62 : NV_ENC_LEVEL_HEVC_61;
        hevc_config.tier = config.qp ? NV_ENC_TIER_HEVC_HIGH : NV_ENC_TIER_HEVC_MAIN;
        hevc_config.chromaFormatIDC = 1U;
        hevc_config.inputBitDepth = NV_ENC_BIT_DEPTH_8;
        hevc_config.outputBitDepth = NV_ENC_BIT_DEPTH_8;
        hevc_config.idrPeriod = config.gop_size;
        hevc_config.repeatSPSPPS = 1U;
        hevc_config.disableSPSPPS = 0U;
        if (config.qp) {
            hevc_config.maxNumRefFramesInDPB = 1U;
            hevc_config.numRefL0 = NV_ENC_NUM_REF_FRAMES_1;
            hevc_config.numRefL1 = NV_ENC_NUM_REF_FRAMES_1;
        }
        NV_ENC_CONFIG_HEVC_VUI_PARAMETERS& vui = hevc_config.hevcVUIParameters;
        vui.videoSignalTypePresentFlag = 1U;
        vui.videoFormat = NV_ENC_VUI_VIDEO_FORMAT_COMPONENT;
        vui.videoFullRangeFlag = 0U;
        vui.colourDescriptionPresentFlag = 1U;
        vui.colourPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT709;
        vui.transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_BT709;
        vui.colourMatrix = NV_ENC_VUI_MATRIX_COEFFS_BT709;
    } else {
        encode.encodeCodecConfig.h264Config.idrPeriod = config.gop_size;
        encode.encodeCodecConfig.h264Config.entropyCodingMode = NV_ENC_H264_ENTROPY_CODING_MODE_CAVLC;
        if (config.qp) {
            // Level 6.1 High caps the stream near 300 Mbps, which constant QP
            // 17 exceeds on motion and then misses frames. One reference keeps
            // motion search inside a 360 Hz frame.
            NV_ENC_CONFIG_H264& h264 = encode.encodeCodecConfig.h264Config;
            h264.level = NV_ENC_LEVEL_H264_62;
            h264.maxNumRefFrames = 1U;
            h264.numRefL0 = NV_ENC_NUM_REF_FRAMES_1;
            h264.numRefL1 = NV_ENC_NUM_REF_FRAMES_1;
        }
        NV_ENC_CONFIG_H264_VUI_PARAMETERS& vui =
            encode.encodeCodecConfig.h264Config.h264VUIParameters;
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
    initialize.encodeGUID = codec_guid;
    initialize.presetGUID = NV_ENC_PRESET_P1_GUID;
    initialize.encodeWidth = config.width;
    initialize.encodeHeight = config.height;
    initialize.darWidth = config.width;
    initialize.darHeight = config.height;
    initialize.frameRateNum = config.fps_num;
    initialize.frameRateDen = config.fps_den;
    initialize.enableEncodeAsync = 0U;
    initialize.enablePTD = 1U;
    initialize.encodeConfig = &encode;
    initialize.tuningInfo = tuning;
    status = nvenc->functions.nvEncInitializeEncoder(nvenc->encoder, &initialize);
    if (status != NV_ENC_SUCCESS) {
        return std::unexpected(GpuError{std::string("nvEncInitializeEncoder(") + codec_name + " P1/" + tuning_name + ") failed: " + std::to_string(status), static_cast<int>(status)});
    }
    nvenc->initialized = true;

    // NVENC returns Annex-B parameter sets. Matroska's FFmpeg muxer converts
    // them to avcC or hvcC when writing the track header.
    std::array<std::uint8_t, 4096> headers{};
    uint32_t headers_size = 0;
    NV_ENC_SEQUENCE_PARAM_PAYLOAD sequence{};
    sequence.version = NV_ENC_SEQUENCE_PARAM_PAYLOAD_VER;
    sequence.inBufferSize = static_cast<uint32_t>(headers.size());
    sequence.spsppsBuffer = headers.data();
    sequence.outSPSPPSPayloadSize = &headers_size;
    status = nvenc->functions.nvEncGetSequenceParams(nvenc->encoder, &sequence);
    if (status != NV_ENC_SUCCESS || headers_size == 0U || headers_size > headers.size()) {
        return std::unexpected(GpuError{std::string("NVENC did not provide bounded ") + codec_name + " parameter-set extradata", static_cast<int>(status)});
    }
    extradata.assign(headers.begin(), headers.begin() + headers_size);
    if (extradata.size() < 5U || extradata[0] != 0U || extradata[1] != 0U ||
        (extradata[2] != 1U && !(extradata[2] == 0U && extradata[3] == 1U))) {
        return std::unexpected(GpuError{std::string("NVENC ") + codec_name + " sequence params are not Annex-B", 0});
    }
    bool have_vps = false;
    bool have_sps = false;
    bool have_pps = false;
    for (std::size_t index = 0; index + 4U < extradata.size();) {
        std::size_t prefix = 0;
        if (extradata[index] == 0U && extradata[index + 1U] == 0U &&
            extradata[index + 2U] == 1U) {
            prefix = 3U;
        } else if (extradata[index] == 0U && extradata[index + 1U] == 0U &&
                   extradata[index + 2U] == 0U && extradata[index + 3U] == 1U) {
            prefix = 4U;
        }
        if (prefix == 0U) { ++index; continue; }
        const std::size_t nal = index + prefix;
        if (nal < extradata.size()) {
            const unsigned int type = hevc
                ? (extradata[nal] >> 1U) & 0x3fU
                : extradata[nal] & 0x1fU;
            if (hevc) {
                have_vps = have_vps || type == 32U;
                have_sps = have_sps || type == 33U;
                have_pps = have_pps || type == 34U;
            } else {
                have_sps = have_sps || type == 7U;
                have_pps = have_pps || type == 8U;
            }
        }
        index = nal + 1U;
    }
    if ((hevc && !have_vps) || !have_sps || !have_pps) {
        return std::unexpected(GpuError{std::string("NVENC sequence params lack a ") + codec_name + " parameter set", 0});
    }

    nvenc->bitstreams.reserve(pipeline_depth);
    for (std::size_t i = 0; i < pipeline_depth; ++i) {
        NV_ENC_CREATE_BITSTREAM_BUFFER create{};
        create.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
        status = nvenc->functions.nvEncCreateBitstreamBuffer(nvenc->encoder, &create);
        if (status != NV_ENC_SUCCESS || !create.bitstreamBuffer) {
            return std::unexpected(GpuError{"nvEncCreateBitstreamBuffer failed: " + std::to_string(status), static_cast<int>(status)});
        }
        nvenc->bitstreams.push_back(create.bitstreamBuffer);
    }
    return {};
}

std::expected<void, GpuError> DirectCudaEglEncoder::Impl::prepare_slots(
    std::span<const capture::DmabufImage> images) {
    if (cuda->api.context_set_current(cuda->context) != 0) {
        return std::unexpected(GpuError{"cuCtxSetCurrent before EGL registration failed", 0});
    }
    slots.reserve(images.size());
    for (const capture::DmabufImage& image : images) {
        SlotInterop slot;
        const auto& plane = image.planes[0];
        slot.offset = plane.offset;
        slot.stride = plane.stride;
        slot.modifier = image.modifier;
        const int duplicate = ::fcntl(plane.fd, F_DUPFD_CLOEXEC, 0);
        if (duplicate < 0) {
            return std::unexpected(GpuError{"duplicating capture DMA-BUF fd failed: " + std::string(std::strerror(errno)), errno});
        }
        slot.dma_buf_fd.reset(duplicate);
        const EGLint attrs[] = {
            EGL_WIDTH, static_cast<EGLint>(image.width),
            EGL_HEIGHT, static_cast<EGLint>(image.height),
            EGL_LINUX_DRM_FOURCC_EXT, static_cast<EGLint>(image.fourcc),
            EGL_DMA_BUF_PLANE0_FD_EXT, slot.dma_buf_fd.get(),
            EGL_DMA_BUF_PLANE0_OFFSET_EXT, static_cast<EGLint>(plane.offset),
            EGL_DMA_BUF_PLANE0_PITCH_EXT, static_cast<EGLint>(plane.stride),
            EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, static_cast<EGLint>(image.modifier & 0xffffffffULL),
            EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, static_cast<EGLint>((image.modifier >> 32U) & 0xffffffffULL),
            EGL_NONE,
        };
        slot.egl_image = egl->api.create_image(egl->display, EGL_NO_CONTEXT,
                                               EGL_LINUX_DMA_BUF_EXT, nullptr, attrs);
        if (slot.egl_image == EGL_NO_IMAGE_KHR) {
            return std::unexpected(GpuError{"eglCreateImageKHR failed with EGL error " + std::to_string(eglGetError()), 0});
        }
        egl->images.push_back(slot.egl_image);
        CUresult cu_status = cuda->api.graphics_egl_register_image(
            &slot.cuda_resource, slot.egl_image, kCudaEglRegisterFlags);
        if (cu_status != 0) {
            return std::unexpected(GpuError{"cuGraphicsEGLRegisterImage failed: " + cuda->api.describe(cu_status), cu_status});
        }
        cuda->resources.push_back(slot.cuda_resource);
        cu_status = cuda->api.graphics_resource_get_mapped_egl_frame(
            &slot.cuda_frame, slot.cuda_resource, 0U, 0U);
        if (cu_status != 0 || slot.cuda_frame.frameType != CU_EGL_FRAME_TYPE_ARRAY ||
            slot.cuda_frame.planeCount != 1U || !slot.cuda_frame.frame.pArray[0] ||
            slot.cuda_frame.width != config.width || slot.cuda_frame.height != config.height ||
            slot.cuda_frame.cuFormat != 1 || slot.cuda_frame.numChannels != 4U ||
            slot.cuda_frame.eglColorFormat != 6) {
            return std::unexpected(GpuError{
                cu_status != 0 ? "cuGraphicsResourceGetMappedEglFrame failed: " + cuda->api.describe(cu_status)
                    : "CUDA EGL import did not expose the expected 8-bit ARGB CUDA array", cu_status});
        }
        NV_ENC_REGISTER_RESOURCE registration{};
        registration.version = NV_ENC_REGISTER_RESOURCE_VER;
        registration.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_CUDAARRAY;
        registration.width = slot.cuda_frame.width;
        registration.height = slot.cuda_frame.height;
        registration.pitch = slot.cuda_frame.width * slot.cuda_frame.numChannels;
        registration.resourceToRegister = slot.cuda_frame.frame.pArray[0];
        registration.bufferFormat = NV_ENC_BUFFER_FORMAT_ARGB;
        registration.bufferUsage = NV_ENC_INPUT_IMAGE;
        const NVENCSTATUS nv_status = nvenc->functions.nvEncRegisterResource(
            nvenc->encoder, &registration);
        if (nv_status != NV_ENC_SUCCESS || !registration.registeredResource) {
            return std::unexpected(GpuError{"nvEncRegisterResource(CUDAARRAY ARGB) failed: " + std::to_string(nv_status), static_cast<int>(nv_status)});
        }
        slot.nvenc_resource = registration.registeredResource;
        nvenc->registered.push_back(slot.nvenc_resource);
        slots.push_back(std::move(slot));
    }
    return {};
}

void DirectCudaEglEncoder::Impl::set_worker_error(std::string message) {
    std::lock_guard lock(mutex);
    if (worker_error.empty()) worker_error = std::move(message);
    changed.notify_all();
}

void DirectCudaEglEncoder::Impl::defer_job(
    PendingEncode&& job, bool was_in_flight, std::string message) {
    std::lock_guard lock(mutex);
    deferred.push_back(std::move(job));
    ++deferred_count;
    if (was_in_flight && in_flight > 0U) --in_flight;
    if (!message.empty() && worker_error.empty()) worker_error = std::move(message);
    counters.pending_capture_leases = in_flight + deferred_count;
    changed.notify_all();
}

std::vector<media::EncodedPacket>
DirectCudaEglEncoder::Impl::collect_completed_locked() {
    std::vector<media::EncodedPacket> result;
    result.reserve(completed.size());
    while (!completed.empty()) {
        result.push_back(std::move(completed.front()));
        completed.pop_front();
    }
    return result;
}

void DirectCudaEglEncoder::Impl::add_timing(
    std::uint64_t EncoderStats::*last, std::uint64_t EncoderStats::*total,
    std::chrono::steady_clock::duration duration) noexcept {
    const auto value = std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
    if (value <= 0) return;
    const std::uint64_t ns = static_cast<std::uint64_t>(value);
    std::lock_guard lock(mutex);
    counters.*last = ns;
    counters.*total += ns;
}

void DirectCudaEglEncoder::Impl::arm_bitstream_lock_deadline() noexcept {
    constexpr auto limit = std::chrono::seconds(5);
    const auto deadline = std::chrono::steady_clock::now() + limit;
    bitstream_lock_deadline_ticks.store(
        deadline.time_since_epoch().count(), std::memory_order_release);
}

void DirectCudaEglEncoder::Impl::clear_bitstream_lock_deadline() noexcept {
    bitstream_lock_deadline_ticks.store(0, std::memory_order_release);
}

void DirectCudaEglEncoder::Impl::watch_bitstream_lock() noexcept {
    while (!lock_watchdog_stop.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const auto deadline = bitstream_lock_deadline_ticks.load(std::memory_order_acquire);
        if (deadline == 0) continue;
        const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        if (now <= deadline) continue;
        // Do not unmap, unlock, or destroy the capture lease. NVENC has no
        // cancel for a blocking lock, and this driver cannot use doNotWait or
        // Linux async completion events. Exiting skips C++ destructors.
        std::cerr << "NVENC bitstream lock exceeded 5s; exiting without "
                     "releasing resources NVENC may still own\n";
        std::_Exit(70);
    }
}

void DirectCudaEglEncoder::Impl::output_worker() noexcept {
    bool context_ready = true;
    if (cuda && cuda->context) {
        const CUresult status = cuda->api.context_set_current(cuda->context);
        if (status != 0) {
            context_ready = false;
            set_worker_error("output worker cuCtxSetCurrent failed: " + cuda->api.describe(status));
        }
    }
    for (;;) {
        std::optional<PendingEncode> job;
        bool have_job = false;
        bool defer_without_lock = false;
        {
            std::unique_lock lock(mutex);
            changed.wait(lock, [&] {
                return producer_done || (!queue.empty() && queue.front().ready);
            });
            if (queue.empty() && producer_done) break;
            if (queue.empty()) continue;
            if (!queue.front().ready && producer_done && !eos_flushed) {
                job.emplace(std::move(queue.front()));
                queue.pop_front();
                defer_without_lock = true;
                have_job = true;
            } else if (queue.front().ready || !context_ready) {
                job.emplace(std::move(queue.front()));
                queue.pop_front();
                have_job = true;
            }
        }
        if (!have_job) continue;
        if (!context_ready) {
            defer_job(std::move(*job), true,
                "retaining NVENC input because the output worker could not make its CUDA context current");
            continue;
        }
        if (defer_without_lock) {
            defer_job(std::move(*job), true,
                "NVENC EOS failed before a delayed input produced a bitstream; retaining its lease until teardown");
            continue;
        }

        // A 5s deadline exits the process if this call does not return.
        // doNotWait is not used: on this driver it returned NV_ENC_SUCCESS
        // with an empty bitstream. Linux has no NVENC async completion event.
        // The deadline path does not unmap or destroy the in-flight lease.
        struct BitstreamLockDeadline {
            Impl& self;
            explicit BitstreamLockDeadline(Impl& self) noexcept : self(self) {
                self.arm_bitstream_lock_deadline();
            }
            ~BitstreamLockDeadline() { self.clear_bitstream_lock_deadline(); }
        };
        NV_ENC_LOCK_BITSTREAM lock_params{};
        lock_params.version = NV_ENC_LOCK_BITSTREAM_VER;
        lock_params.outputBitstream = job->output;
        auto started = std::chrono::steady_clock::now();
        NVENCSTATUS status = NV_ENC_ERR_GENERIC;
        {
            const BitstreamLockDeadline deadline{*this};
            status = nvenc->functions.nvEncLockBitstream(nvenc->encoder, &lock_params);
        }
        auto finished = std::chrono::steady_clock::now();
        {
            std::lock_guard lock(mutex);
            const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(finished - started).count();
            counters.last_encoder_receive_cpu_ns = ns > 0 ? static_cast<std::uint64_t>(ns) : 0U;
            counters.total_encoder_receive_cpu_ns += counters.last_encoder_receive_cpu_ns;
        }
        if (status != NV_ENC_SUCCESS) {
            std::unique_lock lock(mutex);
            if (worker_error.empty()) worker_error = "nvEncLockBitstream failed: " + std::to_string(status);
            changed.notify_all();
            changed.wait(lock, [&] { return eos_submitted || producer_done; });
            const bool retry = eos_flushed;
            lock.unlock();
            if (retry) {
                lock_params = {};
                lock_params.version = NV_ENC_LOCK_BITSTREAM_VER;
                lock_params.outputBitstream = job->output;
                started = std::chrono::steady_clock::now();
                {
                    const BitstreamLockDeadline deadline{*this};
                    status = nvenc->functions.nvEncLockBitstream(nvenc->encoder, &lock_params);
                }
                finished = std::chrono::steady_clock::now();
                std::lock_guard timing_lock(mutex);
                const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(finished - started).count();
                counters.last_encoder_receive_cpu_ns = ns > 0 ? static_cast<std::uint64_t>(ns) : 0U;
                counters.total_encoder_receive_cpu_ns += counters.last_encoder_receive_cpu_ns;
            }
        }

        const bool locked = status == NV_ENC_SUCCESS;
        const bool valid_output = locked && lock_params.bitstreamBufferPtr &&
            lock_params.bitstreamSizeInBytes != 0U &&
            lock_params.bitstreamSizeInBytes <= 64U * 1024U * 1024U;
        media::EncodedPacket packet;
        bool packet_valid = false;
        if (valid_output) {
            if (lock_params.outputTimeStamp != job->captured.sequence) {
                set_worker_error("NVENC output timestamp did not match the captured source sequence");
            } else {
                const auto* bytes = static_cast<const std::uint8_t*>(lock_params.bitstreamBufferPtr);
                packet.data.assign(bytes, bytes + lock_params.bitstreamSizeInBytes);
                packet.codec = config.codec == Codec::hevc
                    ? media::VideoCodec::hevc : media::VideoCodec::h264;
                packet.pts = job->pts;
                packet.dts = job->pts;
                const std::uint64_t scaled_period = 1'000'000ULL * config.fps_den;
                const std::uint64_t duration = std::max<std::uint64_t>(1U,
                    (scaled_period + config.fps_num / 2U) / config.fps_num);
                packet.duration = static_cast<std::int64_t>(duration);
                packet.keyframe = lock_params.pictureType == NV_ENC_PIC_TYPE_IDR;
                packet.time_base = {1, static_cast<int>(kPacketTimeBaseDen)};
                packet_valid = true;
            }
        } else if (locked) {
            set_worker_error("NVENC locked an empty or over-limit bitstream buffer");
        } else {
            set_worker_error("nvEncLockBitstream still failed after EOS: " + std::to_string(status));
        }

        if (locked) {
            nvenc->locked.push_back(job->output);
            const NVENCSTATUS unlock_status = nvenc->functions.nvEncUnlockBitstream(
                nvenc->encoder, job->output);
            if (unlock_status == NV_ENC_SUCCESS) {
                std::erase(nvenc->locked, job->output);
            } else {
                defer_job(std::move(*job), true,
                    "nvEncUnlockBitstream failed: " + std::to_string(unlock_status));
                continue;
            }
        } else {
            defer_job(std::move(*job), true,
                "nvEncLockBitstream failed after EOS: " + std::to_string(status));
            continue;
        }

        const NVENCSTATUS unmap_status = nvenc->functions.nvEncUnmapInputResource(
            nvenc->encoder, job->mapped_input);
        if (unmap_status != NV_ENC_SUCCESS) {
            defer_job(std::move(*job), true,
                "nvEncUnmapInputResource failed: " + std::to_string(unmap_status));
            continue;
        } else {
            {
                std::lock_guard lock(mutex);
                std::erase(nvenc->mapped, job->mapped_input);
            }
            job->captured.lease.release_after_gpu();
            if (packet_valid) {
                const auto latency = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - job->submitted_at).count();
                std::lock_guard lock(mutex);
                counters.last_submit_to_packet_wall_ns = latency > 0
                    ? static_cast<std::uint64_t>(latency) : 0U;
                counters.total_submit_to_packet_wall_ns += counters.last_submit_to_packet_wall_ns;
                counters.max_submit_to_packet_wall_ns = std::max(
                    counters.max_submit_to_packet_wall_ns, counters.last_submit_to_packet_wall_ns);
                ++counters.submit_to_packet_samples;
                ++counters.encoded_frames;
                completed.push_back(std::move(packet));
            }
        }
        {
            std::lock_guard lock(mutex);
            if (in_flight > 0U) --in_flight;
            if (!job->captured.lease && counters.pending_capture_leases > 0U) {
                --counters.pending_capture_leases;
            }
            changed.notify_all();
        }
    }
    if (cuda && cuda->context) (void)cuda->api.context_set_current(nullptr);
    changed.notify_all();
}

std::expected<std::vector<media::EncodedPacket>, GpuError>
DirectCudaEglEncoder::Impl::submit(capture::CapturedFrame&& frame, std::int64_t pts,
                                    std::chrono::steady_clock::time_point submitted_at) {
    if (drained || !nvenc || !nvenc->encoder) {
        return std::unexpected(GpuError{"direct CUDA EGL encoder has been drained", 0});
    }
    if (!frame.lease || frame.lease.slot_index() >= slots.size() || pts < 0) {
        return std::unexpected(GpuError{"captured frame lease, slot, or PTS is invalid", 0});
    }
    const std::size_t slot_index = frame.lease.slot_index();
    const capture::DmabufImage& image = frame.lease.image();
    if (image.width != config.width || image.height != config.height ||
        image.fourcc != DRM_FORMAT_ARGB8888 || image.planes.size() != 1U ||
        image.planes[0].fd < 0 || image.planes[0].offset != slots[slot_index].offset ||
        image.planes[0].stride != slots[slot_index].stride ||
        image.modifier != slots[slot_index].modifier) {
        return std::unexpected(GpuError{"captured lease does not match the configured AR24 pool", 0});
    }
    {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] {
            return !worker_error.empty() || drained || in_flight < pipeline_depth ||
                (in_flight > 0U && unready == in_flight);
        });
        if (!worker_error.empty()) return std::unexpected(GpuError{worker_error, 0});
        if (drained) return std::unexpected(GpuError{"direct CUDA EGL encoder has been drained", 0});
        if (in_flight >= pipeline_depth && unready == in_flight) {
            lock.unlock();
            frame.lease.release_after_gpu();
            return std::unexpected(GpuError{
                "NVENC delayed every input at the bounded pipeline depth; refusing to stall capture", 0});
        }
    }

    const auto import_started = std::chrono::steady_clock::now();
    std::string fence_error;
    if (!wait_for_read_fence(image.planes[0].fd, fence_error)) {
        return std::unexpected(GpuError{std::move(fence_error), errno});
    }
    const CUresult context_status = cuda->api.context_set_current(cuda->context);
    if (context_status != 0) {
        return std::unexpected(GpuError{"cuCtxSetCurrent before encode failed: " + cuda->api.describe(context_status), context_status});
    }
    NV_ENC_MAP_INPUT_RESOURCE map{};
    map.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
    map.registeredResource = slots[slot_index].nvenc_resource;
    NVENCSTATUS status = nvenc->functions.nvEncMapInputResource(nvenc->encoder, &map);
    if (map.mappedResource) {
        std::lock_guard lock(mutex);
        nvenc->mapped.push_back(map.mappedResource);
    }
    if (status != NV_ENC_SUCCESS || !map.mappedResource ||
        map.mappedBufferFmt != NV_ENC_BUFFER_FORMAT_ARGB) {
        if (map.mappedResource) {
            const NVENCSTATUS unmap_status = nvenc->functions.nvEncUnmapInputResource(
                nvenc->encoder, map.mappedResource);
            if (unmap_status == NV_ENC_SUCCESS) {
                std::lock_guard lock(mutex);
                std::erase(nvenc->mapped, map.mappedResource);
            } else {
                PendingEncode retained{.captured = std::move(frame), .mapped_input = map.mappedResource};
                defer_job(std::move(retained), false,
                    "nvEncUnmapInputResource failed after rejected CUDAARRAY map: " +
                    std::to_string(unmap_status));
            }
        }
        return std::unexpected(GpuError{
            "nvEncMapInputResource(CUDAARRAY ARGB) failed or returned an incompatible mapping: " +
                std::to_string(status), static_cast<int>(status)});
    }
    add_timing(&EncoderStats::last_dmabuf_import_cpu_ns,
               &EncoderStats::total_dmabuf_import_cpu_ns,
               std::chrono::steady_clock::now() - import_started);

    NV_ENC_PIC_PARAMS picture{};
    picture.version = NV_ENC_PIC_PARAMS_VER;
    picture.inputWidth = config.width;
    picture.inputHeight = config.height;
    picture.inputPitch = config.width * 4U;
    picture.inputTimeStamp = frame.sequence;
    picture.inputDuration = 1U;
    picture.inputBuffer = map.mappedResource;
    picture.outputBitstream = nvenc->bitstreams[next_output % nvenc->bitstreams.size()];
    picture.bufferFmt = NV_ENC_BUFFER_FORMAT_ARGB;
    picture.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    const auto encode_started = std::chrono::steady_clock::now();
    constexpr unsigned int kBusyRetries = 8U;
    unsigned int retries = 0;
    do {
        status = nvenc->functions.nvEncEncodePicture(nvenc->encoder, &picture);
        if (status != NV_ENC_ERR_ENCODER_BUSY || retries == kBusyRetries) break;
        ++retries;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (true);
    add_timing(&EncoderStats::last_encoder_send_cpu_ns,
               &EncoderStats::total_encoder_send_cpu_ns,
               std::chrono::steady_clock::now() - encode_started);

    if (status != NV_ENC_SUCCESS && status != NV_ENC_ERR_NEED_MORE_INPUT) {
        const NVENCSTATUS unmap_status = nvenc->functions.nvEncUnmapInputResource(
            nvenc->encoder, map.mappedResource);
        if (unmap_status == NV_ENC_SUCCESS) {
            std::lock_guard lock(mutex);
            std::erase(nvenc->mapped, map.mappedResource);
        } else {
            PendingEncode retained{.captured = std::move(frame), .mapped_input = map.mappedResource};
            defer_job(std::move(retained), false,
                "nvEncUnmapInputResource failed after encode rejection: " + std::to_string(unmap_status));
        }
        return std::unexpected(GpuError{"nvEncEncodePicture failed: " + std::to_string(status), static_cast<int>(status)});
    }

    PendingEncode pending{
        .captured = std::move(frame),
        .mapped_input = map.mappedResource,
        .output = picture.outputBitstream,
        .pts = pts,
        .submitted_at = submitted_at,
        .ready = status == NV_ENC_SUCCESS,
    };
    {
        std::lock_guard lock(mutex);
        if (pending.ready) {
            for (PendingEncode& earlier : queue) {
                if (!earlier.ready) {
                    earlier.ready = true;
                    if (unready > 0U) --unready;
                }
            }
        }
        queue.push_back(std::move(pending));
        ++in_flight;
        if (status == NV_ENC_ERR_NEED_MORE_INPUT) ++unready;
        ++next_output;
        ++counters.submitted_frames;
        counters.pending_capture_leases = in_flight + deferred_count;
        changed.notify_all();
        return collect_completed_locked();
    }
}

std::expected<std::vector<media::EncodedPacket>, GpuError>
DirectCudaEglEncoder::Impl::drain() {
    if (drained) {
        std::lock_guard lock(mutex);
        return collect_completed_locked();
    }
    if (!nvenc || !nvenc->encoder || !nvenc->initialized) {
        drained = true;
        std::lock_guard lock(mutex);
        producer_done = true;
        changed.notify_all();
        return collect_completed_locked();
    }
    NV_ENC_PIC_PARAMS eos{};
    eos.version = NV_ENC_PIC_PARAMS_VER;
    eos.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
    const CUresult context_status = cuda->api.context_set_current(cuda->context);
    const NVENCSTATUS eos_status = context_status == 0
        ? nvenc->functions.nvEncEncodePicture(nvenc->encoder, &eos)
        : NV_ENC_ERR_GENERIC;
    {
        std::lock_guard lock(mutex);
        eos_submitted = true;
        eos_flushed = eos_status == NV_ENC_SUCCESS;
        if (eos_status != NV_ENC_SUCCESS && worker_error.empty()) {
            worker_error = context_status != 0
                ? "cuCtxSetCurrent before EOS failed: " + cuda->api.describe(context_status)
                : "nvEncEncodePicture(EOS) failed: " + std::to_string(eos_status);
        }
        if (eos_flushed) {
            for (PendingEncode& pending : queue) {
                if (!pending.ready) {
                    pending.ready = true;
                    if (unready > 0U) --unready;
                }
            }
        }
        producer_done = true;
        drained = true;
    }
    changed.notify_all();
    if (worker.joinable()) worker.join();
    std::lock_guard lock(mutex);
    counters.pending_capture_leases = in_flight + deferred_count;
    auto packets = collect_completed_locked();
    if (!worker_error.empty()) return std::unexpected(GpuError{worker_error, static_cast<int>(eos_status)});
    if (in_flight != 0U || deferred_count != 0U) {
        return std::unexpected(GpuError{"direct CUDA EGL drain retained unresolved capture leases", 0});
    }
    return packets;
}

DirectCudaEglEncoder::DirectCudaEglEncoder(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
DirectCudaEglEncoder::~DirectCudaEglEncoder() = default;
DirectCudaEglEncoder::DirectCudaEglEncoder(DirectCudaEglEncoder&&) noexcept = default;
DirectCudaEglEncoder& DirectCudaEglEncoder::operator=(DirectCudaEglEncoder&&) noexcept = default;

std::expected<DirectCudaEglEncoder, GpuError> DirectCudaEglEncoder::create(
    std::string render_node, std::span<const capture::DmabufImage> images,
    DirectCudaEglConfig config) {
    auto created = Impl::create(std::move(render_node), images, config);
    if (!created) return std::unexpected(created.error());
    return DirectCudaEglEncoder{std::move(*created)};
}

std::expected<std::vector<media::EncodedPacket>, GpuError> DirectCudaEglEncoder::submit(
    capture::CapturedFrame&& frame, std::int64_t pts,
    std::chrono::steady_clock::time_point submitted_at) {
    if (!impl_) return std::unexpected(GpuError{"direct CUDA EGL encoder is not initialized", 0});
    return impl_->submit(std::move(frame), pts, submitted_at);
}

std::expected<std::vector<media::EncodedPacket>, GpuError> DirectCudaEglEncoder::drain() {
    if (!impl_) return std::unexpected(GpuError{"direct CUDA EGL encoder is not initialized", 0});
    return impl_->drain();
}

EncoderStats DirectCudaEglEncoder::stats() const noexcept {
    if (!impl_) return {};
    std::lock_guard lock(impl_->mutex);
    return impl_->counters;
}

std::span<const std::uint8_t> DirectCudaEglEncoder::codec_extradata() const noexcept {
    if (!impl_) return {};
    return impl_->extradata;
}

std::string DirectCudaEglEncoder::cuda_device_name() const {
    if (!impl_) return {};
    return impl_->cuda_device_name;
}

} // namespace fluxrec::gpu
