// SPDX-License-Identifier: GPL-3.0-only
#include "ps/scan.hpp"
#include <QApplication>
#include <QDateTime>
#include <QImage>
#include <QKeyEvent>
#include <QPainter>
#include <QScreen>
#include <QTimer>
#include <QWindow>
#include <QWidget>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <memory>
#include <set>
#ifdef PS_WITH_OPENCV
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>
#endif
using ps::Json;
namespace {
class PatternWindow : public QWidget {
  public:
    QImage pattern;
    std::function<void()> cancel;
    PatternWindow() {
        setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
        setCursor(Qt::BlankCursor);
        setAttribute(Qt::WA_OpaquePaintEvent);
    }
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), Qt::black);
        p.setRenderHint(QPainter::SmoothPixmapTransform, false);
        if (!pattern.isNull())
            p.drawImage(rect(), pattern);
    }
    void keyPressEvent(QKeyEvent* e) override {
        if (e->key() == Qt::Key_Escape || e->key() == Qt::Key_B)
            cancel();
    }
    void closeEvent(QCloseEvent*) override {
        cancel();
    }
};
QImage image(const ps::GrayFrame& f) {
    return QImage(f.pixels.data(), f.width, f.height, f.width, QImage::Format_Grayscale8).copy();
}
#ifdef PS_WITH_OPENCV
class Camera {
    cv::VideoCapture cap;
    int width{}, height{};

  public:
    Json info;
    Camera(const Json& settings) {
        int index = settings.at("index");
        if (index < 0 || index > 31)
            throw std::runtime_error("Camera index must be 0..31");
        if (!cap.open(index, cv::CAP_DSHOW))
            throw std::runtime_error(
                "Cannot open DirectShow camera; check index, privacy settings and other camera applications");
        cap.set(cv::CAP_PROP_FRAME_WIDTH, settings.at("width").get<int>());
        cap.set(cv::CAP_PROP_FRAME_HEIGHT, settings.at("height").get<int>());
        bool manual = cap.set(cv::CAP_PROP_AUTO_EXPOSURE, 0);
        bool exposure = cap.set(cv::CAP_PROP_EXPOSURE, settings.at("exposure").get<double>());
        bool focus = cap.set(cv::CAP_PROP_AUTOFOCUS, 0), wb = cap.set(cv::CAP_PROP_AUTO_WB, 0);
        info = {{"backend", cap.getBackendName()},
                {"manual_exposure_request", manual},
                {"exposure_request", exposure},
                {"auto_focus_off_request", focus},
                {"auto_white_balance_off_request", wb},
                {"exposure_readback", cap.get(cv::CAP_PROP_EXPOSURE)},
                {"control_note", "Driver acknowledgements do not prove exposure/focus/white balance lock; "
                                 "verify camera controls"}};
    }
    ps::RgbFrame read(int flush) {
        cv::Mat raw;
        for (int i = 0; i < flush; ++i) {
            if (!cap.read(raw) || raw.empty())
                throw std::runtime_error("Camera stopped delivering frames");
        }
        if (raw.depth() != CV_8U)
            throw std::runtime_error("Camera must deliver 8-bit frames");
        cv::Mat rgb;
        if (raw.channels() == 3)
            cv::cvtColor(raw, rgb, cv::COLOR_BGR2RGB);
        else if (raw.channels() == 4)
            cv::cvtColor(raw, rgb, cv::COLOR_BGRA2RGB);
        else if (raw.channels() == 1)
            cv::cvtColor(raw, rgb, cv::COLOR_GRAY2RGB);
        else
            throw std::runtime_error("Unsupported camera pixel format");
        if (width && (width != rgb.cols || height != rgb.rows))
            throw std::runtime_error("Camera dimensions changed during scan");
        width = rgb.cols;
        height = rgb.rows;
        if (width < 2 || height < 2 || width > 8192 || height > 8192)
            throw std::runtime_error("Invalid camera resolution");
        info["actual_width"] = width;
        info["actual_height"] = height;
        if (!rgb.isContinuous())
            rgb = rgb.clone();
        return {width, height, std::vector<std::uint8_t>(rgb.data, rgb.data + rgb.total() * 3)};
    }
};
#endif
ps::GrayFrame luminance(const ps::RgbFrame& rgb) {
    ps::GrayFrame f{rgb.width, rgb.height, std::vector<std::uint8_t>(size_t(rgb.width) * rgb.height)};
    for (size_t k = 0; k < f.pixels.size(); ++k)
        f.pixels[k] = std::uint8_t(std::lround(.299 * rgb.pixels[k * 3] + .587 * rgb.pixels[k * 3 + 1] +
                                               .114 * rgb.pixels[k * 3 + 2]));
    return f;
}
} // namespace
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    auto args = app.arguments();
    auto arg = [&](const QString& key, const QString& fallback = QString{}) {
        int i = args.indexOf(key);
        return i >= 0 && i + 1 < args.size() ? args[i + 1] : fallback;
    };
    if (args.contains("--help") || args.size() == 1) {
        std::cout << "projection-scan --scan <folder> [--simulate]\n"
                     "projection-scan --analyze <folder>\n"
                     "projection-scan --probe <folder> --camera 0 --width 1280 --height 720 --exposure -6\n";
        return 0;
    }
    std::filesystem::path path(arg("--scan").toStdWString());
    Json manifest;
    bool owns_session = false, stopped = false;
    std::vector<std::unique_ptr<PatternWindow>> windows;
    auto record = [&] { ps::atomic_write(path / "session.json", manifest.dump(2)); };
    auto finish = [&](const std::string& status, const std::string& error, int code) {
        stopped = true;
        for (auto& w : windows) {
            w->pattern = {};
            w->repaint();
            w->hide();
        }
        if (owns_session) {
            manifest["status"] = status;
            manifest["error"] = error;
            record();
        }
        std::cout << Json{{"status", status}, {"error", error}}.dump() << std::endl;
        app.exit(code);
    };
    try {
        if (args.contains("--check-pattern-paint")) {
            PatternWindow window;
            window.cancel = [] {};
            double ratio = window.devicePixelRatioF();
            window.resize(qRound(64 / ratio), qRound(48 / ratio));
            for (int i = 0; i < ps::gray_pattern_count(64, 48); ++i) {
                auto pattern = ps::gray_pattern(64, 48, i);
                window.pattern = image(pattern);
                auto painted = window.grab().toImage();
                if (painted.size() != QSize(64, 48))
                    throw std::runtime_error("Pattern paint dimensions differ at current DPI");
                for (int y = 0; y < 48; ++y)
                    for (int x = 0; x < 64; ++x)
                        if (qRed(painted.pixel(x, y)) != pattern.pixels[size_t(y) * 64 + x])
                            throw std::runtime_error("Pattern repaint resampled a Gray-code pixel");
            }
            std::cout << "Native pixel pattern painting passed at DPR " << ratio << std::endl;
            return 0;
        }
        if (args.contains("--analyze")) {
            auto result = ps::analyze_scan(std::filesystem::path(arg("--analyze").toStdWString()));
            std::cout << Json{{"status", "analyzed"}, {"projectors", result.at("targets").size()}}.dump()
                      << std::endl;
            return 0;
        }
#ifdef PS_WITH_OPENCV
        std::unique_ptr<Camera> camera;
        if (args.contains("--probe")) {
            auto dir = std::filesystem::path(arg("--probe").toStdWString());
            std::filesystem::create_directories(dir);
            Camera probe(Json{{"index", arg("--camera", "0").toInt()},
                              {"width", arg("--width", "1280").toInt()},
                              {"height", arg("--height", "720").toInt()},
                              {"exposure", arg("--exposure", "-6").toDouble()}});
            auto f = probe.read(8);
            ps::write_ppm(f, dir / "camera.ppm");
            ps::atomic_write(dir / "camera.json", probe.info.dump(2));
            std::cout << probe.info.dump() << std::endl;
            return 0;
        }
#else
        if (args.contains("--probe"))
            throw std::runtime_error("Live camera capture requires PS_WITH_OPENCV=ON");
#endif
        std::ifstream in(path / "session.json");
        in >> manifest;
        in.close();
        if (manifest.at("schema_version") != 1 || manifest.at("model") != "guided-planar-scan" ||
            manifest.at("status") != "pending")
            throw std::runtime_error("Only a new pending scan can be captured");
        if (manifest.at("targets").empty() || manifest.at("targets").size() > 64)
            throw std::runtime_error("Invalid target count");
        bool simulation = args.contains("--simulate");
        int settle = manifest.at("settle_ms"), flush = manifest.at("flush_frames");
        if (settle < 100 || settle > 10000 || flush < 1 || flush > 30)
            throw std::runtime_error("Invalid capture timing");
        std::set<QString> screens;
        size_t n = 0;
        for (const auto& t : manifest.at("targets")) {
            if (t.at("folder") != "projector-" + std::to_string(n++))
                throw std::runtime_error("Invalid target folder");
            int count = ps::gray_pattern_count(t.at("width"), t.at("height"));
            if (t.at("patterns").size() != size_t(count) || t.at("photometric").size() != 5 ||
                t.at("captured") != 0)
                throw std::runtime_error("Invalid capture sequence");
            auto expected = [](int i, const char* suffix) {
                std::ostringstream s;
                s << std::setw(3) << std::setfill('0') << i << suffix;
                return s.str();
            };
            for (int i = 0; i < count; ++i)
                if (t.at("patterns")[i] != expected(i, ".pgm"))
                    throw std::runtime_error("Invalid pattern filename");
            for (int i = 0; i < 5; ++i)
                if (t.at("photometric")[i] != expected(i, ".ppm"))
                    throw std::runtime_error("Invalid photometric filename");
            if (simulation)
                continue;
            auto name = QString::fromStdString(t.at("display"));
            QScreen* screen = nullptr;
            for (auto* s : app.screens())
                if (s->name() == name)
                    screen = s;
            if (!screen || !screens.insert(name).second)
                throw std::runtime_error("Assign a unique connected display to every projector");
            auto size = screen->geometry().size() * screen->devicePixelRatio();
            if (size.width() != t.at("width").get<int>() || size.height() != t.at("height").get<int>())
                throw std::runtime_error("Projector resolution differs from its native display mode; update "
                                         "setup before scanning");
            auto window = std::make_unique<PatternWindow>();
            window->cancel = [&] {
                if (!stopped)
                    finish("cancelled", "Escape/Blackout", 2);
            };
            window->winId();
            window->windowHandle()->setScreen(screen);
            window->setGeometry(screen->geometry());
            windows.push_back(std::move(window));
        }
        // Open the camera before showing any output. Store actual driver settings in the evidence.
#ifdef PS_WITH_OPENCV
        if (!simulation) {
            camera = std::make_unique<Camera>(manifest.at("camera"));
            camera->read(flush);
            manifest["camera_controls"] = camera->info;
        }
#else
        if (!simulation)
            throw std::runtime_error("Live camera capture requires PS_WITH_OPENCV=ON");
#endif
        owns_session = true;
        manifest["status"] = "capturing";
        manifest["synthetic"] = simulation;
        manifest["started_utc"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate).toStdString();
        record();
        for (auto& w : windows)
            w->showFullScreen();
        size_t target = 0;
        int frame = 0;
        auto* timer = new QTimer(&app);
        timer->setSingleShot(true);
        std::function<void()> show_next;
        show_next = [&] {
            if (stopped)
                return;
            if (std::filesystem::exists(path / "cancel.request")) {
                finish("cancelled", "Cancelled by editor", 2);
                return;
            }
            if (target >= manifest["targets"].size()) {
                for (auto& w : windows) {
                    w->pattern = {};
                    w->repaint();
                    w->hide();
                }
                manifest["status"] = "captured";
                record();
                try {
                    ps::analyze_scan(path);
                    finish("complete", "", 0);
                } catch (const std::exception& e) {
                    finish("analysis_failed", e.what(), 1);
                }
                return;
            }
            auto& t = manifest["targets"][target];
            int count = ps::gray_pattern_count(t["width"], t["height"]);
            if (!simulation) {
                for (auto& w : windows)
                    w->pattern = {};
                if (frame < count)
                    windows[target]->pattern = image(ps::gray_pattern(t["width"], t["height"], frame));
                else {
                    int level = std::array<int, 5>{0, 64, 128, 192, 255}[frame - count];
                    QImage flat(t["width"].get<int>(), t["height"].get<int>(), QImage::Format_RGB32);
                    flat.fill(QColor(level, level, level));
                    windows[target]->pattern = std::move(flat);
                }
                for (auto& w : windows)
                    w->repaint();
            }
            timer->start(simulation ? 1 : settle);
        };
        QObject::connect(timer, &QTimer::timeout, &app, [&] {
            if (stopped)
                return;
            try {
                if (std::filesystem::exists(path / "cancel.request")) {
                    finish("cancelled", "Cancelled by editor", 2);
                    return;
                }
                auto& t = manifest["targets"][target];
                int pw = t["width"], ph = t["height"], count = ps::gray_pattern_count(pw, ph);
                ps::RgbFrame capture;
                if (simulation) {
                    int w = manifest["camera"]["width"], h = manifest["camera"]["height"];
                    if (w < 2 || w > 8192 || h < 2 || h > 8192)
                        throw std::runtime_error("Invalid synthetic camera dimensions");
                    capture = {w, h, std::vector<std::uint8_t>(size_t(w) * h * 3, 12)};
                    auto pattern = frame < count ? ps::gray_pattern(pw, ph, frame) : ps::GrayFrame{};
                    double span = 1 + .75 * (manifest["targets"].size() - 1);
                    for (int y = 0; y < h; ++y)
                        for (int x = 0; x < w; ++x) {
                            int px = int(
                                std::lround(double(x) / (w - 1) * (pw - 1) * span - target * .75 * (pw - 1)));
                            int py = int(std::lround(double(y) / (h - 1) * (ph - 1)));
                            if (px < 0 || px >= pw)
                                continue;
                            double level = frame < count
                                               ? pattern.pixels[size_t(py) * pw + px] / 255.
                                               : std::array<double, 5>{0, 64. / 255, 128. / 255, 192. / 255,
                                                                       1}[frame - count];
                            for (int c = 0; c < 3; ++c)
                                capture.pixels[(size_t(y) * w + x) * 3 + c] =
                                    std::uint8_t(12 + level * (215 - target * 15 - c * 4));
                        }
                }
#ifdef PS_WITH_OPENCV
                else {
                    capture = camera->read(flush);
                    manifest["camera_controls"] = camera->info;
                }
#endif
                auto folder = path / t["folder"].get<std::string>();
                if (frame < count)
                    ps::write_pgm(luminance(capture), folder / t["patterns"][frame].get<std::string>());
                else
                    ps::write_ppm(capture, folder / t["photometric"][frame - count].get<std::string>());
                t["captured"] = frame + 1;
                record();
                std::cout << Json{{"status", "capturing"},
                                  {"projector", target + 1},
                                  {"frame", frame + 1},
                                  {"total", count + 5}}
                                 .dump()
                          << std::endl;
                if (++frame == count + 5) {
                    frame = 0;
                    ++target;
                }
                show_next();
            } catch (const std::exception& e) {
                finish("failed", e.what(), 1);
            }
        });
        QTimer cancel_poll;
        cancel_poll.start(50);
        QObject::connect(&cancel_poll, &QTimer::timeout, &app, [&] {
            if (!stopped && std::filesystem::exists(path / "cancel.request"))
                finish("cancelled", "Cancelled by editor", 2);
        });
        QTimer::singleShot(0, &app, show_next);
        return app.exec();
    } catch (const std::exception& e) {
        if (owns_session) {
            manifest["status"] = "failed";
            manifest["error"] = e.what();
            record();
        }
        std::cerr << e.what() << std::endl;
        return 1;
    }
}
