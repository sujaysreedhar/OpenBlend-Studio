// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "model.hpp"
#include <optional>
#include <span>
namespace ps {
struct Vertex {
    float x, y, u, v;
};
struct Triangles {
    std::vector<Vertex> vertices;
};
enum class SelectionMode { Point, Row, Column, All, Boundary };
enum class AxisLock { None, X, Y };
Vec2 evaluate_mesh(const Mesh&, double u, double v);
Mesh resample_mesh(const Mesh&, int columns, int rows);
Triangles triangulate(const Surface&, int subdivisions = 4);
std::optional<Vec2> inverse_map(const Mesh&, Vec2 destination);
std::vector<int> select_points(const Mesh&, int index, SelectionMode);
void move_points(Mesh&, std::span<const int>, Vec2 delta, AxisLock = AxisLock::None);
void interpolate_interior(Mesh&);
double mask_alpha(const Mask&, Vec2);
double blend_weight(const Blend&, Vec2);
double curve_value(const std::vector<Vec2>&, double t);
} // namespace ps
