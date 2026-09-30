# UI freeze validation

`ui_freeze_benchmark.py` checks that a brief UI-thread/process pause does not
pause the recorder that the UI started. It launches a dedicated UI with a fresh
`XDG_CONFIG_HOME`, configured for H.264 NVENC P1, async depth 4, two-pass
conversion, a 48-buffer pool, and audio disabled. It does not read or modify the
normal Fluxrec preferences. The UI receives `--validation-auto-start`, an
opt-in argument that schedules a click on the existing Start recording button
500 ms after the window is shown. It exits with status 2 if the button is
missing or disabled; normal startup behavior is unchanged without that flag.

Run it from a Hyprland desktop with the production UI, recorder, stimulus, and
ID decoder already built:

```sh
python3 tools/ui_freeze_benchmark.py \
  --display DP-1 \
  --stimulus-seconds 16 \
  --motion 0.20 \
  --pause-after 3 \
  --pause-seconds 5 \
  --post-resume-seconds 2 \
  --post-stop-observe-seconds 1.5
```

`--motion` sets the locked plasma stimulus phase change per rendered frame. It
accepts values from 0 through 1000 and defaults to `0.20`; increase it for a
more demanding motion workload (for example, `--motion 4`). The chosen value is
recorded in the summary and checked by the stimulus health monitor.

The runner opens a new Fluxrec window and prints its PID. The validation-only
auto-start clicks **Start recording** after the window enters its event loop.
The runner verifies that the resulting `fluxrec-record` process is the UI's
same-user direct child and that its arguments select the
expected backend, depth, target output, and a new MKV inside the run directory.
It also checks that the preview socket belongs to the current user and lives in
a private mode-0700 directory under `XDG_RUNTIME_DIR`.

Once recording starts, the runner launches a locked plasma stimulus. It waits
for compositor presentation feedback and confirms that the stimulus remains
visible, fullscreen, and on the target output's active workspace. At the pause
point it rechecks both process identities, sends `SIGSTOP` to only the isolated
UI PID (never its process group), monitors stimulus/workspace health and
recorder liveness for five seconds, then sends `SIGCONT` to the same verified
process incarnation in a `finally` block. The report compares MKV size before
and after the pause to check that the recorder continued writing.

After the requested post-resume recording interval, the runner sends `SIGINT`
to the verified recorder child while the stimulus is still visibly advancing.
It observes presentation feedback and the active fullscreen workspace for the
configured post-stop interval (1.5 seconds by default), then lets the stimulus
finish normally and decodes the source IDs. This keeps stimulus teardown out
of the recorded video and verifies that source presentation continued after
the recorder stopped. The summary
and logs are written to a unique `ui-freeze-*` directory under `.validation/`
by default. The test passes only when the UI was stopped and resumed, the file
grew while it was stopped, source presentation advanced after recorder
shutdown, and decoded frames have valid content, ordered source IDs, no
internal gaps, and IDs inside the stimulus's presented range.

The UI currently consumes recorder JSON telemetry inside `QProcess` and does
not persist it to a sidecar. The companion therefore reports recorder
telemetry as unavailable instead of reading the live pipe and competing with
the UI. The Statistics panel remains available in the resumed UI; this test's
machine-readable recorder evidence is process liveness, preview-socket state,
MKV growth, and offline decoded IDs.

CPU-only guard tests run with:

```sh
python3 -m unittest tools.tests.test_ui_freeze_benchmark
```

Those tests cover `/proc` parsing, exact UI marker and executable validation,
recorder argument checks, isolated settings, preview-socket ownership, and the
timing budget that keeps recorder shutdown inside the live stimulus. They do
not launch the live UI, signal a process, or use Wayland/GPU capture.
