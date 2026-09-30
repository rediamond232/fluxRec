#pragma once

#include "preview/preview.hpp"

#include <QObject>
#include <QString>

#include <array>
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

class QSocketNotifier;
class QTemporaryDir;
class QTimer;

namespace fluxrec::ui {

struct PreviewPoolData {
    struct Slot {
        std::uint64_t offset = 0;
        std::uint32_t stride = 0;
        std::uint64_t allocation_size = 0;
        preview::UniqueFd dmabuf_fd;
    };

    preview::PoolBegin description;
    std::array<Slot, preview::max_preview_slots> slot_data;
};

struct PreviewFrameData {
    std::shared_ptr<const PreviewPoolData> pool;
    std::uint32_t slot_index = 0;
    std::uint64_t sequence = 0;
    std::uint64_t presentation_time_ns = 0;
    preview::UniqueFd acquire_fence_fd;
};

// UI-owned AF_UNIX SOCK_SEQPACKET listener and nonblocking protocol endpoint.
// Vulkan image import and rendering belong to PreviewItem on Qt Quick's render
// thread; this object only owns metadata and file descriptors.
class PreviewChannel final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString socketPath READ socket_path NOTIFY socket_path_changed)

public:
    enum class State { stopped, listening, connected, ready, failed };
    Q_ENUM(State)

    explicit PreviewChannel(QObject* parent = nullptr);
    ~PreviewChannel() override;

    [[nodiscard]] bool start_listening(QString* error = nullptr);
    void stop() noexcept;
    // Stop preview after a render/import error. Safe to call from the Quick
    // render thread; recorder recording remains independent of this channel.
    void fail_preview(const QString& message);

    [[nodiscard]] QString socket_path() const;
    [[nodiscard]] State state() const noexcept { return state_.load(); }
    [[nodiscard]] std::shared_ptr<const PreviewPoolData> pool() const;
    [[nodiscard]] std::shared_ptr<PreviewFrameData> take_latest_frame();

    // Safe to call from the scene-graph render thread. The release record is
    // sent asynchronously; the render thread never waits for IPC.
    void queue_release(std::uint64_t generation, std::uint32_t slot_index,
                       std::uint32_t slot_count, std::uint64_t sequence,
                       preview::UniqueFd release_fence_fd,
                       bool track_gpu_completion = true);

signals:
    void socket_path_changed(const QString& path);
    void state_changed(fluxrec::ui::PreviewChannel::State state);
    void pool_ready();
    void frame_available();
    void slot_release_complete(quint64 generation, quint32 slot_index, quint64 sequence);
    void preview_error(const QString& message);

private slots:
    void accept_peer();
    void read_peer();
    void flush_releases();
    void poll_fences();

private:
    struct PendingFrame {
        std::shared_ptr<PreviewFrameData> frame;
        bool signaled = false;
    };
    struct QueuedRelease {
        preview::Message message;
        preview::UniqueFd completion_fence;
        bool track_completion = false;
    };
    struct CompletionFence {
        std::uint64_t generation = 0;
        std::uint32_t slot_index = 0;
        std::uint64_t sequence = 0;
        preview::UniqueFd fd;
    };

    void set_state(State state);
    void set_failed(const QString& message);
    void close_peer() noexcept;
    void handle_message(preview::Message message);
    void publish_signaled_frames();
    void queue_dropped_frame(const std::shared_ptr<PreviewFrameData>& frame);
    void enqueue_release(QueuedRelease release);
    void collect_fences();

    int listener_fd_ = -1;
    int peer_fd_ = -1;
    QSocketNotifier* listener_notifier_ = nullptr;
    QSocketNotifier* peer_read_notifier_ = nullptr;
    QSocketNotifier* peer_write_notifier_ = nullptr;
    QTimer* fence_timer_ = nullptr;
    std::unique_ptr<QTemporaryDir> socket_directory_;
    QString socket_path_;
    std::atomic<State> state_{State::stopped};
    preview::SeqPacketSocket peer_;
    bool hello_received_ = false;
    std::uint64_t session_id_ = 0;
    std::uint64_t last_ready_sequence_ = 0;
    preview::PoolDescriptor descriptor_;
    std::shared_ptr<PreviewPoolData> building_pool_;
    std::shared_ptr<const PreviewPoolData> published_pool_;
    std::deque<PendingFrame> pending_frames_;
    std::deque<QueuedRelease> pending_releases_;
    std::vector<CompletionFence> completion_fences_;
    mutable std::mutex published_mutex_;
    std::shared_ptr<PreviewFrameData> latest_frame_;
};

} // namespace fluxrec::ui

Q_DECLARE_METATYPE(fluxrec::ui::PreviewChannel::State)
