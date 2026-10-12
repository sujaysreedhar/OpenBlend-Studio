// SPDX-License-Identifier: GPL-3.0-only
#define PS_RENDERER12_EXPORTS
#include "ps/plugin12.h"
#include "ps/d3d12_renderer.hpp"
#include <thread>
namespace {
thread_local std::string error;
struct Handle {
    ps::win::Renderer12 renderer;
    ps::Project project;
    std::string output;
    std::thread::id owner{std::this_thread::get_id()};
    Handle(ID3D12Device* device, ID3D12CommandQueue* queue) : renderer(device, queue) {}
};
Handle& get(ps_renderer12 handle) {
    if (!handle)
        throw std::runtime_error("Null DX12 renderer handle");
    auto& h = *static_cast<Handle*>(handle);
    if (h.owner != std::this_thread::get_id())
        throw std::runtime_error("DX12 renderer used from a different thread");
    return h;
}
template <class F> int invoke(F f) {
    try {
        error.clear();
        return f();
    } catch (const std::exception& e) {
        error = e.what();
        return -1;
    } catch (...) {
        error = "Unknown DX12 renderer error";
        return -1;
    }
}
} // namespace
int ps_renderer12_create(uint32_t version, void* device, void* queue, ps_renderer12* result) {
    if (result)
        *result = nullptr;
    return invoke([&] {
        if (version != PS_DX12_ABI_VERSION || !device || !queue || !result)
            throw std::runtime_error("Incompatible DX12 ABI or invalid device/queue");
        *result = new Handle(static_cast<ID3D12Device*>(device), static_cast<ID3D12CommandQueue*>(queue));
        return 0;
    });
}
int ps_renderer12_configure(ps_renderer12 handle, const char* json, const char* output, const char* assets) {
    return invoke([&] {
        auto& h = get(handle);
        if (!json || !output)
            throw std::runtime_error("Missing DX12 project/output");
        auto project = ps::project_from_json(ps::Json::parse(json));
        if (std::none_of(project.outputs.begin(), project.outputs.end(),
                         [&](const auto& o) { return o.id == output; }))
            throw std::runtime_error("Unknown DX12 output");
        h.renderer.set_asset_directory(assets ? std::filesystem::u8path(assets)
                                              : std::filesystem::current_path());
        h.project = std::move(project);
        h.output = output;
        return 0;
    });
}
int ps_renderer12_draw(ps_renderer12 handle, void* source, uint32_t source_state, void* target,
                       uint32_t target_state, uint64_t* completion) {
    if (completion)
        *completion = 0;
    return invoke([&] {
        auto& h = get(handle);
        if (!completion)
            throw std::runtime_error("Missing DX12 completion pointer");
        auto it = std::find_if(h.project.outputs.begin(), h.project.outputs.end(),
                               [&](const auto& o) { return o.id == h.output; });
        if (it == h.project.outputs.end())
            throw std::runtime_error("Configure the DX12 renderer before drawing");
        *completion =
            h.renderer.render(*it, static_cast<ID3D12Resource*>(source), D3D12_RESOURCE_STATES(source_state),
                              static_cast<ID3D12Resource*>(target), D3D12_RESOURCE_STATES(target_state));
        return *completion ? 0 : 1;
    });
}
int ps_renderer12_wait(ps_renderer12 h, uint64_t completion, uint32_t timeout) {
    return invoke([&] { return get(h).renderer.wait(completion, timeout) ? 0 : 1; });
}
void ps_renderer12_destroy(ps_renderer12 handle) {
    invoke([&] {
        if (handle)
            delete &get(handle);
        return 0;
    });
}
const char* ps_renderer12_last_error() {
    return error.c_str();
}
const char* ps_renderer12_capabilities() {
    return R"({"api":"dx12","abi":1,"formats":["rgba8-unorm","bgra8-unorm"],"same_device":true,"in_place":true,"hdr":false,"external_queue_required":true})";
}
