#pragma once

#include "common/config/Config.hpp"
#include "common/logger/Logger.hpp"
#include "common/stats/PerformanceStats.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace lsl::pipeline {

enum class PublisherSessionState { Idle, Starting, Previewing, Streaming, Reconnecting, Stopping, Stopped, Failed };

// RGB24 独占副本，stride 为每行字节数。按比例限制在 960x540 内，最多约 8 FPS。
// sequence 在每次会话中从 1 递增；界面自行复制到 QImage，不接触媒体帧。
struct PublisherPreviewFrame {
    int width = 0;
    int height = 0;
    int stride = 0;
    std::uint64_t sequence = 0;
    std::vector<std::uint8_t> pixels;
};

struct PublisherSessionSnapshot {
    PublisherSessionState state = PublisherSessionState::Idle;
    // 首次观察到成功媒体发送后计时（约 250 ms 采样误差），重连期间继续累计。
    double liveSeconds = 0;
    double videoFps = 0; // 采样区间内实际送入视频编码器的帧数 / 秒。
    double bitrate = 0; // 采样区间内成功发送的编码 payload bit/s。
    std::string error;
    stats::PublisherPerformance performance;
};

// logger 必须比 session 活得更久。析构请求停止并等待媒体资源按流水线顺序释放。
class PublisherSession {
public:
    explicit PublisherSession(logging::Logger& logger);
    ~PublisherSession();
    PublisherSession(const PublisherSession&) = delete;
    PublisherSession& operator=(const PublisherSession&) = delete;

    // 复制并验证配置；已在运行或配置/线程启动失败时返回 false。
    bool start(const config::PublisherConfig& config);
    // 仅设备视频预览：不采集音频、不连接 RTMP、不写本地文件。
    // 仍沿用视频预处理/编码链路，以复用相同的线程和停止顺序。
    bool startPreview(const config::PublisherConfig& config);
    void requestStop() noexcept; // 只提交请求，不在 UI 线程等待退出。
    PublisherSessionSnapshot snapshot() const;
    // 后台只保留最新一帧；尚未产生预览时为 nullptr，新会话会清空。
    std::shared_ptr<const PublisherPreviewFrame> preview() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace lsl::pipeline
