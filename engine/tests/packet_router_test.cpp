#include "engine/packet_router.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

fluxrec::media::EncodedPacket make_packet(std::int64_t sequence) {
    fluxrec::media::EncodedPacket packet;
    packet.data.assign(32, static_cast<std::uint8_t>(sequence & 0xff));
    packet.codec = fluxrec::media::VideoCodec::h264;
    packet.pts = sequence;
    packet.dts = sequence;
    packet.duration = 1;
    packet.keyframe = sequence == 0;
    packet.time_base = {1, 360};
    return packet;
}

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void test_concurrent_close_drains_admitted_packets() {
    fluxrec::telemetry::Telemetry telemetry;
    std::atomic<std::uint64_t> routed_packets{0};
    fluxrec::engine::PacketRouter router{
        telemetry,
        [&](const fluxrec::engine::PacketRouter::PacketPtr& packet) {
            require(packet != nullptr, "router delivered a null packet during close test");
            const auto routed = routed_packets.fetch_add(1, std::memory_order_relaxed);
            if ((routed & 31U) == 0) std::this_thread::yield();
        },
        fluxrec::telemetry::DropStage::mux};

    constexpr std::size_t submission_count = 50'000;
    std::vector<fluxrec::media::EncodedPacket> packets;
    packets.reserve(submission_count);
    for (std::size_t index = 0; index < submission_count; ++index) {
        packets.push_back(make_packet(static_cast<std::int64_t>(index)));
    }

    std::atomic<std::uint64_t> accepted_packets{0};
    std::thread producer([&] {
        for (auto& packet : packets) {
            if (router.try_submit(std::move(packet))) {
                accepted_packets.fetch_add(1, std::memory_order_release);
            }
        }
    });

    const auto start_deadline = std::chrono::steady_clock::now() + 2s;
    while (accepted_packets.load(std::memory_order_acquire) == 0 &&
           std::chrono::steady_clock::now() < start_deadline) {
        std::this_thread::yield();
    }
    if (accepted_packets.load(std::memory_order_acquire) == 0) {
        router.close();
        producer.join();
        throw std::runtime_error("concurrent-close producer did not start submitting");
    }
    router.close();
    producer.join();

    require(routed_packets.load(std::memory_order_relaxed) ==
                accepted_packets.load(std::memory_order_relaxed),
            "close returned before all admitted packets reached the downstream worker");
    require(router.error().empty(), "router reported an unexpected error during close");
}

} // namespace

int main() {
    try {
        fluxrec::telemetry::Telemetry telemetry;
        std::promise<void> sink_entered_promise;
        auto sink_entered = sink_entered_promise.get_future();
        std::promise<void> release_sink_promise;
        auto release_sink = release_sink_promise.get_future().share();
        std::atomic<std::uint64_t> routed_packets{0};

        fluxrec::engine::PacketRouter router{
            telemetry,
            [&](const fluxrec::engine::PacketRouter::PacketPtr& packet) {
                require(packet != nullptr, "router delivered a null packet");
                const auto routed = routed_packets.fetch_add(1, std::memory_order_relaxed);
                if (routed == 0) {
                    sink_entered_promise.set_value();
                    release_sink.wait();
                }
            },
            fluxrec::telemetry::DropStage::mux};

        constexpr std::size_t queue_capacity = 512;
        std::vector<fluxrec::media::EncodedPacket> packets;
        packets.reserve(queue_capacity + 2U);
        for (std::size_t index = 0; index < queue_capacity + 2U; ++index) {
            packets.push_back(make_packet(static_cast<std::int64_t>(index)));
        }

        auto initial_submit = std::async(std::launch::async, [&] {
            return router.try_submit(std::move(packets[0]));
        });
        const bool sink_blocked = sink_entered.wait_for(2s) == std::future_status::ready;
        const bool initial_submit_returned =
            initial_submit.wait_for(2s) == std::future_status::ready;
        if (!sink_blocked || !initial_submit_returned) {
            release_sink_promise.set_value();
            (void)initial_submit.get();
            router.close();
            throw std::runtime_error(
                "producer handoff waited for the blocked downstream sink");
        }
        const bool initial_packet_accepted = initial_submit.get();
        if (!initial_packet_accepted) {
            release_sink_promise.set_value();
            router.close();
            throw std::runtime_error("router rejected the first packet");
        }

        auto producer = std::async(std::launch::async, [&] {
            std::size_t accepted = 0;
            std::size_t rejected = 0;
            for (std::size_t index = 1; index < packets.size(); ++index) {
                if (router.try_submit(std::move(packets[index]))) {
                    ++accepted;
                } else {
                    ++rejected;
                }
            }
            return std::pair{accepted, rejected};
        });
        const bool producer_finished_while_sink_blocked =
            producer.wait_for(2s) == std::future_status::ready;
        release_sink_promise.set_value();
        const auto [accepted, rejected] = producer.get();
        router.close();

        require(producer_finished_while_sink_blocked,
                "producer waited while the downstream sink was blocked");
        require(accepted == queue_capacity,
                "bounded ingress did not accept exactly its available capacity");
        require(rejected == 1, "bounded ingress did not reject exactly one overflow packet");
        require(router.packet_queue_dropped() == 1,
                "packet queue overflow counter did not record the rejected packet");
        require(router.dropped() == 1,
                "route drop counter did not record the rejected packet");
        require(routed_packets.load(std::memory_order_relaxed) == queue_capacity + 1U,
                "router did not drain every accepted packet after the sink resumed");
        require(router.error().empty(), "router reported an unexpected worker error");

        test_concurrent_close_drains_admitted_packets();

        std::cout << "packet router test passed: producer remained decoupled from a blocked "
                     "sink, overflow was counted, and concurrent close drained accepted packets\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "packet router test failed: " << exception.what() << '\n';
        return 1;
    }
}
