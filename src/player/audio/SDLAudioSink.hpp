#pragma once

#include "player/audio/IAudioSink.hpp"
#include "common/logger/Logger.hpp"

#include <memory>

namespace lsl::audio {

class SDLAudioSink final : public IAudioSink {
public:
    // 在主线程初始化音频子系统；只有首次 reset 才打开播放设备。
    SDLAudioSink(const AudioSinkOptions& options, logging::Logger& logger);
    ~SDLAudioSink() override;
    SDLAudioSink(const SDLAudioSink&) = delete;
    SDLAudioSink& operator=(const SDLAudioSink&) = delete;

    void reset(std::uint64_t serial) override;
    bool enqueue(const decoder::DecodedAudioBlock& block) override;
    void finish() override;
    bool drained() const override;
    void clear(std::uint64_t serial) override;
    void setPaused(bool paused) override;
    void setMuted(bool muted) override;
    void setVolume(int volume) override;
    AudioSinkStats stats() const override;
    AudioClock clock() const override;
    void checkError() const override;
    void stop() noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace lsl::audio
