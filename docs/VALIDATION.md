# Validation record — 2026-10-11–13

Environment: Windows x64; locally installed MinGW GCC 13.1, Qt 6.8.3, OpenCV 4.11.0. MSVC Windows CI is supplied but has not been run on a remote CI host. No projectors, calibration camera or Microsoft Flight Simulator installation were used.

## Version 0.4 guided calibration phase

The camera/pattern runner, planar solver, overlap/relative photometric estimates and reversible editor workflow are implemented. Physical calibration remains unqualified.

- Core scan tests use two translated 64×48 projector captures in one 112×48 camera frame. They recover exact source crops and overlap, reject the unilluminated background in confidence masks, attenuate the brighter measured projector, restore the original mesh through presets, and reject missing/reordered/low-contrast captures, bad fits, stale output configuration, wrong project identity and locked apply.
- Scan-process tests exercise actual child-process synthetic capture, manifest completion, reanalysis, preserving completed evidence, cancellation with no result, and malformed-sequence preflight. Synthetic results carry no physical-qualification claim.
- Camera tests verify zero-distortion pixel identity, nonzero lens remapping and rejection of mismatched intrinsics resolution, in addition to the existing ten synthetic checkerboards.
- The editor smoke test drives the real wizard through synthetic capture, before/after review, apply, Undo and Redo. It checks that Stop and Blackout cancel scanning while locked and that cancelled scans leave the project unchanged. The review screenshot is inspected for layout.
- The portable core-only build passes without OpenCV or the editor. The existing core, reliability, worker, GPU, DX12 and native adapter tests continue to run. Localhost authentication tests need networking outside the restricted sandbox; an in-sandbox network failure is retained separately from the passing regression run.
- All eleven portable CTest cases pass with localhost networking available; all five core-only cases pass. Pattern-window pixels match every Gray-code frame at 100% and 200% device pixel ratio. The wizard smoke test passes at both scales; scrollable pages keep its finish/cancel controls accessible on the smaller logical desktop at 200%.
- The packaged editor passes the complete wizard/preview smoke test with only Windows system directories on PATH. Per-directory installation includes the scan runner and records version 0.4.0; removal preserves a user-added asset. Both checks use workspace test directories and create no desktop shortcut.

Passing reports, synthetic raw captures and review screenshots are retained under [evidence/phase2](evidence/phase2/). See [guided calibration](GUIDED_CALIBRATION.md) for the installation workflow and physical completion gate. No live camera, two-projector alignment, hardware-trigger synchronization, spatial color correction or physical color-accuracy result is claimed.

## Version 0.3 reliability phase

The setup checker and output runner are implemented; hardware release qualification remains open. The current remote session exposes one 1920×1080 display at 32 Hz on an RTX 3050, driver 32.0.15.9186, Windows build 26200 / 25H2. This differs from the unavailable display enumeration in the historical 0.1/0.2 session below.

- Nine CTest cases pass: core, qualification, camera, control, worker, gpu, dx12, adapter11 and adapter12. The evaluator rejects stale capture counters, counter resets, occlusion, wrong output size, missing outputs, short duration and false physical-latency/drop claims.
- Worker tests verify control-pipe reconnection without rebuilding outputs, a wall-clock deadline with every display missing, timed simulation, cancellation, acknowledged Blackout and clean Stop.
- WGC integration captures original, warped and restored sample pixels, recovers from minimize/restore and reconnects after source application restart. Executable identity normalizes path separators. Sample state restoration and resize pass. Occluded sample Present calls do not qualify physical output.
- Live Desktop Duplication remains blocked with `0x80070005`, including a probe with desktop access outside the sandbox. No frames or visible-desktop success are claimed for that backend.
- A 60.456-second moving-window capture run fed three simulated outputs: 61 measurement samples and 232 new capture frames, capture/output health, Blackout and Stop passed. Working set grew from 60,817,408 to 61,800,448 bytes during this short run; this is not an endurance or leak qualification. The overall report correctly remains incomplete. An earlier source interruption is retained as a failed report.
- The editor smoke test at 100% and 200% scale exercises the setup dialog, writes real reports and cancels timed runs through both Stop and Blackout while configuration is locked. Physical projectors, MSFS 2020/2024, hot-plug/device loss, eight-hour endurance, physical latency and three-output 60 Hz operation remain untested.

Use the [hardware testing guide](HARDWARE_TESTING.md) to collect reports on the installation PC. Evidence is retained under [evidence/phase1](evidence/phase1/); prior failures and historical evidence remain distinguishable from current passing software checks.

## Initial foundation checks (0.1; historical session)

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
