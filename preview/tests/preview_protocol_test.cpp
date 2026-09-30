#include "preview/preview.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <fcntl.h>
#include <linux/memfd.h>
#include <stdexcept>
#include <span>
#include <string>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <utility>
#include <variant>

namespace {

using namespace fluxrec::preview;

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

UniqueFd make_memfd(std::uint64_t size) {
    const int fd = static_cast<int>(::syscall(SYS_memfd_create, "fluxrec-preview-test",
                                              MFD_CLOEXEC));
    if (fd < 0) throw std::runtime_error("memfd_create failed");
    UniqueFd result(fd);
    if (::ftruncate(fd, static_cast<off_t>(size)) < 0) {
        throw std::runtime_error("ftruncate memfd failed");
    }
    return result;
}

UniqueFd make_fence_fd() {
    const int fd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (fd < 0) throw std::runtime_error("eventfd failed");
    return UniqueFd(fd);
}

void put_le(std::span<std::byte> bytes, std::size_t offset, std::uint64_t value,
            std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        bytes[offset + i] = static_cast<std::byte>(value & 0xffU);
        value >>= 8U;
    }
}

void send_raw_packet(int socket_fd, std::span<const std::byte> bytes, int attached_fd = -1) {
    iovec vector{
        .iov_base = const_cast<std::byte*>(bytes.data()),
        .iov_len = bytes.size(),
    };
    alignas(cmsghdr) std::array<std::byte, CMSG_SPACE(sizeof(int))> control{};
    msghdr header{};
    header.msg_iov = &vector;
    header.msg_iovlen = 1;
    if (attached_fd >= 0) {
        header.msg_control = control.data();
        header.msg_controllen = control.size();
        cmsghdr* cmsg = CMSG_FIRSTHDR(&header);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int));
        std::memcpy(CMSG_DATA(cmsg), &attached_fd, sizeof(attached_fd));
    }
    const ssize_t sent = ::sendmsg(socket_fd, &header, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (sent < 0 || static_cast<std::size_t>(sent) != bytes.size()) {
        throw std::runtime_error("raw test packet send failed");
    }
}

std::array<std::byte, 28> raw_hello_packet() {
    std::array<std::byte, 28> bytes{};
    bytes[0] = static_cast<std::byte>('F');
    bytes[1] = static_cast<std::byte>('R');
    bytes[2] = static_cast<std::byte>('P');
    bytes[3] = static_cast<std::byte>('V');
    put_le(bytes, 4, protocol_major, 2);
    put_le(bytes, 6, protocol_minor, 2);
    put_le(bytes, 8, 1, 2); // Hello
    put_le(bytes, 12, 8, 4);
    put_le(bytes, 20, 1, 8);
    return bytes;
}

PoolBegin pool_begin(std::uint64_t generation = 1) {
    PoolBegin result;
    result.generation = generation;
    result.slot_count = 3;
    result.width = 4;
    result.height = 2;
    result.fourcc = drm_format_abgr8888;
    result.modifier = 0;
    result.device_uuid[0] = 1;
    result.drm_major = 226;
    result.drm_minor = 128;
    return result;
}

PoolSlot pool_slot(std::uint32_t index, std::uint64_t generation = 1) {
    PoolSlot result;
    result.generation = generation;
    result.slot_count = 3;
    result.slot_index = index;
    result.image.width = 4;
    result.image.height = 2;
    result.image.fourcc = drm_format_abgr8888;
    result.image.modifier = 0;
    result.image.offset = 16;
    result.image.stride = 20;
    result.image.allocation_size = 52;
    result.image.buffer_fd = make_memfd(result.image.allocation_size);
    return result;
}

FrameReady frame_ready(std::uint32_t slot, std::uint64_t sequence,
                       std::uint64_t generation = 1) {
    FrameReady result;
    result.generation = generation;
    result.slot_count = 3;
    result.slot_index = slot;
    result.sequence = sequence;
    result.presentation_time_ns = sequence * 1'000'000ULL;
    result.has_presentation_time = true;
    result.acquire_fence_fd = make_fence_fd();
    return result;
}

FrameRelease frame_release(std::uint32_t slot, std::uint64_t sequence,
                           std::uint64_t generation = 1) {
    FrameRelease result;
    result.generation = generation;
    result.slot_count = 3;
    result.slot_index = slot;
    result.sequence = sequence;
    result.release_fence_fd = make_fence_fd();
    return result;
}

template<typename T>
const T& message_as(const Message& message, const char* message_text) {
    const auto* result = std::get_if<T>(&message);
    check(result != nullptr, message_text);
    return *result;
}

void test_metadata_validation() {
    Message hello = Hello{.session_id = 17};
    check(validate(hello).has_value(), "valid hello rejected");
    check(!validate(Message{Hello{}}), "zero session id accepted");

    auto pool = pool_begin();
    check(validate(Message{pool}).has_value(), "valid pool begin rejected");
    auto invalid_pool = pool;
    invalid_pool.slot_count = static_cast<std::uint32_t>(max_preview_slots + 1);
    check(!validate(Message{invalid_pool}), "oversized preview pool accepted");
    invalid_pool = pool;
    invalid_pool.fourcc = 0;
    check(!validate(Message{invalid_pool}), "non-RGBA preview format accepted");
    invalid_pool = pool;
    invalid_pool.modifier = drm_format_modifier_invalid;
    check(!validate(Message{invalid_pool}), "invalid modifier accepted");
    invalid_pool = pool;
    invalid_pool.device_uuid.fill(0);
    check(!validate(Message{invalid_pool}), "zero device UUID accepted");

    auto slot = pool_slot(0);
    check(validate_pool_slot(pool, slot).has_value(), "valid pool slot rejected");
    slot.image.stride = 12;
    check(!validate_pool_slot(pool, slot), "short RGBA stride accepted");
    slot.image.stride = 20;
    slot.image.offset = 33;
    check(!validate_pool_slot(pool, slot), "plane extent beyond allocation accepted");
    slot.image.offset = 16;
    slot.image.allocation_size = UINT64_MAX;
    check(!validate_pool_slot(pool, slot), "allocation size above configured limit accepted");
    slot.image.allocation_size = 52;
    slot.generation = 2;
    check(!validate_pool_slot(pool, slot), "stale slot generation accepted");

    auto frame = frame_ready(0, 1);
    check(validate(Message{FrameReady{
              .generation = frame.generation,
              .slot_count = frame.slot_count,
              .slot_index = frame.slot_index,
              .sequence = frame.sequence,
              .presentation_time_ns = frame.presentation_time_ns,
              .has_presentation_time = frame.has_presentation_time,
              .acquire_fence_fd = make_fence_fd(),
          }}).has_value(), "valid frame ready rejected");
    auto missing_timestamp = frame_ready(0, 2);
    missing_timestamp.has_presentation_time = false;
    check(!validate(Message{FrameReady{
              .generation = missing_timestamp.generation,
              .slot_count = missing_timestamp.slot_count,
              .slot_index = missing_timestamp.slot_index,
              .sequence = missing_timestamp.sequence,
              .presentation_time_ns = missing_timestamp.presentation_time_ns,
              .has_presentation_time = missing_timestamp.has_presentation_time,
              .acquire_fence_fd = make_fence_fd(),
          }}), "timestamp value without presence flag accepted");
}

void test_seqpacket_rights_roundtrip() {
    auto sockets_result = SeqPacketSocket::pair();
    check(sockets_result.has_value(), "could not make SOCK_SEQPACKET pair");
    auto sockets = std::move(*sockets_result);
    auto empty_receive = sockets[1].receive();
    check(!empty_receive && empty_receive.error().code == ErrorCode::would_block,
          "empty nonblocking receive did not report would_block");

    check(sockets[0].send(Message{Hello{.session_id = 99}}).has_value(),
          "hello send failed");
    auto hello_message = sockets[1].receive();
    check(hello_message.has_value(), "hello receive failed");
    check(message_as<Hello>(*hello_message, "received message was not Hello").session_id == 99,
          "hello session id changed in transit");

    auto begin = pool_begin(5);
    check(sockets[0].send(Message{begin}).has_value(), "pool begin send failed");
    auto begin_message = sockets[1].receive();
    check(begin_message.has_value(), "pool begin receive failed");
    const auto& received_begin = message_as<PoolBegin>(*begin_message,
                                                         "received message was not PoolBegin");
    check(received_begin.generation == 5 && received_begin.width == 4 &&
          received_begin.device_uuid == begin.device_uuid,
          "pool metadata changed in transit");

    auto slot = pool_slot(1, 5);
    const int sender_buffer_fd = slot.image.buffer_fd.get();
    Message slot_to_send{std::move(slot)};
    check(sockets[0].send(slot_to_send).has_value(), "pool slot send failed");
    auto slot_message = sockets[1].receive();
    check(slot_message.has_value(), "pool slot receive failed");
    const auto& received_slot = message_as<PoolSlot>(*slot_message,
                                                      "received message was not PoolSlot");
    check(received_slot.generation == 5 && received_slot.slot_index == 1 &&
          received_slot.image.buffer_fd &&
          received_slot.image.buffer_fd.get() != sender_buffer_fd,
          "SCM_RIGHTS did not duplicate the pool DMA-BUF descriptor");
    struct stat buffer_stat {};
    check(::fstat(received_slot.image.buffer_fd.get(), &buffer_stat) == 0 &&
          static_cast<std::uint64_t>(buffer_stat.st_size) == 52,
          "received DMA-BUF stand-in has the wrong allocation size");
    check(::fcntl(sender_buffer_fd, F_GETFD) >= 0,
          "SCM_RIGHTS unexpectedly consumed the sender descriptor");

    check(sockets[0].send(Message{PoolReady{.generation = 5, .slot_count = 3}}).has_value(),
          "pool ready send failed");
    auto ready_message = sockets[1].receive();
    check(ready_message.has_value() &&
          message_as<PoolReady>(*ready_message, "received message was not PoolReady").generation == 5,
          "pool ready roundtrip failed");

    auto frame = frame_ready(1, 1234, 5);
    const int sender_acquire_fd = frame.acquire_fence_fd.get();
    Message frame_to_send{std::move(frame)};
    check(sockets[0].send(frame_to_send).has_value(), "frame send failed");
    auto frame_message = sockets[1].receive();
    check(frame_message.has_value(), "frame receive failed");
    const auto& received_frame = message_as<FrameReady>(*frame_message,
                                                         "received message was not FrameReady");
    check(received_frame.generation == 5 && received_frame.slot_index == 1 &&
          received_frame.sequence == 1234 && received_frame.has_presentation_time &&
          received_frame.presentation_time_ns == 1'234'000'000ULL &&
          received_frame.acquire_fence_fd.get() != sender_acquire_fd,
          "acquire sync-fd metadata changed in transit");

    auto release = frame_release(1, 1234, 5);
    const int sender_release_fd = release.release_fence_fd.get();
    Message release_to_send{std::move(release)};
    check(sockets[1].send(release_to_send).has_value(), "release send failed");
    auto release_message = sockets[0].receive();
    check(release_message.has_value(), "release receive failed");
    const auto& received_release = message_as<FrameRelease>(*release_message,
                                                            "received message was not FrameRelease");
    check(received_release.generation == 5 && received_release.slot_index == 1 &&
          received_release.sequence == 1234 &&
          received_release.release_fence_fd.get() != sender_release_fd,
          "release sync-fd metadata changed in transit");
}

void test_nonblocking_send_and_validation() {
    auto sockets_result = SeqPacketSocket::pair();
    check(sockets_result.has_value(), "could not make SOCK_SEQPACKET pair");
    auto sockets = std::move(*sockets_result);
    int send_buffer = 1024;
    check(::setsockopt(sockets[0].fd(), SOL_SOCKET, SO_SNDBUF,
                       &send_buffer, sizeof(send_buffer)) == 0,
          "could not limit socket send buffer");

    const Message full_record = PeerError{
        .code = 7,
        .message = std::string(max_error_text_bytes, 'x'),
    };
    bool saw_would_block = false;
    for (std::size_t i = 0; i < 10'000; ++i) {
        auto sent = sockets[0].send(full_record);
        if (!sent) {
            check(sent.error().code == ErrorCode::would_block,
                  "send failed for a reason other than nonblocking backpressure");
            saw_would_block = true;
            break;
        }
    }
    check(saw_would_block, "nonblocking socket send did not reach would_block");

    const Message invalid = PoolBegin{};
    auto invalid_send = sockets[0].send(invalid);
    check(!invalid_send && invalid_send.error().code == ErrorCode::invalid_metadata,
          "invalid metadata was sent to the socket");
}

void test_exact_packet_and_rights_validation() {
    auto sockets_result = SeqPacketSocket::pair();
    check(sockets_result.has_value(), "could not make SOCK_SEQPACKET pair");
    auto sockets = std::move(*sockets_result);

    auto hello = raw_hello_packet();
    auto unexpected_fd = make_fence_fd();
    send_raw_packet(sockets[0].fd(), hello, unexpected_fd.get());
    auto fd_mismatch = sockets[1].receive();
    check(!fd_mismatch && fd_mismatch.error().code == ErrorCode::protocol_error,
          "unexpected SCM_RIGHTS descriptor was accepted");

    std::array<std::byte, 29> trailing_payload{};
    std::copy(hello.begin(), hello.end(), trailing_payload.begin());
    put_le(trailing_payload, 12, 9, 4);
    trailing_payload.back() = std::byte{0x5a};
    send_raw_packet(sockets[0].fd(), trailing_payload);
    auto trailing = sockets[1].receive();
    check(!trailing && trailing.error().code == ErrorCode::protocol_error,
          "trailing protocol payload byte was accepted");
}

void test_pool_generation_and_slot_sequence() {
    PoolDescriptor descriptor;
    const auto pool = pool_begin(10);
    check(descriptor.begin(pool).has_value(), "valid generation rejected");
    auto slot0 = pool_slot(0, 10);
    auto slot1 = pool_slot(1, 10);
    auto slot2 = pool_slot(2, 10);
    check(descriptor.add_slot(slot0).has_value(), "slot 0 rejected");
    check(!descriptor.mark_ready(PoolReady{.generation = 10, .slot_count = 3}),
          "incomplete pool became ready");
    check(descriptor.add_slot(slot1).has_value(), "slot 1 rejected");
    check(descriptor.add_slot(slot2).has_value(), "slot 2 rejected");
    check(descriptor.mark_ready(PoolReady{.generation = 10, .slot_count = 3}).has_value(),
          "complete pool did not become ready");

    auto first = frame_ready(1, 40, 10);
    check(descriptor.accept_frame(first).has_value(), "valid frame rejected");
    auto overlapping = frame_ready(1, 41, 10);
    check(!descriptor.accept_frame(overlapping), "busy slot accepted a second frame");
    auto wrong_release = frame_release(1, 39, 10);
    check(!descriptor.accept_release(wrong_release), "mismatched release sequence accepted");
    auto release = frame_release(1, 40, 10);
    check(descriptor.accept_release(release).has_value(), "matching release rejected");
    auto next = frame_ready(1, 41, 10);
    check(descriptor.accept_frame(next).has_value(), "released slot was not reusable");

    auto stale_release = frame_release(1, 41, 9);
    check(!descriptor.accept_release(stale_release), "stale pool generation release accepted");
    auto stale_slot = pool_slot(0, 9);
    check(!descriptor.add_slot(stale_slot), "stale pool slot accepted");
}

void test_fd_raii() {
    auto fd = make_fence_fd();
    const int raw = fd.get();
    check(::fcntl(raw, F_GETFD) >= 0, "test fd was not open");
    {
        UniqueFd moved(std::move(fd));
        check(!fd && moved.get() == raw, "UniqueFd move did not transfer ownership");
    }
    check(::fcntl(raw, F_GETFD) < 0 && errno == EBADF,
          "UniqueFd did not close its descriptor on destruction");
}

} // namespace

int main() {
    test_metadata_validation();
    test_seqpacket_rights_roundtrip();
    test_nonblocking_send_and_validation();
    test_exact_packet_and_rights_validation();
    test_pool_generation_and_slot_sequence();
    test_fd_raii();
    return 0;
}
