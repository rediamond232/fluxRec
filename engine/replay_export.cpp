#include "engine/replay_export.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <exception>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace fluxrec::engine {
namespace {

using WideInteger = __int128_t;

[[nodiscard]] bool is_retryable(mux::EnqueueResult result) noexcept {
    return result == mux::EnqueueResult::queue_full ||
           result == mux::EnqueueResult::queue_busy;
}

[[nodiscard]] const char* enqueue_error_name(mux::EnqueueResult result) noexcept {
    switch (result) {
    case mux::EnqueueResult::accepted: return "accepted";
    case mux::EnqueueResult::queue_full: return "queue full";
    case mux::EnqueueResult::queue_busy: return "queue busy";
    case mux::EnqueueResult::packet_too_large: return "packet too large";
    case mux::EnqueueResult::invalid_packet: return "invalid packet";
    case mux::EnqueueResult::not_ready: return "writer not ready";
    case mux::EnqueueResult::closed: return "writer closed";
    }
    return "unknown enqueue result";
}

[[nodiscard]] bool timestamp_offset_ns(std::int64_t timestamp,
                                       media::Rational time_base,
                                       std::int64_t& result) noexcept {
    if (!time_base.valid() || timestamp == std::numeric_limits<std::int64_t>::min()) {
        return false;
    }
    const WideInteger value = static_cast<WideInteger>(timestamp) *
        time_base.num * 1'000'000'000 / time_base.den;
    if (value < std::numeric_limits<std::int64_t>::min() ||
        value > std::numeric_limits<std::int64_t>::max()) {
        return false;
    }
    result = static_cast<std::int64_t>(value);
    return true;
}

[[nodiscard]] bool packet_dts_offset_ns(const replay::Packet& packet,
                                        std::int64_t& result) noexcept {
    return timestamp_offset_ns(packet.dts, packet.time_base, result);
}

} // namespace

struct ReplayExporter::Impl {
    struct Job {
        std::uint64_t id{};
        mux::MuxConfig mux_config;
        replay::ReplaySnapshot snapshot;
        bool include_audio{};
        std::uint64_t timestamp_origin_time_ns{};
    };

    struct OrderedPacket {
        std::int64_t absolute_dts_ns{};
        std::size_t sequence{};
        std::variant<replay::PacketPtr, replay::AudioPacketPtr> packet;
    };

    explicit Impl(replay::ReplayBuffer& replay_buffer, ReplayExporterConfig value)
        : buffer(replay_buffer), config(value) {
        if (config.max_in_flight_jobs == 0 || config.max_completed_results == 0 ||
            config.writer_ready_timeout <= std::chrono::milliseconds::zero() ||
            config.writer_stall_timeout <= std::chrono::milliseconds::zero() ||
            config.initial_retry_delay <= std::chrono::microseconds::zero() ||
            config.max_retry_delay < config.initial_retry_delay) {
            throw std::invalid_argument("replay exporter limits and timeouts must be positive");
        }
        // Allocate the fixed result ring before starting the worker. Moving a
        // completed result into a preallocated slot cannot allocate, even if
        // memory pressure occurs while an export is finishing.
        completed_results.resize(config.max_completed_results);
        worker = std::thread([this] { run(); });
    }

    ~Impl() { close(); }

    ReplayExportSubmission submit(mux::MuxConfig mux_config,
                                  replay::Nanoseconds requested_duration) {
        if (mux_config.output_path.empty()) {
            return {ReplayExportSubmitStatus::invalid_config, 0,
                    replay::SnapshotStatus::no_keyframe};
        }

        {
            std::lock_guard lock(mutex);
            if (stopping) {
                return {ReplayExportSubmitStatus::stopping, 0,
                        replay::SnapshotStatus::no_keyframe};
            }
            if (in_flight_jobs >= config.max_in_flight_jobs) {
                rejected_capacity.fetch_add(1, std::memory_order_relaxed);
                return {ReplayExportSubmitStatus::queue_full, 0,
                        replay::SnapshotStatus::no_keyframe};
            }
            ++in_flight_jobs;
        }

        replay::ReplaySnapshot snapshot;
        try {
            snapshot = buffer.snapshot(requested_duration);
        } catch (...) {
            release_reservation();
            throw;
        }
        if (!snapshot) {
            const auto status = snapshot.status();
            release_reservation();
            rejected_snapshot.fetch_add(1, std::memory_order_relaxed);
            return {ReplayExportSubmitStatus::snapshot_unavailable, 0, status};
        }

        bool found_packet = false;
        bool codec_matches = true;
        try {
            snapshot.for_each_packet([&](const replay::PacketPtr& packet) {
                if (!found_packet) {
                    found_packet = true;
                    codec_matches = packet && packet->codec == mux_config.codec;
                }
            });
            if (codec_matches && !mux_config.audio_streams.empty()) {
                snapshot.for_each_audio_packet([&](const replay::AudioPacketPtr& packet) {
                    if (!packet || packet->stream_index >= mux_config.audio_streams.size()) {
                        codec_matches = false;
                        return;
                    }
                    const auto& stream = mux_config.audio_streams[packet->stream_index];
                    if (packet->codec != stream.codec ||
                        packet->time_base.num != stream.stream_time_base.num ||
                        packet->time_base.den != stream.stream_time_base.den) {
                        codec_matches = false;
                    }
                });
            }
        } catch (...) {
            snapshot = {};
            release_reservation();
            throw;
        }
        if (!found_packet || !codec_matches) {
            snapshot = {};
            release_reservation();
            rejected_codec_mismatch.fetch_add(1, std::memory_order_relaxed);
            return {ReplayExportSubmitStatus::snapshot_codec_mismatch, 0,
                    replay::SnapshotStatus::ready};
        }

        const bool include_audio = !mux_config.audio_streams.empty();
        const auto timestamp_origin_time_ns = mux_config.video_origin_time_ns;
        if (include_audio) {
            if (timestamp_origin_time_ns == 0 ||
                timestamp_origin_time_ns >
                    static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                snapshot = {};
                release_reservation();
                return {ReplayExportSubmitStatus::invalid_config, 0,
                        replay::SnapshotStatus::ready};
            }
        }

        std::lock_guard lock(mutex);
        if (stopping) {
            snapshot = {};
            --in_flight_jobs;
            return {ReplayExportSubmitStatus::stopping, 0,
                    replay::SnapshotStatus::ready};
        }
        const auto id = next_job_id++;
        try {
            jobs.push_back(Job{id, std::move(mux_config), std::move(snapshot), include_audio,
                               timestamp_origin_time_ns});
        } catch (...) {
            --in_flight_jobs;
            throw;
        }
        submitted_jobs.fetch_add(1, std::memory_order_relaxed);
        work_available.notify_one();
        return {ReplayExportSubmitStatus::queued, id, replay::SnapshotStatus::ready};
    }

    void release_reservation() noexcept {
        std::lock_guard lock(mutex);
        --in_flight_jobs;
    }

    [[nodiscard]] ReplayExportResult process(Job job) noexcept {
        ReplayExportResult result;
        result.job_id = job.id;
        result.snapshot_video_packets = job.snapshot.packet_count();
        result.snapshot_audio_packets = job.include_audio
            ? job.snapshot.audio_packet_count() : 0;
        result.snapshot_packets = result.snapshot_video_packets +
                                 result.snapshot_audio_packets;
        result.snapshot_payload_bytes = job.snapshot.payload_bytes();

        try {
            result.output_path = job.mux_config.output_path;
            mux::MuxWriter writer(std::move(job.mux_config));
            if (!writer.wait_until_ready(config.writer_ready_timeout)) {
                result.error = writer.error_message();
                if (result.error.empty()) {
                    result.error = "mux writer did not become ready before the timeout";
                }
                writer.close();
                const auto mux_stats = writer.stats();
                result.muxed_packets = mux_stats.muxed_packets;
                result.output_file_bytes = mux_stats.output_file_bytes;
                return result;
            }

            auto retry_delay = config.initial_retry_delay;
            auto last_progress = std::chrono::steady_clock::now();
            bool enqueue_failed = false;
            std::string enqueue_error;

            const auto enqueue_packet = [&](const auto& packet) {
                if (enqueue_failed) {
                    return;
                }
                for (;;) {
                    const auto enqueue_result = writer.try_enqueue(packet);
                    if (enqueue_result == mux::EnqueueResult::accepted) {
                        ++result.enqueued_packets;
                        last_progress = std::chrono::steady_clock::now();
                        retry_delay = config.initial_retry_delay;
                        return;
                    }

                    if (!is_retryable(enqueue_result)) {
                        enqueue_failed = true;
                        enqueue_error = "MuxWriter rejected replay packet: ";
                        enqueue_error += enqueue_error_name(enqueue_result);
                        const auto writer_error = writer.error_message();
                        if (!writer_error.empty()) {
                            enqueue_error += ": ";
                            enqueue_error += writer_error;
                        }
                        return;
                    }

                    ++result.enqueue_retries;
                    enqueue_retries.fetch_add(1, std::memory_order_relaxed);
                    const auto now = std::chrono::steady_clock::now();
                    if (now - last_progress >= config.writer_stall_timeout) {
                        enqueue_failed = true;
                        enqueue_error = "MuxWriter queue remained full or busy past the stall timeout";
                        return;
                    }
                    if (writer.state() == mux::WriterState::failed ||
                        writer.state() == mux::WriterState::closed) {
                        enqueue_failed = true;
                        enqueue_error = writer.error_message();
                        if (enqueue_error.empty()) {
                            enqueue_error = "MuxWriter stopped while exporting replay packets";
                        }
                        return;
                    }

                    std::this_thread::sleep_for(retry_delay);
                    const auto maximum_retry_delay =
                        std::chrono::duration_cast<std::chrono::microseconds>(
                            config.max_retry_delay);
                    if (retry_delay >= maximum_retry_delay - retry_delay) {
                        retry_delay = maximum_retry_delay;
                    } else {
                        retry_delay += retry_delay;
                    }
                }
            };

            std::vector<OrderedPacket> ordered_packets;
            ordered_packets.reserve(job.snapshot.total_packet_count());
            std::size_t sequence = 0;
            bool timestamps_valid = true;
            job.snapshot.for_each_packet([&](const replay::PacketPtr& packet) {
                std::int64_t relative_dts_ns = 0;
                if (!packet || !packet_dts_offset_ns(*packet, relative_dts_ns)) {
                    timestamps_valid = false;
                    return;
                }
                const WideInteger absolute_dts_ns =
                    static_cast<WideInteger>(job.timestamp_origin_time_ns) + relative_dts_ns;
                if (absolute_dts_ns < std::numeric_limits<std::int64_t>::min() ||
                    absolute_dts_ns > std::numeric_limits<std::int64_t>::max()) {
                    timestamps_valid = false;
                    return;
                }
                ordered_packets.push_back(OrderedPacket{
                    static_cast<std::int64_t>(absolute_dts_ns), sequence++, packet});
            });
            if (job.include_audio) {
                job.snapshot.for_each_audio_packet([&](const replay::AudioPacketPtr& packet) {
                    std::int64_t absolute_dts_ns = 0;
                    if (!packet || !timestamp_offset_ns(packet->pts, packet->time_base,
                                                        absolute_dts_ns)) {
                        timestamps_valid = false;
                        return;
                    }
                    ordered_packets.push_back(
                        OrderedPacket{absolute_dts_ns, sequence++, packet});
                });
            }
            if (!timestamps_valid || ordered_packets.size() != result.snapshot_packets) {
                enqueue_failed = true;
                enqueue_error = "replay snapshot contains a packet without a usable absolute DTS";
            } else {
                std::sort(ordered_packets.begin(), ordered_packets.end(),
                          [](const OrderedPacket& left, const OrderedPacket& right) {
                              if (left.absolute_dts_ns != right.absolute_dts_ns) {
                                  return left.absolute_dts_ns < right.absolute_dts_ns;
                              }
                              return left.sequence < right.sequence;
                          });
                for (const auto& packet : ordered_packets) {
                    std::visit(enqueue_packet, packet.packet);
                    if (enqueue_failed) break;
                }
            }

            writer.close();
            const auto mux_stats = writer.stats();
            result.muxed_packets = mux_stats.muxed_packets;
            result.output_file_bytes = mux_stats.output_file_bytes;
            if (enqueue_failed) {
                result.error = std::move(enqueue_error);
            } else if (writer.state() == mux::WriterState::failed) {
                result.error = writer.error_message();
            } else if (result.enqueued_packets != result.snapshot_packets ||
                       result.muxed_packets != result.snapshot_packets) {
                result.error = "export packet count did not match the replay snapshot";
            } else {
                result.succeeded = true;
            }
        } catch (const std::exception& exception) {
            set_error_noexcept(result, exception.what());
        } catch (...) {
            set_error_noexcept(result, "unknown error while exporting replay snapshot");
        }
        return result;
    }

    static void set_error_noexcept(ReplayExportResult& result, const char* message) noexcept {
        try {
            result.error = message;
        } catch (...) {
            // Preserve a failed result even when its diagnostic cannot be
            // allocated. An empty error string is preferable to terminating
            // the exporter thread from a noexcept path.
            result.error.clear();
        }
    }

    void run() noexcept {
        for (;;) {
            Job job;
            {
                std::unique_lock lock(mutex);
                work_available.wait(lock, [this] { return stopping || !jobs.empty(); });
                if (jobs.empty() && stopping) {
                    return;
                }
                job = std::move(jobs.front());
                jobs.pop_front();
            }

            auto result = process(std::move(job));
            {
                std::lock_guard lock(mutex);
                --in_flight_jobs;
                if (completed_result_count == completed_results.size()) {
                    completed_results[completed_result_read].reset();
                    completed_result_read = (completed_result_read + 1U) % completed_results.size();
                    --completed_result_count;
                    completed_results_discarded.fetch_add(1, std::memory_order_relaxed);
                }
                if (result.succeeded) {
                    completed_jobs.fetch_add(1, std::memory_order_relaxed);
                } else {
                    failed_jobs.fetch_add(1, std::memory_order_relaxed);
                }
                completed_results[completed_result_write].emplace(std::move(result));
                completed_result_write = (completed_result_write + 1U) % completed_results.size();
                ++completed_result_count;
            }
        }
    }

    bool try_pop_result(ReplayExportResult& result) {
        std::lock_guard lock(mutex);
        if (completed_result_count == 0) {
            return false;
        }
        result = std::move(*completed_results[completed_result_read]);
        completed_results[completed_result_read].reset();
        completed_result_read = (completed_result_read + 1U) % completed_results.size();
        --completed_result_count;
        return true;
    }

    ReplayExporterStats stats() const noexcept {
        ReplayExporterStats result;
        {
            std::lock_guard lock(mutex);
            result.in_flight_jobs = in_flight_jobs;
            result.queued_jobs = jobs.size();
        }
        result.submitted_jobs = submitted_jobs.load(std::memory_order_relaxed);
        result.completed_jobs = completed_jobs.load(std::memory_order_relaxed);
        result.failed_jobs = failed_jobs.load(std::memory_order_relaxed);
        result.rejected_capacity = rejected_capacity.load(std::memory_order_relaxed);
        result.rejected_snapshot = rejected_snapshot.load(std::memory_order_relaxed);
        result.rejected_codec_mismatch = rejected_codec_mismatch.load(std::memory_order_relaxed);
        result.enqueue_retries = enqueue_retries.load(std::memory_order_relaxed);
        result.completed_results_discarded =
            completed_results_discarded.load(std::memory_order_relaxed);
        return result;
    }

    void close() noexcept {
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        work_available.notify_all();
        try {
            if (worker.joinable() && worker.get_id() != std::this_thread::get_id()) {
                worker.join();
            }
        } catch (...) {
            // Explicit shutdown and destruction must not throw.
        }
    }

    replay::ReplayBuffer& buffer;
    ReplayExporterConfig config;
    mutable std::mutex mutex;
    std::condition_variable work_available;
    std::deque<Job> jobs;
    static_assert(std::is_nothrow_move_constructible_v<ReplayExportResult>);
    static_assert(std::is_nothrow_move_assignable_v<ReplayExportResult>);
    std::vector<std::optional<ReplayExportResult>> completed_results;
    std::thread worker;
    std::size_t in_flight_jobs{};
    std::size_t completed_result_read{};
    std::size_t completed_result_write{};
    std::size_t completed_result_count{};
    std::uint64_t next_job_id{1};
    bool stopping{};

    std::atomic<std::uint64_t> submitted_jobs{0};
    std::atomic<std::uint64_t> completed_jobs{0};
    std::atomic<std::uint64_t> failed_jobs{0};
    std::atomic<std::uint64_t> rejected_capacity{0};
    std::atomic<std::uint64_t> rejected_snapshot{0};
    std::atomic<std::uint64_t> rejected_codec_mismatch{0};
    std::atomic<std::uint64_t> enqueue_retries{0};
    std::atomic<std::uint64_t> completed_results_discarded{0};
};

ReplayExporter::ReplayExporter(replay::ReplayBuffer& buffer, ReplayExporterConfig config)
    : impl_(std::make_unique<Impl>(buffer, config)) {}

ReplayExporter::~ReplayExporter() = default;

ReplayExportSubmission ReplayExporter::submit(
    mux::MuxConfig mux_config, replay::Nanoseconds requested_duration) {
    return impl_->submit(std::move(mux_config), requested_duration);
}

bool ReplayExporter::try_pop_result(ReplayExportResult& result) {
    return impl_->try_pop_result(result);
}

ReplayExporterStats ReplayExporter::stats() const noexcept {
    return impl_->stats();
}

void ReplayExporter::close() noexcept {
    if (impl_) {
        impl_->close();
    }
}

} // namespace fluxrec::engine
