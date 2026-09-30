# PipeWire clock metadata probe

`pipewire_clock_probe.cpp` passively compares PipeWire clock metadata for two
explicit `Audio/Source` nodes. Select each source by its PipeWire
`object.serial`; the report also includes each node's `device.id` when
available. The probe does not choose a default source.

## Build and run

From the repository root on a system with the PipeWire development headers:

```sh
g++ -std=c++20 -O2 -Wall -Wextra -Wno-missing-field-initializers \
  -I/usr/include/pipewire-0.3 -I/usr/include/spa-0.2 -D_REENTRANT \
  tools/pipewire_clock_probe/pipewire_clock_probe.cpp \
  -lpipewire-0.3 -pthread -o pipewire-clock-probe

pw-dump | jq -r '.[] | select(.type == "PipeWire:Interface:Node" and
  .info.props["media.class"] == "Audio/Source") |
  [.info.props["object.serial"], .info.props["device.id"],
   .info.props["node.description"]] | @tsv'

./pipewire-clock-probe --source-a 58 --source-b 56 --duration-seconds 600
```

Duration is 5 through 600 seconds. The example serials are machine-specific;
inspect the current PipeWire registry before running. The program resolves
those serials to their current node IDs, creates two mono, 48 kHz PipeWire
capture streams targeted to those exact nodes, and waits until both are linked
and streaming. It then samples `pw_stream_get_time_n()`,
`pw_stream_get_nsec()`, callback `CLOCK_MONOTONIC`, and buffer timing metadata.
It dequeues and returns each buffer without mapping or inspecting its sample
payload. It creates no playback stream and writes only the final JSON report.

## What the rate fit means

For each stream the probe fits PipeWire graph-driver ticks, converted through
the reported `pw_time.rate`, against callback `CLOCK_MONOTONIC` time. It reports
the per-stream fitted rate offset and relative B-to-A rate in ppm. It also
reports source `device.id`, `clock.name`, whether the source itself is marked
as a graph driver, and its effective graph-driver ID.

The rate fit is an observation of the PipeWire graph timelines attached to
these capture streams. It is not a measurement of raw hardware oscillator
rates: PipeWire conversion, resampling, synchronization, graph scheduling, and
host scheduling can affect the observed timelines. Different `device.id`
values establish different PipeWire device objects, and different effective
graph-driver IDs establish different graph drivers; neither proves independent
physical oscillator behavior. A missing `clock.name` is reported as unknown.
In particular, a known name on one source and a missing name on the other does
not support a clock-name comparison.

On this host, the HyperX QuadCast S and Arctis Nova Pro Wireless also expose
ALSA `/proc/asound/card*/pcm0c/sub0/status` counters while PipeWire owns their
capture streams. For USB audio, `hw_ptr` follows the driver's completed frame
count: the USB PCM pointer reads `hwptr_done`, which capture URB completion
advances using the received packet lengths ([USB PCM driver](https://github.com/torvalds/linux/blob/master/sound/usb/pcm.c)).
The status `tstamp` is system timing, not a device link clock; ALSA's default
audio timestamp is derived from `hw_ptr` when the low-level driver does not
provide a supported hardware timestamp ([timestamping documentation](https://cdn.kernel.org/doc/html/latest/sound/designs/timestamping.html),
[PCM pointer/timestamp handling](https://github.com/torvalds/linux/blob/master/sound/core/pcm_lib.c)).
Both devices' capture endpoints are reported as USB `SYNC`, so these counters
can compare delivered packet progress against host time over a long run, but
cannot establish independent ADC oscillator drift for these devices.

This utility does not read, save, or analyze audio sample values; it does not
estimate acoustic or electrical A/V offset, end-to-end latency, or mouth-to-
sound sync. A valid run requires PipeWire permission to create capture streams
for both selected sources. It may activate the capture nodes while running,
but produces no audio playback.
