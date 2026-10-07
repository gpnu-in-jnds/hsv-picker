#pragma once

#include <opencv2/core.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace hsvpicker {

struct Options {
    std::string source = "/dev/video0";
    bool help = false;
};

Options parseOptions(const std::vector<std::string>& arguments);
bool isCameraSource(const std::string& source);

struct HsvStatistics {
    // [hbegin, hend, sbegin, send, vbegin, vend], inclusive extrema.
    std::array<int, 6> range{};
    std::array<std::vector<std::uint64_t>, 3> histograms;
    std::uint64_t pixels = 0;
};

HsvStatistics analyze(const cv::Mat& bgr, const cv::Rect& roi);
std::string rangeText(const HsvStatistics& statistics);
std::string scalarText(const HsvStatistics& statistics, bool upper);

// Mouse coordinates refer to the composed HighGUI image, not screen coordinates.
cv::Rect fitImage(cv::Size image, const cv::Rect& viewport);
cv::Rect sourceSelection(cv::Point first, cv::Point last,
                         const cv::Rect& displayedImage, cv::Size sourceSize);

class Picker {
public:
    static constexpr int canvasWidth = 1180;
    static constexpr int canvasHeight = 720;

    explicit Picker(bool camera);
    void setFrame(const cv::Mat& bgr);
    void onMouse(int event, int x, int y, int flags);
    // Returns false on Q/Esc. Space toggles capture; R clears/resumes; P prints.
    bool onKey(int key);
    cv::Mat render() const;
    bool frozen() const { return paused_ || dragging_; }
    bool takeReportRequest();
    const cv::Rect& imageRect() const { return imageRect_; }
    const cv::Rect& selection() const { return selection_; }
    const std::optional<HsvStatistics>& statistics() const { return statistics_; }

private:
    void selectTo(cv::Point point);
    void refreshStatistics();

    bool camera_;
    bool paused_ = false;
    bool dragging_ = false;
    bool reportRequested_ = false;
    cv::Point anchor_;
    cv::Mat frame_;
    cv::Rect imageRect_;
    cv::Rect selection_;
    std::optional<HsvStatistics> statistics_;
};

} // namespace hsvpicker
