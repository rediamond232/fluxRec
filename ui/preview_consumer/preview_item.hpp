#pragma once

#include "preview_channel.hpp"

#include <QQuickItem>
#include <QQuickView>
#include <QQmlError>
#include <QWidget>
#include <QPointer>

#include <memory>

class QQmlComponent;
class QTimer;

namespace fluxrec::ui {

// Call before QApplication is constructed. Qt Quick must select Vulkan before
// its first window is created for the native DMA-BUF texture path.
void configure_preview_quick_backend();

class PreviewItem : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(fluxrec::ui::PreviewChannel* channel READ channel WRITE set_channel
               NOTIFY channel_changed)
    Q_PROPERTY(bool hasFrame READ has_frame NOTIFY presentation_changed)
    Q_PROPERTY(QString statusText READ status_text NOTIFY presentation_changed)

public:
    explicit PreviewItem(QQuickItem* parent = nullptr);
    ~PreviewItem() override;

    [[nodiscard]] PreviewChannel* channel() const noexcept { return channel_; }
    [[nodiscard]] bool has_frame() const noexcept { return has_frame_; }
    [[nodiscard]] QString status_text() const { return status_text_; }
    void set_channel(PreviewChannel* channel);

signals:
    void channel_changed();
    void presentation_changed();
    void preview_unavailable(const QString& reason);
    void retry_state_requested(bool active);
    void discard_pending_ticket_requested();
    void unavailable_report_requested(const QString& reason);

protected:
    QSGNode* updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* data) override;

private:
    void set_retry_active(bool active);
    void report_unavailable(const QString& reason);
    void set_presentation(bool has_frame, const QString& status);
    void release_unconsumed_frame(const std::shared_ptr<PreviewFrameData>& frame);

    QPointer<PreviewChannel> channel_;
    QTimer* retry_timer_ = nullptr;
    std::shared_ptr<PreviewFrameData> pending_frame_;
    bool has_frame_ = false;
    QString status_text_;
};

// Small Widgets adapter that embeds PreviewItem in a configured QQuickView.
// QQuickWidget initializes its render-control device before a subclass can
// request the external-memory and sync-file extensions needed for DMA-BUFs.
class PreviewQuickWidget final : public QWidget {
    Q_OBJECT

public:
    explicit PreviewQuickWidget(PreviewChannel* channel, QWidget* parent = nullptr);
    ~PreviewQuickWidget() override;
    [[nodiscard]] PreviewItem* preview_item() const noexcept { return item_; }
    [[nodiscard]] QQuickView::Status status() const noexcept;
    [[nodiscard]] QObject* rootObject() const noexcept;
    [[nodiscard]] QList<QQmlError> errors() const;
    [[nodiscard]] QQuickWindow* quickWindow() const noexcept { return view_; }

signals:
    void sceneGraphError(QQuickWindow::SceneGraphError error, const QString& message);
    void statusChanged(QQuickView::Status status);

private:
    void complete_component();

    std::unique_ptr<QQmlComponent> component_;
    QQuickView* view_ = nullptr; // Owned by the window container.
    QWidget* container_ = nullptr;
    PreviewItem* item_ = nullptr;
};

} // namespace fluxrec::ui
