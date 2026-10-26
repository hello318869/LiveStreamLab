#pragma once

#include "player/decoder/AudioDecoder.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace lsl::audio {

struct AudioSinkOptions {
    std::string device = "default";
    int sampleRate = 48000;
    int channels = 2;
    int bufferMs = 100;
    int volume = 100;
    bool muted = false;
    bool paused = false;
};

struct AudioSinkStats {
    std::uint64_t submittedFrames = 0; // 回调已交给 SDL 流的真实媒体帧，不是硬件已播放帧。
    std::uint64_t underruns = 0;
    std::uint64_t droppedFrames = 0;
    std::uint64_t callbacks = 0;
    std::uint64_t serial = 0;
    std::size_t queuedFrames = 0; // 仅应用 PCM 环；SDL 内部缓存另列。
    std::size_t highWaterMarkFrames = 0; // 会话内应用 PCM 环的最大积压，换代不清零。
    std::size_t capacityFrames = 0; // 至少 4096 帧，bufferMs 不是绝对容量上限。
    std::size_t sdlQueuedBytes = 0;
    std::size_t sdlAvailableBytes = 0;
    bool paused = false;
    bool muted = false;
    int volume = 100;
    bool finished = false;
    bool opened = false;
};

struct AudioClock {
    bool valid = false;
    std::int64_t timestampUs = 0;
    std::uint64_t serial = 0;
    std::uint64_t revision = 0; // 同 serial 内的 PCM 时间戳跳变；变化后须重建同步基准。
    std::int64_t bufferedUs = 0; // 已提交媒体中估计尚未播放的时长，不包含应用 PCM 环。
    std::int64_t deviceLatencyUs = 0; // 根据设备处理周期估计，并非物理播放位置测量。
    bool paused = false;
    bool starved = false; // 应用环空、SDL 无可输出样本，且消费停滞超过设备尾部保护期。
    bool finished = false;
};

class IAudioSink {
public:
    virtual ~IAudioSink() = default;
    // 唯一音频生产者调用 reset/enqueue/finish；控制与查询可以从主线程并发调用。
    virtual void reset(std::uint64_t serial) = 0;
    virtual bool enqueue(const decoder::DecodedAudioBlock& block) = 0;
    virtual void finish() = 0;
    virtual bool drained() const = 0;
    // clear 只接受更新的 serial；同代/更旧请求无操作，避免抹去已提交的新代媒体。
    virtual void clear(std::uint64_t serial) = 0;
    virtual void setPaused(bool paused) = 0;
    virtual void setMuted(bool muted) = 0;
    virtual void setVolume(int volume) = 0;
    virtual AudioSinkStats stats() const = 0;
    // 基于媒体 PTS、SDL 缓存和设备处理周期估计的播放位置；回调间按单调时钟推进。
    // 欠载到达已提交媒体终点后冻结，暂停期间冻结；不是声卡/扬声器的精确播放位置。
    virtual AudioClock clock() const = 0;
    virtual void checkError() const = 0;
    // 停止生产线程并 join 后调用；可重复调用，析构也会停止输出。
    virtual void stop() noexcept = 0;
};

} // namespace lsl::audio
