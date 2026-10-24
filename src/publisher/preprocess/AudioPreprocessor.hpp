#pragma once

#include "common/media/FFmpegResources.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace lsl::preprocess {

struct AudioPreprocessOptions {
    int sampleRate = 48000;
    int channels = 2;
    AVSampleFormat sampleFormat = AV_SAMPLE_FMT_FLTP;
    int frameSize = 1024;
    int discontinuityThresholdMs = 100;
    int gainDb = 0; // 软件 PCM 增益，-60..30 dB；0 不改变采样值，也不修改系统设备音量。
    // 仅控制最终 flush 的不足帧长尾部；编码器支持短尾帧时可关闭补静音。
    bool padFinalFrame = true;
};

struct AudioPreprocessStats {
    std::uint64_t inputFrames = 0;
    std::uint64_t inputSamples = 0;
    std::uint64_t outputFrames = 0;
    std::uint64_t outputSamples = 0;
    std::uint64_t silenceSamples = 0;
    std::uint64_t discardedSamples = 0;
    std::uint64_t discontinuities = 0;
    std::uint64_t paddedSamples = 0;
    std::uint64_t clippedValues = 0; // 增益后饱和的采样值数量，累计所有声道。
};

// 单线程使用。除 clippedValues 外，样本计数均指“每声道样本数”。
// inputSamples / discardedSamples 使用输入采样率，其余样本统计使用输出采样率。
// originUs 必须与视频预处理共用；输出 time_base 为 1 / sampleRate。
class AudioPreprocessor {
public:
    explicit AudioPreprocessor(AudioPreprocessOptions options, std::int64_t originUs = 0);
    ~AudioPreprocessor();
    AudioPreprocessor(const AudioPreprocessor&) = delete;
    AudioPreprocessor& operator=(const AudioPreprocessor&) = delete;

    // 单帧输入至多一秒 PCM，单次返回最多 4096 帧；超限报错，调用方应缩小输入分片。
    std::vector<media::FramePtr> process(const AVFrame& input, std::int64_t timestampUs);
    // 取出重采样延迟与 FIFO 尾部；padFinalFrame 控制尾帧补静音或按原长度输出。
    // 重复调用返回空。
    std::vector<media::FramePtr> flush();
    AudioPreprocessStats stats() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace lsl::preprocess
