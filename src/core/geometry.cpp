// SPDX-License-Identifier: GPL-3.0-only
#include "ps/geometry.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
namespace ps {
static Vec2 add(Vec2 a, Vec2 b) {
    return {a.x + b.x, a.y + b.y};
}
static Vec2 sub(Vec2 a, Vec2 b) {
    return {a.x - b.x, a.y - b.y};
}
static Vec2 mul(Vec2 a, double k) {
    return {a.x * k, a.y * k};
}
static Vec2 mix(Vec2 a, Vec2 b, double t) {
    return add(mul(a, 1 - t), mul(b, t));
}
static double cross(Vec2 a, Vec2 b) {
    return a.x * b.y - a.y * b.x;
}
static Vec2 cubic(Vec2 a, Vec2 b, Vec2 c, Vec2 d, double t) {
    return {.5 * ((2 * b.x) + (-a.x + c.x) * t + (2 * a.x - 5 * b.x + 4 * c.x - d.x) * t * t +
                  (-a.x + 3 * b.x - 3 * c.x + d.x) * t * t * t),
            .5 * ((2 * b.y) + (-a.y + c.y) * t + (2 * a.y - 5 * b.y + 4 * c.y - d.y) * t * t +
                  (-a.y + 3 * b.y - 3 * c.y + d.y) * t * t * t)};
}
Vec2 evaluate_mesh(const Mesh& m, double u, double v) {
    if (m.columns < 2 || m.rows < 2 || m.points.size() != size_t(m.columns * m.rows))
        throw std::invalid_argument("Malformed mesh");
    u = std::clamp(u, 0., 1.) * (m.columns - 1);
    v = std::clamp(v, 0., 1.) * (m.rows - 1);
    int x = std::min(int(u), m.columns - 2), y = std::min(int(v), m.rows - 2);
    u -= x;
    v -= y;
    auto raw = [&](int a, int b) { return m.points[size_t(b * m.columns + a)]; };
    auto row = [&](int a, int b) {
        if (a < 0)
            return sub(mul(raw(0, b), 2), raw(1, b));
        if (a >= m.columns)
            return sub(mul(raw(m.columns - 1, b), 2), raw(m.columns - 2, b));
        return raw(a, b);
    };
    auto point = [&](int a, int b) {
        if (b < 0)
            return sub(mul(row(a, 0), 2), row(a, 1));
        if (b >= m.rows)
            return sub(mul(row(a, m.rows - 1), 2), row(a, m.rows - 2));
        return row(a, b);
    };
    if (!m.curved)
        return mix(mix(point(x, y), point(x + 1, y), u), mix(point(x, y + 1), point(x + 1, y + 1), u), v);
    Vec2 r[4];
    for (int k = 0; k < 4; ++k)
        r[k] = cubic(point(x - 1, y + k - 1), point(x, y + k - 1), point(x + 1, y + k - 1),
                     point(x + 2, y + k - 1), u);
    return cubic(r[0], r[1], r[2], r[3], v);
}
Mesh resample_mesh(const Mesh& m, int cols, int rows) {
    auto out = identity_mesh(cols, rows);
    out.curved = m.curved;
    for (int y = 0; y < rows; ++y)
        for (int x = 0; x < cols; ++x)
            out.points[size_t(y * cols + x)] =
                evaluate_mesh(m, double(x) / (cols - 1), double(y) / (rows - 1));
    return out;
}
Triangles triangulate(const Surface& s, int subdivisions) {
    Triangles t;
    int nx = (s.mesh.columns - 1) * std::clamp(subdivisions, 1, 16),
        ny = (s.mesh.rows - 1) * std::clamp(subdivisions, 1, 16);
    auto vertex = [&](int x, int y) {
        double u = double(x) / nx, v = double(y) / ny;
        auto p = evaluate_mesh(s.mesh, u, v);
        return Vertex{float(p.x), float(p.y), float(u), float(v)};
    };
    t.vertices.reserve(size_t(nx) * ny * 6);
    for (int y = 0; y < ny; ++y)
        for (int x = 0; x < nx; ++x) {
            auto a = vertex(x, y), b = vertex(x + 1, y), c = vertex(x, y + 1), d = vertex(x + 1, y + 1);
            for (auto q : {a, b, c, b, d, c})
                t.vertices.push_back(q);
        }
    return t;
}
std::optional<Vec2> inverse_map(const Mesh& m, Vec2 p) {
    // Solve the same tessellated mapping that the renderer uses, then refine curved grids.
    int n = m.curved ? 4 : 1;
    int nx = (m.columns - 1) * n, ny = (m.rows - 1) * n;
    auto tri = [&](Vec2 a, Vec2 b, Vec2 c, Vec2 ua, Vec2 ub, Vec2 uc) -> std::optional<Vec2> {
        auto v0 = sub(b, a), v1 = sub(c, a), vp = sub(p, a);
        double det = cross(v0, v1);
        if (std::abs(det) < 1e-12)
            return {};
        double s = cross(vp, v1) / det, t = cross(v0, vp) / det;
        if (s < -1e-8 || t < -1e-8 || s + t > 1 + 1e-8)
            return {};
        return add(add(mul(ua, 1 - s - t), mul(ub, s)), mul(uc, t));
    };
    for (int y = 0; y < ny; ++y)
        for (int x = 0; x < nx; ++x) {
            Vec2 uv[4] = {{double(x) / nx, double(y) / ny},
                          {double(x + 1) / nx, double(y) / ny},
                          {double(x) / nx, double(y + 1) / ny},
                          {double(x + 1) / nx, double(y + 1) / ny}};
            Vec2 q[4];
            for (int k = 0; k < 4; ++k)
                q[k] = evaluate_mesh(m, uv[k].x, uv[k].y);
            auto r = tri(q[0], q[1], q[2], uv[0], uv[1], uv[2]);
            if (!r)
                r = tri(q[1], q[3], q[2], uv[1], uv[3], uv[2]);
            if (r)
                return r;
        }
    return {};
}
std::vector<int> select_points(const Mesh& m, int index, SelectionMode mode) {
    std::vector<int> out;
    if (index < 0 || index >= int(m.points.size()))
        return out;
    for (int k = 0; k < int(m.points.size()); ++k) {
        int x = k % m.columns, y = k / m.columns;
        if ((mode == SelectionMode::Point && k == index) ||
            (mode == SelectionMode::Row && y == index / m.columns) ||
            (mode == SelectionMode::Column && x == index % m.columns) || mode == SelectionMode::All ||
            (mode == SelectionMode::Boundary && (x == 0 || y == 0 || x == m.columns - 1 || y == m.rows - 1)))
            out.push_back(k);
    }
    return out;
}
void move_points(Mesh& m, std::span<const int> indices, Vec2 d, AxisLock lock) {
    if (lock == AxisLock::X)
        d.y = 0;
    if (lock == AxisLock::Y)
        d.x = 0;
    for (int k : indices)
        if (k >= 0 && k < int(m.points.size()))
            m.points[size_t(k)] = add(m.points[size_t(k)], d);
}
void interpolate_interior(Mesh& m) {
    auto at = [&](int x, int y) { return m.points[size_t(y * m.columns + x)]; };
    for (int y = 1; y < m.rows - 1; ++y)
        for (int x = 1; x < m.columns - 1; ++x) {
            double u = double(x) / (m.columns - 1), v = double(y) / (m.rows - 1);
            auto boundaries =
                add(mix(at(x, 0), at(x, m.rows - 1), v), mix(at(0, y), at(m.columns - 1, y), u));
            auto corners = mix(mix(at(0, 0), at(m.columns - 1, 0), u),
                               mix(at(0, m.rows - 1), at(m.columns - 1, m.rows - 1), u), v);
            m.points[size_t(y * m.columns + x)] = sub(boundaries, corners);
        }
}
double mask_alpha(const Mask& m, Vec2 p) {
    if (!m.enabled)
        return 1.;
    bool inside = true;
    double distance = 1.;
    if (m.polygon.size() >= 3) {
        inside = false;
        distance = std::numeric_limits<double>::max();
        for (size_t i = 0, j = m.polygon.size() - 1; i < m.polygon.size(); j = i++) {
            auto a = m.polygon[j], b = m.polygon[i];
            if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x)
                inside = !inside;
            auto ab = sub(b, a), ap = sub(p, a);
            double len = ab.x * ab.x + ab.y * ab.y;
            double t = len > 0 ? std::clamp((ab.x * ap.x + ab.y * ap.y) / len, 0., 1.) : 0;
            auto delta = sub(p, add(a, mul(ab, t)));
            distance = std::min(distance, std::hypot(delta.x, delta.y));
        }
    }
    double a = inside ? 1. : 0.;
    if (m.feather > 0 && m.polygon.size() >= 3) {
        double t = std::clamp(.5 + (inside ? distance : -distance) / m.feather, 0., 1.);
        a = t * t * (3 - 2 * t);
    }
    return m.inverted ? 1 - a : a;
}
double curve_value(const std::vector<Vec2>& c, double t) {
    t = std::clamp(t, 0., 1.);
    if (c.empty())
        return t;
    if (t <= c.front().x)
        return c.front().y;
    for (size_t i = 1; i < c.size(); ++i)
        if (t <= c[i].x) {
            double d = c[i].x - c[i - 1].x;
            return d > 0 ? std::lerp(c[i - 1].y, c[i].y, (t - c[i - 1].x) / d) : c[i].y;
        }
    return c.back().y;
}
double blend_weight(const Blend& b, Vec2 p) {
    double w = 1;
    auto edge = [&](double d, double width) {
        if (width > 0)
            w *= curve_value(b.curve, d / width);
    };
    edge(p.x, b.left);
    edge(1 - p.x, b.right);
    edge(p.y, b.top);
    edge(1 - p.y, b.bottom);
    return std::pow(std::clamp(w, 0., 1.), b.gamma);
}
} // namespace ps
