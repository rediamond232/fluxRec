#include "telemetry/telemetry.hpp"

#include <array>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;
using fluxrec::telemetry::DropStage;
using fluxrec::telemetry::Queue;
using fluxrec::telemetry::NvencApiCall;
using fluxrec::telemetry::NvencApiTimingBridge;
using fluxrec::telemetry::Telemetry;
using fluxrec::telemetry::Timing;

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

bool close(double left, double right) {
    return std::abs(left - right) < 0.0001;
}

void countersAndQueues() {
    Telemetry telemetry(8);
    telemetry.addCaptured(10);
    telemetry.addCaptureWaylandCompleted(12);
    telemetry.addCaptureAdmitted(9);
    telemetry.addCaptureRateLimited(2);
    telemetry.addCompositorFrames(12);
    telemetry.addUniqueFrames(9);
    telemetry.addSubmittedFrames(8);
    telemetry.addEncodedFrames(7);
    telemetry.addMuxedFrames(6);
    telemetry.addDuplicatedFrames(2);
    telemetry.addIdentityCheckedFrames(13);
    telemetry.addIdentitySkippedFrames(1);
    telemetry.addIdentityUnclassifiedFrames(2);
    telemetry.addDroppedFrames(DropStage::capture, 1);
    telemetry.addDroppedFrames(DropStage::encoder, 2);
    telemetry.addDroppedFrames(DropStage::replay, 4);
    telemetry.setQueueCapacity(Queue::packet, 16);
    telemetry.observeQueue(Queue::packet, 3);
    telemetry.observeQueue(Queue::packet, 11);
    telemetry.observeQueue(Queue::packet, 4);

    const auto snapshot = telemetry.snapshot();
    require(snapshot.frames.captured == 10, "captured count");
    require(snapshot.capture.wayland_completed == 12, "Wayland capture completion count");
    require(snapshot.capture.admitted == 9, "capture admitted count");
    require(snapshot.capture.rate_limited == 2, "intentional capture rate limit count");
    require(snapshot.frames.compositor == 12, "compositor count");
    require(snapshot.frames.unique == 9, "unique count");
    require(snapshot.frames.submitted == 8, "submitted count");
    require(snapshot.frames.encoded == 7, "encoded count");
    require(snapshot.frames.muxed == 6, "muxed count");
    require(snapshot.frames.dropped == 7, "total dropped count");
    require(snapshot.frames.dropped_by_stage[static_cast<std::size_t>(DropStage::capture)] == 1,
            "capture dropped count");
    require(snapshot.frames.dropped_by_stage[static_cast<std::size_t>(DropStage::encoder)] == 2,
            "encoder dropped count");
    require(snapshot.frames.dropped_by_stage[static_cast<std::size_t>(DropStage::replay)] == 4,
            "replay dropped count");
    require(snapshot.frames.duplicated == 2, "duplicated count");
    require(snapshot.frames.identity_checked == 13, "identity checked count");
    require(snapshot.frames.identity_skipped == 1, "identity skipped count");
    require(snapshot.frames.identity_unclassified == 2, "identity unclassified count");
    const auto& packet_queue = snapshot.queues[static_cast<std::size_t>(Queue::packet)];
    require(packet_queue.occupancy == 4, "latest queue occupancy");
    require(packet_queue.high_water == 11, "queue high-water mark");
    require(packet_queue.capacity == 16, "queue capacity");
}

void rollingTimingDistribution() {
    Telemetry telemetry(4);
    telemetry.observe(Timing::frame_time, 10ns);
    telemetry.observe(Timing::frame_time, 20ns);
    telemetry.observe(Timing::frame_time, 30ns);
    telemetry.observe(Timing::frame_time, 40ns);
    telemetry.observe(Timing::frame_time, 50ns);

    const auto snapshot = telemetry.snapshot();
    const auto& frame_time = snapshot.timings[static_cast<std::size_t>(Timing::frame_time)];
    require(frame_time.samples == 4, "timing sample window is bounded");
    require(frame_time.window_capacity == 4, "timing window capacity");
    require(frame_time.average_ns && close(*frame_time.average_ns, 35.0), "rolling average");
    require(frame_time.p50_ns && close(*frame_time.p50_ns, 35.0), "interpolated p50");
    require(frame_time.p95_ns && close(*frame_time.p95_ns, 48.5), "interpolated p95");
    require(frame_time.max_ns && close(*frame_time.max_ns, 50.0), "rolling maximum");
    require(!snapshot.timings[static_cast<std::size_t>(Timing::capture)].average_ns,
            "empty distribution is absent");
}

void gpuConversionBridgeJoinsPassesByFramePts() {
    Telemetry telemetry(8);
    fluxrec::telemetry::GpuConversionTimingBridge bridge(telemetry);

    bridge.observePass(fluxrec::telemetry::GpuConversionPass::first, 10, 100);
    bridge.observePass(fluxrec::telemetry::GpuConversionPass::first, 11, 400);
    bridge.observePass(fluxrec::telemetry::GpuConversionPass::second, 10, 200);
    auto snapshot = telemetry.snapshot();
    auto& conversion = snapshot.timings[static_cast<std::size_t>(Timing::gpu_conversion)];
    require(conversion.samples == 1, "GPU conversion waits for both pass timestamps");
    require(conversion.average_ns && close(*conversion.average_ns, 300.0),
            "GPU conversion sample sums both pass durations by frame PTS");

    // Query readback from the second filter may complete before the first
    // filter's corresponding slot is reused; PTS keeps the pair aligned.
    bridge.observePass(fluxrec::telemetry::GpuConversionPass::second, 11, 600);
    snapshot = telemetry.snapshot();
    const auto& paired = snapshot.timings[static_cast<std::size_t>(Timing::gpu_conversion)];
    require(paired.samples == 2, "out-of-order pass completion yields one sample per frame");
    require(paired.average_ns && close(*paired.average_ns, 650.0),
            "GPU conversion average uses per-frame pass sums");
    require(paired.max_ns && *paired.max_ns == 1000,
            "GPU conversion maximum includes both pass durations");

    bridge.observePass(fluxrec::telemetry::GpuConversionPass::first,
                       std::numeric_limits<std::int64_t>::min(), 900);
    require(telemetry.snapshot().timings[static_cast<std::size_t>(Timing::gpu_conversion)].samples == 2,
            "samples without frame PTS are omitted from the per-frame histogram");
}

void nvencApiBridgeJoinsReorderedCallbacksByOutputPts() {
    Telemetry telemetry(16);
    NvencApiTimingBridge bridge(telemetry);

    bridge.observe(NvencApiCall::encode_picture, 100, 11'000, 1'000);
    bridge.observe(NvencApiCall::encode_picture, 200, 22'000, 2'000);
    bridge.observe(NvencApiCall::encode_picture, 300, 33'000, 3'000);
    // NVENC can return B-frame output in display-PTS order that differs from
    // submission order. Completion must join to the submission with that PTS.
    bridge.observe(NvencApiCall::lock_bitstream, 200, 44'000, 6'000);
    bridge.observe(NvencApiCall::lock_bitstream, 100, 55'000, 7'000);
    bridge.observe(NvencApiCall::lock_bitstream, 300, 66'000, 8'000);
    bridge.observe(NvencApiCall::encode_picture,
                   std::numeric_limits<std::int64_t>::min(), 77'000, 9'000);

    const auto snapshot = telemetry.snapshot();
    require(snapshot.nvenc_api_timing_samples.size() == 3,
            "NVENC API spans are emitted only after their PTS pair is complete");
    require(snapshot.nvenc_api_timing_samples[0].pts == 200 &&
            snapshot.nvenc_api_timing_samples[0].encode_picture_wall_ns == 22'000 &&
            snapshot.nvenc_api_timing_samples[0].lock_bitstream_wall_ns == 44'000 &&
            snapshot.nvenc_api_timing_samples[0].submit_to_bitstream_wall_ns == 4'000,
            "first completed NVENC pair retains the input/output PTS association");
    require(snapshot.nvenc_api_timing_samples[1].pts == 100 &&
            snapshot.nvenc_api_timing_samples[1].encode_picture_wall_ns == 11'000 &&
            snapshot.nvenc_api_timing_samples[1].lock_bitstream_wall_ns == 55'000 &&
            snapshot.nvenc_api_timing_samples[1].submit_to_bitstream_wall_ns == 6'000 &&
            snapshot.nvenc_api_timing_samples[2].pts == 300 &&
            snapshot.nvenc_api_timing_samples[2].submit_to_bitstream_wall_ns == 5'000,
            "B-frame-style output order preserves each submission-to-lock delta");

    const auto& encode = snapshot.timings[
        static_cast<std::size_t>(Timing::nvenc_encode_picture_api_wall)];
    const auto& lock = snapshot.timings[
        static_cast<std::size_t>(Timing::nvenc_lock_bitstream_api_wall)];
    require(encode.samples == 3 && lock.samples == 3,
            "per-frame pairs also populate both API wall-time distributions");
    const auto& availability = snapshot.timings[
        static_cast<std::size_t>(Timing::nvenc_submit_to_bitstream_wall)];
    require(availability.samples == 3 && availability.average_ns &&
                close(*availability.average_ns, 5'000.0) &&
                availability.max_ns && *availability.max_ns == 6'000,
            "PTS-matched submit-to-bitstream wall distribution uses return timestamps");
    require(snapshot.timings[static_cast<std::size_t>(Timing::nvenc_completion)].samples == 0,
            "host bitstream-availability proxy does not populate hardware completion");

    const auto json = Telemetry::toJson(snapshot);
    require(json.find("\"nvenc_encode_picture_api_wall\":{\"samples\":3") !=
                std::string::npos &&
            json.find("\"nvenc_lock_bitstream_api_wall\":{\"samples\":3") !=
                std::string::npos &&
            json.find("\"nvenc_submit_to_bitstream_wall\":{\"samples\":3") !=
                std::string::npos,
            "JSON names the host API call spans and availability proxy explicitly");
    require(json.find("\"nvenc_api_timing_samples\":[{\"pts\":200,\"encode_picture_wall_ns\":22000,\"lock_bitstream_wall_ns\":44000,\"submit_to_bitstream_wall_ns\":4000}") !=
                std::string::npos,
            "JSON retains output PTS correlation for recent paired samples");
}

void nvencApiBridgeOmitsRegressingClockPair() {
    Telemetry telemetry(8);
    NvencApiTimingBridge bridge(telemetry);
    bridge.observe(NvencApiCall::encode_picture, 10, 100, 5'000);
    bridge.observe(NvencApiCall::lock_bitstream, 10, 200, 4'000);

    const auto snapshot = telemetry.snapshot();
    require(snapshot.nvenc_api_timing_samples.size() == 1 &&
                !snapshot.nvenc_api_timing_samples.front().submit_to_bitstream_wall_ns,
            "regressing callback timestamps retain API spans but omit an invalid proxy");
    require(snapshot.timings[static_cast<std::size_t>(Timing::nvenc_submit_to_bitstream_wall)].samples == 0,
            "regressing monotonic event timestamps never underflow into a latency sample");
}

void concurrentCountsAndJson() {
    Telemetry telemetry(32);
    constexpr std::size_t thread_count = 6;
    constexpr std::uint64_t increments = 2000;
    std::vector<std::thread> workers;
    workers.reserve(thread_count);
    for (std::size_t i = 0; i < thread_count; ++i) {
        workers.emplace_back([&telemetry] {
            for (std::uint64_t j = 0; j < increments; ++j) {
                telemetry.addCaptured();
            }
        });
    }
    for (auto& worker : workers) worker.join();
    telemetry.observe(Timing::encoder_receive_cpu, 2200us);
    telemetry.observe(Timing::frame_identity_gpu, 65us);
    const auto json = telemetry.toJson();
    require(telemetry.snapshot().frames.captured == thread_count * increments,
            "concurrent atomic counters");
    require(json.starts_with("{\"timestamp_unix_ns\":"), "JSON object and timestamp");
    require(json.find("\"captured\":12000") != std::string::npos, "JSON frame counters");
    require(json.find("\"capture\":{\"wayland_completed\":null,\"admitted\":null,\"rate_limited\":null}") !=
                std::string::npos,
            "unobserved capture counters serialize distinctly from process_frame count");
    require(json.find("\"encoder_receive_cpu\"") != std::string::npos,
            "JSON emits a clearly labeled CPU timing");
    require(json.find("\"frame_identity_gpu\"") != std::string::npos,
            "JSON frame identity GPU timing");
    require(json.find("\"p99_9\":2200000") != std::string::npos, "JSON timing units and percentiles");
}

void hostAndWallTimingsAreLabeledByClockDomain() {
    constexpr std::array<std::pair<Timing, std::string_view>, 8> metrics{{
        {Timing::dmabuf_import_cpu, "dmabuf_import_cpu"},
        {Timing::cuda_transfer_cpu, "cuda_transfer_cpu"},
        {Timing::filter_submit_cpu, "filter_submit_cpu"},
        {Timing::filter_pull_cpu, "filter_pull_cpu"},
        {Timing::encoder_send_cpu, "encoder_send_cpu"},
        {Timing::encoder_receive_cpu, "encoder_receive_cpu"},
        {Timing::submit_to_packet_wall, "submit_to_packet_wall"},
        {Timing::capture_ready_to_submit_wall, "capture_ready_to_submit_wall"},
    }};
    Telemetry telemetry(16);
    for (std::size_t i = 0; i < metrics.size(); ++i) {
        telemetry.observe(metrics[i].first, std::chrono::nanoseconds{
            static_cast<std::int64_t>(1000U * (i + 1U))});
    }

    const auto snapshot = telemetry.snapshot();
    for (std::size_t i = 0; i < metrics.size(); ++i) {
        const auto& distribution = snapshot.timings[static_cast<std::size_t>(metrics[i].first)];
        require(distribution.samples == 1, "CPU/wall timing has one recorded sample");
        require(distribution.average_ns && close(*distribution.average_ns,
                    static_cast<double>(1000U * (i + 1U))),
                "CPU/wall timing uses nanoseconds");
    }

    const auto json = Telemetry::toJson(snapshot);
    for (const auto& [timing, name] : metrics) {
        (void)timing;
        require(json.find(std::string("\"") + std::string{name} + "\":{\"samples\":1") != std::string::npos,
                "JSON timing key explicitly names CPU or wall clock source");
    }
    require(json.find("\"gpu_conversion\":{\"samples\":0") != std::string::npos,
            "GPU execution timing remains unmeasured");
    require(json.find("\"nvenc_submission\":{\"samples\":0") != std::string::npos,
            "NVENC hardware timing remains unmeasured");
    require(json.find("\"nvenc_completion\":{\"samples\":0") != std::string::npos,
            "NVENC hardware completion timing remains unmeasured");
}

void unobservedFrameMetricsSerializeAsNull() {
    Telemetry telemetry(8);
    const auto json = telemetry.toJson();
    require(json.find("\"compositor\":null") != std::string::npos,
            "unobserved compositor count is null");
    require(json.find("\"capture\":{\"wayland_completed\":null,\"admitted\":null,\"rate_limited\":null}") !=
                std::string::npos,
            "unobserved capture counters serialize as null");
    require(json.find("\"unique\":null") != std::string::npos,
            "unobserved unique count is null");
    require(json.find("\"duplicated\":null") != std::string::npos,
            "unobserved duplicate count is null");
    require(json.find("\"identity_checked\":null") != std::string::npos,
            "unobserved digest count is null");
    require(json.find("\"identity_skipped\":null") != std::string::npos,
            "unobserved digest skip count is null");
    require(json.find("\"replay\":{\"enabled\":null,\"active\":null,\"packet_queue_drops\":null}") !=
                std::string::npos,
            "unobserved replay mode is null");
    require(json.find("\"recording_mode\":null") != std::string::npos,
            "unobserved recording mode is null");
    require(json.find("\"muxed\":null") != std::string::npos,
            "unobserved primary mux count is null");
}

void captureCountersSerializeWithoutConflatingDrops() {
    Telemetry telemetry(8);
    telemetry.addCaptured(7);
    telemetry.addCaptureWaylandCompleted(11);
    telemetry.addCaptureAdmitted(7);
    telemetry.addCaptureRateLimited(3);
    telemetry.addDroppedFrames(DropStage::capture, 1);

    const auto snapshot = telemetry.snapshot();
    require(snapshot.frames.captured == 7,
            "frames.captured remains the process_frame delivery count");
    require(snapshot.frames.dropped_by_stage[static_cast<std::size_t>(DropStage::capture)] == 1,
            "internal capture drop stays in the capture drop counter");
    const auto json = telemetry.toJson();
    require(json.find("\"capture\":{\"wayland_completed\":11,\"admitted\":7,\"rate_limited\":3}") !=
                std::string::npos,
            "JSON reports Wayland completions, admitted frames, and intentional cap rejects");
    require(json.find("\"captured\":7") != std::string::npos,
            "JSON preserves delivered-to-process_frame semantics");
    require(json.find("\"dropped_by_stage\":{\"capture\":1") != std::string::npos,
            "JSON keeps internal capture drops separate from rate-limited completions");
}

void monotonicCounterDeltaDoesNotUnderflowAfterReset() {
    using fluxrec::telemetry::monotonicCounterDelta;
    require(monotonicCounterDelta(4, 9) == 5,
            "increasing cumulative samples produce their delta");
    require(monotonicCounterDelta(9, 3) == 0,
            "a reset or inconsistent sample cannot underflow into a huge delta");
    // The sampler advances its baseline after a reset; subsequent increments
    // are then counted from that new baseline.
    const std::uint64_t reset_sample = 3;
    require(monotonicCounterDelta(reset_sample, 8) == 5,
            "increments after a reset are counted from the new baseline");
}

void replayModeReportsCapabilityAndActivity() {
    Telemetry telemetry(8);

    telemetry.observeReplayMode(true, true, 2, false);
    auto snapshot = telemetry.snapshot();
    require(snapshot.replay.enabled == true, "configured replay ring is enabled");
    require(snapshot.replay.active == true, "configured replay ring is active");
    auto json = telemetry.toJson();
    require(json.find("\"replay\":{\"enabled\":true,\"active\":true,\"packet_queue_drops\":2}") !=
                std::string::npos,
            "JSON reports initially active replay ring");
    require(json.find("\"recording_mode\":\"recording\"") != std::string::npos,
            "JSON reports normal recording mode");

    telemetry.observeReplayMode(true, false, 5);
    snapshot = telemetry.snapshot();
    require(snapshot.replay.enabled == true, "stopped ring remains configured");
    require(snapshot.replay.active == false, "stopped ring is inactive");
    json = telemetry.toJson();
    require(json.find("\"replay\":{\"enabled\":true,\"active\":false,\"packet_queue_drops\":5}") !=
                std::string::npos,
            "JSON distinguishes stopped configured ring");

    telemetry.observeReplayMode(false, false);
    snapshot = telemetry.snapshot();
    require(snapshot.replay.enabled == false, "missing replay ring is disabled");
    require(snapshot.replay.active == false, "missing replay ring is inactive");
    json = telemetry.toJson();
    require(json.find("\"replay\":{\"enabled\":false,\"active\":false,\"packet_queue_drops\":null}") !=
                std::string::npos,
            "JSON reports no replay ring");

    telemetry.observeReplayMode(true, true, 0, true);
    json = telemetry.toJson();
    require(json.find("\"recording_mode\":\"replay_only\"") != std::string::npos,
            "JSON identifies replay-buffer-only mode");
    require(json.find("\"muxed\":null") != std::string::npos,
            "replay-only telemetry leaves primary mux count unreported");
}

void frameRatesUseMonotonicIntervals() {
    fluxrec::telemetry::Snapshot previous{};
    fluxrec::telemetry::Snapshot current{};
    previous.monotonic_timestamp = std::chrono::steady_clock::time_point{};
    current.monotonic_timestamp = previous.monotonic_timestamp + 1s;
    current.frames.captured = 360;
    previous.frames.unique = 0;
    previous.frames.muxed = 0;
    current.frames.unique = 358;
    current.frames.submitted = 357;
    current.frames.encoded = 356;
    current.frames.muxed = 355;

    current.frame_rates = fluxrec::telemetry::calculateFrameRates(previous, current);
    require(current.frame_rates->captured_fps && close(*current.frame_rates->captured_fps, 360.0),
            "captured FPS from counter delta");
    require(current.frame_rates->unique_fps && close(*current.frame_rates->unique_fps, 358.0),
            "unique FPS from counter delta");
    require(!current.frame_rates->compositor_fps,
            "unobserved compositor FPS remains absent");
    require(current.frame_rates->encoded_fps && close(*current.frame_rates->encoded_fps, 356.0),
            "encoded FPS from counter delta");
    const auto json = Telemetry::toJson(current);
    require(json.find("\"fps\":{\"captured\":360") != std::string::npos,
            "JSON contains interval FPS");

    current.monotonic_timestamp = previous.monotonic_timestamp;
    require(!fluxrec::telemetry::calculateFrameRates(previous, current).captured_fps,
            "non-positive intervals produce no FPS");
}

void concurrentQueueAccountingAndReporter() {
    Telemetry telemetry(16);
    constexpr std::size_t thread_count = 6;
    constexpr std::uint64_t iterations = 1000;
    std::vector<std::thread> workers;
    for (std::size_t i = 0; i < thread_count; ++i) {
        workers.emplace_back([&telemetry] {
            for (std::uint64_t j = 0; j < iterations; ++j) {
                telemetry.addQueueEntries(Queue::encode);
                telemetry.removeQueueEntries(Queue::encode);
            }
        });
    }
    for (auto& worker : workers) worker.join();
    const auto snapshot = telemetry.snapshot();
    const auto& encode_queue = snapshot.queues[static_cast<std::size_t>(Queue::encode)];
    require(encode_queue.occupancy == 0, "concurrent queue deltas retain exact occupancy");
    require(encode_queue.high_water > 0, "concurrent queue deltas update high-water mark");

    std::mutex mutex;
    std::condition_variable wake;
    bool received = false;
    {
        fluxrec::telemetry::PeriodicJsonReporter reporter(
            telemetry, 5ms, [&](std::string json) {
                std::lock_guard lock(mutex);
                received = json.starts_with("{\"timestamp_unix_ns\":");
                wake.notify_one();
            });
        std::unique_lock lock(mutex);
        const bool emitted = wake.wait_for(lock, 500ms, [&] { return received; });
        require(emitted, "periodic reporter emits JSON snapshots");
    }
}

} // namespace

int main() {
    countersAndQueues();
    captureCountersSerializeWithoutConflatingDrops();
    monotonicCounterDeltaDoesNotUnderflowAfterReset();
    gpuConversionBridgeJoinsPassesByFramePts();
    nvencApiBridgeJoinsReorderedCallbacksByOutputPts();
    nvencApiBridgeOmitsRegressingClockPair();
    rollingTimingDistribution();
    concurrentCountsAndJson();
    hostAndWallTimingsAreLabeledByClockDomain();
    unobservedFrameMetricsSerializeAsNull();
    replayModeReportsCapabilityAndActivity();
    frameRatesUseMonotonicIntervals();
    concurrentQueueAccountingAndReporter();
    std::cout << "telemetry tests passed\n";
}
