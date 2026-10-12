// SPDX-License-Identifier: GPL-3.0-only
#include "main_window.hpp"
#include "ps/runtime.hpp"
#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QWizard>
#include <QSpinBox>
#include <QAbstractButton>
#include <QAction>
#include <memory>
void MainWindow::smoke(const QString& directory) {
    QDir().mkpath(directory);
    QTimer::singleShot(1200, this, [this, directory] {
        try {
            auto require = [](bool ok, const char* message) {
                if (!ok)
                    throw std::runtime_error(message);
            };
            auto original = project_;
            QTimer::singleShot(0, this, [this] {
                auto* wizard = findChild<QWizard*>();
                if (!wizard)
                    return;
                wizard->findChild<QLineEdit*>("setupName")->setText("Guided setup test");
                wizard->findChild<QSpinBox*>("setupOutputs")->setValue(2);
                wizard->findChild<QCheckBox*>("setupRun")->setChecked(false);
                wizard->button(QWizard::NextButton)->click();
                wizard->button(QWizard::NextButton)->click();
                wizard->button(QWizard::NextButton)->click();
                wizard->button(QWizard::FinishButton)->click();
            });
            setup_installation();
            require(project_.name == "Guided setup test" && project_.outputs.size() == 2,
                    "Guided setup did not create the requested installation");
            project_ = ps::example_project(3);
            undo_.clear();
            view_->fit_view();
            view_->clear_selection();
            refresh();
            auto mouse = [&](QEvent::Type type, QPointF at, Qt::MouseButton button,
                             Qt::MouseButtons buttons) {
                QMouseEvent event(type, at, at, button, buttons, Qt::NoModifier);
                QApplication::sendEvent(view_, &event);
            };
            auto center = view_->point_position({.5, .5});
            mouse(QEvent::MouseButtonPress, center, Qt::LeftButton, Qt::LeftButton);
            mouse(QEvent::MouseMove, center + QPointF(20, 10), Qt::NoButton, Qt::LeftButton);
            mouse(QEvent::MouseButtonRelease, center + QPointF(20, 10), Qt::LeftButton, Qt::NoButton);
            auto edited = project_.outputs[0].surfaces[0].mesh.points[12];
            require(edited.x > .5 && edited.y > .5, "Drag did not update geometry");
            undo_.undo();
            require(project_.outputs[0].surfaces[0].mesh.points[12] == ps::Vec2{.5, .5}, "Drag undo failed");
            undo_.redo();
            require(project_.outputs[0].surfaces[0].mesh.points[12] == edited, "Drag redo failed");
            auto* opacity = tabs_->findChild<QDoubleSpinBox*>("Opacity");
            require(opacity, "Missing opacity field");
            QApplication::setActiveWindow(this);
            opacity->setFocus();
            require(opacity->hasFocus(), "Test could not focus the opacity field");
            opacity->setValue(.75);
            QMetaObject::invokeMethod(opacity, "editingFinished", Qt::DirectConnection);
            require(tabs_->findChild<QDoubleSpinBox*>("Opacity") == opacity,
                    "Property controls were recreated during edit");
            require(opacity->hasFocus(), "Property edit lost keyboard focus");
            undo_.undo();
            require(opacity->value() == 1, "Persistent inspector failed to reflect undo");
            view_->setFocus();
            QWheelEvent wheel(center, center, {}, QPoint(0, 120), Qt::NoButton, Qt::NoModifier,
                              Qt::NoScrollPhase, false);
            QApplication::sendEvent(view_, &wheel);
            require(view_->zoom() > 1, "Viewport zoom failed");
            auto zoomed = view_->point_position({.5, .5});
            require(std::abs(zoomed.x() - center.x()) < .1 && std::abs(zoomed.y() - center.y()) < .1,
                    "Zoom moved the cursor anchor");
            view_->fit_view();
            mouse(QEvent::MouseButtonPress, center, Qt::MiddleButton, Qt::MiddleButton);
            mouse(QEvent::MouseMove, center + QPointF(40, 20), Qt::NoButton, Qt::MiddleButton);
            mouse(QEvent::MouseButtonRelease, center + QPointF(40, 20), Qt::MiddleButton, Qt::NoButton);
            require(std::abs(view_->point_position({.5, .5}).x() - center.x() - 40) < .1,
                    "Viewport pan failed");
            view_->fit_view();
            undo_.undo(); // Restore the original grid before rectangle selection.
            auto a = view_->point_position({-.05, -.05}), b = view_->point_position({.52, .52});
            mouse(QEvent::MouseButtonPress, a, Qt::LeftButton, Qt::LeftButton);
            mouse(QEvent::MouseMove, b, Qt::NoButton, Qt::LeftButton);
            mouse(QEvent::MouseButtonRelease, b, Qt::LeftButton, Qt::NoButton);
            require(view_->selected_points().size() == 9, "Rectangle selection failed");
            view_->set_selection_coordinate(true, .3);
            require(std::abs(view_->selection_value()->x - .3) < 1e-8, "Group coordinate edit failed");
            project_.locked = true;
            auto before = project_.outputs[0].surfaces[0].mesh;
            QKeyEvent key(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
            QApplication::sendEvent(view_, &key);
            require(project_.outputs[0].surfaces[0].mesh == before, "Configuration lock failed");
            set_blackout(true);
            require(remote_.status()["blackout"] == true, "Blackout while locked failed");
            set_blackout(false);
            project_.locked = false;
            ps::Measurements input_latency;
            for (int i = 0; i < 30; ++i) {
                QElapsedTimer clock;
                clock.start();
                QApplication::sendEvent(view_, &key);
                view_->repaint();
                input_latency.add(clock.nsecsElapsed() / 1000000.);
            }
            project_ = original;
            undo_.clear();
            view_->clear_selection();
            refresh();
            preview(false);
            auto result =
                std::make_shared<ps::Json>(ps::Json{{"guided_setup", true},
                                                    {"drag_undo_redo_lock", true},
                                                    {"persistent_property_focus", true},
                                                    {"zoom_pan_rectangle_numeric", true},
                                                    {"input_to_paint_ms", input_latency.json()},
                                                    {"project_valid", ps::validate(project_).empty()},
                                                    {"outputs", project_.outputs.size()}});
            auto* timer = new QTimer(this);
            auto elapsed = std::make_shared<QElapsedTimer>();
            elapsed->start();
            connect(timer, &QTimer::timeout, this, [this, directory, result, timer, elapsed] {
                if (cached_preview_.isNull() && elapsed->elapsed() < 10000)
                    return;
                timer->stop();
                timer->deleteLater();
                (*result)["preview_ready"] = !cached_preview_.isNull();
                (*result)["render_error"] = render_error_.toStdString();
                auto path = std::filesystem::path(directory.toStdWString());
                ps::save_project(project_, path / "smoke-project.json");
                grab().save(directory + "/editor.png");
                cached_preview_.save(directory + "/preview.png");
                ps::atomic_write(path / "smoke-result.json", result->dump(2));
                QApplication::exit(!cached_preview_.isNull() && render_error_.isEmpty() ? 0 : 1);
            });
            timer->start(25);
        } catch (const std::exception& e) {
            ps::atomic_write(std::filesystem::path(directory.toStdWString()) / "smoke-result.json",
                             ps::Json{{"error", e.what()}}.dump(2));
            QApplication::exit(1);
        }
    });
}
