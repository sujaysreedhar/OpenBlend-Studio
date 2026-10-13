# Phase 2 software evidence — 2026-10-13

These files exercise the guided planar calibration implementation in version 0.4. They do not qualify a physical installation.

- `phase2-ctest-passed.xml`: all eleven portable CTest cases pass, including scan core/process, camera, existing reliability/worker/GPU/adapter and localhost-control tests. This run allowed localhost networking.
- `phase2-core-ctest.xml`: all five core-only cases pass without OpenCV or the editor.
- `synthetic-scan`: a complete two-projector synthetic session driven through the actual wizard and scan child process. Ordered raw PGM/PPM captures, confidence/error maps, output/source snapshots and calibration result are retained. The 112×48 camera frame observes two 64×48 projector patterns with shared overlap. No camera or display presentation occurred during this synthetic scan.
- `ui-200-result.json`: wizard capture/review/apply, before/after presets, Undo/Redo and Stop/Blackout cancellation while locked pass at 200% scaling. Existing editor checks also pass.
- `calibration-100.png`, `calibration-200.png`: inspected review-screen screenshots. Scrollable pages keep wizard navigation accessible at 200% scaling.
- `cli-result.json`: reanalysis, CLI apply with blend/color estimates and project validation pass; two reversible presets are saved.
- `packaged-ui-result.json`, `install-result.json`: the portable package passes the editor/wizard smoke test with toolchain directories removed from PATH; installation includes the scan runner, and removal preserves a user-added file.

The restricted initial regression run failed the localhost-network test and revealed scan-runner bugs in Qt argument handling and Windows manifest-file sharing. Those runner issues were fixed and the passing run above supersedes them; the initial logs remain in the local build directory. Physical camera exposure lock, synchronization, projector alignment and photometric accuracy remain unqualified. See [the physical completion gate](../../GUIDED_CALIBRATION.md#physical-completion-gate).
