#pragma once

#include "recorder_settings.hpp"

#include <QDateTime>
#include <QJsonObject>
#include <QObject>
#include <QString>

namespace fluxrec::ui {

struct TelemetrySnapshot {
    QJsonObject values;
    QDateTime received_at;
};

class RecorderBackend : public QObject {
    Q_OBJECT

public:
    enum class State {
        unavailable,
        available,
        starting,
        recording,
        replay_only,
        stopping,
        failed,
    };
    Q_ENUM(State)

    explicit RecorderBackend(QObject* parent = nullptr) : QObject(parent) {}
    ~RecorderBackend() override = default;

    [[nodiscard]] virtual State state() const noexcept = 0;
    [[nodiscard]] virtual bool can_start_recording() const noexcept = 0;
    [[nodiscard]] virtual bool can_start_replay_only_session() const noexcept = 0;
    [[nodiscard]] virtual bool replay_only_session() const noexcept = 0;
    [[nodiscard]] virtual bool replay_buffer_available() const noexcept = 0;
    [[nodiscard]] virtual bool replay_buffer_reported() const noexcept = 0;
    [[nodiscard]] virtual bool replay_buffer_active() const noexcept = 0;
    [[nodiscard]] virtual bool replay_control_pending() const noexcept = 0;
    [[nodiscard]] virtual bool replay_control_target_active() const noexcept = 0;
    [[nodiscard]] virtual bool can_save_replay() const noexcept = 0;
    [[nodiscard]] virtual bool can_start_replay_buffer() const noexcept = 0;
    [[nodiscard]] virtual bool can_stop_replay_buffer() const noexcept = 0;

public slots:
    virtual void set_settings(const fluxrec::ui::RecorderSettings& settings) = 0;
    virtual void start_recording(const QString& output_path) = 0;
    virtual void stop_recording() = 0;
    virtual void start_replay_buffer() = 0;
    virtual void stop_replay_buffer() = 0;
    virtual void save_replay(const QString& output_path) = 0;
    virtual void set_audio_volume(const QString& source, double linear_gain) = 0;
    virtual void set_audio_muted(const QString& source, bool muted) = 0;

signals:
    void state_changed(fluxrec::ui::RecorderBackend::State state);
    void telemetry_received(const fluxrec::ui::TelemetrySnapshot& snapshot);
    void replay_buffer_changed(bool active);
    void diagnostic_line(const QString& line);
    void error_changed(const QString& message);
    void recording_finished(const QString& output_path, bool success);
    void replay_session_finished(bool success);
    void replay_save_queued(const QString& output_path);
    void replay_saved(const QString& output_path, bool success);
    void audio_control_changed(const QString& source, const QJsonObject& values);
    void audio_control_failed(const QString& source, const QString& message);
};

} // namespace fluxrec::ui
