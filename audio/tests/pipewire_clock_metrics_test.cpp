#include "audio/pipewire_clock_metrics.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

void require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "pipewire clock metrics test failed: " << message << '\n';
        std::exit(1);
    }
}

void observe(fluxrec::audio::PipeWireClockMetrics& metrics,
             std::uint64_t ticks, std::uint64_t host_ns) {
    metrics.record_native_timestamp();
    metrics.record_graph_clock_sample(ticks, 1, 48'000, host_ns);
}

void check_rate_fit_and_validity_window() {
    fluxrec::audio::PipeWireClockMetrics metrics;
    constexpr std::uint64_t host_start = 50'000'000'000ULL;
    constexpr std::uint64_t ticks_start = 4'800'000ULL;
    observe(metrics, ticks_start, host_start);

    for (std::uint64_t second = 1; second <= 9; ++second) {
        observe(metrics, ticks_start + second * 48'006ULL,
                host_start + second * 1'000'000'000ULL);
    }
    auto snapshot = metrics.snapshot();
    require(!snapshot.graph_vs_monotonic_valid,
            "rate fit became valid before its ten-second minimum window");
    require(snapshot.native_timestamp_buffers == 10,
            "native timestamp buffer count did not include every accepted stamp");

    observe(metrics, ticks_start + 10 * 48'006ULL,
            host_start + 10'000'000'000ULL);
    snapshot = metrics.snapshot();
    require(snapshot.graph_vs_monotonic_valid,
            "rate fit did not become valid at ten seconds");
    require(snapshot.rate_window_samples == 11,
            "fit window sample count is incorrect");
    require(snapshot.graph_elapsed_ns == 10'001'250'000ULL &&
                snapshot.monotonic_elapsed_ns == 10'000'000'000ULL,
            "reported elapsed times do not reflect ticks/rate and host samples");
    require(snapshot.graph_vs_monotonic_ppm_milli == 125'000,
            "graph-vs-monotonic slope was not 125 ppm");
}

void check_pipewire_timestamp_requires_nonzero_rate() {
    require(fluxrec::audio::pipewire_time_can_stamp(1, 1, 48'000),
            "valid PipeWire timestamp metadata was rejected");
    require(!fluxrec::audio::pipewire_time_can_stamp(1, 0, 48'000),
            "zero rate numerator was accepted for a native timestamp");
    require(!fluxrec::audio::pipewire_time_can_stamp(1, 1, 0),
            "zero rate denominator was accepted for a native timestamp");
    require(!fluxrec::audio::pipewire_time_can_stamp(0, 1, 48'000),
            "zero PipeWire nanosecond time was accepted");
}

void check_rate_change_restarts_window() {
    fluxrec::audio::PipeWireClockMetrics metrics;
    constexpr std::uint64_t host_start = 100'000'000'000ULL;
    observe(metrics, 48'000, host_start);
    for (std::uint64_t second = 1; second <= 10; ++second) {
        observe(metrics, 48'000 + second * 48'000,
                host_start + second * 1'000'000'000ULL);
    }
    require(metrics.snapshot().graph_vs_monotonic_valid,
            "precondition: stable clock window was not measured");

    metrics.record_native_timestamp();
    metrics.record_graph_clock_sample(48'000 + 10 * 48'000 + 96'000,
                                      1, 96'000,
                                      host_start + 11'000'000'000ULL);
    auto snapshot = metrics.snapshot();
    require(!snapshot.graph_vs_monotonic_valid,
            "rate change retained a fit from the previous rate segment");
    require(snapshot.rate_window_resets == 1 && snapshot.rate_window_samples == 1,
            "rate change did not begin a fresh one-sample window");

    for (std::uint64_t second = 2; second <= 11; ++second) {
        metrics.record_native_timestamp();
        metrics.record_graph_clock_sample(
            48'000 + 10 * 48'000 + second * 96'000,
            1, 96'000, host_start + (10 + second) * 1'000'000'000ULL);
    }
    snapshot = metrics.snapshot();
    require(snapshot.graph_vs_monotonic_valid &&
                snapshot.graph_vs_monotonic_ppm_milli == 0,
            "stable post-change rate did not produce an independent zero-slope fit");
}

void check_tick_regression_against_last_sample() {
    fluxrec::audio::PipeWireClockMetrics metrics;
    constexpr std::uint64_t host_start = 200'000'000'000ULL;
    observe(metrics, 1'000, host_start);
    observe(metrics, 481'000, host_start + 10'000'000'000ULL);
    require(metrics.snapshot().graph_vs_monotonic_valid,
            "precondition: ten-second fit was not valid");

    // This regresses from the immediately previous tick but remains above the
    // window anchor. The estimator must detect the discontinuity.
    observe(metrics, 480'500, host_start + 11'000'000'000ULL);
    const auto snapshot = metrics.snapshot();
    require(!snapshot.graph_vs_monotonic_valid,
            "mid-window tick regression was not rejected");
    require(snapshot.rate_window_resets == 1 && snapshot.rate_window_samples == 1,
            "tick regression did not reset the rate window");
}

void check_duplicate_tick_is_ignored() {
    fluxrec::audio::PipeWireClockMetrics metrics;
    constexpr std::uint64_t host_start = 250'000'000'000ULL;
    observe(metrics, 1'000, host_start);

    metrics.record_native_timestamp();
    metrics.record_graph_clock_sample(1'000, 1, 48'000, host_start + 1);
    auto snapshot = metrics.snapshot();
    require(snapshot.rate_window_resets == 0 &&
                snapshot.rate_window_samples == 1 &&
                snapshot.usable_graph_clock_samples == 1,
            "duplicate tick was counted or reset the active fit");

    for (std::uint64_t second = 1; second <= 10; ++second) {
        observe(metrics, 1'000 + second * 48'000,
                host_start + second * 1'000'000'000ULL);
    }
    snapshot = metrics.snapshot();
    require(snapshot.graph_vs_monotonic_valid &&
                snapshot.graph_vs_monotonic_ppm_milli == 0,
            "fit did not recover after ignoring a duplicate tick");
    require(snapshot.rate_window_resets == 0 &&
                snapshot.rate_window_samples == 11 &&
                snapshot.usable_graph_clock_samples == 11,
            "duplicate tick affected the fitted sample count");
}

void check_unusable_samples_break_fit_and_counts_are_distinct() {
    fluxrec::audio::PipeWireClockMetrics metrics;
    constexpr std::uint64_t host_start = 300'000'000'000ULL;
    observe(metrics, 1'000, host_start);
    observe(metrics, 481'000, host_start + 10'000'000'000ULL);
    require(metrics.snapshot().graph_vs_monotonic_valid,
            "precondition: ten-second fit was not valid");

    metrics.record_fallback(true);
    auto snapshot = metrics.snapshot();
    require(!snapshot.graph_vs_monotonic_valid && snapshot.rate_window_samples == 0,
            "a fallback timestamp did not break the graph-clock fit window");
    metrics.record_native_timestamp();
    metrics.record_unusable_graph_clock_sample();
    snapshot = metrics.snapshot();
    require(!snapshot.graph_vs_monotonic_valid && snapshot.rate_window_samples == 0,
            "unusable graph timing did not clear the active fit");
    require(snapshot.rate_window_resets == 1 &&
                snapshot.unusable_graph_clock_samples == 1,
            "unusable graph timing count/reset count is incorrect");
    require(snapshot.native_timestamp_buffers == 3 &&
                snapshot.fallback_timestamp_buffers == 1 &&
                snapshot.invalid_time_metadata_buffers == 1,
            "native, fallback, and malformed metadata counts were conflated");
}

} // namespace

int main() {
    check_pipewire_timestamp_requires_nonzero_rate();
    check_rate_fit_and_validity_window();
    check_rate_change_restarts_window();
    check_tick_regression_against_last_sample();
    check_duplicate_tick_is_ignored();
    check_unusable_samples_break_fit_and_counts_are_distinct();
    std::cout << "PipeWire clock metric rate/reset checks passed\n";
    return 0;
}
