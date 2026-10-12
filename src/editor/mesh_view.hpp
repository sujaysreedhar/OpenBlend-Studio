// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "ps/geometry.hpp"
#include <QImage>
#include <QWidget>
#include <functional>
class MeshView : public QWidget {
  public:
    enum class Mode { Geometry, Source, Mask };
    explicit MeshView(QWidget* parent = nullptr);
    ps::Project* project{};
    int output_index{}, surface_index{};
    Mode mode{Mode::Geometry};
    ps::SelectionMode selection{ps::SelectionMode::Point};
    ps::AxisLock axis{ps::AxisLock::None};
    bool boundary_interpolation{}, show_grid{true};
    bool snap{};
    double snap_step{.01};
    double compare{1};
    std::function<QImage()> source_preview;
    std::function<void()> selection_changed;
    std::function<QImage(bool source)> preview;
    std::function<void()> begin_edit, end_edit, changed;
    void clear_selection();
    void fit_view();
    const std::vector<int>& selected_points() const {
        return selected_;
    }
    std::optional<ps::Vec2> selection_value() const;
    void set_selection_coordinate(bool x_axis, double value);
    QPointF point_position(ps::Vec2 p) const {
        return to_screen(p);
    }
    double zoom() const {
        return zoom_;
    }

  protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void wheelEvent(QWheelEvent*) override;

  private:
    std::vector<int> selected_;
    bool dragging_{};
    bool panning_{}, selecting_{}, additive_{};
    double zoom_{1};
    QPointF pan_, anchor_;
    QRectF selection_rect_;
    QPointF last_;
    QRectF image_rect() const;
    QPointF to_screen(ps::Vec2) const;
    ps::Vec2 to_uv(QPointF) const;
    ps::Surface* surface();
};
