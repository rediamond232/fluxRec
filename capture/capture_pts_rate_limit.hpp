#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace fluxrec::capture::detail {

// Limits frames admitted from compositor timestamps. Credit is measured in
// nanoseconds * frames/second, with 1e9 credit units per frame. The two-token
// capacity gives one startup token plus at most one frame of bounded jitter
// credit, so admissions over any PTS interval T are bounded by max_fps*T+2.
// Timestamp regressions and repeats are rejected without moving the PTS high
// water mark. Missing PTS uses steady-clock deltas, and clock-domain changes
// discard the interval at the boundary rather than crediting an unrelated
// timestamp jump.
class CapturePtsRateLimit {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    explicit CapturePtsRateLimit(std::uint32_t max_fps) noexcept
        : max_fps_(max_fps) {}

    [[nodiscard]] bool admit(std::optional<std::uint64_t> presentation_time_ns,
                             TimePoint ready_at) noexcept {
        if (max_fps_ == 0U) {
            return true;
        }

        if (!initialized_) {
            credit_units_ = capacity_units;
            initialized_ = true;
        }

        if (presentation_time_ns) {
            const std::uint64_t timestamp = *presentation_time_ns;
            if (last_presentation_time_ns_ && timestamp <= *last_presentation_time_ns_) {
                // Treat repeated and backward compositor timestamps as
                // anomalous completions. Keep both the PTS high-water mark
                // and the previous valid clock domain unchanged.
                return false;
            }

            if (previous_domain_ == ClockDomain::presentation &&
                last_presentation_time_ns_) {
                refill(timestamp - *last_presentation_time_ns_);
            }
            last_presentation_time_ns_ = timestamp;
            previous_domain_ = ClockDomain::presentation;
        } else {
            if (previous_domain_ == ClockDomain::steady && last_ready_at_ &&
                ready_at > *last_ready_at_) {
                const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    ready_at - *last_ready_at_);
                if (elapsed.count() > 0) {
                    refill(static_cast<std::uint64_t>(elapsed.count()));
                }
            }
            last_ready_at_ = ready_at;
            previous_domain_ = ClockDomain::steady;
        }

        if (credit_units_ < units_per_frame) {
            return false;
        }
        credit_units_ -= units_per_frame;
        return true;
    }

    [[nodiscard]] std::uint64_t credit_units() const noexcept {
        return credit_units_;
    }

    [[nodiscard]] std::uint64_t max_fps() const noexcept {
        return max_fps_;
    }

private:
    enum class ClockDomain : std::uint8_t { none, presentation, steady };

    static constexpr std::uint64_t units_per_frame = 1'000'000'000ULL;
    static constexpr std::uint64_t capacity_units = 2ULL * units_per_frame;

    void refill(std::uint64_t elapsed_ns) noexcept {
        // PTS and steady-clock deltas are both unsigned, but their product can
        // exceed 64 bits for large timestamp jumps or high configured rates.
        // A wide intermediate lets us clamp exactly at the small bucket cap.
        __extension__ typedef unsigned __int128 WideUnsigned;
        const WideUnsigned added = static_cast<WideUnsigned>(elapsed_ns) * max_fps_;
        const WideUnsigned total = static_cast<WideUnsigned>(credit_units_) + added;
        credit_units_ = total >= capacity_units
            ? capacity_units
            : static_cast<std::uint64_t>(total);
    }

    std::uint32_t max_fps_ = 0;
    std::uint64_t credit_units_ = 0;
    bool initialized_ = false;
    ClockDomain previous_domain_ = ClockDomain::none;
    std::optional<std::uint64_t> last_presentation_time_ns_;
    std::optional<TimePoint> last_ready_at_;
};

} // namespace fluxrec::capture::detail
