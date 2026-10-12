// SPDX-License-Identifier: GPL-3.0-only
#include "control_server.hpp"
#include <QCoreApplication>
#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTimer>
#include <iostream>
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        ControlServer server;
        bool enabled = false;
        server.status = [&] { return ps::Json{{"blackout", enabled}}; };
        server.project = [] { return ps::project_json(ps::example_project()); };
        server.command = [&](const ps::Json& j) -> ps::Json {
            if (j.at("type") != "blackout")
                throw std::runtime_error("Unknown command");
            enabled = j.at("enabled");
            return {{"ok", true}};
        };
        if (!server.start())
            throw std::runtime_error("API listen failed");
        auto request = [&](bool auth, const QByteArray& data = QByteArray()) {
            QNetworkAccessManager manager;
            QNetworkRequest req(QUrl(QString("http://127.0.0.1:%1/v1/%2")
                                         .arg(server.port())
                                         .arg(data.isEmpty() ? "status" : "command")));
            if (auth)
                req.setRawHeader("Authorization", ("Bearer " + server.token()).toUtf8());
            req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
            auto* reply = data.isEmpty() ? manager.get(req) : manager.post(req, data);
            QEventLoop loop;
            QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
            QTimer::singleShot(3000, &loop, &QEventLoop::quit);
            loop.exec();
            return std::pair{reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(),
                             reply->readAll()};
        };
        if (request(false).first != 401)
            throw std::runtime_error("Unauthenticated HTTP accepted");
        if (request(true).first != 200)
            throw std::runtime_error("Authenticated status failed");
        if (request(true, R"({"type":"blackout","enabled":true})").first != 200 || !enabled)
            throw std::runtime_error("Command not applied");
        if (request(true, R"({"type":"invalid"})").first != 400)
            throw std::runtime_error("Invalid command not rejected");
        QWebSocket ws;
        QEventLoop loop;
        bool got = false;
        QObject::connect(&ws, &QWebSocket::connected, &ws,
                         [&] { ws.sendTextMessage("{\"type\":\"status\"}"); });
        QObject::connect(&ws, &QWebSocket::textMessageReceived, &loop, [&](const QString& text) {
            got = ps::Json::parse(text.toStdString()).value("blackout", false);
            loop.quit();
        });
        QNetworkRequest req(QUrl(QString("ws://127.0.0.1:%1").arg(server.websocket_port())));
        req.setRawHeader("Authorization", ("Bearer " + server.token()).toUtf8());
        ws.open(req);
        QTimer::singleShot(3000, &loop, &QEventLoop::quit);
        loop.exec();
        ws.close();
        if (!got)
            throw std::runtime_error("WebSocket status failed");
        auto old = server.token();
        server.stop();
        server.start();
        if (old == server.token())
            throw std::runtime_error("Token did not rotate");
        server.stop();
        std::cout << "HTTP authentication, command errors, WebSocket and token rotation passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
