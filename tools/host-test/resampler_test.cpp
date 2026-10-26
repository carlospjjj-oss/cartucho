// Feeds a sine through an AudioRing and the Resampler at 32768 -> 48000 Hz in
// odd-sized chunks; the output must match an ideal resampling closely, which
// fails if samples are dropped or repeated at chunk boundaries.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "core_host.h"
#include "resampler.h"

namespace cartucho {
void PlatformLog(int, const char*) {}
}  // namespace cartucho

using namespace cartucho;

static size_t Pull(void* user, int16_t* stereo, size_t frames) {
    return static_cast<AudioRing*>(user)->Pop(stereo, frames);
}

int main() {
    const double in_rate = 32768, out_rate = 48000, freq = 440;
    AudioRing ring(1 << 16);
    std::vector<int16_t> in;
    for (int i = 0; i < 40000; i++) {
        const auto v = static_cast<int16_t>(10000 * std::sin(2 * M_PI * freq * i / in_rate));
        in.push_back(v);
        in.push_back(static_cast<int16_t>(-v));
    }
    ring.Push(in.data(), 40000);

    Resampler rs;
    rs.SetRates(in_rate, out_rate);
    std::vector<int16_t> out;
    const int chunks[] = {192, 480, 256, 997, 64, 1};
    for (int c = 0; out.size() / 2 < 50000; c++) {
        const int n = chunks[c % 6];
        std::vector<int16_t> buf(n * 2);
        rs.Process(&Pull, &ring, buf.data(), n);
        out.insert(out.end(), buf.begin(), buf.end());
    }
    double max_err = 0;
    for (size_t i = 10; i < 50000; i++) {
        const double ideal = 10000 * std::sin(2 * M_PI * freq * (i * in_rate / out_rate) / in_rate);
        max_err = std::max(max_err, std::fabs(out[i * 2] - ideal));
        max_err = std::max(max_err, std::fabs(out[i * 2 + 1] + ideal));
    }
    const bool ok = max_err < 60;
    std::printf("%s  resampler max error %.1f of 10000\n", ok ? "ok  " : "FAIL", max_err);
    return ok ? 0 : 1;
}
