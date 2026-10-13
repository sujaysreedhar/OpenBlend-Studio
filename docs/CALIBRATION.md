# Calibration workflows

Version 0.4 adds a [guided camera calibration workflow](GUIDED_CALIBRATION.md) for single-camera planar scans, including pattern presentation/capture, optional undistortion, confidence/error reports, relative photometric estimates and reversible apply. The original offline commands remain available. Physical screen discovery and multiple-camera registration are not implemented.

## Camera intrinsics

Build with `PS_WITH_OPENCV=ON`. Photograph a checkerboard in at least six varied orientations and positions, with fixed focus, zoom and resolution. Use more views covering the image corners in real installations. The CLI accepts inner-corner columns/rows and physical square size in meters:

```powershell
projection-cli camera intrinsics.json 9 6 0.04 board-01.pgm board-02.pgm board-03.pgm board-04.pgm board-05.pgm board-06.pgm
```

OpenCV detects corners and estimates intrinsics/distortion using `calibrateCamera`. Output includes accepted-view count and global RMS reprojection error in pixels. Failed corner detections are skipped; fewer than six successful views is an error. A low residual alone does not prove a physically accurate camera model: poorly distributed views can produce unstable distortion coefficients.

The packaged minimal OpenCV build supports PGM/BMP calibration images and DirectShow camera capture. Renderer image sources use Windows codecs independently. The guided wizard accepts the standalone intrinsics result and consistently undistorts both Gray-code and photometric captures at the calibrated resolution. The legacy offline `calibrate` command still expects consistently prepared input images.

## Planar projector mapping

`projection-cli patterns <folder> <width> <height>` writes binary PGM files in order: white, black, then complementary Gray-code bit pairs for X and Y. Do not change their ordering. Capture each projected pattern from a stationary camera with fixed exposure, focus and white balance. Place camera frames, at identical dimensions, in a separate folder named in matching sort order.

`projection-cli calibrate <capture-folder> <projector-width> <projector-height> <result.json>` decodes coordinates, rejects low-contrast pixels, fits a normalized homography with RANSAC and records residuals/inlier counts. The fitter limits its sample count for runtime. It assumes a planar screen and a camera model adequately corrected for distortion.

Use **Tools → Import planar calibration** to map the full camera image rectangle into the selected projector surface. A camera frame containing substantial space outside the illuminated screen will produce the wrong desired rectangle; crop/prepare a known planar region first. Manual mesh editing remains available after import. Save a preset before applying calibration so you can switch back.

Pure-core tests cover non-power-of-two patterns, exact Gray decoding, contrast rejection, affine homography recovery and outlier rejection. OpenCV tests render ten synthetic checkerboards and recover their known focal length within tolerance. Neither test replaces a camera plus overlapping-projector installation.

## Remaining stages

Hardware-synchronized scanning, dense mappings, cylindrical/dome/imported-mesh fitting, reference constraints, multiple-camera registration, overlap normalization and spatial photometric correction remain unimplemented. The guided scan measures five neutral RGB ramps for relative response fitting; camera limitations and the single renderer gamma control limit this estimate. Physical projector validation is a required release gate.
