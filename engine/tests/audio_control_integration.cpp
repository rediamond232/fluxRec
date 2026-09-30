#include "engine/control_service.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;

class InputPipe {
public:
    InputPipe() {
        int descriptors[2]{};
        if (::pipe2(descriptors, O_CLOEXEC) != 0) {
            throw std::system_error(errno, std::generic_category(), "create command pipe");
        }
        read_fd = descriptors[0];
        write_fd = descriptors[1];
    }

    ~InputPipe() {
        if (read_fd >= 0) ::close(read_fd);
        if (write_fd >= 0) ::close(write_fd);
    }

    InputPipe(const InputPipe&) = delete;
    InputPipe& operator=(const InputPipe&) = delete;

    int read_fd{-1};
    int write_fd{-1};
};

void send_line(int fd, std::string_view line) {
    std::size_t written = 0;
    while (written < line.size()) {
        const auto count = ::write(fd, line.data() + written, line.size() - written);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            throw std::system_error(errno, std::generic_category(), "write audio command");
        }
        written += static_cast<std::size_t>(count);
    }
}

class EventCollector {
public:
    void push(const fluxrec::engine::ControlServiceEvent& event) {
        {
            std::lock_guard lock(mutex_);
            events_.push_back(event);
        }
        changed_.notify_all();
    }

    template<class Predicate>
    fluxrec::engine::ControlServiceEvent wait_for(Predicate predicate) {
        std::unique_lock lock(mutex_);
        if (!changed_.wait_for(lock, 2s, [&] {
                return std::any_of(events_.begin(), events_.end(), predicate);
            })) {
            throw std::runtime_error("timed out waiting for audio control acknowledgement");
        }
        const auto found = std::find_if(events_.begin(), events_.end(), predicate);
        auto result = std::move(*found);
        events_.erase(found);
        return result;
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<fluxrec::engine::ControlServiceEvent> events_;
};

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

} // namespace

int main() {
    try {
        InputPipe command_pipe;
        EventCollector events;
        std::mutex applied_mutex;
        std::vector<fluxrec::engine::ControlCommand> applied;

        fluxrec::engine::ControlServiceConfig config;
        config.input_fd = command_pipe.read_fd;
        config.audio_command_callback = [&](const auto& command) {
            {
                std::lock_guard lock(applied_mutex);
                applied.push_back(command);
            }
        };
        config.event_callback = [&](const auto& event) { events.push(event); };
        fluxrec::engine::ControlService service(std::move(config));
        ::close(command_pipe.read_fd);
        command_pipe.read_fd = -1;

        send_line(command_pipe.write_fd, "set_audio_volume desktop 0.75\n");
        send_line(command_pipe.write_fd, "set_audio_volume track-2 2.0\n");
        send_line(command_pipe.write_fd, "set_audio_mute headset on\n");
        send_line(command_pipe.write_fd, "set_audio_volume has space 1.0\n");
        send_line(command_pipe.write_fd, "set_audio_volume desktop 4.01\n");

        auto event = events.wait_for([](const auto& value) {
            return value.type == fluxrec::engine::ControlServiceEventType::audio_control_changed &&
                   value.audio_track_id == "desktop";
        });
        require(event.ok && event.audio_gain && *event.audio_gain == 0.75F &&
                    !event.audio_muted,
                "desktop gain acknowledgement has wrong source or value");

        event = events.wait_for([](const auto& value) {
            return value.type == fluxrec::engine::ControlServiceEventType::audio_control_changed &&
                   value.audio_track_id == "track-2";
        });
        require(event.ok && event.audio_gain && *event.audio_gain == 2.0F &&
                    !event.audio_muted,
                "track-2 gain acknowledgement has wrong source or value");

        event = events.wait_for([](const auto& value) {
            return value.type == fluxrec::engine::ControlServiceEventType::audio_control_changed &&
                   value.audio_track_id == "headset";
        });
        require(event.ok && !event.audio_gain && event.audio_muted && *event.audio_muted,
                "headset mute acknowledgement has wrong source or value");

        const auto invalid_deadline = std::chrono::steady_clock::now() + 2s;
        while (service.stats().input.invalid_lines < 2 &&
               std::chrono::steady_clock::now() < invalid_deadline) {
            std::this_thread::sleep_for(5ms);
        }
        const auto stats = service.stats();
        require(stats.input.invalid_lines == 2,
                "space id and out-of-range gain were not rejected");
        require(stats.audio_control_commands == 3 && stats.audio_control_errors == 0,
                "audio command and failure counters are incorrect");
        {
            std::lock_guard lock(applied_mutex);
            require(applied.size() == 3 &&
                        applied[0].audio_track_id == "desktop" &&
                        applied[0].type == fluxrec::engine::ControlCommandType::set_audio_volume &&
                        applied[0].audio_gain == 0.75F &&
                        applied[1].audio_track_id == "track-2" &&
                        applied[1].type == fluxrec::engine::ControlCommandType::set_audio_volume &&
                        applied[1].audio_gain == 2.0F &&
                        applied[2].audio_track_id == "headset" &&
                        applied[2].type == fluxrec::engine::ControlCommandType::set_audio_mute &&
                        applied[2].audio_muted,
                    "control callback received incorrect parsed audio commands");
        }

        service.close();
        std::cout << "Audio control protocol parsed and acknowledged gain/mute commands; "
                     "invalid track ids and out-of-range gains were rejected.\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "audio control integration test: " << exception.what() << '\n';
        return 1;
    }
}
