# Guided camera calibration — version 0.4

Phase 2 delivers a usable single-camera workflow for planar surfaces. It projects and captures ordered Gray-code patterns, fits mappings, estimates overlaps and camera-relative RGB response, shows confidence/error maps and before/after grids, and applies changes through Undo and named presets. Physical overlapping-projector validation remains open.

## Editor workflow

1. Save the project. Stop existing output and attached adapters. Select projectors that each have one enabled surface, share the same source and use a full output viewport. Assign a unique display to each projector and match the output resolution to the native display mode. Keep the editor on a separate control monitor.
2. Open **Tools → Guided camera calibration**. Choose the projector checkboxes, DirectShow camera index, desired camera resolution, exposure in driver units, settling interval and number of frames to drain. Choose a new empty evidence folder. Existing sessions are preserved. An OpenCV-enabled build is required for camera capture.
3. Optionally select a camera intrinsics JSON from `projection-cli camera`. The session snapshots this file. Correction uses the original camera matrix at the same resolution, remaps geometry and RGB frames consistently, and rejects a resolution mismatch. Keep zoom, focus and resolution identical to the intrinsics photographs.
4. Probe the camera. Review the preview, actual resolution and driver control acknowledgements. Verify exposure, focus and white balance in the camera software; acknowledgements alone do not prove a lock. Stabilize the camera and lights. Confirm the fixed-camera checkbox.
5. Set the common desired rectangular screen region using normalized camera X/Y/Width/Height. Full-frame is the default, useful for a shared panorama. Crop to the actual desired screen region when the camera sees surrounding space. With intrinsics, these coordinates describe the undistorted image. Every selected projector is mapped into this same region; uncovered space remains uncovered.
6. Start the scan. Selected outputs stay black except the active projector. The runner bypasses existing warp/blend/color and projects raw patterns. It captures white, black, complementary X/Y Gray-code pairs, then five neutral RGB ramps (0, 64, 128, 192, 255). Pattern repaint is followed by software settling and camera-frame draining. There is no hardware trigger or proof of camera/display synchronization; lengthen settling/draining if transitions contaminate the captures.
7. Review RMS error, inliers, white clipping, confidence and error maps. At least 64 decoded/inlier pixels, 70% inliers and a fit within the configured 2-pixel default threshold are required. Dark and low-contrast camera pixels are rejected. RMS is in camera pixels and describes the accepted correspondences; inspect the error map and coverage as well.
8. Compare captured white and predicted before/after grids. The latter is a geometric prediction, not a second photograph of aligned output. Apply geometry, with optional overlap widths and photometric corrections. The editor saves **Before camera calibration** and **Camera calibration** presets and one undo step. Refine Warp, Mask, Blend and Color manually, compare presets through actual output, then save the project.

Stop/Blackout in the editor, the wizard's Stop button, Cancel, and Escape or B in a pattern window stop capture. These emergency controls work even while project configuration is locked. The process is isolated so a hung camera driver can be killed after the editor timeout. Incomplete capture evidence stays on disk and cannot be applied as a successful result. Closing the wizard also stops its runner. A failed fit can be reanalyzed after fixing its region/exposure settings; an incomplete sequence must be recaptured into a new folder.

## Geometry, overlap and photometry

Each homography maps projector pixels into camera pixels. A shared camera-region rectangle is intersected with each projector's footprint; that tile determines the source crop and inverse-mapped mesh. Output clipping handles mesh regions beyond projector bounds. Camera-space confidence images are evidence, not automatically installed as output-space masks. Existing artist masks are preserved.

Overlap reports use axis-aligned footprint intersections. Optional blend widths cover predominantly horizontal or vertical overlaps narrower than half a projector. Arbitrary perspective, diagonal and three-way intersections need manual blending. These widths do not provide automatic spatial overlap normalization or prove complementary light on a real screen.

Five neutral RGB ramp captures estimate black, white and power response for each camera channel over accepted pixels. Non-monotonic, low-contrast or clipped response measurements disable color application while keeping usable geometry. The correction attenuates brighter projectors toward the weakest measured channel level across projectors and applies one median-channel response exponent using the existing renderer controls. This assumes a fixed-exposure camera whose output is approximately sRGB; it is a relative camera estimate, not a colorimeter measurement, full color-matrix correction, black-leakage correction or spatial photometric map. Single-channel differences in gamma still require manual refinement.

Apply checks project identity, output/surface/source identities, resolutions and the saved source/output configuration. If settings changed since capture, scan again. Quality checks and full model validation run before the editor changes the project. The original project remains unchanged on failure.

## Evidence and offline use

Each session contains `session.json`, numbered `projector-N/NNN.pgm` geometry captures, five `NNN.ppm` RGB captures, optional `intrinsics.json`, camera control readbacks, `confidence.pgm`, `error.pgm` and `result.json`. Ordered filenames and capture counts are checked. Session states distinguish pending, capturing, captured, complete, cancelled, interrupted, failed and analysis_failed. Results always carry `physically_validated: false` until a separate physical validation record is collected.

```powershell
# All enabled outputs; the editor supports selecting a subset and changing camera/ROI settings.
projection-cli scan-plan installation.json scans/new-session
projection-scan --scan scans/new-session
projection-scan --analyze scans/new-session
projection-cli scan-apply installation.json scans/new-session/result.json calibrated.json --blend --color

# Software demonstration, without a camera or display presentation.
projection-cli scan-plan installation.json scans/demo
projection-scan --scan scans/demo --simulate

# Camera probe only; does not display patterns.
projection-scan --probe scans/probe --camera 0 --width 1280 --height 720 --exposure -6
```

`scan-analyze` is also available in the CLI. Reanalysis accepts complete captured evidence or an analysis_failed session; cancellation and missing/reordered captures are rejected. Offline acquisition can populate the exact manifest sequence with fixed-size PGM/PPM frames, increment each target's `captured` count, and set status to `captured` only after all frames are present. Do not mark a partially captured session complete.

## Physical completion gate

Use at least two overlapping physical projectors and one stationary camera on a known planar screen. Record projector/camera/lens models, resolution, exposure/focus/white balance, display modes, lighting, settling settings and intrinsics. Measure independent projected grid checkpoints before and after calibration, alignment residuals throughout the overlap, brightness/color differences and any clipped or unobserved regions. Retain raw captures, reports, the original/calibrated projects and comparison photographs. A low fitted residual alone cannot establish physical accuracy.

Dense curved screens, multiple-camera registration, synchronized camera triggers, spatial color/black-level maps and automatic overlap normalization remain later work. The synthetic fixtures and UI tests validate software paths only.
