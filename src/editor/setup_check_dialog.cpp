// SPDX-License-Identifier: GPL-3.0-only
#include "main_window.hpp"
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QStandardPaths>
#include <QTableWidget>
#include <QVBoxLayout>
#include <fstream>
namespace {
class CheckDialog : public QDialog {
  public:
    QProcess process;
    std::function<void()> cancel;
    bool closing{};
    using QDialog::QDialog;
    void closeEvent(QCloseEvent* event) override {
        if (process.state() != QProcess::NotRunning) {
            closing = true;
            if (cancel)
                cancel();
            event->ignore();
        } else
            QDialog::closeEvent(event);
    }
};
} // namespace
void MainWindow::check_setup() {
    if (auto* existing = findChild<QDialog*>("setupCheck")) {
        existing->show();
        existing->raise();
        return;
    }
    auto* dialog = new CheckDialog(this);
    dialog->setObjectName("setupCheck");
    dialog->process.setParent(dialog);
    dialog->process.setObjectName("setupCheckProcess");
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle("Check my setup");
    dialog->resize(990, 670);
    auto* layout = new QVBoxLayout(dialog);
    auto* intro = new QLabel("Check displays, GPU support and source capture. Timed runs measure output "
                             "stability and test Blackout and Stop.");
    intro->setWordWrap(true);
    layout->addWidget(intro);
    auto* form = new QFormLayout;
    layout->addLayout(form);
    auto* mode = new QComboBox;
    mode->setObjectName("checkMode");
    mode->addItem("Setup check only", 0);
    mode->addItem("Output test — 1 minute", 60);
    mode->addItem("Output test — 10 minutes", 600);
    mode->addItem("Endurance — 8 hours", 28800);
    form->addRow("Test", mode);
    auto* capture = new QCheckBox("Probe the project's live capture sources");
    capture->setChecked(true);
    form->addRow(capture);
    auto* simulation = new QCheckBox("Simulate output windows (does not qualify physical displays)");
    simulation->setObjectName("checkSimulation");
    form->addRow(simulation);
    auto* fullscreen = new QCheckBox("Use assigned displays fullscreen");
    form->addRow(fullscreen);
    auto* hz = new QDoubleSpinBox;
    hz->setRange(1, 1000);
    hz->setValue(60);
    hz->setSuffix(" Hz");
    form->addRow("Target refresh", hz);
    auto* directory = new QLineEdit(QDir::current().filePath("reports"));
    directory->setObjectName("checkReportDirectory");
    auto* destination = new QHBoxLayout;
    destination->addWidget(directory);
    auto* browse = new QPushButton("Browse…");
    destination->addWidget(browse);
    form->addRow("Reports folder", destination);
    connect(browse, &QPushButton::clicked, dialog, [=] {
        auto path = QFileDialog::getExistingDirectory(dialog, "Reports folder", directory->text());
        if (!path.isEmpty())
            directory->setText(path);
    });
    connect(simulation, &QCheckBox::toggled, dialog, [=](bool on) {
        if (on)
            fullscreen->setChecked(false);
        fullscreen->setEnabled(!on);
    });
    auto* status =
        new QLabel("Ready. Reports contain system details and project paths; capture pixels are not saved.");
    status->setWordWrap(true);
    status->setObjectName("checkStatus");
    layout->addWidget(status);
    auto* progress = new QProgressBar;
    progress->setRange(0, 1);
    progress->setValue(0);
    layout->addWidget(progress);
    auto* table = new QTableWidget(0, 3);
    table->setObjectName("checkResults");
    table->setHorizontalHeaderLabels({"Check", "Result", "Details / next step"});
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(table, 1);
    auto* actions = new QHBoxLayout;
    layout->addLayout(actions);
    auto* start = new QPushButton("Run checks");
    start->setObjectName("runSetupCheck");
    actions->addWidget(start);
    auto* cancel = new QPushButton("Cancel test");
    cancel->setObjectName("cancelSetupCheck");
    cancel->setEnabled(false);
    actions->addWidget(cancel);
    auto* open = new QPushButton("Open report folder");
    open->setEnabled(false);
    actions->addWidget(open);
    auto* fallback = new QPushButton("Use window capture");
    fallback->setEnabled(adapter_hwnd_ != 0 && !adapter_manifest_.isEmpty());
    actions->addWidget(fallback);
    connect(fallback, &QPushButton::clicked, this, [this] { use_capture_fallback(); });
    auto* close = new QPushButton("Close");
    actions->addWidget(close);
    connect(close, &QPushButton::clicked, dialog, &QDialog::close);
    auto report = std::make_shared<QString>();
    auto bytes = std::make_shared<QByteArray>();
    auto stop_timer = new QTimer(dialog);
    stop_timer->setSingleShot(true);
    connect(stop_timer, &QTimer::timeout, dialog, [=] {
        if (dialog->process.state() != QProcess::NotRunning)
            dialog->process.kill();
    });
    dialog->cancel = [=] {
        cancel->setEnabled(false);
        status->setText("Stopping the test and saving partial evidence…");
        dialog->process.write("cancel\n");
        stop_timer->start(7000);
    };
    connect(cancel, &QPushButton::clicked, dialog, [=] { dialog->cancel(); });
    connect(open, &QPushButton::clicked, dialog,
            [=] { QDesktopServices::openUrl(QUrl::fromLocalFile(*report)); });
    connect(&dialog->process, &QProcess::readyReadStandardOutput, dialog, [=] {
        *bytes += dialog->process.readAllStandardOutput();
        while (bytes->contains('\n')) {
            auto end = bytes->indexOf('\n');
            auto text = bytes->left(end);
            bytes->remove(0, end + 1);
            auto j = ps::Json::parse(text.toStdString(), nullptr, false);
            if (j.is_discarded())
                continue;
            if (j.value("type", "") == "progress") {
                status->setText(QString::fromStdString(j.value("phase", "Running")));
                int length = mode->currentData().toInt();
                if (length) {
                    progress->setRange(0, length);
                    progress->setValue(int(j.value("seconds", 0.)));
                }
            }
        }
    });
    auto show_report = [=] {
        table->setRowCount(0);
        for (const auto& filename : {"setup.json", "summary.json"}) {
            auto path = std::filesystem::path(report->toStdWString()) / "results" / filename;
            std::ifstream file(path);
            auto j = ps::Json::parse(file, nullptr, false);
            if (j.is_discarded())
                continue;
            for (const auto& c : j.value("checks", ps::Json::array())) {
                int row = table->rowCount();
                table->insertRow(row);
                table->setItem(row, 0, new QTableWidgetItem(QString::fromStdString(c.value("id", ""))));
                table->setItem(row, 1, new QTableWidgetItem(QString::fromStdString(c.value("status", ""))));
                auto detail = c.value("detail", "");
                if (!c.value("action", "").empty())
                    detail += " — " + c.at("action").get<std::string>();
                table->setItem(row, 2, new QTableWidgetItem(QString::fromStdString(detail)));
            }
        }
        table->resizeRowsToContents();
    };
    connect(
        &dialog->process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), dialog,
        [=](int code, QProcess::ExitStatus exit) {
            stop_timer->stop();
            start->setEnabled(true);
            cancel->setEnabled(false);
            open->setEnabled(QFileInfo::exists(*report));
            mode->setEnabled(true);
            capture->setEnabled(true);
            simulation->setEnabled(true);
            fullscreen->setEnabled(!simulation->isChecked());
            directory->setEnabled(true);
            browse->setEnabled(true);
            hz->setEnabled(true);
            progress->setRange(0, 1);
            progress->setValue(1);
            show_report();
            status->setText(
                exit != QProcess::NormalExit
                    ? "Test process stopped unexpectedly. Partial reports are in the report folder."
                : code == 0
                    ? "Checks passed. Physical and simulator qualification still require their test records."
                : code == 2 ? "Checks incomplete — see the required actions below."
                : code == 3 ? "Test cancelled. Partial evidence saved."
                            : "A check failed. Inspect the report for details.");
            auto error = dialog->process.readAllStandardError();
            if (!error.isEmpty())
                status->setText(status->text() + " " + QString::fromUtf8(error));
            if (dialog->closing)
                dialog->close();
        });
    connect(&dialog->process, &QProcess::errorOccurred, dialog, [=](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            stop_timer->stop();
            start->setEnabled(true);
            cancel->setEnabled(false);
            mode->setEnabled(true);
            capture->setEnabled(true);
            simulation->setEnabled(true);
            fullscreen->setEnabled(!simulation->isChecked());
            directory->setEnabled(true);
            browse->setEnabled(true);
            hz->setEnabled(true);
            progress->setRange(0, 1);
            progress->setValue(0);
            status->setText("Cannot start projection-qualify.exe: " + dialog->process.errorString());
            if (dialog->closing)
                dialog->close();
        }
    });
    connect(start, &QPushButton::clicked, dialog, [=, this] {
        if (worker_.state() != QProcess::NotRunning || attach_.state() != QProcess::NotRunning ||
            !adapter_manifest_.isEmpty()) {
            status->setText("Stop active output or application integration before running setup tests. The "
                            "capture fallback remains available.");
            return;
        }
        try {
            if (directory->text().trimmed().isEmpty())
                throw std::runtime_error("Choose a reports folder");
            *report = QDir(directory->text())
                          .absoluteFilePath(QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss") +
                                            "-" + QString::fromStdString(ps::make_id()));
            // Preserve the input snapshot beside the checker's new, empty results directory.
            auto candidate = project_;
            auto base = path_.isEmpty() ? std::filesystem::current_path() / "project.json"
                                        : std::filesystem::path(path_.toStdWString());
            auto absolute = [&](std::string& path) {
                if (!path.empty())
                    path = ps::win::utf8(ps::resolve_asset(base, path).wstring());
            };
            for (auto& s : candidate.sources)
                absolute(s.image);
            for (auto& o : candidate.outputs)
                for (auto& s : o.surfaces)
                    absolute(s.mask.bitmap);
            for (auto& p : candidate.presets)
                for (auto& o : p.outputs)
                    for (auto& s : o.surfaces)
                        absolute(s.mask.bitmap);
            if (!QDir().mkpath(*report))
                throw std::runtime_error("Cannot create the selected report folder");
            QString snapshot = QDir(*report).filePath("input-project.json");
            ps::save_project(candidate, std::filesystem::path(snapshot.toStdWString()));
            QStringList args{"--project", snapshot, "--report", QDir(*report).filePath("results")};
            if (capture->isChecked())
                args << "--probe-capture";
            if (mode->currentData().toInt() > 0) {
                args << "--run" << "--seconds" << QString::number(mode->currentData().toInt())
                     << "--target-hz" << QString::number(hz->value());
                if (simulation->isChecked())
                    args << "--simulate";
                else if (fullscreen->isChecked())
                    args << "--fullscreen";
            }
            table->setRowCount(0);
            bytes->clear();
            start->setEnabled(false);
            cancel->setEnabled(true);
            open->setEnabled(false);
            mode->setEnabled(false);
            capture->setEnabled(false);
            simulation->setEnabled(false);
            fullscreen->setEnabled(false);
            directory->setEnabled(false);
            browse->setEnabled(false);
            hz->setEnabled(false);
            progress->setRange(0, 0);
            status->setText("Checking this installation…");
            dialog->process.start(QApplication::applicationDirPath() + "/projection-qualify.exe", args);
        } catch (const std::exception& e) {
            status->setText(QString::fromUtf8(e.what()));
        }
    });
    dialog->show();
}
void MainWindow::use_capture_fallback() {
    if (project_.locked) {
        error("Unlock configuration before changing a source");
        return;
    }
    try {
        auto choices = ps::win::windows();
        auto selected = std::find_if(choices.begin(), choices.end(), [&](const auto& w) {
            return reinterpret_cast<std::uint64_t>(w.hwnd) == adapter_hwnd_;
        });
        if (selected == choices.end())
            throw std::runtime_error(
                "The attached window is unavailable; select it again from Attach application");
        auto target = adapter_output_;
        auto window = *selected;
        bool exists = std::any_of(project_.outputs.begin(), project_.outputs.end(),
                                  [&](const auto& o) { return o.id == target; });
        if (!exists)
            throw std::runtime_error("The adapter's target projector no longer exists");
        stop_output();
        mutate("Use window capture", [&](auto& p) {
            ps::Source s;
            s.id = ps::make_id();
            s.name = window.title;
            s.type = "window";
            s.window_handle = reinterpret_cast<std::uint64_t>(window.hwnd);
            s.window_title = window.title;
            s.executable = window.executable;
            s.window_class = window.window_class;
            p.sources.push_back(s);
            for (auto& o : p.outputs)
                if (o.id == target)
                    for (auto& f : o.surfaces)
                        f.source_id = s.id;
        });
        start_output(false);
    } catch (const std::exception& e) {
        error(QString::fromUtf8(e.what()));
    }
}
