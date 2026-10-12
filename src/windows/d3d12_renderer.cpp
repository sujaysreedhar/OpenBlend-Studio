// SPDX-License-Identifier: GPL-3.0-only
#include "ps/d3d12_renderer.hpp"
#include "ps/geometry.hpp"
#include "shaders.hpp"
#include <d3dcompiler.h>
#include <array>
#include <cstring>
#include <algorithm>
namespace ps::win {
namespace {
struct Constants {
    float crop[4], params[4], widths[4], response[4], gain[4], offset[4], adjustments[4];
};
Com<ID3DBlob> compile(const char* entry, const char* profile) {
    Com<ID3DBlob> code, errors;
    auto hr = D3DCompile(shaders, sizeof(shaders) - 1, nullptr, nullptr, nullptr, entry, profile,
                         D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, code.put(), errors.put());
    if (FAILED(hr))
        throw std::runtime_error(errors ? static_cast<const char*>(errors->GetBufferPointer())
                                        : "DX12 shader compilation failed");
    return code;
}
bool same_object(IUnknown* a, IUnknown* b) {
    Com<IUnknown> x, y;
    check(a->QueryInterface(__uuidof(IUnknown), reinterpret_cast<void**>(x.put())), "COM identity");
    check(b->QueryInterface(__uuidof(IUnknown), reinterpret_cast<void**>(y.put())), "COM identity");
    return x.get() == y.get();
}
} // namespace
struct Renderer12::Impl {
    Com<ID3D12Device> device;
    Com<ID3D12CommandQueue> queue;
    Com<ID3D12RootSignature> root;
    Com<ID3D12PipelineState> warp, rgba, bgra;
    Com<ID3D12Fence> fence;
    HANDLE event{};
    std::uint64_t next_fence{1};
    struct Frame {
        Com<ID3D12CommandAllocator> allocator;
        Com<ID3D12GraphicsCommandList> list;
        Com<ID3D12DescriptorHeap> views, targets;
        Com<ID3D12Resource> constants;
        unsigned char* mapped{};
        std::uint64_t submitted{};
        UINT descriptor{}, constant{};
        std::vector<Com<ID3D12Resource>> retained;
        ~Frame() {
            if (mapped && constants)
                constants->Unmap(0, nullptr);
        }
    };
    std::array<Frame, 3> frames;
    Frame* current{};
    UINT view_stride{}, target_stride{};
    std::filesystem::path assets;
    struct MeshEntry {
        Mesh value;
        Com<ID3D12Resource> buffer;
        UINT count{};
    };
    struct MaskEntry {
        Mask value;
        Com<ID3D12Resource> texture;
    };
    std::unordered_map<std::string, MeshEntry> meshes;
    std::unordered_map<std::string, MaskEntry> masks;
    std::vector<Vec2> curve_points;
    Com<ID3D12Resource> curve, copy, intermediate;
    D3D12_RESOURCE_DESC target_desc{};
    RenderCounters counters;
    explicit Impl(ID3D12Device* d, ID3D12CommandQueue* q) : device(d), queue(q) {
        if (!d || !q || q->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
            throw std::runtime_error("DX12 requires a device and direct queue");
        Com<ID3D12Device> queue_device;
        check(q->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(queue_device.put())),
              "DX12 queue device");
        if (!same_object(d, queue_device.get()))
            throw std::runtime_error("DX12 queue belongs to another device");
        D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, 0};
        D3D12_ROOT_PARAMETER parameters[2]{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        parameters[0].Descriptor.ShaderRegister = 0;
        parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[1].DescriptorTable = {1, &range};
        parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.MaxAnisotropy = 1;
        sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC signature{2, parameters, 1, &sampler,
                                            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
        Com<ID3DBlob> serialized, errors;
        check(D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, serialized.put(),
                                          errors.put()),
              "DX12 root signature serialization");
        check(d->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                     __uuidof(ID3D12RootSignature), reinterpret_cast<void**>(root.put())),
              "DX12 root signature");
        auto vs = compile("warpVS", "vs_5_0"), ps = compile("warpPS", "ps_5_0"),
             fs = compile("fullVS", "vs_5_0"), out = compile("outputPS", "ps_5_0");
        D3D12_INPUT_ELEMENT_DESC elements[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
        D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};
        p.pRootSignature = root.get();
        p.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        p.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        p.InputLayout = {elements, 2};
        p.SampleMask = UINT_MAX;
        p.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        p.SampleDesc.Count = 1;
        p.NumRenderTargets = 1;
        p.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        p.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        p.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        p.RasterizerState.DepthClipEnable = TRUE;
        p.DepthStencilState.DepthEnable = FALSE;
        p.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        auto& b = p.BlendState.RenderTarget[0];
        b.BlendEnable = TRUE;
        b.SrcBlend = D3D12_BLEND_ONE;
        b.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        b.BlendOp = D3D12_BLEND_OP_ADD;
        b.SrcBlendAlpha = D3D12_BLEND_ONE;
        b.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        b.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        check(d->CreateGraphicsPipelineState(&p, __uuidof(ID3D12PipelineState),
                                             reinterpret_cast<void**>(warp.put())),
              "DX12 warp pipeline");
        p.InputLayout = {};
        p.VS = {fs->GetBufferPointer(), fs->GetBufferSize()};
        p.PS = {out->GetBufferPointer(), out->GetBufferSize()};
        b.BlendEnable = FALSE;
        p.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        check(d->CreateGraphicsPipelineState(&p, __uuidof(ID3D12PipelineState),
                                             reinterpret_cast<void**>(rgba.put())),
              "DX12 output pipeline");
        p.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
        check(d->CreateGraphicsPipelineState(&p, __uuidof(ID3D12PipelineState),
                                             reinterpret_cast<void**>(bgra.put())),
              "DX12 BGRA output pipeline");
        check(d->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence),
                             reinterpret_cast<void**>(fence.put())),
              "DX12 fence");
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event)
            throw std::runtime_error("DX12 fence event");
        view_stride = d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        target_stride = d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        for (auto& f : frames) {
            check(d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator),
                                            reinterpret_cast<void**>(f.allocator.put())),
                  "DX12 allocator");
            check(d->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, f.allocator.get(), nullptr,
                                       __uuidof(ID3D12GraphicsCommandList),
                                       reinterpret_cast<void**>(f.list.put())),
                  "DX12 command list");
            check(f.list->Close(), "DX12 initial command list");
            D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1024,
                                            D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
            check(d->CreateDescriptorHeap(&heap, __uuidof(ID3D12DescriptorHeap),
                                          reinterpret_cast<void**>(f.views.put())),
                  "DX12 descriptors");
            heap = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
            check(d->CreateDescriptorHeap(&heap, __uuidof(ID3D12DescriptorHeap),
                                          reinterpret_cast<void**>(f.targets.put())),
                  "DX12 render descriptors");
            f.constants = buffer(256 * 258, D3D12_HEAP_TYPE_UPLOAD);
            D3D12_RANGE read{0, 0};
            check(f.constants->Map(0, &read, reinterpret_cast<void**>(&f.mapped)), "DX12 constant mapping");
        }
    }
    ~Impl() {
        if (event)
            CloseHandle(event);
    }
    Com<ID3D12Resource> buffer(UINT64 bytes, D3D12_HEAP_TYPE type) {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = type;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = bytes;
        desc.Height = desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        Com<ID3D12Resource> r;
        check(device->CreateCommittedResource(
                  &heap, D3D12_HEAP_FLAG_NONE, &desc,
                  type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ
                                                 : D3D12_RESOURCE_STATE_COPY_DEST,
                  nullptr, __uuidof(ID3D12Resource), reinterpret_cast<void**>(r.put())),
              "DX12 buffer");
        return r;
    }
    Com<ID3D12Resource> texture(unsigned w, unsigned h, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags,
                                D3D12_RESOURCE_STATES state) {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = w;
        desc.Height = h;
        desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
        desc.Format = format;
        desc.Flags = flags;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        Com<ID3D12Resource> r;
        check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                              __uuidof(ID3D12Resource), reinterpret_cast<void**>(r.put())),
              "DX12 texture");
        return r;
    }
    void barrier(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        if (before == after)
            return;
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
        current->list->ResourceBarrier(1, &b);
    }
    Com<ID3D12Resource> gray(unsigned w, unsigned h, const std::vector<unsigned char>& bytes) {
        auto r =
            texture(w, h, DXGI_FORMAT_R8_UNORM, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
        auto desc = r->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT rows{};
        UINT64 row_size{}, total{};
        device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows, &row_size, &total);
        auto upload = buffer(total, D3D12_HEAP_TYPE_UPLOAD);
        unsigned char* mapped{};
        D3D12_RANGE no_read{0, 0};
        check(upload->Map(0, &no_read, reinterpret_cast<void**>(&mapped)), "DX12 mask upload");
        for (unsigned y = 0; y < h; ++y)
            std::memcpy(mapped + footprint.Offset + y * footprint.Footprint.RowPitch,
                        bytes.data() + size_t(y) * w, w);
        upload->Unmap(0, nullptr);
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = upload.get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint = footprint;
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = r.get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        current->list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        barrier(r.get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        current->retained.push_back(std::move(upload));
        return r;
    }
    MeshEntry& mesh(const Surface& surface) {
        auto& entry = meshes[surface.id];
        if (!entry.buffer || entry.value != surface.mesh) {
            auto triangles = triangulate(surface, surface.mesh.curved ? 4 : 1);
            auto data = buffer(triangles.vertices.size() * sizeof(Vertex), D3D12_HEAP_TYPE_UPLOAD);
            void* mapped{};
            D3D12_RANGE range{0, 0};
            check(data->Map(0, &range, &mapped), "DX12 mesh mapping");
            std::memcpy(mapped, triangles.vertices.data(), triangles.vertices.size() * sizeof(Vertex));
            data->Unmap(0, nullptr);
            entry.buffer = std::move(data);
            entry.value = surface.mesh;
            entry.count = UINT(triangles.vertices.size());
            ++counters.mesh_uploads;
        }
        current->retained.push_back(entry.buffer);
        return entry;
    }
    ID3D12Resource* mask(const Surface& surface) {
        auto& entry = masks[surface.id];
        if (!entry.texture || entry.value != surface.mask) {
            entry.texture = gray(512, 512, mask_pixels(surface.mask, assets));
            entry.value = surface.mask;
            ++counters.mask_uploads;
        }
        current->retained.push_back(entry.texture);
        return entry.texture.get();
    }
    void constants(const Constants& value) {
        if (current->constant >= 258)
            throw std::runtime_error("DX12 constant capacity exceeded");
        UINT at = current->constant++ * 256;
        std::memcpy(current->mapped + at, &value, sizeof(value));
        current->list->SetGraphicsRootConstantBufferView(0, current->constants->GetGPUVirtualAddress() + at);
    }
    void descriptors(ID3D12Resource* image, ID3D12Resource* mask, ID3D12Resource* curve) {
        auto cpu = current->views->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += size_t(current->descriptor) * view_stride;
        auto gpu = current->views->GetGPUDescriptorHandleForHeapStart();
        gpu.ptr += UINT64(current->descriptor) * view_stride;
        if (current->descriptor + 3 > 1024)
            throw std::runtime_error("DX12 descriptor capacity exceeded");
        for (auto* resource : {image, mask, curve}) {
            D3D12_SHADER_RESOURCE_VIEW_DESC view{};
            view.Format = resource ? resource->GetDesc().Format : DXGI_FORMAT_R8_UNORM;
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            view.Texture2D.MipLevels = 1;
            view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            device->CreateShaderResourceView(resource, &view, cpu);
            cpu.ptr += view_stride;
        }
        current->descriptor += 3;
        current->list->SetGraphicsRootDescriptorTable(1, gpu);
    }
};
Renderer12::Renderer12(ID3D12Device* d, ID3D12CommandQueue* q) : impl_(std::make_unique<Impl>(d, q)) {}
Renderer12::~Renderer12() {
    if (impl_)
        try {
            wait(impl_->next_fence - 1, 5000);
        } catch (...) {
        }
}
void Renderer12::set_asset_directory(std::filesystem::path assets) {
    if (assets != impl_->assets) {
        impl_->assets = std::move(assets);
        impl_->masks.clear();
    }
}
RenderCounters Renderer12::counters() const {
    return impl_->counters;
}
bool Renderer12::wait(std::uint64_t value, unsigned timeout) {
    auto& a = *impl_;
    if (value >= a.next_fence)
        throw std::runtime_error("Unknown DX12 completion fence");
    if (a.fence->GetCompletedValue() >= value)
        return true;
    check(a.fence->SetEventOnCompletion(value, a.event), "DX12 completion notification");
    return WaitForSingleObject(a.event, timeout) == WAIT_OBJECT_0;
}
std::uint64_t Renderer12::render(const Output& output, ID3D12Resource* source,
                                 D3D12_RESOURCE_STATES source_state, ID3D12Resource* target,
                                 D3D12_RESOURCE_STATES target_state, double seconds, bool patterns) {
    auto& a = *impl_;
    if (!source || !target || output.surfaces.size() > 256)
        throw std::runtime_error("Invalid DX12 frame");
    auto d = target->GetDesc(), sd = source->GetDesc();
    auto supported = [](DXGI_FORMAT f) {
        return f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM;
    };
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        sd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || !supported(d.Format) || !supported(sd.Format) ||
        d.SampleDesc.Count != 1 || sd.SampleDesc.Count != 1 || d.DepthOrArraySize != 1 ||
        sd.DepthOrArraySize != 1 || d.MipLevels != 1 || sd.MipLevels != 1 ||
        !(d.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET))
        throw std::runtime_error(
            "DX12 needs single-sample SDR RGBA8/BGRA8 2D resources and an enabled render target");
    if (source == target && source_state != target_state)
        throw std::runtime_error("Aliased DX12 frame states disagree");
    for (auto* resource : {source, target}) {
        Com<ID3D12Device> device;
        check(resource->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(device.put())),
              "DX12 resource device");
        if (!same_object(device.get(), a.device.get()))
            throw std::runtime_error("Cross-device DX12 frame rejected");
    }
    auto& f = a.frames[size_t((a.next_fence - 1) % a.frames.size())];
    auto completed = a.fence->GetCompletedValue();
    if (completed == UINT64_MAX)
        throw std::runtime_error("DX12 device removed");
    if (f.submitted > completed)
        return 0;
    f.retained.clear();
    f.constant = f.descriptor = 0;
    a.current = &f;
    check(f.allocator->Reset(), "DX12 allocator reset");
    check(f.list->Reset(f.allocator.get(), nullptr), "DX12 list reset");
    // Cache mutations below are committed commands on success. Clear GPU caches on abandoned recording.
    try {
        if (!a.copy || a.copy->GetDesc().Width != sd.Width || a.copy->GetDesc().Height != sd.Height ||
            a.copy->GetDesc().Format != sd.Format) {
            a.copy = a.texture(unsigned(sd.Width), sd.Height, sd.Format, D3D12_RESOURCE_FLAG_NONE,
                               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            ++a.counters.target_allocations;
        }
        if (!a.intermediate || a.target_desc.Width != d.Width || a.target_desc.Height != d.Height) {
            a.intermediate = a.texture(unsigned(d.Width), d.Height, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                       D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            a.target_desc = d;
            ++a.counters.target_allocations;
        }
        f.retained.push_back(a.copy);
        f.retained.push_back(a.intermediate);
        // Caller keeps borrowed frame resources alive until completion; never retain swapchain buffers.
        a.barrier(source, source_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
        a.barrier(a.copy.get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        f.list->CopyResource(a.copy.get(), source);
        a.barrier(a.copy.get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        a.barrier(source, D3D12_RESOURCE_STATE_COPY_SOURCE, source_state);
        a.barrier(a.intermediate.get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                  D3D12_RESOURCE_STATE_RENDER_TARGET);
        auto composition = f.targets->GetCPUDescriptorHandleForHeapStart(), destination = composition;
        destination.ptr += a.target_stride;
        a.device->CreateRenderTargetView(a.intermediate.get(), nullptr, composition);
        a.device->CreateRenderTargetView(target, nullptr, destination);
        float clear[4]{};
        f.list->ClearRenderTargetView(composition, clear, 0, nullptr);
        D3D12_VIEWPORT viewport{0, 0, float(d.Width), float(d.Height), 0, 1};
        D3D12_RECT scissor{0, 0, LONG(d.Width), LONG(d.Height)};
        f.list->RSSetViewports(1, &viewport);
        f.list->RSSetScissorRects(1, &scissor);
        f.list->SetGraphicsRootSignature(a.root.get());
        auto* heap = f.views.get();
        f.list->SetDescriptorHeaps(1, &heap);
        f.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        f.list->OMSetRenderTargets(1, &composition, FALSE, nullptr);
        f.list->SetPipelineState(a.warp.get());
        std::erase_if(a.meshes, [&](const auto& entry) {
            return std::none_of(output.surfaces.begin(), output.surfaces.end(),
                                [&](const auto& s) { return s.id == entry.first; });
        });
        std::erase_if(a.masks, [&](const auto& entry) {
            return std::none_of(output.surfaces.begin(), output.surfaces.end(),
                                [&](const auto& s) { return s.id == entry.first; });
        });
        if (output.enabled)
            for (auto& surface : output.surfaces)
                if (surface.enabled) {
                    auto& mesh = a.mesh(surface);
                    D3D12_VERTEX_BUFFER_VIEW view{mesh.buffer->GetGPUVirtualAddress(),
                                                  mesh.count * UINT(sizeof(Vertex)), sizeof(Vertex)};
                    f.list->IASetVertexBuffers(0, 1, &view);
                    Constants c{};
                    c.crop[0] = float(surface.source_rect.x);
                    c.crop[1] = float(surface.source_rect.y);
                    c.crop[2] = float(surface.source_rect.w);
                    c.crop[3] = float(surface.source_rect.h);
                    c.params[0] = float(surface.opacity);
                    c.params[1] = patterns ? 1.f : -1.f;
                    c.params[2] = surface.mask.enabled ? 1.f : 0.f;
                    c.params[3] = float(seconds);
                    a.constants(c);
                    a.descriptors(a.copy.get(), surface.mask.enabled ? a.mask(surface) : nullptr, nullptr);
                    f.list->DrawInstanced(mesh.count, 1, 0, 0);
                }
        a.barrier(a.intermediate.get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        a.barrier(target, target_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
        if (!output.enabled) {
            float black[4] = {0, 0, 0, 1};
            f.list->ClearRenderTargetView(destination, black, 0, nullptr);
        } else {
            if (!a.curve || a.curve_points != output.blend.curve) {
                std::vector<unsigned char> values(1024);
                for (unsigned i = 0; i < 1024; ++i)
                    values[i] = static_cast<unsigned char>(
                        std::clamp(curve_value(output.blend.curve, i / 1023.), 0., 1.) * 255 + .5);
                a.curve = a.gray(1024, 1, values);
                a.curve_points = output.blend.curve;
                ++a.counters.curve_uploads;
            }
            f.retained.push_back(a.curve);
            Constants c{};
            c.widths[0] = float(output.blend.left);
            c.widths[1] = float(output.blend.right);
            c.widths[2] = float(output.blend.top);
            c.widths[3] = float(output.blend.bottom);
            c.response[0] = float(output.blend.gamma);
            c.response[1] = float(output.blend.black_level);
            c.response[2] = float(output.color.gamma);
            for (int i = 0; i < 3; ++i) {
                c.gain[i] = float(output.color.gain[size_t(i)]);
                c.offset[i] = float(output.color.offset[size_t(i)]);
            }
            c.adjustments[0] = float(output.color.brightness);
            c.adjustments[1] = float(output.color.contrast);
            c.adjustments[2] = float(output.color.saturation);
            f.list->SetPipelineState(d.Format == DXGI_FORMAT_B8G8R8A8_UNORM ? a.bgra.get() : a.rgba.get());
            f.list->OMSetRenderTargets(1, &destination, FALSE, nullptr);
            a.constants(c);
            a.descriptors(a.intermediate.get(), nullptr, a.curve.get());
            f.list->DrawInstanced(3, 1, 0, 0);
        }
        a.barrier(target, D3D12_RESOURCE_STATE_RENDER_TARGET, target_state);
        check(f.list->Close(), "DX12 submit list");
    } catch (...) {
        f.list->Close();
        a.meshes.clear();
        a.masks.clear();
        a.curve.reset();
        a.copy.reset();
        a.intermediate.reset();
        f.retained.clear();
        throw;
    }
    ID3D12CommandList* list = f.list.get();
    a.queue->ExecuteCommandLists(1, &list);
    auto value = a.next_fence++;
    check(a.queue->Signal(a.fence.get(), value), "DX12 completion signal");
    f.submitted = value;
    return value;
}
} // namespace ps::win
