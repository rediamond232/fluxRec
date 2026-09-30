#include "capture/capture.hpp"

#include <gbm.h>
#include <drm_fourcc.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <vector>

namespace {

using fluxrec::capture::UniqueFd;
using fluxrec::capture::DmabufImage;
using fluxrec::capture::DmabufPlane;
using fluxrec::capture::CaptureRequirements;

struct Slot {
    std::unique_ptr<gbm_bo, decltype(&gbm_bo_destroy)> bo{nullptr, gbm_bo_destroy};
    std::vector<UniqueFd> plane_fds;
    DmabufImage image;
};

std::optional<std::string> device_path(std::uint64_t expected_device) {
    for (const auto& entry : std::filesystem::directory_iterator("/dev/dri")) {
        struct stat metadata {};
        if (::stat(entry.path().c_str(), &metadata) == 0 &&
            S_ISCHR(metadata.st_mode) &&
            static_cast<std::uint64_t>(metadata.st_rdev) == expected_device) {
            return entry.path().string();
        }
    }
    return std::nullopt;
}

std::optional<Slot> make_slot(gbm_device* device, const CaptureRequirements& requirements,
                              std::uint32_t fourcc, std::uint64_t modifier) {
    Slot slot;
    gbm_bo* raw = gbm_bo_create_with_modifiers2(
        device, requirements.width, requirements.height, fourcc, &modifier, 1,
        GBM_BO_USE_RENDERING);
    if (!raw) {
        return std::nullopt;
    }
    slot.bo.reset(raw);
    if (gbm_bo_get_modifier(raw) != modifier) {
        return std::nullopt;
    }
    const int count = gbm_bo_get_plane_count(raw);
    if (count < 1 || count > 4) {
        return std::nullopt;
    }
    slot.image.width = requirements.width;
    slot.image.height = requirements.height;
    slot.image.fourcc = fourcc;
    slot.image.modifier = modifier;
    slot.plane_fds.reserve(static_cast<std::size_t>(count));
    slot.image.planes.reserve(static_cast<std::size_t>(count));
    for (int plane = 0; plane < count; ++plane) {
        UniqueFd fd{gbm_bo_get_fd_for_plane(raw, plane)};
        if (!fd) {
            return std::nullopt;
        }
        slot.image.planes.push_back(DmabufPlane{
            fd.get(), gbm_bo_get_offset(raw, plane),
            gbm_bo_get_stride_for_plane(raw, plane)});
        slot.plane_fds.push_back(std::move(fd));
    }
    return slot;
}

} // namespace

int main(int argc, char** argv) {
    std::string output = "DP-1";
    int seconds = 5;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--output" && i + 1 < argc) {
            output = argv[++i];
        } else if (arg == "--seconds" && i + 1 < argc) {
            seconds = std::stoi(argv[++i]);
        } else {
            std::cerr << "usage: fluxrec-capture-smoke [--output NAME] [--seconds N]\n";
            return 2;
        }
    }
    if (seconds < 1 || seconds > 300) {
        std::cerr << "seconds must be between 1 and 300\n";
        return 2;
    }

    auto connected = fluxrec::capture::WaylandCapture::connect({.output_name = output});
    if (!connected) {
        std::cerr << "capture connect: " << connected.error().message << '\n';
        return 1;
    }
    auto capture = std::move(*connected);
    const auto& requirements = capture.requirements();
    if (!requirements.dmabuf_device) {
        std::cerr << "capture did not identify a DMA-BUF device\n";
        return 1;
    }
    auto path = device_path(*requirements.dmabuf_device);
    if (!path) {
        std::cerr << "capture DMA-BUF device is not in /dev/dri\n";
        return 1;
    }
    UniqueFd drm_fd{::open(path->c_str(), O_RDWR | O_CLOEXEC)};
    if (!drm_fd) {
        std::cerr << "cannot open " << *path << '\n';
        return 1;
    }
    std::unique_ptr<gbm_device, decltype(&gbm_device_destroy)> gbm{
        gbm_create_device(drm_fd.get()), gbm_device_destroy};
    if (!gbm) {
        std::cerr << "cannot create GBM device for " << *path << '\n';
        return 1;
    }

    std::vector<Slot> slots;
    for (const auto& pair : requirements.dmabuf_formats) {
        if (pair.fourcc != DRM_FORMAT_XRGB8888 && pair.fourcc != DRM_FORMAT_ARGB8888) {
            continue;
        }
        auto first = make_slot(gbm.get(), requirements, pair.fourcc, pair.modifier);
        if (!first) {
            continue;
        }
        slots.push_back(std::move(*first));
        for (int i = 1; i < 8; ++i) {
            auto slot = make_slot(gbm.get(), requirements, pair.fourcc, pair.modifier);
            if (!slot) {
                break;
            }
            slots.push_back(std::move(*slot));
        }
        if (slots.size() == 8) {
            break;
        }
        slots.clear();
    }
    if (slots.empty()) {
        std::cerr << "no capture format/modifier could be allocated as a GBM DMA-BUF\n";
        return 1;
    }

    std::vector<DmabufImage> images;
    images.reserve(slots.size());
    for (const auto& slot : slots) {
        images.push_back(slot.image);
    }
    if (auto result = capture.set_buffer_pool(images); !result) {
        std::cerr << "buffer import: " << result.error().message << '\n';
        return 1;
    }
    std::cout << "using " << slots.size() << " GPU DMA-BUFs on " << *path
              << " format 0x" << std::hex << slots[0].image.fourcc
              << " modifier 0x" << slots[0].image.modifier << std::dec << '\n';
    if (auto result = capture.start(); !result) {
        std::cerr << "capture start: " << result.error().message << '\n';
        return 1;
    }

    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + std::chrono::seconds(seconds);
    std::uint64_t received = 0;
    std::uint64_t distinct_presentation_times = 0;
    std::optional<std::uint64_t> previous_presentation_time;
    std::vector<std::uint64_t> presentation_intervals_ns;
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto frame = capture.wait_pop_frame(std::chrono::milliseconds(100))) {
            ++received;
            if (frame->has_presentation_time &&
                (!previous_presentation_time ||
                 frame->presentation_time_ns != *previous_presentation_time)) {
                ++distinct_presentation_times;
            }
            if (frame->has_presentation_time) {
                if (previous_presentation_time &&
                    frame->presentation_time_ns > *previous_presentation_time) {
                    presentation_intervals_ns.push_back(
                        frame->presentation_time_ns - *previous_presentation_time);
                }
                previous_presentation_time = frame->presentation_time_ns;
            }
            frame->lease.release_after_gpu(); // No GPU consumer in this capture-only probe.
        }
        if (capture.status() == fluxrec::capture::CaptureStatus::failed) {
            break;
        }
    }
    capture.stop();
    const auto capture_stats = capture.stats();
    const double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started).count();
    std::cout << "received=" << received
              << " distinct_presentation_times=" << distinct_presentation_times
              << " capture_completed=" << capture.captured_frames()
              << " dropped=" << capture.dropped_frames()
              << " failed=" << capture.capture_failures()
              << " elapsed_s=" << std::fixed << std::setprecision(3) << elapsed
              << " received_fps=" << (received / elapsed) << '\n';
    if (!presentation_intervals_ns.empty()) {
        std::sort(presentation_intervals_ns.begin(), presentation_intervals_ns.end());
        const auto percentile_ms = [&](double fraction) {
            const std::size_t index = static_cast<std::size_t>(
                (presentation_intervals_ns.size() - 1) * fraction);
            return presentation_intervals_ns[index] / 1'000'000.0;
        };
        std::uint64_t sum = 0;
        for (auto interval : presentation_intervals_ns) {
            sum += interval;
        }
        std::cout << "presentation_interval_ms avg="
                  << (static_cast<double>(sum) / presentation_intervals_ns.size() / 1'000'000.0)
                  << " p50=" << percentile_ms(0.50)
                  << " p95=" << percentile_ms(0.95)
                  << " p99=" << percentile_ms(0.99)
                  << " max=" << percentile_ms(1.0) << '\n';
    }
    if (capture_stats.request_to_ready_samples > 0) {
        std::cout << "request_to_ready_ms avg="
                  << (capture_stats.request_to_ready_average_ns / 1'000'000.0)
                  << " recent_p50=" << (capture_stats.request_to_ready_p50_ns / 1'000'000.0)
                  << " recent_p95=" << (capture_stats.request_to_ready_p95_ns / 1'000'000.0)
                  << " recent_p99=" << (capture_stats.request_to_ready_p99_ns / 1'000'000.0)
                  << " max=" << (capture_stats.request_to_ready_max_ns / 1'000'000.0)
                  << " samples=" << capture_stats.request_to_ready_samples << '\n';
    }
    if (auto error = capture.runtime_error()) {
        std::cerr << "capture runtime error: " << error->message << '\n';
        return 1;
    }
    return received > 0 ? 0 : 1;
}
