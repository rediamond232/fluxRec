#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <variant>

namespace fluxrec::preview {

inline constexpr std::uint16_t protocol_major = 1;
inline constexpr std::uint16_t protocol_minor = 0;
inline constexpr std::size_t max_preview_slots = 8;
inline constexpr std::size_t max_wire_bytes = 512;
inline constexpr std::size_t max_error_text_bytes = 256;
inline constexpr std::uint32_t max_image_dimension = 16'384;
inline constexpr std::uint64_t max_image_pixels = 64ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t max_allocation_bytes = 512ULL * 1024ULL * 1024ULL;

[[nodiscard]] constexpr std::uint32_t drm_fourcc(char a, char b, char c, char d) noexcept {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(a)) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 8U) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 16U) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(d)) << 24U);
}

// DRM_FORMAT_ABGR8888 is byte-ordered RGBA on little-endian systems and maps
// to VK_FORMAT_R8G8B8A8_UNORM for the current Qt preview output.
inline constexpr std::uint32_t drm_format_abgr8888 = drm_fourcc('A', 'B', '2', '4');
inline constexpr std::uint64_t drm_format_modifier_invalid = UINT64_MAX;

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

enum class ErrorCode {
    would_block,
    peer_closed,
    system_error,
    protocol_error,
    invalid_metadata,
};

struct Error {
    ErrorCode code = ErrorCode::system_error;
    int system_error = 0;
    std::string message;
};

struct DmabufFrame {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t fourcc = 0;
    std::uint64_t modifier = 0;
    std::uint64_t offset = 0;
    std::uint32_t stride = 0;
    std::uint64_t allocation_size = 0;
    UniqueFd buffer_fd;
};

struct Hello {
    std::uint64_t session_id = 0;
};

struct PoolBegin {
    std::uint64_t generation = 0;
    std::uint32_t slot_count = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t fourcc = 0;
    std::uint64_t modifier = 0;
    std::array<std::uint8_t, 16> device_uuid{};
    std::uint32_t drm_major = 0;
    std::uint32_t drm_minor = 0;
};

struct PoolSlot {
    std::uint64_t generation = 0;
    std::uint32_t slot_count = 0;
    std::uint32_t slot_index = 0;
    DmabufFrame image;
};

struct PoolReady {
    std::uint64_t generation = 0;
    std::uint32_t slot_count = 0;
};

struct FrameReady {
    std::uint64_t generation = 0;
    std::uint32_t slot_count = 0;
    std::uint32_t slot_index = 0;
    std::uint64_t sequence = 0;
    std::uint64_t presentation_time_ns = 0;
    bool has_presentation_time = false;
    // Exported Vulkan SYNC_FD acquire fence for the preview write.
    UniqueFd acquire_fence_fd;
};

struct FrameRelease {
    std::uint64_t generation = 0;
    std::uint32_t slot_count = 0;
    std::uint32_t slot_index = 0;
    std::uint64_t sequence = 0;
    // Qt Vulkan SYNC_FD release fence, or an already-signaled pollable FD when
    // Vulkan reports that the completion point has already passed. Consumers
    // may poll this FD but must not import the fallback as a Vulkan semaphore.
    UniqueFd release_fence_fd;
};

struct Stop {
    std::uint64_t generation = 0;
};

struct PeerError {
    std::uint32_t code = 0;
    std::string message;
};

using Message = std::variant<Hello, PoolBegin, PoolSlot, PoolReady, FrameReady,
                             FrameRelease, Stop, PeerError>;

[[nodiscard]] std::expected<void, Error> validate(const Message& message);
[[nodiscard]] std::expected<void, Error> validate_pool_slot(
    const PoolBegin& pool, const PoolSlot& slot);

class PoolDescriptor {
public:
    PoolDescriptor() noexcept = default;

    [[nodiscard]] std::expected<void, Error> begin(const PoolBegin& pool);
    [[nodiscard]] std::expected<void, Error> add_slot(const PoolSlot& slot);
    [[nodiscard]] std::expected<void, Error> mark_ready(const PoolReady& ready);
    [[nodiscard]] std::expected<void, Error> accept_frame(const FrameReady& frame);
    [[nodiscard]] std::expected<void, Error> accept_release(const FrameRelease& release);

    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] std::uint64_t generation() const noexcept;
    [[nodiscard]] std::uint32_t slot_count() const noexcept;

private:
    PoolBegin pool_{};
    std::array<bool, max_preview_slots> described_{};
    std::array<bool, max_preview_slots> in_use_{};
    std::array<std::uint64_t, max_preview_slots> last_sequence_{};
    std::size_t described_count_ = 0;
    bool active_ = false;
    bool ready_ = false;
};

class SeqPacketSocket {
public:
    SeqPacketSocket() noexcept = default;

    [[nodiscard]] static std::expected<SeqPacketSocket, Error> adopt(UniqueFd fd);
    [[nodiscard]] static std::expected<std::array<SeqPacketSocket, 2>, Error> pair();

    [[nodiscard]] explicit operator bool() const noexcept;
    [[nodiscard]] int fd() const noexcept;
    [[nodiscard]] std::expected<void, Error> send(const Message& message) const;
    [[nodiscard]] std::expected<Message, Error> receive() const;

private:
    explicit SeqPacketSocket(UniqueFd fd) noexcept;
    UniqueFd fd_;
};

} // namespace fluxrec::preview
