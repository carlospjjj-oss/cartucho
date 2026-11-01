#include "video.h"

#include <stb_image_write.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>

#include "paths.h"
#include "platform.h"
#include "recorder.h"

#ifdef _WIN32
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#endif

namespace cartucho {

namespace {

std::string SafeName(std::string s) {
    for (char& c : s)
        if (std::strchr("\\/:*?\"<>|", c)) c = '_';
    if (s.size() > 80) s.resize(80);
    return s;
}

// The canvas: the first picture scaled by a whole number to about 720 lines,
// sized to multiples of 16 (what video encoders like).
void Canvas(int w, int h, int* cw, int* ch) {
    const int scale = std::max(1, 720 / std::max(1, h));
    *cw = (w * scale + 15) / 16 * 16;
    *ch = (h * scale + 15) / 16 * 16;
}

void SleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

FILE* OpenWrite(const std::string& path) {
#ifdef _WIN32
    const int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, w.data(), n);
    return _wfopen(w.c_str(), L"wb");
#else
    return std::fopen(path.c_str(), "wb");
#endif
}

}  // namespace

VideoWriter::~VideoWriter() {
    Stop();
    if (thread_.joinable()) thread_.join();
}

void VideoWriter::Start(std::shared_ptr<Recorder> rec, double fps, const std::string& dir, const std::string& title) {
    if (running_) return;
    if (thread_.joinable()) thread_.join();
    rec_ = std::move(rec);
    fps_ = fps > 1 ? fps : 60;
    stop_ = false;
    running_ = true;
    thread_ = std::thread(&VideoWriter::Run, this, dir, title);
}

void VideoWriter::Stop() {
    if (rec_) rec_->Stop();   // no new pictures; what is queued still goes in
    if (running_) stop_ = true;
}

std::string VideoWriter::TakeMessage() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string m;
    m.swap(message_);
    return m;
}

void VideoWriter::Run(std::string dir, std::string title) {
    MakeDirs(dir);
    char stamp[32];
    const std::time_t t = std::time(nullptr);
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H-%M-%S", std::localtime(&t));
    const std::string base = JoinPath(dir, SafeName(title) + " " + stamp);
    // Wait for the first picture (its size sets the canvas).
    int w = 0, h = 0;
    while (!stop_) {
        rec_->FirstSize(&w, &h);
        if (w > 0) break;
        SleepMs(20);
    }
    std::string result;
    if (w > 0) {
        bool ok = false;
#ifdef _WIN32
        ok = WriteMp4(base + ".mp4");
        if (ok) result = base + ".mp4";
#endif
        if (!ok) {
            ok = WriteAvi(base + ".avi");
            if (ok) result = base + ".avi";
        }
        std::lock_guard<std::mutex> lock(mutex_);
        message_ = ok ? "Vídeo salvo: " + result : "Não foi possível gravar o vídeo";
    }
    running_ = false;
}

// --- MP4 through Media Foundation (Windows) ------------------------------------

#ifdef _WIN32
namespace {
// Media Foundation is loaded when needed: Windows "N" editions lack it, and
// the program must still start there (the .avi is written instead).
struct Mf {
    HRESULT(WINAPI* Startup)(ULONG, DWORD) = nullptr;
    HRESULT(WINAPI* Shutdown)() = nullptr;
    HRESULT(WINAPI* CreateMediaType)(IMFMediaType**) = nullptr;
    HRESULT(WINAPI* CreateMemoryBuffer)(DWORD, IMFMediaBuffer**) = nullptr;
    HRESULT(WINAPI* CreateSample)(IMFSample**) = nullptr;
    HRESULT(WINAPI* CreateSinkWriterFromURL)(LPCWSTR, IMFByteStream*, IMFAttributes*, IMFSinkWriter**) = nullptr;
    bool Load() {
        HMODULE plat = LoadLibraryW(L"mfplat.dll");
        HMODULE rw = LoadLibraryW(L"mfreadwrite.dll");
        if (!plat || !rw) return false;
        Startup = reinterpret_cast<decltype(Startup)>(GetProcAddress(plat, "MFStartup"));
        Shutdown = reinterpret_cast<decltype(Shutdown)>(GetProcAddress(plat, "MFShutdown"));
        CreateMediaType = reinterpret_cast<decltype(CreateMediaType)>(GetProcAddress(plat, "MFCreateMediaType"));
        CreateMemoryBuffer = reinterpret_cast<decltype(CreateMemoryBuffer)>(GetProcAddress(plat, "MFCreateMemoryBuffer"));
        CreateSample = reinterpret_cast<decltype(CreateSample)>(GetProcAddress(plat, "MFCreateSample"));
        CreateSinkWriterFromURL =
            reinterpret_cast<decltype(CreateSinkWriterFromURL)>(GetProcAddress(rw, "MFCreateSinkWriterFromURL"));
        return Startup && Shutdown && CreateMediaType && CreateMemoryBuffer && CreateSample && CreateSinkWriterFromURL;
    }
};
}  // namespace

bool VideoWriter::WriteMp4(const std::string& path) {
    static Mf mf;
    static const bool have = mf.Load();
    if (!have) return false;
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return false;
    if (FAILED(mf.Startup(MF_VERSION, MFSTARTUP_LITE))) {
        CoUninitialize();
        return false;
    }
    int fw = 0, fh = 0, cw, ch;
    rec_->FirstSize(&fw, &fh);
    Canvas(fw, fh, &cw, &ch);
    const int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring wpath(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath.data(), n);

    IMFSinkWriter* writer = nullptr;
    IMFMediaType* t = nullptr;
    DWORD vs = 0, as = 0;
    bool ok = SUCCEEDED(mf.CreateSinkWriterFromURL(wpath.c_str(), nullptr, nullptr, &writer));
    const UINT32 fps_num = static_cast<UINT32>(std::lround(fps_ * 1000)), fps_den = 1000;
    auto video_type = [&](const GUID& sub) {
        mf.CreateMediaType(&t);
        t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        t->SetGUID(MF_MT_SUBTYPE, sub);
        t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(t, MF_MT_FRAME_SIZE, static_cast<UINT32>(cw), static_cast<UINT32>(ch));
        MFSetAttributeRatio(t, MF_MT_FRAME_RATE, fps_num, fps_den);
        MFSetAttributeRatio(t, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    };
    auto audio_type = [&](const GUID& sub) {
        mf.CreateMediaType(&t);
        t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        t->SetGUID(MF_MT_SUBTYPE, sub);
        t->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        t->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, Recorder::kAudioRate);
        t->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
    };
    if (ok) {
        video_type(MFVideoFormat_H264);
        t->SetUINT32(MF_MT_AVG_BITRATE, 10'000'000);
        ok = SUCCEEDED(writer->AddStream(t, &vs));
        t->Release();
    }
    if (ok) {
        video_type(MFVideoFormat_NV12);
        ok = SUCCEEDED(writer->SetInputMediaType(vs, t, nullptr));
        t->Release();
    }
    if (ok) {
        audio_type(MFAudioFormat_AAC);
        t->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24000);
        ok = SUCCEEDED(writer->AddStream(t, &as));
        t->Release();
    }
    if (ok) {
        audio_type(MFAudioFormat_PCM);
        t->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
        t->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, Recorder::kAudioRate * 4);
        ok = SUCCEEDED(writer->SetInputMediaType(as, t, nullptr));
        t->Release();
    }
    if (ok) ok = SUCCEEDED(writer->BeginWriting());
    if (!ok) {
        PlatformLog(2, "Media Foundation: no H.264/AAC encoder; writing an .avi instead");
        if (writer) writer->Release();
        RemoveFile(path);
        mf.Shutdown();
        CoUninitialize();
        return false;
    }

    const LONGLONG frame_100ns = static_cast<LONGLONG>(1e7 / fps_);
    const size_t vbytes = static_cast<size_t>(cw) * ch * 3 / 2;
    std::vector<int16_t> pcm(4096 * 2);
    auto write = [&](DWORD stream, const void* data, size_t size, LONGLONG time, LONGLONG duration) {
        IMFMediaBuffer* buf = nullptr;
        IMFSample* s = nullptr;
        if (FAILED(mf.CreateMemoryBuffer(static_cast<DWORD>(size), &buf))) return false;
        BYTE* p = nullptr;
        buf->Lock(&p, nullptr, nullptr);
        if (data) std::memcpy(p, data, size);
        buf->Unlock();
        buf->SetCurrentLength(static_cast<DWORD>(size));
        mf.CreateSample(&s);
        s->AddBuffer(buf);
        s->SetSampleTime(time);
        s->SetSampleDuration(duration);
        const bool r = SUCCEEDED(writer->WriteSample(stream, s));
        s->Release();
        buf->Release();
        return r;
    };
    std::vector<uint8_t> frame(vbytes);
    bool good = true;
    for (;;) {
        const bool stopping = stop_;
        bool did = false;
        uint8_t* y = frame.data();
        uint8_t* uv = y + static_cast<size_t>(cw) * ch;
        const int64_t pts = rec_->FillVideo(y, cw, uv, uv + 1, cw, 2, cw, ch);
        if (pts >= 0) {
            good = write(vs, frame.data(), vbytes, pts * 10, frame_100ns) && good;
            did = true;
        }
        int64_t apts = 0;
        const size_t got = rec_->FillAudio(pcm.data(), pcm.size() / 2, &apts);
        if (got > 0) {
            good = write(as, pcm.data(), got * 4, apts * 10, static_cast<LONGLONG>(got) * 10'000'000 / Recorder::kAudioRate) &&
                   good;
            did = true;
        }
        if (stopping && !did) break;
        if (!did) SleepMs(4);
    }
    good = SUCCEEDED(writer->Finalize()) && good;
    writer->Release();
    mf.Shutdown();
    CoUninitialize();
    return good;
}
#else
bool VideoWriter::WriteMp4(const std::string&) { return false; }
#endif

// --- AVI (Motion JPEG + PCM) ------------------------------------------------------

namespace {

class Avi {
public:
    bool Open(const std::string& path, int w, int h, double fps) {
        f_ = OpenWrite(path);
        if (!f_) return false;
        w_ = w;
        h_ = h;
        fps_ = fps;
        WriteHeader();
        return true;
    }
    void Video(const std::vector<uint8_t>& jpg) { Chunk("00dc", jpg.data(), jpg.size(), true); frames_++; }
    void Audio(const int16_t* pcm, size_t frames) {
        Chunk("01wb", pcm, frames * 4, false);
        audio_frames_ += frames;
    }
    bool Close() {
        if (!f_) return false;
        const long movi_end = std::ftell(f_);
        // Index.
        Put("idx1");
        U32(static_cast<uint32_t>(index_.size() * 16));
        for (const auto& e : index_) {
            Put(e.id);
            U32(e.key ? 0x10 : 0);
            U32(e.offset);
            U32(e.size);
        }
        const long end = std::ftell(f_);
        Patch(4, static_cast<uint32_t>(end - 8));
        Patch(movi_size_at_, static_cast<uint32_t>(movi_end - movi_size_at_ - 4));
        Patch(total_frames_at_, frames_);
        Patch(video_length_at_, frames_);
        Patch(audio_length_at_, static_cast<uint32_t>(audio_frames_));
        const bool ok = std::ferror(f_) == 0;
        std::fclose(f_);
        f_ = nullptr;
        return ok;
    }

private:
    struct Entry { char id[5]; uint32_t offset, size; bool key; };
    void Put(const char* four) { std::fwrite(four, 1, 4, f_); }
    void U32(uint32_t v) { uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)}; std::fwrite(b, 1, 4, f_); }
    void U16(uint16_t v) { uint8_t b[2] = {uint8_t(v), uint8_t(v >> 8)}; std::fwrite(b, 1, 2, f_); }
    void Patch(long at, uint32_t v) {
        const long here = std::ftell(f_);
        std::fseek(f_, at, SEEK_SET);
        U32(v);
        std::fseek(f_, here, SEEK_SET);
    }
    void Chunk(const char* id, const void* data, size_t size, bool key) {
        Entry e;
        std::memcpy(e.id, id, 5);
        e.offset = static_cast<uint32_t>(std::ftell(f_) - movi_start_);
        e.size = static_cast<uint32_t>(size);
        e.key = key;
        index_.push_back(e);
        Put(id);
        U32(static_cast<uint32_t>(size));
        std::fwrite(data, 1, size, f_);
        if (size & 1) std::fputc(0, f_);
    }
    void WriteHeader() {
        const uint32_t rate = static_cast<uint32_t>(std::lround(fps_ * 1000)), scale = 1000;
        Put("RIFF"); U32(0); Put("AVI ");
        Put("LIST"); const long hdrl_size_at = std::ftell(f_); U32(0); Put("hdrl");
        Put("avih"); U32(56);
        U32(static_cast<uint32_t>(1e6 / fps_)); U32(0); U32(0); U32(0x10);
        total_frames_at_ = std::ftell(f_); U32(0);
        U32(0); U32(2); U32(0); U32(static_cast<uint32_t>(w_)); U32(static_cast<uint32_t>(h_)); U32(0); U32(0); U32(0); U32(0);
        // Video stream.
        Put("LIST"); const long v_at = std::ftell(f_); U32(0); Put("strl");
        Put("strh"); U32(56); Put("vids"); Put("MJPG"); U32(0); U16(0); U16(0); U32(0); U32(scale); U32(rate); U32(0);
        video_length_at_ = std::ftell(f_); U32(0);
        U32(0); U32(0xFFFFFFFF); U32(0); U16(0); U16(0); U16(static_cast<uint16_t>(w_)); U16(static_cast<uint16_t>(h_));
        Put("strf"); U32(40); U32(40); U32(static_cast<uint32_t>(w_)); U32(static_cast<uint32_t>(h_)); U16(1); U16(24);
        Put("MJPG"); U32(static_cast<uint32_t>(w_ * h_ * 3)); U32(0); U32(0); U32(0); U32(0);
        Patch(v_at, static_cast<uint32_t>(std::ftell(f_) - v_at - 4));
        // Audio stream.
        Put("LIST"); const long a_at = std::ftell(f_); U32(0); Put("strl");
        Put("strh"); U32(56); Put("auds"); U32(0); U32(0); U16(0); U16(0); U32(0); U32(1); U32(Recorder::kAudioRate); U32(0);
        audio_length_at_ = std::ftell(f_); U32(0);
        U32(0); U32(0xFFFFFFFF); U32(4); U16(0); U16(0); U16(0); U16(0);
        Put("strf"); U32(16); U16(1); U16(2); U32(Recorder::kAudioRate); U32(Recorder::kAudioRate * 4); U16(4); U16(16);
        Patch(a_at, static_cast<uint32_t>(std::ftell(f_) - a_at - 4));
        Patch(hdrl_size_at, static_cast<uint32_t>(std::ftell(f_) - hdrl_size_at - 4));
        Put("LIST"); movi_size_at_ = std::ftell(f_); U32(0); Put("movi");
        movi_start_ = std::ftell(f_) - 4;
    }

    FILE* f_ = nullptr;
    int w_ = 0, h_ = 0;
    double fps_ = 60;
    long movi_size_at_ = 0, movi_start_ = 0, total_frames_at_ = 0, video_length_at_ = 0, audio_length_at_ = 0;
    uint32_t frames_ = 0;
    uint64_t audio_frames_ = 0;
    std::vector<Entry> index_;
};

void JpegOut(void* ctx, void* data, int size) {
    auto* v = static_cast<std::vector<uint8_t>*>(ctx);
    v->insert(v->end(), static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
}

}  // namespace

bool VideoWriter::WriteAvi(const std::string& path) {
    int fw = 0, fh = 0, cw, ch;
    rec_->FirstSize(&fw, &fh);
    Canvas(fw, fh, &cw, &ch);
    Avi avi;
    if (!avi.Open(path, cw, ch, fps_)) return false;
    std::vector<uint8_t> y(static_cast<size_t>(cw) * ch), u(static_cast<size_t>(cw / 2) * (ch / 2)), v(u.size());
    std::vector<uint8_t> rgb(static_cast<size_t>(cw) * ch * 3), jpg, last;
    std::vector<int16_t> pcm(4096 * 2);
    int64_t next_frame = 0;
    for (;;) {
        const bool stopping = stop_;
        bool did = false;
        const int64_t pts = rec_->FillVideo(y.data(), cw, u.data(), v.data(), cw / 2, 1, cw, ch);
        if (pts >= 0) {
            did = true;
            for (int r = 0; r < ch; r++)
                for (int c = 0; c < cw; c++) {
                    const int Y = (y[static_cast<size_t>(r) * cw + c] - 16) * 298;
                    const int U = u[static_cast<size_t>(r / 2) * (cw / 2) + c / 2] - 128;
                    const int V = v[static_cast<size_t>(r / 2) * (cw / 2) + c / 2] - 128;
                    uint8_t* o = &rgb[(static_cast<size_t>(r) * cw + c) * 3];
                    o[0] = static_cast<uint8_t>(std::clamp((Y + 409 * V + 128) >> 8, 0, 255));
                    o[1] = static_cast<uint8_t>(std::clamp((Y - 100 * U - 208 * V + 128) >> 8, 0, 255));
                    o[2] = static_cast<uint8_t>(std::clamp((Y + 516 * U + 128) >> 8, 0, 255));
                }
            jpg.clear();
            stbi_write_jpg_to_func(JpegOut, &jpg, cw, ch, 3, rgb.data(), 85);
            // Constant frame rate: repeat the last picture over dropped frames.
            const int64_t index = std::llround(static_cast<double>(pts) * fps_ / 1e6);
            while (next_frame < index && !last.empty() && index - next_frame < 600) {
                avi.Video(last);
                next_frame++;
            }
            if (index >= next_frame) {
                avi.Video(jpg);
                next_frame = index + 1;
            }
            last.swap(jpg);
        }
        int64_t apts = 0;
        const size_t got = rec_->FillAudio(pcm.data(), pcm.size() / 2, &apts);
        if (got > 0) {
            avi.Audio(pcm.data(), got);
            did = true;
        }
        if (stopping && !did) break;
        if (!did) SleepMs(4);
    }
    return avi.Close();
}

}  // namespace cartucho
