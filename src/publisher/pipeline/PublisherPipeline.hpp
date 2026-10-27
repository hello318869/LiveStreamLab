#pragma once

#include "common/config/CommandLine.hpp"
#include "common/stats/PerformanceStats.hpp"
#include "common/utils/NetworkStatus.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>

namespace lsl::pipeline {

// 流水线只发布独立计数快照，不把媒体资源交给控制界面。
struct PublisherPipelineStats {
    std::uint64_t encodedVideoFrames = 0;
    std::uint64_t sentPackets = 0;
    std::uint64_t sentBytes = 0; // 成功发送的编码 payload，不含 FLV/RTMP/TCP 开销。
    network::ConnectionState networkState = network::ConnectionState::Idle;
    stats::PublisherPerformance performance;
    bool finished = false; // 所有流水线/RTMP 线程 join 后发布的末次快照。
};

using PublisherStatsCallback = std::function<void(const PublisherPipelineStats&)>;
struct PublisherPreviewFrame;
using PublisherPreviewCallback = std::function<void(std::shared_ptr<const PublisherPreviewFrame>)>;

// 可嵌入的核心：不安装进程信号处理器；停止标志由本次会话独占。
// callback 在协调线程约每 250 ms 调用，媒体工作线程仍由流水线停止并 join。
int runPublisherPipeline(const config::PublisherConfig& config, logging::Logger& logger,
                         const std::atomic<bool>& stop,
                         const PublisherStatsCallback& callback = {},
                         const PublisherPreviewCallback& previewCallback = {});

// 保留命令行设备查询、Ctrl+C/SIGTERM 与既有运行方式。
int runPublisher(const config::PublisherConfig& config,
                 const config::CommandLineOptions& options, logging::Logger& logger);
} // namespace lsl::pipeline
