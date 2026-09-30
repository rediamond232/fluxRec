#include "capture_pts_rate_limit.hpp"

#include <cstdint>
#include <iostream>
#include <optional>
#include <vector>

namespace {

using Limit = fluxrec::capture::detail::CapturePtsRateLimit;
using Clock = Limit::Clock;

bool check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

bool test_unlimited() {
    Limit limit{0};
    const auto now = Clock::time_point{};
    return check(limit.admit(100U, now), "uncapped mode should admit a frame") &&
           check(limit.admit(100U, now), "uncapped mode should not reject repeated timestamps");
}

bool test_jitter_near_360_cap() {
    Limit limit{360};
    std::uint64_t pts = 50'000'000U;
    constexpr std::uint64_t short_interval_ns = 2'777'000U;
    constexpr std::uint64_t long_interval_ns = 2'788'000U;
    constexpr std::size_t samples = 10'001;

    for (std::size_t index = 0; index < samples; ++index) {
        if (index != 0U) {
            // Nine intervals just below the nominal 360 Hz period followed by
            // one interval that repays the jitter; the average stays below
            // the cap while a strict pairwise-spacing test would reject.
            pts += index % 10U == 0U ? long_interval_ns : short_interval_ns;
        }
        if (!limit.admit(pts, Clock::time_point{})) {
            return check(false, "bounded PTS credit should absorb native-rate jitter");
        }
    }
    return true;
}

bool test_two_token_output_bound() {
    constexpr std::uint32_t fps = 120U;
    constexpr std::uint64_t source_interval_ns = 1'000'000U;
    constexpr std::size_t source_samples = 4'001;
    Limit limit{fps};
    std::vector<std::uint64_t> admitted_pts;

    for (std::size_t index = 0; index < source_samples; ++index) {
        const std::uint64_t pts = static_cast<std::uint64_t>(index) * source_interval_ns;
        if (limit.admit(pts, Clock::time_point{})) {
            admitted_pts.push_back(pts);
        }
    }

    if (!check(!admitted_pts.empty(), "over-cap source should still produce admitted frames")) {
        return false;
    }
    for (std::size_t first = 0; first < admitted_pts.size(); ++first) {
        for (std::size_t last = first; last < admitted_pts.size(); ++last) {
            const std::uint64_t count = static_cast<std::uint64_t>(last - first + 1U);
            if (count <= 2U) {
                continue;
            }
            const std::uint64_t span_ns = admitted_pts[last] - admitted_pts[first];
            const unsigned __int128 required_credit =
                static_cast<unsigned __int128>(count - 2U) * 1'000'000'000ULL;
            const unsigned __int128 available_credit =
                static_cast<unsigned __int128>(span_ns) * fps;
            if (required_credit > available_credit) {
                return check(false, "admissions exceeded the documented max_fps*T+2 bound");
            }
        }
    }
    return true;
}

bool test_bad_pts_rejected_and_high_water_retained() {
    Limit limit{60};
    const auto now = Clock::time_point{};
    if (!check(limit.admit(10'000U, now), "first PTS frame should be admitted") ||
        !check(!limit.admit(10'000U, now), "repeated PTS should be rejected") ||
        !check(!limit.admit(9'000U, now), "backward PTS should be rejected")) {
        return false;
    }
    return check(limit.admit(20'000'010U, now),
                 "a later increasing PTS should refill from the retained high-water mark");
}

bool test_missing_pts_uses_separate_steady_domain() {
    Limit limit{60};
    const auto start = Clock::time_point{};
    if (!check(limit.admit(100U, start), "initial PTS frame should be admitted") ||
        !check(limit.admit(std::nullopt, start + std::chrono::seconds{1}),
               "first missing-PTS frame should use remaining bucket credit") ||
        !check(!limit.admit(std::nullopt, start + std::chrono::milliseconds{1010}),
               "a 10 ms steady interval should not refill a full frame token")) {
        return false;
    }
    const std::uint64_t credit_before_switch = limit.credit_units();
    if (!check(!limit.admit(100U, start + std::chrono::milliseconds{1015}),
               "a repeated PTS should be rejected without changing clock domains") ||
        !check(!limit.admit(1'000'000'100U, start + std::chrono::milliseconds{1020}),
               "switching back to PTS should not credit a cross-domain clock jump") ||
        !check(limit.credit_units() == credit_before_switch,
               "a clock-domain switch should preserve, not refill, bucket credit")) {
        return false;
    }
    return check(limit.admit(1'010'000'100U, start + std::chrono::milliseconds{1030}),
                 "the next PTS interval should refill normally after re-anchoring");
}

} // namespace

int main() {
    return test_unlimited() && test_jitter_near_360_cap() && test_two_token_output_bound() &&
           test_bad_pts_rejected_and_high_water_retained() &&
           test_missing_pts_uses_separate_steady_domain()
        ? 0
        : 1;
}
