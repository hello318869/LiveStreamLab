#pragma once

#include "common/logger/Logger.hpp"
#include "player/demuxer/DemuxTypes.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace lsl::decoder {

// 帧结构独占，像素缓冲由 FFmpeg 引用计数管理；交接后解码器不再修改它。
// timestampUs 是显示顺序的媒体时间，缺失时保留 AV_NOPTS_VALUE，交由播放器补时。
struct DecodedVideoFrame {
    media::FramePtr frame;
    std::uint64_t serial = 0;
    std::int64_t timestampUs = AV_NOPTS_VALUE;
    std::int64_t durationUs = 0;
};

struct VideoDecodeStats {
    std::uint64_t inputPackets = 0;
    std::uint64_t decodedFrames = 0;
    std::uint64_t decodeErrors = 0;
    std::uint64_t droppedFrames = 0;
    std::uint64_t droppedPackets = 0; // 等待关键帧、过时代次或坏包时尚未产生帧。
    std::uint64_t decodeTimeUs = 0; // send/receive 与像素适配的累计耗时，可计算平均每帧成本。
    std::uint64_t lastDecodeTimeUs = 0;
    std::uint64_t maxDecodeTimeUs = 0;
    std::uint64_t resets = 0;
};

// 仅由一个解码消费者调用；Reset 后旧 serial 的包不再进入 codec context。
// reset({}, serial) 可用于失去视频流的会话，此时 decode/flush 都不产生帧。
class VideoDecoder {
public:
    explicit VideoDecoder(logging::Logger& logger);
    ~VideoDecoder();
    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    void reset(const demuxer::TrackInfo& track, std::uint64_t serial);
    std::vector<DecodedVideoFrame> decode(const AVPacket& packet, std::uint64_t serial);
    std::vector<DecodedVideoFrame> flush();
    VideoDecodeStats stats() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace lsl::decoder
