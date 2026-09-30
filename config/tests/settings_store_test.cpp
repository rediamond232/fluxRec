#include "config/settings.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

using fluxrec::config::ColorScheme;
using fluxrec::config::ErrorCode;
using fluxrec::config::VideoRateControl;
using fluxrec::config::VideoEncoderTune;
using fluxrec::config::VideoCodec;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        std::string pattern = (std::filesystem::temp_directory_path() /
                               "fluxrec-config-test.XXXXXX").string();
        pattern.push_back('\0');
        char* result = ::mkdtemp(pattern.data());
        if (result == nullptr) throw std::runtime_error("mkdtemp failed");
        path_ = result;
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

class Environment final {
public:
    Environment(const char* name, const std::string& value) : name_(name) {
        if (const char* previous = std::getenv(name); previous != nullptr) {
            had_previous_ = true;
            previous_ = previous;
        }
        if (::setenv(name, value.c_str(), 1) != 0) {
            throw std::runtime_error(std::string("setenv failed for ") + name);
        }
    }
    ~Environment() {
        if (had_previous_) ::setenv(name_.c_str(), previous_.c_str(), 1);
        else ::unsetenv(name_.c_str());
    }

private:
    std::string name_;
    std::string previous_;
    bool had_previous_{false};
};

void writeText(const std::filesystem::path& path, const std::string& contents) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("cannot open settings file for test input");
    output << contents;
    output.close();
    if (!output) throw std::runtime_error("cannot write settings test input");
}

void testStoreRoundTripAndErrors() {
    TemporaryDirectory temporary;
    const auto home = temporary.path() / "home";
    const auto config_home = temporary.path() / "xdg-config";
    Environment home_environment("HOME", home.string());
    Environment xdg_environment("XDG_CONFIG_HOME", config_home.string());

    auto store_result = fluxrec::config::Store::forCurrentUser();
    require(store_result.has_value(), "XDG store resolution failed");
    auto& store = *store_result;
    const auto expected_file = config_home / "fluxrec" / "settings.json";
    require(store.filePath() == expected_file, "XDG config path resolution was incorrect");

    auto missing = store.load();
    require(!missing && missing.error().code == ErrorCode::not_found,
            "missing settings must return the explicit not_found error");

    auto defaults = fluxrec::config::makeDefaults();
    require(defaults.has_value(), "explicit defaults could not be constructed");
    require(defaults->recording.output_directory ==
                (home / "Videos" / "Fluxrec").string(),
            "default output directory did not use HOME");
    require(fluxrec::config::validate(*defaults).has_value(),
            "constructed defaults did not validate");
    require(defaults->recording.capture_pool_size == 48,
            "default capture pool size should match the tested recording configuration");
    require(defaults->recording.rate_control == VideoRateControl::bitrate &&
            defaults->recording.qp == 28,
            "default video rate control should be bitrate with an inactive saved QP of 28");

    auto settings = *defaults;
    settings.recording.codec = VideoCodec::hevc;
    settings.recording.rate_control = VideoRateControl::constant_quantizer;
    settings.recording.bitrate_bps = 123'456'789;
    settings.recording.qp = 31;
    settings.recording.gop_size = 72;
    settings.recording.display_name = "DP-test";
    settings.recording.output_width = 1920;
    settings.recording.output_height = 1080;
    settings.recording.max_fps = 240;
    settings.recording.encoder_tune = VideoEncoderTune::hq;
    settings.recording.capture_pool_size = 16;
    settings.recording.async_depth = 5;
    settings.recording.replay_duration_seconds = 90;
    settings.desktop_audio = {"alsa_output.test.monitor", 0.75F, true};
    settings.microphone_audio = {"alsa_input.test", 1.25F, false};
    settings.ui.color_scheme = ColorScheme::dark;
    settings.ui.show_performance_overlay = false;
    settings.ui.start_minimized = true;
    settings.ui.window_width = 1440;
    settings.ui.window_height = 900;
    settings.ui.window_maximized = true;
    require(fluxrec::config::validate(settings).has_value(),
            "representative settings did not validate");
    auto incomplete_resolution = settings;
    incomplete_resolution.recording.output_height = 0;
    require(!fluxrec::config::validate(incomplete_resolution),
            "a partially specified output resolution must be rejected");
    auto unsupported_capture_rate = settings;
    unsupported_capture_rate.recording.max_fps = 90;
    require(!fluxrec::config::validate(unsupported_capture_rate),
            "an unsupported max capture rate must be rejected");
    auto unsupported_qp = settings;
    unsupported_qp.recording.qp = 52;
    require(!fluxrec::config::validate(unsupported_qp),
            "QP above the supported 8-bit range must be rejected");

    auto saved = store.save(settings);
    require(saved.has_value(), saved ? "" : saved.error().message);
    struct stat file_status {};
    require(::stat(expected_file.c_str(), &file_status) == 0,
            "saved settings file was not created");
    require((file_status.st_mode & 0777) == 0600,
            "saved settings file must have mode 0600");

    auto loaded = store.load();
    require(loaded.has_value(), loaded ? "" : loaded.error().message);
    const auto& actual = *loaded;
    require(actual.recording.output_directory == settings.recording.output_directory &&
            actual.recording.codec == settings.recording.codec &&
            actual.recording.rate_control == settings.recording.rate_control &&
            actual.recording.bitrate_bps == settings.recording.bitrate_bps &&
            actual.recording.qp == settings.recording.qp &&
            actual.recording.gop_size == settings.recording.gop_size &&
            actual.recording.display_name == settings.recording.display_name &&
            actual.recording.output_width == settings.recording.output_width &&
            actual.recording.output_height == settings.recording.output_height &&
            actual.recording.max_fps == settings.recording.max_fps &&
            actual.recording.encoder_tune == settings.recording.encoder_tune &&
            actual.recording.capture_pool_size == settings.recording.capture_pool_size &&
            actual.recording.async_depth == settings.recording.async_depth &&
            actual.recording.replay_duration_seconds == settings.recording.replay_duration_seconds,
            "recording settings did not survive the round trip");
    require(actual.desktop_audio.device == settings.desktop_audio.device &&
            actual.desktop_audio.gain == settings.desktop_audio.gain &&
            actual.desktop_audio.muted == settings.desktop_audio.muted &&
            actual.microphone_audio.device == settings.microphone_audio.device &&
            actual.microphone_audio.gain == settings.microphone_audio.gain &&
            actual.microphone_audio.muted == settings.microphone_audio.muted,
            "audio settings did not survive the round trip");
    require(actual.ui.color_scheme == settings.ui.color_scheme &&
            actual.ui.show_performance_overlay == settings.ui.show_performance_overlay &&
            actual.ui.start_minimized == settings.ui.start_minimized &&
            actual.ui.window_width == settings.ui.window_width &&
            actual.ui.window_height == settings.ui.window_height &&
            actual.ui.window_maximized == settings.ui.window_maximized,
            "UI settings did not survive the round trip");

    writeText(expected_file, "{\n");
    auto malformed = store.load();
    require(!malformed && malformed.error().code == ErrorCode::malformed_data,
            "malformed JSON must return the explicit malformed_data error");

    writeText(expected_file, "{\"schema_version\": 999}\n");
    auto future_version = store.load();
    require(!future_version &&
            future_version.error().code == ErrorCode::unsupported_schema_version,
            "unknown schema versions must return the explicit unsupported version error");

    writeText(expected_file, "{\"schema_version\": 1}\n");
    auto previous_version = store.load();
    require(!previous_version &&
            previous_version.error().code == ErrorCode::unsupported_schema_version,
            "older schema versions must not silently omit newly added settings");
}

} // namespace

int main() {
    try {
        testStoreRoundTripAndErrors();
        std::cout << "config settings store test passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "config settings store test failed: " << exception.what() << '\n';
        return 1;
    }
}
