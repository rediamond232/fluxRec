#pragma once

#include <chrono>
#include <memory>
#include <optional>

namespace fluxrec::telemetry {

// Optional operating-system and NVIDIA telemetry. Each utilization value is
// absent when its source is unavailable or did not return a valid sample.
struct SystemMetrics {
    // Process CPU usage is normalized to one logical CPU, so a multithreaded
    // process can report values greater than 100 percent.
    std::optional<double> process_cpu_percent;
    std::optional<double> gpu_utilization_percent;
    std::optional<double> gpu_memory_utilization_percent;
    std::optional<double> encoder_utilization_percent;
    std::optional<unsigned> encoder_sample_period_us;
};

// Samples /proc and NVML on demand. Call sample() from a low-rate stats thread,
// not from a capture, conversion, or encode callback. Concurrent calls are
// serialized; the sampler never reads or maps video buffers.
//
// NVML is loaded at runtime, so NVIDIA's management library is optional and is
// not a build or link dependency. gpu_index selects NVML's device ordinal.
class SystemMetricsSampler final {
public:
    explicit SystemMetricsSampler(std::optional<unsigned> gpu_index = 0);
    ~SystemMetricsSampler();

    SystemMetricsSampler(const SystemMetricsSampler&) = delete;
    SystemMetricsSampler& operator=(const SystemMetricsSampler&) = delete;
    SystemMetricsSampler(SystemMetricsSampler&&) = delete;
    SystemMetricsSampler& operator=(SystemMetricsSampler&&) = delete;

    [[nodiscard]] SystemMetrics sample() noexcept;
    [[nodiscard]] bool nvmlAvailable() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace fluxrec::telemetry
