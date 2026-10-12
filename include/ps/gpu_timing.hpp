// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "windows.hpp"
#include "runtime.hpp"
#include <array>
namespace ps::win {
class GpuTiming {
    struct Sample {
        Com<ID3D11Query> disjoint, begin, end;
        bool pending{};
    };
    std::array<Sample, 4> ring_;
    Com<ID3D11DeviceContext> context_;
    Sample* active_{};

  public:
    Measurements milliseconds;
    explicit GpuTiming(ID3D11Device* device) {
        device->GetImmediateContext(context_.put());
        for (auto& q : ring_) {
            D3D11_QUERY_DESC d{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
            check(device->CreateQuery(&d, q.disjoint.put()), "GPU timing frequency");
            d.Query = D3D11_QUERY_TIMESTAMP;
            check(device->CreateQuery(&d, q.begin.put()), "GPU timing start");
            check(device->CreateQuery(&d, q.end.put()), "GPU timing end");
        }
    }
    void begin() {
        active_ = nullptr;
        for (auto& q : ring_) {
            if (q.pending) {
                D3D11_QUERY_DATA_TIMESTAMP_DISJOINT d{};
                UINT64 a{}, b{};
                auto flags = D3D11_ASYNC_GETDATA_DONOTFLUSH;
                if (context_->GetData(q.disjoint.get(), &d, sizeof(d), flags) == S_OK &&
                    context_->GetData(q.begin.get(), &a, sizeof(a), flags) == S_OK &&
                    context_->GetData(q.end.get(), &b, sizeof(b), flags) == S_OK) {
                    q.pending = false;
                    if (!d.Disjoint && d.Frequency)
                        milliseconds.add(double(b - a) * 1000 / d.Frequency);
                }
            }
            if (!q.pending && !active_)
                active_ = &q;
        }
        if (active_) {
            context_->Begin(active_->disjoint.get());
            context_->End(active_->begin.get());
        }
    }
    void end() {
        if (active_) {
            context_->End(active_->end.get());
            context_->End(active_->disjoint.get());
            active_->pending = true;
            active_ = nullptr;
        }
    }
};
inline std::int64_t qpc_100ns() {
    LARGE_INTEGER now{}, frequency{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    return std::int64_t(double(now.QuadPart) * 10000000. / double(frequency.QuadPart));
}
} // namespace ps::win
