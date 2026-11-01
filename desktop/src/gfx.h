// Cartucho PC - pictures as GL textures (covers, photos, slot pictures).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cartucho {

struct Texture {
    unsigned id = 0;
    int w = 0, h = 0;
    bool valid() const { return id != 0; }
    unsigned long long im() const { return id; }  // ImTextureID (an integer in this ImGui)
};

Texture TextureFromRgba(const uint8_t* rgba, int w, int h);
Texture LoadTexture(const std::string& path);                    // PNG/JPG
Texture LoadTextureMem(const std::vector<uint8_t>& bytes);
void FreeTexture(Texture* t);
// Decodes an image file to RGBA (for work off the GL thread).
bool DecodeImage(const std::string& path, std::vector<uint8_t>* rgba, int* w, int* h, int max_side = 0);
bool DecodeImageMem(const uint8_t* data, size_t size, std::vector<uint8_t>* rgba, int* w, int* h, int max_side = 0);
// Writes RGBA as PNG, scaled down to fit `max_side` (0 keeps the size).
bool WritePng(const std::string& path, const uint8_t* rgba, int w, int h, int max_side = 0);

}  // namespace cartucho
