#pragma once

#include "common/config/Config.hpp"
#include "player/decoder/AudioDecoder.hpp"

namespace lsl::audio { class IAudioSink; }
namespace lsl::demuxer { class Demuxer; }

namespace lsl::player {

struct AudioPlaybackStats {
    decoder::AudioDecodeStats decode;
    std::uint64_t consumedPackets = 0;
    std::uint64_t serial = 0;
    bool activeTrack = false; // 当前代次已发现音轨且启用了声音输出。
    bool finished = false;
};

// 独立消费音频包，PCM 环满时只等待本线程；不参与视频调度。
// sink==nullptr 表示用户禁用音频，仍及时消费音频包以保持网络缓冲健康。
class AudioPlayback {
public:
    AudioPlayback(const config::PlayerConfig& options, demuxer::Demuxer& input,
                  audio::IAudioSink* sink, logging::Logger& logger);
    ~AudioPlayback();
    AudioPlayback(const AudioPlayback&) = delete;
    AudioPlayback& operator=(const AudioPlayback&) = delete;

    // 控制线程先给所有工作线程发停止，再统一 join；stop 为析构兜底。
    void requestStop() noexcept;
    void join() noexcept;
    void stop() noexcept;
    AudioPlaybackStats stats() const; // 同时向主线程传播工作线程错误。

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace lsl::player
