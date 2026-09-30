#pragma once

#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace fluxrec::gpu {

enum class VulkanRateControlMode {
    automatic,
    cbr,
    vbr,
};

[[nodiscard]] constexpr std::string_view toString(
    VulkanRateControlMode mode) noexcept {
    switch (mode) {
        case VulkanRateControlMode::automatic: return "auto";
        case VulkanRateControlMode::cbr: return "cbr";
        case VulkanRateControlMode::vbr: return "vbr";
    }
    return "invalid";
}

// An absent environment value keeps FFmpeg's default auto mode. CQP callers
// should bypass this parser so the diagnostic override cannot affect CQP.
[[nodiscard]] inline std::expected<std::optional<VulkanRateControlMode>, std::string>
parseVulkanRateControlModeOverride(std::optional<std::string_view> value) {
    if (!value) return std::optional<VulkanRateControlMode>{};
    if (*value == "auto") return VulkanRateControlMode::automatic;
    if (*value == "cbr") return VulkanRateControlMode::cbr;
    if (*value == "vbr") return VulkanRateControlMode::vbr;
    return std::unexpected(std::string(
        "FLUXREC_GPU_RC_MODE must be auto, cbr, or vbr"));
}

} // namespace fluxrec::gpu
