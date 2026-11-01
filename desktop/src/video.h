// Cartucho PC - "Gravar vídeo": the engine's Recorder hands out the game's
// own picture (YUV) and sound (48 kHz); this writes them to a file on a
// thread of its own. Windows: an .mp4 (H.264 + AAC) through Media
// Foundation; elsewhere, or if that is missing, an .avi (Motion JPEG + PCM).
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace cartucho {

class Recorder;

class VideoWriter {
public:
    ~VideoWriter();
    // Starts writing into `dir` a file named after `title`. The recorder
    // must already be started (by the emulation thread).
    void Start(std::shared_ptr<Recorder> rec, double fps, const std::string& dir, const std::string& title);
    // Stops; the file is finished on the writer's thread.
    void Stop();
    bool running() const { return running_; }
    // The result once done ("Vídeo salvo em ..."), taken once.
    std::string TakeMessage();

private:
    void Run(std::string dir, std::string title);
    bool WriteMp4(const std::string& path);
    bool WriteAvi(const std::string& path);
    std::shared_ptr<Recorder> rec_;
    double fps_ = 60;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_{false};
    std::thread thread_;
    std::mutex mutex_;
    std::string message_;
};

}  // namespace cartucho
