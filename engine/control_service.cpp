#include "engine/control_service.hpp"

#include <array>
#include <atomic>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace fluxrec::engine {

struct ControlService::Impl {
    explicit Impl(ControlServiceConfig value) : config(std::move(value)) {
        if (config.replay_buffer == nullptr && !config.audio_command_callback) {
            throw std::invalid_argument("control service requires replay or audio controls");
        }
        if (config.replay_buffer != nullptr &&
            config.replay_duration <= replay::Nanoseconds::zero()) {
            throw std::invalid_argument("control service replay duration must be positive");
        }
        if (config.poll_interval <= std::chrono::milliseconds::zero()) {
            throw std::invalid_argument("control service poll interval must be positive");
        }

        input = std::make_unique<ControlPipe>(config.input_fd);
        if (config.replay_buffer != nullptr) {
            exporter = std::make_unique<ReplayExporter>(
                *config.replay_buffer, config.exporter_config);
        }
        running.store(true, std::memory_order_release);
        try {
            worker = std::thread([this] { run(); });
        } catch (...) {
            running.store(false, std::memory_order_release);
            throw;
        }
    }

    ~Impl() { close(); }

    ControlServiceStats stats() const noexcept {
        const auto input_stats = input->stats();
        return {
            .input = input_stats,
            .replay_jobs_submitted = replay_jobs_submitted.load(std::memory_order_relaxed),
            .replay_jobs_succeeded = replay_jobs_succeeded.load(std::memory_order_relaxed),
            .replay_jobs_failed = replay_jobs_failed.load(std::memory_order_relaxed),
            .replay_submit_retries = replay_submit_retries.load(std::memory_order_relaxed),
            .replay_start_commands = replay_start_commands.load(std::memory_order_relaxed),
            .replay_stop_commands = replay_stop_commands.load(std::memory_order_relaxed),
            .replay_mode_errors = replay_mode_errors.load(std::memory_order_relaxed),
            .audio_control_commands = audio_control_commands.load(std::memory_order_relaxed),
            .audio_control_errors = audio_control_errors.load(std::memory_order_relaxed),
            .pending_saves = pending_save_count.load(std::memory_order_acquire),
            .running = running.load(std::memory_order_acquire),
            .stop_requested = stop_requested.load(std::memory_order_acquire),
        };
    }

    bool stop_requested_status() const noexcept {
        return stop_requested.load(std::memory_order_acquire);
    }

    void close() noexcept {
        std::lock_guard lock(close_mutex);
        if (closed) {
            return;
        }

        close_requested.store(true, std::memory_order_release);
        // This joins only the poll-based input reader. The service worker keeps
        // draining commands and replay jobs while input is being closed.
        input->close();
        input_closed.store(true, std::memory_order_release);
        if (worker.joinable() && worker.get_id() == std::this_thread::get_id()) {
            // The callback contract forbids close() on this thread, but keep a
            // mistaken call from self-joining. A later owner-thread close can
            // still join after the worker exits.
            return;
        }
        if (worker.joinable()) {
            worker.join();
        }
        closed = true;
    }

private:
    void emit(ControlServiceEventType type, const std::string& path = {},
              std::uint64_t job_id = 0,
              ReplayExportSubmitStatus submit_status = ReplayExportSubmitStatus::queued,
              std::size_t snapshot_packets = 0,
              std::uint64_t output_file_bytes = 0,
              const char* message = nullptr,
              std::optional<replay::SnapshotStatus> snapshot_status = std::nullopt) noexcept {
        if (!config.event_callback) {
            return;
        }
        try {
            ControlServiceEvent event;
            event.type = type;
            event.command = type == ControlServiceEventType::stop_requested
                ? ControlCommandType::stop : ControlCommandType::save_replay;
            event.output_path = path;
            event.job_id = job_id;
            event.submit_status = submit_status;
            event.snapshot_status = snapshot_status;
            event.ok = type != ControlServiceEventType::replay_failed &&
                       type != ControlServiceEventType::replay_control_failed &&
                       type != ControlServiceEventType::audio_control_failed;
            event.snapshot_packets = snapshot_packets;
            event.output_file_bytes = output_file_bytes;
            if (message != nullptr) {
                event.message = message;
            }
            config.event_callback(event);
        } catch (...) {
            // The observer is optional and cannot be allowed to terminate the
            // service or interfere with the capture/encode thread.
        }
    }

    void emit_replay_mode(ControlCommandType command, bool ok, bool active,
                          bool state_changed, const char* message = nullptr) noexcept {
        if (!config.event_callback) {
            return;
        }
        try {
            ControlServiceEvent event;
            event.command = command;
            event.ok = ok;
            event.active = active;
            event.state_changed = state_changed;
            event.type = !ok ? ControlServiceEventType::replay_control_failed
                : (command == ControlCommandType::start_replay
                    ? ControlServiceEventType::replay_started
                    : ControlServiceEventType::replay_stopped);
            if (message != nullptr) {
                event.message = message;
            }
            config.event_callback(event);
        } catch (...) {
            // A UI observer cannot be allowed to interrupt replay control.
        }
    }

    void emit_audio_control(const ControlCommand& command, bool ok,
                            const char* message = nullptr) noexcept {
        if (!config.event_callback) {
            return;
        }
        try {
            ControlServiceEvent event;
            event.type = ok ? ControlServiceEventType::audio_control_changed
                            : ControlServiceEventType::audio_control_failed;
            event.command = command.type;
            event.ok = ok;
            event.audio_track_id = command.audio_track_id;
            if (command.type == ControlCommandType::set_audio_volume) {
                event.audio_gain = command.audio_gain;
            } else if (command.type == ControlCommandType::set_audio_mute) {
                event.audio_muted = command.audio_muted;
            }
            if (message != nullptr) {
                event.message = message;
            }
            config.event_callback(event);
        } catch (...) {
            // A UI observer cannot interrupt or delay the audio-control path.
        }
    }

    void apply_audio_control(const ControlCommand& command) noexcept {
        audio_control_commands.fetch_add(1, std::memory_order_relaxed);
        if (!config.audio_command_callback) {
            audio_control_errors.fetch_add(1, std::memory_order_relaxed);
            emit_audio_control(command, false, "audio capture is disabled");
            return;
        }
        try {
            config.audio_command_callback(command);
            emit_audio_control(command, true);
        } catch (const std::exception& exception) {
            audio_control_errors.fetch_add(1, std::memory_order_relaxed);
            emit_audio_control(command, false, exception.what());
        } catch (...) {
            audio_control_errors.fetch_add(1, std::memory_order_relaxed);
            emit_audio_control(command, false, "could not apply audio control");
        }
    }

    void observe_stop_command() noexcept {
        if (input->stats().stop_command_seen &&
            !stop_requested.exchange(true, std::memory_order_acq_rel)) {
            emit(ControlServiceEventType::stop_requested);
        }
    }

    void collect_commands() noexcept {
        // Do not process later mode changes while a save is waiting for
        // exporter capacity. That preserves command order, so start/stop
        // cannot clear or freeze the ring ahead of an earlier save request.
        if (pending_size != 0) {
            return;
        }

        ControlCommand command;
        while (input->try_pop(command)) {
            if (command.type == ControlCommandType::stop) {
                // stop_requested is published by observe_stop_command() as
                // soon as the input worker accepts the command. Consume its
                // queue entry here without coupling it to SIGINT handling.
                command.path.clear();
                continue;
            }

            if (command.type == ControlCommandType::start_replay ||
                command.type == ControlCommandType::stop_replay) {
                change_replay_mode(command.type);
                continue;
            }

            if (command.type == ControlCommandType::set_audio_volume ||
                command.type == ControlCommandType::set_audio_mute) {
                apply_audio_control(command);
                continue;
            }

            if (command.type == ControlCommandType::save_replay &&
                (config.replay_buffer == nullptr || exporter == nullptr)) {
                replay_jobs_failed.fetch_add(1, std::memory_order_relaxed);
                emit(ControlServiceEventType::replay_failed, command.path, 0,
                     ReplayExportSubmitStatus::invalid_config, 0, 0,
                     "replay buffer is not enabled");
                continue;
            }

            const auto index = (pending_head + pending_size) % pending_paths.size();
            pending_paths[index] = std::move(command.path);
            ++pending_size;
            pending_save_count.store(pending_size, std::memory_order_release);
            // submit_pending_front runs on the next worker iteration. Leave
            // following commands in ControlPipe until this save is accepted
            // or rejected, preserving the input stream's ordering.
            return;
        }
    }

    void change_replay_mode(ControlCommandType command) noexcept {
        const bool starting = command == ControlCommandType::start_replay;
        if (starting) {
            replay_start_commands.fetch_add(1, std::memory_order_relaxed);
        } else {
            replay_stop_commands.fetch_add(1, std::memory_order_relaxed);
        }

        if (config.replay_buffer == nullptr) {
            replay_mode_errors.fetch_add(1, std::memory_order_relaxed);
            emit_replay_mode(command, false, false, false,
                             "replay buffer is not enabled");
            return;
        }

        try {
            const bool changed = starting
                ? config.replay_buffer->start()
                : config.replay_buffer->stop();
            const bool active = config.replay_buffer->active();
            const bool ok = active == starting;
            if (!ok) {
                replay_mode_errors.fetch_add(1, std::memory_order_relaxed);
            }
            emit_replay_mode(command, ok, active, changed,
                ok ? (changed ? nullptr : (starting ? "already active" : "already stopped"))
                   : "replay buffer did not enter the requested state");
        } catch (const std::exception& exception) {
            replay_mode_errors.fetch_add(1, std::memory_order_relaxed);
            emit_replay_mode(command, false,
                             config.replay_buffer != nullptr && config.replay_buffer->active(), false,
                             exception.what());
        } catch (...) {
            replay_mode_errors.fetch_add(1, std::memory_order_relaxed);
            emit_replay_mode(command, false,
                             config.replay_buffer != nullptr && config.replay_buffer->active(), false,
                             "unknown error changing replay buffer state");
        }
    }

    void remove_pending_front() noexcept {
        pending_paths[pending_head].clear();
        pending_head = (pending_head + 1) % pending_paths.size();
        --pending_size;
        pending_save_count.store(pending_size, std::memory_order_release);
    }

    void submit_pending_front() noexcept {
        if (pending_size == 0) {
            return;
        }
        if (!exporter) {
            replay_jobs_failed.fetch_add(pending_size, std::memory_order_relaxed);
            pending_size = 0;
            pending_save_count.store(0, std::memory_order_release);
            return;
        }

        auto& path = pending_paths[pending_head];
        try {
            auto mux_config = config.mux_config_template;
            mux_config.output_path = path;
            const auto submission = exporter->submit(
                std::move(mux_config), config.replay_duration);
            if (submission.status == ReplayExportSubmitStatus::queue_full) {
                replay_submit_retries.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            if (!submission) {
                replay_jobs_failed.fetch_add(1, std::memory_order_relaxed);
                const auto message = submission.status == ReplayExportSubmitStatus::snapshot_unavailable
                    ? std::string(submit_status_message(submission.status)) + ": " +
                          snapshot_status_name(submission.snapshot_status)
                    : std::string(submit_status_message(submission.status));
                emit(ControlServiceEventType::replay_failed, path,
                     submission.job_id, submission.status, 0, 0,
                     message.c_str(),
                     submission.status == ReplayExportSubmitStatus::snapshot_unavailable
                         ? std::optional<replay::SnapshotStatus>{submission.snapshot_status}
                         : std::nullopt);
                remove_pending_front();
                return;
            }

            replay_jobs_submitted.fetch_add(1, std::memory_order_relaxed);
            emit(ControlServiceEventType::replay_queued, path, submission.job_id);
            remove_pending_front();
        } catch (const std::exception& exception) {
            replay_jobs_failed.fetch_add(1, std::memory_order_relaxed);
            emit(ControlServiceEventType::replay_failed, path, 0,
                 ReplayExportSubmitStatus::snapshot_unavailable, 0, 0,
                 exception.what());
            remove_pending_front();
        } catch (...) {
            replay_jobs_failed.fetch_add(1, std::memory_order_relaxed);
            emit(ControlServiceEventType::replay_failed, path, 0,
                 ReplayExportSubmitStatus::snapshot_unavailable, 0, 0,
                 "unknown exception while submitting replay export");
            remove_pending_front();
        }
    }

    static const char* submit_status_message(ReplayExportSubmitStatus status) noexcept {
        switch (status) {
        case ReplayExportSubmitStatus::queued: return "queued";
        case ReplayExportSubmitStatus::invalid_config: return "invalid replay export configuration";
        case ReplayExportSubmitStatus::queue_full: return "replay exporter is at capacity";
        case ReplayExportSubmitStatus::snapshot_unavailable: return "replay snapshot is unavailable";
        case ReplayExportSubmitStatus::snapshot_codec_mismatch: return "replay codec does not match the mux configuration";
        case ReplayExportSubmitStatus::stopping: return "replay exporter is stopping";
        }
        return "unknown replay export submission status";
    }

    static const char* snapshot_status_name(replay::SnapshotStatus status) noexcept {
        switch (status) {
        case replay::SnapshotStatus::ready: return "ready";
        case replay::SnapshotStatus::invalid_duration: return "invalid_duration";
        case replay::SnapshotStatus::insufficient_history: return "insufficient_history";
        case replay::SnapshotStatus::no_keyframe: return "no_keyframe";
        case replay::SnapshotStatus::keyframe_preroll_exceeded: return "keyframe_preroll_exceeded";
        case replay::SnapshotStatus::snapshot_memory_budget: return "snapshot_memory_budget";
        case replay::SnapshotStatus::snapshot_capacity: return "snapshot_capacity";
        }
        return "unknown";
    }

    void collect_results() noexcept {
        if (!exporter) {
            return;
        }
        ReplayExportResult result;
        for (;;) {
            try {
                if (!exporter->try_pop_result(result)) {
                    return;
                }
            } catch (...) {
                return;
            }

            if (result.succeeded) {
                replay_jobs_succeeded.fetch_add(1, std::memory_order_relaxed);
                emit(ControlServiceEventType::replay_succeeded, result.output_path,
                     result.job_id, ReplayExportSubmitStatus::queued,
                     result.snapshot_packets, result.output_file_bytes);
            } else {
                replay_jobs_failed.fetch_add(1, std::memory_order_relaxed);
                emit(ControlServiceEventType::replay_failed, result.output_path,
                     result.job_id, ReplayExportSubmitStatus::queued,
                     result.snapshot_packets, result.output_file_bytes,
                     result.error.c_str());
            }
        }
    }

    bool input_is_drained() const noexcept {
        const auto input_stats = input->stats();
        return input_closed.load(std::memory_order_acquire) &&
               input_stats.commands_enqueued == input_stats.commands_popped;
    }

    void run() noexcept {
        try {
            for (;;) {
                observe_stop_command();
                collect_commands();
                collect_results();
                submit_pending_front();

                if (close_requested.load(std::memory_order_acquire) &&
                    input_is_drained() && pending_size == 0) {
                    break;
                }
                std::this_thread::sleep_for(config.poll_interval);
            }
        } catch (...) {
            // Keep shutdown bounded if a library call unexpectedly escapes.
            // Accepted exporter jobs are still closed and their results
            // inspected below.
            replay_jobs_failed.fetch_add(pending_size, std::memory_order_relaxed);
            pending_size = 0;
            pending_save_count.store(0, std::memory_order_release);
        }

        if (exporter) {
            exporter->close();
            collect_results();
        }
        running.store(false, std::memory_order_release);
    }

    ControlServiceConfig config;
    std::unique_ptr<ControlPipe> input;
    std::unique_ptr<ReplayExporter> exporter;
    std::array<std::string, ControlPipe::queue_capacity> pending_paths{};
    std::size_t pending_head{};
    std::size_t pending_size{};
    std::atomic<std::size_t> pending_save_count{0};
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> close_requested{false};
    std::atomic<bool> input_closed{false};
    std::atomic<bool> running{false};
    std::atomic<std::uint64_t> replay_jobs_submitted{0};
    std::atomic<std::uint64_t> replay_jobs_succeeded{0};
    std::atomic<std::uint64_t> replay_jobs_failed{0};
    std::atomic<std::uint64_t> replay_submit_retries{0};
    std::atomic<std::uint64_t> replay_start_commands{0};
    std::atomic<std::uint64_t> replay_stop_commands{0};
    std::atomic<std::uint64_t> replay_mode_errors{0};
    std::atomic<std::uint64_t> audio_control_commands{0};
    std::atomic<std::uint64_t> audio_control_errors{0};
    std::thread worker;
    std::mutex close_mutex;
    bool closed{};
};

ControlService::ControlService(ControlServiceConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

ControlService::~ControlService() = default;

bool ControlService::stop_requested() const noexcept {
    return impl_ && impl_->stop_requested_status();
}

ControlServiceStats ControlService::stats() const noexcept {
    return impl_ ? impl_->stats() : ControlServiceStats{};
}

void ControlService::close() noexcept {
    if (impl_) {
        impl_->close();
    }
}

} // namespace fluxrec::engine
