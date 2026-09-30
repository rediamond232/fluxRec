// Offline, post-record decoder for stimulus/src/main.cpp's seven-segment ID.
//
// Build (using the system FFmpeg development packages):
//   c++ -std=c++23 -O2 tools/decode_stimulus_ids.cpp -o /tmp/decode-stimulus-ids $(pkg-config --cflags --libs libavformat libavcodec libavutil)
//
// This tool reads decoded luma samples from a saved video. It is not linked
// into Fluxrec's capture or recording path.
// Black frames are counted separately from malformed nonblack glyph frames;
// sequence comparisons use only consecutive valid frames in decoder order.

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/pixdesc.h>
}

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

// Segment bits follow the shader's a,b,c,d,e,f,g assignments.
constexpr std::array<uint8_t, 10> kDigitMasks{
    0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07, 0x7f, 0x6f};
// Compressed dark segment samples can sit close to the threshold. Calibration
// against a high-motion replay and sustained H.264/HEVC clips selected 100;
// black-frame detection remains intact in regression clips.
constexpr uint8_t kSegmentThreshold = 100;

struct Options {
  std::string path;
  uint8_t threshold = kSegmentThreshold;
  uint32_t max_examples = 20;
  uint32_t debug_frames = 0;
  uint32_t skip_initial_frames = 0;
};

std::string av_error(int error) {
  std::array<char, AV_ERROR_MAX_STRING_SIZE> text{};
  av_strerror(error, text.data(), text.size());
  return text.data();
}

bool parse_u32(std::string_view input, uint32_t& output) {
  uint32_t value = 0;
  const auto [end, error] = std::from_chars(input.data(), input.data() + input.size(), value);
  if (error != std::errc{} || end != input.data() + input.size()) return false;
  output = value;
  return true;
}

void usage(std::ostream& out, const char* program) {
  out << "Usage: " << program << " [options] recording.mkv\n"
      << "Decode the GPU-drawn ten-digit frame ID from a saved stimulus recording.\n"
      << "  --threshold Y       Luma threshold separating dark/light segments (default 100)\n"
      << "  --examples N        Maximum sequence anomaly examples (default 20)\n"
      << "  --debug-frames N    Print ID and confidence for first N decoded frames\n"
      << "  --skip-initial-frames N  Decode but exclude first N frames from identity counts\n"
      << "  --help              Show this help\n";
}

bool parse_options(int argc, char** argv, Options& options) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    if (arg == "--help" || arg == "-h") {
      usage(std::cout, argv[0]);
      return false;
    }
    if (arg == "--threshold" || arg == "--examples" || arg == "--debug-frames" ||
        arg == "--skip-initial-frames") {
      if (++i >= argc) {
        std::cerr << "missing value for " << arg << '\n';
        return false;
      }
      uint32_t value = 0;
      if (!parse_u32(argv[i], value)) {
        std::cerr << "invalid unsigned integer for " << arg << '\n';
        return false;
      }
      if (arg == "--threshold") {
        if (value == 0 || value >= 255) {
          std::cerr << "--threshold must be from 1 through 254\n";
          return false;
        }
        options.threshold = static_cast<uint8_t>(value);
      } else if (arg == "--examples") {
        options.max_examples = value;
      } else if (arg == "--debug-frames") {
        options.debug_frames = value;
      } else {
        options.skip_initial_frames = value;
      }
      continue;
    }
    if (!arg.empty() && arg.front() == '-') {
      std::cerr << "unknown option: " << arg << '\n';
      return false;
    }
    if (!options.path.empty()) {
      std::cerr << "only one input file may be provided\n";
      return false;
    }
    options.path = arg;
  }
  if (options.path.empty()) {
    usage(std::cerr, argv[0]);
    return false;
  }
  return true;
}

struct SegmentPoint {
  int x_local;
  int y_local;
};

// Pixel coordinates are chosen inside each solid segment, away from its edges.
// x_local/y_local are offsets from the shader's slot origin (x=8, top=8).
constexpr std::array<SegmentPoint, 7> kSamples{{
    {9, 1},   // a: x in [5,15), y in [0,3)
    {15, 7},  // b: x in [14,18), y in [3,13)
    {15, 19}, // c: x in [14,18), y in [15,25)
    {9, 26},  // d: x in [5,15), y in [25,28)
    {2, 19},  // e: x in [1,5), y in [15,25)
    {2, 7},   // f: x in [1,5), y in [3,13)
    {9, 13},  // g: x in [5,15), y in [12,16)
}};

struct DecodedId {
  uint32_t value = 0;
  double confidence = 0.0;
  uint32_t invalid_digit_masks = 0;
  bool all_segments_dark = false;
};

uint32_t sample_segment_luma(const AVFrame& frame, int slot_base_x,
                             const SegmentPoint& sample) {
  // A 3x3 luma patch averages codec noise while remaining inside every segment.
  // Shader coordinates refer to pixel centers. The sample table is the matching
  // source pixel index after the top-left origin conversion.
  const int center_x = slot_base_x + sample.x_local;
  const int center_y = 8 + sample.y_local;
  uint32_t sum = 0;
  for (int dy = -1; dy <= 1; ++dy) {
    const auto* row = frame.data[0] + (center_y + dy) * frame.linesize[0];
    for (int dx = -1; dx <= 1; ++dx) {
      sum += row[center_x + dx];
    }
  }
  return (sum + 4U) / 9U;
}

DecodedId decode_id(const AVFrame& frame, uint8_t threshold) {
  std::array<uint8_t, 10> digits{};
  std::array<double, 10> digit_confidence{};
  uint32_t invalid_digit_masks = 0;
  uint32_t lit_segments = 0;

  for (size_t digit_index = 0; digit_index < digits.size(); ++digit_index) {
    // Shader slots 0 and 1 are "I" and "D", slot 2 is blank, then 10 digits.
    const int slot_base_x = 8 + static_cast<int>(digit_index + 3) * 21;
    std::array<uint8_t, 7> samples{};
    uint8_t observed_mask = 0;
    double observed_margin = 0.0;
    for (size_t segment = 0; segment < samples.size(); ++segment) {
      const uint32_t luma = sample_segment_luma(frame, slot_base_x, kSamples[segment]);
      samples[segment] = static_cast<uint8_t>(luma);
      const bool lit = luma > threshold;
      if (lit) {
        observed_mask |= static_cast<uint8_t>(1U << segment);
        ++lit_segments;
      }
      const double normalized_margin = lit
          ? static_cast<double>(luma - threshold) / (255.0 - threshold)
          : static_cast<double>(threshold - luma) / threshold;
      observed_margin += std::clamp(normalized_margin, 0.0, 1.0);
    }

    int best_hamming = 8;
    size_t best_digit = 0;
    for (size_t candidate = 0; candidate < kDigitMasks.size(); ++candidate) {
      const int distance = std::popcount(static_cast<unsigned>(
          observed_mask ^ kDigitMasks[candidate]));
      if (distance < best_hamming) {
        best_hamming = distance;
        best_digit = candidate;
      }
    }

    digits[digit_index] = static_cast<uint8_t>(best_digit);
    const double exactness = best_hamming == 0 ? 1.0 : 1.0 - best_hamming / 7.0;
    // Mean luma distance from the on/off threshold, normalized to the
    // available range on each side. Invalid masks are counted separately.
    digit_confidence[digit_index] = (observed_margin / 7.0) * exactness;
    if (best_hamming != 0) ++invalid_digit_masks;
  }

  uint32_t value = 0;
  for (const uint8_t digit : digits) value = value * 10U + digit;
  double confidence = 0.0;
  for (const double digit_score : digit_confidence) confidence += digit_score;
  confidence /= static_cast<double>(digit_confidence.size());
  return {value, confidence, invalid_digit_masks, lit_segments == 0};
}

struct Decoder {
  AVFormatContext* format = nullptr;
  AVCodecContext* codec = nullptr;
  AVPacket* packet = nullptr;
  AVFrame* frame = nullptr;
  int stream_index = -1;

  ~Decoder() {
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&codec);
    avformat_close_input(&format);
  }
};

bool process_frame(const AVFrame& frame, const Options& options, uint64_t& count,
                   uint64_t& decoded_count,
                   double& confidence_sum, double& confidence_min,
                   uint64_t& invalid_digit_masks, uint64_t& invalid_frames,
                   uint64_t& black_frames, uint64_t& valid_id_frames,
                   uint64_t& valid_segments, uint64_t& invalid_spans,
                   uint64_t& unknown_transitions_across_invalid_spans,
                   std::optional<uint32_t>& first_id, std::optional<uint32_t>& last_valid_id,
                   std::optional<uint32_t>& previous_id, bool& invalid_span_open,
                   bool& have_prior_valid_id, uint64_t& adjacent_valid_transitions,
                   uint64_t& duplicates, uint64_t& gap_count, uint64_t& gap_transitions,
                   uint64_t& backwards,
                   std::vector<std::string>& anomalies, int64_t timestamp,
                   std::string& error) {
  if (frame.width < 284 || frame.height < 36 || frame.data[0] == nullptr ||
      frame.linesize[0] < frame.width) {
    error = "decoded frame is too small or has no readable luma plane";
    return false;
  }
  const AVPixFmtDescriptor* format = av_pix_fmt_desc_get(
      static_cast<AVPixelFormat>(frame.format));
  const bool readable_luma_layout = format != nullptr &&
      ((format->flags & AV_PIX_FMT_FLAG_PLANAR) != 0 || format->nb_components == 1) &&
      (format->flags & AV_PIX_FMT_FLAG_RGB) == 0;
  if (format == nullptr || format->comp[0].depth != 8 || !readable_luma_layout) {
    error = "the ID reader supports decoded 8-bit luma frames; got " +
            std::string(format == nullptr ? "unknown pixel format" : format->name);
    return false;
  }

  ++decoded_count;
  if (decoded_count <= options.skip_initial_frames) return true;

  const DecodedId decoded = decode_id(frame, options.threshold);
  ++count;
  const bool black = decoded.all_segments_dark;
  const bool invalid = !black && decoded.invalid_digit_masks != 0;
  const bool valid = !black && !invalid;
  if (black) {
    ++black_frames;
  } else if (invalid) {
    ++invalid_frames;
    invalid_digit_masks += decoded.invalid_digit_masks;
  }

  if (count <= options.debug_frames) {
    std::cerr << "debug frame=" << (decoded_count - 1) << " pts=" << timestamp;
    if (black) {
      std::cerr << " id=unknown status=black";
    } else if (invalid) {
      std::cerr << " id=unknown status=invalid invalid_digit_masks="
                << decoded.invalid_digit_masks;
    } else {
      std::cerr << " id=" << std::setw(10) << std::setfill('0') << decoded.value
                << std::setfill(' ') << " confidence=" << std::fixed
                << std::setprecision(3) << decoded.confidence;
    }
    std::cerr << '\n';
  }

  if (!valid) {
    if (!invalid_span_open) ++invalid_spans;
    invalid_span_open = true;
    previous_id.reset();
    return true;
  }

  ++valid_id_frames;
  confidence_sum += decoded.confidence;
  confidence_min = std::min(confidence_min, decoded.confidence);
  if (!first_id.has_value()) first_id = decoded.value;
  last_valid_id = decoded.value;
  if (invalid_span_open) {
    ++valid_segments;
    if (have_prior_valid_id) ++unknown_transitions_across_invalid_spans;
    invalid_span_open = false;
  } else if (!previous_id.has_value()) {
    ++valid_segments;
  }

  if (previous_id.has_value()) {
    ++adjacent_valid_transitions;
    const uint32_t delta = decoded.value - *previous_id;
    if (delta == 0) {
      ++duplicates;
      if (anomalies.size() < options.max_examples) {
        anomalies.push_back("duplicate frame=" + std::to_string(decoded_count - 1) +
                            " id=" + std::to_string(decoded.value));
      }
    } else if (delta < 0x80000000U) {
      if (delta > 1) {
        gap_count += static_cast<uint64_t>(delta - 1U);
        ++gap_transitions;
        if (anomalies.size() < options.max_examples) {
          anomalies.push_back("gap before frame=" + std::to_string(decoded_count - 1) +
                              " ids=" + std::to_string(*previous_id) + "->" +
                              std::to_string(decoded.value) + " missing=" +
                              std::to_string(delta - 1U));
        }
      }
    } else {
      ++backwards;
      if (anomalies.size() < options.max_examples) {
        anomalies.push_back("backward/out-of-order frame=" + std::to_string(decoded_count - 1) +
                            " ids=" + std::to_string(*previous_id) + "->" +
                            std::to_string(decoded.value));
      }
    }
  }
  previous_id = decoded.value;
  have_prior_valid_id = true;
  return true;
}

bool drain_decoder(Decoder& decoder, const Options& options, uint64_t& count,
                   uint64_t& decoded_count,
                   double& confidence_sum, double& confidence_min,
                   uint64_t& invalid_digit_masks, uint64_t& invalid_frames,
                   uint64_t& black_frames, uint64_t& valid_id_frames,
                   uint64_t& valid_segments, uint64_t& invalid_spans,
                   uint64_t& unknown_transitions_across_invalid_spans,
                   std::optional<uint32_t>& first_id, std::optional<uint32_t>& last_valid_id,
                   std::optional<uint32_t>& previous_id, bool& invalid_span_open,
                   bool& have_prior_valid_id, uint64_t& adjacent_valid_transitions,
                   uint64_t& duplicates, uint64_t& gap_count, uint64_t& gap_transitions,
                   uint64_t& backwards,
                   std::vector<std::string>& anomalies, std::string& error) {
  for (;;) {
    const int result = avcodec_receive_frame(decoder.codec, decoder.frame);
    if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return true;
    if (result < 0) {
      error = "video decode failed: " + av_error(result);
      return false;
    }
    const int64_t timestamp = decoder.frame->best_effort_timestamp;
    if (!process_frame(*decoder.frame, options, count, decoded_count,
                       confidence_sum, confidence_min,
                       invalid_digit_masks, invalid_frames, black_frames,
                       valid_id_frames, valid_segments, invalid_spans,
                       unknown_transitions_across_invalid_spans, first_id, last_valid_id,
                       previous_id, invalid_span_open, have_prior_valid_id,
                       adjacent_valid_transitions, duplicates, gap_count,
                       gap_transitions, backwards,
                       anomalies, timestamp, error)) {
      av_frame_unref(decoder.frame);
      return false;
    }
    av_frame_unref(decoder.frame);
  }
}

int run(const Options& options) {
  Decoder decoder;
  int result = avformat_open_input(&decoder.format, options.path.c_str(), nullptr, nullptr);
  if (result < 0) {
    std::cerr << "cannot open input: " << av_error(result) << '\n';
    return 1;
  }
  result = avformat_find_stream_info(decoder.format, nullptr);
  if (result < 0) {
    std::cerr << "cannot read stream info: " << av_error(result) << '\n';
    return 1;
  }

  const AVCodec* codec = nullptr;
  decoder.stream_index = av_find_best_stream(decoder.format, AVMEDIA_TYPE_VIDEO, -1, -1,
                                              &codec, 0);
  if (decoder.stream_index < 0 || codec == nullptr) {
    std::cerr << "input has no decodable video stream: "
              << av_error(decoder.stream_index) << '\n';
    return 1;
  }
  decoder.codec = avcodec_alloc_context3(codec);
  if (decoder.codec == nullptr) {
    std::cerr << "cannot allocate video decoder context\n";
    return 1;
  }
  result = avcodec_parameters_to_context(
      decoder.codec, decoder.format->streams[decoder.stream_index]->codecpar);
  if (result < 0) {
    std::cerr << "cannot copy video parameters: " << av_error(result) << '\n';
    return 1;
  }
  decoder.codec->thread_count = 0;
  result = avcodec_open2(decoder.codec, codec, nullptr);
  if (result < 0) {
    std::cerr << "cannot open video decoder: " << av_error(result) << '\n';
    return 1;
  }
  decoder.packet = av_packet_alloc();
  decoder.frame = av_frame_alloc();
  if (decoder.packet == nullptr || decoder.frame == nullptr) {
    std::cerr << "cannot allocate FFmpeg packet/frame\n";
    return 1;
  }

  const AVStream* stream = decoder.format->streams[decoder.stream_index];
  std::cout << "input=" << options.path << " codec=" << codec->name << " size="
            << decoder.codec->width << 'x' << decoder.codec->height << " time_base="
            << stream->time_base.num << '/' << stream->time_base.den
            << " mode=offline_luma_decode threshold=" << static_cast<unsigned>(options.threshold)
            << '\n';

  uint64_t frame_count = 0;
  uint64_t decoded_count = 0;
  double confidence_sum = 0.0;
  double confidence_min = 1.0;
  uint64_t invalid_digit_masks = 0;
  uint64_t invalid_frames = 0;
  uint64_t black_frames = 0;
  uint64_t valid_id_frames = 0;
  uint64_t valid_segments = 0;
  uint64_t invalid_spans = 0;
  uint64_t unknown_transitions_across_invalid_spans = 0;
  std::optional<uint32_t> first_id;
  std::optional<uint32_t> last_valid_id;
  std::optional<uint32_t> previous_id;
  bool invalid_span_open = false;
  bool have_prior_valid_id = false;
  uint64_t adjacent_valid_transitions = 0;
  uint64_t duplicates = 0;
  uint64_t gap_count = 0;
  uint64_t gap_transitions = 0;
  uint64_t backwards = 0;
  std::vector<std::string> anomalies;
  std::string error;

  while ((result = av_read_frame(decoder.format, decoder.packet)) >= 0) {
    if (decoder.packet->stream_index == decoder.stream_index) {
      result = avcodec_send_packet(decoder.codec, decoder.packet);
      if (result < 0) {
        error = "cannot submit packet to decoder: " + av_error(result);
        av_packet_unref(decoder.packet);
        std::cerr << error << '\n';
        return 1;
      }
      if (!drain_decoder(decoder, options, frame_count, decoded_count,
                         confidence_sum, confidence_min,
                         invalid_digit_masks, invalid_frames, black_frames,
                         valid_id_frames, valid_segments, invalid_spans,
                         unknown_transitions_across_invalid_spans,
                         first_id, last_valid_id, previous_id, invalid_span_open,
                         have_prior_valid_id, adjacent_valid_transitions,
                         duplicates, gap_count,
                         gap_transitions, backwards,
                         anomalies, error)) {
        std::cerr << error << '\n';
        return 1;
      }
    }
    av_packet_unref(decoder.packet);
  }
  if (result != AVERROR_EOF) {
    std::cerr << "error reading input: " << av_error(result) << '\n';
    return 1;
  }

  result = avcodec_send_packet(decoder.codec, nullptr);
  if (result < 0 && result != AVERROR_EOF) {
    std::cerr << "cannot flush video decoder: " << av_error(result) << '\n';
    return 1;
  }
  if (!drain_decoder(decoder, options, frame_count, decoded_count,
                     confidence_sum, confidence_min,
                     invalid_digit_masks, invalid_frames, black_frames,
                     valid_id_frames, valid_segments, invalid_spans,
                     unknown_transitions_across_invalid_spans,
                     first_id, last_valid_id, previous_id, invalid_span_open,
                     have_prior_valid_id, adjacent_valid_transitions,
                     duplicates, gap_count,
                     gap_transitions, backwards,
                     anomalies, error)) {
    std::cerr << error << '\n';
    return 1;
  }

  std::cout << "decoded_frames=" << decoded_count
            << " analyzed_frames=" << frame_count
            << " ignored_initial_frames=" << (decoded_count - frame_count);
  if (first_id.has_value() && last_valid_id.has_value()) {
    std::cout << " first_id=" << *first_id << " last_id=" << *last_valid_id;
  }
  std::cout << " valid_id_frames=" << valid_id_frames
            << " valid_segments=" << valid_segments
            << " adjacent_valid_transitions=" << adjacent_valid_transitions
            << " invalid_spans=" << invalid_spans
            << " unknown_transitions_across_invalid_spans="
            << unknown_transitions_across_invalid_spans
            << " duplicate_transitions=" << duplicates
            << " duplicate_transitions_between_adjacent_valid_frames=" << duplicates
            << " gap_transitions=" << gap_transitions
            << " gap_transitions_between_adjacent_valid_frames=" << gap_transitions
            << " missing_ids_between_frames=" << gap_count
            << " missing_ids_between_adjacent_valid_frames=" << gap_count
            << " backward_transitions=" << backwards
            << " backward_transitions_between_adjacent_valid_frames=" << backwards
            << " black_frames=" << black_frames
            << " invalid_frames=" << invalid_frames
            << " invalid_frames_excludes_black=1";
  if (valid_id_frames != 0) {
    std::cout << std::fixed << std::setprecision(4)
              << " mean_confidence=" << confidence_sum / static_cast<double>(valid_id_frames)
              << " min_confidence=" << confidence_min
              << " invalid_digit_masks=" << invalid_digit_masks;
  }
  std::cout << '\n';
  const uint64_t total_sequence_anomalies = duplicates + gap_transitions + backwards;
  if (total_sequence_anomalies == 0) {
    std::cout << "sequence_anomalies=none\n";
  } else {
    std::cout << "sequence_anomaly_examples=" << anomalies.size()
              << " suppressed=" << (total_sequence_anomalies - anomalies.size()) << '\n';
    for (const std::string& anomaly : anomalies) std::cout << "  " << anomaly << '\n';
  }
  if (frame_count == 0) {
    std::cerr << "no frames were decoded\n";
    return 1;
  }
  if (invalid_digit_masks > 0) {
    std::cerr << "warning: at least one sampled digit produced an invalid seven-segment mask; "
                 "inspect confidence and use --debug-frames to diagnose segment thresholds\n";
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 2 && (std::string_view(argv[1]) == "--help" ||
                    std::string_view(argv[1]) == "-h")) {
    usage(std::cout, argv[0]);
    return 0;
  }
  Options options;
  if (!parse_options(argc, argv, options)) return 2;
  return run(options);
}
