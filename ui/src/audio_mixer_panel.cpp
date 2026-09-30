#include "audio_mixer_panel.hpp"

#include "audio/track_config.hpp"
#include "theme_tokens.hpp"

#include <QComboBox>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSlider>
#include <QStyle>
#include <QTimer>
#include <QVariantMap>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

namespace fluxrec::ui {
namespace {

std::optional<double> finiteNumber(const QJsonValue& value) {
    if (!value.isDouble()) return std::nullopt;
    const double result = value.toDouble();
    if (!std::isfinite(result)) return std::nullopt;
    return result;
}

QString formatCount(const QJsonValue& value) {
    const auto number = finiteNumber(value);
    if (!number || *number < 0.0) return QObject::tr("Not reported");
    return QLocale().toString(static_cast<qulonglong>(*number));
}

QString formatGain(double value) {
    return QLocale().toString(value, 'f', 2) + QStringLiteral("×");
}

QString formatAmplitude(const QJsonValue& value) {
    const auto number = finiteNumber(value);
    if (!number || *number < 0.0 || *number > 1.0) return QObject::tr("Not reported");
    return QLocale().toString(*number * 100.0, 'f', 1) + QStringLiteral("% FS");
}

QLabel* plainLabel(const QString& text, const QString& object_name = {}) {
    auto* label = new QLabel(text);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    if (!object_name.isEmpty()) label->setObjectName(object_name);
    return label;
}

[[nodiscard]] QVariantMap devicePayload(const QString& target, const QString& input_kind) {
    return QVariantMap{
        {QStringLiteral("target"), target},
        {QStringLiteral("kind"), input_kind},
    };
}

[[nodiscard]] std::vector<fluxrec::audio::AudioTrack> toAudioTracks(
    const QList<AudioTrackSettings>& tracks) {
    std::vector<fluxrec::audio::AudioTrack> converted;
    converted.reserve(static_cast<std::size_t>(tracks.size()));
    for (const AudioTrackSettings& track : tracks) {
        fluxrec::audio::InputKind kind = fluxrec::audio::InputKind::source;
        if (track.input_kind == QStringLiteral("sink-monitor") ||
            track.input_kind == QStringLiteral("sink_monitor")) {
            kind = fluxrec::audio::InputKind::sink_monitor;
        }
        converted.push_back({
            track.id.toStdString(),
            track.name.toStdString(),
            kind,
            track.input_target.toStdString(),
            track.monitor_target.toStdString(),
        });
    }
    return converted;
}

[[nodiscard]] int trackNumberFromId(const QString& id) {
    static const QString prefix = QStringLiteral("track-");
    if (!id.startsWith(prefix)) return 0;
    bool ok = false;
    const int number = id.mid(prefix.size()).toInt(&ok);
    return ok ? number : 0;
}

} // namespace

class AudioTrackRow final : public QFrame {
public:
    explicit AudioTrackRow(AudioTrackSettings track, QWidget* parent = nullptr)
        : QFrame(parent), track_(std::move(track)) {
        setObjectName(QStringLiteral("audioTrackRow"));
        setFrameShape(QFrame::StyledPanel);
        setProperty("trackId", track_.id);

        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(theme::panel_inset, 7, theme::panel_inset, 7);
        root->setSpacing(4);

        auto* heading = new QHBoxLayout;
        heading->setSpacing(theme::cluster_gap);
        name_edit_ = new QLineEdit(track_.name, this);
        name_edit_->setObjectName(QStringLiteral("audioTrackName"));
        name_edit_->setAccessibleName(QObject::tr("Audio track name"));
        name_edit_->setMaxLength(64);
        heading->addWidget(name_edit_, 1);
        state_label_ = plainLabel(QObject::tr("Not reported"), QStringLiteral("audioState"));
        state_label_->setAccessibleName(QObject::tr("Audio track stream state"));
        heading->addWidget(state_label_);
        remove_button_ = new QPushButton(QObject::tr("Remove"), this);
        remove_button_->setObjectName(QStringLiteral("removeAudioTrackButton"));
        remove_button_->setAccessibleName(QObject::tr("Remove audio track"));
        heading->addWidget(remove_button_);
        root->addLayout(heading);

        auto* devices = new QHBoxLayout;
        devices->setSpacing(theme::cluster_gap);
        input_combo_ = new QComboBox(this);
        input_combo_->setObjectName(QStringLiteral("audioTrackInput"));
        input_combo_->setAccessibleName(QObject::tr("Audio track input device"));
        input_combo_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        output_combo_ = new QComboBox(this);
        output_combo_->setObjectName(QStringLiteral("audioTrackOutput"));
        output_combo_->setAccessibleName(QObject::tr("Audio track monitor output"));
        output_combo_->setAccessibleDescription(QObject::tr(
            "Play this track’s captured input through the selected speakers while the session is running."));
        output_combo_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        devices->addWidget(plainLabel(QObject::tr("Input")));
        devices->addWidget(input_combo_, 1);
        devices->addWidget(plainLabel(QObject::tr("Output")));
        devices->addWidget(output_combo_, 1);
        root->addLayout(devices);

        auto* meters = new QGridLayout;
        meters->setContentsMargins(0, 0, 0, 0);
        meters->setHorizontalSpacing(theme::cluster_gap);
        meters->setVerticalSpacing(2);
        peak_label_ = plainLabel(QObject::tr("Peak not reported"), QStringLiteral("audioMeterLabel"));
        rms_label_ = plainLabel(QObject::tr("RMS not reported"), QStringLiteral("audioMeterLabel"));
        peak_bar_ = make_meter(QObject::tr("Audio peak amplitude"));
        rms_bar_ = make_meter(QObject::tr("Audio RMS amplitude"));
        meters->addWidget(peak_label_, 0, 0);
        meters->addWidget(peak_bar_, 0, 1);
        meters->addWidget(rms_label_, 1, 0);
        meters->addWidget(rms_bar_, 1, 1);
        meters->setColumnStretch(1, 1);
        root->addLayout(meters);

        auto* controls = new QHBoxLayout;
        controls->setSpacing(theme::cluster_gap);
        gain_label_ = plainLabel(QObject::tr("Gain —"), QStringLiteral("audioGainLabel"));
        gain_label_->setMinimumWidth(105);
        gain_slider_ = new QSlider(Qt::Horizontal, this);
        gain_slider_->setObjectName(track_.id + QStringLiteral("GainSlider"));
        gain_slider_->setRange(0, 400);
        gain_slider_->setSingleStep(1);
        gain_slider_->setPageStep(10);
        gain_slider_->setAccessibleName(QObject::tr("Track linear gain"));
        gain_slider_->setAccessibleDescription(QObject::tr(
            "Set linear software gain from 0.00× to 4.00×. The Gain label shows the recorder-reported value until this request is acknowledged."));
        gain_slider_->setToolTip(QObject::tr(
            "Set requested linear gain · 0.00× to 4.00×. Gain label remains the reported value until acknowledgement."));
        gain_slider_->setVisible(false);
        mute_button_ = new QPushButton(QObject::tr("Mute"), this);
        mute_button_->setObjectName(track_.id + QStringLiteral("MuteButton"));
        mute_button_->setVisible(false);
        mute_button_->setMinimumWidth(76);
        status_label_ = plainLabel({}, QStringLiteral("secondaryText"));
        status_label_->setObjectName(track_.id + QStringLiteral("AudioStatus"));
        status_label_->setAccessibleName(QObject::tr("Audio control status"));
        control_timeout_ = new QTimer(this);
        control_timeout_->setSingleShot(true);
        control_timeout_->setInterval(5000);
        connect(control_timeout_, &QTimer::timeout, this, [this] {
            if (!control_pending_) return;
            control_pending_ = false;
            pending_field_.clear();
            status_text_ = QObject::tr("The recorder did not acknowledge this audio change.");
            status_is_error_ = true;
            render_state();
            update_control_display();
        });
        controls->addWidget(gain_label_);
        controls->addWidget(gain_slider_, 1);
        controls->addWidget(mute_button_);
        controls->addWidget(status_label_);
        root->addLayout(controls);

        buffers_label_ = plainLabel(QObject::tr("Buffers · not reported"), QStringLiteral("secondaryText"));
        buffers_label_->setAccessibleName(QObject::tr("Received and dropped buffer counts"));
        root->addWidget(buffers_label_);

        connect(gain_slider_, &QSlider::sliderReleased, this, [this] { request_gain(); });
        connect(gain_slider_, &QSlider::valueChanged, this, [this] {
            if (!gain_slider_->isSliderDown()) request_gain();
        });
        connect(mute_button_, &QPushButton::clicked, this, [this] {
            if (!muted_known_ || !controls_allowed_ || control_pending_) return;
            const bool requested_mute = !muted_;
            control_pending_ = true;
            pending_field_ = QStringLiteral("muted");
            control_timeout_->start();
            update_control_display();
            if (mute_handler_) mute_handler_(track_.id, requested_mute);
        });
        connect(name_edit_, &QLineEdit::editingFinished, this, [this] {
            const QString trimmed = name_edit_->text().trimmed();
            if (trimmed.isEmpty() || trimmed.size() > 64 || trimmed == track_.name) {
                const QSignalBlocker blocker(name_edit_);
                name_edit_->setText(track_.name);
                return;
            }
            track_.name = trimmed;
            if (track_changed_) track_changed_(track_);
        });
        connect(input_combo_, &QComboBox::currentIndexChanged, this, [this](int index) {
            if (index < 0) return;
            const QVariantMap payload = input_combo_->itemData(index).toMap();
            const QString target = payload.value(QStringLiteral("target")).toString();
            const QString kind = payload.value(QStringLiteral("kind")).toString();
            if (target == track_.input_target && kind == track_.input_kind) return;
            track_.input_target = target;
            if (!kind.isEmpty()) track_.input_kind = kind;
            if (track_changed_) track_changed_(track_);
        });
        connect(output_combo_, &QComboBox::currentIndexChanged, this, [this](int index) {
            if (index < 0) return;
            const QString target = output_combo_->itemData(index).toString();
            if (target == track_.monitor_target) return;
            track_.monitor_target = target;
            if (track_changed_) track_changed_(track_);
        });
        connect(remove_button_, &QPushButton::clicked, this, [this] {
            if (remove_handler_) remove_handler_(track_.id);
        });
    }

    void setHandlers(std::function<void(const QString&, double)> volume,
                     std::function<void(const QString&, bool)> mute) {
        volume_handler_ = std::move(volume);
        mute_handler_ = std::move(mute);
    }

    void setTrackChangedHandler(std::function<void(AudioTrackSettings)> handler) {
        track_changed_ = std::move(handler);
    }

    void setRemoveHandler(std::function<void(const QString&)> handler) {
        remove_handler_ = std::move(handler);
    }

    void setRecordingActive(bool active) {
        recording_active_ = active;
        update_control_display();
    }

    void setEditEnabled(bool enabled) {
        name_edit_->setEnabled(enabled);
        input_combo_->setEnabled(enabled);
        output_combo_->setEnabled(enabled);
        remove_button_->setEnabled(enabled);
    }

    void beginSession() {
        control_timeout_->stop();
        state_name_.clear();
        pending_field_.clear();
        status_text_.clear();
        monitor_error_.clear();
        last_values_ = {};
        gain_ = 0.0;
        muted_ = false;
        gain_known_ = false;
        muted_known_ = false;
        control_pending_ = false;
        controls_allowed_ = false;
        status_is_error_ = false;
        has_meter_values_ = false;
        render_state();
        render_meters({}, false);
        update_control_display();
    }

    void setDevices(const QList<AudioDeviceEntry>& inputs,
                    const QList<AudioDeviceEntry>& outputs) {
        {
            const QSignalBlocker blocker(input_combo_);
            input_combo_->clear();
            bool found = false;
            for (const AudioDeviceEntry& entry : inputs) {
                const QString label = entry.description.isEmpty() ? entry.target : entry.description;
                input_combo_->addItem(label, devicePayload(entry.target, entry.input_kind));
                if (entry.target == track_.input_target) {
                    found = true;
                    input_combo_->setCurrentIndex(input_combo_->count() - 1);
                }
            }
            if (!found) {
                const QString label = track_.input_target.isEmpty()
                    ? QObject::tr("Default")
                    : track_.input_target + QObject::tr(" (unavailable)");
                input_combo_->insertItem(
                    0, label, devicePayload(track_.input_target, track_.input_kind));
                input_combo_->setCurrentIndex(0);
            }
        }
        {
            const QSignalBlocker blocker(output_combo_);
            output_combo_->clear();
            output_combo_->addItem(QObject::tr("None"), QString());
            bool found = track_.monitor_target.isEmpty();
            if (found) output_combo_->setCurrentIndex(0);
            for (const AudioDeviceEntry& entry : outputs) {
                const QString label = entry.description.isEmpty() ? entry.target : entry.description;
                output_combo_->addItem(label, entry.target);
                if (entry.target == track_.monitor_target) {
                    found = true;
                    output_combo_->setCurrentIndex(output_combo_->count() - 1);
                }
            }
            if (!found) {
                const QString label =
                    track_.monitor_target + QObject::tr(" (unavailable)");
                output_combo_->insertItem(1, label, track_.monitor_target);
                output_combo_->setCurrentIndex(1);
            }
        }
    }

    void setTelemetry(const QJsonObject& values, bool meters_live) {
        const QString next_state = values.value(QStringLiteral("state")).toString();
        state_name_ = next_state;
        if (values.contains(QStringLiteral("gain")) && values.value(QStringLiteral("gain")).isDouble()) {
            const double gain = values.value(QStringLiteral("gain")).toDouble();
            if (std::isfinite(gain) && gain >= 0.0 && gain <= 4.0) {
                gain_ = gain;
                gain_known_ = true;
            }
        }
        if (values.value(QStringLiteral("muted")).isBool()) {
            muted_ = values.value(QStringLiteral("muted")).toBool();
            muted_known_ = true;
        }
        monitor_error_ = values.value(QStringLiteral("monitor_error")).toString();
        render_state();
        render_meters(values, meters_live);

        const QString received = formatCount(values.value(QStringLiteral("buffers_received")));
        const QString dropped = formatCount(values.value(QStringLiteral("buffers_dropped")));
        buffers_label_->setText(QObject::tr("Buffers · received %1 · dropped %2")
                                    .arg(received, dropped));
        buffers_label_->setAccessibleDescription(buffers_label_->text());
        update_control_display();
    }

    void refreshMeters(bool live) {
        if (!has_meter_values_) return;
        render_meters(last_values_, live);
    }

    void applyControlChanged(const QJsonObject& values) {
        QString applied;
        bool updated = false;
        if (values.value(QStringLiteral("gain")).isDouble()) {
            const auto gain = finiteNumber(values.value(QStringLiteral("gain")));
            if (gain && *gain >= 0.0 && *gain <= 4.0) {
                gain_ = *gain;
                gain_known_ = true;
                updated = true;
                if (pending_field_ == QStringLiteral("gain")) {
                    control_pending_ = false;
                    control_timeout_->stop();
                    pending_field_.clear();
                    applied = QObject::tr("Gain applied by recorder");
                }
            }
        }
        if (values.value(QStringLiteral("muted")).isBool()) {
            muted_ = values.value(QStringLiteral("muted")).toBool();
            muted_known_ = true;
            updated = true;
            if (pending_field_ == QStringLiteral("muted")) {
                control_pending_ = false;
                control_timeout_->stop();
                pending_field_.clear();
                applied = QObject::tr("Mute state applied by recorder");
            }
        }
        if (!applied.isEmpty()) {
            status_text_ = applied;
            status_is_error_ = false;
        } else if (updated) {
            control_timeout_->stop();
            status_text_ = QObject::tr("Recorder audio state acknowledged");
            status_is_error_ = false;
        }
        render_state();
        update_control_display();
    }

    void applyControlFailed(const QString& message) {
        control_timeout_->stop();
        control_pending_ = false;
        pending_field_.clear();
        status_text_ = message;
        status_is_error_ = true;
        render_state();
        update_control_display();
    }

    [[nodiscard]] QString source() const { return track_.id; }
    [[nodiscard]] AudioTrackSettings track() const { return track_; }

private:
    void request_gain() {
        if (!gain_known_ || !controls_allowed_ || control_pending_) return;
        const double requested_gain = static_cast<double>(gain_slider_->value()) / 100.0;
        if (std::abs(requested_gain - gain_) < 0.005) return;
        control_pending_ = true;
        pending_field_ = QStringLiteral("gain");
        control_timeout_->start();
        update_control_display();
        if (volume_handler_) volume_handler_(track_.id, requested_gain);
    }

    QProgressBar* make_meter(const QString& name) {
        auto* meter = new QProgressBar(this);
        meter->setObjectName(name.contains(QObject::tr("peak"), Qt::CaseInsensitive)
                                 ? QStringLiteral("peakMeter") : QStringLiteral("rmsMeter"));
        meter->setRange(0, 1000);
        meter->setValue(0);
        meter->setTextVisible(false);
        meter->setFixedHeight(12);
        meter->setAccessibleName(name);
        meter->setAccessibleDescription(QObject::tr("Linear amplitude as a percentage of full scale."));
        return meter;
    }

    void render_state() {
        QString title;
        QString property;
        if (state_name_ == QStringLiteral("streaming")) {
            title = QObject::tr("Streaming");
            property = QStringLiteral("streaming");
        } else if (state_name_ == QStringLiteral("connecting")) {
            title = QObject::tr("Connecting…");
            property = QStringLiteral("connecting");
        } else if (state_name_ == QStringLiteral("stopped")) {
            title = QObject::tr("Stopped");
            property = QStringLiteral("stopped");
        } else if (state_name_ == QStringLiteral("error")) {
            title = QObject::tr("Error · see engine log");
            property = QStringLiteral("error");
        } else {
            title = QObject::tr("Status not reported");
            property = QStringLiteral("unknown");
        }
        state_label_->setText(title);
        state_label_->setProperty("audioState", property);
        state_label_->setAccessibleDescription(title);
        state_label_->style()->unpolish(state_label_);
        state_label_->style()->polish(state_label_);
        if (status_is_error_) status_label_->setProperty("validation", true);
        else status_label_->setProperty("validation", false);
        status_label_->style()->unpolish(status_label_);
        status_label_->style()->polish(status_label_);
    }

    void render_meters(const QJsonObject& values, bool live) {
        last_values_ = values;
        const auto samples = finiteNumber(values.value(QStringLiteral("samples")));
        const bool has_samples = samples && *samples > 0.0;
        const auto peak = finiteNumber(values.value(QStringLiteral("peak")));
        const auto rms = finiteNumber(values.value(QStringLiteral("rms")));
        const bool peak_valid = has_samples && peak && *peak >= 0.0 && *peak <= 1.0;
        const bool rms_valid = has_samples && rms && *rms >= 0.0 && *rms <= 1.0;
        has_meter_values_ = peak_valid || rms_valid;

        if (peak_valid) {
            peak_label_->setText(QObject::tr("Peak · %1%2")
                .arg(formatAmplitude(QJsonValue(*peak)), live ? QString{} : QObject::tr(" · last report")));
            peak_bar_->setValue(live ? static_cast<int>(std::lround(*peak * 1000.0)) : 0);
        } else {
            peak_label_->setText(QObject::tr("Peak · not reported"));
            peak_bar_->setValue(0);
        }
        if (rms_valid) {
            rms_label_->setText(QObject::tr("RMS · %1%2")
                .arg(formatAmplitude(QJsonValue(*rms)), live ? QString{} : QObject::tr(" · last report")));
            rms_bar_->setValue(live ? static_cast<int>(std::lround(*rms * 1000.0)) : 0);
        } else {
            rms_label_->setText(QObject::tr("RMS · not reported"));
            rms_bar_->setValue(0);
        }
        const bool stale = has_samples && !live;
        for (QProgressBar* meter : {peak_bar_, rms_bar_}) {
            meter->setProperty("stale", stale);
            meter->style()->unpolish(meter);
            meter->style()->polish(meter);
        }
        peak_label_->setAccessibleDescription(peak_label_->text());
        rms_label_->setAccessibleDescription(rms_label_->text());
    }

    void update_control_display() {
        gain_label_->setText(gain_known_
            ? QObject::tr("Gain %1").arg(formatGain(gain_))
            : QObject::tr("Gain —"));
        const bool streaming = state_name_ == QStringLiteral("streaming");
        const bool usable = recording_active_ && streaming;
        gain_slider_->setVisible(usable && gain_known_);
        if (gain_known_) {
            if (!control_pending_ || pending_field_ != QStringLiteral("gain")) {
                const QSignalBlocker blocker(gain_slider_);
                gain_slider_->setValue(static_cast<int>(std::lround(gain_ * 100.0)));
            }
        }
        mute_button_->setVisible(usable && muted_known_);
        mute_button_->setText(muted_ ? QObject::tr("Unmute") : QObject::tr("Mute"));
        mute_button_->setAccessibleName(muted_
            ? QObject::tr("Unmute %1 audio").arg(track_.name)
            : QObject::tr("Mute %1 audio").arg(track_.name));
        mute_button_->setAccessibleDescription(muted_
            ? QObject::tr("The recorder reports this source muted.")
            : QObject::tr("The recorder reports this source unmuted."));

        controls_allowed_ = usable;
        gain_slider_->setEnabled(usable && gain_known_ && !control_pending_);
        mute_button_->setEnabled(usable && muted_known_ && !control_pending_);

        QString display_status = status_text_;
        bool display_error = status_is_error_;
        if (control_pending_) {
            display_status = pending_field_ == QStringLiteral("gain")
                ? QObject::tr("Waiting for gain acknowledgement…")
                : QObject::tr("Waiting for mute acknowledgement…");
            display_error = false;
        } else if (!monitor_error_.isEmpty() && display_status.isEmpty()) {
            display_status = monitor_error_;
            display_error = true;
        } else if (usable && display_status == QObject::tr("Controls available when the source connects")) {
            display_status.clear();
        } else if (display_status.isEmpty() && !usable && recording_active_ &&
                   state_name_ == QStringLiteral("connecting")) {
            display_status = QObject::tr("Controls available when the stream connects");
        }
        status_label_->setText(display_status);
        status_label_->setVisible(!display_status.isEmpty());
        status_label_->setProperty("validation", display_error);
        status_label_->style()->unpolish(status_label_);
        status_label_->style()->polish(status_label_);
        if (!usable && !control_pending_) {
            gain_slider_->setToolTip(QObject::tr("Audio control is available when the source is streaming during recording."));
            mute_button_->setToolTip(QObject::tr("Audio control is available when the source is streaming during recording."));
        } else {
            gain_slider_->setToolTip(QObject::tr(
                "Set requested linear gain · 0.00× to 4.00×. Gain label remains the reported value until acknowledgement."));
            mute_button_->setToolTip(muted_
                ? QObject::tr("Unmute this audio source") : QObject::tr("Mute this audio source"));
        }
    }

    AudioTrackSettings track_;
    QLineEdit* name_edit_ = nullptr;
    QComboBox* input_combo_ = nullptr;
    QComboBox* output_combo_ = nullptr;
    QPushButton* remove_button_ = nullptr;
    QLabel* state_label_ = nullptr;
    QLabel* peak_label_ = nullptr;
    QLabel* rms_label_ = nullptr;
    QLabel* gain_label_ = nullptr;
    QLabel* status_label_ = nullptr;
    QLabel* buffers_label_ = nullptr;
    QProgressBar* peak_bar_ = nullptr;
    QProgressBar* rms_bar_ = nullptr;
    QSlider* gain_slider_ = nullptr;
    QPushButton* mute_button_ = nullptr;
    QTimer* control_timeout_ = nullptr;
    std::function<void(const QString&, double)> volume_handler_;
    std::function<void(const QString&, bool)> mute_handler_;
    std::function<void(AudioTrackSettings)> track_changed_;
    std::function<void(const QString&)> remove_handler_;
    QJsonObject last_values_;
    QString state_name_;
    QString pending_field_;
    QString status_text_;
    QString monitor_error_;
    double gain_ = 0.0;
    bool muted_ = false;
    bool gain_known_ = false;
    bool muted_known_ = false;
    bool control_pending_ = false;
    bool controls_allowed_ = false;
    bool recording_active_ = false;
    bool status_is_error_ = false;
    bool has_meter_values_ = false;
};

AudioMixerPanel::AudioMixerPanel(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(theme::panel_inset, theme::panel_inset,
                             theme::panel_inset, theme::panel_inset);
    root->setSpacing(theme::cluster_gap);

    auto* heading = new QHBoxLayout;
    heading->addWidget(plainLabel(tr("Audio mixer"), QStringLiteral("sectionTitle")));
    heading->addStretch(1);
    freshness_label_ = plainLabel(tr("No audio telemetry"), QStringLiteral("secondaryText"));
    freshness_label_->setAccessibleName(tr("Audio telemetry freshness"));
    heading->addWidget(freshness_label_);
    root->addLayout(heading);

    empty_state_ = plainLabel({}, QStringLiteral("secondaryText"));
    empty_state_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    empty_state_->setMaximumWidth(640);
    empty_state_->setAccessibleName(tr("Audio mixer status"));
    root->addWidget(empty_state_);

    devices_error_label_ = plainLabel({}, QStringLiteral("secondaryText"));
    devices_error_label_->setObjectName(QStringLiteral("audioDevicesUnavailable"));
    devices_error_label_->setVisible(false);
    root->addWidget(devices_error_label_);

    rows_layout_ = new QVBoxLayout;
    rows_layout_->setContentsMargins(0, 0, 0, 0);
    rows_layout_->setSpacing(theme::cluster_gap);
    root->addLayout(rows_layout_);

    add_button_ = new QPushButton(tr("Add track"), this);
    add_button_->setObjectName(QStringLiteral("addAudioTrackButton"));
    add_button_->setAccessibleName(tr("Add audio track"));
    connect(add_button_, &QPushButton::clicked, this, [this] { appendTrack(); });
    root->addWidget(add_button_, 0, Qt::AlignLeft);

    summary_label_ = plainLabel({}, QStringLiteral("secondaryText"));
    summary_label_->setAccessibleName(tr("Audio packet statistics"));
    root->addWidget(summary_label_);
    root->addStretch(1);

    render();
}

void AudioMixerPanel::setAudioConfigured(bool enabled) {
    configured_ = enabled;
    render();
}

void AudioMixerPanel::setRecordingActive(bool active) {
    recording_active_ = active;
    for (AudioTrackRow* row : rows_) row->setRecordingActive(active);
    refreshFreshness();
}

void AudioMixerPanel::setReplayOnlySession(bool active, bool audio_enabled) {
    replay_only_session_ = active;
    replay_only_audio_enabled_ = audio_enabled;
    render();
    refreshFreshness();
}

void AudioMixerPanel::beginSession() {
    has_snapshot_ = false;
    telemetry_enabled_known_ = false;
    telemetry_enabled_ = false;
    audio_snapshot_ = {};
    received_at_ = {};
    summary_label_->clear();
    for (AudioTrackRow* row : rows_) row->beginSession();
    render();
    refreshFreshness();
}

void AudioMixerPanel::setSnapshot(const QJsonObject& audio, const QDateTime& received_at) {
    audio_snapshot_ = audio;
    received_at_ = received_at;
    has_snapshot_ = true;
    const QJsonValue enabled = audio.value(QStringLiteral("enabled"));
    telemetry_enabled_known_ = enabled.isBool();
    telemetry_enabled_ = enabled.toBool(false);
    const bool meters_live =
        recording_active_ && received_at_.secsTo(QDateTime::currentDateTime()) <= 3;
    const QJsonArray tracks = audio.value(QStringLiteral("tracks")).toArray();
    for (const QJsonValue& value : tracks) {
        const QJsonObject track_object = value.toObject();
        const QString id = track_object.value(QStringLiteral("id")).toString();
        if (auto* row = rowForSource(id)) row->setTelemetry(track_object, meters_live);
    }
    const QString captured = formatCount(audio.value(QStringLiteral("captured_packets")));
    const QString callback_drops = formatCount(audio.value(QStringLiteral("callback_drops")));
    const QString unforwarded_drops = formatCount(audio.value(QStringLiteral("unforwarded_drops")));
    const QString mux_drops = formatCount(audio.value(QStringLiteral("mux_drops")));
    const QString queue_packets = formatCount(audio.value(QStringLiteral("queue_packets")));
    const QString queue_bytes = formatCount(audio.value(QStringLiteral("queue_bytes")));
    const QString replay_stored = formatCount(audio.value(QStringLiteral("replay_stored_packets")));
    const QString replay_drops = formatCount(audio.value(QStringLiteral("replay_drops")));
    const QString sync_errors = formatCount(audio.value(QStringLiteral("sync_errors")));
    const QString audio_errors = formatCount(audio.value(QStringLiteral("audio_errors")));
    summary_label_->setText(tr("Packets %1 · callback drops %2 · unforwarded %3 · mux drops %4\n"
                               "Replay stored %5 · replay drops %6 · queue %7 packets / %8 bytes\n"
                               "Sync errors %9 · audio errors %10")
                                .arg(captured, callback_drops, unforwarded_drops, mux_drops,
                                     replay_stored, replay_drops, queue_packets, queue_bytes)
                                .arg(sync_errors, audio_errors));
    summary_label_->setVisible(telemetry_enabled_);
    render();
    refreshFreshness();
}

void AudioMixerPanel::refreshFreshness() {
    if (replay_only_session_ && !replay_only_audio_enabled_) {
        freshness_label_->setText(tr("Not used · audio is off for replay-only"));
        for (AudioTrackRow* row : rows_) row->refreshMeters(false);
        return;
    }
    if (!has_snapshot_) {
        freshness_label_->setText(recording_active_ && configured_
            ? tr("Waiting for audio telemetry") : tr("No audio telemetry"));
        return;
    }
    const qint64 age = received_at_.secsTo(QDateTime::currentDateTime());
    const bool live = recording_active_ && age <= 3;
    if (recording_active_ && age > 3) {
        freshness_label_->setText(tr("Stale · last report %1 s ago").arg(age));
    } else if (live) {
        freshness_label_->setText(tr("Live · reported %1 s ago").arg(age));
    } else {
        freshness_label_->setText(tr("Last report · %1")
            .arg(QLocale().toString(received_at_.time(), QLocale::ShortFormat)));
    }
    for (AudioTrackRow* row : rows_) row->refreshMeters(live);
    render();
}

void AudioMixerPanel::applyControlChanged(const QString& source, const QJsonObject& values) {
    if (auto* row = rowForSource(source)) row->applyControlChanged(values);
}

void AudioMixerPanel::applyControlFailed(const QString& source, const QString& message) {
    if (auto* row = rowForSource(source)) row->applyControlFailed(message);
}

void AudioMixerPanel::setControlHandlers(
    std::function<void(const QString&, double)> volume_handler,
    std::function<void(const QString&, bool)> mute_handler) {
    volume_handler_ = std::move(volume_handler);
    mute_handler_ = std::move(mute_handler);
    for (AudioTrackRow* row : rows_) wireRow(row);
}

void AudioMixerPanel::setTracks(QList<AudioTrackSettings> tracks, bool custom) {
    custom_ = custom;
    if (tracks_ == tracks) {
        render();
        return;
    }
    tracks_ = std::move(tracks);
    rebuildRows();
    render();
}

void AudioMixerPanel::setDevices(const AudioDeviceCatalog& catalog) {
    inputs_ = catalog.inputs();
    outputs_ = catalog.outputs();
    devices_error_ = catalog.error();
    devices_error_label_->setText(devices_error_);
    devices_error_label_->setVisible(!devices_error_.isEmpty());
    refreshDeviceCombos();
}

void AudioMixerPanel::setSessionBusy(bool busy) {
    session_busy_ = busy;
    updateEditEnabled();
}

void AudioMixerPanel::rebuildRows() {
    while (QLayoutItem* item = rows_layout_->takeAt(0)) {
        if (QWidget* widget = item->widget()) {
            widget->hide();
            widget->setParent(nullptr);
            delete widget;
        }
        delete item;
    }
    rows_.clear();
    rows_.reserve(static_cast<std::size_t>(tracks_.size()));
    for (const AudioTrackSettings& track : tracks_) {
        auto* row = new AudioTrackRow(track, this);
        wireRow(row);
        row->setRecordingActive(recording_active_);
        row->setDevices(inputs_, outputs_);
        rows_layout_->addWidget(row);
        rows_.push_back(row);
    }
    updateEditEnabled();
}

void AudioMixerPanel::refreshDeviceCombos() {
    for (AudioTrackRow* row : rows_) row->setDevices(inputs_, outputs_);
}

void AudioMixerPanel::updateEditEnabled() {
    const bool editable = !session_busy_;
    const bool can_add =
        editable && static_cast<std::size_t>(tracks_.size()) < fluxrec::audio::max_audio_tracks;
    add_button_->setEnabled(can_add);
    for (AudioTrackRow* row : rows_) row->setEditEnabled(editable);
}

void AudioMixerPanel::emitTracksEdited() {
    custom_ = true;
    emit tracksEdited(tracks_);
}

void AudioMixerPanel::appendTrack() {
    if (static_cast<std::size_t>(tracks_.size()) >= fluxrec::audio::max_audio_tracks) return;
    const auto converted = toAudioTracks(tracks_);
    const QString id = QString::fromStdString(fluxrec::audio::nextTrackId(converted));
    const int number = trackNumberFromId(id);
    tracks_.push_back({
        id,
        number > 0 ? tr("Track %1").arg(number) : tr("Track"),
        QStringLiteral("source"),
        QString(),
        QString(),
    });
    rebuildRows();
    emitTracksEdited();
    render();
}

void AudioMixerPanel::removeTrack(const QString& id) {
    const auto existing = std::find_if(tracks_.begin(), tracks_.end(),
                                       [&id](const AudioTrackSettings& track) {
                                           return track.id == id;
                                       });
    if (existing == tracks_.end()) return;
    tracks_.erase(existing);
    rebuildRows();
    emitTracksEdited();
    render();
}

void AudioMixerPanel::onRowEdited(const QString& id, AudioTrackSettings track) {
    for (AudioTrackSettings& existing : tracks_) {
        if (existing.id != id) continue;
        existing = std::move(track);
        emitTracksEdited();
        return;
    }
}

void AudioMixerPanel::wireRow(AudioTrackRow* row) {
    row->setHandlers(
        [this](const QString& source, double gain) {
            if (volume_handler_) volume_handler_(source, gain);
        },
        [this](const QString& source, bool muted) {
            if (mute_handler_) mute_handler_(source, muted);
        });
    row->setTrackChangedHandler([this](AudioTrackSettings track) {
        onRowEdited(track.id, std::move(track));
    });
    row->setRemoveHandler([this](const QString& id) { removeTrack(id); });
}

void AudioMixerPanel::render() {
    summary_label_->setVisible(has_snapshot_ && telemetry_enabled_);

    QString message;
    if (replay_only_session_ && !replay_only_audio_enabled_) {
        message = tr("Audio capture is off for this replay-only session. Enable audio tracks in Settings to retain audio with replay history.");
    } else if (replay_only_session_) {
        message = tr("Audio tracks are retained with replay history. No primary recording file is written.");
    } else if (has_snapshot_ && telemetry_enabled_known_ && !telemetry_enabled_) {
        message = tr("Audio capture is off for this recorder session. Settings changes apply to the next capture session.");
    } else if (has_snapshot_ && !telemetry_enabled_known_) {
        message = tr("The recorder has not reported audio capability or levels yet.");
    } else if (recording_active_ && configured_) {
        message = tr("Waiting for the recorder to report audio tracks. No meters are simulated.");
    } else if (configured_) {
        message = tr("Audio tracks are enabled in Settings. Start a recording or replay-only session to connect the audio streams.");
    } else {
        message = tr("Audio capture is off. Enable audio tracks in Settings. The mixer keeps the track list for the next session.");
    }
    empty_state_->setText(message);
    empty_state_->setAccessibleDescription(message);
    empty_state_->setVisible(!message.isEmpty());
}

AudioTrackRow* AudioMixerPanel::rowForSource(const QString& source) const {
    for (AudioTrackRow* row : rows_) {
        if (row->source() == source) return row;
    }
    return nullptr;
}

} // namespace fluxrec::ui
