// SPDX-License-Identifier: GPL-3.0-only
#include "main_window.hpp"
#include <QApplication>
#include <QStyleFactory>
#include <roapi.h>
int main(int argc, char** argv) {
#ifdef _WIN32
    RoInitialize(RO_INIT_SINGLETHREADED);
#endif
    QApplication app(argc, argv);
    app.setApplicationName("Projection Studio");
    app.setOrganizationName("ProjectionStudio");
    app.setStyle(QStyleFactory::create("Fusion"));
    app.setStyleSheet(R"(
      QWidget { background:#202a33; color:#dce7ed; font-family:'Segoe UI'; font-size:12px; }
      QMainWindow, QMenuBar, QToolBar { background:#182129; }
      QDockWidget::title { padding:12px; color:#8da3b1; font-size:11px; }
      QTreeWidget, QScrollArea, QScrollArea>QWidget>QWidget { background:#202a33; border:0; }
      QTreeWidget::item { padding:8px 3px; } QTreeWidget::item:selected { background:#314a4b; color:#85f1d4; }
      QTabWidget::pane { border:0; } QTabBar::tab { background:#182129; padding:10px 9px; color:#92a7b5; }
      QTabBar::tab:selected { color:#6ce8c9; border-bottom:2px solid #6ce8c9; }
      QPushButton, QToolButton { background:#2b3945; border:1px solid #3c4f5d; border-radius:4px; padding:7px 10px; }
      QPushButton:hover, QToolButton:hover { background:#354b56; }
      QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox { background:#16212a; border:1px solid #3a4b58; border-radius:3px; padding:6px; }
      QStatusBar { background:#142029; color:#8da7b5; } QMenu { background:#24313b; } QMenu::item:selected { background:#38515a; }
      QCheckBox { spacing:8px; } QToolBar { border:0; padding:5px; spacing:6px; }
    )");
    MainWindow window;
    QStringList args = app.arguments();
    if (args.contains("--project")) {
        int i = args.indexOf("--project");
        if (i + 1 < args.size())
            window.load(args[i + 1]);
    }
    window.show();
    if (args.contains("--smoke")) {
        int i = args.indexOf("--smoke");
        window.smoke(i + 1 < args.size() ? args[i + 1] : "smoke");
    }
    return app.exec();
}
