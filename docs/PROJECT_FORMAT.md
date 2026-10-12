# Project format 2

UTF-8 JSON, `schema_version: 2`. Version 1 migrates in memory; its first upgraded save preserves the original `.v1.bak` alongside the rotating `.bak`. `projection-cli validate` and the editor both use the same structural and numerical validation. Unknown schema versions are rejected. Unknown fields are currently ignored and are not preserved when saving; this format is not forward compatible.

`examples/three-projector.json` and `examples/irregular-surfaces.json` are complete examples. All IDs are stable opaque strings and must be unique in the live project. Presets refer to existing sources and retain output/surface IDs within their saved snapshot.

| Object | Fields and interpretation |
|---|---|
| Project | `id`, `name`, `locked`, `sources[]`, `outputs[]`, `presets[]`, `applications[]`, `screen`, `calibration`, `startup_preset` (empty or an existing preset ID) |
| Source | `id`, `name`, `type` = pattern/image/desktop/window; `pattern`, `image`, `display`, `window_handle`, `window_title`, `executable`, `window_class`, `width`, `height`, `cursor`. Window handles are runtime hints, not durable identities. |
| Output | `id`, `name`, `display`, design `width/height`, `enabled`, `viewport`, `blend`, `color`, `surfaces[]`. Fullscreen output uses native display pixels in `viewport`; desktop mode does not change Windows display settings. |
| Rect | `x,y,w,h` in normalized coordinates. Source crop and output viewport must be inside 0..1 with positive size. |
| Surface | `id`, `name`, `source_id`, `enabled`, `opacity`, `source_rect`, `mesh`, `mask`. Array order is compositing order. |
| Mesh | `columns`, `rows` (2..64), `curved`, row-major `points[]` of `{x,y}` destination positions. Source UVs form a regular grid. Destination coordinates may extend beyond the output and are clipped. |
| Mask | `enabled`, `inverted`, `feather`, `polygon[]`, `bitmap`. Coordinates are local surface UV. Polygon and bitmap multiply; inversion applies to the combined mask. Bitmap alpha/luminance are interpreted by the WIC mask loader. |
| Blend | `left,right,top,bottom` overlap widths, `gamma`, `black_level`, `curve[]` with increasing x and endpoints 0/1. Widths are fractions of output dimensions. |
| Color | linear-light RGB `gain[3]`, `offset[3]`, `brightness`, `contrast`, `saturation`, `gamma`. Output is encoded to SDR sRGB. |
| Preset | `id`, `name`, `outputs[]` snapshot. Changing a preset does not create/delete sources. |
| Application | `id`, `name`, `executable` full path or filename, `api` capture/dx11/dx12, `simulator_version` empty/2020/2024, `output_id`, `enabled`, `excluded`. Exclusions are checked by the editor before attaching. |
| Screen | `shape` plane/cylinder, `width`, `height`, `radius` in meters, `arc_degrees`, `eye:{x,y,z}`. Cylinder export currently assumes a centered eyepoint. |
| Calibration | Metadata object, including model, dimensions, homography and residuals for the planar importer. `physically_validated` remains false until measured. |

The camera coordinate origin is the top left, x right and y down. Mesh coordinates use the same orientation. Frustum exports separately document a right-handed 3D convention and matrix storage order.

Paths are UTF-8, absolute or relative to the project directory. Save-as to another directory resolves existing assets to absolute paths. There is no proprietary format conversion. Files larger than 32 MiB, non-finite values, duplicate live IDs, invalid meshes and desktop feedback routes are rejected.

Saves write a sibling temporary file, flush it, copy the previous file to `.bak`, then replace the primary file atomically (`MoveFileExW` with write-through on Windows). On failure the old primary stays available where the OS permits; leftovers are cleaned up. Network filesystems and sudden power loss are not qualified. Restoring a backup also saves the displaced primary as the new backup.

Rendering order: source sample/crop → sRGB decode → warped premultiplied surface/mask composition in RGBA16F → output blending and manual black-floor adjustment → color response → sRGB encode. Mesh tessellation and mask construction currently run on CPU; ordinary frame composition stays on GPU. Editor previews use readback and are not representative of output performance.

Opening a project applies `startup_preset` when specified. Profiles operate only on explicit application attachment. Sources reconnect only to unique compatible executable/class/title matches. `examples/msfs-2024.json` demonstrates schema 2; the earlier examples also exercise legacy migration.
