// SPDX-License-Identifier: GPL-3.0-only
#include "ps/color.hpp"
#include <algorithm>
#include <cmath>
namespace ps {
double srgb_to_linear(double x) {
    return x <= .04045 ? x / 12.92 : std::pow((x + .055) / 1.055, 2.4);
}
double linear_to_srgb(double x) {
    return x <= .0031308 ? 12.92 * x : 1.055 * std::pow(x, 1 / 2.4) - .055;
}
Color correct_color(Color c, const ColorSettings& s, double weight, double black) {
    double lum = .2126 * c.r + .7152 * c.g + .0722 * c.b;
    double channels[3] = {c.r, c.g, c.b};
    for (int i = 0; i < 3; ++i) {
        double v = lum + (channels[i] - lum) * s.saturation;
        v = ((v - .5) * s.contrast + .5 + s.brightness) * s.gain[i] + s.offset[i];
        // Raise the non-overlap black floor; overlap black leakage already adds optically.
        channels[i] =
            std::pow(std::clamp(v * weight + black * std::max(0., 2 * weight - 1), 0., 1.), 1 / s.gamma);
    }
    return {channels[0], channels[1], channels[2], c.a};
}
Color test_pattern(const std::string& pattern, double u, double v, double time) {
    if (pattern == "white")
        return {1, 1, 1, 1};
    if (pattern == "black")
        return {0, 0, 0, 1};
    if (pattern == "red")
        return {1, 0, 0, 1};
    if (pattern == "green")
        return {0, 1, 0, 1};
    if (pattern == "blue")
        return {0, 0, 1, 1};
    if (pattern == "gradient")
        return {u, u, u, 1};
    if (pattern == "checker") {
        double k = (int(u * 16) + int(v * 9)) % 2 ? .85 : .05;
        return {k, k, k, 1};
    }
    if (pattern == "motion") {
        double x = std::fmod(time * .2, 1.);
        return std::abs(u - x) < .035 ? Color{1, .4, .1, 1} : Color{.02, .025, .03, 1};
    }
    double gx = std::abs(u * 16 - std::round(u * 16)), gy = std::abs(v * 9 - std::round(v * 9));
    if (std::abs(u - .5) < .002 || std::abs(v - .5) < .003)
        return {1, .38, .12, 1};
    if (gx < .02 || gy < .02)
        return {.4, .65, .8, 1};
    if (u < .003 || u > .997 || v < .004 || v > .996)
        return {1, 1, 1, 1};
    return {.045, .065, .09, 1};
}
} // namespace ps
