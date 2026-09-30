// Offline, post-record checker for stimulus/src/main.cpp's GPU-rendered colorchart.
//
/* Example build:
   c++ -std=c++23 -O2 tools/decode_colorchart.cpp -o /tmp/fluxrec-decode-colorchart \
     $(pkg-config --cflags --libs libavformat libavcodec libavutil libswscale)
*/
//
// Decoded RGB data stays inside this offline analyzer. This tool is not part of
// Fluxrec's capture or recording path.

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct Options {
  std::string path;
  uint32_t frame_limit = 30;
  bool help = false;
};

struct Rgb {
  double r = 0.0;
  double g = 0.0;
  double b = 0.0;
};

struct Accumulator {
  long double r = 0.0;
  long double g = 0.0;
  long double b = 0.0;
  uint64_t count = 0;

  void add(Rgb value) {
    r += value.r;
    g += value.g;
    b += value.b;
    ++count;
  }

  [[nodiscard]] Rgb mean() const {
    if (count == 0) return {};
    const long double divisor = static_cast<long double>(count);
    return {static_cast<double>(r / divisor), static_cast<double>(g / divisor),
            static_cast<double>(b / divisor)};
  }
};

struct Decoder {
  AVFormatContext* format = nullptr;
  AVCodecContext* codec = nullptr;
  AVPacket* packet = nullptr;
  AVFrame* frame = nullptr;
  SwsContext* sws = nullptr;
  int stream_index = -1;
  AVColorSpace effective_color_space = AVCOL_SPC_UNSPECIFIED;
  AVColorRange effective_color_range = AVCOL_RANGE_UNSPECIFIED;

  ~Decoder() {
    sws_freeContext(sws);
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&codec);
    avformat_close_input(&format);
  }
};

struct ChartSamples {
  static constexpr size_t kBarCount = 8;
  static constexpr size_t kRampCount = 4;
  static constexpr size_t kRampPoints = 5;
  std::array<Accumulator, kBarCount> bars;
  std::array<std::array<Accumulator, kRampPoints>, kRampCount> ramps;
  std::array<Accumulator, 2> checker;
};

constexpr std::array<std::string_view, ChartSamples::kBarCount> kBarNames{
    "black", "white", "red", "green", "blue", "yellow", "cyan", "magenta"};
constexpr std::array<Rgb, ChartSamples::kBarCount> kBarExpected{{
    {0, 0, 0}, {255, 255, 255}, {255, 0, 0}, {0, 255, 0},
    {0, 0, 255}, {255, 255, 0}, {0, 255, 255}, {255, 0, 255},
}};
constexpr std::array<std::string_view, ChartSamples::kRampCount> kRampNames{
    "gray", "red", "green", "blue"};
constexpr std::array<double, ChartSamples::kRampPoints> kRampXs{
    0.125, 0.3125, 0.5, 0.6875, 0.875};
constexpr std::array<double, 2> kCheckerXs{0.03125, 0.09375};
constexpr double kCheckerY = 0.97375;

std::string av_error(int error) {
  std::array<char, AV_ERROR_MAX_STRING_SIZE> text{};
  av_strerror(error, text.data(), text.size());
  return text.data();
}

bool parse_u32(std::string_view text, uint32_t& value) {
  uint32_t parsed = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed);
  if (error != std::errc{} || end != text.data() + text.size()) return false;
  value = parsed;
  return true;
}

void usage(std::ostream& out, const char* program) {
  out << "Usage: " << program << " [--frames N] recording.mkv\n"
      << "Decode and measure the GPU-drawn color chart in a saved recording.\n"
      << "  --frames N  Maximum number of decoded frames to sample (default 30)\n"
      << "  --help      Show this help\n";
}

bool parse_options(int argc, char** argv, Options& options) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    if (arg == "--help" || arg == "-h") {
      usage(std::cout, argv[0]);
      options.help = true;
      return false;
    }
    if (arg == "--frames") {
      if (++i >= argc || !parse_u32(argv[i], options.frame_limit) ||
          options.frame_limit == 0) {
        std::cerr << "--frames must be a positive integer\n";
        return false;
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
    options.path.assign(arg);
  }
  if (options.path.empty()) {
    usage(std::cerr, argv[0]);
    return false;
  }
  return true;
}

int sample_radius(int dimension) {
  // A 5x5 to 9x9 patch is wide enough to average compression noise while staying
  // comfortably inside each chart patch and away from the moving ID overlay.
  return std::clamp(dimension / 800, 2, 4);
}

Rgb sample_patch(const uint8_t* rgb, int width, int height, int x, int y) {
  const int radius_x = sample_radius(width);
  const int radius_y = sample_radius(height);
  const int min_x = std::clamp(x - radius_x, 0, width - 1);
  const int max_x = std::clamp(x + radius_x, 0, width - 1);
  const int min_y = std::clamp(y - radius_y, 0, height - 1);
  const int max_y = std::clamp(y + radius_y, 0, height - 1);
  long double red = 0.0;
  long double green = 0.0;
  long double blue = 0.0;
  uint64_t count = 0;
  for (int row = min_y; row <= max_y; ++row) {
    for (int column = min_x; column <= max_x; ++column) {
      const size_t offset = (static_cast<size_t>(row) * static_cast<size_t>(width) +
                             static_cast<size_t>(column)) * 3U;
      red += rgb[offset];
      green += rgb[offset + 1U];
      blue += rgb[offset + 2U];
      ++count;
    }
  }
  const long double divisor = static_cast<long double>(count);
  return {static_cast<double>(red / divisor), static_cast<double>(green / divisor),
          static_cast<double>(blue / divisor)};
}

int coordinate(double fraction, int dimension) {
  const double value = std::clamp(fraction, 0.0, 1.0) * static_cast<double>(dimension);
  return std::clamp(static_cast<int>(value), 0, dimension - 1);
}

void add_frame(const AVFrame& frame, Decoder& decoder, ChartSamples& samples) {
  const int width = frame.width;
  const int height = frame.height;
  if (width <= 0 || height <= 0 || frame.format < 0) return;

  decoder.sws = sws_getCachedContext(
      decoder.sws, width, height, static_cast<AVPixelFormat>(frame.format), width, height,
      AV_PIX_FMT_RGB24, SWS_POINT, nullptr, nullptr, nullptr);
  if (decoder.sws == nullptr) throw std::runtime_error("creating libswscale RGB decoder failed");

  AVColorSpace color_space = frame.colorspace;
  if (color_space == AVCOL_SPC_UNSPECIFIED) color_space = decoder.codec->colorspace;
  decoder.effective_color_space = color_space;
  const int sws_space = color_space == AVCOL_SPC_BT709 ? SWS_CS_ITU709 : SWS_CS_ITU601;
  AVColorRange color_range = frame.color_range;
  if (color_range == AVCOL_RANGE_UNSPECIFIED) color_range = decoder.codec->color_range;
  decoder.effective_color_range = color_range;
  const int source_full_range = color_range == AVCOL_RANGE_JPEG ? 1 : 0;
  const int* coefficients = sws_getCoefficients(sws_space);
  if (sws_setColorspaceDetails(decoder.sws, coefficients, source_full_range,
                               coefficients, 1, 0, 1 << 16, 1 << 16) < 0) {
    throw std::runtime_error("setting libswscale color metadata failed");
  }

  std::vector<uint8_t> rgb(static_cast<size_t>(width) * static_cast<size_t>(height) * 3U);
  std::array<uint8_t*, 4> destination{rgb.data(), nullptr, nullptr, nullptr};
  std::array<int, 4> destination_stride{width * 3, 0, 0, 0};
  const int converted_rows = sws_scale(decoder.sws, frame.data, frame.linesize, 0, height,
                                       destination.data(), destination_stride.data());
  if (converted_rows != height) throw std::runtime_error("libswscale returned a short frame");

  constexpr double kBarsY = 0.32;
  for (size_t index = 0; index < samples.bars.size(); ++index) {
    const double x = (static_cast<double>(index) + 0.5) / 8.0;
    samples.bars[index].add(sample_patch(rgb.data(), width, height,
                                         coordinate(x, width), coordinate(kBarsY, height)));
  }

  constexpr std::array<double, ChartSamples::kRampCount> kRampYs{
      0.685, 0.755, 0.825, 0.895};
  for (size_t ramp = 0; ramp < samples.ramps.size(); ++ramp) {
    for (size_t point = 0; point < samples.ramps[ramp].size(); ++point) {
      samples.ramps[ramp][point].add(sample_patch(
          rgb.data(), width, height, coordinate(kRampXs[point], width),
          coordinate(kRampYs[ramp], height)));
    }
  }

  // Sample well inside checker row 2. Labels below identify these x positions
  // only; no dark/light polarity is assumed, so a flipped origin or cell offset
  // leaves the absolute contrast meaningful.
  for (size_t cell = 0; cell < samples.checker.size(); ++cell) {
    samples.checker[cell].add(sample_patch(
        rgb.data(), width, height, coordinate(kCheckerXs[cell], width),
        coordinate(kCheckerY, height)));
  }
}

Rgb expected_ramp(size_t ramp, double x) {
  const double level = 255.0 * x;
  switch (ramp) {
    case 0: return {level, level, level};
    case 1: return {level, 0, 0};
    case 2: return {0, level, 0};
    default: return {0, 0, level};
  }
}

double max_error(Rgb actual, Rgb expected) {
  return std::max({std::abs(actual.r - expected.r), std::abs(actual.g - expected.g),
                   std::abs(actual.b - expected.b)});
}

double mean_error(Rgb actual, Rgb expected) {
  return (std::abs(actual.r - expected.r) + std::abs(actual.g - expected.g) +
          std::abs(actual.b - expected.b)) / 3.0;
}

void print_rgb(Rgb value) {
  std::cout << std::fixed << std::setprecision(1) << value.r << ',' << value.g << ','
            << value.b;
}

void print_report(const ChartSamples& samples, const Decoder& decoder,
                  uint64_t decoded_frames, int width, int height) {
  std::cout << "decoded_frames=" << decoded_frames << " dimensions=" << width << 'x' << height
            << " codec=" << avcodec_get_name(decoder.codec->codec_id)
            << " declared_matrix="
            << (decoder.effective_color_space == AVCOL_SPC_BT709 ? "bt709" :
                decoder.effective_color_space == AVCOL_SPC_UNSPECIFIED ? "unspecified" : "other")
            << " analyzer_matrix="
            << (decoder.effective_color_space == AVCOL_SPC_BT709 ? "bt709" : "bt601_fallback")
            << " declared_range="
            << (decoder.effective_color_range == AVCOL_RANGE_MPEG ? "limited" :
                decoder.effective_color_range == AVCOL_RANGE_JPEG ? "full" : "unspecified")
            << " analyzer_range="
            << (decoder.effective_color_range == AVCOL_RANGE_JPEG ? "full" : "limited_fallback")
            << '\n';

  double bar_abs_sum = 0.0;
  double bar_max_error = 0.0;
  for (size_t index = 0; index < samples.bars.size(); ++index) {
    const Rgb actual = samples.bars[index].mean();
    const Rgb expected = kBarExpected[index];
    const double error = mean_error(actual, expected);
    bar_abs_sum += error;
    bar_max_error = std::max(bar_max_error, max_error(actual, expected));
    std::cout << "bar." << kBarNames[index] << " expected=";
    print_rgb(expected);
    std::cout << " actual=";
    print_rgb(actual);
    std::cout << " mae=" << std::fixed << std::setprecision(2) << error << '\n';
  }

  bool all_ramps_monotonic = true;
  double gray_max_neutral_spread = 0.0;
  double ramp_abs_sum = 0.0;
  uint64_t ramp_count = 0;
  for (size_t ramp = 0; ramp < samples.ramps.size(); ++ramp) {
    double previous = -1.0;
    for (size_t point = 0; point < samples.ramps[ramp].size(); ++point) {
      const Rgb actual = samples.ramps[ramp][point].mean();
      const Rgb expected = expected_ramp(ramp, kRampXs[point]);
      const double error = mean_error(actual, expected);
      ramp_abs_sum += error;
      ++ramp_count;
      const double active = ramp == 0 ? (actual.r + actual.g + actual.b) / 3.0
                                      : ramp == 1 ? actual.r
                                                  : ramp == 2 ? actual.g : actual.b;
      if (active + 2.0 < previous) all_ramps_monotonic = false;
      previous = active;
      if (ramp == 0) {
        gray_max_neutral_spread = std::max(gray_max_neutral_spread,
            std::max({std::abs(actual.r - actual.g), std::abs(actual.g - actual.b),
                      std::abs(actual.b - actual.r)}));
      }
      std::cout << "ramp." << kRampNames[ramp] << '.' << point
                << " x=" << std::fixed << std::setprecision(4) << kRampXs[point]
                << " expected=";
      print_rgb(expected);
      std::cout << " actual=";
      print_rgb(actual);
      std::cout << " mae=" << std::fixed << std::setprecision(2) << error << '\n';
    }
  }

  const Rgb checker_cell0 = samples.checker[0].mean();
  const Rgb checker_cell1 = samples.checker[1].mean();
  const double cell0_luma = checker_cell0.r * 0.2126 + checker_cell0.g * 0.7152 +
                            checker_cell0.b * 0.0722;
  const double cell1_luma = checker_cell1.r * 0.2126 + checker_cell1.g * 0.7152 +
                            checker_cell1.b * 0.0722;
  std::cout << "checker.cell0_rgb=";
  print_rgb(checker_cell0);
  std::cout << " cell1_rgb=";
  print_rgb(checker_cell1);
  std::cout << " samples_xy=(" << std::fixed << std::setprecision(5)
            << kCheckerXs[0] << ',' << kCheckerY << ";"
            << kCheckerXs[1] << ',' << kCheckerY << ')'
            << "\nchecker.cell0_luma=" << std::fixed << std::setprecision(1) << cell0_luma
            << " cell1_luma=" << cell1_luma
            << " luma_abs_contrast=" << std::abs(cell1_luma - cell0_luma)
            << " (about 193.8 expected for opposite cells)\n";
  std::cout << "summary.bar_mean_mae=" << std::fixed << std::setprecision(2)
            << bar_abs_sum / static_cast<double>(samples.bars.size())
            << " bar_max_channel_error=" << bar_max_error
            << " ramp_mean_mae=" << ramp_abs_sum / static_cast<double>(ramp_count)
            << " gray_max_channel_spread=" << gray_max_neutral_spread
            << " all_sampled_ramps_monotonic=" << (all_ramps_monotonic ? "true" : "false")
            << '\n';
  std::cout << "Interpretation: compare measured values across runs/codecs. This end-to-end check "
               "does not isolate a faulty stage or certify a display profile.\n";
}

bool receive_frames(Decoder& decoder, ChartSamples& samples, uint32_t limit,
                    uint64_t& decoded_frames, int& width, int& height) {
  for (;;) {
    const int result = avcodec_receive_frame(decoder.codec, decoder.frame);
    if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return true;
    if (result < 0) {
      std::cerr << "decoding frame failed: " << av_error(result) << '\n';
      return false;
    }
    width = decoder.frame->width;
    height = decoder.frame->height;
    try {
      add_frame(*decoder.frame, decoder, samples);
    } catch (const std::exception& exception) {
      std::cerr << exception.what() << '\n';
      av_frame_unref(decoder.frame);
      return false;
    }
    ++decoded_frames;
    av_frame_unref(decoder.frame);
    if (decoded_frames >= limit) return true;
  }
}

int run(const Options& options) {
  Decoder decoder;
  int result = avformat_open_input(&decoder.format, options.path.c_str(), nullptr, nullptr);
  if (result < 0) {
    std::cerr << "opening input failed: " << av_error(result) << '\n';
    return 1;
  }
  result = avformat_find_stream_info(decoder.format, nullptr);
  if (result < 0) {
    std::cerr << "reading stream information failed: " << av_error(result) << '\n';
    return 1;
  }
  const AVCodec* codec = nullptr;
  result = av_find_best_stream(decoder.format, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
  if (result < 0 || codec == nullptr) {
    std::cerr << "finding a video stream failed: " << av_error(result) << '\n';
    return 1;
  }
  decoder.stream_index = result;
  decoder.codec = avcodec_alloc_context3(codec);
  decoder.packet = av_packet_alloc();
  decoder.frame = av_frame_alloc();
  if (decoder.codec == nullptr || decoder.packet == nullptr || decoder.frame == nullptr) {
    std::cerr << "allocating FFmpeg decoder objects failed\n";
    return 1;
  }
  result = avcodec_parameters_to_context(decoder.codec,
      decoder.format->streams[decoder.stream_index]->codecpar);
  if (result < 0 || (result = avcodec_open2(decoder.codec, codec, nullptr)) < 0) {
    std::cerr << "opening video decoder failed: " << av_error(result) << '\n';
    return 1;
  }

  ChartSamples samples;
  uint64_t decoded_frames = 0;
  int width = 0;
  int height = 0;
  auto drain = [&]() {
    return receive_frames(decoder, samples, options.frame_limit, decoded_frames, width, height);
  };
  bool success = true;
  while (decoded_frames < options.frame_limit && (result = av_read_frame(decoder.format, decoder.packet)) >= 0) {
    if (decoder.packet->stream_index == decoder.stream_index) {
      int sent = avcodec_send_packet(decoder.codec, decoder.packet);
      if (sent == AVERROR(EAGAIN)) {
        success = drain();
        if (success) sent = avcodec_send_packet(decoder.codec, decoder.packet);
      }
      if (sent < 0) {
        std::cerr << "sending packet to decoder failed: " << av_error(sent) << '\n';
        success = false;
      } else if (!drain()) {
        success = false;
      }
    }
    av_packet_unref(decoder.packet);
    if (!success) break;
  }
  if (success && decoded_frames < options.frame_limit) {
    result = avcodec_send_packet(decoder.codec, nullptr);
    if (result < 0 && result != AVERROR_EOF) {
      std::cerr << "flushing video decoder failed: " << av_error(result) << '\n';
      success = false;
    } else if (!drain()) {
      success = false;
    }
  }
  if (!success) return 1;
  if (decoded_frames == 0) {
    std::cerr << "input contained no decodable video frames\n";
    return 1;
  }
  print_report(samples, decoder, decoded_frames, width, height);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!parse_options(argc, argv, options)) return options.help ? 0 : 2;
  return run(options);
}
