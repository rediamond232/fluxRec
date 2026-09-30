#include "gpu/gpu.hpp"
#include "gpu/direct_cuda_egl_encoder.hpp"
#include "gpu/direct_nv12.hpp"
#include "gpu/dmabuf_black_probe.hpp"
#include "gpu/frame_identity.hpp"
#include "gpu/preview_producer.hpp"
#include "gpu/rate_control_mode.hpp"

#include <gbm.h>
#include <drm_fourcc.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/buffer.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/hwcontext_vulkan.h>
#include <libavutil/log.h>
#include <libavutil/opt.h>
}
#include <vulkan/vulkan.h>

#include <linux/dma-buf.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <fcntl.h>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <poll.h>
#include <string>
#include <string_view>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace fluxrec::gpu {
namespace {

constexpr AVRational kPacketTimeBase{1, 1'000'000};
constexpr std::uint64_t kInputFramesExecPoolDrainTimeoutNs = 2'000'000'000ULL;

std::string av_error_string(int error) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
    if (av_strerror(error, buffer.data(), buffer.size()) < 0) {
        return "unknown FFmpeg error";
    }
    return buffer.data();
}

GpuError error(std::string message, int code = 0) {
    if (code < 0) {
        message += ": ";
        message += av_error_string(code);
    }
    return GpuError{std::move(message), code};
}

bool input_frames_ref_audit_enabled() noexcept {
    static const bool enabled = []() noexcept {
        const char* value = std::getenv("FLUXREC_GPU_INPUT_REF_AUDIT");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

using InputFramesExecPoolDrain = int (*)(AVBufferRef*, std::uint64_t);

InputFramesExecPoolDrain resolve_input_frames_exec_pool_drain() noexcept {
    return reinterpret_cast<InputFramesExecPoolDrain>(
        dlsym(RTLD_DEFAULT, "av_vk_frame_ctx_exec_pool_drain"));
}

void log_input_frames_ref_audit(std::string_view stage,
                                const AVBufferRef* input_frames) noexcept {
    if (!input_frames_ref_audit_enabled()) return;

    if (input_frames) {
        av_log(nullptr, AV_LOG_INFO,
               "FLUXREC_GPU_INPUT_REF_AUDIT stage=%.*s input_frames_ref_count=%d\n",
               static_cast<int>(stage.size()), stage.data(),
               av_buffer_get_ref_count(input_frames));
    } else {
        av_log(nullptr, AV_LOG_INFO,
               "FLUXREC_GPU_INPUT_REF_AUDIT stage=%.*s input_frames_ref_count=unavailable input_frames_present=0\n",
               static_cast<int>(stage.size()), stage.data());
    }
}

std::optional<std::string_view> ffmpeg_encoder_tune(EncoderTune tune) {
    switch (tune) {
        case EncoderTune::default_tuning: return "default";
        case EncoderTune::ll: return "ll";
        case EncoderTune::ull: return "ull";
        case EncoderTune::hq: return "hq";
    }
    return std::nullopt;
}

std::optional<std::string_view> ffmpeg_h264_coder(H264Coder coder) {
    switch (coder) {
        case H264Coder::cabac: return "cabac";
        case H264Coder::vlc: return "vlc";
    }
    return std::nullopt;
}

std::optional<std::string_view> ffmpeg_nvenc_h264_coder(H264Coder coder) {
    switch (coder) {
        case H264Coder::cabac: return "cabac";
        case H264Coder::vlc: return "cavlc";
    }
    return std::nullopt;
}

std::optional<std::string> drm_node_path(std::uint64_t wanted) {
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator("/dev/dri", ec)) {
        if (ec) {
            break;
        }
        struct stat info {};
        if (::stat(entry.path().c_str(), &info) == 0 && S_ISCHR(info.st_mode) &&
            static_cast<std::uint64_t>(info.st_rdev) == wanted) {
            return entry.path().string();
        }
    }
    return std::nullopt;
}

bool device_matches_render_node(VkPhysicalDevice physical, const struct stat& node_stat) {
    std::uint32_t ext_count = 0;
    if (vkEnumerateDeviceExtensionProperties(physical, nullptr, &ext_count, nullptr) != VK_SUCCESS) {
        return false;
    }
    std::vector<VkExtensionProperties> extensions(ext_count);
    if (vkEnumerateDeviceExtensionProperties(physical, nullptr, &ext_count,
                                              extensions.data()) != VK_SUCCESS) {
        return false;
    }
    const bool has_drm_properties = std::any_of(extensions.begin(), extensions.end(), [](const auto& e) {
        return std::strcmp(e.extensionName, VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME) == 0;
    });
    if (!has_drm_properties) {
        return false;
    }

    VkPhysicalDeviceDrmPropertiesEXT drm_props{};
    drm_props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT;
    VkPhysicalDeviceProperties2 properties{};
    properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    properties.pNext = &drm_props;
    vkGetPhysicalDeviceProperties2(physical, &properties);
    if (!drm_props.hasRender) {
        return false;
    }
    return drm_props.renderMajor == static_cast<std::uint32_t>(major(node_stat.st_rdev)) &&
           drm_props.renderMinor == static_cast<std::uint32_t>(minor(node_stat.st_rdev));
}

enum class EncoderBackend { vulkan, nvenc, experimental_cuda_egl };

std::expected<EncoderBackend, GpuError> selected_encoder_backend() {
    const char* value = std::getenv("FLUXREC_GPU_ENCODER_BACKEND");
    if (!value || !*value || std::strcmp(value, "vulkan") == 0) return EncoderBackend::vulkan;
    if (std::strcmp(value, "nvenc") == 0) return EncoderBackend::nvenc;
    if (std::strcmp(value, "experimental-cuda-egl") == 0) {
        return EncoderBackend::experimental_cuda_egl;
    }
    return std::unexpected(error(
        "FLUXREC_GPU_ENCODER_BACKEND must be 'vulkan', 'nvenc', or 'experimental-cuda-egl'"));
}

bool valid_nvenc_preset(std::string_view preset) {
    return preset == "p1" || preset == "p2" || preset == "p3" ||
           preset == "p4" || preset == "p5" || preset == "p6" ||
           preset == "p7";
}

bool environment_enabled(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
}

struct BoDeleter {
    void operator()(gbm_bo* bo) const noexcept {
        if (bo) {
            gbm_bo_destroy(bo);
        }
    }
};

struct GbmSlot {
    std::unique_ptr<gbm_bo, BoDeleter> bo;
    std::vector<capture::UniqueFd> fds;
    capture::DmabufImage image;
};

std::optional<GbmSlot> make_gbm_slot(gbm_device* device,
                                     const capture::CaptureRequirements& requirements,
                                     std::uint32_t fourcc, std::uint64_t modifier) {
    GbmSlot slot;
    gbm_bo* bo = gbm_bo_create_with_modifiers2(device, requirements.width, requirements.height,
                                                fourcc, &modifier, 1, GBM_BO_USE_RENDERING);
    if (!bo) {
        return std::nullopt;
    }
    slot.bo.reset(bo);
    if (gbm_bo_get_modifier(bo) != modifier) {
        return std::nullopt;
    }
    const int plane_count = gbm_bo_get_plane_count(bo);
    if (plane_count != 1) {
        return std::nullopt;
    }
    capture::UniqueFd fd{gbm_bo_get_fd_for_plane(bo, 0)};
    if (!fd) {
        return std::nullopt;
    }
    slot.image.width = requirements.width;
    slot.image.height = requirements.height;
    slot.image.fourcc = fourcc;
    slot.image.modifier = modifier;
    slot.image.planes.push_back({fd.get(), gbm_bo_get_offset(bo, 0),
                                 gbm_bo_get_stride_for_plane(bo, 0)});
    slot.fds.push_back(std::move(fd));
    return slot;
}

struct DrmDescriptorOwner {
    AVDRMFrameDescriptor descriptor{};
    capture::UniqueFd fd;
};

struct ScopedCpuTiming {
    std::uint64_t& last_ns;
    std::uint64_t& total_ns;
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();

    ~ScopedCpuTiming() noexcept {
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started).count();
        const auto duration_ns = elapsed > 0 ? static_cast<std::uint64_t>(elapsed) : 0;
        last_ns += duration_ns;
        total_ns += duration_ns;
    }
};

std::uint64_t elapsed_ns(std::chrono::steady_clock::time_point started) noexcept {
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - started).count();
    return elapsed > 0 ? static_cast<std::uint64_t>(elapsed) : 0;
}

void free_drm_descriptor(void* opaque, std::uint8_t*) {
    delete static_cast<DrmDescriptorOwner*>(opaque);
}

AVFrame* make_drm_frame(const capture::DmabufImage& image, GpuError& failure) {
    if (image.planes.size() != 1 || image.planes[0].fd < 0 || image.width == 0 || image.height == 0) {
        failure = error("only one-plane XRGB8888/ARGB8888 DMA-BUF images are supported");
        return nullptr;
    }
    const auto& plane = image.planes[0];
    const int duplicate = ::fcntl(plane.fd, F_DUPFD_CLOEXEC, 0);
    if (duplicate < 0) {
        failure = error("duplicating capture DMA-BUF fd failed", AVERROR(errno));
        return nullptr;
    }
    auto* owner = new (std::nothrow) DrmDescriptorOwner;
    if (!owner) {
        ::close(duplicate);
        failure = error("allocating DRM descriptor failed", AVERROR(ENOMEM));
        return nullptr;
    }
    owner->fd.reset(duplicate);
    owner->descriptor.nb_objects = 1;
    owner->descriptor.objects[0].fd = duplicate;
    owner->descriptor.objects[0].format_modifier = image.modifier;
    const auto estimated_size = static_cast<std::uint64_t>(plane.offset) +
        static_cast<std::uint64_t>(plane.stride) * static_cast<std::uint64_t>(image.height);
    owner->descriptor.objects[0].size = static_cast<std::size_t>(estimated_size);
    owner->descriptor.nb_layers = 1;
    owner->descriptor.layers[0].format = image.fourcc;
    owner->descriptor.layers[0].nb_planes = 1;
    owner->descriptor.layers[0].planes[0].object_index = 0;
    owner->descriptor.layers[0].planes[0].offset = static_cast<std::ptrdiff_t>(plane.offset);
    owner->descriptor.layers[0].planes[0].pitch = static_cast<std::ptrdiff_t>(plane.stride);

    AVFrame* frame = av_frame_alloc();
    if (!frame) {
        delete owner;
        failure = error("allocating DRM frame failed", AVERROR(ENOMEM));
        return nullptr;
    }
    frame->format = AV_PIX_FMT_DRM_PRIME;
    frame->width = static_cast<int>(image.width);
    frame->height = static_cast<int>(image.height);
    frame->data[0] = reinterpret_cast<std::uint8_t*>(&owner->descriptor);
    frame->buf[0] = av_buffer_create(frame->data[0], sizeof(owner->descriptor),
                                     free_drm_descriptor, owner, 0);
    if (!frame->buf[0]) {
        delete owner;
        av_frame_free(&frame);
        failure = error("creating DRM descriptor AVBuffer failed", AVERROR(ENOMEM));
        return nullptr;
    }
    frame->colorspace = AVCOL_SPC_BT709;
    frame->color_primaries = AVCOL_PRI_BT709;
    frame->color_trc = AVCOL_TRC_IEC61966_2_1;
    frame->color_range = AVCOL_RANGE_JPEG;
    return frame;
}

int create_device_filter(AVFilterContext** context, const AVFilter* filter,
                         const char* name, const char* args, AVFilterGraph* graph,
                         AVBufferRef* device) {
    *context = avfilter_graph_alloc_filter(graph, filter, name);
    if (!*context) return AVERROR(ENOMEM);
    (*context)->hw_device_ctx = av_buffer_ref(device);
    if (!(*context)->hw_device_ctx) return AVERROR(ENOMEM);
    return avfilter_init_str(*context, args);
}

void destroy_filter_graph(AVFilterGraph*& graph) {
    avfilter_graph_free(&graph);
}

std::expected<void, GpuError> register_device_timing_callback(
    AVFilterGraph* graph, const EncoderConfig& config) {
    if (!config.device_timing_callback) return {};

    using SetCallback = void (*)(AVFilterGraph*, DeviceTimingCallback, void*);
    const auto setter = reinterpret_cast<SetCallback>(
        dlsym(RTLD_DEFAULT, "avfilter_graph_set_fluxrec_device_timing_callback"));
    if (!setter) {
        return std::unexpected(error(
            "device timing was requested, but the loaded private libavfilter lacks "
            "the Fluxrec timing callback; rebuild the private FFmpeg runtime"));
    }

    setter(graph, config.device_timing_callback, config.device_timing_opaque);
    return {};
}

std::expected<void, GpuError> register_nvenc_api_timing_callback(
    AVCodecContext* codec_context, const EncoderConfig& config) {
    if (!config.nvenc_api_timing_callback) return {};

    using SetCallback = void (*)(AVCodecContext*, NvencApiTimingCallback, void*);
    const auto setter = reinterpret_cast<SetCallback>(
        dlsym(RTLD_DEFAULT, "avcodec_set_fluxrec_nvenc_api_timing_callback"));
    if (!setter) {
        return std::unexpected(error(
            "NVENC API timing was requested, but the loaded private libavcodec lacks "
            "the Fluxrec API timing callback; build the instrumented FFmpeg runtime"));
    }

    setter(codec_context, config.nvenc_api_timing_callback,
           config.nvenc_api_timing_opaque);
    return {};
}

} // namespace

struct GpuEncoder::Impl {
    struct PendingFrame {
        capture::FrameLease lease;
        AVFrame* imported = nullptr;
        AVFrame* converted = nullptr;
        AVFrame* nvenc_source = nullptr;
        AVFrame* nvenc_frame = nullptr;
        detail::DirectFrameResources direct_resources;
        detail::Nv12ColorCorrectionResources color_resources;
        std::chrono::steady_clock::time_point submitted_at{};
        std::uint64_t sequence = 0;
        std::uint64_t presentation_time_ns = 0;
        bool has_presentation_time = false;
    };

    EncoderConfig config;
    GpuInfo gpu_info;
    capture::UniqueFd drm_fd;
    std::unique_ptr<gbm_device, decltype(&gbm_device_destroy)> gbm{nullptr, gbm_device_destroy};
    std::vector<GbmSlot> slots;
    std::vector<capture::DmabufImage> public_images;
    AVBufferRef* vulkan_device = nullptr;
    AVBufferRef* cuda_device = nullptr;
    AVBufferRef* cuda_frames = nullptr;
    AVBufferRef* input_frames = nullptr;
    InputFramesExecPoolDrain input_frames_exec_pool_drain = nullptr;
    AVBufferRef* direct_output_frames = nullptr;
    std::optional<FrameIdentity> frame_identity;
    std::optional<detail::DmabufBlackProbe> dmabuf_black_probe;
    std::uint64_t frame_identity_skipped_frames = 0;
    std::map<std::uint64_t, std::int64_t> frame_identity_pts;
    std::optional<detail::DirectNv12Converter> direct_converter;
    std::optional<detail::Nv12ColorCorrector> nv12_color_corrector;
    std::optional<PreviewProducer> preview_producer;
    std::string preview_setup_error;
    AVFilterGraph* filter_graph = nullptr;
    AVFilterContext* filter_source = nullptr;
    AVFilterContext* filter_sink = nullptr;
    AVCodecContext* codec_context = nullptr;
    const AVCodec* codec = nullptr;
    std::map<std::int64_t, PendingFrame> pending_frames;
    EncoderStats counters;
    std::int64_t previous_pts = AV_NOPTS_VALUE;
    std::optional<std::uint64_t> pts_origin_ns;
    bool drained = false;
    bool direct_conversion = false;
    bool nvenc_backend = false;
    bool direct_cuda_egl_backend = false;
    bool dmabuf_sync_probe = false;
    bool dmabuf_sync_wait = false;
    std::uint32_t preimport_delay_us = 0;
    bool color_correct_nv12 = false;
    bool rgb_blackdetect_before_scale = false;
    bool nv12_blackdetect_after_first = false;
    bool nv12_blackdetect_after_second = false;
    std::string extradata;
    std::optional<DirectCudaEglEncoder> direct_cuda_egl;

    void probe_dmabuf_sync_file(const capture::CapturedFrame& captured) const noexcept {
        if (!dmabuf_sync_probe || !captured.lease) return;
        // The known direct-path corruption is at sequence 9, 18, ... . Sample
        // both that phase and its immediately preceding good phase. EXPORT is
        // observational: it does not consume the dma-resv fences, and FFmpeg
        // remains responsible for its normal sync-file import in av_hwframe_map.
        const auto phase = captured.sequence % 9U;
        if (phase != 0U && phase != 8U) return;
        const auto& image = captured.lease.image();
        if (image.planes.empty() || image.planes.front().fd < 0) return;

        dma_buf_export_sync_file exported{};
        exported.flags = DMA_BUF_SYNC_READ;
        exported.fd = -1;
        int ioctl_result;
        do {
            ioctl_result = ::ioctl(image.planes.front().fd,
                                   DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &exported);
        } while (ioctl_result < 0 && errno == EINTR);
        const int ioctl_error = ioctl_result < 0 ? errno : 0;

        int poll_result = -1;
        int poll_error = 0;
        short poll_events = 0;
        if (ioctl_result == 0 && exported.fd >= 0) {
            pollfd descriptor{};
            descriptor.fd = exported.fd;
            descriptor.events = POLLIN;
            do {
                poll_result = ::poll(&descriptor, 1, 0);
            } while (poll_result < 0 && errno == EINTR);
            poll_error = poll_result < 0 ? errno : 0;
            poll_events = descriptor.revents;
            ::close(exported.fd);
        }

        std::fprintf(stderr,
                     "{\"type\":\"dmabuf_sync_probe\",\"capture_sequence\":%llu,\"slot\":%zu,\"dma_buf_fd\":%d,\"ioctl_result\":%d,\"ioctl_errno\":%d,\"poll_result\":%d,\"poll_errno\":%d,\"poll_revents\":%d,\"signaled\":%s}\n",
                     static_cast<unsigned long long>(captured.sequence),
                     captured.lease.slot_index(), image.planes.front().fd,
                     ioctl_result, ioctl_error, poll_result, poll_error,
                     static_cast<int>(poll_events),
                     ioctl_result == 0 && poll_result == 1 &&
                         (poll_events & (POLLIN | POLLERR | POLLHUP)) != 0
                         ? "true" : "false");
    }

    void release_pending_frame(PendingFrame& pending) noexcept {
        if (direct_converter && pending.imported && pending.converted) {
            direct_converter->release(pending.direct_resources, pending.imported,
                                      pending.converted);
        }
        if (nv12_color_corrector && pending.imported && pending.converted) {
            nv12_color_corrector->release(pending.color_resources, pending.imported,
                                          pending.converted);
        }
        av_frame_free(&pending.converted);
        av_frame_free(&pending.imported);
        av_frame_free(&pending.nvenc_frame);
        av_frame_free(&pending.nvenc_source);
        pending.lease.release_after_gpu();
    }

    void release_all_pending_frames() noexcept {
        for (auto& [pts, pending] : pending_frames) {
            (void)pts;
            release_pending_frame(pending);
        }
        pending_frames.clear();
        counters.pending_capture_leases = 0;
    }

    void reset_last_pipeline_timings() noexcept {
        counters.last_dmabuf_import_cpu_ns = 0;
        counters.last_cuda_transfer_cpu_ns = 0;
        counters.last_filter_submit_cpu_ns = 0;
        counters.last_filter_pull_cpu_ns = 0;
        counters.last_encoder_send_cpu_ns = 0;
        counters.last_encoder_receive_cpu_ns = 0;
        counters.last_submit_to_packet_wall_ns = 0;
    }

    void initialize_preview() {
        if (config.preview_socket_path.empty()) return;
        PreviewProducerConfig preview_config;
        preview_config.socket_path = config.preview_socket_path;
        preview_config.source_width = config.output_width;
        preview_config.source_height = config.output_height;
        auto connected = PreviewProducer::connect(vulkan_device, std::move(preview_config));
        if (!connected) {
            preview_setup_error = connected.error().message;
            av_log(nullptr, AV_LOG_WARNING,
                   "optional GPU preview unavailable: %s\n", preview_setup_error.c_str());
            return;
        }
        preview_producer.emplace(std::move(*connected));
        av_log(nullptr, AV_LOG_INFO,
               "GPU preview connected: 960x540 RGBA DMA-BUF, three slots, up to 60 FPS\n");
    }

    void submit_preview(AVFrame* frame, std::uint64_t sequence,
                        std::uint64_t presentation_time_ns,
                        bool has_presentation_time) noexcept {
        if (!preview_producer) return;
        preview_producer->submit(frame, sequence, presentation_time_ns,
                                 has_presentation_time);
    }

    int send_encoder_frame(AVFrame* frame) {
        ScopedCpuTiming timing{counters.last_encoder_send_cpu_ns,
                               counters.total_encoder_send_cpu_ns};
        return avcodec_send_frame(codec_context, frame);
    }

    int drain_input_frames_exec_pool(std::string_view stage) noexcept {
        if (!input_frames_exec_pool_drain || !input_frames) return 0;

        log_input_frames_ref_audit("before_compute_exec_pool_drain", input_frames);
        const int result = input_frames_exec_pool_drain(
            input_frames, kInputFramesExecPoolDrainTimeoutNs);
        if (result < 0) {
            av_log(nullptr, AV_LOG_ERROR,
                   "FLUXREC_GPU_INPUT_CTX_DRAIN stage=%.*s failed: %d\n",
                   static_cast<int>(stage.size()), stage.data(), result);
        } else {
            av_log(nullptr, AV_LOG_INFO,
                   "FLUXREC_GPU_INPUT_CTX_DRAIN stage=%.*s completed\n",
                   static_cast<int>(stage.size()), stage.data());
        }
        log_input_frames_ref_audit("after_compute_exec_pool_drain", input_frames);
        return result;
    }

    ~Impl() {
        direct_cuda_egl.reset();
        if (input_frames_exec_pool_drain && input_frames) {
            const int drain_result =
                drain_input_frames_exec_pool("destructor_retry");
            if (drain_result < 0) {
                av_log(nullptr, AV_LOG_ERROR,
                       "FLUXREC_GPU_INPUT_CTX_DRAIN could not prove Vulkan input work complete; exiting without C++ resource teardown\n");
                std::fflush(nullptr);
                std::_Exit(EXIT_FAILURE);
            }
        }
        log_input_frames_ref_audit("before_codec_free", input_frames);
        if (codec_context) {
            avcodec_free_context(&codec_context);
        }
        log_input_frames_ref_audit("after_codec_free", input_frames);
        log_input_frames_ref_audit("before_filter_graph_free", input_frames);
        destroy_filter_graph(filter_graph);
        log_input_frames_ref_audit("after_filter_graph_free", input_frames);
        log_input_frames_ref_audit("before_pending_frames_release", input_frames);
        release_all_pending_frames();
        log_input_frames_ref_audit("after_pending_frames_release", input_frames);
        log_input_frames_ref_audit("before_auxiliary_gpu_state_reset", input_frames);
        preview_producer.reset();
        direct_converter.reset();
        nv12_color_corrector.reset();
        log_input_frames_ref_audit("before_frame_identity_reset", input_frames);
        frame_identity.reset();
        log_input_frames_ref_audit("after_frame_identity_reset", input_frames);
        if (dmabuf_black_probe) {
            dmabuf_black_probe->finish();
            dmabuf_black_probe.reset();
        }
        av_buffer_unref(&direct_output_frames);
        av_buffer_unref(&cuda_frames);
        av_buffer_unref(&cuda_device);
        log_input_frames_ref_audit("before_input_frames_unref", input_frames);
        av_buffer_unref(&input_frames);
        log_input_frames_ref_audit("after_input_frames_unref", input_frames);
        av_buffer_unref(&vulkan_device);
    }

    std::expected<void, GpuError> initialize(const capture::CaptureRequirements& requirements) {
        const char* input_context_drain_env =
            std::getenv("FLUXREC_GPU_INPUT_CTX_DRAIN");
        const bool input_context_drain_enabled = input_context_drain_env &&
            std::strcmp(input_context_drain_env, "1") == 0;
        if (input_context_drain_env && input_context_drain_env[0] != '\0' &&
            std::strcmp(input_context_drain_env, "0") != 0 &&
            !input_context_drain_enabled) {
            return std::unexpected(error(
                "FLUXREC_GPU_INPUT_CTX_DRAIN must be '0' or '1'"));
        }
        if (input_context_drain_enabled) {
            input_frames_exec_pool_drain = resolve_input_frames_exec_pool_drain();
            if (!input_frames_exec_pool_drain) {
                return std::unexpected(error(
                    "FLUXREC_GPU_INPUT_CTX_DRAIN=1 requires "
                    "av_vk_frame_ctx_exec_pool_drain in the loaded private libavutil"));
            }
            av_log(nullptr, AV_LOG_WARNING,
                   "WARNING: FLUXREC_GPU_INPUT_CTX_DRAIN=1 is an opt-in private FFmpeg teardown diagnostic. It polls the input Vulkan pool with a two-second budget; if completion remains unproven during the destructor retry, the process exits without resource unwinding.\n");
        }
        auto selected_backend = selected_encoder_backend();
        if (!selected_backend) return std::unexpected(selected_backend.error());
        nvenc_backend = *selected_backend != EncoderBackend::vulkan;
        direct_cuda_egl_backend =
            *selected_backend == EncoderBackend::experimental_cuda_egl;
        if (direct_cuda_egl_backend) {
            if (config.codec != Codec::h264 && config.codec != Codec::hevc) {
                return std::unexpected(error(
                    "experimental-cuda-egl supports H.264 and HEVC"));
            }
            if (requirements.width != 2560U || requirements.height != 1440U ||
                config.b_frames != 0U ||
                (config.output_width != 0U && config.output_width != 2560U) ||
                (config.output_height != 0U && config.output_height != 1440U) ||
                config.verify_frame_ids ||
                !config.preview_socket_path.empty() || config.capture_pool_size < 3U ||
                config.capture_pool_size > 16U || config.async_depth < 1 ||
                config.async_depth > 16 ||
                config.conversion_mode == ConversionMode::direct_compute ||
                config.device_timing_callback != nullptr ||
                config.nvenc_api_timing_callback != nullptr) {
                return std::unexpected(error(
                    "experimental-cuda-egl supports only native 2560x1440 H.264 or HEVC, zero B-frames, bitrate or constant-QP rate control, a capture pool of 3–16 slots, async depth 1–16, and no scale conversion, frame-ID probe, preview, or FFmpeg timing callback"));
            }
            if (config.encoder_tune != EncoderTune::default_tuning &&
                config.encoder_tune != EncoderTune::ll) {
                return std::unexpected(error(
                    "experimental-cuda-egl only supports NVENC low-latency tuning"));
            }
            if (config.codec == Codec::h264 && config.h264_coder != H264Coder::vlc) {
                return std::unexpected(error(
                    "experimental-cuda-egl only supports H.264 CAVLC"));
            }
            if (config.qp) {
                if (*config.qp > 51U) {
                    return std::unexpected(error(
                        "experimental-cuda-egl constant QP must be between 0 and 51"));
                }
            } else if (config.bitrate <= 0 || config.bitrate > 500'000'000) {
                return std::unexpected(error(
                    "experimental-cuda-egl bitrate must be between 1 and 500 Mbps"));
            }
            for (const char* unsupported : {
                     "FLUXREC_GPU_INPUT_CTX_DRAIN",
                     "FLUXREC_GPU_DMABUF_SYNC_PROBE",
                     "FLUXREC_GPU_DMABUF_BLACK_PROBE",
                     "FLUXREC_GPU_PREIMPORT_DELAY_US",
                     "FLUXREC_GPU_BLACKDETECT_NV12",
                     "FLUXREC_GPU_BLACKDETECT_RGB_SOURCE",
                     "FLUXREC_GPU_COLOR_CORRECT_NV12",
                     "FLUXREC_GPU_RC_MODE"}) {
                if (environment_enabled(unsupported)) {
                    return std::unexpected(error(std::string(unsupported) +
                        " is not supported by experimental-cuda-egl"));
                }
            }
            if (environment_enabled("FLUXREC_NVENC_MAXRATE_MBPS")) {
                return std::unexpected(error(
                    "FLUXREC_NVENC_MAXRATE_MBPS is not supported by experimental-cuda-egl"));
            }
            if (const char* preset = std::getenv("FLUXREC_NVENC_PRESET");
                preset && *preset && std::strcmp(preset, "p1") != 0) {
                return std::unexpected(error(
                    "experimental-cuda-egl uses fixed NVENC preset p1"));
            }
            config.output_width = 2560U;
            config.output_height = 1440U;
        }
        if (config.b_frames > 4) {
            return std::unexpected(error("NVENC B-frame count must be between 0 and 4"));
        }
        if (config.b_frames > 0 && config.gop_size <= config.b_frames + 1) {
            return std::unexpected(error(
                "GOP size must be at least the B-frame count plus two"));
        }
        if (config.b_frames > 0 && !nvenc_backend) {
            return std::unexpected(error(
                "B-frames are currently supported only by the NVENC backend"));
        }
        if (nvenc_backend && !direct_cuda_egl_backend &&
            config.conversion_mode == ConversionMode::direct_compute) {
            return std::unexpected(error(
                "the NVENC backend currently requires the two-pass Vulkan conversion path"));
        }
        const char* sync_probe_env = std::getenv("FLUXREC_GPU_DMABUF_SYNC_PROBE");
        dmabuf_sync_probe = sync_probe_env && std::strcmp(sync_probe_env, "1") == 0;
        if (dmabuf_sync_probe) {
            std::fprintf(stderr,
                         "WARNING: FLUXREC_GPU_DMABUF_SYNC_PROBE=1 exports and polls read sync-files before selected DRM_PRIME imports; it does not map or read pixels.\n");
        }
        const char* sync_wait_env = std::getenv("FLUXREC_GPU_DMABUF_SYNC_WAIT");
        dmabuf_sync_wait = sync_wait_env && std::strcmp(sync_wait_env, "1") == 0;
        if (sync_wait_env && sync_wait_env[0] != '\0' &&
            std::strcmp(sync_wait_env, "0") != 0 && !dmabuf_sync_wait) {
            return std::unexpected(error(
                "FLUXREC_GPU_DMABUF_SYNC_WAIT must be '0' or '1'"));
        }
        if (dmabuf_sync_wait) {
            std::fprintf(stderr,
                "WARNING: FLUXREC_GPU_DMABUF_SYNC_WAIT=1 exports and waits up to 10 seconds for each leased DMA-BUF read sync-file before FFmpeg import; it does not submit Vulkan work or read pixels and serializes capture timing.\n");
        }
        const char* preimport_delay_env =
            std::getenv("FLUXREC_GPU_PREIMPORT_DELAY_US");
        if (preimport_delay_env) {
            const std::string_view value{preimport_delay_env};
            std::uint32_t delay_us = 0;
            const auto parsed = std::from_chars(value.data(),
                value.data() + value.size(), delay_us);
            if (value.empty() || parsed.ec != std::errc{} ||
                parsed.ptr != value.data() + value.size() || delay_us > 2000U) {
                return std::unexpected(error(
                    "FLUXREC_GPU_PREIMPORT_DELAY_US must be an integer from 0 through 2000"));
            }
            preimport_delay_us = delay_us;
        }
        if (preimport_delay_us > 0U) {
            std::fprintf(stderr,
                "WARNING: FLUXREC_GPU_PREIMPORT_DELAY_US=%u adds a per-frame host sleep after the producer-fence wait and before the optional raw probe/FFmpeg import; actual sleep can overshoot due to scheduler delay. This diagnostic serializes submission timing and is disabled by default.\n",
                preimport_delay_us);
        }
        const char* raw_black_probe_env =
            std::getenv("FLUXREC_GPU_DMABUF_BLACK_PROBE");
        const bool raw_black_probe_enabled =
            raw_black_probe_env && std::strcmp(raw_black_probe_env, "1") == 0;
        if (raw_black_probe_env && raw_black_probe_env[0] != '\0' &&
            std::strcmp(raw_black_probe_env, "0") != 0 && !raw_black_probe_enabled) {
            return std::unexpected(error(
                "FLUXREC_GPU_DMABUF_BLACK_PROBE must be '0' or '1'"));
        }
        if (raw_black_probe_enabled) {
            std::fprintf(stderr,
                "WARNING: FLUXREC_GPU_DMABUF_BLACK_PROBE=1 enables a separate raw DMA-BUF Vulkan import and per-frame GPU counter readback. It waits for the DMA-BUF writer sync-file and a Vulkan completion fence before FFmpeg import, so it serializes this diagnostic path and changes timing. It never reads source pixels on the CPU. If the completion-fence wait fails or times out after submit, the process exits without unwinding to preserve in-flight lease and Vulkan object lifetimes.\n");
        }
        const char* blackdetect_env = std::getenv("FLUXREC_GPU_BLACKDETECT_NV12");
        if (blackdetect_env && std::strcmp(blackdetect_env, "1") == 0) {
            nv12_blackdetect_after_second = true;
        } else if (blackdetect_env && std::strcmp(blackdetect_env, "first") == 0) {
            nv12_blackdetect_after_first = true;
        } else if (blackdetect_env && std::strcmp(blackdetect_env, "second") == 0) {
            nv12_blackdetect_after_second = true;
        } else if (blackdetect_env && std::strcmp(blackdetect_env, "both") == 0) {
            nv12_blackdetect_after_first = true;
            nv12_blackdetect_after_second = true;
        } else if (blackdetect_env && blackdetect_env[0] != '\0' &&
                   std::strcmp(blackdetect_env, "0") != 0) {
            return std::unexpected(error(
                "FLUXREC_GPU_BLACKDETECT_NV12 must be '0', '1', 'first', 'second', or 'both'"));
        }
        if (nv12_blackdetect_after_first || nv12_blackdetect_after_second) {
            std::fprintf(stderr,
                         "WARNING: FLUXREC_GPU_BLACKDETECT_NV12=%s inserts FFmpeg blackdetect_vulkan after the selected two-pass NV12 stage(s); it inspects pixels on-GPU and does not map raw pixels to CPU.\n",
                         blackdetect_env);
        }
        const char* rgb_blackdetect_env =
            std::getenv("FLUXREC_GPU_BLACKDETECT_RGB_SOURCE");
        if (rgb_blackdetect_env && std::strcmp(rgb_blackdetect_env, "1") == 0) {
            rgb_blackdetect_before_scale = true;
        } else if (rgb_blackdetect_env && rgb_blackdetect_env[0] != '\0' &&
                   std::strcmp(rgb_blackdetect_env, "0") != 0) {
            return std::unexpected(error(
                "FLUXREC_GPU_BLACKDETECT_RGB_SOURCE must be '0' or '1'"));
        }
        if (rgb_blackdetect_before_scale) {
            std::fprintf(stderr,
                         "WARNING: FLUXREC_GPU_BLACKDETECT_RGB_SOURCE=1 inserts blackdetect_vulkan before the first scale pass; it inspects imported RGB pixels on-GPU and reads back only aggregate counters.\n");
            if (config.conversion_mode == ConversionMode::direct_compute) {
                return std::unexpected(error(
                    "RGB source blackdetect requires the two-pass Vulkan filter graph"));
            }
        }
        const char* color_correct_env = std::getenv("FLUXREC_GPU_COLOR_CORRECT_NV12");
        // The extra GPU pass is still experimental: it corrects the AR24
        // channel interpretation, but an out-of-place encode pool regressed
        // long captures. Keep the established filter-sink path as default.
        color_correct_nv12 = false;
        if (color_correct_env && std::strcmp(color_correct_env, "0") == 0) {
            color_correct_nv12 = false;
        } else if (color_correct_env && std::strcmp(color_correct_env, "1") == 0) {
            color_correct_nv12 = true;
        } else if (color_correct_env && color_correct_env[0] != '\0') {
            return std::unexpected(error("FLUXREC_GPU_COLOR_CORRECT_NV12 must be '0' or '1'"));
        }
        if (color_correct_nv12) {
            std::fprintf(stderr,
                         "WARNING: FLUXREC_GPU_COLOR_CORRECT_NV12=1 enables experimental in-place limited-range BT.709 NV12 red/blue correction on the two-pass Vulkan path.\n");
            if (config.conversion_mode == ConversionMode::direct_compute) {
                return std::unexpected(error("NV12 color correction is available only with the two-pass Vulkan conversion path"));
            }
        }
        if (requirements.width <= 2 || requirements.height <= 2 ||
            (requirements.width & 1U) != 0 || (requirements.height & 1U) != 0) {
            return std::unexpected(error("capture dimensions must be even and at least 4 pixels"));
        }
        if (config.qp && *config.qp > 51) {
            return std::unexpected(error(
                "constant-QP value must be between 0 and 51 for the current 8-bit video path"));
        }
        if (config.output_width == 0) config.output_width = requirements.width;
        if (config.output_height == 0) config.output_height = requirements.height;
        if (config.output_width == 0 || config.output_height == 0 ||
            (config.output_width & 1U) != 0 || (config.output_height & 1U) != 0 ||
            config.fps_num == 0 || config.fps_den == 0 || config.capture_pool_size < 2 ||
            config.capture_pool_size > static_cast<std::size_t>(INT_MAX)) {
            return std::unexpected(error("invalid encoder dimensions, rate, or pool size"));
        }
        if (!requirements.dmabuf_device) {
            return std::unexpected(error("capture did not provide its DMA-BUF device identity"));
        }
        const auto path = drm_node_path(*requirements.dmabuf_device);
        if (!path) {
            return std::unexpected(error("capture DMA-BUF device is not present in /dev/dri"));
        }
        struct stat drm_stat {};
        if (::stat(path->c_str(), &drm_stat) < 0) {
            return std::unexpected(error("stat on capture render node failed", AVERROR(errno)));
        }
        drm_fd.reset(::open(path->c_str(), O_RDWR | O_CLOEXEC));
        if (!drm_fd) {
            return std::unexpected(error("opening capture render node " + *path + " failed",
                                         AVERROR(errno)));
        }
        gbm.reset(gbm_create_device(drm_fd.get()));
        if (!gbm) {
            return std::unexpected(error("creating GBM allocator for " + *path + " failed"));
        }

        // The direct DMA-BUF/EGL/CUDA/NVENC path does not use Vulkan. Skip
        // FFmpeg Vulkan device creation so a Vulkan init failure cannot block
        // it and so startup does not pay for an unused device.
        if (!direct_cuda_egl_backend) {
        // FFmpeg enumerates Vulkan devices by index. Select the index whose
        // DRM render-node identity exactly matches the capture allocator.
        for (unsigned index = 0; index < 16; ++index) {
            const std::string index_text = std::to_string(index);
            AVBufferRef* candidate = nullptr;
            AVDictionary* vulkan_options = nullptr;
            if (nvenc_backend) {
                const int option_result = av_dict_set(
                    &vulkan_options, "disable_multiplane", "1", 0);
                if (option_result < 0) {
                    av_dict_free(&vulkan_options);
                    return std::unexpected(error(
                        "setting Vulkan separate-plane NV12 allocation for CUDA interop failed",
                        option_result));
                }
            }
            const int result = av_hwdevice_ctx_create(&candidate, AV_HWDEVICE_TYPE_VULKAN,
                                                       index_text.c_str(), vulkan_options, 0);
            av_dict_free(&vulkan_options);
            if (result < 0) {
                if (index == 0) {
                    return std::unexpected(error("creating FFmpeg Vulkan device failed", result));
                }
                break;
            }
            auto* device_context = reinterpret_cast<AVHWDeviceContext*>(candidate->data);
            auto* vulkan_context = static_cast<AVVulkanDeviceContext*>(device_context->hwctx);
            if (device_matches_render_node(vulkan_context->phys_dev, drm_stat)) {
                vulkan_device = candidate;
                VkPhysicalDeviceProperties properties{};
                vkGetPhysicalDeviceProperties(vulkan_context->phys_dev, &properties);
                gpu_info.device_name = properties.deviceName;
                break;
            }
            av_buffer_unref(&candidate);
        }
        if (!vulkan_device) {
            return std::unexpected(error("no FFmpeg Vulkan physical device reports the capture DMA-BUF render node; refusing cross-device or CPU fallback"));
        }
        }

        if (nvenc_backend && !direct_cuda_egl_backend) {
            const char* codec_name = config.codec == Codec::h264
                ? "h264_nvenc" : "hevc_nvenc";
            if (!avcodec_find_encoder_by_name(codec_name)) {
                return std::unexpected(error(std::string("FFmpeg encoder is unavailable: ") +
                                             codec_name));
            }
            const int derive_result = av_hwdevice_ctx_create_derived(
                &cuda_device, AV_HWDEVICE_TYPE_CUDA, vulkan_device, 0);
            if (derive_result < 0) {
                std::string message = "creating a CUDA device from the selected Vulkan UUID failed";
                if (derive_result == AVERROR(ENOSYS)) {
                    message += "; the active FFmpeg libavutil must be built with CUDA hardware-frame support (CONFIG_CUDA=1)";
                }
                return std::unexpected(error(std::move(message), derive_result));
            }
            cuda_frames = av_hwframe_ctx_alloc(cuda_device);
            if (!cuda_frames) {
                return std::unexpected(error(
                    "allocating the independent CUDA NV12 encoder frames pool failed",
                    AVERROR(ENOMEM)));
            }
            auto* cuda_context = reinterpret_cast<AVHWFramesContext*>(cuda_frames->data);
            cuda_context->format = AV_PIX_FMT_CUDA;
            cuda_context->sw_format = AV_PIX_FMT_NV12;
            cuda_context->width = static_cast<int>(config.output_width);
            cuda_context->height = static_cast<int>(config.output_height);
            cuda_context->initial_pool_size = static_cast<int>(std::min<std::size_t>(
                config.capture_pool_size +
                    static_cast<std::size_t>(std::max(config.async_depth, 0)) + 8U,
                static_cast<std::size_t>(INT_MAX)));
            const int init_result = av_hwframe_ctx_init(cuda_frames);
            if (init_result < 0) {
                return std::unexpected(error(
                    "initializing the independent CUDA NV12 encoder frames pool failed",
                    init_result));
            }
            av_log(nullptr, AV_LOG_INFO,
                   "NVENC selected: Vulkan device UUID matched to CUDA; independent CUDA NV12 pool has %d initial frames\n",
                   cuda_context->initial_pool_size);
        }

        bool allocated = false;
        const std::array<std::uint32_t, 2> prefer_fourcc{DRM_FORMAT_ARGB8888,
                                                          DRM_FORMAT_XRGB8888};
        for (const std::uint32_t wanted_fourcc : prefer_fourcc) {
            for (const auto& pair : requirements.dmabuf_formats) {
                if (pair.fourcc != wanted_fourcc) continue;
                auto first = make_gbm_slot(gbm.get(), requirements, pair.fourcc, pair.modifier);
                if (!first) continue;
                std::vector<GbmSlot> candidate_slots;
                candidate_slots.reserve(config.capture_pool_size);
                candidate_slots.push_back(std::move(*first));
                while (candidate_slots.size() < config.capture_pool_size) {
                    auto slot = make_gbm_slot(gbm.get(), requirements, pair.fourcc, pair.modifier);
                    if (!slot) break;
                    candidate_slots.push_back(std::move(*slot));
                }
                if (candidate_slots.size() != config.capture_pool_size) continue;
                slots = std::move(candidate_slots);
                gpu_info.capture_fourcc = pair.fourcc;
                gpu_info.capture_modifier = pair.modifier;
                gpu_info.capture_plane_count = 1;
                allocated = true;
                break;
            }
            if (allocated) break;
        }
        if (!allocated) {
            return std::unexpected(error("GBM could not allocate the requested capture pool with an exact advertised AR24/XR24 modifier"));
        }
        public_images.reserve(slots.size());
        for (const auto& slot : slots) public_images.push_back(slot.image);

        if (direct_cuda_egl_backend) {
            DirectCudaEglConfig direct_config;
            direct_config.codec = config.codec;
            direct_config.width = config.output_width;
            direct_config.height = config.output_height;
            direct_config.fps_num = config.fps_num;
            direct_config.fps_den = config.fps_den;
            direct_config.gop_size = config.gop_size;
            direct_config.bitrate = config.bitrate;
            direct_config.qp = config.qp;
            direct_config.pool_size = public_images.size();
            direct_config.async_depth = config.async_depth;
            auto direct = DirectCudaEglEncoder::create(*path, public_images,
                                                        direct_config);
            if (!direct) return std::unexpected(direct.error());
            direct_cuda_egl.emplace(std::move(*direct));
            gpu_info.device_name = direct_cuda_egl->cuda_device_name();
            gpu_info.cpu_pixel_fallback_enabled = false;
            gpu_info.conversion_path =
                "experimental DMA-BUF -> EGLImage -> CUDA EGL array -> NVENC CUDAARRAY; no scaling; desktop support is empirical and NVIDIA documents this CUDA EGL interface for Tegra";
            const std::string rate_control = config.qp
                ? "constant-QP " + std::to_string(*config.qp)
                : "VBR";
            av_log(nullptr, AV_LOG_WARNING,
                   "EXPERIMENTAL direct CUDA EGL encoder initialized for native 2560x1440 %s %s on %s; FFmpeg Vulkan device was not created; CUDA EGL desktop support is outside NVIDIA's documented Tegra support scope\n",
                   config.codec == Codec::hevc ? "HEVC" : "H.264",
                   rate_control.c_str(),
                   gpu_info.device_name.empty() ? "an unnamed CUDA device" : gpu_info.device_name.c_str());
            return {};
        }

        const AVPixelFormat sw_format = gpu_info.capture_fourcc == DRM_FORMAT_ARGB8888
            ? AV_PIX_FMT_BGRA : AV_PIX_FMT_BGR0;
        input_frames = av_hwframe_ctx_alloc(vulkan_device);
        if (!input_frames) {
            return std::unexpected(error("allocating imported Vulkan frame context failed", AVERROR(ENOMEM)));
        }
        log_input_frames_ref_audit("after_input_frames_alloc", input_frames);
        auto* input_context = reinterpret_cast<AVHWFramesContext*>(input_frames->data);
        input_context->format = AV_PIX_FMT_VULKAN;
        input_context->sw_format = sw_format;
        input_context->width = static_cast<int>(requirements.width);
        input_context->height = static_cast<int>(requirements.height);
        input_context->initial_pool_size = 2;
        int result = av_hwframe_ctx_init(input_frames);
        if (result < 0) {
            return std::unexpected(error("initializing imported Vulkan frame context failed", result));
        }
        log_input_frames_ref_audit("after_input_frames_init", input_frames);

        if (raw_black_probe_enabled) {
#ifdef FLUXREC_GPU_DMABUF_BLACK_SHADER_PATH
            auto probe = detail::DmabufBlackProbe::create(
                vulkan_device, public_images, requirements.width, requirements.height,
                gpu_info.capture_fourcc, gpu_info.capture_modifier,
                FLUXREC_GPU_DMABUF_BLACK_SHADER_PATH);
            if (!probe) {
                return std::unexpected(error(
                    "initializing independent raw DMA-BUF black probe failed: " +
                    probe.error().message, probe.error().code));
            }
            dmabuf_black_probe.emplace(std::move(*probe));
#else
            return std::unexpected(error(
                "raw DMA-BUF black probe was enabled, but its shader path was not configured"));
#endif
        }

        if (config.verify_frame_ids) {
            FrameIdentityOptions identity_options;
            identity_options.width = requirements.width;
            identity_options.height = requirements.height;
            identity_options.ring_size = 8;
            identity_options.measure_gpu_time = true;
            identity_options.mode = config.frame_identity_mode;
            identity_options.unrelated_iterations = config.frame_identity_unrelated_iterations;
#ifdef FLUXREC_FRAME_IDENTITY_SHADER_DIR
            identity_options.shader_directory = FLUXREC_FRAME_IDENTITY_SHADER_DIR;
#endif
            auto identity = FrameIdentity::create(vulkan_device, std::move(identity_options));
            if (!identity) {
                return std::unexpected(error("initializing frame identity failed: " +
                                             identity.error().message,
                                             identity.error().code));
            }
            frame_identity.emplace(std::move(*identity));
        }

        // Import a GPU-allocated pool image before capture starts. DIRECT makes
        // any software staging/copy mapping an error instead of a fallback.
        GpuError import_error;
        AVFrame* drm_frame = make_drm_frame(slots.front().image, import_error);
        if (!drm_frame) return std::unexpected(std::move(import_error));
        AVFrame* imported = av_frame_alloc();
        if (!imported) {
            av_frame_free(&drm_frame);
            return std::unexpected(error("allocating Vulkan import probe frame failed", AVERROR(ENOMEM)));
        }
        imported->format = AV_PIX_FMT_VULKAN;
        imported->width = static_cast<int>(requirements.width);
        imported->height = static_cast<int>(requirements.height);
        imported->hw_frames_ctx = av_buffer_ref(input_frames);
        if (!imported->hw_frames_ctx) {
            av_frame_free(&imported);
            av_frame_free(&drm_frame);
            return std::unexpected(error("referencing Vulkan import frame context failed", AVERROR(ENOMEM)));
        }
        result = av_hwframe_map(imported, drm_frame,
                                AV_HWFRAME_MAP_READ | AV_HWFRAME_MAP_DIRECT);
        av_frame_free(&drm_frame);
        av_frame_free(&imported);
        if (result < 0) {
            return std::unexpected(error("DRM_PRIME→Vulkan direct DMA-BUF import failed for selected GBM modifier", result));
        }
        gpu_info.drm_prime_import_proven = true;
        gpu_info.cpu_pixel_fallback_enabled = false;
        // The direct native-size path is still experimental: async-depth
        // captures can produce periodic black frames even though the DMA-BUF
        // import and compute dispatch succeed. Keep automatic selection on
        // the visually validated two-pass path until direct mode passes the
        // frame-identity tests.
        if (!rgb_blackdetect_before_scale &&
            config.conversion_mode == ConversionMode::direct_compute &&
            config.output_width == requirements.width && config.output_height == requirements.height) {
            auto direct_result = initialize_direct_encoder(requirements);
            if (direct_result) {
                direct_conversion = true;
                gpu_info.conversion_path = "experimental native-size Vulkan compute shader RGB-to-NV12; FFmpeg Vulkan Video encode";
                const char* codec_name = config.codec == Codec::h264 ? "h264_vulkan" : "hevc_vulkan";
                av_log(nullptr, AV_LOG_INFO,
                       "fluxrec GPU path: DRM_PRIME direct import -> native-size Vulkan compute NV12 -> %s (%s)\n",
                       codec_name, gpu_info.device_name.c_str());
                initialize_preview();
                return {};
            }
            if (config.conversion_mode == ConversionMode::direct_compute) {
                return std::unexpected(direct_result.error());
            }
            av_log(nullptr, AV_LOG_WARNING,
                   "native-size Vulkan compute path unavailable (%s); using two-pass Vulkan fallback\n",
                   direct_result.error().message.c_str());
            clear_direct_state();
        } else if (config.conversion_mode == ConversionMode::direct_compute) {
            return std::unexpected(error(
                rgb_blackdetect_before_scale
                    ? "RGB source blackdetect requires the two-pass Vulkan filter graph"
                    : "direct compute conversion requires output dimensions to match the capture dimensions"));
        }

        gpu_info.conversion_path = color_correct_nv12
            ? "FFmpeg scale_vulkan two-pass NV12 + GPU-only limited-range BT.709 red/blue correction; CPU pixel fallback disabled"
            : "FFmpeg scale_vulkan two-pass GPU workaround; CPU pixel fallback disabled";
        auto graph_result = initialize_filter_and_encoder(requirements);
        if (!graph_result) return graph_result;
        initialize_preview();
        return {};
    }

    void clear_direct_state() noexcept {
        if (codec_context) avcodec_free_context(&codec_context);
        direct_converter.reset();
        av_buffer_unref(&direct_output_frames);
    }

    std::expected<void, GpuError> open_nvenc_encoder(AVBufferRef* encoder_frames) {
        const char* codec_name = config.codec == Codec::h264
            ? "h264_nvenc" : "hevc_nvenc";
        const auto tune = ffmpeg_encoder_tune(config.encoder_tune);
        if (!tune) {
            return std::unexpected(error("invalid NVENC tuning mode in EncoderConfig"));
        }
        std::optional<std::string_view> nvenc_coder;
        if (config.codec == Codec::h264) {
            nvenc_coder = ffmpeg_nvenc_h264_coder(config.h264_coder);
            if (!nvenc_coder) {
                return std::unexpected(error(
                    "invalid H.264 entropy coder in EncoderConfig"));
            }
        } else if (config.h264_coder_explicit) {
            return std::unexpected(error(
                "H.264 entropy coder selection applies only to H.264"));
        }
        std::optional<std::int64_t> nvenc_max_rate_bps;
        if (const char* const max_rate_value =
                std::getenv("FLUXREC_NVENC_MAXRATE_MBPS")) {
            if (config.qp) {
                return std::unexpected(error(
                    "FLUXREC_NVENC_MAXRATE_MBPS cannot be used with constant-QP mode"));
            }
            const std::string_view value{max_rate_value};
            std::uint32_t max_rate_mbps = 0;
            const auto parsed = std::from_chars(value.data(),
                value.data() + value.size(), max_rate_mbps);
            if (value.empty() || parsed.ec != std::errc{} ||
                parsed.ptr != value.data() + value.size() ||
                max_rate_mbps < 1 || max_rate_mbps > 500) {
                return std::unexpected(error(
                    "FLUXREC_NVENC_MAXRATE_MBPS must be an integer from 1 through 500"));
            }
            nvenc_max_rate_bps = static_cast<std::int64_t>(max_rate_mbps) * 1'000'000;
            if (*nvenc_max_rate_bps < config.bitrate) {
                return std::unexpected(error(
                    "FLUXREC_NVENC_MAXRATE_MBPS must be at least the configured average bitrate"));
            }
        }
        const char* preset_value = std::getenv("FLUXREC_NVENC_PRESET");
        const std::string_view preset = preset_value && *preset_value
            ? std::string_view{preset_value} : std::string_view{"p1"};
        if (!valid_nvenc_preset(preset)) {
            return std::unexpected(error(
                "FLUXREC_NVENC_PRESET must be one of p1, p2, p3, p4, p5, p6, or p7"));
        }

        std::optional<VulkanRateControlMode> rate_control_override;
        if (!config.qp) {
            const char* const environment_value = std::getenv("FLUXREC_GPU_RC_MODE");
            const std::optional<std::string_view> value = environment_value
                ? std::optional<std::string_view>{environment_value}
                : std::nullopt;
            const auto parsed = parseVulkanRateControlModeOverride(value);
            if (!parsed) return std::unexpected(error(parsed.error()));
            rate_control_override = *parsed;
        }

        codec = avcodec_find_encoder_by_name(codec_name);
        if (!codec) {
            return std::unexpected(error(std::string("FFmpeg encoder is unavailable: ") +
                                         codec_name));
        }
        codec_context = avcodec_alloc_context3(codec);
        if (!codec_context) {
            return std::unexpected(error("allocating NVENC encoder context failed",
                                         AVERROR(ENOMEM)));
        }
        codec_context->width = static_cast<int>(config.output_width);
        codec_context->height = static_cast<int>(config.output_height);
        codec_context->pix_fmt = AV_PIX_FMT_CUDA;
        codec_context->time_base = kPacketTimeBase;
        codec_context->framerate = AVRational{static_cast<int>(config.fps_num),
                                               static_cast<int>(config.fps_den)};
        codec_context->bit_rate = config.qp ? 0 : config.bitrate;
        if (nvenc_max_rate_bps) {
            codec_context->rc_max_rate = *nvenc_max_rate_bps;
        }
        codec_context->gop_size = static_cast<int>(config.gop_size);
        codec_context->max_b_frames = static_cast<int>(config.b_frames);
        codec_context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        if (config.b_frames == 0) {
            codec_context->flags |= AV_CODEC_FLAG_LOW_DELAY;
        }
        codec_context->color_range = AVCOL_RANGE_MPEG;
        codec_context->colorspace = AVCOL_SPC_BT709;
        codec_context->color_primaries = AVCOL_PRI_BT709;
        codec_context->color_trc = AVCOL_TRC_BT709;
        codec_context->hw_device_ctx = av_buffer_ref(cuda_device);
        codec_context->hw_frames_ctx = av_buffer_ref(encoder_frames);
        if (!codec_context->hw_device_ctx || !codec_context->hw_frames_ctx) {
            return std::unexpected(error("referencing NVENC CUDA contexts failed",
                                         AVERROR(ENOMEM)));
        }

        const std::size_t surface_count = std::min<std::size_t>(
            std::max<std::size_t>(config.capture_pool_size +
                static_cast<std::size_t>(std::max(config.async_depth, 0)) + 8U, 8U),
            64U);
        const int nvenc_async_depth = std::clamp(
            config.async_depth, 1, static_cast<int>(surface_count) - 1);

        AVDictionary* options = nullptr;
        int option_result = av_dict_set(&options, "preset", preset.data(), 0);
        // FFmpeg's `qp` option multiplies by AVCodecContext i_quant_factor
        // (-0.8) and b_quant_offset (1.25). A requested QP 17 therefore
        // became I-frame QP 14, and those heavier IDRs stalled capture.
        // init_qpI/P/B write the same value into every picture type.
        // Ultra-low-latency plus one reference keeps that QP inside the
        // 1440p360 frame time; the validated bitrate path stays on ll.
        const bool constant_qp = config.qp.has_value();
        const std::string_view selected_tune = constant_qp && *tune == "default"
            ? std::string_view{"ull"}
            : (*tune == "default" ? std::string_view{"ll"} : *tune);
        if (option_result >= 0) {
            option_result = av_dict_set(&options, "tune", selected_tune.data(), 0);
        }
        if (option_result >= 0 && config.b_frames == 0) {
            option_result = av_dict_set(&options, "zerolatency", "1", 0);
        }
        if (option_result >= 0) {
            const std::string delay = std::to_string(nvenc_async_depth);
            option_result = av_dict_set(&options, "delay", delay.c_str(), 0);
        }
        if (option_result >= 0) {
            const std::string surfaces = std::to_string(surface_count);
            option_result = av_dict_set(&options, "surfaces", surfaces.c_str(), 0);
        }
        if (option_result >= 0 && nvenc_coder) {
            const std::string coder{*nvenc_coder};
            option_result = av_dict_set(&options, "coder", coder.c_str(), 0);
        }
        if (option_result >= 0 && constant_qp) {
            const std::string qp = std::to_string(*config.qp);
            option_result = av_dict_set(&options, "rc", "constqp", 0);
            if (option_result >= 0) {
                option_result = av_dict_set(&options, "init_qpI", qp.c_str(), 0);
            }
            if (option_result >= 0) {
                option_result = av_dict_set(&options, "init_qpP", qp.c_str(), 0);
            }
            if (option_result >= 0) {
                option_result = av_dict_set(&options, "init_qpB", qp.c_str(), 0);
            }
            if (option_result >= 0) {
                option_result = av_dict_set(&options, "refs", "1", 0);
            }
            if (option_result >= 0) {
                option_result = av_dict_set(&options, "dpb_size", "1", 0);
            }
            if (option_result >= 0) {
                option_result = av_dict_set(&options, "multipass", "disabled", 0);
            }
            if (option_result >= 0) {
                option_result = av_dict_set(&options, "rc-lookahead", "0", 0);
            }
            if (option_result >= 0 && config.codec == Codec::h264) {
                option_result = av_dict_set(&options, "level", "6.2", 0);
            }
        } else if (option_result >= 0 && rate_control_override &&
                   *rate_control_override != VulkanRateControlMode::automatic) {
            const std::string mode{toString(*rate_control_override)};
            option_result = av_dict_set(&options, "rc", mode.c_str(), 0);
        }
        if (option_result < 0) {
            av_dict_free(&options);
            return std::unexpected(error("setting NVENC encoder options failed",
                                         option_result));
        }

        const int result = avcodec_open2(codec_context, codec, &options);
        AVDictionaryEntry* unhandled = av_dict_get(options, nullptr, nullptr,
                                                    AV_DICT_IGNORE_SUFFIX);
        std::string unhandled_option;
        if (unhandled) unhandled_option = unhandled->key;
        av_dict_free(&options);
        if (result < 0) {
            return std::unexpected(error(std::string("opening ") + codec_name +
                                         " with CUDA NV12 frames failed", result));
        }
        if (!unhandled_option.empty()) {
            return std::unexpected(error(std::string(codec_name) +
                " did not accept requested option '" + unhandled_option + "'"));
        }
        if (codec_context->pix_fmt != AV_PIX_FMT_CUDA) {
            return std::unexpected(error("NVENC changed its input format away from AV_PIX_FMT_CUDA"));
        }
        if (config.b_frames > 0 && codec_context->has_b_frames <= 0) {
            return std::unexpected(error(std::string(codec_name) +
                " did not enable the requested NVENC B-frame reordering"));
        }
        if (auto registered = register_nvenc_api_timing_callback(codec_context, config);
            !registered) {
            return registered;
        }
        if (config.nvenc_api_timing_callback) {
            av_log(nullptr, AV_LOG_INFO,
                   "NVENC API timing enabled: host wall spans with input/output PTS correlation; not hardware execution time\n");
        }

        av_log(nullptr, AV_LOG_INFO,
               "NVENC encoder opened: backend=cuda preset=%.*s tune=%.*s surfaces=%zu delay=%d b_frames=%u has_b_frames=%d low_latency=%d rc=%s%s\n",
               static_cast<int>(preset.size()), preset.data(),
               static_cast<int>(selected_tune.size()), selected_tune.data(),
               surface_count, nvenc_async_depth, config.b_frames, codec_context->has_b_frames,
               config.b_frames == 0 ? 1 : 0,
               config.qp ? "constqp" : (rate_control_override &&
                   *rate_control_override != VulkanRateControlMode::automatic
                       ? toString(*rate_control_override).data() : "auto"),
               config.qp ? " (QP configured)" : "");
        if (nvenc_coder) {
            av_log(nullptr, AV_LOG_INFO,
                   "NVENC H.264 entropy coder: requested %.*s\n",
                   static_cast<int>(nvenc_coder->size()), nvenc_coder->data());
        }
        av_log(nullptr, AV_LOG_INFO, "NVENC encoder opened with %d extradata bytes\n",
               codec_context->extradata_size);
        if (nvenc_max_rate_bps) {
            av_log(nullptr, AV_LOG_INFO,
                   "NVENC diagnostic maximum bitrate: requested=%lld bps accepted=%lld bps VBV buffer=%d bits\n",
                   static_cast<long long>(*nvenc_max_rate_bps),
                   static_cast<long long>(codec_context->rc_max_rate),
                   codec_context->rc_buffer_size);
        }
        if (codec_context->extradata && codec_context->extradata_size > 0) {
            extradata.assign(reinterpret_cast<const char*>(codec_context->extradata),
                             static_cast<std::size_t>(codec_context->extradata_size));
        }
        return {};
    }

    std::expected<void, GpuError> open_encoder(AVBufferRef* encoder_frames) {
        if (nvenc_backend) return open_nvenc_encoder(encoder_frames);
        const char* codec_name = config.codec == Codec::h264 ? "h264_vulkan" : "hevc_vulkan";
        if (config.qp && *config.qp > 51) {
            return std::unexpected(error(
                "constant-QP value must be between 0 and 51 for the current 8-bit video path"));
        }
        const auto tune = ffmpeg_encoder_tune(config.encoder_tune);
        if (!tune) {
            return std::unexpected(error("invalid Vulkan encoder tuning mode in EncoderConfig"));
        }
        const auto h264_coder = ffmpeg_h264_coder(config.h264_coder);
        if (!h264_coder) {
            return std::unexpected(error("invalid H.264 entropy coder in EncoderConfig"));
        }
        std::optional<VulkanRateControlMode> rate_control_mode_override;
        if (!config.qp) {
            const char* const environment_value = std::getenv("FLUXREC_GPU_RC_MODE");
            const std::optional<std::string_view> value = environment_value
                ? std::optional<std::string_view>{environment_value}
                : std::nullopt;
            const auto parsed = parseVulkanRateControlModeOverride(value);
            if (!parsed) {
                return std::unexpected(error(parsed.error()));
            }
            rate_control_mode_override = *parsed;
        }
        codec = avcodec_find_encoder_by_name(codec_name);
        if (!codec) {
            return std::unexpected(error(std::string("FFmpeg encoder is unavailable: ") + codec_name));
        }
        codec_context = avcodec_alloc_context3(codec);
        if (!codec_context) {
            return std::unexpected(error("allocating Vulkan encoder context failed", AVERROR(ENOMEM)));
        }
        codec_context->width = static_cast<int>(config.output_width);
        codec_context->height = static_cast<int>(config.output_height);
        codec_context->pix_fmt = AV_PIX_FMT_VULKAN;
        codec_context->time_base = kPacketTimeBase;
        codec_context->framerate = AVRational{static_cast<int>(config.fps_num),
                                               static_cast<int>(config.fps_den)};
        codec_context->bit_rate = config.qp ? 0 : config.bitrate;
        codec_context->gop_size = static_cast<int>(config.gop_size);
        codec_context->max_b_frames = 0;
        codec_context->flags |= AV_CODEC_FLAG_LOW_DELAY | AV_CODEC_FLAG_GLOBAL_HEADER;
        codec_context->color_range = AVCOL_RANGE_MPEG;
        codec_context->colorspace = AVCOL_SPC_BT709;
        codec_context->color_primaries = AVCOL_PRI_BT709;
        codec_context->color_trc = AVCOL_TRC_BT709;
        codec_context->hw_device_ctx = av_buffer_ref(vulkan_device);
        codec_context->hw_frames_ctx = av_buffer_ref(encoder_frames);
        if (!codec_context->hw_device_ctx || !codec_context->hw_frames_ctx) {
            return std::unexpected(error("referencing encoder Vulkan contexts failed", AVERROR(ENOMEM)));
        }
        AVDictionary* options = nullptr;
        av_dict_set_int(&options, "async_depth", config.async_depth, 0);
        const bool has_cqp_options = config.qp.has_value();
        if (has_cqp_options) {
            int option_result = av_dict_set(&options, "rc_mode", "cqp", 0);
            if (option_result >= 0) {
                const std::string qp_value = std::to_string(*config.qp);
                option_result = av_dict_set(&options, "qp", qp_value.c_str(), 0);
            }
            if (option_result < 0) {
                av_dict_free(&options);
                return std::unexpected(error("setting Vulkan encoder CQP options failed",
                                             option_result));
            }
        }
        const bool has_rate_control_mode_override =
            rate_control_mode_override.has_value();
        if (has_rate_control_mode_override) {
            const std::string mode_value{toString(*rate_control_mode_override)};
            const int option_result = av_dict_set(
                &options, "rc_mode", mode_value.c_str(), 0);
            if (option_result < 0) {
                av_dict_free(&options);
                return std::unexpected(error(
                    "setting Vulkan encoder bitrate rate-control mode failed",
                    option_result));
            }
        }
        const bool has_h264_coder_option = config.codec == Codec::h264;
        if (has_h264_coder_option) {
            const std::string coder_value(*h264_coder);
            const int option_result = av_dict_set(&options, "coder", coder_value.c_str(), 0);
            if (option_result < 0) {
                av_dict_free(&options);
                return std::unexpected(error("setting H.264 Vulkan entropy coder option failed",
                                             option_result));
            }
        }
        const bool has_tune_override = *tune != "default";
        if (has_tune_override) {
            const std::string tune_value(*tune);
            const int option_result = av_dict_set(&options, "tune", tune_value.c_str(), 0);
            if (option_result < 0) {
                av_dict_free(&options);
                return std::unexpected(error("setting Vulkan encoder tune option failed", option_result));
            }
        }
        const int result = avcodec_open2(codec_context, codec, &options);
        const bool rate_control_mode_was_unhandled =
            has_rate_control_mode_override &&
            av_dict_get(options, "rc_mode", nullptr, 0) != nullptr;
        const bool cqp_rate_control_was_unhandled = has_cqp_options &&
            av_dict_get(options, "rc_mode", nullptr, 0) != nullptr;
        const bool cqp_value_was_unhandled = has_cqp_options &&
            av_dict_get(options, "qp", nullptr, 0) != nullptr;
        const bool h264_coder_was_unhandled = has_h264_coder_option &&
            av_dict_get(options, "coder", nullptr, 0) != nullptr;
        const bool tune_was_unhandled = has_tune_override &&
                                        av_dict_get(options, "tune", nullptr, 0) != nullptr;
        av_dict_free(&options);
        if (result < 0) {
            if (has_cqp_options) {
                std::string message = std::string("opening ") + codec_name +
                    " with constant-QP rate control at QP " +
                    std::to_string(*config.qp) +
                    " failed; the FFmpeg Vulkan encoder or driver may not support this "
                    "CQP configuration for the selected codec/profile";
                if (has_tune_override) {
                    message += " and tune '" + std::string(*tune) + "'";
                }
                return std::unexpected(error(std::move(message), result));
            }
            if (has_rate_control_mode_override) {
                return std::unexpected(error(
                    std::string("opening ") + codec_name +
                    " with requested bitrate rc_mode=" +
                    std::string(toString(*rate_control_mode_override)) +
                    " at target bitrate " + std::to_string(config.bitrate) +
                    " bps failed",
                    result));
            }
            if (has_h264_coder_option) {
                return std::unexpected(error(std::string("opening ") + codec_name +
                                             " with H.264 entropy coder '" +
                                             std::string(*h264_coder) + "' failed",
                                             result));
            }
            if (has_tune_override) {
                return std::unexpected(error(std::string("opening ") + codec_name +
                                             " with encoder tune '" + std::string(*tune) +
                                             "' failed; this FFmpeg Vulkan encoder or driver may not support the tune for this codec/profile",
                                             result));
            }
            return std::unexpected(error(std::string("opening ") + codec_name + " failed", result));
        }
        if (cqp_rate_control_was_unhandled || cqp_value_was_unhandled) {
            return std::unexpected(error(std::string(codec_name) +
                " did not accept the requested CQP options (rc_mode=cqp, qp=" +
                std::to_string(*config.qp) + ")"));
        }
        if (rate_control_mode_was_unhandled) {
            return std::unexpected(error(
                std::string(codec_name) +
                " did not accept FLUXREC_GPU_RC_MODE='" +
                std::string(toString(*rate_control_mode_override)) +
                "' at target bitrate " + std::to_string(config.bitrate) + " bps"));
        }
        if (h264_coder_was_unhandled) {
            return std::unexpected(error(std::string(codec_name) +
                                         " did not accept requested H.264 entropy coder '" +
                                         std::string(*h264_coder) + "'"));
        }
        if (tune_was_unhandled) {
            return std::unexpected(error(std::string(codec_name) +
                                         " did not accept requested encoder tune '" +
                                         std::string(*tune) + "'"));
        }
        if (codec_context->pix_fmt != AV_PIX_FMT_VULKAN) {
            return std::unexpected(error("encoder changed its input format away from AV_PIX_FMT_VULKAN"));
        }
        if (config.qp) {
            av_log(nullptr, AV_LOG_INFO,
                   "Vulkan encoder rate control: constant-QP (QP %u)\n", *config.qp);
        } else {
            const auto mode = rate_control_mode_override.value_or(
                VulkanRateControlMode::automatic);
            av_log(nullptr, AV_LOG_INFO,
                   "Vulkan encoder bitrate rate control: requested rc_mode=%.*s (%s), "
                   "target_bitrate=%lld bps\n",
                   static_cast<int>(toString(mode).size()), toString(mode).data(),
                   has_rate_control_mode_override ? "environment override"
                                                 : "FFmpeg default",
                   static_cast<long long>(config.bitrate));
        }
        av_log(nullptr, AV_LOG_INFO, "Vulkan encoder opened with %d extradata bytes\n",
               codec_context->extradata_size);
        if (codec_context->extradata && codec_context->extradata_size > 0) {
            extradata.assign(reinterpret_cast<const char*>(codec_context->extradata),
                             static_cast<std::size_t>(codec_context->extradata_size));
        }
        return {};
    }

    std::expected<void, GpuError> initialize_direct_encoder(
        const capture::CaptureRequirements& requirements) {
        direct_output_frames = av_hwframe_ctx_alloc(vulkan_device);
        if (!direct_output_frames) {
            return std::unexpected(error("allocating direct Vulkan NV12 frames context failed", AVERROR(ENOMEM)));
        }
        auto* frames = reinterpret_cast<AVHWFramesContext*>(direct_output_frames->data);
        frames->format = AV_PIX_FMT_VULKAN;
        frames->sw_format = AV_PIX_FMT_NV12;
        frames->width = static_cast<int>(config.output_width);
        frames->height = static_cast<int>(config.output_height);
        frames->initial_pool_size = static_cast<int>(std::min<std::size_t>(
            config.capture_pool_size + static_cast<std::size_t>(std::max(config.async_depth, 0)) + 8U,
            static_cast<std::size_t>(INT_MAX)));
        auto* vk_frames = static_cast<AVVulkanFramesContext*>(frames->hwctx);
        vk_frames->flags = AV_VK_FRAME_FLAG_NONE;
        vk_frames->usage = static_cast<VkImageUsageFlagBits>(
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR);
        int result = av_hwframe_ctx_init(direct_output_frames);
        if (result < 0) {
            return std::unexpected(error("initializing separate-plane Vulkan NV12 encoder frames failed", result));
        }
        auto* initialized_vk_frames = static_cast<AVVulkanFramesContext*>(frames->hwctx);
        av_log(nullptr, AV_LOG_INFO,
               "direct Vulkan NV12 image context: format[0]=%d format[1]=%d usage=0x%x flags=0x%llx\n",
               static_cast<int>(initialized_vk_frames->format[0]),
               static_cast<int>(initialized_vk_frames->format[1]),
               static_cast<unsigned int>(initialized_vk_frames->usage),
               static_cast<unsigned long long>(initialized_vk_frames->flags));

        auto converter = detail::DirectNv12Converter::create(
            input_frames, direct_output_frames, requirements.width, requirements.height,
            static_cast<std::uint32_t>(config.capture_pool_size +
                                       static_cast<std::size_t>(std::max(config.async_depth, 0)) + 8U));
        if (!converter) return std::unexpected(converter.error());
        direct_converter.emplace(std::move(*converter));
        auto encoder = open_encoder(direct_output_frames);
        if (!encoder) return encoder;
        return {};
    }

    std::expected<void, GpuError> initialize_filter_and_encoder(
        const capture::CaptureRequirements& requirements) {
        filter_graph = avfilter_graph_alloc();
        if (!filter_graph) {
            return std::unexpected(error("allocating Vulkan filter graph failed", AVERROR(ENOMEM)));
        }
        if (auto registered = register_device_timing_callback(filter_graph, config);
            !registered) {
            return std::unexpected(registered.error());
        }
        avfilter_graph_set_auto_convert(filter_graph, AVFILTER_AUTO_CONVERT_NONE);

        const AVFilter* buffer_filter = avfilter_get_by_name("buffer");
        const AVFilter* scale_filter = avfilter_get_by_name("scale_vulkan");
        const AVFilter* sink_filter = avfilter_get_by_name("buffersink");
        const AVFilter* blackdetect_filter =
            (rgb_blackdetect_before_scale || nv12_blackdetect_after_first ||
             nv12_blackdetect_after_second)
            ? avfilter_get_by_name("blackdetect_vulkan") : nullptr;
        if (!buffer_filter || !scale_filter || !sink_filter) {
            return std::unexpected(error("FFmpeg is missing buffer, scale_vulkan, or buffersink filter"));
        }
        if ((rgb_blackdetect_before_scale || nv12_blackdetect_after_first ||
             nv12_blackdetect_after_second) && !blackdetect_filter) {
            return std::unexpected(error("FFmpeg is missing the requested blackdetect_vulkan diagnostic filter"));
        }
        AVFilterContext* source = avfilter_graph_alloc_filter(filter_graph, buffer_filter,
                                                              "capture_src");
        if (!source) {
            return std::unexpected(error("allocating Vulkan buffer source failed", AVERROR(ENOMEM)));
        }
        const std::string source_args = "video_size=" + std::to_string(requirements.width) + "x" +
            std::to_string(requirements.height) + ":pix_fmt=" +
            std::to_string(AV_PIX_FMT_VULKAN) + ":time_base=1/1000000:pixel_aspect=1/1";
        int result = 0;
        auto* parameters = av_buffersrc_parameters_alloc();
        if (!parameters) {
            return std::unexpected(error("allocating Vulkan source parameters failed", AVERROR(ENOMEM)));
        }
        parameters->format = AV_PIX_FMT_VULKAN;
        parameters->time_base = kPacketTimeBase;
        parameters->width = static_cast<int>(requirements.width);
        parameters->height = static_cast<int>(requirements.height);
        parameters->frame_rate = AVRational{static_cast<int>(config.fps_num),
                                             static_cast<int>(config.fps_den)};
        parameters->color_space = AVCOL_SPC_BT709;
        parameters->color_range = AVCOL_RANGE_JPEG;
        parameters->hw_frames_ctx = av_buffer_ref(input_frames);
        if (!parameters->hw_frames_ctx) {
            av_free(parameters);
            return std::unexpected(error("referencing Vulkan source frames failed", AVERROR(ENOMEM)));
        }
        result = av_buffersrc_parameters_set(source, parameters);
        av_buffer_unref(&parameters->hw_frames_ctx);
        av_free(parameters);
        if (result < 0) {
            return std::unexpected(error("configuring Vulkan buffer source failed", result));
        }
        result = avfilter_init_str(source, source_args.c_str());
        if (result < 0) {
            return std::unexpected(error("creating Vulkan buffer source failed", result));
        }

        AVFilterContext* first = nullptr;
        AVFilterContext* second = nullptr;
        const std::string shrink_args = "w=" + std::to_string(requirements.width - 2U) +
            ":h=" + std::to_string(requirements.height - 2U) + ":format=nv12:out_range=limited";
        result = create_device_filter(&first, scale_filter, "gpu_rgb_to_nv12",
                                      shrink_args.c_str(), filter_graph, vulkan_device);
        if (result < 0) {
            return std::unexpected(error("creating first Vulkan compute conversion pass failed", result));
        }
        const std::string resize_args = "w=" + std::to_string(config.output_width) +
            ":h=" + std::to_string(config.output_height) + ":format=nv12";
        result = create_device_filter(&second, scale_filter, "gpu_output_resize",
                                      resize_args.c_str(), filter_graph, vulkan_device);
        if (result < 0) {
            return std::unexpected(error("creating second Vulkan resize pass failed", result));
        }
        AVFilterContext* blackdetect_after_first = nullptr;
        AVFilterContext* blackdetect_after_second = nullptr;
        AVFilterContext* blackdetect_before_scale = nullptr;
        const char* blackdetect_args =
            "d=0:picture_black_ratio_th=0.98:pixel_black_th=0.1";
        if (rgb_blackdetect_before_scale) {
            result = create_device_filter(&blackdetect_before_scale,
                                          blackdetect_filter,
                                          "rgb_blackdetect_before_scale",
                                          blackdetect_args, filter_graph,
                                          vulkan_device);
            if (result < 0) {
                return std::unexpected(error(
                    "creating imported-RGB on-GPU blackdetect probe failed", result));
            }
        }
        if (nv12_blackdetect_after_first) {
            result = create_device_filter(&blackdetect_after_first, blackdetect_filter,
                                          "nv12_blackdetect_after_first", blackdetect_args,
                                          filter_graph, vulkan_device);
            if (result < 0) {
                return std::unexpected(error(
                    "creating first-stage on-GPU NV12 blackdetect probe failed", result));
            }
        }
        if (nv12_blackdetect_after_second) {
            result = create_device_filter(&blackdetect_after_second, blackdetect_filter,
                                          "nv12_blackdetect_after_second", blackdetect_args,
                                          filter_graph, vulkan_device);
            if (result < 0) {
                return std::unexpected(error(
                    "creating second-stage on-GPU NV12 blackdetect probe failed", result));
            }
        }
        AVFilterContext* sink = nullptr;
        result = avfilter_graph_create_filter(&sink, sink_filter, "encode_sink", nullptr, nullptr,
                                              filter_graph);
        if (result < 0) {
            return std::unexpected(error("creating Vulkan buffer sink failed", result));
        }
        AVFilterContext* before_first = blackdetect_before_scale
            ? blackdetect_before_scale : first;
        AVFilterContext* after_first = blackdetect_after_first
            ? blackdetect_after_first : second;
        AVFilterContext* after_second = blackdetect_after_second
            ? blackdetect_after_second : sink;
        if ((result = avfilter_link(source, 0, before_first, 0)) < 0 ||
            (blackdetect_before_scale &&
             (result = avfilter_link(blackdetect_before_scale, 0, first, 0)) < 0) ||
            (result = avfilter_link(first, 0, after_first, 0)) < 0 ||
            (blackdetect_after_first &&
             (result = avfilter_link(blackdetect_after_first, 0, second, 0)) < 0) ||
            (result = avfilter_link(second, 0, after_second, 0)) < 0 ||
            (blackdetect_after_second &&
             (result = avfilter_link(blackdetect_after_second, 0, sink, 0)) < 0)) {
            return std::unexpected(error("linking Vulkan filter graph failed", result));
        }
        result = avfilter_graph_config(filter_graph, nullptr);
        if (result < 0) {
            return std::unexpected(error("configuring Vulkan-only two-pass conversion graph failed", result));
        }
        log_input_frames_ref_audit("after_filter_graph_config", input_frames);
        if (av_buffersink_get_format(sink) != AV_PIX_FMT_VULKAN ||
            av_buffersink_get_w(sink) != static_cast<int>(config.output_width) ||
            av_buffersink_get_h(sink) != static_cast<int>(config.output_height)) {
            return std::unexpected(error("filter graph did not negotiate the requested Vulkan output frames"));
        }
        AVBufferRef* sink_frames = av_buffersink_get_hw_frames_ctx(sink);
        if (!sink_frames) {
            return std::unexpected(error("Vulkan output filter did not expose its hardware frames context"));
        }
        const auto* output_frames = reinterpret_cast<const AVHWFramesContext*>(sink_frames->data);
        if (output_frames->format != AV_PIX_FMT_VULKAN || output_frames->sw_format != AV_PIX_FMT_NV12) {
            return std::unexpected(error("conversion graph output must be AV_PIX_FMT_VULKAN with NV12 storage"));
        }

        AVBufferRef* encoder_frames = nvenc_backend ? cuda_frames : sink_frames;
        if (color_correct_nv12) {
            auto corrector = detail::Nv12ColorCorrector::create(
                sink_frames, sink_frames, config.output_width, config.output_height,
                static_cast<std::uint32_t>(config.capture_pool_size +
                    static_cast<std::size_t>(std::max(config.async_depth, 0)) + 8U));
            if (!corrector) return std::unexpected(corrector.error());
            nv12_color_corrector.emplace(std::move(*corrector));
        }
        auto encoder_result = open_encoder(encoder_frames);
        if (!encoder_result) return encoder_result;
        filter_source = source;
        filter_sink = sink;
        const char* codec_name = config.codec == Codec::h264 ? "h264_vulkan" : "hevc_vulkan";
        const char* selected_codec_name = nvenc_backend
            ? (config.codec == Codec::h264 ? "h264_nvenc" : "hevc_nvenc")
            : codec_name;
        if (nvenc_backend) {
            const auto* vk_frames = static_cast<const AVVulkanFramesContext*>(
                output_frames->hwctx);
            if (!vk_frames || vk_frames->format[1] == VK_FORMAT_UNDEFINED) {
                return std::unexpected(error(
                    "Vulkan NV12 filter output did not allocate one image per plane; CUDA NVENC requires separate-plane images"));
            }
            av_log(nullptr, AV_LOG_INFO,
                   "CUDA interop Vulkan NV12 images: Y format=%d, UV format=%d\n",
                   static_cast<int>(vk_frames->format[0]),
                   static_cast<int>(vk_frames->format[1]));
            gpu_info.conversion_path = color_correct_nv12
                ? "FFmpeg scale_vulkan two-pass NV12 + GPU-only correction -> Vulkan/CUDA external-memory transfer -> NVENC"
                : "FFmpeg scale_vulkan two-pass NV12 -> Vulkan/CUDA external-memory transfer -> NVENC";
        }
        av_log(nullptr, AV_LOG_INFO,
               "fluxrec GPU path: DRM_PRIME direct import -> %s -> %s (%s)\n",
               color_correct_nv12 ? "Vulkan two-pass NV12 plus in-place BT.709 red/blue correction" : "Vulkan two-pass NV12",
               nvenc_backend ? selected_codec_name : codec_name,
               gpu_info.device_name.c_str());
        return {};
    }

    std::int64_t frame_pts(const capture::CapturedFrame& frame) {
        std::int64_t pts = AV_NOPTS_VALUE;
        if (frame.has_presentation_time) {
            if (!pts_origin_ns) pts_origin_ns = frame.presentation_time_ns;
            const std::uint64_t elapsed_ns = frame.presentation_time_ns >= *pts_origin_ns
                ? frame.presentation_time_ns - *pts_origin_ns : 0;
            if (elapsed_ns / 1000ULL <=
                static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                pts = static_cast<std::int64_t>(elapsed_ns / 1000ULL);
            }
        }
        if (pts == AV_NOPTS_VALUE) {
            const long double period = 1'000'000.0L * config.fps_den / config.fps_num;
            const long double timestamp = static_cast<long double>(frame.sequence) * period;
            if (timestamp <= static_cast<long double>(std::numeric_limits<std::int64_t>::max())) {
                pts = static_cast<std::int64_t>(timestamp);
            }
        }
        if (pts == AV_NOPTS_VALUE) pts = previous_pts == AV_NOPTS_VALUE ? 0 : previous_pts + 1;
        if (previous_pts != AV_NOPTS_VALUE && pts <= previous_pts) pts = previous_pts + 1;
        previous_pts = pts;
        return pts;
    }

    std::expected<std::vector<media::EncodedPacket>, GpuError> receive_packets(bool flushing = false) {
        std::vector<media::EncodedPacket> packets;
        AVPacket* packet = av_packet_alloc();
        if (!packet) return std::unexpected(error("allocating encoded packet failed", AVERROR(ENOMEM)));
        for (;;) {
            int result = 0;
            {
                ScopedCpuTiming timing{counters.last_encoder_receive_cpu_ns,
                                       counters.total_encoder_receive_cpu_ns};
                result = avcodec_receive_packet(codec_context, packet);
            }
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) break;
            if (result < 0) {
                av_packet_free(&packet);
                return std::unexpected(error("receiving encoded packet failed", result));
            }
            if (packet->pts == AV_NOPTS_VALUE || packet->dts == AV_NOPTS_VALUE) {
                av_packet_free(&packet);
                return std::unexpected(error(
                    "encoder emitted a packet without both PTS and DTS; refusing to fabricate timestamps and retaining unmatched capture leases",
                    AVERROR_INVALIDDATA));
            }
            auto pending = pending_frames.find(packet->pts);
            if (pending == pending_frames.end()) {
                av_packet_free(&packet);
                return std::unexpected(error(
                    "encoded packet PTS does not match a pending capture frame; retaining unmatched capture leases",
                    AVERROR_INVALIDDATA));
            }
            media::EncodedPacket output;
            output.data.assign(packet->data, packet->data + packet->size);
            output.codec = config.codec == Codec::h264 ? media::VideoCodec::h264 : media::VideoCodec::hevc;
            output.pts = packet->pts;
            output.dts = packet->dts;
            output.duration = packet->duration;
            output.keyframe = (packet->flags & AV_PKT_FLAG_KEY) != 0;
            output.time_base = {kPacketTimeBase.num, kPacketTimeBase.den};
            const std::uint64_t latency_ns = elapsed_ns(pending->second.submitted_at);
            counters.last_submit_to_packet_wall_ns = latency_ns;
            counters.total_submit_to_packet_wall_ns += latency_ns;
            ++counters.submit_to_packet_samples;
            counters.max_submit_to_packet_wall_ns =
                std::max(counters.max_submit_to_packet_wall_ns, latency_ns);
            release_pending_frame(pending->second);
            pending_frames.erase(pending);
            packets.push_back(std::move(output));
            ++counters.encoded_frames;
            av_packet_unref(packet);
        }
        av_packet_free(&packet);
        if (flushing && !pending_frames.empty() && packets.empty()) {
            // A codec may coalesce or discard a frame on flush; retained DMA-BUF
            // leases can be returned only after the codec has completed.
        }
        counters.pending_capture_leases = pending_frames.size();
        return packets;
    }

    std::expected<void, GpuError> encode_filtered_frame(AVFrame* frame,
                                                         std::vector<media::EncodedPacket>& packets) {
        if (frame->format != AV_PIX_FMT_VULKAN || !frame->hw_frames_ctx) {
            return std::unexpected(error("filter emitted a non-Vulkan frame; refusing CPU pixel fallback"));
        }
        const auto* hwframes = reinterpret_cast<const AVHWFramesContext*>(frame->hw_frames_ctx->data);
        if (hwframes->format != AV_PIX_FMT_VULKAN || hwframes->sw_format != AV_PIX_FMT_NV12) {
            return std::unexpected(error("filter emitted Vulkan frame without NV12 storage"));
        }
        AVFrame* encoder_frame = frame;
        if (color_correct_nv12) {
            const auto pending_it = pending_frames.find(frame->pts);
            if (pending_it == pending_frames.end()) {
                return std::unexpected(error("NV12 correction could not find the capture lease for filter output PTS"));
            }
            PendingFrame& pending = pending_it->second;
            if (pending.imported || pending.converted) {
                return std::unexpected(error("duplicate filter output PTS entered NV12 correction twice"));
            }
            AVFrame* input_copy = av_frame_clone(frame);
            if (!input_copy) {
                return std::unexpected(error("retaining Vulkan NV12 source frame for correction failed", AVERROR(ENOMEM)));
            }
            // Keep two distinct AVFrame headers for PendingFrame's imported
            // and converted ownership, while both refer to the same filter
            // sink AVVkFrame/image. The correction pass writes back into that
            // image before it is handed to the encoder.
            AVFrame* corrected = av_frame_clone(frame);
            if (!corrected) {
                av_frame_free(&input_copy);
                return std::unexpected(error("allocating corrected Vulkan NV12 frame failed", AVERROR(ENOMEM)));
            }
            corrected->pts = frame->pts;
            corrected->colorspace = AVCOL_SPC_BT709;
            corrected->color_primaries = AVCOL_PRI_BT709;
            corrected->color_trc = AVCOL_TRC_BT709;
            corrected->color_range = AVCOL_RANGE_MPEG;
            auto resources = nv12_color_corrector->dispatch(frame, corrected);
            if (!resources) {
                av_frame_free(&corrected);
                av_frame_free(&input_copy);
                return std::unexpected(resources.error());
            }
            pending.imported = input_copy;
            pending.converted = corrected;
            pending.color_resources = *resources;
            encoder_frame = corrected;
        }
        if (preview_producer) {
            const auto pending_it = pending_frames.find(frame->pts);
            if (pending_it != pending_frames.end()) {
                const PendingFrame& pending = pending_it->second;
                submit_preview(encoder_frame, pending.sequence,
                               pending.presentation_time_ns,
                               pending.has_presentation_time);
            }
        }
        if (nvenc_backend) {
            const auto pending_it = pending_frames.find(frame->pts);
            if (pending_it == pending_frames.end()) {
                return std::unexpected(error(
                    "NVENC transfer could not find the capture lease for filter output PTS"));
            }
            PendingFrame& pending = pending_it->second;
            if (pending.nvenc_source || pending.nvenc_frame) {
                return std::unexpected(error(
                    "duplicate filter output PTS entered the Vulkan-to-CUDA transfer twice"));
            }
            AVFrame* retained_source = av_frame_clone(encoder_frame);
            if (!retained_source) {
                return std::unexpected(error(
                    "retaining Vulkan NV12 source through CUDA encode failed",
                    AVERROR(ENOMEM)));
            }
            AVFrame* cuda_frame = av_frame_alloc();
            if (!cuda_frame) {
                av_frame_free(&retained_source);
                return std::unexpected(error(
                    "allocating CUDA NV12 encoder frame failed", AVERROR(ENOMEM)));
            }
            int transfer_result = av_hwframe_get_buffer(cuda_frames, cuda_frame, 0);
            if (transfer_result >= 0) {
                ScopedCpuTiming transfer_timing{counters.last_cuda_transfer_cpu_ns,
                                                  counters.total_cuda_transfer_cpu_ns};
                transfer_result = av_hwframe_transfer_data(cuda_frame, encoder_frame, 0);
            }
            if (transfer_result >= 0) {
                transfer_result = av_frame_copy_props(cuda_frame, encoder_frame);
            }
            if (transfer_result < 0) {
                av_frame_free(&cuda_frame);
                av_frame_free(&retained_source);
                return std::unexpected(error(
                    "GPU-only Vulkan NV12 to CUDA NV12 transfer failed; verify Vulkan external-memory and external-semaphore support",
                    transfer_result));
            }
            if (cuda_frame->format != AV_PIX_FMT_CUDA || !cuda_frame->hw_frames_ctx) {
                av_frame_free(&cuda_frame);
                av_frame_free(&retained_source);
                return std::unexpected(error(
                    "Vulkan-to-CUDA transfer returned a non-CUDA frame; refusing CPU pixel fallback"));
            }
            const auto* cuda_hwframes = reinterpret_cast<const AVHWFramesContext*>(
                cuda_frame->hw_frames_ctx->data);
            if (cuda_hwframes->format != AV_PIX_FMT_CUDA ||
                cuda_hwframes->sw_format != AV_PIX_FMT_NV12) {
                av_frame_free(&cuda_frame);
                av_frame_free(&retained_source);
                return std::unexpected(error(
                    "Vulkan-to-CUDA transfer returned a CUDA frame without NV12 storage"));
            }
            cuda_frame->pts = encoder_frame->pts;
            pending.nvenc_source = retained_source;
            pending.nvenc_frame = cuda_frame;
            encoder_frame = cuda_frame;
        }
        int result = send_encoder_frame(encoder_frame);
        if (result == AVERROR(EAGAIN)) {
            auto received = receive_packets();
            if (!received) return std::unexpected(received.error());
            for (auto& packet : *received) packets.push_back(std::move(packet));
            result = send_encoder_frame(encoder_frame);
        }
        if (result < 0) {
            return std::unexpected(error(nvenc_backend
                ? "submitting CUDA frame to NVENC failed"
                : "submitting Vulkan frame to encoder failed", result));
        }
        auto received = receive_packets();
        if (!received) return std::unexpected(received.error());
        for (auto& packet : *received) packets.push_back(std::move(packet));
        return {};
    }

    std::expected<void, GpuError> encode_direct_frame(
        capture::CapturedFrame& captured, AVFrame* imported, std::int64_t pts,
        std::chrono::steady_clock::time_point submitted_at,
        std::vector<media::EncodedPacket>& packets) {
        AVFrame* converted = av_frame_alloc();
        if (!converted) {
            av_frame_free(&imported);
            return std::unexpected(error("allocating direct Vulkan NV12 output frame failed", AVERROR(ENOMEM)));
        }
        converted->format = AV_PIX_FMT_VULKAN;
        converted->width = static_cast<int>(config.output_width);
        converted->height = static_cast<int>(config.output_height);
        converted->hw_frames_ctx = av_buffer_ref(direct_output_frames);
        if (!converted->hw_frames_ctx) {
            av_frame_free(&converted);
            av_frame_free(&imported);
            return std::unexpected(error("referencing direct NV12 output frames failed", AVERROR(ENOMEM)));
        }
        int result = av_hwframe_get_buffer(direct_output_frames, converted, 0);
        if (result < 0) {
            av_frame_free(&converted);
            av_frame_free(&imported);
            return std::unexpected(error("allocating direct Vulkan NV12 frame failed", result));
        }
        converted->pts = pts;
        converted->colorspace = AVCOL_SPC_BT709;
        converted->color_primaries = AVCOL_PRI_BT709;
        converted->color_trc = AVCOL_TRC_BT709;
        converted->color_range = AVCOL_RANGE_MPEG;

        const auto conversion_begin = std::chrono::steady_clock::now();
        auto resources = direct_converter->dispatch(imported, converted, captured);
        const auto conversion_end = std::chrono::steady_clock::now();
        const auto conversion_ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(conversion_end - conversion_begin).count());
        counters.last_conversion_submit_ns = conversion_ns;
        counters.total_conversion_submit_ns += conversion_ns;
        if (!resources) {
            av_frame_free(&converted);
            av_frame_free(&imported);
            return std::unexpected(resources.error());
        }

        // Preview samples the completed NV12 conversion. It retains its own
        // AVFrame reference through the producer-side fence, so the preview
        // pass neither extends the capture lease nor depends on its lifetime.
        submit_preview(converted, captured.sequence, captured.presentation_time_ns,
                       captured.has_presentation_time);

        PendingFrame pending{
            .lease = std::move(captured.lease),
            .imported = imported,
            .converted = converted,
            .direct_resources = *resources,
            .color_resources = {},
            .submitted_at = submitted_at,
            .sequence = captured.sequence,
            .presentation_time_ns = captured.presentation_time_ns,
            .has_presentation_time = captured.has_presentation_time,
        };
        auto [pending_it, inserted] = pending_frames.emplace(pts, std::move(pending));
        if (!inserted) {
            release_pending_frame(pending);
            return std::unexpected(error("duplicate frame PTS while a DMA-BUF lease is pending"));
        }
        ++counters.submitted_frames;
        counters.pending_capture_leases = pending_frames.size();

        auto send_frame = [&]() {
            const auto encode_begin = std::chrono::steady_clock::now();
            const int send_result = send_encoder_frame(converted);
            const auto encode_end = std::chrono::steady_clock::now();
            const auto encode_ns = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(encode_end - encode_begin).count());
            counters.last_encoder_submit_ns = encode_ns;
            counters.total_encoder_submit_ns += encode_ns;
            return send_result;
        };

        result = send_frame();
        if (result == AVERROR(EAGAIN)) {
            auto received = receive_packets();
            if (!received) {
                release_pending_frame(pending_it->second);
                pending_frames.erase(pending_it);
                return std::unexpected(received.error());
            }
            for (auto& packet : *received) packets.push_back(std::move(packet));
            result = send_frame();
        }
        if (result < 0) {
            release_pending_frame(pending_it->second);
            pending_frames.erase(pending_it);
            return std::unexpected(error("submitting direct Vulkan NV12 frame to encoder failed", result));
        }
        auto received = receive_packets();
        if (!received) return std::unexpected(received.error());
        for (auto& packet : *received) packets.push_back(std::move(packet));
        return {};
    }

    std::expected<std::vector<media::EncodedPacket>, GpuError> submit(capture::CapturedFrame&& captured) {
        reset_last_pipeline_timings();
        const auto submitted_at = std::chrono::steady_clock::now();
        if (drained) return std::unexpected(error("encoder has already been drained"));
        if (!captured.lease) return std::unexpected(error("captured frame has no live DMA-BUF lease"));
        const auto& image = captured.lease.image();
        const std::int64_t pts = frame_pts(captured);
        if (direct_cuda_egl_backend) {
            if (!direct_cuda_egl || image.width != 2560U || image.height != 1440U ||
                image.fourcc != gpu_info.capture_fourcc ||
                image.modifier != gpu_info.capture_modifier || image.planes.size() != 1U) {
                return std::unexpected(error(
                    "captured DMA-BUF descriptor differs from the experimental CUDA EGL pool"));
            }
            return direct_cuda_egl->submit(std::move(captured), pts, submitted_at);
        }
        if (image.width != static_cast<std::uint32_t>(reinterpret_cast<AVHWFramesContext*>(input_frames->data)->width) ||
            image.height != static_cast<std::uint32_t>(reinterpret_cast<AVHWFramesContext*>(input_frames->data)->height) ||
            image.fourcc != gpu_info.capture_fourcc || image.modifier != gpu_info.capture_modifier) {
            return std::unexpected(error("captured DMA-BUF descriptor differs from the configured pool"));
        }
        if (dmabuf_sync_wait) {
            if (image.planes.empty() || image.planes.front().fd < 0) {
                return std::unexpected(error(
                    "DMA-BUF sync wait requires a valid capture plane fd"));
            }
            auto waited = detail::wait_for_dma_buf_read_dependency(
                image.planes.front().fd, captured.sequence);
            if (!waited) {
                return std::unexpected(error(
                    "DMA-BUF read dependency wait failed before FFmpeg import: " +
                        waited.error().message,
                waited.error().code));
            }
        }
        if (preimport_delay_us > 0U) {
            std::this_thread::sleep_for(
                std::chrono::microseconds{preimport_delay_us});
        }
        if (dmabuf_black_probe) {
            auto sampled = dmabuf_black_probe->inspect(captured);
            if (!sampled) {
                return std::unexpected(error(
                    "independent raw DMA-BUF black probe failed: " +
                    sampled.error().message, sampled.error().code));
            }
        }
        AVFrame* imported = nullptr;
        int result = 0;
        {
            ScopedCpuTiming import_timing{counters.last_dmabuf_import_cpu_ns,
                                          counters.total_dmabuf_import_cpu_ns};
            GpuError import_error;
            AVFrame* drm_frame = make_drm_frame(image, import_error);
            if (!drm_frame) return std::unexpected(std::move(import_error));
            drm_frame->pts = pts;
            imported = av_frame_alloc();
            if (!imported) {
                av_frame_free(&drm_frame);
                return std::unexpected(error("allocating Vulkan import frame failed", AVERROR(ENOMEM)));
            }
            imported->format = AV_PIX_FMT_VULKAN;
            imported->pts = pts;
            imported->width = drm_frame->width;
            imported->height = drm_frame->height;
            imported->hw_frames_ctx = av_buffer_ref(input_frames);
            if (!imported->hw_frames_ctx) {
                av_frame_free(&imported);
                av_frame_free(&drm_frame);
                return std::unexpected(error("referencing input Vulkan frames failed", AVERROR(ENOMEM)));
            }
            imported->colorspace = AVCOL_SPC_BT709;
            imported->color_primaries = AVCOL_PRI_BT709;
            imported->color_trc = AVCOL_TRC_IEC61966_2_1;
            imported->color_range = AVCOL_RANGE_JPEG;
            probe_dmabuf_sync_file(captured);
            result = av_hwframe_map(imported, drm_frame,
                                    AV_HWFRAME_MAP_READ | AV_HWFRAME_MAP_DIRECT);
            av_frame_free(&drm_frame);
            if (result < 0) {
                av_frame_free(&imported);
                return std::unexpected(error("importing capture DMA-BUF into Vulkan failed", result));
            }
            if (imported->format != AV_PIX_FMT_VULKAN) {
                av_frame_free(&imported);
                return std::unexpected(error("DRM_PRIME import did not produce AV_PIX_FMT_VULKAN"));
            }
        }
        if (frame_identity) {
            const auto [sequence_entry, inserted] =
                frame_identity_pts.emplace(captured.sequence, pts);
            if (!inserted) {
                av_frame_free(&imported);
                return std::unexpected(error("duplicate capture sequence in frame identity tracking"));
            }
            auto identity = frame_identity->try_submit(imported, captured.sequence);
            if (!identity) {
                frame_identity_pts.erase(sequence_entry);
                av_frame_free(&imported);
                return std::unexpected(error("submitting frame identity failed: " +
                                             identity.error().message,
                                             identity.error().code));
            }
            if (*identity == FrameIdentitySubmit::skipped_ring_full) {
                ++frame_identity_skipped_frames;
                frame_identity_pts.erase(sequence_entry);
            }
        }
        if (direct_conversion) {
            if (pending_frames.contains(pts)) {
                av_frame_free(&imported);
                return std::unexpected(error("duplicate frame PTS while a DMA-BUF lease is pending"));
            }
            std::vector<media::EncodedPacket> packets;
            auto encoded = encode_direct_frame(captured, imported, pts, submitted_at, packets);
            if (!encoded) return std::unexpected(encoded.error());
            return packets;
        }
        {
            ScopedCpuTiming filter_submit_timing{counters.last_filter_submit_cpu_ns,
                                                 counters.total_filter_submit_cpu_ns};
            result = av_buffersrc_add_frame_flags(filter_source, imported,
                                                  AV_BUFFERSRC_FLAG_KEEP_REF);
        }
        av_frame_free(&imported);
        if (result < 0) {
            return std::unexpected(error("submitting Vulkan import to conversion graph failed", result));
        }
        PendingFrame pending{
            .lease = std::move(captured.lease),
            .imported = nullptr,
            .converted = nullptr,
            .direct_resources = {},
            .color_resources = {},
            .submitted_at = submitted_at,
            .sequence = captured.sequence,
            .presentation_time_ns = captured.presentation_time_ns,
            .has_presentation_time = captured.has_presentation_time,
        };
        if (!pending_frames.emplace(pts, std::move(pending)).second) {
            return std::unexpected(error("duplicate frame PTS while a DMA-BUF lease is pending"));
        }
        ++counters.submitted_frames;
        counters.pending_capture_leases = pending_frames.size();

        std::vector<media::EncodedPacket> packets;
        AVFrame* output = av_frame_alloc();
        if (!output) return std::unexpected(error("allocating Vulkan filter output frame failed", AVERROR(ENOMEM)));
        for (;;) {
            {
                ScopedCpuTiming filter_pull_timing{counters.last_filter_pull_cpu_ns,
                                                   counters.total_filter_pull_cpu_ns};
                result = av_buffersink_get_frame(filter_sink, output);
            }
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) break;
            if (result < 0) {
                av_frame_free(&output);
                return std::unexpected(error("receiving Vulkan filter output failed", result));
            }
            auto encoded = encode_filtered_frame(output, packets);
            av_frame_unref(output);
            if (!encoded) {
                av_frame_free(&output);
                return std::unexpected(encoded.error());
            }
        }
        av_frame_free(&output);
        return packets;
    }

    std::expected<std::vector<media::EncodedPacket>, GpuError> drain() {
        if (direct_cuda_egl_backend) {
            if (!direct_cuda_egl) {
                return std::unexpected(error("experimental CUDA EGL backend is not initialized"));
            }
            return direct_cuda_egl->drain();
        }
        reset_last_pipeline_timings();
        if (drained) return std::vector<media::EncodedPacket>{};
        drained = true;
        std::vector<media::EncodedPacket> packets;
        int result = 0;
        if (!direct_conversion) {
            result = av_buffersrc_add_frame_flags(filter_source, nullptr, 0);
            if (result < 0 && result != AVERROR_EOF) {
                return std::unexpected(error("flushing Vulkan conversion graph failed", result));
            }
            AVFrame* output = av_frame_alloc();
            if (!output) return std::unexpected(error("allocating drain frame failed", AVERROR(ENOMEM)));
            for (;;) {
                {
                    ScopedCpuTiming filter_pull_timing{counters.last_filter_pull_cpu_ns,
                                                       counters.total_filter_pull_cpu_ns};
                    result = av_buffersink_get_frame(filter_sink, output);
                }
                if (result == AVERROR_EOF || result == AVERROR(EAGAIN)) break;
                if (result < 0) {
                    av_frame_free(&output);
                    return std::unexpected(error("draining Vulkan filter graph failed", result));
                }
                auto encoded = encode_filtered_frame(output, packets);
                av_frame_unref(output);
                if (!encoded) {
                    av_frame_free(&output);
                    return std::unexpected(encoded.error());
                }
            }
            av_frame_free(&output);
        }
        result = send_encoder_frame(nullptr);
        if (result < 0 && result != AVERROR_EOF) {
            return std::unexpected(error("flushing Vulkan encoder failed", result));
        }
        auto received = receive_packets(true);
        if (!received) return std::unexpected(received.error());
        for (auto& packet : *received) packets.push_back(std::move(packet));
        if (frame_identity) {
            auto identity_drained = frame_identity->drain();
            if (!identity_drained) {
                return std::unexpected(error("draining frame identity failed: " +
                                             identity_drained.error().message,
                                             identity_drained.error().code));
            }
        }
        if (dmabuf_black_probe) dmabuf_black_probe->finish();
        release_all_pending_frames();
        result = drain_input_frames_exec_pool("recorder_drain");
        if (result < 0) {
            return std::unexpected(error(
                "draining Vulkan input execution pool failed; teardown will retry and fail-stop if work remains pending",
                result));
        }
        return packets;
    }

    std::expected<FrameIdentityPoll, GpuError> poll_frame_identity() {
        FrameIdentityPoll result;
        result.skipped_frames = frame_identity_skipped_frames;
        if (!frame_identity) return result;
        auto completed = frame_identity->poll();
        if (!completed) {
            return std::unexpected(error("polling frame identity failed: " +
                                         completed.error().message,
                                         completed.error().code));
        }
        std::vector<FrameIdentityPoll::Sample> samples;
        samples.reserve(completed->size());
        for (auto& digest : *completed) {
            FrameIdentityPoll::Sample sample;
            const auto pts = frame_identity_pts.find(digest.sequence);
            if (pts != frame_identity_pts.end()) {
                sample.pts = pts->second;
                frame_identity_pts.erase(pts);
            }
            sample.digest = std::move(digest);
            samples.push_back(std::move(sample));
        }
        result.completed = std::move(samples);
        return result;
    }
};

GpuEncoder::GpuEncoder(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
GpuEncoder::~GpuEncoder() = default;
GpuEncoder::GpuEncoder(GpuEncoder&&) noexcept = default;
GpuEncoder& GpuEncoder::operator=(GpuEncoder&&) noexcept = default;

std::expected<GpuEncoder, GpuError> GpuEncoder::create(
    const capture::CaptureRequirements& requirements, EncoderConfig config) {
    auto impl = std::make_unique<Impl>();
    impl->config = config;
    auto initialized = impl->initialize(requirements);
    if (!initialized) return std::unexpected(initialized.error());
    return GpuEncoder{std::move(impl)};
}

std::span<const capture::DmabufImage> GpuEncoder::capture_buffers() const noexcept {
    if (!impl_) return {};
    return impl_->public_images;
}

std::expected<std::vector<media::EncodedPacket>, GpuError> GpuEncoder::submit(
    capture::CapturedFrame&& frame) {
    if (!impl_) return std::unexpected(error("GPU encoder is not initialized"));
    return impl_->submit(std::move(frame));
}

std::expected<std::vector<media::EncodedPacket>, GpuError> GpuEncoder::drain() {
    if (!impl_) return std::unexpected(error("GPU encoder is not initialized"));
    return impl_->drain();
}

std::expected<FrameIdentityPoll, GpuError> GpuEncoder::poll_frame_identity() {
    if (!impl_) return std::unexpected(error("GPU encoder is not initialized"));
    return impl_->poll_frame_identity();
}

const GpuInfo& GpuEncoder::info() const noexcept {
    static const GpuInfo empty{};
    return impl_ ? impl_->gpu_info : empty;
}

EncoderStats GpuEncoder::stats() const noexcept {
    if (!impl_) return {};
    return impl_->direct_cuda_egl ? impl_->direct_cuda_egl->stats() : impl_->counters;
}

PreviewStats GpuEncoder::preview_stats() const {
    PreviewStats result;
    if (!impl_) return result;
    result.requested = !impl_->config.preview_socket_path.empty();
    result.last_error = impl_->preview_setup_error;
    if (!impl_->preview_producer) return result;
    const PreviewProducerStats producer = impl_->preview_producer->stats();
    result.enabled = producer.enabled;
    result.frames_submitted = producer.frames_submitted;
    result.frames_rate_limited = producer.frames_rate_limited;
    result.frames_no_free_slot = producer.frames_no_free_slot;
    result.frames_source_unavailable = producer.frames_source_unavailable;
    result.frames_transport_dropped = producer.frames_transport_dropped;
    result.frames_published = producer.frames_published;
    if (!producer.last_error.empty()) result.last_error = producer.last_error;
    return result;
}

media::Rational GpuEncoder::packet_time_base() const noexcept {
    return {kPacketTimeBase.num, kPacketTimeBase.den};
}

std::span<const std::uint8_t> GpuEncoder::codec_extradata() const noexcept {
    if (!impl_) return {};
    if (impl_->direct_cuda_egl) return impl_->direct_cuda_egl->codec_extradata();
    return {reinterpret_cast<const std::uint8_t*>(impl_->extradata.data()), impl_->extradata.size()};
}

} // namespace fluxrec::gpu
