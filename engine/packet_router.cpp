#include "engine/packet_router.hpp"

#include "mux/mux_writer.hpp"
#include "replay/replay_buffer.hpp"

#include <stdexcept>
#include <type_traits>
#include <utility>

namespace fluxrec::engine {
namespace {

static_assert(std::is_nothrow_move_assignable_v<media::EncodedPacket>,
              "the producer ingress queue requires allocation-free packet moves");

} // namespace

PacketRouter::ProducerActivity::ProducerActivity(
    std::atomic<std::uint32_t>& counter) noexcept
    : counter_(counter) {
    counter_.fetch_add(1, std::memory_order_acq_rel);
}

PacketRouter::ProducerActivity::~ProducerActivity() {
    counter_.fetch_sub(1, std::memory_order_release);
}

PacketRouter::PacketRouter(mux::MuxWriter* writer, replay::ReplayBuffer* replay,
                           telemetry::Telemetry& telemetry)
    : writer_(writer), replay_(replay), telemetry_(telemetry),
      queue_overflow_drop_stage_(writer != nullptr
          ? telemetry::DropStage::mux : telemetry::DropStage::replay),
      ring_(max_packets_ + 1U) {
    start_worker();
}

PacketRouter::PacketRouter(telemetry::Telemetry& telemetry, PacketSink sink,
                           telemetry::DropStage queue_overflow_drop_stage)
    : telemetry_(telemetry), sink_(std::move(sink)),
      queue_overflow_drop_stage_(queue_overflow_drop_stage),
      ring_(max_packets_ + 1U) {
    if (!sink_) {
        throw std::invalid_argument("packet router sink must be set");
    }
    start_worker();
}

PacketRouter::~PacketRouter() { close(); }

void PacketRouter::start_worker() {
    telemetry_.setQueueCapacity(telemetry::Queue::packet, max_packets_);
    telemetry_.observeQueue(telemetry::Queue::packet, 0);
    worker_ = std::thread([this] { run(); });
}

bool PacketRouter::try_submit(media::EncodedPacket&& packet) {
    if (stopping_.load(std::memory_order_acquire)) {
        note_route_drop();
        return false;
    }
    ProducerActivity activity{producers_active_};
    if (stopping_.load(std::memory_order_acquire)) {
        note_route_drop();
        return false;
    }
    const auto bytes = packet.data.size();
    const auto head = head_.load(std::memory_order_relaxed);
    const auto next = (head + 1U) % ring_.size();
    const auto queued_bytes = queued_bytes_.load(std::memory_order_acquire);
    const bool queue_full = next == tail_.load(std::memory_order_acquire) ||
                            bytes > max_bytes_ || queued_bytes > max_bytes_ - bytes;
    if (stopping_.load(std::memory_order_acquire)) {
        note_route_drop();
        return false;
    }
    if (queue_full) {
        packet_queue_dropped_.fetch_add(1, std::memory_order_relaxed);
        note_route_drop();
        return false;
    }

    ring_[head] = std::move(packet);
    queued_bytes_.fetch_add(bytes, std::memory_order_release);
    head_.store(next, std::memory_order_release);
    telemetry_.addQueueEntries(telemetry::Queue::packet);
    available_.release();
    return true;
}

void PacketRouter::close() noexcept {
    std::lock_guard close_lock(close_mutex_);
    if (!stopping_.exchange(true, std::memory_order_acq_rel)) {
        while (producers_active_.load(std::memory_order_acquire) != 0) {
            std::this_thread::yield();
        }
        available_.release();
    }
    if (worker_.joinable()) {
        worker_.join();
    }
}

std::uint64_t PacketRouter::dropped() const noexcept {
    return dropped_.load(std::memory_order_relaxed);
}

std::uint64_t PacketRouter::packet_queue_dropped() const noexcept {
    return packet_queue_dropped_.load(std::memory_order_relaxed);
}

std::uint64_t PacketRouter::replay_dropped() const noexcept {
    return replay_dropped_.load(std::memory_order_relaxed);
}

std::string PacketRouter::error() const {
    std::lock_guard lock(error_mutex_);
    return worker_error_;
}

void PacketRouter::route(const PacketPtr& packet) {
    if (sink_) {
        sink_(packet);
        return;
    }

    if (replay_ != nullptr) {
        const auto append_result = replay_->append(packet);
        if (append_result != replay::AppendResult::stored) {
            replay_dropped_.fetch_add(1, std::memory_order_relaxed);
            if (append_result != replay::AppendResult::inactive) {
                telemetry_.addDroppedFrames(telemetry::DropStage::replay);
            }
        }
    }
    if (writer_ == nullptr) {
        return;
    }

    auto result = writer_->try_enqueue(packet);
    for (unsigned int retry = 0;
         result == mux::EnqueueResult::queue_busy &&
         retry < max_queue_busy_retries_ &&
         writer_->state() == mux::WriterState::running;
         ++retry) {
        std::this_thread::yield();
        result = writer_->try_enqueue(packet);
    }
    if (result != mux::EnqueueResult::accepted) {
        note_route_drop();
    }
}

void PacketRouter::note_route_drop() noexcept {
    dropped_.fetch_add(1, std::memory_order_relaxed);
    telemetry_.addDroppedFrames(queue_overflow_drop_stage_);
}

void PacketRouter::run() noexcept {
    PacketPtr in_progress;
    try {
        for (;;) {
            available_.acquire();
            const auto tail = tail_.load(std::memory_order_relaxed);
            if (tail == head_.load(std::memory_order_acquire)) {
                if (stopping_.load(std::memory_order_acquire)) return;
                continue;
            }

            auto& queued_packet = ring_[tail];
            const auto bytes = queued_packet.data.size();
            // Shared ownership and its allocator cost stay on this worker. The
            // consumer publishes the next tail only after moving the packet
            // value out of the preallocated ring slot.
            in_progress = std::make_shared<const media::EncodedPacket>(
                std::move(queued_packet));
            tail_.store((tail + 1U) % ring_.size(), std::memory_order_release);
            queued_bytes_.fetch_sub(bytes, std::memory_order_release);
            telemetry_.removeQueueEntries(telemetry::Queue::packet);

            route(in_progress);
            in_progress.reset();
        }
    } catch (const std::exception& exception) {
        set_worker_error(exception.what());
        if (in_progress) {
            note_route_drop();
            in_progress.reset();
        }
        discard_queued_packets();
    } catch (...) {
        set_worker_error("packet router worker failed with an unknown exception");
        if (in_progress) {
            note_route_drop();
            in_progress.reset();
        }
        discard_queued_packets();
    }
}

void PacketRouter::set_worker_error(const char* message) noexcept {
    try {
        std::lock_guard lock(error_mutex_);
        if (worker_error_.empty()) {
            worker_error_ = message ? message : "packet router failed";
        }
    } catch (...) {
        // Preserve the stop state even if formatting the diagnostic fails.
    }
    stopping_.store(true, std::memory_order_release);
    available_.release();
}

void PacketRouter::discard_queued_packets() noexcept {
    while (producers_active_.load(std::memory_order_acquire) != 0) {
        std::this_thread::yield();
    }
    for (;;) {
        const auto tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) return;
        auto& packet = ring_[tail];
        queued_bytes_.fetch_sub(packet.data.size(), std::memory_order_release);
        packet = {};
        tail_.store((tail + 1U) % ring_.size(), std::memory_order_release);
        note_route_drop();
        telemetry_.removeQueueEntries(telemetry::Queue::packet);
    }
}

} // namespace fluxrec::engine
