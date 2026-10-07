#include "hsv_picker.hpp"

#include <opencv2/highgui.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/videoio.hpp>
#include <opencv2/core/version.hpp>

#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {
constexpr const char* windowName = "HSV Picker";

void mouseCallback(int event, int x, int y, int flags, void* userData) {
    static_cast<hsvpicker::Picker*>(userData)->onMouse(event, x, y, flags);
}

void printSelection(const hsvpicker::Picker& picker) {
    if (!picker.statistics()) {
        return;
    }
    const auto& stats = *picker.statistics();
    const auto& roi = picker.selection();
    std::cout << "ROI: x=" << roi.x << " y=" << roi.y << " w=" << roi.width
              << " h=" << roi.height << " pixels=" << stats.pixels << '\n'
              << "[hbegin, hend, sbegin, send, vbegin, vend] = "
              << hsvpicker::rangeText(stats) << '\n'
              << "const cv::Scalar lower = " << hsvpicker::scalarText(stats, false) << ";\n"
              << "const cv::Scalar upper = " << hsvpicker::scalarText(stats, true) << ";\n"
              << "cv::inRange(hsv, lower, upper, mask);\n" << std::flush;
}

class WindowMonitor {
public:
    explicit WindowMonitor(bool checkProperties) : checkProperties_(checkProperties) {}

    bool isOpen() {
        if (!checkProperties_) {
            return true;
        }
        try {
            const double visible = cv::getWindowProperty(windowName, cv::WND_PROP_VISIBLE);
            if (visible >= 0) {
                visibilitySupported_ = true;
                return visible > 0;
            }
            if (visibilitySupported_) {
                return false;
            }
        } catch (const cv::Exception&) {
            if (visibilitySupported_) {
                return false;
            }
        }

        // Older GTK backends always return -1 for VISIBLE. AUTOSIZE is 0/1
        // while the window exists, then -1 (or an exception) after closing it.
        // Only infer closure after the property was confirmed to work once.
        try {
            const double autosize = cv::getWindowProperty(windowName, cv::WND_PROP_AUTOSIZE);
            if (autosize >= 0) {
                autosizeSupported_ = true;
                return true;
            }
        } catch (const cv::Exception&) {
            // An unsupported property must not terminate the application.
        }
        return !autosizeSupported_;
    }

private:
    bool checkProperties_;
    bool visibilitySupported_ = false;
    bool autosizeSupported_ = false;
};
} // namespace

int main(int argc, char** argv) {
    try {
        std::vector<std::string> arguments;
        for (int i = 1; i < argc; ++i) {
            arguments.emplace_back(argv[i]);
        }
        const auto options = hsvpicker::parseOptions(arguments);
        if (options.help) {
            std::cout << "Usage: hsv-picker [--source DEVICE_OR_IMAGE]\n"
                      << "Default source: /dev/video0\n"
                      << "Examples:\n"
                      << "  hsv-picker --source /dev/video2\n"
                      << "  hsv-picker --source ./image.png\n"
                      << "Controls: drag left mouse to select; Space freezes/resumes camera;\n"
                      << "          R clears/resumes; P prints OpenCV bounds; Q/Esc quits.\n"
                      << "HSV units: COLOR_BGR2HSV, H 0..179, S/V 0..255.\n";
            return EXIT_SUCCESS;
        }

        std::cout << "OpenCV: " << CV_VERSION << '\n'
                  << "Source: " << options.source << '\n' << std::flush;
        const bool camera = hsvpicker::isCameraSource(options.source);
        cv::VideoCapture capture;
        cv::Mat frame;
        if (camera) {
            if (!capture.open(options.source, cv::CAP_V4L2)) {
                throw std::runtime_error("Cannot open camera: " + options.source +
                                         " (check device path and permissions)");
            }
            if (!capture.read(frame) || frame.empty()) {
                throw std::runtime_error("Cannot read the first camera frame: " + options.source);
            }
        } else {
            frame = cv::imread(options.source, cv::IMREAD_COLOR);
            if (frame.empty()) {
                throw std::runtime_error("Cannot load image: " + options.source);
            }
        }

        hsvpicker::Picker picker(camera);
        picker.setFrame(frame);
        // HighGUI maps mouse callbacks back to composed-image coordinates when
        // scaling the image. NORMAL permits resizing/maximizing on the desktop.
        cv::namedWindow(windowName, cv::WINDOW_NORMAL | cv::WINDOW_KEEPRATIO |
                                    cv::WINDOW_GUI_NORMAL);
        cv::imshow(windowName, picker.render());
        // Request the initial size after assigning the image, so the backend
        // does not replace it with its small default window dimensions.
        cv::resizeWindow(windowName, 1440, 880);
        cv::setMouseCallback(windowName, mouseCallback, &picker);
        bool checkProperties = true;
#ifdef HSV_HAS_UI_FRAMEWORK
        // The native Wayland backend does not implement getWindowProperty.
        const auto backend = cv::currentUIFramework();
        checkProperties = backend != "WAYLAND";
        std::cout << "HighGUI backend: " << backend << '\n';
#endif
        WindowMonitor windowMonitor(checkProperties);
        std::cout << "Drag left mouse to select. Camera freezes during dragging and resumes on release.\n"
                  << "Bounds use H 0..179 and S/V 0..255; release the mouse or press P to print.\n"
                  << std::flush;
        while (true) {
            cv::imshow(windowName, picker.render());
            const int key = cv::waitKey(20);
            if (!windowMonitor.isOpen() || !picker.onKey(key < 0 ? key : key & 0xff)) {
                break;
            }
            if (picker.takeReportRequest()) {
                printSelection(picker);
            }
            // Read after processing events, so a drag samples the frame just displayed.
            if (camera && !picker.frozen()) {
                if (!capture.read(frame) || frame.empty()) {
                    throw std::runtime_error("Camera stream stopped: " + options.source);
                }
                picker.setFrame(frame);
            }
        }
        cv::destroyAllWindows();
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "hsv-picker: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
