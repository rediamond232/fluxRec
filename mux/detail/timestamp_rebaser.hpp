#pragma once

extern "C" {
#include <libavutil/avutil.h>
#include <libavcodec/packet.h>
}

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

namespace fluxrec::mux::detail {

// Matroska's muxer negotiates its own stream timebase (usually 1/1000). Apply
// this only after rescaling each packet into AVStream::time_base.
class TimestampRebaser final {
public:
    explicit TimestampRebaser(bool rebase_origin = true) noexcept
        : rebase_origin_(rebase_origin) {}

    void apply(AVPacket* packet) {
        if (packet == nullptr) {
            throw std::invalid_argument("cannot rebase a null packet");
        }

        if (rebase_origin_ && !origin_set_) {
            const auto first_timestamp = packet->dts != AV_NOPTS_VALUE ? packet->dts : packet->pts;
            if (first_timestamp != AV_NOPTS_VALUE) {
                origin_ = first_timestamp;
                origin_set_ = true;
            }
        }
        if (rebase_origin_ && origin_set_) {
            if (packet->pts != AV_NOPTS_VALUE) {
                packet->pts = checked_subtract(packet->pts, origin_, "rebasing packet PTS");
            }
            if (packet->dts != AV_NOPTS_VALUE) {
                packet->dts = checked_subtract(packet->dts, origin_, "rebasing packet DTS");
            }
        }

        if (packet->dts == AV_NOPTS_VALUE) {
            return;
        }
        if (last_dts_set_ && packet->dts <= last_dts_) {
            if (last_dts_ == std::numeric_limits<std::int64_t>::max()) {
                throw std::runtime_error("DTS sequence exhausted the timestamp range");
            }
            const auto corrected_dts = last_dts_ + 1;
            const auto correction = checked_subtract(corrected_dts, packet->dts,
                                                     "correcting packet DTS");
            packet->dts = corrected_dts;
            if (packet->pts != AV_NOPTS_VALUE) {
                packet->pts = checked_add(packet->pts, correction,
                                          "preserving the PTS-DTS offset");
            }
        }
        last_dts_ = packet->dts;
        last_dts_set_ = true;
    }

private:
    [[nodiscard]] static std::int64_t checked_subtract(std::int64_t left, std::int64_t right,
                                                        const char* operation) {
        std::int64_t result = 0;
        if (__builtin_sub_overflow(left, right, &result) || result == AV_NOPTS_VALUE) {
            throw std::runtime_error(std::string(operation) + " overflowed the timestamp range");
        }
        return result;
    }

    [[nodiscard]] static std::int64_t checked_add(std::int64_t left, std::int64_t right,
                                                  const char* operation) {
        std::int64_t result = 0;
        if (__builtin_add_overflow(left, right, &result) || result == AV_NOPTS_VALUE) {
            throw std::runtime_error(std::string(operation) + " overflowed the timestamp range");
        }
        return result;
    }

    bool rebase_origin_{true};
    bool origin_set_{false};
    std::int64_t origin_{0};
    bool last_dts_set_{false};
    std::int64_t last_dts_{0};
};

} // namespace fluxrec::mux::detail
