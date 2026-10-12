// SPDX-License-Identifier: GPL-3.0-only
#include "preview_service.hpp"
#include "ps/windows.hpp"
#include <QTimer>
#include <array>
#include <chrono>
#include <cstring>
#include <roapi.h>

using namespace ps;
using namespace ps::win;
namespace {
struct PreviewEngine : QObject {
    Com<ID3D11Device> device;
    Com<ID3D11DeviceContext> context;
    std::unique_ptr<Renderer> renderer;
    Com<ID3D11Texture2D> target;
    Com<ID3D11RenderTargetView> rt;
    struct Slot {
        Com<ID3D11Texture2D> texture;
        Com<ID3D11Query> ready;
        QString key;
        bool pending{};
        std::uint64_t serial{};
    };
    std::array<Slot, 3> readbacks;
    std::unordered_map<std::string, Com<ID3D11ShaderResourceView>> images;
    std::unordered_map<std::string, std::unique_ptr<WindowCapture>> windows;
    std::unordered_map<std::string, std::unique_ptr<DesktopCapture>> desktops;
    std::unordered_map<std::string, Json> sources;
    std::filesystem::path assets;
    unsigned width{}, height{};
    std::uint64_t serial{}, delivered{};
    std::optional<PreviewRequest> request;
    std::chrono::steady_clock::time_point retry{};
    bool initialized{};
    ~PreviewEngine() override {
        windows.clear();
        desktops.clear();
        images.clear();
        renderer.reset();
        rt.reset();
        target.reset();
        readbacks = {};
        context.reset();
        device.reset();
        if (initialized)
            RoUninitialize();
    }
    void initialize() {
        if (!initialized) {
            check(RoInitialize(RO_INIT_MULTITHREADED), "Preview COM initialization");
            initialized = true;
        }
        if (device)
            return;
        try {
            device = create_device();
        } catch (...) {
            device = create_device(true);
        }
        device->GetImmediateContext(context.put());
        renderer = std::make_unique<Renderer>(device.get());
    }
    void configure(PreviewRequest value) {
        if (assets != value.assets) {
            assets = value.assets;
            images.clear();
        }
        for (const auto& source : value.project.sources) {
            Json signature = source;
            if (sources[source.id] != signature) {
                images.erase(source.id);
                windows.erase(source.id);
                desktops.erase(source.id);
                sources[source.id] = std::move(signature);
            }
        }
        auto removed = [&](const auto& entry) { return !find_source(value.project, entry.first); };
        std::erase_if(sources, removed);
        std::erase_if(images, removed);
        std::erase_if(windows, removed);
        std::erase_if(desktops, removed);
        request = std::move(value);
    }
    void ensure_target() {
        if (target && width == request->width && height == request->height)
            return;
        context->OMSetRenderTargets(0, nullptr, nullptr);
        rt.reset();
        target.reset();
        readbacks = {};
        width = request->width;
        height = request->height;
        D3D11_TEXTURE2D_DESC d{};
        d.Width = width;
        d.Height = height;
        d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.BindFlags = D3D11_BIND_RENDER_TARGET;
        check(device->CreateTexture2D(&d, nullptr, target.put()), "Preview target");
        check(device->CreateRenderTargetView(target.get(), nullptr, rt.put()), "Preview target view");
        d.BindFlags = 0;
        d.Usage = D3D11_USAGE_STAGING;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        for (auto& slot : readbacks) {
            check(device->CreateTexture2D(&d, nullptr, slot.texture.put()), "Preview readback ring");
            D3D11_QUERY_DESC q{D3D11_QUERY_EVENT, 0};
            check(device->CreateQuery(&q, slot.ready.put()), "Preview completion query");
        }
    }
    QImage poll(QString& key) {
        Slot* newest = nullptr;
        for (auto& slot : readbacks) {
            if (!slot.pending)
                continue;
            auto hr = context->GetData(slot.ready.get(), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH);
            check(hr, "Preview completion");
            if (hr == S_OK) {
                slot.pending = false;
                if (slot.serial > delivered && (!newest || slot.serial > newest->serial))
                    newest = &slot;
            }
        }
        if (!newest)
            return {};
        D3D11_MAPPED_SUBRESOURCE m{};
        auto hr = context->Map(newest->texture.get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
            newest->pending = true;
            return {};
        }
        check(hr, "Preview readback");
        QImage image(int(width), int(height), QImage::Format_RGBA8888);
        for (unsigned y = 0; y < height; ++y)
            std::memcpy(image.scanLine(int(y)), static_cast<const char*>(m.pData) + y * m.RowPitch,
                        width * 4);
        context->Unmap(newest->texture.get(), 0);
        delivered = newest->serial;
        key = newest->key;
        return image;
    }
    void render() {
        renderer->set_asset_directory(assets);
        std::unordered_map<std::string, SourceTexture> frame;
        auto provider = [&](const std::string& id) -> SourceTexture {
            if (frame.contains(id))
                return frame.at(id);
            auto* s = find_source(request->project, id);
            SourceTexture texture{nullptr, 2};
            if (!s)
                return texture;
            if (s->type == "pattern")
                texture.pattern = pattern_index(s->pattern);
            else if (s->type == "image") {
                auto& image = images[id];
                if (!image)
                    image = renderer->load_image(resolve_asset(assets / "project.json", s->image));
                texture = {image.get(), -1};
            } else if (s->type == "window") {
                auto& capture = windows[id];
                if (!capture)
                    capture = std::make_unique<WindowCapture>(device.get(), resolve_window(*s), s->cursor);
                capture->update();
                texture = {capture->view(), -1};
            } else if (s->type == "desktop") {
                auto& capture = desktops[id];
                if (!capture)
                    capture = std::make_unique<DesktopCapture>(device.get(), s->display, s->cursor);
                capture->update();
                texture = {capture->view(), -1};
            }
            frame[id] = texture;
            return texture;
        };
        for (auto& slot : readbacks)
            if (!slot.pending) {
                auto seconds =
                    std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
                        .count();
                renderer->render(request->output, provider, rt.get(), width, height, seconds);
                context->CopyResource(slot.texture.get(), target.get());
                context->End(slot.ready.get());
                context->Flush(); // Submit; completion is polled without waiting on the UI or GPU.
                slot.key = request->key;
                slot.serial = ++serial;
                slot.pending = true;
                break;
            }
    }
};
} // namespace
PreviewService::PreviewService(QObject* parent) : QObject(parent), shared_(std::make_shared<Shared>()) {
    auto* engine = new PreviewEngine;
    engine->moveToThread(&thread_);
    connect(&thread_, &QThread::finished, engine, &QObject::deleteLater);
    connect(&thread_, &QThread::started, engine, [this, engine, shared = shared_] {
        auto* timer = new QTimer(engine);
        timer->setTimerType(Qt::PreciseTimer);
        QObject::connect(timer, &QTimer::timeout, engine, [this, engine, shared] {
            std::optional<PreviewRequest> newest;
            {
                std::lock_guard lock(shared->mutex);
                if (shared->paused)
                    return;
                newest.swap(shared->request);
            }
            if (newest)
                engine->configure(std::move(*newest));
            if (!engine->request || std::chrono::steady_clock::now() < engine->retry)
                return;
            QImage image;
            QString key, error;
            try {
                engine->initialize();
                engine->ensure_target();
                image = engine->poll(key);
                engine->render();
            } catch (const std::exception& e) {
                error = QString::fromUtf8(e.what());
                key = engine->request->key;
                engine->windows.clear();
                engine->desktops.clear();
                if (engine->device && FAILED(engine->device->GetDeviceRemovedReason())) {
                    engine->renderer.reset();
                    engine->images.clear();
                    engine->rt.reset();
                    engine->target.reset();
                    engine->readbacks = {};
                    engine->context.reset();
                    engine->device.reset();
                }
                engine->retry = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            }
            if (image.isNull() && error.isEmpty())
                return;
            {
                std::lock_guard lock(shared->mutex);
                if (shared->delivery_pending)
                    return;
                shared->delivery_pending = true;
            }
            QMetaObject::invokeMethod(
                this,
                [this, shared, image, key, error] {
                    {
                        std::lock_guard lock(shared->mutex);
                        shared->delivery_pending = false;
                    }
                    if (completed)
                        completed(key, image, error);
                },
                Qt::QueuedConnection);
        });
        timer->start(33);
    });
    thread_.start();
}
PreviewService::~PreviewService() {
    thread_.quit();
    thread_.wait();
}
void PreviewService::submit(PreviewRequest request) {
    std::lock_guard lock(shared_->mutex);
    shared_->request = std::move(request);
    shared_->paused = false;
}
void PreviewService::pause() {
    std::lock_guard lock(shared_->mutex);
    shared_->paused = true;
}
