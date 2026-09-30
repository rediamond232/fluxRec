#include "capture/capture.hpp"
#include "gpu/frame_identity.hpp"

extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/hwcontext_vulkan.h>
#include <libavutil/log.h>
#include <libavutil/mem.h>
#include <libavutil/version.h>
}

#include <drm_fourcc.h>
#include <gbm.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <expected>
#include <fcntl.h>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

#ifndef FLUXREC_FRAME_IDENTITY_SHADER_DIR
#define FLUXREC_FRAME_IDENTITY_SHADER_DIR "build/gpu/shaders"
#endif

constexpr std::size_t kPoolSize = 8;

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
struct HwDeviceDeleter {
    void operator()(AVBufferRef* value) const noexcept { av_buffer_unref(&value); }
};
struct HwFramesDeleter {
    void operator()(AVBufferRef* value) const noexcept { av_buffer_unref(&value); }
};
struct FrameDeleter {
    void operator()(AVFrame* value) const noexcept { av_frame_free(&value); }
};

using GbmDevicePtr = std::unique_ptr<gbm_device, GbmDeviceDeleter>;
using GbmBoPtr = std::unique_ptr<gbm_bo, GbmBoDeleter>;
using HwDeviceRef = std::unique_ptr<AVBufferRef, HwDeviceDeleter>;
using HwFramesRef = std::unique_ptr<AVBufferRef, HwFramesDeleter>;
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;

struct Candidate {
    std::uint32_t fourcc = 0;
    std::uint64_t modifier = 0;
};

struct PoolBuffer {
    GbmBoPtr bo;
    Fd dma_buf;
    fluxrec::capture::DmabufImage image;
};

struct Options {
    std::string render_node = "/dev/dri/renderD128";
    std::string output_name = "DP-1";
    std::string shader_directory = FLUXREC_FRAME_IDENTITY_SHADER_DIR;
    std::chrono::seconds duration{8};
    bool measure_gpu_time = false;
};

struct GpuTimingSummary {
    std::uint64_t samples = 0;
    std::uint64_t total_ns = 0;
    std::uint64_t minimum_ns = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t maximum_ns = 0;
};

std::string fourcc_string(std::uint32_t value) {
    std::array<char, 5> chars{
        static_cast<char>(value & 0xffU),
        static_cast<char>((value >> 8U) & 0xffU),
        static_cast<char>((value >> 16U) & 0xffU),
        static_cast<char>((value >> 24U) & 0xffU),
        '\0',
    };
    for (std::size_t i = 0; i < 4; ++i) {
        const auto c = static_cast<unsigned char>(chars[i]);
        if (c < 0x20U || c > 0x7eU) chars[i] = '?';
    }
    return std::string(chars.data(), 4);
}

std::string av_error_string(int error) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
    av_strerror(error, buffer.data(), buffer.size());
    return buffer.data();
}

std::optional<std::uint64_t> dma_buf_size(int fd) {
    errno = 0;
    const off_t end = ::lseek(fd, 0, SEEK_END);
    if (end <= 0) return std::nullopt;
    return static_cast<std::uint64_t>(end);
}

void free_descriptor_buffer(void*, std::uint8_t* data) {
    auto* descriptor = reinterpret_cast<AVDRMFrameDescriptor*>(data);
    for (int i = 0; i < descriptor->nb_objects; ++i) {
        if (descriptor->objects[i].fd >= 0) ::close(descriptor->objects[i].fd);
    }
    av_free(descriptor);
}

void print_usage(const char* program) {
    std::fprintf(stderr,
                 "Usage: %s [--device /dev/dri/renderD128] [--output DP-1] "
                 "[--seconds N] [--shader-dir PATH] [--measure-gpu-time]\n"
                 "Capture live Wayland frames and read back only one 16-byte GPU digest per frame.\n"
                 "Move or animate desktop content during the run; exit status 0 requires at least\n"
                 "two distinct frame digests.\n",
                 program);
}

bool parse_options(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument(argv[i]);
        if (argument == "--help") {
            print_usage(argv[0]);
            return false;
        }
        if ((argument == "--device" || argument == "--output" || argument == "--seconds" ||
             argument == "--shader-dir") && i + 1 >= argc) {
            std::fprintf(stderr, "Missing value for %s\n", argv[i]);
            return false;
        }
        if (argument == "--device") {
            options.render_node = argv[++i];
        } else if (argument == "--output") {
            options.output_name = argv[++i];
        } else if (argument == "--shader-dir") {
            options.shader_directory = argv[++i];
        } else if (argument == "--seconds") {
            char* end = nullptr;
            errno = 0;
            const long value = std::strtol(argv[++i], &end, 10);
            if (errno != 0 || end == argv[i] || *end != '\0' || value < 1 || value > 3600) {
                std::fprintf(stderr, "--seconds must be an integer in 1..3600\n");
                return false;
            }
            options.duration = std::chrono::seconds(value);
        } else if (argument == "--measure-gpu-time") {
            options.measure_gpu_time = true;
        } else if (argument != "--device" && argument != "--output" &&
                   argument != "--seconds" && argument != "--shader-dir") {
            std::fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            return false;
        }
    }
    return true;
}

std::expected<FramePtr, std::string> make_drm_frame(
    const fluxrec::capture::CapturedFrame& captured) {
    const auto& image = captured.lease.image();
    if (image.planes.size() != 1 ||
        (image.fourcc != DRM_FORMAT_ARGB8888 && image.fourcc != DRM_FORMAT_XRGB8888)) {
        return std::unexpected("capture lease is not one-plane AR24/XR24");
    }
    auto plane_fd_result = captured.lease.duplicate_plane_fd(0);
    if (!plane_fd_result)
        return std::unexpected("duplicating captured DMA-BUF fd failed: " +
                               plane_fd_result.error().message);
    auto plane_fd = std::move(*plane_fd_result);
    const auto size = dma_buf_size(plane_fd.get());
    if (!size || *size > std::numeric_limits<std::size_t>::max())
        return std::unexpected("DMA-BUF size is unavailable or exceeds AVDRMFrameDescriptor limits");

    auto* descriptor = static_cast<AVDRMFrameDescriptor*>(av_mallocz(sizeof(AVDRMFrameDescriptor)));
    if (!descriptor) return std::unexpected("allocating AVDRMFrameDescriptor failed");
    descriptor->nb_objects = 1;
    descriptor->objects[0].fd = plane_fd.release();
    descriptor->objects[0].size = static_cast<std::size_t>(*size);
    descriptor->objects[0].format_modifier = image.modifier;
    descriptor->nb_layers = 1;
    descriptor->layers[0].format = image.fourcc;
    descriptor->layers[0].nb_planes = 1;
    descriptor->layers[0].planes[0].object_index = 0;
    descriptor->layers[0].planes[0].offset = static_cast<std::ptrdiff_t>(image.planes[0].offset);
    descriptor->layers[0].planes[0].pitch = static_cast<std::ptrdiff_t>(image.planes[0].stride);

    FramePtr frame(av_frame_alloc());
    if (!frame) {
        free_descriptor_buffer(nullptr, reinterpret_cast<std::uint8_t*>(descriptor));
        return std::unexpected("allocating DRM_PRIME AVFrame failed");
    }
    frame->format = AV_PIX_FMT_DRM_PRIME;
    frame->width = static_cast<int>(image.width);
    frame->height = static_cast<int>(image.height);
    frame->data[0] = reinterpret_cast<std::uint8_t*>(descriptor);
    frame->buf[0] = av_buffer_create(reinterpret_cast<std::uint8_t*>(descriptor),
                                     sizeof(*descriptor), free_descriptor_buffer, nullptr, 0);
    if (!frame->buf[0]) {
        free_descriptor_buffer(nullptr, reinterpret_cast<std::uint8_t*>(descriptor));
        return std::unexpected("wrapping DRM_PRIME descriptor in AVBufferRef failed");
    }
    return frame;
}

bool allocate_capture_pool(const fluxrec::capture::CaptureRequirements& requirements,
                           const std::string& render_node,
                           std::vector<PoolBuffer>& buffers,
                           Candidate& selected) {
    Fd render_fd(::open(render_node.c_str(), O_RDWR | O_CLOEXEC));
    if (!render_fd) {
        std::fprintf(stderr, "Could not open %s: %s\n", render_node.c_str(), std::strerror(errno));
        return false;
    }
    struct stat render_stat {};
    if (::fstat(render_fd.get(), &render_stat) != 0) {
        std::fprintf(stderr, "Could not stat %s: %s\n", render_node.c_str(), std::strerror(errno));
        return false;
    }
    if (requirements.dmabuf_device &&
        static_cast<std::uint64_t>(render_stat.st_rdev) != *requirements.dmabuf_device) {
        std::fprintf(stderr,
                     "Capture device dev_t=%" PRIu64 " does not match %s dev_t=%u.\n",
                     *requirements.dmabuf_device, render_node.c_str(),
                     static_cast<unsigned int>(render_stat.st_rdev));
        return false;
    }

    GbmDevicePtr gbm(gbm_create_device(render_fd.get()));
    if (!gbm) {
        std::fprintf(stderr, "gbm_create_device(%s) failed.\n", render_node.c_str());
        return false;
    }
    std::vector<Candidate> candidates;
    for (const auto& format : requirements.dmabuf_formats) {
        if (format.fourcc == DRM_FORMAT_ARGB8888 || format.fourcc == DRM_FORMAT_XRGB8888)
            candidates.push_back({format.fourcc, format.modifier});
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& lhs,
                                                              const Candidate& rhs) {
        return lhs.fourcc == DRM_FORMAT_ARGB8888 && rhs.fourcc != DRM_FORMAT_ARGB8888;
    });
    if (candidates.empty()) {
        std::fprintf(stderr, "Wayland capture did not advertise AR24/XR24 for this output.\n");
        return false;
    }

    buffers.reserve(kPoolSize);
    for (std::size_t slot = 0; slot < kPoolSize; ++slot) {
        PoolBuffer buffer;
        bool allocated = false;
        for (const Candidate& candidate : candidates) {
            const std::uint64_t modifier = candidate.modifier;
            buffer.bo.reset(gbm_bo_create_with_modifiers2(
                gbm.get(), requirements.width, requirements.height, candidate.fourcc,
                &modifier, 1U, GBM_BO_USE_RENDERING));
            if (!buffer.bo) continue;
            if (gbm_bo_get_modifier(buffer.bo.get()) != candidate.modifier ||
                gbm_bo_get_format(buffer.bo.get()) != candidate.fourcc ||
                gbm_bo_get_plane_count(buffer.bo.get()) != 1) {
                buffer.bo.reset();
                continue;
            }
            selected = candidate;
            allocated = true;
            break;
        }
        if (!allocated) {
            std::fprintf(stderr, "GBM could not allocate capture slot %zu with an advertised AR24/XR24 modifier.\n",
                         slot);
            return false;
        }
        buffer.dma_buf.reset(gbm_bo_get_fd_for_plane(buffer.bo.get(), 0));
        if (!buffer.dma_buf) {
            std::fprintf(stderr, "gbm_bo_get_fd_for_plane failed: %s\n", std::strerror(errno));
            return false;
        }
        buffer.image.width = requirements.width;
        buffer.image.height = requirements.height;
        buffer.image.fourcc = selected.fourcc;
        buffer.image.modifier = selected.modifier;
        buffer.image.planes.push_back({buffer.dma_buf.get(), gbm_bo_get_offset(buffer.bo.get(), 0),
                                       gbm_bo_get_stride_for_plane(buffer.bo.get(), 0)});
        buffers.push_back(std::move(buffer));
    }
    return true;
}

bool collect_completed(fluxrec::gpu::FrameIdentity& identity,
                       std::map<std::uint64_t, fluxrec::capture::FrameLease>& pending,
                       std::set<std::array<std::uint32_t, 4>>& unique,
                       std::uint64_t& completed_count,
                       std::uint64_t& distinct_transitions,
                       std::optional<std::array<std::uint32_t, 4>>& last_digest,
                       GpuTimingSummary& gpu_timing) {
    auto result = identity.poll();
    if (!result) {
        std::fprintf(stderr, "FrameIdentity::poll failed: %s\n", result.error().message.c_str());
        return false;
    }
    for (const auto& digest : *result) {
        ++completed_count;
        unique.insert(digest.words);
        const bool changed = !last_digest || *last_digest != digest.words;
        if (changed) {
            if (last_digest) ++distinct_transitions;
            std::printf("digest change seq=%" PRIu64 " %08" PRIx32 "%08" PRIx32
                        "%08" PRIx32 "%08" PRIx32,
                        digest.sequence, digest.words[0], digest.words[1], digest.words[2],
                        digest.words[3]);
            if (digest.compared_to_sequence) {
                std::printf(" compared_to=%" PRIu64 " adjacent=%s",
                            *digest.compared_to_sequence,
                            digest.comparison_is_adjacent_capture ? "yes" : "no");
            }
            std::putchar('\n');
        }
        last_digest = digest.words;
        if (digest.gpu_time_ns) {
            ++gpu_timing.samples;
            gpu_timing.total_ns += *digest.gpu_time_ns;
            gpu_timing.minimum_ns = std::min(gpu_timing.minimum_ns, *digest.gpu_time_ns);
            gpu_timing.maximum_ns = std::max(gpu_timing.maximum_ns, *digest.gpu_time_ns);
        }
        const auto lease = pending.find(digest.sequence);
        if (lease != pending.end()) {
            lease->second.release_after_gpu();
            pending.erase(lease);
        }
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        print_usage(argv[0]);
        return 0;
    }
    Options options;
    if (!parse_options(argc, argv, options)) return 2;

    fluxrec::capture::CaptureConfig capture_config;
    capture_config.output_name = options.output_name;
    capture_config.frame_queue_capacity = kPoolSize;
    auto capture_result = fluxrec::capture::WaylandCapture::connect(capture_config);
    if (!capture_result) {
        std::fprintf(stderr, "Wayland capture negotiation failed: %s (system error %d)\n",
                     capture_result.error().message.c_str(), capture_result.error().system_error);
        return 1;
    }
    auto capture = std::move(*capture_result);
    const auto& requirements = capture.requirements();
    std::printf("Output %s: %ux%u; advertised formats=%zu\n", options.output_name.c_str(),
                requirements.width, requirements.height, requirements.dmabuf_formats.size());

    std::vector<PoolBuffer> pool;
    Candidate selected;
    if (!allocate_capture_pool(requirements, options.render_node, pool, selected)) return 1;
    std::vector<fluxrec::capture::DmabufImage> images;
    images.reserve(pool.size());
    for (const auto& buffer : pool) images.push_back(buffer.image);
    auto pool_result = capture.set_buffer_pool(images);
    if (!pool_result) {
        std::fprintf(stderr, "Wayland capture buffer pool setup failed: %s (system error %d)\n",
                     pool_result.error().message.c_str(), pool_result.error().system_error);
        return 1;
    }
    std::printf("Capture DMA-BUF pool: %zu x %s modifier=0x%016" PRIx64
                "; capture never maps pixels to CPU memory.\n",
                pool.size(), fourcc_string(selected.fourcc).c_str(), selected.modifier);

    av_log_set_level(AV_LOG_WARNING);
    AVBufferRef* device_raw = nullptr;
    int result = av_hwdevice_ctx_create(&device_raw, AV_HWDEVICE_TYPE_VULKAN, "0", nullptr, 0);
    if (result < 0) {
        std::fprintf(stderr, "FFmpeg Vulkan device creation failed: %s (%d)\n",
                     av_error_string(result).c_str(), result);
        return 1;
    }
    HwDeviceRef device(device_raw);
    auto* device_context = reinterpret_cast<AVHWDeviceContext*>(device->data);
    auto* vulkan_context = static_cast<AVVulkanDeviceContext*>(device_context->hwctx);
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(vulkan_context->phys_dev, &properties);
    std::printf("FFmpeg Vulkan device: %s (vendor=0x%04x device=0x%04x)\n",
                properties.deviceName, properties.vendorID, properties.deviceID);

    AVBufferRef* frames_raw = av_hwframe_ctx_alloc(device.get());
    if (!frames_raw) {
        std::fprintf(stderr, "Allocating FFmpeg Vulkan frames context failed.\n");
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
        std::fprintf(stderr, "FFmpeg Vulkan frames context init failed: %s (%d)\n",
                     av_error_string(result).c_str(), result);
        return 1;
    }

    fluxrec::gpu::FrameIdentityOptions identity_options;
    identity_options.width = requirements.width;
    identity_options.height = requirements.height;
    identity_options.shader_directory = options.shader_directory;
    identity_options.ring_size = kPoolSize;
    identity_options.measure_gpu_time = options.measure_gpu_time;
    auto identity_result = fluxrec::gpu::FrameIdentity::create(device.get(), identity_options);
    if (!identity_result) {
        std::fprintf(stderr, "FrameIdentity initialization failed: %s (code %d)\n",
                     identity_result.error().message.c_str(), identity_result.error().code);
        return 1;
    }
    auto identity = std::move(*identity_result);

    auto start_result = capture.start();
    if (!start_result) {
        std::fprintf(stderr, "Starting Wayland capture failed: %s (system error %d)\n",
                     start_result.error().message.c_str(), start_result.error().system_error);
        return 1;
    }

    std::map<std::uint64_t, fluxrec::capture::FrameLease> pending;
    std::set<std::array<std::uint32_t, 4>> unique_digests;
    std::optional<std::array<std::uint32_t, 4>> last_digest;
    GpuTimingSummary gpu_timing;
    std::uint64_t submitted_count = 0;
    std::uint64_t skipped_count = 0;
    std::uint64_t completed_count = 0;
    std::uint64_t distinct_transitions = 0;
    bool failed = false;
    const auto stop_at = std::chrono::steady_clock::now() + options.duration;
    while (std::chrono::steady_clock::now() < stop_at) {
        auto frame = capture.wait_pop_frame(std::chrono::milliseconds(20));
        if (frame) {
            auto drm_frame = make_drm_frame(*frame);
            if (!drm_frame) {
                std::fprintf(stderr, "Building DRM_PRIME frame failed: %s\n", drm_frame.error().c_str());
                failed = true;
                break;
            }
            FramePtr vulkan_frame(av_frame_alloc());
            if (!vulkan_frame) {
                std::fprintf(stderr, "Allocating imported Vulkan AVFrame failed.\n");
                failed = true;
                break;
            }
            vulkan_frame->format = AV_PIX_FMT_VULKAN;
            vulkan_frame->hw_frames_ctx = av_buffer_ref(frames.get());
            if (!vulkan_frame->hw_frames_ctx) {
                std::fprintf(stderr, "Retaining imported Vulkan frames context failed.\n");
                failed = true;
                break;
            }
            result = av_hwframe_map(vulkan_frame.get(), drm_frame->get(),
                                    AV_HWFRAME_MAP_READ | AV_HWFRAME_MAP_DIRECT);
            if (result < 0) {
                std::fprintf(stderr, "Direct DRM_PRIME -> Vulkan import failed: %s (%d)\n",
                             av_error_string(result).c_str(), result);
                failed = true;
                break;
            }
            auto submit_result = identity.try_submit(vulkan_frame.get(), frame->sequence);
            if (!submit_result) {
                std::fprintf(stderr, "FrameIdentity submit failed: %s (code %d)\n",
                             submit_result.error().message.c_str(), submit_result.error().code);
                failed = true;
                break;
            }
            if (*submit_result == fluxrec::gpu::FrameIdentitySubmit::submitted) {
                pending.emplace(frame->sequence, std::move(frame->lease));
                ++submitted_count;
            } else {
                ++skipped_count;
            }
        }
        if (!collect_completed(identity, pending, unique_digests, completed_count,
                               distinct_transitions, last_digest, gpu_timing)) {
            failed = true;
            break;
        }
        if (capture.status() == fluxrec::capture::CaptureStatus::failed) {
            const auto error = capture.runtime_error();
            std::fprintf(stderr, "Capture failed: %s\n", error ? error->message.c_str() : "unknown error");
            failed = true;
            break;
        }
    }

    capture.stop();
    const auto drain_result = identity.drain();
    if (!drain_result) {
        std::fprintf(stderr, "Draining FrameIdentity failed: %s (code %d)\n",
                     drain_result.error().message.c_str(), drain_result.error().code);
        failed = true;
    } else if (!collect_completed(identity, pending, unique_digests, completed_count,
                                  distinct_transitions, last_digest, gpu_timing)) {
        failed = true;
    }
    pending.clear();
    const auto stats = capture.stats();
    std::printf("Result: captured=%" PRIu64 " submitted=%" PRIu64 " digests=%" PRIu64
                " ring_skips=%" PRIu64 " unique_digests=%zu distinct_transitions=%" PRIu64
                " pool_drops=%" PRIu64 "\n",
                stats.completed, submitted_count, completed_count, skipped_count,
                unique_digests.size(), distinct_transitions, stats.dropped);
    if (options.measure_gpu_time && gpu_timing.samples != 0) {
        std::printf("Digest GPU time: samples=%" PRIu64 " min=%" PRIu64 " ns mean=%" PRIu64
                    " ns max=%" PRIu64 " ns; readback per completed frame is 16 bytes.\n",
                    gpu_timing.samples, gpu_timing.minimum_ns,
                    gpu_timing.total_ns / gpu_timing.samples, gpu_timing.maximum_ns);
    }
    if (failed) return 1;
    if (completed_count == 0) {
        std::fprintf(stderr, "No digest completed; check output, device, modifier and shader paths.\n");
        return 1;
    }
    if (unique_digests.size() < 2) {
        std::fprintf(stderr,
                     "Only one digest was observed. Animate or move visible desktop content and rerun.\n");
        return 1;
    }
    return 0;
}
