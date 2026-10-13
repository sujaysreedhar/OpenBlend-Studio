// SPDX-License-Identifier: GPL-3.0-only
#include "main_window.hpp"
#include "ps/scan.hpp"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHeaderView>
#include <QListWidget>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QWizard>
#include <QWizardPage>
#include <fstream>
namespace {
class ScanPage : public QWizardPage {
  public:
    std::function<bool()> ready = [] { return true; };
    bool isComplete() const override {
        return ready();
    }
    void changed() {
        emit completeChanged();
    }
};
ps::Json read_json(const QString& file) {
    std::ifstream in(std::filesystem::path(file.toStdWString()));
    if (!in)
        throw std::runtime_error("Cannot read scan report");
    ps::Json j;
    in >> j;
    return j;
}
QLabel* help(const QString& text) {
    auto* l = new QLabel(text);
    l->setWordWrap(true);
    return l;
}
QVBoxLayout* page_layout(QWizardPage* page) {
    auto* outer = new QVBoxLayout(page);
    auto* scroll = new QScrollArea;
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    auto* body = new QWidget;
    scroll->setWidget(body);
    outer->addWidget(scroll);
    return new QVBoxLayout(body);
}
} // namespace
void MainWindow::calibrate_installation() {
    if (project_.locked) {
        error("Unlock configuration before calibration");
        return;
    }
    QWizard wizard(this);
    wizard.setObjectName("calibrationWizard");
    wizard.setWindowTitle("Guided camera calibration");
    auto available = screen()->availableGeometry().size();
    wizard.resize(std::min(920, int(available.width() * .95)), std::min(760, int(available.height() * .9)));
    wizard.setOption(QWizard::NoBackButtonOnLastPage, false);
    auto* setup = new ScanPage;
    setup->setTitle("Choose camera and planar projectors");
    auto* config = page_layout(setup);
    config->addWidget(help("Use a stationary camera with fixed exposure, focus and white balance. Selected "
                           "projectors must each have one enabled surface and share a source. Other selected "
                           "outputs stay black while one is scanned."));
    auto* targets = new QListWidget;
    targets->setObjectName("scanTargets");
    targets->setMaximumHeight(110);
    for (const auto& o : project_.outputs) {
        auto* item = new QListWidgetItem(QString::fromStdString(o.name), targets);
        item->setData(Qt::UserRole, QString::fromStdString(o.id));
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(o.enabled && o.surfaces.size() == 1 && o.surfaces[0].enabled ? Qt::Checked
                                                                                         : Qt::Unchecked);
    }
    config->addWidget(targets);
    auto* fields = new QFormLayout;
    config->addLayout(fields);
    auto* mode = new QComboBox;
    mode->setObjectName("scanMode");
    mode->addItems({"Camera and physical displays", "Synthetic demonstration", "Analyze saved captures"});
#ifndef PS_WITH_OPENCV
    mode->setCurrentIndex(1);
#endif
    fields->addRow("Capture mode", mode);
    auto spin = [&](const char* name, int lo, int hi, int v) {
        auto* s = new QSpinBox;
        s->setObjectName(name);
        s->setRange(lo, hi);
        s->setValue(v);
        return s;
    };
    auto* camera = spin("scanCamera", 0, 31, 0);
    fields->addRow("DirectShow camera index", camera);
    auto* width = spin("scanCameraWidth", 2, 8192, 1280);
    auto* height = spin("scanCameraHeight", 2, 8192, 720);
    fields->addRow("Camera width / height", width);
    fields->addRow("", height);
    auto* exposure = new QDoubleSpinBox;
    exposure->setRange(-20, 20);
    exposure->setValue(-6);
    fields->addRow("Exposure (driver units)", exposure);
    auto* settle = spin("scanSettle", 100, 10000, 500);
    fields->addRow("Pattern settling (ms)", settle);
    auto* flush = spin("scanFlush", 1, 30, 4);
    fields->addRow("Drain camera frames per pattern", flush);
    auto* contrast = spin("scanContrast", 1, 254, 20);
    fields->addRow("Minimum contrast (8-bit)", contrast);
    auto* intrinsics = new QLineEdit;
    intrinsics->setPlaceholderText("Optional camera intrinsics JSON");
    auto* intrinsics_row = new QHBoxLayout;
    intrinsics_row->addWidget(intrinsics);
    auto* choose_intrinsics = new QPushButton("Browse…");
    intrinsics_row->addWidget(choose_intrinsics);
    fields->addRow("Lens correction", intrinsics_row);
    connect(choose_intrinsics, &QPushButton::clicked, &wizard, [&] {
        auto f = QFileDialog::getOpenFileName(&wizard, "Camera intrinsics", {}, "JSON (*.json)");
        if (!f.isEmpty())
            intrinsics->setText(f);
    });
    auto* folder =
        new QLineEdit(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) +
                      "/ProjectionStudio/Scans/" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss"));
    folder->setObjectName("scanFolder");
    auto* folder_row = new QHBoxLayout;
    folder_row->addWidget(folder);
    auto* browse = new QPushButton("Browse…");
    folder_row->addWidget(browse);
    fields->addRow("Evidence folder", folder_row);
    connect(browse, &QPushButton::clicked, &wizard, [&] {
        auto f = QFileDialog::getExistingDirectory(&wizard, "Choose scan folder");
        if (!f.isEmpty())
            folder->setText(f);
    });
    wizard.addPage(setup);
    auto* framing = new ScanPage;
    framing->setTitle("Check camera framing and screen region");
    auto* framing_layout = page_layout(framing);
    framing_layout->addWidget(
        help("Probe the camera, check its driver settings, then select the desired rectangular screen area "
             "in normalized camera coordinates. All projectors use this common region. Areas outside each "
             "projector are clipped by the output. This workflow assumes a planar screen."));
    auto* probe = new QPushButton("Probe camera and exposure");
    probe->setObjectName("probeScanCamera");
    framing_layout->addWidget(probe);
    auto* camera_preview = new QLabel("Camera preview appears here");
    camera_preview->setAlignment(Qt::AlignCenter);
    camera_preview->setMinimumHeight(220);
    framing_layout->addWidget(camera_preview);
    auto* camera_status = help("Camera controls are driver-dependent. Check manual controls in the camera "
                               "application when unsupported.");
    framing_layout->addWidget(camera_status);
    auto* region_row = new QHBoxLayout;
    framing_layout->addLayout(region_row);
    std::array<QDoubleSpinBox*, 4> region{};
    for (int i = 0; i < 4; ++i) {
        region_row->addWidget(new QLabel(QStringList{"X", "Y", "Width", "Height"}[i]));
        region[i] = new QDoubleSpinBox;
        region[i]->setRange(0, 1);
        region[i]->setDecimals(4);
        region[i]->setSingleStep(.01);
        region[i]->setValue(i < 2 ? 0 : 1);
        region_row->addWidget(region[i]);
    }
    auto* fixed = new QCheckBox(
        "I checked that the camera is stationary and exposure, focus and white balance are fixed");
    fixed->setObjectName("scanCameraFixed");
    framing_layout->addWidget(fixed);
    bool probed = false;
    framing->ready = [&] { return mode->currentIndex() != 0 || (probed && fixed->isChecked()); };
    connect(fixed, &QCheckBox::toggled, framing, &ScanPage::changed);
    connect(mode, &QComboBox::currentIndexChanged, framing, &ScanPage::changed);
    wizard.addPage(framing);
    auto* capture = new ScanPage;
    capture->setTitle("Capture ordered patterns");
    auto* capture_layout = page_layout(capture);
    capture_layout->addWidget(
        help("Start creates a new evidence session, displays Gray-code patterns at native resolution, drains "
             "camera frames after settling, and captures five neutral ramps. Escape or B in an output, Stop, "
             "Blackout, or Cancel stops scanning. Evidence is kept after interruption."));
    auto* bar = new QHBoxLayout;
    capture_layout->addLayout(bar);
    auto* start = new QPushButton("Start scan");
    start->setObjectName("startCalibrationScan");
    bar->addWidget(start);
    auto* cancel = new QPushButton("Stop / Blackout");
    cancel->setObjectName("cancelCalibrationScan");
    cancel->setEnabled(false);
    bar->addWidget(cancel);
    auto* status = help("Ready");
    status->setObjectName("scanStatus");
    capture_layout->addWidget(status);
    auto* log = new QPlainTextEdit;
    log->setReadOnly(true);
    log->setMaximumBlockCount(300);
    capture_layout->addWidget(log);
    wizard.addPage(capture);
    auto* review = new ScanPage;
    review->setTitle("Review calibration before applying");
    auto* review_layout = page_layout(review);
    auto* summary = help("Complete a scan first");
    review_layout->addWidget(summary);
    auto* table = new QTableWidget(0, 5);
    table->setObjectName("scanResults");
    table->setHorizontalHeaderLabels({"Projector", "RMS px", "Inliers", "White clipping", "Color fit"});
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table->setMaximumHeight(130);
    review_layout->addWidget(table);
    auto* previews = new QHBoxLayout;
    review_layout->addLayout(previews);
    auto* projector = new QComboBox;
    previews->addWidget(projector);
    auto* layer = new QComboBox;
    layer->setObjectName("scanPreviewLayer");
    layer->addItems({"Captured white", "Confidence mask", "Error map", "Grid before calibration",
                     "Predicted grid after calibration"});
    previews->addWidget(layer);
    auto* preview = new QLabel;
    preview->setAlignment(Qt::AlignCenter);
    preview->setMinimumHeight(230);
    review_layout->addWidget(preview);
    auto* blend = new QCheckBox("Apply estimated overlap widths (verify manually)");
    review_layout->addWidget(blend);
    auto* color = new QCheckBox("Apply relative RGB/brightness response correction (camera estimate)");
    review_layout->addWidget(color);
    review_layout->addWidget(
        help("Apply saves before/after presets and one undo step. Refine the mesh, masks, blend and color in "
             "the editor, then use Simulate outputs or Present fullscreen to compare presets. Confidence "
             "maps describe camera pixels; existing output masks stay editable. Physical accuracy needs "
             "overlapping projectors and a camera."));
    wizard.addPage(review);
    ps::Json result;
    QString active_folder;
    bool analyzing = false, cancelling = false, probing = false;
    QProcess runner;
    QTimer deadline;
    deadline.setSingleShot(true);
#ifdef _WIN32
    struct Job {
        HANDLE handle{CreateJobObjectW(nullptr, nullptr)};
        Job() {
            if (handle) {
                JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
                info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
                if (!SetInformationJobObject(handle, JobObjectExtendedLimitInformation, &info,
                                             sizeof(info))) {
                    CloseHandle(handle);
                    handle = nullptr;
                }
            }
        }
        ~Job() {
            if (handle)
                CloseHandle(handle);
        }
    } job;
    connect(&runner, &QProcess::started, &wizard, [&] {
        auto handle = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, DWORD(runner.processId()));
        bool protected_process = job.handle && handle && AssignProcessToJobObject(job.handle, handle);
        if (handle)
            CloseHandle(handle);
        if (!protected_process) {
            runner.kill();
            status->setText("Cannot supervise the camera process; scan stopped");
        }
    });
#endif
    capture->ready = [&] { return runner.state() == QProcess::NotRunning && !result.is_null(); };
    review->ready = capture->ready;
    auto render_preview = [&] {
        if (result.is_null() || projector->currentIndex() < 0)
            return;
        try {
            const auto& t = result.at("targets").at(projector->currentIndex());
            auto base = active_folder + "/projector-" + QString::number(projector->currentIndex());
            QImage img;
            if (layer->currentIndex() == 1)
                img.load(base + "/confidence.pgm");
            else if (layer->currentIndex() == 2)
                img.load(base + "/error.pgm");
            else {
                img.load(base + "/000.pgm");
#ifdef PS_WITH_OPENCV
                if (result.at("undistorted").get<bool>()) {
                    auto session = read_json(active_folder + "/session.json");
                    auto f = ps::undistort_capture(
                        ps::read_pgm(std::filesystem::path((base + "/000.pgm").toStdWString())),
                        read_json(active_folder + "/" +
                                  QString::fromStdString(session.at("camera").at("intrinsics"))));
                    img =
                        QImage(f.pixels.data(), f.width, f.height, f.width, QImage::Format_Grayscale8).copy();
                }
#endif
                if (layer->currentIndex() >= 3) {
                    img = img.convertToFormat(QImage::Format_RGB32);
                    QPainter painter(&img);
                    painter.setPen(QPen(QColor(0, 220, 130), 2));
                    ps::Homography h;
                    h.matrix = t.at("matrix").get<std::array<double, 9>>();
                    auto mesh =
                        layer->currentIndex() == 3 ? ps::identity_mesh(17, 17) : t.at("mesh").get<ps::Mesh>();
                    auto at = [&](int x, int y) {
                        auto p = mesh.points[size_t(y) * mesh.columns + x];
                        auto q = ps::transform(
                            h, {p.x * (t.at("width").get<int>() - 1), p.y * (t.at("height").get<int>() - 1)});
                        return QPointF(q.x, q.y);
                    };
                    for (int y = 0; y < mesh.rows; ++y)
                        for (int x = 0; x < mesh.columns; ++x) {
                            if (x)
                                painter.drawLine(at(x - 1, y), at(x, y));
                            if (y)
                                painter.drawLine(at(x, y - 1), at(x, y));
                        }
                }
            }
            preview->setPixmap(
                QPixmap::fromImage(img).scaled(800, 270, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        } catch (const std::exception& e) {
            preview->setText(e.what());
        }
    };
    connect(projector, &QComboBox::currentIndexChanged, &wizard, render_preview);
    connect(layer, &QComboBox::currentIndexChanged, &wizard, render_preview);
    auto show_result = [&] {
        table->setRowCount(0);
        projector->clear();
        bool valid_color = true;
        for (const auto& t : result.at("targets")) {
            int row = table->rowCount();
            table->insertRow(row);
            QStringList cells{
                QString::fromStdString(t.at("name")),
                QString::number(t.at("rms_pixels").get<double>(), 'f', 3),
                QString::number(t.at("inliers").get<qulonglong>()),
                QString::number(t.at("clipped_white_fraction").get<double>() * 100, 'f', 1) + "%",
                t.at("photometric_valid").get<bool>() ? "Usable estimate" : "Exposure/response rejected"};
            for (int c = 0; c < 5; ++c)
                table->setItem(row, c, new QTableWidgetItem(cells[c]));
            valid_color &= t.at("photometric_valid").get<bool>();
            projector->addItem(cells[0]);
        }
        color->setEnabled(valid_color);
        color->setChecked(false);
        auto session = read_json(active_folder + "/session.json");
        summary->setText(QString("%1 projector(s), %2 footprint overlap(s). %3 Evidence: %4")
                             .arg(table->rowCount())
                             .arg(result.at("overlaps").size())
                             .arg(session.value("synthetic", false)
                                      ? "Synthetic demonstration; no hardware measurement."
                                      : "Software capture; physical alignment remains unqualified.")
                             .arg(active_folder));
        render_preview();
        capture->changed();
        review->changed();
    };
    auto controls = [&](bool running) {
        start->setEnabled(!running);
        probe->setEnabled(!running);
        cancel->setEnabled(running);
        setup->setEnabled(!running);
        framing->setEnabled(!running);
        wizard.button(QWizard::BackButton)->setEnabled(!running);
        capture->changed();
        review->changed();
    };
    auto abort = [&] {
        if (runner.state() == QProcess::NotRunning)
            return;
        cancelling = true;
        status->setText("Stopping scan…");
        if (!probing && !analyzing && !active_folder.isEmpty())
            try {
                ps::atomic_write(std::filesystem::path((active_folder + "/cancel.request").toStdWString()),
                                 "cancel");
            } catch (...) {
            }
        else
            runner.kill();
        deadline.start(2000);
    };
    connect(cancel, &QPushButton::clicked, &wizard, abort);
    connect(&deadline, &QTimer::timeout, &wizard, [&] {
        cancelling = true;
        runner.kill();
        status->setText("Runner stopped after timeout; incomplete evidence is preserved");
    });
    connect(&runner, &QProcess::readyReadStandardOutput, &wizard, [&] {
        auto text = QString::fromUtf8(runner.readAllStandardOutput());
        log->appendPlainText(text.trimmed());
        if (!cancelling)
            deadline.start(60000);
        if (!probing)
            status->setText(text.trimmed().right(220));
    });
    connect(&runner, &QProcess::readyReadStandardError, &wizard,
            [&] { log->appendPlainText(QString::fromUtf8(runner.readAllStandardError()).trimmed()); });
    connect(&runner, &QProcess::errorOccurred, &wizard, [&](QProcess::ProcessError err) {
        if (err == QProcess::FailedToStart) {
            deadline.stop();
            status->setText(runner.errorString());
            controls(false);
        }
    });
    connect(&runner, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), &wizard,
            [&](int code, QProcess::ExitStatus exit) {
                deadline.stop();
                controls(false);
                try {
                    if (cancelling) {
                        status->setText("Scan cancelled; evidence retained");
                        result = nullptr;
                        if (!probing && !analyzing) {
                            auto session = read_json(active_folder + "/session.json");
                            if (session.at("status") == "capturing" || session.at("status") == "pending") {
                                session["status"] = "interrupted";
                                session["error"] = "Runner stopped by editor";
                                ps::atomic_write(
                                    std::filesystem::path((active_folder + "/session.json").toStdWString()),
                                    session.dump(2));
                            }
                        }
                    } else if (code != 0 || exit != QProcess::NormalExit) {
                        status->setText("Scan failed. Review the log and retained session before retrying.");
                    } else if (probing) {
                        auto j = read_json(temporary_.path() + "/camera-probe/camera.json");
                        QImage img(temporary_.path() + "/camera-probe/camera.ppm");
                        camera_preview->setPixmap(
                            QPixmap::fromImage(img).scaled(780, 270, Qt::KeepAspectRatio));
                        camera_status->setText(QString::fromStdString(j.dump(2)));
                        probed = true;
                        framing->changed();
                        status->setText("Camera probe complete");
                    } else {
                        result = read_json(active_folder + "/result.json");
                        show_result();
                        status->setText("Calibration ready for review");
                    }
                } catch (const std::exception& e) {
                    result = nullptr;
                    status->setText(e.what());
                }
                capture->changed();
                review->changed();
            });
    auto launch = [&](const QStringList& args) {
        cancelling = false;
        log->clear();
        controls(true);
        deadline.start(60000);
        runner.start(QApplication::applicationDirPath() + "/projection-scan.exe", args);
    };
    connect(probe, &QPushButton::clicked, &wizard, [&] {
        probed = false;
        framing->changed();
        probing = true;
        analyzing = false;
        launch({"--probe", temporary_.path() + "/camera-probe", "--camera", QString::number(camera->value()),
                "--width", QString::number(width->value()), "--height", QString::number(height->value()),
                "--exposure", QString::number(exposure->value())});
    });
    for (auto* s : {camera, width, height})
        connect(s, &QSpinBox::valueChanged, &wizard, [&] {
            probed = false;
            framing->changed();
        });
    connect(exposure, &QDoubleSpinBox::valueChanged, &wizard, [&] {
        probed = false;
        framing->changed();
    });
    connect(start, &QPushButton::clicked, &wizard, [&] {
        try {
            result = nullptr;
            capture->changed();
            review->changed();
            active_folder = folder->text();
            probing = false;
            analyzing = mode->currentIndex() == 2;
            if (analyzing) {
                launch({"--analyze", active_folder});
                return;
            }
            if (mode->currentIndex() == 0 && !(probed && fixed->isChecked()))
                throw std::runtime_error("Probe the camera and confirm fixed camera settings first");
            if (worker_.state() != QProcess::NotRunning || !adapter_manifest_.isEmpty())
                throw std::runtime_error("Stop output and attached adapters before scanning");
            std::vector<std::string> ids;
            for (int i = 0; i < targets->count(); ++i)
                if (targets->item(i)->checkState() == Qt::Checked)
                    ids.push_back(targets->item(i)->data(Qt::UserRole).toString().toStdString());
            ps::ScanOptions o;
            o.camera_index = camera->value();
            o.camera_width = width->value();
            o.camera_height = height->value();
            o.exposure = exposure->value();
            o.settle_ms = settle->value();
            o.flush_frames = flush->value();
            o.min_contrast = contrast->value();
            o.intrinsics = intrinsics->text().toStdString();
            o.region = {region[0]->value(), region[1]->value(), region[2]->value(), region[3]->value()};
            ps::create_scan(std::filesystem::path(active_folder.toStdWString()),
                            ps::scan_manifest(project_, ids, o));
            QStringList args{"--scan", active_folder};
            if (mode->currentIndex() == 1)
                args << "--simulate";
            launch(args);
        } catch (const std::exception& e) {
            status->setText(e.what());
            controls(false);
        }
    });
    connect(&wizard, &QWizard::currentIdChanged, &wizard, [&](int id) {
        if (id == 3 && !result.is_null())
            render_preview();
    });
    if (wizard.exec() == QDialog::Accepted && !result.is_null()) {
        try {
            auto calibrated = ps::apply_scan(project_, result, blend->isChecked(), color->isChecked());
            mutate("Apply camera calibration", [&](auto& p) { p = calibrated; });
        } catch (const std::exception& e) {
            error(e.what());
        }
    }
    if (runner.state() != QProcess::NotRunning) {
        abort();
        if (!runner.waitForFinished(2500)) {
            runner.kill();
            runner.waitForFinished(1000);
        }
    }
}
