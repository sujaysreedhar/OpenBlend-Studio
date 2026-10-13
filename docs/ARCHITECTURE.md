# Implementation map

| Component | Responsibility |
|---|---|
| `include/ps`, `src/core` | UI-independent project model, JSON persistence, geometry/interpolation, mask/blend math, camera calibration and simulator exports |
| `src/editor` | Qt dockable editor, mesh interactions, patch-based undo, runtime process control and authenticated API |
| `src/windows/d3d11_renderer.cpp`, `shaders.hpp` | D3D11 resources and HLSL warp/compose/photometric pipeline; reused in preview, output process, C ABI and application hook |
| `desktop_capture.cpp`, `window_capture.cpp` | DXGI duplication and free-threaded WGC sessions, GPU frame copying and loss diagnostics |
| `setup_check.cpp`, `qualify.cpp`, `qualification.cpp` | GPU/display/capture inspection; supervised timed runs, bounded telemetry journal and conservative evidence evaluation |
| `worker.cpp` | Named-pipe command handling, GPU groups, output windows, swapchains and frame pacing in a separate process |
| `dx11_adapter.cpp`, `attach.cpp` | Explicit application selection/loading, Present/Present1/resize interception and full D3D11 context-state isolation |
| `renderer_api.cpp`, `include/ps/plugin.h` | Versioned C interface for application-owned D3D11 textures and render targets |
| `src/cli` | Project validation/examples, calibration processing, frustum export and camera backup restoration |
| `tests` | Portable math/persistence checks, software-GPU comparisons, synthetic camera calibration, local API and native application integration |

The editor sends a validated full snapshot, then revisioned JSON patches with acknowledgements. Stale or invalid patches leave accepted state unchanged and request full resynchronization. Geometry/color edits update rendering without rebuilding windows; assignment/viewport changes rebuild only the affected target. Source changes invalidate only their capture/image entries. Display enumeration and device checks retry affected targets/groups. External asset paths are made absolute in the editor's temporary runtime project. Rendering commands stay local; remote clients talk to the editor, not directly to a worker.

The renderer composites linear-light premultiplied surfaces into RGBA16F and then applies projector blending/color to an SDR backbuffer. Mesh tessellation, mask generation and curve uploads are cached by complete input values; constants update independently. Preview runs on a dedicated thread at up to 30 Hz with three reusable staging buffers and nonblocking readback. Requests/deliveries are bounded and obsolete requests are replaced. Outputs use independent DXGI waitable swapchains, refresh-aware fallback pacing and occlusion probes. This is not multi-projector genlock.

The adapter uses a separate context-state object while rendering, then restores the application state. Resources are keyed by swapchain and rebuilt on resize. An invalid manifest or unsupported SDR format disables warping without intentionally stopping the application. Release/resize interception retires per-swapchain state. Disabling leaves the DLL/hooks installed until process exit.

No public API promises universal games, cross-adapter shared textures, frame interpolation, HDR/stereo, virtual-display installation or proprietary file compatibility. The feature matrix identifies which interfaces have implementations and which descriptors only reserve future contracts.

## DX12 and telemetry

`d3d12_renderer.cpp` shares shaders and asset generation with DX11 but uses native root signatures, PSOs, heaps, command lists and fences. Three in-flight slots retain private intermediate/upload resources until completion; busy slots return without submitting. Borrowed source/target resources remain caller-owned until completion. States are restored and in-place rendering uses a private copy.

`dx12_adapter.cpp` observes factory creation and associates only the actual DIRECT queue, or accepts explicit registration. Unknown queues pass through with diagnostics. It submits its own list on that queue. Only SDR RGBA8/BGRA8 output is supported.

Telemetry separates CPU submission, GPU timestamps, present intervals, source age and available DXGI presentation statistics. Unavailable dropped-frame measurements stay null. Source age is not physical capture-to-photon latency. See [API contracts](CONTROL_API.md) and [qualification](VALIDATION.md).

The setup dialog launches `projection-qualify` asynchronously. The checker snapshots the project, runs prerequisites and supervises its worker in a Windows kill-on-close job. Timed runs preserve setup, telemetry and final evidence separately. Report evaluation is in the UI-independent core; physical delivery remains an explicit unmeasured gate. See [hardware testing](HARDWARE_TESTING.md).

The calibration wizard launches `projection-scan` asynchronously in a kill-on-close Windows job. Camera access is isolated from the editor. Raw Qt pattern windows bypass saved mapping/color; selected outputs are black between individual scans. Native display dimensions account for device pixel ratio. Camera capture uses OpenCV DirectShow with configurable software settling/frame draining and recorded control acknowledgements. A cancellation marker requests graceful shutdown; the editor kills an unresponsive runner after a bounded timeout.

`scan.cpp` in the portable core validates ordered evidence, computes projector-to-camera homographies, camera-space confidence/error maps, shared-region meshes/source crops, overlap and relative RGB-response estimates. OpenCV optionally remaps geometry and RGB captures using session-local intrinsics. Apply checks saved project/output/source identity and settings, validates quality/model constraints, and returns a new project with before/after presets; the editor commits it through Undo. Physical qualification stays separate. See [guided calibration](GUIDED_CALIBRATION.md).
