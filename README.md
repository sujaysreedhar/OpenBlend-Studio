# OpenBlend Studio

**Open-source screen warping, projector edge blending, and projection mapping for Windows.**

Turn overlapping projector images into one continuous display. OpenBlend Studio provides tools to shape projected images, soften their overlap, mask unwanted regions, and adjust color across outputs. It is intended for multi-projector installations, immersive displays, simulator setups, and projection experiments.

Written in C++20 with Qt 6, Direct3D 11/12 and HLSL, with optional OpenCV calibration tools. Application windows, executables and packages use the name **Projection Studio**. Code is **GPL-3.0-only**.

## Status: 0.2.0 experimental

The three-phase upgrade adds resource caching and asynchronous previews, a guided setup plus precise workbench, and native DX12 integration. Manual editing/rendering, schema migration and application samples have automated checks. Live capture, physical projectors and Microsoft Flight Simulator compatibility still require hardware testing. See the [upgrade record](docs/UPGRADE.md), [feature checklist](docs/FEATURES.md) and [validation evidence](docs/VALIDATION.md).

## Features

- Warp grids with linear/curved interpolation, point/group/row/column/boundary selection, axis locks and undo/redo.
- Zoom, pan, fit, rectangle selection, numeric coordinates, snapping and source/output comparison.
- Linear-light edge blending, editable curves, polygon/bitmap masks, feathering, color response and manual black-level adjustment.
- Pattern/image/window/desktop sources, cropping, independent surface routing, fullscreen or simulated outputs.
- Guided setup, profile management, startup presets, editable shortcuts, saved dock layout, output identification, blackout and emergency stop.
- Cached GPU resources, preview thread with three staging buffers, per-output pacing, revisioned IPC, selective rebuild/retry and diagnostics export.
- Opt-in DX11/DX12 adapters; DX12 command queues must be observed or explicitly registered. Both are SDR and x64 only.
- Separate MSFS 2020/2024 profiles and camera preview/backup/restore, generic frustum exports, localhost HTTP/WebSocket control and versioned C renderer APIs.
- Offline Gray-code/planar calibration and optional OpenCV checkerboard intrinsics. Physical calibration remains unqualified.

## Quick start

Extract the portable package and run `projection-studio.exe`. First launch opens guided setup. No driver, service or administrator installation is required.

1. Use **Setup** to choose source, geometry and displays. Begin with patterns and simulated outputs.
2. Select a surface in **Installation**. In **Warp**, drag points, Ctrl-select, drag empty space for rectangle selection, and use arrow keys to nudge (Shift for larger steps).
3. Wheel to zoom, middle-drag to pan, F/Home to fit. Numeric coordinates translate the selected group; snapping can be bypassed with Alt while dragging.
4. Use **Source**, **Mask**, **Blend** and **Color** to tune routing and output. Use the source/output wipe to compare the original and mapped image.
5. Choose **Simulate outputs**, or assign displays and choose **Present fullscreen**. Keep capture sources separate from output areas.
6. Save the JSON project and its referenced assets. Tools manages application profiles, presets/startup and shortcuts.

**Stop**, Escape in an output window, and Ctrl+Shift+Escape in the editor stop output. **Blackout** (Ctrl+B by default) clears outputs. These remain available while configuration is locked. **Identify** temporarily numbers the outputs.

Window sources remember executable, class and title as well as a runtime handle; unique compatible windows can reconnect. Ambiguous matches require selection. Saves retain `.bak`; migrating a schema-1 file also preserves `.v1.bak`. [Project format](docs/PROJECT_FORMAT.md).

## Build

Requirements: Windows x64, Python/pip, CMake/Ninja, and either Visual Studio 2022 C++ with Windows SDK 10 or the downloaded MinGW toolchain. Bootstrap pins Qt 6.8.3, CMake 3.31.6, Ninja 1.11.1.3 and optional OpenCV 4.11.0.

From an x64 Visual Studio Developer PowerShell:

```powershell
./scripts/bootstrap.ps1 -Compiler msvc -WithOpenCV
$env:PATH = "$PWD/.tools/opencv-msvc/x64/vc17/bin;" + $env:PATH
cmake --preset windows-msvc -DPS_WITH_OPENCV=ON -DOpenCV_DIR="$PWD/.tools/opencv-msvc"
cmake --build --preset windows-msvc --parallel 4
ctest --preset windows-msvc
```

Locally tested MinGW GCC 13.1 build:

```powershell
./scripts/bootstrap.ps1 -Compiler mingw -WithOpenCV
. ./scripts/env.ps1
cmake --preset portable -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ -DPS_WITH_OPENCV=ON -DOpenCV_DIR="$env:OpenCV_DIR"
cmake --build --preset portable --parallel 4
ctest --preset portable
./build/portable/projection-studio.exe
```

OpenCV is optional: omit its bootstrap/configuration arguments for manual mapping and Gray-code processing. The minimal OpenCV build reads PGM/BMP calibration images; the Windows renderer uses WIC for PNG/JPEG and other image sources. The `core` preset builds the UI-independent core/CLI; Linux rendering remains future work. [MSVC CI](.github/workflows/windows.yml) is supplied but has not been run remotely here.

## Application and simulator integration

Use **Tools → Application profiles** to select capture, DX11 or DX12, projector and exclusions. Then explicitly select a running window using **Tools → Attach application**. Stop disables processing; the hook DLL remains loaded until the application exits. Restart the application before switching between DX11 and DX12 adapters.

DX12 observes the DIRECT queue of swapchains created after attachment. Existing chains require explicit registration or recreation. Unknown queues pass through with diagnostics; use capture when an application offers neither option. A DLL-load success is not proof of warped output. Both adapters support SDR RGBA8/BGRA8; our native samples cover Present/Present1, multiple chains, resize, recreation and disable/re-enable.

MSFS 2020/DX11 and 2024/DX12 profiles are disabled until configured. **Tools → MSFS camera export** selects the version, previews an appended camera and creates a backup before applying. **Restore MSFS camera backup** previews restoration and checks for concurrent changes. Close MSFS before applying edits to an editable `cameras.cfg`. Packaged/encrypted aircraft files are unsupported.

Neither simulator was tested here. Zoom needs simulator-specific calibration. Camera exports and image warping cannot generate missing viewpoints or enable multi-view rendering. [Compatibility and qualification](docs/COMPATIBILITY.md).

## CLI, calibration and control

```powershell
./projection-cli.exe example installation.json 3
./projection-cli.exe validate installation.json
./projection-cli.exe views installation.json frustums.json
./projection-cli.exe patterns patterns 1920 1080
./projection-cli.exe calibrate captured-patterns 1920 1080 planar-result.json
./projection-cli.exe restore path/to/cameras.cfg
./projection-benchmark.exe --outputs 3 --width 1920 --height 1080 --frames 120 --report benchmark.json
```

Pattern scanning is offline: project numbered patterns and record matching exposures with fixed exposure/focus. Import the planar fit through Tools. Camera intrinsics require at least six distinct checkerboard views. Synthetic tests validate math, not physical alignment. [Calibration guide](docs/CALIBRATION.md).

Enable **Tools → Local control API** for authenticated localhost HTTP/WebSocket editing and telemetry. It is disabled by default and uses a fresh token. [Control, IPC and C ABI contracts](docs/CONTROL_API.md), [architecture](docs/ARCHITECTURE.md).

## Validation and packaging

CTest covers core/persistence/schema migration, camera fixtures, authentication, D3D11/DX12 pixels, adapters and worker recovery. Tests labeled `desktop` require Windows presentation support; headless CI excludes them with `-LE desktop`. Live capture and remote loading also have separate checks:

```powershell
# From the build directory with runtime PATH configured
./ps-integration-tests.exe
./ps-integration-tests.exe --adapter-only
./projection-studio.exe --smoke ../smoke
```

The full integration test needs a visible, unlocked desktop. Offscreen timing excludes capture and presentation, and cannot qualify physical 60 Hz output. [Validation record](docs/VALIDATION.md) distinguishes passing checks from unqualified hardware.

Run `./scripts/package.ps1 -SourceBundle` after building to produce portable and corresponding-source ZIPs. The extracted package includes per-user install/uninstall scripts, examples, SDK headers, docs and dependency notices. The source bundle includes available pinned dependency archives and SHA256 hashes. No signed installer or driver is provided.

## License

Copyright (C) 2026 Sujay Sreedhar and contributors.

Project-authored code and documentation are licensed under the **GNU General Public License, version 3 only (`GPL-3.0-only`)**. See [LICENSE](LICENSE) for the full terms and [COPYRIGHT](COPYRIGHT) for the project notice. The software is provided without warranty.

Third-party components retain their own copyrights and licenses; see [dependency notices](THIRD_PARTY_NOTICES.md) and the [licenses directory](licenses/).

This is an independent implementation. No Fly Elise source/assets or proprietary format/protocol compatibility is included.
