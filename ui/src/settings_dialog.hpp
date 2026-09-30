#pragma once

#include "recorder_settings.hpp"

#include <QDialog>

class QCloseEvent;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QSpinBox;

namespace fluxrec::ui {

class WaylandOutputPicker;

class SettingsDialog final : public QDialog {
    Q_OBJECT

public:
    explicit SettingsDialog(const RecorderSettings& settings, QWidget* parent = nullptr,
                            bool scene_source_editable = true);
    [[nodiscard]] RecorderSettings settings() const;

public slots:
    void reject() override;

protected:
    void closeEvent(QCloseEvent* event) override;

private slots:
    void choose_output_directory();
    void choose_executable();
    void accept_if_valid();
    void discard_changes();

private:
    [[nodiscard]] bool has_changes() const;
    [[nodiscard]] bool validate(QString* message) const;
    void confirm_discard();

    RecorderSettings initial_;
    QLineEdit* executable_ = nullptr;
    QLineEdit* output_directory_ = nullptr;
    WaylandOutputPicker* output_picker_ = nullptr;
    QComboBox* codec_ = nullptr;
    QComboBox* encoder_backend_ = nullptr;
    QComboBox* nvenc_preset_ = nullptr;
    QLabel* nvenc_preset_label_ = nullptr;
    QSpinBox* nvenc_b_frames_ = nullptr;
    QLabel* nvenc_b_frames_label_ = nullptr;
    QLabel* nvenc_note_ = nullptr;
    QComboBox* resolution_ = nullptr;
    QWidget* custom_resolution_fields_ = nullptr;
    QSpinBox* resolution_width_ = nullptr;
    QSpinBox* resolution_height_ = nullptr;
    QCheckBox* experimental_hevc_ack_ = nullptr;
    QLabel* experimental_hevc_note_ = nullptr;
    QCheckBox* verify_frame_ids_ = nullptr;
    QComboBox* max_fps_ = nullptr;
    QComboBox* rate_control_ = nullptr;
    QSpinBox* bitrate_mbps_ = nullptr;
    QSpinBox* qp_ = nullptr;
    QLabel* qp_note_ = nullptr;
    QSpinBox* gop_size_ = nullptr;
    QSpinBox* pool_size_ = nullptr;
    QSpinBox* async_depth_ = nullptr;
    QLabel* async_depth_label_ = nullptr;
    int vulkan_async_depth_value_ = 8;
    int nvenc_async_depth_value_ = 4;
    int direct_async_depth_value_ = 12;
    int general_pool_value_ = 48;
    int direct_pool_value_ = 16;
    QString active_encoder_backend_ = QStringLiteral("vulkan");
    QLabel* direct_backend_note_ = nullptr;
    QSpinBox* replay_history_seconds_ = nullptr;
    QCheckBox* audio_enabled_ = nullptr;
    QLabel* validation_message_ = nullptr;
};

} // namespace fluxrec::ui
