// SPDX-License-Identifier: GPL-3.0-only
#include "mesh_view.hpp"
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
MeshView::MeshView(QWidget* parent) : QWidget(parent) {
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(480, 320);
    setMouseTracking(true);
}
ps::Surface* MeshView::surface() {
    if (!project || output_index < 0 || output_index >= int(project->outputs.size()))
        return nullptr;
    auto& o = project->outputs[size_t(output_index)];
    if (surface_index < 0 || surface_index >= int(o.surfaces.size()))
        return nullptr;
    return &o.surfaces[size_t(surface_index)];
}
void MeshView::clear_selection() {
    selected_.clear();
    if (selection_changed)
        selection_changed();
    update();
}
QRectF MeshView::image_rect() const {
    double aspect = 16. / 9;
    if (project && output_index >= 0 && output_index < int(project->outputs.size())) {
        auto& o = project->outputs[size_t(output_index)];
        aspect = double(o.width) / o.height;
    }
    double w = width() - 80., h = w / aspect;
    if (h > height() - 100.) {
        h = height() - 100.;
        w = h * aspect;
    }
    w *= zoom_;
    h *= zoom_;
    return {(width() - w) / 2 + pan_.x(), (height() - h) / 2 + pan_.y(), w, h};
}
void MeshView::fit_view() {
    zoom_ = 1;
    pan_ = {};
    update();
}
void MeshView::wheelEvent(QWheelEvent* event) {
    auto uv = to_uv(event->position());
    zoom_ = std::clamp(zoom_ * std::pow(1.0015, event->angleDelta().y()), .25, 12.);
    pan_ += event->position() - to_screen(uv);
    update();
    event->accept();
}
std::optional<ps::Vec2> MeshView::selection_value() const {
    if (!project || mode != Mode::Geometry || selected_.empty() || output_index < 0 ||
        output_index >= int(project->outputs.size()))
        return {};
    const auto& surfaces = project->outputs[size_t(output_index)].surfaces;
    if (surface_index < 0 || surface_index >= int(surfaces.size()))
        return {};
    const auto& points = surfaces[size_t(surface_index)].mesh.points;
    ps::Vec2 result{};
    int count = 0;
    for (int k : selected_)
        if (k >= 0 && k < int(points.size())) {
            result.x += points[size_t(k)].x;
            result.y += points[size_t(k)].y;
            ++count;
        }
    if (!count)
        return {};
    return ps::Vec2{result.x / count, result.y / count};
}
void MeshView::set_selection_coordinate(bool x_axis, double value) {
    auto origin = selection_value();
    auto* f = surface();
    if (!origin || !f || project->locked)
        return;
    if (begin_edit)
        begin_edit();
    ps::move_points(f->mesh, selected_,
                    x_axis ? ps::Vec2{value - origin->x, 0} : ps::Vec2{0, value - origin->y},
                    x_axis ? ps::AxisLock::X : ps::AxisLock::Y);
    if (boundary_interpolation)
        ps::interpolate_interior(f->mesh);
    if (changed)
        changed();
    if (end_edit)
        end_edit();
    if (selection_changed)
        selection_changed();
    update();
}
QPointF MeshView::to_screen(ps::Vec2 v) const {
    auto r = image_rect();
    return {r.x() + v.x * r.width(), r.y() + v.y * r.height()};
}
ps::Vec2 MeshView::to_uv(QPointF p) const {
    auto r = image_rect();
    return {(p.x() - r.x()) / r.width(), (p.y() - r.y()) / r.height()};
}
void MeshView::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor("#11171d"));
    p.setRenderHint(QPainter::Antialiasing);
    auto r = image_rect();
    p.setPen(QColor("#26313b"));
    for (int x = 20; x < width(); x += 24)
        for (int y = 20; y < height(); y += 24)
            p.drawPoint(x, y);
    p.fillRect(r, Qt::black);
    if (preview) {
        auto image = preview(mode == Mode::Source);
        if (!image.isNull())
            p.drawImage(r, image);
    }
    if (source_preview && compare < 1 && mode == Mode::Geometry) {
        auto image = source_preview();
        if (!image.isNull()) {
            p.save();
            p.setClipRect(QRectF(r.left(), r.top(), r.width() * (1 - compare), r.height()));
            p.drawImage(r, image);
            p.restore();
            p.setPen(QPen(QColor("#fff1ab"), 2));
            p.drawLine(QPointF(r.left() + r.width() * (1 - compare), r.top()),
                       QPointF(r.left() + r.width() * (1 - compare), r.bottom()));
        }
    }
    p.setPen(QColor("#60717e"));
    p.drawRect(r);
    auto* f = surface();
    if (!f)
        return;
    p.setPen(QColor("#9aaeba"));
    p.drawText(QPointF(r.left(), r.top() - 18), mode == Mode::Geometry ? "DESTINATION / WARP MESH"
                                                : mode == Mode::Source ? "SOURCE / CROP REGION"
                                                                       : "MASK / POLYGON");
    p.drawText(QPointF(r.left(), r.bottom() + 28),
               project->locked ? "Configuration locked"
                               : "Wheel: zoom  /  Middle-drag: pan  /  F: fit  /  Drag empty space: select");
    std::vector<ps::Vec2> points;
    if (mode == Mode::Geometry) {
        points = f->mesh.points;
        if (show_grid) {
            p.setPen(QPen(QColor(86, 222, 190, 170), 1));
            auto& m = f->mesh;
            for (int y = 0; y < m.rows; ++y)
                for (int x = 0; x < m.columns; ++x) {
                    auto a = to_screen(m.points[size_t(y * m.columns + x)]);
                    if (x + 1 < m.columns)
                        p.drawLine(a, to_screen(m.points[size_t(y * m.columns + x + 1)]));
                    if (y + 1 < m.rows)
                        p.drawLine(a, to_screen(m.points[size_t((y + 1) * m.columns + x)]));
                }
        }
    } else if (mode == Mode::Source) {
        auto s = f->source_rect;
        points = {{s.x, s.y}, {s.x + s.w, s.y}, {s.x + s.w, s.y + s.h}, {s.x, s.y + s.h}};
        QPolygonF polygon;
        for (auto q : points)
            polygon << to_screen(q);
        p.setBrush(QColor(78, 222, 187, 30));
        p.setPen(QPen(QColor("#56debe"), 2));
        p.drawPolygon(polygon);
    } else {
        points = f->mask.polygon;
        QPolygonF polygon;
        for (auto q : points)
            polygon << to_screen(q);
        p.setBrush(QColor(78, 222, 187, 30));
        p.setPen(QPen(QColor("#56debe"), 2));
        p.drawPolygon(polygon);
        p.setPen(QColor("#a7b7c2"));
        p.drawText(QPointF(r.left(), r.top() - 38),
                   "Right-click to add a vertex; Delete removes selected vertices");
    }
    for (size_t k = 0; k < points.size(); ++k) {
        bool selected = std::find(selected_.begin(), selected_.end(), int(k)) != selected_.end();
        p.setBrush(selected ? QColor("#fff1ab") : QColor("#56debe"));
        p.setPen(QPen(QColor("#10221f"), 1));
        p.drawEllipse(to_screen(points[k]), selected ? 5 : 3.5, selected ? 5 : 3.5);
    }
    if (selecting_) {
        p.setPen(QPen(QColor("#56debe"), 1, Qt::DashLine));
        p.setBrush(QColor(86, 222, 190, 25));
        p.drawRect(selection_rect_);
    }
}
void MeshView::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::MiddleButton) {
        panning_ = true;
        last_ = event->position();
        setCursor(Qt::ClosedHandCursor);
        return;
    }
    auto* f = surface();
    if (!f || project->locked)
        return;
    setFocus();
    auto uv = to_uv(event->position());
    if (event->button() == Qt::RightButton && mode == Mode::Mask) {
        if (begin_edit)
            begin_edit();
        f->mask.enabled = true;
        f->mask.polygon.push_back(uv);
        if (changed)
            changed();
        if (end_edit)
            end_edit();
        return;
    }
    std::vector<ps::Vec2> points = mode == Mode::Geometry ? f->mesh.points : f->mask.polygon;
    if (mode == Mode::Source) {
        auto s = f->source_rect;
        points = {{s.x, s.y}, {s.x + s.w, s.y}, {s.x + s.w, s.y + s.h}, {s.x, s.y + s.h}};
    }
    int hit = -1;
    double best = 14;
    for (size_t i = 0; i < points.size(); ++i) {
        auto delta = to_screen(points[i]) - event->position();
        double distance = std::hypot(delta.x(), delta.y());
        if (distance < best) {
            best = distance;
            hit = int(i);
        }
    }
    if (hit < 0) {
        additive_ = event->modifiers() & Qt::ControlModifier;
        if (!additive_)
            selected_.clear();
        selecting_ = event->button() == Qt::LeftButton;
        anchor_ = event->position();
        selection_rect_ = QRectF(anchor_, anchor_);
        if (selection_changed)
            selection_changed();
        update();
        return;
    }
    auto chosen = mode == Mode::Geometry ? ps::select_points(f->mesh, hit, selection) : std::vector<int>{hit};
    if (event->modifiers() & Qt::ControlModifier) {
        for (int i : chosen)
            if (std::find(selected_.begin(), selected_.end(), i) == selected_.end())
                selected_.push_back(i);
    } else
        selected_ = chosen;
    if (begin_edit)
        begin_edit();
    dragging_ = true;
    if (selection_changed)
        selection_changed();
    last_ = event->position();
    update();
}
void MeshView::mouseMoveEvent(QMouseEvent* event) {
    if (panning_) {
        pan_ += event->position() - last_;
        last_ = event->position();
        update();
        return;
    }
    if (selecting_) {
        selection_rect_ = QRectF(anchor_, event->position()).normalized();
        update();
        return;
    }
    if (!dragging_)
        return;
    auto* f = surface();
    if (!f)
        return;
    auto a = to_uv(last_), b = to_uv(event->position());
    ps::Vec2 delta{b.x - a.x, b.y - a.y};
    last_ = event->position();
    if (mode == Mode::Geometry) {
        if (snap && !selected_.empty() && !(event->modifiers() & Qt::AltModifier)) {
            auto origin = f->mesh.points[size_t(selected_[0])];
            delta = {std::round((origin.x + delta.x) / snap_step) * snap_step - origin.x,
                     std::round((origin.y + delta.y) / snap_step) * snap_step - origin.y};
            // Preserve sub-grid motion until a snap boundary is crossed.
            if (delta.x == 0 && delta.y == 0) {
                last_ = to_screen(a);
                return;
            }
        }
        ps::move_points(f->mesh, selected_, delta, axis);
        if (boundary_interpolation)
            ps::interpolate_interior(f->mesh);
    } else if (mode == Mode::Mask) {
        for (int k : selected_)
            if (k < int(f->mask.polygon.size())) {
                f->mask.polygon[size_t(k)].x = std::clamp(f->mask.polygon[size_t(k)].x + delta.x, 0., 1.);
                f->mask.polygon[size_t(k)].y = std::clamp(f->mask.polygon[size_t(k)].y + delta.y, 0., 1.);
            }
    } else if (!selected_.empty()) {
        auto& r = f->source_rect;
        double x0 = r.x, y0 = r.y, x1 = r.x + r.w, y1 = r.y + r.h;
        int corner = selected_[0];
        if (corner == 0 || corner == 3)
            x0 = std::clamp(b.x, 0., x1 - .005);
        else
            x1 = std::clamp(b.x, x0 + .005, 1.);
        if (corner == 0 || corner == 1)
            y0 = std::clamp(b.y, 0., y1 - .005);
        else
            y1 = std::clamp(b.y, y0 + .005, 1.);
        r = {x0, y0, x1 - x0, y1 - y0};
    }
    if (changed)
        changed();
    if (selection_changed)
        selection_changed();
    update();
}
void MeshView::mouseReleaseEvent(QMouseEvent*) {
    if (panning_) {
        panning_ = false;
        unsetCursor();
        return;
    }
    if (selecting_) {
        selecting_ = false;
        if (auto* f = surface()) {
            std::vector<ps::Vec2> source_points;
            if (mode == Mode::Source) {
                auto r = f->source_rect;
                source_points = {{r.x, r.y}, {r.x + r.w, r.y}, {r.x + r.w, r.y + r.h}, {r.x, r.y + r.h}};
            }
            const auto& points = mode == Mode::Geometry ? f->mesh.points
                                 : mode == Mode::Mask   ? f->mask.polygon
                                                        : source_points;
            for (size_t i = 0; i < points.size(); ++i)
                if (selection_rect_.contains(to_screen(points[i])) &&
                    std::find(selected_.begin(), selected_.end(), int(i)) == selected_.end())
                    selected_.push_back(int(i));
        }
        if (selection_changed)
            selection_changed();
        update();
        return;
    }
    if (dragging_) {
        dragging_ = false;
        if (end_edit)
            end_edit();
    }
}
void MeshView::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_F || event->key() == Qt::Key_Home) {
        fit_view();
        return;
    }
    auto* f = surface();
    if (!f || project->locked)
        return;
    if (event->matches(QKeySequence::SelectAll) && mode == Mode::Geometry) {
        selected_ = ps::select_points(f->mesh, 0, ps::SelectionMode::All);
        if (selection_changed)
            selection_changed();
        update();
        return;
    }
    double step = (event->modifiers() & Qt::ShiftModifier) ? .01 : .001;
    ps::Vec2 d{};
    if (event->key() == Qt::Key_Left)
        d.x = -step;
    else if (event->key() == Qt::Key_Right)
        d.x = step;
    else if (event->key() == Qt::Key_Up)
        d.y = -step;
    else if (event->key() == Qt::Key_Down)
        d.y = step;
    else if (event->key() == Qt::Key_Delete && mode == Mode::Mask && !selected_.empty()) {
        if (begin_edit)
            begin_edit();
        std::sort(selected_.rbegin(), selected_.rend());
        for (int k : selected_)
            if (f->mask.polygon.size() > 3 && k < int(f->mask.polygon.size()))
                f->mask.polygon.erase(f->mask.polygon.begin() + k);
        selected_.clear();
        if (changed)
            changed();
        if (end_edit)
            end_edit();
        return;
    } else {
        QWidget::keyPressEvent(event);
        return;
    }
    if (mode == Mode::Geometry && !selected_.empty()) {
        if (begin_edit)
            begin_edit();
        ps::move_points(f->mesh, selected_, d, axis);
        if (boundary_interpolation)
            ps::interpolate_interior(f->mesh);
        if (changed)
            changed();
        if (end_edit)
            end_edit();
        update();
    }
}
