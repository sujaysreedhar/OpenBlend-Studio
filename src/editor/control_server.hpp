// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "ps/model.hpp"
#include <QHash>
#include <QSet>
#include <QTcpServer>
#include <QTcpSocket>
#include <QWebSocket>
#include <QWebSocketServer>
#include <functional>
class ControlServer : public QObject {
  public:
    explicit ControlServer(QObject* parent = nullptr);
    ~ControlServer() override;
    bool start();
    void stop();
    bool active() const;
    QString token() const;
    quint16 port() const;
    quint16 websocket_port() const {
        return websocket_.serverPort();
    }
    std::function<ps::Json()> status;
    std::function<ps::Json(const ps::Json&)> command;
    std::function<ps::Json()> project;

  private:
    QTcpServer server_;
    QString token_;
    QHash<QTcpSocket*, QByteArray> buffers_;
    QWebSocketServer websocket_{"Projection Studio", QWebSocketServer::NonSecureMode};
    QSet<QWebSocket*> clients_;
    void consume(QTcpSocket*);
    void respond(QTcpSocket*, int, const ps::Json&);
};
