#pragma once

#include "gpu/gpu.hpp"

#include <cstdint>
#include <expected>
#include <memory>
#include <string>

struct AVBufferRef;
struct AVFrame;

namespace fluxrec::gpu {

struct PreviewProducerConfig {
    std::string socket_path;
    std::uint32_t source_width = 0;
    std::uint32_t source_height = 0;
    std::uint32_t width = 960;
    std::uint32_t height = 540;
    std::uint32_t max_fps = 60;
    std::uint32_t slot_count = 3;
    std::uint64_t session_id = 0;
};

struct PreviewProducerStats {
    bool enabled = false;
    std::uint64_t frames_submitted = 0;
    std::uint64_t frames_rate_limited = 0;
    std::uint64_t frames_no_free_slot = 0;
    std::uint64_t frames_source_unavailable = 0;
    std::uint64_t frames_transport_dropped = 0;
    std::uint64_t frames_published = 0;
    std::string last_error;
};

// Optional, GPU-only preview producer. The caller supplies a converted
// single-image Vulkan NV12 frame; the producer retains an AVFrame reference
// through its local write fence. Preview failures are isolated from recording:
// submit() accounts for the failure and disables only this optional channel.
class PreviewProducer final {
public:
    PreviewProducer() noexcept = default;
    ~PreviewProducer();

    PreviewProducer(PreviewProducer&&) noexcept;
    PreviewProducer& operator=(PreviewProducer&&) noexcept;

    PreviewProducer(const PreviewProducer&) = delete;
    PreviewProducer& operator=(const PreviewProducer&) = delete;

    [[nodiscard]] static std::expected<PreviewProducer, GpuError> connect(
        AVBufferRef* vulkan_device_ref, PreviewProducerConfig config);

    // Services nonblocking socket messages and release-fence readiness even
    // when the FPS limiter decides not to render a preview for this frame.
    // This method never waits for the UI or on a GPU fence.
    void submit(AVFrame* source, std::uint64_t sequence,
                std::uint64_t presentation_time_ns,
                bool has_presentation_time) noexcept;

    [[nodiscard]] PreviewProducerStats stats() const;

private:
    struct Impl;
    explicit PreviewProducer(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace fluxrec::gpu
