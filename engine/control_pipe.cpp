#include "engine/control_pipe.hpp"

#include "audio/track_config.hpp"

#include <array>
#include <atomic>
#include <charconv>
#include <cerrno>
#include <cmath>
#include <fcntl.h>
#include <poll.h>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <utility>

namespace fluxrec::engine {
namespace {

constexpr int poll_timeout_ms = 100;
constexpr std::string_view save_replay_prefix{"save_replay "};
constexpr std::string_view set_audio_volume_prefix{"set_audio_volume "};
constexpr std::string_view set_audio_mute_prefix{"set_audio_mute "};

} // namespace

struct ControlPipe::Impl {
    explicit Impl(int input_fd) {
        if (input_fd < 0) {
            throw std::invalid_argument("control pipe input descriptor must be valid");
        }
        fd = ::fcntl(input_fd, F_DUPFD_CLOEXEC, 0);
        if (fd < 0) {
            throw std::system_error(errno, std::generic_category(),
                                    "duplicating control input descriptor");
        }

        running.store(true, std::memory_order_release);
        try {
            worker = std::thread([this] { run(); });
        } catch (...) {
            running.store(false, std::memory_order_release);
            ::close(fd);
            fd = -1;
            throw;
        }
    }

    ~Impl() { close(); }

    bool try_pop(ControlCommand& command) noexcept {
        const auto tail_value = tail.load(std::memory_order_relaxed);
        const auto head_value = head.load(std::memory_order_acquire);
        if (tail_value == head_value) {
            return false;
        }

        command = std::move(queue[tail_value % queue_capacity]);
        tail.store(tail_value + 1, std::memory_order_release);
        commands_popped.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    void close() noexcept {
        stopping.store(true, std::memory_order_release);
        if (worker.joinable()) {
            worker.join();
        }
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    }

    ControlPipeStats stats() const noexcept {
        return {
            .commands_enqueued = commands_enqueued.load(std::memory_order_relaxed),
            .commands_popped = commands_popped.load(std::memory_order_relaxed),
            .invalid_lines = invalid_lines.load(std::memory_order_relaxed),
            .oversized_lines = oversized_lines.load(std::memory_order_relaxed),
            .queue_full_lines = queue_full_lines.load(std::memory_order_relaxed),
            .eof = eof.load(std::memory_order_relaxed),
            .stop_command_seen = stop_command_seen.load(std::memory_order_relaxed),
            .running = running.load(std::memory_order_acquire),
            .read_error = read_error.load(std::memory_order_relaxed),
        };
    }

private:
    bool push(ControlCommand&& command) noexcept {
        const auto head_value = head.load(std::memory_order_relaxed);
        const auto tail_value = tail.load(std::memory_order_acquire);
        const auto occupied = head_value - tail_value;
        // Keep one slot available for stop even when save requests fill the
        // queue. This lets shutdown reach the control path under load.
        const auto limit = command.type == ControlCommandType::stop
            ? queue_capacity : queue_capacity - 1;
        if (occupied >= limit) {
            return false;
        }

        queue[head_value % queue_capacity] = std::move(command);
        head.store(head_value + 1, std::memory_order_release);
        commands_enqueued.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    static bool valid_absolute_path(std::string_view path) noexcept {
        if (path.empty() || path.front() != '/') {
            return false;
        }
        for (const unsigned char byte : path) {
            if (byte < 0x20U || byte == 0x7fU) {
                return false;
            }
        }
        return true;
    }

    static bool parse_audio_command(std::string_view value,
                                    ControlCommand& command) {
        const auto parse_arguments = [](std::string_view arguments,
                                        std::string_view& source,
                                        std::string_view& setting) noexcept {
            const auto separator = arguments.find(' ');
            if (separator == std::string_view::npos || separator == 0 ||
                separator + 1 >= arguments.size()) {
                return false;
            }
            source = arguments.substr(0, separator);
            setting = arguments.substr(separator + 1);
            return setting.find_first_of(" \t\r\n") == std::string_view::npos;
        };

        std::string_view source_text;
        std::string_view setting;
        if (value.starts_with(set_audio_volume_prefix)) {
            if (!parse_arguments(value.substr(set_audio_volume_prefix.size()),
                                 source_text, setting) ||
                !audio::validTrackId(source_text)) {
                return false;
            }
            float gain = 0.0F;
            const auto parsed = std::from_chars(setting.data(),
                                                setting.data() + setting.size(), gain,
                                                std::chars_format::general);
            if (parsed.ec != std::errc{} ||
                parsed.ptr != setting.data() + setting.size() ||
                !std::isfinite(gain) || gain < 0.0F || gain > 4.0F) {
                return false;
            }
            command.type = ControlCommandType::set_audio_volume;
            command.audio_track_id = std::string(source_text);
            command.audio_gain = gain;
            return true;
        }
        if (value.starts_with(set_audio_mute_prefix)) {
            if (!parse_arguments(value.substr(set_audio_mute_prefix.size()),
                                 source_text, setting) ||
                !audio::validTrackId(source_text) ||
                (setting != "on" && setting != "off")) {
                return false;
            }
            command.type = ControlCommandType::set_audio_mute;
            command.audio_track_id = std::string(source_text);
            command.audio_muted = setting == "on";
            return true;
        }
        return false;
    }

    // Returns true when the accepted command is stop, so the worker can exit
    // immediately after publishing it.
    bool finish_line() {
        if (discarding_line) {
            discarding_line = false;
            line_size = 0;
            return false;
        }

        auto size = line_size;
        line_size = 0;
        if (size != 0 && line[size - 1] == '\r') {
            --size;
        }
        if (size == 0) {
            return false;
        }

        const std::string_view value{line.data(), size};
        ControlCommand command;
        if (value == "stop") {
            command.type = ControlCommandType::stop;
        } else if (value == "start_replay") {
            command.type = ControlCommandType::start_replay;
        } else if (value == "stop_replay") {
            command.type = ControlCommandType::stop_replay;
        } else if (value.starts_with(save_replay_prefix)) {
            const auto path = value.substr(save_replay_prefix.size());
            if (!valid_absolute_path(path)) {
                invalid_lines.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            command.type = ControlCommandType::save_replay;
            command.path.assign(path);
        } else if (value.starts_with(set_audio_volume_prefix) ||
                   value.starts_with(set_audio_mute_prefix)) {
            if (!parse_audio_command(value, command)) {
                invalid_lines.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
        } else {
            invalid_lines.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        const bool is_stop = command.type == ControlCommandType::stop;
        if (!push(std::move(command))) {
            queue_full_lines.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (is_stop) {
            stop_command_seen.store(true, std::memory_order_release);
        }
        return is_stop;
    }

    bool consume_byte(char byte) {
        if (byte == '\n') {
            return finish_line();
        }
        if (discarding_line) {
            return false;
        }
        if (line_size == line.size()) {
            line_size = 0;
            discarding_line = true;
            oversized_lines.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        line[line_size++] = byte;
        return false;
    }

    void set_read_error(int error) noexcept {
        int expected = 0;
        (void)read_error.compare_exchange_strong(expected, error,
                                                  std::memory_order_relaxed);
    }

    void run() noexcept {
        try {
            while (!stopping.load(std::memory_order_acquire)) {
                pollfd descriptor{fd, POLLIN | POLLHUP | POLLERR, 0};
                const int poll_result = ::poll(&descriptor, 1, poll_timeout_ms);
                if (poll_result < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    set_read_error(errno);
                    break;
                }
                if (poll_result == 0) {
                    continue;
                }
                if ((descriptor.revents & POLLNVAL) != 0) {
                    set_read_error(EBADF);
                    break;
                }
                if ((descriptor.revents & (POLLIN | POLLHUP | POLLERR)) == 0) {
                    continue;
                }

                // A one-byte read after poll readiness cannot wait for a
                // larger buffer to fill. It also keeps close() bounded even
                // when stdin is an ordinary blocking pipe or terminal.
                char byte{};
                const auto count = ::read(fd, &byte, 1);
                if (count == 1) {
                    if (consume_byte(byte)) {
                        break;
                    }
                    continue;
                }
                if (count == 0) {
                    eof.store(true, std::memory_order_release);
                    if (!stopping.load(std::memory_order_acquire) && line_size != 0) {
                        (void)finish_line();
                    }
                    break;
                }
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                    continue;
                }
                set_read_error(errno);
                break;
            }
        } catch (...) {
            // Keep allocation or library exceptions confined to this worker.
            set_read_error(ENOMEM);
        }
        running.store(false, std::memory_order_release);
    }

    int fd{-1};
    std::array<ControlCommand, queue_capacity> queue{};
    std::atomic<std::uint64_t> head{0};
    std::atomic<std::uint64_t> tail{0};
    std::array<char, max_line_bytes> line{};
    std::size_t line_size{};
    bool discarding_line{};
    std::atomic<bool> stopping{false};
    std::atomic<bool> running{false};
    std::atomic<bool> eof{false};
    std::atomic<bool> stop_command_seen{false};
    std::atomic<int> read_error{0};
    std::atomic<std::uint64_t> commands_enqueued{0};
    std::atomic<std::uint64_t> commands_popped{0};
    std::atomic<std::uint64_t> invalid_lines{0};
    std::atomic<std::uint64_t> oversized_lines{0};
    std::atomic<std::uint64_t> queue_full_lines{0};
    std::thread worker;
};

ControlPipe::ControlPipe(int input_fd) : impl_(std::make_unique<Impl>(input_fd)) {}

ControlPipe::~ControlPipe() = default;

bool ControlPipe::try_pop(ControlCommand& command) noexcept {
    return impl_ && impl_->try_pop(command);
}

void ControlPipe::close() noexcept {
    if (impl_) {
        impl_->close();
    }
}

ControlPipeStats ControlPipe::stats() const noexcept {
    return impl_ ? impl_->stats() : ControlPipeStats{};
}

} // namespace fluxrec::engine
