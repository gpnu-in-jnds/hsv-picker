#include "hsv_picker.hpp"

#include <opencv2/highgui.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <functional>
#include <iostream>
#include <numeric>
#include <stdexcept>

namespace {
void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void requiresInvalidArgument(const std::function<void()>& action) {
    try {
        action();
    } catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error("Expected invalid_argument");
}

void testOptions() {
    require(hsvpicker::parseOptions({}).source == "/dev/video0", "Default camera path");
    require(hsvpicker::parseOptions({"--source", "/dev/video2"}).source == "/dev/video2", "Camera option");
    require(hsvpicker::parseOptions({"--source=some image.png"}).source == "some image.png", "Image option");
    require(hsvpicker::parseOptions({"--help"}).help, "Help option");
    require(hsvpicker::isCameraSource("/dev/video0"), "Video source detection");
    require(hsvpicker::isCameraSource("/dev/v4l/by-id/camera"), "Stable camera symlink detection");
    require(!hsvpicker::isCameraSource("photo.png"), "Image source detection");
    requiresInvalidArgument([] { hsvpicker::parseOptions({"--source"}); });
    requiresInvalidArgument([] { hsvpicker::parseOptions({"--source="}); });
    requiresInvalidArgument([] { hsvpicker::parseOptions({"--source", "--help"}); });
    requiresInvalidArgument([] { hsvpicker::parseOptions({"--unknown"}); });
}

void testStatistics() {
    // Analyze a non-contiguous ROI; the magenta border must not contribute.
    cv::Mat image(4, 5, CV_8UC3, cv::Scalar(255, 0, 255));
    const cv::Rect roi(1, 1, 3, 2);
    image.at<cv::Vec3b>(1, 1) = {0, 0, 255};   // red
    image.at<cv::Vec3b>(1, 2) = {0, 255, 0};   // green
    image.at<cv::Vec3b>(1, 3) = {255, 0, 0};   // blue
    image.at<cv::Vec3b>(2, 1) = {0, 0, 0};     // black
    image.at<cv::Vec3b>(2, 2) = {255, 255, 255}; // white
    image.at<cv::Vec3b>(2, 3) = {0, 0, 255};   // red again
    const auto stats = hsvpicker::analyze(image, roi);
    require(stats.range == std::array<int, 6>{0, 120, 0, 255, 0, 255}, "Raw OpenCV HSV extrema");
    require(stats.pixels == 6, "ROI pixel count");
    require(stats.histograms[0].size() == 180 && stats.histograms[1].size() == 256 &&
            stats.histograms[2].size() == 256, "Histogram bin counts");
    require(stats.histograms[0][0] == 4 && stats.histograms[0][60] == 1 &&
            stats.histograms[0][120] == 1 && stats.histograms[0][150] == 0, "Hue frequencies and ROI exclusion");
    require(stats.histograms[1][0] == 2 && stats.histograms[1][255] == 4, "Saturation frequencies");
    require(stats.histograms[2][0] == 1 && stats.histograms[2][255] == 5, "Value frequencies");
    for (const auto& bins : stats.histograms) {
        require(std::accumulate(bins.begin(), bins.end(), std::uint64_t{0}) == stats.pixels,
                "Every ROI pixel counted once in every channel");
    }
    require(hsvpicker::rangeText(stats) == "[0, 120, 0, 255, 0, 255]", "Six-value order");
    require(hsvpicker::scalarText(stats, false) == "cv::Scalar(0, 0, 0)", "Lower scalar order");
    require(hsvpicker::scalarText(stats, true) == "cv::Scalar(120, 255, 255)", "Upper scalar order");
    cv::Mat hsv, mask;
    cv::cvtColor(image(roi), hsv, cv::COLOR_BGR2HSV);
    const auto& r = stats.range;
    cv::inRange(hsv, cv::Scalar(r[0], r[2], r[4]), cv::Scalar(r[1], r[3], r[5]), mask);
    require(cv::countNonZero(mask) == 6, "Bounds are directly usable with inRange");

    const auto green = hsvpicker::analyze(image, cv::Rect(2, 1, 1, 1));
    require(green.range == std::array<int, 6>{60, 60, 255, 255, 255, 255}, "Single pixel has equal endpoints");
    cv::Mat reds(1, 2, CV_8UC3);
    reds.at<cv::Vec3b>(0, 0) = {0, 0, 255};
    reds.at<cv::Vec3b>(0, 1) = {8, 0, 255};
    const auto wrap = hsvpicker::analyze(reds, cv::Rect(0, 0, 2, 1));
    require(wrap.range == std::array<int, 6>{0, 179, 255, 255, 255, 255}, "Hue wrap preserves literal extrema");
    require(wrap.histograms[0][179] == 1, "Final hue bin included");
    requiresInvalidArgument([&] { hsvpicker::analyze(image, cv::Rect()); });
    requiresInvalidArgument([&] { hsvpicker::analyze(image, cv::Rect(-1, 0, 2, 2)); });
    requiresInvalidArgument([&] { hsvpicker::analyze(cv::Mat(), roi); });
    requiresInvalidArgument([&] { hsvpicker::analyze(cv::Mat(2, 2, CV_32FC3), cv::Rect(0, 0, 1, 1)); });
}

void testCoordinates() {
    const cv::Rect viewport(16, 88, 720, 570);
    const cv::Size original(4000, 2000);
    const auto display = hsvpicker::fitImage(original, viewport);
    require(display == cv::Rect(16, 193, 720, 360), "Letterbox preserves image aspect ratio");
    const auto bottomRight = display.br() - cv::Point(1, 1);
    require(hsvpicker::sourceSelection(display.tl(), bottomRight, display, original) ==
            cv::Rect(0, 0, 4000, 2000), "Whole image retains last row and column");
    require(hsvpicker::sourceSelection(bottomRight, display.tl(), display, original) ==
            cv::Rect(0, 0, 4000, 2000), "Reverse drag works");
    require(hsvpicker::sourceSelection(display.tl(), cv::Point(9999, 9999), display, original) ==
            cv::Rect(0, 0, 4000, 2000), "Out-of-image drag is clamped");
    require(hsvpicker::sourceSelection(display.tl(), display.tl(), display, original) ==
            cv::Rect(0, 0, 6, 6), "A downscaled display pixel covers original pixels");
    const auto portrait = hsvpicker::fitImage(cv::Size(100, 1000), viewport);
    require(portrait.width == 57 && portrait.height == 570 && portrait.x > viewport.x,
            "Portrait image is centered with horizontal padding");
    const auto enlarged = hsvpicker::fitImage(cv::Size(2, 1), viewport);
    const auto sample = hsvpicker::sourceSelection(enlarged.tl(), enlarged.tl(), enlarged, cv::Size(2, 1));
    require(sample == cv::Rect(0, 0, 1, 1), "Upscaled pixel maps to a valid original pixel");
    requiresInvalidArgument([&] { hsvpicker::fitImage(cv::Size(), viewport); });
}

void testPicker(const std::string& previewPath) {
    cv::Mat image(120, 200, CV_8UC3, cv::Scalar(0, 255, 0));
    image(cv::Rect(100, 0, 100, 120)).setTo(cv::Scalar(255, 0, 0));
    hsvpicker::Picker picker(true);
    picker.setFrame(image);
    require(!picker.frozen() && !picker.statistics(), "Camera starts live with no ROI");
    picker.onMouse(cv::EVENT_LBUTTONDOWN, 0, 0, cv::EVENT_FLAG_LBUTTON);
    require(!picker.frozen() && picker.selection().empty(), "Clicks outside image are ignored");
    const auto display = picker.imageRect();
    const auto first = display.tl() + cv::Point(10, 10);
    const auto last = display.tl() + cv::Point(80, 80);
    picker.onMouse(cv::EVENT_LBUTTONDOWN, first.x, first.y, cv::EVENT_FLAG_LBUTTON);
    require(picker.frozen() && picker.statistics(), "Drag freezes camera and starts analysis");
    // A backend may send motion events without the left-button flag while held.
    // Only an explicit release or cancellation should finish the selection.
    int previousWidth = picker.selection().width;
    for (int step = 1; step <= 16; ++step) {
        const int flags = step % 3 == 0 ? cv::EVENT_FLAG_SHIFTKEY :
                          step % 3 == 1 ? 0 : cv::EVENT_FLAG_CTRLKEY;
        picker.onMouse(cv::EVENT_MOUSEMOVE, first.x + step * 4, first.y + step * 4, flags);
        require(picker.frozen() && !picker.takeReportRequest(),
                "Motion without a left-button flag must not end dragging");
        require(picker.selection().width > previousWidth,
                "Continuous motion keeps growing the selection");
        previousWidth = picker.selection().width;
    }
    picker.onMouse(cv::EVENT_MOUSEMOVE, last.x, last.y, cv::EVENT_FLAG_LBUTTON);
    const auto duringDrag = picker.selection();
    require(duringDrag.width > 1 && duringDrag.height > 1, "Stats update while dragging");
    picker.onKey(' ');
    require(picker.frozen(), "Space cannot resume during a drag");
    picker.onMouse(cv::EVENT_LBUTTONUP, last.x, last.y, 0);
    require(!picker.frozen(), "Camera automatically resumes after mouse release");
    require(picker.statistics()->range == std::array<int, 6>{60, 60, 255, 255, 255, 255},
            "ROI stats sample original image");
    require(picker.takeReportRequest() && !picker.takeReportRequest(), "Release prints once");
    picker.onMouse(cv::EVENT_MOUSEMOVE, last.x + 10, last.y + 10, cv::EVENT_FLAG_LBUTTON);
    require(picker.selection() == duringDrag && !picker.frozen() && !picker.takeReportRequest(),
            "Motion after release cannot reopen or change the completed selection");
    picker.onKey('p');
    require(picker.takeReportRequest(), "P requests bounds output");
    cv::Mat nextFrame(image.size(), CV_8UC3, cv::Scalar(0, 0, 255));
    if (!picker.frozen()) {
        picker.setFrame(nextFrame);
    }
    require(picker.selection() == duringDrag && picker.statistics()->range[0] == 0,
            "Live selected ROI updates with new camera frames");
    picker.onKey(' ');
    require(picker.frozen(), "Space manually freezes camera");
    picker.onKey(' ');
    require(!picker.frozen(), "Space resumes a manually paused camera");
    picker.onKey(' ');
    picker.onMouse(cv::EVENT_LBUTTONDOWN, first.x, first.y, cv::EVENT_FLAG_LBUTTON);
    picker.onMouse(cv::EVENT_LBUTTONUP, last.x, last.y, 0);
    require(picker.frozen(), "A selection preserves a deliberate manual pause");
    picker.onKey('r');
    require(!picker.statistics() && picker.selection().empty() && !picker.frozen() &&
            !picker.takeReportRequest(), "R clears ROI, pending output, and pause state");
    if (!picker.frozen()) {
        picker.setFrame(image);
    }
    const auto resumedPixel = picker.render().at<cv::Vec3b>(display.y + display.height / 2, display.x + 10);
    require(resumedPixel == cv::Vec3b(0, 255, 0), "Next camera frame is displayed after cancellation");

    picker.onMouse(cv::EVENT_LBUTTONDOWN, first.x, first.y, cv::EVENT_FLAG_LBUTTON);
    require(picker.frozen(), "New drag temporarily freezes camera");
    picker.onKey('R');
    picker.onMouse(cv::EVENT_LBUTTONUP, last.x, last.y, 0);
    require(!picker.frozen() && picker.selection().empty() && !picker.takeReportRequest(),
            "Cancel during dragging resumes camera and ignores the later mouse release");
    const auto end = display.br() - cv::Point(1, 1);
    picker.onMouse(cv::EVENT_LBUTTONDOWN, end.x, end.y, cv::EVENT_FLAG_LBUTTON);
    picker.onMouse(cv::EVENT_MOUSEMOVE, -100, -100, 0);
    require(picker.selection() == cv::Rect(0, 0, 200, 120) && !picker.takeReportRequest() && picker.frozen(),
            "Out-of-image motion is clamped and continues the active drag");
    picker.onMouse(cv::EVENT_LBUTTONUP, -100, -100, 0);
    require(picker.takeReportRequest() && !picker.frozen(),
            "Explicit release ends an out-of-image drag and resumes capture");
    require(!picker.onKey('q') && !picker.onKey(27), "Q and Escape exit");
    picker.setFrame(cv::Mat(20, 30, CV_8UC3, cv::Scalar(0, 0, 0)));
    require(!picker.statistics(), "Resolution change clears old selection");

    hsvpicker::Picker still(false);
    still.setFrame(image);
    const auto stillRect = still.imageRect();
    still.onMouse(cv::EVENT_LBUTTONDOWN, stillRect.x, stillRect.y, cv::EVENT_FLAG_LBUTTON);
    still.onMouse(cv::EVENT_LBUTTONUP, stillRect.br().x - 1, stillRect.br().y - 1, 0);
    require(still.statistics()->pixels == 24000 && !still.frozen(), "Image selection works");
    still.onKey(' ');
    require(!still.frozen(), "Space on an image is harmless");
    const auto canvas = still.render();
    require(canvas.size() == cv::Size(hsvpicker::Picker::canvasWidth, hsvpicker::Picker::canvasHeight) &&
            canvas.type() == CV_8UC3, "Composed window image has expected format");
    if (!previewPath.empty()) {
        require(cv::imwrite(previewPath, canvas), "Write render preview");
    }
    still.onKey('r');
    require(!still.render().empty(), "Empty selection renders histogram placeholders");
}
} // namespace

int main(int argc, char** argv) {
    try {
        testOptions();
        testStatistics();
        testCoordinates();
        testPicker(argc > 1 ? argv[1] : "");
        std::cout << "All HSV statistics, coordinates, options, and interaction checks passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Test failed: " << error.what() << '\n';
        return 1;
    }
}
