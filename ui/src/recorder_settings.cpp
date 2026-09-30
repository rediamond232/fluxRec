#include "recorder_settings.hpp"

#include "audio/track_config.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QSet>
#include <QStringList>
#include <QStandardPaths>
#include <QUuid>

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

namespace fluxrec::ui {
namespace {

QString discoveredRecorder() {
    const QString self = QCoreApplication::applicationFilePath();
    const QFileInfo info(self);
    if (!self.isEmpty() && info.exists() && info.isFile() && info.isExecutable()) {
        return info.canonicalFilePath().isEmpty() ? info.absoluteFilePath()
                                                  : info.canonicalFilePath();
    }
    return QStandardPaths::findExecutable(QStringLiteral("fluxrec"));
}

QString uniqueSceneName(const QSet<QString>& used_names, int preferred_number) {
    int number = preferred_number;
    QString candidate;
    do {
        candidate = QStringLiteral("Scene %1").arg(number++);
    } while (used_names.contains(candidate.toCaseFolded()));
    return candidate;
}

[[nodiscard]] QString inputKindToSettings(fluxrec::audio::InputKind kind) {
    return kind == fluxrec::audio::InputKind::sink_monitor
        ? QStringLiteral("sink-monitor")
        : QStringLiteral("source");
}

[[nodiscard]] std::optional<fluxrec::audio::InputKind> inputKindFromSettings(const QString& kind) {
    if (kind == QStringLiteral("source")) return fluxrec::audio::InputKind::source;
    if (kind == QStringLiteral("sink-monitor") || kind == QStringLiteral("sink_monitor")) {
        return fluxrec::audio::InputKind::sink_monitor;
    }
    return std::nullopt;
}

[[nodiscard]] AudioTrackSettings toSettingsTrack(const fluxrec::audio::AudioTrack& track) {
    return {
        QString::fromStdString(track.id),
        QString::fromStdString(track.name),
        inputKindToSettings(track.input_kind),
        QString::fromStdString(track.input_target),
        QString::fromStdString(track.monitor_target),
    };
}

[[nodiscard]] std::optional<std::vector<fluxrec::audio::AudioTrack>> toAudioTracks(
    const QList<AudioTrackSettings>& tracks) {
    std::vector<fluxrec::audio::AudioTrack> converted;
    converted.reserve(static_cast<std::size_t>(tracks.size()));
    for (const AudioTrackSettings& track : tracks) {
        const auto kind = inputKindFromSettings(track.input_kind);
        if (!kind) return std::nullopt;
        converted.push_back({
            track.id.toStdString(),
            track.name.toStdString(),
            *kind,
            track.input_target.toStdString(),
            track.monitor_target.toStdString(),
        });
    }
    return converted;
}

[[nodiscard]] bool audioTracksAreValid(const QList<AudioTrackSettings>& tracks) {
    const auto converted = toAudioTracks(tracks);
    if (!converted) return false;
    return static_cast<bool>(fluxrec::audio::validateAudioTracks(*converted));
}

[[nodiscard]] QString argumentTarget(const QString& target) {
    return target.isEmpty() ? QStringLiteral("-") : target;
}

} // namespace

void normalizeSceneProfiles(RecorderSettings& settings) {
    QString fallback_output = settings.display_name.trimmed();
    if (fallback_output.isEmpty()) fallback_output = QStringLiteral("DP-1");

    if (settings.scenes.isEmpty()) {
        settings.scenes.push_back({QStringLiteral("scene-default"),
                                   QStringLiteral("Scene 1"), fallback_output});
    }

    QSet<QString> ids;
    QSet<QString> names;
    for (qsizetype index = 0; index < settings.scenes.size(); ++index) {
        SceneProfile& scene = settings.scenes[index];
        scene.id = scene.id.trimmed();
        if (scene.id.isEmpty() || ids.contains(scene.id)) {
            scene.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        }
        ids.insert(scene.id);

        scene.name = scene.name.trimmed();
        if (scene.name.isEmpty() || names.contains(scene.name.toCaseFolded())) {
            scene.name = uniqueSceneName(names, static_cast<int>(index) + 1);
        }
        names.insert(scene.name.toCaseFolded());

        scene.output_name = scene.output_name.trimmed();
        if (scene.output_name.isEmpty()) scene.output_name = fallback_output;
    }

    const auto selected = std::find_if(settings.scenes.cbegin(), settings.scenes.cend(),
        [&settings](const SceneProfile& scene) {
            return scene.id == settings.selected_scene_id;
        });
    if (selected == settings.scenes.cend()) {
        settings.selected_scene_id = settings.scenes.first().id;
    }
    const auto active = std::find_if(settings.scenes.cbegin(), settings.scenes.cend(),
        [&settings](const SceneProfile& scene) {
            return scene.id == settings.selected_scene_id;
        });
    settings.display_name = active->output_name;
}

void normalizeEncoderSettings(RecorderSettings& settings) {
    if (settings.encoder_backend != QStringLiteral("vulkan") &&
        settings.encoder_backend != QStringLiteral("nvenc") &&
        settings.encoder_backend != QStringLiteral("experimental-cuda-egl")) {
        settings.encoder_backend = QStringLiteral("nvenc");
    }

    const QStringList nvenc_presets{
        QStringLiteral("p1"), QStringLiteral("p2"), QStringLiteral("p3"),
        QStringLiteral("p4"), QStringLiteral("p5"), QStringLiteral("p6"),
        QStringLiteral("p7"),
    };
    if (!nvenc_presets.contains(settings.nvenc_preset)) {
        settings.nvenc_preset = QStringLiteral("p1");
    }

    settings.nvenc_b_frames = qBound(0, settings.nvenc_b_frames, 4);
    settings.async_depth = qBound(1, settings.async_depth, 64);
    settings.nvenc_async_depth = qBound(1, settings.nvenc_async_depth, 64);
    settings.direct_async_depth = qBound(1, settings.direct_async_depth, 16);
    settings.direct_pool_size = qBound(3, settings.direct_pool_size, 16);
}

QString defaultOutputDirectory() {
    QString directory = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    if (directory.isEmpty()) {
        directory = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    }
    if (directory.isEmpty()) {
        directory = QDir::homePath();
    }
    return QDir(directory).filePath(QStringLiteral("Fluxrec"));
}

RecorderSettings loadSettings() {
    QSettings settings;
    RecorderSettings result;
    result.executable = settings.value(QStringLiteral("recorder/executable"),
                                       discoveredRecorder()).toString();
    result.output_directory = settings.value(QStringLiteral("recording/outputDirectory"),
                                             defaultOutputDirectory()).toString();
    result.display_name = settings.value(QStringLiteral("capture/displayName"),
                                         result.display_name).toString();
    result.encoder_backend = settings.value(QStringLiteral("recording/encoderBackend"),
                                            result.encoder_backend).toString();
    result.nvenc_preset = settings.value(QStringLiteral("recording/nvencPreset"),
                                         result.nvenc_preset).toString();
    result.nvenc_b_frames = settings.value(QStringLiteral("recording/nvencBFrames"),
                                           result.nvenc_b_frames).toInt();
    result.verify_frame_ids = settings.value(QStringLiteral("recording/verifyFrameIds"),
                                              result.verify_frame_ids).toBool();
    result.codec = settings.value(QStringLiteral("recording/codec"), result.codec).toString();
    result.output_width = settings.value(QStringLiteral("recording/outputWidth"),
                                        result.output_width).toInt();
    result.output_height = settings.value(QStringLiteral("recording/outputHeight"),
                                         result.output_height).toInt();
    result.allow_experimental_hevc = settings.value(
        QStringLiteral("recording/allowExperimentalHevc"),
        result.allow_experimental_hevc).toBool();
    result.max_fps = settings.value(QStringLiteral("capture/maxFps"), result.max_fps).toInt();
    result.rate_control = settings.value(QStringLiteral("recording/rateControl"),
                                         result.rate_control).toString();
    if (result.rate_control != QStringLiteral("bitrate") &&
        result.rate_control != QStringLiteral("constant_qp")) {
        result.rate_control = QStringLiteral("bitrate");
    }
    result.bitrate_mbps = settings.value(QStringLiteral("recording/bitrateMbps"),
                                         result.bitrate_mbps).toInt();
    result.qp = settings.value(QStringLiteral("recording/qp"), result.qp).toInt();
    result.qp = qBound(0, result.qp, 51);
    result.gop_size = settings.value(QStringLiteral("recording/gopSize"),
                                     result.gop_size).toInt();
    result.pool_size = settings.value(QStringLiteral("capture/poolSize"), result.pool_size).toInt();
    result.async_depth = settings.value(QStringLiteral("recording/asyncDepth"),
                                        result.async_depth).toInt();
    result.nvenc_async_depth = settings.value(QStringLiteral("recording/nvencAsyncDepth"),
                                              result.nvenc_async_depth).toInt();
    result.direct_async_depth = settings.value(QStringLiteral("recording/directAsyncDepth"),
                                               result.direct_async_depth).toInt();
    result.direct_pool_size = settings.value(QStringLiteral("capture/directPoolSize"),
                                             result.direct_pool_size).toInt();
    result.replay_history_seconds = settings.value(QStringLiteral("replay/historySeconds"),
                                                   result.replay_history_seconds).toInt();
    result.audio_enabled = settings.value(QStringLiteral("audio/enabled"),
                                         result.audio_enabled).toBool();
    result.audio_tracks_custom = settings.value(QStringLiteral("audio/tracksCustom"),
                                                false).toBool();
    const int track_count = settings.beginReadArray(QStringLiteral("audio/tracks"));
    for (int index = 0; index < track_count; ++index) {
        settings.setArrayIndex(index);
        result.audio_tracks.push_back({
            settings.value(QStringLiteral("id")).toString(),
            settings.value(QStringLiteral("name")).toString(),
            settings.value(QStringLiteral("inputKind")).toString(),
            settings.value(QStringLiteral("inputTarget")).toString(),
            settings.value(QStringLiteral("monitorTarget")).toString(),
        });
    }
    settings.endArray();
    if (result.audio_tracks_custom && !audioTracksAreValid(result.audio_tracks)) {
        result.audio_tracks_custom = false;
        result.audio_tracks.clear();
    }
    const int scene_count = settings.beginReadArray(QStringLiteral("scenes/profiles"));
    for (int index = 0; index < scene_count; ++index) {
        settings.setArrayIndex(index);
        result.scenes.push_back({
            settings.value(QStringLiteral("id")).toString(),
            settings.value(QStringLiteral("name")).toString(),
            settings.value(QStringLiteral("outputName")).toString(),
        });
    }
    settings.endArray();
    result.selected_scene_id = settings.value(QStringLiteral("scenes/selectedId")).toString();
    normalizeSceneProfiles(result);
    normalizeEncoderSettings(result);
    return result;
}

void saveSettings(const RecorderSettings& settings) {
    RecorderSettings normalized = settings;
    normalizeSceneProfiles(normalized);
    normalizeEncoderSettings(normalized);
    QSettings values;
    values.setValue(QStringLiteral("recorder/executable"), normalized.executable);
    values.setValue(QStringLiteral("recording/outputDirectory"), normalized.output_directory);
    values.setValue(QStringLiteral("capture/displayName"), normalized.display_name);
    values.setValue(QStringLiteral("recording/encoderBackend"), normalized.encoder_backend);
    values.setValue(QStringLiteral("recording/nvencPreset"), normalized.nvenc_preset);
    values.setValue(QStringLiteral("recording/nvencBFrames"), normalized.nvenc_b_frames);
    values.setValue(QStringLiteral("recording/verifyFrameIds"), normalized.verify_frame_ids);
    values.setValue(QStringLiteral("recording/codec"), normalized.codec);
    values.setValue(QStringLiteral("recording/outputWidth"), normalized.output_width);
    values.setValue(QStringLiteral("recording/outputHeight"), normalized.output_height);
    values.setValue(QStringLiteral("recording/allowExperimentalHevc"),
                    normalized.allow_experimental_hevc);
    values.setValue(QStringLiteral("capture/maxFps"), normalized.max_fps);
    values.setValue(QStringLiteral("recording/rateControl"), normalized.rate_control);
    values.setValue(QStringLiteral("recording/bitrateMbps"), normalized.bitrate_mbps);
    values.setValue(QStringLiteral("recording/qp"), normalized.qp);
    values.setValue(QStringLiteral("recording/gopSize"), normalized.gop_size);
    values.setValue(QStringLiteral("capture/poolSize"), normalized.pool_size);
    values.setValue(QStringLiteral("recording/asyncDepth"), normalized.async_depth);
    values.setValue(QStringLiteral("recording/nvencAsyncDepth"), normalized.nvenc_async_depth);
    values.setValue(QStringLiteral("recording/directAsyncDepth"), normalized.direct_async_depth);
    values.setValue(QStringLiteral("capture/directPoolSize"), normalized.direct_pool_size);
    values.setValue(QStringLiteral("replay/historySeconds"), normalized.replay_history_seconds);
    values.setValue(QStringLiteral("audio/enabled"), normalized.audio_enabled);
    values.setValue(QStringLiteral("audio/tracksCustom"), normalized.audio_tracks_custom);
    values.beginWriteArray(QStringLiteral("audio/tracks"), normalized.audio_tracks.size());
    for (qsizetype index = 0; index < normalized.audio_tracks.size(); ++index) {
        values.setArrayIndex(static_cast<int>(index));
        const AudioTrackSettings& track = normalized.audio_tracks[index];
        values.setValue(QStringLiteral("id"), track.id);
        values.setValue(QStringLiteral("name"), track.name);
        values.setValue(QStringLiteral("inputKind"), track.input_kind);
        values.setValue(QStringLiteral("inputTarget"), track.input_target);
        values.setValue(QStringLiteral("monitorTarget"), track.monitor_target);
    }
    values.endArray();
    values.setValue(QStringLiteral("scenes/selectedId"), normalized.selected_scene_id);
    values.beginWriteArray(QStringLiteral("scenes/profiles"), normalized.scenes.size());
    for (qsizetype index = 0; index < normalized.scenes.size(); ++index) {
        values.setArrayIndex(static_cast<int>(index));
        const SceneProfile& scene = normalized.scenes[index];
        values.setValue(QStringLiteral("id"), scene.id);
        values.setValue(QStringLiteral("name"), scene.name);
        values.setValue(QStringLiteral("outputName"), scene.output_name);
    }
    values.endArray();
    values.sync();
}

QList<AudioTrackSettings> effectiveAudioTracks(const RecorderSettings& settings) {
    if (settings.audio_tracks_custom) {
        return settings.audio_tracks;
    }
    QList<AudioTrackSettings> tracks;
    for (const fluxrec::audio::AudioTrack& track : fluxrec::audio::defaultAudioTracks()) {
        tracks.push_back(toSettingsTrack(track));
    }
    return tracks;
}

void appendAudioTrackArguments(QStringList& arguments, const RecorderSettings& settings) {
    arguments << QStringLiteral("--audio")
              << (settings.audio_enabled ? QStringLiteral("on") : QStringLiteral("off"));
    if (!settings.audio_enabled) return;
    if (!settings.audio_tracks_custom) {
        arguments << QStringLiteral("--audio-tracks") << QStringLiteral("default");
        return;
    }
    for (const AudioTrackSettings& track : settings.audio_tracks) {
        const QString kind = track.input_kind == QStringLiteral("sink_monitor")
            ? QStringLiteral("sink-monitor")
            : track.input_kind;
        arguments << QStringLiteral("--audio-track")
                  << track.id
                  << track.name
                  << kind
                  << argumentTarget(track.input_target)
                  << argumentTarget(track.monitor_target);
    }
}

QString makeOutputPath(const RecorderSettings& settings) {
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"));
    const QString base = QStringLiteral("Fluxrec_%1").arg(stamp);
    QDir directory(settings.output_directory);
    QString candidate = directory.absoluteFilePath(base + QStringLiteral(".mkv"));
    for (int suffix = 2; QFileInfo::exists(candidate); ++suffix) {
        candidate = directory.absoluteFilePath(QStringLiteral("%1_%2.mkv")
                                                   .arg(base)
                                                   .arg(suffix, 2, 10, QLatin1Char('0')));
    }
    return candidate;
}

} // namespace fluxrec::ui
