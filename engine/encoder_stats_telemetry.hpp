#pragma once

#include "gpu/gpu.hpp"
#include "telemetry/telemetry.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace fluxrec::engine {

// Feeds cumulative encoder counters to telemetry exactly once. The direct
// NVENC backend updates some fields from its output worker, so deltas between
// adjacent submit calls are not a reliable observation boundary.
class EncoderStatsTelemetryObserver final {
public:
    EncoderStatsTelemetryObserver(telemetry::Telemetry& telemetry,
                                  gpu::EncoderStats initial) noexcept
        : telemetry_(telemetry), previous_(initial) {}

    void observe(const gpu::EncoderStats& current) noexcept {
        telemetry_.addSubmittedFrames(delta(previous_.submitted_frames,
                                            current.submitted_frames));
        telemetry_.addEncodedFrames(delta(previous_.encoded_frames,
                                          current.encoded_frames));

        observe_total(telemetry::Timing::dmabuf_import_cpu,
                     previous_.total_dmabuf_import_cpu_ns,
                     current.total_dmabuf_import_cpu_ns);
        observe_total(telemetry::Timing::cuda_transfer_cpu,
                     previous_.total_cuda_transfer_cpu_ns,
                     current.total_cuda_transfer_cpu_ns);
        observe_total(telemetry::Timing::filter_submit_cpu,
                     previous_.total_filter_submit_cpu_ns,
                     current.total_filter_submit_cpu_ns);
        observe_total(telemetry::Timing::filter_pull_cpu,
                     previous_.total_filter_pull_cpu_ns,
                     current.total_filter_pull_cpu_ns);
        observe_total(telemetry::Timing::encoder_send_cpu,
                     previous_.total_encoder_send_cpu_ns,
                     current.total_encoder_send_cpu_ns);
        observe_total(telemetry::Timing::encoder_receive_cpu,
                     previous_.total_encoder_receive_cpu_ns,
                     current.total_encoder_receive_cpu_ns);

        const std::uint64_t packet_samples = delta(previous_.submit_to_packet_samples,
                                                   current.submit_to_packet_samples);
        if (packet_samples != 0) {
            const std::uint64_t latency_total = delta(
                previous_.total_submit_to_packet_wall_ns,
                current.total_submit_to_packet_wall_ns);
            const std::uint64_t per_packet = latency_total / packet_samples;
            const auto capacity = static_cast<std::uint64_t>(
                telemetry_.timingWindowCapacity());
            // Stats expose a cumulative latency total and sample count, not each
            // individual async latency. Repeat the interval mean so the timing
            // sample count still reflects every completed packet.
            const std::uint64_t repetitions = std::min(packet_samples, capacity);
            for (std::uint64_t i = 0; i < repetitions; ++i) {
                telemetry_.observe(telemetry::Timing::submit_to_packet_wall,
                                   as_duration(per_packet));
            }
        }

        // Advancing even after a counter reset prevents a later increase from
        // replaying values that were already observed before the reset.
        previous_ = current;
    }

private:
    static constexpr std::uint64_t delta(std::uint64_t previous,
                                         std::uint64_t current) noexcept {
        return telemetry::monotonicCounterDelta(previous, current);
    }

    static constexpr std::chrono::nanoseconds as_duration(std::uint64_t ns) noexcept {
        constexpr auto max = static_cast<std::uint64_t>(
            std::numeric_limits<std::chrono::nanoseconds::rep>::max());
        return std::chrono::nanoseconds{static_cast<std::chrono::nanoseconds::rep>(
            std::min(ns, max))};
    }

    void observe_total(telemetry::Timing timing, std::uint64_t previous,
                       std::uint64_t current) noexcept {
        const std::uint64_t elapsed = delta(previous, current);
        if (elapsed != 0) telemetry_.observe(timing, as_duration(elapsed));
    }

    telemetry::Telemetry& telemetry_;
    gpu::EncoderStats previous_;
};

} // namespace fluxrec::engine
