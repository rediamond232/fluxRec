#include "gpu/rate_control_mode.hpp"

#include <array>
#include <iostream>
#include <optional>
#include <string_view>
#include <utility>

int main() {
    using fluxrec::gpu::VulkanRateControlMode;
    using fluxrec::gpu::parseVulkanRateControlModeOverride;

    bool ok = true;
    const auto unset = parseVulkanRateControlModeOverride(std::nullopt);
    ok &= unset.has_value() && !unset->has_value();

    const std::array<std::pair<std::string_view, VulkanRateControlMode>, 3> valid{{
        {"auto", VulkanRateControlMode::automatic},
        {"cbr", VulkanRateControlMode::cbr},
        {"vbr", VulkanRateControlMode::vbr},
    }};
    for (const auto& [text, expected] : valid) {
        const auto parsed = parseVulkanRateControlModeOverride(text);
        ok &= parsed.has_value() && parsed->has_value() && **parsed == expected;
    }

    for (const auto invalid : {std::string_view{}, std::string_view{"CBR"},
                                std::string_view{"driver"}, std::string_view{"cqp"}}) {
        const auto parsed = parseVulkanRateControlModeOverride(invalid);
        ok &= !parsed.has_value();
        if (!parsed) {
            ok &= parsed.error() == "FLUXREC_GPU_RC_MODE must be auto, cbr, or vbr";
        }
    }

    if (!ok) {
        std::cerr << "Vulkan bitrate rate-control override parsing failed\n";
        return 1;
    }
    return 0;
}
