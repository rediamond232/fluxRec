#pragma once

#include "recorder_backend.hpp"
#include "preview_channel.hpp"

#include <QByteArray>
#include <QProcess>
#include <QTimer>

namespace fluxrec::ui {

class ProcessRecorderBackend final : public RecorderBackend {
    Q_OBJECT

public:
    explicit ProcessRecorderBackend(const RecorderSettings& settings,
                                    QObject* parent = nullptr);

    [[nodiscard]] State state() const noexcept override { return state_; }
    [[nodiscard]] PreviewChannel* preview_channel() const noexcept { return preview_channel_; }
    [[nodiscard]] bool can_start_recording() const noexcept override;
    [[nodiscard]] bool can_start_replay_only_session() const noexcept override;
    [[nodiscard]] bool replay_only_session() const noexcept override { return active_replay_only_; }
    [[nodiscard]] bool audio_enabled_for_session() const noexcept { return active_audio_enabled_; }
    [[nodiscard]] bool replay_buffer_available() const noexcept override { return replay_available_; }
    [[nodiscard]] bool replay_buffer_reported() const noexcept override { return replay_reported_; }
    [[nodiscard]] bool replay_buffer_active() const noexcept override { return replay_active_; }
    [[nodiscard]] bool replay_control_pending() const noexcept override { return replay_control_pending_; }
    [[nodiscard]] bool replay_control_target_active() const noexcept override { return replay_target_active_; }
    [[nodiscard]] bool can_save_replay() const noexcept override;
    [[nodiscard]] bool can_start_replay_buffer() const noexcept override;
    [[nodiscard]] bool can_stop_replay_buffer() const noexcept override;

public slots:
    void set_settings(const RecorderSettings& settings) override;
    void start_recording(const QString& output_path) override;
    void stop_recording() override;
    void start_replay_buffer() override;
    void stop_replay_buffer() override;
    void save_replay(const QString& output_path) override;
    void set_audio_volume(const QString& source, double linear_gain) override;
    void set_audio_muted(const QString& source, bool muted) override;

private slots:
    void handle_started();
    void handle_finished(int exit_code, QProcess::ExitStatus exit_status);
    void handle_process_error(QProcess::ProcessError error);
    void read_stdout();
    void read_stderr();
    void handle_stop_timeout();
    void handle_replay_control_timeout();

private:
    void refresh_executable();
    void start_replay_only_session();
    void set_state(State state);
    void parse_complete_lines();
    void handle_json_line(const QByteArray& line);
    void send_sigint();
    void append_diagnostic(const QString& line);
    [[nodiscard]] bool write_control_line(const QByteArray& command);
    [[nodiscard]] QString process_error_summary(int exit_code,
                                                QProcess::ExitStatus exit_status) const;

    RecorderSettings settings_;
    PreviewChannel* preview_channel_ = nullptr;
    QString executable_path_;
    State state_ = State::unavailable;
    QProcess process_;
    QTimer stop_timer_;
    QTimer replay_control_timer_;
    QByteArray stdout_buffer_;
    QByteArray stderr_buffer_;
    QString active_output_path_;
    QString last_error_;
    bool stop_requested_ = false;
    bool forced_termination_ = false;
    bool replay_available_ = false;
    bool replay_reported_ = false;
    bool replay_active_ = false;
    bool replay_control_pending_ = false;
    bool replay_target_active_ = false;
    bool active_audio_enabled_ = false;
    bool active_replay_only_ = false;
};

} // namespace fluxrec::ui
