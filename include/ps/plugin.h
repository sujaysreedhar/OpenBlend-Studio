/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define PS_ABI_VERSION 1
typedef enum ps_pixel_format { PS_BGRA8_SRGB = 1, PS_RGBA16_FLOAT = 2, PS_RGB10A2_PQ = 3 } ps_pixel_format;
typedef struct ps_gpu_frame {
    uint32_t struct_size, abi_version;
    uint64_t adapter_luid, shared_texture_handle, synchronization_handle;
    uint64_t timestamp_ns, frame_id;
    uint32_t width, height;
    ps_pixel_format format;
    uint32_t reserved;
} ps_gpu_frame;
typedef struct ps_pose {
    uint32_t struct_size, abi_version;
    uint64_t timestamp_ns, frame_id;
    double position_meters[3], orientation_xyzw[4];
} ps_pose;
/* In-process DX11 renderer API. Interfaces are borrowed void* COM pointers.
 * All calls for a handle must run on one thread, with no concurrent use of the
 * device's immediate context. Functions return 0 on success; errors never cross
 * the ABI. Text arguments are UTF-8. Error storage is thread-local.
 */
#ifdef _WIN32
#ifdef PS_RENDERER_EXPORTS
#define PS_API __declspec(dllexport)
#else
#define PS_API __declspec(dllimport)
#endif
#else
#define PS_API
#endif
typedef void* ps_renderer;
PS_API uint32_t ps_renderer_abi_version(void);
PS_API const char* ps_renderer_last_error(void);
PS_API int ps_renderer_create(uint32_t abi_version, void* d3d11_device, ps_renderer* result);
PS_API int ps_renderer_configure(ps_renderer, const char* project_json, const char* output_id,
                                 const char* asset_directory);
PS_API int ps_renderer_draw(ps_renderer, void* source_texture, void* target_rtv, uint32_t width,
                            uint32_t height);
PS_API void ps_renderer_destroy(ps_renderer);
#ifdef __cplusplus
}
#endif
