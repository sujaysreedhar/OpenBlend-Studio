# Calibration workflows

The current calibration deliverable is offline processing. It does not automatically drive a projector/camera scan, discover physical screens, or combine multiple cameras.

## Camera intrinsics

Build with `PS_WITH_OPENCV=ON`. Photograph a checkerboard in at least six varied orientations and positions, with fixed focus, zoom and resolution. Use more views covering the image corners in real installations. The CLI accepts inner-corner columns/rows and physical square size in meters:

```powershell
projection-cli camera intrinsics.json 9 6 0.04 board-01.pgm board-02.pgm board-03.pgm board-04.pgm board-05.pgm board-06.pgm
```

OpenCV detects corners and estimates intrinsics/distortion using `calibrateCamera`. Output includes accepted-view count and global RMS reprojection error in pixels. Failed corner detections are skipped; fewer than six successful views is an error. A low residual alone does not prove a physically accurate camera model: poorly distributed views can produce unstable distortion coefficients.

The packaged minimal OpenCV build supports PGM/BMP calibration images. Renderer image sources use Windows codecs independently. The standalone intrinsics result is not yet automatically consumed to undistort the Gray-code capture sequence; undistort the images consistently before fitting if lens distortion is significant.

## Planar projector mapping

`projection-cli patterns <folder> <width> <height>` writes binary PGM files in order: white, black, then complementary Gray-code bit pairs for X and Y. Do not change their ordering. Capture each projected pattern from a stationary camera with fixed exposure, focus and white balance. Place camera frames, at identical dimensions, in a separate folder named in matching sort order.

`projection-cli calibrate <capture-folder> <projector-width> <projector-height> <result.json>` decodes coordinates, rejects low-contrast pixels, fits a normalized homography with RANSAC and records residuals/inlier counts. The fitter limits its sample count for runtime. It assumes a planar screen and a camera model adequately corrected for distortion.

Use **Tools → Import planar calibration** to map the full camera image rectangle into the selected projector surface. A camera frame containing substantial space outside the illuminated screen will produce the wrong desired rectangle; crop/prepare a known planar region first. Manual mesh editing remains available after import. Save a preset before applying calibration so you can switch back.

Pure-core tests cover non-power-of-two patterns, exact Gray decoding, contrast rejection, affine homography recovery and outlier rejection. OpenCV tests render ten synthetic checkerboards and recover their known focal length within tolerance. Neither test replaces a camera plus overlapping-projector installation.

## Remaining stages

Real-time synchronized scanning, camera undistortion integration, dense mappings, cylindrical/dome/imported-mesh fitting, reference constraints, multiple-camera registration, overlap normalization and spatial photometric correction remain unimplemented. Scalar `photometric_fit` is available in the core but has no automatic measurement workflow. Physical projector validation is a required release gate.
