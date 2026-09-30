#pragma once

#include <cstdint>
#include <optional>

namespace fluxrec::gpu::detail {

struct DmabufBlackInterval {
    std::uint64_t first_sequence = 0;
    std::uint64_t last_sequence = 0;
    std::uint64_t first_presentation_time_ns = 0;
    std::uint64_t last_presentation_time_ns = 0;
    std::uint64_t black_frames = 0;
    bool closed_at_end = false;
};

[[nodiscard]] constexpr bool is_near_black_frame(std::uint32_t black_pixels,
                                                  std::uint32_t total_pixels) noexcept {
    return total_pixels != 0U && black_pixels <= total_pixels &&
           static_cast<std::uint64_t>(black_pixels) * 100U >=
               static_cast<std::uint64_t>(total_pixels) * 98U;
}

class DmabufBlackIntervalTracker final {
public:
    [[nodiscard]] std::optional<DmabufBlackInterval> observe(
        std::uint64_t sequence, std::uint64_t presentation_time_ns,
        std::uint32_t black_pixels, std::uint32_t total_pixels) noexcept {
        if (!is_near_black_frame(black_pixels, total_pixels)) {
            if (!active_) return std::nullopt;
            DmabufBlackInterval completed = interval_;
            completed.last_presentation_time_ns = previous_black_pts_ns_;
            active_ = false;
            const bool has_contiguous_successor =
                interval_.last_sequence != UINT64_MAX &&
                sequence == interval_.last_sequence + 1U;
            completed.closed_at_end = !has_contiguous_successor;
            return completed;
        }

        const bool contiguous = active_ &&
            interval_.last_sequence != UINT64_MAX &&
            sequence == interval_.last_sequence + 1U;
        if (!contiguous) {
            std::optional<DmabufBlackInterval> previous;
            if (active_) {
                previous = interval_;
                previous->closed_at_end = true;
            }
            interval_ = DmabufBlackInterval{
                .first_sequence = sequence,
                .last_sequence = sequence,
                .first_presentation_time_ns = presentation_time_ns,
                .last_presentation_time_ns = presentation_time_ns,
                .black_frames = 1U,
                .closed_at_end = false,
            };
            previous_black_pts_ns_ = presentation_time_ns;
            active_ = true;
            return previous;
        }

        interval_.last_sequence = sequence;
        interval_.last_presentation_time_ns = presentation_time_ns;
        ++interval_.black_frames;
        previous_black_pts_ns_ = presentation_time_ns;
        return std::nullopt;
    }

    [[nodiscard]] std::optional<DmabufBlackInterval> finish() noexcept {
        if (!active_) return std::nullopt;
        DmabufBlackInterval completed = interval_;
        completed.last_presentation_time_ns = previous_black_pts_ns_;
        completed.closed_at_end = true;
        active_ = false;
        return completed;
    }

private:
    DmabufBlackInterval interval_{};
    std::uint64_t previous_black_pts_ns_ = 0;
    bool active_ = false;
};

} // namespace fluxrec::gpu::detail
