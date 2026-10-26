#pragma once

#include "common/config/Config.hpp"
#include "common/media/FFmpegResources.hpp"
#include "common/utils/NetworkStatus.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace lsl::rtmp {

struct PublisherStats {
    network::ConnectionStats network;
    // 成功交给 av_interleaved_write_frame 的编码 payload，不含 FLV/RTMP/TCP 开销。
    std::uint64_t sentBytes = 0;
    std::uint64_t sentPackets = 0;
    std::uint64_t droppedPackets = 0;
    std::uint64_t networkErrors = 0;
    std::uint64_t reconnects = 0;
    std::size_t videoQueued = 0;
    std::size_t audioQueued = 0;
    std::size_t queuedPackets = 0;
    std::size_t queueHighWaterMark = 0; // 会话期间两轨合计的最大排队包数。
    double durationSeconds = 0;
    double averageBitrate = 0;
};

// 生产者只登记参数、移动独占包；所有封装和网络 I/O 均在内部唯一复用线程。
// set*Parameters 必须先于该轨的 submit*；start 按输入实际存在的轨调用一次。
// 各轨允许不同生产线程，同一轨的参数/包/finish 必须保持顺序。
class RtmpPublisher {
public:
    RtmpPublisher(const config::PublisherConfig& config, logging::Logger& logger);
    ~RtmpPublisher();
    RtmpPublisher(const RtmpPublisher&) = delete;
    RtmpPublisher& operator=(const RtmpPublisher&) = delete;

    void start(bool video, bool audio);
    void setVideoParameters(const AVCodecParameters& parameters, AVRational timeBase);
    void setAudioParameters(const AVCodecParameters& parameters, AVRational timeBase);
    void submitVideo(media::PacketPtr packet);
    void submitAudio(media::PacketPtr packet);
    void finishVideo();
    void finishAudio();
    // EOF 排空并写尾，最长给予一次 writeTimeoutMs 的总排空预算；幂等。
    void finish();
    // Ctrl+C/异常：立即中断网络，丢弃网络尾包；随后 submit 安静丢包。
    // 不影响调用者独立持有的本地录制文件及编码器排空。
    void requestStop() noexcept;
    void checkError() const;
    PublisherStats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace lsl::rtmp
