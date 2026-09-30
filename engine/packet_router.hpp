#pragma once

#include "mux/packet.hpp"
#include "telemetry/telemetry.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <semaphore>
#include <string>
#include <thread>
#include <vector>

namespace fluxrec::mux {
class MuxWriter;
}

namespace fluxrec::replay {
class ReplayBuffer;
}

namespace fluxrec::engine {

class PacketRouter final {
public:
    using PacketPtr = std::shared_ptr<const media::EncodedPacket>;
    // An injected sink runs on the router worker and may block it; the
    // bounded ingress ring then fills and rejects new packets.
    using PacketSink = std::function<void(const PacketPtr&)>;

    PacketRouter(mux::MuxWriter* writer, replay::ReplayBuffer* replay,
                 telemetry::Telemetry& telemetry);
    PacketRouter(telemetry::Telemetry& telemetry, PacketSink sink,
                 telemetry::DropStage queue_overflow_stage = telemetry::DropStage::mux);
    ~PacketRouter();

    PacketRouter(const PacketRouter&) = delete;
    PacketRouter& operator=(const PacketRouter&) = delete;

    // Single-producer contract: all calls must come from one producer thread
    // and must not overlap. Never waits for the downstream worker. The packet
    // value is moved into preallocated storage; shared ownership is created by
    // the worker. Returns false if the bounded ring is full or the router has
    // stopped.
    [[nodiscard]] bool try_submit(media::EncodedPacket&& packet);

    // Stops admission, drains queued packets (or discards them after a worker
    // error), and joins the worker. Call on a control thread, not from the
    // PacketSink callback.
    void close() noexcept;

    [[nodiscard]] std::uint64_t dropped() const noexcept;
    // Counts ingress capacity/byte-limit rejects only; excludes close rejects
    // and downstream mux/replay drops.
    [[nodiscard]] std::uint64_t packet_queue_dropped() const noexcept;
    [[nodiscard]] std::uint64_t replay_dropped() const noexcept;
    [[nodiscard]] std::string error() const;

private:
    struct ProducerActivity {
        explicit ProducerActivity(std::atomic<std::uint32_t>& counter) noexcept;
        ~ProducerActivity();
        std::atomic<std::uint32_t>& counter_;
    };

    void start_worker();
    void route(const PacketPtr& packet);
    void note_route_drop() noexcept;
    void run() noexcept;
    void set_worker_error(const char* message) noexcept;
    void discard_queued_packets() noexcept;

    static constexpr std::size_t max_packets_ = 512;
    static constexpr std::size_t max_bytes_ = 64U * 1024U * 1024U;
    static constexpr unsigned int max_queue_busy_retries_ = 32;
    static constexpr std::ptrdiff_t semaphore_max_ =
        static_cast<std::ptrdiff_t>(max_packets_ + 1U);

    mux::MuxWriter* writer_{};
    replay::ReplayBuffer* replay_{};
    telemetry::Telemetry& telemetry_;
    PacketSink sink_;
    telemetry::DropStage queue_overflow_drop_stage_{telemetry::DropStage::mux};
    std::vector<media::EncodedPacket> ring_;
    std::atomic<std::size_t> head_{0};
    std::atomic<std::size_t> tail_{0};
    std::atomic<std::size_t> queued_bytes_{0};
    std::atomic<bool> stopping_{false};
    std::atomic<std::uint32_t> producers_active_{0};
    std::counting_semaphore<semaphore_max_> available_{0};
    std::atomic<std::uint64_t> dropped_{0};
    std::atomic<std::uint64_t> packet_queue_dropped_{0};
    std::atomic<std::uint64_t> replay_dropped_{0};
    mutable std::mutex error_mutex_;
    std::mutex close_mutex_;
    std::string worker_error_;
    std::thread worker_;
};

} // namespace fluxrec::engine
