#include "mux/mux_writer.hpp"
#include "mux/detail/timestamp_rebaser.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/log.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
}

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
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
    if (result < 0) {
        throw std::runtime_error(std::string(operation) + ": " + ff_error(result));
    }
}

class Encoder {
public:
    Encoder() {
        const AVCodec* codec = avcodec_find_encoder_by_name("libx264");
        if (codec == nullptr) {
            throw std::runtime_error("integration test requires FFmpeg's libx264 encoder");
        }
        context_ = avcodec_alloc_context3(codec);
        if (context_ == nullptr) {
            throw std::runtime_error("allocate test encoder context");
        }
        context_->width = width;
        context_->height = height;
        context_->pix_fmt = AV_PIX_FMT_YUV420P;
        context_->time_base = AVRational{1, 30};
        context_->framerate = AVRational{30, 1};
        context_->gop_size = 30;
        context_->max_b_frames = 0;
        context_->bit_rate = 200'000;
        context_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        check(av_opt_set(context_->priv_data, "preset", "ultrafast", 0), "set libx264 preset");
        check(av_opt_set(context_->priv_data, "tune", "zerolatency", 0), "set libx264 tune");
        check(avcodec_open2(context_, codec, nullptr), "open libx264 test encoder");

        frame_ = av_frame_alloc();
        if (frame_ == nullptr) {
            throw std::runtime_error("allocate synthetic input frame");
        }
        frame_->format = context_->pix_fmt;
        frame_->width = context_->width;
        frame_->height = context_->height;
        check(av_frame_get_buffer(frame_, 32), "allocate synthetic YUV frame");
    }

    ~Encoder() {
        av_frame_free(&frame_);
        avcodec_free_context(&context_);
    }

    Encoder(const Encoder&) = delete;
    Encoder& operator=(const Encoder&) = delete;

    [[nodiscard]] AVCodecContext* context() const noexcept { return context_; }

    [[nodiscard]] std::vector<fluxrec::media::EncodedPacket> encode() {
        std::vector<fluxrec::media::EncodedPacket> result;
        for (int frame_index = 0; frame_index < 5; ++frame_index) {
            check(av_frame_make_writable(frame_), "prepare synthetic YUV frame");
            for (int y = 0; y < height; ++y) {
                for (int x = 0; x < width; ++x) {
                    frame_->data[0][y * frame_->linesize[0] + x] =
                        static_cast<std::uint8_t>((x + y + frame_index * 19) & 0xff);
                }
            }
            for (int y = 0; y < height / 2; ++y) {
                for (int x = 0; x < width / 2; ++x) {
                    frame_->data[1][y * frame_->linesize[1] + x] =
                        static_cast<std::uint8_t>(96 + frame_index * 3);
                    frame_->data[2][y * frame_->linesize[2] + x] =
                        static_cast<std::uint8_t>(160 - frame_index * 3);
                }
            }
            frame_->pts = frame_index;
            check(avcodec_send_frame(context_, frame_), "submit synthetic test frame");
            drain(result);
        }
        check(avcodec_send_frame(context_, nullptr), "flush synthetic test encoder");
        drain(result);
        if (result.empty()) {
            throw std::runtime_error("test encoder produced no compressed packets");
        }
        return result;
    }

private:
    void drain(std::vector<fluxrec::media::EncodedPacket>& destination) {
        AVPacket* packet = av_packet_alloc();
        if (packet == nullptr) {
            throw std::runtime_error("allocate test encoded packet");
        }
        for (;;) {
            const int result = avcodec_receive_packet(context_, packet);
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
                break;
            }
            check(result, "receive synthetic encoded packet");
            fluxrec::media::EncodedPacket encoded;
            encoded.data.assign(packet->data, packet->data + packet->size);
            encoded.codec = fluxrec::media::VideoCodec::h264;
            encoded.pts = packet->pts == AV_NOPTS_VALUE ? 0 : packet->pts;
            encoded.dts = packet->dts == AV_NOPTS_VALUE ? encoded.pts : packet->dts;
            encoded.duration = packet->duration > 0 ? packet->duration : 1;
            encoded.keyframe = (packet->flags & AV_PKT_FLAG_KEY) != 0;
            encoded.time_base = fluxrec::media::Rational{context_->time_base.num,
                                                        context_->time_base.den};
            destination.push_back(std::move(encoded));
            av_packet_unref(packet);
        }
        av_packet_free(&packet);
    }

    static constexpr int width = 64;
    static constexpr int height = 64;
    AVCodecContext* context_{nullptr};
    AVFrame* frame_{nullptr};
};

class AudioEncoder {
public:
    explicit AudioEncoder(fluxrec::media::AudioCodec codec) : codec_(codec) {
        const AVCodec* encoder = codec == fluxrec::media::AudioCodec::aac
            ? avcodec_find_encoder(AV_CODEC_ID_AAC)
            : avcodec_find_encoder_by_name("libopus");
        if (encoder == nullptr && codec == fluxrec::media::AudioCodec::opus) {
            encoder = avcodec_find_encoder(AV_CODEC_ID_OPUS);
        }
        if (encoder == nullptr) {
            throw std::runtime_error("integration test requires AAC and Opus encoders");
        }
        context_ = avcodec_alloc_context3(encoder);
        if (context_ == nullptr) {
            throw std::runtime_error("allocate synthetic audio encoder context");
        }
        context_->sample_rate = sample_rate;
        context_->time_base = AVRational{1, sample_rate};
        context_->bit_rate = 96'000;
        context_->sample_fmt = choose_sample_format(encoder);
        if (context_->sample_fmt == AV_SAMPLE_FMT_NONE) {
            throw std::runtime_error("synthetic audio encoder has no supported sample format");
        }
        context_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        av_channel_layout_default(&context_->ch_layout, channels);
        check(avcodec_open2(context_, encoder, nullptr), "open synthetic audio encoder");

        fluxrec::mux::AudioStreamConfig config;
        config.codec = codec;
        config.sample_rate = context_->sample_rate;
        config.channels = context_->ch_layout.nb_channels;
        config.bit_rate = context_->bit_rate;
        config.stream_time_base = {context_->time_base.num, context_->time_base.den};
        if (context_->extradata != nullptr && context_->extradata_size > 0) {
            config.codec_extradata.assign(context_->extradata,
                                          context_->extradata + context_->extradata_size);
        }
        config_ = std::move(config);
        frame_size_ = context_->frame_size > 0 ? context_->frame_size : 960;
    }

    ~AudioEncoder() { avcodec_free_context(&context_); }
    AudioEncoder(const AudioEncoder&) = delete;
    AudioEncoder& operator=(const AudioEncoder&) = delete;

    [[nodiscard]] const fluxrec::mux::AudioStreamConfig& config() const noexcept { return config_; }

    [[nodiscard]] std::vector<fluxrec::media::EncodedAudioPacket> encode(
        std::size_t stream_index, std::uint64_t start_time_ns, int frame_count) {
        std::vector<fluxrec::media::EncodedAudioPacket> result;
        const auto start_pts = av_rescale_q(static_cast<std::int64_t>(start_time_ns),
                                            AVRational{1, 1'000'000'000}, context_->time_base);
        for (int index = 0; index < frame_count; ++index) {
            AVFrame* frame = av_frame_alloc();
            if (frame == nullptr) {
                throw std::runtime_error("allocate synthetic audio input frame");
            }
            frame->format = context_->sample_fmt;
            frame->sample_rate = context_->sample_rate;
            frame->nb_samples = frame_size_;
            frame->pts = start_pts + static_cast<std::int64_t>(index) * frame_size_;
            check(av_channel_layout_copy(&frame->ch_layout, &context_->ch_layout),
                  "copy synthetic audio channel layout");
            int frame_result = av_frame_get_buffer(frame, 0);
            if (frame_result >= 0) {
                frame_result = av_samples_set_silence(frame->extended_data, 0, frame_size_,
                                                       channels, context_->sample_fmt);
            }
            if (frame_result < 0) {
                av_frame_free(&frame);
                check(frame_result, "allocate synthetic audio samples");
            }
            const int send_result = avcodec_send_frame(context_, frame);
            av_frame_free(&frame);
            check(send_result, "submit synthetic audio frame");
            drain(result, stream_index);
        }
        check(avcodec_send_frame(context_, nullptr), "flush synthetic audio encoder");
        drain(result, stream_index);
        if (result.empty()) {
            throw std::runtime_error("synthetic audio encoder produced no packets");
        }
        return result;
    }

private:
    static AVSampleFormat choose_sample_format(const AVCodec* encoder) noexcept {
        const void* supported = nullptr;
        if (avcodec_get_supported_config(nullptr, encoder, AV_CODEC_CONFIG_SAMPLE_FORMAT,
                                         0, &supported, nullptr) < 0 || !supported) {
            return AV_SAMPLE_FMT_FLTP;
        }
        const auto* formats = static_cast<const AVSampleFormat*>(supported);
        for (const auto desired : {AV_SAMPLE_FMT_FLTP, AV_SAMPLE_FMT_FLT, AV_SAMPLE_FMT_S16}) {
            for (const auto* format = formats; *format != AV_SAMPLE_FMT_NONE; ++format) {
                if (*format == desired) return desired;
            }
        }
        return AV_SAMPLE_FMT_NONE;
    }

    void drain(std::vector<fluxrec::media::EncodedAudioPacket>& destination,
               std::size_t stream_index) {
        AVPacket* packet = av_packet_alloc();
        if (packet == nullptr) {
            throw std::runtime_error("allocate synthetic audio encoded packet");
        }
        for (;;) {
            const int result = avcodec_receive_packet(context_, packet);
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
                break;
            }
            check(result, "receive synthetic audio packet");
            if (packet->pts == AV_NOPTS_VALUE) {
                av_packet_free(&packet);
                throw std::runtime_error("synthetic audio packet has no presentation timestamp");
            }
            fluxrec::media::EncodedAudioPacket encoded;
            encoded.stream_index = stream_index;
            encoded.codec = codec_;
            encoded.pts = packet->pts;
            encoded.duration = packet->duration > 0 ? packet->duration : frame_size_;
            encoded.capture_time_ns = static_cast<std::uint64_t>(av_rescale_q(
                packet->pts, context_->time_base, AVRational{1, 1'000'000'000}));
            encoded.keyframe = (packet->flags & AV_PKT_FLAG_KEY) != 0;
            encoded.time_base = {context_->time_base.num, context_->time_base.den};
            encoded.data.assign(packet->data, packet->data + packet->size);
            destination.push_back(std::move(encoded));
            av_packet_unref(packet);
        }
        av_packet_free(&packet);
    }

    static constexpr int sample_rate = 48'000;
    static constexpr int channels = 2;
    fluxrec::media::AudioCodec codec_;
    AVCodecContext* context_{nullptr};
    fluxrec::mux::AudioStreamConfig config_;
    int frame_size_{960};
};

void verify_file(const std::filesystem::path& path, std::size_t expected_packets) {
    AVFormatContext* format = nullptr;
    check(avformat_open_input(&format, path.c_str(), nullptr, nullptr), "open muxed Matroska test file");
    try {
        check(avformat_find_stream_info(format, nullptr), "read muxed Matroska stream information");
        if (format->nb_streams != 1 || format->streams[0]->codecpar->codec_id != AV_CODEC_ID_H264 ||
            format->streams[0]->codecpar->width != 64 || format->streams[0]->codecpar->height != 64) {
            throw std::runtime_error("muxed Matroska video stream metadata did not round-trip");
        }

        std::size_t packet_count = 0;
        std::int64_t previous_dts = AV_NOPTS_VALUE;
        std::int64_t first_pts = AV_NOPTS_VALUE;
        std::int64_t first_dts = AV_NOPTS_VALUE;
        std::int64_t last_dts = AV_NOPTS_VALUE;
        bool saw_keyframe = false;
        AVPacket* packet = av_packet_alloc();
        if (packet == nullptr) {
            throw std::runtime_error("allocate demux verification packet");
        }
        while (av_read_frame(format, packet) >= 0) {
            if (packet->stream_index == 0) {
                ++packet_count;
                if (packet->pts == AV_NOPTS_VALUE || packet->dts == AV_NOPTS_VALUE ||
                    (previous_dts != AV_NOPTS_VALUE && packet->dts <= previous_dts)) {
                    av_packet_free(&packet);
                    throw std::runtime_error("muxed packet timestamps did not preserve decode order");
                }
                if (packet_count == 1) {
                    first_pts = packet->pts;
                    first_dts = packet->dts;
                }
                previous_dts = packet->dts;
                last_dts = packet->dts;
                saw_keyframe = saw_keyframe || (packet->flags & AV_PKT_FLAG_KEY) != 0;
            }
            av_packet_unref(packet);
        }
        av_packet_free(&packet);
        if (packet_count != expected_packets) {
            throw std::runtime_error("muxed packet count did not round-trip through Matroska");
        }
        if (!saw_keyframe) {
            throw std::runtime_error("muxed Matroska stream did not preserve its keyframe flag");
        }
        if (first_dts != 0 || first_pts != 0 || last_dts != static_cast<std::int64_t>(expected_packets - 1U)) {
            throw std::runtime_error("mux did not rebase timestamps or repair DTS collisions after timebase rescaling");
        }
    } catch (...) {
        avformat_close_input(&format);
        throw;
    }
    avformat_close_input(&format);
}

template <typename Packet>
void enqueue_with_retry(fluxrec::mux::MuxWriter& writer, Packet& packet) {
    for (;;) {
        const auto result = writer.try_enqueue(std::move(packet));
        if (result == fluxrec::mux::EnqueueResult::accepted) {
            return;
        }
        if (result == fluxrec::mux::EnqueueResult::queue_full ||
            result == fluxrec::mux::EnqueueResult::queue_busy) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            continue;
        }
        throw std::runtime_error("audio-video mux enqueue failed with result " +
                                 std::to_string(static_cast<int>(result)));
    }
}

void verify_audio_video_file(const std::filesystem::path& path,
                             std::size_t expected_video_packets,
                             std::size_t expected_aac_packets,
                             std::size_t expected_opus_packets) {
    AVFormatContext* format = nullptr;
    check(avformat_open_input(&format, path.c_str(), nullptr, nullptr),
          "open audio-video Matroska test file");
    try {
        check(avformat_find_stream_info(format, nullptr),
              "read audio-video Matroska stream information");
        if (format->nb_streams != 3) {
            throw std::runtime_error("audio-video Matroska file did not contain three tracks");
        }

        int video_index = -1;
        int aac_index = -1;
        int opus_index = -1;
        for (unsigned int index = 0; index < format->nb_streams; ++index) {
            const AVCodecParameters* parameters = format->streams[index]->codecpar;
            if (parameters->codec_id == AV_CODEC_ID_H264) {
                video_index = static_cast<int>(index);
            } else if (parameters->codec_id == AV_CODEC_ID_AAC) {
                aac_index = static_cast<int>(index);
            } else if (parameters->codec_id == AV_CODEC_ID_OPUS) {
                opus_index = static_cast<int>(index);
            }
            if (parameters->codec_type == AVMEDIA_TYPE_AUDIO &&
                (parameters->sample_rate != 48'000 || parameters->ch_layout.nb_channels != 2)) {
                throw std::runtime_error("audio track sample format did not round-trip");
            }
        }
        if (video_index < 0 || aac_index < 0 || opus_index < 0) {
            throw std::runtime_error("Matroska track codec metadata did not round-trip");
        }

        std::array<std::size_t, 3> packet_counts{};
        std::array<std::int64_t, 3> previous_dts{AV_NOPTS_VALUE, AV_NOPTS_VALUE, AV_NOPTS_VALUE};
        std::array<std::int64_t, 3> first_pts{AV_NOPTS_VALUE, AV_NOPTS_VALUE, AV_NOPTS_VALUE};
        AVPacket* packet = av_packet_alloc();
        if (packet == nullptr) {
            throw std::runtime_error("allocate audio-video demux verification packet");
        }
        while (av_read_frame(format, packet) >= 0) {
            const auto index = packet->stream_index;
            if (index >= 0 && index < static_cast<int>(format->nb_streams)) {
                ++packet_counts[static_cast<std::size_t>(index)];
                if (packet->dts == AV_NOPTS_VALUE || packet->pts == AV_NOPTS_VALUE ||
                    (previous_dts[static_cast<std::size_t>(index)] != AV_NOPTS_VALUE &&
                     packet->dts <= previous_dts[static_cast<std::size_t>(index)])) {
                    av_packet_free(&packet);
                    throw std::runtime_error("audio-video track timestamps lost decode order");
                }
                if (first_pts[static_cast<std::size_t>(index)] == AV_NOPTS_VALUE) {
                    first_pts[static_cast<std::size_t>(index)] = packet->pts;
                }
                previous_dts[static_cast<std::size_t>(index)] = packet->dts;
            }
            av_packet_unref(packet);
        }
        av_packet_free(&packet);
        if (packet_counts[static_cast<std::size_t>(video_index)] != expected_video_packets ||
            packet_counts[static_cast<std::size_t>(aac_index)] != expected_aac_packets ||
            packet_counts[static_cast<std::size_t>(opus_index)] != expected_opus_packets) {
            throw std::runtime_error("audio-video track packet counts did not round-trip");
        }

        const auto stream_seconds = [format, &first_pts](int index) {
            return static_cast<double>(first_pts[static_cast<std::size_t>(index)]) *
                   av_q2d(format->streams[index]->time_base);
        };
        if (!(stream_seconds(aac_index) < stream_seconds(video_index)) ||
            !(stream_seconds(opus_index) > stream_seconds(video_index))) {
            throw std::runtime_error("audio timestamps were not aligned to the shared video origin");
        }
    } catch (...) {
        avformat_close_input(&format);
        throw;
    }
    avformat_close_input(&format);
}

void verify_audio_video_mux(const std::filesystem::path& path) {
    std::error_code filesystem_error;
    std::filesystem::remove(path, filesystem_error);

    constexpr std::uint64_t video_origin_ns = 20'000'000'000ULL;
    Encoder video_encoder;
    auto video_packets = video_encoder.encode();
    for (std::size_t index = 0; index < video_packets.size(); ++index) {
        auto& packet = video_packets[index];
        packet.pts = static_cast<std::int64_t>(index) * 40'000;
        packet.dts = packet.pts;
        packet.duration = 40'000;
        packet.time_base = {1, 1'000'000};
    }

    AudioEncoder desktop_encoder(fluxrec::media::AudioCodec::aac);
    AudioEncoder microphone_encoder(fluxrec::media::AudioCodec::opus);
    auto desktop_packets = desktop_encoder.encode(0, video_origin_ns - 20'000'000ULL, 8);
    auto microphone_packets = microphone_encoder.encode(1, video_origin_ns + 20'000'000ULL, 8);

    fluxrec::mux::MuxConfig config;
    config.output_path = path.string();
    config.codec = fluxrec::media::VideoCodec::h264;
    config.width = 64;
    config.height = 64;
    config.stream_time_base = {1, 1000};
    config.frame_rate = {25, 1};
    config.bit_rate = video_encoder.context()->bit_rate;
    if (video_encoder.context()->extradata != nullptr &&
        video_encoder.context()->extradata_size > 0) {
        config.codec_extradata.assign(video_encoder.context()->extradata,
                                      video_encoder.context()->extradata +
                                          video_encoder.context()->extradata_size);
    }
    config.audio_streams = {desktop_encoder.config(), microphone_encoder.config()};
    config.video_origin_time_ns = video_origin_ns;
    config.queue_capacity_packets = 64;
    config.queue_capacity_bytes = 2U * 1024U * 1024U;

    fluxrec::mux::MuxWriter writer(std::move(config));
    if (!writer.wait_until_ready(std::chrono::seconds(5))) {
        throw std::runtime_error("audio-video mux worker did not become ready: " +
                                 writer.error_message());
    }

    std::exception_ptr producer_error;
    std::mutex error_mutex;
    std::atomic<bool> microphone_allowed{false};
    const auto capture_error = [&] {
        std::lock_guard lock(error_mutex);
        if (!producer_error) {
            producer_error = std::current_exception();
        }
    };
    std::thread video_thread([&] {
        try {
            for (auto& packet : video_packets) enqueue_with_retry(writer, packet);
        } catch (...) {
            capture_error();
        }
    });
    std::thread desktop_thread([&] {
        try {
            for (auto& packet : desktop_packets) enqueue_with_retry(writer, packet);
        } catch (...) {
            capture_error();
        }
    });
    std::thread microphone_thread([&] {
        try {
            while (!microphone_allowed.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            for (auto& packet : microphone_packets) enqueue_with_retry(writer, packet);
        } catch (...) {
            capture_error();
        }
    });
    video_thread.join();
    desktop_thread.join();
    bool interleaver_progressed = false;
    for (int attempt = 0; attempt < 500; ++attempt) {
        const auto progress = writer.stats();
        if (progress.queued_packets == 0 && progress.output_file_bytes > 0) {
            interleaver_progressed = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    microphone_allowed.store(true, std::memory_order_release);
    microphone_thread.join();
    if (producer_error) {
        std::rethrow_exception(producer_error);
    }
    if (!interleaver_progressed) {
        throw std::runtime_error("mux interleaver waited indefinitely for the delayed microphone track");
    }
    writer.close();
    if (writer.state() != fluxrec::mux::WriterState::closed) {
        throw std::runtime_error("audio-video mux worker failed: " + writer.error_message());
    }
    const auto stats = writer.stats();
    const auto expected_packets = video_packets.size() + desktop_packets.size() +
                                 microphone_packets.size();
    const auto expected_audio_packets = desktop_packets.size() + microphone_packets.size();
    if (stats.enqueued_packets != expected_packets || stats.muxed_packets != expected_packets ||
        stats.muxed_video_packets != video_packets.size() ||
        stats.muxed_packets - stats.muxed_video_packets != expected_audio_packets ||
        stats.queue_full_drops != 0) {
        throw std::runtime_error(
            "mixed mux counts did not separate video frames from audio packets");
    }
    verify_audio_video_file(path, video_packets.size(), desktop_packets.size(),
                            microphone_packets.size());
}

void test_audio_track_counts() {
    constexpr std::uint64_t video_origin_time_ns = 20'000'000'000ULL;
    struct FfmpegErrorLogs {
        FfmpegErrorLogs() : previous(av_log_get_level()) { av_log_set_level(AV_LOG_ERROR); }
        ~FfmpegErrorLogs() { av_log_set_level(previous); }
        int previous;
    } ffmpeg_error_logs;
    Encoder video_encoder;

    const auto make_config = [&](const std::filesystem::path& path,
                                 std::vector<fluxrec::mux::AudioStreamConfig> audio_streams) {
        fluxrec::mux::MuxConfig config;
        config.output_path = path.string();
        config.codec = fluxrec::media::VideoCodec::h264;
        config.width = 64;
        config.height = 64;
        config.stream_time_base = {1, 1000};
        config.frame_rate = {25, 1};
        config.bit_rate = video_encoder.context()->bit_rate;
        if (video_encoder.context()->extradata != nullptr &&
            video_encoder.context()->extradata_size > 0) {
            config.codec_extradata.assign(video_encoder.context()->extradata,
                                          video_encoder.context()->extradata +
                                              video_encoder.context()->extradata_size);
        }
        config.audio_streams = std::move(audio_streams);
        config.video_origin_time_ns = video_origin_time_ns;
        config.queue_capacity_packets = 64;
        config.queue_capacity_bytes = 2U * 1024U * 1024U;
        return config;
    };

    fluxrec::mux::AudioStreamConfig reused_audio_config;
    for (const std::size_t track_count : {std::size_t{1}, std::size_t{8}}) {
        std::vector<std::unique_ptr<AudioEncoder>> encoders;
        std::vector<fluxrec::mux::AudioStreamConfig> audio_streams;
        encoders.reserve(track_count);
        audio_streams.reserve(track_count);
        for (std::size_t index = 0; index < track_count; ++index) {
            encoders.push_back(std::make_unique<AudioEncoder>(fluxrec::media::AudioCodec::aac));
            audio_streams.push_back(encoders.back()->config());
        }
        reused_audio_config = audio_streams.front();

        const auto path = std::filesystem::temp_directory_path() /
                          ("fluxrec_mux_tracks_" + std::to_string(track_count) + ".mkv");
        std::error_code filesystem_error;
        std::filesystem::remove(path, filesystem_error);

        fluxrec::mux::MuxWriter writer(make_config(path, audio_streams));
        if (!writer.wait_until_ready(std::chrono::seconds(5))) {
            throw std::runtime_error("audio track-count mux worker did not become ready: " +
                                     writer.error_message());
        }

        fluxrec::media::EncodedAudioPacket out_of_range;
        bool have_out_of_range = false;
        for (std::size_t index = 0; index < track_count; ++index) {
            auto packets = encoders[index]->encode(index, video_origin_time_ns, 1);
            if (track_count == 1 && !packets.empty()) {
                out_of_range = packets.front();
                out_of_range.stream_index = track_count;
                have_out_of_range = true;
            }
            for (auto& packet : packets) {
                enqueue_with_retry(writer, packet);
            }
        }
        if (have_out_of_range &&
            writer.try_enqueue(std::move(out_of_range)) !=
                fluxrec::mux::EnqueueResult::invalid_packet) {
            throw std::runtime_error(
                "mux accepted an audio packet with an out-of-range stream_index");
        }

        writer.close();
        if (writer.state() != fluxrec::mux::WriterState::closed) {
            throw std::runtime_error("audio track-count mux worker failed: " +
                                     writer.error_message());
        }

        AVFormatContext* format = nullptr;
        check(avformat_open_input(&format, path.c_str(), nullptr, nullptr),
              "open multi-track Matroska test file");
        try {
            if (format->nb_streams != track_count + 1) {
                throw std::runtime_error("muxed file stream count did not match configured tracks");
            }
            for (std::size_t index = 0; index < track_count; ++index) {
                if (format->streams[index + 1]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
                    throw std::runtime_error("configured audio track was not muxed as audio");
                }
            }
        } catch (...) {
            avformat_close_input(&format);
            throw;
        }
        avformat_close_input(&format);
        std::filesystem::remove(path, filesystem_error);
    }

    const auto nine_path = std::filesystem::temp_directory_path() / "fluxrec_mux_tracks_9.mkv";
    try {
        fluxrec::mux::MuxWriter writer(make_config(
            nine_path, std::vector<fluxrec::mux::AudioStreamConfig>(9, reused_audio_config)));
        throw std::runtime_error("mux accepted nine audio streams");
    } catch (const std::invalid_argument& error) {
        if (std::string(error.what()).find("at most eight") == std::string::npos) {
            throw std::runtime_error(
                std::string("nine-stream mux error did not mention the eight-stream limit: ") +
                error.what());
        }
    }
}

} // namespace

namespace {

void verify_rebaser_offset_and_minimal_dts_correction() {
    constexpr std::int64_t monotonic_origin_us = 7'123'456'789'012;
    constexpr AVRational source_time_base{1, 1'000'000};
    constexpr AVRational stream_time_base{1, 1000};
    fluxrec::mux::detail::TimestampRebaser rebaser;

    const auto normalize = [&rebaser](std::int64_t dts_us) {
        AVPacket packet{};
        packet.dts = dts_us;
        packet.pts = dts_us + 5'000;
        av_packet_rescale_ts(&packet, source_time_base, stream_time_base);
        rebaser.apply(&packet);
        if (packet.pts - packet.dts != 5) {
            throw std::runtime_error("timestamp correction changed the PTS-DTS offset");
        }
        return packet;
    };

    const auto first = normalize(monotonic_origin_us);
    const auto second = normalize(monotonic_origin_us + 200);
    const auto third = normalize(monotonic_origin_us + 500);
    if (first.dts != 0 || second.dts != 1 || third.dts != 2) {
        throw std::runtime_error("timestamp rebasing did not use the first DTS and minimally repair collisions");
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        av_log_set_level(AV_LOG_ERROR);
        verify_rebaser_offset_and_minimal_dts_correction();
        const auto output = argc > 1
            ? std::filesystem::path(argv[1])
            : std::filesystem::temp_directory_path() / "fluxrec_mux_integration.mkv";
        std::error_code filesystem_error;
        std::filesystem::remove(output, filesystem_error);

        Encoder encoder;
        auto packets = encoder.encode();
        const auto* codec = encoder.context();

        // Model an encoder using CLOCK_MONOTONIC microseconds, as the live
        // capture path does. Compress frame intervals below Matroska's 1 ms
        // timebase so adjacent timestamps collide after rescaling.
        constexpr std::int64_t monotonic_origin_us = 7'123'456'789'012;
        std::int64_t frame_index = 0;
        for (auto& packet : packets) {
            packet.dts = monotonic_origin_us + frame_index * 500;
            packet.pts = packet.dts;
            packet.duration = 500;
            packet.time_base = {1, 1'000'000};
            ++frame_index;
        }

        fluxrec::mux::MuxConfig config;
        std::array<std::atomic<std::uint64_t>,
                   static_cast<std::size_t>(fluxrec::telemetry::Timing::count)> timing_samples{};
        config.output_path = output.string();
        config.codec = fluxrec::media::VideoCodec::h264;
        config.width = 64;
        config.height = 64;
        config.stream_time_base = {1, 1000};
        config.frame_rate = {30, 1};
        config.bit_rate = codec->bit_rate;
        if (codec->extradata != nullptr && codec->extradata_size > 0) {
            config.codec_extradata.assign(codec->extradata,
                                          codec->extradata + codec->extradata_size);
        }
        config.queue_capacity_packets = 2;
        config.queue_capacity_bytes = 2U * 1024U * 1024U;
        config.timing_observer = [&timing_samples](fluxrec::telemetry::Timing timing,
                                                   std::chrono::nanoseconds) {
            const auto index = static_cast<std::size_t>(timing);
            if (index < timing_samples.size()) {
                timing_samples[index].fetch_add(1, std::memory_order_relaxed);
            }
        };

        fluxrec::mux::MuxWriter writer(std::move(config));
        if (!writer.wait_until_ready(std::chrono::seconds(5))) {
            throw std::runtime_error("mux worker did not become ready: " + writer.error_message());
        }
        fluxrec::media::EncodedPacket missing_pts;
        missing_pts.data = {0x00};
        missing_pts.codec = fluxrec::media::VideoCodec::h264;
        missing_pts.pts = AV_NOPTS_VALUE;
        missing_pts.dts = 0;
        missing_pts.time_base = {1, 30};
        if (writer.try_enqueue(std::move(missing_pts)) !=
            fluxrec::mux::EnqueueResult::invalid_packet) {
            throw std::runtime_error("mux accepted an encoded video packet without PTS");
        }
        fluxrec::media::EncodedPacket missing_dts;
        missing_dts.data = {0x00};
        missing_dts.codec = fluxrec::media::VideoCodec::h264;
        missing_dts.pts = 0;
        missing_dts.dts = AV_NOPTS_VALUE;
        missing_dts.time_base = {1, 30};
        if (writer.try_enqueue(std::move(missing_dts)) !=
            fluxrec::mux::EnqueueResult::invalid_packet) {
            throw std::runtime_error("mux accepted an encoded video packet without DTS");
        }
        const auto expected_packets = packets.size();
        for (auto& packet : packets) {
            for (;;) {
                const auto result = writer.try_enqueue(std::move(packet));
                if (result == fluxrec::mux::EnqueueResult::accepted) {
                    break;
                }
                if (result == fluxrec::mux::EnqueueResult::queue_full ||
                    result == fluxrec::mux::EnqueueResult::queue_busy) {
                    // The test deliberately uses a tiny bounded queue; retrying
                    // here is test-driver behavior, never the mux writer's policy.
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
                throw std::runtime_error("mux enqueue failed with result " +
                                         std::to_string(static_cast<int>(result)) + ": " +
                                         writer.error_message());
            }
        }
        writer.close();
        if (writer.state() != fluxrec::mux::WriterState::closed) {
            throw std::runtime_error("mux worker failed: " + writer.error_message());
        }
        const auto stats = writer.stats();
        if (stats.muxed_packets != expected_packets || stats.invalid_packets != 2 ||
            stats.output_file_bytes == 0 ||
            stats.queue_high_water_packets == 0 ||
            stats.queue_wait_samples != expected_packets || stats.disk_write_calls == 0 ||
            stats.total_disk_write_latency_ns < stats.max_disk_write_latency_ns ||
            timing_samples[static_cast<std::size_t>(fluxrec::telemetry::Timing::mux_queue_wait)] !=
                expected_packets ||
            timing_samples[static_cast<std::size_t>(fluxrec::telemetry::Timing::mux)] !=
                expected_packets ||
            timing_samples[static_cast<std::size_t>(fluxrec::telemetry::Timing::disk_write)] !=
                stats.disk_write_calls) {
            throw std::runtime_error("mux statistics did not record output packets and file size");
        }
        verify_file(output, expected_packets);
        auto audio_video_output = output;
        audio_video_output.replace_filename(output.stem().string() + "_av.mkv");
        verify_audio_video_mux(audio_video_output);
        test_audio_track_counts();
        std::cout << "mux integration passed: " << stats.muxed_packets << " packets, "
                  << stats.output_file_bytes << " bytes, max mux latency "
                  << stats.max_mux_latency_ns << " ns\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "mux integration failed: " << exception.what() << '\n';
        return 1;
    }
}
