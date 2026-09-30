#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/dict.h>
#include <spa/utils/result.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <time.h>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

constexpr std::size_t kMaxMetadataSamples = 262'144;

struct NodeInfo {
  std::uint32_t id{};
  std::uint64_t serial{};
  std::optional<std::uint64_t> device_id;
  std::optional<std::uint32_t> driver_id;
  bool can_drive{};
  std::string name;
  std::string description;
  std::string media_class;
  std::string clock_name;
  std::string device_bus;
};

struct LinkInfo {
  std::uint32_t id{};
  std::optional<std::uint32_t> output_node;
  std::optional<std::uint32_t> input_node;
};

class Probe;

struct NodeBinding {
  Probe* owner{};
  std::uint32_t id{};
  pw_node* node{};
  spa_hook hook{};
};

struct ClockSample {
  std::uint64_t host_monotonic_ns{};
  std::int64_t pipewire_now_ns{};
  std::uint64_t pipewire_nsec_ns{};
  std::uint64_t ticks{};
  std::int64_t delay{};
  std::uint32_t rate_num{};
  std::uint32_t rate_denom{};
  std::uint64_t buffer_time_ns{};
  bool time_query_ok{};
};

struct StreamProbe {
  StreamProbe() : samples(std::make_unique<ClockSample[]>(kMaxMetadataSamples)) {}

  class Probe* owner{};
  std::size_t index{};
  pw_stream* stream{};
  spa_hook hook{};
  std::atomic<int> state{PW_STREAM_STATE_UNCONNECTED};
  std::atomic<std::uint64_t> time_queries_ok{0};
  std::atomic<std::uint64_t> time_queries_failed{0};
  std::atomic<std::uint64_t> buffers_seen{0};
  std::atomic<std::uint64_t> metadata_overflow{0};
  std::atomic<std::uint32_t> stream_node_id{SPA_ID_INVALID};
  std::unique_ptr<ClockSample[]> samples;
  std::atomic<std::size_t> sample_count{0};
  mutable std::mutex error_mutex;
  std::string startup_error;

  [[nodiscard]] std::string error_message() const {
    std::lock_guard lock(error_mutex);
    return startup_error;
  }
};

[[nodiscard]] std::uint64_t monotonic_now_ns() noexcept {
  timespec now{};
  if (::clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
  return static_cast<std::uint64_t>(now.tv_sec) * 1'000'000'000ULL +
         static_cast<std::uint64_t>(now.tv_nsec);
}

[[nodiscard]] std::string string_prop(const spa_dict* props, const char* key) {
  const char* value = props ? spa_dict_lookup(props, key) : nullptr;
  return value ? value : "";
}

template<class Integer>
[[nodiscard]] std::optional<Integer> integer_prop(const spa_dict* props,
                                                  const char* key) {
  const auto value = string_prop(props, key);
  if (value.empty()) return std::nullopt;
  Integer parsed{};
  const auto* begin = value.data();
  const auto* end = begin + value.size();
  const auto [next, error] = std::from_chars(begin, end, parsed);
  if (error != std::errc{} || next != end) return std::nullopt;
  return parsed;
}

[[nodiscard]] bool bool_prop(const spa_dict* props, const char* key) {
  const auto value = string_prop(props, key);
  return value == "true" || value == "1" || value == "yes";
}

[[nodiscard]] std::string json_escape(std::string_view value) {
  std::ostringstream out;
  for (const unsigned char ch : value) {
    switch (ch) {
      case '"': out << "\\\""; break;
      case '\\': out << "\\\\"; break;
      case '\b': out << "\\b"; break;
      case '\f': out << "\\f"; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default:
        if (ch < 0x20U) {
          out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
              << static_cast<unsigned int>(ch) << std::dec << std::setfill(' ');
        } else {
          out << static_cast<char>(ch);
        }
    }
  }
  return out.str();
}

[[nodiscard]] std::string optional_number(std::optional<std::uint64_t> value) {
  return value ? std::to_string(*value) : "null";
}

[[nodiscard]] std::string optional_number(std::optional<std::uint32_t> value) {
  return value ? std::to_string(*value) : "null";
}

struct Fit {
  bool valid{};
  std::size_t points{};
  double slope{};
  double intercept{};
  double max_residual_ns{};
};

template<class X, class Y>
[[nodiscard]] Fit linear_fit(const std::vector<ClockSample>& samples,
                             X x_value, Y y_value) {
  Fit result;
  if (samples.size() < 3) return result;

  long double x_origin = 0;
  long double y_origin = 0;
  bool found_origin = false;
  for (const auto& sample : samples) {
    if (!sample.time_query_ok) continue;
    const long double x = x_value(sample);
    const long double y = y_value(sample);
    if (!std::isfinite(static_cast<double>(x)) ||
        !std::isfinite(static_cast<double>(y))) continue;
    x_origin = x;
    y_origin = y;
    found_origin = true;
    break;
  }
  if (!found_origin) return result;

  long double mean_x = 0;
  long double mean_y = 0;
  long double sum_xx = 0;
  long double sum_xy = 0;
  long double max_residual = 0;
  std::size_t count = 0;
  for (const auto& sample : samples) {
    if (!sample.time_query_ok) continue;
    const long double x = x_value(sample) - x_origin;
    const long double y = y_value(sample) - y_origin;
    if (!std::isfinite(static_cast<double>(x)) ||
        !std::isfinite(static_cast<double>(y))) continue;
    ++count;
    const auto delta_x = x - mean_x;
    const auto delta_y = y - mean_y;
    mean_x += delta_x / static_cast<long double>(count);
    mean_y += delta_y / static_cast<long double>(count);
    sum_xx += delta_x * (x - mean_x);
    sum_xy += delta_x * (y - mean_y);
  }
  if (count < 3 || sum_xx <= 0) return result;

  const long double slope = sum_xy / sum_xx;
  const long double intercept = mean_y - slope * mean_x;
  for (const auto& sample : samples) {
    if (!sample.time_query_ok) continue;
    const long double x = x_value(sample) - x_origin;
    const long double y = y_value(sample) - y_origin;
    if (!std::isfinite(static_cast<double>(x)) ||
        !std::isfinite(static_cast<double>(y))) continue;
    max_residual = std::max(max_residual, std::abs(y - (intercept + slope * x)));
  }
  result.valid = std::isfinite(static_cast<double>(slope));
  result.points = count;
  result.slope = static_cast<double>(slope);
  result.intercept = static_cast<double>(intercept);
  result.max_residual_ns = static_cast<double>(max_residual * 1'000'000'000.0L);
  return result;
}

class Probe final {
public:
  Probe(std::array<std::uint64_t, 2> source_serials, std::uint32_t duration_seconds)
      : source_serials_(source_serials), duration_seconds_(duration_seconds) {
    stream_data_[0].owner = this;
    stream_data_[0].index = 0;
    stream_data_[1].owner = this;
    stream_data_[1].index = 1;
  }

  ~Probe() { stop(); }

  Probe(const Probe&) = delete;
  Probe& operator=(const Probe&) = delete;

  void run() {
    setup();
    wait_for_sources();
    pw_thread_loop_lock(loop_);
    try {
      create_stream(0);
      create_stream(1);
    } catch (...) {
      pw_thread_loop_unlock(loop_);
      throw;
    }
    pw_thread_loop_unlock(loop_);
    wait_until_ready();
    start_ns_ = monotonic_now_ns();
    std::this_thread::sleep_for(std::chrono::seconds{duration_seconds_});
    stop();
    finish_ns_ = monotonic_now_ns();
  }

  [[nodiscard]] std::string report() const {
    const auto info_a = selected_node(0);
    const auto info_b = selected_node(1);
    const auto graph_driver_a = effective_driver_id(info_a);
    const auto graph_driver_b = effective_driver_id(info_b);
    const bool graph_drivers_known = graph_driver_a.has_value() && graph_driver_b.has_value();
    const bool graph_drivers_distinct = graph_drivers_known &&
                                         graph_driver_a != graph_driver_b;
    const bool devices_known = info_a.device_id.has_value() && info_b.device_id.has_value();
    const bool physical_devices_distinct = devices_known &&
                                           info_a.device_id != info_b.device_id;
    const bool clock_names_known = !info_a.clock_name.empty() && !info_b.clock_name.empty();
    const bool clock_names_distinct = clock_names_known &&
                                      info_a.clock_name != info_b.clock_name;

    const auto fit_a = fit_for(stream_data_[0]);
    const auto fit_b = fit_for(stream_data_[1]);
    std::optional<double> relative_ppm;
    if (fit_a.valid && fit_b.valid && fit_a.slope != 0.0) {
      relative_ppm = ((fit_b.slope / fit_a.slope) - 1.0) * 1'000'000.0;
    }

    std::ostringstream out;
    out << std::setprecision(12)
        << "{\n"
        << "  \"probe\": \"pipewire_clock_probe\",\n"
        << "  \"duration_requested_seconds\": " << duration_seconds_ << ",\n"
        << "  \"duration_observed_seconds\": "
        << (finish_ns_ >= start_ns_ ? (finish_ns_ - start_ns_) / 1'000'000'000.0 : 0.0)
        << ",\n"
        << "  \"audio_sample_values_read\": false,\n"
        << "  \"audio_sample_values_retained\": false,\n"
        << "  \"playback_streams_created\": 0,\n"
        << "  \"sources\": [\n"
        << node_json(0, info_a, graph_driver_a, fit_a) << ",\n"
        << node_json(1, info_b, graph_driver_b, fit_b) << "\n"
        << "  ],\n"
        << "  \"domain_comparison\": {\n"
        << "    \"physical_device_ids_distinct\": "
        << (devices_known ? (physical_devices_distinct ? "true" : "false") : "null") << ",\n"
        << "    \"pipewire_clock_names_distinct\": "
        << (clock_names_known ? (clock_names_distinct ? "true" : "false") : "null") << ",\n"
        << "    \"graph_driver_ids_distinct\": "
        << (graph_drivers_known ? (graph_drivers_distinct ? "true" : "false") : "null") << ",\n"
        << "    \"assessment\": \"" << domain_assessment(
               devices_known, physical_devices_distinct, clock_names_known,
               clock_names_distinct, graph_drivers_known, graph_drivers_distinct)
        << "\"\n"
        << "  },\n"
        << "  \"relative_rate_b_to_a_ppm\": "
        << (relative_ppm ? number(*relative_ppm) : "null") << ",\n"
        << "  \"relative_rate_note\": \"Difference between fitted PipeWire graph-driver tick rates against CLOCK_MONOTONIC; PipeWire may resample or synchronize the sources, so this is not guaranteed to be raw hardware oscillator drift.\",\n"
        << "  \"limits\": [\n"
        << "    \"This probe measures clock metadata only; it does not estimate acoustic or electrical A/V offset.\",\n"
        << "    \"PipeWire time.ticks is graph-driver time. Distinct driver IDs do not prove unsynchronized hardware oscillators.\",\n"
        << "    \"The capture buffers are dequeued and returned without mapping or reading sample payloads.\",\n"
        << "    \"A missing clock.name or driver ID is reported as unknown rather than inferred from node names.\"\n"
        << "  ]\n"
        << "}\n";
    return out.str();
  }

private:
  static void on_registry_global(void* data, std::uint32_t id, std::uint32_t,
                                 const char* type, std::uint32_t version,
                                 const spa_dict* props) noexcept {
    auto& self = *static_cast<Probe*>(data);
    try {
      if (type && std::strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
        NodeInfo node;
        node.id = id;
        node.serial = integer_prop<std::uint64_t>(props, PW_KEY_OBJECT_SERIAL).value_or(id);
        node.device_id = integer_prop<std::uint64_t>(props, PW_KEY_DEVICE_ID);
        node.driver_id = integer_prop<std::uint32_t>(props, PW_KEY_NODE_DRIVER_ID);
        node.can_drive = bool_prop(props, PW_KEY_NODE_DRIVER);
        node.name = string_prop(props, PW_KEY_NODE_NAME);
        node.description = string_prop(props, PW_KEY_NODE_DESCRIPTION);
        node.media_class = string_prop(props, PW_KEY_MEDIA_CLASS);
        node.clock_name = string_prop(props, "clock.name");
        node.device_bus = string_prop(props, "device.bus");
        const bool selected = node.serial == self.source_serials_[0] ||
                              node.serial == self.source_serials_[1];
        {
          std::lock_guard lock(self.registry_mutex_);
          self.nodes_[id] = std::move(node);
        }
        if (selected) self.bind_node(
            id, std::min(version, static_cast<std::uint32_t>(PW_VERSION_NODE)));
      } else if (type && std::strcmp(type, PW_TYPE_INTERFACE_Link) == 0) {
        LinkInfo link;
        link.id = id;
        link.output_node = integer_prop<std::uint32_t>(props, PW_KEY_LINK_OUTPUT_NODE);
        link.input_node = integer_prop<std::uint32_t>(props, PW_KEY_LINK_INPUT_NODE);
        std::lock_guard lock(self.registry_mutex_);
        self.links_[id] = std::move(link);
      }
      self.signal_loop();
    } catch (...) {
      self.registry_callback_errors_.fetch_add(1, std::memory_order_relaxed);
    }
  }

  static void on_registry_global_remove(void* data, std::uint32_t id) noexcept {
    auto& self = *static_cast<Probe*>(data);
    std::lock_guard lock(self.registry_mutex_);
    self.nodes_.erase(id);
    self.links_.erase(id);
    const auto binding = self.node_bindings_.find(id);
    if (binding != self.node_bindings_.end()) {
      if (binding->second->node) {
        pw_proxy_destroy(reinterpret_cast<pw_proxy*>(binding->second->node));
      }
      self.node_bindings_.erase(binding);
    }
    self.signal_loop();
  }

  static void on_node_info(void* data, const pw_node_info* info) noexcept {
    if (!info || !info->props) return;
    auto& binding = *static_cast<NodeBinding*>(data);
    auto& self = *binding.owner;
    std::lock_guard lock(self.registry_mutex_);
    const auto found = self.nodes_.find(binding.id);
    if (found == self.nodes_.end()) return;
    auto& node = found->second;
    if (const auto value = integer_prop<std::uint64_t>(info->props, PW_KEY_DEVICE_ID))
      node.device_id = value;
    if (const auto value = integer_prop<std::uint32_t>(info->props, PW_KEY_NODE_DRIVER_ID))
      node.driver_id = value;
    if (spa_dict_lookup(info->props, PW_KEY_NODE_DRIVER))
      node.can_drive = bool_prop(info->props, PW_KEY_NODE_DRIVER);
    if (const auto value = string_prop(info->props, PW_KEY_NODE_NAME); !value.empty())
      node.name = value;
    if (const auto value = string_prop(info->props, PW_KEY_NODE_DESCRIPTION); !value.empty())
      node.description = value;
    if (const auto value = string_prop(info->props, PW_KEY_MEDIA_CLASS); !value.empty())
      node.media_class = value;
    if (const auto value = string_prop(info->props, "clock.name"); !value.empty())
      node.clock_name = value;
    if (const auto value = string_prop(info->props, "device.bus"); !value.empty())
      node.device_bus = value;
    self.signal_loop();
  }

  static const pw_node_events& node_events() {
    static const pw_node_events events = [] {
      pw_node_events result{};
      result.version = PW_VERSION_NODE_EVENTS;
      result.info = on_node_info;
      return result;
    }();
    return events;
  }

  void bind_node(std::uint32_t id, std::uint32_t version) {
    auto binding = std::make_unique<NodeBinding>();
    binding->owner = this;
    binding->id = id;
    binding->node = static_cast<pw_node*>(pw_registry_bind(
        registry_, id, PW_TYPE_INTERFACE_Node, version, 0));
    if (!binding->node) return;
    auto* raw = binding.get();
    {
      std::lock_guard lock(registry_mutex_);
      node_bindings_[id] = std::move(binding);
    }
    (void)pw_node_add_listener(raw->node, &raw->hook, &node_events(), raw);
  }

  static const pw_registry_events& registry_events() {
    static const pw_registry_events events = [] {
      pw_registry_events result{};
      result.version = PW_VERSION_REGISTRY_EVENTS;
      result.global = on_registry_global;
      result.global_remove = on_registry_global_remove;
      return result;
    }();
    return events;
  }

  static void on_stream_state(void* data, pw_stream_state, pw_stream_state state,
                              const char* error) noexcept {
    auto& stream = *static_cast<StreamProbe*>(data);
    stream.state.store(state, std::memory_order_release);
    if (state == PW_STREAM_STATE_ERROR && error) {
      std::lock_guard lock(stream.error_mutex);
      stream.startup_error = error;
    }
    const auto stream_node_id = pw_stream_get_node_id(stream.stream);
    if (stream_node_id != SPA_ID_INVALID) {
      stream.stream_node_id.store(stream_node_id, std::memory_order_release);
    }
    stream.owner->signal_loop();
  }

  static void on_stream_process(void* data) noexcept {
    auto& stream = *static_cast<StreamProbe*>(data);
    while (auto* buffer = pw_stream_dequeue_buffer(stream.stream)) {
      ClockSample sample;
      sample.host_monotonic_ns = monotonic_now_ns();
      sample.buffer_time_ns = buffer->time;
      sample.pipewire_nsec_ns = pw_stream_get_nsec(stream.stream);
      pw_time time{};
      if (pw_stream_get_time_n(stream.stream, &time, sizeof(time)) == 0) {
        sample.time_query_ok = true;
        sample.pipewire_now_ns = time.now;
        sample.ticks = time.ticks;
        sample.delay = time.delay;
        sample.rate_num = time.rate.num;
        sample.rate_denom = time.rate.denom;
        stream.time_queries_ok.fetch_add(1, std::memory_order_relaxed);
      } else {
        stream.time_queries_failed.fetch_add(1, std::memory_order_relaxed);
      }

      const auto index = stream.sample_count.fetch_add(1, std::memory_order_relaxed);
      if (index < kMaxMetadataSamples) {
        stream.samples[index] = sample;
      } else {
        stream.metadata_overflow.fetch_add(1, std::memory_order_relaxed);
      }
      stream.buffers_seen.fetch_add(1, std::memory_order_relaxed);
      (void)pw_stream_queue_buffer(stream.stream, buffer);
    }
  }

  static const pw_stream_events& stream_events() {
    static const pw_stream_events events = [] {
      pw_stream_events result{};
      result.version = PW_VERSION_STREAM_EVENTS;
      result.state_changed = on_stream_state;
      result.process = on_stream_process;
      return result;
    }();
    return events;
  }

  void setup() {
    static std::once_flag initialized;
    std::call_once(initialized, [] { pw_init(nullptr, nullptr); });

    loop_ = pw_thread_loop_new("fluxrec-clock-probe", nullptr);
    if (!loop_) throw std::runtime_error("could not create PipeWire thread loop");
    context_ = pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0);
    if (!context_) throw std::runtime_error("could not create PipeWire context");
    core_ = pw_context_connect(context_, nullptr, 0);
    if (!core_) throw std::runtime_error("could not connect to PipeWire");
    registry_ = pw_core_get_registry(core_, PW_VERSION_REGISTRY, 0);
    if (!registry_) throw std::runtime_error("could not get PipeWire registry");
    pw_registry_add_listener(registry_, &registry_hook_, &registry_events(), this);

    if (pw_thread_loop_start(loop_) < 0) {
      throw std::runtime_error("could not start PipeWire thread loop");
    }
    loop_started_ = true;
  }

  void create_stream(std::size_t index) {
    auto& state = stream_data_[index];
    auto* props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "Production",
        PW_KEY_MEDIA_CLASS, "Stream/Input/Audio",
        PW_KEY_NODE_NAME, index == 0 ? "fluxrec.clockprobe.source-a"
                                     : "fluxrec.clockprobe.source-b",
        PW_KEY_NODE_DESCRIPTION, index == 0 ? "Fluxrec metadata-only clock probe A"
                                            : "Fluxrec metadata-only clock probe B",
        PW_KEY_STREAM_IS_LIVE, "true",
        PW_KEY_STREAM_MONITOR, "false",
        nullptr);
    if (!props) throw std::runtime_error("could not allocate PipeWire stream properties");
    state.stream = pw_stream_new(core_, index == 0 ? "Fluxrec Clock Probe A"
                                                   : "Fluxrec Clock Probe B",
                                props);
    if (!state.stream) throw std::runtime_error("could not create PipeWire capture stream");
    pw_stream_add_listener(state.stream, &state.hook, &stream_events(), &state);

    spa_audio_info_raw audio_info{};
    audio_info.format = SPA_AUDIO_FORMAT_F32_LE;
    audio_info.rate = 48'000;
    audio_info.channels = 1;
    audio_info.position[0] = SPA_AUDIO_CHANNEL_MONO;
    std::array<std::uint8_t, 512> pod_storage{};
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(pod_storage.data(), pod_storage.size());
    const spa_pod* params[] = {
        spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &audio_info),
    };
    if (!params[0]) throw std::runtime_error("could not build capture format");
    const auto result = pw_stream_connect(
        state.stream, PW_DIRECTION_INPUT, source_node_ids_[index],
        static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT), params, 1);
    if (result < 0) {
      throw std::runtime_error(std::string("could not connect PipeWire capture stream: ") +
                               spa_strerror(result));
    }
  }

  void wait_for_sources() {
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    while (std::chrono::steady_clock::now() < deadline) {
      const auto node_a = find_selected_node(0);
      const auto node_b = find_selected_node(1);
      if (node_a && node_b) {
        source_node_ids_ = {node_a->id, node_b->id};
        return;
      }
      pw_thread_loop_lock(loop_);
      (void)pw_thread_loop_timed_wait(loop_, 1);
      pw_thread_loop_unlock(loop_);
    }
    throw std::runtime_error(
        "explicit object.serial values were not both present as PipeWire Audio/Source nodes");
  }

  void wait_until_ready() {
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    while (std::chrono::steady_clock::now() < deadline) {
      const auto node_a = find_selected_node(0);
      const auto node_b = find_selected_node(1);
      const auto stream_id_a = stream_data_[0].stream_node_id.load(std::memory_order_acquire);
      const auto stream_id_b = stream_data_[1].stream_node_id.load(std::memory_order_acquire);
      const bool streaming = stream_data_[0].state.load(std::memory_order_acquire) ==
                                 PW_STREAM_STATE_STREAMING &&
                             stream_data_[1].state.load(std::memory_order_acquire) ==
                                 PW_STREAM_STATE_STREAMING;
      const auto error_a = stream_data_[0].error_message();
      const auto error_b = stream_data_[1].error_message();
      if (!error_a.empty() || !error_b.empty()) {
        throw std::runtime_error("PipeWire stream error: " +
            (error_a.empty() ? error_b : error_a));
      }
      if (node_a && node_b && node_a->id == source_node_ids_[0] &&
          node_b->id == source_node_ids_[1] && streaming &&
          stream_id_a != SPA_ID_INVALID && stream_id_b != SPA_ID_INVALID &&
          linked(source_node_ids_[0], stream_id_a) &&
          linked(source_node_ids_[1], stream_id_b)) {
        return;
      }
      pw_thread_loop_lock(loop_);
      (void)pw_thread_loop_timed_wait(loop_, 1);
      pw_thread_loop_unlock(loop_);
    }
    const auto node_a = find_selected_node(0);
    const auto node_b = find_selected_node(1);
    const auto stream_id_a = stream_data_[0].stream_node_id.load(std::memory_order_acquire);
    const auto stream_id_b = stream_data_[1].stream_node_id.load(std::memory_order_acquire);
    std::ostringstream detail;
    detail << "capture streams did not become streaming and link to both explicit source IDs"
           << " (source_nodes=" << node_a.has_value() << "," << node_b.has_value()
           << "; stream_states="
           << stream_data_[0].state.load(std::memory_order_acquire) << ","
           << stream_data_[1].state.load(std::memory_order_acquire)
           << "; stream_node_ids=" << stream_id_a << "," << stream_id_b
           << "; linked="
           << (node_a && stream_id_a != SPA_ID_INVALID &&
               linked(source_node_ids_[0], stream_id_a))
           << ","
           << (node_b && stream_id_b != SPA_ID_INVALID &&
               linked(source_node_ids_[1], stream_id_b))
           << "; registry_callback_errors="
           << registry_callback_errors_.load(std::memory_order_relaxed) << ")";
    throw std::runtime_error(detail.str());
  }

  [[nodiscard]] std::optional<NodeInfo> find_selected_node(std::size_t index) const {
    std::lock_guard lock(registry_mutex_);
    for (const auto& [id, node] : nodes_) {
      (void)id;
      if (node.serial == source_serials_[index]) {
        if (node.media_class != "Audio/Source") return std::nullopt;
        return node;
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] NodeInfo selected_node(std::size_t index) const {
    const auto found = find_selected_node(index);
    if (!found) throw std::runtime_error("selected source disappeared from PipeWire registry");
    return *found;
  }

  [[nodiscard]] bool linked(std::uint32_t source_node, std::uint32_t stream_node) const {
    std::lock_guard lock(registry_mutex_);
    for (const auto& [id, link] : links_) {
      (void)id;
      if (link.output_node == source_node && link.input_node == stream_node) return true;
    }
    return false;
  }

  [[nodiscard]] static std::optional<std::uint32_t> effective_driver_id(
      const NodeInfo& node) {
    if (node.can_drive) return node.id;
    return node.driver_id;
  }

  [[nodiscard]] static std::string domain_assessment(
      bool devices_known, bool physical_devices_distinct,
      bool clock_names_known, bool clock_names_distinct,
      bool graph_drivers_known, bool graph_drivers_distinct) {
    if (clock_names_known) {
      return clock_names_distinct ? "distinct_pipewire_clock_names"
                                  : "same_pipewire_clock_name";
    }
    if (graph_drivers_known && graph_drivers_distinct && devices_known &&
        physical_devices_distinct) {
      return "distinct_physical_devices_and_graph_drivers_clock_name_unreported";
    }
    if (graph_drivers_known && !graph_drivers_distinct) return "shared_graph_driver";
    return "unknown";
  }

  [[nodiscard]] static Fit fit_for(const StreamProbe& stream) {
    const auto count = std::min(stream.sample_count.load(std::memory_order_acquire),
                                kMaxMetadataSamples);
    std::vector<ClockSample> samples(stream.samples.get(), stream.samples.get() + count);
    const auto host_fit = linear_fit(samples,
        [](const ClockSample& sample) {
          return static_cast<long double>(sample.host_monotonic_ns) / 1'000'000'000.0L;
        },
        [](const ClockSample& sample) {
          if (sample.rate_denom == 0) return std::numeric_limits<long double>::quiet_NaN();
          return static_cast<long double>(sample.ticks) * sample.rate_num /
                 sample.rate_denom;
        });
    return host_fit;
  }

  [[nodiscard]] static std::string fit_json(const StreamProbe& stream, const Fit& fit) {
    const auto count = std::min(stream.sample_count.load(std::memory_order_acquire),
                                kMaxMetadataSamples);
    std::uint64_t rate_changes = 0;
    std::uint64_t tick_resets = 0;
    std::uint64_t buffer_time_samples = 0;
    std::optional<std::pair<std::uint32_t, std::uint32_t>> last_rate;
    std::optional<std::uint64_t> last_ticks;
    std::optional<std::uint64_t> first_buffer_time;
    std::optional<std::uint64_t> last_buffer_time;
    for (std::size_t i = 0; i < count; ++i) {
      const auto& sample = stream.samples[i];
      if (sample.buffer_time_ns != 0) {
        ++buffer_time_samples;
        if (!first_buffer_time) first_buffer_time = sample.buffer_time_ns;
        last_buffer_time = sample.buffer_time_ns;
      }
      if (sample.rate_denom != 0) {
        const std::pair rate{sample.rate_num, sample.rate_denom};
        if (last_rate && rate != *last_rate) ++rate_changes;
        last_rate = rate;
      }
      if (sample.time_query_ok) {
        if (last_ticks && sample.ticks < *last_ticks) ++tick_resets;
        last_ticks = sample.ticks;
      }
    }
    const double ppm = (fit.slope - 1.0) * 1'000'000.0;
    std::ostringstream out;
    out << std::setprecision(12)
        << "{\"stream_buffers\":" << stream.buffers_seen.load(std::memory_order_relaxed)
        << ",\"clock_samples_stored\":" << count
        << ",\"metadata_overflow\":" << stream.metadata_overflow.load(std::memory_order_relaxed)
        << ",\"time_queries_ok\":" << stream.time_queries_ok.load(std::memory_order_relaxed)
        << ",\"time_queries_failed\":" << stream.time_queries_failed.load(std::memory_order_relaxed)
        << ",\"host_monotonic_first_ns\":"
        << (count == 0 ? "null" : std::to_string(stream.samples[0].host_monotonic_ns))
        << ",\"host_monotonic_last_ns\":"
        << (count == 0 ? "null" : std::to_string(stream.samples[count - 1].host_monotonic_ns))
        << ",\"pipewire_now_first_ns\":"
        << (count == 0 || !stream.samples[0].time_query_ok
                ? "null" : std::to_string(stream.samples[0].pipewire_now_ns))
        << ",\"pipewire_now_last_ns\":"
        << (count == 0 || !stream.samples[count - 1].time_query_ok
                ? "null" : std::to_string(stream.samples[count - 1].pipewire_now_ns))
        << ",\"pipewire_nsec_first_ns\":"
        << (count == 0 ? "null" : std::to_string(stream.samples[0].pipewire_nsec_ns))
        << ",\"pipewire_nsec_last_ns\":"
        << (count == 0 ? "null" : std::to_string(stream.samples[count - 1].pipewire_nsec_ns))
        << ",\"pipewire_rate_changes\":" << rate_changes
        << ",\"pipewire_tick_resets\":" << tick_resets
        << ",\"buffer_time_nonzero_samples\":" << buffer_time_samples
        << ",\"buffer_time_first_ns\":"
        << (first_buffer_time ? std::to_string(*first_buffer_time) : "null")
        << ",\"buffer_time_last_ns\":"
        << (last_buffer_time ? std::to_string(*last_buffer_time) : "null")
        << ",\"host_monotonic_fit\":{\"valid\":" << (fit.valid ? "true" : "false")
        << ",\"points\":" << fit.points
        << ",\"ticks_seconds_per_host_second_slope\":"
        << (fit.valid ? number(fit.slope) : "null")
        << ",\"rate_offset_ppm\":" << (fit.valid ? number(ppm) : "null")
        << ",\"max_fit_residual_ns\":"
        << (fit.valid ? number(fit.max_residual_ns) : "null") << "}}";
    return out.str();
  }

  [[nodiscard]] std::string node_json(std::size_t index, const NodeInfo& node,
                                      std::optional<std::uint32_t> driver_id,
                                      const Fit& fit) const {
    const auto& stream = stream_data_[index];
    std::ostringstream out;
    out << "    {\"source_serial\":" << node.serial
        << ",\"node_id\":" << node.id
        << ",\"name\":\"" << json_escape(node.name) << "\""
        << ",\"description\":\"" << json_escape(node.description) << "\""
        << ",\"media_class\":\"" << json_escape(node.media_class) << "\""
        << ",\"device_id\":" << optional_number(node.device_id)
        << ",\"device_bus\":\"" << json_escape(node.device_bus) << "\""
        << ",\"clock_name\":\"" << json_escape(node.clock_name) << "\""
        << ",\"node_can_drive_graph\":" << (node.can_drive ? "true" : "false")
        << ",\"node_driver_id_property\":" << optional_number(node.driver_id)
        << ",\"effective_graph_driver_id\":" << optional_number(driver_id)
        << ",\"stream_node_id\":"
        << stream.stream_node_id.load(std::memory_order_acquire)
        << ",\"linked_to_selected_source\":true"
        << ",\"clock_fit\":" << fit_json(stream, fit) << "}";
    return out.str();
  }

  [[nodiscard]] static std::string number(double value) {
    if (!std::isfinite(value)) return "null";
    std::ostringstream out;
    out << std::setprecision(12) << value;
    return out.str();
  }

  void signal_loop() noexcept {
    if (loop_ && loop_started_) pw_thread_loop_signal(loop_, false);
  }

  void stop() noexcept {
    if (!loop_) return;
    if (loop_started_) pw_thread_loop_lock(loop_);
    for (auto& state : stream_data_) {
      if (!state.stream) continue;
      (void)pw_stream_disconnect(state.stream);
      pw_stream_destroy(state.stream);
      state.stream = nullptr;
    }
    for (auto& [id, binding] : node_bindings_) {
      (void)id;
      spa_hook_remove(&binding->hook);
      if (binding->node) {
        pw_proxy_destroy(reinterpret_cast<pw_proxy*>(binding->node));
        binding->node = nullptr;
      }
    }
    node_bindings_.clear();
    if (registry_) {
      pw_proxy_destroy(reinterpret_cast<pw_proxy*>(registry_));
      registry_ = nullptr;
    }
    if (core_) {
      pw_core_disconnect(core_);
      core_ = nullptr;
    }
    if (context_) {
      pw_context_destroy(context_);
      context_ = nullptr;
    }
    if (loop_started_) pw_thread_loop_unlock(loop_);
    if (loop_started_) {
      pw_thread_loop_stop(loop_);
      loop_started_ = false;
    }
    pw_thread_loop_destroy(loop_);
    loop_ = nullptr;
  }

  std::array<std::uint64_t, 2> source_serials_{};
  std::uint32_t duration_seconds_{};
  std::array<StreamProbe, 2> stream_data_{};
  std::array<std::uint32_t, 2> source_node_ids_{};
  pw_thread_loop* loop_{};
  pw_context* context_{};
  pw_core* core_{};
  pw_registry* registry_{};
  spa_hook registry_hook_{};
  bool loop_started_{};
  mutable std::mutex registry_mutex_;
  std::map<std::uint32_t, NodeInfo> nodes_;
  std::map<std::uint32_t, LinkInfo> links_;
  std::map<std::uint32_t, std::unique_ptr<NodeBinding>> node_bindings_;
  std::atomic<std::uint64_t> registry_callback_errors_{0};
  std::uint64_t start_ns_{};
  std::uint64_t finish_ns_{};
};

[[nodiscard]] std::uint64_t parse_uint64(std::string_view value,
                                         std::string_view option) {
  std::uint64_t parsed{};
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (error != std::errc{} || end != value.data() + value.size() || parsed == 0) {
    throw std::invalid_argument(std::string(option) + " requires a positive integer");
  }
  return parsed;
}

[[nodiscard]] std::uint32_t parse_duration(std::string_view value) {
  const auto parsed = parse_uint64(value, "--duration-seconds");
  if (parsed < 5 || parsed > 600) {
    throw std::invalid_argument("--duration-seconds must be between 5 and 600");
  }
  return static_cast<std::uint32_t>(parsed);
}

void usage(const char* executable) {
  std::cerr << "usage: " << executable
            << " --source-a SERIAL --source-b SERIAL [--duration-seconds N]\n"
            << "Select two PipeWire Audio/Source object.serial values explicitly.\n"
            << "Duration range: 5..600 seconds. No audio payload is read or saved.\n";
}

}  // namespace

int main(int argc, char** argv) {
  try {
    std::optional<std::uint64_t> source_a;
    std::optional<std::uint64_t> source_b;
    std::uint32_t duration_seconds = 10;
    for (int index = 1; index < argc; ++index) {
      const std::string_view option{argv[index]};
      if (option == "--help" || option == "-h") {
        usage(argv[0]);
        return 0;
      }
      if (index + 1 >= argc) {
        throw std::invalid_argument(std::string(option) + " requires a value");
      }
      const std::string_view value{argv[++index]};
      if (option == "--source-a") source_a = parse_uint64(value, option);
      else if (option == "--source-b") source_b = parse_uint64(value, option);
      else if (option == "--duration-seconds") duration_seconds = parse_duration(value);
      else throw std::invalid_argument("unknown option: " + std::string(option));
    }
    if (!source_a || !source_b) {
      usage(argv[0]);
      return 2;
    }
    if (*source_a == *source_b) {
      throw std::invalid_argument("--source-a and --source-b must select different object serials");
    }
    Probe probe({*source_a, *source_b}, duration_seconds);
    probe.run();
    std::cout << probe.report();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "pipewire clock probe: " << error.what() << '\n';
    return 1;
  }
}
