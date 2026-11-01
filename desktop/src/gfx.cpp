#include "gfx.h"

#include <glad/gles2.h>

#include <algorithm>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#include <stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include "paths.h"

namespace cartucho {

namespace {
// Box-filter downscale, good enough for thumbnails.
void Shrink(std::vector<uint8_t>* rgba, int* w, int* h, int max_side) {
    if (max_side <= 0 || std::max(*w, *h) <= max_side)
        return;
    const float s = static_cast<float>(max_side) / std::max(*w, *h);
    const int nw = std::max(1, static_cast<int>(*w * s)), nh = std::max(1, static_cast<int>(*h * s));
    std::vector<uint8_t> out(static_cast<size_t>(nw) * nh * 4);
    for (int y = 0; y < nh; y++) {
        const int y0 = y * *h / nh, y1 = std::max(y0 + 1, (y + 1) * *h / nh);
        for (int x = 0; x < nw; x++) {
            const int x0 = x * *w / nw, x1 = std::max(x0 + 1, (x + 1) * *w / nw);
            unsigned sum[4] = {0, 0, 0, 0}, n = 0;
            for (int yy = y0; yy < y1; yy++)
                for (int xx = x0; xx < x1; xx++) {
                    const uint8_t* p = &(*rgba)[(static_cast<size_t>(yy) * *w + xx) * 4];
                    for (int c = 0; c < 4; c++) sum[c] += p[c];
                    n++;
                }
            uint8_t* o = &out[(static_cast<size_t>(y) * nw + x) * 4];
            for (int c = 0; c < 4; c++) o[c] = static_cast<uint8_t>(sum[c] / n);
        }
    }
    rgba->swap(out);
    *w = nw;
    *h = nh;
}
}  // namespace

Texture TextureFromRgba(const uint8_t* rgba, int w, int h) {
    Texture t;
    if (!rgba || w <= 0 || h <= 0)
        return t;
    glGenTextures(1, &t.id);
    glBindTexture(GL_TEXTURE_2D, t.id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    t.w = w;
    t.h = h;
    return t;
}

bool DecodeImageMem(const uint8_t* data, size_t size, std::vector<uint8_t>* rgba, int* w, int* h, int max_side) {
    int n = 0;
    stbi_uc* px = stbi_load_from_memory(data, static_cast<int>(size), w, h, &n, 4);
    if (!px)
        return false;
    rgba->assign(px, px + static_cast<size_t>(*w) * *h * 4);
    stbi_image_free(px);
    Shrink(rgba, w, h, max_side);
    return true;
}

bool DecodeImage(const std::string& path, std::vector<uint8_t>* rgba, int* w, int* h, int max_side) {
    std::vector<uint8_t> bytes;
    if (!ReadFileBytes(path, &bytes) || bytes.empty())
        return false;
    return DecodeImageMem(bytes.data(), bytes.size(), rgba, w, h, max_side);
}

Texture LoadTextureMem(const std::vector<uint8_t>& bytes) {
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    if (bytes.empty() || !DecodeImageMem(bytes.data(), bytes.size(), &rgba, &w, &h, 1024))
        return Texture{};
    return TextureFromRgba(rgba.data(), w, h);
}

Texture LoadTexture(const std::string& path) {
    std::vector<uint8_t> bytes;
    ReadFileBytes(path, &bytes);
    return LoadTextureMem(bytes);
}

void FreeTexture(Texture* t) {
    if (t->id)
        glDeleteTextures(1, &t->id);
    *t = Texture{};
}

bool WritePng(const std::string& path, const uint8_t* rgba, int w, int h, int max_side) {
    std::vector<uint8_t> px(rgba, rgba + static_cast<size_t>(w) * h * 4);
    Shrink(&px, &w, &h, max_side);
    for (size_t i = 3; i < px.size(); i += 4)
        px[i] = 255;
    struct Ctx { std::vector<uint8_t> out; } ctx;
    stbi_write_png_to_func([](void* c, void* d, int n) {
        auto* v = &static_cast<Ctx*>(c)->out;
        v->insert(v->end(), static_cast<uint8_t*>(d), static_cast<uint8_t*>(d) + n);
    }, &ctx, w, h, 4, px.data(), w * 4);
    return !ctx.out.empty() && WriteFileBytes(path, ctx.out.data(), ctx.out.size());
}

}  // namespace cartucho
