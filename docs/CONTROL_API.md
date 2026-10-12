# Control and renderer interfaces

## Local HTTP and WebSocket

Off by default. Enable using Tools → Local control API. The editor chooses free ports on `127.0.0.1`, copies a fresh token to the clipboard, and shows the HTTP address. Stopping the API invalidates the token and closes connections. Tokens are not stored in projects. Requests are limited to 1 MiB; HTTP connections time out after five seconds.

Send `Authorization: Bearer <token>` on every HTTP request and on the WebSocket handshake. There are no unauthenticated mutation endpoints or query-string tokens. No LAN binding or TLS configuration is provided yet; use a separately authenticated tunnel if accessing localhost from another machine.

| Request | Result |
|---|---|
| `GET /v1/status` | Project name, output count, running/blackout/lock state, version, WebSocket port |
| `GET /v1/project` | Project-format JSON, including asset paths |
| `POST /v1/command` | JSON command; successful response `{"ok":true}` |

Commands:

```json
{"type":"blackout","enabled":true}
{"type":"stop"}
{"type":"preset","id":"saved-preset-id"}
{"type":"output","id":"output-id","enabled":false}
{"type":"project","project":{"schema_version":2,"...":"a complete valid project"}}
```

Project replacement is a complete validated document, not a merge patch. It enters the editor's undo history. Locking rejects project/preset/output edits; blackout and stop remain usable. Invalid commands return HTTP 400, missing/wrong authorization returns 401. Responses never intentionally echo tokens.

Connect to `ws://127.0.0.1:<websocket_port>` with the same authorization header. Send the commands above or `{"type":"status"}` / `{"type":"get_project"}` as text messages. Status is broadcast once per second; slow clients skip telemetry when their pending write queue grows. Application errors are JSON `{"error":"message"}`. Browser WebSocket APIs cannot set arbitrary handshake headers; use a native client or a local authenticated bridge. The server deliberately does not accept tokens in URLs.

## Editor / worker IPC

The editor launches `projection-renderer --project <runtime.json> --pipe <random-name> --status <status.json>`. `--blackout` starts black. The local named pipe carries newline-delimited UTF-8 JSON up to 32 MiB. Protocol 2 commands:

```json
{"type":"project","revision":1,"project":{"schema_version":2,"...":"full document"}}
{"type":"patch","base_revision":1,"revision":2,"patch":[{"op":"replace","path":"/outputs/0/enabled","value":false}]}
{"type":"identify","seconds":10}
{"type":"blackout","enabled":true}
{"type":"stop"}
```

Revisions increase. Invalid candidates leave the accepted project unchanged. A stale base returns `error` with current `revision` and `resync_required:true`. `ack` contains `command` and accepted `revision`. The editor coalesces edits with one project update in flight and resynchronizes on timeout. Legacy full snapshots without revision remain accepted.

`stats` includes per-output GPU, CPU submission, present interval and source-age-at-submit distributions (rolling p50/p95/p99, lifetime mean/max), resource counts/generations, occlusion, working set, GPU memory and source/recovery diagnostics. `fps` is render cycles per second, not physical refresh. Unavailable measurements are null. `dropped_frames:null` and `physical_latency_measured:false` must not be interpreted as zero latency/drops.

Escape in an output window stops the worker. Editor Stop first sends IPC, then terminates its own worker if it cannot exit in time. Closing the editor disables its adapter manifest before deleting temporary files.

## Explicit DX11 / DX12 adapters

`projection-attach <PID> <absolute-dll-path> <absolute-manifest-path>` loads only the selected process. Same architecture is required; DWM is rejected. The manifest is:

```json
{
  "enabled": true,
  "blackout": false,
  "target_hwnd": 123456,
  "output_id": "projector-id",
  "project": {"schema_version": 2, "...": "complete project"}
}
```

Adapters check changes at most ten times per second using native Windows timestamp precision. Missing/invalid configuration disables warping. `enabled:false` restores original presentation; hooks and DLL remain until process exit. All surface routes within the selected output use the application's current backbuffer as input. This does not synthesize independent simulator views or route a game's frame to other processes.

For DX11, `diagnostic_readback:true` enables low-frequency GPU readback of center/edge red values and rendered-frame count to `<manifest>.status.json`; use for testing only because it stalls the GPU. The normal path does not read frames back to CPU. No global hook, DWM injection, privilege escalation, or protected-process bypass is implemented.

## C renderer ABI, version 1

`sdk/include/ps/plugin.h` and `projection-render-api.dll` provide:

- `ps_renderer_create(version, ID3D11Device*, &handle)`
- `ps_renderer_configure(handle, project_json_utf8, output_id, assets_directory_utf8)`
- `ps_renderer_draw(handle, ID3D11Texture2D*, ID3D11RenderTargetView*, width, height)`
- `ps_renderer_destroy(handle)` and thread-local `ps_renderer_last_error()`

Call from one thread and serialize access to the device's immediate context. Source must support shader-resource views, be SDR RGBA/BGRA8, and belong to the same device as the render target. Source and target must be distinct resources. All source IDs use the supplied texture in this API. The implementation isolates/restores graphics context state. Return 0 means success; exceptions never cross the C boundary. The caller retains ownership of borrowed COM pointers and the DLL must outlive its handles.

`ps_gpu_frame` and `ps_pose` specify reserved descriptor contracts for future shared-texture and timestamped-pose integrations. **There is no shared-handle transport, plugin loader or pose-processing implementation yet.** The in-process drawing functions above are the implemented integration surface.

## DX12 queue association and C ABI

Select `projection-dx12.dll` with the same attachment command/manifest. The adapter observes `CreateSwapChain` and `CreateSwapChainForHwnd`; for DX12 their device argument is the DIRECT command queue ([Microsoft contract](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgifactory2-createswapchainforhwnd)). A late-attached chain with no observed queue remains untouched with actionable status. Integrations inside the selected process may obtain `PSRegisterD3D12Swapchain(IDXGISwapChain*, ID3D12CommandQueue*)` using `GetProcAddress`; 0 is success. Register before presenting, serialized with rendering. Device identities must match. Restart the application before switching between DX11 and DX12 adapters; do not unload active hooks.

`include/ps/plugin12.h` and `projection-render-api12.dll` provide an independent version-1 ABI. The DX11 ABI is unchanged:

- `ps_renderer12_create(version, device, direct_queue, &handle)`
- `ps_renderer12_configure(handle, project_json, output_id, assets_directory)`
- `ps_renderer12_draw(handle, source, source_state, target, target_state, &completion)`
- `ps_renderer12_wait(handle, completion, timeout_ms)`
- `ps_renderer12_destroy`, `ps_renderer12_last_error`, `ps_renderer12_capabilities`

Draw returns 0 submitted, 1 busy/no work, -1 error. Wait returns 0 complete, 1 timeout, -1 error. States use `D3D12_RESOURCE_STATES`. Use one owner thread; synchronize other queues first, and retain source/target until the token completes. States are restored; aliasing is supported. Resources must be same-device, single-sample, single-layer/mip RGBA8/BGRA8 textures. No shared-handle import or HDR is implemented.
