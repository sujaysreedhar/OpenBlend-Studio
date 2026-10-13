# Setup and hardware testing — 0.3

Version 0.3 implements the first phase of the next roadmap: setup inspection, bounded stability runs, evidence collection and capture lifecycle recovery. The physical three-output/60 Hz, eight-hour and simulator release gates remain open. Guided camera scanning and additional graphics backends are later phases.

## Check an installation

Open **Tools → Check my setup**. Stop any editor output or application adapter before starting a check. The checker snapshots the current project and inspects:

- Windows build and remote-session state; GPU names, LUIDs, driver versions and DX11/DX12 device creation.
- Attached display positions, sizes, refresh rates and rotations; projector assignments and missing image/mask assets.
- Optional three-second probes of configured WGC/window and Desktop Duplication sources. The probe records frame counts and arrival ages, without saving pixels.
- Matching running application profiles, executable versions and loaded graphics runtimes. Loaded modules do not establish the active graphics API or a DX12 swapchain's queue.

Select setup-only, one-minute, ten-minute or eight-hour tests. Begin with simulation. For hardware runs assign every tested output to a display, enable it, probe live capture and resolve blocked checks. Use a moving source on a distinct display or a dedicated application window; recursive desktop routing is rejected. The checker does not attach to applications automatically.

Simulation creates small output windows and cannot qualify the configured output resolution. A non-simulated windowed run uses the project's native pixel dimensions. Fullscreen uses assigned display regions; telemetry reports any mismatch with the expected size. Keep outputs visible throughout the measurement.

The editor remains responsive while the separate checker and renderer run. Cancel requests blackout and Stop, saves partial evidence and closes the renderer. Closing the checker also cancels. The editor’s Stop and Blackout controls cancel a running test even while the project is locked; starting another output or adapter is blocked until the test ends. The renderer has a wall-clock limit even with no available outputs; a Windows job terminates it if its supervising checker exits unexpectedly. Escape in an output window remains an emergency exit.

Reports are placed in a new timestamped folder under the chosen destination. Each contains `input-project.json` and a `results` directory. **Open report folder** opens it. Reports include system metadata and local paths; review them before sharing. Existing nonempty report directories are never overwritten.

If an attached application's adapter cannot process its swapchain, use **Tools → Use window capture for attached application**. It disables warping, adds a persistent window source, routes the selected projector's surfaces to it, and starts simulated output. Review the mapping before presenting fullscreen. The adapter DLL stays loaded until the application exits; restart before changing adapter APIs.

## Command line

From an extracted portable package:

```powershell
# Inspect and probe; no output windows are started.
./projection-qualify.exe --project installation.json --report reports/setup-001 --probe-capture

# Exercise the worker, capture, telemetry, Blackout and Stop on one monitor.
./projection-qualify.exe --project installation.json --report reports/simulation-001 --probe-capture --run --simulate --seconds 60

# On the physical installation, after the setup checks pass:
./projection-qualify.exe --project installation.json --report reports/endurance-001 --probe-capture --run --fullscreen --seconds 28800 --warmup 30 --target-hz 60
```

Duration accepts 3–86400 seconds; warm-up accepts 0–300 seconds. The measured interval starts at the first telemetry sample after warm-up. Ctrl+C/Ctrl+Break or input on the checker's stdin requests cancellation. Exit codes: `0` checks passed, `1` failed/error, `2` incomplete, `3` cancelled. A timed run always remains incomplete until external physical-delivery evidence is supplied separately; `release_qualified` stays false.

| Report | Contents |
|---|---|
| `setup.json` | Inventory, capture probe results, statuses and corrective actions |
| `project.json` | Runtime snapshot with absolute asset paths; simulation clears display assignments |
| `telemetry.jsonl` | Approximately one sample per second; GPU/CPU/Present/source-age distributions, memory, resources, errors and recoveries |
| `summary.json` | Duration, per-output submission rate, interruptions, capture health, cancellation and shutdown results |
| `worker-status.json` | Last worker state; successful shutdown records `type: stopped` |

The journal is streamed and capped at 1 GiB. Samples and summaries retain partial evidence if the run fails; abrupt termination can leave only the journal and setup report. A storage error is a failed run, never a passing endurance result.

## Interpret results

`pass` applies only to the named check. `fail` means an observed failure. `blocked` means a prerequisite or measurement was unavailable. `not_run` means the gate was not exercised. Unavailable values remain null, not zero.

Output checks require the requested dimensions, no missing output, counter reset, occlusion or error, and a successful Present-call rate of at least 95% of the requested rate. Capture checks require new frames during measurement for every routed live source and GPU that uses it; a frame received only during warm-up cannot pass. Transient recovery counters remain evidence even when the error has cleared before a telemetry sample.

GPU time measures rendering, source age measures how old a captured timestamp was when submitted, and successful Present-call rate measures submissions. None establishes physical refresh, dropped display frames or end-to-end latency. Raw DXGI frame statistics are included when available; their counters are not converted into a fabricated dropped-frame count. See Microsoft's [frame-statistics contract](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-getframestatistics).

## Physical and simulator release gates

Run three real 1920×1080 outputs at 60 Hz with a moving source. Record the project, display/projector models and modes, Windows/GPU/driver versions and cabling. Retain the one-minute baseline and an eight-hour steady-state run. Record memory growth and any errors; there is no automatic memory-leak threshold or physical qualification from software telemetry alone.

Use separate recovery runs for minimize/restore, source close/relaunch, resize, DPI/rotation changes, hot-plug, device removal, presets, Blackout and Stop. Record the time and observed recovery for each event. Expected interruptions will fail the steady-state health check; preserve those results and explain the recovery outcome separately. Use external measurement for capture-to-display latency and actual dropped frames.

For MSFS 2020 and MSFS 2024, record the simulator and aircraft versions, selected graphics API, graphics/frame-generation settings, window mode and adapter diagnostics. Verify geometry, camera configuration, resize and disabling warping independently. Test DX12 queue association explicitly; use window capture when the queue cannot be associated. Neither simulator is qualified by the bundled native samples.

Only real overlapping projectors and a camera can establish physical blend, alignment or color accuracy. The [validation record](VALIDATION.md) and [compatibility matrix](COMPATIBILITY.md) distinguish completed software checks from these outstanding gates.
