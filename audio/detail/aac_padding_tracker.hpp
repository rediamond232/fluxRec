#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace fluxrec::audio::detail {

// AAC can delay packet output while preserving the submitted frame PTS. Keep
// only a small, fixed number of partial-frame markers until their packet PTS
// arrives; this adds no heap use or unbounded buffering to the audio worker.
class AacPaddingTracker final {
public:
    static constexpr std::size_t capacity = 8;

    [[nodiscard]] bool remember(std::int64_t frame_pts,
                                std::uint32_t padding_samples) noexcept {
        if (padding_samples == 0) return true;
        for (const auto& marker : markers_) {
            if (marker.active && marker.frame_pts == frame_pts) return false;
        }
        for (auto& marker : markers_) {
            if (marker.active) continue;
            marker = Marker{frame_pts, padding_samples, true};
            ++size_;
            return true;
        }
        return false;
    }

    [[nodiscard]] std::uint32_t take(std::int64_t packet_pts) noexcept {
        for (auto& marker : markers_) {
            if (!marker.active || marker.frame_pts != packet_pts) continue;
            const auto samples = marker.padding_samples;
            marker = {};
            --size_;
            return samples;
        }
        return 0;
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }

private:
    struct Marker {
        std::int64_t frame_pts{};
        std::uint32_t padding_samples{};
        bool active{};
    };

    std::array<Marker, capacity> markers_{};
    std::size_t size_{};
};

} // namespace fluxrec::audio::detail
