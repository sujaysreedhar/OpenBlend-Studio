// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "windows.hpp"
#include <d3d12.h>
namespace ps::win {
// Caller serializes calls and supplies the actual direct presentation queue.
class Renderer12 {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    Renderer12(ID3D12Device*, ID3D12CommandQueue*);
    ~Renderer12();
    void set_asset_directory(std::filesystem::path);
    // Restores both caller states. Source and target may alias; source is copied first.
    // Returns 0 without recording/submitting when all three frame slots are busy.
    std::uint64_t render(const Output&, ID3D12Resource* source, D3D12_RESOURCE_STATES source_state,
                         ID3D12Resource* target, D3D12_RESOURCE_STATES target_state, double seconds = 0,
                         bool patterns = false);
    bool wait(std::uint64_t fence, unsigned timeout_ms);
    RenderCounters counters() const;
};
} // namespace ps::win
