#include "capture_rate_limit.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>

namespace {

using RateLimit = fluxrec::capture::detail::CaptureRateLimit;
using Clock = RateLimit::Clock;
using TimePoint = RateLimit::TimePoint;

bool check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

bool test_uncapped() {
    RateLimit limit{0};
    const TimePoint now{};
    limit.request_started(now);
    return check(limit.interval() == Clock::duration::zero(),
                 "zero max_fps should have a zero interval") &&
           check(!limit.deadline(), "uncapped mode should not create a deadline") &&
           check(limit.can_start(now), "uncapped mode should allow the next request immediately");
}

bool test_caps() {
    constexpr std::array<std::uint32_t, 4> rates{60U, 120U, 240U, 360U};
    for (const std::uint32_t rate : rates) {
        RateLimit limit{rate};
        const Clock::duration interval = limit.interval();
        const TimePoint initial{};
        if (!check(interval > Clock::duration::zero(), "a positive cap needs a positive interval") ||
            !check(limit.can_start(initial), "the first capture request should be immediate")) {
            return false;
        }

        limit.request_started(initial);
        const auto allowed_at = initial + interval;
        if (!check(!limit.can_start(allowed_at - Clock::duration{1}),
                   "a request before the cap interval must be blocked") ||
            !check(limit.can_start(allowed_at), "a request at the cap deadline should be allowed")) {
            return false;
        }

        // Simulate a frame whose compositor response takes many periods. The
        // next deadline must be anchored to the actual start, not an old tick.
        const auto delayed_start = allowed_at + interval * 8;
        if (!check(limit.can_start(delayed_start), "a delayed completion should not remain blocked")) {
            return false;
        }
        limit.request_started(delayed_start);
        if (!check(!limit.can_start(delayed_start),
                   "a delayed completion must not trigger a catch-up request burst") ||
            !check(limit.can_start(delayed_start + interval),
                   "the next request should become eligible one full interval later")) {
            return false;
        }

        const auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(interval).count();
        const std::uint64_t minimum_ns =
            (1'000'000'000ULL + static_cast<std::uint64_t>(rate) - 1U) / rate;
        if (!check(elapsed_ns >= static_cast<std::int64_t>(minimum_ns),
                   "rounded request interval must not exceed the configured FPS cap")) {
            return false;
        }

        RateLimit one_second_limit{rate};
        auto simulated_now = TimePoint{};
        const auto one_second = std::chrono::duration_cast<Clock::duration>(std::chrono::seconds{1});
        const auto end = simulated_now + one_second;
        std::uint32_t starts = 0;
        while (simulated_now < end) {
            if (one_second_limit.can_start(simulated_now)) {
                ++starts;
                one_second_limit.request_started(simulated_now);
                continue;
            }
            const auto next = one_second_limit.deadline();
            if (!next || *next <= simulated_now) {
                return check(false, "rate limiter failed to advance the next deadline");
            }
            simulated_now = *next;
        }
        if (!check(starts <= rate, "simulated one-second window exceeded the configured cap")) {
            return false;
        }
    }
    return true;
}

bool test_ppoll_timeout_precision() {
    const auto submillisecond = std::chrono::ceil<Clock::duration>(std::chrono::microseconds{500});
    const auto submillisecond_timeout = RateLimit::ppoll_timeout(submillisecond);
    if (!check(submillisecond_timeout.tv_sec == 0 && submillisecond_timeout.tv_nsec == 500'000,
               "ppoll timeout conversion must preserve a 500 microsecond wait")) {
        return false;
    }

    const auto frame_interval = std::chrono::ceil<Clock::duration>(std::chrono::nanoseconds{8'333'334});
    const auto frame_interval_timeout = RateLimit::ppoll_timeout(frame_interval);
    if (!check(frame_interval_timeout.tv_sec == 0 && frame_interval_timeout.tv_nsec == 8'333'334,
               "ppoll timeout conversion must preserve a sub-millisecond frame interval")) {
        return false;
    }

    const auto expired_timeout = RateLimit::ppoll_timeout(Clock::duration::zero());
    return check(expired_timeout.tv_sec == 0 && expired_timeout.tv_nsec == 0,
                 "expired deadlines should poll without a wait");
}

} // namespace

int main() {
    return test_uncapped() && test_caps() && test_ppoll_timeout_precision() ? 0 : 1;
}
