#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <wayland-client.h>
#include <wayland-egl.h>

#include "presentation-time-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <poll.h>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <unistd.h>
#include <unordered_set>

#include <linux/input-event-codes.h>

namespace {

using Clock = std::chrono::steady_clock;

enum class Pattern : int {
  Plasma = 0,
  Bars = 1,
  Checker = 2,
  Solid = 3,
  ColorChart = 4,
};

struct Options {
  bool fullscreen = false;
  bool help = false;
  bool lock_pattern_motion = false;
  int width = 1280;
  int height = 720;
  double duration_seconds = 0.0;
  uint64_t frame_limit = 0;
  float motion = 0.20F;
  Pattern pattern = Pattern::Plasma;
  std::string output_name;
};

struct App;

struct Output {
  App* app = nullptr;
  uint32_t global_name = 0;
  uint32_t version = 1;
  wl_output* proxy = nullptr;
  std::string name;
};

struct Feedback {
  App* app = nullptr;
  struct wp_presentation_feedback* proxy = nullptr;
  uint32_t frame_id = 0;
};

struct App {
  explicit App(Options parsed_options) : options(std::move(parsed_options)) {}

  Options options;
  wl_display* display = nullptr;
  wl_registry* registry = nullptr;
  wl_compositor* compositor = nullptr;
  xdg_wm_base* wm_base = nullptr;
  wl_surface* surface = nullptr;
  xdg_surface* xdg_surface_proxy = nullptr;
  xdg_toplevel* toplevel = nullptr;
  wl_seat* seat = nullptr;
  wl_keyboard* keyboard = nullptr;
  wp_presentation* presentation = nullptr;
  std::unordered_set<Output*> outputs;
  Output* selected_output = nullptr;

  wl_callback* frame_callback = nullptr;
  wl_egl_window* native_window = nullptr;
  EGLDisplay egl_display = EGL_NO_DISPLAY;
  EGLConfig egl_config = nullptr;
  EGLContext egl_context = EGL_NO_CONTEXT;
  EGLSurface egl_surface = EGL_NO_SURFACE;
  GLuint program = 0;
  GLint resolution_location = -1;
  GLint frame_location = -1;
  GLint pattern_location = -1;
  GLint phase_location = -1;
  GLuint vertex_array = 0;

  int width = 1280;
  int height = 720;
  bool configured = false;
  bool egl_ready = false;
  bool fullscreen = false;
  bool done = false;
  std::string error;
  uint32_t next_frame_id = 0;
  uint64_t submitted = 0;
  uint64_t presented = 0;
  uint64_t discarded = 0;
  uint64_t callbacks = 0;
  uint64_t presentation_id_gaps = 0;
  uint32_t first_presented_id = 0;
  uint32_t last_presented_id = 0;
  bool have_presented_id = false;
  bool readiness_reported = false;
  uint64_t last_refresh_ns = 0;
  std::unordered_set<Feedback*> outstanding_feedback;
  Clock::time_point first_submit{};
  Clock::time_point last_stats = Clock::now();
  uint64_t last_stats_submitted = 0;
  uint64_t last_stats_presented = 0;
  uint64_t last_stats_discarded = 0;
  uint64_t last_stats_callbacks = 0;
  bool started = false;
};

constexpr const char* kVertexShader = R"GLSL(#version 300 es
void main() {
  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";

constexpr const char* kFragmentShader = R"GLSL(#version 300 es
precision highp float;
precision highp int;

uniform vec2 uResolution;
uniform uint uFrame;
uniform int uPattern;
uniform float uPhase;
out vec4 outColor;

uint segmentsForDigit(uint d) {
  switch (d) {
    case 0u: return 0x3fu;
    case 1u: return 0x06u;
    case 2u: return 0x5bu;
    case 3u: return 0x4fu;
    case 4u: return 0x66u;
    case 5u: return 0x6du;
    case 6u: return 0x7du;
    case 7u: return 0x07u;
    case 8u: return 0x7fu;
    case 9u: return 0x6fu;
    default: return 0u;
  }
}

uint decimalPlace(int digit_index) {
  uint place = 1u;
  for (int i = 0; i < 9 - digit_index; ++i) {
    place *= 10u;
  }
  return place;
}

bool litSegment(uint mask, uint bit) {
  return (mask & bit) != 0u;
}

vec3 backgroundPattern(vec2 uv) {
  float phase = uPhase;
  if (uPattern == 0) {
    float a = sin(uv.x * 18.0 + uv.y * 5.0 + phase * 1.4);
    float b = sin(uv.y * 23.0 - phase * 1.7);
    float c = sin((uv.x + uv.y) * 16.0 + phase * 1.1);
    return vec3(0.5 + 0.5 * sin(a + phase),
                0.5 + 0.5 * sin(b + phase * 0.73 + 2.1),
                0.5 + 0.5 * sin(c - phase * 0.91 + 4.2));
  }
  if (uPattern == 1) {
    float wave = sin((uv.x * 24.0 + uv.y * 9.0) + phase * 2.0);
    float band = floor((wave * 0.5 + 0.5) * 8.0);
    float hue = fract(band * 0.11);
    return vec3(hue, 0.25 + 0.65 * hue, 1.0 - hue);
  }
  if (uPattern == 2) {
    vec2 cell = floor(uv * vec2(28.0, 16.0));
    float alternate = mod(cell.x + cell.y, 2.0);
    float hue = fract((cell.x * 0.17 + cell.y * 0.31) * 0.07);
    vec3 a = vec3(hue, 0.2 + 0.8 * fract(hue + 0.34), 0.95 - hue * 0.6);
    float pulse = 0.15 + 0.85 * (0.5 + 0.5 * sin(phase * 2.0 + (cell.x - cell.y) * 0.12));
    return mix(a * 0.16, a, alternate) * pulse;
  }
  if (uPattern == 4) {
    float fromTop = 1.0 - uv.y;
    float x = clamp(uv.x, 0.0, 1.0);
    if (fromTop < 0.65) {
      int bar = clamp(int(floor(x * 8.0)), 0, 7);
      if (bar == 0) return vec3(0.0);
      if (bar == 1) return vec3(1.0);
      if (bar == 2) return vec3(1.0, 0.0, 0.0);
      if (bar == 3) return vec3(0.0, 1.0, 0.0);
      if (bar == 4) return vec3(0.0, 0.0, 1.0);
      if (bar == 5) return vec3(1.0, 1.0, 0.0);
      if (bar == 6) return vec3(0.0, 1.0, 1.0);
      return vec3(1.0, 0.0, 1.0);
    }
    if (fromTop < 0.72) return vec3(x);
    if (fromTop < 0.79) return vec3(x, 0.0, 0.0);
    if (fromTop < 0.86) return vec3(0.0, x, 0.0);
    if (fromTop < 0.93) return vec3(0.0, 0.0, x);
    vec2 cell = floor(vec2(x * 16.0, (fromTop - 0.93) * (4.0 / 0.07)));
    float alternate = mod(cell.x + cell.y, 2.0);
    return vec3(mix(0.12, 0.88, alternate));
  }
  float hue = fract(phase / 6.2831853);
  return 0.5 + 0.5 * cos(6.2831853 * (hue + vec3(0.0, 0.33, 0.67)));
}

void main() {
  vec2 uv = gl_FragCoord.xy / uResolution;
  vec3 color = backgroundPattern(uv);

  // GPU-rendered frame ID overlay: seven-segment "ID" followed by ten digits.
  float fromTop = uResolution.y - gl_FragCoord.y;
  float px = gl_FragCoord.x;
  if (px < 284.0 && fromTop < 46.0) {
    color = vec3(0.012, 0.020, 0.023);
    if (px >= 8.0 && px < 281.0 && fromTop >= 8.0 && fromTop < 36.0) {
      int slot = int(floor((px - 8.0) / 21.0));
      uint mask = 0u;
      if (slot == 0) {
        mask = 0x06u; // I
      } else if (slot == 1) {
        mask = 0x5eu; // D
      } else if (slot >= 3 && slot <= 12) {
        int index = slot - 3;
        uint digit = (uFrame / decimalPlace(index)) % 10u;
        mask = segmentsForDigit(digit);
      }

      float localX = mod(px - 8.0, 21.0);
      float localY = fromTop - 8.0;
      bool a = localY >= 0.0 && localY < 3.0 && localX >= 5.0 && localX < 15.0;
      bool b = localY >= 3.0 && localY < 13.0 && localX >= 14.0 && localX < 18.0;
      bool c = localY >= 15.0 && localY < 25.0 && localX >= 14.0 && localX < 18.0;
      bool d = localY >= 25.0 && localY < 28.0 && localX >= 5.0 && localX < 15.0;
      bool e = localY >= 15.0 && localY < 25.0 && localX >= 1.0 && localX < 5.0;
      bool f = localY >= 3.0 && localY < 13.0 && localX >= 1.0 && localX < 5.0;
      bool g = localY >= 12.0 && localY < 16.0 && localX >= 5.0 && localX < 15.0;
      bool lit = (a && litSegment(mask, 0x01u)) ||
                 (b && litSegment(mask, 0x02u)) ||
                 (c && litSegment(mask, 0x04u)) ||
                 (d && litSegment(mask, 0x08u)) ||
                 (e && litSegment(mask, 0x10u)) ||
                 (f && litSegment(mask, 0x20u)) ||
                 (g && litSegment(mask, 0x40u));
      if (lit) {
        color = vec3(0.62, 1.0, 0.76);
      }
    }
  }

  outColor = vec4(color, 1.0);
}
)GLSL";

const char* pattern_name(Pattern pattern) {
  switch (pattern) {
    case Pattern::Plasma: return "plasma";
    case Pattern::Bars: return "bars";
    case Pattern::Checker: return "checker";
    case Pattern::Solid: return "solid";
    case Pattern::ColorChart: return "colorchart";
  }
  return "unknown";
}

bool parse_unsigned(std::string_view value, uint64_t& result) {
  if (value.empty()) return false;
  uint64_t parsed = 0;
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (error != std::errc{} || end != value.data() + value.size()) return false;
  result = parsed;
  return true;
}

bool parse_positive_int(std::string_view value, int& result) {
  uint64_t parsed = 0;
  if (!parse_unsigned(value, parsed) || parsed == 0 ||
      parsed > static_cast<uint64_t>(std::numeric_limits<int>::max())) {
    return false;
  }
  result = static_cast<int>(parsed);
  return true;
}

bool parse_pattern(std::string_view value, Pattern& pattern) {
  if (value == "plasma") pattern = Pattern::Plasma;
  else if (value == "bars") pattern = Pattern::Bars;
  else if (value == "checker") pattern = Pattern::Checker;
  else if (value == "solid") pattern = Pattern::Solid;
  else if (value == "colorchart") pattern = Pattern::ColorChart;
  else return false;
  return true;
}

void print_usage(std::ostream& out, const char* program) {
  out << "Usage: " << program << " [options]\n"
      << "  --windowed                 Start in a window (default)\n"
      << "  --fullscreen               Start full-screen\n"
      << "  --output NAME              Select output by name; implies --fullscreen\n"
      << "  --width PIXELS              Initial window width (default 1280)\n"
      << "  --height PIXELS             Initial window height (default 720)\n"
      << "  --pattern NAME              plasma, bars, checker, solid, or colorchart\n"
      << "  --motion VALUE              Phase change per rendered frame (default 0.20)\n"
      << "  --duration SECONDS          Stop after this duration; 0 runs until quit\n"
      << "  --frames COUNT              Stop after submitting COUNT images\n"
      << "  --lock-pattern-motion       Ignore keyboard pattern/motion changes\n"
      << "  --help                      Show this help\n\n"
      << "Keyboard: Esc/Q quit, Space cycles patterns, 1-5 select patterns,\n"
      << "+/- change motion speed, F toggles full-screen. The lock option\n"
      << "ignores pattern and motion keys while retaining the quit keys.\n";
}

bool parse_options(int argc, char** argv, Options& options, std::string& error) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    const auto argument_value = [&](std::string_view name, std::string_view& value) {
      if (i + 1 >= argc) {
        error = "missing value for " + std::string(name);
        return false;
      }
      value = argv[++i];
      return true;
    };

    std::string_view value;
    if (arg == "--help" || arg == "-h") {
      options.help = true;
    } else if (arg == "--lock-pattern-motion") {
      options.lock_pattern_motion = true;
    } else if (arg == "--fullscreen") {
      options.fullscreen = true;
    } else if (arg == "--windowed") {
      options.fullscreen = false;
    } else if (arg == "--output") {
      if (!argument_value(arg, value)) return false;
      options.output_name.assign(value);
      options.fullscreen = true;
    } else if (arg == "--width") {
      if (!argument_value(arg, value) || !parse_positive_int(value, options.width)) {
        if (error.empty()) error = "--width must be a positive integer";
        return false;
      }
    } else if (arg == "--height") {
      if (!argument_value(arg, value) || !parse_positive_int(value, options.height)) {
        if (error.empty()) error = "--height must be a positive integer";
        return false;
      }
    } else if (arg == "--frames") {
      if (!argument_value(arg, value) || !parse_unsigned(value, options.frame_limit) ||
          options.frame_limit == 0) {
        if (error.empty()) error = "--frames must be a positive integer";
        return false;
      }
    } else if (arg == "--duration") {
      if (!argument_value(arg, value)) return false;
      char* end = nullptr;
      const std::string duration(value);
      options.duration_seconds = std::strtod(duration.c_str(), &end);
      if (end == duration.c_str() || *end != '\0' || !std::isfinite(options.duration_seconds) ||
          options.duration_seconds < 0.0) {
        error = "--duration must be a non-negative number of seconds";
        return false;
      }
    } else if (arg == "--motion") {
      if (!argument_value(arg, value)) return false;
      char* end = nullptr;
      const std::string motion(value);
      options.motion = std::strtof(motion.c_str(), &end);
      if (end == motion.c_str() || *end != '\0' || !std::isfinite(options.motion) ||
          options.motion < 0.0F || options.motion > 1000.0F) {
        error = "--motion must be a finite value between 0 and 1000";
        return false;
      }
    } else if (arg == "--pattern") {
      if (!argument_value(arg, value) || !parse_pattern(value, options.pattern)) {
        if (error.empty()) error = "--pattern must be plasma, bars, checker, solid, or colorchart";
        return false;
      }
    } else {
      error = "unknown option: " + std::string(arg);
      return false;
    }
  }
  return true;
}

void set_error(App& app, std::string message) {
  if (app.error.empty()) app.error = std::move(message);
  app.done = true;
}

void output_geometry(void*, wl_output*, int32_t, int32_t, int32_t, int32_t,
                     int32_t, const char*, const char*, int32_t) {}
void output_mode(void*, wl_output*, uint32_t, int32_t, int32_t, int32_t) {}
void output_done(void*, wl_output*) {}
void output_scale(void*, wl_output*, int32_t) {}
void output_description(void*, wl_output*, const char*) {}

void output_name(void* data, wl_output*, const char* name) {
  auto* output = static_cast<Output*>(data);
  output->name = name == nullptr ? "" : name;
}

const wl_output_listener kOutputListener = {
  .geometry = output_geometry,
  .mode = output_mode,
  .done = output_done,
  .scale = output_scale,
  .name = output_name,
  .description = output_description,
};

void toplevel_configure(void* data, xdg_toplevel*, int32_t width, int32_t height,
                        wl_array*) {
  auto* app = static_cast<App*>(data);
  if (width > 0) app->width = width;
  if (height > 0) app->height = height;
  if (app->native_window != nullptr) {
    wl_egl_window_resize(app->native_window, app->width, app->height, 0, 0);
  }
}

void toplevel_close(void* data, xdg_toplevel*) {
  static_cast<App*>(data)->done = true;
}

void toplevel_configure_bounds(void*, xdg_toplevel*, int32_t, int32_t) {}
void toplevel_wm_capabilities(void*, xdg_toplevel*, wl_array*) {}

const xdg_toplevel_listener kToplevelListener = {
  .configure = toplevel_configure,
  .close = toplevel_close,
  .configure_bounds = toplevel_configure_bounds,
  .wm_capabilities = toplevel_wm_capabilities,
};

void xdg_surface_configure(void* data, xdg_surface* surface, uint32_t serial) {
  auto* app = static_cast<App*>(data);
  xdg_surface_ack_configure(surface, serial);
  app->configured = true;
}

const xdg_surface_listener kXdgSurfaceListener = {
  .configure = xdg_surface_configure,
};

void wm_base_ping(void*, xdg_wm_base* base, uint32_t serial) {
  xdg_wm_base_pong(base, serial);
}

const xdg_wm_base_listener kWmBaseListener = {
  .ping = wm_base_ping,
};

void presentation_clock_id(void*, wp_presentation*, uint32_t) {}

const wp_presentation_listener kPresentationListener = {
  .clock_id = presentation_clock_id,
};

void feedback_sync_output(void*, struct wp_presentation_feedback*, wl_output*) {}

void feedback_presented(void* data, struct wp_presentation_feedback* proxy,
                        uint32_t, uint32_t, uint32_t, uint32_t refresh,
                        uint32_t, uint32_t, uint32_t) {
  auto* info = static_cast<Feedback*>(data);
  App& app = *info->app;
  ++app.presented;
  app.last_refresh_ns = refresh;
  if (app.have_presented_id) {
    const uint32_t delta = info->frame_id - app.last_presented_id;
    if (delta != 0 && delta < 0x80000000U) {
      app.presentation_id_gaps += static_cast<uint64_t>(delta - 1U);
      app.last_presented_id = info->frame_id;
    }
  } else {
    app.have_presented_id = true;
    app.first_presented_id = info->frame_id;
    app.last_presented_id = info->frame_id;
  }
  if (!app.readiness_reported) {
    app.readiness_reported = true;
    std::cerr << "stimulus-ready: first-presented id=" << app.first_presented_id
              << " refresh-ns=" << refresh << std::endl;
  }
  app.outstanding_feedback.erase(info);
  // The protocol marks this event as a server-side destructor. Drop the local
  // proxy without sending another destroy request.
  wl_proxy_destroy(reinterpret_cast<wl_proxy*>(proxy));
  delete info;
}

void feedback_discarded(void* data, struct wp_presentation_feedback* proxy) {
  auto* info = static_cast<Feedback*>(data);
  App& app = *info->app;
  ++app.discarded;
  app.outstanding_feedback.erase(info);
  wl_proxy_destroy(reinterpret_cast<wl_proxy*>(proxy));
  delete info;
}

const wp_presentation_feedback_listener kFeedbackListener = {
  .sync_output = feedback_sync_output,
  .presented = feedback_presented,
  .discarded = feedback_discarded,
};

void keyboard_keymap(void*, wl_keyboard*, uint32_t, int32_t fd, uint32_t) {
  if (fd >= 0) close(fd);
}

void keyboard_enter(void*, wl_keyboard*, uint32_t, wl_surface*, wl_array*) {}
void keyboard_leave(void*, wl_keyboard*, uint32_t, wl_surface*) {}
void keyboard_modifiers(void*, wl_keyboard*, uint32_t, uint32_t, uint32_t, uint32_t,
                        uint32_t) {}
void keyboard_repeat_info(void*, wl_keyboard*, int32_t, int32_t) {}

void draw_frame(App& app);

void next_pattern(App& app) {
  constexpr int kPatternCount = 5;
  const int next = (static_cast<int>(app.options.pattern) + 1) % kPatternCount;
  app.options.pattern = static_cast<Pattern>(next);
  std::cerr << "pattern=" << pattern_name(app.options.pattern) << '\n';
}

void toggle_fullscreen(App& app) {
  if (app.fullscreen) {
    xdg_toplevel_unset_fullscreen(app.toplevel);
    app.fullscreen = false;
  } else {
    xdg_toplevel_set_fullscreen(app.toplevel,
      app.selected_output == nullptr ? nullptr : app.selected_output->proxy);
    app.fullscreen = true;
  }
  wl_surface_commit(app.surface);
}

void handle_key(App& app, uint32_t key) {
  switch (key) {
    case KEY_ESC:
    case KEY_Q:
      app.done = true;
      return;
    case KEY_SPACE:
      if (app.options.lock_pattern_motion) return;
      next_pattern(app);
      return;
    case KEY_1:
      if (app.options.lock_pattern_motion) return;
      app.options.pattern = Pattern::Plasma;
      break;
    case KEY_2:
      if (app.options.lock_pattern_motion) return;
      app.options.pattern = Pattern::Bars;
      break;
    case KEY_3:
      if (app.options.lock_pattern_motion) return;
      app.options.pattern = Pattern::Checker;
      break;
    case KEY_4:
      if (app.options.lock_pattern_motion) return;
      app.options.pattern = Pattern::Solid;
      break;
    case KEY_5:
      if (app.options.lock_pattern_motion) return;
      app.options.pattern = Pattern::ColorChart;
      break;
    case KEY_F:
      toggle_fullscreen(app);
      return;
    case KEY_EQUAL:
    case KEY_KPPLUS:
      if (app.options.lock_pattern_motion) return;
      app.options.motion = std::min(app.options.motion * 1.25F + 0.01F, 1000.0F);
      std::cerr << "motion=" << app.options.motion << '\n';
      return;
    case KEY_MINUS:
    case KEY_KPMINUS:
      if (app.options.lock_pattern_motion) return;
      app.options.motion = std::max(app.options.motion / 1.25F - 0.01F, 0.0F);
      std::cerr << "motion=" << app.options.motion << '\n';
      return;
    default:
      return;
  }
  std::cerr << "pattern=" << pattern_name(app.options.pattern) << '\n';
}

void keyboard_key(void* data, wl_keyboard*, uint32_t, uint32_t, uint32_t key,
                  uint32_t state) {
  auto* app = static_cast<App*>(data);
  if (state == WL_KEYBOARD_KEY_STATE_PRESSED) handle_key(*app, key);
}

const wl_keyboard_listener kKeyboardListener = {
  .keymap = keyboard_keymap,
  .enter = keyboard_enter,
  .leave = keyboard_leave,
  .key = keyboard_key,
  .modifiers = keyboard_modifiers,
  .repeat_info = keyboard_repeat_info,
};

void seat_capabilities(void* data, wl_seat* seat, uint32_t capabilities) {
  auto* app = static_cast<App*>(data);
  const bool has_keyboard = (capabilities & WL_SEAT_CAPABILITY_KEYBOARD) != 0;
  if (has_keyboard && app->keyboard == nullptr) {
    app->keyboard = wl_seat_get_keyboard(seat);
    if (app->keyboard == nullptr ||
        wl_keyboard_add_listener(app->keyboard, &kKeyboardListener, app) != 0) {
      set_error(*app, "failed to initialize the Wayland keyboard listener");
    }
  } else if (!has_keyboard && app->keyboard != nullptr) {
    if (wl_keyboard_get_version(app->keyboard) >= WL_KEYBOARD_RELEASE_SINCE_VERSION) {
      wl_keyboard_release(app->keyboard);
    } else {
      wl_keyboard_destroy(app->keyboard);
    }
    app->keyboard = nullptr;
  }
}

void seat_name(void*, wl_seat*, const char*) {}

const wl_seat_listener kSeatListener = {
  .capabilities = seat_capabilities,
  .name = seat_name,
};

void registry_global(void* data, wl_registry* registry, uint32_t name,
                     const char* interface, uint32_t version) {
  auto* app = static_cast<App*>(data);
  if (std::strcmp(interface, wl_output_interface.name) == 0) {
    auto output = std::make_unique<Output>();
    output->app = app;
    output->global_name = name;
    output->version = std::min(version, 4U);
    output->proxy = static_cast<wl_output*>(wl_registry_bind(
      registry, name, &wl_output_interface, output->version));
    if (output->proxy == nullptr ||
        wl_output_add_listener(output->proxy, &kOutputListener, output.get()) != 0) {
      if (output->proxy != nullptr) wl_output_destroy(output->proxy);
      set_error(*app, "failed to bind a Wayland output");
      return;
    }
    app->outputs.insert(output.release());
  } else if (std::strcmp(interface, xdg_wm_base_interface.name) == 0) {
    const uint32_t bind_version = std::min(version, 6U);
    app->wm_base = static_cast<xdg_wm_base*>(wl_registry_bind(
      registry, name, &xdg_wm_base_interface, bind_version));
    if (app->wm_base == nullptr ||
        xdg_wm_base_add_listener(app->wm_base, &kWmBaseListener, app) != 0) {
      set_error(*app, "failed to bind xdg-shell");
    }
  } else if (std::strcmp(interface, wl_seat_interface.name) == 0 && app->seat == nullptr) {
    const uint32_t bind_version = std::min(version, 7U);
    app->seat = static_cast<wl_seat*>(wl_registry_bind(
      registry, name, &wl_seat_interface, bind_version));
    if (app->seat == nullptr || wl_seat_add_listener(app->seat, &kSeatListener, app) != 0) {
      set_error(*app, "failed to bind the Wayland seat");
    }
  } else if (std::strcmp(interface, wl_compositor_interface.name) == 0 &&
             app->compositor == nullptr) {
    const uint32_t bind_version = std::min(version, 6U);
    app->compositor = static_cast<wl_compositor*>(wl_registry_bind(
      registry, name, &wl_compositor_interface, bind_version));
    if (app->compositor == nullptr) {
      set_error(*app, "failed to bind the Wayland compositor");
    }
  } else if (std::strcmp(interface, wp_presentation_interface.name) == 0) {
    const uint32_t bind_version = std::min(version, 2U);
    app->presentation = static_cast<wp_presentation*>(wl_registry_bind(
      registry, name, &wp_presentation_interface, bind_version));
    if (app->presentation == nullptr ||
        wp_presentation_add_listener(app->presentation, &kPresentationListener, app) != 0) {
      set_error(*app, "failed to bind presentation-time feedback");
    }
  }
}

void registry_global_remove(void* data, wl_registry*, uint32_t name) {
  auto* app = static_cast<App*>(data);
  for (auto it = app->outputs.begin(); it != app->outputs.end(); ++it) {
    Output* output = *it;
    if (output->global_name == name) {
      if (app->selected_output == output) app->selected_output = nullptr;
      if (wl_output_get_version(output->proxy) >= WL_OUTPUT_RELEASE_SINCE_VERSION) {
        wl_output_release(output->proxy);
      } else {
        wl_output_destroy(output->proxy);
      }
      delete output;
      app->outputs.erase(it);
      break;
    }
  }
}

const wl_registry_listener kRegistryListener = {
  .global = registry_global,
  .global_remove = registry_global_remove,
};

GLuint compile_shader(GLenum type, const char* source, std::string& error) {
  const GLuint shader = glCreateShader(type);
  glShaderSource(shader, 1, &source, nullptr);
  glCompileShader(shader);

  GLint compiled = GL_FALSE;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
  if (compiled == GL_TRUE) return shader;

  GLint log_length = 0;
  glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_length);
  std::string log(static_cast<size_t>(std::max(log_length, 1)), '\0');
  glGetShaderInfoLog(shader, log_length, nullptr, log.data());
  error = "GLES shader compilation failed: " + log;
  glDeleteShader(shader);
  return 0;
}

bool initialize_gl(App& app) {
  std::string shader_error;
  const GLuint vertex = compile_shader(GL_VERTEX_SHADER, kVertexShader, shader_error);
  if (vertex == 0) {
    app.error = shader_error;
    return false;
  }
  const GLuint fragment = compile_shader(GL_FRAGMENT_SHADER, kFragmentShader, shader_error);
  if (fragment == 0) {
    glDeleteShader(vertex);
    app.error = shader_error;
    return false;
  }

  app.program = glCreateProgram();
  glAttachShader(app.program, vertex);
  glAttachShader(app.program, fragment);
  glLinkProgram(app.program);
  glDeleteShader(vertex);
  glDeleteShader(fragment);

  GLint linked = GL_FALSE;
  glGetProgramiv(app.program, GL_LINK_STATUS, &linked);
  if (linked != GL_TRUE) {
    GLint log_length = 0;
    glGetProgramiv(app.program, GL_INFO_LOG_LENGTH, &log_length);
    std::string log(static_cast<size_t>(std::max(log_length, 1)), '\0');
    glGetProgramInfoLog(app.program, log_length, nullptr, log.data());
    app.error = "GLES program link failed: " + log;
    return false;
  }

  app.resolution_location = glGetUniformLocation(app.program, "uResolution");
  app.frame_location = glGetUniformLocation(app.program, "uFrame");
  app.pattern_location = glGetUniformLocation(app.program, "uPattern");
  app.phase_location = glGetUniformLocation(app.program, "uPhase");
  if (app.resolution_location < 0 || app.frame_location < 0 ||
      app.pattern_location < 0 || app.phase_location < 0) {
    app.error = "GLES shader uniforms were optimized out or unavailable";
    return false;
  }

  glDisable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);
  glDisable(GL_CULL_FACE);
  glGenVertexArrays(1, &app.vertex_array);
  glBindVertexArray(app.vertex_array);
  return true;
}

bool initialize_egl(App& app) {
  app.egl_display = eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR, app.display, nullptr);
  if (app.egl_display == EGL_NO_DISPLAY) {
    app.error = "eglGetDisplay failed for the Wayland display";
    return false;
  }
  EGLint egl_major = 0;
  EGLint egl_minor = 0;
  if (eglInitialize(app.egl_display, &egl_major, &egl_minor) != EGL_TRUE) {
    app.error = "eglInitialize failed";
    return false;
  }
  if (eglBindAPI(EGL_OPENGL_ES_API) != EGL_TRUE) {
    app.error = "eglBindAPI(EGL_OPENGL_ES_API) failed";
    return false;
  }

  const EGLint config_attributes[] = {
    EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
    EGL_RED_SIZE, 8,
    EGL_GREEN_SIZE, 8,
    EGL_BLUE_SIZE, 8,
    EGL_ALPHA_SIZE, 0,
    EGL_NONE,
  };
  EGLint config_count = 0;
  if (eglChooseConfig(app.egl_display, config_attributes, &app.egl_config,
                      1, &config_count) != EGL_TRUE || config_count == 0) {
    app.error = "no EGL window config supporting GLES 3 was found";
    return false;
  }

  const EGLint context_attributes[] = {
    EGL_CONTEXT_CLIENT_VERSION, 3,
    EGL_NONE,
  };
  app.egl_context = eglCreateContext(app.egl_display, app.egl_config,
                                     EGL_NO_CONTEXT, context_attributes);
  if (app.egl_context == EGL_NO_CONTEXT) {
    app.error = "eglCreateContext for GLES 3 failed";
    return false;
  }

  app.native_window = wl_egl_window_create(app.surface, app.width, app.height);
  if (app.native_window == nullptr) {
    app.error = "wl_egl_window_create failed";
    return false;
  }
  app.egl_surface = eglCreateWindowSurface(
    app.egl_display, app.egl_config,
    reinterpret_cast<EGLNativeWindowType>(app.native_window), nullptr);
  if (app.egl_surface == EGL_NO_SURFACE) {
    app.error = "eglCreateWindowSurface failed";
    return false;
  }
  if (eglMakeCurrent(app.egl_display, app.egl_surface, app.egl_surface,
                     app.egl_context) != EGL_TRUE) {
    app.error = "eglMakeCurrent failed";
    return false;
  }
  app.egl_ready = true;
  // wl_surface.frame already gates submissions to compositor refreshes. An
  // additional EGL interval of 1 can add a second vblank wait after the frame
  // callback and reduce a 360 Hz surface to half rate.
  (void)eglSwapInterval(app.egl_display, 0);
  if (!initialize_gl(app)) return false;
  return true;
}

void report_statistics(App& app, Clock::time_point now) {
  const double elapsed = std::chrono::duration<double>(now - app.last_stats).count();
  if (elapsed < 1.0) return;
  const double submitted_rate = static_cast<double>(app.submitted - app.last_stats_submitted) / elapsed;
  const double presented_rate = static_cast<double>(app.presented - app.last_stats_presented) / elapsed;
  const double discarded_rate = static_cast<double>(app.discarded - app.last_stats_discarded) / elapsed;
  const double callback_rate = static_cast<double>(app.callbacks - app.last_stats_callbacks) / elapsed;

  std::cerr << "stimulus: submitted/s=" << submitted_rate
            << " presented/s=" << presented_rate
            << " frame-callbacks/s=" << callback_rate
            << " discarded/s=" << discarded_rate
            << " presented-id-gaps=" << app.presentation_id_gaps;
  if (app.last_refresh_ns != 0) {
    std::cerr << " refresh-us=" << (static_cast<double>(app.last_refresh_ns) / 1000.0);
  }
  std::cerr << " totals=" << app.submitted << '/' << app.presented << '/' << app.discarded
            << " pattern=" << pattern_name(app.options.pattern) << '\n';

  app.last_stats = now;
  app.last_stats_submitted = app.submitted;
  app.last_stats_presented = app.presented;
  app.last_stats_discarded = app.discarded;
  app.last_stats_callbacks = app.callbacks;
}

void frame_done(void* data, wl_callback* callback, uint32_t) {
  auto* app = static_cast<App*>(data);
  if (app->frame_callback == callback) app->frame_callback = nullptr;
  wl_callback_destroy(callback);
  ++app->callbacks;
  if (app->done || !app->egl_ready) return;

  const auto now = Clock::now();
  report_statistics(*app, now);
  if ((app->options.frame_limit != 0 && app->submitted >= app->options.frame_limit) ||
      (app->options.duration_seconds > 0.0 && app->started &&
       std::chrono::duration<double>(now - app->first_submit).count() >=
         app->options.duration_seconds)) {
    app->done = true;
    return;
  }
  draw_frame(*app);
}

const wl_callback_listener kFrameListener = {
  .done = frame_done,
};

void draw_frame(App& app) {
  if (app.done || !app.egl_ready || app.frame_callback != nullptr) return;

  app.frame_callback = wl_surface_frame(app.surface);
  if (app.frame_callback == nullptr ||
      wl_callback_add_listener(app.frame_callback, &kFrameListener, &app) != 0) {
    set_error(app, "failed to request a Wayland frame callback");
    return;
  }

  const uint32_t frame_id = app.next_frame_id++;
  if (app.presentation != nullptr) {
    auto* info = new Feedback{&app, nullptr, frame_id};
    info->proxy = wp_presentation_feedback(app.presentation, app.surface);
    if (info->proxy == nullptr ||
        wp_presentation_feedback_add_listener(info->proxy, &kFeedbackListener, info) != 0) {
      if (info->proxy != nullptr) {
        wp_presentation_feedback_destroy(info->proxy);
      }
      delete info;
      set_error(app, "failed to request presentation feedback");
      return;
    }
    app.outstanding_feedback.insert(info);
  }

  glViewport(0, 0, app.width, app.height);
  glUseProgram(app.program);
  glUniform2f(app.resolution_location, static_cast<float>(app.width),
              static_cast<float>(app.height));
  glUniform1ui(app.frame_location, frame_id);
  glUniform1i(app.pattern_location, static_cast<int>(app.options.pattern));
  constexpr double kTau = 6.28318530717958647692;
  const float phase = static_cast<float>(std::fmod(
    static_cast<double>(frame_id) * static_cast<double>(app.options.motion), kTau));
  glUniform1f(app.phase_location, phase);
  glDrawArrays(GL_TRIANGLES, 0, 3);

  if (eglSwapBuffers(app.egl_display, app.egl_surface) != EGL_TRUE) {
    set_error(app, "eglSwapBuffers failed");
    return;
  }
  if (!app.started) {
    app.started = true;
    app.first_submit = Clock::now();
  }
  ++app.submitted;
}

bool initialize_wayland(App& app) {
  app.display = wl_display_connect(nullptr);
  if (app.display == nullptr) {
    app.error = "could not connect to the Wayland display";
    return false;
  }
  app.registry = wl_display_get_registry(app.display);
  if (app.registry == nullptr ||
      wl_registry_add_listener(app.registry, &kRegistryListener, &app) != 0) {
    app.error = "failed to initialize the Wayland registry";
    return false;
  }
  if (wl_display_roundtrip(app.display) < 0 || wl_display_roundtrip(app.display) < 0) {
    app.error = "Wayland registry roundtrip failed";
    return false;
  }
  if (!app.error.empty()) return false;
  if (app.wm_base == nullptr) {
    app.error = "the compositor does not advertise xdg-shell";
    return false;
  }
  if (app.compositor == nullptr) {
    app.error = "the compositor does not advertise wl_compositor";
    return false;
  }

  if (!app.options.output_name.empty()) {
    for (Output* output : app.outputs) {
      if (output->name == app.options.output_name) {
        app.selected_output = output;
        break;
      }
    }
    if (app.selected_output == nullptr) {
      app.error = "Wayland output not found: " + app.options.output_name;
      return false;
    }
  }

  app.width = app.options.width;
  app.height = app.options.height;
  app.fullscreen = app.options.fullscreen;
  app.surface = wl_compositor_create_surface(app.compositor);
  if (app.surface == nullptr) {
    app.error = "wl_compositor_create_surface failed";
    return false;
  }
  app.xdg_surface_proxy = xdg_wm_base_get_xdg_surface(app.wm_base, app.surface);
  if (app.xdg_surface_proxy == nullptr ||
      xdg_surface_add_listener(app.xdg_surface_proxy, &kXdgSurfaceListener, &app) != 0) {
    app.error = "failed to create the xdg_surface";
    return false;
  }
  app.toplevel = xdg_surface_get_toplevel(app.xdg_surface_proxy);
  if (app.toplevel == nullptr ||
      xdg_toplevel_add_listener(app.toplevel, &kToplevelListener, &app) != 0) {
    app.error = "failed to create the xdg_toplevel";
    return false;
  }
  xdg_toplevel_set_title(app.toplevel, "Fluxrec Motion Stimulus");
  xdg_toplevel_set_app_id(app.toplevel, "io.fluxrec.stimulus");
  if (app.fullscreen) {
    xdg_toplevel_set_fullscreen(
      app.toplevel, app.selected_output == nullptr ? nullptr : app.selected_output->proxy);
  }
  wl_surface_commit(app.surface);

  while (!app.configured && !app.done) {
    if (wl_display_dispatch(app.display) < 0) {
      app.error = "Wayland connection failed while waiting for initial configure";
      return false;
    }
  }
  if (!app.configured || app.done) {
    app.error = "the Wayland surface closed before initial configure";
    return false;
  }
  if (!initialize_egl(app)) return false;
  const auto* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
  const auto* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
  std::cerr << "GLES renderer=" << (renderer == nullptr ? "unknown" : renderer)
            << " version=" << (version == nullptr ? "unknown" : version) << '\n';
  if (app.presentation == nullptr) {
    std::cerr << "presentation-time unavailable; reported presentation counts will remain zero\n";
  }
  draw_frame(app);
  return true;
}

void cleanup(App& app) {
  for (Feedback* info : app.outstanding_feedback) {
    if (info->proxy != nullptr) wp_presentation_feedback_destroy(info->proxy);
    delete info;
  }
  app.outstanding_feedback.clear();

  if (app.frame_callback != nullptr) {
    wl_callback_destroy(app.frame_callback);
    app.frame_callback = nullptr;
  }

  if (app.egl_ready) {
    if (app.vertex_array != 0) glDeleteVertexArrays(1, &app.vertex_array);
    if (app.program != 0) glDeleteProgram(app.program);
  }
  if (app.egl_display != EGL_NO_DISPLAY && app.egl_context != EGL_NO_CONTEXT) {
    (void)eglMakeCurrent(app.egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  }
  if (app.egl_display != EGL_NO_DISPLAY && app.egl_surface != EGL_NO_SURFACE) {
    eglDestroySurface(app.egl_display, app.egl_surface);
  }
  if (app.native_window != nullptr) wl_egl_window_destroy(app.native_window);
  if (app.egl_display != EGL_NO_DISPLAY && app.egl_context != EGL_NO_CONTEXT) {
    eglDestroyContext(app.egl_display, app.egl_context);
  }
  if (app.egl_display != EGL_NO_DISPLAY) eglTerminate(app.egl_display);

  if (app.toplevel != nullptr) xdg_toplevel_destroy(app.toplevel);
  if (app.xdg_surface_proxy != nullptr) xdg_surface_destroy(app.xdg_surface_proxy);
  if (app.surface != nullptr) wl_surface_destroy(app.surface);
  for (Output* output : app.outputs) {
    if (wl_output_get_version(output->proxy) >= WL_OUTPUT_RELEASE_SINCE_VERSION) {
      wl_output_release(output->proxy);
    } else {
      wl_output_destroy(output->proxy);
    }
    delete output;
  }
  app.outputs.clear();
  if (app.presentation != nullptr) wp_presentation_destroy(app.presentation);
  if (app.keyboard != nullptr) {
    if (wl_keyboard_get_version(app.keyboard) >= WL_KEYBOARD_RELEASE_SINCE_VERSION) {
      wl_keyboard_release(app.keyboard);
    } else {
      wl_keyboard_destroy(app.keyboard);
    }
  }
  if (app.seat != nullptr) {
    if (wl_seat_get_version(app.seat) >= WL_SEAT_RELEASE_SINCE_VERSION) {
      wl_seat_release(app.seat);
    } else {
      wl_seat_destroy(app.seat);
    }
  }
  if (app.wm_base != nullptr) xdg_wm_base_destroy(app.wm_base);
  if (app.compositor != nullptr) wl_compositor_destroy(app.compositor);
  if (app.registry != nullptr) wl_registry_destroy(app.registry);
  if (app.display != nullptr) wl_display_disconnect(app.display);
}

void event_loop(App& app) {
  const int display_fd = wl_display_get_fd(app.display);
  while (!app.done) {
    if (wl_display_dispatch_pending(app.display) < 0) {
      set_error(app, "Wayland event dispatch failed");
      break;
    }

    bool want_write = false;
    if (wl_display_flush(app.display) < 0) {
      if (errno == EAGAIN) want_write = true;
      else {
        set_error(app, "Wayland display flush failed");
        break;
      }
    }

    pollfd descriptor{};
    descriptor.fd = display_fd;
    descriptor.events = static_cast<short>(POLLIN | (want_write ? POLLOUT : 0));
    const int poll_result = poll(&descriptor, 1, 250);
    if (poll_result < 0) {
      if (errno == EINTR) continue;
      set_error(app, "poll on the Wayland display failed");
      break;
    }
    if (poll_result == 0) {
      report_statistics(app, Clock::now());
      continue;
    }
    if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
      set_error(app, "Wayland display connection closed");
      break;
    }
    if ((descriptor.revents & POLLIN) != 0 && wl_display_dispatch(app.display) < 0) {
      set_error(app, "Wayland event dispatch failed");
      break;
    }
    report_statistics(app, Clock::now());
  }
}

void drain_feedback(App& app) {
  if (app.presentation == nullptr || app.display == nullptr || !app.error.empty()) return;
  const auto deadline = Clock::now() + std::chrono::milliseconds(150);
  const int display_fd = wl_display_get_fd(app.display);
  while (!app.outstanding_feedback.empty() && Clock::now() < deadline) {
    if (wl_display_dispatch_pending(app.display) < 0 || app.outstanding_feedback.empty()) break;
    if (wl_display_flush(app.display) < 0 && errno != EAGAIN) break;
    pollfd descriptor{};
    descriptor.fd = display_fd;
    descriptor.events = POLLIN;
    const int result = poll(&descriptor, 1, 20);
    if (result < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if ((descriptor.revents & POLLIN) != 0 && wl_display_dispatch(app.display) < 0) break;
    if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) break;
  }
}

} // namespace

int main(int argc, char** argv) {
  Options options;
  std::string error;
  if (!parse_options(argc, argv, options, error)) {
    std::cerr << "error: " << error << '\n';
    print_usage(std::cerr, argv[0]);
    return 2;
  }
  if (options.help) {
    print_usage(std::cout, argv[0]);
    return 0;
  }

  App app(std::move(options));
  if (!initialize_wayland(app)) {
    std::cerr << "error: " << (app.error.empty() ? "Wayland initialization failed" : app.error) << '\n';
    cleanup(app);
    return 1;
  }
  event_loop(app);
  drain_feedback(app);
  if (!app.error.empty()) std::cerr << "error: " << app.error << '\n';
  double elapsed = 0.0;
  if (app.started) elapsed = std::chrono::duration<double>(Clock::now() - app.first_submit).count();
  std::cerr << "final: elapsed-s=" << elapsed
            << " submitted=" << app.submitted
            << " presented=" << app.presented
            << " discarded=" << app.discarded
            << " presented-id-gaps=" << app.presentation_id_gaps;
  if (app.have_presented_id) {
    std::cerr << " source-first-id=" << app.first_presented_id
              << " source-last-id=" << app.last_presented_id;
  }
  if (elapsed > 0.0) {
    std::cerr << " submitted-fps=" << (static_cast<double>(app.submitted) / elapsed)
              << " presented-fps=" << (static_cast<double>(app.presented) / elapsed);
  }
  if (app.last_refresh_ns != 0) {
    std::cerr << " next-refresh-us=" << (static_cast<double>(app.last_refresh_ns) / 1000.0);
  }
  std::cerr << '\n';
  cleanup(app);
  return app.error.empty() ? 0 : 1;
}
