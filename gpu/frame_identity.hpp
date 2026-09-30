#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct AVBufferRef;
struct AVFrame;

namespace fluxrec::gpu {

struct FrameIdentityError {
    std::string message;
    int code = 0;
};

enum class FrameIdentityMode {
    digest,
    source_pass_only,
    unrelated_workload,
};

struct FrameIdentityOptions {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::filesystem::path shader_directory = "build/gpu/shaders";
    std::size_t ring_size = 8;
    bool measure_gpu_time = false;
    FrameIdentityMode mode = FrameIdentityMode::digest;
    std::uint32_t unrelated_iterations = 1;
};

enum class FrameIdentitySubmit {
    submitted,
    skipped_ring_full,
};

struct FrameIdentityDigest {
    std::uint64_t sequence = 0;
    std::array<std::uint32_t, 4> words{};
    // False only in source_pass_only mode, where the source-read pass is
    // timed but no reduced digest is copied to host memory.
    bool digest_available = true;
    std::optional<std::uint64_t> compared_to_sequence;
    bool same_as_compared_digest = false;
    bool comparison_is_adjacent_capture = false;
    std::optional<std::uint64_t> gpu_time_ns;
};

// Optional, asynchronous validation utility. It hashes all visible RGB pixels
// from an FFmpeg AV_PIX_FMT_VULKAN input frame and reads back only 16 digest
// bytes. Submit frames before they enter the Vulkan conversion graph. The
// caller continues to own capture/drop policy; a full digest ring skips hashing
// rather than stalling capture.
class FrameIdentity final {
public:
    FrameIdentity() noexcept = default;
    ~FrameIdentity();

    FrameIdentity(FrameIdentity&&) noexcept;
    FrameIdentity& operator=(FrameIdentity&&) noexcept;
    FrameIdentity(const FrameIdentity&) = delete;
    FrameIdentity& operator=(const FrameIdentity&) = delete;

    [[nodiscard]] static std::expected<FrameIdentity, FrameIdentityError> create(
        AVBufferRef* vulkan_device, FrameIdentityOptions options);

    // Nonblocking. Returns skipped_ring_full if all readback slots are still in
    // flight. `sequence` must increase monotonically even when a digest is
    // skipped, so adjacency reporting remains accurate.
    [[nodiscard]] std::expected<FrameIdentitySubmit, FrameIdentityError> try_submit(
        const AVFrame* source, std::uint64_t sequence);

    // Nonblocking collection. The only mapped GPU data is each completed
    // slot's 16-byte digest readback.
    [[nodiscard]] std::expected<std::vector<FrameIdentityDigest>, FrameIdentityError> poll();

    // Explicitly waits for this utility's submissions during shutdown, then
    // makes their digest records available through poll().
    [[nodiscard]] std::expected<void, FrameIdentityError> drain();

    [[nodiscard]] std::size_t in_flight() const noexcept;
    [[nodiscard]] std::size_t ring_size() const noexcept;

private:
    struct Impl;
    explicit FrameIdentity(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace fluxrec::gpu
