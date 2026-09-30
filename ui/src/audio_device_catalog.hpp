#pragma once

#include <QList>
#include <QString>
#include <QStringView>

namespace fluxrec::ui {

struct AudioDeviceEntry {
    QString target;
    QString description;
    QString input_kind;
};

[[nodiscard]] bool isAudioSourceNode(QStringView media_class, QStringView node_name);
[[nodiscard]] bool isAudioSinkNode(QStringView media_class);
// Drops a sink monitor from the input list when a source already uses the same
// display name. The sink remains available as a monitor output.
[[nodiscard]] QList<AudioDeviceEntry> dedupeAudioInputs(QList<AudioDeviceEntry> inputs);

class AudioDeviceCatalog {
public:
    [[nodiscard]] bool refresh();
    [[nodiscard]] QString error() const;
    [[nodiscard]] QList<AudioDeviceEntry> inputs() const;
    [[nodiscard]] QList<AudioDeviceEntry> outputs() const;
    [[nodiscard]] bool available() const;
    void assign(QList<AudioDeviceEntry> inputs, QList<AudioDeviceEntry> outputs, QString error);

private:
    QList<AudioDeviceEntry> inputs_;
    QList<AudioDeviceEntry> outputs_;
    QString error_;
};

} // namespace fluxrec::ui
