# Three-phase upgrade — version 0.2.0

Software changes are implemented across performance, UI and compatibility. Physical-projector and simulator release gates remain open; see [validation](VALIDATION.md).

| Phase | Delivered behavior | Evidence |
|---|---|---|
| 1: Performance | Mesh/mask/curve caches, linear-time feather filter, bounded 30 Hz preview thread/three staging buffers, per-output waitable pacing, revisioned IPC, selective target/capture invalidation and retry, async process control, timing/memory diagnostics | Cache/pixel assertions, large snapshot/stale patch/selective rebuild/simulated recovery worker tests, offscreen benchmark |
| 2: UI | Persistent controls/focus, zoom/pan/fit, rectangle/group selection, numeric coordinates, snapping, source/output wipe, search, setup wizard, profile/preset/startup management, shortcuts/dock layouts, diagnostics export, visible Stop/Blackout/Identify | Native Qt setup/edit/focus/undo/lock/preview smoke tests and DPI screenshots |
| 3: Support | Native DX12 renderer and separate C ABI, observed/registered queues, DX11/DX12 Present/Present1/resize/recreation/disable, desktop rotation/cursor shaders, persistent window identity, separate MSFS profiles/camera preview/backup/restore, schema-1 migration | DX12/DX11 pixel comparisons, synthetic rotation/cursor cases, adapter tests, core/camera fixtures |

DX12 never guesses an unknown command queue. Simulator profiles are configuration assistance, not simulator qualification. Only SDR/x64 are supported. Capture, physical hot-plug/device loss, multi-projector pacing and long-running stability need visible hardware tests. Source-age telemetry is not capture-to-photon latency; unavailable dropped-frame counts are null.

OpenGL/Vulkan/legacy APIs, virtual display drivers, cross-GPU transport, automated curved/multi-camera scanning, HDR/stereo and interpolation remain later roadmap work.

## Reproduce

```powershell
. ./scripts/env.ps1
cmake --build --preset portable --parallel 4
ctest --preset portable
./build/portable/projection-studio.exe --smoke build/smoke
./build/portable/projection-benchmark.exe --outputs 3 --width 1920 --height 1080 --frames 120 --report build/benchmark.json
```

Control tests need localhost networking. Desktop-labeled tests require Windows presentation support; headless CI excludes them with `-LE desktop`. Run omitted tests on a desktop before qualifying that machine. `ps-integration-tests --adapter-only` separately checks explicit loading into our own sample process.
