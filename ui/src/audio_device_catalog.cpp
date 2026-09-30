#include "audio_device_catalog.hpp"

#include <pipewire/pipewire.h>

#include <QSet>

#include <ctime>
#include <cstring>
#include <mutex>

namespace fluxrec::ui {

namespace {

struct EnumSession {
    pw_thread_loop* loop = nullptr;
    QList<AudioDeviceEntry> inputs;
    QList<AudioDeviceEntry> outputs;
    int sync_seq = 0;
    bool done = false;
};

[[nodiscard]] QString dictString(const spa_dict* props, const char* key) {
    if (props == nullptr) {
        return {};
    }
    const char* value = spa_dict_lookup(props, key);
    return value == nullptr ? QString{} : QString::fromUtf8(value);
}

void on_registry_global(void* data, std::uint32_t id, std::uint32_t /*permissions*/,
                        const char* type, std::uint32_t /*version*/,
                        const spa_dict* props) {
    auto* session = static_cast<EnumSession*>(data);
    if (type == nullptr || std::strcmp(type, PW_TYPE_INTERFACE_Node) != 0) {
        return;
    }

    const QString media_class = dictString(props, PW_KEY_MEDIA_CLASS);
    const QString node_name = dictString(props, PW_KEY_NODE_NAME);
    const QString node_description = dictString(props, PW_KEY_NODE_DESCRIPTION);
    QString serial = dictString(props, PW_KEY_OBJECT_SERIAL);
    if (serial.isEmpty()) {
        serial = QString::number(id);
    }

    const QString target = !node_name.isEmpty() ? node_name : serial;
    QString description = node_description;
    if (description.isEmpty()) {
        description = node_name;
    }
    if (description.isEmpty()) {
        description = serial;
    }

    if (isAudioSourceNode(media_class, node_name)) {
        session->inputs.push_back(
            {target, description, QStringLiteral("source")});
    }
    if (isAudioSinkNode(media_class)) {
        session->inputs.push_back(
            {target, description, QStringLiteral("sink-monitor")});
        session->outputs.push_back({target, description, QString{}});
    }
}

void on_core_done(void* data, std::uint32_t id, int seq) {
    auto* session = static_cast<EnumSession*>(data);
    if (id == PW_ID_CORE && seq == session->sync_seq) {
        session->done = true;
        pw_thread_loop_signal(session->loop, false);
    }
}

} // namespace

bool isAudioSourceNode(QStringView media_class, QStringView node_name) {
    return media_class == QLatin1String("Audio/Source") &&
           !node_name.endsWith(QLatin1String(".monitor"));
}

bool isAudioSinkNode(QStringView media_class) {
    return media_class == QLatin1String("Audio/Sink");
}

namespace {

[[nodiscard]] QString inputLabel(const AudioDeviceEntry& entry) {
    return entry.description.isEmpty() ? entry.target : entry.description;
}

} // namespace

QList<AudioDeviceEntry> dedupeAudioInputs(QList<AudioDeviceEntry> inputs) {
    QSet<QString> source_labels;
    for (const AudioDeviceEntry& entry : inputs) {
        if (entry.input_kind == QLatin1String("source")) {
            source_labels.insert(inputLabel(entry));
        }
    }
    QList<AudioDeviceEntry> unique;
    unique.reserve(inputs.size());
    for (const AudioDeviceEntry& entry : inputs) {
        if (entry.input_kind == QLatin1String("sink-monitor") &&
            source_labels.contains(inputLabel(entry))) {
            continue;
        }
        unique.push_back(entry);
    }
    return unique;
}

bool AudioDeviceCatalog::refresh() {
    EnumSession session;

    static std::once_flag pipewire_initialized;
    std::call_once(pipewire_initialized, [] { pw_init(nullptr, nullptr); });

    session.loop = pw_thread_loop_new("fluxrec-audio-devices", nullptr);
    if (session.loop == nullptr) {
        error_ = QStringLiteral("Audio devices are unavailable.");
        return false;
    }

    pw_context* context =
        pw_context_new(pw_thread_loop_get_loop(session.loop), nullptr, 0);
    if (context == nullptr) {
        pw_thread_loop_destroy(session.loop);
        error_ = QStringLiteral("Audio devices are unavailable.");
        return false;
    }

    pw_core* core = pw_context_connect(context, nullptr, 0);
    if (core == nullptr) {
        pw_context_destroy(context);
        pw_thread_loop_destroy(session.loop);
        error_ = QStringLiteral("Audio devices are unavailable.");
        return false;
    }

    pw_registry* registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
    if (registry == nullptr) {
        pw_core_disconnect(core);
        pw_context_destroy(context);
        pw_thread_loop_destroy(session.loop);
        error_ = QStringLiteral("Audio devices are unavailable.");
        return false;
    }

    spa_hook registry_hook{};
    spa_hook core_hook{};
    static const pw_registry_events registry_events = [] {
        pw_registry_events events{};
        events.version = PW_VERSION_REGISTRY_EVENTS;
        events.global = on_registry_global;
        return events;
    }();
    static const pw_core_events core_events = [] {
        pw_core_events events{};
        events.version = PW_VERSION_CORE_EVENTS;
        events.done = on_core_done;
        return events;
    }();

    pw_registry_add_listener(registry, &registry_hook, &registry_events, &session);
    pw_core_add_listener(core, &core_hook, &core_events, &session);

    if (pw_thread_loop_start(session.loop) < 0) {
        spa_hook_remove(&registry_hook);
        spa_hook_remove(&core_hook);
        pw_proxy_destroy(reinterpret_cast<pw_proxy*>(registry));
        pw_core_disconnect(core);
        pw_context_destroy(context);
        pw_thread_loop_destroy(session.loop);
        error_ = QStringLiteral("Audio devices are unavailable.");
        return false;
    }

    pw_thread_loop_lock(session.loop);
    session.sync_seq = static_cast<int>(pw_core_sync(core, PW_ID_CORE, 0));
    struct timespec deadline{};
    pw_thread_loop_get_time(session.loop, &deadline, 2'000'000'000LL);
    while (!session.done) {
        if (pw_thread_loop_timed_wait_full(session.loop, &deadline) != 0) {
            break;
        }
    }

    spa_hook_remove(&registry_hook);
    spa_hook_remove(&core_hook);
    pw_proxy_destroy(reinterpret_cast<pw_proxy*>(registry));
    pw_core_disconnect(core);
    pw_context_destroy(context);
    pw_thread_loop_unlock(session.loop);
    pw_thread_loop_stop(session.loop);
    pw_thread_loop_destroy(session.loop);

    if (!session.done) {
        error_ = QStringLiteral("Audio devices are unavailable.");
        return false;
    }

    inputs_ = dedupeAudioInputs(std::move(session.inputs));
    outputs_ = std::move(session.outputs);
    error_.clear();
    return true;
}

QString AudioDeviceCatalog::error() const {
    return error_;
}

QList<AudioDeviceEntry> AudioDeviceCatalog::inputs() const {
    return inputs_;
}

QList<AudioDeviceEntry> AudioDeviceCatalog::outputs() const {
    return outputs_;
}

bool AudioDeviceCatalog::available() const {
    return error_.isEmpty();
}

void AudioDeviceCatalog::assign(QList<AudioDeviceEntry> inputs,
                                QList<AudioDeviceEntry> outputs,
                                QString error) {
    inputs_ = std::move(inputs);
    outputs_ = std::move(outputs);
    error_ = std::move(error);
}

} // namespace fluxrec::ui
