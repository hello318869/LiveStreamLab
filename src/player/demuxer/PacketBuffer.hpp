#pragma once

#include "common/config/Config.hpp"
#include "player/demuxer/DemuxTypes.hpp"

namespace lsl::demuxer {

// 单个拉流生产者与两个解码消费者之间的交接点。两路共同使用 serial，
// 积压恢复时同时丢弃音视频，防止只清一路造成旧音频/新画面混用。
class PacketBuffer {
public:
    explicit PacketBuffer(const config::DemuxConfig& options);
    ~PacketBuffer();
    PacketBuffer(const PacketBuffer&) = delete;
    PacketBuffer& operator=(const PacketBuffer&) = delete;

    void reset(std::shared_ptr<const StreamInfo> streams);
    void invalidate();
    // 同步器按观察到的代次请求双轨换代；并发重连、停止或 EOF 后不再清理。
    bool resynchronize(std::uint64_t expectedSerial);
    bool push(TrackKind kind, media::PacketPtr packet, std::int64_t timestampUs);
    PacketEvent pop(TrackKind kind, int timeoutMs);
    void finish();
    void stop() noexcept;
    BufferStats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace lsl::demuxer
