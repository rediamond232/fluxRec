#include "preview/preview.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <type_traits>
#include <utility>

namespace fluxrec::preview {
namespace {

constexpr std::size_t header_bytes = 20;
constexpr std::size_t max_rights = 4;
constexpr std::uint32_t timestamp_present = 1U;

enum class WireType : std::uint16_t {
    hello = 1,
    pool_begin = 2,
    pool_slot = 3,
    pool_ready = 4,
    frame_ready = 5,
    frame_release = 6,
    stop = 7,
    peer_error = 8,
};

Error error(ErrorCode code, std::string message, int system_error = 0) {
    return Error{code, system_error, std::move(message)};
}

bool descriptor_is_open(int fd) noexcept {
    return fd >= 0 && ::fcntl(fd, F_GETFD) >= 0;
}

bool uuid_is_nonzero(const std::array<std::uint8_t, 16>& uuid) noexcept {
    return std::any_of(uuid.begin(), uuid.end(), [](std::uint8_t value) {
        return value != 0;
    });
}

std::expected<void, Error> validate_pool_begin(const PoolBegin& pool) {
    if (pool.generation == 0) {
        return std::unexpected(error(ErrorCode::invalid_metadata,
                                     "preview pool generation must be nonzero"));
    }
    if (pool.slot_count < 2 || pool.slot_count > max_preview_slots) {
        return std::unexpected(error(ErrorCode::invalid_metadata,
                                     "preview pool slot count is outside [2, 8]"));
    }
    if (pool.width == 0 || pool.height == 0 ||
        pool.width > max_image_dimension || pool.height > max_image_dimension ||
        static_cast<std::uint64_t>(pool.width) * pool.height > max_image_pixels) {
        return std::unexpected(error(ErrorCode::invalid_metadata,
                                     "preview dimensions are zero or exceed the configured limits"));
    }
    if (pool.fourcc != drm_format_abgr8888) {
        return std::unexpected(error(ErrorCode::invalid_metadata,
                                     "preview pool format must be DRM_FORMAT_ABGR8888"));
    }
    if (pool.modifier == drm_format_modifier_invalid) {
        return std::unexpected(error(ErrorCode::invalid_metadata,
                                     "preview pool modifier must be negotiated, not INVALID"));
    }
    if (!uuid_is_nonzero(pool.device_uuid)) {
        return std::unexpected(error(ErrorCode::invalid_metadata,
                                     "preview pool device UUID must not be all zero"));
    }
    if (pool.drm_major == 0 && pool.drm_minor == 0) {
        return std::unexpected(error(ErrorCode::invalid_metadata,
                                     "preview pool DRM device identity must not be zero"));
    }
    return {};
}

std::expected<void, Error> validate_image(const DmabufFrame& image) {
    if (!descriptor_is_open(image.buffer_fd.get())) {
        return std::unexpected(error(ErrorCode::invalid_metadata,
                                     "preview DMA-BUF descriptor is not open"));
    }
    if (image.width == 0 || image.height == 0 ||
        image.width > max_image_dimension || image.height > max_image_dimension ||
        static_cast<std::uint64_t>(image.width) * image.height > max_image_pixels) {
        return std::unexpected(error(ErrorCode::invalid_metadata,
                                     "preview image dimensions are zero or exceed the configured limits"));
    }
    if (image.fourcc != drm_format_abgr8888) {
        return std::unexpected(error(ErrorCode::invalid_metadata,
                                     "preview image format must be DRM_FORMAT_ABGR8888"));
    }
    if (image.modifier == drm_format_modifier_invalid) {
        return std::unexpected(error(ErrorCode::invalid_metadata,
                                     "preview image modifier must be negotiated, not INVALID"));
    }
    if (image.offset % 4U != 0 || image.stride % 4U != 0) {
        return std::unexpected(error(ErrorCode::invalid_metadata,
                                     "preview image offset and stride must be four-byte aligned"));
    }
    if (image.allocation_size == 0 || image.allocation_size > max_allocation_bytes) {
        return std::unexpected(error(ErrorCode::invalid_metadata,
                                     "preview allocation size is zero or exceeds 512 MiB"));
    }

    const std::uint64_t row_bytes = static_cast<std::uint64_t>(image.width) * 4U;
    if (image.stride < row_bytes) {
        return std::unexpected(error(ErrorCode::invalid_metadata,
                                     "preview image stride is shorter than one RGBA row"));
    }
    const std::uint64_t rows_before_last = static_cast<std::uint64_t>(image.height - 1U);
    const std::uint64_t stride = image.stride;
    if (rows_before_last != 0 && stride >
        (std::numeric_limits<std::uint64_t>::max() - row_bytes) / rows_before_last) {
        return std::unexpected(error(ErrorCode::invalid_metadata,
                                     "preview image plane extent overflows"));
    }
    const std::uint64_t plane_bytes = rows_before_last * stride + row_bytes;
    if (image.offset > std::numeric_limits<std::uint64_t>::max() - plane_bytes ||
        image.offset + plane_bytes > image.allocation_size) {
        return std::unexpected(error(ErrorCode::invalid_metadata,
                                     "preview image plane extent exceeds its allocation"));
    }
    return {};
}

std::expected<void, Error> validate_frame_ready(const FrameReady& frame) {
    if (frame.generation == 0 || frame.slot_count < 2 ||
        frame.slot_count > max_preview_slots || frame.slot_index >= frame.slot_count ||
        frame.sequence == 0 || !descriptor_is_open(frame.acquire_fence_fd.get())) {
        return std::unexpected(error(ErrorCode::invalid_metadata,
                                     "frame generation, slot, sequence, or acquire fence is invalid"));
    }
    if ((frame.has_presentation_time && frame.presentation_time_ns == 0) ||
        (!frame.has_presentation_time && frame.presentation_time_ns != 0)) {
        return std::unexpected(error(ErrorCode::invalid_metadata,
                                     "frame presentation timestamp and presence flag disagree"));
    }
    return {};
}

std::expected<void, Error> validate_frame_release(const FrameRelease& release) {
    if (release.generation == 0 || release.slot_count < 2 ||
        release.slot_count > max_preview_slots || release.slot_index >= release.slot_count ||
        release.sequence == 0 || !descriptor_is_open(release.release_fence_fd.get())) {
        return std::unexpected(error(ErrorCode::invalid_metadata,
                                     "release generation, slot, sequence, or release fence is invalid"));
    }
    return {};
}

class Writer {
public:
    [[nodiscard]] bool u8(std::uint8_t value) noexcept {
        return raw(&value, sizeof(value));
    }

    [[nodiscard]] bool u16(std::uint16_t value) noexcept {
        const std::array<std::uint8_t, 2> bytes{
            static_cast<std::uint8_t>(value),
            static_cast<std::uint8_t>(value >> 8U),
        };
        return raw(bytes.data(), bytes.size());
    }

    [[nodiscard]] bool u32(std::uint32_t value) noexcept {
        const std::array<std::uint8_t, 4> bytes{
            static_cast<std::uint8_t>(value),
            static_cast<std::uint8_t>(value >> 8U),
            static_cast<std::uint8_t>(value >> 16U),
            static_cast<std::uint8_t>(value >> 24U),
        };
        return raw(bytes.data(), bytes.size());
    }

    [[nodiscard]] bool u64(std::uint64_t value) noexcept {
        const std::array<std::uint8_t, 8> bytes{
            static_cast<std::uint8_t>(value),
            static_cast<std::uint8_t>(value >> 8U),
            static_cast<std::uint8_t>(value >> 16U),
            static_cast<std::uint8_t>(value >> 24U),
            static_cast<std::uint8_t>(value >> 32U),
            static_cast<std::uint8_t>(value >> 40U),
            static_cast<std::uint8_t>(value >> 48U),
            static_cast<std::uint8_t>(value >> 56U),
        };
        return raw(bytes.data(), bytes.size());
    }

    [[nodiscard]] bool raw(const void* bytes, std::size_t count) noexcept {
        if (count > storage_.size() - size_) return false;
        std::memcpy(storage_.data() + size_, bytes, count);
        size_ += count;
        return true;
    }

    [[nodiscard]] bool text(const std::string& value) noexcept {
        return raw(value.data(), value.size());
    }

    [[nodiscard]] const std::byte* data() const noexcept { return storage_.data(); }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }

private:
    std::array<std::byte, max_wire_bytes> storage_{};
    std::size_t size_ = 0;
};

class Reader {
public:
    explicit Reader(std::span<const std::byte> bytes) noexcept : bytes_(bytes) {}

    [[nodiscard]] bool u8(std::uint8_t& value) noexcept {
        return raw(&value, sizeof(value));
    }

    [[nodiscard]] bool u16(std::uint16_t& value) noexcept {
        std::array<std::uint8_t, 2> bytes{};
        if (!raw(bytes.data(), bytes.size())) return false;
        value = static_cast<std::uint16_t>(bytes[0]) |
            static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[1]) << 8U);
        return true;
    }

    [[nodiscard]] bool u32(std::uint32_t& value) noexcept {
        std::array<std::uint8_t, 4> bytes{};
        if (!raw(bytes.data(), bytes.size())) return false;
        value = static_cast<std::uint32_t>(bytes[0]) |
            (static_cast<std::uint32_t>(bytes[1]) << 8U) |
            (static_cast<std::uint32_t>(bytes[2]) << 16U) |
            (static_cast<std::uint32_t>(bytes[3]) << 24U);
        return true;
    }

    [[nodiscard]] bool u64(std::uint64_t& value) noexcept {
        std::array<std::uint8_t, 8> bytes{};
        if (!raw(bytes.data(), bytes.size())) return false;
        value = static_cast<std::uint64_t>(bytes[0]) |
            (static_cast<std::uint64_t>(bytes[1]) << 8U) |
            (static_cast<std::uint64_t>(bytes[2]) << 16U) |
            (static_cast<std::uint64_t>(bytes[3]) << 24U) |
            (static_cast<std::uint64_t>(bytes[4]) << 32U) |
            (static_cast<std::uint64_t>(bytes[5]) << 40U) |
            (static_cast<std::uint64_t>(bytes[6]) << 48U) |
            (static_cast<std::uint64_t>(bytes[7]) << 56U);
        return true;
    }

    [[nodiscard]] bool raw(void* output, std::size_t count) noexcept {
        if (count > bytes_.size() - offset_) return false;
        std::memcpy(output, bytes_.data() + offset_, count);
        offset_ += count;
        return true;
    }

    [[nodiscard]] bool text(std::string& output, std::size_t count) {
        if (count > bytes_.size() - offset_) return false;
        output.assign(reinterpret_cast<const char*>(bytes_.data() + offset_), count);
        offset_ += count;
        return true;
    }

    [[nodiscard]] std::size_t remaining() const noexcept {
        return bytes_.size() - offset_;
    }

private:
    std::span<const std::byte> bytes_;
    std::size_t offset_ = 0;
};

struct EncodedMessage {
    std::array<std::byte, max_wire_bytes> bytes{};
    std::size_t size = 0;
    std::array<int, max_rights> fds{};
    std::size_t fd_count = 0;
};

std::expected<EncodedMessage, Error> encode(const Message& message) {
    if (auto valid = validate(message); !valid) {
        return std::unexpected(valid.error());
    }

    Writer payload;
    EncodedMessage encoded;
    WireType type{};
    auto add_fd = [&](int fd) {
        if (encoded.fd_count >= encoded.fds.size()) return false;
        encoded.fds[encoded.fd_count++] = fd;
        return true;
    };
    bool okay = std::visit([&](const auto& value) -> bool {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, Hello>) {
            type = WireType::hello;
            return payload.u64(value.session_id);
        } else if constexpr (std::is_same_v<T, PoolBegin>) {
            type = WireType::pool_begin;
            return payload.u64(value.generation) && payload.u32(value.slot_count) &&
                payload.u32(value.width) && payload.u32(value.height) &&
                payload.u32(value.fourcc) && payload.u64(value.modifier) &&
                payload.raw(value.device_uuid.data(), value.device_uuid.size()) &&
                payload.u32(value.drm_major) && payload.u32(value.drm_minor);
        } else if constexpr (std::is_same_v<T, PoolSlot>) {
            type = WireType::pool_slot;
            return payload.u64(value.generation) && payload.u32(value.slot_count) &&
                payload.u32(value.slot_index) && payload.u32(value.image.width) &&
                payload.u32(value.image.height) && payload.u32(value.image.fourcc) &&
                payload.u64(value.image.modifier) && payload.u64(value.image.offset) &&
                payload.u32(value.image.stride) && payload.u64(value.image.allocation_size) &&
                add_fd(value.image.buffer_fd.get());
        } else if constexpr (std::is_same_v<T, PoolReady>) {
            type = WireType::pool_ready;
            return payload.u64(value.generation) && payload.u32(value.slot_count);
        } else if constexpr (std::is_same_v<T, FrameReady>) {
            type = WireType::frame_ready;
            const std::uint32_t flags = value.has_presentation_time ? timestamp_present : 0U;
            return payload.u64(value.generation) && payload.u32(value.slot_count) &&
                payload.u32(value.slot_index) && payload.u64(value.sequence) &&
                payload.u64(value.presentation_time_ns) && payload.u32(flags) &&
                add_fd(value.acquire_fence_fd.get());
        } else if constexpr (std::is_same_v<T, FrameRelease>) {
            type = WireType::frame_release;
            return payload.u64(value.generation) && payload.u32(value.slot_count) &&
                payload.u32(value.slot_index) && payload.u64(value.sequence) &&
                add_fd(value.release_fence_fd.get());
        } else if constexpr (std::is_same_v<T, Stop>) {
            type = WireType::stop;
            return payload.u64(value.generation);
        } else {
            type = WireType::peer_error;
            const auto message_size = static_cast<std::uint16_t>(value.message.size());
            return payload.u32(value.code) && payload.u16(message_size) &&
                payload.text(value.message);
        }
    }, message);
    if (!okay || payload.size() + header_bytes > encoded.bytes.size()) {
        return std::unexpected(error(ErrorCode::invalid_metadata,
                                     "message exceeds the protocol packet bounds"));
    }

    Writer wire;
    constexpr std::array<char, 4> magic{'F', 'R', 'P', 'V'};
    const auto payload_size = static_cast<std::uint32_t>(payload.size());
    if (!wire.raw(magic.data(), magic.size()) ||
        !wire.u16(protocol_major) || !wire.u16(protocol_minor) ||
        !wire.u16(static_cast<std::uint16_t>(type)) || !wire.u16(0) ||
        !wire.u32(payload_size) || !wire.u16(static_cast<std::uint16_t>(encoded.fd_count)) ||
        !wire.u16(0) || !wire.raw(payload.data(), payload.size())) {
        return std::unexpected(error(ErrorCode::invalid_metadata,
                                     "could not encode a bounded protocol packet"));
    }
    std::memcpy(encoded.bytes.data(), wire.data(), wire.size());
    encoded.size = wire.size();
    return encoded;
}

std::expected<Message, Error> decode(std::span<const std::byte> packet,
                                     std::array<UniqueFd, max_rights>& fds,
                                     std::size_t fd_count) {
    if (packet.size() < header_bytes || packet.size() > max_wire_bytes) {
        return std::unexpected(error(ErrorCode::protocol_error,
                                     "protocol datagram has an invalid size"));
    }
    Reader header(packet);
    std::array<char, 4> magic{};
    std::uint16_t major = 0;
    std::uint16_t minor = 0;
    std::uint16_t raw_type = 0;
    std::uint16_t reserved_a = 0;
    std::uint32_t payload_size = 0;
    std::uint16_t expected_fds = 0;
    std::uint16_t reserved_b = 0;
    if (!header.raw(magic.data(), magic.size()) || !header.u16(major) ||
        !header.u16(minor) || !header.u16(raw_type) || !header.u16(reserved_a) ||
        !header.u32(payload_size) || !header.u16(expected_fds) || !header.u16(reserved_b)) {
        return std::unexpected(error(ErrorCode::protocol_error,
                                     "protocol header is truncated"));
    }
    constexpr std::array<char, 4> expected_magic{'F', 'R', 'P', 'V'};
    if (magic != expected_magic || major != protocol_major || minor != protocol_minor ||
        reserved_a != 0 || reserved_b != 0) {
        return std::unexpected(error(ErrorCode::protocol_error,
                                     "protocol magic, version, or reserved fields are invalid"));
    }
    if (payload_size != packet.size() - header_bytes || expected_fds != fd_count ||
        expected_fds > max_rights) {
        return std::unexpected(error(ErrorCode::protocol_error,
                                     "payload length or SCM_RIGHTS count does not match the header"));
    }

    Reader payload(packet.subspan(header_bytes));
    Message message;
    switch (static_cast<WireType>(raw_type)) {
    case WireType::hello: {
        Hello value;
        if (!payload.u64(value.session_id)) break;
        message = value;
        break;
    }
    case WireType::pool_begin: {
        PoolBegin value;
        if (!payload.u64(value.generation) || !payload.u32(value.slot_count) ||
            !payload.u32(value.width) || !payload.u32(value.height) ||
            !payload.u32(value.fourcc) || !payload.u64(value.modifier) ||
            !payload.raw(value.device_uuid.data(), value.device_uuid.size()) ||
            !payload.u32(value.drm_major) || !payload.u32(value.drm_minor)) break;
        message = value;
        break;
    }
    case WireType::pool_slot: {
        PoolSlot value;
        if (!payload.u64(value.generation) || !payload.u32(value.slot_count) ||
            !payload.u32(value.slot_index) || !payload.u32(value.image.width) ||
            !payload.u32(value.image.height) || !payload.u32(value.image.fourcc) ||
            !payload.u64(value.image.modifier) || !payload.u64(value.image.offset) ||
            !payload.u32(value.image.stride) ||
            !payload.u64(value.image.allocation_size) || fd_count != 1) break;
        value.image.buffer_fd = std::move(fds[0]);
        message = std::move(value);
        break;
    }
    case WireType::pool_ready: {
        PoolReady value;
        if (!payload.u64(value.generation) || !payload.u32(value.slot_count)) break;
        message = value;
        break;
    }
    case WireType::frame_ready: {
        FrameReady value;
        std::uint32_t flags = 0;
        if (!payload.u64(value.generation) || !payload.u32(value.slot_count) ||
            !payload.u32(value.slot_index) || !payload.u64(value.sequence) ||
            !payload.u64(value.presentation_time_ns) || !payload.u32(flags) ||
            (flags & ~timestamp_present) != 0U || fd_count != 1) break;
        value.has_presentation_time = (flags & timestamp_present) != 0U;
        value.acquire_fence_fd = std::move(fds[0]);
        message = std::move(value);
        break;
    }
    case WireType::frame_release: {
        FrameRelease value;
        if (!payload.u64(value.generation) || !payload.u32(value.slot_count) ||
            !payload.u32(value.slot_index) || !payload.u64(value.sequence) ||
            fd_count != 1) break;
        value.release_fence_fd = std::move(fds[0]);
        message = std::move(value);
        break;
    }
    case WireType::stop: {
        Stop value;
        if (!payload.u64(value.generation)) break;
        message = value;
        break;
    }
    case WireType::peer_error: {
        PeerError value;
        std::uint16_t text_size = 0;
        if (!payload.u32(value.code) || !payload.u16(text_size) ||
            text_size > max_error_text_bytes || !payload.text(value.message, text_size)) break;
        message = std::move(value);
        break;
    }
    default:
        return std::unexpected(error(ErrorCode::protocol_error,
                                     "protocol message type is unknown"));
    }
    if (payload.remaining() != 0) {
        return std::unexpected(error(ErrorCode::protocol_error,
                                     "protocol payload has trailing or missing fields"));
    }
    if (auto valid = validate(message); !valid) {
        return std::unexpected(valid.error());
    }
    return message;
}

Error socket_error(int value, const char* operation) {
    if (value == EAGAIN || value == EWOULDBLOCK) {
        return error(ErrorCode::would_block, std::string(operation) + " would block", value);
    }
    if (value == EPIPE || value == ECONNRESET || value == ENOTCONN) {
        return error(ErrorCode::peer_closed, std::string(operation) + " found a closed peer", value);
    }
    return error(ErrorCode::system_error,
                 std::string(operation) + ": " + std::strerror(value), value);
}

} // namespace

UniqueFd::UniqueFd(int fd) noexcept : fd_(fd) {}
UniqueFd::~UniqueFd() { reset(); }

UniqueFd::UniqueFd(UniqueFd&& other) noexcept : fd_(other.release()) {}

UniqueFd& UniqueFd::operator=(UniqueFd&& other) noexcept {
    if (this != &other) reset(other.release());
    return *this;
}

int UniqueFd::get() const noexcept { return fd_; }
UniqueFd::operator bool() const noexcept { return fd_ >= 0; }
int UniqueFd::release() noexcept {
    const int fd = fd_;
    fd_ = -1;
    return fd;
}
void UniqueFd::reset(int fd) noexcept {
    if (fd_ >= 0 && fd_ != fd) (void)::close(fd_);
    fd_ = fd;
}

std::expected<void, Error> validate(const Message& message) {
    return std::visit([](const auto& value) -> std::expected<void, Error> {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, Hello>) {
            if (value.session_id == 0) {
                return std::unexpected(error(ErrorCode::invalid_metadata,
                                             "session id must be nonzero"));
            }
        } else if constexpr (std::is_same_v<T, PoolBegin>) {
            return validate_pool_begin(value);
        } else if constexpr (std::is_same_v<T, PoolSlot>) {
            if (value.generation == 0 || value.slot_count < 2 ||
                value.slot_count > max_preview_slots || value.slot_index >= value.slot_count) {
                return std::unexpected(error(ErrorCode::invalid_metadata,
                                             "preview slot generation, count, or index is invalid"));
            }
            return validate_image(value.image);
        } else if constexpr (std::is_same_v<T, PoolReady>) {
            if (value.generation == 0 || value.slot_count < 2 ||
                value.slot_count > max_preview_slots) {
                return std::unexpected(error(ErrorCode::invalid_metadata,
                                             "ready pool generation or slot count is invalid"));
            }
        } else if constexpr (std::is_same_v<T, FrameReady>) {
            return validate_frame_ready(value);
        } else if constexpr (std::is_same_v<T, FrameRelease>) {
            return validate_frame_release(value);
        } else if constexpr (std::is_same_v<T, Stop>) {
            if (value.generation == 0) {
                return std::unexpected(error(ErrorCode::invalid_metadata,
                                             "stop generation must be nonzero"));
            }
        } else {
            if (value.message.size() > max_error_text_bytes ||
                value.message.find('\0') != std::string::npos) {
                return std::unexpected(error(ErrorCode::invalid_metadata,
                                             "peer error message is too long or contains NUL"));
            }
        }
        return {};
    }, message);
}

std::expected<void, Error> validate_pool_slot(const PoolBegin& pool,
                                              const PoolSlot& slot) {
    if (auto valid = validate_pool_begin(pool); !valid) return valid;
    if (slot.generation == 0 || slot.slot_count < 2 ||
        slot.slot_count > max_preview_slots || slot.slot_index >= slot.slot_count) {
        return std::unexpected(error(ErrorCode::invalid_metadata,
                                     "preview slot generation, count, or index is invalid"));
    }
    if (auto valid = validate_image(slot.image); !valid) return valid;
    if (slot.generation != pool.generation || slot.slot_count != pool.slot_count ||
        slot.image.width != pool.width || slot.image.height != pool.height ||
        slot.image.fourcc != pool.fourcc || slot.image.modifier != pool.modifier) {
        return std::unexpected(error(ErrorCode::invalid_metadata,
                                     "preview slot metadata does not match its pool generation"));
    }
    return {};
}

std::expected<void, Error> PoolDescriptor::begin(const PoolBegin& pool) {
    if (auto valid = validate_pool_begin(pool); !valid) return valid;
    if (active_ && (pool.generation <= pool_.generation ||
                    std::any_of(in_use_.begin(), in_use_.end(), [](bool used) { return used; }))) {
        return std::unexpected(error(ErrorCode::invalid_metadata,
                                     "new pool generation must increase after all old slots are released"));
    }
    pool_ = pool;
    described_.fill(false);
    in_use_.fill(false);
    last_sequence_.fill(0);
    described_count_ = 0;
    active_ = true;
    ready_ = false;
    return {};
}

std::expected<void, Error> PoolDescriptor::add_slot(const PoolSlot& slot) {
    if (!active_) {
        return std::unexpected(error(ErrorCode::invalid_metadata,
                                     "preview slot arrived before a pool generation"));
    }
    if (auto valid = validate_pool_slot(pool_, slot); !valid) return valid;
    if (ready_ || described_[slot.slot_index]) {
        return std::unexpected(error(ErrorCode::invalid_metadata,
                                     "preview slot is duplicated or arrived after pool ready"));
    }
    described_[slot.slot_index] = true;
    ++described_count_;
    return {};
}

std::expected<void, Error> PoolDescriptor::mark_ready(const PoolReady& ready) {
    if (!active_ || ready_ || ready.generation != pool_.generation ||
        ready.slot_count != pool_.slot_count || described_count_ != pool_.slot_count) {
        return std::unexpected(error(ErrorCode::invalid_metadata,
                                     "pool-ready does not match a complete pool generation"));
    }
    if (auto valid = validate(Message{ready}); !valid) return valid;
    ready_ = true;
    return {};
}

std::expected<void, Error> PoolDescriptor::accept_frame(const FrameReady& frame) {
    if (auto valid = validate_frame_ready(frame); !valid) return valid;
    if (!active_ || !ready_ || frame.generation != pool_.generation ||
        frame.slot_count != pool_.slot_count || frame.slot_index >= pool_.slot_count ||
        !described_[frame.slot_index] || in_use_[frame.slot_index] ||
        frame.sequence <= last_sequence_[frame.slot_index]) {
        return std::unexpected(error(ErrorCode::invalid_metadata,
                                     "frame uses a stale generation, unknown slot, or busy/nonmonotonic slot"));
    }
    in_use_[frame.slot_index] = true;
    last_sequence_[frame.slot_index] = frame.sequence;
    return {};
}

std::expected<void, Error> PoolDescriptor::accept_release(const FrameRelease& release) {
    if (!active_ || !ready_ || release.generation != pool_.generation ||
        release.slot_count != pool_.slot_count || release.slot_index >= pool_.slot_count ||
        !in_use_[release.slot_index] ||
        release.sequence != last_sequence_[release.slot_index]) {
        return std::unexpected(error(ErrorCode::invalid_metadata,
                                     "release uses a stale generation or does not match an in-use frame"));
    }
    if (auto valid = validate_frame_release(release); !valid) return valid;
    in_use_[release.slot_index] = false;
    return {};
}

bool PoolDescriptor::active() const noexcept { return active_; }
bool PoolDescriptor::ready() const noexcept { return ready_; }
std::uint64_t PoolDescriptor::generation() const noexcept { return pool_.generation; }
std::uint32_t PoolDescriptor::slot_count() const noexcept { return pool_.slot_count; }

SeqPacketSocket::SeqPacketSocket(UniqueFd fd) noexcept : fd_(std::move(fd)) {}

std::expected<SeqPacketSocket, Error> SeqPacketSocket::adopt(UniqueFd fd) {
    if (!fd) {
        return std::unexpected(error(ErrorCode::invalid_metadata,
                                     "cannot adopt an invalid socket descriptor"));
    }
    int socket_type = 0;
    socklen_t socket_type_size = sizeof(socket_type);
    if (::getsockopt(fd.get(), SOL_SOCKET, SO_TYPE, &socket_type, &socket_type_size) < 0) {
        return std::unexpected(socket_error(errno, "getsockopt(SO_TYPE)"));
    }
    int socket_domain = 0;
    socklen_t socket_domain_size = sizeof(socket_domain);
#ifdef SO_DOMAIN
    if (::getsockopt(fd.get(), SOL_SOCKET, SO_DOMAIN, &socket_domain, &socket_domain_size) < 0) {
        return std::unexpected(socket_error(errno, "getsockopt(SO_DOMAIN)"));
    }
#else
    (void)socket_domain_size;
    socket_domain = AF_UNIX;
#endif
    if (socket_type != SOCK_SEQPACKET || socket_domain != AF_UNIX) {
        return std::unexpected(error(ErrorCode::invalid_metadata,
                                     "preview transport requires an AF_UNIX SOCK_SEQPACKET socket"));
    }
    const int status_flags = ::fcntl(fd.get(), F_GETFL);
    if (status_flags < 0 || ::fcntl(fd.get(), F_SETFL, status_flags | O_NONBLOCK) < 0) {
        return std::unexpected(socket_error(errno, "fcntl(O_NONBLOCK)"));
    }
    const int descriptor_flags = ::fcntl(fd.get(), F_GETFD);
    if (descriptor_flags < 0 ||
        ::fcntl(fd.get(), F_SETFD, descriptor_flags | FD_CLOEXEC) < 0) {
        return std::unexpected(socket_error(errno, "fcntl(FD_CLOEXEC)"));
    }
    return SeqPacketSocket(std::move(fd));
}

std::expected<std::array<SeqPacketSocket, 2>, Error> SeqPacketSocket::pair() {
    std::array<int, 2> sockets{-1, -1};
    if (::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK,
                     0, sockets.data()) < 0) {
        return std::unexpected(socket_error(errno, "socketpair(AF_UNIX, SOCK_SEQPACKET)"));
    }
    auto first = adopt(UniqueFd(sockets[0]));
    if (!first) {
        (void)::close(sockets[1]);
        return std::unexpected(first.error());
    }
    auto second = adopt(UniqueFd(sockets[1]));
    if (!second) return std::unexpected(second.error());
    return std::array<SeqPacketSocket, 2>{std::move(*first), std::move(*second)};
}

SeqPacketSocket::operator bool() const noexcept { return static_cast<bool>(fd_); }
int SeqPacketSocket::fd() const noexcept { return fd_.get(); }

std::expected<void, Error> SeqPacketSocket::send(const Message& message) const {
    if (!fd_) {
        return std::unexpected(error(ErrorCode::peer_closed,
                                     "preview socket is not open"));
    }
    auto encoded = encode(message);
    if (!encoded) return std::unexpected(encoded.error());

    std::array<std::byte, CMSG_SPACE(sizeof(int) * max_rights)> control{};
    iovec vector{
        .iov_base = encoded->bytes.data(),
        .iov_len = encoded->size,
    };
    msghdr header{};
    header.msg_iov = &vector;
    header.msg_iovlen = 1;
    if (encoded->fd_count > 0) {
        header.msg_control = control.data();
        header.msg_controllen = CMSG_SPACE(sizeof(int) * encoded->fd_count);
        cmsghdr* cmsg = CMSG_FIRSTHDR(&header);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int) * encoded->fd_count);
        std::memcpy(CMSG_DATA(cmsg), encoded->fds.data(),
                    sizeof(int) * encoded->fd_count);
    }
    const ssize_t sent = ::sendmsg(fd_.get(), &header, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (sent < 0) return std::unexpected(socket_error(errno, "sendmsg"));
    if (static_cast<std::size_t>(sent) != encoded->size) {
        return std::unexpected(error(ErrorCode::system_error,
                                     "SOCK_SEQPACKET send was not atomic"));
    }
    return {};
}

std::expected<Message, Error> SeqPacketSocket::receive() const {
    if (!fd_) {
        return std::unexpected(error(ErrorCode::peer_closed,
                                     "preview socket is not open"));
    }
    std::array<std::byte, max_wire_bytes> packet{};
    alignas(cmsghdr) std::array<std::byte, CMSG_SPACE(sizeof(int) * max_rights)> control{};
    iovec vector{
        .iov_base = packet.data(),
        .iov_len = packet.size(),
    };
    msghdr header{};
    header.msg_iov = &vector;
    header.msg_iovlen = 1;
    header.msg_control = control.data();
    header.msg_controllen = control.size();
    const ssize_t received = ::recvmsg(fd_.get(), &header, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
    if (received < 0) return std::unexpected(socket_error(errno, "recvmsg"));
    if (received == 0) {
        return std::unexpected(error(ErrorCode::peer_closed,
                                     "preview socket peer closed"));
    }

    std::array<UniqueFd, max_rights> received_fds{};
    std::size_t received_fd_count = 0;
    bool invalid_control = false;
    for (cmsghdr* cmsg = CMSG_FIRSTHDR(&header); cmsg != nullptr;
         cmsg = CMSG_NXTHDR(&header, cmsg)) {
        if (cmsg->cmsg_len < CMSG_LEN(0)) {
            invalid_control = true;
            continue;
        }
        if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) {
            invalid_control = true;
            continue;
        }
        const std::size_t data_bytes = cmsg->cmsg_len - CMSG_LEN(0);
        if ((data_bytes % sizeof(int)) != 0U) {
            invalid_control = true;
            continue;
        }
        const std::size_t count = data_bytes / sizeof(int);
        const auto* raw_fds = reinterpret_cast<const int*>(CMSG_DATA(cmsg));
        for (std::size_t i = 0; i < count; ++i) {
            if (received_fd_count < received_fds.size()) {
                received_fds[received_fd_count++].reset(raw_fds[i]);
            } else {
                (void)::close(raw_fds[i]);
                invalid_control = true;
            }
        }
    }
    if (invalid_control || (header.msg_flags & MSG_CTRUNC) != 0) {
        return std::unexpected(error(ErrorCode::protocol_error,
                                     "SCM_RIGHTS control data is invalid or truncated"));
    }
    if ((header.msg_flags & MSG_TRUNC) != 0) {
        return std::unexpected(error(ErrorCode::protocol_error,
                                     "preview datagram exceeded the maximum packet size"));
    }
    return decode(std::span<const std::byte>(packet.data(),
                                             static_cast<std::size_t>(received)),
                  received_fds, received_fd_count);
}

} // namespace fluxrec::preview
