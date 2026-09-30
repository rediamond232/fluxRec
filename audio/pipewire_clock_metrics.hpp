#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>

namespace fluxrec::audio {

[[nodiscard]] constexpr bool pipewire_time_can_stamp(
    std::int64_t now_ns, std::uint32_t rate_num,
    std::uint32_t rate_denom) noexcept {
    return now_ns > 0 && rate_num != 0 && rate_denom != 0;
}

struct PipeWireClockMetricsSnapshot {
    std::uint64_t native_timestamp_buffers{};
    std::uint64_t fallback_timestamp_buffers{};
    std::uint64_t invalid_time_metadata_buffers{};
    std::uint64_t usable_graph_clock_samples{};
    std::uint64_t unusable_graph_clock_samples{};
    std::uint64_t rate_window_resets{};
    std::uint64_t rate_window_samples{};
    std::uint64_t graph_elapsed_ns{};
    std::uint64_t monotonic_elapsed_ns{};
    // Parts per million, scaled by 1000 to retain fractional ppm.
    std::int64_t graph_vs_monotonic_ppm_milli{};
    bool graph_vs_monotonic_valid{};
};

// Fixed-size, single-writer telemetry for one PipeWire stream. The callback
// thread records graph-clock ticks/rate beside an independent CLOCK_MONOTONIC
// sample. The reported slope is only a PipeWire graph-clock vs host-clock
// observation; it does not identify a physical device oscillator's drift.
class PipeWireClockMetrics final {
public:
    static constexpr std::uint64_t minimum_rate_window_ns = 10'000'000'000ULL;

    void reset() noexcept {
        anchored_ = false;
        anchor_ticks_ = 0;
        last_ticks_ = 0;
        anchor_monotonic_ns_ = 0;
        last_monotonic_ns_ = 0;
        rate_num_ = 0;
        rate_denom_ = 0;
        window_samples_ = 0;
        native_timestamp_buffers_.store(0, std::memory_order_relaxed);
        fallback_timestamp_buffers_.store(0, std::memory_order_relaxed);
        invalid_time_metadata_buffers_.store(0, std::memory_order_relaxed);
        usable_graph_clock_samples_.store(0, std::memory_order_relaxed);
        unusable_graph_clock_samples_.store(0, std::memory_order_relaxed);
        rate_window_resets_.store(0, std::memory_order_relaxed);
        rate_window_samples_.store(0, std::memory_order_relaxed);
        graph_elapsed_ns_.store(0, std::memory_order_relaxed);
        monotonic_elapsed_ns_.store(0, std::memory_order_relaxed);
        graph_vs_monotonic_ppm_milli_.store(0, std::memory_order_relaxed);
        graph_vs_monotonic_valid_.store(false, std::memory_order_release);
    }

    void record_native_timestamp() noexcept {
        native_timestamp_buffers_.fetch_add(1, std::memory_order_relaxed);
    }

    // rate_num/rate_denom is PipeWire's seconds-per-tick fraction. This fit
    // is available only when the timestamp report also provides usable ticks.
    void record_graph_clock_sample(std::uint64_t ticks,
                                   std::uint32_t rate_num,
                                   std::uint32_t rate_denom,
                                   std::uint64_t callback_monotonic_ns) noexcept {
        if (ticks == 0 || rate_num == 0 || rate_denom == 0 ||
            callback_monotonic_ns == 0) {
            record_unusable_graph_clock_sample();
            return;
        }
        if (!anchored_) {
            usable_graph_clock_samples_.fetch_add(1, std::memory_order_relaxed);
            begin_window(ticks, rate_num, rate_denom, callback_monotonic_ns, false);
            return;
        }

        const bool same_rate =
            static_cast<std::uint64_t>(rate_num) * rate_denom_ ==
            static_cast<std::uint64_t>(rate_num_) * rate_denom;
        if (!same_rate || ticks < last_ticks_ ||
            callback_monotonic_ns <= last_monotonic_ns_) {
            usable_graph_clock_samples_.fetch_add(1, std::memory_order_relaxed);
            begin_window(ticks, rate_num, rate_denom, callback_monotonic_ns, true);
            return;
        }
        if (ticks == last_ticks_) {
            // PipeWire may report the same graph tick in adjacent callbacks.
            // Ignore that duplicate without moving the fit's last observation.
            return;
        }
        usable_graph_clock_samples_.fetch_add(1, std::memory_order_relaxed);

        ++window_samples_;
        const auto tick_delta = ticks - anchor_ticks_;
        const auto host_delta = callback_monotonic_ns - anchor_monotonic_ns_;
        const long double graph_delta_exact =
            static_cast<long double>(tick_delta) * rate_num * 1'000'000'000.0L /
            rate_denom;
        if (!(graph_delta_exact > 0.0L) ||
            graph_delta_exact > static_cast<long double>(
                                    std::numeric_limits<std::uint64_t>::max())) {
            begin_window(ticks, rate_num, rate_denom, callback_monotonic_ns, true);
            return;
        }
        const auto graph_delta = static_cast<std::uint64_t>(graph_delta_exact + 0.5L);
        last_ticks_ = ticks;
        last_monotonic_ns_ = callback_monotonic_ns;
        graph_elapsed_ns_.store(graph_delta, std::memory_order_relaxed);
        monotonic_elapsed_ns_.store(host_delta, std::memory_order_relaxed);
        rate_window_samples_.store(window_samples_, std::memory_order_relaxed);

        if (host_delta >= minimum_rate_window_ns) {
            const long double ppm_milli =
                (static_cast<long double>(graph_delta) - host_delta) *
                1'000'000'000.0L / static_cast<long double>(host_delta);
            const long double bounded = std::clamp(
                ppm_milli,
                static_cast<long double>(std::numeric_limits<std::int64_t>::min()),
                static_cast<long double>(std::numeric_limits<std::int64_t>::max()));
            graph_vs_monotonic_ppm_milli_.store(
                static_cast<std::int64_t>(std::round(bounded)),
                std::memory_order_relaxed);
            graph_vs_monotonic_valid_.store(true, std::memory_order_release);
        } else {
            graph_vs_monotonic_valid_.store(false, std::memory_order_release);
        }
    }

    void record_fallback(bool invalid_metadata) noexcept {
        fallback_timestamp_buffers_.fetch_add(1, std::memory_order_relaxed);
        if (invalid_metadata) {
            invalid_time_metadata_buffers_.fetch_add(1, std::memory_order_relaxed);
        }
        end_window_for_gap();
    }

    void record_unusable_graph_clock_sample() noexcept {
        unusable_graph_clock_samples_.fetch_add(1, std::memory_order_relaxed);
        end_window_for_gap();
    }

    void end_window_for_gap() noexcept {
        if (anchored_) {
            rate_window_resets_.fetch_add(1, std::memory_order_relaxed);
            anchored_ = false;
            window_samples_ = 0;
            rate_window_samples_.store(0, std::memory_order_relaxed);
            graph_elapsed_ns_.store(0, std::memory_order_relaxed);
            monotonic_elapsed_ns_.store(0, std::memory_order_relaxed);
            graph_vs_monotonic_ppm_milli_.store(0, std::memory_order_relaxed);
            graph_vs_monotonic_valid_.store(false, std::memory_order_release);
        }
    }

    [[nodiscard]] PipeWireClockMetricsSnapshot snapshot() const noexcept {
        PipeWireClockMetricsSnapshot result;
        result.native_timestamp_buffers =
            native_timestamp_buffers_.load(std::memory_order_relaxed);
        result.fallback_timestamp_buffers =
            fallback_timestamp_buffers_.load(std::memory_order_relaxed);
        result.invalid_time_metadata_buffers =
            invalid_time_metadata_buffers_.load(std::memory_order_relaxed);
        result.usable_graph_clock_samples =
            usable_graph_clock_samples_.load(std::memory_order_relaxed);
        result.unusable_graph_clock_samples =
            unusable_graph_clock_samples_.load(std::memory_order_relaxed);
        result.rate_window_resets = rate_window_resets_.load(std::memory_order_relaxed);
        result.rate_window_samples = rate_window_samples_.load(std::memory_order_relaxed);
        result.graph_elapsed_ns = graph_elapsed_ns_.load(std::memory_order_relaxed);
        result.monotonic_elapsed_ns = monotonic_elapsed_ns_.load(std::memory_order_relaxed);
        result.graph_vs_monotonic_ppm_milli =
            graph_vs_monotonic_ppm_milli_.load(std::memory_order_relaxed);
        result.graph_vs_monotonic_valid =
            graph_vs_monotonic_valid_.load(std::memory_order_acquire);
        return result;
    }

private:
    void begin_window(std::uint64_t ticks, std::uint32_t rate_num,
                      std::uint32_t rate_denom,
                      std::uint64_t callback_monotonic_ns,
                      bool count_reset) noexcept {
        if (count_reset) {
            rate_window_resets_.fetch_add(1, std::memory_order_relaxed);
        }
        anchored_ = true;
        anchor_ticks_ = ticks;
        last_ticks_ = ticks;
        anchor_monotonic_ns_ = callback_monotonic_ns;
        last_monotonic_ns_ = callback_monotonic_ns;
        rate_num_ = rate_num;
        rate_denom_ = rate_denom;
        window_samples_ = 1;
        rate_window_samples_.store(1, std::memory_order_relaxed);
        graph_elapsed_ns_.store(0, std::memory_order_relaxed);
        monotonic_elapsed_ns_.store(0, std::memory_order_relaxed);
        graph_vs_monotonic_ppm_milli_.store(0, std::memory_order_relaxed);
        graph_vs_monotonic_valid_.store(false, std::memory_order_release);
    }

    bool anchored_{};
    std::uint64_t anchor_ticks_{};
    std::uint64_t last_ticks_{};
    std::uint64_t anchor_monotonic_ns_{};
    std::uint64_t last_monotonic_ns_{};
    std::uint32_t rate_num_{};
    std::uint32_t rate_denom_{};
    std::uint64_t window_samples_{};

    std::atomic<std::uint64_t> native_timestamp_buffers_{0};
    std::atomic<std::uint64_t> fallback_timestamp_buffers_{0};
    std::atomic<std::uint64_t> invalid_time_metadata_buffers_{0};
    std::atomic<std::uint64_t> usable_graph_clock_samples_{0};
    std::atomic<std::uint64_t> unusable_graph_clock_samples_{0};
    std::atomic<std::uint64_t> rate_window_resets_{0};
    std::atomic<std::uint64_t> rate_window_samples_{0};
    std::atomic<std::uint64_t> graph_elapsed_ns_{0};
    std::atomic<std::uint64_t> monotonic_elapsed_ns_{0};
    std::atomic<std::int64_t> graph_vs_monotonic_ppm_milli_{0};
    std::atomic<bool> graph_vs_monotonic_valid_{false};
};

} // namespace fluxrec::audio
