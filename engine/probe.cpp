#include "capture/capture.hpp"

#include <array>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>

namespace {

std::string fourcc_name(std::uint32_t code) {
    std::array<char, 5> characters{
        static_cast<char>(code & 0xff),
        static_cast<char>((code >> 8) & 0xff),
        static_cast<char>((code >> 16) & 0xff),
        static_cast<char>((code >> 24) & 0xff),
        '\0',
    };
    return characters.data();
}

} // namespace

int main(int argc, char** argv) {
    fluxrec::capture::CaptureConfig config;
    if (argc == 3 && std::string_view(argv[1]) == "--output") {
        config.output_name = argv[2];
    } else if (argc != 1) {
        std::cerr << "usage: fluxrec-probe [--output OUTPUT]\n";
        return 2;
    }

    auto connected = fluxrec::capture::WaylandCapture::connect(std::move(config));
    if (!connected) {
        std::cerr << "capture negotiation failed: " << connected.error().message << '\n';
        return 1;
    }

    const auto& requirements = connected->requirements();
    std::cout << "output: " << (argc == 3 ? argv[2] : "DP-1") << '\n';
    std::cout << "capture size: " << requirements.width << 'x' << requirements.height << '\n';
    if (requirements.dmabuf_device) {
        std::cout << "capture device dev_t: " << *requirements.dmabuf_device << '\n';
    } else {
        std::cout << "capture device dev_t: unspecified\n";
    }
    std::cout << "DMA-BUF format/modifier pairs: "
              << requirements.dmabuf_formats.size() << '\n';
    for (const auto& pair : requirements.dmabuf_formats) {
        std::cout << "  " << fourcc_name(pair.fourcc) << " modifier 0x"
                  << std::hex << std::setw(16) << std::setfill('0') << pair.modifier
                  << std::dec << std::setfill(' ') << '\n';
    }

    if (requirements.dmabuf_formats.empty()) {
        std::cerr << "capture session offered no DMA-BUF format; refusing CPU fallback\n";
        return 1;
    }
    return 0;
}

