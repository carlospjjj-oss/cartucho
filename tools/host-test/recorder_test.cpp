// Video recording: pictures scaled into the encoder's YUV canvas with their
// emulated times, sound converted to 48 kHz with matching times.
#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <cstdio>
#include <vector>

#include "recorder.h"

using namespace cartucho;

int failures = 0;
#define CHECK(cond, ...)                           \
    do {                                           \
        std::printf((cond) ? "ok    " : "FAIL  "); \
        std::printf(__VA_ARGS__);                  \
        std::printf("\n");                         \
        if (!(cond)) failures++;                   \
    } while (0)

int main() {
    Recorder rec;
    const double fps = 59.7275;
    const double rate = 32768;
    rec.Start(fps, rate);
    // A 160x144 picture: left half white, right half pure red.
    std::vector<uint8_t> pic(160 * 144 * 4);
    for (int y = 0; y < 144; y++)
        for (int x = 0; x < 160; x++) {
            uint8_t* p = &pic[(y * 160 + x) * 4];
            if (x < 80) { p[0] = p[1] = p[2] = 255; } else { p[0] = 255; p[1] = p[2] = 0; }
            p[3] = 255;
        }
    std::vector<int16_t> audio;
    for (int f = 0; f < 120; f++) {
        rec.PushVideo(pic.data(), 160, 144);
        // One frame of a 1 kHz tone at the core rate.
        const int n = static_cast<int>(rate / fps);
        audio.resize(n * 2);
        for (int i = 0; i < n; i++)
            audio[i * 2] = audio[i * 2 + 1] = static_cast<int16_t>(8000 * std::sin(2 * M_PI * 1000 * (f * n + i) / rate));
        rec.PushAudio(audio.data(), n);
        if (f == 0) {
            int w, h;
            rec.FirstSize(&w, &h);
            CHECK(w == 160 && h == 144, "first picture size %dx%d", w, h);
        }
    }
    // Canvas 480x432 (3x), NV12-like layout (U and V interleaved).
    const int W = 480, H = 432;
    std::vector<uint8_t> y(W * H), uv(W * H / 2);
    int64_t last = -1;
    int pictures = 0;
    bool increasing = true;
    while (rec.HasVideo()) {
        const int64_t pts = rec.FillVideo(y.data(), W, uv.data(), uv.data() + 1, W, 2, W, H);
        if (pts <= last) increasing = false;
        last = pts;
        pictures++;
    }
    CHECK(pictures == 10, "pictures waiting are capped (%d kept of 120)", pictures);
    CHECK(increasing, "picture times increase");
    // The kept ones are the first ten: frame 9 at 9/fps.
    CHECK(llabs(last - static_cast<int64_t>(9 * 1e6 / fps)) <= 1, "time from emulated frames (%lld us)", (long long)last);
    const int yl = y[200 * W + 100], yr = y[200 * W + 380];
    CHECK(yl > 230 && yr > 70 && yr < 90, "white then red, scaled 3x (Y %d / %d)", yl, yr);
    const int u_r = uv[(100 * W) + 380 / 2 * 2], v_r = uv[(100 * W) + 380 / 2 * 2 + 1];
    CHECK(v_r > 230 && u_r < 110, "red chroma (U %d V %d)", u_r, v_r);

    const size_t avail = rec.AudioAvailable();
    const double want = 120 * std::floor(rate / fps) * 48000 / rate;
    CHECK(std::fabs(avail - want) < 10, "audio at 48 kHz: %zu frames (expected about %.0f)", avail, want);
    std::vector<int16_t> out(4096 * 2);
    int64_t t0 = -1, t1 = -1;
    rec.FillAudio(out.data(), 4096, &t0);
    rec.FillAudio(out.data(), 4096, &t1);
    CHECK(t0 == 0 && t1 == 4096LL * 1000000 / 48000, "audio times follow the samples (%lld, %lld)", (long long)t0, (long long)t1);
    int peak = 0;
    for (size_t i = 0; i < out.size(); i++) peak = std::max(peak, std::abs(static_cast<int>(out[i])));
    CHECK(peak > 7500 && peak <= 8000, "tone survives resampling (peak %d)", peak);
    rec.Stop();
    rec.PushVideo(pic.data(), 160, 144);
    CHECK(!rec.HasVideo(), "nothing recorded after stopping");
    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
