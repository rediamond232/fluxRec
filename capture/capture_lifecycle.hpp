#pragma once

#include "capture.hpp"

#include <cstdint>

namespace fluxrec::capture::detail {

[[nodiscard]] constexpr bool should_fail_selected_output_removal(
    CaptureStatus status, std::uint32_t removed_global,
    std::uint32_t selected_global) noexcept {
    return selected_global != 0U && removed_global == selected_global &&
           (status == CaptureStatus::connected || status == CaptureStatus::running);
}

} // namespace fluxrec::capture::detail
