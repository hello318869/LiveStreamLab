#pragma once

#include "common/logger/Logger.hpp"
#include "player/demuxer/DemuxTypes.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace lsl::decoder {

// samples 为交错 F32；每声道帧数 = samples.size()/channels，单块最多 1024 帧。
// timestampUs 是本块第一个样本的媒体时间；没有可用时间戳时保留 AV_NOPTS_VALUE。
struct DecodedAudioBlock {
    std::vector<float> samples;
    std::uint64_t serial = 0;
    int sampleRate = 48000;
    int channels = 2;
    std::int64_t timestampUs = AV_NOPTS_VALUE;
};

struct AudioDecodeStats {
    std::uint64_t inputPackets = 0;
    std::uint64_t decodedFrames = 0;
    std::uint64_t outputSamples = 0; // 每声道 PCM 帧数，不乘声道数。
    std::uint64_t decodeErrors = 0;
    std::uint64_t droppedPackets = 0;
    std::uint64_t decodeTimeUs = 0;
    std::uint64_t resets = 0;
    std::uint64_t droppedFrames = 0;
    std::uint64_t discardedSamples = 0;
    std::uint64_t discontinuities = 0;
};

// 单个工作线程独占 codec/swr；不引用视频或执行跨流同步。
// 单次返回至多两秒 PCM；正常 EOF flush，Reset 则直接丢弃旧代次延迟。
class AudioDecoder {
public:
    AudioDecoder(int sampleRate, int channels, logging::Logger& logger);
    ~AudioDecoder();
    AudioDecoder(const AudioDecoder&) = delete;
    AudioDecoder& operator=(const AudioDecoder&) = delete;

    void reset(const demuxer::TrackInfo& track, std::uint64_t serial);
    std::vector<DecodedAudioBlock> decode(const AVPacket& packet, std::uint64_t serial);
    std::vector<DecodedAudioBlock> flush();
    AudioDecodeStats stats() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace lsl::decoder
