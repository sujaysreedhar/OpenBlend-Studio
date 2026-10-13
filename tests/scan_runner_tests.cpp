// SPDX-License-Identifier: GPL-3.0-only
#include "ps/scan.hpp"
#include <QCoreApplication>
#include <QProcess>
#include <QProcessEnvironment>
#include <fstream>
#include <iostream>
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    auto root = std::filesystem::current_path() / ("scan-runner-test-" + ps::make_id());
    try {
        auto require = [](bool ok, const char* message) {
            if (!ok)
                throw std::runtime_error(message);
        };
        auto p = ps::example_project(2);
        for (auto& o : p.outputs) {
            o.width = 64;
            o.height = 48;
        }
        ps::ScanOptions options;
        options.camera_width = 112;
        options.camera_height = 48;
        auto plan = ps::scan_manifest(p, {p.outputs[0].id, p.outputs[1].id}, options);
        auto run = [&](const QStringList& args, const QString& scale = QString("1")) {
            QProcess worker;
            auto env = QProcessEnvironment::systemEnvironment();
            env.insert("QT_QPA_PLATFORM", "offscreen");
            env.insert("QT_SCALE_FACTOR", scale);
            worker.setProcessEnvironment(env);
            worker.start(QCoreApplication::applicationDirPath() + "/projection-scan.exe", args);
            require(worker.waitForStarted(5000), "Scan runner did not start");
            if (!worker.waitForFinished(30000)) {
                worker.kill();
                worker.waitForFinished(1000);
                throw std::runtime_error("Scan runner timed out");
            }
            if (worker.exitCode() != 0)
                std::cout << worker.readAllStandardError().toStdString();
            return worker.exitCode();
        };
        require(run({"--check-pattern-paint"}) == 0, "Native pattern paint failed");
        require(run({"--check-pattern-paint"}, "2") == 0, "Scaled native pattern paint failed");
        auto folder = root / "complete";
        ps::create_scan(folder, plan);
        auto path = QString::fromStdWString(folder.wstring());
        require(run({"--scan", path, "--simulate"}) == 0, "Synthetic runner failed");
        auto read = [](const std::filesystem::path& f) {
            std::ifstream in(f);
            ps::Json j;
            in >> j;
            return j;
        };
        auto session = read(folder / "session.json"), result = read(folder / "result.json");
        require(session.at("status") == "complete" && session.at("synthetic") == true,
                "Runner did not record synthetic completion");
        require(result.at("physically_validated") == false, "Synthetic run claims hardware qualification");
        require(ps::validate(ps::apply_scan(p, result, true, true)).empty(),
                "Runner result cannot be applied");
        require(run({"--analyze", path}) == 0, "Complete evidence cannot be reanalyzed");
        require(run({"--scan", path, "--simulate"}) != 0, "Complete evidence overwritten");
        require(session == read(folder / "session.json"), "Rejected rerun changed evidence");
        auto cancelled = root / "cancelled";
        ps::create_scan(cancelled, plan);
        ps::atomic_write(cancelled / "cancel.request", "cancel");
        require(run({"--scan", QString::fromStdWString(cancelled.wstring()), "--simulate"}) == 2,
                "Cancellation did not stop runner");
        require(read(cancelled / "session.json").at("status") == "cancelled" &&
                    !std::filesystem::exists(cancelled / "result.json"),
                "Cancelled scan presented complete result");
        auto invalid = root / "invalid";
        plan["targets"][0]["patterns"][0] = "../escape.pgm";
        ps::create_scan(invalid, plan);
        require(run({"--scan", QString::fromStdWString(invalid.wstring()), "--simulate"}) != 0,
                "Malformed sequence accepted");
        require(!std::filesystem::exists(invalid / "escape.pgm"), "Runner wrote outside target folder");
        std::filesystem::remove_all(root);
        std::cout << "Scan process completion, reanalysis, preserved evidence, cancellation and preflight "
                     "tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
