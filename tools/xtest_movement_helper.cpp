#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/Xutil.h>
#include <X11/extensions/XTest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <charconv>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

volatile std::sig_atomic_t stop_requested = 0;

extern "C" void request_stop(int) {
    stop_requested = 1;
}

struct Options {
    int seconds{30};
    int hold_ms{200};
    int gap_ms{50};
    std::string pattern{"strafe"};
    std::string expect_class;
    std::string expect_title;
    bool dry_run{};
    bool identify{};
};

struct ClientInfo {
    Window top_level{};
    std::string wm_class;
    std::string title;
};

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool contains_case_insensitive(std::string_view value, std::string_view needle) {
    if (needle.empty()) {
        return true;
    }
    return lower(std::string(value)).find(lower(std::string(needle))) !=
           std::string::npos;
}

bool parse_int(std::string_view value, int& result) {
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size();
}

void print_usage(std::ostream& out) {
    out << "Usage:\n"
        << "  xtest-movement-helper --identify\n"
        << "  xtest-movement-helper --dry-run [--seconds 30] [--pattern strafe|forward-strafe]\n"
        << "  xtest-movement-helper --expect-class TEXT --expect-title TEXT "
           "[--seconds 30] [--pattern strafe|forward-strafe] "
           "[--hold-ms 200] [--gap-ms 50]\n\n"
        << "Live mode only sends short WASD taps to the currently focused X11/XWayland "
           "window, after matching both its WM_CLASS and title. Duration is capped at 30s.\n";
}

std::optional<Options> parse_options(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--help" || arg == "-h") {
            print_usage(std::cout);
            std::exit(0);
        }
        if (arg == "--dry-run") {
            options.dry_run = true;
            continue;
        }
        if (arg == "--identify") {
            options.identify = true;
            continue;
        }
        if (i + 1 >= argc) {
            std::cerr << "missing value for " << arg << '\n';
            return std::nullopt;
        }
        const std::string_view value(argv[++i]);
        if (arg == "--seconds") {
            if (!parse_int(value, options.seconds)) {
                std::cerr << "invalid --seconds value\n";
                return std::nullopt;
            }
        } else if (arg == "--hold-ms") {
            if (!parse_int(value, options.hold_ms)) {
                std::cerr << "invalid --hold-ms value\n";
                return std::nullopt;
            }
        } else if (arg == "--gap-ms") {
            if (!parse_int(value, options.gap_ms)) {
                std::cerr << "invalid --gap-ms value\n";
                return std::nullopt;
            }
        } else if (arg == "--pattern") {
            options.pattern = value;
        } else if (arg == "--expect-class") {
            options.expect_class = value;
        } else if (arg == "--expect-title") {
            options.expect_title = value;
        } else {
            std::cerr << "unknown option: " << arg << '\n';
            return std::nullopt;
        }
    }

    if (options.seconds < 1 || options.seconds > 30) {
        std::cerr << "--seconds must be from 1 to 30\n";
        return std::nullopt;
    }
    if (options.hold_ms < 50 || options.hold_ms > 300) {
        std::cerr << "--hold-ms must be from 50 to 300\n";
        return std::nullopt;
    }
    if (options.gap_ms < 0 || options.gap_ms > 500) {
        std::cerr << "--gap-ms must be from 0 to 500\n";
        return std::nullopt;
    }
    if (options.pattern != "strafe" && options.pattern != "forward-strafe") {
        std::cerr << "--pattern must be strafe or forward-strafe\n";
        return std::nullopt;
    }
    if (options.identify && (options.dry_run || argc != 2)) {
        std::cerr << "--identify cannot be combined with other options\n";
        return std::nullopt;
    }
    if (options.dry_run && (!options.expect_class.empty() || !options.expect_title.empty())) {
        std::cerr << "--dry-run does not take window match options\n";
        return std::nullopt;
    }
    if (!options.dry_run && !options.identify &&
        (options.expect_class.empty() || options.expect_title.empty())) {
        std::cerr << "live mode requires both --expect-class and --expect-title\n";
        return std::nullopt;
    }
    return options;
}

std::string get_text_property(Display* display, Window window, Atom property) {
    Atom actual_type{};
    int actual_format{};
    unsigned long item_count{};
    unsigned long bytes_after{};
    unsigned char* data = nullptr;
    const int status = XGetWindowProperty(display, window, property, 0, 4096, False,
                                          AnyPropertyType, &actual_type, &actual_format,
                                          &item_count, &bytes_after, &data);
    std::string result;
    if (status == Success && data != nullptr && actual_format == 8) {
        result.assign(reinterpret_cast<const char*>(data), item_count);
    }
    if (data != nullptr) {
        XFree(data);
    }
    return result;
}

std::string class_for_window(Display* display, Window window) {
    XClassHint hint{};
    if (!XGetClassHint(display, window, &hint)) {
        return {};
    }
    std::string result;
    if (hint.res_name != nullptr) {
        result = hint.res_name;
    }
    if (hint.res_class != nullptr) {
        if (!result.empty()) {
            result += '/';
        }
        result += hint.res_class;
    }
    if (hint.res_name != nullptr) {
        XFree(hint.res_name);
    }
    if (hint.res_class != nullptr) {
        XFree(hint.res_class);
    }
    return result;
}

std::optional<ClientInfo> current_client(Display* display) {
    Window focus{};
    int revert_to{};
    XGetInputFocus(display, &focus, &revert_to);
    if (focus == None || focus == PointerRoot) {
        return std::nullopt;
    }

    const Window root = DefaultRootWindow(display);
    Window current = focus;
    ClientInfo info{};
    bool has_class = false;
    bool has_title = false;
    for (int depth = 0; depth < 32 && current != None && current != root; ++depth) {
        if (!has_class) {
            auto class_name = class_for_window(display, current);
            if (!class_name.empty()) {
                info.top_level = current;
                info.wm_class = std::move(class_name);
                has_class = true;
            }
        }
        if (!has_title) {
            auto title = get_text_property(display, current, XA_WM_NAME);
            if (!title.empty()) {
                info.title = std::move(title);
                has_title = true;
            }
        }
        if (has_class && has_title) {
            return info;
        }
        Window query_root{};
        Window parent{};
        Window* children = nullptr;
        unsigned int child_count{};
        if (!XQueryTree(display, current, &query_root, &parent, &children, &child_count)) {
            break;
        }
        if (children != nullptr) {
            XFree(children);
        }
        if (parent == current || parent == None) {
            break;
        }
        current = parent;
    }
    return has_class || has_title ? std::optional<ClientInfo>(std::move(info))
                                  : std::nullopt;
}

bool client_matches(const ClientInfo& info, const Options& options) {
    return contains_case_insensitive(info.wm_class, options.expect_class) &&
           contains_case_insensitive(info.title, options.expect_title);
}

bool focus_is_target(Display* display, Window expected_window, const Options& options) {
    const auto info = current_client(display);
    return info && info->top_level == expected_window && client_matches(*info, options);
}

std::string window_id(Window window) {
    char text[32]{};
    std::snprintf(text, sizeof(text), "0x%lx", static_cast<unsigned long>(window));
    return text;
}

class KeyReleaser {
public:
    explicit KeyReleaser(Display* display) : display_(display) {}
    ~KeyReleaser() { release_all(); }

    bool press(KeyCode code) {
        if (code == 0 || XTestFakeKeyEvent(display_, code, True, CurrentTime) == 0) {
            return false;
        }
        held_[code] = true;
        XFlush(display_);
        return true;
    }

    void release(KeyCode code) {
        if (code != 0 && held_[code]) {
            XTestFakeKeyEvent(display_, code, False, CurrentTime);
            held_[code] = false;
            XFlush(display_);
        }
    }

    void release_all() {
        for (std::size_t i = 0; i < held_.size(); ++i) {
            if (held_[i]) {
                XTestFakeKeyEvent(display_, static_cast<KeyCode>(i), False, CurrentTime);
                held_[i] = false;
            }
        }
        XFlush(display_);
    }

private:
    Display* display_{};
    std::array<bool, 256> held_{};
};

std::vector<std::vector<KeySym>> make_pattern(const std::string& pattern) {
    if (pattern == "strafe") {
        return {{XK_a}, {XK_d}};
    }
    return {{XK_w, XK_a}, {XK_w, XK_d}};
}

bool send_segment(Display* display, KeyReleaser& keys, Window target,
                  const Options& options, const std::vector<KeyCode>& codes,
                  std::chrono::steady_clock::time_point end_time) {
    for (const auto code : codes) {
        if (!focus_is_target(display, target, options)) {
            std::cerr << "focus changed before key press; aborting\n";
            return false;
        }
        if (!keys.press(code)) {
            std::cerr << "XTestFakeKeyEvent press failed\n";
            return false;
        }
    }
    XSync(display, False);

    const auto hold_end = std::min(end_time,
        std::chrono::steady_clock::now() + std::chrono::milliseconds(options.hold_ms));
    while (!stop_requested && std::chrono::steady_clock::now() < hold_end) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (!focus_is_target(display, target, options)) {
            std::cerr << "focus changed during key hold; releasing and aborting\n";
            keys.release_all();
            XSync(display, False);
            return false;
        }
    }
    keys.release_all();
    XSync(display, False);
    return true;
}

int run_identify() {
    Display* display = XOpenDisplay(nullptr);
    if (display == nullptr) {
        std::cerr << "cannot open DISPLAY; Minecraft must be an X11/XWayland window\n";
        return 2;
    }
    const auto info = current_client(display);
    if (!info) {
        std::cerr << "no focused X11 client could be identified\n";
        XCloseDisplay(display);
        return 2;
    }
    std::cout << "focused_window=" << window_id(info->top_level)
              << " wm_class=\"" << info->wm_class << "\""
              << " title=\"" << info->title << "\"\n";
    XCloseDisplay(display);
    return 0;
}

int run_live(const Options& options) {
    Display* display = XOpenDisplay(nullptr);
    if (display == nullptr) {
        std::cerr << "cannot open DISPLAY; refusing input injection\n";
        return 2;
    }
    int xtest_event_base{};
    int xtest_error_base{};
    int xtest_major{};
    int xtest_minor{};
    if (!XTestQueryExtension(display, &xtest_event_base, &xtest_error_base,
                             &xtest_major, &xtest_minor)) {
        std::cerr << "XTest extension unavailable; refusing input injection\n";
        XCloseDisplay(display);
        return 2;
    }
    const auto info = current_client(display);
    if (!info || !client_matches(*info, options)) {
        std::cerr << "focused X11 window does not match required class/title; refusing input. ";
        if (info) {
            std::cerr << "focused wm_class=\"" << info->wm_class
                      << "\" title=\"" << info->title << "\"\n";
        } else {
            std::cerr << "No client identity found. Run --identify while Minecraft is focused.\n";
        }
        XCloseDisplay(display);
        return 2;
    }
    const Window target = info->top_level;
    std::cout << "target=" << window_id(target) << " wm_class=\"" << info->wm_class
              << "\" title=\"" << info->title << "\" pattern=" << options.pattern
              << " seconds=" << options.seconds << " hold_ms=" << options.hold_ms << '\n';
    std::cout.flush();

    struct sigaction action{};
    action.sa_handler = request_stop;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);

    std::array<KeyCode, 4> keycodes{};
    keycodes[0] = XKeysymToKeycode(display, XK_w);
    keycodes[1] = XKeysymToKeycode(display, XK_a);
    keycodes[2] = XKeysymToKeycode(display, XK_s);
    keycodes[3] = XKeysymToKeycode(display, XK_d);
    if (std::any_of(keycodes.begin(), keycodes.end(), [](KeyCode code) { return code == 0; })) {
        std::cerr << "could not map WASD keycodes; refusing input injection\n";
        XCloseDisplay(display);
        return 2;
    }

    std::vector<std::vector<KeyCode>> pattern;
    for (const auto& segment : make_pattern(options.pattern)) {
        std::vector<KeyCode> codes;
        for (const auto symbol : segment) {
            codes.push_back(XKeysymToKeycode(display, symbol));
        }
        pattern.push_back(std::move(codes));
    }

    std::uint64_t completed_segments = 0;
    bool completed = true;
    {
        KeyReleaser keys(display);
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(options.seconds);
        std::size_t index = 0;
        while (!stop_requested && std::chrono::steady_clock::now() < deadline) {
            if (!focus_is_target(display, target, options)) {
                std::cerr << "focus changed between segments; aborting\n";
                completed = false;
                break;
            }
            if (!send_segment(display, keys, target, options, pattern[index], deadline)) {
                completed = false;
                break;
            }
            ++completed_segments;
            index = (index + 1) % pattern.size();
            if (options.gap_ms > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(options.gap_ms));
            }
        }
        keys.release_all();
        XSync(display, False);
        if (stop_requested) {
            std::cout << "status=interrupted segments_completed=" << completed_segments << '\n';
        } else if (completed) {
            std::cout << "status=completed segments_completed=" << completed_segments << '\n';
        } else {
            std::cout << "status=aborted segments_completed=" << completed_segments << '\n';
        }
    }
    XCloseDisplay(display);
    if (stop_requested) {
        return 130;
    }
    return completed ? 0 : 3;
}

} // namespace

int main(int argc, char** argv) {
    const auto options = parse_options(argc, argv);
    if (!options) {
        print_usage(std::cerr);
        return 2;
    }
    if (options->identify) {
        return run_identify();
    }
    if (options->dry_run) {
        std::cout << "DRY RUN: no X display connection and no input events.\n"
                  << "pattern=" << options->pattern
                  << " seconds=" << options->seconds
                  << " hold_ms=" << options->hold_ms
                  << " gap_ms=" << options->gap_ms << '\n';
        if (options->pattern == "strafe") {
            std::cout << "sequence: A for hold_ms, release, gap; D for hold_ms, release, gap; repeat\n";
        } else {
            std::cout << "sequence: W+A for hold_ms, release, gap; W+D for hold_ms, release, gap; repeat\n";
        }
        return 0;
    }
    return run_live(*options);
}
