#include "audio_output.h"

#include "platform.h"

namespace cartucho {

namespace {

size_t PullFromHost(void* user, int16_t* stereo, size_t frames) {
    return static_cast<CoreHost*>(user)->PopAudio(stereo, frames);
}

}  // namespace

AudioOutput::~AudioOutput() {
    Stop();
}

bool AudioOutput::Start(CoreHost* host) {
    if (stream_)
        return true;
    host_ = host;
    AAudioStreamBuilder* builder = nullptr;
    if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK)
        return false;
    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(builder, 2);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setUsage(builder, AAUDIO_USAGE_GAME);
    AAudioStreamBuilder_setContentType(builder, AAUDIO_CONTENT_TYPE_SONIFICATION);
    AAudioStreamBuilder_setDataCallback(builder, &AudioOutput::DataCallback, this);
    AAudioStreamBuilder_setErrorCallback(builder, &AudioOutput::ErrorCallback, this);
    const aaudio_result_t r = AAudioStreamBuilder_openStream(builder, &stream_);
    AAudioStreamBuilder_delete(builder);
    if (r != AAUDIO_OK) {
        PlatformLog(3, AAudio_convertResultToText(r));
        stream_ = nullptr;
        return false;
    }
    device_rate_ = AAudioStream_getSampleRate(stream_);
    resampler_.Reset();
    resampler_.SetRates(host_->sample_rate(), device_rate_);
    // Keep a little more than two device bursts queued at the core rate:
    // enough to ride out a slow frame, little enough to stay responsive.
    const int32_t burst = AAudioStream_getFramesPerBurst(stream_);
    AAudioStream_setBufferSizeInFrames(stream_, burst * 2);
    const double frame_frames = host_->sample_rate() / host_->fps();
    target_queue_ = static_cast<size_t>(frame_frames * 2 + burst * 2 * host_->sample_rate() /
                                                                device_rate_);
    if (AAudioStream_requestStart(stream_) != AAUDIO_OK) {
        AAudioStream_close(stream_);
        stream_ = nullptr;
        return false;
    }
    return true;
}

void AudioOutput::Stop() {
    if (!stream_)
        return;
    AAudioStream_requestStop(stream_);
    AAudioStream_close(stream_);
    stream_ = nullptr;
}

aaudio_data_callback_result_t AudioOutput::DataCallback(AAudioStream*, void* user,
                                                        void* audio_data, int32_t num_frames) {
    auto* self = static_cast<AudioOutput*>(user);
    self->resampler_.Process(&PullFromHost, self->host_, static_cast<int16_t*>(audio_data),
                             num_frames);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

void AudioOutput::ErrorCallback(AAudioStream*, void* user, aaudio_result_t error) {
    // Headphones unplugged, Bluetooth switched...: the stream is dead. It
    // must not be closed from this callback; the render thread reopens it.
    auto* self = static_cast<AudioOutput*>(user);
    if (error == AAUDIO_ERROR_DISCONNECTED)
        self->restart_needed_ = true;
}

bool AudioOutput::RestartIfNeeded() {
    if (!restart_needed_.exchange(false))
        return false;
    CoreHost* host = host_;
    Stop();
    return Start(host);
}

}  // namespace cartucho
