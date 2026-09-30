#include "preview_channel.hpp"

#include <QDir>
#include <QFile>
#include <QMetaObject>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <QThread>
#include <QTemporaryDir>
#include <QTimer>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>

namespace fluxrec::ui {
namespace {

QString errorText(const preview::Error& error) {
    QString message = QString::fromStdString(error.message);
    if (error.system_error != 0) {
        message += QStringLiteral(" (%1)").arg(QString::fromLocal8Bit(std::strerror(error.system_error)));
    }
    return message;
}

preview::UniqueFd duplicateFd(int fd) {
    if (fd < 0) return {};
    return preview::UniqueFd{::fcntl(fd, F_DUPFD_CLOEXEC, 0)};
}

bool fenceSignaled(int fd, bool& failed) {
    pollfd descriptor{.fd = fd, .events = POLLIN, .revents = 0};
    int result;
    do {
        result = ::poll(&descriptor, 1, 0);
    } while (result < 0 && errno == EINTR);
    if (result < 0) {
        failed = true;
        return false;
    }
    if (result == 0) return false;
    failed = (descriptor.revents & (POLLNVAL | POLLERR)) != 0;
    return !failed && (descriptor.revents & (POLLIN | POLLERR | POLLHUP)) != 0;
}

} // namespace

PreviewChannel::PreviewChannel(QObject* parent) : QObject(parent) {
    fence_timer_ = new QTimer(this);
    fence_timer_->setInterval(3);
    connect(fence_timer_, &QTimer::timeout, this, &PreviewChannel::poll_fences);
}

PreviewChannel::~PreviewChannel() { stop(); }

bool PreviewChannel::start_listening(QString* error) {
    if (state_ == State::listening || state_ == State::connected || state_ == State::ready) {
        if (error) *error = tr("The preview socket is already active.");
        return false;
    }
    stop();

    QString runtime_path = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (runtime_path.isEmpty()) runtime_path = QDir::tempPath();
    socket_directory_ = std::make_unique<QTemporaryDir>(
        QDir(runtime_path).filePath(QStringLiteral("fluxrec-preview-XXXXXX")));
    if (!socket_directory_->isValid()) {
        const QString message = tr("Could not create a private preview socket directory.");
        if (error) *error = message;
        set_failed(message);
        return false;
    }
    socket_directory_->setAutoRemove(true);
    socket_path_ = QDir(socket_directory_->path()).filePath(QStringLiteral("preview.sock"));
    const QByteArray encoded_path = QFile::encodeName(socket_path_);
    sockaddr_un address{};
    if (encoded_path.size() >= static_cast<qsizetype>(sizeof(address.sun_path))) {
        const QString message = tr("The runtime socket path is too long for the preview channel.");
        if (error) *error = message;
        set_failed(message);
        return false;
    }

    listener_fd_ = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (listener_fd_ < 0) {
        const QString message = tr("Could not create the preview socket: %1")
            .arg(QString::fromLocal8Bit(std::strerror(errno)));
        if (error) *error = message;
        set_failed(message);
        return false;
    }
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, encoded_path.constData(),
                static_cast<std::size_t>(encoded_path.size()));
    address.sun_path[encoded_path.size()] = '\0';
    if (::bind(listener_fd_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0 ||
        ::chmod(encoded_path.constData(), S_IRUSR | S_IWUSR) < 0 ||
        ::listen(listener_fd_, 1) < 0) {
        const QString message = tr("Could not open the preview socket: %1")
            .arg(QString::fromLocal8Bit(std::strerror(errno)));
        if (error) *error = message;
        set_failed(message);
        return false;
    }

    listener_notifier_ = new QSocketNotifier(listener_fd_, QSocketNotifier::Read, this);
    connect(listener_notifier_, &QSocketNotifier::activated, this,
            [this] { accept_peer(); });
    set_state(State::listening);
    emit socket_path_changed(socket_path_);
    return true;
}

void PreviewChannel::stop() noexcept {
    if (listener_notifier_) {
        listener_notifier_->setEnabled(false);
        delete listener_notifier_;
        listener_notifier_ = nullptr;
    }
    close_peer();
    if (listener_fd_ >= 0) {
        (void)::close(listener_fd_);
        listener_fd_ = -1;
    }
    fence_timer_->stop();
    pending_frames_.clear();
    pending_releases_.clear();
    completion_fences_.clear();
    building_pool_.reset();
    {
        std::lock_guard lock(published_mutex_);
        latest_frame_.reset();
        published_pool_.reset();
    }
    descriptor_ = preview::PoolDescriptor{};
    hello_received_ = false;
    session_id_ = 0;
    last_ready_sequence_ = 0;
    socket_path_.clear();
    socket_directory_.reset();
    set_state(State::stopped);
    emit socket_path_changed({});
}

void PreviewChannel::fail_preview(const QString& message) {
    if (QThread::currentThread() == thread()) {
        set_failed(message);
        return;
    }
    QMetaObject::invokeMethod(this, [this, message] { set_failed(message); },
                              Qt::QueuedConnection);
}

QString PreviewChannel::socket_path() const { return socket_path_; }

std::shared_ptr<const PreviewPoolData> PreviewChannel::pool() const {
    std::lock_guard lock(published_mutex_);
    return published_pool_;
}

std::shared_ptr<PreviewFrameData> PreviewChannel::take_latest_frame() {
    std::lock_guard lock(published_mutex_);
    return std::exchange(latest_frame_, {});
}

void PreviewChannel::queue_release(std::uint64_t generation, std::uint32_t slot_index,
                                   std::uint32_t slot_count, std::uint64_t sequence,
                                   preview::UniqueFd release_fence_fd,
                                   bool track_gpu_completion) {
    if (!release_fence_fd) return;
    auto queued = std::make_shared<QueuedRelease>();
    auto local_fence = track_gpu_completion ? duplicateFd(release_fence_fd.get())
                                           : preview::UniqueFd{};
    if (track_gpu_completion && !local_fence) track_gpu_completion = false;
    queued->message = preview::FrameRelease{
        .generation = generation,
        .slot_count = slot_count,
        .slot_index = slot_index,
        .sequence = sequence,
        .release_fence_fd = std::move(release_fence_fd),
    };
    queued->completion_fence = std::move(local_fence);
    queued->track_completion = track_gpu_completion;

    QMetaObject::invokeMethod(this, [this, queued] {
        enqueue_release(std::move(*queued));
        flush_releases();
    }, Qt::QueuedConnection);
}

void PreviewChannel::accept_peer() {
    if (peer_fd_ >= 0 || listener_fd_ < 0) return;
    sockaddr_un address{};
    socklen_t length = sizeof(address);
    const int accepted = ::accept4(listener_fd_, reinterpret_cast<sockaddr*>(&address), &length,
                                   SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (accepted < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return;
        set_failed(tr("Could not accept the preview connection: %1")
                       .arg(QString::fromLocal8Bit(std::strerror(errno))));
        return;
    }

    ucred credentials{};
    socklen_t credentials_size = sizeof(credentials);
    if (::getsockopt(accepted, SOL_SOCKET, SO_PEERCRED, &credentials, &credentials_size) < 0 ||
        credentials.uid != ::getuid()) {
        (void)::close(accepted);
        set_failed(tr("The preview connection came from an unexpected user."));
        return;
    }
    auto adopted = preview::SeqPacketSocket::adopt(preview::UniqueFd{accepted});
    if (!adopted) {
        set_failed(errorText(adopted.error()));
        return;
    }
    peer_ = std::move(*adopted);
    peer_fd_ = peer_.fd();
    if (listener_notifier_) listener_notifier_->setEnabled(false);
    peer_read_notifier_ = new QSocketNotifier(peer_fd_, QSocketNotifier::Read, this);
    peer_write_notifier_ = new QSocketNotifier(peer_fd_, QSocketNotifier::Write, this);
    peer_write_notifier_->setEnabled(false);
    connect(peer_read_notifier_, &QSocketNotifier::activated, this,
            [this] { read_peer(); });
    connect(peer_write_notifier_, &QSocketNotifier::activated, this,
            [this] { flush_releases(); });
    set_state(State::connected);
    read_peer();
}

void PreviewChannel::read_peer() {
    if (!peer_) return;
    for (;;) {
        auto received = peer_.receive();
        if (!received) {
            if (received.error().code == preview::ErrorCode::would_block) break;
            if (received.error().code == preview::ErrorCode::peer_closed) {
                close_peer();
                pending_frames_.clear();
                pending_releases_.clear();
                completion_fences_.clear();
                building_pool_.reset();
                {
                    std::lock_guard lock(published_mutex_);
                    latest_frame_.reset();
                    published_pool_.reset();
                }
                set_state(State::stopped);
                emit frame_available();
                break;
            }
            set_failed(errorText(received.error()));
            return;
        }
        handle_message(std::move(*received));
        if (state_ == State::failed || state_ == State::stopped) return;
    }
}

void PreviewChannel::flush_releases() {
    if (!peer_ || pending_releases_.empty()) {
        if (peer_write_notifier_) peer_write_notifier_->setEnabled(false);
        return;
    }
    while (!pending_releases_.empty()) {
        auto& pending = pending_releases_.front();
        auto sent = peer_.send(pending.message);
        if (!sent) {
            if (sent.error().code == preview::ErrorCode::would_block) {
                if (peer_write_notifier_) peer_write_notifier_->setEnabled(true);
                return;
            }
            set_failed(errorText(sent.error()));
            return;
        }
        const auto& release = std::get<preview::FrameRelease>(pending.message);
        if (auto accepted = descriptor_.accept_release(release); !accepted) {
            set_failed(errorText(accepted.error()));
            return;
        }
        if (pending.track_completion && pending.completion_fence) {
            completion_fences_.push_back(CompletionFence{
                .generation = release.generation,
                .slot_index = release.slot_index,
                .sequence = release.sequence,
                .fd = std::move(pending.completion_fence),
            });
            fence_timer_->start();
        }
        pending_releases_.pop_front();
    }
    if (peer_write_notifier_) peer_write_notifier_->setEnabled(false);
}

void PreviewChannel::poll_fences() {
    collect_fences();
    publish_signaled_frames();
    if (pending_frames_.empty() && completion_fences_.empty()) fence_timer_->stop();
}

void PreviewChannel::handle_message(preview::Message message) {
    if (!hello_received_) {
        if (!std::holds_alternative<preview::Hello>(message)) {
            set_failed(tr("The preview protocol did not begin with HELLO."));
            return;
        }
        const auto& hello = std::get<preview::Hello>(message);
        if (auto valid = preview::validate(message); !valid) {
            set_failed(errorText(valid.error()));
            return;
        }
        session_id_ = hello.session_id;
        hello_received_ = true;
        return;
    }

    if (std::holds_alternative<preview::Hello>(message)) {
        set_failed(tr("The preview peer sent HELLO more than once."));
        return;
    }
    if (auto* pool = std::get_if<preview::PoolBegin>(&message)) {
        if (auto begun = descriptor_.begin(*pool); !begun) {
            set_failed(errorText(begun.error()));
            return;
        }
        building_pool_ = std::make_shared<PreviewPoolData>();
        building_pool_->description = *pool;
        last_ready_sequence_ = 0;
        return;
    }
    if (auto* slot = std::get_if<preview::PoolSlot>(&message)) {
        if (!building_pool_) {
            set_failed(tr("A preview slot arrived without a pool."));
            return;
        }
        if (auto added = descriptor_.add_slot(*slot); !added) {
            set_failed(errorText(added.error()));
            return;
        }
        auto& destination = building_pool_->slot_data[slot->slot_index];
        destination.offset = slot->image.offset;
        destination.stride = slot->image.stride;
        destination.allocation_size = slot->image.allocation_size;
        destination.dmabuf_fd = std::move(slot->image.buffer_fd);
        return;
    }
    if (auto* ready = std::get_if<preview::PoolReady>(&message)) {
        if (!building_pool_) {
            set_failed(tr("POOL_READY arrived without an active pool description."));
            return;
        }
        if (auto marked = descriptor_.mark_ready(*ready); !marked) {
            set_failed(errorText(marked.error()));
            return;
        }
        {
            std::lock_guard lock(published_mutex_);
            published_pool_ = building_pool_;
        }
        building_pool_.reset();
        set_state(State::ready);
        emit pool_ready();
        return;
    }
    if (auto* frame = std::get_if<preview::FrameReady>(&message)) {
        if (auto accepted = descriptor_.accept_frame(*frame); !accepted) {
            set_failed(errorText(accepted.error()));
            return;
        }
        std::shared_ptr<const PreviewPoolData> active_pool;
        {
            std::lock_guard lock(published_mutex_);
            active_pool = published_pool_;
        }
        if (!active_pool || active_pool->description.generation != frame->generation) {
            set_failed(tr("A preview frame refers to an unavailable pool generation."));
            return;
        }
        auto data = std::make_shared<PreviewFrameData>();
        data->pool = std::move(active_pool);
        data->slot_index = frame->slot_index;
        data->sequence = frame->sequence;
        data->presentation_time_ns = frame->presentation_time_ns;
        data->acquire_fence_fd = std::move(frame->acquire_fence_fd);
        pending_frames_.push_back(PendingFrame{.frame = std::move(data), .signaled = false});
        if (pending_frames_.size() > preview::max_preview_slots) {
            set_failed(tr("The preview peer exceeded the bounded in-flight frame count."));
            return;
        }
        fence_timer_->start();
        publish_signaled_frames();
        return;
    }
    if (auto* stop = std::get_if<preview::Stop>(&message)) {
        if (!descriptor_.active() || stop->generation != descriptor_.generation()) {
            set_failed(tr("Preview STOP refers to a stale pool generation."));
            return;
        }
        std::shared_ptr<PreviewFrameData> unused_latest;
        {
            std::lock_guard lock(published_mutex_);
            unused_latest = std::exchange(latest_frame_, {});
            published_pool_.reset();
        }
        if (unused_latest) queue_dropped_frame(unused_latest);
        set_state(State::stopped);
        emit frame_available();
        return;
    }
    if (const auto* error = std::get_if<preview::PeerError>(&message)) {
        set_failed(tr("Preview producer error %1: %2")
                       .arg(error->code)
                       .arg(QString::fromStdString(error->message)));
        return;
    }

    set_failed(tr("The preview peer sent a message that is invalid in the consumer direction."));
}

void PreviewChannel::publish_signaled_frames() {
    bool changed = false;
    const State current_state = state();
    for (auto it = pending_frames_.begin(); it != pending_frames_.end();) {
        bool failed = false;
        if (!fenceSignaled(it->frame->acquire_fence_fd.get(), failed)) {
            if (failed) {
                set_failed(tr("Could not poll a preview acquire fence."));
                return;
            }
            ++it;
            continue;
        }
        it->signaled = true;
        std::shared_ptr<PreviewFrameData> ready = std::move(it->frame);
        it = pending_frames_.erase(it);
        if (current_state == State::stopped || current_state == State::failed) {
            queue_dropped_frame(ready);
            continue;
        }
        if (ready->sequence <= last_ready_sequence_) {
            queue_dropped_frame(ready);
            continue;
        }
        last_ready_sequence_ = ready->sequence;
        std::shared_ptr<PreviewFrameData> dropped;
        {
            std::lock_guard lock(published_mutex_);
            dropped = std::exchange(latest_frame_, std::move(ready));
        }
        if (dropped) queue_dropped_frame(dropped);
        changed = true;
    }
    if (changed) emit frame_available();
}

void PreviewChannel::queue_dropped_frame(const std::shared_ptr<PreviewFrameData>& frame) {
    if (!frame || !frame->pool) return;
    preview::UniqueFd fence = duplicateFd(frame->acquire_fence_fd.get());
    if (!fence) {
        set_failed(tr("Could not duplicate a completed preview fence."));
        return;
    }
    enqueue_release(QueuedRelease{
        .message = preview::FrameRelease{
            .generation = frame->pool->description.generation,
            .slot_count = frame->pool->description.slot_count,
            .slot_index = frame->slot_index,
            .sequence = frame->sequence,
            .release_fence_fd = std::move(fence),
        },
        .track_completion = false,
    });
    flush_releases();
}

void PreviewChannel::enqueue_release(QueuedRelease release) {
    if (!peer_ || state_ == State::failed) return;
    pending_releases_.push_back(std::move(release));
}

void PreviewChannel::collect_fences() {
    for (auto it = completion_fences_.begin(); it != completion_fences_.end();) {
        bool failed = false;
        if (!fenceSignaled(it->fd.get(), failed)) {
            if (failed) {
                set_failed(tr("Could not poll a preview release fence."));
                return;
            }
            ++it;
            continue;
        }
        emit slot_release_complete(it->generation, it->slot_index, it->sequence);
        it = completion_fences_.erase(it);
    }
}

void PreviewChannel::set_state(State state) {
    const State previous = state_.exchange(state);
    if (previous == state) return;
    emit state_changed(state);
}

void PreviewChannel::set_failed(const QString& message) {
    if (state_ == State::failed) return;
    close_peer();
    if (listener_notifier_) {
        listener_notifier_->setEnabled(false);
        delete listener_notifier_;
        listener_notifier_ = nullptr;
    }
    if (listener_fd_ >= 0) {
        (void)::close(listener_fd_);
        listener_fd_ = -1;
    }
    fence_timer_->stop();
    pending_frames_.clear();
    pending_releases_.clear();
    completion_fences_.clear();
    building_pool_.reset();
    {
        std::lock_guard lock(published_mutex_);
        latest_frame_.reset();
        published_pool_.reset();
    }
    descriptor_ = preview::PoolDescriptor{};
    socket_path_.clear();
    socket_directory_.reset();
    set_state(State::failed);
    emit socket_path_changed({});
    emit preview_error(message);
    emit frame_available();
}

void PreviewChannel::close_peer() noexcept {
    if (peer_read_notifier_) {
        peer_read_notifier_->setEnabled(false);
        delete peer_read_notifier_;
        peer_read_notifier_ = nullptr;
    }
    if (peer_write_notifier_) {
        peer_write_notifier_->setEnabled(false);
        delete peer_write_notifier_;
        peer_write_notifier_ = nullptr;
    }
    peer_ = preview::SeqPacketSocket{};
    peer_fd_ = -1;
}

} // namespace fluxrec::ui
