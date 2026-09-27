#include "text.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <string.h>

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

const TextRenderer::Glyph &TextRenderer::glyph(char32_t cp, int px)
{
    uint64_t key = (uint64_t)px << 32 | cp;
    auto it = glyphs_.find(key);
    if (it != glyphs_.end())
        return it->second;
    if (glyphs_.size() > 4096)
        glyphs_.clear();  // font sizes come from the layout; this never grows in practice
    Glyph &gl = glyphs_[key];
    FT_Face face = (FT_Face)face_;
    FT_Set_Pixel_Sizes(face, 0, px);
    if (FT_Load_Char(face, cp, FT_LOAD_RENDER))
        return gl;
    FT_GlyphSlot g = face->glyph;
    const FT_Bitmap &bm = g->bitmap;
    gl.w = bm.width;
    gl.h = bm.rows;
    gl.left = g->bitmap_left;
    gl.top = g->bitmap_top;
    gl.advance = g->advance.x >> 6;
    gl.bits.resize((size_t)gl.w * gl.h);
    for (int r = 0; r < gl.h; r++)
        memcpy(&gl.bits[(size_t)r * gl.w], bm.buffer + r * bm.pitch, gl.w);
    return gl;
}

void TextRenderer::metrics(int px, int *asc, int *desc)
{
    auto it = metrics_.find(px);
    if (it == metrics_.end()) {
        FT_Face face = (FT_Face)face_;
        FT_Set_Pixel_Sizes(face, 0, px);
        it = metrics_.emplace(px, std::make_pair((int)(face->size->metrics.ascender >> 6),
                                                 (int)-(face->size->metrics.descender >> 6))).first;
    }
    *asc = it->second.first;
    *desc = it->second.second;
}

cv::Size TextRenderer::measure(const std::string &utf8, int px, int *baseline)
{
    if (!face_)
        return cv::Size(0, 0);
    std::lock_guard<std::mutex> lk(g_ft_mtx);
    int w = 0;
    for (char32_t cp : utf8_to_u32(utf8))
        w += glyph(cp, px).advance;
    int asc, desc;
    metrics(px, &asc, &desc);
    if (baseline)
        *baseline = desc;
    return cv::Size(w, asc);
}

void TextRenderer::draw(cv::Mat &img, const std::string &utf8, cv::Point org, int px, const cv::Scalar &color)
{
    if (!face_ || img.type() != CV_8UC4)
        return;
    std::lock_guard<std::mutex> lk(g_ft_mtx);
    const unsigned cb = (unsigned)color[0], cg = (unsigned)color[1], cr = (unsigned)color[2];
    const unsigned ca = (unsigned)color[3];
    int x = org.x;
    for (char32_t cp : utf8_to_u32(utf8)) {
        const Glyph &g = glyph(cp, px);
        int x0 = x + g.left, y0 = org.y - g.top;
        x += g.advance;
        // clip the glyph box once instead of per pixel
        int c0 = std::max(0, -x0), c1 = std::min(g.w, img.cols - x0);
        int r0 = std::max(0, -y0), r1 = std::min(g.h, img.rows - y0);
        for (int r = r0; r < r1; r++) {
            uint8_t *p = img.ptr<uint8_t>(y0 + r) + (x0 + c0) * 4;
            const uint8_t *src = &g.bits[(size_t)r * g.w];
            for (int c = c0; c < c1; c++, p += 4) {
                unsigned s = src[c];
                if (!s)
                    continue;
                // alpha-blend color over the pixel, keeping the overlay alpha
                unsigned a = s * ca / 255, na = 255 - a;
                p[0] = (uint8_t)((cb * a + p[0] * na) / 255);
                p[1] = (uint8_t)((cg * a + p[1] * na) / 255);
                p[2] = (uint8_t)((cr * a + p[2] * na) / 255);
                if (p[3] < a)
                    p[3] = (uint8_t)a;
            }
        }
    }
}

void TextRenderer::draw_centered(cv::Mat &img, const std::string &utf8, const cv::Rect &r, int px,
                                 const cv::Scalar &color)
{
    cv::Size sz = measure(utf8, px);
    int cap = px * 72 / 100;  // approx. cap height of DejaVu Sans
    draw(img, utf8, cv::Point(r.x + (r.width - sz.width) / 2, r.y + (r.height + cap) / 2), px, color);
}
