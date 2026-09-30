#include "engine/encoder_stats_telemetry.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

const fluxrec::telemetry::DistributionSnapshot& timing(
    const fluxrec::telemetry::Snapshot& snapshot,
    fluxrec::telemetry::Timing key) {
    return snapshot.timings[static_cast<std::size_t>(key)];
}

void cumulativeStatsIncludeAsynchronousUpdatesExactlyOnce() {
    using namespace fluxrec;
    telemetry::Telemetry telemetry{16};
    gpu::EncoderStats initial;
    engine::EncoderStatsTelemetryObserver observer{telemetry, initial};

    // Submit counts advance synchronously while packets and receive timings
    // arrive later from the encoder output worker.
    gpu::EncoderStats after_submit = initial;
    after_submit.submitted_frames = 3;
    after_submit.total_encoder_receive_cpu_ns = 4'000;
    observer.observe(after_submit);
    observer.observe(after_submit);
    auto snapshot = telemetry.snapshot();
    require(snapshot.frames.submitted == 3, "submission deltas are observed once");
    require(snapshot.frames.encoded == 0, "pending async outputs stay unencoded");
    require(timing(snapshot, telemetry::Timing::encoder_receive_cpu).samples == 1,
            "async receive timing total is observed");

    gpu::EncoderStats after_worker = after_submit;
    after_worker.encoded_frames = 2;
    after_worker.total_encoder_receive_cpu_ns = 10'000;
    after_worker.submit_to_packet_samples = 2;
    after_worker.total_submit_to_packet_wall_ns = 6'000;
    observer.observe(after_worker);
    observer.observe(after_worker);
    snapshot = telemetry.snapshot();
    require(snapshot.frames.encoded == 2, "async encoded frames are observed once");
    const auto& latency = timing(snapshot, telemetry::Timing::submit_to_packet_wall);
    require(latency.samples == 2, "batched packet latency count is retained");
    require(latency.average_ns && std::abs(*latency.average_ns - 3'000.0) < 0.01,
            "batched packet latency uses cumulative mean");

    // Final drain may release a last packet without another submit call.
    gpu::EncoderStats after_drain = after_worker;
    after_drain.encoded_frames = 3;
    after_drain.total_encoder_receive_cpu_ns = 12'000;
    after_drain.submit_to_packet_samples = 3;
    after_drain.total_submit_to_packet_wall_ns = 11'000;
    observer.observe(after_drain);
    observer.observe(after_drain);
    snapshot = telemetry.snapshot();
    require(snapshot.frames.submitted == 3 && snapshot.frames.encoded == 3,
            "final-drain deltas are included once");
    require(timing(snapshot, telemetry::Timing::encoder_receive_cpu).samples == 3,
            "async receive CPU timings include submit and drain intervals");
    require(timing(snapshot, telemetry::Timing::submit_to_packet_wall).samples == 3,
            "final-drain packet latency is counted once");
    require(timing(snapshot, telemetry::Timing::submit_to_packet_wall).average_ns &&
                std::abs(*timing(snapshot, telemetry::Timing::submit_to_packet_wall).average_ns -
                         (11'000.0 / 3.0)) < 0.01,
            "packet latency mean includes the final drained packet");
}

} // namespace

int main() {
    cumulativeStatsIncludeAsynchronousUpdatesExactlyOnce();
    std::cout << "all encoder stats telemetry tests passed\n";
}
