#include "process_recorder_backend.hpp"

#include "audio/track_config.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLocale>
#include <QStandardPaths>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cmath>
#include <unistd.h>

namespace fluxrec::ui {
namespace {

constexpr qsizetype max_json_line_bytes = 1024 * 1024;
constexpr qsizetype max_stderr_bytes = 64 * 1024;
constexpr int graceful_stop_timeout_ms = 8000;
constexpr int control_ack_timeout_ms = 5000;

QString resolveExecutable(const QString& configured) {
    if (configured.trimmed().isEmpty()) {
        const QString self = QCoreApplication::applicationFilePath();
        const QFileInfo info(self);
        if (!self.isEmpty() && info.exists() && info.isFile() && info.isExecutable()) {
            return info.canonicalFilePath().isEmpty() ? info.absoluteFilePath()
                                                      : info.canonicalFilePath();
        }
        return {};
    }

    const QFileInfo direct(configured);
    QString path;
    if (configured.contains(QLatin1Char('/'))) {
        path = direct.absoluteFilePath();
    } else {
        path = QStandardPaths::findExecutable(configured);
    }
    const QFileInfo resolved(path);
    if (path.isEmpty() || !resolved.exists() || !resolved.isFile() || !resolved.isExecutable()) {
        return {};
    }
    return resolved.canonicalFilePath().isEmpty()
        ? resolved.absoluteFilePath() : resolved.canonicalFilePath();
}

bool launchesEmbeddedRecorder(const QString& executable_path) {
    const QString self = QFileInfo(QCoreApplication::applicationFilePath()).canonicalFilePath();
    const QString child = QFileInfo(executable_path).canonicalFilePath();
    return !self.isEmpty() && self == child;
}

void startRecorderProcess(QProcess& process, const QString& executable_path,
                          QStringList arguments) {
    if (launchesEmbeddedRecorder(executable_path)) {
        arguments.prepend(QStringLiteral("--engine"));
    }
    process.setProgram(executable_path);
    process.setArguments(arguments);
}

bool directCudaEgl(const RecorderSettings& settings) {
    return settings.encoder_backend == QStringLiteral("experimental-cuda-egl");
}

bool validOutputResolution(const RecorderSettings& settings) {
    if (directCudaEgl(settings)) {
        const bool native_size = settings.output_width == 0 && settings.output_height == 0;
        return native_size ||
            (settings.output_width == 2560 && settings.output_height == 1440);
    }
    if (settings.output_width == 0 && settings.output_height == 0) return true;
    return settings.output_width >= 4 && settings.output_width <= 16384 &&
        settings.output_height >= 4 && settings.output_height <= 16384 &&
        settings.output_width % 2 == 0 && settings.output_height % 2 == 0;
}

bool validCodecSelection(const RecorderSettings& settings) {
    if (settings.codec == QStringLiteral("h264")) return true;
    return settings.codec == QStringLiteral("hevc") && settings.allow_experimental_hevc;
}

bool validSupportedRateControl(const RecorderSettings& settings) {
    if (directCudaEgl(settings) &&
        (settings.direct_pool_size < 3 || settings.direct_pool_size > 16 ||
         settings.direct_async_depth < 1 || settings.direct_async_depth > 16)) {
        return false;
    }
    if (settings.rate_control == QStringLiteral("bitrate")) {
        return settings.bitrate_mbps >= 1 && settings.bitrate_mbps <= 500;
    }
    if (settings.rate_control == QStringLiteral("constant_qp")) {
        return settings.qp >= 0 && settings.qp <= 51;
    }
    return false;
}

void appendRateControlArguments(QStringList& arguments, const RecorderSettings& settings) {
    if (settings.rate_control == QStringLiteral("constant_qp")) {
        arguments << QStringLiteral("--qp") << QString::number(settings.qp);
        return;
    }
    arguments << QStringLiteral("--bitrate-mbps") << QString::number(settings.bitrate_mbps);
}

void appendEncoderBackendArguments(QStringList& arguments,
                                   const RecorderSettings& settings) {
    arguments << QStringLiteral("--backend") << settings.encoder_backend;
    if (directCudaEgl(settings)) {
        arguments << QStringLiteral("--async-depth")
                  << QString::number(settings.direct_async_depth);
        arguments << QStringLiteral("--b-frames") << QStringLiteral("0");
    } else if (settings.encoder_backend == QStringLiteral("nvenc")) {
        arguments << QStringLiteral("--nvenc-preset") << settings.nvenc_preset;
        arguments << QStringLiteral("--async-depth") << QString::number(settings.nvenc_async_depth);
        arguments << QStringLiteral("--b-frames") << QString::number(settings.nvenc_b_frames);
    } else {
        arguments << QStringLiteral("--async-depth") << QString::number(settings.async_depth);
    }
}

void appendFrameIdentityArguments(QStringList& arguments,
                                  const RecorderSettings& settings) {
    if (settings.verify_frame_ids) {
        arguments << QStringLiteral("--verify-frame-ids");
    }
}

} // namespace

ProcessRecorderBackend::ProcessRecorderBackend(const RecorderSettings& settings, QObject* parent)
    : RecorderBackend(parent), settings_(settings) {
    normalizeEncoderSettings(settings_);
    preview_channel_ = new PreviewChannel(this);
    process_.setProcessChannelMode(QProcess::SeparateChannels);
    stop_timer_.setSingleShot(true);
    replay_control_timer_.setSingleShot(true);

    connect(&process_, &QProcess::started, this, &ProcessRecorderBackend::handle_started);
    connect(&process_, &QProcess::finished, this, &ProcessRecorderBackend::handle_finished);
    connect(&process_, &QProcess::errorOccurred,
            this, &ProcessRecorderBackend::handle_process_error);
    connect(&process_, &QProcess::readyReadStandardOutput,
            this, &ProcessRecorderBackend::read_stdout);
    connect(&process_, &QProcess::readyReadStandardError,
            this, &ProcessRecorderBackend::read_stderr);
    connect(&stop_timer_, &QTimer::timeout,
            this, &ProcessRecorderBackend::handle_stop_timeout);
    connect(&replay_control_timer_, &QTimer::timeout,
            this, &ProcessRecorderBackend::handle_replay_control_timeout);

    refresh_executable();
}

bool ProcessRecorderBackend::can_start_recording() const noexcept {
    return !executable_path_.isEmpty() && process_.state() == QProcess::NotRunning &&
        state_ != State::starting && state_ != State::recording &&
        state_ != State::replay_only && state_ != State::stopping &&
        validOutputResolution(settings_) && validCodecSelection(settings_) &&
        validSupportedRateControl(settings_);
}

bool ProcessRecorderBackend::can_start_replay_only_session() const noexcept {
    return !executable_path_.isEmpty() && settings_.replay_history_seconds > 0 &&
        process_.state() == QProcess::NotRunning && state_ == State::available &&
        validOutputResolution(settings_) && validCodecSelection(settings_) &&
        validSupportedRateControl(settings_);
}

bool ProcessRecorderBackend::can_save_replay() const noexcept {
    return replay_available_ && replay_reported_ && !replay_control_pending_ &&
        (state_ == State::recording || state_ == State::replay_only) &&
        process_.state() == QProcess::Running;
}

bool ProcessRecorderBackend::can_start_replay_buffer() const noexcept {
    if (can_start_replay_only_session()) return true;
    return replay_available_ && replay_reported_ && !replay_active_ && !replay_control_pending_ &&
        (state_ == State::recording || state_ == State::replay_only) &&
        process_.state() == QProcess::Running;
}

bool ProcessRecorderBackend::can_stop_replay_buffer() const noexcept {
    return replay_available_ && replay_reported_ && replay_active_ && !replay_control_pending_ &&
        (state_ == State::recording || state_ == State::replay_only) &&
        process_.state() == QProcess::Running;
}

void ProcessRecorderBackend::set_settings(const RecorderSettings& settings) {
    settings_ = settings;
    normalizeEncoderSettings(settings_);
    if (process_.state() == QProcess::NotRunning) {
        refresh_executable();
        replay_available_ = false;
        replay_reported_ = false;
        replay_active_ = false;
        replay_control_pending_ = false;
        replay_control_timer_.stop();
        emit replay_buffer_changed(false);
    }
}

void ProcessRecorderBackend::refresh_executable() {
    executable_path_ = resolveExecutable(settings_.executable);
    set_state(executable_path_.isEmpty() ? State::unavailable : State::available);
}

void ProcessRecorderBackend::start_recording(const QString& output_path) {
    if (!can_start_recording()) return;
    if (output_path.trimmed().isEmpty()) {
        const QString error = tr("Choose a recording output path in Settings.");
        emit error_changed(error);
        return;
    }
    if (!QDir().mkpath(QFileInfo(output_path).absolutePath())) {
        const QString error = tr("Fluxrec could not create the recording folder.");
        emit error_changed(error);
        return;
    }

    stdout_buffer_.clear();
    stderr_buffer_.clear();
    active_output_path_ = output_path;
    last_error_.clear();
    stop_requested_ = false;
    forced_termination_ = false;
    active_replay_only_ = false;
    replay_available_ = false;
    replay_reported_ = false;
    replay_active_ = false;
    replay_control_pending_ = false;
    replay_target_active_ = false;
    replay_control_timer_.stop();
    active_audio_enabled_ = settings_.audio_enabled;
    emit replay_buffer_changed(false);
    emit error_changed({});

    const bool direct = directCudaEgl(settings_);
    QStringList arguments{
        QStringLiteral("--output"), output_path,
        QStringLiteral("--until-signal"),
        QStringLiteral("--display"), settings_.display_name,
        QStringLiteral("--codec"), settings_.codec,
        QStringLiteral("--gop"), QString::number(settings_.gop_size),
        QStringLiteral("--max-fps"), QString::number(settings_.max_fps),
        QStringLiteral("--pool"), QString::number(direct ? settings_.direct_pool_size
                                                         : settings_.pool_size),
    };
    appendAudioTrackArguments(arguments, settings_);
    if (!direct) {
        arguments << QStringLiteral("--conversion") << QStringLiteral("two-pass");
    }
    appendEncoderBackendArguments(arguments, settings_);
    appendRateControlArguments(arguments, settings_);
    if (direct) {
        append_diagnostic(tr(
            "Direct CUDA EGL does not use the GPU preview or frame-identity digest."));
    } else {
        appendFrameIdentityArguments(arguments, settings_);
        QString preview_error;
        if (preview_channel_->start_listening(&preview_error)) {
            arguments << QStringLiteral("--preview-socket") << preview_channel_->socket_path();
            append_diagnostic(tr("GPU preview socket opened."));
        } else {
            append_diagnostic(tr("GPU preview is unavailable: %1").arg(preview_error));
        }
    }
    if (settings_.output_width > 0 && settings_.output_height > 0) {
        arguments << QStringLiteral("--resolution")
                  << QStringLiteral("%1x%2").arg(settings_.output_width).arg(settings_.output_height);
    }
    if (settings_.replay_history_seconds > 0) {
        arguments << QStringLiteral("--replay-seconds")
                  << QString::number(settings_.replay_history_seconds);
    }

    append_diagnostic(tr("Starting %1").arg(executable_path_));
    append_diagnostic(tr("Output: %1").arg(output_path));
    startRecorderProcess(process_, executable_path_, arguments);
    set_state(State::starting);
    process_.start();
}

void ProcessRecorderBackend::start_replay_only_session() {
    if (!can_start_replay_only_session()) return;

    stdout_buffer_.clear();
    stderr_buffer_.clear();
    active_output_path_.clear();
    last_error_.clear();
    stop_requested_ = false;
    forced_termination_ = false;
    active_replay_only_ = true;
    active_audio_enabled_ = settings_.audio_enabled;
    replay_available_ = false;
    replay_reported_ = false;
    replay_active_ = false;
    replay_control_pending_ = false;
    replay_target_active_ = false;
    replay_control_timer_.stop();
    emit replay_buffer_changed(false);
    emit error_changed({});

    const bool direct = directCudaEgl(settings_);
    QStringList arguments{
        QStringLiteral("--replay-only"),
        QStringLiteral("--replay-seconds"), QString::number(settings_.replay_history_seconds),
        QStringLiteral("--until-signal"),
        QStringLiteral("--display"), settings_.display_name,
        QStringLiteral("--codec"), settings_.codec,
        QStringLiteral("--gop"), QString::number(settings_.gop_size),
        QStringLiteral("--max-fps"), QString::number(settings_.max_fps),
        QStringLiteral("--pool"), QString::number(direct ? settings_.direct_pool_size
                                                         : settings_.pool_size),
    };
    appendAudioTrackArguments(arguments, settings_);
    if (!direct) {
        arguments << QStringLiteral("--conversion") << QStringLiteral("two-pass");
    }
    appendEncoderBackendArguments(arguments, settings_);
    appendRateControlArguments(arguments, settings_);
    if (direct) {
        append_diagnostic(tr(
            "Direct CUDA EGL does not use the GPU preview or frame-identity digest."));
    } else {
        appendFrameIdentityArguments(arguments, settings_);
        QString preview_error;
        if (preview_channel_->start_listening(&preview_error)) {
            arguments << QStringLiteral("--preview-socket") << preview_channel_->socket_path();
            append_diagnostic(tr("GPU preview socket opened."));
        } else {
            append_diagnostic(tr("GPU preview is unavailable: %1").arg(preview_error));
        }
    }
    if (settings_.output_width > 0 && settings_.output_height > 0) {
        arguments << QStringLiteral("--resolution")
                  << QStringLiteral("%1x%2").arg(settings_.output_width).arg(settings_.output_height);
    }

    append_diagnostic(tr("Starting replay-only capture session with %1").arg(executable_path_));
    append_diagnostic(tr("Replay history: %1 seconds · no primary recording file")
                          .arg(settings_.replay_history_seconds));
    startRecorderProcess(process_, executable_path_, arguments);
    set_state(State::starting);
    process_.start();
}

void ProcessRecorderBackend::stop_recording() {
    if (process_.state() == QProcess::NotRunning || state_ == State::stopping) return;
    stop_requested_ = true;
    set_state(State::stopping);
    stop_timer_.start(graceful_stop_timeout_ms);
    if (process_.state() == QProcess::Running) {
        if (!active_replay_only_ || !write_control_line(QByteArrayLiteral("stop\n"))) {
            send_sigint();
        }
    }
}

void ProcessRecorderBackend::start_replay_buffer() {
    if (can_start_replay_only_session()) {
        start_replay_only_session();
        return;
    }
    if (!can_start_replay_buffer()) return;
    if (!write_control_line(QByteArrayLiteral("start_replay\n"))) {
        emit error_changed(tr("Could not send the replay start command to the recorder."));
        return;
    }
    replay_control_pending_ = true;
    replay_target_active_ = true;
    replay_control_timer_.start(control_ack_timeout_ms);
    append_diagnostic(tr("Replay start requested."));
    emit replay_buffer_changed(replay_active_);
}

void ProcessRecorderBackend::stop_replay_buffer() {
    if (!can_stop_replay_buffer()) return;
    if (!write_control_line(QByteArrayLiteral("stop_replay\n"))) {
        emit error_changed(tr("Could not send the replay stop command to the recorder."));
        return;
    }
    replay_control_pending_ = true;
    replay_target_active_ = false;
    replay_control_timer_.start(control_ack_timeout_ms);
    append_diagnostic(tr("Replay stop requested."));
    emit replay_buffer_changed(replay_active_);
}

void ProcessRecorderBackend::save_replay(const QString& output_path) {
    if (!can_save_replay()) return;
    const QString absolute_path = QFileInfo(output_path).absoluteFilePath();
    emit error_changed({});
    const bool has_control_character = std::any_of(absolute_path.cbegin(), absolute_path.cend(),
        [](QChar character) { return character.unicode() < 0x20 || character.unicode() == 0x7f; });
    if (has_control_character) {
        emit error_changed(tr("Replay output paths cannot contain control characters."));
        emit replay_saved(absolute_path, false);
        return;
    }
    const QByteArray command = QByteArrayLiteral("save_replay ") + absolute_path.toUtf8() + '\n';
    if (!write_control_line(command)) {
        emit error_changed(tr("Could not send the replay request to the recorder."));
        emit replay_saved(absolute_path, false);
        return;
    }
    append_diagnostic(tr("Replay save request sent: %1").arg(absolute_path));
}

void ProcessRecorderBackend::set_audio_volume(const QString& source, double linear_gain) {
    if (!fluxrec::audio::validTrackId(source.toStdString())) {
        emit audio_control_failed(source, tr("Unknown audio source."));
        return;
    }
    if (!active_audio_enabled_ ||
        (state_ != State::recording && state_ != State::replay_only) ||
        process_.state() != QProcess::Running) {
        emit audio_control_failed(source, tr("Audio controls are unavailable in this session."));
        return;
    }
    if (!std::isfinite(linear_gain) || linear_gain < 0.0 || linear_gain > 4.0) {
        emit audio_control_failed(source, tr("Gain must be between 0.00× and 4.00×."));
        return;
    }
    const QByteArray command = QByteArrayLiteral("set_audio_volume ") + source.toUtf8() + ' ' +
        QLocale::c().toString(linear_gain, 'f', 2).toLatin1() + '\n';
    if (!write_control_line(command)) {
        emit audio_control_failed(source, tr("Could not send the gain change to the recorder."));
        return;
    }
    append_diagnostic(tr("Audio gain requested for %1.").arg(source));
}

void ProcessRecorderBackend::set_audio_muted(const QString& source, bool muted) {
    if (!fluxrec::audio::validTrackId(source.toStdString())) {
        emit audio_control_failed(source, tr("Unknown audio source."));
        return;
    }
    if (!active_audio_enabled_ ||
        (state_ != State::recording && state_ != State::replay_only) ||
        process_.state() != QProcess::Running) {
        emit audio_control_failed(source, tr("Audio controls are unavailable in this session."));
        return;
    }
    const QByteArray command = QByteArrayLiteral("set_audio_mute ") + source.toUtf8() + ' ' +
        (muted ? QByteArrayLiteral("on") : QByteArrayLiteral("off")) + '\n';
    if (!write_control_line(command)) {
        emit audio_control_failed(source, tr("Could not send the mute change to the recorder."));
        return;
    }
    append_diagnostic(tr("Audio mute requested for %1.").arg(source));
}

void ProcessRecorderBackend::handle_started() {
    if (stop_requested_) {
        set_state(State::stopping);
        stop_timer_.start(graceful_stop_timeout_ms);
        send_sigint();
        return;
    }
    replay_active_ = false;
    replay_available_ = false;
    replay_reported_ = false;
    replay_control_pending_ = false;
    replay_control_timer_.stop();
    emit replay_buffer_changed(false);
    set_state(active_replay_only_ ? State::replay_only : State::recording);
    append_diagnostic(active_replay_only_
        ? tr("Replay-only recorder process started.")
        : tr("Recorder process started."));
}

void ProcessRecorderBackend::handle_finished(int exit_code, QProcess::ExitStatus exit_status) {
    read_stdout();
    read_stderr();
    preview_channel_->stop();
    if (!stderr_buffer_.trimmed().isEmpty()) {
        append_diagnostic(QString::fromUtf8(stderr_buffer_.trimmed()));
    }
    stderr_buffer_.clear();
    stop_timer_.stop();

    const bool completed_replay_only = active_replay_only_;
    const QFileInfo output_file(active_output_path_);
    const bool process_success = exit_status == QProcess::NormalExit && exit_code == 0 &&
        !forced_termination_;
    const bool success = process_success && (completed_replay_only ||
        (output_file.isFile() && output_file.size() > 0));
    if (!success) {
        last_error_ = process_success && !completed_replay_only
            ? tr("The recorder exited cleanly but no non-empty MKV file was found.")
            : process_error_summary(exit_code, exit_status);
        emit error_changed(last_error_);
        append_diagnostic(last_error_);
    } else {
        emit error_changed({});
        append_diagnostic(completed_replay_only
            ? tr("Replay-only session stopped cleanly; no primary recording file was written.")
            : tr("Recording finalized."));
    }

    replay_active_ = false;
    replay_available_ = false;
    replay_reported_ = false;
    replay_control_pending_ = false;
    replay_control_timer_.stop();
    active_audio_enabled_ = false;
    active_replay_only_ = false;
    emit replay_buffer_changed(false);
    executable_path_ = resolveExecutable(settings_.executable);
    set_state(executable_path_.isEmpty() ? State::unavailable
                                         : success ? State::available : State::failed);
    if (completed_replay_only) {
        emit replay_session_finished(success);
    } else {
        emit recording_finished(active_output_path_, success);
    }
    active_output_path_.clear();
    stop_requested_ = false;
    forced_termination_ = false;
}

void ProcessRecorderBackend::handle_process_error(QProcess::ProcessError error) {
    if (error == QProcess::FailedToStart) {
        preview_channel_->stop();
        const bool failed_replay_only = active_replay_only_;
        replay_available_ = false;
        replay_reported_ = false;
        replay_active_ = false;
        replay_control_pending_ = false;
        replay_control_timer_.stop();
        active_audio_enabled_ = false;
        active_replay_only_ = false;
        emit replay_buffer_changed(false);
        last_error_ = tr("Could not start the recorder. Check its path and executable permissions in Settings.");
        emit error_changed(last_error_);
        append_diagnostic(last_error_);
        set_state(State::failed);
        const QString failed_path = active_output_path_;
        active_output_path_.clear();
        stop_requested_ = false;
        if (failed_replay_only) {
            emit replay_session_finished(false);
        } else {
            emit recording_finished(failed_path, false);
        }
    } else if (error == QProcess::Crashed) {
        last_error_ = tr("The recorder process stopped unexpectedly. See the engine log for details.");
        emit error_changed(last_error_);
        set_state(State::failed);
    }
}

void ProcessRecorderBackend::read_stdout() {
    stdout_buffer_.append(process_.readAllStandardOutput());
    if (stdout_buffer_.size() > max_json_line_bytes && !stdout_buffer_.contains('\n')) {
        append_diagnostic(tr("Ignored an oversized telemetry line."));
        stdout_buffer_.clear();
        return;
    }
    parse_complete_lines();
}

void ProcessRecorderBackend::read_stderr() {
    stderr_buffer_.append(process_.readAllStandardError());
    if (stderr_buffer_.size() > max_stderr_bytes) {
        stderr_buffer_.remove(0, stderr_buffer_.size() - max_stderr_bytes);
    }
    while (true) {
        const qsizetype newline = stderr_buffer_.indexOf('\n');
        if (newline < 0) break;
        const QByteArray line = stderr_buffer_.left(newline).trimmed();
        stderr_buffer_.remove(0, newline + 1);
        if (!line.isEmpty()) append_diagnostic(QString::fromUtf8(line));
    }
}

void ProcessRecorderBackend::parse_complete_lines() {
    for (;;) {
        const qsizetype newline = stdout_buffer_.indexOf('\n');
        if (newline < 0) return;
        const QByteArray line = stdout_buffer_.left(newline).trimmed();
        stdout_buffer_.remove(0, newline + 1);
        if (line.size() > max_json_line_bytes) {
            append_diagnostic(tr("Ignored an oversized telemetry line."));
            continue;
        }
        if (!line.isEmpty()) handle_json_line(line);
    }
}

void ProcessRecorderBackend::handle_json_line(const QByteArray& line) {
    QJsonParseError parse_error;
    const QJsonDocument document = QJsonDocument::fromJson(line, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
        append_diagnostic(tr("Ignored malformed recorder output: %1")
                              .arg(QString::fromUtf8(line.left(240))));
        return;
    }

    const QJsonObject object = document.object();
    const QString type = object.value(QStringLiteral("type")).toString();
    if (type == QStringLiteral("error")) {
        const QString message = object.value(QStringLiteral("message")).toString();
        if (!message.isEmpty()) {
            last_error_ = message;
            emit error_changed(message);
        }
        append_diagnostic(QString::fromUtf8(line));
        return;
    }
    if (type == QStringLiteral("replay_saved")) {
        const QString path = object.value(QStringLiteral("path")).toString();
        const bool success = object.value(QStringLiteral("ok")).toBool(false);
        const QString message = object.value(QStringLiteral("message")).toString();
        if (success) {
            emit error_changed({});
            append_diagnostic(tr("Replay saved: %1").arg(path));
        } else {
            const QString diagnostic = message.isEmpty()
                ? tr("Replay save failed: %1").arg(path)
                : tr("Replay save failed: %1 · %2").arg(path, message);
            append_diagnostic(diagnostic);
            emit error_changed(diagnostic);
        }
        emit replay_saved(path, success);
        return;
    }
    if (type == QStringLiteral("replay_queued")) {
        const QString path = object.value(QStringLiteral("path")).toString();
        append_diagnostic(tr("Replay export queued: %1").arg(path));
        emit replay_save_queued(path);
        return;
    }
    if (type == QStringLiteral("replay_started") ||
        type == QStringLiteral("replay_stopped") ||
        type == QStringLiteral("replay_control_failed")) {
        const QString command = object.value(QStringLiteral("command")).toString();
        if (command != QStringLiteral("start_replay") &&
            command != QStringLiteral("stop_replay")) {
            append_diagnostic(tr("Ignored replay response with an unknown command."));
            return;
        }
        const bool success = object.value(QStringLiteral("ok")).toBool(false);
        if (object.value(QStringLiteral("active")).isBool()) {
            replay_active_ = object.value(QStringLiteral("active")).toBool();
            replay_reported_ = true;
        }
        replay_target_active_ = replay_active_;
        replay_control_pending_ = false;
        replay_control_timer_.stop();
        const QString message = object.value(QStringLiteral("message")).toString();
        if (success) {
            emit error_changed({});
            append_diagnostic(message.isEmpty()
                ? tr("Replay buffer %1.").arg(replay_active_ ? tr("started") : tr("stopped"))
                : tr("Replay control acknowledged: %1").arg(message));
        } else {
            const QString error = message.isEmpty()
                ? tr("The recorder rejected the replay control command.") : message;
            emit error_changed(error);
            append_diagnostic(tr("Replay control failed: %1").arg(error));
        }
        emit replay_buffer_changed(replay_active_);
        return;
    }
    if (type == QStringLiteral("audio_control_changed")) {
        const QString source = object.value(QStringLiteral("source")).toString();
        if (!fluxrec::audio::validTrackId(source.toStdString())) {
            append_diagnostic(tr("Ignored audio control acknowledgement for an unknown source."));
            return;
        }
        QJsonObject values;
        if (object.value(QStringLiteral("gain")).isDouble()) {
            values.insert(QStringLiteral("gain"), object.value(QStringLiteral("gain")));
        }
        if (object.value(QStringLiteral("muted")).isBool()) {
            values.insert(QStringLiteral("muted"), object.value(QStringLiteral("muted")));
        }
        if (values.isEmpty()) {
            append_diagnostic(tr("Ignored empty audio control acknowledgement."));
            return;
        }
        append_diagnostic(tr("Audio control acknowledged for %1.").arg(source));
        emit audio_control_changed(source, values);
        return;
    }
    if (type == QStringLiteral("audio_control_failed")) {
        const QString source = object.value(QStringLiteral("source")).toString();
        const QString message = object.value(QStringLiteral("message")).toString();
        const QString error = message.isEmpty()
            ? tr("The recorder rejected the audio control change.") : message;
        append_diagnostic(tr("Audio control failed for %1: %2").arg(source, error));
        emit audio_control_failed(source, error);
        return;
    }
    if (object.contains(QStringLiteral("frames")) &&
        object.value(QStringLiteral("frames")).isObject()) {
        const QJsonObject replay = object.value(QStringLiteral("replay")).toObject();
        const QJsonValue enabled = replay.value(QStringLiteral("enabled"));
        const QJsonValue active = replay.value(QStringLiteral("active"));
        const bool old_available = replay_available_;
        const bool old_reported = replay_reported_;
        const bool old_active = replay_active_;
        if (enabled.isBool()) replay_available_ = enabled.toBool();
        if (active.isBool()) {
            replay_active_ = active.toBool();
            replay_reported_ = true;
        }
        if (!replay_available_) replay_active_ = false;
        if (!replay_control_pending_) replay_target_active_ = replay_active_;
        if (old_available != replay_available_ || old_reported != replay_reported_ ||
            old_active != replay_active_) {
            emit replay_buffer_changed(replay_active_);
        }
        emit telemetry_received(TelemetrySnapshot{object, QDateTime::currentDateTime()});
        return;
    }
    append_diagnostic(QString::fromUtf8(line.left(240)));
}

void ProcessRecorderBackend::send_sigint() {
    const qint64 pid = process_.processId();
    if (pid <= 0) return;
    if (::kill(static_cast<pid_t>(pid), SIGINT) != 0 && errno != ESRCH) {
        last_error_ = tr("Could not signal the recorder to finish. The process will be allowed to time out.");
        emit error_changed(last_error_);
    }
}

void ProcessRecorderBackend::handle_stop_timeout() {
    if (process_.state() == QProcess::NotRunning) return;
    forced_termination_ = true;
    last_error_ = tr("The recorder did not finish within eight seconds; it was stopped forcefully.");
    emit error_changed(last_error_);
    append_diagnostic(last_error_);
    process_.kill();
}

void ProcessRecorderBackend::handle_replay_control_timeout() {
    if (!replay_control_pending_) return;
    replay_control_pending_ = false;
    replay_target_active_ = replay_active_;
    const QString message = tr("The recorder did not acknowledge the replay mode change.");
    emit error_changed(message);
    append_diagnostic(message);
    emit replay_buffer_changed(replay_active_);
}

bool ProcessRecorderBackend::write_control_line(const QByteArray& command) {
    return process_.state() == QProcess::Running && process_.write(command) == command.size();
}

void ProcessRecorderBackend::append_diagnostic(const QString& line) {
    if (!line.trimmed().isEmpty()) emit diagnostic_line(line);
}

QString ProcessRecorderBackend::process_error_summary(
    int exit_code, QProcess::ExitStatus exit_status) const {
    if (forced_termination_) return last_error_;
    if (!last_error_.isEmpty()) return last_error_;
    if (exit_status == QProcess::CrashExit) {
        return active_replay_only_
            ? tr("The replay-only recorder process crashed. See the engine log for details.")
            : tr("The recorder process crashed before the output file was finalized.");
    }
    if (exit_code != 0) {
        return tr("The recorder exited with code %1. See the engine log for details.").arg(exit_code);
    }
    return active_replay_only_
        ? tr("The replay-only recorder stopped without a clean exit.")
        : tr("The recorder stopped without confirming a completed output file.");
}

void ProcessRecorderBackend::set_state(State state) {
    if (state_ == state) return;
    state_ = state;
    emit state_changed(state_);
}

} // namespace fluxrec::ui
