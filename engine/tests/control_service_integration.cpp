#include "engine/control_service.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
}

#include <array>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>
#include <fcntl.h>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;

[[nodiscard]] std::string ff_error(int error) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> text{};
    av_strerror(error, text.data(), text.size());
    return text.data();
}

void check(int result, const char* operation) {
    if (result < 0) {
        throw std::runtime_error(std::string(operation) + ": " + ff_error(result));
    }
}

class SyntheticEncoder {
public:
    SyntheticEncoder() {
        const AVCodec* codec = avcodec_find_encoder_by_name("libx264");
        if (codec == nullptr) {
            throw std::runtime_error("control integration test requires libx264");
        }
        context_ = avcodec_alloc_context3(codec);
        if (context_ == nullptr) {
            throw std::runtime_error("allocate synthetic encoder context");
        }
        context_->width = width;
        context_->height = height;
        context_->pix_fmt = AV_PIX_FMT_YUV420P;
        context_->time_base = AVRational{1, 30};
        context_->framerate = AVRational{30, 1};
        context_->gop_size = 15;
        context_->max_b_frames = 0;
        context_->bit_rate = 300'000;
        context_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        check(av_opt_set(context_->priv_data, "preset", "ultrafast", 0), "set x264 preset");
        check(av_opt_set(context_->priv_data, "tune", "zerolatency", 0), "set x264 tune");
        check(avcodec_open2(context_, codec, nullptr), "open synthetic H.264 encoder");

        frame_ = av_frame_alloc();
        if (frame_ == nullptr) {
            throw std::runtime_error("allocate synthetic YUV frame");
        }
        frame_->format = context_->pix_fmt;
        frame_->width = width;
        frame_->height = height;
        check(av_frame_get_buffer(frame_, 32), "allocate synthetic frame planes");
    }

    ~SyntheticEncoder() {
        av_frame_free(&frame_);
        avcodec_free_context(&context_);
    }

    SyntheticEncoder(const SyntheticEncoder&) = delete;
    SyntheticEncoder& operator=(const SyntheticEncoder&) = delete;

    [[nodiscard]] AVCodecContext* context() const noexcept { return context_; }

    [[nodiscard]] std::vector<fluxrec::media::EncodedPacket> encode(int count) {
        std::vector<fluxrec::media::EncodedPacket> packets;
        for (int index = 0; index < count; ++index) {
            check(av_frame_make_writable(frame_), "make synthetic frame writable");
            for (int y = 0; y < height; ++y) {
                auto* row = frame_->data[0] + y * frame_->linesize[0];
                for (int x = 0; x < width; ++x) {
                    row[x] = static_cast<std::uint8_t>((x * 3 + y * 5 + index * 11) & 0xff);
                }
            }
            for (int y = 0; y < height / 2; ++y) {
                auto* u_row = frame_->data[1] + y * frame_->linesize[1];
                auto* v_row = frame_->data[2] + y * frame_->linesize[2];
                for (int x = 0; x < width / 2; ++x) {
                    u_row[x] = static_cast<std::uint8_t>(96 + ((index + x) & 31));
                    v_row[x] = static_cast<std::uint8_t>(160 - ((index + y) & 31));
                }
            }
            frame_->pts = index;
            check(avcodec_send_frame(context_, frame_), "submit synthetic frame");
            drain(packets);
        }
        check(avcodec_send_frame(context_, nullptr), "flush synthetic encoder");
        drain(packets);
        if (packets.empty()) {
            throw std::runtime_error("synthetic encoder produced no packets");
        }
        return packets;
    }

private:
    void drain(std::vector<fluxrec::media::EncodedPacket>& packets) {
        AVPacket* packet = av_packet_alloc();
        if (packet == nullptr) {
            throw std::runtime_error("allocate synthetic output packet");
        }
        for (;;) {
            const int result = avcodec_receive_packet(context_, packet);
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
                break;
            }
            check(result, "receive synthetic packet");
            fluxrec::media::EncodedPacket encoded;
            encoded.data.assign(packet->data, packet->data + packet->size);
            encoded.codec = fluxrec::media::VideoCodec::h264;
            encoded.pts = packet->pts;
            encoded.dts = packet->dts;
            encoded.duration = packet->duration > 0 ? packet->duration : 1;
            encoded.keyframe = (packet->flags & AV_PKT_FLAG_KEY) != 0;
            encoded.time_base = {context_->time_base.num, context_->time_base.den};
            packets.push_back(std::move(encoded));
            av_packet_unref(packet);
        }
        av_packet_free(&packet);
    }

    static constexpr int width = 64;
    static constexpr int height = 64;
    AVCodecContext* context_{};
    AVFrame* frame_{};
};

class EventCollector {
public:
    void push(const fluxrec::engine::ControlServiceEvent& event) {
        {
            std::lock_guard lock(mutex_);
            events_.push_back(event);
        }
        changed_.notify_all();
    }

    template <typename Predicate>
    fluxrec::engine::ControlServiceEvent wait_for(Predicate predicate) {
        std::unique_lock lock(mutex_);
        const auto ready = [&] {
            return std::any_of(events_.begin(), events_.end(), predicate);
        };
        if (!changed_.wait_for(lock, 10s, ready)) {
            throw std::runtime_error("timed out waiting for control-service event");
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
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            throw std::system_error(errno, std::generic_category(), "write control command");
        }
        written += static_cast<std::size_t>(count);
    }
}

void verify_export(const std::filesystem::path& path, std::size_t expected_packets) {
    AVFormatContext* format = nullptr;
    check(avformat_open_input(&format, path.c_str(), nullptr, nullptr), "open replay file");
    try {
        check(avformat_find_stream_info(format, nullptr), "read replay stream info");
        if (format->nb_streams != 1 ||
            format->streams[0]->codecpar->codec_id != AV_CODEC_ID_H264 ||
            format->streams[0]->codecpar->width != 64 ||
            format->streams[0]->codecpar->height != 64) {
            throw std::runtime_error("saved replay metadata did not round-trip");
        }
        AVPacket* packet = av_packet_alloc();
        if (packet == nullptr) {
            throw std::runtime_error("allocate replay verification packet");
        }
        std::size_t count = 0;
        std::int64_t previous_dts = AV_NOPTS_VALUE;
        while (av_read_frame(format, packet) >= 0) {
            if (packet->stream_index == 0) {
                if (packet->dts == AV_NOPTS_VALUE ||
                    (previous_dts != AV_NOPTS_VALUE && packet->dts <= previous_dts)) {
                    av_packet_free(&packet);
                    throw std::runtime_error("saved replay has invalid packet DTS ordering");
                }
                previous_dts = packet->dts;
                ++count;
            }
            av_packet_unref(packet);
        }
        av_packet_free(&packet);
        if (count != expected_packets) {
            throw std::runtime_error("saved replay packet count differs from export result");
        }
    } catch (...) {
        avformat_close_input(&format);
        throw;
    }
    avformat_close_input(&format);
}

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

} // namespace

int main() {
    try {
        const auto directory = std::filesystem::temp_directory_path();
        const auto prefix = "fluxrec_control_service_" + std::to_string(::getpid());
        const auto frozen_path = directory / (prefix + "_frozen.mkv");
        const auto empty_path = directory / (prefix + "_empty.mkv");
        std::error_code ignored;
        std::filesystem::remove(frozen_path, ignored);
        std::filesystem::remove(empty_path, ignored);

        SyntheticEncoder encoder;
        auto packets = encoder.encode(61);
        fluxrec::replay::ReplayBufferConfig replay_config;
        replay_config.history_duration = 2s;
        replay_config.max_keyframe_preroll = 1s;
        replay_config.max_bytes = 8U * 1024U * 1024U;
        replay_config.max_packets = 1024;
        fluxrec::replay::ReplayBuffer replay_buffer(replay_config);
        for (auto& packet : packets) {
            require(replay_buffer.append(std::move(packet)) ==
                        fluxrec::replay::AppendResult::stored,
                    "could not seed replay buffer with synthetic video packets");
        }

        fluxrec::mux::MuxConfig mux_template;
        mux_template.codec = fluxrec::media::VideoCodec::h264;
        mux_template.width = 64;
        mux_template.height = 64;
        mux_template.stream_time_base = {1, 1000};
        mux_template.frame_rate = {30, 1};
        mux_template.bit_rate = encoder.context()->bit_rate;
        mux_template.queue_capacity_packets = 2;
        if (encoder.context()->extradata != nullptr && encoder.context()->extradata_size > 0) {
            mux_template.codec_extradata.assign(
                encoder.context()->extradata,
                encoder.context()->extradata + encoder.context()->extradata_size);
        }

        InputPipe command_pipe;
        EventCollector events;
        fluxrec::engine::ControlServiceConfig config;
        config.replay_buffer = &replay_buffer;
        config.mux_config_template = std::move(mux_template);
        config.replay_duration = 1s;
        config.input_fd = command_pipe.read_fd;
        config.event_callback = [&events](const auto& event) { events.push(event); };
        fluxrec::engine::ControlService service(std::move(config));
        ::close(command_pipe.read_fd);
        command_pipe.read_fd = -1;

        send_line(command_pipe.write_fd, "start_replay\n");
        auto event = events.wait_for([](const auto& value) {
            return value.type == fluxrec::engine::ControlServiceEventType::replay_started;
        });
        require(event.ok && event.active && !event.state_changed,
                "start_replay did not acknowledge the already-active buffer");

        send_line(command_pipe.write_fd, "stop_replay\n");
        event = events.wait_for([](const auto& value) {
            return value.type == fluxrec::engine::ControlServiceEventType::replay_stopped;
        });
        require(event.ok && !event.active && event.state_changed,
                "stop_replay did not freeze the active buffer");

        send_line(command_pipe.write_fd, "save_replay " + frozen_path.string() + "\n");
        event = events.wait_for([&](const auto& value) {
            return value.type == fluxrec::engine::ControlServiceEventType::replay_queued &&
                   value.output_path == frozen_path.string();
        });
        require(event.job_id != 0, "frozen replay save was not queued");
        const auto saved = events.wait_for([&](const auto& value) {
            return value.type == fluxrec::engine::ControlServiceEventType::replay_succeeded &&
                   value.output_path == frozen_path.string();
        });
        if (!saved.ok || saved.job_id != event.job_id || saved.snapshot_packets != 31) {
            throw std::runtime_error("save_replay result mismatch: ok=" +
                std::to_string(saved.ok) + " queued_job=" + std::to_string(event.job_id) +
                " saved_job=" + std::to_string(saved.job_id) + " packets=" +
                std::to_string(saved.snapshot_packets));
        }
        verify_export(frozen_path, saved.snapshot_packets);

        send_line(command_pipe.write_fd, "start_replay\n");
        event = events.wait_for([](const auto& value) {
            return value.type == fluxrec::engine::ControlServiceEventType::replay_started &&
                   value.state_changed;
        });
        require(event.ok && event.active && replay_buffer.stats().packet_count == 0,
                "start_replay did not clear the previous replay generation");

        send_line(command_pipe.write_fd, "save_replay " + empty_path.string() + "\n");
        event = events.wait_for([&](const auto& value) {
            return value.type == fluxrec::engine::ControlServiceEventType::replay_failed &&
                   value.output_path == empty_path.string();
        });
        require(event.submit_status == fluxrec::engine::ReplayExportSubmitStatus::snapshot_unavailable,
                "replay history from before start_replay leaked into the new generation");
        require(event.snapshot_status == fluxrec::replay::SnapshotStatus::no_keyframe &&
                    event.message.find("no_keyframe") != std::string::npos,
                "unavailable replay event did not expose the precise snapshot failure");

        send_line(command_pipe.write_fd, "stop_replay\n");
        event = events.wait_for([](const auto& value) {
            return value.type == fluxrec::engine::ControlServiceEventType::replay_stopped &&
                   value.state_changed;
        });
        require(event.ok && !event.active, "second stop_replay failed");

        send_line(command_pipe.write_fd, "not_a_command\n");
        const auto invalid_deadline = std::chrono::steady_clock::now() + 2s;
        while (service.stats().input.invalid_lines == 0 &&
               std::chrono::steady_clock::now() < invalid_deadline) {
            std::this_thread::sleep_for(1ms);
        }
        require(service.stats().input.invalid_lines == 1,
                "unknown control command was not reported as an invalid line");

        send_line(command_pipe.write_fd, "stop\n");
        const auto stop_deadline = std::chrono::steady_clock::now() + 2s;
        while (!service.stop_requested() && std::chrono::steady_clock::now() < stop_deadline) {
            std::this_thread::sleep_for(1ms);
        }
        require(service.stop_requested(), "stop did not raise the independent stop request");
        ::close(command_pipe.write_fd);
        command_pipe.write_fd = -1;
        service.close();

        const auto stats = service.stats();
        require(stats.replay_jobs_submitted == 1 && stats.replay_jobs_succeeded == 1 &&
                    stats.replay_jobs_failed == 1 && stats.replay_mode_errors == 0,
                "control service replay counters did not match accepted commands");
        std::filesystem::remove(frozen_path, ignored);
        std::filesystem::remove(empty_path, ignored);
        std::cout << "control service integration passed: frozen replay exported with "
                  << saved.snapshot_packets << " packets; start clears old history; stop request independent\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "control service integration failed: " << exception.what() << '\n';
        return 1;
    }
}
