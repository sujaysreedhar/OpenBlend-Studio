// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "control_server.hpp"
#include "mesh_view.hpp"
#include "preview_service.hpp"
#include <QLabel>
#include <QLocalSocket>
#include <QMainWindow>
#include <QProcess>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeWidget>
#include <QElapsedTimer>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QUndoStack>
#include <memory>
#ifdef _WIN32
#include "ps/windows.hpp"
#endif
class MainWindow : public QMainWindow {
  public:
    MainWindow();
    ~MainWindow();
    void load(const QString&);
    void save(const QString& path = {});
    void refresh();
    void smoke(const QString& directory);

  protected:
    void closeEvent(QCloseEvent*) override;

  private:
    ps::Project project_;
    ps::Project before_;
    QString path_;
    QUndoStack undo_;
    MeshView* view_{};
    QTreeWidget* tree_{};
    QTabWidget* tabs_{};
    QLabel* info_{};
    QLabel* runtime_{};
    QProcess worker_;
    QLocalSocket pipe_;
    QTemporaryDir temporary_;
    QTimer live_, connect_timer_;
    ControlServer remote_;
    bool refreshing_{}, blackout_{}, pending_update_{};
    QString pipe_name_;
    QByteArray received_;
    QImage cached_preview_;
    bool preview_dirty_{true}, preview_source_{};
    QString render_error_;
    PreviewService previews_;
    PreviewService comparison_;
    QImage comparison_image_;
    QString preview_key_;
    QProcess attach_;
    QTimer stop_timer_, attach_timer_;
    int queued_output_{-1};
    bool stopping_{};
    ps::Json worker_document_, sending_document_, last_telemetry_;
    std::uint64_t worker_revision_{}, sending_revision_{};
    QElapsedTimer update_clock_;
    bool resync_{true};
    QString tree_signature_;
    QLineEdit* search_{};
    QDoubleSpinBox *selection_x_{}, *selection_y_{};
    QLabel* selection_label_{};
    void update_selection();
    void setup_installation();
    void manage_profiles();
    void manage_presets();
    void edit_shortcuts();
    void show_diagnostics();
    void filter_installation(const QString&);
#ifdef _WIN32
#endif
    ps::Output* output();
    ps::Surface* surface();
    ps::Source* source();
    void mutate(const QString&, const std::function<void(ps::Project&)>&);
    void commit_edit();
    void build_properties();
    void start_output(bool fullscreen);
    void stop_output();
    void send(const ps::Json&);
    void set_blackout(bool);
    void import_calibration();
    void export_views();
    void camera_patch(bool restore = false);
    void configure_remote();
    void attach_application();
    void update_adapter();
    QString adapter_manifest_;
    std::uint64_t adapter_hwnd_{};
    std::string adapter_output_;
    QImage preview(bool source_mode);
    void error(const QString&);
};
