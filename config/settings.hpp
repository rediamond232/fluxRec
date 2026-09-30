#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

namespace fluxrec::config {

inline constexpr std::uint32_t current_schema_version = 5;

enum class VideoCodec {
    h264,
    hevc,
};

enum class VideoRateControl {
    bitrate,
    constant_quantizer,
};

enum class ColorScheme {
    system,
    light,
    dark,
};

enum class VideoEncoderTune {
    default_tuning,
    ll,
    ull,
    hq,
};

struct AudioSourceSettings {
    // Empty selects the PipeWire default source or sink monitor.
    std::string device;
    float gain{1.0F};
    bool muted{false};
};

struct RecordingSettings {
    // An absolute directory keeps output placement independent of the launch cwd.
    std::string output_directory;
    VideoCodec codec{VideoCodec::h264};
    VideoRateControl rate_control{VideoRateControl::bitrate};
    // Kept while QP mode is selected so switching back restores the target.
    std::uint64_t bitrate_bps{50'000'000};
    // Used only in constant-quantizer mode; the value remains saved otherwise.
    std::uint32_t qp{28};
    // The current backend supports GOP size. It does not expose preset or B-frame controls.
    std::uint32_t gop_size{120};
    // Empty selects the default output chosen by the caller.
    std::string display_name;
    // Zero by zero means native capture size. Otherwise both dimensions must
    // be even and within the application's supported dimension bound.
    std::uint32_t output_width{0};
    std::uint32_t output_height{0};
    // Capture request pacing: zero is uncapped; positive rates use the UI's
    // supported tiers. Source presentation timestamps remain unchanged.
    std::uint32_t max_fps{0};
    VideoEncoderTune encoder_tune{VideoEncoderTune::default_tuning};
    std::uint32_t capture_pool_size{48};
    std::uint32_t async_depth{8};
    std::uint32_t replay_duration_seconds{30};
};

struct UiSettings {
    ColorScheme color_scheme{ColorScheme::system};
    bool show_performance_overlay{true};
    bool start_minimized{false};
    std::uint32_t window_width{1100};
    std::uint32_t window_height{760};
    bool window_maximized{false};
};

struct Settings {
    RecordingSettings recording;
    AudioSourceSettings desktop_audio;
    AudioSourceSettings microphone_audio;
    UiSettings ui;
};

enum class ErrorCode {
    invalid_settings,
    invalid_config_path,
    not_found,
    io_error,
    malformed_data,
    unsupported_schema_version,
};

struct Error {
    ErrorCode code{ErrorCode::io_error};
    std::string message;
};

// Produces explicit user defaults. Fails if HOME is unavailable or unusable;
// callers should surface that error instead of silently inventing a path.
[[nodiscard]] std::expected<Settings, Error> makeDefaults();

// Checks all ranges and path/string constraints before settings are used or saved.
[[nodiscard]] std::expected<void, Error> validate(const Settings& settings);

[[nodiscard]] std::string_view toString(VideoCodec codec) noexcept;
[[nodiscard]] std::string_view toString(VideoRateControl rate_control) noexcept;
[[nodiscard]] std::string_view toString(ColorScheme scheme) noexcept;
[[nodiscard]] std::string_view toString(VideoEncoderTune tune) noexcept;

class Store final {
public:
    explicit Store(std::filesystem::path file_path);

    // Resolves $XDG_CONFIG_HOME/fluxrec/settings.json, or
    // $HOME/.config/fluxrec/settings.json when XDG_CONFIG_HOME is unset.
    [[nodiscard]] static std::expected<Store, Error> forCurrentUser();

    // A missing, malformed, or invalid file is an error. Defaults are never
    // substituted implicitly; callers can explicitly call makeDefaults().
    [[nodiscard]] std::expected<Settings, Error> load() const;

    // Validates first, then atomically replaces the file. The temporary file is
    // created beside the destination with mode 0600 and both file and directory
    // are synced before success is returned.
    [[nodiscard]] std::expected<void, Error> save(const Settings& settings) const;

    [[nodiscard]] const std::filesystem::path& filePath() const noexcept;

private:
    std::filesystem::path file_path_;
};

} // namespace fluxrec::config
