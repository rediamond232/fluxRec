#include "settings_dialog.hpp"

#include "theme_tokens.hpp"
#include "wayland_output_picker.hpp"

#include <QCloseEvent>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QVBoxLayout>

namespace fluxrec::ui {
namespace {

QWidget* browseField(QLineEdit* input, QPushButton* button, QWidget* parent) {
    auto* row = new QWidget(parent);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(theme::cluster_gap);
    layout->addWidget(input, 1);
    layout->addWidget(button);
    return row;
}

void setSpinRange(QSpinBox* spin, int minimum, int maximum, const QString& suffix) {
    spin->setRange(minimum, maximum);
    spin->setSuffix(suffix);
    spin->setKeyboardTracking(false);
}

} // namespace

SettingsDialog::SettingsDialog(const RecorderSettings& settings, QWidget* parent,
                               bool scene_source_editable)
    : QDialog(parent), initial_(settings) {
    normalizeSceneProfiles(initial_);
    normalizeEncoderSettings(initial_);
    setWindowTitle(tr("Fluxrec settings"));
    setModal(true);
    setMinimumWidth(520);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(theme::panel_inset + 10, theme::panel_inset + 8,
                             theme::panel_inset + 10, theme::panel_inset + 6);
    root->setSpacing(theme::panel_inset);

    auto* intro = new QLabel(
        tr("Choose how Fluxrec starts the recorder. Changes apply to the next recorder session."), this);
    intro->setWordWrap(true);
    intro->setObjectName(QStringLiteral("settingsIntro"));
    root->addWidget(intro);

    auto* recorder_group = new QGroupBox(tr("Recorder"), this);
    auto* recorder_form = new QFormLayout(recorder_group);
    recorder_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    recorder_form->setHorizontalSpacing(theme::panel_inset);
    recorder_form->setVerticalSpacing(theme::cluster_gap + 3);

    executable_ = new QLineEdit(settings.executable, recorder_group);
    executable_->setPlaceholderText(tr("This Fluxrec program"));
    executable_->setClearButtonEnabled(true);
    executable_->setAccessibleName(tr("Recorder executable path"));
    executable_->setToolTip(tr(
        "Leave blank to record with this Fluxrec program. Capture still runs as a separate process."));
    auto* executable_browse = new QPushButton(tr("Browse…"), recorder_group);
    executable_browse->setObjectName(QStringLiteral("browseExecutableButton"));
    executable_browse->setMinimumHeight(theme::dense_control_height);
    recorder_form->addRow(tr("Recorder executable"),
                          browseField(executable_, executable_browse, recorder_group));
    connect(executable_browse, &QPushButton::clicked,
            this, &SettingsDialog::choose_executable);

    output_directory_ = new QLineEdit(settings.output_directory, recorder_group);
    output_directory_->setClearButtonEnabled(true);
    output_directory_->setAccessibleName(tr("Recording output folder"));
    auto* directory_browse = new QPushButton(tr("Browse…"), recorder_group);
    directory_browse->setObjectName(QStringLiteral("browseOutputButton"));
    directory_browse->setMinimumHeight(theme::dense_control_height);
    recorder_form->addRow(tr("Output folder"),
                          browseField(output_directory_, directory_browse, recorder_group));
    connect(directory_browse, &QPushButton::clicked,
            this, &SettingsDialog::choose_output_directory);

    output_picker_ = new WaylandOutputPicker(initial_.display_name, recorder_group);
    output_picker_->setObjectName(QStringLiteral("selectedSceneOutputPicker"));
    output_picker_->setAccessibleName(tr("Selected scene Wayland output"));
    output_picker_->setToolTip(tr(
        "The selected scene's single Wayland output, passed to the recorder when the next session starts."));
    output_picker_->setEnabled(scene_source_editable);
    if (!scene_source_editable) {
        const QString lock_description = tr(
            "Scene outputs cannot be changed while the recorder is starting, recording, capturing replay-only, or stopping.");
        output_picker_->setToolTip(lock_description);
        output_picker_->setControlAccessibleDescription(lock_description);
    }
    recorder_form->addRow(tr("Selected scene output"), output_picker_);

    codec_ = new QComboBox(recorder_group);
    codec_->setAccessibleName(tr("Video codec"));
    codec_->addItem(tr("H.264"), QStringLiteral("h264"));
    codec_->addItem(tr("HEVC / H.265 · experimental"), QStringLiteral("hevc"));
    codec_->setToolTip(tr(
        "H.264 is the recommended default. HEVC requires a separate experimental-use acknowledgement."));
    const int codec_index = codec_->findData(settings.codec);
    codec_->setCurrentIndex(codec_index >= 0 ? codec_index : 0);
    recorder_form->addRow(tr("Video codec"), codec_);

    auto* hevc_ack_row = new QWidget(recorder_group);
    auto* hevc_ack_layout = new QVBoxLayout(hevc_ack_row);
    hevc_ack_layout->setContentsMargins(0, 0, 0, 0);
    hevc_ack_layout->setSpacing(theme::cluster_gap);
    experimental_hevc_ack_ = new QCheckBox(
        tr("I understand and want to enable experimental HEVC capture"), hevc_ack_row);
    experimental_hevc_ack_->setChecked(settings.allow_experimental_hevc);
    experimental_hevc_ack_->setAccessibleName(tr("Acknowledge experimental HEVC capture limitations"));
    experimental_hevc_note_ = new QLabel(
        tr("HEVC remains experimental. A 10-second 1440p Vulkan Video test at a 120 FPS cap encoded 1,192 valid frames at about 119 FPS with no black frames; an NVENC run passed a 60-second source-ID check at async depth 4 and pool 48. A separate 20-second high-motion HEVC NVENC test passed with 7,201 consecutive IDs. The direct CUDA EGL path decoded exact high-motion source IDs through 60 seconds at about 359.3 FPS with Main-profile BT.709 limited metadata. High-motion Vulkan sessions, game HEVC, alternate NVENC presets, and other configurations remain unvalidated."),
        hevc_ack_row);
    experimental_hevc_note_->setWordWrap(true);
    experimental_hevc_note_->setObjectName(QStringLiteral("settingsNote"));
    experimental_hevc_note_->setAccessibleName(tr("Measured HEVC validation warning"));
    hevc_ack_layout->addWidget(experimental_hevc_ack_);
    hevc_ack_layout->addWidget(experimental_hevc_note_);
    auto* hevc_ack_label = new QLabel(tr("HEVC status"), recorder_group);
    recorder_form->addRow(hevc_ack_label, hevc_ack_row);
    auto sync_hevc_visibility = [this, hevc_ack_label, hevc_ack_row] {
        const bool hevc = codec_->currentData().toString() == QStringLiteral("hevc");
        hevc_ack_label->setVisible(hevc);
        hevc_ack_row->setVisible(hevc);
    };
    connect(codec_, &QComboBox::currentIndexChanged, this,
            [sync_hevc_visibility](int) { sync_hevc_visibility(); });
    sync_hevc_visibility();

    auto* frame_identity_row = new QWidget(recorder_group);
    auto* frame_identity_layout = new QVBoxLayout(frame_identity_row);
    frame_identity_layout->setContentsMargins(0, 0, 0, 0);
    frame_identity_layout->setSpacing(theme::cluster_gap);
    verify_frame_ids_ = new QCheckBox(
        tr("GPU frame identity checks · diagnostic"), frame_identity_row);
    verify_frame_ids_->setObjectName(QStringLiteral("verifyFrameIdsCheck"));
    verify_frame_ids_->setChecked(settings.verify_frame_ids);
    verify_frame_ids_->setAccessibleName(tr("Enable GPU frame identity diagnostics"));
    verify_frame_ids_->setToolTip(tr(
        "Hashes captured RGB images on the GPU to classify adjacent frame content. "
        "It adds a full-image compute pass, synchronization, and per-frame diagnostic logging, "
        "and may reduce recording FPS. Statistics shows checked, skipped, and unclassified "
        "coverage; unreported values remain Not reported. Off by default."));
    auto* frame_identity_note = new QLabel(
        tr("Optional GPU diagnostic. Adds GPU work and may reduce recording FPS. "
           "Identity coverage appears in Statistics."),
        frame_identity_row);
    frame_identity_note->setWordWrap(true);
    frame_identity_note->setObjectName(QStringLiteral("frameIdentityDiagnosticNote"));
    frame_identity_note->setAccessibleName(tr("GPU frame identity diagnostic cost"));
    frame_identity_layout->addWidget(verify_frame_ids_);
    frame_identity_layout->addWidget(frame_identity_note);
    recorder_form->addRow(tr("Frame identity"), frame_identity_row);

    encoder_backend_ = new QComboBox(recorder_group);
    encoder_backend_->setObjectName(QStringLiteral("encoderBackendCombo"));
    encoder_backend_->setAccessibleName(tr("GPU encoder backend"));
    encoder_backend_->addItem(tr("NVIDIA NVENC · default"), QStringLiteral("nvenc"));
    encoder_backend_->addItem(tr("Vulkan Video · optional"), QStringLiteral("vulkan"));
    encoder_backend_->addItem(tr("Direct CUDA EGL · experimental"),
                              QStringLiteral("experimental-cuda-egl"));
    encoder_backend_->setToolTip(tr(
        "NVIDIA NVENC is the default and requires compatible NVIDIA hardware and an FFmpeg runtime. Vulkan Video remains selectable. Direct CUDA EGL is an experimental application-level GPU path for native 2560×1440 capture. It is not allocation-level zero-copy. Backend changes apply to the next recorder session."));
    const int backend_index = encoder_backend_->findData(initial_.encoder_backend);
    encoder_backend_->setCurrentIndex(backend_index >= 0 ? backend_index : 0);
    recorder_form->addRow(tr("GPU encoder backend"), encoder_backend_);

    nvenc_preset_ = new QComboBox(recorder_group);
    nvenc_preset_->setObjectName(QStringLiteral("nvencPresetCombo"));
    nvenc_preset_->setAccessibleName(tr("NVENC preset"));
    for (int preset = 1; preset <= 7; ++preset) {
        const QString value = QStringLiteral("p%1").arg(preset);
        nvenc_preset_->addItem(value.toUpper(), value);
    }
    nvenc_preset_->setToolTip(tr(
        "Selects the P1–P7 preset passed to FFmpeg when NVENC is selected. Vulkan Video does not use this setting."));
    const int nvenc_preset_index = nvenc_preset_->findData(initial_.nvenc_preset);
    nvenc_preset_->setCurrentIndex(nvenc_preset_index >= 0 ? nvenc_preset_index : 0);
    nvenc_preset_label_ = new QLabel(tr("NVENC preset"), recorder_group);
    recorder_form->addRow(nvenc_preset_label_, nvenc_preset_);
    nvenc_note_ = new QLabel(
        tr("Depth 4 passed 60-second H.264 and HEVC source-ID checks with a 48-buffer pool. Controlled H.264 and HEVC runs at depth 8 produced periodic black output; use depth 4 for normal recordings."),
        recorder_group);
    nvenc_note_->setWordWrap(true);
    nvenc_note_->setObjectName(QStringLiteral("nvencValidationNote"));
    nvenc_note_->setAccessibleName(tr("NVENC validation status"));
    recorder_form->addRow(nvenc_note_);

    nvenc_b_frames_ = new QSpinBox(recorder_group);
    nvenc_b_frames_->setObjectName(QStringLiteral("nvencBFramesSpin"));
    setSpinRange(nvenc_b_frames_, 0, 4, tr(" frames"));
    nvenc_b_frames_->setValue(initial_.nvenc_b_frames);
    nvenc_b_frames_->setAccessibleName(tr("NVENC B-frame count"));
    nvenc_b_frames_->setToolTip(tr(
        "Maximum B-frames between reference frames (0–4). Zero keeps low-latency I/P-only encoding. Nonzero values add frame reordering and require NVENC/device support; GOP must be at least the count plus two frames. HEVC with two B-frames missed source frames at 1440p360 on the tested RTX 3090 Ti."));
    nvenc_b_frames_->setAccessibleDescription(nvenc_b_frames_->toolTip());
    nvenc_b_frames_label_ = new QLabel(tr("NVENC B-frames"), recorder_group);
    recorder_form->addRow(nvenc_b_frames_label_, nvenc_b_frames_);

    resolution_ = new QComboBox(recorder_group);
    resolution_->setAccessibleName(tr("Video output resolution"));
    resolution_->addItem(tr("Native capture size"), QStringLiteral("native"));
    resolution_->addItem(QStringLiteral("1280 × 720"), QStringLiteral("1280x720"));
    resolution_->addItem(QStringLiteral("1920 × 1080"), QStringLiteral("1920x1080"));
    resolution_->addItem(QStringLiteral("2560 × 1440"), QStringLiteral("2560x1440"));
    resolution_->addItem(QStringLiteral("3840 × 2160"), QStringLiteral("3840x2160"));
    resolution_->addItem(tr("Custom…"), QStringLiteral("custom"));
    QString saved_resolution = settings.output_width == 0 && settings.output_height == 0
        ? QStringLiteral("native")
        : QStringLiteral("%1x%2").arg(settings.output_width).arg(settings.output_height);
    int resolution_index = resolution_->findData(saved_resolution);
    if (resolution_index < 0) resolution_index = resolution_->findData(QStringLiteral("custom"));
    resolution_->setCurrentIndex(resolution_index);
    resolution_->setToolTip(tr(
        "Select native capture dimensions or scale to an output size using the selected two-pass GPU conversion route."));
    recorder_form->addRow(tr("Output resolution"), resolution_);

    custom_resolution_fields_ = new QWidget(recorder_group);
    auto* custom_resolution_layout = new QHBoxLayout(custom_resolution_fields_);
    custom_resolution_layout->setContentsMargins(0, 0, 0, 0);
    custom_resolution_layout->setSpacing(theme::cluster_gap);
    resolution_width_ = new QSpinBox(custom_resolution_fields_);
    resolution_width_->setRange(4, 16384);
    resolution_width_->setSingleStep(2);
    resolution_width_->setSuffix(tr(" px"));
    resolution_width_->setValue(settings.output_width > 0 ? settings.output_width : 1920);
    resolution_width_->setAccessibleName(tr("Custom output width in pixels"));
    resolution_height_ = new QSpinBox(custom_resolution_fields_);
    resolution_height_->setRange(4, 16384);
    resolution_height_->setSingleStep(2);
    resolution_height_->setSuffix(tr(" px"));
    resolution_height_->setValue(settings.output_height > 0 ? settings.output_height : 1080);
    resolution_height_->setAccessibleName(tr("Custom output height in pixels"));
    custom_resolution_layout->addWidget(resolution_width_);
    custom_resolution_layout->addWidget(new QLabel(QStringLiteral("×"), custom_resolution_fields_));
    custom_resolution_layout->addWidget(resolution_height_);
    recorder_form->addRow(tr("Custom dimensions"), custom_resolution_fields_);
    auto* resolution_note = new QLabel(
        tr("Custom dimensions must be even values from 4 to 16,384. Device or codec limits may reject a size; native capture avoids scaling."),
        recorder_group);
    resolution_note->setWordWrap(true);
    resolution_note->setObjectName(QStringLiteral("settingsNote"));
    recorder_form->addRow(resolution_note);
    auto sync_resolution_visibility = [this] {
        custom_resolution_fields_->setVisible(
            resolution_->currentData().toString() == QStringLiteral("custom"));
    };
    connect(resolution_, &QComboBox::currentIndexChanged, this,
            [sync_resolution_visibility](int) { sync_resolution_visibility(); });
    sync_resolution_visibility();

    max_fps_ = new QComboBox(recorder_group);
    max_fps_->setAccessibleName(tr("Maximum admitted capture rate"));
    max_fps_->addItem(tr("Uncapped · compositor/display cadence (default)"), 0);
    max_fps_->addItem(tr("60 FPS capture cap"), 60);
    max_fps_->addItem(tr("120 FPS capture cap"), 120);
    max_fps_->addItem(tr("240 FPS capture cap"), 240);
    max_fps_->addItem(tr("360 FPS capture cap · 8 s checked"), 360);
    const int fps_index = max_fps_->findData(settings.max_fps);
    max_fps_->setCurrentIndex(fps_index >= 0 ? fps_index : 0);
    max_fps_->setToolTip(tr(
        "Limits frames admitted to the recording pipeline using compositor presentation timestamps. "
        "It does not synthesize frames or guarantee a fixed output rate. Guarded 10-second H.264 "
        "NVENC checks observed mean steady encode rates of 59.7, 119.4, and 236.6 FPS at the "
        "60, 120, and 240 FPS caps, respectively. The runs sampled valid, nonblack source IDs and "
        "had no internal drops or pool starvation. A paired "
        "8-second H.264 test at the 360 FPS cap decoded 2,881 consecutive source IDs at about "
        "360 FPS with no black, invalid, missing, or dropped frames."));
    recorder_form->addRow(tr("Capture FPS cap"), max_fps_);
    auto* fps_note = new QLabel(
        tr("A cap limits frames admitted using compositor timestamps; it is not a guaranteed output rate. "
           "Guarded 10-second H.264 NVENC checks observed mean steady encode rates of 59.7, 119.4, "
           "and 236.6 FPS at the 60, 120, and 240 FPS caps, respectively. "
           "A paired 8-second H.264 test at the 360 FPS cap decoded 2,881 consecutive source IDs at "
           "about 360 FPS."),
        recorder_group);
    fps_note->setWordWrap(true);
    fps_note->setObjectName(QStringLiteral("settingsNote"));
    fps_note->setAccessibleName(tr("Capture FPS cap explanation"));
    recorder_form->addRow(fps_note);

    rate_control_ = new QComboBox(recorder_group);
    rate_control_->setObjectName(QStringLiteral("rateControlCombo"));
    rate_control_->setAccessibleName(tr("Video rate control"));
    rate_control_->addItem(tr("Bitrate · default"), QStringLiteral("bitrate"));
    rate_control_->addItem(tr("CQP · experimental"),
                           QStringLiteral("constant_qp"));
    rate_control_->setToolTip(tr(
        "CQP sets a fixed quantizer from 0 to 51. Lower values preserve more detail and usually produce larger files. H.264 QP 28 has been tested on one NVIDIA RTX 3090 Ti setup; other codec, driver, and QP combinations remain experimental."));
    const int rate_control_index = rate_control_->findData(settings.rate_control);
    rate_control_->setCurrentIndex(rate_control_index >= 0 ? rate_control_index : 0);
    recorder_form->addRow(tr("Rate control"), rate_control_);

    bitrate_mbps_ = new QSpinBox(recorder_group);
    bitrate_mbps_->setObjectName(QStringLiteral("bitrateMbpsSpin"));
    setSpinRange(bitrate_mbps_, 1, 500, tr(" Mbps"));
    bitrate_mbps_->setValue(settings.bitrate_mbps);
    bitrate_mbps_->setAccessibleName(tr("Average bitrate target"));
    bitrate_mbps_->setToolTip(tr(
        "Average bitrate target in megabits per second (1–500 Mbps). "
        "It is not a maximum bitrate or file-size limit. "
        "Statistics shows the observed output bitrate. "
        "One high-motion H.264 NVENC CAVLC VBR recording with a 50 Mbps target "
        "measured about 83 Mbps output bitrate. Its saved value is inactive in CQP mode."));
    recorder_form->addRow(tr("Average bitrate target"), bitrate_mbps_);
    auto* bitrate_note = new QLabel(
        tr("Not a maximum bitrate or file-size limit. Statistics shows the observed output bitrate."),
        recorder_group);
    bitrate_note->setObjectName(QStringLiteral("bitrateTargetNote"));
    bitrate_note->setWordWrap(true);
    bitrate_note->setAccessibleName(tr("Average bitrate target explanation"));
    recorder_form->addRow(bitrate_note);

    qp_ = new QSpinBox(recorder_group);
    qp_->setObjectName(QStringLiteral("qpSpin"));
    setSpinRange(qp_, 0, 51, {});
    qp_->setValue(settings.qp);
    qp_->setAccessibleName(tr("Constant quantizer QP"));
    qp_->setToolTip(tr(
        "QP ranges from 0 to 51 for the current 8-bit NV12 path. Lower values generally preserve more detail and use more bitrate. This saved value is inactive in bitrate mode."));
    recorder_form->addRow(tr("QP (0–51)"), qp_);
    qp_note_ = new QLabel(
        tr("QP is editable only in CQP mode. CQP is experimental: H.264 QP 28 was checked on one NVIDIA RTX 3090 Ti setup; other QPs, codecs, and driver/device combinations may be unsupported."),
        recorder_group);
    qp_note_->setObjectName(QStringLiteral("cqpExperimentalNote"));
    qp_note_->setWordWrap(true);
    qp_note_->setAccessibleName(tr("CQP validation status"));
    recorder_form->addRow(qp_note_);
    const auto update_rate_control_controls = [this] {
        const bool use_cqp = rate_control_->currentData().toString() ==
            QStringLiteral("constant_qp");
        bitrate_mbps_->setEnabled(!use_cqp);
        const QString bitrate_description = tr(
            "It is not a maximum bitrate or file-size limit. Statistics shows the observed output bitrate.");
        bitrate_mbps_->setAccessibleDescription(use_cqp
            ? tr("Saved average bitrate target from 1 to 500 Mbps; inactive while CQP is selected. %1")
                  .arg(bitrate_description)
            : tr("Average bitrate target from 1 to 500 Mbps. %1")
                  .arg(bitrate_description));
        qp_->setEnabled(use_cqp);
        qp_->setAccessibleDescription(use_cqp
            ? tr("Active constant quantizer from 0 to 51. Lower values generally preserve more detail.")
            : tr("Saved constant quantizer from 0 to 51; inactive until CQP is selected."));
    };
    connect(rate_control_, &QComboBox::currentIndexChanged, this,
            [update_rate_control_controls](int) { update_rate_control_controls(); });
    update_rate_control_controls();

    gop_size_ = new QSpinBox(recorder_group);
    setSpinRange(gop_size_, 1, 3600, tr(" frames"));
    gop_size_->setValue(settings.gop_size);
    gop_size_->setAccessibleName(tr("GOP keyframe interval"));
    gop_size_->setToolTip(tr(
        "Sets the encoder GOP interval in frames (1–3600) for recording and replay-only capture. Lower values request more frequent keyframes. Default: 120 frames."));
    recorder_form->addRow(tr("GOP / keyframe interval"), gop_size_);

    pool_size_ = new QSpinBox(recorder_group);
    setSpinRange(pool_size_, 2, 64, tr(" buffers"));
    pool_size_->setValue(settings.pool_size);
    pool_size_->setAccessibleName(tr("GPU frame pool size"));
    pool_size_->setToolTip(tr(
        "Default: 48 buffers (zero missed IDs in one motion test). Larger pools reserve more GPU memory and may add queueing latency; tune for your workload."));
    recorder_form->addRow(tr("GPU frame pool"), pool_size_);

    async_depth_ = new QSpinBox(recorder_group);
    async_depth_->setObjectName(QStringLiteral("encoderAsyncDepthSpin"));
    setSpinRange(async_depth_, 1, 64, tr(" frames"));
    vulkan_async_depth_value_ = initial_.async_depth;
    nvenc_async_depth_value_ = initial_.nvenc_async_depth;
    direct_async_depth_value_ = initial_.direct_async_depth;
    general_pool_value_ = initial_.pool_size;
    direct_pool_value_ = initial_.direct_pool_size;
    active_encoder_backend_ = encoder_backend_->currentData().toString();
    const bool opening_direct = active_encoder_backend_ == QStringLiteral("experimental-cuda-egl");
    if (opening_direct) {
        setSpinRange(pool_size_, 3, 16, tr(" buffers"));
        pool_size_->setValue(direct_pool_value_);
        setSpinRange(async_depth_, 1, 16, tr(" frames"));
    }
    async_depth_->setValue(opening_direct ? direct_async_depth_value_
        : active_encoder_backend_ == QStringLiteral("nvenc")
            ? nvenc_async_depth_value_ : vulkan_async_depth_value_);
    async_depth_label_ = new QLabel(recorder_group);
    recorder_form->addRow(async_depth_label_, async_depth_);

    direct_backend_note_ = new QLabel(recorder_group);
    direct_backend_note_->setWordWrap(true);
    direct_backend_note_->setObjectName(QStringLiteral("directCudaEglNote"));
    direct_backend_note_->setAccessibleName(tr("Direct CUDA EGL constraints"));
    direct_backend_note_->setText(tr(
        "Application-level direct GPU path: DMA-BUF to EGL to CUDA to NVENC, with no explicit raw-pixel copy in the application. This is not allocation-level zero-copy; desktop CUDA EGL registration is outside NVIDIA's documented Tegra support, and driver-internal copies are unknown. Native size or 2560×1440 only, H.264 CAVLC or HEVC Main, bitrate or experimental constant QP, zero B-frames, pool 3–16, async depth 1–16. Constant QP on this path has not been live-checked. GPU preview and the frame-identity digest are unavailable."));
    recorder_form->addRow(direct_backend_note_);

    const auto set_combo_item_enabled = [](QComboBox* combo, int index, bool enabled) {
        if (auto* model = qobject_cast<QStandardItemModel*>(combo->model())) {
            if (auto* item = model->item(index)) item->setEnabled(enabled);
        }
    };
    const auto update_encoder_controls = [this, set_combo_item_enabled] {
        const QString backend = encoder_backend_->currentData().toString();
        const bool use_nvenc = backend == QStringLiteral("nvenc");
        const bool use_direct = backend == QStringLiteral("experimental-cuda-egl");
        nvenc_preset_label_->setVisible(use_nvenc);
        nvenc_preset_->setVisible(use_nvenc);
        nvenc_preset_->setEnabled(use_nvenc);
        nvenc_note_->setVisible(use_nvenc);
        nvenc_b_frames_label_->setVisible(use_nvenc);
        nvenc_b_frames_->setVisible(use_nvenc);
        nvenc_b_frames_->setEnabled(use_nvenc);
        direct_backend_note_->setVisible(use_direct);
        verify_frame_ids_->setEnabled(!use_direct);
        for (int index = 0; index < resolution_->count(); ++index) {
            const QString choice = resolution_->itemData(index).toString();
            const bool allowed = !use_direct || choice == QStringLiteral("native") ||
                choice == QStringLiteral("2560x1440");
            set_combo_item_enabled(resolution_, index, allowed);
        }
        if (use_direct) {
            async_depth_label_->setText(tr("Direct CUDA EGL async depth"));
            async_depth_->setAccessibleName(tr("Direct CUDA EGL asynchronous depth"));
            const QString depth_guidance = tr(
                "Direct CUDA EGL default: 12 frames, range 1–16. Saved separately from the Vulkan Video and NVENC depths. A 60-second high-motion H.264 and HEVC check used depth 12.");
            async_depth_->setToolTip(depth_guidance);
            async_depth_->setAccessibleDescription(depth_guidance);
            pool_size_->setToolTip(tr(
                "Direct CUDA EGL accepts 3–16 capture buffers. Default: 16. This value is saved separately from the other backends' pool. Five-second motion-1 checks skipped 9 source IDs with both 4 and 8 buffers while capture_dropped stayed 0; the default 16-buffer pool stayed exact on matched runs."));
        } else if (use_nvenc) {
            async_depth_label_->setText(tr("NVENC async depth"));
            async_depth_->setAccessibleName(tr("NVENC asynchronous depth"));
            const QString depth_guidance = tr("NVENC default: 4 frames. H.264 and HEVC each passed a 60-second source-ID check at depth 4 with a 48-buffer pool. Controlled H.264 and HEVC runs at depth 8 produced periodic black output; use depth 4 for normal recordings.");
            async_depth_->setToolTip(depth_guidance);
            async_depth_->setAccessibleDescription(depth_guidance);
            pool_size_->setToolTip(tr(
                "Default: 48 buffers (zero missed IDs in one motion test). Larger pools reserve more GPU memory and may add queueing latency; tune for your workload."));
        } else {
            async_depth_label_->setText(tr("Vulkan Video async depth"));
            async_depth_->setAccessibleName(tr("Vulkan Video asynchronous depth"));
            const QString depth_guidance = tr("Vulkan Video default: 8 frames. This value is saved separately from the NVENC async depth.");
            async_depth_->setToolTip(depth_guidance);
            async_depth_->setAccessibleDescription(depth_guidance);
            pool_size_->setToolTip(tr(
                "Default: 48 buffers (zero missed IDs in one motion test). Larger pools reserve more GPU memory and may add queueing latency; tune for your workload."));
        }
        if (auto* conversion = findChild<QLabel*>(QStringLiteral("gpuConversionNote"))) {
            if (use_direct) {
                conversion->setText(tr(
                    "Not used. Direct CUDA EGL keeps the frame on the GPU from DMA-BUF through EGL and CUDA into NVENC. It does not run the Vulkan two-pass converter."));
                conversion->setAccessibleName(tr("GPU conversion is not used on Direct CUDA EGL"));
            } else {
                conversion->setText(tr(
                    "Two-pass GPU conversion is selected while the direct native-size Vulkan path's black-frame fault remains unresolved. This fallback adds GPU work."));
                conversion->setAccessibleName(tr("GPU conversion route status"));
            }
        }
    };
    connect(async_depth_, &QSpinBox::valueChanged, this, [this](int value) {
        if (active_encoder_backend_ == QStringLiteral("nvenc")) {
            nvenc_async_depth_value_ = value;
        } else if (active_encoder_backend_ == QStringLiteral("experimental-cuda-egl")) {
            direct_async_depth_value_ = value;
        } else {
            vulkan_async_depth_value_ = value;
        }
    });
    connect(pool_size_, &QSpinBox::valueChanged, this, [this](int value) {
        if (active_encoder_backend_ == QStringLiteral("experimental-cuda-egl")) {
            direct_pool_value_ = value;
        } else {
            general_pool_value_ = value;
        }
    });
    connect(encoder_backend_, &QComboBox::currentIndexChanged, this, [this, update_encoder_controls](int) {
        const int previous_depth = async_depth_->value();
        const int previous_pool = pool_size_->value();
        if (active_encoder_backend_ == QStringLiteral("nvenc")) {
            nvenc_async_depth_value_ = previous_depth;
            general_pool_value_ = previous_pool;
        } else if (active_encoder_backend_ == QStringLiteral("experimental-cuda-egl")) {
            direct_async_depth_value_ = previous_depth;
            direct_pool_value_ = previous_pool;
        } else {
            vulkan_async_depth_value_ = previous_depth;
            general_pool_value_ = previous_pool;
        }
        active_encoder_backend_ = encoder_backend_->currentData().toString();
        const bool now_direct = active_encoder_backend_ ==
            QStringLiteral("experimental-cuda-egl");
        setSpinRange(async_depth_, 1, now_direct ? 16 : 64, tr(" frames"));
        setSpinRange(pool_size_, now_direct ? 3 : 2, now_direct ? 16 : 64, tr(" buffers"));
        async_depth_->setValue(now_direct ? direct_async_depth_value_
            : active_encoder_backend_ == QStringLiteral("nvenc")
                ? nvenc_async_depth_value_ : vulkan_async_depth_value_);
        pool_size_->setValue(now_direct ? direct_pool_value_ : general_pool_value_);
        update_encoder_controls();
    });
    update_encoder_controls();

    auto* conversion_note = new QLabel(recorder_group);
    conversion_note->setWordWrap(true);
    conversion_note->setObjectName(QStringLiteral("gpuConversionNote"));
    conversion_note->setAccessibleName(tr("GPU conversion route status"));
    recorder_form->addRow(tr("GPU conversion"), conversion_note);
    update_encoder_controls();

    audio_enabled_ = new QCheckBox(tr("Capture audio tracks"), recorder_group);
    audio_enabled_->setChecked(settings.audio_enabled);
    audio_enabled_->setAccessibleName(tr("Capture audio tracks"));
    audio_enabled_->setToolTip(tr(
        "The audio mixer chooses each track’s input and monitor output. Applies to the next capture session."));
    recorder_form->addRow(tr("Audio capture"), audio_enabled_);
    root->addWidget(recorder_group);

    auto* replay_group = new QGroupBox(tr("Replay buffer"), this);
    auto* replay_form = new QFormLayout(replay_group);
    replay_form->setHorizontalSpacing(theme::panel_inset);
    replay_form->setVerticalSpacing(theme::cluster_gap);
    replay_history_seconds_ = new QSpinBox(replay_group);
    setSpinRange(replay_history_seconds_, 0, 3600, tr(" seconds"));
    replay_history_seconds_->setSpecialValueText(tr("Off"));
    replay_history_seconds_->setValue(settings.replay_history_seconds);
    replay_history_seconds_->setAccessibleName(tr("Replay history duration"));
    replay_form->addRow(tr("History while recording"), replay_history_seconds_);
    auto* replay_note = new QLabel(
        tr("With history enabled, the buffer starts with recording and can be stopped or restarted. Stopping preserves retained history for saving."),
        replay_group);
    replay_note->setWordWrap(true);
    replay_note->setObjectName(QStringLiteral("settingsNote"));
    replay_form->addRow(replay_note);
    root->addWidget(replay_group);

    validation_message_ = new QLabel(this);
    validation_message_->setObjectName(QStringLiteral("validationMessage"));
    validation_message_->setWordWrap(true);
    validation_message_->setVisible(false);
    validation_message_->setAccessibleName(tr("Settings validation message"));
    root->addWidget(validation_message_);

    auto* buttons = new QDialogButtonBox(this);
    auto* discard = buttons->addButton(tr("Discard changes"), QDialogButtonBox::RejectRole);
    auto* save = buttons->addButton(tr("Save changes"), QDialogButtonBox::AcceptRole);
    save->setDefault(true);
    save->setObjectName(QStringLiteral("saveSettingsButton"));
    discard->setObjectName(QStringLiteral("discardSettingsButton"));
    connect(discard, &QPushButton::clicked, this, &SettingsDialog::discard_changes);
    connect(save, &QPushButton::clicked, this, &SettingsDialog::accept_if_valid);
    root->addWidget(buttons);
}

RecorderSettings SettingsDialog::settings() const {
    RecorderSettings result = initial_;
    result.executable = executable_->text().trimmed();
    result.output_directory = output_directory_->text().trimmed();
    result.display_name = output_picker_->outputName();
    for (SceneProfile& scene : result.scenes) {
        if (scene.id == result.selected_scene_id) {
            scene.output_name = result.display_name;
            break;
        }
    }
    result.codec = codec_->currentData().toString();
    result.encoder_backend = encoder_backend_->currentData().toString();
    result.nvenc_preset = nvenc_preset_->currentData().toString();
    result.nvenc_b_frames = nvenc_b_frames_->value();
    result.verify_frame_ids = verify_frame_ids_->isChecked();
    result.allow_experimental_hevc = result.codec == QStringLiteral("hevc") &&
        experimental_hevc_ack_->isChecked();
    const QString selected_resolution = resolution_->currentData().toString();
    if (selected_resolution == QStringLiteral("native")) {
        result.output_width = 0;
        result.output_height = 0;
    } else if (selected_resolution == QStringLiteral("custom")) {
        result.output_width = resolution_width_->value();
        result.output_height = resolution_height_->value();
    } else {
        const QStringList dimensions = selected_resolution.split(QLatin1Char('x'));
        result.output_width = dimensions.value(0).toInt();
        result.output_height = dimensions.value(1).toInt();
    }
    result.rate_control = rate_control_->currentData().toString();
    result.bitrate_mbps = bitrate_mbps_->value();
    result.qp = qp_->value();
    result.max_fps = max_fps_->currentData().toInt();
    result.gop_size = gop_size_->value();
    if (result.encoder_backend == QStringLiteral("experimental-cuda-egl")) {
        result.direct_pool_size = pool_size_->value();
        result.pool_size = general_pool_value_;
    } else {
        result.pool_size = pool_size_->value();
        result.direct_pool_size = direct_pool_value_;
    }
    result.async_depth = vulkan_async_depth_value_;
    result.nvenc_async_depth = nvenc_async_depth_value_;
    result.direct_async_depth = direct_async_depth_value_;
    result.replay_history_seconds = replay_history_seconds_->value();
    result.audio_enabled = audio_enabled_->isChecked();
    normalizeSceneProfiles(result);
    normalizeEncoderSettings(result);
    return result;
}

void SettingsDialog::choose_output_directory() {
    const QString selected = QFileDialog::getExistingDirectory(
        this, tr("Choose recording folder"), output_directory_->text(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (!selected.isEmpty()) output_directory_->setText(selected);
}

void SettingsDialog::choose_executable() {
    const QString selected = QFileDialog::getOpenFileName(
        this, tr("Choose fluxrec-record"), executable_->text(), tr("Programs (*)"));
    if (!selected.isEmpty()) executable_->setText(selected);
}

void SettingsDialog::accept_if_valid() {
    QString message;
    if (!validate(&message)) {
        validation_message_->setText(message);
        validation_message_->setVisible(true);
        validation_message_->setProperty("validation", true);
        validation_message_->style()->unpolish(validation_message_);
        validation_message_->style()->polish(validation_message_);
        const RecorderSettings value = settings();
        if (value.output_directory.isEmpty()) {
            output_directory_->setFocus();
        } else if (value.display_name.isEmpty() ||
                   value.display_name.contains(QLatin1Char('\n')) ||
                   value.display_name.contains(QLatin1Char('\r'))) {
            output_picker_->focusInput();
        } else if (value.codec == QStringLiteral("hevc") &&
                   !value.allow_experimental_hevc) {
            codec_->setFocus();
        } else if ((value.output_width == 0) != (value.output_height == 0) ||
                   value.output_width < 0 || value.output_height < 0 ||
                   value.output_width > 16384 || value.output_height > 16384 ||
                   (value.output_width != 0 &&
                    (value.output_width < 4 || value.output_height < 4)) ||
                   (value.output_width > 0 &&
                    ((value.output_width % 2) != 0 || (value.output_height % 2) != 0))) {
            resolution_->setFocus();
        } else {
            executable_->setFocus();
        }
        return;
    }
    QDialog::accept();
}

void SettingsDialog::discard_changes() {
    QDialog::reject();
}

bool SettingsDialog::validate(QString* message) const {
    const RecorderSettings value = settings();
    if (value.output_directory.isEmpty()) {
        *message = tr("Choose a folder for new MKV recordings.");
        return false;
    }
    if (value.display_name.isEmpty() || value.display_name.contains(QLatin1Char('\n')) ||
        value.display_name.contains(QLatin1Char('\r'))) {
        *message = tr("Enter one Wayland output name.");
        output_picker_->setControlAccessibleDescription(*message);
        return false;
    }
    if (value.encoder_backend == QStringLiteral("experimental-cuda-egl")) {
        const bool native_size = value.output_width == 0 && value.output_height == 0;
        const bool native_1440 = value.output_width == 2560 && value.output_height == 1440;
        if (!native_size && !native_1440) {
            *message = tr("Direct CUDA EGL records native capture size or 2560×1440 only. Other output sizes stay saved for the other backends, but this backend will not start until you select one of those two.");
            return false;
        }
        if (value.direct_pool_size < 3 || value.direct_pool_size > 16) {
            *message = tr("Direct CUDA EGL requires a capture pool of 3–16 buffers.");
            return false;
        }
        if (value.direct_async_depth < 1 || value.direct_async_depth > 16) {
            *message = tr("Direct CUDA EGL async depth must be from 1 to 16 frames.");
            return false;
        }
    }
    if (value.codec == QStringLiteral("hevc") && !value.allow_experimental_hevc) {
        *message = tr("HEVC remains experimental: a 10-second 1440p Vulkan Video test at a 120 FPS cap produced 1,192 valid frames with no black frames, and an NVENC run passed a 60-second source-ID check at async depth 4 and pool 48. A separate 20-second high-motion HEVC NVENC test passed with 7,201 consecutive IDs. The direct CUDA EGL path decoded exact high-motion source IDs through 60 seconds at about 359.3 FPS. High-motion Vulkan sessions, game HEVC, alternate NVENC presets, and other configurations remain unvalidated. Select H.264 or check the experimental acknowledgement to continue.");
        return false;
    }
    if ((value.output_width == 0) != (value.output_height == 0) ||
        value.output_width < 0 || value.output_height < 0 ||
        value.output_width > 16384 || value.output_height > 16384 ||
        (value.output_width != 0 &&
         (value.output_width < 4 || value.output_height < 4)) ||
        (value.output_width > 0 &&
         ((value.output_width % 2) != 0 || (value.output_height % 2) != 0))) {
        *message = tr("Output dimensions must both use native capture size, or both be even values from 4 to 16,384.");
        return false;
    }
    if (!value.executable.isEmpty()) {
        const QFileInfo executable(value.executable);
        if (value.executable.contains(QLatin1Char('/')) &&
            (!executable.exists() || !executable.isFile() || !executable.isExecutable())) {
            *message = tr("The recorder path must point to an executable file.");
            return false;
        }
    }
    return true;
}

bool SettingsDialog::has_changes() const {
    const RecorderSettings value = settings();
    return value.executable != initial_.executable ||
        value.output_directory != initial_.output_directory ||
        value.display_name != initial_.display_name || value.codec != initial_.codec ||
        value.scenes != initial_.scenes ||
        value.selected_scene_id != initial_.selected_scene_id ||
        value.output_width != initial_.output_width || value.output_height != initial_.output_height ||
        value.allow_experimental_hevc != initial_.allow_experimental_hevc ||
        value.encoder_backend != initial_.encoder_backend ||
        value.nvenc_preset != initial_.nvenc_preset ||
        value.nvenc_b_frames != initial_.nvenc_b_frames ||
        value.verify_frame_ids != initial_.verify_frame_ids ||
        value.max_fps != initial_.max_fps || value.rate_control != initial_.rate_control ||
        value.bitrate_mbps != initial_.bitrate_mbps || value.qp != initial_.qp ||
        value.gop_size != initial_.gop_size ||
        value.pool_size != initial_.pool_size ||
        value.direct_pool_size != initial_.direct_pool_size ||
        value.async_depth != initial_.async_depth ||
        value.nvenc_async_depth != initial_.nvenc_async_depth ||
        value.direct_async_depth != initial_.direct_async_depth ||
        value.replay_history_seconds != initial_.replay_history_seconds ||
        value.audio_enabled != initial_.audio_enabled;
}

void SettingsDialog::reject() {
    if (!has_changes()) {
        QDialog::reject();
        return;
    }
    confirm_discard();
}

void SettingsDialog::closeEvent(QCloseEvent* event) {
    if (!has_changes()) {
        event->accept();
        return;
    }
    event->ignore();
    confirm_discard();
}

void SettingsDialog::confirm_discard() {
    QMessageBox dialog(QMessageBox::Warning, tr("Discard settings changes?"),
                       tr("Your settings have not been saved."), QMessageBox::NoButton, this);
    auto* keep = dialog.addButton(tr("Keep editing"), QMessageBox::RejectRole);
    auto* discard = dialog.addButton(tr("Discard changes"), QMessageBox::DestructiveRole);
    dialog.setDefaultButton(qobject_cast<QPushButton*>(keep));
    dialog.setEscapeButton(qobject_cast<QPushButton*>(keep));
    dialog.exec();
    if (dialog.clickedButton() == discard) QDialog::reject();
}

} // namespace fluxrec::ui
