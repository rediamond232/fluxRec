#pragma once

#include "process_recorder_backend.hpp"

#include <QElapsedTimer>
#include <QMainWindow>

class QCloseEvent;
class QAction;
class QDockWidget;
class QLabel;
class QStackedWidget;
class QPlainTextEdit;
class QPushButton;
class QTabWidget;
class QTimer;
class QWidget;

namespace fluxrec::ui {

class AudioMixerPanel;
class SceneProfilesPanel;

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(RecorderSettings settings, QWidget* parent = nullptr);

protected:
    void closeEvent(QCloseEvent* event) override;

private slots:
    void open_settings();
    void start_recording();
    void stop_recording();
    void start_replay_buffer();
    void save_replay();
    void refresh_state();
    void update_elapsed_time();
    void handle_recording_finished(const QString& output_path, bool success);
    void handle_replay_session_finished(bool success);

private:
    void create_workspace();
    void create_menus();
    void create_toolbar();
    void apply_settings(const RecorderSettings& settings);
    void apply_scene_configuration();
    void update_controls();
    void update_source_panel();
    void update_output_path();
    void update_capture_path();
    [[nodiscard]] bool process_active() const noexcept;

    RecorderSettings settings_;
    ProcessRecorderBackend* backend_ = nullptr;
    SceneProfilesPanel* source_panel_ = nullptr;
    AudioMixerPanel* audio_panel_ = nullptr;
    QWidget* statistics_panel_ = nullptr;
    QDockWidget* sources_dock_ = nullptr;
    QDockWidget* controls_dock_ = nullptr;
    QDockWidget* workspace_dock_ = nullptr;
    QDockWidget* log_dock_ = nullptr;
    QLabel* engine_state_label_ = nullptr;
    QLabel* toolbar_state_label_ = nullptr;
    QLabel* state_detail_label_ = nullptr;
    QLabel* output_file_label_ = nullptr;
    QLabel* recording_elapsed_label_ = nullptr;
    QLabel* controls_error_label_ = nullptr;
    QLabel* replay_state_label_ = nullptr;
    QPushButton* start_recording_button_ = nullptr;
    QPushButton* stop_recording_button_ = nullptr;
    QPushButton* start_replay_button_ = nullptr;
    QPushButton* stop_replay_button_ = nullptr;
    QPushButton* save_replay_button_ = nullptr;
    QPlainTextEdit* engine_log_ = nullptr;
    QWidget* preview_canvas_ = nullptr;
    QStackedWidget* preview_stack_ = nullptr;
    QWidget* preview_quick_ = nullptr;
    QTabWidget* workspace_tabs_ = nullptr;
    QAction* settings_action_ = nullptr;
    QTimer* refresh_timer_ = nullptr;
    QElapsedTimer recording_clock_;
    bool close_after_session_ = false;
    bool replay_save_pending_ = false;
    QString pending_replay_path_;
    QString active_session_output_name_;
    QString operation_status_;
};

} // namespace fluxrec::ui
