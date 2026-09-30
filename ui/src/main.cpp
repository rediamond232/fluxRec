#include "audio_device_catalog.hpp"
#include "audio_mixer_panel.hpp"
#include "engine/app_entry.hpp"
#include "main_window.hpp"
#include "preview_item.hpp"
#include "recorder_settings.hpp"
#include "scene_profiles_panel.hpp"
#include "settings_dialog.hpp"
#include "theme.hpp"
#include "wayland_output_picker.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDialog>
#include <QEventLoop>
#include <QFile>
#include <QKeyEvent>
#include <QAbstractButton>
#include <QLabel>
#include <QListWidget>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QJsonObject>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStandardItemModel>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTemporaryDir>
#include <QTimer>

#include <cstdio>
#include <cstring>

int fluxrec_ui_main(int argc, char** argv) {
    if (qEnvironmentVariable("QT_QPA_PLATFORM") != QStringLiteral("offscreen")) {
        fluxrec::ui::configure_preview_quick_backend();
    }
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("Fluxrec"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("fluxrec.local"));
    QCoreApplication::setApplicationName(QStringLiteral("Fluxrec"));
    QGuiApplication::setDesktopFileName(QStringLiteral("fluxrec"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    fluxrec::ui::applyDarkTheme(application);

    bool smoke = false;
    bool validation_auto_start = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--smoke") == 0) smoke = true;
        if (std::strcmp(argv[i], "--validation-auto-start") == 0) {
            validation_auto_start = true;
        }
    }
    if (smoke && validation_auto_start) {
        std::fputs("--validation-auto-start cannot be combined with --smoke.\n", stderr);
        return 2;
    }
    if (validation_auto_start &&
        qEnvironmentVariable("QT_QPA_PLATFORM") == QStringLiteral("offscreen")) {
        std::fputs("--validation-auto-start requires a live Qt desktop platform.\n", stderr);
        return 2;
    }

    QTemporaryDir smoke_settings_directory;
    if (smoke) {
        if (!smoke_settings_directory.isValid()) {
            std::fputs("Could not create isolated settings for UI smoke test.\n", stderr);
            return 2;
        }
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                           smoke_settings_directory.path());
        QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope,
                           smoke_settings_directory.path());
    }

    const fluxrec::ui::RecorderSettings settings = fluxrec::ui::loadSettings();
    fluxrec::ui::MainWindow window{settings};
    window.show();
    if (validation_auto_start) {
        QTimer::singleShot(500, &window, [&window, &application] {
            auto* start_button = window.findChild<QPushButton*>(QStringLiteral("recordButton"));
            if (start_button == nullptr) {
                std::fputs(
                    "Validation auto-start failed: the Start recording button is missing.\n",
                    stderr);
                application.exit(2);
                return;
            }
            if (!start_button->isEnabled()) {
                std::fputs(
                    "Validation auto-start failed: the Start recording button is disabled.\n",
                    stderr);
                application.exit(2);
                return;
            }
            start_button->click();
            std::fputs("Validation auto-start clicked the enabled Start recording button.\n",
                       stderr);
        });
    }
    if (smoke) {
        fluxrec::ui::SettingsDialog settings_probe{fluxrec::ui::RecorderSettings{}, &window};
        auto* rate_control = settings_probe.findChild<QComboBox*>(
            QStringLiteral("rateControlCombo"));
        auto* bitrate = settings_probe.findChild<QSpinBox*>(
            QStringLiteral("bitrateMbpsSpin"));
        auto* bitrate_note = settings_probe.findChild<QLabel*>(
            QStringLiteral("bitrateTargetNote"));
        auto* qp = settings_probe.findChild<QSpinBox*>(QStringLiteral("qpSpin"));
        auto* selected_scene_output = settings_probe.findChild<fluxrec::ui::WaylandOutputPicker*>(
            QStringLiteral("selectedSceneOutputPicker"));
        auto* encoder_backend = settings_probe.findChild<QComboBox*>(
            QStringLiteral("encoderBackendCombo"));
        auto* nvenc_preset = settings_probe.findChild<QComboBox*>(
            QStringLiteral("nvencPresetCombo"));
        auto* nvenc_b_frames = settings_probe.findChild<QSpinBox*>(
            QStringLiteral("nvencBFramesSpin"));
        auto* async_depth = settings_probe.findChild<QSpinBox*>(
            QStringLiteral("encoderAsyncDepthSpin"));
        auto* nvenc_note = settings_probe.findChild<QLabel*>(
            QStringLiteral("nvencValidationNote"));
        auto* frame_identity = settings_probe.findChild<QCheckBox*>(
            QStringLiteral("verifyFrameIdsCheck"));
        auto* frame_identity_note = settings_probe.findChild<QLabel*>(
            QStringLiteral("frameIdentityDiagnosticNote"));
        auto* manual_output_field = selected_scene_output
            ? selected_scene_output->findChild<QLineEdit*>(QStringLiteral("waylandOutputManualEdit"))
            : nullptr;
        auto* output_availability = selected_scene_output
            ? selected_scene_output->findChild<QLabel*>(QStringLiteral("waylandOutputAvailability"))
            : nullptr;
        auto* rate_model = rate_control
            ? qobject_cast<QStandardItemModel*>(rate_control->model()) : nullptr;
        const QStandardItem* cqp_item = rate_model ? rate_model->item(1) : nullptr;
        const fluxrec::ui::RecorderSettings default_values = settings_probe.settings();
        const bool selected_output_detected =
            fluxrec::ui::WaylandOutputPicker::outputIsDetected(default_values.display_name);
        if (!rate_control || !bitrate || !bitrate_note || !qp || !selected_scene_output ||
            !encoder_backend || !nvenc_preset || !nvenc_b_frames || !async_depth || !nvenc_note ||
            !frame_identity || !frame_identity_note || frame_identity->isChecked() ||
            default_values.verify_frame_ids ||
            frame_identity->accessibleName() != QStringLiteral("Enable GPU frame identity diagnostics") ||
            !frame_identity->toolTip().contains(QStringLiteral("may reduce recording FPS")) ||
            !frame_identity->toolTip().contains(QStringLiteral("unreported values remain Not reported")) ||
            !frame_identity_note->text().contains(QStringLiteral("may reduce recording FPS")) ||
            frame_identity_note->text().contains(QStringLiteral("GB/s")) ||
            !manual_output_field || !output_availability ||
            selected_scene_output->outputName() != default_values.display_name ||
            selected_output_detected == !manual_output_field->isHidden() ||
            (selected_output_detected &&
             !output_availability->text().contains(QStringLiteral("Detected"))) ||
            (!selected_output_detected &&
             !output_availability->text().contains(QStringLiteral("Manual fallback"))) ||
            !selected_scene_output->isEnabled() || !cqp_item || !cqp_item->isEnabled() ||
            cqp_item->text() != QStringLiteral("CQP · experimental") ||
            default_values.rate_control != QStringLiteral("bitrate") ||
            default_values.bitrate_mbps != 50 || default_values.qp != 28 ||
            encoder_backend->currentData().toString() != QStringLiteral("nvenc") ||
            encoder_backend->itemText(0) != QStringLiteral("NVIDIA NVENC · default") ||
            encoder_backend->findData(QStringLiteral("vulkan")) < 0 ||
            default_values.encoder_backend != QStringLiteral("nvenc") ||
            default_values.nvenc_preset != QStringLiteral("p1") ||
            default_values.nvenc_b_frames != 0 || nvenc_b_frames->value() != 0 ||
            default_values.async_depth != 8 || default_values.nvenc_async_depth != 4 ||
            async_depth->value() != 4 ||
            async_depth->accessibleName() != QStringLiteral("NVENC asynchronous depth") ||
            nvenc_preset->count() != 7 || nvenc_preset->isHidden() ||
            !nvenc_preset->isEnabled() || nvenc_note->isHidden() ||
            nvenc_b_frames->isHidden() || !nvenc_b_frames->isEnabled() ||
            !nvenc_note->text().contains(QStringLiteral("H.264 and HEVC runs at depth 8")) ||
            !nvenc_note->text().contains(QStringLiteral("periodic black output")) ||
            !nvenc_note->text().contains(QStringLiteral("use depth 4 for normal recordings")) ||
            !bitrate->isEnabled() || qp->isEnabled() || qp->minimum() != 0 ||
            qp->maximum() != 51 ||
            rate_control->accessibleName() != QStringLiteral("Video rate control") ||
            bitrate->accessibleName() != QStringLiteral("Average bitrate target") ||
            !bitrate->toolTip().contains(QStringLiteral("not a maximum bitrate or file-size limit")) ||
            !bitrate->accessibleDescription().contains(
                QStringLiteral("Statistics shows the observed output bitrate")) ||
            !bitrate_note->text().contains(
                QStringLiteral("Statistics shows the observed output bitrate")) ||
            qp->accessibleName() != QStringLiteral("Constant quantizer QP")) {
            std::fputs("CQP settings controls do not match their bitrate-mode defaults.\n",
                       stderr);
            return 2;
        }

        fluxrec::ui::RecorderSettings vulkan_probe_settings;
        vulkan_probe_settings.encoder_backend = QStringLiteral("vulkan");
        fluxrec::ui::SettingsDialog backend_probe{vulkan_probe_settings, &window};
        auto* backend_choice = backend_probe.findChild<QComboBox*>(
            QStringLiteral("encoderBackendCombo"));
        auto* preset_choice = backend_probe.findChild<QComboBox*>(
            QStringLiteral("nvencPresetCombo"));
        auto* b_frames_choice = backend_probe.findChild<QSpinBox*>(
            QStringLiteral("nvencBFramesSpin"));
        auto* depth_choice = backend_probe.findChild<QSpinBox*>(
            QStringLiteral("encoderAsyncDepthSpin"));
        if (!backend_choice || !preset_choice || !b_frames_choice || !depth_choice) {
            std::fputs("Encoder backend controls are missing from Settings.\n", stderr);
            return 2;
        }
        backend_choice->setCurrentIndex(backend_choice->findData(QStringLiteral("nvenc")));
        if (depth_choice->value() != 4 || b_frames_choice->value() != 0 ||
            b_frames_choice->isHidden() || !b_frames_choice->isEnabled() ||
            preset_choice->isHidden() ||
            !preset_choice->isEnabled() || backend_probe.settings().encoder_backend !=
                QStringLiteral("nvenc") ||
            !depth_choice->toolTip().contains(QStringLiteral("depth 8 produced periodic black output")) ||
            !depth_choice->toolTip().contains(QStringLiteral("use depth 4 for normal recordings")) ||
            depth_choice->accessibleDescription() != depth_choice->toolTip()) {
            std::fputs("Selecting NVENC did not show its controls or load async depth 4.\n", stderr);
            return 2;
        }
        depth_choice->setValue(5);
        b_frames_choice->setValue(2);
        preset_choice->setCurrentIndex(preset_choice->findData(QStringLiteral("p4")));
        backend_choice->setCurrentIndex(backend_choice->findData(QStringLiteral("vulkan")));
        if (depth_choice->value() != 8 || !preset_choice->isHidden() ||
            preset_choice->isEnabled() || !b_frames_choice->isHidden() ||
            b_frames_choice->isEnabled()) {
            std::fputs("Vulkan Video must restore its own async depth and hide the NVENC preset.\n",
                       stderr);
            return 2;
        }
        depth_choice->setValue(6);
        backend_choice->setCurrentIndex(backend_choice->findData(QStringLiteral("nvenc")));
        const fluxrec::ui::RecorderSettings encoder_round_trip = backend_probe.settings();
        if (depth_choice->value() != 5 || encoder_round_trip.encoder_backend !=
                QStringLiteral("nvenc") || encoder_round_trip.nvenc_preset !=
                QStringLiteral("p4") || encoder_round_trip.nvenc_b_frames != 2 ||
            encoder_round_trip.async_depth != 6 ||
            encoder_round_trip.nvenc_async_depth != 5) {
            std::fputs("Backend preset, B-frame setting, or async depth did not persist in Settings.\n",
                       stderr);
            return 2;
        }

        fluxrec::ui::RecorderSettings direct_initial;
        direct_initial.encoder_backend = QStringLiteral("experimental-cuda-egl");
        direct_initial.pool_size = 48;
        direct_initial.direct_pool_size = 16;
        direct_initial.output_width = 1920;
        direct_initial.output_height = 1080;
        direct_initial.rate_control = QStringLiteral("constant_qp");
        direct_initial.verify_frame_ids = true;
        fluxrec::ui::SettingsDialog direct_probe{direct_initial, &window};
        auto* direct_backend = direct_probe.findChild<QComboBox*>(
            QStringLiteral("encoderBackendCombo"));
        auto* direct_depth = direct_probe.findChild<QSpinBox*>(
            QStringLiteral("encoderAsyncDepthSpin"));
        auto* direct_note = direct_probe.findChild<QLabel*>(
            QStringLiteral("directCudaEglNote"));
        auto* direct_identity = direct_probe.findChild<QCheckBox*>(
            QStringLiteral("verifyFrameIdsCheck"));
        auto* direct_rate = direct_probe.findChild<QComboBox*>(
            QStringLiteral("rateControlCombo"));
        auto* direct_rate_model = direct_rate
            ? qobject_cast<QStandardItemModel*>(direct_rate->model()) : nullptr;
        const QStandardItem* direct_cqp_item = direct_rate_model
            ? direct_rate_model->item(direct_rate->findData(QStringLiteral("constant_qp")))
            : nullptr;
        if (!direct_backend || !direct_depth || !direct_note || !direct_identity ||
            !direct_rate || !direct_cqp_item || !direct_cqp_item->isEnabled() ||
            direct_backend->findData(QStringLiteral("experimental-cuda-egl")) < 0 ||
            direct_backend->currentData().toString() !=
                QStringLiteral("experimental-cuda-egl") ||
            direct_rate->currentData().toString() != QStringLiteral("constant_qp") ||
            direct_depth->value() != 12 || direct_depth->maximum() != 16 ||
            direct_note->isHidden() ||
            !direct_note->text().contains(QStringLiteral("not allocation-level zero-copy")) ||
            !direct_note->text().contains(QStringLiteral("experimental constant QP")) ||
            !direct_probe.findChild<QLabel*>(QStringLiteral("gpuConversionNote")) ||
            !direct_probe.findChild<QLabel*>(QStringLiteral("gpuConversionNote"))->text()
                .contains(QStringLiteral("does not run the Vulkan two-pass converter")) ||
            direct_identity->isEnabled() ||
            direct_probe.settings().pool_size != 48 ||
            direct_probe.settings().direct_pool_size != 16 ||
            direct_probe.settings().rate_control != QStringLiteral("constant_qp") ||
            direct_probe.settings().output_width != 1920) {
            std::fputs("Direct CUDA EGL settings did not expose its constraints.\n", stderr);
            return 2;
        }
        direct_rate->setCurrentIndex(direct_rate->findData(QStringLiteral("bitrate")));
        auto* direct_resolution = direct_probe.findChild<QComboBox*>();
        for (auto* combo : direct_probe.findChildren<QComboBox*>()) {
            if (combo->findData(QStringLiteral("native")) >= 0 &&
                combo->findData(QStringLiteral("1920x1080")) >= 0) {
                direct_resolution = combo;
                break;
            }
        }
        if (!direct_resolution) {
            std::fputs("Direct CUDA EGL resolution control is missing.\n", stderr);
            return 2;
        }
        direct_resolution->setCurrentIndex(direct_resolution->findData(QStringLiteral("native")));
        const fluxrec::ui::RecorderSettings direct_ready = direct_probe.settings();
        if (direct_ready.output_width != 0 || direct_ready.output_height != 0 ||
            direct_ready.rate_control != QStringLiteral("bitrate") ||
            direct_ready.pool_size != 48 || direct_ready.direct_pool_size != 16 ||
            direct_ready.direct_async_depth != 12 || !direct_ready.verify_frame_ids ||
            direct_identity->isEnabled()) {
            std::fputs("Direct CUDA EGL did not keep separate pool settings or clear native size.\n",
                       stderr);
            return 2;
        }
        fluxrec::ui::MainWindow direct_window{direct_ready};
        auto* direct_route = direct_window.findChild<QLabel*>(QStringLiteral("pipelineRouteNote"));
        auto* direct_stage = direct_window.findChild<QLabel*>(QStringLiteral("pipelineStage1"));
        auto* direct_toolbar = direct_window.findChild<QLabel*>(QStringLiteral("toolbarBackend"));
        auto* direct_preview = direct_window.findChild<QWidget*>(QStringLiteral("previewCanvas"));
        if (!direct_route || !direct_stage || !direct_toolbar || !direct_preview ||
            direct_route->text() != QStringLiteral("Direct CUDA EGL") ||
            direct_stage->text() != QStringLiteral("EGL") ||
            direct_toolbar->text() != QStringLiteral("Backend · Direct CUDA EGL") ||
            direct_preview->accessibleName() != QStringLiteral("GPU preview not used") ||
            !direct_route->accessibleName().contains(QStringLiteral("not allocation-level zero-copy"))) {
            std::fputs("Direct CUDA EGL was not shown on the workbench rail.\n", stderr);
            return 2;
        }

        rate_control->setFocus();
        QKeyEvent move_to_cqp{QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier};
        QApplication::sendEvent(rate_control, &move_to_cqp);
        QKeyEvent release_cqp{QEvent::KeyRelease, Qt::Key_Down, Qt::NoModifier};
        QApplication::sendEvent(rate_control, &release_cqp);
        qp->setValue(31);
        const fluxrec::ui::RecorderSettings remembered = settings_probe.settings();
        fluxrec::ui::SettingsDialog busy_settings_probe{remembered, &window, false};
        auto* locked_scene_output = busy_settings_probe.findChild<fluxrec::ui::WaylandOutputPicker*>(
            QStringLiteral("selectedSceneOutputPicker"));
        if (!locked_scene_output || locked_scene_output->isEnabled() ||
            !locked_scene_output->toolTip().contains(QStringLiteral("cannot be changed"))) {
            std::fputs("Settings must lock scene output edits during an active session.\n", stderr);
            return 2;
        }
        if (rate_control->currentIndex() != 1 || bitrate->isEnabled() || !qp->isEnabled()) {
            std::fputs("Selecting CQP did not enable QP and disable the bitrate field.\n", stderr);
            return 2;
        }
        rate_control->setCurrentIndex(0);
        const fluxrec::ui::RecorderSettings switched_back = settings_probe.settings();
        if (!bitrate->isEnabled() || qp->isEnabled() ||
            switched_back.rate_control != QStringLiteral("bitrate") ||
            switched_back.bitrate_mbps != 50 || switched_back.qp != 31) {
            std::fputs("Switching rate-control mode did not preserve inactive values.\n", stderr);
            return 2;
        }
        rate_control->setCurrentIndex(1);
        fluxrec::ui::RecorderSettings scene_round_trip_settings = remembered;
        scene_round_trip_settings.scenes = {
            {QStringLiteral("scene-main"), QStringLiteral("Main desktop"), QStringLiteral("DP-1")},
            {QStringLiteral("scene-projector"), QStringLiteral("Projector"),
             QStringLiteral("HDMI-A-1")},
        };
        scene_round_trip_settings.selected_scene_id = QStringLiteral("scene-projector");
        scene_round_trip_settings.display_name = QStringLiteral("HDMI-A-1");
        scene_round_trip_settings.encoder_backend = QStringLiteral("nvenc");
        scene_round_trip_settings.nvenc_preset = QStringLiteral("p4");
        scene_round_trip_settings.nvenc_b_frames = 3;
        scene_round_trip_settings.async_depth = 6;
        scene_round_trip_settings.nvenc_async_depth = 5;
        fluxrec::ui::saveSettings(scene_round_trip_settings);
        const fluxrec::ui::RecorderSettings round_trip = fluxrec::ui::loadSettings();
        if (scene_round_trip_settings.rate_control != QStringLiteral("constant_qp") ||
            scene_round_trip_settings.bitrate_mbps != 50 || scene_round_trip_settings.qp != 31 ||
            round_trip.rate_control != QStringLiteral("constant_qp") ||
            round_trip.bitrate_mbps != 50 || round_trip.qp != 31 ||
            round_trip.scenes != scene_round_trip_settings.scenes ||
            round_trip.selected_scene_id != QStringLiteral("scene-projector") ||
            round_trip.display_name != QStringLiteral("HDMI-A-1") ||
            round_trip.encoder_backend != QStringLiteral("nvenc") ||
            round_trip.nvenc_preset != QStringLiteral("p4") ||
            round_trip.nvenc_b_frames != 3 ||
            round_trip.verify_frame_ids ||
            round_trip.async_depth != 6 || round_trip.nvenc_async_depth != 5) {
            std::fputs("Recorder and scene settings did not round-trip correctly.\n", stderr);
            return 2;
        }

        scene_round_trip_settings.verify_frame_ids = true;
        fluxrec::ui::saveSettings(scene_round_trip_settings);
        const fluxrec::ui::RecorderSettings identity_round_trip = fluxrec::ui::loadSettings();
        if (!identity_round_trip.verify_frame_ids) {
            std::fputs("GPU frame identity diagnostic setting did not persist.\n", stderr);
            return 2;
        }
        scene_round_trip_settings.encoder_backend = QStringLiteral("vulkan");
        fluxrec::ui::saveSettings(scene_round_trip_settings);
        const fluxrec::ui::RecorderSettings saved_vulkan_choice = fluxrec::ui::loadSettings();
        if (saved_vulkan_choice.encoder_backend != QStringLiteral("vulkan")) {
            std::fputs("An explicitly saved Vulkan Video backend choice was not preserved.\n",
                       stderr);
            return 2;
        }

        scene_round_trip_settings.audio_tracks_custom = true;
        scene_round_trip_settings.audio_tracks = {
            {QStringLiteral("desktop"), QStringLiteral("Desktop audio"),
             QStringLiteral("sink-monitor"), QString(), QString()},
            {QStringLiteral("track-1"), QStringLiteral("Headset"),
             QStringLiteral("source"), QStringLiteral("alsa_input.usb"),
             QStringLiteral("alsa_output.speakers")},
        };
        scene_round_trip_settings.audio_enabled = true;
        fluxrec::ui::saveSettings(scene_round_trip_settings);
        const auto audio_round_trip = fluxrec::ui::loadSettings();
        if (!audio_round_trip.audio_tracks_custom ||
            audio_round_trip.audio_tracks != scene_round_trip_settings.audio_tracks ||
            !audio_round_trip.audio_enabled) {
            std::fputs("audio tracks did not round-trip\n", stderr);
            return 2;
        }
        const auto default_effective =
            fluxrec::ui::effectiveAudioTracks(fluxrec::ui::RecorderSettings{});
        if (default_effective.size() != 2 ||
            default_effective[0].id != QStringLiteral("desktop") ||
            default_effective[0].name != QStringLiteral("Desktop audio") ||
            default_effective[0].input_kind != QStringLiteral("sink-monitor") ||
            !default_effective[0].input_target.isEmpty() ||
            !default_effective[0].monitor_target.isEmpty() ||
            default_effective[1].id != QStringLiteral("microphone") ||
            default_effective[1].name != QStringLiteral("Microphone") ||
            default_effective[1].input_kind != QStringLiteral("source") ||
            !default_effective[1].input_target.isEmpty() ||
            !default_effective[1].monitor_target.isEmpty()) {
            std::fputs("effectiveAudioTracks defaults were not the two default tracks.\n", stderr);
            return 2;
        }
        fluxrec::ui::RecorderSettings empty_custom;
        empty_custom.audio_tracks_custom = true;
        empty_custom.audio_tracks = {};
        if (!fluxrec::ui::effectiveAudioTracks(empty_custom).isEmpty()) {
            std::fputs("A custom empty audio track list must stay empty.\n", stderr);
            return 2;
        }

        fluxrec::ui::RecorderSettings duplicate_custom = scene_round_trip_settings;
        duplicate_custom.audio_tracks_custom = true;
        duplicate_custom.audio_tracks = {
            {QStringLiteral("desktop"), QStringLiteral("Desktop audio"),
             QStringLiteral("sink-monitor"), QString(), QString()},
            {QStringLiteral("desktop"), QStringLiteral("Copy"),
             QStringLiteral("source"), QString(), QString()},
        };
        fluxrec::ui::saveSettings(duplicate_custom);
        const auto duplicate_loaded = fluxrec::ui::loadSettings();
        if (duplicate_loaded.audio_tracks_custom ||
            fluxrec::ui::effectiveAudioTracks(duplicate_loaded) != default_effective) {
            std::fputs("A duplicate audio track id did not fall back to defaults.\n", stderr);
            return 2;
        }

        fluxrec::ui::RecorderSettings blank_name_custom = scene_round_trip_settings;
        blank_name_custom.audio_tracks_custom = true;
        blank_name_custom.audio_tracks = {
            {QStringLiteral("desktop"), QStringLiteral("   "),
             QStringLiteral("sink-monitor"), QString(), QString()},
        };
        fluxrec::ui::saveSettings(blank_name_custom);
        const auto blank_name_loaded = fluxrec::ui::loadSettings();
        if (blank_name_loaded.audio_tracks_custom ||
            fluxrec::ui::effectiveAudioTracks(blank_name_loaded) != default_effective) {
            std::fputs("A blank audio track name did not fall back to defaults.\n", stderr);
            return 2;
        }

        fluxrec::ui::RecorderSettings nine_custom = scene_round_trip_settings;
        nine_custom.audio_tracks_custom = true;
        nine_custom.audio_tracks.clear();
        for (int index = 0; index < 9; ++index) {
            nine_custom.audio_tracks.push_back({
                QStringLiteral("track-%1").arg(index + 1),
                QStringLiteral("Track %1").arg(index + 1),
                QStringLiteral("source"), QString(), QString(),
            });
        }
        fluxrec::ui::saveSettings(nine_custom);
        const auto nine_loaded = fluxrec::ui::loadSettings();
        if (nine_loaded.audio_tracks_custom ||
            fluxrec::ui::effectiveAudioTracks(nine_loaded) != default_effective) {
            std::fputs("A ninth audio track entry did not fall back to defaults.\n", stderr);
            return 2;
        }

        QStringList audio_arguments;
        fluxrec::ui::appendAudioTrackArguments(audio_arguments, scene_round_trip_settings);
        const QStringList expected_audio_arguments{
            QStringLiteral("--audio"), QStringLiteral("on"),
            QStringLiteral("--audio-track"), QStringLiteral("desktop"),
            QStringLiteral("Desktop audio"), QStringLiteral("sink-monitor"),
            QStringLiteral("-"), QStringLiteral("-"),
            QStringLiteral("--audio-track"), QStringLiteral("track-1"),
            QStringLiteral("Headset"), QStringLiteral("source"),
            QStringLiteral("alsa_input.usb"), QStringLiteral("alsa_output.speakers"),
        };
        if (audio_arguments != expected_audio_arguments) {
            std::fputs("appendAudioTrackArguments did not emit the expected track list.\n", stderr);
            return 2;
        }
        fluxrec::ui::RecorderSettings default_launch;
        default_launch.audio_enabled = true;
        default_launch.audio_tracks_custom = false;
        QStringList default_launch_arguments;
        fluxrec::ui::appendAudioTrackArguments(default_launch_arguments, default_launch);
        if (default_launch_arguments != QStringList{
                QStringLiteral("--audio"), QStringLiteral("on"),
                QStringLiteral("--audio-tracks"), QStringLiteral("default")}) {
            std::fputs("appendAudioTrackArguments must emit --audio-tracks default for "
                       "unsaved tracks.\n",
                       stderr);
            return 2;
        }
        fluxrec::ui::RecorderSettings empty_custom_launch;
        empty_custom_launch.audio_enabled = true;
        empty_custom_launch.audio_tracks_custom = true;
        empty_custom_launch.audio_tracks = {};
        QStringList empty_custom_arguments;
        fluxrec::ui::appendAudioTrackArguments(empty_custom_arguments, empty_custom_launch);
        if (empty_custom_arguments !=
                QStringList{QStringLiteral("--audio"), QStringLiteral("on")} ||
            empty_custom_arguments.contains(QStringLiteral("--audio-track")) ||
            empty_custom_arguments.contains(QStringLiteral("--audio-tracks"))) {
            std::fputs("appendAudioTrackArguments must emit --audio on only for a custom "
                       "empty track list.\n",
                       stderr);
            return 2;
        }
        fluxrec::ui::RecorderSettings audio_off = scene_round_trip_settings;
        audio_off.audio_enabled = false;
        QStringList audio_off_arguments;
        fluxrec::ui::appendAudioTrackArguments(audio_off_arguments, audio_off);
        if (audio_off_arguments !=
                QStringList{QStringLiteral("--audio"), QStringLiteral("off")} ||
            audio_off_arguments.contains(QStringLiteral("--audio-track"))) {
            std::fputs("appendAudioTrackArguments must emit --audio off without tracks.\n",
                       stderr);
            return 2;
        }

        if (!fluxrec::ui::isAudioSourceNode(QStringLiteral("Audio/Source"),
                                            QStringLiteral("alsa_input.usb")) ||
            fluxrec::ui::isAudioSourceNode(QStringLiteral("Audio/Source"),
                                           QStringLiteral("speakers.monitor")) ||
            !fluxrec::ui::isAudioSinkNode(QStringLiteral("Audio/Sink")) ||
            fluxrec::ui::isAudioSourceNode(QStringLiteral("Video/Source"),
                                           QStringLiteral("camera")) ||
            fluxrec::ui::isAudioSinkNode(QStringLiteral("Audio/Source"))) {
            std::fputs("PipeWire audio node classification helpers are wrong.\n", stderr);
            return 2;
        }
        const QList<fluxrec::ui::AudioDeviceEntry> duplicated_inputs{
            {QStringLiteral("alsa_input.quadcast"),
             QStringLiteral("HyperX QuadCast S Analog Stereo"), QStringLiteral("source")},
            {QStringLiteral("alsa_output.quadcast"),
             QStringLiteral("HyperX QuadCast S Analog Stereo"), QStringLiteral("sink-monitor")},
            {QStringLiteral("alsa_output.built_in"),
             QStringLiteral("Built-in Audio Analog Stereo"), QStringLiteral("sink-monitor")},
        };
        const QList<fluxrec::ui::AudioDeviceEntry> unique_inputs =
            fluxrec::ui::dedupeAudioInputs(duplicated_inputs);
        if (unique_inputs.size() != 2 ||
            unique_inputs[0].target != QStringLiteral("alsa_input.quadcast") ||
            unique_inputs[1].target != QStringLiteral("alsa_output.built_in")) {
            std::fputs("Input devices that share a name with an output were not deduplicated.\n",
                       stderr);
            return 2;
        }

        fluxrec::ui::AudioMixerPanel mixer_probe{&window};
        QList<fluxrec::ui::AudioTrackSettings> mixer_edited;
        QObject::connect(&mixer_probe, &fluxrec::ui::AudioMixerPanel::tracksEdited,
                         &application,
                         [&mixer_edited](const QList<fluxrec::ui::AudioTrackSettings>& tracks) {
                             mixer_edited = tracks;
                         });
        const auto mixer_defaults =
            fluxrec::ui::effectiveAudioTracks(fluxrec::ui::RecorderSettings{});
        mixer_probe.setTracks(mixer_defaults, false);
        auto* add_audio_track =
            mixer_probe.findChild<QPushButton*>(QStringLiteral("addAudioTrackButton"));
        if (!add_audio_track) {
            std::fputs("addAudioTrackButton is not found.\n", stderr);
            return 2;
        }
        for (int click = 0; click < 6; ++click) {
            add_audio_track->click();
        }
        const auto mixer_rows =
            mixer_probe.findChildren<QWidget*>(QStringLiteral("audioTrackRow"));
        if (mixer_rows.size() != 8 || mixer_edited.size() != 8) {
            std::fputs("Adding six tracks did not produce eight mixer rows.\n", stderr);
            return 2;
        }
        for (int index = 0; index < 6; ++index) {
            if (mixer_edited[index + 2].id !=
                QStringLiteral("track-%1").arg(index + 1)) {
                std::fputs("Added mixer tracks did not use track-1 through track-6.\n",
                           stderr);
                return 2;
            }
        }
        const fluxrec::ui::AudioTrackSettings& last_added = mixer_edited.last();
        if (last_added.id != QStringLiteral("track-6") ||
            last_added.name != QStringLiteral("Track 6") ||
            last_added.input_kind != QStringLiteral("source") ||
            !last_added.input_target.isEmpty() || !last_added.monitor_target.isEmpty()) {
            std::fputs("The sixth added mixer track did not match Track 6 defaults.\n",
                       stderr);
            return 2;
        }
        if (add_audio_track->isEnabled()) {
            std::fputs("addAudioTrackButton stayed enabled at eight rows.\n", stderr);
            return 2;
        }
        mixer_probe.setSessionBusy(true);
        if (add_audio_track->isEnabled()) {
            std::fputs("addAudioTrackButton stayed enabled while the session was busy.\n",
                       stderr);
            return 2;
        }
        for (QWidget* row : mixer_rows) {
            auto* remove =
                row->findChild<QPushButton*>(QStringLiteral("removeAudioTrackButton"));
            auto* input = row->findChild<QComboBox*>(QStringLiteral("audioTrackInput"));
            auto* output = row->findChild<QComboBox*>(QStringLiteral("audioTrackOutput"));
            if (!remove || !input || !output || remove->isEnabled() || input->isEnabled() ||
                output->isEnabled()) {
                std::fputs(
                    "Session busy did not disable remove buttons and device combos.\n",
                    stderr);
                return 2;
            }
        }
        mixer_probe.setSessionBusy(false);
        if (add_audio_track->isEnabled()) {
            std::fputs("addAudioTrackButton re-enabled past eight rows after the session ended.\n",
                       stderr);
            return 2;
        }
        for (QWidget* row : mixer_rows) {
            auto* remove =
                row->findChild<QPushButton*>(QStringLiteral("removeAudioTrackButton"));
            auto* input = row->findChild<QComboBox*>(QStringLiteral("audioTrackInput"));
            auto* output = row->findChild<QComboBox*>(QStringLiteral("audioTrackOutput"));
            if (!remove || !input || !output || !remove->isEnabled() || !input->isEnabled() ||
                !output->isEnabled()) {
                std::fputs(
                    "Leaving a busy session did not re-enable remove buttons and combos.\n",
                    stderr);
                return 2;
            }
        }

        mixer_probe.setTracks(
            {{QStringLiteral("desktop"), QStringLiteral("Desktop audio"),
              QStringLiteral("source"), QStringLiteral("missing-mic"), QString()}},
            true);
        fluxrec::ui::AudioDeviceCatalog unavailable_catalog;
        unavailable_catalog.assign(
            {{QStringLiteral("real-mic"), QStringLiteral("Real microphone"),
              QStringLiteral("source")}},
            {}, QStringLiteral("Audio devices are unavailable."));
        mixer_probe.setDevices(unavailable_catalog);
        auto* missing_input =
            mixer_probe.findChild<QComboBox*>(QStringLiteral("audioTrackInput"));
        bool missing_marked = false;
        if (missing_input) {
            for (int index = 0; index < missing_input->count(); ++index) {
                if (missing_input->itemText(index) ==
                    QStringLiteral("missing-mic (unavailable)")) {
                    missing_marked = true;
                    break;
                }
            }
        }
        bool unavailable_label = false;
        for (QLabel* label : mixer_probe.findChildren<QLabel*>()) {
            if (label->text() == QStringLiteral("Audio devices are unavailable.")) {
                unavailable_label = true;
                break;
            }
        }
        if (!missing_marked || !unavailable_label) {
            std::fputs(
                "Missing audio devices were not kept with an unavailable marker and label.\n",
                stderr);
            return 2;
        }

        const QList<fluxrec::ui::SceneProfile> sample_scenes{
            {QStringLiteral("scene-main"), QStringLiteral("Main desktop"), QStringLiteral("DP-1")},
            {QStringLiteral("scene-projector"), QStringLiteral("Projector"),
             QStringLiteral("HDMI-A-1")},
        };
        fluxrec::ui::SceneProfilesPanel scene_probe{
            sample_scenes, QStringLiteral("scene-main"), &window};
        auto* scene_list = scene_probe.findChild<QListWidget*>(QStringLiteral("sceneList"));
        auto* selected_source = scene_probe.findChild<QLabel*>(QStringLiteral("sceneSourceName"));
        auto* source_status = scene_probe.findChild<QLabel*>(QStringLiteral("sceneSourceStatus"));
        auto* add_scene = scene_probe.findChild<QPushButton*>(QStringLiteral("addSceneButton"));
        auto* rename_scene = scene_probe.findChild<QPushButton*>(QStringLiteral("renameSceneButton"));
        auto* remove_scene = scene_probe.findChild<QPushButton*>(QStringLiteral("removeSceneButton"));
        auto* edit_output = scene_probe.findChild<QPushButton*>(QStringLiteral("editSceneOutputButton"));
        int scene_change_count = 0;
        QObject::connect(&scene_probe, &fluxrec::ui::SceneProfilesPanel::configurationChanged,
                         &application, [&scene_change_count] { ++scene_change_count; });
        if (!scene_list || !selected_source || !source_status || !add_scene || !rename_scene ||
            !remove_scene || !edit_output || scene_list->count() != 2 ||
            scene_probe.selectedOutputName() != QStringLiteral("DP-1") ||
            !add_scene->isEnabled() || !rename_scene->isEnabled() || !remove_scene->isEnabled() ||
            !edit_output->isEnabled()) {
            std::fputs("Scene/source panel did not initialize with the selected profile.\n", stderr);
            return 2;
        }
        scene_list->setCurrentRow(1);
        if (scene_probe.selectedSceneId() != QStringLiteral("scene-projector") ||
            scene_probe.selectedOutputName() != QStringLiteral("HDMI-A-1") ||
            scene_change_count != 1 ||
            selected_source->text() != QStringLiteral("Projector · HDMI-A-1")) {
            std::fputs("Selecting a scene did not update its output source.\n", stderr);
            return 2;
        }

        const auto drive_scene_dialog = [&application](QPushButton* trigger, auto configure) {
            bool dialog_seen = false;
            QTimer watchdog;
            watchdog.setSingleShot(true);
            QObject::connect(&watchdog, &QTimer::timeout, &application, [] {
                if (QWidget* modal = QApplication::activeModalWidget()) modal->close();
            });
            QTimer::singleShot(0, &application, [&application, &dialog_seen, configure] {
                if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget())) {
                    dialog_seen = true;
                    configure(dialog);
                }
            });
            watchdog.start(2000);
            trigger->click();
            watchdog.stop();
            return dialog_seen;
        };
        const auto confirm_scene_dialog = [](QDialog* dialog) {
            if (auto* confirm = dialog->findChild<QPushButton*>(
                    QStringLiteral("confirmSceneEditButton"))) {
                confirm->click();
            }
        };
        bool scene_validation_accessible = false;
        if (!drive_scene_dialog(add_scene, [&](QDialog* dialog) {
                auto* name = dialog->findChild<QLineEdit*>(QStringLiteral("sceneNameEdit"));
                auto* output = dialog->findChild<fluxrec::ui::WaylandOutputPicker*>(
                    QStringLiteral("sceneOutputPicker"));
                auto* error = dialog->findChild<QLabel*>(QStringLiteral("validationMessage"));
                if (!name || !output || !error) return;
                name->clear();
                output->setOutputName(QStringLiteral("HDMI-A-2"));
                confirm_scene_dialog(dialog);
                scene_validation_accessible = error->isVisible() &&
                    name->accessibleDescription() == error->text();
                name->setText(QStringLiteral("Presentation"));
                scene_validation_accessible = scene_validation_accessible &&
                    !error->isVisible() && name->accessibleDescription().isEmpty();
                confirm_scene_dialog(dialog);
            }) || !scene_validation_accessible || scene_list->count() != 3 ||
            scene_probe.selectedOutputName() != QStringLiteral("HDMI-A-2")) {
            std::fputs("Adding a scene did not validate accessibly and select its output profile.\n", stderr);
            return 2;
        }
        if (!drive_scene_dialog(rename_scene, [&](QDialog* dialog) {
                if (auto* name = dialog->findChild<QLineEdit*>(QStringLiteral("sceneNameEdit"))) {
                    name->setText(QStringLiteral("Presentation edited"));
                }
                confirm_scene_dialog(dialog);
            }) || !scene_probe.profiles().last().name.contains(QStringLiteral("edited"))) {
            std::fputs("Renaming a scene did not update its saved name.\n", stderr);
            return 2;
        }
        bool output_dialog_hides_scene_name = false;
        if (!drive_scene_dialog(edit_output, [&](QDialog* dialog) {
                auto* name = dialog->findChild<QLineEdit*>(QStringLiteral("sceneNameEdit"));
                auto* output = dialog->findChild<fluxrec::ui::WaylandOutputPicker*>(
                    QStringLiteral("sceneOutputPicker"));
                output_dialog_hides_scene_name = name && !name->isVisible();
                if (output) {
                    output->setOutputName(QStringLiteral("DP-3"));
                }
                confirm_scene_dialog(dialog);
            }) || !output_dialog_hides_scene_name ||
            scene_probe.selectedOutputName() != QStringLiteral("DP-3")) {
            std::fputs("Editing a scene source did not update its Wayland output.\n", stderr);
            return 2;
        }
        if (!drive_scene_dialog(remove_scene, [](QDialog* dialog) {
                auto* confirmation = qobject_cast<QMessageBox*>(dialog);
                if (!confirmation) return;
                for (QAbstractButton* button : confirmation->buttons()) {
                    if (button->text() == QStringLiteral("Remove scene")) {
                        button->click();
                        return;
                    }
                }
            }) || scene_list->count() != 2 ||
            scene_probe.selectedSceneId() != QStringLiteral("scene-projector")) {
            std::fputs("Removing a scene did not retain a valid selected profile.\n", stderr);
            return 2;
        }
        scene_probe.setSessionContext(QStringLiteral("Recording active"),
                                      QStringLiteral("DP-1"), true);
        if (scene_list->isEnabled() || add_scene->isEnabled() || rename_scene->isEnabled() ||
            remove_scene->isEnabled() || edit_output->isEnabled() ||
            !source_status->text().contains(QStringLiteral("DP-1")) ||
            !source_status->text().contains(QStringLiteral("after this session stops"))) {
            std::fputs("Scene mutations were not locked or active source status was unclear.\n",
                       stderr);
            return 2;
        }
        scene_probe.setProfiles({sample_scenes.first()}, QStringLiteral("scene-main"));
        if (scene_list->count() != 1 || remove_scene->isEnabled()) {
            std::fputs("The panel must keep one scene and disable removing the final scene.\n",
                       stderr);
            return 2;
        }

        fluxrec::ui::RecorderSettings backend_settings;
        backend_settings.executable = QStandardPaths::findExecutable(QStringLiteral("true"));
        backend_settings.replay_history_seconds = 5;
        fluxrec::ui::ProcessRecorderBackend backend{backend_settings};
        if (!backend.can_start_recording() || !backend.can_start_replay_only_session()) {
            std::fputs("Default bitrate settings should allow recorder and replay-only starts.\n",
                       stderr);
            return 2;
        }
        backend_settings.rate_control = QStringLiteral("constant_qp");
        backend_settings.qp = 28;
        backend.set_settings(backend_settings);
        if (!backend.can_start_recording() || !backend.can_start_replay_only_session()) {
            std::fputs("Supported CQP settings should allow both capture launch paths.\n", stderr);
            return 2;
        }

        const auto read_launch_arguments = [](fluxrec::ui::RecorderSettings launch_settings,
                                              bool replay_only) {
            QTemporaryDir argument_directory;
            if (!argument_directory.isValid()) return QStringList{};
            const QString argument_path = argument_directory.filePath(QStringLiteral("args.txt"));
            const QString script_path = argument_directory.filePath(QStringLiteral("fake-recorder"));
            QFile fake_recorder(script_path);
            if (!fake_recorder.open(QIODevice::WriteOnly | QIODevice::Text)) return QStringList{};
            const QByteArray script = QStringLiteral("#!/bin/sh\nprintf '%s\\n' \"$@\" > '%1'\n")
                .arg(argument_path).toUtf8();
            if (fake_recorder.write(script) != script.size()) return QStringList{};
            fake_recorder.close();
            fake_recorder.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                         QFileDevice::ExeOwner | QFileDevice::ReadGroup |
                                         QFileDevice::ExeGroup | QFileDevice::ReadOther |
                                         QFileDevice::ExeOther);

            launch_settings.executable = script_path;
            launch_settings.replay_history_seconds = 5;
            fluxrec::ui::ProcessRecorderBackend fake_backend{launch_settings};
            QEventLoop loop;
            QTimer timeout;
            timeout.setSingleShot(true);
            bool completed = false;
            if (replay_only) {
                QObject::connect(&fake_backend,
                    &fluxrec::ui::RecorderBackend::replay_session_finished, &loop,
                    [&loop, &completed](bool) { completed = true; loop.quit(); });
            } else {
                QObject::connect(&fake_backend,
                    &fluxrec::ui::RecorderBackend::recording_finished, &loop,
                    [&loop, &completed](const QString&, bool) { completed = true; loop.quit(); });
            }
            QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
            timeout.start(3000);
            if (replay_only) fake_backend.start_replay_buffer();
            else fake_backend.start_recording(argument_directory.filePath(QStringLiteral("out.mkv")));
            loop.exec();
            timeout.stop();
            if (!completed) return QStringList{};

            QFile captured_arguments(argument_path);
            if (!captured_arguments.open(QIODevice::ReadOnly | QIODevice::Text)) {
                return QStringList{};
            }
            return QString::fromUtf8(captured_arguments.readAll())
                .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        };
        const auto encoder_arguments_match = [](const QStringList& arguments,
                                                 const QString& backend_name,
                                                 const QString& depth,
                                                 const QString& preset,
                                                 const QString& b_frames) {
            const int backend_flag = arguments.indexOf(QStringLiteral("--backend"));
            const int depth_flag = arguments.indexOf(QStringLiteral("--async-depth"));
            if (backend_flag < 0 || arguments.value(backend_flag + 1) != backend_name ||
                depth_flag < 0 || arguments.value(depth_flag + 1) != depth) {
                return false;
            }
            const int preset_flag = arguments.indexOf(QStringLiteral("--nvenc-preset"));
            const int b_frames_flag = arguments.indexOf(QStringLiteral("--b-frames"));
            if (backend_name == QStringLiteral("nvenc")) {
                return preset_flag >= 0 && arguments.value(preset_flag + 1) == preset &&
                       b_frames_flag >= 0 && arguments.value(b_frames_flag + 1) == b_frames;
            }
            return preset_flag < 0 && b_frames_flag < 0;
        };
        for (const bool replay_only : {false, true}) {
            fluxrec::ui::RecorderSettings launch_settings;
            launch_settings.async_depth = 9;
            launch_settings.nvenc_async_depth = 5;
            launch_settings.nvenc_preset = QStringLiteral("p4");
            launch_settings.nvenc_b_frames = 3;
            launch_settings.replay_history_seconds = 5;
            for (const QString& backend_name : {QStringLiteral("vulkan"), QStringLiteral("nvenc")}) {
                launch_settings.encoder_backend = backend_name;
                const QString depth = backend_name == QStringLiteral("nvenc")
                    ? QStringLiteral("5") : QStringLiteral("9");
                for (const bool identity_enabled : {false, true}) {
                    launch_settings.verify_frame_ids = identity_enabled;
                    const QStringList arguments = read_launch_arguments(launch_settings, replay_only);
                    const bool identity_argument_present =
                        arguments.contains(QStringLiteral("--verify-frame-ids"));
                    if (!encoder_arguments_match(arguments, backend_name, depth,
                            launch_settings.nvenc_preset, QStringLiteral("3")) ||
                        identity_argument_present != identity_enabled) {
                        std::fputs("Recorder launch arguments did not match backend-specific or frame identity settings.\n",
                                   stderr);
                        return 2;
                    }
                }
            }
        }
        {
            fluxrec::ui::RecorderSettings direct_launch;
            direct_launch.encoder_backend = QStringLiteral("experimental-cuda-egl");
            direct_launch.direct_async_depth = 12;
            direct_launch.direct_pool_size = 16;
            direct_launch.pool_size = 48;
            direct_launch.verify_frame_ids = true;
            direct_launch.rate_control = QStringLiteral("bitrate");
            direct_launch.replay_history_seconds = 5;
            const QStringList arguments = read_launch_arguments(direct_launch, false);
            if (arguments.indexOf(QStringLiteral("--backend")) < 0 ||
                arguments.value(arguments.indexOf(QStringLiteral("--backend")) + 1) !=
                    QStringLiteral("experimental-cuda-egl") ||
                arguments.value(arguments.indexOf(QStringLiteral("--async-depth")) + 1) !=
                    QStringLiteral("12") ||
                arguments.value(arguments.indexOf(QStringLiteral("--pool")) + 1) !=
                    QStringLiteral("16") ||
                arguments.value(arguments.indexOf(QStringLiteral("--b-frames")) + 1) !=
                    QStringLiteral("0") ||
                arguments.contains(QStringLiteral("--conversion")) ||
                arguments.contains(QStringLiteral("--preview-socket")) ||
                arguments.contains(QStringLiteral("--verify-frame-ids")) ||
                arguments.contains(QStringLiteral("--nvenc-preset")) ||
                arguments.contains(QStringLiteral("--qp"))) {
                std::fputs("Direct CUDA EGL launch arguments included an unsupported option.\n",
                           stderr);
                return 2;
            }
            fluxrec::ui::RecorderSettings direct_cqp_launch = direct_launch;
            direct_cqp_launch.rate_control = QStringLiteral("constant_qp");
            direct_cqp_launch.qp = 28;
            for (const bool replay_only : {false, true}) {
                const QStringList cqp_arguments =
                    read_launch_arguments(direct_cqp_launch, replay_only);
                const int qp_flag = cqp_arguments.indexOf(QStringLiteral("--qp"));
                if (qp_flag < 0 || cqp_arguments.value(qp_flag + 1) != QStringLiteral("28") ||
                    cqp_arguments.contains(QStringLiteral("--bitrate-mbps")) ||
                    cqp_arguments.contains(QStringLiteral("--bitrate-bps")) ||
                    cqp_arguments.contains(QStringLiteral("--conversion")) ||
                    cqp_arguments.contains(QStringLiteral("--nvenc-preset"))) {
                    std::fputs("Direct CUDA EGL CQP launch did not pass --qp without a bitrate.\n",
                               stderr);
                    return 2;
                }
            }
        }

        auto* frame_counts = window.findChild<QTableWidget*>(QStringLiteral("frameCountsTable"));
        auto* telemetry_backend = window.findChild<fluxrec::ui::ProcessRecorderBackend*>();
        const auto find_counter_row = [](QTableWidget* table, const QString& label) {
            if (!table) return -1;
            for (int row = 0; row < table->rowCount(); ++row) {
                const QTableWidgetItem* item = table->item(row, 0);
                if (item && item->text() == label) return row;
            }
            return -1;
        };
        if (!frame_counts || !telemetry_backend) {
            std::fputs("Statistics identity coverage rows are unavailable.\n", stderr);
            return 2;
        }
        const int unique_row = find_counter_row(frame_counts, QStringLiteral("Unique frames"));
        const int duplicate_row = find_counter_row(frame_counts, QStringLiteral("Duplicated frames"));
        const int checked_row = find_counter_row(frame_counts,
            QStringLiteral("Identity digests checked"));
        const int skipped_row = find_counter_row(frame_counts,
            QStringLiteral("Identity checks skipped"));
        const int unclassified_row = find_counter_row(frame_counts,
            QStringLiteral("Identity results unclassified"));
        fluxrec::ui::TelemetrySnapshot missing_identity_coverage;
        missing_identity_coverage.values.insert(QStringLiteral("frames"),
            QJsonObject{{QStringLiteral("unique"), 7},
                        {QStringLiteral("duplicated"), 0}});
        missing_identity_coverage.received_at = QDateTime::currentDateTime();
        Q_EMIT telemetry_backend->telemetry_received(missing_identity_coverage);
        if (unique_row < 0 || duplicate_row < 0 || checked_row < 0 || skipped_row < 0 ||
            unclassified_row < 0 ||
            frame_counts->item(unique_row, 1)->text() != QStringLiteral("7") ||
            frame_counts->item(duplicate_row, 1)->text() != QStringLiteral("0") ||
            frame_counts->item(checked_row, 1)->text() != QStringLiteral("—") ||
            !frame_counts->item(checked_row, 1)->data(Qt::AccessibleTextRole).toString()
                 .contains(QStringLiteral("Not reported")) ||
            frame_counts->item(skipped_row, 1)->text() != QStringLiteral("—") ||
            frame_counts->item(unclassified_row, 1)->text() != QStringLiteral("—")) {
            std::fputs("Statistics must preserve real zeros and show absent identity coverage as not reported.\n",
                       stderr);
            return 2;
        }
        fluxrec::ui::TelemetrySnapshot reported_identity_coverage;
        reported_identity_coverage.values.insert(QStringLiteral("frames"),
            QJsonObject{{QStringLiteral("unique"), 10},
                        {QStringLiteral("duplicated"), 2},
                        {QStringLiteral("identity_checked"), 13},
                        {QStringLiteral("identity_skipped"), 1},
                        {QStringLiteral("identity_unclassified"), 2}});
        reported_identity_coverage.received_at = QDateTime::currentDateTime();
        Q_EMIT telemetry_backend->telemetry_received(reported_identity_coverage);
        if (frame_counts->item(checked_row, 1)->text() != QStringLiteral("13") ||
            frame_counts->item(skipped_row, 1)->text() != QStringLiteral("1") ||
            frame_counts->item(unclassified_row, 1)->text() != QStringLiteral("2")) {
            std::fputs("Statistics did not display reported frame identity coverage counts.\n",
                       stderr);
            return 2;
        }

        auto* average_fps = window.findChild<QTableWidget*>(QStringLiteral("averageFpsTable"));
        auto* average_note = window.findChild<QLabel*>(QStringLiteral("averageFpsNote"));
        const int average_captured_row = find_counter_row(average_fps, QStringLiteral("Captured"));
        if (!average_fps || !average_note || average_captured_row < 0 ||
            !average_note->text().contains(QStringLiteral("stop is left out")) ||
            average_fps->item(average_captured_row, 1)->text() != QStringLiteral("—")) {
            std::fputs("Average FPS tab is missing its empty-state reading.\n", stderr);
            return 2;
        }
        QTemporaryDir average_directory;
        if (!average_directory.isValid()) {
            std::fputs("Could not create a directory for the average FPS smoke recorder.\n", stderr);
            return 2;
        }
        const QString average_script_path = average_directory.filePath(QStringLiteral("average-recorder"));
        QFile average_script(average_script_path);
        if (!average_script.open(QIODevice::WriteOnly | QIODevice::Text)) {
            std::fputs("Could not write the average FPS smoke recorder.\n", stderr);
            return 2;
        }
        const QByteArray average_script_text =
            "#!/usr/bin/env python3\n"
            "import signal, sys, time\n"
            "def emit(ts, frames, rate):\n"
            "    sys.stdout.write("
            "'{\"timestamp_unix_ns\":%d,\"recording_mode\":\"recording\","
            "\"frames\":{\"captured\":%d,\"encoded\":%d,\"muxed\":%d},"
            "\"fps\":{\"captured\":%s,\"encoded\":%s,\"muxed\":%s}}\\n' "
            "% (ts, frames, frames, frames, rate, rate, rate))\n"
            "    sys.stdout.flush()\n"
            "for ts, frames in ((1000000000, 360), (2000000000, 720), (3000000000, 1080)):\n"
            "    emit(ts, frames, '360')\n"
            "def stop(signum, frame):\n"
            "    emit(4000000000, 1360, '280')\n"
            "    sys.exit(0)\n"
            "signal.signal(signal.SIGINT, stop)\n"
            "signal.signal(signal.SIGTERM, stop)\n"
            "time.sleep(30)\n";
        if (average_script.write(average_script_text) != average_script_text.size()) {
            std::fputs("Could not write the average FPS smoke recorder.\n", stderr);
            return 2;
        }
        average_script.close();
        average_script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                      QFileDevice::ExeOwner | QFileDevice::ReadGroup |
                                      QFileDevice::ExeGroup | QFileDevice::ReadOther |
                                      QFileDevice::ExeOther);
        fluxrec::ui::RecorderSettings average_settings = settings;
        average_settings.executable = average_script_path;
        average_settings.output_directory = average_directory.path();
        telemetry_backend->set_settings(average_settings);
        if (!telemetry_backend->can_start_recording()) {
            std::fputs("Average FPS smoke recorder could not be started.\n", stderr);
            return 2;
        }
        telemetry_backend->start_recording(average_directory.filePath(QStringLiteral("out.mkv")));
        const QString average_rate = QLocale().toString(360.0, 'f', 1) + QStringLiteral(" FPS");
        const QString included_frames = QLocale().toString(1080ull);
        const QString stopped_frames = QLocale().toString(1360ull);
        const QString included_seconds = QLocale().toString(3.0, 'f', 1);
        const auto wait_for_text = [](QTableWidget* table, int row, int column, const QString& expected) {
            QEventLoop loop;
            QTimer poll;
            QTimer timeout;
            timeout.setSingleShot(true);
            bool matched = false;
            QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
                const QTableWidgetItem* item = table->item(row, column);
                if (item && item->text() == expected) {
                    matched = true;
                    loop.quit();
                }
            });
            QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
            poll.start(20);
            timeout.start(3000);
            loop.exec();
            poll.stop();
            return matched;
        };
        const int delivered_row = find_counter_row(frame_counts,
                                                    QStringLiteral("Frames delivered for processing"));
        if (delivered_row < 0 ||
            !wait_for_text(average_fps, average_captured_row, 1, average_rate) ||
            average_fps->item(average_captured_row, 2)->text() != included_frames ||
            average_fps->item(average_captured_row, 3)->text() != included_seconds) {
            std::fputs("Average FPS did not settle on the complete one-second samples.\n", stderr);
            return 2;
        }
        telemetry_backend->stop_recording();
        if (!wait_for_text(frame_counts, delivered_row, 1, stopped_frames) ||
            average_fps->item(average_captured_row, 1)->text() != average_rate ||
            average_fps->item(average_captured_row, 2)->text() != included_frames) {
            std::fputs("Average FPS included the second in which recording stopped.\n", stderr);
            return 2;
        }

        std::fputs("Fluxrec UI smoke passed; only CPU-only fake recorder processes were launched.\n",
                   stdout);
        QTimer::singleShot(250, &application, &QCoreApplication::quit);
    }
    return application.exec();
}
