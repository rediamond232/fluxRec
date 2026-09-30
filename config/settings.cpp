#include "config/settings.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <sstream>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace fluxrec::config {
namespace {

using Json = nlohmann::json;
constexpr std::size_t max_config_bytes = 1024U * 1024U;

Error error(ErrorCode code, std::string message) {
    return Error{code, std::move(message)};
}

std::string systemError(std::string_view operation, int value = errno) {
    return std::string(operation) + ": " + std::strerror(value);
}

bool validUtf8(std::string_view text) noexcept {
    std::size_t i = 0;
    while (i < text.size()) {
        const auto first = static_cast<unsigned char>(text[i]);
        if (first <= 0x7fU) {
            ++i;
            continue;
        }

        std::size_t length = 0;
        std::uint32_t codepoint = 0;
        if (first >= 0xc2U && first <= 0xdfU) {
            length = 2;
            codepoint = first & 0x1fU;
        } else if (first >= 0xe0U && first <= 0xefU) {
            length = 3;
            codepoint = first & 0x0fU;
        } else if (first >= 0xf0U && first <= 0xf4U) {
            length = 4;
            codepoint = first & 0x07U;
        } else {
            return false;
        }
        if (i + length > text.size()) return false;
        for (std::size_t j = 1; j < length; ++j) {
            const auto next = static_cast<unsigned char>(text[i + j]);
            if ((next & 0xc0U) != 0x80U) return false;
            codepoint = (codepoint << 6U) | (next & 0x3fU);
        }
        if ((length == 2 && codepoint < 0x80U) ||
            (length == 3 && codepoint < 0x800U) ||
            (length == 4 && codepoint < 0x10000U) ||
            codepoint > 0x10ffffU ||
            (codepoint >= 0xd800U && codepoint <= 0xdfffU)) {
            return false;
        }
        i += length;
    }
    return true;
}

bool validText(const std::string& value, std::size_t max_bytes,
               bool allow_empty, bool allow_control) noexcept {
    if ((!allow_empty && value.empty()) || value.size() > max_bytes || !validUtf8(value)) {
        return false;
    }
    for (const unsigned char character : value) {
        if (character == 0U || (!allow_control && character < 0x20U)) return false;
    }
    return true;
}

std::expected<std::uint64_t, Error> readUnsigned(const Json& object,
                                                std::string_view key,
                                                std::uint64_t maximum) {
    const auto& value = object.at(std::string(key));
    std::uint64_t result{};
    if (value.is_number_unsigned()) {
        result = value.get<std::uint64_t>();
    } else if (value.is_number_integer()) {
        const auto signed_value = value.get<std::int64_t>();
        if (signed_value < 0) {
            return std::unexpected(error(ErrorCode::malformed_data,
                "setting '" + std::string(key) + "' cannot be negative"));
        }
        result = static_cast<std::uint64_t>(signed_value);
    } else {
        return std::unexpected(error(ErrorCode::malformed_data,
            "setting '" + std::string(key) + "' must be an integer"));
    }
    if (result > maximum) {
        return std::unexpected(error(ErrorCode::malformed_data,
            "setting '" + std::string(key) + "' exceeds its supported range"));
    }
    return result;
}

std::expected<std::string, Error> readString(const Json& object,
                                             std::string_view key) {
    const auto& value = object.at(std::string(key));
    if (!value.is_string()) {
        return std::unexpected(error(ErrorCode::malformed_data,
            "setting '" + std::string(key) + "' must be a string"));
    }
    return value.get<std::string>();
}

std::expected<bool, Error> readBool(const Json& object, std::string_view key) {
    const auto& value = object.at(std::string(key));
    if (!value.is_boolean()) {
        return std::unexpected(error(ErrorCode::malformed_data,
            "setting '" + std::string(key) + "' must be a boolean"));
    }
    return value.get<bool>();
}

std::expected<float, Error> readGain(const Json& object, std::string_view key) {
    const auto& value = object.at(std::string(key));
    if (!value.is_number()) {
        return std::unexpected(error(ErrorCode::malformed_data,
            "setting '" + std::string(key) + "' must be a number"));
    }
    const double parsed = value.get<double>();
    if (!std::isfinite(parsed) || parsed > std::numeric_limits<float>::max() ||
        parsed < -std::numeric_limits<float>::max()) {
        return std::unexpected(error(ErrorCode::malformed_data,
            "setting '" + std::string(key) + "' is not a finite float"));
    }
    return static_cast<float>(parsed);
}

std::expected<void, Error> validateAudio(const AudioSourceSettings& audio,
                                         std::string_view label) {
    if (!validText(audio.device, 1024, true, false)) {
        return std::unexpected(error(ErrorCode::invalid_settings,
            std::string(label) + " device must be valid UTF-8, at most 1024 bytes, "
            "and contain no control characters"));
    }
    if (!std::isfinite(audio.gain) || audio.gain < 0.0F || audio.gain > 4.0F) {
        return std::unexpected(error(ErrorCode::invalid_settings,
            std::string(label) + " gain must be finite and between 0 and 4"));
    }
    return {};
}

Json audioToJson(const AudioSourceSettings& audio) {
    return Json{{"device", audio.device}, {"gain", audio.gain}, {"muted", audio.muted}};
}

std::expected<AudioSourceSettings, Error> audioFromJson(const Json& object,
                                                        std::string_view label) {
    if (!object.is_object()) {
        return std::unexpected(error(ErrorCode::malformed_data,
            std::string(label) + " must be an object"));
    }
    auto device = readString(object, "device");
    if (!device) return std::unexpected(device.error());
    auto gain = readGain(object, "gain");
    if (!gain) return std::unexpected(gain.error());
    auto muted = readBool(object, "muted");
    if (!muted) return std::unexpected(muted.error());
    AudioSourceSettings result{std::move(*device), *gain, *muted};
    if (auto result_validation = validateAudio(result, label); !result_validation) {
        return std::unexpected(error(ErrorCode::malformed_data,
                                     result_validation.error().message));
    }
    return result;
}

Json settingsToJson(const Settings& settings) {
    const char* codec = settings.recording.codec == VideoCodec::h264 ? "h264" : "hevc";
    const char* rate_control = settings.recording.rate_control == VideoRateControl::bitrate
        ? "bitrate" : "constant_qp";
    const char* tune = "default";
    switch (settings.recording.encoder_tune) {
    case VideoEncoderTune::default_tuning: tune = "default"; break;
    case VideoEncoderTune::ll: tune = "ll"; break;
    case VideoEncoderTune::ull: tune = "ull"; break;
    case VideoEncoderTune::hq: tune = "hq"; break;
    }
    const char* scheme = "system";
    switch (settings.ui.color_scheme) {
    case ColorScheme::system: scheme = "system"; break;
    case ColorScheme::light: scheme = "light"; break;
    case ColorScheme::dark: scheme = "dark"; break;
    }
    return Json{
        {"schema_version", current_schema_version},
        {"recording", {
            {"output_directory", settings.recording.output_directory},
            {"codec", codec},
            {"rate_control", rate_control},
            {"bitrate_bps", settings.recording.bitrate_bps},
            {"qp", settings.recording.qp},
            {"gop_size", settings.recording.gop_size},
            {"display_name", settings.recording.display_name},
            {"output_width", settings.recording.output_width},
            {"output_height", settings.recording.output_height},
            {"max_fps", settings.recording.max_fps},
            {"encoder_tune", tune},
            {"capture_pool_size", settings.recording.capture_pool_size},
            {"async_depth", settings.recording.async_depth},
            {"replay_duration_seconds", settings.recording.replay_duration_seconds},
        }},
        {"audio", {
            {"desktop", audioToJson(settings.desktop_audio)},
            {"microphone", audioToJson(settings.microphone_audio)},
        }},
        {"ui", {
            {"color_scheme", scheme},
            {"show_performance_overlay", settings.ui.show_performance_overlay},
            {"start_minimized", settings.ui.start_minimized},
            {"window_width", settings.ui.window_width},
            {"window_height", settings.ui.window_height},
            {"window_maximized", settings.ui.window_maximized},
        }},
    };
}

std::expected<Settings, Error> settingsFromJson(const Json& document) {
    if (!document.is_object()) {
        return std::unexpected(error(ErrorCode::malformed_data,
                                     "settings document must be a JSON object"));
    }
    auto version = readUnsigned(document, "schema_version",
                                std::numeric_limits<std::uint32_t>::max());
    if (!version) return std::unexpected(version.error());
    if (*version != current_schema_version) {
        return std::unexpected(error(ErrorCode::unsupported_schema_version,
            "settings schema version " + std::to_string(*version) +
            " is not supported (current version is " +
            std::to_string(current_schema_version) + ")"));
    }

    const auto& recording = document.at("recording");
    const auto& audio = document.at("audio");
    const auto& ui = document.at("ui");
    if (!recording.is_object() || !audio.is_object() || !ui.is_object()) {
        return std::unexpected(error(ErrorCode::malformed_data,
            "recording, audio, and ui settings must be JSON objects"));
    }

    Settings result;
    auto output_directory = readString(recording, "output_directory");
    if (!output_directory) return std::unexpected(output_directory.error());
    result.recording.output_directory = std::move(*output_directory);

    auto codec = readString(recording, "codec");
    if (!codec) return std::unexpected(codec.error());
    if (*codec == "h264") result.recording.codec = VideoCodec::h264;
    else if (*codec == "hevc") result.recording.codec = VideoCodec::hevc;
    else return std::unexpected(error(ErrorCode::malformed_data,
                                     "setting 'codec' must be 'h264' or 'hevc'"));

    auto rate_control = readString(recording, "rate_control");
    if (!rate_control) return std::unexpected(rate_control.error());
    if (*rate_control == "bitrate") {
        result.recording.rate_control = VideoRateControl::bitrate;
    } else if (*rate_control == "constant_qp") {
        result.recording.rate_control = VideoRateControl::constant_quantizer;
    } else {
        return std::unexpected(error(ErrorCode::malformed_data,
            "setting 'rate_control' must be 'bitrate' or 'constant_qp'"));
    }

    auto bitrate = readUnsigned(recording, "bitrate_bps", 500'000'000);
    if (!bitrate) return std::unexpected(bitrate.error());
    result.recording.bitrate_bps = *bitrate;
    auto qp = readUnsigned(recording, "qp", 51);
    if (!qp) return std::unexpected(qp.error());
    result.recording.qp = static_cast<std::uint32_t>(*qp);
    auto gop = readUnsigned(recording, "gop_size", 3600);
    if (!gop) return std::unexpected(gop.error());
    result.recording.gop_size = static_cast<std::uint32_t>(*gop);
    auto display = readString(recording, "display_name");
    if (!display) return std::unexpected(display.error());
    result.recording.display_name = std::move(*display);
    auto output_width = readUnsigned(recording, "output_width", 16'384);
    if (!output_width) return std::unexpected(output_width.error());
    result.recording.output_width = static_cast<std::uint32_t>(*output_width);
    auto output_height = readUnsigned(recording, "output_height", 16'384);
    if (!output_height) return std::unexpected(output_height.error());
    result.recording.output_height = static_cast<std::uint32_t>(*output_height);
    auto max_fps = readUnsigned(recording, "max_fps", 360);
    if (!max_fps) return std::unexpected(max_fps.error());
    result.recording.max_fps = static_cast<std::uint32_t>(*max_fps);
    auto encoder_tune = readString(recording, "encoder_tune");
    if (!encoder_tune) return std::unexpected(encoder_tune.error());
    if (*encoder_tune == "default") result.recording.encoder_tune = VideoEncoderTune::default_tuning;
    else if (*encoder_tune == "ll") result.recording.encoder_tune = VideoEncoderTune::ll;
    else if (*encoder_tune == "ull") result.recording.encoder_tune = VideoEncoderTune::ull;
    else if (*encoder_tune == "hq") result.recording.encoder_tune = VideoEncoderTune::hq;
    else return std::unexpected(error(ErrorCode::malformed_data,
                                      "setting 'encoder_tune' must be default, ll, ull, or hq"));
    auto pool = readUnsigned(recording, "capture_pool_size", 64);
    if (!pool) return std::unexpected(pool.error());
    result.recording.capture_pool_size = static_cast<std::uint32_t>(*pool);
    auto depth = readUnsigned(recording, "async_depth", 64);
    if (!depth) return std::unexpected(depth.error());
    result.recording.async_depth = static_cast<std::uint32_t>(*depth);
    auto replay = readUnsigned(recording, "replay_duration_seconds", 3600);
    if (!replay) return std::unexpected(replay.error());
    result.recording.replay_duration_seconds = static_cast<std::uint32_t>(*replay);

    auto desktop = audioFromJson(audio.at("desktop"), "desktop audio");
    if (!desktop) return std::unexpected(desktop.error());
    result.desktop_audio = std::move(*desktop);
    auto microphone = audioFromJson(audio.at("microphone"), "microphone audio");
    if (!microphone) return std::unexpected(microphone.error());
    result.microphone_audio = std::move(*microphone);

    auto scheme = readString(ui, "color_scheme");
    if (!scheme) return std::unexpected(scheme.error());
    if (*scheme == "system") result.ui.color_scheme = ColorScheme::system;
    else if (*scheme == "light") result.ui.color_scheme = ColorScheme::light;
    else if (*scheme == "dark") result.ui.color_scheme = ColorScheme::dark;
    else return std::unexpected(error(ErrorCode::malformed_data,
                                     "setting 'color_scheme' must be system, light, or dark"));
    auto overlay = readBool(ui, "show_performance_overlay");
    if (!overlay) return std::unexpected(overlay.error());
    result.ui.show_performance_overlay = *overlay;
    auto minimized = readBool(ui, "start_minimized");
    if (!minimized) return std::unexpected(minimized.error());
    result.ui.start_minimized = *minimized;
    auto width = readUnsigned(ui, "window_width", 16384);
    if (!width) return std::unexpected(width.error());
    result.ui.window_width = static_cast<std::uint32_t>(*width);
    auto height = readUnsigned(ui, "window_height", 16384);
    if (!height) return std::unexpected(height.error());
    result.ui.window_height = static_cast<std::uint32_t>(*height);
    auto maximized = readBool(ui, "window_maximized");
    if (!maximized) return std::unexpected(maximized.error());
    result.ui.window_maximized = *maximized;

    if (auto validation = validate(result); !validation) {
        return std::unexpected(error(ErrorCode::malformed_data, validation.error().message));
    }
    return result;
}

class FileDescriptor final {
public:
    explicit FileDescriptor(int fd = -1) noexcept : fd_(fd) {}
    ~FileDescriptor() { if (fd_ >= 0) ::close(fd_); }
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    FileDescriptor(FileDescriptor&& other) noexcept : fd_(other.release()) {}
    FileDescriptor& operator=(FileDescriptor&& other) noexcept {
        if (this != &other) {
            if (fd_ >= 0) ::close(fd_);
            fd_ = other.release();
        }
        return *this;
    }
    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] int release() noexcept { const int result = fd_; fd_ = -1; return result; }
private:
    int fd_;
};

std::expected<std::string, Error> readFile(const std::filesystem::path& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        const int saved_errno = errno;
        if (saved_errno == ENOENT) {
            return std::unexpected(error(ErrorCode::not_found,
                                         "settings file does not exist: " + path.string()));
        }
        return std::unexpected(error(ErrorCode::io_error,
            "cannot open settings file '" + path.string() + "': " +
            std::strerror(saved_errno)));
    }
    FileDescriptor file(fd);
    struct stat status {};
    if (::fstat(fd, &status) != 0) {
        return std::unexpected(error(ErrorCode::io_error,
                                     systemError("cannot inspect settings file")));
    }
    if (!S_ISREG(status.st_mode)) {
        return std::unexpected(error(ErrorCode::io_error,
                                     "settings path is not a regular file"));
    }
    if (status.st_size < 0 || static_cast<std::uint64_t>(status.st_size) > max_config_bytes) {
        return std::unexpected(error(ErrorCode::malformed_data,
                                     "settings file exceeds the 1 MiB size limit"));
    }

    std::string contents;
    contents.reserve(static_cast<std::size_t>(status.st_size));
    char chunk[8192];
    for (;;) {
        const auto count = ::read(fd, chunk, sizeof(chunk));
        if (count == 0) break;
        if (count < 0) {
            if (errno == EINTR) continue;
            return std::unexpected(error(ErrorCode::io_error,
                                         systemError("cannot read settings file")));
        }
        const auto amount = static_cast<std::size_t>(count);
        if (amount > max_config_bytes - contents.size()) {
            return std::unexpected(error(ErrorCode::malformed_data,
                                         "settings file exceeds the 1 MiB size limit"));
        }
        contents.append(chunk, amount);
    }
    return contents;
}

std::expected<std::filesystem::path, Error> checkedParent(const std::filesystem::path& path) {
    if (path.empty() || path.filename().empty() || path.filename() == "." ||
        path.filename() == "..") {
        return std::unexpected(error(ErrorCode::invalid_config_path,
                                     "settings file path must include a filename"));
    }
    auto parent = path.parent_path();
    if (parent.empty()) parent = ".";
    return parent;
}

std::expected<void, Error> writeAll(int fd, std::string_view contents) {
    std::size_t written = 0;
    while (written < contents.size()) {
        const auto count = ::write(fd, contents.data() + written, contents.size() - written);
        if (count < 0) {
            if (errno == EINTR) continue;
            return std::unexpected(error(ErrorCode::io_error, systemError("writing settings")));
        }
        if (count == 0) {
            return std::unexpected(error(ErrorCode::io_error,
                                         "writing settings made no progress"));
        }
        written += static_cast<std::size_t>(count);
    }
    return {};
}

} // namespace

std::expected<Settings, Error> makeDefaults() {
    const char* home_value = std::getenv("HOME");
    if (home_value == nullptr || home_value[0] == '\0') {
        return std::unexpected(error(ErrorCode::invalid_config_path,
                                     "HOME is unset; cannot choose a default output directory"));
    }
    const std::filesystem::path home{home_value};
    if (!home.is_absolute() || !validText(home.string(), 4096, false, false)) {
        return std::unexpected(error(ErrorCode::invalid_config_path,
                                     "HOME must be an absolute UTF-8 path"));
    }
    Settings settings;
    settings.recording.output_directory = (home / "Videos" / "Fluxrec").string();
    if (auto result = validate(settings); !result) return std::unexpected(result.error());
    return settings;
}

std::expected<void, Error> validate(const Settings& settings) {
    const auto& recording = settings.recording;
    if (!validText(recording.output_directory, 4096, false, false) ||
        !std::filesystem::path(recording.output_directory).is_absolute()) {
        return std::unexpected(error(ErrorCode::invalid_settings,
            "output directory must be a nonempty absolute UTF-8 path of at most 4096 bytes"));
    }
    if (recording.codec != VideoCodec::h264 && recording.codec != VideoCodec::hevc) {
        return std::unexpected(error(ErrorCode::invalid_settings,
                                     "video codec must be H.264 or HEVC"));
    }
    if (recording.rate_control != VideoRateControl::bitrate &&
        recording.rate_control != VideoRateControl::constant_quantizer) {
        return std::unexpected(error(ErrorCode::invalid_settings,
                                     "video rate control must be bitrate or constant QP"));
    }
    if (recording.bitrate_bps < 1'000'000 || recording.bitrate_bps > 500'000'000) {
        return std::unexpected(error(ErrorCode::invalid_settings,
                                     "video bitrate must be between 1 and 500 Mbps"));
    }
    if (recording.qp > 51) {
        return std::unexpected(error(ErrorCode::invalid_settings,
                                     "constant-QP value must be between 0 and 51"));
    }
    if (recording.gop_size < 1 || recording.gop_size > 3600) {
        return std::unexpected(error(ErrorCode::invalid_settings,
                                     "GOP size must be between 1 and 3600 frames"));
    }
    if (!validText(recording.display_name, 255, true, false)) {
        return std::unexpected(error(ErrorCode::invalid_settings,
            "display name must be valid UTF-8, at most 255 bytes, and contain no controls"));
    }
    const bool native_size = recording.output_width == 0 && recording.output_height == 0;
    if (!native_size &&
        (recording.output_width < 4 || recording.output_width > 16'384 ||
         recording.output_height < 4 || recording.output_height > 16'384 ||
         (recording.output_width & 1U) != 0 || (recording.output_height & 1U) != 0)) {
        return std::unexpected(error(ErrorCode::invalid_settings,
            "output width and height must both be zero for native size, or even values "
            "between 4 and 16384"));
    }
    if (recording.max_fps != 0 && recording.max_fps != 60 &&
        recording.max_fps != 120 && recording.max_fps != 240 &&
        recording.max_fps != 360) {
        return std::unexpected(error(ErrorCode::invalid_settings,
            "maximum capture rate must be 0 (uncapped), 60, 120, 240, or 360 FPS"));
    }
    if (recording.encoder_tune != VideoEncoderTune::default_tuning &&
        recording.encoder_tune != VideoEncoderTune::ll &&
        recording.encoder_tune != VideoEncoderTune::ull &&
        recording.encoder_tune != VideoEncoderTune::hq) {
        return std::unexpected(error(ErrorCode::invalid_settings,
                                     "video encoder tune must be default, ll, ull, or hq"));
    }
    if (recording.capture_pool_size < 2 || recording.capture_pool_size > 64) {
        return std::unexpected(error(ErrorCode::invalid_settings,
                                     "capture pool size must be between 2 and 64"));
    }
    if (recording.async_depth < 1 || recording.async_depth > 64) {
        return std::unexpected(error(ErrorCode::invalid_settings,
                                     "encoder async depth must be between 1 and 64"));
    }
    if (recording.replay_duration_seconds > 3600) {
        return std::unexpected(error(ErrorCode::invalid_settings,
                                     "replay duration must be between 0 and 3600 seconds"));
    }
    if (auto result = validateAudio(settings.desktop_audio, "desktop audio"); !result) return result;
    if (auto result = validateAudio(settings.microphone_audio, "microphone audio"); !result) return result;

    if (settings.ui.color_scheme != ColorScheme::system &&
        settings.ui.color_scheme != ColorScheme::light &&
        settings.ui.color_scheme != ColorScheme::dark) {
        return std::unexpected(error(ErrorCode::invalid_settings,
                                     "color scheme must be system, light, or dark"));
    }
    if (settings.ui.window_width < 640 || settings.ui.window_width > 16384 ||
        settings.ui.window_height < 480 || settings.ui.window_height > 16384) {
        return std::unexpected(error(ErrorCode::invalid_settings,
                                     "UI window dimensions are outside the supported range"));
    }
    return {};
}

std::string_view toString(VideoCodec codec) noexcept {
    switch (codec) {
    case VideoCodec::h264: return "h264";
    case VideoCodec::hevc: return "hevc";
    }
    return "invalid";
}

std::string_view toString(VideoRateControl rate_control) noexcept {
    switch (rate_control) {
    case VideoRateControl::bitrate: return "bitrate";
    case VideoRateControl::constant_quantizer: return "constant_qp";
    }
    return "invalid";
}

std::string_view toString(ColorScheme scheme) noexcept {
    switch (scheme) {
    case ColorScheme::system: return "system";
    case ColorScheme::light: return "light";
    case ColorScheme::dark: return "dark";
    }
    return "invalid";
}

std::string_view toString(VideoEncoderTune tune) noexcept {
    switch (tune) {
    case VideoEncoderTune::default_tuning: return "default";
    case VideoEncoderTune::ll: return "ll";
    case VideoEncoderTune::ull: return "ull";
    case VideoEncoderTune::hq: return "hq";
    }
    return "invalid";
}

Store::Store(std::filesystem::path file_path) : file_path_(std::move(file_path)) {}

std::expected<Store, Error> Store::forCurrentUser() {
    std::filesystem::path config_home;
    const char* xdg_value = std::getenv("XDG_CONFIG_HOME");
    if (xdg_value != nullptr && xdg_value[0] != '\0') {
        config_home = xdg_value;
        if (!config_home.is_absolute() || !validText(config_home.string(), 4096, false, false)) {
            return std::unexpected(error(ErrorCode::invalid_config_path,
                "XDG_CONFIG_HOME must be an absolute UTF-8 path"));
        }
    } else {
        const char* home_value = std::getenv("HOME");
        if (home_value == nullptr || home_value[0] == '\0') {
            return std::unexpected(error(ErrorCode::invalid_config_path,
                "HOME is unset and XDG_CONFIG_HOME is unavailable"));
        }
        const std::filesystem::path home{home_value};
        if (!home.is_absolute() || !validText(home.string(), 4096, false, false)) {
            return std::unexpected(error(ErrorCode::invalid_config_path,
                                         "HOME must be an absolute UTF-8 path"));
        }
        config_home = home / ".config";
    }
    return Store(config_home / "fluxrec" / "settings.json");
}

std::expected<Settings, Error> Store::load() const {
    if (auto parent = checkedParent(file_path_); !parent) {
        return std::unexpected(parent.error());
    }
    auto contents = readFile(file_path_);
    if (!contents) return std::unexpected(contents.error());

    try {
        const Json document = Json::parse(*contents);
        try {
            return settingsFromJson(document);
        } catch (const Json::exception& exception) {
            return std::unexpected(error(ErrorCode::malformed_data,
                std::string("settings document is missing or has an invalid field: ") +
                exception.what()));
        }
    } catch (const Json::exception& exception) {
        return std::unexpected(error(ErrorCode::malformed_data,
            std::string("settings file contains invalid JSON: ") + exception.what()));
    }
}

std::expected<void, Error> Store::save(const Settings& settings) const {
    if (auto parent_result = checkedParent(file_path_); !parent_result) {
        return std::unexpected(parent_result.error());
    }
    if (auto validation = validate(settings); !validation) return validation;

    std::string contents;
    try {
        contents = settingsToJson(settings).dump(2) + "\n";
    } catch (const Json::exception& exception) {
        return std::unexpected(error(ErrorCode::invalid_settings,
            std::string("cannot serialize settings: ") + exception.what()));
    }
    if (contents.size() > max_config_bytes) {
        return std::unexpected(error(ErrorCode::invalid_settings,
                                     "serialized settings exceed the 1 MiB size limit"));
    }

    const auto parent = file_path_.parent_path().empty()
        ? std::filesystem::path(".") : file_path_.parent_path();
    std::error_code filesystem_error;
    const bool parent_existed = std::filesystem::exists(parent, filesystem_error);
    if (filesystem_error) {
        return std::unexpected(error(ErrorCode::io_error,
            "cannot inspect settings directory '" + parent.string() + "': " +
            filesystem_error.message()));
    }
    std::filesystem::create_directories(parent, filesystem_error);
    if (filesystem_error) {
        return std::unexpected(error(ErrorCode::io_error,
            "cannot create settings directory '" + parent.string() + "': " +
            filesystem_error.message()));
    }
    if (!parent_existed && ::chmod(parent.c_str(), 0700) != 0) {
        return std::unexpected(error(ErrorCode::io_error,
                                     systemError("cannot protect settings directory")));
    }

    FileDescriptor directory(::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (directory.get() < 0) {
        return std::unexpected(error(ErrorCode::io_error,
                                     systemError("cannot open settings directory")));
    }

    static std::atomic<std::uint64_t> sequence{0};
    std::string temporary_name;
    FileDescriptor temporary;
    for (unsigned int attempt = 0; attempt < 128; ++attempt) {
        temporary_name = ".fluxrec-settings." + std::to_string(::getpid()) + "." +
                         std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)) +
                         ".tmp";
        const int fd = ::openat(directory.get(), temporary_name.c_str(),
                                O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd >= 0) {
            temporary = FileDescriptor(fd);
            break;
        }
        if (errno != EEXIST) {
            return std::unexpected(error(ErrorCode::io_error,
                                         systemError("cannot create temporary settings file")));
        }
    }
    if (temporary.get() < 0) {
        return std::unexpected(error(ErrorCode::io_error,
            "could not allocate a unique temporary settings file"));
    }

    if (auto write_result = writeAll(temporary.get(), contents); !write_result) {
        ::unlinkat(directory.get(), temporary_name.c_str(), 0);
        return write_result;
    }
    if (::fsync(temporary.get()) != 0) {
        const auto failure = error(ErrorCode::io_error, systemError("cannot sync settings file"));
        ::unlinkat(directory.get(), temporary_name.c_str(), 0);
        return std::unexpected(failure);
    }
    const int temp_fd = temporary.release();
    if (::close(temp_fd) != 0) {
        const auto failure = error(ErrorCode::io_error, systemError("cannot close settings file"));
        ::unlinkat(directory.get(), temporary_name.c_str(), 0);
        return std::unexpected(failure);
    }

    if (::renameat(directory.get(), temporary_name.c_str(), directory.get(),
                   file_path_.filename().c_str()) != 0) {
        const auto failure = error(ErrorCode::io_error, systemError("cannot replace settings file"));
        ::unlinkat(directory.get(), temporary_name.c_str(), 0);
        return std::unexpected(failure);
    }
    if (::fsync(directory.get()) != 0) {
        return std::unexpected(error(ErrorCode::io_error,
                                     systemError("cannot sync settings directory")));
    }
    return {};
}

const std::filesystem::path& Store::filePath() const noexcept { return file_path_; }

} // namespace fluxrec::config
