#pragma once

#include "common/media/FFmpegResources.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace lsl::encoder {

struct VideoEncoderOptions {
    int width = 1280;
    int height = 720;
    int fps = 25;
    int bitrate = 2000000;
    int gopSize = 50;
    std::string preset = "veryfast"; // libx264 的编码速度预设；libopenh264 使用自身默认策略。
    bool lowLatency = true;
    bool globalHeader = false;     // 写 MP4 等容器时由输出格式的 AVFMT_GLOBALHEADER 决定。
    // 用首个预处理帧的描述初始化编码器，使 H.264 VUI / 容器参数与实际像素一致。
    AVColorRange colorRange = AVCOL_RANGE_UNSPECIFIED;
    AVColorSpace colorSpace = AVCOL_SPC_UNSPECIFIED;
    AVColorPrimaries colorPrimaries = AVCOL_PRI_UNSPECIFIED;
    AVColorTransferCharacteristic colorTransfer = AVCOL_TRC_UNSPECIFIED;
    AVChromaLocation chromaLocation = AVCHROMA_LOC_UNSPECIFIED;
};

struct VideoEncoderStats {
    std::uint64_t inputFrames = 0;
    std::uint64_t outputPackets = 0;
    std::uint64_t outputBytes = 0;
    std::uint64_t keyPackets = 0;
};

// 单线程使用，接收 VideoPreprocessor 输出的指定尺寸 YUV420P 帧。
// 输入 PTS / duration、输出 PTS / DTS / duration 均以 timeBase()（1/fps）为单位。
// 不能按一次 encode 对应一个包来消费：内部延迟会产生零包，排空时也可能产生多个包。
// 每个输出独占 AVPacket 结构，可直接 move 进封装队列；交接后编码器不再修改它。
class VideoEncoder {
public:
    explicit VideoEncoder(VideoEncoderOptions options);
    ~VideoEncoder();
    VideoEncoder(const VideoEncoder&) = delete;
    VideoEncoder& operator=(const VideoEncoder&) = delete;

    std::vector<media::PacketPtr> encode(const AVFrame& frame);
    // 先发送一次空帧，再取尽延迟包。幂等；调用后不再接受新帧。
    std::vector<media::PacketPtr> flush();
    AVRational timeBase() const noexcept;
    // 返回独立 codec parameters（含 extradata），供后续封装器拥有。
    media::CodecParametersPtr parameters() const;
    VideoEncoderStats stats() const noexcept;
    std::string codecName() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace lsl::encoder
