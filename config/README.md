# Settings module

`fluxrec_config` is a Qt-independent C++23 settings model and store. It stores
versioned JSON under `$XDG_CONFIG_HOME/fluxrec/settings.json`, falling back to
`$HOME/.config/fluxrec/settings.json`. Missing, malformed, unsupported-version,
and invalid settings return explicit errors. Initial settings are created only
when the caller explicitly invokes `makeDefaults()` and then saves them.

The module persists the active backend controls (H.264/HEVC, bitrate or constant-QP,
GOP size, encoder tune, output resolution, capture pool and async depth), output directory and display,
replay duration, PipeWire device/gain/mute values, and basic UI preferences.
Output dimensions default to native capture size; an explicit size uses the
Vulkan conversion path's GPU resize. `max_fps` paces capture requests (zero is
uncapped) and preserves compositor presentation timestamps. Supported caps are
60, 120, 240, and 360 FPS. The settings model enforces an even-dimension
application bound of 4 through 16,384 pixels; the selected Vulkan encoder checks
its device and profile limits when it opens. The current Vulkan
Video `tune` setting accepts `default`, `ll`, `ull`, or `hq`; the default leaves
FFmpeg's Vulkan encoder tuning at its default. These are not NVENC P1–P7 presets.
Constant-QP values are bounded from 0 to 51 for the current 8-bit NV12 path; H.264
QP 28 was live checked on one NVIDIA RTX 3090 Ti setup, while other combinations
remain experimental. The backend does not expose preset or B-frame controls.

Saving validates the model, writes a same-directory temporary file with mode
0600, syncs it, atomically renames it, then syncs the containing directory.
Loading refuses symlinks and non-regular files and caps input at 1 MiB.

## Integration

Add `config` to the root component list and link the consumer to
`fluxrec_config`. The component requires the CMake package
`nlohmann_json` (version 3.2 or newer), but its public header has no JSON or Qt
dependency. Typical initialization is:

```cpp
auto store = fluxrec::config::Store::forCurrentUser();
if (!store) { /* report store.error().message */ }
auto settings = store->load();
if (!settings && settings.error().code == fluxrec::config::ErrorCode::not_found) {
    auto initial = fluxrec::config::makeDefaults();
    if (initial) {
        // Save only as an explicit initialization decision by the application.
        (void)store->save(*initial);
    }
}
```
