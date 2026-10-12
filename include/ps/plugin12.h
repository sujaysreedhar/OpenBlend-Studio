/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define PS_DX12_ABI_VERSION 1
#ifdef PS_RENDERER12_EXPORTS
#define PS12_API __declspec(dllexport)
#else
#define PS12_API __declspec(dllimport)
#endif
typedef void* ps_renderer12;
/* Borrowed COM pointers. One calling thread; direct queue must belong to device.
 * Caller retains source/target resources until the returned completion token finishes.
 * Source/target states are D3D12_RESOURCE_STATES. The renderer restores them.
 * Caller synchronizes other queues before draw; resources may alias.
 * 0 = submitted, 1 = busy (no work submitted), -1 = error. */
PS12_API int ps_renderer12_create(uint32_t version, void* device, void* queue, ps_renderer12* result);
PS12_API int ps_renderer12_configure(ps_renderer12, const char* project_json, const char* output_id,
                                     const char* assets);
PS12_API int ps_renderer12_draw(ps_renderer12, void* source, uint32_t source_state, void* target,
                                uint32_t target_state, uint64_t* completion);
/* 0 = complete, 1 = timeout, -1 = error. Completion tokens are handle-local. */
PS12_API int ps_renderer12_wait(ps_renderer12, uint64_t completion, uint32_t timeout_ms);
PS12_API void ps_renderer12_destroy(ps_renderer12);
PS12_API const char* ps_renderer12_last_error(void);
PS12_API const char* ps_renderer12_capabilities(void);
#ifdef __cplusplus
}
#endif
