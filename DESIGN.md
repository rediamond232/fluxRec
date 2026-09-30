---
version: alpha
name: "Fluxrec"
description: "A precise Linux recording workbench for high-rate Wayland capture."
colors:
  canvas: "#0B0F14"
  surface: "#121923"
  raised: "#1A2430"
  line: "#303C49"
  text: "#E7ECF2"
  muted: "#9AA9B9"
  primary: "#B9ABFF"
  primary-ink: "#19162A"
  success: "#79D8B5"
  warning: "#FFAE73"
  danger: "#FF7687"
  focus: "#D3C9FF"
typography:
  sans:
    fontFamily: "Noto Sans, sans-serif"
    fontSize: "14px"
    lineHeight: "1.4"
  mono:
    fontFamily: "Noto Sans Mono, monospace"
    fontSize: "12px"
    lineHeight: "1.45"
rounded:
  DEFAULT: "5px"
  sm: "3px"
  lg: "8px"
spacing:
  panel: "12px"
  cluster: "8px"
  control: "36px"
  dense-control: "30px"
components:
  button-primary:
    backgroundColor: "{colors.primary}"
    textColor: "{colors.primary-ink}"
    rounded: "{rounded.DEFAULT}"
    height: "{spacing.control}"
  panel:
    backgroundColor: "{colors.surface}"
    textColor: "{colors.text}"
    rounded: "{rounded.DEFAULT}"
    padding: "{spacing.panel}"
  preview:
    backgroundColor: "{colors.canvas}"
    textColor: "{colors.text}"
  divider:
    backgroundColor: "{colors.line}"
  selected-surface:
    backgroundColor: "{colors.raised}"
    textColor: "{colors.primary}"
  telemetry:
    backgroundColor: "{colors.canvas}"
    textColor: "{colors.muted}"
    typography: "{typography.mono}"
  healthy-status:
    backgroundColor: "{colors.surface}"
    textColor: "{colors.success}"
  attention-status:
    backgroundColor: "{colors.surface}"
    textColor: "{colors.warning}"
  failure-status:
    backgroundColor: "{colors.surface}"
    textColor: "{colors.danger}"
  focus-ring:
    backgroundColor: "{colors.surface}"
    textColor: "{colors.focus}"
---

# Fluxrec Design System

## Overview

### Creative North Star

Fluxrec should feel like a compact signal workbench: the quiet, legible face of a measurement instrument used beside a live workstation. Thin rules, fixed-width readings, and a single route line connect the actual stages from Wayland capture through Vulkan and NVENC to Matroska. The route line is the signature; it never animates or lights up without a matching engine state.

### Product context and register

- **Audience and primary job:** Linux desktop users who need to record a selected Wayland output at high refresh rates and verify that the capture and encode path is keeping pace.
- **Target market(s) and evidence:** Linux desktop software; no geographic or commercial market requirement is present in `docs/objective.md`.
- **Locale(s) and language policy:** English interface for the initial release. Human-readable counts use the system locale; rates and file sizes remain explicit about units. New locales require a maintained translation catalogue.
- **Usage scene:** A resizable desktop control room beside a game or full-screen application; recording state and performance readings must stay readable at a glance.
- **Register:** Product-first, with the capture pipeline providing a restrained technical identity.
- **Memorable signature:** A fine four-stage signal rail that reflects real engine status and stays quiet while unavailable.
- **Restraint:** Preview pixels, meters, rates, and stage activity are never simulated. Empty and unavailable surfaces explain the capability boundary.
- **Anti-references:** Recreating OBS's branded visuals or exact screen styling, borrowed logos or artwork, generic neon-green “gaming” dashboards, glassmorphism, and decorative equalizer bars that imply audio data. Fluxrec may use the familiar docked producer workflow, with its own palette, typography, labels, and compact signal-rail identity.
- **Token ownership/runtime mapping:** This file owns palette and geometry tokens. `ui/src/theme_tokens.hpp` adapts the colors and dimensions to Qt; `ui/scripts/check_theme_tokens.py` compares the C++ adapter against these tokens during every UI build.

## Colors

The canvas and raised surfaces use cool graphite; thin blue-gray rules separate tools without turning them into floating cards. Violet belongs to Fluxrec actions, focus, and measured audio amplitude. Green, amber, and red mean successful, caution, and failure/recording status only. Labels always accompany color. Keep the text token for primary copy and the muted token for secondary copy; never use the muted token for small essential values. QPalette and the application QSS use the exact frontmatter values. Use platform text and focus colors if a future high-contrast mode is added.

## Typography

Noto Sans is the interface face with system sans-serif fallback. Noto Sans Mono is reserved for rates, counters, timestamps, and command diagnostics so changing values keep a stable edge. Section labels use sentence case and compact tracking; body copy does not use all caps. Numeric readings include units in adjacent text, never color alone. Both families cover broad scripts, with platform fallback left enabled.

## Layout

The main window is a user-resizable instrument bench: scene/source rail on the left, a broad preview surface in the center, recording controls on the right, and a lower mixer/statistics workspace. Dock boundaries own resizing; panels retain readable minimum widths and can be hidden or restored through the View menu. Use 12px panel insets and 8px related-control gaps. Telemetry tables scroll within their own panel. Do not let a missing preview, log, or telemetry update resize another panel.

## Elevation & Depth

Hierarchy comes from two graphite surface levels and hairline borders, not shadows or blur. Modal settings and confirmation dialogs may use the platform's ordinary dialog elevation. Avoid drop shadows on static panels and avoid translucent backdrops over performance readings.

## Shapes

Use square-edged dock geometry with a 5px radius on action controls and status tags. Do not make every region a pill or rounded card. Selection uses a violet left rule and tinted background; keyboard focus uses a clear outer focus ring. Dividers stay one pixel and maintain contrast with the canvas.

## Components

### Foundational visual states

Controls distinguish hover, pressed, focus, disabled, and busy states through surface, outline, text, and accessible labels. Disabled actions retain an explanation in their accessible description and tooltip. Loading is an indeterminate status because capture startup has no meaningful progress percentage. Empty panels reserve their normal footprint and describe the next integration needed.

### Buttons and actions

Use one violet solid primary action per decision area. Stop and settings actions use neutral outlines. Keep the system recording indicator red with both a dot and text. Never color a disabled action as active. Keep button width stable while busy and use direct verbs: “Start recording,” “Stop recording,” and “Save replay.”

### Navigation and data display

Native Qt docks provide resizable, hideable workspace panels. The left Scenes & Sources dock lists named capture profiles and the single Wayland output selected by each profile; scene operations are locked while capture is active, and the panel distinguishes the configured next source from the source used by the current session. It never implies that profiles composite multiple outputs. Peer workspace views use native tabs with clear selected states. Statistics use right-aligned fixed-width numbers and explicit units. A value absent from telemetry is an em dash plus “Not reported” in its accessible description; a reported zero is shown as zero. Do not draw charts or meters from stale or missing values.

### Forms and overlays

Settings use labeled native Qt controls, explicit ranges and units, and inline validation. Output resolution offers native capture dimensions, common presets, and validated custom sizes; custom scaling uses the currently selected two-pass GPU route. GOP/keyframe interval is always expressed in frames; avoid implying that a frame count is an exact time duration because the source presentation timing remains compositor-driven. Capture FPS options are ceilings for request pacing, never promises of an achieved or constant output rate; call out unvalidated ceiling values beside the control. Rate-control modes state which arguments are passed and which saved values are inactive. Bitrate remains the default; selectable CQP is experimental, bounded to QP 0–51, and has only been live checked at H.264 QP 28 on one NVIDIA RTX 3090 Ti setup. Its QP is inactive in bitrate mode, and the saved bitrate is omitted from CQP launches. Codec risks stay beside the codec choice, and higher-risk codecs require explicit acknowledgement without silently changing a saved choice. The settings window owns unsaved-change recovery. Confirming an application close names the active recording or replay-only session and its actual stop behavior. No system alert is used for product feedback; persistent engine errors stay next to the controls and diagnostics remain available in an optional log dock.

### Iconography

Use Qt's platform standard media icons only alongside visible action labels. Do not add external icon packs or borrowed OBS artwork. Status meaning is always written beside its marker.

### Motion

Routine dashboard updates do not animate or flash. A short fade may be used for a deliberate dialog transition, but controls and readings remain stationary. Respect the platform's reduced-motion preference; no operation depends on animation.

### Content and data visualization

Use direct, calm language. “Not reported” means the engine has not supplied a value; “Unavailable” means the engine or feature is not present; “Stale” means the last report is older than three seconds. Keep FPS, milliseconds, bytes, and Mbps explicit. Never derive unique-frame rate from capture FPS or present a target frame rate as a measured result.

## Do's and Don'ts

- **Do:** Keep the actual capture route visible when the engine reports it.
- **Do:** Show missing preview and audio sources as useful, stable empty states.
- **Don't:** Fill an unavailable state with sample video, zeroes, or animated meters.
- **Don't:** use OBS's logo, iconography, or colors as Fluxrec's brand system.
