#pragma once

#include "audio_device_catalog.hpp"
#include "recorder_settings.hpp"

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QWidget>

#include <functional>
#include <vector>

class QLabel;
class QPushButton;
class QVBoxLayout;

namespace fluxrec::ui {

class AudioTrackRow;

class AudioMixerPanel final : public QWidget {
    Q_OBJECT

public:
    explicit AudioMixerPanel(QWidget* parent = nullptr);

    void setAudioConfigured(bool enabled);
    void setRecordingActive(bool active);
    void setReplayOnlySession(bool active, bool audio_enabled);
    void beginSession();
    void setSnapshot(const QJsonObject& audio, const QDateTime& received_at);
    void refreshFreshness();
    void applyControlChanged(const QString& source, const QJsonObject& values);
    void applyControlFailed(const QString& source, const QString& message);
    void setControlHandlers(std::function<void(const QString&, double)> volume_handler,
                            std::function<void(const QString&, bool)> mute_handler);
    void setTracks(QList<AudioTrackSettings> tracks, bool custom);
    void setDevices(const AudioDeviceCatalog& catalog);
    void setSessionBusy(bool busy);

signals:
    void tracksEdited(QList<AudioTrackSettings> tracks);

private:
    void render();
    void rebuildRows();
    void refreshDeviceCombos();
    void updateEditEnabled();
    void emitTracksEdited();
    void appendTrack();
    void removeTrack(const QString& id);
    void onRowEdited(const QString& id, AudioTrackSettings track);
    [[nodiscard]] AudioTrackRow* rowForSource(const QString& source) const;
    void wireRow(AudioTrackRow* row);

    QLabel* empty_state_ = nullptr;
    QLabel* devices_error_label_ = nullptr;
    QLabel* freshness_label_ = nullptr;
    QLabel* summary_label_ = nullptr;
    QVBoxLayout* rows_layout_ = nullptr;
    QPushButton* add_button_ = nullptr;
    std::vector<AudioTrackRow*> rows_;
    QList<AudioTrackSettings> tracks_;
    QList<AudioDeviceEntry> inputs_;
    QList<AudioDeviceEntry> outputs_;
    QString devices_error_;
    QDateTime received_at_;
    bool configured_ = false;
    bool recording_active_ = false;
    bool replay_only_session_ = false;
    bool replay_only_audio_enabled_ = false;
    bool has_snapshot_ = false;
    bool telemetry_enabled_known_ = false;
    bool telemetry_enabled_ = false;
    bool session_busy_ = false;
    bool custom_ = false;
    QJsonObject audio_snapshot_;
    std::function<void(const QString&, double)> volume_handler_;
    std::function<void(const QString&, bool)> mute_handler_;
};

} // namespace fluxrec::ui
