// SPDX-License-Identifier: GPL-3.0-only
#include "ps/model.hpp"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QLocalSocket>
#include <QProcess>
#include <QThread>
#include <deque>
#include <iostream>
#include <fstream>
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
        socket.abort();
        pending.clear();
        QThread::msleep(100);
        deadline.restart();
        while (socket.state() != QLocalSocket::ConnectedState && deadline.elapsed() < 5000) {
            socket.abort();
            socket.connectToServer(pipe);
            if (!socket.waitForConnected(100))
                QThread::msleep(20);
        }
        require(socket.state() == QLocalSocket::ConnectedState, "Control pipe did not reconnect");
        auto reconnected = stats(5);
        require(generation(reconnected, stable) == retained, "Control reconnect rebuilt outputs");
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
                                                      {"control_reconnect", true},
                                                      {"physical_hotplug_qualified", false},
                                                      {"telemetry", dark}}
                                                     .dump(2));
        auto unavailable = project;
        for (auto& output : unavailable.outputs)
            output.display = "absent-test-display";
        save_project(unavailable, dir / "unavailable.json");
        worker.start(QCoreApplication::applicationDirPath() + "/projection-renderer.exe",
                     {"--project", QString::fromStdWString((dir / "unavailable.json").wstring()),
                      "--duration", "1", "--status",
                      QString::fromStdWString((dir / "deadline-status.json").wstring())});
        require(worker.waitForStarted(5000) && worker.waitForFinished(5000) && worker.exitCode() == 0,
                "Worker duration limit failed when all outputs were missing");
        auto bounded = Json::parse(std::ifstream(dir / "deadline-status.json"));
        require(bounded.at("type") == "stopped" && bounded.at("outputs") == 0,
                "Missing-output deadline evidence invalid");
        auto run_checker = [&](const char* name, bool cancel) {
            QProcess checker;
            auto destination = dir / name;
            checker.start(QCoreApplication::applicationDirPath() + "/projection-qualify.exe",
                          {"--project", QString::fromStdWString((dir / "project.json").wstring()), "--report",
                           QString::fromStdWString(destination.wstring()), "--run", "--simulate", "--seconds",
                           cancel ? "60" : "3", "--warmup", "0"});
            require(checker.waitForStarted(5000), "Qualification runner did not start");
            QByteArray progress;
            QElapsedTimer deadline;
            deadline.start();
            bool cancelled = false;
            while (checker.state() != QProcess::NotRunning && deadline.elapsed() < 20000) {
                checker.waitForReadyRead(25);
                progress += checker.readAllStandardOutput();
                if (cancel && !cancelled && progress.contains("measuring output")) {
                    checker.write("cancel\n");
                    checker.waitForBytesWritten(1000);
                    cancelled = true;
                }
            }
            if (checker.state() != QProcess::NotRunning) {
                checker.kill();
                checker.waitForFinished(2000);
                throw std::runtime_error("Qualification runner timeout");
            }
            require(checker.exitCode() == (cancel ? 3 : 2),
                    "Qualification exit code does not distinguish simulation/cancellation");
            auto summary = Json::parse(std::ifstream(destination / "summary.json"));
            require(!summary.at("release_qualified").get<bool>(),
                    "Runner falsely claimed release qualification");
            require(summary.value("cancelled", false) == cancel, "Cancellation was not retained");
            auto stopped = Json::parse(std::ifstream(destination / "worker-status.json"));
            require(stopped.at("type") == "stopped", "Qualification runner left output process running");
            require(stopped.at("blackout") == true, "Qualification runner did not black out before stopping");
            return summary;
        };
        auto normal_run = run_checker("short-run", false), cancelled_run = run_checker("cancelled-run", true);
        atomic_write("qualification-runner-results.json",
                     Json{{"timed_run", normal_run}, {"cancelled_run", cancelled_run}}.dump(2));
        std::cout << "Worker IPC, selective rebuild, simulated recovery, timed runner and cancellation tests "
                     "passed\n";
        return 0;
    } catch (const std::exception& e) {
        worker.kill();
        worker.waitForFinished(2000);
        std::cerr << e.what() << '\n';
        return 1;
    }
}
