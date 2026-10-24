#pragma once

#include "common/media/FFmpegResources.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace lsl::encode {

struct AudioEncoderOptions {
    int sampleRate = 48000;
    int channels = 2;
    int bitrate = 128000;
    AVSampleFormat preferredSampleFormat = AV_SAMPLE_FMT_FLTP;
    bool globalHeader = false;
};

struct AudioEncoderStats {
    std::uint64_t inputFrames = 0;
    std::uint64_t inputSamples = 0; // 每声道样本数；包括上游显式补入的静音。
    std::uint64_t outputPackets = 0;
    std::uint64_t outputBytes = 0;
};

// 单线程 AAC-LC 编码器。先创建编码器，再按实际 sampleFormat / frameSize 配置预处理器。
// 输入与输出时间基均为 1/sampleRate；保留共用媒体时间轴，不把首帧 PTS 强行归零。
// 一帧可能暂不产生包，flush 也可能产生多个包；每个输出包都独占 AVPacket 结构。
class AudioEncoder {
public:
    explicit AudioEncoder(AudioEncoderOptions options);
    ~AudioEncoder();
    AudioEncoder(const AudioEncoder&) = delete;
    AudioEncoder& operator=(const AudioEncoder&) = delete;

    // 常规帧必须含 frameSize() 个样本。支持短尾帧时可送入一次不足帧长的最终帧，
    // 随后只能 flush；中途发生时间跳变的片段仍须由预处理器补齐帧长。
    std::vector<media::PacketPtr> encode(const AVFrame& frame);
    // 取尽编码延迟包，幂等。没有输入时不会凭空生成音频包。
    std::vector<media::PacketPtr> flush();
    AVRational timeBase() const noexcept;
    // 独立参数含 AudioSpecificConfig / initial_padding，封装器可以直接拥有。
    media::CodecParametersPtr parameters() const;
    AVSampleFormat sampleFormat() const noexcept;
    int sampleRate() const noexcept;
    int channels() const noexcept;
    int frameSize() const noexcept;
    bool supportsSmallLastFrame() const noexcept;
    std::string codecName() const;
    AudioEncoderStats stats() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace lsl::encode
