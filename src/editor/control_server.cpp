// SPDX-License-Identifier: GPL-3.0-only
#include "control_server.hpp"
#include <QCryptographicHash>
#include <QTimer>
#include <QUuid>
ControlServer::ControlServer(QObject* parent) : QObject(parent) {
    connect(&websocket_, &QWebSocketServer::newConnection, this, [this] {
        while (auto* s = websocket_.nextPendingConnection()) {
            connect(s, &QWebSocket::disconnected, s, &QObject::deleteLater);
            if (s->request().rawHeader("Authorization") != ("Bearer " + token_).toUtf8()) {
                s->close(QWebSocketProtocol::CloseCodePolicyViolated, "Bearer token required");
                continue;
            }
            clients_.insert(s);
            s->setMaxAllowedIncomingMessageSize(1024 * 1024);
            s->setMaxAllowedIncomingFrameSize(1024 * 1024);
            connect(s, &QWebSocket::disconnected, this, [this, s] { clients_.remove(s); });
            connect(s, &QWebSocket::textMessageReceived, this, [this, s](const QString& text) {
                try {
                    auto j = ps::Json::parse(text.toStdString());
                    auto type = j.value("type", "");
                    auto result = type == "status"        ? status()
                                  : type == "get_project" ? project()
                                                          : command(j);
                    s->sendTextMessage(QString::fromStdString(result.dump()));
                } catch (const std::exception& e) {
                    s->sendTextMessage(QString::fromStdString(ps::Json{{"error", e.what()}}.dump()));
                }
            });
        }
    });
    auto* telemetry = new QTimer(this);
    telemetry->setInterval(1000);
    connect(telemetry, &QTimer::timeout, this, [this] {
        if (!status || clients_.empty())
            return;
        auto payload = QString::fromStdString(status().dump());
        for (auto* client : clients_)
            if (client->bytesToWrite() < 65536)
                client->sendTextMessage(payload);
    });
    telemetry->start();
    connect(&server_, &QTcpServer::newConnection, this, [this] {
        while (auto* s = server_.nextPendingConnection()) {
            buffers_[s] = {};
            connect(s, &QTcpSocket::readyRead, this, [this, s] { consume(s); });
            connect(s, &QTcpSocket::disconnected, this, [this, s] {
                buffers_.remove(s);
                s->deleteLater();
            });
            QTimer::singleShot(5000, s, [s] { s->disconnectFromHost(); });
        }
    });
}
bool ControlServer::start() {
    if (active())
        return true;
    token_ = QUuid::createUuid().toString(QUuid::WithoutBraces) +
             QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!server_.listen(QHostAddress::LocalHost, 0))
        return false;
    if (!websocket_.listen(QHostAddress::LocalHost, 0)) {
        stop();
        return false;
    }
    return true;
}
ControlServer::~ControlServer() {
    for (auto* s : buffers_.keys()) {
        QObject::disconnect(s, nullptr, this, nullptr);
        s->abort();
    }
    for (auto* s : clients_) {
        QObject::disconnect(s, nullptr, this, nullptr);
        s->abort();
    }
    server_.close();
    websocket_.close();
}
void ControlServer::stop() {
    server_.close();
    websocket_.close();
    for (auto* client : QSet<QWebSocket*>(clients_))
        client->close();
    for (auto* socket : buffers_.keys())
        socket->disconnectFromHost();
    token_.clear();
}
bool ControlServer::active() const {
    return server_.isListening();
}
QString ControlServer::token() const {
    return token_;
}
quint16 ControlServer::port() const {
    return server_.serverPort();
}
void ControlServer::respond(QTcpSocket* s, int code, const ps::Json& j) {
    auto body = QByteArray::fromStdString(j.dump());
    auto reason = code == 200 ? "OK" : code == 401 ? "Unauthorized" : "Bad Request";
    s->write("HTTP/1.1 " + QByteArray::number(code) + " " + reason +
             "\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " +
             QByteArray::number(body.size()) + "\r\n\r\n" + body);
    s->disconnectFromHost();
}
void ControlServer::consume(QTcpSocket* s) {
    auto& buffer = buffers_[s];
    buffer += s->readAll();
    if (buffer.size() > 1024 * 1024) {
        respond(s, 400, {{"error", "Request exceeds 1 MiB"}});
        return;
    }
    auto end = buffer.indexOf("\r\n\r\n");
    if (end < 0)
        return;
    auto lines = buffer.left(end).split('\n');
    if (lines.empty())
        return;
    auto request = lines[0].trimmed().split(' ');
    if (request.size() != 3) {
        respond(s, 400, {{"error", "Malformed request"}});
        return;
    }
    int length = 0;
    QByteArray auth;
    for (auto line : lines) {
        line = line.trimmed();
        auto colon = line.indexOf(':');
        if (colon < 0)
            continue;
        auto name = line.left(colon).toLower();
        auto value = line.mid(colon + 1).trimmed();
        if (name == "content-length") {
            bool ok;
            length = value.toInt(&ok);
            if (!ok || length < 0 || length > 1024 * 1024) {
                respond(s, 400, {{"error", "Invalid Content-Length"}});
                return;
            }
        }
        if (name == "authorization")
            auth = value;
    }
    auto expected = ("Bearer " + token_).toUtf8();
    if (QCryptographicHash::hash(auth, QCryptographicHash::Sha256) !=
        QCryptographicHash::hash(expected, QCryptographicHash::Sha256)) {
        respond(s, 401, {{"error", "Bearer token required"}});
        return;
    }
    if (buffer.size() < end + 4 + length)
        return;
    try {
        if (request[0] == "GET" && request[1] == "/v1/status")
            respond(s, 200, status ? status() : ps::Json::object());
        else if (request[0] == "GET" && request[1] == "/v1/project")
            respond(s, 200, project ? project() : ps::Json::object());
        else if (request[0] == "POST" && request[1] == "/v1/command") {
            auto j = ps::Json::parse(buffer.mid(end + 4, length).toStdString());
            respond(s, 200, command ? command(j) : ps::Json{{"error", "No command handler"}});
        } else
            respond(s, 400, {{"error", "Unknown endpoint"}});
    } catch (const std::exception& e) {
        respond(s, 400, {{"error", e.what()}});
    }
}
