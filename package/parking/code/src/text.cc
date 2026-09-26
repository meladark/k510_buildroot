#include "text.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <algorithm>
#include <cstdio>
#include <mutex>

static std::mutex g_ft_mtx;  // FT_Face is not thread safe

std::u32string utf8_to_u32(const std::string &s)
{
    std::u32string o;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = s[i];
        char32_t cp;
        int n;
        if (c < 0x80) { cp = c; n = 1; }
        else if ((c >> 5) == 6) { cp = c & 0x1f; n = 2; }
        else if ((c >> 4) == 14) { cp = c & 0x0f; n = 3; }
        else if ((c >> 3) == 30) { cp = c & 0x07; n = 4; }
        else { i++; continue; }
        if (i + n > s.size())
            break;
        for (int k = 1; k < n; k++)
            cp = (cp << 6) | (s[i + k] & 0x3f);
        o += cp;
        i += n;
    }
    return o;
}

bool TextRenderer::init(const char *font_path)
{
    FT_Library lib;
    if (FT_Init_FreeType(&lib))
        return false;
    FT_Face face;
    if (FT_New_Face(lib, font_path, 0, &face)) {
        fprintf(stderr, "text: cannot load font %s\n", font_path);
        FT_Done_FreeType(lib);
        return false;
    }
    lib_ = lib;
    face_ = face;
    return true;
}

cv::Size TextRenderer::measure(const std::string &utf8, int px, int *baseline)
{
    if (!face_)
        return cv::Size(0, 0);
    std::lock_guard<std::mutex> lk(g_ft_mtx);
    FT_Face face = (FT_Face)face_;
    FT_Set_Pixel_Sizes(face, 0, px);
    int w = 0;
    for (char32_t cp : utf8_to_u32(utf8)) {
        if (FT_Load_Char(face, cp, FT_LOAD_DEFAULT))
            continue;
        w += face->glyph->advance.x >> 6;
    }
    int asc = face->size->metrics.ascender >> 6;
    int desc = -(face->size->metrics.descender >> 6);
    if (baseline)
        *baseline = desc;
    return cv::Size(w, asc);
}

void TextRenderer::draw(cv::Mat &img, const std::string &utf8, cv::Point org, int px, const cv::Scalar &color)
{
    if (!face_ || img.type() != CV_8UC4)
        return;
    std::lock_guard<std::mutex> lk(g_ft_mtx);
    FT_Face face = (FT_Face)face_;
    FT_Set_Pixel_Sizes(face, 0, px);
    int x = org.x;
    for (char32_t cp : utf8_to_u32(utf8)) {
        if (FT_Load_Char(face, cp, FT_LOAD_RENDER))
            continue;
        FT_GlyphSlot g = face->glyph;
        const FT_Bitmap &bm = g->bitmap;
        int x0 = x + g->bitmap_left, y0 = org.y - g->bitmap_top;
        for (unsigned r = 0; r < bm.rows; r++) {
            int y = y0 + (int)r;
            if (y < 0 || y >= img.rows)
                continue;
            uint8_t *dst = img.ptr<uint8_t>(y);
            const uint8_t *src = bm.buffer + r * bm.pitch;
            for (unsigned c = 0; c < bm.width; c++) {
                int xx = x0 + (int)c;
                if (xx < 0 || xx >= img.cols || !src[c])
                    continue;
                // alpha-blend color over the pixel, keeping the overlay alpha
                unsigned a = src[c] * (unsigned)color[3] / 255;
                uint8_t *p = dst + xx * 4;
                for (int k = 0; k < 3; k++)
                    p[k] = (uint8_t)((color[k] * a + p[k] * (255 - a)) / 255);
                p[3] = (uint8_t)std::max<unsigned>(p[3], a);
            }
        }
        x += g->advance.x >> 6;
    }
}

void TextRenderer::draw_centered(cv::Mat &img, const std::string &utf8, const cv::Rect &r, int px,
                                 const cv::Scalar &color)
{
    cv::Size sz = measure(utf8, px);
    int cap = px * 72 / 100;  // approx. cap height of DejaVu Sans
    draw(img, utf8, cv::Point(r.x + (r.width - sz.width) / 2, r.y + (r.height + cap) / 2), px, color);
}
