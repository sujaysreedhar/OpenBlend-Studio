// SPDX-License-Identifier: GPL-3.0-only
#include "ps/model.hpp"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QLocalSocket>
#include <QProcess>
#include <QThread>
#include <deque>
#include <iostream>
using namespace ps;
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QProcess worker;
    try {
        auto dir = std::filesystem::current_path() / ("worker-test-" + make_id());
        auto project = example_project(3);
        save_project(project, dir / "project.json");
        QString pipe = "ps-worker-test-" + QString::fromStdString(make_id());
        worker.start(QCoreApplication::applicationDirPath() + "/projection-renderer.exe",
                     {"--project", QString::fromStdWString((dir / "project.json").wstring()), "--status",
                      QString::fromStdWString((dir / "status.json").wstring()), "--pipe", pipe});
        if (!worker.waitForStarted(5000))
            throw std::runtime_error("Worker failed to start");
        QLocalSocket socket;
        QElapsedTimer deadline;
        deadline.start();
        while (socket.state() != QLocalSocket::ConnectedState && deadline.elapsed() < 10000) {
            socket.abort();
            socket.connectToServer(pipe);
            if (!socket.waitForConnected(100))
                QThread::msleep(20);
        }
        if (socket.state() != QLocalSocket::ConnectedState)
            throw std::runtime_error("Worker pipe unavailable");
        QByteArray pending;
        auto send = [&](const Json& message) {
            auto text = message.dump() + "\n";
            socket.write(text.data(), qint64(text.size()));
            socket.flush();
        };
        auto wait = [&](const std::function<bool(const Json&)>& predicate) {
            QElapsedTimer timer;
            timer.start();
            while (timer.elapsed() < 10000) {
                pending += socket.readAll();
                while (pending.contains('\n')) {
                    auto end = pending.indexOf('\n');
                    auto j = Json::parse(pending.left(end).toStdString());
                    pending.remove(0, end + 1);
                    if (predicate(j))
                        return j;
                }
                socket.waitForReadyRead(25);
                QCoreApplication::processEvents();
            }
            throw std::runtime_error("Timed out waiting for worker evidence");
        };
        auto stats = [&](std::uint64_t revision) {
            return wait([&](const Json& j) {
                return j.value("type", "") == "stats" && j.value("revision", 999ULL) == revision;
            });
        };
        auto require = [](bool valid, const char* message) {
            if (!valid)
                throw std::runtime_error(message);
        };
        auto initial = stats(0);
        require(initial.at("outputs") == 3, "Three simulated outputs not created");
        auto id = project.outputs[0].id, stable = project.outputs[1].id;
        auto generation = [&](const Json& j, const std::string& key) {
            return j.at("output_stats").at(key).at("resource_generation");
        };
        auto first = generation(initial, id), retained = generation(initial, stable);
        auto document = project_json(project);
        auto edited = document;
        // A substantial first snapshot also exercises writes exceeding a single pipe buffer.
        edited["name"] = std::string(80000, 'X');
        send({{"type", "project"}, {"revision", 1}, {"project", edited}});
        auto full = stats(1);
        require(generation(full, stable) == retained, "Full synchronization recreated unchanged output");
        document = edited;
        edited["outputs"][0]["surfaces"][0]["opacity"] = .75;
        send({{"type", "patch"},
              {"base_revision", 1},
              {"revision", 2},
              {"patch", Json::diff(document, edited)}});
        auto color = stats(2);
        require(generation(color, id) == first && generation(color, stable) == retained,
                "Color edit rebuilt swapchains");
        send({{"type", "patch"}, {"base_revision", 1}, {"revision", 3}, {"patch", Json::array()}});
        auto rejected = wait([](const Json& j) { return j.value("type", "") == "error"; });
        require(rejected.at("revision") == 2 && rejected.value("resync_required", false),
                "Stale patch not rejected atomically");
        document = edited;
        edited["outputs"][0]["viewport"]["w"] = .8;
        send({{"type", "patch"},
              {"base_revision", 2},
              {"revision", 3},
              {"patch", Json::diff(document, edited)}});
        auto resized = stats(3);
        require(generation(resized, id) != first && generation(resized, stable) == retained,
                "Selective output rebuild failed");
        document = edited;
        edited["outputs"][0]["display"] = "disconnected-test-display";
        send({{"type", "patch"},
              {"base_revision", 3},
              {"revision", 4},
              {"patch", Json::diff(document, edited)}});
        auto lost = stats(4);
        require(lost.at("outputs") == 2 && lost.at("output_errors").contains(id),
                "Missing output not isolated");
        require(generation(lost, stable) == retained, "Missing display interrupted another output");
        document = edited;
        edited["outputs"][0]["display"] = "";
        send({{"type", "patch"},
              {"base_revision", 4},
              {"revision", 5},
              {"patch", Json::diff(document, edited)}});
        auto restored = stats(5);
        require(restored.at("outputs") == 3 && generation(restored, stable) == retained,
                "Output recovery failed");
        send({{"type", "identify"}, {"seconds", 1}});
        wait([](const Json& j) { return j.value("command", "") == "identify"; });
        send({{"type", "blackout"}, {"enabled", true}});
        auto dark =
            wait([](const Json& j) { return j.value("type", "") == "stats" && j.value("blackout", false); });
        require(dark.at("dropped_frames").is_null() && !dark.at("physical_latency_measured").get<bool>(),
                "Unmeasured presentation reported as qualified");
        send({{"type", "stop"}});
        if (!worker.waitForFinished(5000))
            throw std::runtime_error("Graceful stop timed out");
        require(worker.exitCode() == 0, "Worker exit failure");
        atomic_write("worker-test-results.json", Json{{"passed", true},
                                                      {"selective_rebuild", true},
                                                      {"revision_resync", true},
                                                      {"large_snapshot", true},
                                                      {"simulated_disconnect_recovery", true},
                                                      {"blackout_stop_identify", true},
                                                      {"physical_hotplug_qualified", false},
                                                      {"telemetry", dark}}
                                                     .dump(2));
        std::cout << "Worker IPC, selective rebuild, simulated recovery and control tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        worker.kill();
        worker.waitForFinished(2000);
        std::cerr << e.what() << '\n';
        return 1;
    }
}
