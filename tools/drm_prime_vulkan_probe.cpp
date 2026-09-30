#include "capture/capture.hpp"

extern "C" {
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/hwcontext_vulkan.h>
#include <libavutil/log.h>
#include <libavutil/mem.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
#include <libavutil/version.h>
}

#include <drm_fourcc.h>
#include <gbm.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

class Fd final {
public:
    explicit Fd(int value = -1) noexcept : value_(value) {}
    ~Fd() { reset(); }
    Fd(Fd&& other) noexcept : value_(std::exchange(other.value_, -1)) {}
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) reset(std::exchange(other.value_, -1));
        return *this;
    }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    [[nodiscard]] int get() const noexcept { return value_; }
    [[nodiscard]] explicit operator bool() const noexcept { return value_ >= 0; }
    [[nodiscard]] int release() noexcept { return std::exchange(value_, -1); }
    void reset(int value = -1) noexcept {
        if (value_ >= 0) ::close(value_);
        value_ = value;
    }

private:
    int value_;
};

struct GbmDeviceDeleter {
    void operator()(gbm_device* value) const noexcept {
        if (value) gbm_device_destroy(value);
    }
};

struct GbmBoDeleter {
    void operator()(gbm_bo* value) const noexcept {
        if (value) gbm_bo_destroy(value);
    }
};

using GbmDevicePtr = std::unique_ptr<gbm_device, GbmDeviceDeleter>;
using GbmBoPtr = std::unique_ptr<gbm_bo, GbmBoDeleter>;

struct HwDeviceDeleter {
    void operator()(AVBufferRef* value) const noexcept { av_buffer_unref(&value); }
};

struct HwFramesDeleter {
    void operator()(AVBufferRef* value) const noexcept { av_buffer_unref(&value); }
};

struct FrameDeleter {
    void operator()(AVFrame* value) const noexcept { av_frame_free(&value); }
};

using HwDeviceRef = std::unique_ptr<AVBufferRef, HwDeviceDeleter>;
using HwFramesRef = std::unique_ptr<AVBufferRef, HwFramesDeleter>;
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;

struct Candidate {
    std::uint32_t fourcc = 0;
    std::uint64_t modifier = 0;
};

std::string fourcc_string(std::uint32_t value) {
    std::array<char, 5> chars{
        static_cast<char>(value & 0xffU),
        static_cast<char>((value >> 8U) & 0xffU),
        static_cast<char>((value >> 16U) & 0xffU),
        static_cast<char>((value >> 24U) & 0xffU),
        '\0',
    };
    for (int i = 0; i < 4; ++i) {
        const auto c = static_cast<unsigned char>(chars[static_cast<std::size_t>(i)]);
        if (c < 0x20U || c > 0x7eU) chars[static_cast<std::size_t>(i)] = '?';
    }
    return std::string(chars.data(), 4);
}

std::string av_error_string(int error) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
    av_strerror(error, buffer.data(), buffer.size());
    return buffer.data();
}

const char* vk_format_name(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R8G8B8A8_UNORM: return "VK_FORMAT_R8G8B8A8_UNORM";
    case VK_FORMAT_B8G8R8A8_UNORM: return "VK_FORMAT_B8G8R8A8_UNORM";
    default: return "unlisted VkFormat";
    }
}

void free_descriptor_buffer(void*, std::uint8_t* data) {
    auto* descriptor = reinterpret_cast<AVDRMFrameDescriptor*>(data);
    for (int i = 0; i < descriptor->nb_objects; ++i) {
        if (descriptor->objects[i].fd >= 0) ::close(descriptor->objects[i].fd);
    }
    av_free(descriptor);
}

std::optional<std::uint64_t> dma_buf_size(int fd) {
    errno = 0;
    const off_t end = ::lseek(fd, 0, SEEK_END);
    if (end <= 0) return std::nullopt;
    return static_cast<std::uint64_t>(end);
}

bool parse_arguments(int argc, char** argv, std::string& render_node,
                     std::string& output_name) {
    render_node = "/dev/dri/renderD128";
    output_name = "DP-1";
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument(argv[i]);
        if (argument == "--device" && i + 1 < argc) {
            render_node = argv[++i];
        } else if (argument == "--output" && i + 1 < argc) {
            output_name = argv[++i];
        } else if (argument == "--help") {
            std::fprintf(stderr, "Usage: %s [--device /dev/dri/renderD128] [--output DP-1]\n", argv[0]);
            return false;
        } else {
            std::fprintf(stderr, "Unknown or incomplete argument: %s\n", argv[i]);
            return false;
        }
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    std::string render_node;
    std::string output_name;
    if (!parse_arguments(argc, argv, render_node, output_name)) return argc > 1 ? 2 : 0;

    fluxrec::capture::CaptureConfig capture_config;
    capture_config.output_name = output_name;
    auto capture_result = fluxrec::capture::WaylandCapture::connect(capture_config);
    if (!capture_result) {
        std::fprintf(stderr, "Capture negotiation failed: %s (system error %d)\n",
                     capture_result.error().message.c_str(), capture_result.error().system_error);
        return 1;
    }
    auto capture = std::move(*capture_result);
    const auto& requirements = capture.requirements();
    std::printf("Capture output: %s, %ux%u\n", output_name.c_str(), requirements.width,
                requirements.height);
    std::printf("Advertised AR24/XR24 modifiers:\n");

    std::vector<Candidate> candidates;
    for (const auto& format : requirements.dmabuf_formats) {
        if (format.fourcc != DRM_FORMAT_ARGB8888 && format.fourcc != DRM_FORMAT_XRGB8888) continue;
        std::printf("  %s modifier=0x%016" PRIx64 " vendor=0x%02" PRIx64 "\n",
                    fourcc_string(format.fourcc).c_str(), format.modifier,
                    (format.modifier >> 56U) & 0xffU);
        if (((format.modifier >> 56U) & 0xffU) == DRM_FORMAT_MOD_VENDOR_NVIDIA)
            candidates.push_back({format.fourcc, format.modifier});
    }
    if (candidates.empty()) {
        std::fprintf(stderr,
                     "The compositor did not advertise an NVIDIA AR24/XR24 modifier for %s.\n",
                     output_name.c_str());
        return 1;
    }

    // Prefer AR24 and then preserve the compositor's advertised modifier order.
    std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& lhs,
                                                              const Candidate& rhs) {
        return lhs.fourcc == DRM_FORMAT_ARGB8888 && rhs.fourcc != DRM_FORMAT_ARGB8888;
    });

    Fd render_fd(::open(render_node.c_str(), O_RDWR | O_CLOEXEC));
    if (!render_fd) {
        std::fprintf(stderr, "Could not open %s: %s\n", render_node.c_str(), std::strerror(errno));
        return 1;
    }
    struct stat render_stat {};
    if (::fstat(render_fd.get(), &render_stat) != 0) {
        std::fprintf(stderr, "Could not stat %s: %s\n", render_node.c_str(), std::strerror(errno));
        return 1;
    }
    if (requirements.dmabuf_device &&
        static_cast<std::uint64_t>(render_stat.st_rdev) != *requirements.dmabuf_device) {
        std::fprintf(stderr,
                     "Capture device dev_t=%" PRIu64 " does not match %s dev_t=%u.\n",
                     *requirements.dmabuf_device, render_node.c_str(),
                     static_cast<unsigned int>(render_stat.st_rdev));
        return 1;
    }
    std::printf("Allocator device: %s (major=%u minor=%u)\n", render_node.c_str(),
                static_cast<unsigned int>(major(render_stat.st_rdev)),
                static_cast<unsigned int>(minor(render_stat.st_rdev)));

    GbmDevicePtr gbm(gbm_create_device(render_fd.get()));
    if (!gbm) {
        std::fprintf(stderr, "gbm_create_device(%s) failed.\n", render_node.c_str());
        return 1;
    }
    std::printf("GBM backend: %s\n", gbm_device_get_backend_name(gbm.get()));

    GbmBoPtr bo;
    Candidate selected;
    for (const Candidate& candidate : candidates) {
        const std::uint64_t modifier = candidate.modifier;
        bo.reset(gbm_bo_create_with_modifiers2(
            gbm.get(), requirements.width, requirements.height, candidate.fourcc,
            &modifier, 1U, GBM_BO_USE_RENDERING));
        if (bo) {
            selected = candidate;
            break;
        }
        std::fprintf(stderr, "GBM allocation failed for %s modifier 0x%016" PRIx64 "\n",
                     fourcc_string(candidate.fourcc).c_str(), candidate.modifier);
    }
    if (!bo) {
        std::fprintf(stderr, "Could not allocate any compositor-advertised AR24/XR24 modifier.\n");
        return 1;
    }

    const int plane_count = gbm_bo_get_plane_count(bo.get());
    if (plane_count != 1) {
        std::fprintf(stderr, "Expected one packed RGB plane, GBM returned %d.\n", plane_count);
        return 1;
    }
    const std::uint64_t actual_modifier = gbm_bo_get_modifier(bo.get());
    if (actual_modifier != selected.modifier || gbm_bo_get_format(bo.get()) != selected.fourcc) {
        std::fprintf(stderr,
                     "GBM returned a different layout: format=%s modifier=0x%016" PRIx64 "\n",
                     fourcc_string(gbm_bo_get_format(bo.get())).c_str(), actual_modifier);
        return 1;
    }

    Fd dma_buf_fd(gbm_bo_get_fd_for_plane(bo.get(), 0));
    if (!dma_buf_fd) {
        std::fprintf(stderr, "gbm_bo_get_fd_for_plane failed: %s\n", std::strerror(errno));
        return 1;
    }
    const auto object_size = dma_buf_size(dma_buf_fd.get());
    if (!object_size) {
        std::fprintf(stderr, "DMA-BUF lseek(SEEK_END) did not report the object size.\n");
        return 1;
    }
    const std::uint32_t stride = gbm_bo_get_stride_for_plane(bo.get(), 0);
    const std::uint32_t offset = gbm_bo_get_offset(bo.get(), 0);
    std::printf("Allocated GBM BO: format=%s modifier=0x%016" PRIx64
                " planes=%d stride=%u offset=%u size=%" PRIu64 "\n",
                fourcc_string(selected.fourcc).c_str(), actual_modifier, plane_count,
                stride, offset, *object_size);

    auto* descriptor = static_cast<AVDRMFrameDescriptor*>(
        av_mallocz(sizeof(AVDRMFrameDescriptor)));
    if (!descriptor) {
        std::fprintf(stderr, "Could not allocate AVDRMFrameDescriptor.\n");
        return 1;
    }
    descriptor->nb_objects = 1;
    descriptor->objects[0].fd = dma_buf_fd.release();
    descriptor->objects[0].size = static_cast<std::size_t>(*object_size);
    descriptor->objects[0].format_modifier = actual_modifier;
    descriptor->nb_layers = 1;
    descriptor->layers[0].format = selected.fourcc;
    descriptor->layers[0].nb_planes = 1;
    descriptor->layers[0].planes[0].object_index = 0;
    descriptor->layers[0].planes[0].offset = static_cast<std::ptrdiff_t>(offset);
    descriptor->layers[0].planes[0].pitch = static_cast<std::ptrdiff_t>(stride);

    FramePtr drm_frame(av_frame_alloc());
    if (!drm_frame) {
        free_descriptor_buffer(nullptr, reinterpret_cast<std::uint8_t*>(descriptor));
        std::fprintf(stderr, "Could not allocate source AVFrame.\n");
        return 1;
    }
    drm_frame->format = AV_PIX_FMT_DRM_PRIME;
    drm_frame->width = static_cast<int>(requirements.width);
    drm_frame->height = static_cast<int>(requirements.height);
    drm_frame->data[0] = reinterpret_cast<std::uint8_t*>(descriptor);
    drm_frame->buf[0] = av_buffer_create(reinterpret_cast<std::uint8_t*>(descriptor),
                                         sizeof(*descriptor), free_descriptor_buffer,
                                         nullptr, 0);
    if (!drm_frame->buf[0]) {
        free_descriptor_buffer(nullptr, reinterpret_cast<std::uint8_t*>(descriptor));
        std::fprintf(stderr, "Could not attach AVDRMFrameDescriptor to source AVFrame.\n");
        return 1;
    }
    std::printf("AVDRMFrameDescriptor: fourcc=%s, one object and one plane; no pixel access.\n",
                fourcc_string(selected.fourcc).c_str());

    av_log_set_level(AV_LOG_INFO);
    AVBufferRef* device_raw = nullptr;
    int result = av_hwdevice_ctx_create(&device_raw, AV_HWDEVICE_TYPE_VULKAN,
                                       "0", nullptr, 0);
    if (result < 0) {
        std::fprintf(stderr, "Vulkan device creation failed: %s (%d).\n",
                     av_error_string(result).c_str(), result);
        return 1;
    }
    HwDeviceRef device(device_raw);
    const auto* av_device_context = reinterpret_cast<const AVHWDeviceContext*>(device->data);
    const auto* vulkan_device = static_cast<const AVVulkanDeviceContext*>(
        av_device_context->hwctx);
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(vulkan_device->phys_dev, &properties);
    std::printf("FFmpeg Vulkan device: %s (vendor=0x%04x device=0x%04x)\n",
                properties.deviceName, properties.vendorID, properties.deviceID);

    AVBufferRef* frames_raw = av_hwframe_ctx_alloc(device.get());
    if (!frames_raw) {
        std::fprintf(stderr, "Could not allocate Vulkan AVHWFramesContext.\n");
        return 1;
    }
    HwFramesRef frames(frames_raw);
    auto* frame_context = reinterpret_cast<AVHWFramesContext*>(frames->data);
    frame_context->format = AV_PIX_FMT_VULKAN;
    frame_context->sw_format = AV_PIX_FMT_BGRA;
    frame_context->width = static_cast<int>(requirements.width);
    frame_context->height = static_cast<int>(requirements.height);
    auto* vulkan_frames = static_cast<AVVulkanFramesContext*>(frame_context->hwctx);
    vulkan_frames->usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    result = av_hwframe_ctx_init(frames.get());
    if (result < 0) {
        std::fprintf(stderr, "Vulkan AVHWFramesContext init failed: %s (%d).\n",
                     av_error_string(result).c_str(), result);
        return 1;
    }

    FramePtr vulkan_frame(av_frame_alloc());
    if (!vulkan_frame) {
        std::fprintf(stderr, "Could not allocate destination AVFrame.\n");
        return 1;
    }
    vulkan_frame->format = AV_PIX_FMT_VULKAN;
    vulkan_frame->hw_frames_ctx = av_buffer_ref(frames.get());
    if (!vulkan_frame->hw_frames_ctx) {
        std::fprintf(stderr, "Could not reference Vulkan AVHWFramesContext.\n");
        return 1;
    }

    std::printf("FFmpeg runtime: %s (libavutil major=%u)\n", av_version_info(),
                AV_VERSION_MAJOR(avutil_version()));
    std::printf("Calling av_hwframe_map(DRM_PRIME -> VULKAN, READ|DIRECT)...\n");
    result = av_hwframe_map(vulkan_frame.get(), drm_frame.get(),
                            AV_HWFRAME_MAP_READ | AV_HWFRAME_MAP_DIRECT);
    if (result < 0) {
        std::fprintf(stderr, "av_hwframe_map failed: %s (%d).\n",
                     av_error_string(result).c_str(), result);
        return 1;
    }

    const auto* vk_frame = reinterpret_cast<const AVVkFrame*>(vulkan_frame->data[0]);
    if (!vk_frame || vk_frame->img[0] == VK_NULL_HANDLE) {
        std::fprintf(stderr, "av_hwframe_map succeeded but returned no Vulkan image.\n");
        return 1;
    }
    std::printf("Direct DRM_PRIME -> VULKAN import succeeded.\n");
    std::printf("AVFrame formats: source=%s destination=%s; Vulkan sw_format=%s\n",
                av_get_pix_fmt_name(static_cast<AVPixelFormat>(drm_frame->format)),
                av_get_pix_fmt_name(static_cast<AVPixelFormat>(vulkan_frame->format)),
                av_get_pix_fmt_name(frame_context->sw_format));
    std::printf("AVVulkanFramesContext format[0]=%s (%d); AVVkFrame image[0]=%p tiling=%d layout=%d\n",
                vk_format_name(vulkan_frames->format[0]),
                static_cast<int>(vulkan_frames->format[0]),
                reinterpret_cast<void*>(vk_frame->img[0]),
                static_cast<int>(vk_frame->tiling), static_cast<int>(vk_frame->layout[0]));
    std::printf("Imported layout: fourcc=%s modifier=0x%016" PRIx64
                "; no CPU pixel map, transfer, or copy was performed.\n",
                fourcc_string(selected.fourcc).c_str(), actual_modifier);
    return 0;
}
