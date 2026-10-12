// SPDX-License-Identifier: GPL-3.0-only
#include "main_window.hpp"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHeaderView>
#include <QKeySequenceEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QTableWidget>
#include <QTextEdit>
#include <QVBoxLayout>
#include <QWizard>
#include <QWizardPage>
#include <QSpinBox>
#include <QAction>
#include <set>
#include <cmath>
namespace {
QString qs(const std::string& text) {
    return QString::fromStdString(text);
}
QDialogButtonBox* buttons(QDialog* dialog, QVBoxLayout* layout) {
    auto* box = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    layout->addWidget(box);
    QObject::connect(box, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
    QObject::connect(box, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    return box;
}
} // namespace
void MainWindow::filter_installation(const QString& text) {
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
        auto* root = tree_->topLevelItem(i);
        bool parent = root->text(0).contains(text, Qt::CaseInsensitive), any = parent;
        for (int j = 0; j < root->childCount(); ++j) {
            auto* child = root->child(j);
            bool match = parent || child->text(0).contains(text, Qt::CaseInsensitive);
            child->setHidden(!match);
            any |= match;
        }
        root->setHidden(!any);
        if (!text.isEmpty() && any)
            root->setExpanded(true);
    }
}
void MainWindow::update_selection() {
    if (!selection_x_)
        return;
    auto value = view_->selection_value();
    selection_x_->setEnabled(value.has_value() && !project_.locked);
    selection_y_->setEnabled(value.has_value() && !project_.locked);
    QSignalBlocker x(selection_x_), y(selection_y_);
    selection_label_->setText(
        QString("%1 points selected · normalized coordinates").arg(view_->selected_points().size()));
    if (!value) {
        selection_x_->setPrefix("");
        selection_y_->setPrefix("");
    }
    if (value) {
        if (!selection_x_->hasFocus())
            selection_x_->setValue(value->x);
        if (!selection_y_->hasFocus())
            selection_y_->setValue(value->y);
        if (auto* f = surface()) {
            bool mixed_x = false, mixed_y = false;
            for (int k : view_->selected_points())
                if (k >= 0 && k < int(f->mesh.points.size())) {
                    mixed_x |= std::abs(f->mesh.points[size_t(k)].x - value->x) > 1e-6;
                    mixed_y |= std::abs(f->mesh.points[size_t(k)].y - value->y) > 1e-6;
                }
            selection_x_->setPrefix(mixed_x ? "Mean " : "");
            selection_y_->setPrefix(mixed_y ? "Mean " : "");
        }
    }
}
void MainWindow::setup_installation() {
    if (project_.locked) {
        statusBar()->showMessage("Unlock configuration before setup", 3000);
        return;
    }
    if (!undo_.isClean() &&
        QMessageBox::question(this, "New installation", "Replace the current unsaved installation?") !=
            QMessageBox::Yes)
        return;
    QWizard wizard(this);
    wizard.setWindowTitle("Set up an installation");
    wizard.resize(750, 580);
    auto* first = new QWizardPage;
    first->setTitle("Choose your installation");
    first->setSubTitle("Start with a test pattern. Configure precise geometry in the workbench after setup.");
    auto* form = new QFormLayout(first);
    auto* name = new QLineEdit("My installation");
    name->setObjectName("setupName");
    form->addRow("Name", name);
    auto* count = new QSpinBox;
    count->setRange(1, 16);
    count->setValue(3);
    count->setObjectName("setupOutputs");
    form->addRow("Projectors", count);
    auto* shape = new QComboBox;
    shape->addItems({"Cylindrical screen", "Flat screen", "Irregular surface"});
    form->addRow("Geometry", shape);
    auto* simulated = new QCheckBox("Simulate on this monitor");
    simulated->setChecked(true);
    form->addRow(simulated);
    wizard.addPage(first);
    auto* content = new QWizardPage;
    content->setTitle("Choose a source");
    auto* content_form = new QFormLayout(content);
    auto* type = new QComboBox;
    type->addItems({"Alignment pattern", "Image", "Application window", "Desktop display"});
    content_form->addRow("Source type", type);
    auto* source_choice = new QComboBox;
    content_form->addRow("Source", source_choice);
    auto* browse = new QPushButton("Choose image…");
    content_form->addRow(browse);
    auto* source_help = new QLabel("Use a source display separate from your projector outputs.");
    source_help->setWordWrap(true);
    content_form->addRow(source_help);
    std::vector<ps::win::WindowInfo> windows;
    std::vector<ps::win::Display> displays, source_displays;
    QString image_path;
    connect(browse, &QPushButton::clicked, &wizard, [&] {
        image_path = QFileDialog::getOpenFileName(&wizard, "Choose source image", {},
                                                  "Images (*.png *.jpg *.jpeg *.bmp)");
        if (!image_path.isEmpty()) {
            source_choice->clear();
            source_choice->addItem(image_path);
        }
    });
    auto populate = [&] {
        source_choice->clear();
        browse->setVisible(type->currentIndex() == 1);
        try {
            if (type->currentIndex() == 0)
                source_choice->addItems({"grid", "checker", "gradient", "white", "motion"});
            if (type->currentIndex() == 1 && !image_path.isEmpty())
                source_choice->addItem(image_path);
            if (type->currentIndex() == 2) {
                windows = ps::win::windows();
                for (auto& w : windows)
                    source_choice->addItem(qs(w.title));
            }
            if (type->currentIndex() == 3) {
                source_displays = ps::win::displays();
                for (auto& d : source_displays)
                    source_choice->addItem(qs(d.name));
            }
        } catch (const std::exception& e) {
            source_help->setText(qs(e.what()));
        }
    };
    connect(type, qOverload<int>(&QComboBox::currentIndexChanged), &wizard, [&](int) { populate(); });
    populate();
    wizard.addPage(content);
    auto* assignment = new QWizardPage;
    assignment->setTitle("Assign outputs");
    assignment->setSubTitle(
        "Simulation uses separate windows. Fullscreen requires a connected display for every output.");
    auto* assignments = new QVBoxLayout(assignment);
    auto* table = new QTableWidget(0, 2);
    table->setHorizontalHeaderLabels({"Projector", "Display"});
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    assignments->addWidget(table);
    wizard.addPage(assignment);
    auto* final = new QWizardPage;
    final->setTitle("Align, blend, and save");
    auto* final_layout = new QVBoxLayout(final);
    auto* instructions = new QLabel(
        "1. Identify each output from the toolbar.\n\n2. In Warp, move mesh points onto the target "
        "surface.\n\n3. In Blend and Color, match adjacent projectors.\n\n4. Save a preset and the "
        "installation.\n\nBlackout and Stop remain available while configuration is locked.");
    instructions->setWordWrap(true);
    final_layout->addWidget(instructions);
    auto* run = new QCheckBox("Start simulated outputs when finished");
    run->setObjectName("setupRun");
    run->setChecked(true);
    final_layout->addWidget(run);
    wizard.addPage(final);
    connect(&wizard, &QWizard::currentIdChanged, &wizard, [&](int page) {
        if (page == 2) {
            try {
                displays = ps::win::displays();
            } catch (...) {
                displays.clear();
            }
            table->setRowCount(count->value());
            for (int i = 0; i < count->value(); ++i) {
                auto* item = new QTableWidgetItem(QString("Projector %1").arg(i + 1));
                item->setFlags(item->flags() & ~Qt::ItemIsEditable);
                table->setItem(i, 0, item);
                auto* display = new QComboBox;
                display->addItem("Simulation", QString());
                for (auto& d : displays)
                    display->addItem(qs(d.name), qs(d.id));
                display->setEnabled(!simulated->isChecked());
                table->setCellWidget(i, 1, display);
            }
        }
    });
    while (wizard.exec() == QDialog::Accepted) {
        try {
            auto candidate = ps::example_project(count->value(), shape->currentIndex() == 2);
            candidate.name = name->text().trimmed().toStdString();
            candidate.screen.shape = shape->currentIndex() == 0 ? "cylinder" : "plane";
            auto& source = candidate.sources[0];
            int selected = source_choice->currentIndex();
            if (selected < 0)
                throw std::runtime_error("Choose an available source before finishing setup");
            if (type->currentIndex() == 0)
                source.pattern = source_choice->currentText().toStdString();
            else if (type->currentIndex() == 1) {
                source.type = "image";
                source.image = image_path.toStdString();
                source.name = "Image source";
            } else if (type->currentIndex() == 2) {
                auto& w = windows.at(size_t(selected));
                source.type = "window";
                source.name = w.title;
                source.window_handle = reinterpret_cast<std::uint64_t>(w.hwnd);
                source.window_title = w.title;
                source.executable = w.executable;
                source.window_class = w.window_class;
            } else {
                source.type = "desktop";
                source.display = source_displays.at(size_t(selected)).id;
                source.name = "Desktop source";
            }
            for (int i = 0; i < count->value(); ++i) {
                auto id = static_cast<QComboBox*>(table->cellWidget(i, 1))->currentData().toString();
                if (!simulated->isChecked() && id.isEmpty())
                    throw std::runtime_error("Assign every display, or enable simulation");
                candidate.outputs[size_t(i)].display = simulated->isChecked() ? "" : id.toStdString();
            }
            auto errors = ps::validate(candidate);
            if (!errors.empty())
                throw std::runtime_error(errors[0]);
            stop_output();
            project_ = std::move(candidate);
            path_.clear();
            undo_.clear();
            undo_.resetClean();
            view_->output_index = view_->surface_index = 0;
            view_->fit_view();
            refresh();
            if (!qApp->arguments().contains("--smoke"))
                QSettings().setValue("setupCompleted", true);
            if (run->isChecked())
                start_output(false);
            return;
        } catch (const std::exception& e) {
            error(qs(e.what()));
        }
    }
}
void MainWindow::manage_profiles() {
    if (project_.locked) {
        statusBar()->showMessage("Unlock configuration to edit application profiles", 3000);
        return;
    }
    QDialog dialog(this);
    dialog.setWindowTitle("Application profiles");
    dialog.resize(1000, 500);
    auto* layout = new QVBoxLayout(&dialog);
    auto* help = new QLabel("Profiles select the API and projector when you explicitly attach an "
                            "application. Exclusions always take precedence.");
    help->setWordWrap(true);
    layout->addWidget(help);
    auto* table = new QTableWidget(0, 7);
    table->setHorizontalHeaderLabels(
        {"Name", "Executable path or name", "API", "Simulator", "Projector", "Enabled", "Excluded"});
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table->horizontalHeader()->setStretchLastSection(true);
    layout->addWidget(table);
    auto add = [&](ps::ApplicationProfile profile) {
        int row = table->rowCount();
        table->insertRow(row);
        auto* name = new QTableWidgetItem(qs(profile.name));
        name->setData(Qt::UserRole, qs(profile.id));
        table->setItem(row, 0, name);
        table->setItem(row, 1, new QTableWidgetItem(qs(profile.executable)));
        auto* api = new QComboBox;
        api->addItems({"capture", "dx11", "dx12"});
        api->setCurrentText(qs(profile.api));
        table->setCellWidget(row, 2, api);
        auto* sim = new QComboBox;
        sim->addItems({"Generic", "2020", "2024"});
        sim->setCurrentText(profile.simulator_version.empty() ? "Generic" : qs(profile.simulator_version));
        table->setCellWidget(row, 3, sim);
        auto* output = new QComboBox;
        for (auto& o : project_.outputs)
            output->addItem(qs(o.name), qs(o.id));
        output->setCurrentIndex(std::max(0, output->findData(qs(profile.output_id))));
        table->setCellWidget(row, 4, output);
        for (int column = 5; column <= 6; ++column) {
            auto* box = new QCheckBox;
            box->setChecked(column == 5 ? profile.enabled : profile.excluded);
            table->setCellWidget(row, column, box);
        }
    };
    for (auto& p : project_.applications)
        add(p);
    auto* bar = new QHBoxLayout;
    layout->addLayout(bar);
    auto* create = new QPushButton("Add profile");
    bar->addWidget(create);
    connect(create, &QPushButton::clicked, &dialog, [&] {
        ps::ApplicationProfile p;
        p.id = ps::make_id();
        p.name = "Application";
        add(p);
    });
    auto* browse = new QPushButton("Choose executable…");
    bar->addWidget(browse);
    connect(browse, &QPushButton::clicked, &dialog, [&] {
        int row = table->currentRow();
        if (row < 0)
            return;
        auto path =
            QFileDialog::getOpenFileName(&dialog, "Application executable", {}, "Applications (*.exe)");
        if (!path.isEmpty())
            table->item(row, 1)->setText(path);
    });
    auto* remove = new QPushButton("Remove selected");
    bar->addWidget(remove);
    connect(remove, &QPushButton::clicked, &dialog, [&] {
        if (table->currentRow() >= 0)
            table->removeRow(table->currentRow());
    });
    buttons(&dialog, layout);
    if (dialog.exec() != QDialog::Accepted)
        return;
    mutate("Edit application profiles", [&](auto& p) {
        p.applications.clear();
        for (int row = 0; row < table->rowCount(); ++row) {
            ps::ApplicationProfile a;
            a.id = table->item(row, 0)->data(Qt::UserRole).toString().toStdString();
            a.name = table->item(row, 0)->text().toStdString();
            a.executable = table->item(row, 1)->text().toStdString();
            a.api = static_cast<QComboBox*>(table->cellWidget(row, 2))->currentText().toStdString();
            auto version = static_cast<QComboBox*>(table->cellWidget(row, 3))->currentText();
            a.simulator_version = version == "Generic" ? "" : version.toStdString();
            a.output_id =
                static_cast<QComboBox*>(table->cellWidget(row, 4))->currentData().toString().toStdString();
            a.enabled = static_cast<QCheckBox*>(table->cellWidget(row, 5))->isChecked();
            a.excluded = static_cast<QCheckBox*>(table->cellWidget(row, 6))->isChecked();
            p.applications.push_back(std::move(a));
        }
    });
}
void MainWindow::manage_presets() {
    if (project_.locked) {
        statusBar()->showMessage("Unlock configuration to manage presets", 3000);
        return;
    }
    QDialog dialog(this);
    dialog.setWindowTitle("Presets and startup");
    dialog.resize(550, 400);
    auto* layout = new QVBoxLayout(&dialog);
    auto* table = new QTableWidget(0, 1);
    table->setHorizontalHeaderLabels({"Preset name"});
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    layout->addWidget(table);
    auto presets = project_.presets;
    auto* startup = new QComboBox;
    startup->addItem("Use the saved output state", QString());
    for (auto& p : presets) {
        int row = table->rowCount();
        table->insertRow(row);
        auto* item = new QTableWidgetItem(qs(p.name));
        item->setData(Qt::UserRole, qs(p.id));
        table->setItem(row, 0, item);
        startup->addItem(qs(p.name), qs(p.id));
    }
    startup->setCurrentIndex(std::max(0, startup->findData(qs(project_.startup_preset))));
    layout->addWidget(new QLabel("Preset applied when opening this project"));
    layout->addWidget(startup);
    auto* remove = new QPushButton("Delete selected preset");
    layout->addWidget(remove);
    connect(remove, &QPushButton::clicked, &dialog, [&] {
        int row = table->currentRow();
        if (row >= 0) {
            auto id = table->item(row, 0)->data(Qt::UserRole);
            int index = startup->findData(id);
            if (index >= 0)
                startup->removeItem(index);
            table->removeRow(row);
        }
    });
    buttons(&dialog, layout);
    if (dialog.exec() != QDialog::Accepted)
        return;
    mutate("Manage presets", [&](auto& p) {
        p.presets.clear();
        for (int row = 0; row < table->rowCount(); ++row)
            for (auto preset : presets)
                if (qs(preset.id) == table->item(row, 0)->data(Qt::UserRole).toString()) {
                    preset.name = table->item(row, 0)->text().toStdString();
                    p.presets.push_back(std::move(preset));
                }
        p.startup_preset = startup->currentData().toString().toStdString();
    });
}
void MainWindow::edit_shortcuts() {
    QDialog dialog(this);
    dialog.setWindowTitle("Keyboard shortcuts");
    auto* layout = new QVBoxLayout(&dialog);
    auto* table = new QTableWidget(0, 2);
    table->setHorizontalHeaderLabels({"Action", "Shortcut"});
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    layout->addWidget(table);
    std::vector<QAction*> actions;
    for (auto* action : findChildren<QAction*>())
        if (action->property("configurableShortcut").toBool()) {
            int row = table->rowCount();
            table->insertRow(row);
            auto* item = new QTableWidgetItem(action->text());
            item->setFlags(item->flags() & ~Qt::ItemIsEditable);
            table->setItem(row, 0, item);
            table->setCellWidget(row, 1, new QKeySequenceEdit(action->shortcut()));
            actions.push_back(action);
        }
    layout->addWidget(new QLabel("Ctrl+Shift+Esc remains available as an emergency stop."));
    buttons(&dialog, layout);
    if (dialog.exec() != QDialog::Accepted)
        return;
    std::set<QString> used;
    for (size_t i = 0; i < actions.size(); ++i) {
        auto sequence = static_cast<QKeySequenceEdit*>(table->cellWidget(int(i), 1))->keySequence();
        if (!sequence.isEmpty() &&
            (!used.insert(sequence.toString()).second || sequence == QKeySequence("Ctrl+Shift+Esc"))) {
            error("Shortcut conflict. Choose a unique shortcut for each action.");
            return;
        }
    }
    QSettings settings;
    for (size_t i = 0; i < actions.size(); ++i) {
        auto sequence = static_cast<QKeySequenceEdit*>(table->cellWidget(int(i), 1))->keySequence();
        actions[i]->setShortcut(sequence);
        settings.setValue("shortcuts/" + actions[i]->objectName(), sequence.toString());
    }
}
void MainWindow::show_diagnostics() {
    auto* dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle("Output diagnostics");
    dialog->resize(760, 550);
    auto* layout = new QVBoxLayout(dialog);
    auto* text = new QTextEdit;
    text->setReadOnly(true);
    layout->addWidget(text);
    auto* export_button = new QPushButton("Export diagnostic report…");
    layout->addWidget(export_button);
    auto snapshot = [this] {
        return ps::Json{
            {"version", "0.2.0"},
            {"preview_error", render_error_.toStdString()},
            {"renderer", last_telemetry_},
            {"qualification", "See docs/VALIDATION.md; software telemetry is not physical display latency"}};
    };
    auto* timer = new QTimer(dialog);
    connect(timer, &QTimer::timeout, dialog,
            [text, snapshot] { text->setPlainText(qs(snapshot().dump(2))); });
    timer->start(1000);
    text->setPlainText(qs(snapshot().dump(2)));
    connect(export_button, &QPushButton::clicked, dialog, [this, dialog, snapshot] {
        auto path = QFileDialog::getSaveFileName(dialog, "Export diagnostics", {}, "JSON (*.json)");
        if (!path.isEmpty())
            try {
                ps::atomic_write(std::filesystem::path(path.toStdWString()), snapshot().dump(2));
            } catch (const std::exception& e) {
                error(qs(e.what()));
            }
    });
    dialog->show();
}
