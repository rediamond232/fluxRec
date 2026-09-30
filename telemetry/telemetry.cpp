#include "telemetry/telemetry.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <locale>
#include <limits>
#include <numeric>
#include <stdexcept>

namespace fluxrec::telemetry {
namespace {

enum CounterIndex : std::size_t {
    captured_index,
    compositor_index,
    unique_index,
    submitted_index,
    encoded_index,
    muxed_index,
    duplicated_index,
    identity_checked_index,
    identity_skipped_index,
    identity_unclassified_index,
};

enum CaptureCounterIndex : std::size_t {
    capture_wayland_completed_index,
    capture_admitted_index,
    capture_rate_limited_index,
};

constexpr std::size_t index(DropStage stage) noexcept {
    return static_cast<std::size_t>(stage);
}

constexpr std::size_t index(Queue queue) noexcept {
    return static_cast<std::size_t>(queue);
}

constexpr std::size_t index(Timing timing) noexcept {
    return static_cast<std::size_t>(timing);
}

const char* name(DropStage stage) noexcept {
    switch (stage) {
    case DropStage::capture: return "capture";
    case DropStage::conversion: return "conversion";
    case DropStage::encoder: return "encoder";
    case DropStage::mux: return "mux";
    case DropStage::replay: return "replay";
    case DropStage::count: break;
    }
    return "unknown";
}

const char* name(Queue queue) noexcept {
    switch (queue) {
    case Queue::gpu_frame_pool: return "gpu_frame_pool";
    case Queue::capture: return "capture";
    case Queue::encode: return "encode";
    case Queue::packet: return "packet";
    case Queue::mux: return "mux";
    case Queue::count: break;
    }
    return "unknown";
}

const char* name(Timing timing) noexcept {
    switch (timing) {
    case Timing::frame_time: return "frame_time";
    case Timing::capture: return "capture";
    case Timing::capture_ready_to_submit_wall: return "capture_ready_to_submit_wall";
    case Timing::gpu_conversion: return "gpu_conversion";
    case Timing::nvenc_submission: return "nvenc_submission";
    case Timing::nvenc_completion: return "nvenc_completion";
    case Timing::nvenc_encode_picture_api_wall: return "nvenc_encode_picture_api_wall";
    case Timing::nvenc_lock_bitstream_api_wall: return "nvenc_lock_bitstream_api_wall";
    case Timing::nvenc_submit_to_bitstream_wall: return "nvenc_submit_to_bitstream_wall";
    case Timing::dmabuf_import_cpu: return "dmabuf_import_cpu";
    case Timing::cuda_transfer_cpu: return "cuda_transfer_cpu";
    case Timing::filter_submit_cpu: return "filter_submit_cpu";
    case Timing::filter_pull_cpu: return "filter_pull_cpu";
    case Timing::encoder_send_cpu: return "encoder_send_cpu";
    case Timing::encoder_receive_cpu: return "encoder_receive_cpu";
    case Timing::submit_to_packet_wall: return "submit_to_packet_wall";
    case Timing::frame_identity_gpu: return "frame_identity_gpu";
    case Timing::mux_queue_wait: return "mux_queue_wait";
    case Timing::mux: return "mux";
    case Timing::disk_write: return "disk_write";
    case Timing::count: break;
    }
    return "unknown";
}

double quantile(const std::vector<std::uint64_t>& sorted_samples, double q) {
    if (sorted_samples.size() == 1) {
        return static_cast<double>(sorted_samples.front());
    }
    const double position = q * static_cast<double>(sorted_samples.size() - 1);
    const auto lower = static_cast<std::size_t>(std::floor(position));
    const auto upper = static_cast<std::size_t>(std::ceil(position));
    const double fraction = position - static_cast<double>(lower);
    const double low_value = static_cast<double>(sorted_samples[lower]);
    const double high_value = static_cast<double>(sorted_samples[upper]);
    return low_value + (high_value - low_value) * fraction;
}

void writeOptional(std::ostringstream& out, const std::optional<double>& value) {
    if (value.has_value()) {
        out << *value;
    } else {
        out << "null";
    }
}

void writeOptional(std::ostringstream& out, const std::optional<std::uint64_t>& value) {
    if (value.has_value()) {
        out << *value;
    } else {
        out << "null";
    }
}

void writeOptional(std::ostringstream& out, const std::optional<unsigned>& value) {
    if (value.has_value()) {
        out << *value;
    } else {
        out << "null";
    }
}

void writeOptional(std::ostringstream& out, const std::optional<bool>& value) {
    if (value.has_value()) {
        out << (*value ? "true" : "false");
    } else {
        out << "null";
    }
}

std::optional<double> rate(std::uint64_t previous,
                           std::uint64_t current,
                           double elapsed_seconds) noexcept {
    if (elapsed_seconds <= 0.0 || current < previous) {
        return std::nullopt;
    }
    return static_cast<double>(current - previous) / elapsed_seconds;
}

std::optional<double> rate(const std::optional<std::uint64_t>& previous,
                           const std::optional<std::uint64_t>& current,
                           double elapsed_seconds) noexcept {
    if (!previous || !current) {
        return std::nullopt;
    }
    return rate(*previous, *current, elapsed_seconds);
}

} // namespace

FrameRates calculateFrameRates(const Snapshot& previous, const Snapshot& current) noexcept {
    const double elapsed_seconds = std::chrono::duration<double>(
        current.monotonic_timestamp - previous.monotonic_timestamp).count();
    return {
        rate(previous.frames.captured, current.frames.captured, elapsed_seconds),
        rate(previous.frames.compositor, current.frames.compositor, elapsed_seconds),
        rate(previous.frames.unique, current.frames.unique, elapsed_seconds),
        rate(previous.frames.submitted, current.frames.submitted, elapsed_seconds),
        rate(previous.frames.encoded, current.frames.encoded, elapsed_seconds),
        rate(previous.frames.muxed, current.frames.muxed, elapsed_seconds),
    };
}

Telemetry::Telemetry(std::size_t timing_window_capacity)
    : timings_{TimingState{timing_window_capacity},
               TimingState{timing_window_capacity},
               TimingState{timing_window_capacity},
               TimingState{timing_window_capacity},
               TimingState{timing_window_capacity},
               TimingState{timing_window_capacity},
               TimingState{timing_window_capacity},
               TimingState{timing_window_capacity},
               TimingState{timing_window_capacity},
               TimingState{timing_window_capacity},
               TimingState{timing_window_capacity},
               TimingState{timing_window_capacity},
               TimingState{timing_window_capacity},
               TimingState{timing_window_capacity},
               TimingState{timing_window_capacity},
               TimingState{timing_window_capacity},
               TimingState{timing_window_capacity},
               TimingState{timing_window_capacity},
               TimingState{timing_window_capacity},
               TimingState{timing_window_capacity}},
      timing_window_capacity_(timing_window_capacity) {
    if (timing_window_capacity == 0) {
        throw std::invalid_argument("telemetry timing window capacity must be greater than zero");
    }
    for (auto& counter : counters_) {
        counter.store(0, std::memory_order_relaxed);
    }
    for (auto& observed : counter_observed_) {
        observed.store(false, std::memory_order_relaxed);
    }
    for (auto& drops : drops_) {
        drops.store(0, std::memory_order_relaxed);
    }
}

void Telemetry::addCaptured(std::uint64_t count) noexcept {
    counters_[captured_index].fetch_add(count, std::memory_order_relaxed);
    counter_observed_[captured_index].store(true, std::memory_order_relaxed);
}

void Telemetry::addCaptureWaylandCompleted(std::uint64_t count) noexcept {
    capture_counters_[capture_wayland_completed_index].fetch_add(count,
                                                                 std::memory_order_relaxed);
    capture_counter_observed_[capture_wayland_completed_index].store(
        true, std::memory_order_relaxed);
}

void Telemetry::addCaptureAdmitted(std::uint64_t count) noexcept {
    capture_counters_[capture_admitted_index].fetch_add(count, std::memory_order_relaxed);
    capture_counter_observed_[capture_admitted_index].store(true, std::memory_order_relaxed);
}

void Telemetry::addCaptureRateLimited(std::uint64_t count) noexcept {
    capture_counters_[capture_rate_limited_index].fetch_add(count, std::memory_order_relaxed);
    capture_counter_observed_[capture_rate_limited_index].store(true, std::memory_order_relaxed);
}

void Telemetry::addCompositorFrames(std::uint64_t count) noexcept {
    counters_[compositor_index].fetch_add(count, std::memory_order_relaxed);
    counter_observed_[compositor_index].store(true, std::memory_order_relaxed);
}

void Telemetry::addUniqueFrames(std::uint64_t count) noexcept {
    counters_[unique_index].fetch_add(count, std::memory_order_relaxed);
    counter_observed_[unique_index].store(true, std::memory_order_relaxed);
}

void Telemetry::addSubmittedFrames(std::uint64_t count) noexcept {
    counters_[submitted_index].fetch_add(count, std::memory_order_relaxed);
    counter_observed_[submitted_index].store(true, std::memory_order_relaxed);
}

void Telemetry::addEncodedFrames(std::uint64_t count) noexcept {
    counters_[encoded_index].fetch_add(count, std::memory_order_relaxed);
    counter_observed_[encoded_index].store(true, std::memory_order_relaxed);
}

void Telemetry::addMuxedFrames(std::uint64_t count) noexcept {
    counters_[muxed_index].fetch_add(count, std::memory_order_relaxed);
    counter_observed_[muxed_index].store(true, std::memory_order_relaxed);
}

void Telemetry::addDuplicatedFrames(std::uint64_t count) noexcept {
    counters_[duplicated_index].fetch_add(count, std::memory_order_relaxed);
    counter_observed_[duplicated_index].store(true, std::memory_order_relaxed);
}

void Telemetry::addIdentityCheckedFrames(std::uint64_t count) noexcept {
    counters_[identity_checked_index].fetch_add(count, std::memory_order_relaxed);
    counter_observed_[identity_checked_index].store(true, std::memory_order_relaxed);
}

void Telemetry::addIdentitySkippedFrames(std::uint64_t count) noexcept {
    counters_[identity_skipped_index].fetch_add(count, std::memory_order_relaxed);
    counter_observed_[identity_skipped_index].store(true, std::memory_order_relaxed);
}

void Telemetry::addIdentityUnclassifiedFrames(std::uint64_t count) noexcept {
    counters_[identity_unclassified_index].fetch_add(count, std::memory_order_relaxed);
    counter_observed_[identity_unclassified_index].store(true, std::memory_order_relaxed);
}

void Telemetry::addDroppedFrames(DropStage stage, std::uint64_t count) noexcept {
    const auto stage_index = index(stage);
    if (stage_index >= drops_.size()) {
        return;
    }
    drops_[stage_index].fetch_add(count, std::memory_order_relaxed);
}

void Telemetry::updateHighWater(QueueState& state, std::uint64_t occupancy) noexcept {
    auto high_water = state.high_water.load(std::memory_order_relaxed);
    while (occupancy > high_water &&
           !state.high_water.compare_exchange_weak(high_water, occupancy,
                                                   std::memory_order_relaxed,
                                                   std::memory_order_relaxed)) {
    }
}

void Telemetry::observeQueue(Queue queue, std::uint64_t occupancy) noexcept {
    const auto queue_index = index(queue);
    if (queue_index >= queues_.size()) {
        return;
    }
    auto& state = queues_[queue_index];
    state.occupancy.store(occupancy, std::memory_order_relaxed);
    state.occupancy_observed.store(true, std::memory_order_relaxed);
    updateHighWater(state, occupancy);
}

void Telemetry::addQueueEntries(Queue queue, std::uint64_t count) noexcept {
    const auto queue_index = index(queue);
    if (queue_index >= queues_.size()) {
        return;
    }
    auto& state = queues_[queue_index];
    const auto previous = state.occupancy.fetch_add(count, std::memory_order_relaxed);
    state.occupancy_observed.store(true, std::memory_order_relaxed);
    updateHighWater(state, previous + count);
}

void Telemetry::removeQueueEntries(Queue queue, std::uint64_t count) noexcept {
    const auto queue_index = index(queue);
    if (queue_index >= queues_.size()) {
        return;
    }
    auto& occupancy = queues_[queue_index].occupancy;
    auto current = occupancy.load(std::memory_order_relaxed);
    while (!occupancy.compare_exchange_weak(current, current >= count ? current - count : 0,
                                            std::memory_order_relaxed,
                                            std::memory_order_relaxed)) {
    }
    queues_[queue_index].occupancy_observed.store(true, std::memory_order_relaxed);
}

void Telemetry::setQueueCapacity(Queue queue, std::uint64_t capacity) noexcept {
    const auto queue_index = index(queue);
    if (queue_index < queues_.size()) {
        queues_[queue_index].capacity.store(capacity, std::memory_order_relaxed);
        queues_[queue_index].capacity_observed.store(true, std::memory_order_relaxed);
    }
}

void Telemetry::observeOutputStats(std::uint64_t file_bytes,
                                   std::uint64_t encoded_payload_bytes) noexcept {
    output_file_bytes_.store(file_bytes, std::memory_order_relaxed);
    encoded_payload_bytes_.store(encoded_payload_bytes, std::memory_order_relaxed);
    output_stats_observed_.store(true, std::memory_order_relaxed);
}

void Telemetry::observeReplayStats(std::uint64_t replay_payload_bytes,
                                   std::uint64_t snapshot_payload_bytes,
                                   std::uint64_t payload_budget_bytes) noexcept {
    replay_payload_bytes_.store(replay_payload_bytes, std::memory_order_relaxed);
    replay_snapshot_payload_bytes_.store(snapshot_payload_bytes, std::memory_order_relaxed);
    replay_budget_bytes_.store(payload_budget_bytes, std::memory_order_relaxed);
    replay_stats_observed_.store(true, std::memory_order_relaxed);
}

void Telemetry::observeReplayMode(bool enabled, bool active,
                                  std::uint64_t packet_queue_drops,
                                  bool replay_only) noexcept {
    replay_enabled_.store(enabled, std::memory_order_relaxed);
    replay_active_.store(enabled && active, std::memory_order_relaxed);
    replay_packet_queue_drops_.store(packet_queue_drops, std::memory_order_relaxed);
    replay_only_.store(replay_only, std::memory_order_relaxed);
    replay_mode_observed_.store(true, std::memory_order_release);
}

void Telemetry::observeSystemMetrics(const SystemMetrics& metrics) noexcept {
    const std::lock_guard lock(system_metrics_mutex_);
    system_metrics_ = metrics;
}

void Telemetry::observe(Timing timing, std::chrono::nanoseconds duration) noexcept {
    const auto timing_index = index(timing);
    if (timing_index >= timings_.size() || duration.count() < 0) {
        return;
    }
    auto& state = timings_[timing_index];
    std::lock_guard lock(state.mutex);
    state.samples[state.next] = static_cast<std::uint64_t>(duration.count());
    state.next = (state.next + 1) % state.samples.size();
    if (state.size < state.samples.size()) {
        ++state.size;
    }
}

void Telemetry::observeNvencApiTiming(const NvencApiTimingSample& sample) noexcept {
    if (sample.pts == std::numeric_limits<std::int64_t>::min() ||
        sample.encode_picture_wall_ns > static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max()) ||
        sample.lock_bitstream_wall_ns > static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max()) ||
        (sample.submit_to_bitstream_wall_ns &&
         *sample.submit_to_bitstream_wall_ns > static_cast<std::uint64_t>(
             std::numeric_limits<std::int64_t>::max()))) {
        return;
    }

    observe(Timing::nvenc_encode_picture_api_wall,
            std::chrono::nanoseconds{static_cast<std::int64_t>(
                sample.encode_picture_wall_ns)});
    observe(Timing::nvenc_lock_bitstream_api_wall,
            std::chrono::nanoseconds{static_cast<std::int64_t>(
                sample.lock_bitstream_wall_ns)});
    if (sample.submit_to_bitstream_wall_ns) {
        observe(Timing::nvenc_submit_to_bitstream_wall,
                std::chrono::nanoseconds{static_cast<std::int64_t>(
                    *sample.submit_to_bitstream_wall_ns)});
    }

    const std::lock_guard lock(nvenc_api_timing_mutex_);
    nvenc_api_timing_samples_[nvenc_api_timing_next_] = sample;
    nvenc_api_timing_next_ = (nvenc_api_timing_next_ + 1) % nvenc_api_timing_samples_.size();
    if (nvenc_api_timing_size_ < nvenc_api_timing_samples_.size()) {
        ++nvenc_api_timing_size_;
    }
}

GpuConversionTimingBridge::GpuConversionTimingBridge(Telemetry& telemetry) noexcept
    : telemetry_(telemetry) {}

void GpuConversionTimingBridge::observePass(GpuConversionPass pass,
                                            std::int64_t frame_pts,
                                            std::uint64_t elapsed_ns) noexcept {
    // AV_NOPTS_VALUE cannot be used to correlate the two filter results, and
    // Telemetry stores signed nanosecond durations.
    if (frame_pts == std::numeric_limits<std::int64_t>::min() ||
        elapsed_ns > static_cast<std::uint64_t>(
                         std::numeric_limits<std::int64_t>::max()) ||
        (pass != GpuConversionPass::first &&
         pass != GpuConversionPass::second)) {
        return;
    }

    std::optional<std::int64_t> completed_frame_ns;
    {
        const std::lock_guard lock(pending_mutex_);
        PendingFrame* matching = nullptr;
        PendingFrame* available = nullptr;
        PendingFrame* oldest = &pending_.front();
        for (auto& pending : pending_) {
            if (pending.used && pending.pts == frame_pts) {
                matching = &pending;
                break;
            }
            if (!pending.used && !available) {
                available = &pending;
            }
            if (pending.used && (!oldest->used || pending.pts < oldest->pts)) {
                oldest = &pending;
            }
        }

        PendingFrame* sample = matching;
        if (!sample) {
            sample = available ? available : oldest;
            *sample = PendingFrame{.pts = frame_pts, .used = true};
        }

        if (pass == GpuConversionPass::first) {
            sample->first_ns = elapsed_ns;
            sample->has_first = true;
        } else {
            sample->second_ns = elapsed_ns;
            sample->has_second = true;
        }

        if (sample->has_first && sample->has_second &&
            sample->first_ns <= static_cast<std::uint64_t>(
                                    std::numeric_limits<std::int64_t>::max()) -
                                    sample->second_ns) {
            completed_frame_ns = static_cast<std::int64_t>(
                sample->first_ns + sample->second_ns);
            sample->used = false;
        } else if (sample->has_first && sample->has_second) {
            sample->used = false;
        }
    }

    if (completed_frame_ns) {
        telemetry_.observe(Timing::gpu_conversion,
                           std::chrono::nanoseconds{*completed_frame_ns});
    }
}

NvencApiTimingBridge::NvencApiTimingBridge(Telemetry& telemetry) noexcept
    : telemetry_(telemetry) {}

void NvencApiTimingBridge::observe(NvencApiCall call, std::int64_t frame_pts,
                                   std::uint64_t elapsed_ns,
                                   std::uint64_t event_monotonic_ns) noexcept {
    if (frame_pts == std::numeric_limits<std::int64_t>::min() ||
        elapsed_ns > static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max()) ||
        (call != NvencApiCall::encode_picture &&
         call != NvencApiCall::lock_bitstream)) {
        return;
    }

    std::optional<NvencApiTimingSample> completed;
    {
        const std::lock_guard lock(pending_mutex_);
        PendingFrame* matching = nullptr;
        PendingFrame* available = nullptr;
        PendingFrame* oldest = &pending_.front();
        for (auto& pending : pending_) {
            if (pending.used && pending.pts == frame_pts) {
                matching = &pending;
                break;
            }
            if (!pending.used && !available) {
                available = &pending;
            }
            if (pending.used && (!oldest->used || pending.pts < oldest->pts)) {
                oldest = &pending;
            }
        }

        PendingFrame* sample = matching;
        if (!sample) {
            sample = available ? available : oldest;
            *sample = PendingFrame{.pts = frame_pts, .used = true};
        }

        if (call == NvencApiCall::encode_picture) {
            sample->encode_picture_wall_ns = elapsed_ns;
            sample->encode_picture_return_monotonic_ns = event_monotonic_ns;
            sample->has_encode_picture = true;
        } else {
            sample->lock_bitstream_wall_ns = elapsed_ns;
            sample->lock_bitstream_return_monotonic_ns = event_monotonic_ns;
            sample->has_lock_bitstream = true;
        }

        if (sample->has_encode_picture && sample->has_lock_bitstream) {
            std::optional<std::uint64_t> submit_to_bitstream_wall_ns;
            if (sample->encode_picture_return_monotonic_ns != 0 &&
                sample->lock_bitstream_return_monotonic_ns >=
                    sample->encode_picture_return_monotonic_ns &&
                sample->lock_bitstream_return_monotonic_ns <= static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max())) {
                submit_to_bitstream_wall_ns =
                    sample->lock_bitstream_return_monotonic_ns -
                    sample->encode_picture_return_monotonic_ns;
            }
            completed = NvencApiTimingSample{
                sample->pts, sample->encode_picture_wall_ns,
                sample->lock_bitstream_wall_ns,
                submit_to_bitstream_wall_ns};
            sample->used = false;
        }
    }

    if (completed) telemetry_.observeNvencApiTiming(*completed);
}

Snapshot Telemetry::snapshot() const {
    Snapshot result{};
    result.timestamp = std::chrono::system_clock::now();
    result.monotonic_timestamp = std::chrono::steady_clock::now();
    result.frames.captured = counters_[captured_index].load(std::memory_order_relaxed);
    if (capture_counter_observed_[capture_wayland_completed_index].load(
            std::memory_order_relaxed)) {
        result.capture.wayland_completed = capture_counters_[capture_wayland_completed_index].load(
            std::memory_order_relaxed);
    }
    if (capture_counter_observed_[capture_admitted_index].load(std::memory_order_relaxed)) {
        result.capture.admitted = capture_counters_[capture_admitted_index].load(
            std::memory_order_relaxed);
    }
    if (capture_counter_observed_[capture_rate_limited_index].load(std::memory_order_relaxed)) {
        result.capture.rate_limited = capture_counters_[capture_rate_limited_index].load(
            std::memory_order_relaxed);
    }
    if (counter_observed_[compositor_index].load(std::memory_order_relaxed)) {
        result.frames.compositor = counters_[compositor_index].load(std::memory_order_relaxed);
    }
    if (counter_observed_[unique_index].load(std::memory_order_relaxed)) {
        result.frames.unique = counters_[unique_index].load(std::memory_order_relaxed);
    }
    result.frames.submitted = counters_[submitted_index].load(std::memory_order_relaxed);
    result.frames.encoded = counters_[encoded_index].load(std::memory_order_relaxed);
    if (counter_observed_[muxed_index].load(std::memory_order_relaxed)) {
        result.frames.muxed = counters_[muxed_index].load(std::memory_order_relaxed);
    }
    if (counter_observed_[duplicated_index].load(std::memory_order_relaxed)) {
        result.frames.duplicated = counters_[duplicated_index].load(std::memory_order_relaxed);
    }
    if (counter_observed_[identity_checked_index].load(std::memory_order_relaxed)) {
        result.frames.identity_checked = counters_[identity_checked_index].load(std::memory_order_relaxed);
    }
    if (counter_observed_[identity_skipped_index].load(std::memory_order_relaxed)) {
        result.frames.identity_skipped = counters_[identity_skipped_index].load(std::memory_order_relaxed);
    }
    if (counter_observed_[identity_unclassified_index].load(std::memory_order_relaxed)) {
        result.frames.identity_unclassified = counters_[identity_unclassified_index].load(std::memory_order_relaxed);
    }
    for (std::size_t i = 0; i < drops_.size(); ++i) {
        result.frames.dropped_by_stage[i] = drops_[i].load(std::memory_order_relaxed);
        result.frames.dropped += result.frames.dropped_by_stage[i];
    }

    for (std::size_t i = 0; i < queues_.size(); ++i) {
        auto& queue = result.queues[i];
        if (queues_[i].occupancy_observed.load(std::memory_order_relaxed)) {
            queue.occupancy = queues_[i].occupancy.load(std::memory_order_relaxed);
            queue.high_water = queues_[i].high_water.load(std::memory_order_relaxed);
        }
        if (queues_[i].capacity_observed.load(std::memory_order_relaxed)) {
            queue.capacity = queues_[i].capacity.load(std::memory_order_relaxed);
        }
    }

    if (output_stats_observed_.load(std::memory_order_relaxed)) {
        result.storage.output_file_bytes = output_file_bytes_.load(std::memory_order_relaxed);
        result.storage.encoded_payload_bytes = encoded_payload_bytes_.load(std::memory_order_relaxed);
    }
    if (replay_stats_observed_.load(std::memory_order_relaxed)) {
        result.storage.replay_payload_bytes = replay_payload_bytes_.load(std::memory_order_relaxed);
        result.storage.replay_snapshot_payload_bytes =
            replay_snapshot_payload_bytes_.load(std::memory_order_relaxed);
        result.storage.replay_budget_bytes = replay_budget_bytes_.load(std::memory_order_relaxed);
    }
    if (replay_mode_observed_.load(std::memory_order_acquire)) {
        result.replay.enabled = replay_enabled_.load(std::memory_order_relaxed);
        result.replay.active = replay_active_.load(std::memory_order_relaxed);
        result.replay_only = replay_only_.load(std::memory_order_relaxed);
        if (*result.replay.enabled) {
            result.replay.packet_queue_drops =
                replay_packet_queue_drops_.load(std::memory_order_relaxed);
        }
    }

    {
        const std::lock_guard lock(system_metrics_mutex_);
        result.system = system_metrics_;
    }

    {
        const std::lock_guard lock(nvenc_api_timing_mutex_);
        result.nvenc_api_timing_samples.reserve(nvenc_api_timing_size_);
        const std::size_t first = (nvenc_api_timing_next_ +
            nvenc_api_timing_samples_.size() - nvenc_api_timing_size_) %
            nvenc_api_timing_samples_.size();
        for (std::size_t i = 0; i < nvenc_api_timing_size_; ++i) {
            result.nvenc_api_timing_samples.push_back(
                nvenc_api_timing_samples_[(first + i) % nvenc_api_timing_samples_.size()]);
        }
    }

    for (std::size_t i = 0; i < timings_.size(); ++i) {
        const auto& state = timings_[i];
        std::vector<std::uint64_t> observations;
        {
            std::lock_guard lock(state.mutex);
            observations.assign(state.samples.begin(), state.samples.begin() + static_cast<std::ptrdiff_t>(state.size));
        }

        auto& distribution = result.timings[i];
        distribution.samples = observations.size();
        distribution.window_capacity = timing_window_capacity_;
        if (observations.empty()) {
            continue;
        }
        const long double total = std::accumulate(observations.begin(), observations.end(), 0.0L);
        distribution.average_ns = static_cast<double>(total / observations.size());
        std::sort(observations.begin(), observations.end());
        distribution.p50_ns = quantile(observations, 0.50);
        distribution.p95_ns = quantile(observations, 0.95);
        distribution.p99_ns = quantile(observations, 0.99);
        distribution.p99_9_ns = quantile(observations, 0.999);
        distribution.max_ns = static_cast<double>(observations.back());
    }
    return result;
}

std::string Telemetry::toJson() const {
    return toJson(snapshot());
}

std::string Telemetry::toJson(const Snapshot& snapshot) {
    const auto unix_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        snapshot.timestamp.time_since_epoch()).count();
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(12);
    out << "{\"timestamp_unix_ns\":" << unix_ns;
    out << ",\"frames\":{";
    out << "\"captured\":" << snapshot.frames.captured
        << ",\"compositor\":"; writeOptional(out, snapshot.frames.compositor);
    out << ",\"unique\":"; writeOptional(out, snapshot.frames.unique);
    out << ",\"submitted\":" << snapshot.frames.submitted
        << ",\"encoded\":" << snapshot.frames.encoded
        << ",\"muxed\":"; writeOptional(out, snapshot.frames.muxed);
    out << ",\"dropped\":" << snapshot.frames.dropped
        << ",\"dropped_by_stage\":{";
    for (std::size_t i = 0; i < static_cast<std::size_t>(DropStage::count); ++i) {
        if (i != 0) out << ',';
        out << '"' << name(static_cast<DropStage>(i)) << "\":"
            << snapshot.frames.dropped_by_stage[i];
    }
    out << "},\"duplicated\":"; writeOptional(out, snapshot.frames.duplicated);
    out << ",\"identity_checked\":"; writeOptional(out, snapshot.frames.identity_checked);
    out << ",\"identity_skipped\":"; writeOptional(out, snapshot.frames.identity_skipped);
    out << ",\"identity_unclassified\":"; writeOptional(out, snapshot.frames.identity_unclassified);
    out << '}';

    out << ",\"capture\":{\"wayland_completed\":";
    writeOptional(out, snapshot.capture.wayland_completed);
    out << ",\"admitted\":"; writeOptional(out, snapshot.capture.admitted);
    out << ",\"rate_limited\":"; writeOptional(out, snapshot.capture.rate_limited);
    out << '}';

    out << ",\"fps\":";
    if (!snapshot.frame_rates) {
        out << "null";
    } else {
        const auto& rates = *snapshot.frame_rates;
        out << "{\"captured\":"; writeOptional(out, rates.captured_fps);
        out << ",\"compositor\":"; writeOptional(out, rates.compositor_fps);
        out << ",\"unique\":"; writeOptional(out, rates.unique_fps);
        out << ",\"submitted\":"; writeOptional(out, rates.submitted_fps);
        out << ",\"encoded\":"; writeOptional(out, rates.encoded_fps);
        out << ",\"muxed\":"; writeOptional(out, rates.muxed_fps);
        out << '}';
    }

    out << ",\"recording_mode\":";
    if (!snapshot.replay_only) {
        out << "null";
    } else {
        out << '"' << (*snapshot.replay_only ? "replay_only" : "recording") << '"';
    }

    out << ",\"queues\":{";
    for (std::size_t i = 0; i < static_cast<std::size_t>(Queue::count); ++i) {
        if (i != 0) out << ',';
        const auto& queue = snapshot.queues[i];
        out << '"' << name(static_cast<Queue>(i)) << "\":{";
        out << "\"occupancy\":"; writeOptional(out, queue.occupancy);
        out << ",\"high_water\":"; writeOptional(out, queue.high_water);
        out << ",\"capacity\":"; writeOptional(out, queue.capacity);
        out << '}';
    }
    out << '}';

    out << ",\"storage\":{";
    out << "\"output_file_bytes\":"; writeOptional(out, snapshot.storage.output_file_bytes);
    out << ",\"output_file_bitrate_bps\":";
    writeOptional(out, snapshot.storage.output_file_bitrate_bps);
    out << ",\"encoded_payload_bytes\":";
    writeOptional(out, snapshot.storage.encoded_payload_bytes);
    out << ",\"replay_payload_bytes\":";
    writeOptional(out, snapshot.storage.replay_payload_bytes);
    out << ",\"replay_snapshot_payload_bytes\":";
    writeOptional(out, snapshot.storage.replay_snapshot_payload_bytes);
    out << ",\"replay_budget_bytes\":";
    writeOptional(out, snapshot.storage.replay_budget_bytes);
    out << '}';

    out << ",\"replay\":{";
    out << "\"enabled\":"; writeOptional(out, snapshot.replay.enabled);
    out << ",\"active\":"; writeOptional(out, snapshot.replay.active);
    out << ",\"packet_queue_drops\":";
    writeOptional(out, snapshot.replay.packet_queue_drops);
    out << '}';

    out << ",\"timings_ns\":{";
    for (std::size_t i = 0; i < static_cast<std::size_t>(Timing::count); ++i) {
        if (i != 0) out << ',';
        const auto& timing = snapshot.timings[i];
        out << '"' << name(static_cast<Timing>(i)) << "\":{";
        out << "\"samples\":" << timing.samples
            << ",\"window_capacity\":" << timing.window_capacity
            << ",\"average\":";
        writeOptional(out, timing.average_ns);
        out << ",\"p50\":"; writeOptional(out, timing.p50_ns);
        out << ",\"p95\":"; writeOptional(out, timing.p95_ns);
        out << ",\"p99\":"; writeOptional(out, timing.p99_ns);
        out << ",\"p99_9\":"; writeOptional(out, timing.p99_9_ns);
        out << ",\"max\":"; writeOptional(out, timing.max_ns);
        out << '}';
    }
    out << "},\"nvenc_api_timing_samples\":[";
    for (std::size_t i = 0; i < snapshot.nvenc_api_timing_samples.size(); ++i) {
        if (i != 0) out << ',';
        const auto& sample = snapshot.nvenc_api_timing_samples[i];
        out << "{\"pts\":" << sample.pts
            << ",\"encode_picture_wall_ns\":" << sample.encode_picture_wall_ns
            << ",\"lock_bitstream_wall_ns\":" << sample.lock_bitstream_wall_ns
            << ",\"submit_to_bitstream_wall_ns\":";
        writeOptional(out, sample.submit_to_bitstream_wall_ns);
        out
            << '}';
    }
    out << "],\"system\":";
    if (!snapshot.system) {
        out << "null";
    } else {
        const auto& system = *snapshot.system;
        out << "{\"process_cpu_percent\":";
        writeOptional(out, system.process_cpu_percent);
        out << ",\"gpu_utilization_percent\":";
        writeOptional(out, system.gpu_utilization_percent);
        out << ",\"gpu_memory_utilization_percent\":";
        writeOptional(out, system.gpu_memory_utilization_percent);
        out << ",\"encoder_utilization_percent\":";
        writeOptional(out, system.encoder_utilization_percent);
        out << ",\"encoder_sample_period_us\":";
        writeOptional(out, system.encoder_sample_period_us);
        out << '}';
    }
    out << '}';
    return out.str();
}

PeriodicJsonReporter::PeriodicJsonReporter(const Telemetry& telemetry,
                                           std::chrono::milliseconds interval,
                                           Sink sink) {
    if (interval <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("telemetry reporter interval must be positive");
    }
    if (!sink) {
        throw std::invalid_argument("telemetry reporter sink must be callable");
    }
    worker_ = std::jthread([this, &telemetry, interval, sink = std::move(sink)](std::stop_token stop) mutable {
        std::unique_lock lock(wait_mutex_);
        std::optional<Snapshot> previous;
        while (!stop.stop_requested()) {
            wake_.wait_for(lock, stop, interval, [] { return false; });
            if (stop.stop_requested()) {
                break;
            }
            lock.unlock();
            try {
                auto current = telemetry.snapshot();
                if (previous) {
                    current.frame_rates = calculateFrameRates(*previous, current);
                    const double elapsed_seconds = std::chrono::duration<double>(
                        current.monotonic_timestamp - previous->monotonic_timestamp).count();
                    const auto bytes_per_second = rate(
                        previous->storage.output_file_bytes,
                        current.storage.output_file_bytes,
                        elapsed_seconds);
                    if (bytes_per_second) {
                        current.storage.output_file_bitrate_bps = *bytes_per_second * 8.0;
                    }
                }
                previous = current;
                sink(Telemetry::toJson(current));
            } catch (...) {
                // A failing diagnostics sink must not terminate the process.
                // The next interval retries with a fresh snapshot.
            }
            lock.lock();
        }
    });
}

PeriodicJsonReporter::~PeriodicJsonReporter() {
    worker_.request_stop();
    wake_.notify_all();
}

} // namespace fluxrec::telemetry
