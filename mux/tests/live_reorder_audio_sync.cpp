#include "mux/mux_writer.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
}

#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

[[nodiscard]] std::string ff_error(int error) {
    char text[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(error, text, sizeof(text));
    return text;
}

void check(int result, const char* operation) {
    if (result < 0) throw std::runtime_error(std::string(operation) + ": " + ff_error(result));
}

class CodecHeaders {
public:
    CodecHeaders() {
        const AVCodec* video_codec = avcodec_find_encoder_by_name("libx264");
        const AVCodec* audio_codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
        if (video_codec == nullptr || audio_codec == nullptr) {
            throw std::runtime_error("test requires libx264 and AAC encoders for codec private data");
        }
        video_ = avcodec_alloc_context3(video_codec);
        audio_ = avcodec_alloc_context3(audio_codec);
        if (video_ == nullptr || audio_ == nullptr) {
            throw std::runtime_error("allocate test codec contexts");
        }
        video_->width = 16;
        video_->height = 16;
        video_->pix_fmt = AV_PIX_FMT_YUV420P;
        video_->time_base = AVRational{1, 30};
        video_->framerate = AVRational{30, 1};
        video_->gop_size = 30;
        video_->max_b_frames = 0;
        video_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        check(av_opt_set(video_->priv_data, "preset", "ultrafast", 0), "set test x264 preset");
        check(av_opt_set(video_->priv_data, "tune", "zerolatency", 0), "set test x264 tune");
        check(avcodec_open2(video_, video_codec, nullptr), "open test x264 encoder");

        audio_->sample_rate = 48'000;
        audio_->time_base = AVRational{1, 48'000};
        audio_->bit_rate = 96'000;
        audio_->sample_fmt = AV_SAMPLE_FMT_FLTP;
        audio_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        av_channel_layout_default(&audio_->ch_layout, 2);
        check(avcodec_open2(audio_, audio_codec, nullptr), "open test AAC encoder");
    }

    ~CodecHeaders() {
        avcodec_free_context(&video_);
        avcodec_free_context(&audio_);
    }
    CodecHeaders(const CodecHeaders&) = delete;
    CodecHeaders& operator=(const CodecHeaders&) = delete;

    [[nodiscard]] std::vector<std::uint8_t> video_extradata() const {
        return copy_extradata(video_);
    }
    [[nodiscard]] std::vector<std::uint8_t> audio_extradata() const {
        return copy_extradata(audio_);
    }

private:
    [[nodiscard]] static std::vector<std::uint8_t> copy_extradata(
        const AVCodecContext* context) {
        if (context->extradata == nullptr || context->extradata_size <= 0) {
            throw std::runtime_error("test encoder did not provide codec private data");
        }
        return {context->extradata, context->extradata + context->extradata_size};
    }

    AVCodecContext* video_{};
    AVCodecContext* audio_{};
};

struct Times {
    std::int64_t first_video_pts{AV_NOPTS_VALUE};
    std::int64_t first_video_dts{AV_NOPTS_VALUE};
    std::int64_t first_audio_pts{AV_NOPTS_VALUE};
    AVRational video_time_base{0, 1};
    AVRational audio_time_base{0, 1};
};

[[nodiscard]] Times read_times(const std::filesystem::path& path) {
    AVFormatContext* format = nullptr;
    check(avformat_open_input(&format, path.c_str(), nullptr, nullptr), "open test mux output");
    try {
        AVPacket* packet = av_packet_alloc();
        if (packet == nullptr) throw std::runtime_error("allocate test demux packet");
        Times result;
        int video_index = -1;
        int audio_index = -1;
        for (unsigned int index = 0; index < format->nb_streams; ++index) {
            const auto codec = format->streams[index]->codecpar->codec_id;
            if (codec == AV_CODEC_ID_H264) video_index = static_cast<int>(index);
            if (codec == AV_CODEC_ID_AAC) audio_index = static_cast<int>(index);
        }
        if (video_index < 0 || audio_index < 0) {
            av_packet_free(&packet);
            throw std::runtime_error("test mux output did not expose H.264 and AAC tracks");
        }
        result.video_time_base = format->streams[video_index]->time_base;
        result.audio_time_base = format->streams[audio_index]->time_base;
        while (av_read_frame(format, packet) >= 0) {
            if (packet->stream_index == video_index && result.first_video_pts == AV_NOPTS_VALUE) {
                result.first_video_pts = packet->pts;
                result.first_video_dts = packet->dts;
            }
            if (packet->stream_index == audio_index && result.first_audio_pts == AV_NOPTS_VALUE) {
                result.first_audio_pts = packet->pts;
            }
            av_packet_unref(packet);
        }
        av_packet_free(&packet);
        avformat_close_input(&format);
        return result;
    } catch (...) {
        avformat_close_input(&format);
        throw;
    }
}

[[nodiscard]] double seconds(std::int64_t pts, AVRational time_base) {
    return static_cast<double>(pts) * av_q2d(time_base);
}

void run_case(const CodecHeaders& headers, const std::filesystem::path& path,
              std::int64_t first_video_dts, bool enqueue_audio_before_video) {
    constexpr std::uint64_t video_origin_ns = 20'000'000'000ULL;
    std::error_code ignored;
    std::filesystem::remove(path, ignored);

    fluxrec::mux::AudioStreamConfig audio_config;
    audio_config.codec = fluxrec::media::AudioCodec::aac;
    audio_config.sample_rate = 48'000;
    audio_config.channels = 2;
    audio_config.bit_rate = 96'000;
    audio_config.stream_time_base = {1, 48'000};
    audio_config.codec_extradata = headers.audio_extradata();

    fluxrec::mux::MuxConfig config;
    config.output_path = path.string();
    config.codec = fluxrec::media::VideoCodec::h264;
    config.width = 16;
    config.height = 16;
    config.stream_time_base = {1, 30};
    config.frame_rate = {30, 1};
    config.codec_extradata = headers.video_extradata();
    config.audio_streams = {audio_config};
    config.video_origin_time_ns = video_origin_ns;
    config.queue_capacity_packets = 1;
    config.queue_capacity_bytes = 8;

    fluxrec::mux::MuxWriter writer(std::move(config));
    if (!writer.wait_until_ready(std::chrono::seconds(5))) {
        throw std::runtime_error("test mux writer did not become ready: " + writer.error_message());
    }

    fluxrec::media::EncodedAudioPacket audio;
    audio.stream_index = 0;
    audio.data.assign(4, 0x55);
    audio.codec = fluxrec::media::AudioCodec::aac;
    audio.pts = 20'000'000'000LL * 48'000 / 1'000'000'000;
    audio.duration = 1024;
    audio.capture_time_ns = video_origin_ns;
    audio.time_base = {1, 48'000};

    fluxrec::media::EncodedPacket video;
    video.data = {0, 0, 1, 0x65, 0x88};
    video.codec = fluxrec::media::VideoCodec::h264;
    video.pts = 0;
    video.dts = first_video_dts;
    video.duration = 1;
    video.keyframe = true;
    video.time_base = {1, 30};

    const auto enqueue = [&writer](auto&& packet, const char* label) {
        auto result = fluxrec::mux::EnqueueResult::queue_busy;
        for (int attempt = 0; attempt < 100 && result == fluxrec::mux::EnqueueResult::queue_busy;
             ++attempt) {
            result = writer.try_enqueue(std::move(packet));
            if (result == fluxrec::mux::EnqueueResult::queue_busy) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        if (result != fluxrec::mux::EnqueueResult::accepted) {
            throw std::runtime_error(std::string("could not enqueue ") + label + ": " +
                                     std::to_string(static_cast<int>(result)));
        }
    };
    if (enqueue_audio_before_video) {
        auto excess_audio = audio;
        enqueue(std::move(audio), "initial AAC packet");
        auto excess_result = fluxrec::mux::EnqueueResult::queue_busy;
        for (int attempt = 0; attempt < 100 &&
             excess_result == fluxrec::mux::EnqueueResult::queue_busy; ++attempt) {
            excess_result = writer.try_enqueue(std::move(excess_audio));
            if (excess_result == fluxrec::mux::EnqueueResult::queue_busy) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        if (excess_result != fluxrec::mux::EnqueueResult::queue_full) {
            throw std::runtime_error("pre-video audio exceeded the configured packet bound");
        }
        // Confirm the worker reached the audio packet before the first video
        // packet is released. It must retain it without consuming the queue.
        for (int attempt = 0; attempt < 100 && writer.stats().queue_wait_samples == 0; ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (writer.stats().queue_wait_samples != 0) {
            throw std::runtime_error("mux worker consumed audio before seeing the first video DTS");
        }
        enqueue(std::move(video), "first video packet");
    } else {
        enqueue(std::move(video), "first video packet");
        enqueue(std::move(audio), "initial AAC packet");
    }
    writer.close();
    if (writer.state() != fluxrec::mux::WriterState::closed ||
        writer.stats().muxed_packets != 2) {
        throw std::runtime_error("mux writer did not drain both reordered A/V packets: " +
                                 writer.error_message());
    }

    const auto times = read_times(path);
    if (times.first_video_pts == AV_NOPTS_VALUE || times.first_audio_pts == AV_NOPTS_VALUE) {
        throw std::runtime_error("test mux output is missing an expected packet (video pts=" +
                                 std::to_string(times.first_video_pts) + ", video dts=" +
                                 std::to_string(times.first_video_dts) + ", audio pts=" +
                                 std::to_string(times.first_audio_pts) + ")");
    }
    const auto av_delta = seconds(times.first_audio_pts, times.audio_time_base) -
                          seconds(times.first_video_pts, times.video_time_base);
    if (std::abs(av_delta) > 0.0011) {
        throw std::runtime_error("first AAC and video presentation timestamps differ by " +
                                 std::to_string(av_delta * 1000.0) + " ms");
    }
    if (first_video_dts == 0 &&
        (times.first_video_pts != 0 || times.first_audio_pts != 0)) {
        throw std::runtime_error("zero-B-frame timestamps changed from the shared origin");
    }
    const auto stats = writer.stats();
    if (stats.queue_high_water_packets > 2 || stats.queue_high_water_bytes > 16) {
        throw std::runtime_error("first-video queue reservation exceeded its bounded allowance");
    }
}

void run_close_without_video(const CodecHeaders& headers,
                             const std::filesystem::path& path) {
    constexpr std::uint64_t video_origin_ns = 20'000'000'000ULL;
    std::error_code ignored;
    std::filesystem::remove(path, ignored);

    fluxrec::mux::AudioStreamConfig audio_config;
    audio_config.codec = fluxrec::media::AudioCodec::aac;
    audio_config.sample_rate = 48'000;
    audio_config.channels = 2;
    audio_config.bit_rate = 96'000;
    audio_config.stream_time_base = {1, 48'000};
    audio_config.codec_extradata = headers.audio_extradata();

    fluxrec::mux::MuxConfig config;
    config.output_path = path.string();
    config.codec = fluxrec::media::VideoCodec::h264;
    config.width = 16;
    config.height = 16;
    config.stream_time_base = {1, 30};
    config.frame_rate = {30, 1};
    config.codec_extradata = headers.video_extradata();
    config.audio_streams = {audio_config};
    config.video_origin_time_ns = video_origin_ns;
    config.queue_capacity_packets = 1;
    config.queue_capacity_bytes = 32;

    fluxrec::mux::MuxWriter writer(std::move(config));
    if (!writer.wait_until_ready(std::chrono::seconds(5))) {
        throw std::runtime_error("no-video mux writer did not become ready: " +
                                 writer.error_message());
    }
    fluxrec::media::EncodedAudioPacket audio;
    audio.stream_index = 0;
    audio.data.assign(24, 0x55);
    audio.codec = fluxrec::media::AudioCodec::aac;
    audio.pts = 960'000;
    audio.duration = 1024;
    audio.capture_time_ns = video_origin_ns;
    audio.time_base = {1, 48'000};
    if (writer.try_enqueue(std::move(audio)) != fluxrec::mux::EnqueueResult::accepted) {
        throw std::runtime_error("could not enqueue audio for no-video shutdown case");
    }
    writer.close();
    if (writer.state() != fluxrec::mux::WriterState::closed ||
        writer.stats().muxed_packets != 1) {
        throw std::runtime_error("shutdown without a video packet did not drain audio");
    }
}

} // namespace

int main() {
    try {
        const auto directory = std::filesystem::temp_directory_path();
        CodecHeaders headers;
        run_case(headers, directory / "fluxrec_live_reorder_audio_sync_bframes.mkv", -2, true);
        run_case(headers, directory / "fluxrec_live_reorder_audio_sync_zero_bframes.mkv", 0, true);
        run_close_without_video(headers,
            directory / "fluxrec_live_reorder_audio_sync_no_video.mkv");
        std::cout << "live reorder audio sync integration passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "live reorder audio sync integration failed: " << exception.what() << '\n';
        return 1;
    }
}
