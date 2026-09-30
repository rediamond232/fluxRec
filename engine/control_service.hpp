#pragma once

#include "engine/control_pipe.hpp"
#include "engine/replay_export.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unistd.h>

namespace fluxrec::engine {

enum class ControlServiceEventType : std::uint8_t {
    stop_requested,
    replay_started,
    replay_stopped,
    replay_control_failed,
    replay_queued,
    replay_succeeded,
    replay_failed,
    audio_control_changed,
    audio_control_failed,
};

struct ControlServiceEvent {
    ControlServiceEventType type{ControlServiceEventType::replay_failed};
    ControlCommandType command{ControlCommandType::save_replay};
    std::string output_path;
    std::uint64_t job_id{};
    ReplayExportSubmitStatus submit_status{ReplayExportSubmitStatus::queued};
    std::optional<replay::SnapshotStatus> snapshot_status;
    bool ok{};
    bool active{};
    bool state_changed{};
    std::size_t snapshot_packets{};
    std::uint64_t output_file_bytes{};
    std::string audio_track_id;
    std::optional<float> audio_gain;
    std::optional<bool> audio_muted;
    std::string message;
};

struct ControlServiceConfig {
    // Optional. The buffer must outlive ControlService. Replay commands are
    // accepted only when a buffer is configured.
    replay::ReplayBuffer* replay_buffer{};
    mux::MuxConfig mux_config_template;
    replay::Nanoseconds replay_duration{};

    int input_fd{STDIN_FILENO};
    std::chrono::milliseconds poll_interval{5};
    ReplayExporterConfig exporter_config{};
    // Invoked on the control worker thread. It must be short and should only
    // publish gain/mute changes to audio capture; it must not call close().
    std::function<void(const ControlCommand&)> audio_command_callback;
    // Invoked on the control worker thread. Keep the callback short; it must
    // not call close() on this service.
    std::function<void(const ControlServiceEvent&)> event_callback;
};

struct ControlServiceStats {
    ControlPipeStats input;
    std::uint64_t replay_jobs_submitted{};
    std::uint64_t replay_jobs_succeeded{};
    std::uint64_t replay_jobs_failed{};
    std::uint64_t replay_submit_retries{};
    std::uint64_t replay_start_commands{};
    std::uint64_t replay_stop_commands{};
    std::uint64_t replay_mode_errors{};
    std::uint64_t audio_control_commands{};
    std::uint64_t audio_control_errors{};
    std::size_t pending_saves{};
    bool running{};
    // True only when the independent stdin "stop" command was received.
    bool stop_requested{};
};

// Bridges stdin commands to replay and audio controls away from the
// capture/encode path. The reader queue and single pending-save barrier are
// bounded. A full ReplayExporter queue leaves that save queued for a later
// retry and preserves command ordering; it never makes capture wait for
// exporter capacity or I/O.
class ControlService final {
public:
    explicit ControlService(ControlServiceConfig config);
    ~ControlService();

    ControlService(const ControlService&) = delete;
    ControlService& operator=(const ControlService&) = delete;
    ControlService(ControlService&&) = delete;
    ControlService& operator=(ControlService&&) = delete;

    // Read by the engine control loop. This reflects the stdin stop command
    // and is independent of SIGINT/SIGTERM handling.
    [[nodiscard]] bool stop_requested() const noexcept;
    // A lock-free snapshot for the engine/UI control path.
    [[nodiscard]] ControlServiceStats stats() const noexcept;

    // Stops accepting stdin input, finishes all commands already accepted,
    // drains replay exports, and joins the service thread.
    void close() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace fluxrec::engine
