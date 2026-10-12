// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "model.hpp"
namespace ps {
double srgb_to_linear(double);
double linear_to_srgb(double);
Color correct_color(Color linear, const ColorSettings&, double weight, double black_level);
Color test_pattern(const std::string&, double u, double v, double time = 0);
} // namespace ps
