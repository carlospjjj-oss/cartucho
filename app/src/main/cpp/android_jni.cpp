// Cartucho - JNI entry points for com.cartucho.emu.NativeBridge.
//
// One session per process: the game screen runs in its own process
// (":game" in the manifest), so a core's global state never outlives a game.
// Calls marked "GL thread" come from the GLSurfaceView renderer; the others
// are safe from the UI thread.
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android/log.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <jni.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <deque>

#include <chrono>
#include <memory>
#include <mutex>
#include <string>

#include "audio_output.h"
#include "platform.h"
#include "session.h"

namespace cartucho {

// --- Log kept in memory for error reports -----------------------------------
//
// The last lines the frontend and the cores logged, so a report can show what
// happened right before a problem. A fixed array of fixed-size lines, so the
// crash handler can write it out without allocating.

constexpr int kLogLines = 300;
constexpr int kLogLineSize = 200;
char g_log[kLogLines][kLogLineSize];
int g_log_next = 0;  // slot for the next line
int g_log_count = 0;
std::mutex g_log_mutex;
int g_crash_fd = -1;

void PlatformLog(int level, const char* text) {
    static const int prio[] = {ANDROID_LOG_DEBUG, ANDROID_LOG_INFO, ANDROID_LOG_WARN,
                               ANDROID_LOG_ERROR};
    __android_log_write(prio[level & 3], "Cartucho", text);
    static const char tags[] = "DIWE";
    std::lock_guard<std::mutex> lock(g_log_mutex);
    char* slot = g_log[g_log_next];
    std::snprintf(slot, kLogLineSize, "%c %s", tags[level & 3], text);
    g_log_next = (g_log_next + 1) % kLogLines;
    if (g_log_count < kLogLines)
        g_log_count++;
}

std::string RecentLog() {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    std::string out;
    const int start = (g_log_next - g_log_count + kLogLines) % kLogLines;
    for (int i = 0; i < g_log_count; i++) {
        out += g_log[(start + i) % kLogLines];
        out += '\n';
    }
    return out;
}

namespace {

void WriteAll(int fd, const char* s) {
    size_t len = std::strlen(s);
    while (len > 0) {
        const ssize_t n = write(fd, s, len);
        if (n <= 0)
            return;
        s += n;
        len -= static_cast<size_t>(n);
    }
}

// Crash: dump the log to the file opened in advance, then let the default
// handler finish the process (and Android its own tombstone). Only
// async-signal-safe calls; the log is read without its lock, best effort.
struct sigaction g_old_actions[32];

void CrashHandler(int sig, siginfo_t* info, void* context) {
    if (g_crash_fd >= 0) {
        char head[160];
        std::snprintf(head, sizeof(head), "NATIVE CRASH: signal %d (%s) at address %p\n", sig,
                      sig == SIGSEGV   ? "SIGSEGV"
                      : sig == SIGABRT ? "SIGABRT"
                      : sig == SIGBUS  ? "SIGBUS"
                      : sig == SIGILL  ? "SIGILL"
                      : sig == SIGFPE  ? "SIGFPE"
                                       : "?",
                      info ? info->si_addr : nullptr);
        WriteAll(g_crash_fd, head);
        WriteAll(g_crash_fd, "--- last log lines ---\n");
        const int start = (g_log_next - g_log_count + kLogLines) % kLogLines;
        for (int i = 0; i < g_log_count; i++) {
            WriteAll(g_crash_fd, g_log[(start + i) % kLogLines]);
            WriteAll(g_crash_fd, "\n");
        }
        fsync(g_crash_fd);
        close(g_crash_fd);
        g_crash_fd = -1;
    }
    // Restore and re-raise.
    sigaction(sig, &g_old_actions[sig], nullptr);
    raise(sig);
}

void InstallCrashHandler() {
    static bool installed = false;
    if (installed)
        return;
    installed = true;
    struct sigaction sa {};
    sa.sa_sigaction = CrashHandler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    for (int sig : {SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE})
        sigaction(sig, &sa, &g_old_actions[sig]);
}

}  // namespace

retro_proc_address_t PlatformGetProcAddress(const char* sym) {
    if (auto p = reinterpret_cast<retro_proc_address_t>(eglGetProcAddress(sym)))
        return p;
    // Older drivers only return extensions from eglGetProcAddress.
    static void* gles = dlopen("libGLESv3.so", RTLD_NOW | RTLD_LOCAL);
    static void* gles2 = dlopen("libGLESv2.so", RTLD_NOW | RTLD_LOCAL);
    void* p = gles ? dlsym(gles, sym) : nullptr;
    if (!p && gles2)
        p = dlsym(gles2, sym);
    return reinterpret_cast<retro_proc_address_t>(p);
}

}  // namespace cartucho

using cartucho::AudioOutput;
using cartucho::Filter;
using cartucho::Session;
using cartucho::Achievements;

namespace {

std::unique_ptr<Session> g_session;
// The session holds the achievements too; the app's HTTP
// thread reaches them only through this, under the lock.
std::mutex g_ra_mutex;
std::shared_ptr<Achievements> g_ra;

// Video recording, reached the same way by the app's encoder thread.
std::mutex g_rec_mutex;
std::shared_ptr<cartucho::Recorder> g_rec;

std::shared_ptr<cartucho::Recorder> g_preview;

// which: 0 = the player's video, 1 = the library's moving preview.
std::shared_ptr<cartucho::Recorder> CurrentRecorder(int which = 0) {
    std::lock_guard<std::mutex> lock(g_rec_mutex);
    return which == 1 ? g_preview : g_rec;
}

std::shared_ptr<Achievements> CurrentAchievements() {
    std::lock_guard<std::mutex> lock(g_ra_mutex);
    return g_ra;
}
// The TV draws from its own thread; it must never see the session go away
// under it (close() takes this too).
std::mutex g_tv_mutex;
std::unique_ptr<AudioOutput> g_audio;
// The audio stream is started and stopped from the UI thread (activity
// lifecycle) and polled from the render thread.
std::mutex g_audio_mutex;
std::string g_rom, g_save;
int g_rom_fd = -1;

std::string ToStd(JNIEnv* env, jstring s) {
    if (!s)
        return {};
    const char* chars = env->GetStringUTFChars(s, nullptr);
    std::string out(chars ? chars : "");
    if (chars)
        env->ReleaseStringUTFChars(s, chars);
    return out;
}

jstring ToJava(JNIEnv* env, const std::string& s) {
    return env->NewStringUTF(s.c_str());
}

double NowSeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

}  // namespace

#define JNI_FN(ret, name) extern "C" JNIEXPORT ret JNICALL Java_com_cartucho_emu_NativeBridge_##name

// Loads the core library. Returns null on success, else an error message.
JNI_FN(jstring, open)(JNIEnv* env, jobject, jstring core, jstring system_dir, jstring save_dir) {
    g_session = std::make_unique<Session>();
    std::string error;
    if (!g_session->Open(ToStd(env, core), ToStd(env, system_dir), ToStd(env, save_dir),
                         &error)) {
        g_session.reset();
        return ToJava(env, error);
    }
    return nullptr;
}

JNI_FN(void, setGame)(JNIEnv* env, jobject, jstring rom, jstring save, jint fd) {
    g_rom = ToStd(env, rom);
    g_save = ToStd(env, save);
    g_rom_fd = fd;
    if (g_session)
        g_session->host().SetRomFd(fd);
}

JNI_FN(void, setOption)(JNIEnv* env, jobject, jstring key, jstring value) {
    if (g_session)
        g_session->host().SetOption(ToStd(env, key), ToStd(env, value));
}

// Declared options as alternating key / "Description; a|b|c" / current value.
JNI_FN(jobjectArray, options)(JNIEnv* env, jobject) {
    jclass string_class = env->FindClass("java/lang/String");
    if (!g_session)
        return env->NewObjectArray(0, string_class, nullptr);
    const auto declared = g_session->host().DeclaredOptions();
    jobjectArray out =
        env->NewObjectArray(static_cast<jsize>(declared.size() * 3), string_class, nullptr);
    jsize i = 0;
    for (const auto& [key, decl] : declared) {
        env->SetObjectArrayElement(out, i++, ToJava(env, key));
        env->SetObjectArrayElement(out, i++, ToJava(env, decl));
        env->SetObjectArrayElement(out, i++, ToJava(env, g_session->host().OptionValue(key)));
    }
    return out;
}

// GL thread. Returns null on success, else an error message.
JNI_FN(jstring, surfaceCreated)(JNIEnv* env, jobject) {
    if (!g_session)
        return ToJava(env, "No core loaded");
    std::string error;
    if (!g_session->OnSurfaceCreated(g_rom, g_save, &error))
        return ToJava(env, error);
    return nullptr;
}

// GL thread.
JNI_FN(void, surfaceChanged)(JNIEnv*, jobject, jint width, jint height) {
    if (g_session)
        g_session->OnSurfaceChanged(width, height);
}

// GL thread, once per display refresh.
JNI_FN(void, drawFrame)(JNIEnv*, jobject) {
    if (!g_session)
        return;
    std::unique_lock<std::mutex> audio_lock(g_audio_mutex);
    if (g_audio) {
        g_audio->RestartIfNeeded();
        if (g_audio->running() && g_audio->target_queue_frames() > 0)
            g_session->set_audio_fill(static_cast<double>(g_session->host().QueuedAudioFrames()) /
                                      g_audio->target_queue_frames());
        else
            g_session->set_audio_fill(-1.0);
    }
    audio_lock.unlock();
    g_session->Frame(NowSeconds());
}

JNI_FN(void, startAudio)(JNIEnv*, jobject) {
    if (!g_session || !g_session->host().game_loaded())
        return;
    std::lock_guard<std::mutex> lock(g_audio_mutex);
    if (!g_audio)
        g_audio = std::make_unique<AudioOutput>();
    g_audio->Start(&g_session->host());
}

JNI_FN(void, stopAudio)(JNIEnv*, jobject) {
    std::lock_guard<std::mutex> lock(g_audio_mutex);
    if (g_audio)
        g_audio->Stop();
}

JNI_FN(void, setPaused)(JNIEnv*, jobject, jboolean paused) {
    if (g_session)
        g_session->set_paused(paused);
}

JNI_FN(void, setSpeed)(JNIEnv*, jobject, jfloat speed) {
    if (g_session)
        g_session->set_speed(speed);
}

JNI_FN(void, setButtons)(JNIEnv*, jobject, jint mask) {
    if (g_session)
        g_session->host().SetButtons(static_cast<uint32_t>(mask));
}

JNI_FN(void, setAnalog)(JNIEnv*, jobject, jint stick, jfloat x, jfloat y) {
    if (!g_session)
        return;
    auto conv = [](float v) {
        v = v < -1.f ? -1.f : (v > 1.f ? 1.f : v);
        return static_cast<int16_t>(v * 0x7fff);
    };
    g_session->host().SetAnalog(stick, conv(x), conv(y));
}

JNI_FN(void, touch)(JNIEnv*, jobject, jfloat x, jfloat y, jboolean pressed) {
    if (g_session)
        g_session->Touch(x, y, pressed);
}

// GL thread. Returns null on success, else an error message.
JNI_FN(jstring, saveState)(JNIEnv* env, jobject, jstring path) {
    std::string error = "No game is running";
    if (g_session && g_session->host().SaveState(ToStd(env, path), &error))
        return nullptr;
    return ToJava(env, error);
}

// GL thread. Returns null on success, else an error message.
JNI_FN(jstring, loadState)(JNIEnv* env, jobject, jstring path) {
    std::string error = "No game is running";
    if (g_session && g_session->host().LoadState(ToStd(env, path), &error)) {
        g_session->ClearRewind();
        return nullptr;
    }
    return ToJava(env, error);
}

// GL thread.
JNI_FN(void, reset)(JNIEnv*, jobject) {
    if (g_session) {
        g_session->host().Reset();
        g_session->ClearRewind();
        if (auto ra = CurrentAchievements())
            ra->Reset();
    }
}

// GL thread, game loaded: sign in to RetroAchievements and load this game's set.
JNI_FN(void, raStart)(JNIEnv* env, jobject, jstring user, jstring token, jint console) {
    if (!g_session)
        return;
    auto ra = std::make_shared<Achievements>(&g_session->host());
    {
        std::lock_guard<std::mutex> lock(g_ra_mutex);
        g_ra = ra;
    }
    g_session->set_achievements(ra);
    ra->Start(ToStd(env, user), ToStd(env, token), console);
}

// Any thread: next request for the server as {id, url, post data, content type}, or null.
JNI_FN(jobjectArray, raTakeRequest)(JNIEnv* env, jobject) {
    auto ra = CurrentAchievements();
    Achievements::Request r;
    if (!ra || !ra->TakeRequest(&r))
        return nullptr;
    jclass string_class = env->FindClass("java/lang/String");
    jobjectArray out = env->NewObjectArray(4, string_class, nullptr);
    env->SetObjectArrayElement(out, 0, ToJava(env, std::to_string(r.id)));
    env->SetObjectArrayElement(out, 1, ToJava(env, r.url));
    env->SetObjectArrayElement(out, 2, ToJava(env, r.post_data));
    env->SetObjectArrayElement(out, 3, ToJava(env, r.content_type));
    return out;
}

JNI_FN(void, raRespond)(JNIEnv* env, jobject, jint id, jint status, jbyteArray body) {
    auto ra = CurrentAchievements();
    if (!ra)
        return;
    std::string text;
    if (body) {
        const jsize n = env->GetArrayLength(body);
        text.resize(static_cast<size_t>(n));
        env->GetByteArrayRegion(body, 0, n, reinterpret_cast<jbyte*>(text.data()));
    }
    ra->Respond(id, status, std::move(text));
}

// Controller type on port 0 (e.g. the PlayStation DualShock).
JNI_FN(void, setPortDevice)(JNIEnv*, jobject, jint device) {
    if (g_session)
        g_session->host().SetPortDevice(static_cast<unsigned>(device));
}

// GL thread. Discs of a multi-disc game: labels, with the one inserted first
// as "*label"; empty for single-disc games.
JNI_FN(jobjectArray, disks)(JNIEnv* env, jobject) {
    jclass string_class = env->FindClass("java/lang/String");
    std::vector<std::string> labels;
    if (g_session) {
        auto& host = g_session->host();
        const unsigned n = host.DiskCount();
        const int current = host.DiskIndex();
        if (n > 1)
            for (unsigned i = 0; i < n; i++)
                labels.push_back((static_cast<int>(i) == current ? "*" : "") + host.DiskLabel(i));
    }
    jobjectArray out = env->NewObjectArray(static_cast<jsize>(labels.size()), string_class, nullptr);
    for (size_t i = 0; i < labels.size(); i++)
        env->SetObjectArrayElement(out, static_cast<jsize>(i), ToJava(env, labels[i]));
    return out;
}

// GL thread.
JNI_FN(jboolean, setDisk)(JNIEnv*, jobject, jint index) {
    return g_session && g_session->host().SetDisk(static_cast<unsigned>(index));
}

// GL thread: start recording (a new video).
JNI_FN(void, recStart)(JNIEnv*, jobject) {
    if (!g_session)
        return;
    auto rec = CurrentRecorder();
    if (!rec) {
        rec = std::make_shared<cartucho::Recorder>();
        std::lock_guard<std::mutex> lock(g_rec_mutex);
        g_rec = rec;
    }
    g_session->set_recorder(rec);
    rec->Start(g_session->host().fps(), g_session->host().sample_rate());
}

// GL thread: keep the last seconds played for the library's moving preview.
JNI_FN(void, previewStart)(JNIEnv*, jobject) {
    if (!g_session)
        return;
    auto preview = std::make_shared<cartucho::Recorder>();
    {
        std::lock_guard<std::mutex> lock(g_rec_mutex);
        g_preview = preview;
    }
    g_session->set_preview(preview);
    // 6 seconds at 10 pictures per second, at most 320 pixels wide.
    preview->StartRing(g_session->host().fps(), 60, std::max(1, static_cast<int>(g_session->host().fps() / 10 + 0.5)), 320);
}

// Any thread. which: 0 = the player's video, 1 = the preview (stops adding pictures).
JNI_FN(void, recStop)(JNIEnv*, jobject, jint which) {
    if (auto rec = CurrentRecorder(which))
        rec->Stop();
}

// {width, height} of the first recorded picture (0, 0 until there is one).
JNI_FN(jintArray, recFirstSize)(JNIEnv* env, jobject, jint which) {
    int w = 0, h = 0;
    if (auto rec = CurrentRecorder(which))
        rec->FirstSize(&w, &h);
    jintArray out = env->NewIntArray(2);
    const jint v[2] = {w, h};
    env->SetIntArrayRegion(out, 0, 2, v);
    return out;
}

JNI_FN(jboolean, recHasVideo)(JNIEnv*, jobject, jint which) {
    auto rec = CurrentRecorder(which);
    return rec && rec->HasVideo();
}

// Fills the encoder's YUV planes (direct buffers); returns the time in µs or -1.
JNI_FN(jlong, recFillVideo)(JNIEnv* env, jobject, jint which, jobject y, jint y_stride, jobject u, jobject v,
                            jint uv_stride, jint uv_pixel_stride, jint width, jint height) {
    auto rec = CurrentRecorder(which);
    if (!rec)
        return -1;
    auto* py = static_cast<uint8_t*>(env->GetDirectBufferAddress(y));
    auto* pu = static_cast<uint8_t*>(env->GetDirectBufferAddress(u));
    auto* pv = static_cast<uint8_t*>(env->GetDirectBufferAddress(v));
    if (!py || !pu || !pv)
        return -1;
    return rec->FillVideo(py, y_stride, pu, pv, uv_stride, uv_pixel_stride, width, height);
}

JNI_FN(jint, recAudioAvailable)(JNIEnv*, jobject) {
    auto rec = CurrentRecorder();
    return rec ? static_cast<jint>(rec->AudioAvailable()) : 0;
}

// Up to `max_frames` 48 kHz stereo frames into `out` (direct buffer); pts[0] = their time in µs.
JNI_FN(jint, recFillAudio)(JNIEnv* env, jobject, jobject out, jint max_frames, jlongArray pts) {
    auto rec = CurrentRecorder();
    auto* p = static_cast<int16_t*>(env->GetDirectBufferAddress(out));
    if (!rec || !p)
        return 0;
    int64_t t = 0;
    const size_t n = rec->FillAudio(p, static_cast<size_t>(max_frames), &t);
    const jlong tv = t;
    env->SetLongArrayRegion(pts, 0, 1, &tv);
    return static_cast<jint>(n);
}

// The game's achievements, "1|title|description|points[|progress]" (1 = earned).
JNI_FN(jobjectArray, raList)(JNIEnv* env, jobject) {
    jclass string_class = env->FindClass("java/lang/String");
    auto ra = CurrentAchievements();
    const auto lines = ra ? ra->List() : std::vector<std::string>();
    jobjectArray out = env->NewObjectArray(static_cast<jsize>(lines.size()), string_class, nullptr);
    for (size_t i = 0; i < lines.size(); i++)
        env->SetObjectArrayElement(out, static_cast<jsize>(i), ToJava(env, lines[i]));
    return out;
}

// GL thread.
JNI_FN(void, flushSave)(JNIEnv*, jobject) {
    if (g_session)
        g_session->host().FlushSaveRam();
}

// GL thread: unloads the game (writing its save) before the process ends.
JNI_FN(void, close)(JNIEnv*, jobject) {
    std::lock_guard<std::mutex> tv_lock(g_tv_mutex);
    {
        std::lock_guard<std::mutex> lock(g_audio_mutex);
        if (g_audio)
            g_audio->Stop();
        g_audio.reset();
    }
    {
        std::lock_guard<std::mutex> lock(g_ra_mutex);
        g_ra.reset();
    }
    {
        std::lock_guard<std::mutex> lock(g_rec_mutex);
        if (g_rec)
            g_rec->Stop();
        g_rec.reset();
        if (g_preview)
            g_preview->Stop();
        g_preview.reset();
    }
    g_session.reset();
}

JNI_FN(void, setDisplay)(JNIEnv*, jobject, jint filter, jboolean integer_scale, jint valign,
                         jfloat bottom_reserve) {
    if (!g_session)
        return;
    auto& p = g_session->presenter();
    if (filter >= 0 && filter < static_cast<int>(Filter::kCount))
        p.set_filter(static_cast<Filter>(filter));
    p.set_integer_scale(integer_scale);
    p.set_vertical_align(valign);
    p.set_bottom_reserve(bottom_reserve);
}

// [x, y, width, height] of the picture on the surface, top-left origin.
JNI_FN(jintArray, viewport)(JNIEnv* env, jobject) {
    jint v[4] = {0, 0, 0, 0};
    if (g_session) {
        const auto r = g_session->presenter().viewport();
        v[0] = r.x;
        v[1] = r.y;
        v[2] = r.w;
        v[3] = r.h;
    }
    jintArray out = env->NewIntArray(4);
    env->SetIntArrayRegion(out, 0, 4, v);
    return out;
}

JNI_FN(jfloat, measuredFps)(JNIEnv*, jobject) {
    return g_session ? static_cast<jfloat>(g_session->measured_fps()) : 0.f;
}

JNI_FN(jstring, takeMessage)(JNIEnv* env, jobject) {
    if (!g_session)
        return nullptr;
    const std::string m = g_session->host().TakeMessage();
    return m.empty() ? nullptr : ToJava(env, m);
}

JNI_FN(jboolean, shutdownRequested)(JNIEnv*, jobject) {
    return g_session && g_session->host().shutdown_requested();
}

JNI_FN(jboolean, usesHardwareRender)(JNIEnv*, jobject) {
    return g_session && g_session->host().uses_hw_render();
}

// --- Network link -----------------------------------------------------------

JNI_FN(jboolean, linkSupported)(JNIEnv*, jobject) {
    return g_session && g_session->link_supported();
}

// GL thread. mode: 0 GBA cable, 1 GBA wireless adapter, 2 Game Boy.
JNI_FN(void, linkStart)(JNIEnv* env, jobject, jint mode, jboolean server, jint players,
                        jstring host, jint port) {
    if (g_session)
        g_session->LinkStart(mode, server, players, ToStd(env, host), port);
}

// GL thread.
JNI_FN(void, linkStop)(JNIEnv*, jobject) {
    if (g_session)
        g_session->LinkStop();
}

// -1 off, 0 connected, 1 failed, 2 connecting.
JNI_FN(jint, linkState)(JNIEnv*, jobject) {
    return g_session ? g_session->link_state() : -1;
}

JNI_FN(jstring, linkMessage)(JNIEnv* env, jobject) {
    if (!g_session)
        return nullptr;
    const std::string m = g_session->link_message();
    return m.empty() ? nullptr : ToJava(env, m);
}

// --- Error reports ------------------------------------------------------------

// Where a native crash writes the recent log; opened now, since a crashing
// process can no longer open files safely. Delete it after a clean exit.
JNI_FN(void, setCrashFile)(JNIEnv* env, jobject, jstring path) {
    if (cartucho::g_crash_fd >= 0)
        close(cartucho::g_crash_fd);
    cartucho::g_crash_fd = open(ToStd(env, path).c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    cartucho::InstallCrashHandler();
}

JNI_FN(jstring, recentLog)(JNIEnv* env, jobject) {
    return ToJava(env, cartucho::RecentLog());
}

// --- Rewind, cheats, screenshots ------------------------------------------------

JNI_FN(jboolean, rewindAvailable)(JNIEnv*, jobject) {
    return g_session && g_session->rewind_available();
}

JNI_FN(void, setRewindEnabled)(JNIEnv*, jobject, jboolean on) {
    if (g_session)
        g_session->set_rewind_enabled(on);
}

// Any thread: hold to go back in time.
JNI_FN(void, setRewinding)(JNIEnv*, jobject, jboolean on) {
    if (g_session)
        g_session->set_rewinding(on);
}

// GL thread.
JNI_FN(void, cheatReset)(JNIEnv*, jobject) {
    if (g_session)
        g_session->host().CheatReset();
}

// GL thread. Several code lines go joined with '+', as cores expect.
JNI_FN(void, cheatSet)(JNIEnv* env, jobject, jint index, jboolean enabled, jstring code) {
    if (g_session)
        g_session->host().CheatSet(static_cast<unsigned>(index), enabled, ToStd(env, code));
}

// GL thread. [width, height, ARGB pixels...] of the current picture, or null.
JNI_FN(jintArray, screenshot)(JNIEnv* env, jobject) {
    if (!g_session)
        return nullptr;
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    if (!g_session->Screenshot(&rgba, &w, &h) || rgba.size() < static_cast<size_t>(w) * h * 4)
        return nullptr;
    std::vector<jint> out(2 + static_cast<size_t>(w) * h);
    out[0] = w;
    out[1] = h;
    for (size_t i = 0; i < static_cast<size_t>(w) * h; i++) {
        const uint8_t* p = &rgba[i * 4];
        out[2 + i] = static_cast<jint>(0xff000000u | (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | p[2]);
    }
    jintArray arr = env->NewIntArray(static_cast<jsize>(out.size()));
    env->SetIntArrayRegion(arr, 0, static_cast<jsize>(out.size()), out.data());
    return arr;
}

// --- Motion sensors, rumble, microphone -------------------------------------------

// Bits of the sensors the core switched on (1 accel, 2 gyro, 4 light).
JNI_FN(jint, sensorsWanted)(JNIEnv*, jobject) {
    return g_session ? g_session->host().sensors_wanted() : 0;
}

// Accelerometer (m/s^2) and gyroscope (rad/s), already in screen axes.
JNI_FN(void, setMotion)(JNIEnv*, jobject, jfloat ax, jfloat ay, jfloat az, jfloat gx, jfloat gy,
                        jfloat gz) {
    if (!g_session)
        return;
    g_session->host().SetAccel(ax, ay, az);
    g_session->host().SetGyro(gx, gy, gz);
}

JNI_FN(void, setLight)(JNIEnv*, jobject, jfloat lux) {
    if (g_session)
        g_session->host().SetLight(lux);
}

JNI_FN(void, setAccelInG)(JNIEnv*, jobject, jboolean on) {
    if (g_session)
        g_session->host().set_accel_in_g(on);
}

// Rumble strength asked by the core: strong << 16 | weak (0..65535 each).
JNI_FN(jint, rumble)(JNIEnv*, jobject) {
    if (!g_session)
        return 0;
    return static_cast<jint>((static_cast<uint32_t>(g_session->host().rumble_strong()) << 16) |
                             g_session->host().rumble_weak());
}

// Sample rate the core wants while listening, 0 when the microphone is off.
JNI_FN(jint, micRate)(JNIEnv*, jobject) {
    return g_session ? static_cast<jint>(g_session->host().mic_active_rate()) : 0;
}

JNI_FN(void, pushMic)(JNIEnv* env, jobject, jshortArray samples, jint count, jint rate) {
    if (!g_session || count <= 0)
        return;
    jshort* data = env->GetShortArrayElements(samples, nullptr);
    g_session->host().PushMic(reinterpret_cast<const int16_t*>(data), static_cast<size_t>(count),
                              static_cast<unsigned>(rate));
    env->ReleaseShortArrayElements(samples, data, JNI_ABORT);
}

// --- Run-ahead ------------------------------------------------------------------

JNI_FN(void, setRunAhead)(JNIEnv*, jobject, jint frames) {
    if (g_session)
        g_session->set_run_ahead(frames);
}

JNI_FN(jboolean, runAheadPossible)(JNIEnv*, jobject) {
    return g_session && g_session->run_ahead_possible();
}

// --- TV mode ----------------------------------------------------------------------

// Main GL thread. Crops are x0, y0, x1, y1 fractions of the picture.
JNI_FN(void, setTvMode)(JNIEnv* env, jobject, jboolean on, jfloatArray phone, jboolean phone_picture,
                        jfloatArray tv) {
    if (!g_session)
        return;
    auto crop = [env](jfloatArray a) {
        cartucho::Crop c;
        if (a && env->GetArrayLength(a) >= 4) {
            jfloat v[4];
            env->GetFloatArrayRegion(a, 0, 4, v);
            c.x0 = v[0];
            c.y0 = v[1];
            c.x1 = v[2];
            c.y1 = v[3];
        }
        return c;
    };
    g_session->SetTvMode(on, crop(phone), phone_picture, crop(tv));
}

// TV GL thread (a context sharing objects with the main one).
JNI_FN(void, tvSurfaceCreated)(JNIEnv*, jobject) {
    std::lock_guard<std::mutex> lock(g_tv_mutex);
    if (g_session)
        g_session->TvSurfaceCreated();
}

JNI_FN(void, tvSurfaceChanged)(JNIEnv*, jobject, jint width, jint height) {
    std::lock_guard<std::mutex> lock(g_tv_mutex);
    if (g_session)
        g_session->TvSurfaceChanged(width, height);
}

JNI_FN(void, tvDrawFrame)(JNIEnv*, jobject) {
    std::lock_guard<std::mutex> lock(g_tv_mutex);
    if (g_session && g_session->tv_mode())
        g_session->TvDraw();
}
