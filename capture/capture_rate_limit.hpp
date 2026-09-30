#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <time.h>

namespace fluxrec::capture::detail {

// A deterministic, worker-thread request pacer. Each actual request anchors a
// fresh deadline so delayed completions never cause catch-up bursts.
class CaptureRateLimit {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    using Duration = Clock::duration;

    explicit CaptureRateLimit(std::uint32_t max_fps) noexcept
        : interval_(interval_for(max_fps)) {}

    [[nodiscard]] bool can_start(TimePoint now) const noexcept {
        return interval_ == Duration::zero() || !next_start_allowed_ ||
               now >= *next_start_allowed_;
    }

    void request_started(TimePoint started_at) noexcept {
        if (interval_ != Duration::zero()) {
            next_start_allowed_ = started_at + interval_;
        }
    }

    [[nodiscard]] std::optional<TimePoint> deadline() const noexcept {
        return next_start_allowed_;
    }

    [[nodiscard]] Duration interval() const noexcept {
        return interval_;
    }

    [[nodiscard]] static ::timespec ppoll_timeout(Duration remaining) noexcept {
        if (remaining <= Duration::zero()) {
            return ::timespec{};
        }

        constexpr std::int64_t nanoseconds_per_second = 1'000'000'000;
        const std::int64_t total_nanoseconds =
            std::chrono::ceil<std::chrono::nanoseconds>(remaining).count();
        ::timespec result{};
        result.tv_sec = static_cast<decltype(result.tv_sec)>(
            total_nanoseconds / nanoseconds_per_second);
        result.tv_nsec = static_cast<decltype(result.tv_nsec)>(
            total_nanoseconds % nanoseconds_per_second);
        return result;
    }

private:
    static Duration interval_for(std::uint32_t max_fps) noexcept {
        if (max_fps == 0U) {
            return Duration::zero();
        }

        constexpr std::uint64_t nanoseconds_per_second = 1'000'000'000ULL;
        const std::uint64_t rate = max_fps;
        const std::uint64_t interval_ns =
            (nanoseconds_per_second + rate - 1U) / rate;
        const auto requested = std::chrono::nanoseconds{
            static_cast<std::chrono::nanoseconds::rep>(interval_ns)};
        return std::chrono::ceil<Duration>(requested);
    }

    Duration interval_ = Duration::zero();
    std::optional<TimePoint> next_start_allowed_;
};

} // namespace fluxrec::capture::detail
