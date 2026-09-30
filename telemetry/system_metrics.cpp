#include "telemetry/system_metrics.hpp"

#include <cmath>
#include <cstdint>
#include <dlfcn.h>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <unistd.h>

namespace fluxrec::telemetry {
namespace {

using NvmlReturn = int;
using NvmlDevice = void*;

struct NvmlUtilization {
    unsigned gpu;
    unsigned memory;
};

using NvmlInit = NvmlReturn (*)();
using NvmlShutdown = NvmlReturn (*)();
using NvmlGetDeviceByIndex = NvmlReturn (*)(unsigned, NvmlDevice*);
using NvmlGetUtilization = NvmlReturn (*)(NvmlDevice, NvmlUtilization*);
using NvmlGetEncoderUtilization = NvmlReturn (*)(NvmlDevice, unsigned*, unsigned*);

template<typename Function>
Function getSymbol(void* library, const char* name) noexcept {
    return reinterpret_cast<Function>(dlsym(library, name));
}

std::optional<std::uint64_t> processCpuTicks() noexcept {
    try {
        std::ifstream stat("/proc/self/stat");
        std::string line;
        if (!stat || !std::getline(stat, line)) {
            return std::nullopt;
        }

        // comm (field 2) can contain whitespace and ')' characters. The last
        // closing parenthesis is therefore the reliable start of field 3.
        const auto comm_end = line.rfind(')');
        if (comm_end == std::string::npos || comm_end + 1 >= line.size()) {
            return std::nullopt;
        }

        std::istringstream fields(line.substr(comm_end + 1));
        std::string field;
        if (!(fields >> field)) { // state, field 3
            return std::nullopt;
        }

        // Skip fields 4 through 13; utime and stime are fields 14 and 15.
        for (unsigned index = 0; index < 10; ++index) {
            if (!(fields >> field)) {
                return std::nullopt;
            }
        }

        std::uint64_t user_ticks{};
        std::uint64_t system_ticks{};
        if (!(fields >> user_ticks >> system_ticks)) {
            return std::nullopt;
        }
        return user_ticks + system_ticks;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<double> validPercent(unsigned value) noexcept {
    if (value > 100) {
        return std::nullopt;
    }
    return static_cast<double>(value);
}

} // namespace

struct SystemMetricsSampler::Impl {
    explicit Impl(std::optional<unsigned> requested_index) {
        if (!requested_index) {
            return;
        }

        library = dlopen("libnvidia-ml.so.1", RTLD_LAZY | RTLD_LOCAL);
        if (!library) {
            return;
        }

        init = getSymbol<NvmlInit>(library, "nvmlInit_v2");
        if (!init) {
            init = getSymbol<NvmlInit>(library, "nvmlInit");
        }
        shutdown = getSymbol<NvmlShutdown>(library, "nvmlShutdown");

        auto get_device = getSymbol<NvmlGetDeviceByIndex>(library,
                                                         "nvmlDeviceGetHandleByIndex_v2");
        if (!get_device) {
            get_device = getSymbol<NvmlGetDeviceByIndex>(library,
                                                        "nvmlDeviceGetHandleByIndex");
        }
        get_utilization = getSymbol<NvmlGetUtilization>(library,
                                                       "nvmlDeviceGetUtilizationRates");
        get_encoder_utilization = getSymbol<NvmlGetEncoderUtilization>(
            library, "nvmlDeviceGetEncoderUtilization");

        if (!init || !shutdown || !get_device || init() != nvml_success) {
            closeLibrary();
            return;
        }
        initialized = true;

        if (get_device(*requested_index, &device) != nvml_success) {
            closeLibrary();
            return;
        }
        available = true;
    }

    ~Impl() { closeLibrary(); }

    void closeLibrary() noexcept {
        if (initialized && shutdown) {
            (void)shutdown();
        }
        initialized = false;
        device = nullptr;
        available = false;
        if (library) {
            (void)dlclose(library);
            library = nullptr;
        }
    }

    SystemMetrics sample() noexcept {
        SystemMetrics result;
        const auto now = std::chrono::steady_clock::now();

        if (const auto ticks = processCpuTicks()) {
            const long ticks_per_second = sysconf(_SC_CLK_TCK);
            if (ticks_per_second > 0 && previous_process_ticks &&
                previous_process_time && *ticks >= *previous_process_ticks) {
                const double elapsed = std::chrono::duration<double>(
                    now - *previous_process_time).count();
                if (elapsed > 0.0) {
                    const auto delta = *ticks - *previous_process_ticks;
                    const double percent = static_cast<double>(delta) * 100.0 /
                        (static_cast<double>(ticks_per_second) * elapsed);
                    if (std::isfinite(percent) && percent >= 0.0) {
                        result.process_cpu_percent = percent;
                    }
                }
            }
            previous_process_ticks = *ticks;
            previous_process_time = now;
        } else {
            previous_process_ticks.reset();
            previous_process_time.reset();
        }

        if (!available || !device) {
            return result;
        }

        if (get_utilization) {
            NvmlUtilization utilization{};
            if (get_utilization(device, &utilization) == nvml_success) {
                result.gpu_utilization_percent = validPercent(utilization.gpu);
                result.gpu_memory_utilization_percent = validPercent(utilization.memory);
            }
        }

        if (get_encoder_utilization) {
            unsigned utilization{};
            unsigned sample_period_us{};
            if (get_encoder_utilization(device, &utilization, &sample_period_us) == nvml_success) {
                result.encoder_utilization_percent = validPercent(utilization);
                result.encoder_sample_period_us = sample_period_us;
            }
        }
        return result;
    }

    static constexpr NvmlReturn nvml_success = 0;
    void* library{};
    NvmlInit init{};
    NvmlShutdown shutdown{};
    NvmlGetUtilization get_utilization{};
    NvmlGetEncoderUtilization get_encoder_utilization{};
    NvmlDevice device{};
    bool initialized{};
    bool available{};
    std::mutex mutex;
    std::optional<std::uint64_t> previous_process_ticks;
    std::optional<std::chrono::steady_clock::time_point> previous_process_time;
};

SystemMetricsSampler::SystemMetricsSampler(std::optional<unsigned> gpu_index)
    : impl_(std::make_unique<Impl>(gpu_index)) {}

SystemMetricsSampler::~SystemMetricsSampler() = default;

SystemMetrics SystemMetricsSampler::sample() noexcept {
    if (!impl_) {
        return {};
    }
    try {
        const std::lock_guard lock(impl_->mutex);
        return impl_->sample();
    } catch (...) {
        return {};
    }
}

bool SystemMetricsSampler::nvmlAvailable() const noexcept {
    return impl_ && impl_->available;
}

} // namespace fluxrec::telemetry
