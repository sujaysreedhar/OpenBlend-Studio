// SPDX-License-Identifier: GPL-3.0-only
#include "ps/setup_check.hpp"
#include "ps/qualification.hpp"
#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QLocalSocket>
#include <QProcess>
#include <QThread>
#include <atomic>
#include <cmath>
#include <fstream>
#include <iostream>
#include <roapi.h>
using namespace ps;
using namespace ps::win;
namespace {
std::atomic<bool> interrupted{};
BOOL WINAPI console_signal(DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT) {
        interrupted = true;
        return TRUE;
    }
    return FALSE;
}
bool cancelled() {
    auto input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD available{};
    if (input != INVALID_HANDLE_VALUE && PeekNamedPipe(input, nullptr, 0, nullptr, &available, nullptr) &&
        available) {
        char data[256];
        DWORD read{};
        ReadFile(input, data, std::min<DWORD>(available, sizeof(data)), &read, nullptr);
        if (read)
            interrupted = true; // stdin input requests cancellation; never executes commands.
    }
    return interrupted;
}
struct Job {
    HANDLE handle = CreateJobObjectW(nullptr, nullptr);
    Job() {
        if (!handle)
            throw std::runtime_error("Cannot create qualification process job");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit{};
        limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(handle, JobObjectExtendedLimitInformation, &limit, sizeof(limit))) {
            CloseHandle(handle);
            handle = nullptr;
            throw std::runtime_error("Cannot configure child cleanup");
        }
    }
    void assign(qint64 pid) {
        HANDLE process = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, DWORD(pid));
        if (!process)
            throw std::runtime_error("Cannot supervise renderer process");
        bool ok = AssignProcessToJobObject(handle, process);
        CloseHandle(process);
        if (!ok)
            throw std::runtime_error("Cannot guarantee renderer cleanup in this session");
    }
    ~Job() {
        if (handle)
            CloseHandle(handle);
    }
};
void progress(const std::string& phase, double seconds = 0) {
    std::cout << Json{{"type", "progress"}, {"phase", phase}, {"seconds", seconds}}.dump() << std::endl;
}
} // namespace
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    RoInitialize(RO_INIT_MULTITHREADED);
    SetConsoleCtrlHandler(console_signal, TRUE);
    std::filesystem::path report_path;
    QProcess worker;
    bool owns_report = false;
    try {
        QString project_arg, report_arg;
        bool run = false, simulate = false, fullscreen = false, probe = false;
        double seconds = 60, warmup = 5, hz = 60;
        auto args = app.arguments();
        for (int i = 1; i < args.size(); ++i) {
            auto arg = args[i];
            auto value = [&] {
                if (++i >= args.size())
                    throw std::invalid_argument("Missing option value");
                return args[i];
            };
            auto number = [&] {
                bool ok;
                double n = value().toDouble(&ok);
                if (!ok || !std::isfinite(n))
                    throw std::invalid_argument("Invalid number");
                return n;
            };
            if (arg == "--project")
                project_arg = value();
            else if (arg == "--report")
                report_arg = value();
            else if (arg == "--seconds")
                seconds = number();
            else if (arg == "--warmup")
                warmup = number();
            else if (arg == "--target-hz")
                hz = number();
            else if (arg == "--run")
                run = true;
            else if (arg == "--simulate")
                simulate = true;
            else if (arg == "--fullscreen")
                fullscreen = true;
            else if (arg == "--probe-capture")
                probe = true;
            else if (arg == "--help") {
                std::cout << "projection-qualify --project file.json --report directory [--probe-capture] "
                             "[--run --seconds 60 --warmup 5 --target-hz 60 --fullscreen | --simulate]\n"
                          << "Exit 0: checks passed, 1: failed/error, 2: incomplete, 3: cancelled. Eight "
                             "hours: --seconds 28800. Ctrl+C cancels.\n";
                return 0;
            } else
                throw std::invalid_argument("Unknown qualification option");
        }
        if (project_arg.isEmpty() || report_arg.isEmpty() || seconds < 3 || seconds > 86400 || warmup < 0 ||
            warmup > 300 || hz < 1 || hz > 1000 || simulate && fullscreen)
            throw std::invalid_argument("Use --project and --report; seconds 3–86400, warmup 0–300, Hz "
                                        "1–1000; fullscreen and simulation are exclusive");
        report_path = std::filesystem::absolute(report_arg.toStdWString());
        // Never overwrite evidence from an earlier run.
        if (std::filesystem::exists(report_path) && !std::filesystem::is_empty(report_path))
            throw std::invalid_argument("Choose a new empty report directory");
        std::filesystem::create_directories(report_path);
        owns_report = true;
        auto project_path = std::filesystem::absolute(project_arg.toStdWString());
        auto project = load_project(project_path);
        progress("checking setup");
        auto setup = check_setup(project, project_path, probe, 3, cancelled);
        atomic_write(report_path / "setup.json", setup.dump(2));
        auto finish = [&](Json summary) {
            summary["application_version"] = "0.3.0";
            summary["setup_status"] = setup.at("status");
            summary["release_qualified"] = false;
            atomic_write(report_path / "summary.json", summary.dump(2));
            std::cout << Json{{"type", "result"},
                              {"status", summary.value("status", "incomplete")},
                              {"report", utf8(report_path.wstring())}}
                             .dump()
                      << std::endl;
            return summary.value("cancelled", false)       ? 3
                   : summary.value("status", "") == "fail" ? 1
                   : summary.value("status", "") == "pass" ? 0
                                                           : 2;
        };
        if (cancelled())
            return finish({{"status", "incomplete"}, {"cancelled", true}});
        if (!run)
            return finish({{"status", setup.at("status")}, {"kind", "setup-only"}});
        if (!simulate && setup.at("status") != "pass")
            return finish(
                {{"status", "incomplete"},
                 {"kind", "output-run"},
                 {"reason",
                  "Resolve setup checks before a physical output run. Simulation remains available."}});
        auto absolute = [&](std::string& asset) {
            if (!asset.empty())
                asset = utf8(resolve_asset(project_path, asset).wstring());
        };
        for (auto& s : project.sources)
            absolute(s.image);
        for (auto& o : project.outputs) {
            if (simulate)
                o.display.clear();
            for (auto& s : o.surfaces)
                absolute(s.mask.bitmap);
        }
        for (auto& preset : project.presets)
            for (auto& o : preset.outputs)
                for (auto& s : o.surfaces)
                    absolute(s.mask.bitmap);
        auto snapshot = report_path / "project.json";
        save_project(project, snapshot);
        std::ofstream journal(report_path / "telemetry.jsonl", std::ios::binary);
        journal.exceptions(std::ios::badbit | std::ios::failbit);
        auto state_path = report_path / "worker-status.json";
        QString pipe = "projection-check-" + QString::fromStdString(make_id());
        QStringList worker_args{"--project",  QString::fromStdWString(snapshot.wstring()),
                                "--pipe",     pipe,
                                "--status",   QString::fromStdWString(state_path.wstring()),
                                "--duration", QString::number(seconds + warmup + 30)};
        if (fullscreen)
            worker_args << "--fullscreen";
        else if (!simulate)
            worker_args << "--native-size";
        Job job;
        worker.start(app.applicationDirPath() + "/projection-renderer.exe", worker_args);
        if (!worker.waitForStarted(5000))
            throw std::runtime_error("Renderer could not start");
        job.assign(worker.processId());
        QLocalSocket socket;
        QElapsedTimer timer;
        timer.start();
        while (socket.state() != QLocalSocket::ConnectedState && timer.elapsed() < 10000 && !cancelled()) {
            socket.abort();
            socket.connectToServer(pipe);
            if (!socket.waitForConnected(100))
                QThread::msleep(20);
        }
        if (socket.state() != QLocalSocket::ConnectedState)
            throw std::runtime_error("Renderer control pipe unavailable");
        QByteArray pending;
        RunEvidence evidence;
        double baseline = -1;
        bool black = false, black_ack = false, completed = false;
        std::uint64_t bytes_written = 0;
        auto send = [&](const Json& j) {
            auto s = j.dump() + "\n";
            socket.write(s.data(), qint64(s.size()));
            socket.flush();
        };
        auto poll = [&](bool measuring) {
            socket.waitForReadyRead(25);
            QCoreApplication::processEvents();
            pending += socket.readAll();
            if (pending.size() > 32 * 1024 * 1024)
                throw std::runtime_error("Oversized renderer response");
            while (pending.contains('\n')) {
                auto end = pending.indexOf('\n');
                auto line = pending.left(end);
                pending.remove(0, end + 1);
                auto j = Json::parse(line.toStdString());
                if (j.value("type", "") == "stats") {
                    auto text = j.dump() + "\n";
                    bytes_written += text.size();
                    if (bytes_written > 1024ULL * 1024 * 1024)
                        throw std::runtime_error("Telemetry exceeds 1 GiB; evidence saved");
                    journal << text;
                    journal.flush();
                    black |= j.value("blackout", false);
                    double elapsed = j.at("elapsed_seconds");
                    if (measuring && elapsed >= warmup) {
                        if (baseline < 0)
                            baseline = elapsed;
                        evidence.observe(j, project);
                        progress("measuring output", elapsed - baseline);
                        completed = elapsed - baseline >= seconds;
                    }
                }
                if (j.value("type", "") == "ack" && j.value("command", "") == "blackout")
                    black_ack = true;
                if (j.value("type", "") == "error")
                    throw std::runtime_error(j.value("message", "Renderer rejected command"));
            }
        };
        progress("warming up");
        timer.restart();
        while (!completed && !cancelled() && worker.state() != QProcess::NotRunning &&
               timer.elapsed() < (seconds + warmup + 15) * 1000)
            poll(true);
        progress("checking blackout and stop");
        send({{"type", "blackout"}, {"enabled", true}});
        QElapsedTimer shutdown;
        shutdown.start();
        while ((!black || !black_ack) && shutdown.elapsed() < 2500 && worker.state() != QProcess::NotRunning)
            poll(false);
        send({{"type", "stop"}});
        bool stopped = worker.waitForFinished(4000) && worker.exitStatus() == QProcess::NormalExit &&
                       worker.exitCode() == 0;
        if (!stopped) {
            worker.kill();
            worker.waitForFinished(2000);
        }
        auto summary =
            evidence.report(project, seconds, hz, simulate, cancelled(), stopped, black && black_ack);
        summary["journal_bytes"] = bytes_written;
        summary["warmup_seconds"] = warmup;
        return finish(std::move(summary));
    } catch (const std::exception& e) {
        if (worker.state() != QProcess::NotRunning) {
            worker.kill();
            worker.waitForFinished(2000);
        }
        std::cerr << e.what() << '\n';
        // Do not overwrite an existing run when its directory was rejected.
        if (owns_report && std::filesystem::exists(report_path / "setup.json") &&
            !std::filesystem::exists(report_path / "summary.json"))
            try {
                atomic_write(report_path / "summary.json",
                             Json{{"status", cancelled() ? "incomplete" : "fail"},
                                  {"cancelled", cancelled()},
                                  {"error", e.what()},
                                  {"release_qualified", false}}
                                 .dump(2));
            } catch (...) {
            }
        return cancelled() ? 3 : 1;
    }
}
