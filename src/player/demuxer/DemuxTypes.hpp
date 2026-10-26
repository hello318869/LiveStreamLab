#pragma once

#include "common/media/FFmpegResources.hpp"

#include <cstdint>
#include <memory>

namespace lsl::demuxer {

enum class TrackKind { Video, Audio };
enum class EventKind { Reset, Packet, End, Stopped, Timeout };

struct TrackInfo {
    int index = -1;
    AVRational timeBase{0, 1};
    media::CodecParametersPtr parameters;
};

// codecpar 深拷贝到快照中，输入上下文关闭后仍可用于创建解码器。
// 会话可改变流编号、编码参数和时间基；消费者不得借用 AVStream。
struct StreamInfo {
    std::uint64_t session = 0;
    TrackInfo video;
    TrackInfo audio;
};

struct PacketEvent {
    EventKind kind = EventKind::Timeout;
    TrackKind track = TrackKind::Video;
    media::PacketPtr packet;
    std::shared_ptr<const StreamInfo> streams;
    // 每次重连、积压清空或时间戳跳变递增。M10/M11 必须在 Reset 时
    // 清理解码器和帧队列，M13 同时重建时钟；旧 serial 的输出不得播放。
    std::uint64_t serial = 0;
    std::int64_t timestampUs = AV_NOPTS_VALUE; // 仅监控；包的原始 PTS/DTS 不修改。
};

struct BufferStats {
    std::uint64_t acceptedPackets = 0;
    std::uint64_t droppedPackets = 0;
    std::uint64_t resets = 0;
    std::uint64_t overflows = 0;
    std::size_t videoPackets = 0;
    std::size_t audioPackets = 0;
    std::size_t videoHighWaterMark = 0;
    std::size_t audioHighWaterMark = 0;
    std::size_t bytes = 0;
    std::size_t peakBytes = 0;
    std::int64_t bufferedMs = 0;
    std::uint64_t serial = 0;
};

} // namespace lsl::demuxer
