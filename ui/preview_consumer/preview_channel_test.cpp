#include "preview_channel.hpp"

#include <QSignalSpy>
#include <QTest>
#include <QFile>
#include <QFileInfo>

#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/memfd.h>
#include <optional>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

using namespace fluxrec::ui;
using namespace fluxrec::preview;

namespace {

UniqueFd duplicate(int fd) {
    return UniqueFd{::fcntl(fd, F_DUPFD_CLOEXEC, 0)};
}

UniqueFd make_memfd() {
    const int fd = ::memfd_create("fluxrec-preview-test", MFD_CLOEXEC);
    if (fd < 0) return {};
    if (::ftruncate(fd, 64 * 64 * 4) < 0) {
        (void)::close(fd);
        return {};
    }
    return UniqueFd{fd};
}

std::optional<SeqPacketSocket> connect_peer(const QString& path) {
    const int fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0) return std::nullopt;
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const QByteArray encoded = QFile::encodeName(path);
    if (encoded.size() >= static_cast<qsizetype>(sizeof(address.sun_path))) {
        (void)::close(fd);
        return std::nullopt;
    }
    std::memcpy(address.sun_path, encoded.constData(), static_cast<std::size_t>(encoded.size()));
    address.sun_path[encoded.size()] = '\0';
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0) {
        (void)::close(fd);
        return std::nullopt;
    }
    auto adopted = SeqPacketSocket::adopt(UniqueFd{fd});
    if (!adopted) return std::nullopt;
    return std::move(*adopted);
}

PoolBegin test_pool() {
    PoolBegin pool;
    pool.generation = 7;
    pool.slot_count = 2;
    pool.width = 64;
    pool.height = 64;
    pool.fourcc = drm_format_abgr8888;
    pool.modifier = 0;
    pool.device_uuid[0] = 0x42;
    pool.drm_major = 1;
    pool.drm_minor = 2;
    return pool;
}

PoolSlot test_slot(const PoolBegin& pool, std::uint32_t slot_index, int fd) {
    PoolSlot slot;
    slot.generation = pool.generation;
    slot.slot_count = pool.slot_count;
    slot.slot_index = slot_index;
    slot.image.width = pool.width;
    slot.image.height = pool.height;
    slot.image.fourcc = pool.fourcc;
    slot.image.modifier = pool.modifier;
    slot.image.stride = pool.width * 4;
    slot.image.allocation_size = static_cast<std::uint64_t>(slot.image.stride) * pool.height;
    slot.image.buffer_fd = duplicate(fd);
    return slot;
}

} // namespace

class PreviewChannelTest final : public QObject {
    Q_OBJECT

private slots:
    void listenerCreatesPrivateSocketAndImportsPool();
    void signaledFramesArePublishedAndReleasedAsynchronously();
};

void PreviewChannelTest::listenerCreatesPrivateSocketAndImportsPool() {
    PreviewChannel channel;
    QString error;
    QVERIFY2(channel.start_listening(&error), qPrintable(error));
    QCOMPARE(channel.state(), PreviewChannel::State::listening);
    QVERIFY(QFileInfo::exists(channel.socket_path()));
    struct stat socket_status{};
    const QByteArray encoded_path = QFile::encodeName(channel.socket_path());
    QVERIFY(::stat(encoded_path.constData(), &socket_status) == 0);
    QCOMPARE(socket_status.st_mode & 0777, static_cast<mode_t>(0600));

    auto peer = connect_peer(channel.socket_path());
    QVERIFY(peer.has_value());
    QTRY_COMPARE_WITH_TIMEOUT(channel.state(), PreviewChannel::State::connected, 1000);
    QVERIFY(peer->send(Message{Hello{.session_id = 99}}));

    const PoolBegin pool = test_pool();
    QVERIFY(peer->send(Message{pool}));
    UniqueFd image_fd = make_memfd();
    QVERIFY(image_fd);
    QVERIFY(peer->send(Message{test_slot(pool, 0, image_fd.get())}));
    QVERIFY(peer->send(Message{test_slot(pool, 1, image_fd.get())}));
    QVERIFY(peer->send(Message{PoolReady{.generation = pool.generation,
                                         .slot_count = pool.slot_count}}));
    QTRY_COMPARE_WITH_TIMEOUT(channel.state(), PreviewChannel::State::ready, 1000);
    const auto imported_pool = channel.pool();
    QVERIFY(imported_pool);
    QCOMPARE(imported_pool->description.generation, pool.generation);
    QCOMPARE(imported_pool->slot_data[0].stride, pool.width * 4);
    QCOMPARE(imported_pool->slot_data[1].dmabuf_fd.get() >= 0, true);
}

void PreviewChannelTest::signaledFramesArePublishedAndReleasedAsynchronously() {
    PreviewChannel channel;
    QString error;
    QVERIFY2(channel.start_listening(&error), qPrintable(error));
    auto peer = connect_peer(channel.socket_path());
    QVERIFY(peer.has_value());
    QTRY_COMPARE_WITH_TIMEOUT(channel.state(), PreviewChannel::State::connected, 1000);

    QVERIFY(peer->send(Message{Hello{.session_id = 101}}));
    const PoolBegin pool = test_pool();
    QVERIFY(peer->send(Message{pool}));
    UniqueFd image_fd = make_memfd();
    QVERIFY(image_fd);
    QVERIFY(peer->send(Message{test_slot(pool, 0, image_fd.get())}));
    QVERIFY(peer->send(Message{test_slot(pool, 1, image_fd.get())}));
    QVERIFY(peer->send(Message{PoolReady{.generation = pool.generation,
                                         .slot_count = pool.slot_count}}));
    QTRY_COMPARE_WITH_TIMEOUT(channel.state(), PreviewChannel::State::ready, 1000);

    QSignalSpy frame_spy(&channel, &PreviewChannel::frame_available);
    QSignalSpy release_spy(&channel, &PreviewChannel::slot_release_complete);
    UniqueFd acquire{::eventfd(1, EFD_CLOEXEC | EFD_NONBLOCK)};
    QVERIFY(acquire);
    FrameReady frame;
    frame.generation = pool.generation;
    frame.slot_count = pool.slot_count;
    frame.slot_index = 0;
    frame.sequence = 23;
    frame.acquire_fence_fd = duplicate(acquire.get());
    QVERIFY(peer->send(Message{std::move(frame)}));
    QTRY_VERIFY_WITH_TIMEOUT(!frame_spy.isEmpty(), 1000);
    auto published = channel.take_latest_frame();
    QVERIFY(published);
    QCOMPARE(published->sequence, static_cast<std::uint64_t>(23));

    UniqueFd release{::eventfd(1, EFD_CLOEXEC | EFD_NONBLOCK)};
    channel.queue_release(published->pool->description.generation,
                          published->slot_index,
                          published->pool->description.slot_count,
                          published->sequence, duplicate(release.get()), true);
    QTRY_VERIFY_WITH_TIMEOUT(!release_spy.isEmpty(), 1000);
    auto received = peer->receive();
    QVERIFY(received.has_value());
    QVERIFY(std::holds_alternative<FrameRelease>(*received));
    const auto& release_record = std::get<FrameRelease>(*received);
    QCOMPARE(release_record.generation, pool.generation);
    QCOMPARE(release_record.slot_index, 0U);
    QCOMPARE(release_record.sequence, static_cast<std::uint64_t>(23));
}

QTEST_GUILESS_MAIN(PreviewChannelTest)
#include "preview_channel_test.moc"
