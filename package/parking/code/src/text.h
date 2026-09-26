#pragma once

#include <string>

#include <opencv2/core.hpp>

// UTF-8 text (incl. Cyrillic) onto BGRA canvases via FreeType; the OpenCV
// Hershey fonts only cover ASCII.
class TextRenderer {
public:
    bool init(const char *font_path = "/usr/share/fonts/dejavu/DejaVuSans.ttf");
    bool ok() const { return face_ != nullptr; }
    // org = left end of the baseline; px = font size in pixels
    void draw(cv::Mat &bgra, const std::string &utf8, cv::Point org, int px, const cv::Scalar &color);
    cv::Size measure(const std::string &utf8, int px, int *baseline = nullptr);
    // Draws centered inside r (vertically by cap height).
    void draw_centered(cv::Mat &bgra, const std::string &utf8, const cv::Rect &r, int px, const cv::Scalar &color);

private:
    void *lib_ = nullptr;   // FT_Library
    void *face_ = nullptr;  // FT_Face
};

std::u32string utf8_to_u32(const std::string &s);
