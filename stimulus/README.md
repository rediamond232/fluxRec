# Wayland motion stimulus

`fluxrec-stimulus` is a small native Wayland client for testing whether a
capture path returns genuinely new monitor frames at high refresh rates. It
draws a full-screen procedural pattern and a changing `ID` counter with a GLES
fragment shader. It does not create, map, upload, or copy a CPU framebuffer.

The client requests the next image after each `wl_surface.frame` callback and
lets that callback pace submissions without adding a second EGL vblank wait.
When the compositor advertises
`wp_presentation`, the client also requests presentation feedback for every
submitted image and reports actual presentation cadence, refresh interval,
and discarded updates. Frame callbacks pace submissions; the compositor's
active output mode determines the refresh rate. The on-screen counter counts
submitted images, so gaps in a capture can be compared with the sequence the
compositor displayed.

## Build

From this directory:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The build needs Wayland client development files, EGL, GLES 3, `wayland-scanner`,
and the `wayland-protocols` XML files for xdg-shell and presentation-time.

## Run

The default is a 1280x720 window. Full-screen mode is opt-in:

```sh
./build/fluxrec-stimulus --fullscreen --output DP-1
```

Options:

```text
--windowed                 Start in a window (the default)
--fullscreen               Start full-screen
--output NAME              Select output by Wayland output name; implies full-screen
--width PIXELS              Initial window width (default 1280)
--height PIXELS             Initial window height (default 720)
--pattern NAME              plasma, bars, checker, solid, or colorchart
--motion VALUE              Pattern phase change per rendered frame (default 0.20)
--duration SECONDS          Exit after this many seconds; 0 runs until quit
--frames COUNT              Exit after submitting this many images
--help                      Show options
```

Keyboard controls while the window has focus: `Esc` or `Q` quits, `Space`
cycles the pattern, `1` through `5` select a pattern, `+`/`-` change motion
speed, and `F` toggles full-screen. The app prints one compact statistics
line per second. Configure the monitor refresh mode in the compositor to test
60, 120, 240, or 360 Hz; this client does not synthesize or claim a target FPS.

`--pattern colorchart` draws a static end-to-end color check on the GPU. The
upper 65% contains black, white, red, green, blue, yellow, cyan, and magenta
bars. The lower rows contain grayscale, red, green, and blue ramps, then a
neutral checkerboard. The changing frame ID remains in the upper-left corner;
the offline decoder samples patch centers away from it. Example capture setup:

```sh
./build/fluxrec-stimulus --fullscreen --output DP-1 --pattern colorchart
```

Use `fluxrec-decode-colorchart recording.mkv` to decode the recording and
report per-patch RGB measurements, ramp errors, two position-indexed checker
samples, and their absolute luma contrast. The checker report does not label
either sample as dark or light, so a reversed screen origin or one-cell offset
does not invert the diagnostic. The checker uses neutral grays to keep its
high-frequency edges meaningful after 4:2:0 chroma subsampling. Measurements include the complete display, capture,
conversion, encode, mux, and decode path. They can flag channel swaps,
clipping, crushed range, non-monotonic ramps, and severe color errors; they do
not independently identify which stage caused an error or certify a display
color profile.
