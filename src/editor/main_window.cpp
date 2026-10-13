// SPDX-License-Identifier: GPL-3.0-only
#include "main_window.hpp"
#include "ps/calibration.hpp"
#include "ps/simulator.hpp"
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QInputDialog>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QShortcut>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTextEdit>
#include <QToolBar>
#include <QToolButton>
#include <QUuid>
#include <fstream>
#include <QSignalBlocker>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QSettings>
#include <QSlider>
#include <QScrollBar>
#include <set>
namespace {
std::filesystem::path fs(const QString& q) {
    return std::filesystem::path(q.toStdWString());
}
QString qs(const std::string& s) {
    return QString::fromStdString(s);
}
template <class T> T* control(QFormLayout* form, const QString& label) {
    auto* parent = form->parentWidget();
    auto* widget = parent->findChild<T*>(label, Qt::FindDirectChildrenOnly);
    if (!widget) {
        widget = new T(parent);
        widget->setObjectName(label);
        widget->setAccessibleName(label);
        if constexpr (std::is_same_v<T, QPushButton> || std::is_same_v<T, QCheckBox>) {
            widget->setText(label);
            form->addRow(widget);
        } else
            form->addRow(label, widget);
    }
    QObject::disconnect(widget, nullptr, nullptr, nullptr);
    return widget;
}
class Edit : public QUndoCommand {
    ps::Project* target_;
    ps::Json undo_patch_, redo_patch_;
    std::function<void()> changed_;

  public:
    Edit(ps::Project* target, ps::Project before, ps::Project after, const QString& title,
         std::function<void()> changed)
        : QUndoCommand(title), target_(target), changed_(std::move(changed)) {
        auto a = ps::project_json(before), b = ps::project_json(after);
        undo_patch_ = ps::Json::diff(b, a);
        redo_patch_ = ps::Json::diff(a, b);
    }
    void undo() override {
        *target_ = ps::project_from_json(ps::project_json(*target_).patch(undo_patch_));
        changed_();
    }
    void redo() override {
        *target_ = ps::project_from_json(ps::project_json(*target_).patch(redo_patch_));
        changed_();
    }
};
ps::Project absolute_assets(ps::Project p, const QString& project_path) {
    auto base = project_path.isEmpty() ? std::filesystem::current_path() / "project.json" : fs(project_path);
    auto absolute = [&](std::string& path) {
        if (!path.empty()) {
            auto resolved = ps::resolve_asset(base, path);
            auto u = resolved.u8string();
            path.assign(reinterpret_cast<const char*>(u.data()), u.size());
        }
    };
    for (auto& s : p.sources)
        absolute(s.image);
    for (auto& o : p.outputs)
        for (auto& f : o.surfaces)
            absolute(f.mask.bitmap);
    for (auto& preset : p.presets)
        for (auto& o : preset.outputs)
            for (auto& f : o.surfaces)
                absolute(f.mask.bitmap);
    return p;
}
} // namespace
MainWindow::MainWindow() : project_(ps::example_project(3)), remote_(this) {
    SetWindowDisplayAffinity(reinterpret_cast<HWND>(winId()), 0x11);
    resize(1420, 900);
    setWindowTitle("Projection Studio");
    setDockNestingEnabled(true);
    auto* file = menuBar()->addMenu("&Project");
    file->addAction(
        "New installation", this,
        [this] {
            if (!undo_.isClean() && QMessageBox::question(this, "New installation",
                                                          "Discard unsaved changes?") != QMessageBox::Yes)
                return;
            stop_output();
            project_ = ps::example_project();
            path_.clear();
            undo_.clear();
            view_->output_index = view_->surface_index = 0;
            refresh();
        },
        QKeySequence::New);
    file->addAction(
        "Open...", this,
        [this] {
            auto path =
                QFileDialog::getOpenFileName(this, "Open project", {}, "Projection projects (*.json)");
            if (!path.isEmpty())
                load(path);
        },
        QKeySequence::Open);
    file->addAction("Save", this, [this] { save(); }, QKeySequence::Save);
    file->addAction(
        "Save as...", this,
        [this] {
            auto path =
                QFileDialog::getSaveFileName(this, "Save project", path_, "Projection projects (*.json)");
            if (!path.isEmpty())
                save(path);
        },
        QKeySequence::SaveAs);
    file->addSeparator();
    file->addAction("Recover backup...", this, [this] {
        auto path =
            QFileDialog::getOpenFileName(this, "Recover project backup", {}, "Project backups (*.bak)");
        if (!path.isEmpty()) {
            load(path);
            path_.clear();
            undo_.resetClean();
        }
    });
    file->addAction("Guided setup…", this, [this] { setup_installation(); });
    file->addAction("Export view frustums...", this, [this] { export_views(); });
    file->addAction("Quit", this, &QWidget::close, QKeySequence::Quit);
    auto* edit = menuBar()->addMenu("&Edit");
    auto* undoAction = undo_.createUndoAction(this, "Undo");
    undoAction->setObjectName("undoMapping");
    undoAction->setShortcut(QKeySequence::Undo);
    edit->addAction(undoAction);
    auto* redoAction = undo_.createRedoAction(this, "Redo");
    redoAction->setObjectName("redoMapping");
    redoAction->setShortcut(QKeySequence::Redo);
    edit->addAction(redoAction);
    auto* lock = edit->addAction("Lock configuration");
    lock->setObjectName("configurationLock");
    lock->setCheckable(true);
    connect(lock, &QAction::toggled, this, [this](bool value) {
        project_.locked = value;
        undo_.resetClean();
        refresh();
    });
    auto* examples = menuBar()->addMenu("&Examples");
    auto example = [this](int n, bool irregular) {
        if (!undo_.isClean() &&
            QMessageBox::question(this, "Load example", "Discard unsaved changes?") != QMessageBox::Yes)
            return;
        stop_output();
        project_ = ps::example_project(n, irregular);
        path_.clear();
        undo_.clear();
        view_->output_index = view_->surface_index = 0;
        refresh();
    };
    examples->addAction("Single projector", this, [example] { example(1, false); });
    examples->addAction("Three-projector flight deck", this, [example] { example(3, false); });
    examples->addAction("Architectural facets", this, [example] { example(1, true); });
    auto* tools = menuBar()->addMenu("&Tools");
    auto* check_action = tools->addAction("Check my setup…", this, [this] { check_setup(); });
    check_action->setObjectName("checkSetupAction");
    tools->addAction("Use window capture for attached application…", this,
                     [this] { use_capture_fallback(); });
    tools->addAction("Attach application…", this, [this] { attach_application(); });
    tools->addAction("Application profiles…", this, [this] { manage_profiles(); });
    tools->addAction("Presets and startup…", this, [this] { manage_presets(); });
    tools->addAction("Keyboard shortcuts…", this, [this] { edit_shortcuts(); });
    tools->addAction("Output diagnostics…", this, [this] { show_diagnostics(); });
    tools->addAction("MSFS camera export...", this, [this] { camera_patch(); });
    tools->addAction("Restore MSFS camera backup...", this, [this] { camera_patch(true); });
    tools->addAction("Import planar calibration...", this, [this] { import_calibration(); });
    tools->addAction("Guided camera calibration...", this, [this] { calibrate_installation(); });
    tools->addAction("Local control API...", this, [this] { configure_remote(); });
    tools->addAction("Show capabilities", this, [this] {
        QMessageBox::information(
            this, "Build capabilities",
            "Implemented: manual mapping, source crops, masks, blending, color, presets, desktop/window "
            "capture, DX11/DX12 adapters, planar Gray-code calibration and MSFS 2020/2024 camera "
            "exports.\n\nHardware "
            "and simulator qualification is pending. OpenGL/Vulkan, virtual display, HDR/stereo, head "
            "tracking, "
            "interpolation and native scanout backends are not implemented in this build. See "
            "docs/FEATURES.md.");
    });
    auto* toolbar = addToolBar("Installation");
    toolbar->setObjectName("installationToolbar");
    toolbar->setMovable(false);
    toolbar->setIconSize({18, 18});
    auto* wordmark = new QLabel("  PROJECTION <b>STUDIO</b>  ");
    wordmark->setStyleSheet("font-size:15px;letter-spacing:1px;color:#e2efef;padding:10px");
    toolbar->addWidget(wordmark);
    toolbar->addSeparator();
    toolbar->addAction("Setup", this, [this] { setup_installation(); });
    toolbar->addAction("Save", this, [this] { save(); });
    toolbar->addSeparator();
    toolbar->addAction("Simulate outputs", this, [this] { start_output(false); });
    toolbar->addAction("Present fullscreen", this, [this] { start_output(true); });
    auto* stop = toolbar->addAction("Stop", this, [this] { stop_output(); });
    stop->setObjectName("stopOutputs");
    stop->setShortcut(QKeySequence("Ctrl+Escape"));
    toolbar->addAction("Identify", this, [this] {
        if (worker_.state() == QProcess::Running)
            send({{"type", "identify"}, {"seconds", 10}});
        else
            statusBar()->showMessage("Start outputs to identify their projector numbers", 4000);
    });
    auto* black = toolbar->addAction("Blackout");
    black->setObjectName("blackoutAction");
    black->setCheckable(true);
    black->setShortcut(QKeySequence("Ctrl+B"));
    connect(black, &QAction::toggled, this, [this](bool enabled) { set_blackout(enabled); });
    new QShortcut(QKeySequence("Ctrl+Shift+Escape"), this, [this] { stop_output(); });
    view_ = new MeshView(this);
    view_->project = &project_;
    setCentralWidget(view_);
    view_->preview = [this](bool source_mode) { return preview(source_mode); };
    previews_.completed = [this](const QString& key, const QImage& image, const QString& error) {
        if (key != preview_key_)
            return;
        render_error_ = error;
        if (!image.isNull())
            cached_preview_ = image;
        else if (!error.isEmpty()) {
            cached_preview_ = QImage(960, 540, QImage::Format_RGB32);
            cached_preview_.fill(QColor("#131a21"));
            QPainter painter(&cached_preview_);
            painter.setPen(Qt::white);
            painter.drawText(cached_preview_.rect().adjusted(30, 30, -30, -30), Qt::TextWordWrap,
                             "Preview unavailable — retrying\n" + error);
        }
        view_->update();
    };
    comparison_.completed = [this](const QString& key, const QImage& image, const QString&) {
        if (key == preview_key_ + "/compare") {
            comparison_image_ = image;
            view_->update();
        }
    };
    view_->source_preview = [this] { return comparison_image_; };
    view_->selection_changed = [this] { update_selection(); };
    auto* navigation = addToolBar("Viewport");
    navigation->setObjectName("viewportToolbar");
    addToolBarBreak();
    addToolBar(Qt::TopToolBarArea, navigation);
    navigation->addAction("Fit view", this, [this] { view_->fit_view(); });
    auto* snap = new QCheckBox("Snap 1%");
    navigation->addWidget(snap);
    connect(snap, &QCheckBox::toggled, this, [this](bool on) { view_->snap = on; });
    navigation->addSeparator();
    navigation->addWidget(new QLabel(" Source / output "));
    auto* compare = new QSlider(Qt::Horizontal);
    compare->setAccessibleName("Source and output comparison");
    compare->setRange(0, 100);
    compare->setValue(100);
    compare->setFixedWidth(130);
    navigation->addWidget(compare);
    connect(compare, &QSlider::valueChanged, this, [this](int value) {
        view_->compare = value / 100.;
        preview_dirty_ = true;
        view_->update();
    });
    navigation->addSeparator();
    selection_label_ = new QLabel;
    navigation->addWidget(selection_label_);
    for (auto** field : {&selection_x_, &selection_y_}) {
        *field = new QDoubleSpinBox;
        (*field)->setRange(-4, 5);
        (*field)->setDecimals(4);
        (*field)->setSingleStep(.001);
        (*field)->setFixedWidth(110);
        navigation->addWidget(*field);
    }
    selection_x_->setAccessibleName("Selected X coordinate or group mean");
    selection_y_->setAccessibleName("Selected Y coordinate or group mean");
    connect(selection_x_, &QDoubleSpinBox::editingFinished, this,
            [this] { view_->set_selection_coordinate(true, selection_x_->value()); });
    connect(selection_y_, &QDoubleSpinBox::editingFinished, this,
            [this] { view_->set_selection_coordinate(false, selection_y_->value()); });
    view_->begin_edit = [this] { before_ = project_; };
    view_->end_edit = [this] { commit_edit(); };
    view_->changed = [this] {
        preview_dirty_ = true;
        pending_update_ = true;
    };
    auto* installation = new QDockWidget("INSTALLATION", this);
    installation->setObjectName("installation");
    auto* left = new QWidget;
    auto* leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(8, 8, 8, 8);
    info_ = new QLabel;
    info_->setWordWrap(true);
    info_->setStyleSheet("color:#92a5b1;padding:6px");
    leftLayout->addWidget(info_);
    search_ = new QLineEdit;
    search_->setPlaceholderText("Find projectors or surfaces…");
    search_->setAccessibleName("Search installation");
    leftLayout->addWidget(search_);
    connect(search_, &QLineEdit::textChanged, this,
            [this](const QString& text) { filter_installation(text); });
    tree_ = new QTreeWidget;
    tree_->setHeaderHidden(true);
    tree_->setIndentation(18);
    leftLayout->addWidget(tree_);
    auto* addOutput = new QPushButton("+ Projector");
    leftLayout->addWidget(addOutput);
    connect(addOutput, &QPushButton::clicked, this, [this] {
        mutate("Add projector", [this](auto& p) {
            auto o = ps::example_project().outputs[0];
            o.name = "Projector " + std::to_string(p.outputs.size() + 1);
            o.surfaces[0].source_id = p.sources[0].id;
            p.outputs.push_back(o);
            view_->output_index = int(p.outputs.size() - 1);
            view_->surface_index = 0;
        });
    });
    auto* addSurface = new QPushButton("+ Mapping surface");
    leftLayout->addWidget(addSurface);
    connect(addSurface, &QPushButton::clicked, this, [this] {
        mutate("Add surface", [this](auto&) {
            auto* o = output();
            if (!o)
                return;
            ps::Surface s;
            s.id = ps::make_id();
            s.name = "Surface " + std::to_string(o->surfaces.size() + 1);
            s.source_id = project_.sources[0].id;
            s.mesh = ps::identity_mesh();
            o->surfaces.push_back(s);
            view_->surface_index = int(o->surfaces.size() - 1);
        });
    });
    auto* remove = new QPushButton("Remove selected surface");
    leftLayout->addWidget(remove);
    connect(remove, &QPushButton::clicked, this, [this] {
        mutate("Remove surface", [this](auto&) {
            auto* o = output();
            if (o && surface())
                o->surfaces.erase(o->surfaces.begin() + view_->surface_index);
            view_->surface_index = 0;
        });
    });
    installation->setWidget(left);
    addDockWidget(Qt::LeftDockWidgetArea, installation);
    installation->setMinimumWidth(225);
    connect(tree_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item, QTreeWidgetItem*) {
        if (refreshing_ || !item)
            return;
        view_->output_index = item->data(0, Qt::UserRole).toInt();
        view_->surface_index = item->data(0, Qt::UserRole + 1).toInt();
        view_->clear_selection();
        build_properties();
        preview_dirty_ = true;
        view_->update();
    });
    auto* properties = new QDockWidget("CONTROLS", this);
    properties->setObjectName("controls");
    tabs_ = new QTabWidget;
    properties->setWidget(tabs_);
    addDockWidget(Qt::RightDockWidgetArea, properties);
    properties->setMinimumWidth(330);
    connect(tabs_, &QTabWidget::currentChanged, this, [this](int index) {
        if (refreshing_)
            return;
        view_->mode = index == 0   ? MeshView::Mode::Source
                      : index == 2 ? MeshView::Mode::Mask
                                   : MeshView::Mode::Geometry;
        view_->clear_selection();
        preview_dirty_ = true;
    });
    runtime_ = new QLabel("Output stopped  |  Ctrl+Shift+Esc: emergency stop");
    statusBar()->addPermanentWidget(runtime_, 1);
    connect(&undo_, &QUndoStack::cleanChanged, this, [this](bool) {
        setWindowTitle(qs(project_.name) + (undo_.isClean() ? "" : " *") + " - Projection Studio");
    });
    connect(&pipe_, &QLocalSocket::readyRead, this, [this] {
        received_ += pipe_.readAll();
        qsizetype at;
        while ((at = received_.indexOf('\n')) >= 0) {
            auto line = received_.left(at);
            received_.remove(0, at + 1);
            try {
                auto j = ps::Json::parse(line.toStdString());
                if (j.value("type", "") == "ack" &&
                    (j.value("command", "") == "patch" || j.value("command", "") == "project") &&
                    j.value("revision", std::uint64_t{}) == sending_revision_) {
                    worker_revision_ = sending_revision_;
                    sending_revision_ = 0;
                    worker_document_ = std::move(sending_document_);
                    resync_ = false;
                }
                if (j.value("resync_required", false)) {
                    worker_revision_ = j.value("revision", std::uint64_t{});
                    sending_revision_ = 0;
                    resync_ = true;
                    pending_update_ = true;
                }
                if (j.value("type", "") == "stats") {
                    last_telemetry_ = j;
                    runtime_->setText(QString("%1 outputs  |  %2 render cycles/s  |  %3 ms submit  |  %4")
                                          .arg(j.value("outputs", 0))
                                          .arg(j.value("fps", 0.), 0, 'f', 1)
                                          .arg(j.value("render_ms", 0.), 0, 'f', 2)
                                          .arg(blackout_ ? "BLACKOUT" : "LIVE"));
                }
                if (j.contains("source_errors") && !j["source_errors"].empty())
                    runtime_->setText("Capture: " +
                                      qs(j["source_errors"].begin().value().get<std::string>()));
                else if (j.value("type", "") == "error")
                    runtime_->setText(qs(j.value("message", "Renderer error")));
            } catch (const std::exception&) {
            };
        }
    });
    connect(&worker_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus) {
                connect_timer_.stop();
                stop_timer_.stop();
                stopping_ = false;
                pipe_.abort();
                runtime_->setText("Output stopped");
                if (queued_output_ >= 0) {
                    bool fullscreen = queued_output_ != 0;
                    queued_output_ = -1;
                    QTimer::singleShot(0, this, [this, fullscreen] { start_output(fullscreen); });
                }
                if (code) {
                    QFile file(temporary_.filePath("status.json"));
                    if (file.open(QIODevice::ReadOnly)) {
                        try {
                            auto j = ps::Json::parse(file.readAll().toStdString());
                            runtime_->setText(qs(j.value("message", "Renderer stopped with an error")));
                        } catch (...) {
                        };
                    }
                }
            });
    stop_timer_.setSingleShot(true);
    connect(&stop_timer_, &QTimer::timeout, this, [this] {
        if (worker_.state() != QProcess::NotRunning)
            worker_.kill();
    });
    attach_timer_.setSingleShot(true);
    connect(&attach_timer_, &QTimer::timeout, this, [this] {
        attach_.kill();
        stop_output();
        runtime_->setText("Application attachment timed out");
    });
    connect(&attach_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus status) {
                attach_timer_.stop();
                if (code || status != QProcess::NormalExit) {
                    auto message = attach_.readAllStandardError();
                    stop_output();
                    runtime_->setText("Attachment failed: " + QString::fromUtf8(message));
                } else if (!adapter_manifest_.isEmpty())
                    runtime_->setText("Application adapter loaded | Stop disables warping");
            });
    connect(&worker_, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError) { runtime_->setText("Output: " + worker_.errorString()); });
    connect(&attach_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        attach_timer_.stop();
        runtime_->setText("Attachment: " + attach_.errorString());
    });
    connect_timer_.setInterval(50);
    connect(&connect_timer_, &QTimer::timeout, this, [this] {
        if (pipe_.state() == QLocalSocket::ConnectedState) {
            connect_timer_.stop();
            send({{"type", "blackout"}, {"enabled", blackout_}});
            resync_ = true;
            pending_update_ = true;
        } else if (pipe_.state() == QLocalSocket::UnconnectedState)
            pipe_.connectToServer(pipe_name_);
    });
    auto* adapter_status = new QTimer(this);
    adapter_status->setInterval(1000);
    connect(adapter_status, &QTimer::timeout, this, [this] {
        if (adapter_manifest_.isEmpty() || attach_.state() != QProcess::NotRunning)
            return;
        QFile file(adapter_manifest_ + ".status.json");
        if (!file.open(QIODevice::ReadOnly))
            return;
        auto status = ps::Json::parse(file.readAll().toStdString(), nullptr, false);
        if (status.is_discarded())
            return;
        last_telemetry_["application_adapter"] = status;
        runtime_->setText(qs(status.value("message", "Application adapter active")));
    });
    adapter_status->start();
    live_.setInterval(33);
    connect(&live_, &QTimer::timeout, this, [this] {
        if (sending_revision_ && update_clock_.elapsed() > 2000) {
            sending_revision_ = 0;
            resync_ = true;
            pending_update_ = true;
        }
        if (pending_update_ && !sending_revision_ && pipe_.state() == QLocalSocket::ConnectedState) {
            sending_document_ = ps::project_json(absolute_assets(project_, path_));
            sending_revision_ = worker_revision_ + 1;
            auto patch = resync_ ? ps::Json::array() : ps::Json::diff(worker_document_, sending_document_);
            if (resync_ || patch.dump().size() >= sending_document_.dump().size())
                send({{"type", "project"}, {"revision", sending_revision_}, {"project", sending_document_}});
            else
                send({{"type", "patch"},
                      {"revision", sending_revision_},
                      {"base_revision", worker_revision_},
                      {"patch", patch}});
            update_clock_.restart();
            update_adapter();
            pending_update_ = false;
        }
        if (pending_update_ && !adapter_manifest_.isEmpty()) {
            update_adapter();
            pending_update_ = false;
        }
        if (preview_dirty_)
            view_->update();
    });
    live_.start();
    remote_.status = [this] {
        return ps::Json{{"version", "0.4.0"},
                        {"project", project_.name},
                        {"outputs", project_.outputs.size()},
                        {"running", worker_.state() != QProcess::NotRunning},
                        {"blackout", blackout_},
                        {"locked", project_.locked},
                        {"websocket_port", remote_.websocket_port()},
                        {"telemetry", last_telemetry_}};
    };
    remote_.project = [this] { return ps::project_json(project_); };
    remote_.command = [this](const ps::Json& j) -> ps::Json {
        std::string type = j.at("type");
        if (type == "blackout")
            set_blackout(j.at("enabled").get<bool>());
        else if (type == "stop")
            stop_output();
        else if (type == "project") {
            if (project_.locked)
                throw std::runtime_error("Configuration locked");
            auto candidate = ps::project_from_json(j.at("project"));
            mutate("Remote project edit", [&](auto& p) { p = candidate; });
        } else if (type == "preset") {
            if (project_.locked)
                throw std::runtime_error("Configuration locked");
            std::string id = j.at("id");
            auto candidate = project_;
            if (!ps::activate_preset(candidate, id))
                throw std::runtime_error("Unknown or invalid preset");
            mutate("Remote preset", [&](auto& p) {
                if (!ps::activate_preset(p, id))
                    throw std::runtime_error("Unknown preset");
            });
        } else if (type == "output") {
            if (project_.locked)
                throw std::runtime_error("Configuration locked");
            std::string id = j.at("id");
            bool enabled = j.at("enabled");
            auto candidate = project_;
            bool exists = false;
            for (auto& o : candidate.outputs)
                if (o.id == id) {
                    o.enabled = enabled;
                    exists = true;
                }
            if (!exists)
                throw std::runtime_error("Unknown output");
            auto validation = ps::validate(candidate);
            if (!validation.empty())
                throw std::runtime_error(validation.front());
            mutate("Remote output", [&](auto& p) {
                bool found = false;
                for (auto& o : p.outputs)
                    if (o.id == id) {
                        o.enabled = enabled;
                        found = true;
                    }
                if (!found)
                    throw std::runtime_error("Unknown output");
            });
        } else
            throw std::runtime_error("Unknown command");
        return {{"ok", true}};
    };
    QSettings settings;
    for (auto* action : findChildren<QAction*>()) {
        if (action->shortcut().isEmpty() && action->objectName().isEmpty())
            continue;
        if (action->objectName().isEmpty())
            action->setObjectName(action->text().remove('&'));
        action->setProperty("configurableShortcut", true);
        auto key = "shortcuts/" + action->objectName();
        if (settings.contains(key))
            action->setShortcut(QKeySequence(settings.value(key).toString()));
    }
    if (!qApp->arguments().contains("--smoke")) {
        restoreGeometry(settings.value("windowGeometry").toByteArray());
        restoreState(settings.value("dockLayout").toByteArray());
        if (!settings.value("setupCompleted", false).toBool() && !qApp->arguments().contains("--project"))
            QTimer::singleShot(0, this, [this] { setup_installation(); });
    }
    refresh();
}
MainWindow::~MainWindow() {
    stop_output();
}
ps::Output* MainWindow::output() {
    return view_->output_index >= 0 && view_->output_index < int(project_.outputs.size())
               ? &project_.outputs[size_t(view_->output_index)]
               : nullptr;
}
ps::Surface* MainWindow::surface() {
    auto* o = output();
    return o && view_->surface_index >= 0 && view_->surface_index < int(o->surfaces.size())
               ? &o->surfaces[size_t(view_->surface_index)]
               : nullptr;
}
ps::Source* MainWindow::source() {
    auto* f = surface();
    if (f)
        for (auto& s : project_.sources)
            if (s.id == f->source_id)
                return &s;
    return nullptr;
}
void MainWindow::error(const QString& text) {
    QMessageBox::warning(this, "Projection Studio", text);
}
void MainWindow::mutate(const QString& name, const std::function<void(ps::Project&)>& f) {
    if (project_.locked) {
        statusBar()->showMessage("Unlock the configuration to edit", 3000);
        return;
    }
    auto before = project_;
    try {
        f(project_);
        auto errors = ps::validate(project_);
        if (!errors.empty())
            throw std::runtime_error(errors.front());
        auto after = project_;
        if (ps::project_json(after) == ps::project_json(before))
            return;
        project_ = before;
        undo_.push(new Edit(&project_, std::move(before), std::move(after), name, [this] { refresh(); }));
    } catch (const std::exception& e) {
        project_ = std::move(before);
        error(qs(e.what()));
        refresh();
    }
}
void MainWindow::commit_edit() {
    auto errors = ps::validate(project_);
    if (!errors.empty()) {
        project_ = before_;
        refresh();
        return;
    }
    auto after = project_;
    if (ps::project_json(after) == ps::project_json(before_))
        return;
    project_ = before_;
    undo_.push(new Edit(&project_, before_, std::move(after), "Edit mapping", [this] { refresh(); }));
}
void MainWindow::refresh() {
    if (auto* lock = findChild<QAction*>("configurationLock")) {
        QSignalBlocker blocker(lock);
        lock->setChecked(project_.locked);
    }
    findChild<QAction*>("undoMapping")->setEnabled(!project_.locked && undo_.canUndo());
    findChild<QAction*>("redoMapping")->setEnabled(!project_.locked && undo_.canRedo());
    refreshing_ = true;
    view_->output_index = std::clamp(view_->output_index, 0, int(project_.outputs.size()) - 1);
    auto* o = output();
    if (o)
        view_->surface_index = std::clamp(view_->surface_index, 0, std::max(0, int(o->surfaces.size()) - 1));
    ps::Json tree_state = ps::Json::array();
    size_t total = 0;
    for (const auto& target : project_.outputs) {
        ps::Json entry = {target.id, target.name};
        for (const auto& surface : target.surfaces) {
            entry.push_back({surface.id, surface.name});
            ++total;
        }
        tree_state.push_back(entry);
    }
    auto signature = qs(tree_state.dump());
    if (signature != tree_signature_) {
        int scroll = tree_->verticalScrollBar()->value();
        std::set<QString> collapsed;
        for (int i = 0; i < tree_->topLevelItemCount(); ++i)
            if (!tree_->topLevelItem(i)->isExpanded())
                collapsed.insert(tree_->topLevelItem(i)->text(0));
        tree_->clear();
        QTreeWidgetItem* selected = nullptr;
        for (size_t i = 0; i < project_.outputs.size(); ++i) {
            auto& output = project_.outputs[i];
            auto* root = new QTreeWidgetItem(tree_, {qs(output.name)});
            root->setData(0, Qt::UserRole, int(i));
            root->setData(0, Qt::UserRole + 1, 0);
            root->setForeground(0, QColor("#e2ecef"));
            if (int(i) == view_->output_index)
                selected = root;
            for (size_t k = 0; k < output.surfaces.size(); ++k) {
                auto* child = new QTreeWidgetItem(root, {qs(output.surfaces[k].name)});
                child->setData(0, Qt::UserRole, int(i));
                child->setData(0, Qt::UserRole + 1, int(k));
                if (int(i) == view_->output_index && int(k) == view_->surface_index)
                    selected = child;
            }
            root->setExpanded(true);
        }
        if (selected)
            tree_->setCurrentItem(selected);

        for (int i = 0; i < tree_->topLevelItemCount(); ++i)
            if (collapsed.contains(tree_->topLevelItem(i)->text(0)))
                tree_->topLevelItem(i)->setExpanded(false);
        tree_->verticalScrollBar()->setValue(scroll);
        tree_signature_ = signature;
        filter_installation(search_->text());
    }
    info_->setText(qs(project_.name) +
                   QString("\n%1 projectors / %2 surfaces").arg(project_.outputs.size()).arg(total));
    refreshing_ = false;
    build_properties();
    update_selection();
    preview_dirty_ = true;
    pending_update_ = true;
    view_->update();
    setWindowTitle(qs(project_.name) + (undo_.isClean() ? "" : " *") + " - Projection Studio");
}
void MainWindow::load(const QString& path) {
    if (!undo_.isClean() &&
        QMessageBox::question(this, "Open project", "Discard unsaved changes?") != QMessageBox::Yes)
        return;
    try {
        auto candidate = ps::load_project(fs(path));
        stop_output();
        project_ = std::move(candidate);
        if (!project_.startup_preset.empty())
            ps::activate_preset(project_, project_.startup_preset);
        path_ = path;
        undo_.clear();
        view_->output_index = view_->surface_index = 0;
        refresh();
    } catch (const std::exception& e) {
        error(qs(e.what()));
    }
}
void MainWindow::save(const QString& requested) {
    QString path = requested.isEmpty() ? path_ : requested;
    if (path.isEmpty())
        path = QFileDialog::getSaveFileName(this, "Save project", QDir::currentPath() + "/installation.json",
                                            "Projection projects (*.json)");
    if (path.isEmpty())
        return;
    try {
        auto save = project_;
        if (!path_.isEmpty() && QFileInfo(path_).absolutePath() != QFileInfo(path).absolutePath())
            save = absolute_assets(project_, path_);
        ps::save_project(save, fs(path));
        project_ = std::move(save);
        path_ = path;
        undo_.setClean();
        statusBar()->showMessage("Project saved", 2500);
    } catch (const std::exception& e) {
        error(qs(e.what()));
    }
}
void MainWindow::build_properties() {
    int active = tabs_->currentIndex();
    refreshing_ = true;
    auto tab = [&](const QString& title) {
        auto* scroll = tabs_->findChild<QScrollArea*>("panel/" + title);
        if (!scroll) {
            scroll = new QScrollArea;
            scroll->setObjectName("panel/" + title);
            scroll->setWidgetResizable(true);
            auto* page = new QWidget;
            auto* form = new QFormLayout(page);
            form->setContentsMargins(14, 18, 14, 18);
            form->setSpacing(12);
            form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
            scroll->setWidget(page);
            tabs_->addTab(scroll, title);
        }
        return static_cast<QFormLayout*>(scroll->widget()->layout());
    };
    auto number = [&](QFormLayout* f, const QString& label, double value, double min, double max, double step,
                      const std::function<void(double)>& change) {
        auto* box = control<QDoubleSpinBox>(f, label);
        box->setRange(min, max);
        box->setDecimals(3);
        box->setSingleStep(step);
        box->setValue(value);
        connect(box, &QDoubleSpinBox::editingFinished, this, [this, box, change, value = box->value()] {
            if (!refreshing_ && box->value() != value)
                change(box->value());
        });
        return box;
    };
    auto button = [&](QFormLayout* f, const QString& name, const std::function<void()>& action) {
        auto* b = control<QPushButton>(f, name);
        connect(b, &QPushButton::clicked, this, action);
    };
    auto check = [&](QFormLayout* f, const QString& name, bool value,
                     const std::function<void(bool)>& action) {
        auto* c = control<QCheckBox>(f, name);
        c->setChecked(value);
        connect(c, &QCheckBox::toggled, this, [this, action](bool v) {
            if (!refreshing_)
                action(v);
        });
    };
    auto* sourceForm = tab("Source");
    auto* geometry = tab("Warp");
    auto* masks = tab("Mask");
    auto* blending = tab("Blend");
    auto* color = tab("Color");
    auto* setup = tab("Setup");
    auto* o = output();
    auto* s = surface();
    auto* src = source();
    for (auto* form : {sourceForm, geometry, masks})
        form->parentWidget()->setEnabled(o && s && src && !project_.locked);
    for (auto* form : {blending, color, setup})
        form->parentWidget()->setEnabled(o && !project_.locked);
    if (o && s && src) {
        auto* picker = control<QComboBox>(sourceForm, "Content");
        picker->clear();
        for (auto& v : project_.sources)
            picker->addItem(qs(v.name), qs(v.id));
        picker->setCurrentIndex(picker->findData(qs(s->source_id)));
        connect(picker, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, picker](int) {
            mutate("Route source",
                   [&](auto&) { surface()->source_id = picker->currentData().toString().toStdString(); });
        });
        auto* patterns = control<QComboBox>(sourceForm, "Test pattern");
        patterns->clear();
        patterns->addItems(
            {"grid", "white", "black", "red", "green", "blue", "gradient", "checker", "motion"});
        patterns->setCurrentText(qs(src->pattern));
        connect(patterns, &QComboBox::currentTextChanged, this, [this](const QString& text) {
            mutate("Set test pattern", [&](auto&) {
                source()->type = "pattern";
                source()->pattern = text.toStdString();
            });
        });
        button(sourceForm, "Add image source...", [this] {
            auto path =
                QFileDialog::getOpenFileName(this, "Source image", {}, "Images (*.png *.bmp *.jpg *.jpeg)");
            if (path.isEmpty())
                return;
            mutate("Add image source", [&](auto& p) {
                ps::Source src;
                src.id = ps::make_id();
                src.name = QFileInfo(path).fileName().toStdString();
                src.type = "image";
                src.image = path.toStdString();
                p.sources.push_back(src);
                surface()->source_id = src.id;
            });
        });
#ifdef _WIN32
        button(sourceForm, "Add window capture...", [this] {
            try {
                auto windows = ps::win::windows();
                QStringList names;
                for (auto& w : windows)
                    names << QString("%1  [PID %2]").arg(qs(w.title)).arg(w.pid);
                if (names.isEmpty())
                    throw std::runtime_error("No capturable application windows");
                bool ok;
                auto choice = QInputDialog::getItem(this, "Capture application window",
                                                    "Select the window to capture", names, 0, false, &ok);
                if (!ok)
                    return;
                auto selected = windows[size_t(names.indexOf(choice))];
                mutate("Add window source", [&](auto& p) {
                    ps::Source src;
                    src.id = ps::make_id();
                    src.name = selected.title;
                    src.type = "window";
                    src.window_handle = reinterpret_cast<std::uint64_t>(selected.hwnd);
                    src.window_title = selected.title;
                    src.executable = selected.executable;
                    src.window_class = selected.window_class;
                    p.sources.push_back(src);
                    surface()->source_id = src.id;
                });
            } catch (const std::exception& e) {
                error(qs(e.what()));
            }
        });
        button(sourceForm, "Add desktop capture...", [this] {
            try {
                auto displays = ps::win::displays();
                QStringList names;
                for (auto& d : displays)
                    names << qs(d.name);
                bool ok;
                auto choice = QInputDialog::getItem(this, "Capture display",
                                                    "Choose a source display distinct from projector outputs",
                                                    names, 0, false, &ok);
                if (!ok)
                    return;
                auto selected = displays[size_t(names.indexOf(choice))];
                mutate("Add desktop source", [&](auto& p) {
                    ps::Source src;
                    src.id = ps::make_id();
                    src.name = "Desktop " + selected.id;
                    src.type = "desktop";
                    src.display = selected.id;
                    src.width = selected.bounds.right - selected.bounds.left;
                    src.height = selected.bounds.bottom - selected.bounds.top;
                    p.sources.push_back(src);
                    surface()->source_id = src.id;
                });
            } catch (const std::exception& e) {
                error(qs(e.what()));
            }
        });
#endif

        number(sourceForm, "Left", s->source_rect.x, 0, .999, .01, [this](double v) {
            mutate("Crop left", [&](auto&) {
                auto& r = surface()->source_rect;
                r.w = std::min(r.w, 1 - v);
                r.x = v;
            });
        });
        number(sourceForm, "Top", s->source_rect.y, 0, .999, .01, [this](double v) {
            mutate("Crop top", [&](auto&) {
                auto& r = surface()->source_rect;
                r.h = std::min(r.h, 1 - v);
                r.y = v;
            });
        });
        number(sourceForm, "Width", s->source_rect.w, .001, 1, .01, [this](double v) {
            mutate("Crop width",
                   [&](auto&) { surface()->source_rect.w = std::min(v, 1 - surface()->source_rect.x); });
        });
        number(sourceForm, "Height", s->source_rect.h, .001, 1, .01, [this](double v) {
            mutate("Crop height",
                   [&](auto&) { surface()->source_rect.h = std::min(v, 1 - surface()->source_rect.y); });
        });
        check(geometry, "Surface enabled", s->enabled,
              [this](bool v) { mutate("Enable surface", [&](auto&) { surface()->enabled = v; }); });
        auto* cols = control<QSpinBox>(geometry, "Columns");
        cols->setRange(2, 64);
        cols->setValue(s->mesh.columns);
        connect(cols, &QSpinBox::editingFinished, this, [this, cols] {
            if (cols->value() != surface()->mesh.columns)
                mutate("Resize grid", [&](auto&) {
                    surface()->mesh = ps::resample_mesh(surface()->mesh, cols->value(), surface()->mesh.rows);
                });
        });
        auto* rows = control<QSpinBox>(geometry, "Rows");
        rows->setRange(2, 64);
        rows->setValue(s->mesh.rows);
        connect(rows, &QSpinBox::editingFinished, this, [this, rows] {
            if (rows->value() != surface()->mesh.rows)
                mutate("Resize grid", [&](auto&) {
                    surface()->mesh =
                        ps::resample_mesh(surface()->mesh, surface()->mesh.columns, rows->value());
                });
        });
        check(geometry, "Smooth curved mesh", s->mesh.curved,
              [this](bool v) { mutate("Interpolation", [&](auto&) { surface()->mesh.curved = v; }); });
        auto* select = control<QComboBox>(geometry, "Move");
        select->clear();
        select->addItems({"Point", "Row", "Column", "All", "Boundary"});
        select->setCurrentIndex(int(view_->selection));
        connect(select, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int i) {
            view_->selection = ps::SelectionMode(i);
            view_->clear_selection();
        });
        auto* axis = control<QComboBox>(geometry, "Axis");
        axis->clear();
        axis->addItems({"Free", "X only", "Y only"});
        axis->setCurrentIndex(int(view_->axis));
        connect(axis, qOverload<int>(&QComboBox::currentIndexChanged), this,
                [this](int i) { view_->axis = ps::AxisLock(i); });
        check(geometry, "Interpolate interior from edges", view_->boundary_interpolation,
              [this](bool v) { view_->boundary_interpolation = v; });
        number(geometry, "Opacity", s->opacity, 0, 1, .05,
               [this](double v) { mutate("Opacity", [&](auto&) { surface()->opacity = v; }); });
        button(geometry, "Reset geometry", [this] {
            mutate("Reset mesh", [&](auto&) {
                surface()->mesh = ps::identity_mesh(surface()->mesh.columns, surface()->mesh.rows);
            });
        });
        button(geometry, "Duplicate surface", [this] {
            mutate("Duplicate surface", [&](auto&) {
                auto clone = *surface();
                clone.id = ps::make_id();
                clone.name += " copy";
                output()->surfaces.push_back(clone);
            });
        });
        check(masks, "Mask enabled", s->mask.enabled,
              [this](bool v) { mutate("Enable mask", [&](auto&) { surface()->mask.enabled = v; }); });
        check(masks, "Invert mask", s->mask.inverted,
              [this](bool v) { mutate("Invert mask", [&](auto&) { surface()->mask.inverted = v; }); });
        number(masks, "Feather", s->mask.feather, 0, .5, .005,
               [this](double v) { mutate("Mask feather", [&](auto&) { surface()->mask.feather = v; }); });
        button(masks, "Create rectangle mask", [this] {
            mutate("Rectangle mask", [&](auto&) {
                surface()->mask.enabled = true;
                surface()->mask.polygon = {{.1, .1}, {.9, .1}, {.9, .9}, {.1, .9}};
            });
        });
        button(masks, "Load bitmap mask...", [this] {
            auto path = QFileDialog::getOpenFileName(this, "Mask image", {}, "Images (*.png *.bmp)");
            if (!path.isEmpty())
                mutate("Bitmap mask", [&](auto&) {
                    surface()->mask.bitmap = path.toStdString();
                    surface()->mask.enabled = true;
                });
        });
        button(masks, "Clear mask", [this] { mutate("Clear mask", [&](auto&) { surface()->mask = {}; }); });
    }
    if (o) {
        number(blending, "Left overlap", o->blend.left, 0, 1, .01,
               [this](double v) { mutate("Left blend", [&](auto&) { output()->blend.left = v; }); });
        number(blending, "Right overlap", o->blend.right, 0, 1, .01,
               [this](double v) { mutate("Right blend", [&](auto&) { output()->blend.right = v; }); });
        number(blending, "Top overlap", o->blend.top, 0, 1, .01,
               [this](double v) { mutate("Top blend", [&](auto&) { output()->blend.top = v; }); });
        number(blending, "Bottom overlap", o->blend.bottom, 0, 1, .01,
               [this](double v) { mutate("Bottom blend", [&](auto&) { output()->blend.bottom = v; }); });
        number(blending, "Blend gamma", o->blend.gamma, .1, 8, .05,
               [this](double v) { mutate("Blend gamma", [&](auto&) { output()->blend.gamma = v; }); });
        number(blending, "Black compensation", o->blend.black_level, 0, 1, .001,
               [this](double v) { mutate("Black level", [&](auto&) { output()->blend.black_level = v; }); });
        button(blending, "Edit blend curve...", [this] {
            QString text;
            for (auto p : output()->blend.curve)
                text += QString::number(p.x) + "," + QString::number(p.y) + "\n";
            bool ok;
            auto answer = QInputDialog::getMultiLineText(
                this, "Blend curve",
                "One position,intensity pair per line. Positions must increase from 0 to 1.", text, &ok);
            if (!ok)
                return;
            mutate("Blend curve", [&](auto&) {
                std::vector<ps::Vec2> points;
                for (auto line : answer.split('\n', Qt::SkipEmptyParts)) {
                    auto parts = line.split(',');
                    if (parts.size() != 2)
                        throw std::runtime_error("Expected x,y pairs");
                    bool x, y;
                    double a = parts[0].toDouble(&x), b = parts[1].toDouble(&y);
                    if (!x || !y)
                        throw std::runtime_error("Invalid number");
                    points.push_back({a, b});
                }
                output()->blend.curve = points;
            });
        });
        for (int i = 0; i < 3; ++i)
            number(color, QStringList{"Red gain", "Green gain", "Blue gain"}[i], o->color.gain[size_t(i)], 0,
                   4, .01, [this, i](double v) {
                       mutate("Channel gain", [&](auto&) { output()->color.gain[size_t(i)] = v; });
                   });
        number(color, "Gamma", o->color.gamma, .1, 8, .05,
               [this](double v) { mutate("Gamma", [&](auto&) { output()->color.gamma = v; }); });
        number(color, "Brightness", o->color.brightness, -1, 1, .01,
               [this](double v) { mutate("Brightness", [&](auto&) { output()->color.brightness = v; }); });
        number(color, "Contrast", o->color.contrast, 0, 4, .05,
               [this](double v) { mutate("Contrast", [&](auto&) { output()->color.contrast = v; }); });
        number(color, "Saturation", o->color.saturation, 0, 4, .05,
               [this](double v) { mutate("Saturation", [&](auto&) { output()->color.saturation = v; }); });
        button(color, "Reset color", [this] { mutate("Reset color", [&](auto&) { output()->color = {}; }); });
        check(setup, "Projector enabled", o->enabled,
              [this](bool v) { mutate("Enable output", [&](auto&) { output()->enabled = v; }); });
        button(setup, "Rename projector...", [this] {
            bool ok;
            auto name = QInputDialog::getText(this, "Projector name", "Name", QLineEdit::Normal,
                                              qs(output()->name), &ok);
            if (ok)
                mutate("Rename projector", [&](auto&) { output()->name = name.toStdString(); });
        });
#ifdef _WIN32
        auto* display = control<QComboBox>(setup, "Display");
        display->clear();
        display->addItem("Simulation / no assigned display", QString());
        try {
            for (auto& d : ps::win::displays())
                display->addItem(qs(d.id), qs(d.id));
        } catch (...) {
        }
        display->setCurrentIndex(std::max(0, display->findData(qs(o->display))));
        connect(display, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, display](int) {
            mutate("Assign display",
                   [&](auto&) { output()->display = display->currentData().toString().toStdString(); });
        });
#endif
        number(setup, "Width / pixels", o->width, 64, 16384, 1,
               [this](double v) { mutate("Output resolution", [&](auto&) { output()->width = int(v); }); });
        number(setup, "Height / pixels", o->height, 64, 16384, 1,
               [this](double v) { mutate("Output resolution", [&](auto&) { output()->height = int(v); }); });
        number(setup, "Display region X", o->viewport.x, 0, 1, .01,
               [this](double v) { mutate("Output region", [&](auto&) { output()->viewport.x = v; }); });
        number(setup, "Display region Y", o->viewport.y, 0, 1, .01,
               [this](double v) { mutate("Output region", [&](auto&) { output()->viewport.y = v; }); });
        number(setup, "Region width", o->viewport.w, .001, 1, .01,
               [this](double v) { mutate("Output region", [&](auto&) { output()->viewport.w = v; }); });
        number(setup, "Region height", o->viewport.h, .001, 1, .01,
               [this](double v) { mutate("Output region", [&](auto&) { output()->viewport.h = v; }); });
        button(setup, "Save preset...", [this] {
            bool ok;
            auto name = QInputDialog::getText(this, "Save preset", "Preset name", QLineEdit::Normal, {}, &ok);
            if (ok && !name.isEmpty())
                mutate("Save preset",
                       [&](auto& p) { p.presets.push_back({ps::make_id(), name.toStdString(), p.outputs}); });
        });
        auto* presets = control<QComboBox>(setup, "Preset");
        presets->clear();
        presets->addItem("Choose a saved preset", QString());
        for (auto& p : project_.presets)
            presets->addItem(qs(p.name), qs(p.id));
        connect(presets, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, presets](int i) {
            if (i > 0)
                mutate("Activate preset", [&](auto& p) {
                    ps::activate_preset(p, presets->currentData().toString().toStdString());
                });
        });
        number(setup, "Screen width / m", project_.screen.width, .01, 1000, .1,
               [this](double v) { mutate("Screen width", [&](auto& p) { p.screen.width = v; }); });
        number(setup, "Screen height / m", project_.screen.height, .01, 1000, .1,
               [this](double v) { mutate("Screen height", [&](auto& p) { p.screen.height = v; }); });
        number(setup, "Eye distance / m", project_.screen.eye.z, .01, 1000, .1,
               [this](double v) { mutate("Eye distance", [&](auto& p) { p.screen.eye.z = v; }); });
        auto* shape = control<QComboBox>(setup, "Screen shape");
        shape->clear();
        shape->addItems({"plane", "cylinder"});
        shape->setCurrentText(qs(project_.screen.shape));
        connect(shape, &QComboBox::currentTextChanged, this, [this](const QString& text) {
            mutate("Screen shape", [&](auto& p) { p.screen.shape = text.toStdString(); });
        });
        number(setup, "Cylinder radius / m", project_.screen.radius, .01, 1000, .1,
               [this](double v) { mutate("Screen radius", [&](auto& p) { p.screen.radius = v; }); });
        number(setup, "Cylinder arc / degrees", project_.screen.arc_degrees, 1, 360, 5,
               [this](double v) { mutate("Screen arc", [&](auto& p) { p.screen.arc_degrees = v; }); });
        button(setup, "Export camera frustums...", [this] { export_views(); });
    }
    tabs_->setCurrentIndex(active < 0 ? 1 : std::min(active, tabs_->count() - 1));
    refreshing_ = false;
    view_->mode = tabs_->currentIndex() == 0   ? MeshView::Mode::Source
                  : tabs_->currentIndex() == 2 ? MeshView::Mode::Mask
                                               : MeshView::Mode::Geometry;
}
QImage MainWindow::preview(bool source_mode) {
    auto* o = output();
    if (!o)
        return {};
    QString key =
        qs(o->id) + (source_mode ? "/source/" + (surface() ? qs(surface()->id) : QString()) : "/output");
    if (preview_key_ != key) {
        cached_preview_ = {};
        preview_dirty_ = true;
        preview_key_ = key;
    }
    if (preview_dirty_ || preview_source_ != source_mode) {
        preview_dirty_ = false;
        preview_source_ = source_mode;
        auto output_copy = *o;
        if (source_mode && surface()) {
            auto f = *surface();
            f.mesh = ps::identity_mesh(2, 2);
            f.source_rect = {};
            f.mask = {};
            f.opacity = 1;
            f.enabled = true;
            output_copy.surfaces = {f};
            output_copy.blend = {};
            output_copy.color = {};
            output_copy.enabled = true;
        }
        if (!source_mode && view_->compare < 1 && surface()) {
            auto comparison = *o;
            auto f = *surface();
            f.mesh = ps::identity_mesh(2, 2);
            f.source_rect = {};
            f.mask = {};
            f.opacity = 1;
            f.enabled = true;
            comparison.surfaces = {f};
            comparison.blend = {};
            comparison.color = {};
            comparison.enabled = true;
            comparison_.submit({project_, std::move(comparison),
                                path_.isEmpty() ? std::filesystem::current_path() : fs(path_).parent_path(),
                                key + "/compare", 960, 540});
        } else
            comparison_.pause();
        unsigned w = 960, h = unsigned(std::clamp(960. * o->height / o->width, 64., 960.));
        previews_.submit({project_, std::move(output_copy),
                          path_.isEmpty() ? std::filesystem::current_path() : fs(path_).parent_path(), key, w,
                          h});
    }
    return cached_preview_;
}
void MainWindow::send(const ps::Json& j) {
    if (pipe_.state() == QLocalSocket::ConnectedState) {
        pipe_.write(QByteArray::fromStdString(j.dump() + "\n"));
        pipe_.flush();
    }
}
void MainWindow::update_adapter() {
    if (adapter_manifest_.isEmpty())
        return;
    try {
        ps::Json j = {{"project", ps::project_json(absolute_assets(project_, path_))},
                      {"target_hwnd", adapter_hwnd_},
                      {"output_id", adapter_output_},
                      {"enabled", true},
                      {"blackout", blackout_}};
        ps::atomic_write(fs(adapter_manifest_), j.dump());
    } catch (const std::exception& e) {
        runtime_->setText(qs(e.what()));
    }
}
void MainWindow::attach_application() {
    if (auto* test = findChild<QProcess*>("setupCheckProcess");
        test && test->state() != QProcess::NotRunning) {
        error("Stop the setup test before attaching an application");
        return;
    }
    if (!output())
        return;
    try {
        auto windows = ps::win::windows();
        QStringList names;
        for (auto& w : windows)
            names << QString("%1  [PID %2] — %3").arg(qs(w.title)).arg(w.pid).arg(qs(w.executable));
        if (names.isEmpty())
            throw std::runtime_error("No running application windows");
        bool ok;
        auto choice = QInputDialog::getItem(
            this, "Attach application",
            "Select an application. Its profile chooses capture, DX11, or DX12 integration.", names, 0, false,
            &ok);
        if (!ok)
            return;
        auto selected = windows[size_t(names.indexOf(choice))];
        const ps::ApplicationProfile* matching = nullptr;
        for (auto& profile : project_.applications) {
            auto exe = qs(profile.executable);
            bool match = exe.compare(qs(selected.executable), Qt::CaseInsensitive) == 0 ||
                         exe.compare(QFileInfo(qs(selected.executable)).fileName(), Qt::CaseInsensitive) == 0;
            if (match && profile.excluded)
                throw std::runtime_error("Application is excluded by its profile");
            if (match && profile.enabled)
                matching = &profile;
        }
        QString api;
        if (matching)
            api = qs(matching->api);
        else
            api = QInputDialog::getItem(this, "Application integration",
                                        "Choose the application's graphics API", {"capture", "dx11", "dx12"},
                                        0, false, &ok);
        if (!matching && !ok)
            return;
        std::string target = matching ? matching->output_id : output()->id;
        if (api == "capture") {
            mutate("Capture application profile", [&](auto& project) {
                ps::Source source;
                source.id = ps::make_id();
                source.name = selected.title;
                source.type = "window";
                source.window_handle = reinterpret_cast<std::uint64_t>(selected.hwnd);
                source.window_title = selected.title;
                source.executable = selected.executable;
                source.window_class = selected.window_class;
                project.sources.push_back(source);
                for (auto& out : project.outputs)
                    if (out.id == target)
                        for (auto& surface : out.surfaces)
                            surface.source_id = source.id;
            });
            start_output(false);
            return;
        }
        stop_output();
        adapter_hwnd_ = reinterpret_cast<std::uint64_t>(selected.hwnd);
        adapter_output_ = target;
        adapter_manifest_ = temporary_.filePath("adapter.json");
        update_adapter();
        attach_.start(QApplication::applicationDirPath() + "/projection-attach.exe",
                      {QString::number(selected.pid),
                       QApplication::applicationDirPath() + "/projection-" + api + ".dll",
                       adapter_manifest_});
        attach_timer_.start(15000);
        runtime_->setText("Attaching application… Stop cancels");
    } catch (const std::exception& e) {
        stop_output();
        error(qs(e.what()));
    }
}
void MainWindow::start_output(bool fullscreen) {
    if (auto* test = findChild<QProcess*>("setupCheckProcess");
        test && test->state() != QProcess::NotRunning) {
        error("Stop the setup test before starting another output");
        return;
    }
    stop_output();
    if (worker_.state() != QProcess::NotRunning) {
        queued_output_ = fullscreen ? 1 : 0;
        return;
    }
    try {
        auto runtime = absolute_assets(project_, path_);
        auto path = temporary_.filePath("live.json");
        ps::save_project(runtime, fs(path));
        pipe_name_ = "projection-studio-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
        QStringList args{"--project", path,       "--pipe",
                         pipe_name_,  "--status", temporary_.filePath("status.json")};
        if (fullscreen)
            args << "--fullscreen";
        if (blackout_)
            args << "--blackout";
        worker_revision_ = sending_revision_ = 0;
        resync_ = true;
        worker_.start(QApplication::applicationDirPath() + "/projection-renderer.exe", args);
        runtime_->setText("Starting native outputs...");
        connect_timer_.start();
    } catch (const std::exception& e) {
        error(qs(e.what()));
    }
}
void MainWindow::stop_output() {
    if (auto* cancel = findChild<QPushButton*>("cancelCalibrationScan"))
        cancel->click();
    if (auto* cancel = findChild<QPushButton*>("cancelSetupCheck"))
        cancel->click();
    if (!adapter_manifest_.isEmpty()) {
        try {
            auto file = fs(adapter_manifest_);
            auto j = ps::Json::parse(std::ifstream(file));
            j["enabled"] = false;
            ps::atomic_write(file, j.dump());
        } catch (...) {
        }
        adapter_manifest_.clear();
    }
    queued_output_ = -1;
    attach_timer_.stop();
    if (attach_.state() != QProcess::NotRunning)
        attach_.kill();
    if (worker_.state() != QProcess::NotRunning && !stopping_) {
        stopping_ = true;
        send({{"type", "stop"}});
        stop_timer_.start(1500);
        runtime_->setText("Stopping output…");
    }
    connect_timer_.stop();
}
void MainWindow::set_blackout(bool enabled) {
    if (enabled)
        if (auto* cancel = findChild<QPushButton*>("cancelCalibrationScan"))
            cancel->click();
    if (enabled)
        if (auto* cancel = findChild<QPushButton*>("cancelSetupCheck"))
            cancel->click();
    if (auto* action = findChild<QAction*>("blackoutAction")) {
        QSignalBlocker blocker(action);
        action->setChecked(enabled);
    }
    blackout_ = enabled;
    send({{"type", "blackout"}, {"enabled", enabled}});
    update_adapter();
}
void MainWindow::export_views() {
    auto path = QFileDialog::getSaveFileName(this, "Export view frustums", {}, "JSON (*.json)");
    if (path.isEmpty())
        return;
    try {
        ps::atomic_write(fs(path), ps::export_views(project_).dump(2));
    } catch (const std::exception& e) {
        error(qs(e.what()));
    }
}
void MainWindow::import_calibration() {
    auto path =
        QFileDialog::getOpenFileName(this, "Import planar calibration", {}, "Calibration JSON (*.json)");
    if (path.isEmpty())
        return;
    try {
        std::ifstream in(fs(path));
        ps::Json j;
        in >> j;
        if (j.at("model") != "planar-homography")
            throw std::runtime_error("Expected planar-homography calibration");
        ps::Homography h;
        h.matrix = j.at("matrix").get<std::array<double, 9>>();
        auto mesh = ps::calibrated_mesh(h, j.at("projector_width"), j.at("projector_height"),
                                        {0, 0, double(j.at("camera_width").get<int>() - 1),
                                         double(j.at("camera_height").get<int>() - 1)});
        mutate("Apply planar calibration", [&](auto& p) {
            if (!surface())
                throw std::runtime_error("Select a surface first");
            surface()->mesh = mesh;
            p.calibration = j;
        });
    } catch (const std::exception& e) {
        error(qs(e.what()));
    }
}
void MainWindow::camera_patch(bool restore) {
    auto path = QFileDialog::getOpenFileName(this, "Preview MSFS camera configuration", {},
                                             "Camera configuration (cameras.cfg)");
    if (path.isEmpty())
        return;
    try {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            throw std::runtime_error("Cannot read cameras.cfg");
        auto original = file.readAll().toStdString();
        file.close();
        bool ok = false;
        QString version;
        std::string replacement;
        if (restore) {
            QFile backup(path + ".bak");
            if (!backup.open(QIODevice::ReadOnly))
                throw std::runtime_error("No readable camera backup exists");
            replacement = backup.readAll().toStdString();
        } else {
            version = QInputDialog::getItem(this, "Simulator version", "Target simulator", {"2020", "2024"},
                                            0, false, &ok);
            if (!ok)
                return;
            auto views = ps::screen_views(project_);
            double yaw = views.at(size_t(view_->output_index)).yaw;
            double zoom = QInputDialog::getDouble(
                this, "MSFS initial zoom",
                "Use a zoom calibrated in the simulator; this value is not a generic field of view.", .5, .01,
                128, 3, &ok);
            if (!ok)
                return;
            replacement = ps::msfs_camera_patch(original, yaw, 0, zoom, version.toStdString());
        }
        QDialog dialog(this);
        dialog.setWindowTitle(restore ? "Restore MSFS camera backup" : "MSFS " + version + " camera preview");
        dialog.resize(720, 620);
        auto* layout = new QVBoxLayout(&dialog);
        layout->addWidget(new QLabel(restore
                                         ? "Preview the backup below. Close MSFS before restoring. The "
                                           "current file becomes the new backup."
                                         : "A camera is appended; existing cameras are preserved. Close MSFS "
                                           "before applying. This does not enable multi-view rendering."));
        auto* text = new QTextEdit;
        text->setReadOnly(true);
        text->setPlainText(qs(replacement));
        layout->addWidget(text);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        if (dialog.exec() == QDialog::Accepted)
            ps::apply_msfs_camera(fs(path), original, replacement);
    } catch (const std::exception& e) {
        error(qs(e.what()));
    }
}
void MainWindow::configure_remote() {
    if (remote_.active()) {
        if (QMessageBox::question(
                this, "Local control API",
                QString("Listening on http://127.0.0.1:%1\nToken is on your clipboard. Stop the API?")
                    .arg(remote_.port())) == QMessageBox::Yes)
            remote_.stop();
        return;
    }
    if (!remote_.start()) {
        error("Cannot start local control API");
        return;
    }
    QApplication::clipboard()->setText(remote_.token());
    QMessageBox::information(
        this, "Local control API",
        QString("Listening on http://127.0.0.1:%1\nBearer token copied to clipboard.\n\nGET /v1/status "
                "(includes WebSocket port)\nGET /v1/project\nPOST /v1/command\n\nAccess is limited to this "
                "computer. Token expires when stopped.")
            .arg(remote_.port()));
}
void MainWindow::closeEvent(QCloseEvent* e) {
    if (!undo_.isClean()) {
        auto reply = QMessageBox::question(this, "Unsaved project", "Save changes before closing?",
                                           QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
        if (reply == QMessageBox::Cancel) {
            e->ignore();
            return;
        }
        if (reply == QMessageBox::Save) {
            save();
            if (!undo_.isClean()) {
                e->ignore();
                return;
            }
        }
    }
    QSettings settings;
    settings.setValue("windowGeometry", saveGeometry());
    settings.setValue("dockLayout", saveState());
    stop_output();
    e->accept();
}
