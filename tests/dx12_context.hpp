// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "ps/d3d12_renderer.hpp"
#include <dxgi1_4.h>
#include <functional>
#include <cstring>
using namespace ps;
using namespace ps::win;
namespace {
struct Context {
    Com<ID3D12Device> device;
    Com<ID3D12CommandQueue> queue;
    Com<ID3D12Fence> fence;
    HANDLE event{};
    UINT64 serial{};
    explicit Context(bool hardware = false) {
        Com<IDXGIFactory4> factory;
        check(CreateDXGIFactory1(__uuidof(IDXGIFactory4), reinterpret_cast<void**>(factory.put())),
              "Test DXGI factory");
        Com<IDXGIAdapter> warp;
        check(factory->EnumWarpAdapter(__uuidof(IDXGIAdapter), reinterpret_cast<void**>(warp.put())),
              "DX12 WARP adapter");
        check(D3D12CreateDevice(hardware ? nullptr : warp.get(), D3D_FEATURE_LEVEL_11_0,
                                __uuidof(ID3D12Device), reinterpret_cast<void**>(device.put())),
              "Test DX12 device");
        D3D12_COMMAND_QUEUE_DESC d{};
        check(device->CreateCommandQueue(&d, __uuidof(ID3D12CommandQueue),
                                         reinterpret_cast<void**>(queue.put())),
              "Test DX12 queue");
        check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence),
                                  reinterpret_cast<void**>(fence.put())),
              "Test DX12 fence");
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }
    ~Context() {
        if (event)
            CloseHandle(event);
    }
    void run(const std::function<void(ID3D12GraphicsCommandList*)>& record) {
        Com<ID3D12CommandAllocator> allocator;
        Com<ID3D12GraphicsCommandList> list;
        check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator),
                                             reinterpret_cast<void**>(allocator.put())),
              "Test allocator");
        check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.get(), nullptr,
                                        __uuidof(ID3D12GraphicsCommandList),
                                        reinterpret_cast<void**>(list.put())),
              "Test list");
        record(list.get());
        check(list->Close(), "Test close list");
        ID3D12CommandList* commands = list.get();
        queue->ExecuteCommandLists(1, &commands);
        check(queue->Signal(fence.get(), ++serial), "Test signal");
        check(fence->SetEventOnCompletion(serial, event), "Test completion");
        if (WaitForSingleObject(event, 10000) != WAIT_OBJECT_0)
            throw std::runtime_error("DX12 test GPU timeout");
    }
    Com<ID3D12Resource> texture(unsigned w, unsigned h, DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM) {
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = w;
        d.Height = h;
        d.DepthOrArraySize = d.MipLevels = d.SampleDesc.Count = 1;
        d.Format = format;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        Com<ID3D12Resource> r;
        check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON,
                                              nullptr, __uuidof(ID3D12Resource),
                                              reinterpret_cast<void**>(r.put())),
              "Test texture");
        return r;
    }
    static void barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* r, D3D12_RESOURCE_STATES a,
                        D3D12_RESOURCE_STATES b) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b};
        list->ResourceBarrier(1, &barrier);
    }
    std::vector<unsigned char> transfer(ID3D12Resource* texture,
                                        const std::vector<unsigned char>* pixels = nullptr) {
        auto d = texture->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT rows{};
        UINT64 row_size{}, bytes{};
        device->GetCopyableFootprints(&d, 0, 1, 0, &footprint, &rows, &row_size, &bytes);
        D3D12_RESOURCE_DESC b{};
        b.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        b.Width = bytes;
        b.Height = b.DepthOrArraySize = b.MipLevels = b.SampleDesc.Count = 1;
        b.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = pixels ? D3D12_HEAP_TYPE_UPLOAD : D3D12_HEAP_TYPE_READBACK;
        Com<ID3D12Resource> buffer;
        check(device->CreateCommittedResource(
                  &heap, D3D12_HEAP_FLAG_NONE, &b,
                  pixels ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                  __uuidof(ID3D12Resource), reinterpret_cast<void**>(buffer.put())),
              "Test transfer buffer");
        if (pixels) {
            unsigned char* data{};
            D3D12_RANGE none{0, 0};
            check(buffer->Map(0, &none, reinterpret_cast<void**>(&data)), "Test upload map");
            for (unsigned y = 0; y < d.Height; ++y)
                std::memcpy(data + y * footprint.Footprint.RowPitch,
                            pixels->data() + size_t(y) * size_t(d.Width) * 4, size_t(d.Width) * 4);
            buffer->Unmap(0, nullptr);
        }
        run([&](ID3D12GraphicsCommandList* list) {
            auto state = pixels ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_COPY_SOURCE;
            barrier(list, texture, D3D12_RESOURCE_STATE_COMMON, state);
            D3D12_TEXTURE_COPY_LOCATION image{};
            image.pResource = texture;
            image.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_TEXTURE_COPY_LOCATION data{};
            data.pResource = buffer.get();
            data.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            data.PlacedFootprint = footprint;
            list->CopyTextureRegion(pixels ? &image : &data, 0, 0, 0, pixels ? &data : &image, nullptr);
            barrier(list, texture, state, D3D12_RESOURCE_STATE_COMMON);
        });
        std::vector<unsigned char> result(size_t(d.Width) * d.Height * 4);
        if (!pixels) {
            unsigned char* data{};
            D3D12_RANGE read{0, size_t(bytes)};
            check(buffer->Map(0, &read, reinterpret_cast<void**>(&data)), "Test readback map");
            for (unsigned y = 0; y < d.Height; ++y)
                std::memcpy(result.data() + size_t(y) * size_t(d.Width) * 4,
                            data + y * footprint.Footprint.RowPitch, size_t(d.Width) * 4);
            D3D12_RANGE no_write{0, 0};
            buffer->Unmap(0, &no_write);
        }
        return result;
    }
};
} // namespace