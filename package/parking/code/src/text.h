#pragma once

#include <stdint.h>
#include <string>
#include <unordered_map>
#include <vector>

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
    // Rendered glyphs by (size, code point): the menu redraws the same few
    // hundred glyphs every frame and FreeType rasterization is the slow part.
    struct Glyph {
        std::vector<uint8_t> bits;  // w x h coverage
        int w = 0, h = 0, left = 0, top = 0, advance = 0;
    };
    const Glyph &glyph(char32_t cp, int px);  // g_ft_mtx held
    void metrics(int px, int *asc, int *desc); // g_ft_mtx held

    void *lib_ = nullptr;   // FT_Library
    void *face_ = nullptr;  // FT_Face
    std::unordered_map<uint64_t, Glyph> glyphs_;
    std::unordered_map<int, std::pair<int, int>> metrics_;  // px -> ascender, descender
};

std::u32string utf8_to_u32(const std::string &s);
