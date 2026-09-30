#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace fluxrec::media {

enum class VideoCodec {
    h264,
    hevc,
};

enum class AudioCodec {
    aac,
    opus,
};

struct Rational {
    int num{0};
    int den{1};

    [[nodiscard]] constexpr bool valid() const noexcept {
        return num > 0 && den > 0;
    }
};

// An encoded video packet. `data` contains only compressed codec data; raw
// frames are deliberately outside the mux/replay interfaces.
struct EncodedPacket {
    std::vector<std::uint8_t> data;
    VideoCodec codec{VideoCodec::h264};
    std::int64_t pts{0};
    std::int64_t dts{0};
    std::int64_t duration{0};
    bool keyframe{false};
    Rational time_base{1, 1};
};

// Encoded audio only; `stream_index` selects a configured MuxConfig audio
// stream. `pts` is not zero based: it uses the absolute CLOCK_MONOTONIC-derived
// sample clock emitted by audio::Capture. `capture_time_ns` records the same
// timestamp in nanoseconds. The mux subtracts the shared video origin from pts
// in this packet's timebase. Do not pass audio packets already rebased to zero.
struct EncodedAudioPacket {
    std::size_t stream_index{0};
    std::vector<std::uint8_t> data;
    AudioCodec codec{AudioCodec::aac};
    std::int64_t pts{0};
    std::int64_t duration{0};
    std::uint64_t capture_time_ns{0};
    // Per-channel samples at the end of this AAC packet added as silence to
    // complete a partial frame. MuxWriter writes this as Matroska
    // DiscardPadding via AV_PKT_DATA_SKIP_SAMPLES. Zero means no tail pad.
    std::uint32_t discard_padding_samples{0};
    bool keyframe{false};
    Rational time_base{1, 48'000};
};

} // namespace fluxrec::media
