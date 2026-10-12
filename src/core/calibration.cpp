// SPDX-License-Identifier: GPL-3.0-only
#include "ps/calibration.hpp"
#include "ps/geometry.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <numeric>
#include <random>
#include <stdexcept>
namespace ps {
static int bits(int n) {
    int b = 0;
    while ((1 << b) < n)
        ++b;
    return b;
}
std::vector<GrayFrame> gray_patterns(int width, int height) {
    if (width < 2 || height < 2 || width > 8192 || height > 8192)
        throw std::invalid_argument("Pattern dimensions must be 2..8192");
    std::vector<GrayFrame> out;
    auto frame = [&](int axis, int bit, int invert) {
        GrayFrame f{width, height, std::vector<std::uint8_t>(size_t(width) * height)};
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x) {
                int q = axis ? y : x;
                int g = q ^ (q >> 1);
                f.pixels[size_t(y) * width + x] =
                    std::uint8_t(axis == 2   ? 255
                                 : axis == 3 ? 0
                                             : (((g >> bit) & 1) ^ invert) * 255);
            }
        out.push_back(std::move(f));
    };
    frame(2, 0, 0);
    frame(3, 0, 0);
    for (int axis = 0; axis < 2; ++axis)
        for (int bit = bits(axis ? height : width) - 1; bit >= 0; --bit) {
            frame(axis, bit, 0);
            frame(axis, bit, 1);
        }
    return out;
}
std::vector<Correspondence> decode_gray(const std::vector<GrayFrame>& frames, int pw, int ph, int threshold) {
    if (pw < 2 || ph < 2 || pw > 8192 || ph > 8192)
        throw std::invalid_argument("Invalid projector resolution");
    if (frames.size() != size_t(2 + 2 * (bits(pw) + bits(ph))))
        throw std::invalid_argument("Incorrect number of captured patterns");
    int w = frames[0].width, h = frames[0].height;
    if (w <= 0 || h <= 0)
        throw std::invalid_argument("Empty capture");
    for (auto& f : frames)
        if (f.width != w || f.height != h || f.pixels.size() != size_t(w) * h)
            throw std::invalid_argument("Capture dimensions differ");
    std::vector<Correspondence> out;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            size_t k = size_t(y) * w + x;
            int contrast = int(frames[0].pixels[k]) - int(frames[1].pixels[k]);
            if (contrast < threshold)
                continue;
            int index = 2, value[2]{};
            int conf = contrast;
            bool valid = true;
            for (int axis = 0; axis < 2; ++axis) {
                int g = 0;
                for (int b = bits(axis ? ph : pw) - 1; b >= 0; --b) {
                    int diff = int(frames[index].pixels[k]) - int(frames[index + 1].pixels[k]);
                    index += 2;
                    conf = std::min(conf, std::abs(diff));
                    if (std::abs(diff) < threshold)
                        valid = false;
                    if (diff > 0)
                        g |= 1 << b;
                }
                int binary = g;
                for (int n = g >> 1; n; n >>= 1)
                    binary ^= n;
                value[axis] = binary;
            }
            if (valid && value[0] < pw && value[1] < ph)
                out.push_back({{double(x), double(y)}, {double(value[0]), double(value[1])}, conf / 255.});
        }
    return out;
}
Vec2 transform(const Homography& h, Vec2 p) {
    auto& m = h.matrix;
    double d = m[6] * p.x + m[7] * p.y + m[8];
    if (std::abs(d) < 1e-12)
        throw std::runtime_error("Singular homography");
    return {(m[0] * p.x + m[1] * p.y + m[2]) / d, (m[3] * p.x + m[4] * p.y + m[5]) / d};
}
static Homography least_squares(std::span<const Correspondence> p) {
    // Normalize coordinates before solving to avoid pixel-coordinate conditioning problems.
    Vec2 src{}, dst{};
    for (auto& q : p) {
        src.x += q.projector.x;
        src.y += q.projector.y;
        dst.x += q.camera.x;
        dst.y += q.camera.y;
    }
    src.x /= p.size();
    src.y /= p.size();
    dst.x /= p.size();
    dst.y /= p.size();
    double ss = 0, ds = 0;
    for (auto& q : p) {
        ss += std::hypot(q.projector.x - src.x, q.projector.y - src.y);
        ds += std::hypot(q.camera.x - dst.x, q.camera.y - dst.y);
    }
    if (ss < 1e-8 || ds < 1e-8)
        throw std::runtime_error("Degenerate calibration points");
    ss = p.size() / ss;
    ds = p.size() / ds;
    double a[8][9]{};
    auto accumulate = [&](const std::array<double, 8>& row, double target, double weight) {
        for (int i = 0; i < 8; ++i) {
            for (int j = 0; j < 8; ++j)
                a[i][j] += weight * row[i] * row[j];
            a[i][8] += weight * row[i] * target;
        }
    };
    for (auto& q : p) {
        double x = (q.projector.x - src.x) * ss, y = (q.projector.y - src.y) * ss,
               u = (q.camera.x - dst.x) * ds, v = (q.camera.y - dst.y) * ds;
        accumulate({x, y, 1, 0, 0, 0, -u * x, -u * y}, u, std::max(.01, q.confidence));
        accumulate({0, 0, 0, x, y, 1, -v * x, -v * y}, v, std::max(.01, q.confidence));
    }
    for (int i = 0; i < 8; ++i) {
        int best = i;
        for (int k = i + 1; k < 8; ++k)
            if (std::abs(a[k][i]) > std::abs(a[best][i]))
                best = k;
        if (std::abs(a[best][i]) < 1e-10)
            throw std::runtime_error("Calibration points are collinear or degenerate");
        for (int j = 0; j < 9; ++j)
            std::swap(a[i][j], a[best][j]);
        double d = a[i][i];
        for (int j = i; j < 9; ++j)
            a[i][j] /= d;
        for (int k = 0; k < 8; ++k)
            if (k != i) {
                double f = a[k][i];
                for (int j = i; j < 9; ++j)
                    a[k][j] -= f * a[i][j];
            }
    }
    std::array<double, 9> n{};
    for (int i = 0; i < 8; ++i)
        n[i] = a[i][8];
    n[8] = 1;
    auto product = [](const auto& x, const auto& y) {
        std::array<double, 9> r{};
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                for (int k = 0; k < 3; ++k)
                    r[i * 3 + j] += x[i * 3 + k] * y[k * 3 + j];
        return r;
    };
    auto m = product(std::array<double, 9>{1 / ds, 0, dst.x, 0, 1 / ds, dst.y, 0, 0, 1},
                     product(n, std::array<double, 9>{ss, 0, -src.x * ss, 0, ss, -src.y * ss, 0, 0, 1}));
    Homography h;
    h.matrix = m;
    return h;
}
Homography fit_homography(std::span<const Correspondence> p, double threshold) {
    if (p.size() < 4 || threshold <= 0)
        throw std::invalid_argument("At least four calibration correspondences are required");
    std::mt19937 rng(42);
    std::uniform_int_distribution<size_t> d(0, p.size() - 1);
    std::vector<Correspondence> best;
    for (int iteration = 0; iteration < (p.size() == 4 ? 1 : 256); ++iteration) {
        std::array<Correspondence, 4> sample;
        std::array<size_t, 4> chosen{};
        for (int k = 0; k < 4; ++k) {
            size_t index;
            bool repeated;
            do {
                index = p.size() == 4 ? size_t(k) : d(rng);
                repeated = false;
                for (int j = 0; j < k; ++j)
                    repeated |= chosen[j] == index;
            } while (repeated);
            chosen[k] = index;
            sample[k] = p[index];
        }
        try {
            auto h = least_squares(sample);
            std::vector<Correspondence> in;
            for (auto& q : p) {
                auto t = transform(h, q.projector);
                if (std::hypot(t.x - q.camera.x, t.y - q.camera.y) <= threshold)
                    in.push_back(q);
            }
            if (in.size() > best.size())
                best = std::move(in);
        } catch (const std::runtime_error&) {
        }
    }
    if (best.size() < 4)
        throw std::runtime_error("Could not fit a reliable planar mapping");
    auto h = least_squares(best);
    h.inliers = best.size();
    double sum = 0;
    for (auto& q : best) {
        auto t = transform(h, q.projector);
        double error = std::hypot(t.x - q.camera.x, t.y - q.camera.y);
        sum += error * error;
        h.max_error = std::max(h.max_error, error);
    }
    h.rms = std::sqrt(sum / best.size());
    return h;
}
Mesh calibrated_mesh(const Homography& h, int w, int height, Rect bounds, int cols, int rows) {
    if (w < 2 || height < 2 || bounds.w <= 0 || bounds.h <= 0)
        throw std::invalid_argument("Invalid calibration bounds");
    // Desired source UV is a camera-plane rectangle. Invert H to obtain destination projector UV.
    auto& a = h.matrix;
    double det = a[0] * (a[4] * a[8] - a[5] * a[7]) - a[1] * (a[3] * a[8] - a[5] * a[6]) +
                 a[2] * (a[3] * a[7] - a[4] * a[6]);
    if (std::abs(det) < 1e-12)
        throw std::runtime_error("Singular calibration");
    Homography inv;
    inv.matrix = {a[4] * a[8] - a[5] * a[7], a[2] * a[7] - a[1] * a[8], a[1] * a[5] - a[2] * a[4],
                  a[5] * a[6] - a[3] * a[8], a[0] * a[8] - a[2] * a[6], a[2] * a[3] - a[0] * a[5],
                  a[3] * a[7] - a[4] * a[6], a[1] * a[6] - a[0] * a[7], a[0] * a[4] - a[1] * a[3]};
    for (auto& v : inv.matrix)
        v /= det;
    auto m = identity_mesh(cols, rows);
    for (auto& uv : m.points) {
        auto q = transform(inv, {bounds.x + uv.x * bounds.w, bounds.y + uv.y * bounds.h});
        uv = {q.x / (w - 1), q.y / (height - 1)};
    }
    return m;
}
void write_pgm(const GrayFrame& f, const std::filesystem::path& path) {
    std::ofstream out(path, std::ios::binary);
    out.exceptions(std::ios::badbit | std::ios::failbit);
    out << "P5\n" << f.width << ' ' << f.height << "\n255\n";
    out.write(reinterpret_cast<const char*>(f.pixels.data()), std::streamsize(f.pixels.size()));
}
GrayFrame read_pgm(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::string magic;
    int w = 0, h = 0, max = 0;
    in >> magic >> w >> h >> max;
    if (magic != "P5" || max != 255 || w < 1 || h < 1 || w > 8192 || h > 8192)
        throw std::runtime_error("Expected binary 8-bit PGM");
    in.get();
    GrayFrame f{w, h, std::vector<std::uint8_t>(size_t(w) * h)};
    in.read(reinterpret_cast<char*>(f.pixels.data()), std::streamsize(f.pixels.size()));
    if (!in)
        throw std::runtime_error("Truncated PGM");
    return f;
}
Json photometric_fit(std::span<const double> input, std::span<const double> measured) {
    if (input.size() != measured.size() || input.size() < 3)
        throw std::invalid_argument("At least three matching measurements required");
    double black = *std::min_element(measured.begin(), measured.end()),
           white = *std::max_element(measured.begin(), measured.end());
    if (white - black <= 1e-6)
        throw std::runtime_error("Insufficient measured contrast");
    double xx = 0, xy = 0;
    for (size_t i = 0; i < input.size(); ++i) {
        double y = (measured[i] - black) / (white - black);
        if (input[i] > 0 && input[i] < 1 && y > 0 && y < 1) {
            double x = std::log(input[i]);
            xx += x * x;
            xy += x * std::log(y);
        }
    }
    if (xx == 0)
        throw std::runtime_error("Need a valid mid-gray measurement");
    return {{"black", black}, {"white", white}, {"gamma", xy / xx}};
}
} // namespace ps
