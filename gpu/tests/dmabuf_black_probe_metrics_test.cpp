#include "gpu/dmabuf_black_probe_metrics.hpp"

#include <cstdint>
#include <cstdio>

namespace {

bool check(bool condition, const char* message) {
    if (!condition) std::fprintf(stderr, "FAIL: %s\n", message);
    return condition;
}

} // namespace

int main() {
    using fluxrec::gpu::detail::DmabufBlackIntervalTracker;
    using fluxrec::gpu::detail::is_near_black_frame;

    bool ok = true;
    ok &= check(is_near_black_frame(98U, 100U), "98% luma-black pixels are near-black");
    ok &= check(!is_near_black_frame(97U, 100U), "97% luma-black pixels are not near-black");
    ok &= check(!is_near_black_frame(0U, 0U), "empty frames are rejected");
    ok &= check(!is_near_black_frame(101U, 100U), "invalid aggregate counts are rejected");

    DmabufBlackIntervalTracker tracker;
    ok &= check(!tracker.observe(1U, 100U, 100U, 100U), "first black frame opens interval");
    ok &= check(!tracker.observe(2U, 200U, 99U, 100U), "contiguous black frame extends interval");
    const auto closed = tracker.observe(3U, 300U, 0U, 100U);
    ok &= check(closed.has_value(), "good successor closes interval");
    if (closed) {
        ok &= check(closed->first_sequence == 1U && closed->last_sequence == 2U,
                    "closed interval sequence bounds");
        ok &= check(closed->last_presentation_time_ns == 200U,
                    "interval ends at final black presentation time");
        ok &= check(closed->black_frames == 2U && !closed->closed_at_end,
                    "good successor closes a complete interval");
    }
    ok &= check(!tracker.finish(), "closed interval does not reappear at finish");

    ok &= check(!tracker.observe(10U, 1000U, 100U, 100U), "new black interval opens");
    const auto gap = tracker.observe(12U, 1200U, 100U, 100U);
    ok &= check(gap.has_value(), "sequence gap emits the previous interval");
    if (gap) {
        ok &= check(gap->first_sequence == 10U && gap->last_sequence == 10U &&
                    gap->closed_at_end, "sequence gap marks interval as truncated");
    }
    const auto final = tracker.finish();
    ok &= check(final.has_value(), "finish emits the active interval");
    if (final) {
        ok &= check(final->first_sequence == 12U && final->last_sequence == 12U &&
                    final->closed_at_end, "finish marks interval as truncated");
    }

    return ok ? 0 : 1;
}
