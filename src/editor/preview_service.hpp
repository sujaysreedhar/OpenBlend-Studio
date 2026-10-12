// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "ps/model.hpp"
#include <QImage>
#include <QObject>
#include <QThread>
#include <functional>
#include <mutex>
#include <optional>

struct PreviewRequest {
    ps::Project project;
    ps::Output output;
    std::filesystem::path assets;
    QString key;
    unsigned width{960}, height{540};
};
// One replaceable request and one outstanding UI delivery, regardless of input rate.
class PreviewService : public QObject {
  public:
    explicit PreviewService(QObject* parent = nullptr);
    ~PreviewService();
    void submit(PreviewRequest);
    void pause();
    std::function<void(const QString&, const QImage&, const QString&)> completed;

  private:
    struct Shared {
        std::mutex mutex;
        std::optional<PreviewRequest> request;
        bool delivery_pending{};
        bool paused{true};
    };
    std::shared_ptr<Shared> shared_;
    QThread thread_;
};
