#pragma once

#include "common/config/Config.hpp"
#include "player/demuxer/DemuxTypes.hpp"

namespace lsl::demuxer {

struct DemuxStats {
    std::uint64_t videoPackets = 0;
    std::uint64_t audioPackets = 0;
    std::uint64_t ignoredPackets = 0;
    std::uint64_t bytes = 0;
    std::uint64_t connections = 0;
    std::uint64_t reconnects = 0;
    std::uint64_t timestampResets = 0;
    BufferStats buffer;
};

// 单个网络线程独占 AVFormatContext；每轨允许一个消费者。
// start 只允许一次。析构会停止并 join，logger 的生命期必须覆盖本对象。
class Demuxer {
public:
    Demuxer(const config::PlayerConfig& config, logging::Logger& logger);
    ~Demuxer();
    Demuxer(const Demuxer&) = delete;
    Demuxer& operator=(const Demuxer&) = delete;

    void start();
    PacketEvent pop(TrackKind track, int timeoutMs = 20);
    void requestStop() noexcept;
    void join();
    void checkError() const;
    DemuxStats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace lsl::demuxer
