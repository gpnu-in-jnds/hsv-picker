#include "hsv_picker.hpp"

#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace hsvpicker {
namespace {
const cv::Scalar background(25, 23, 21);
const cv::Scalar panel(38, 35, 32);
const cv::Scalar ink(240, 237, 231);
const cv::Scalar muted(168, 164, 157);
const cv::Scalar accent(126, 220, 108);
const cv::Rect viewport(16, 88, 720, 570);

void text(cv::Mat& canvas, const std::string& value, int x, int y,
          double scale = 0.48, cv::Scalar color = ink) {
    cv::putText(canvas, value, cv::Point(x, y), cv::FONT_HERSHEY_SIMPLEX,
                scale, color, 1, cv::LINE_AA);
}

void histogram(cv::Mat& canvas, const HsvStatistics* stats, int channel, int top) {
    const int left = 766;
    const int width = 390;
    const int height = 74;
    const int bottom = top + 100;
    const char* names[] = {"H / Hue", "S / Saturation", "V / Value"};
    const int bins = channel == 0 ? 180 : 256;
    const cv::Scalar colors[] = {cv::Scalar(110, 185, 230),
                                 cv::Scalar(215, 160, 110),
                                 cv::Scalar(130, 220, 140)};
    text(canvas, names[channel], left, top + 14, 0.48, colors[channel]);
    std::uint64_t peak = 0;
    if (stats) {
        const auto& counts = stats->histograms[channel];
        peak = *std::max_element(counts.begin(), counts.end());
        for (int bin = 0; bin < bins; ++bin) {
            if (counts[bin] == 0) {
                continue;
            }
            const int barHeight = std::max(1, static_cast<int>(std::lround(
                static_cast<double>(counts[bin]) / static_cast<double>(peak) * height)));
            const int x1 = left + bin * width / bins;
            const int x2 = left + (bin + 1) * width / bins - 1;
            cv::Scalar color = colors[channel];
            if (channel == 0) {
                cv::Mat hsv(1, 1, CV_8UC3, cv::Scalar(bin, 220, 240));
                cv::Mat bgr;
                cv::cvtColor(hsv, bgr, cv::COLOR_HSV2BGR);
                const auto pixel = bgr.at<cv::Vec3b>(0, 0);
                color = cv::Scalar(pixel[0], pixel[1], pixel[2]);
            }
            cv::rectangle(canvas, cv::Point(x1, bottom - barHeight),
                          cv::Point(x2, bottom - 1), color, cv::FILLED);
        }
    }
    text(canvas, "peak count: " + std::to_string(peak), left + 188, top + 14, 0.40, muted);
    cv::line(canvas, {left, bottom}, {left + width, bottom}, muted);
    text(canvas, "0", left, bottom + 17, 0.40, muted);
    text(canvas, std::to_string((bins - 1) / 2), left + width / 2 - 12,
         bottom + 17, 0.40, muted);
    text(canvas, std::to_string(bins - 1), left + width - 26,
         bottom + 17, 0.40, muted);
}
} // namespace

Options parseOptions(const std::vector<std::string>& arguments) {
    Options options;
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const auto& arg = arguments[i];
        if (arg == "--help" || arg == "-h") {
            options.help = true;
        } else if (arg == "--source") {
            if (i + 1 == arguments.size() || arguments[i + 1].empty() ||
                arguments[i + 1].rfind("--", 0) == 0) {
                throw std::invalid_argument("--source requires a device or image path");
            }
            options.source = arguments[++i];
        } else if (arg.rfind("--source=", 0) == 0) {
            options.source = arg.substr(9);
            if (options.source.empty()) {
                throw std::invalid_argument("--source must not be empty");
            }
        } else {
            throw std::invalid_argument("Unknown argument: " + arg);
        }
    }
    return options;
}

bool isCameraSource(const std::string& source) {
    return source.rfind("/dev/video", 0) == 0 || source.rfind("/dev/v4l/", 0) == 0;
}

HsvStatistics analyze(const cv::Mat& bgr, const cv::Rect& roi) {
    if (bgr.empty() || bgr.type() != CV_8UC3) {
        throw std::invalid_argument("HSV analysis requires a nonempty 8-bit BGR image");
    }
    if (roi.empty() || (roi & cv::Rect(0, 0, bgr.cols, bgr.rows)) != roi) {
        throw std::invalid_argument("Selection is empty or outside the image");
    }
    cv::Mat hsv;
    cv::cvtColor(bgr(roi), hsv, cv::COLOR_BGR2HSV);
    HsvStatistics result;
    result.range = {179, 0, 255, 0, 255, 0};
    result.histograms = {std::vector<std::uint64_t>(180),
                         std::vector<std::uint64_t>(256),
                         std::vector<std::uint64_t>(256)};
    result.pixels = static_cast<std::uint64_t>(roi.width) * roi.height;
    for (int y = 0; y < hsv.rows; ++y) {
        const auto* row = hsv.ptr<cv::Vec3b>(y);
        for (int x = 0; x < hsv.cols; ++x) {
            for (int channel = 0; channel < 3; ++channel) {
                const int value = row[x][channel];
                ++result.histograms[channel][value];
                result.range[channel * 2] = std::min(result.range[channel * 2], value);
                result.range[channel * 2 + 1] = std::max(result.range[channel * 2 + 1], value);
            }
        }
    }
    return result;
}

std::string rangeText(const HsvStatistics& statistics) {
    std::string result = "[";
    for (std::size_t i = 0; i < statistics.range.size(); ++i) {
        result += (i == 0 ? "" : ", ") + std::to_string(statistics.range[i]);
    }
    return result + "]";
}

std::string scalarText(const HsvStatistics& statistics, bool upper) {
    const auto& r = statistics.range;
    const int offset = upper ? 1 : 0;
    return "cv::Scalar(" + std::to_string(r[offset]) + ", " +
           std::to_string(r[offset + 2]) + ", " + std::to_string(r[offset + 4]) + ")";
}

cv::Rect fitImage(cv::Size image, const cv::Rect& bounds) {
    if (image.empty() || bounds.empty()) {
        throw std::invalid_argument("Image and viewport sizes must be positive");
    }
    const double scale = std::min(static_cast<double>(bounds.width) / image.width,
                                  static_cast<double>(bounds.height) / image.height);
    const int width = std::clamp(static_cast<int>(std::lround(image.width * scale)), 1, bounds.width);
    const int height = std::clamp(static_cast<int>(std::lround(image.height * scale)), 1, bounds.height);
    return {bounds.x + (bounds.width - width) / 2,
            bounds.y + (bounds.height - height) / 2, width, height};
}

cv::Rect sourceSelection(cv::Point first, cv::Point last,
                         const cv::Rect& display, cv::Size source) {
    if (display.empty() || source.empty()) {
        throw std::invalid_argument("Image and display sizes must be positive");
    }
    // Include both mouse endpoints, and all source pixels covered by display pixels.
    // Integer arithmetic avoids floating-point errors at full-image boundaries.
    const auto axis = [](int a, int b, int origin, int extent, int sourceExtent) {
        a = std::clamp(a - origin, 0, extent - 1);
        b = std::clamp(b - origin, 0, extent - 1);
        const int start = static_cast<int>(static_cast<std::int64_t>(std::min(a, b)) * sourceExtent / extent);
        const auto endNumerator = static_cast<std::int64_t>(std::max(a, b) + 1) * sourceExtent;
        const int end = static_cast<int>((endNumerator + extent - 1) / extent);
        return std::pair<int, int>{start, end};
    };
    const auto horizontal = axis(first.x, last.x, display.x, display.width, source.width);
    const auto vertical = axis(first.y, last.y, display.y, display.height, source.height);
    return {horizontal.first, vertical.first, horizontal.second - horizontal.first,
            vertical.second - vertical.first};
}

Picker::Picker(bool camera) : camera_(camera) {}

void Picker::setFrame(const cv::Mat& bgr) {
    if (bgr.empty() || bgr.type() != CV_8UC3) {
        throw std::invalid_argument("Source must provide a nonempty 8-bit BGR image");
    }
    if (bgr.size() != frame_.size()) {
        selection_ = {};
        statistics_.reset();
        dragging_ = false;
    }
    frame_ = bgr.clone();
    imageRect_ = fitImage(frame_.size(), viewport);
    refreshStatistics();
}

void Picker::refreshStatistics() {
    if (selection_.empty()) {
        statistics_.reset();
    } else {
        statistics_ = analyze(frame_, selection_);
    }
}

void Picker::selectTo(cv::Point point) {
    selection_ = sourceSelection(anchor_, point, imageRect_, frame_.size());
    refreshStatistics();
}

void Picker::onMouse(int event, int x, int y, int /*flags*/) {
    if (frame_.empty()) {
        return;
    }
    const cv::Point point(x, y);
    if (event == cv::EVENT_LBUTTONDOWN && imageRect_.contains(point)) {
        anchor_ = point;
        dragging_ = true;
        selectTo(point);
    } else if (dragging_ && event == cv::EVENT_LBUTTONUP) {
        selectTo(point);
        dragging_ = false;
        reportRequested_ = true;
    } else if (dragging_ && event == cv::EVENT_MOUSEMOVE) {
        // Keep dragging until an explicit button-up or R cancellation.
        // Motion-event button flags are not a reliable release notification.
        selectTo(point);
    }
}

bool Picker::onKey(int key) {
    if (key == 27 || key == 'q' || key == 'Q') {
        return false;
    }
    if (key == ' ' && camera_ && !dragging_) {
        paused_ = !paused_;
    } else if (key == 'r' || key == 'R') {
        dragging_ = false;
        paused_ = false;
        selection_ = {};
        statistics_.reset();
        reportRequested_ = false;
    } else if ((key == 'p' || key == 'P') && statistics_) {
        reportRequested_ = true;
    }
    return true;
}

bool Picker::takeReportRequest() {
    const bool result = reportRequested_;
    reportRequested_ = false;
    return result;
}

cv::Mat Picker::render() const {
    cv::Mat canvas(canvasHeight, canvasWidth, CV_8UC3, background);
    text(canvas, "HSV PICKER", 16, 32, 0.82, accent);
    const std::string status = camera_ ? (frozen() ? "CAMERA / FROZEN" : "CAMERA / LIVE") : "IMAGE";
    text(canvas, status, 560, 32, 0.48, accent);
    text(canvas, "Drag left mouse to select. Values use OpenCV 8-bit HSV.", 16, 61, 0.48, muted);
    cv::rectangle(canvas, viewport, panel, cv::FILLED);
    cv::rectangle(canvas, cv::Rect(750, 88, 414, 570), panel, cv::FILLED);

    if (!frame_.empty()) {
        cv::Mat resized;
        cv::resize(frame_, resized, imageRect_.size(), 0, 0,
                   imageRect_.width < frame_.cols ? cv::INTER_AREA : cv::INTER_NEAREST);
        resized.copyTo(canvas(imageRect_));
        if (!selection_.empty()) {
            const int x1 = imageRect_.x + static_cast<int>(
                static_cast<std::int64_t>(selection_.x) * imageRect_.width / frame_.cols);
            const int y1 = imageRect_.y + static_cast<int>(
                static_cast<std::int64_t>(selection_.y) * imageRect_.height / frame_.rows);
            const int x2 = imageRect_.x + static_cast<int>((
                static_cast<std::int64_t>(selection_.x + selection_.width) * imageRect_.width
                + frame_.cols - 1) / frame_.cols) - 1;
            const int y2 = imageRect_.y + static_cast<int>((
                static_cast<std::int64_t>(selection_.y + selection_.height) * imageRect_.height
                + frame_.rows - 1) / frame_.rows) - 1;
            cv::rectangle(canvas, {x1, y1}, {x2, y2}, cv::Scalar(0, 0, 0), 3);
            cv::rectangle(canvas, {x1, y1}, {x2, y2}, accent, 1);
        }
    }

    text(canvas, "[hbegin,hend,sbegin,send,vbegin,vend]", 766, 112, 0.43, muted);
    const auto* stats = statistics_ ? &*statistics_ : nullptr;
    text(canvas, stats ? rangeText(*stats) : "Select an image region", 766, 141, 0.64, accent);
    text(canvas, "Inclusive min / max. H: 0..179, S/V: 0..255", 766, 164, 0.40, muted);
    text(canvas, stats ? "lower = " + scalarText(*stats, false) : "lower = --", 766, 190, 0.46);
    text(canvas, stats ? "upper = " + scalarText(*stats, true) : "upper = --", 766, 214, 0.46);
    text(canvas, "Histograms: x = HSV value, y = pixel count", 766, 244, 0.41, muted);
    for (int channel = 0; channel < 3; ++channel) {
        histogram(canvas, stats, channel, 259 + channel * 127);
    }

    if (stats) {
        text(canvas, "ROI: x=" + std::to_string(selection_.x) + " y=" + std::to_string(selection_.y) +
             " w=" + std::to_string(selection_.width) + " h=" + std::to_string(selection_.height) +
             " | pixels=" + std::to_string(stats->pixels), 16, 681, 0.45, muted);
    } else {
        text(canvas, "No selection | Statistics are computed from original pixels.", 16, 681, 0.45, muted);
    }
    text(canvas, "Space: freeze/resume camera   R: clear/resume   P: print bounds   Q / Esc: quit",
         16, 706, 0.46, muted);
    return canvas;
}

} // namespace hsvpicker
