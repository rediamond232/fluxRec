#include "engine/replay_export.hpp"
#include "audio/detail/aac_padding_tracker.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
}

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <array>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
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
    explicit Encoder(bool enable_b_frames = false) : enable_b_frames_(enable_b_frames) {
        const AVCodec* codec = avcodec_find_encoder_by_name("libx264");
        if (codec == nullptr) {
            throw std::runtime_error("test requires FFmpeg's libx264 encoder");
        }
        context_ = avcodec_alloc_context3(codec);
        if (context_ == nullptr) {
            throw std::runtime_error("allocate synthetic H.264 encoder context");
        }
        context_->width = width;
        context_->height = height;
        context_->pix_fmt = AV_PIX_FMT_YUV420P;
        context_->time_base = AVRational{1, 30};
        context_->framerate = AVRational{30, 1};
        context_->gop_size = 15;
        context_->max_b_frames = enable_b_frames_ ? 2 : 0;
        context_->bit_rate = 300'000;
        context_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        check(av_opt_set(context_->priv_data, "preset", "ultrafast", 0), "set x264 preset");
        if (!enable_b_frames_) {
            check(av_opt_set(context_->priv_data, "tune", "zerolatency", 0), "set x264 tune");
        }
        check(avcodec_open2(context_, codec, nullptr), "open synthetic H.264 encoder");

        frame_ = av_frame_alloc();
        if (frame_ == nullptr) {
            throw std::runtime_error("allocate synthetic encoder frame");
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

    [[nodiscard]] std::vector<fluxrec::media::EncodedPacket> encode(int frame_count) {
        std::vector<fluxrec::media::EncodedPacket> result;
        for (int frame_index = 0; frame_index < frame_count; ++frame_index) {
            check(av_frame_make_writable(frame_), "prepare synthetic frame");
            for (int y = 0; y < height; ++y) {
                auto* row = frame_->data[0] + y * frame_->linesize[0];
                for (int x = 0; x < width; ++x) {
                    row[x] = static_cast<std::uint8_t>((x * 3 + y * 5 + frame_index * 11) & 0xff);
                }
            }
            for (int y = 0; y < height / 2; ++y) {
                auto* u_row = frame_->data[1] + y * frame_->linesize[1];
                auto* v_row = frame_->data[2] + y * frame_->linesize[2];
                for (int x = 0; x < width / 2; ++x) {
                    u_row[x] = static_cast<std::uint8_t>(96 + ((frame_index + x) & 31));
                    v_row[x] = static_cast<std::uint8_t>(160 - ((frame_index + y) & 31));
                }
            }
            frame_->pts = frame_index;
            check(avcodec_send_frame(context_, frame_), "submit synthetic frame");
            drain(result);
        }
        check(avcodec_send_frame(context_, nullptr), "flush synthetic encoder");
        drain(result);
        if (result.empty()) {
            throw std::runtime_error("synthetic encoder produced no packets");
        }
        return result;
    }

private:
    void drain(std::vector<fluxrec::media::EncodedPacket>& destination) {
        AVPacket* packet = av_packet_alloc();
        if (packet == nullptr) {
            throw std::runtime_error("allocate synthetic output packet");
        }
        for (;;) {
            const int received = avcodec_receive_packet(context_, packet);
            if (received == AVERROR(EAGAIN) || received == AVERROR_EOF) {
                break;
            }
            check(received, "receive synthetic H.264 packet");
            fluxrec::media::EncodedPacket encoded;
            encoded.data.assign(packet->data, packet->data + packet->size);
            encoded.codec = fluxrec::media::VideoCodec::h264;
            encoded.pts = packet->pts;
            encoded.dts = packet->dts;
            encoded.duration = packet->duration > 0 ? packet->duration : 1;
            encoded.keyframe = (packet->flags & AV_PKT_FLAG_KEY) != 0;
            encoded.time_base = {context_->time_base.num, context_->time_base.den};
            destination.push_back(std::move(encoded));
            av_packet_unref(packet);
        }
        av_packet_free(&packet);
    }

    static constexpr int width = 64;
    static constexpr int height = 64;
    bool enable_b_frames_{};
    AVCodecContext* context_{};
    AVFrame* frame_{};
};

class AudioEncoder {
public:
    AudioEncoder() {
        const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
        if (codec == nullptr) throw std::runtime_error("test requires FFmpeg's AAC encoder");
        context_ = avcodec_alloc_context3(codec);
        if (context_ == nullptr) throw std::runtime_error("allocate synthetic AAC encoder");
        context_->sample_rate = sample_rate;
        context_->time_base = AVRational{1, sample_rate};
        context_->bit_rate = 96'000;
        context_->sample_fmt = AV_SAMPLE_FMT_FLTP;
        context_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        av_channel_layout_default(&context_->ch_layout, channels);
        check(avcodec_open2(context_, codec, nullptr), "open synthetic AAC encoder");

        config_.codec = fluxrec::media::AudioCodec::aac;
        config_.sample_rate = context_->sample_rate;
        config_.channels = context_->ch_layout.nb_channels;
        config_.bit_rate = context_->bit_rate;
        config_.initial_padding = context_->initial_padding;
        config_.stream_time_base = {context_->time_base.num, context_->time_base.den};
        if (context_->extradata != nullptr && context_->extradata_size > 0) {
            config_.codec_extradata.assign(context_->extradata,
                                           context_->extradata + context_->extradata_size);
        }
        frame_size_ = context_->frame_size > 0 ? context_->frame_size : 1024;
    }

    ~AudioEncoder() { avcodec_free_context(&context_); }
    AudioEncoder(const AudioEncoder&) = delete;
    AudioEncoder& operator=(const AudioEncoder&) = delete;

    [[nodiscard]] const fluxrec::mux::AudioStreamConfig& config() const noexcept {
        return config_;
    }

    [[nodiscard]] std::vector<fluxrec::media::EncodedAudioPacket> encode(
        std::size_t stream_index, std::uint64_t start_time_ns, int frame_count,
        int final_valid_sample_frames = -1) {
        std::vector<fluxrec::media::EncodedAudioPacket> result;
        fluxrec::audio::detail::AacPaddingTracker tail_padding;
        const auto start_pts = av_rescale_q(static_cast<std::int64_t>(start_time_ns),
                                            AVRational{1, 1'000'000'000}, context_->time_base);
        for (int index = 0; index < frame_count; ++index) {
            AVFrame* frame = av_frame_alloc();
            if (frame == nullptr) throw std::runtime_error("allocate synthetic AAC frame");
            frame->format = context_->sample_fmt;
            frame->sample_rate = context_->sample_rate;
            frame->nb_samples = frame_size_;
            frame->pts = start_pts + static_cast<std::int64_t>(index) * frame_size_;
            check(av_channel_layout_copy(&frame->ch_layout, &context_->ch_layout),
                  "copy synthetic AAC channel layout");
            int result_code = av_frame_get_buffer(frame, 0);
            if (result_code >= 0) {
                result_code = av_samples_set_silence(frame->extended_data, 0, frame_size_,
                                                     channels, context_->sample_fmt);
            }
            if (result_code < 0) {
                av_frame_free(&frame);
                check(result_code, "allocate synthetic AAC samples");
            }
            const int valid_samples = index == frame_count - 1 &&
                    final_valid_sample_frames >= 0
                ? final_valid_sample_frames : frame_size_;
            if (valid_samples < 0 || valid_samples > frame_size_) {
                av_frame_free(&frame);
                throw std::runtime_error("invalid synthetic AAC partial-frame sample count");
            }
            for (int channel = 0; channel < channels; ++channel) {
                auto* samples = reinterpret_cast<float*>(frame->extended_data[channel]);
                for (int sample = 0; sample < valid_samples; ++sample) {
                    const auto absolute_sample =
                        static_cast<std::uint64_t>(index) * frame_size_ +
                        static_cast<std::uint64_t>(sample);
                    samples[sample] = 0.25F * std::sin(
                        static_cast<float>(absolute_sample) * 0.0575958653F);
                }
            }
            if (valid_samples < frame_size_ &&
                !tail_padding.remember(frame->pts,
                    static_cast<std::uint32_t>(frame_size_ - valid_samples))) {
                av_frame_free(&frame);
                throw std::runtime_error("could not track synthetic AAC tail padding");
            }
            const auto sent = avcodec_send_frame(context_, frame);
            av_frame_free(&frame);
            check(sent, "submit synthetic AAC frame");
            drain(result, stream_index, tail_padding);
        }
        check(avcodec_send_frame(context_, nullptr), "flush synthetic AAC encoder");
        drain(result, stream_index, tail_padding);
        if (tail_padding.size() != 0) {
            throw std::runtime_error("AAC encoder flush did not return the padded frame packet");
        }
        if (result.empty()) throw std::runtime_error("synthetic AAC encoder produced no packets");
        return result;
    }

private:
    void drain(std::vector<fluxrec::media::EncodedAudioPacket>& destination,
               std::size_t stream_index,
               fluxrec::audio::detail::AacPaddingTracker& tail_padding) {
        AVPacket* packet = av_packet_alloc();
        if (packet == nullptr) throw std::runtime_error("allocate synthetic AAC packet");
        for (;;) {
            const int received = avcodec_receive_packet(context_, packet);
            if (received == AVERROR(EAGAIN) || received == AVERROR_EOF) break;
            check(received, "receive synthetic AAC packet");
            fluxrec::media::EncodedAudioPacket encoded;
            encoded.stream_index = stream_index;
            encoded.data.assign(packet->data, packet->data + packet->size);
            encoded.codec = fluxrec::media::AudioCodec::aac;
            encoded.pts = packet->pts;
            encoded.duration = packet->duration > 0 ? packet->duration : frame_size_;
            encoded.capture_time_ns = static_cast<std::uint64_t>(av_rescale_q(
                packet->pts, context_->time_base, AVRational{1, 1'000'000'000}));
            if (packet->pts != AV_NOPTS_VALUE) {
                encoded.discard_padding_samples = tail_padding.take(packet->pts);
            }
            encoded.keyframe = (packet->flags & AV_PKT_FLAG_KEY) != 0;
            encoded.time_base = {context_->time_base.num, context_->time_base.den};
            destination.push_back(std::move(encoded));
            av_packet_unref(packet);
        }
        av_packet_free(&packet);
    }

    static constexpr int sample_rate = 48'000;
    static constexpr int channels = 2;
    AVCodecContext* context_{};
    fluxrec::mux::AudioStreamConfig config_;
    int frame_size_{1024};
};

void verify_single_frame_aac_tail_padding() {
    constexpr std::uint64_t origin_ns = 20'000'000'000ULL;
    constexpr int valid_sample_frames = 476;
    constexpr auto padding_samples = static_cast<std::uint32_t>(1024 - valid_sample_frames);
    AudioEncoder encoder;
    const auto packets = encoder.encode(0, origin_ns, 1, valid_sample_frames);
    const auto submitted_pts = av_rescale_q(
        static_cast<std::int64_t>(origin_ns), AVRational{1, 1'000'000'000},
        AVRational{1, 48'000});
    const auto priming_pts = submitted_pts - encoder.config().initial_padding;
    const auto priming = std::find_if(packets.begin(), packets.end(),
        [priming_pts](const auto& packet) { return packet.pts == priming_pts; });
    const auto padded = std::find_if(packets.begin(), packets.end(),
        [submitted_pts](const auto& packet) { return packet.pts == submitted_pts; });
    const auto marked_count = std::count_if(packets.begin(), packets.end(),
        [](const auto& packet) { return packet.discard_padding_samples != 0; });
    if (priming == packets.end() || priming->discard_padding_samples != 0 ||
        padded == packets.end() || padded->discard_padding_samples != padding_samples ||
        marked_count != 1) {
        throw std::runtime_error(
            "single-frame AAC priming packet consumed or lost the tail-padding marker");
    }
}

void verify_export(const std::filesystem::path& path, std::size_t expected_packets) {
    AVFormatContext* format = nullptr;
    check(avformat_open_input(&format, path.c_str(), nullptr, nullptr), "open replay export");
    try {
        check(avformat_find_stream_info(format, nullptr), "read replay export stream info");
        if (format->nb_streams != 1 ||
            format->streams[0]->codecpar->codec_id != AV_CODEC_ID_H264 ||
            format->streams[0]->codecpar->width != 64 ||
            format->streams[0]->codecpar->height != 64) {
            throw std::runtime_error("replay export stream metadata did not round-trip");
        }

        AVPacket* packet = av_packet_alloc();
        if (packet == nullptr) {
            throw std::runtime_error("allocate export verification packet");
        }
        std::size_t packet_count = 0;
        std::int64_t first_pts = AV_NOPTS_VALUE;
        std::int64_t first_dts = AV_NOPTS_VALUE;
        std::int64_t previous_dts = AV_NOPTS_VALUE;
        std::int64_t last_dts = AV_NOPTS_VALUE;
        bool saw_keyframe = false;
        while (av_read_frame(format, packet) >= 0) {
            if (packet->stream_index == 0) {
                ++packet_count;
                if (packet->pts == AV_NOPTS_VALUE || packet->dts == AV_NOPTS_VALUE ||
                    (previous_dts != AV_NOPTS_VALUE && packet->dts <= previous_dts)) {
                    av_packet_free(&packet);
                    throw std::runtime_error("replay export timestamps are not strictly ordered");
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
            throw std::runtime_error("replay export packet count differs from its snapshot");
        }
        if (first_dts != 0 || first_pts != 0 || last_dts < 990 || last_dts > 1010 ||
            !saw_keyframe) {
            throw std::runtime_error("replay export did not rebase timestamps or preserve its keyframe");
        }
    } catch (...) {
        avformat_close_input(&format);
        throw;
    }
    avformat_close_input(&format);
}

void verify_audio_export(const std::filesystem::path& path,
                         std::size_t expected_video_packets,
                         std::size_t expected_audio_packets,
                         double expected_video_pts_dts_offset_ms,
                         int expected_aac_initial_padding,
                         std::uint32_t expected_discard_padding_samples) {
    AVFormatContext* format = nullptr;
    check(avformat_open_input(&format, path.c_str(), nullptr, nullptr),
          "open replay audio export");
    try {
        check(avformat_find_stream_info(format, nullptr), "read replay audio stream info");
        if (format->nb_streams != 2) {
            throw std::runtime_error("audio replay export did not contain two tracks");
        }
        int video_index = -1;
        int audio_index = -1;
        for (unsigned int index = 0; index < format->nb_streams; ++index) {
            const auto codec = format->streams[index]->codecpar->codec_id;
            if (codec == AV_CODEC_ID_H264) video_index = static_cast<int>(index);
            if (codec == AV_CODEC_ID_AAC) audio_index = static_cast<int>(index);
        }
        if (video_index < 0 || audio_index < 0) {
            throw std::runtime_error("audio replay export codec metadata did not round-trip");
        }
        const auto* audio_parameters = format->streams[audio_index]->codecpar;
        if (audio_parameters->initial_padding != expected_aac_initial_padding) {
            throw std::runtime_error(
                "Matroska AAC CodecDelay did not round-trip as encoder initial padding");
        }
        const auto codec_delay_ns = av_rescale_q(
            audio_parameters->initial_padding,
            AVRational{1, audio_parameters->sample_rate},
            AVRational{1, 1'000'000'000});
        if (expected_aac_initial_padding != 1024 || codec_delay_ns != 21'333'333) {
            throw std::runtime_error(
                "AAC fixture did not preserve the expected 21.333333 ms CodecDelay");
        }

        std::array<std::size_t, 2> counts{};
        std::array<std::int64_t, 2> previous_dts{AV_NOPTS_VALUE, AV_NOPTS_VALUE};
        std::int64_t first_video_dts = AV_NOPTS_VALUE;
        std::int64_t first_video_pts = AV_NOPTS_VALUE;
        std::int64_t last_video_pts = AV_NOPTS_VALUE;
        std::int64_t first_audio_dts = AV_NOPTS_VALUE;
        std::int64_t first_audio_pts = AV_NOPTS_VALUE;
        std::int64_t last_audio_pts = AV_NOPTS_VALUE;
        std::int64_t keyframe_pts = AV_NOPTS_VALUE;
        std::int64_t previous_global_dts_us = std::numeric_limits<std::int64_t>::min();
        bool saw_preserved_keyframe_offset = false;
        std::size_t discard_padding_packet_count = 0;
        std::uint32_t observed_discard_padding_samples = 0;
        std::int64_t discard_padding_packet_pts = AV_NOPTS_VALUE;
        AVPacket* packet = av_packet_alloc();
        if (packet == nullptr) throw std::runtime_error("allocate replay audio packet");
        while (av_read_frame(format, packet) >= 0) {
            const auto stream_index = packet->stream_index;
            const auto slot = stream_index == video_index ? 0U : 1U;
            ++counts[slot];
            if (packet->pts == AV_NOPTS_VALUE ||
                (packet->dts != AV_NOPTS_VALUE && packet->dts < 0) ||
                (packet->dts != AV_NOPTS_VALUE && previous_dts[slot] != AV_NOPTS_VALUE &&
                 packet->dts <= previous_dts[slot])) {
                av_packet_free(&packet);
                throw std::runtime_error("replay audio export has invalid or negative DTS");
            }
            if (packet->dts != AV_NOPTS_VALUE) previous_dts[slot] = packet->dts;
            if (packet->dts != AV_NOPTS_VALUE) {
                const auto global_dts_us = av_rescale_q(
                    packet->dts, format->streams[stream_index]->time_base,
                    AVRational{1, 1'000'000});
                if (global_dts_us < previous_global_dts_us) {
                    av_packet_free(&packet);
                    throw std::runtime_error("replay export packets were not interleaved by DTS");
                }
                previous_global_dts_us = global_dts_us;
            }
            if (stream_index == video_index) {
                if (first_video_pts == AV_NOPTS_VALUE) {
                    first_video_dts = packet->dts;
                    first_video_pts = packet->pts;
                }
                last_video_pts = packet->pts;
                if (keyframe_pts == AV_NOPTS_VALUE &&
                    (packet->flags & AV_PKT_FLAG_KEY) != 0) {
                    keyframe_pts = packet->pts;
                }
                if ((packet->flags & AV_PKT_FLAG_KEY) != 0 &&
                    packet->pts != AV_NOPTS_VALUE && packet->dts != AV_NOPTS_VALUE) {
                    const auto time_base = format->streams[video_index]->time_base;
                    const double output_offset_ms =
                        static_cast<double>(packet->pts - packet->dts) *
                        av_q2d(time_base) * 1000.0;
                    if (std::abs(output_offset_ms - expected_video_pts_dts_offset_ms) <= 1.1) {
                        saw_preserved_keyframe_offset = true;
                    }
                }
            } else if (first_audio_pts == AV_NOPTS_VALUE) {
                first_audio_dts = packet->dts;
                first_audio_pts = packet->pts;
                last_audio_pts = packet->pts;
            } else {
                last_audio_pts = packet->pts;
            }
            if (stream_index == audio_index) {
                for (int side_index = 0; side_index < packet->side_data_elems; ++side_index) {
                    const auto& side_data = packet->side_data[side_index];
                    if (side_data.type != AV_PKT_DATA_SKIP_SAMPLES ||
                        side_data.size < 10) {
                        continue;
                    }
                    const auto* data = side_data.data;
                    const auto discard_samples =
                        static_cast<std::uint32_t>(data[4]) |
                        (static_cast<std::uint32_t>(data[5]) << 8U) |
                        (static_cast<std::uint32_t>(data[6]) << 16U) |
                        (static_cast<std::uint32_t>(data[7]) << 24U);
                    if (discard_samples != 0) {
                        ++discard_padding_packet_count;
                        observed_discard_padding_samples = discard_samples;
                        discard_padding_packet_pts = packet->pts;
                    }
                }
            }
            av_packet_unref(packet);
        }
        av_packet_free(&packet);
        if (counts[0] != expected_video_packets || counts[1] != expected_audio_packets ||
            first_video_pts <= 0 || (first_video_dts != AV_NOPTS_VALUE && first_video_dts < 0) ||
            keyframe_pts == AV_NOPTS_VALUE ||
            (first_audio_dts != AV_NOPTS_VALUE && first_audio_dts < 0) ||
            first_audio_pts == AV_NOPTS_VALUE || last_video_pts == AV_NOPTS_VALUE ||
            last_audio_pts == AV_NOPTS_VALUE || !saw_preserved_keyframe_offset) {
            throw std::runtime_error("audio replay export packet counts or DTS values changed");
        }
        if (discard_padding_packet_count != 1 ||
            observed_discard_padding_samples != expected_discard_padding_samples ||
            discard_padding_packet_pts != last_audio_pts) {
            throw std::runtime_error(
                "Matroska AAC DiscardPadding did not round-trip on the final audio packet");
        }

        const auto to_ms = [](std::int64_t timestamp, AVRational time_base) {
            return static_cast<double>(timestamp) * av_q2d(time_base) * 1000.0;
        };
        const auto video_keyframe_ms = to_ms(
            keyframe_pts, format->streams[video_index]->time_base);
        const auto video_first_ms = to_ms(
            first_video_pts, format->streams[video_index]->time_base);
        const auto video_last_ms = to_ms(
            last_video_pts, format->streams[video_index]->time_base);
        const auto audio_first_ms = to_ms(
            first_audio_pts, format->streams[audio_index]->time_base);
        const auto audio_last_ms = to_ms(
            last_audio_pts, format->streams[audio_index]->time_base);
        if (audio_first_ms + 1.0 < video_keyframe_ms ||
            audio_first_ms - video_keyframe_ms > 30.0 ||
            audio_first_ms + 1.0 < video_first_ms ||
            audio_first_ms - video_first_ms > 30.0) {
            throw std::runtime_error("replay audio did not start in sync with the selected keyframe");
        }
        const double last_av_delta_ms = video_last_ms - audio_last_ms;
        if (last_av_delta_ms < -1.0 || last_av_delta_ms > 30.0) {
            throw std::runtime_error("replay audio and video do not stay aligned at the selected window end");
        }
    } catch (...) {
        avformat_close_input(&format);
        throw;
    }
    avformat_close_input(&format);
}

} // namespace

int main(int argc, char** argv) {
    try {
        verify_single_frame_aac_tail_padding();
        const auto output = argc > 1
            ? std::filesystem::path(argv[1])
            : std::filesystem::temp_directory_path() / "fluxrec_replay_export_integration.mkv";
        auto audio_output = output;
        audio_output.replace_filename(output.stem().string() + "_av.mkv");
        std::error_code filesystem_error;
        std::filesystem::remove(output, filesystem_error);
        std::filesystem::remove(audio_output, filesystem_error);

        Encoder encoder;
        auto packets = encoder.encode(61); // 2 seconds at 30 fps, including a keyframe at 1 second.
        const auto* codec_context = encoder.context();

        fluxrec::replay::ReplayBufferConfig replay_config;
        replay_config.history_duration = std::chrono::seconds(2);
        replay_config.max_keyframe_preroll = std::chrono::seconds(1);
        replay_config.max_bytes = 8U * 1024U * 1024U;
        replay_config.max_packets = 1024;
        fluxrec::replay::ReplayBuffer replay_buffer(replay_config);
        for (auto& packet : packets) {
            const auto append_result = replay_buffer.append(std::move(packet));
            if (append_result != fluxrec::replay::AppendResult::stored) {
                throw std::runtime_error("failed to fill replay buffer with encoded packets");
            }
        }

        const auto snapshot = replay_buffer.snapshot(std::chrono::seconds(1));
        if (!snapshot || snapshot.packet_count() != 31) {
            throw std::runtime_error("replay buffer did not produce the expected one-second GOP snapshot");
        }
        const auto expected_packets = snapshot.packet_count();

        fluxrec::mux::MuxConfig mux_config;
        mux_config.output_path = output.string();
        mux_config.codec = fluxrec::media::VideoCodec::h264;
        mux_config.width = 64;
        mux_config.height = 64;
        mux_config.stream_time_base = {1, 1000};
        mux_config.frame_rate = {30, 1};
        mux_config.bit_rate = codec_context->bit_rate;
        if (codec_context->extradata != nullptr && codec_context->extradata_size > 0) {
            mux_config.codec_extradata.assign(codec_context->extradata,
                                              codec_context->extradata + codec_context->extradata_size);
        }
        // A tiny packet queue makes the exporter exercise its worker-side
        // backpressure retry path without changing the capture producer.
        mux_config.queue_capacity_packets = 2;
        mux_config.queue_capacity_bytes = 2U * 1024U * 1024U;

        fluxrec::engine::ReplayExporter exporter(replay_buffer);
        const auto submission = exporter.submit(std::move(mux_config), std::chrono::seconds(1));
        if (!submission) {
            throw std::runtime_error("replay snapshot submission failed with status " +
                                     std::to_string(static_cast<int>(submission.status)));
        }
        exporter.close();

        fluxrec::engine::ReplayExportResult result;
        if (!exporter.try_pop_result(result) || !result.succeeded ||
            result.job_id != submission.job_id || result.snapshot_packets != expected_packets ||
            result.snapshot_video_packets != expected_packets || result.snapshot_audio_packets != 0 ||
            result.enqueued_packets != expected_packets || result.muxed_packets != expected_packets) {
            throw std::runtime_error("asynchronous replay export failed: " + result.error);
        }
        verify_export(output, expected_packets);

        const auto stats = exporter.stats();
        if (stats.submitted_jobs != 1 || stats.completed_jobs != 1 || stats.failed_jobs != 0 ||
            stats.in_flight_jobs != 0) {
            throw std::runtime_error("replay exporter counters did not report one completed job");
        }

        constexpr std::uint64_t audio_video_origin_ns = 20'000'000'000ULL;
        Encoder reordered_encoder(true);
        auto reordered_packets = reordered_encoder.encode(61);
        const auto& first_reordered = reordered_packets.front();
        if (!first_reordered.keyframe || first_reordered.pts == first_reordered.dts) {
            throw std::runtime_error("synthetic reordered-video fixture did not start with PTS != DTS");
        }
        fluxrec::replay::ReplayBuffer av_replay_buffer(replay_config);
        for (auto& video_packet : reordered_packets) {
            if (av_replay_buffer.append(std::move(video_packet)) !=
                fluxrec::replay::AppendResult::stored) {
                throw std::runtime_error("failed to append reordered video packets");
            }
        }
        AudioEncoder audio_encoder;
        constexpr int final_valid_samples = 476;
        constexpr auto expected_discard_padding_samples =
            static_cast<std::uint32_t>(1024 - final_valid_samples);
        auto audio_packets = audio_encoder.encode(
            0, audio_video_origin_ns, 94, final_valid_samples);
        const auto marked_tail_packets = std::count_if(
            audio_packets.begin(), audio_packets.end(), [](const auto& packet) {
                return packet.discard_padding_samples != 0;
            });
        if (marked_tail_packets != 1 ||
            audio_packets.back().discard_padding_samples != expected_discard_padding_samples) {
            throw std::runtime_error("synthetic AAC fixture did not mark exactly its padded final packet");
        }
        for (auto& audio_packet : audio_packets) {
            const auto relative_ns = static_cast<std::int64_t>(audio_packet.capture_time_ns) -
                                     static_cast<std::int64_t>(audio_video_origin_ns);
            if (av_replay_buffer.append_audio(std::move(audio_packet),
                    std::chrono::nanoseconds(relative_ns)) !=
                fluxrec::replay::AppendResult::stored) {
                throw std::runtime_error("failed to append synthetic AAC packets");
            }
        }

        const auto av_snapshot = av_replay_buffer.snapshot(std::chrono::seconds(1));
        if (!av_snapshot || av_snapshot.packet_count() != 31 ||
            av_snapshot.audio_packet_count() == 0) {
            throw std::runtime_error("audio replay snapshot did not contain its selected tracks");
        }
        std::int64_t selected_keyframe_pts = AV_NOPTS_VALUE;
        std::int64_t selected_keyframe_dts = AV_NOPTS_VALUE;
        bool snapshot_has_padded_audio_packet = false;
        av_snapshot.for_each_packet([&](const auto& video_packet) {
            if (selected_keyframe_pts == AV_NOPTS_VALUE) {
                selected_keyframe_pts = video_packet->pts;
                selected_keyframe_dts = video_packet->dts;
            }
        });
        if (selected_keyframe_pts == selected_keyframe_dts) {
            throw std::runtime_error("selected replay keyframe did not preserve its PTS/DTS offset");
        }
        if (selected_keyframe_dts == std::numeric_limits<std::int64_t>::min()) {
            throw std::runtime_error("selected replay keyframe has no DTS for the export fixture");
        }
        av_snapshot.for_each_audio_packet([&](const auto& audio_packet) {
            const auto relative_ns = static_cast<std::int64_t>(audio_packet->capture_time_ns) -
                                     static_cast<std::int64_t>(audio_video_origin_ns);
            if (relative_ns < av_snapshot.start_time().count() ||
                relative_ns > av_snapshot.end_time().count()) {
                throw std::runtime_error("replay snapshot selected AAC outside its video window");
            }
            snapshot_has_padded_audio_packet = snapshot_has_padded_audio_packet ||
                audio_packet->discard_padding_samples == expected_discard_padding_samples;
        });
        if (!snapshot_has_padded_audio_packet) {
            throw std::runtime_error("replay snapshot did not retain the padded AAC tail packet");
        }

        fluxrec::mux::MuxConfig av_mux_config;
        av_mux_config.output_path = audio_output.string();
        av_mux_config.codec = fluxrec::media::VideoCodec::h264;
        av_mux_config.width = 64;
        av_mux_config.height = 64;
        av_mux_config.stream_time_base = {1, 1000};
        av_mux_config.frame_rate = {30, 1};
        av_mux_config.bit_rate = reordered_encoder.context()->bit_rate;
        if (reordered_encoder.context()->extradata != nullptr &&
            reordered_encoder.context()->extradata_size > 0) {
            av_mux_config.codec_extradata.assign(
                reordered_encoder.context()->extradata,
                reordered_encoder.context()->extradata +
                    reordered_encoder.context()->extradata_size);
        }
        av_mux_config.audio_streams = {audio_encoder.config()};
        av_mux_config.video_origin_time_ns = audio_video_origin_ns;
        av_mux_config.queue_capacity_packets = 2;
        av_mux_config.queue_capacity_bytes = 2U * 1024U * 1024U;

        fluxrec::engine::ReplayExporter av_exporter(av_replay_buffer);
        const auto av_submission = av_exporter.submit(
            std::move(av_mux_config), std::chrono::seconds(1));
        if (!av_submission) {
            throw std::runtime_error("audio replay snapshot submission failed with status " +
                                     std::to_string(static_cast<int>(av_submission.status)));
        }
        av_exporter.close();
        fluxrec::engine::ReplayExportResult av_result;
        const auto expected_av_packets = av_snapshot.total_packet_count();
        if (!av_exporter.try_pop_result(av_result) || !av_result.succeeded ||
            av_result.job_id != av_submission.job_id ||
            av_result.snapshot_video_packets != av_snapshot.packet_count() ||
            av_result.snapshot_audio_packets != av_snapshot.audio_packet_count() ||
            av_result.snapshot_packets != expected_av_packets ||
            av_result.enqueued_packets != expected_av_packets ||
            av_result.muxed_packets != expected_av_packets) {
            throw std::runtime_error("audio replay export failed: " + av_result.error);
        }
        const double expected_video_pts_dts_offset_ms =
            static_cast<double>(selected_keyframe_pts - selected_keyframe_dts) *
            av_q2d(AVRational{reordered_packets.front().time_base.num,
                              reordered_packets.front().time_base.den}) * 1000.0;
        verify_audio_export(audio_output, av_snapshot.packet_count(),
                            av_snapshot.audio_packet_count(),
                            expected_video_pts_dts_offset_ms,
                            audio_encoder.config().initial_padding,
                            expected_discard_padding_samples);

        std::cout << "replay export integration passed: " << result.muxed_packets
                  << " video-only packets and " << av_result.muxed_packets
                  << " audio-video packets\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "replay export integration failed: " << exception.what() << '\n';
        return 1;
    }
}
