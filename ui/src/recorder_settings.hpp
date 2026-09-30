#pragma once

#include <QList>
#include <QString>
#include <QStringList>

namespace fluxrec::ui {

struct SceneProfile {
    QString id;
    QString name;
    QString output_name;

    friend bool operator==(const SceneProfile&, const SceneProfile&) = default;
};

struct AudioTrackSettings {
    QString id;
    QString name;
    QString input_kind;
    QString input_target;
    QString monitor_target;

    friend bool operator==(const AudioTrackSettings&, const AudioTrackSettings&) = default;
};

struct RecorderSettings {
    QString executable;
    QString output_directory;
    QString display_name = QStringLiteral("DP-1");
    QString codec = QStringLiteral("h264");
    QString encoder_backend = QStringLiteral("nvenc");
    QString nvenc_preset = QStringLiteral("p1");
    int nvenc_b_frames = 0;
    bool verify_frame_ids = false;
    int output_width = 0;
    int output_height = 0;
    bool allow_experimental_hevc = false;
    int max_fps = 0;
    QString rate_control = QStringLiteral("bitrate");
    int bitrate_mbps = 50;
    int qp = 28;
    int gop_size = 120;
    int pool_size = 48;
    int async_depth = 8;
    int nvenc_async_depth = 4;
    // Measured default for experimental-cuda-egl. Saved separately so editing
    // it does not replace the Vulkan or NVENC depths.
    int direct_async_depth = 12;
    int direct_pool_size = 16;
    int replay_history_seconds = 0;
    bool audio_enabled = false;
    bool audio_tracks_custom = false;
    QList<AudioTrackSettings> audio_tracks;
    QList<SceneProfile> scenes;
    QString selected_scene_id;
};

[[nodiscard]] RecorderSettings loadSettings();
void saveSettings(const RecorderSettings& settings);
void normalizeSceneProfiles(RecorderSettings& settings);
void normalizeEncoderSettings(RecorderSettings& settings);
[[nodiscard]] QList<AudioTrackSettings> effectiveAudioTracks(const RecorderSettings& settings);
void appendAudioTrackArguments(QStringList& arguments, const RecorderSettings& settings);
[[nodiscard]] QString defaultOutputDirectory();
[[nodiscard]] QString makeOutputPath(const RecorderSettings& settings);

} // namespace fluxrec::ui
