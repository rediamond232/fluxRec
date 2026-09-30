#include "capture_lifecycle.hpp"

#include <cstdint>
#include <iostream>

namespace {

bool check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

bool test_selected_output_removal_is_terminal_before_and_during_capture() {
    using fluxrec::capture::CaptureStatus;
    using fluxrec::capture::detail::should_fail_selected_output_removal;
    constexpr std::uint32_t selected_global = 23U;

    return check(should_fail_selected_output_removal(
                     CaptureStatus::connected, selected_global, selected_global),
                 "selected output removal while connected must fail before start") &&
           check(should_fail_selected_output_removal(
                     CaptureStatus::running, selected_global, selected_global),
                 "selected output removal while running must fail capture") &&
           check(!should_fail_selected_output_removal(
                     CaptureStatus::connected, 24U, selected_global),
                 "removal of another output must not fail capture") &&
           check(!should_fail_selected_output_removal(
                     CaptureStatus::connected, selected_global, 0U),
                 "an unselected output must not fail capture") &&
           check(!should_fail_selected_output_removal(
                     CaptureStatus::stopped, selected_global, selected_global) &&
                     !should_fail_selected_output_removal(
                         CaptureStatus::failed, selected_global, selected_global),
                 "output removal after capture termination must not change status");
}

} // namespace

int main() {
    return test_selected_output_removal_is_terminal_before_and_during_capture() ? 0 : 1;
}
