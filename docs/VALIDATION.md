# Validation record — 2026-10-11–12

Environment: Windows x64; locally installed MinGW GCC 13.1, Qt 6.8.3, OpenCV 4.11.0. MSVC Windows CI is supplied but has not been run on a remote CI host. No projectors, calibration camera or Microsoft Flight Simulator installation were used.

| Check | Evidence / result |
|---|---|
| Core geometry and persistence | `ps-tests` passes identity/curved interpolation, inverse mapping, selection, masks, blend normalization, sRGB roundtrips, JSON roundtrips, 16-output/256-surface model, Gray decoding, homography outliers, atomic save/backup and stale camera-file protection. |
| GPU reference | `ps-gpu-tests`, D3D11 WARP software rasterizer: identity/warped pixels, masks, linear-light blending, color response, true blackout, synthetic output capacity and C renderer ABI. |
| Camera intrinsics | `ps-camera-tests`: ten rendered checkerboards, known fx=900/fy=910 recovered approximately 900.96/911.80; RMS approximately 0.324 pixels. Distortion estimation from synthetic limited-coverage views is not a physical qualification. |
| Local control | `ps-control-tests`: missing authorization rejected, authenticated status/commands, invalid-command rejection, WebSocket exchange, token rotation. Runs outside the restricted network sandbox. |
| Native editor | `--smoke`: exercises real drag events, undo/redo and locking; saves/validates a three-output project, renders D3D preview and saves editor/preview images. Screenshot inspected for visible layout/clipping. |
| DX11 application adapter | `ps-integration-tests --adapter-only` loads the adapter into its own sample process, checks warped GPU pixels, resize to 960×600, original viewport/topology restoration and disable configuration. Sample recorded zero state errors. |
| WGC live capture | **Blocked/unqualified in this execution desktop.** Capture item/session creation succeeds outside the sandbox, but all sample presentations were `DXGI_STATUS_OCCLUDED` and WGC delivered no frames. The full integration test remains failing here; adapter-only success does not count as capture success. |
| DXGI live capture | Backend implemented; visible-desktop and capture-to-output qualification outstanding. |
| GPU performance | NVIDIA GeForce RTX 3050, three 1920×1080 offscreen outputs, 120 measured frames: GPU mean 1.398 ms, p95 3.077 ms; CPU submission mean 0.064 ms. Excludes capture/presentation; cannot certify 60 Hz output latency or dropped frames. [Raw results](evidence/benchmark.json). |
| Output process | Three simulated outputs rendered 120 frames and stopped cleanly with an explicit frame limit. This proves the worker/rendering path; the unavailable interactive display still prevents fullscreen qualification. |
| Portable distribution | Packaged app and CLI launch with a clean Windows-only PATH. Installation/removal in a workspace test directory succeeded and preserved a user-added project file. No public release/signing or other-PC compatibility claim. |
| Desktop visibility | This execution session exposes no DXGI desktop outputs, even outside the sandbox. Fullscreen and desktop-duplication qualification cannot run here. Windowed simulation falls back to the default GPU when no physical display is enumerated. |
| Simulator/projector/hardware | **Not tested:** MSFS 2020/2024, real blend/color alignment, multiple physical outputs/GPUs, HDR/stereo, projector/camera calibration, device-loss/hot-plug endurance. |

Run from the build directory after setting runtime PATH:

```powershell
ctest --output-on-failure
./ps-integration-tests.exe
./ps-integration-tests.exe --adapter-only
./projection-studio.exe --smoke ../smoke
./projection-renderer.exe --project ../../examples/three-projector.json --frames 120 --status worker-status.json
./projection-benchmark.exe --outputs 3 --width 1920 --height 1080 --frames 120 --report benchmark.json
```

The full capture integration requires a visible, unlocked interactive desktop. Do not suppress its failure or replace it with a synthetic test when qualifying a capture release. Sandbox access-denied errors are distinct from backend errors. The adapter-only test deliberately bypasses capture and records `wgc:false` in its JSON evidence.

Version 0.2 implements window identity/retry, rotation/cursor composition, target/group recovery and multiple-swapchain lifecycle handling. Their synthetic/native-sample tests do not replace physical validation. Remaining qualification includes source/application restart with visible frames, rotated monitors, real device removal, DPI/hot-plug stress, capture latency, multi-GPU synchronization and endurance.

## Version 0.2 upgrade checks

Eight CTest cases pass locally: core, camera, control, worker, gpu, dx12, adapter11 and adapter12. Control authentication requires a loopback-network-capable process. Desktop-labeled cases are omitted in headless CI; the MSVC workflow remains unexecuted remotely.

- Core: schema-1 migration with permanent original backup, revisioned patch validation/stale rejection and MSFS 2020/2024 camera fixtures.
- D3D11 GPU: existing reference images plus exact cache invalidation, four desktop rotations and all three cursor formats.
- DX12 GPU: full-image comparison against DX11, warped grids/masks/linear color/blend/blackout, in-place input/output, multiple in-flight edits and versioned ABI errors.
- Adapters: Present/Present1, untouched secondary windows, repeated resize, chain recreation, original state (DX11), explicit/observed queue association (DX12), unknown-queue passthrough and disable/re-enable. Native 100 ns file timestamps fix rapid-update loss with MinGW filesystem timestamps.
- Worker: >64 KiB snapshot, revision acknowledgements, stale-patch rejection, color edits retaining resources, selective viewport rebuild, simulated missing-display/reconnection with other outputs retained, identify/blackout/stop. Physical disconnect/device removal not tested.
- Qt smoke at 100%, 150% and 200% scale: guided setup, persistent widget/focus, drag/undo/redo/lock, zoom/pan, rectangle/numeric editing, asynchronous preview. Screenshots reviewed. The 30-event input-to-paint measurement is synthetic editor latency, not source-to-screen latency.

Raw upgrade reports are in [evidence/upgrade](evidence/upgrade/). The prior 0.1 reports above are retained as historical evidence. Portable packaging is checked separately with a Windows-only PATH; source bundles include the new adapters, SDK header and pinned dependencies.

Final 0.2 render-only benchmark: RTX 3050, 3 × 1920 × 1080, 120 measured frames; CPU submission mean **0.0358 ms** (earlier 0.1 run: 0.064 ms), GPU mean **1.928 ms**, p95 **3.892 ms**. Each output uploaded its mesh/curve and allocated its composition target only once. GPU timing varied between runs; this is not a controlled GPU-speedup claim. Capture and presentation are excluded. [Raw 0.2 benchmark](evidence/upgrade/benchmark.json).

UI smoke p95 synthetic input-to-paint: **1.027 ms at 100%**, **0.963 ms at 150%**, **1.252 ms at 200%**, 30 events each. The 200% screenshot was inspected; panels and controls remain readable without overlap. Physical-DPI monitor transitions remain a separate test. The explicit DX11 loading sample again recorded zero state errors and all 183 presents occluded; it does not qualify visible output or WGC.
