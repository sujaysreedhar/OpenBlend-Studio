# Compatibility and simulator setup

| Path | Implemented behavior | Qualification |
|---|---|---|
| DX11 x64 SDR | Explicit attach, Present/Present1, resize/recreation, context restoration, multiple chains | Own samples pass; MSFS 2020 and games unqualified |
| DX12 x64 SDR | Native renderer, observed/registered DIRECT queue, Present/Present1, resize/recreation, multiple chains | Pixel and adapter samples pass; MSFS 2024 unqualified |
| Desktop Duplication | GPU rotation 0/90/180/270 degrees and color/monochrome/masked cursor composition | Synthetic GPU checks pass; live rotated monitors unqualified |
| WGC | Window identity and reconnect/retry, optional cursor | Session creation works; no frames in this occluded desktop |
| MSFS 2020/2024 cameras | Version selection, append preview, stale-file guard, backup/restore | Fixture tests pass; simulator validation outstanding |
| GL/Vulkan/DX9/10, x86, HDR/stereo | Future backends | Unsupported |

## Simulator workflow

1. Align test patterns in simulation before assigning physical outputs.
2. Configure an application profile with executable, target projector and matching API. The default 2020/DX11 and 2024/DX12 profiles are disabled until configured. Exclusions take precedence.
3. Explicitly attach the running application and inspect diagnostics. DX12 unknown queues require recreation/registration; use capture when neither is available. DLL loading alone does not prove warped output.
4. Set screen/eye geometry and export generic frustums. Tools → MSFS camera export selects the version and previews a camera appended to an editable aircraft `cameras.cfg`. Close the simulator before applying. Both versions use documented `InitialPbh` and `InitialZoom` fields; this does not enable multi-view rendering. Packaged/encrypted aircraft files are unsupported.
5. Tools → Restore MSFS camera backup previews restoration. Apply checks for concurrent changes. The CLI also supports `projection-cli restore cameras.cfg`.

Orientation, zoom and image warping cannot generate missing viewpoints. Exact zoom/FOV and aircraft/multiple-view behavior must be verified in the selected simulator. References: [2020 camera fields](https://docs.flightsimulator.com/html/Content_Configuration/Cameras/Cameras_CFG/cameras_cfg.htm), [2024 camera fields](https://docs.flightsimulator.com/msfs2024/retail/content-configuration/cfg-files/cameras.cfg/).

## External qualification

Record Windows build, simulator/application/aircraft version, API/settings, GPU/LUID, driver, display models, resolution, rotation and refresh. Measure capture availability, latency, present intervals, actual drops and memory over an endurance run. Exercise resize, minimize, DPI changes, source exit/restart, display disconnect/reconnect, device removal, presets, blackout and Stop. Simulated recovery, offscreen timings and WARP pixels do not qualify physical tests.
