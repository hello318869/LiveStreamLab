#pragma once

#include <cstddef>
#include <cstdint>

namespace lsl::stats {

// 各行由所属队列定义单位，不把帧、音频块、压缩包或 PCM 样本相加。
struct QueueDepth {
    std::size_t size = 0;
    std::size_t capacity = 0;
    std::size_t highWaterMark = 0;
};

struct PublisherPerformance {
    bool sampled = false;
    double captureFps = 0;
    double encodeFps = 0;
    double sendBitrate = 0; // 成功交给封装/网络的编码 payload bit/s，不含协议开销。
    std::uint64_t capturedVideoFrames = 0;
    std::uint64_t encodedVideoFrames = 0;
    std::uint64_t sentBytes = 0;
    QueueDepth rawVideo; // 帧
    QueueDepth rawAudio; // 音频块
    QueueDepth outputEvents; // 参数/编码包/结束事件
    QueueDepth networkPackets; // 音视频编码包共用容量
    std::size_t networkVideoPackets = 0;
    std::size_t networkAudioPackets = 0;
    std::uint64_t rawVideoDrops = 0;
    std::uint64_t preprocessVideoDrops = 0;
    std::uint64_t droppedVideoFrames = 0; // 上面两类的合计，不包含压缩包或未生成的补帧槽。
    std::uint64_t droppedPackets = 0;
    std::uint64_t abortedOutputEvents = 0; // 输出队列中止时丢弃的混合事件，不能计为视频帧或压缩包。
};

enum class LatencyStatus { NoTimestamp, Estimated, ClockError, Inactive };

struct PlayerPerformance {
    bool sampled = false;
    double decodeFps = 0;
    double receiveBitrate = 0; // 收到的编码 payload bit/s，不含协议开销。
    std::uint64_t decodedVideoFrames = 0;
    std::uint64_t receivedBytes = 0;
    QueueDepth videoPackets; // 压缩包
    QueueDepth audioPackets; // 压缩包
    QueueDepth videoFrames; // 解码后视频帧
    QueueDepth pcm; // 每声道 PCM 样本帧
    double pcmMilliseconds = 0;
    std::uint64_t droppedVideoFrames = 0;
    std::uint64_t droppedPackets = 0;
    bool syncValid = false;
    double syncErrorMs = 0; // 视频 PTS - 音频估计播放位置；正值表示视频超前。
    LatencyStatus latencyStatus = LatencyStatus::Inactive;
    double endToEndMs = 0; // 带采集 Unix 时间戳的视频帧到实际显示回调，依赖收发系统时钟一致。
};

} // namespace lsl::stats
