#pragma once

#include "common/config/Config.hpp"
#include "common/logger/Logger.hpp"
#include "common/stats/PerformanceStats.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace lsl::player {

enum class PlayerSessionState { Idle, Connecting, Buffering, Playing, Paused, Reconnecting, Stopping, Stopped, Failed };

// RGB24 方形像素，转换时已按源 SAR 修正显示比例；最大 1920x1080。
// stride 为每行字节数，pixels 可含对齐/尾部 padding，界面按 stride 复制。
struct PlayerVideoFrame {
    int width = 0;
    int height = 0;
    int stride = 0;
    std::uint64_t sequence = 0;
    std::int64_t capturedUnixUs = 0; // 仅本应用 UDU SEI 中的设备视频采集输出时间，0 表示未携带。
    std::vector<std::uint8_t> pixels;
};

struct PlayerSessionSnapshot {
    PlayerSessionState state = PlayerSessionState::Idle;
    bool muted = false;
    bool paused = false;
    bool hasVideo = false;
    bool hasAudio = false;
    int volume = 100;
    // 首次实际音频提交/同步显示后的会话经过时间；暂停冻结、重连累计，非媒体 PTS。
    double playbackSeconds = 0;
    std::string error;
    std::uint64_t frameRevision = 0; // 新帧或清帧均递增，跨多次 start 保持递增。
    stats::PlayerPerformance performance;
};

class PlayerSession {
public:
    // 构造、start、析构必须由应用主线程调用，以保证 SDL 音频子系统生命周期。
    explicit PlayerSession(logging::Logger& logger);
    ~PlayerSession();
    PlayerSession(const PlayerSession&) = delete;
    PlayerSession& operator=(const PlayerSession&) = delete;

    bool start(const config::PlayerConfig& config);
    void requestStop() noexcept;
    void setMuted(bool muted);
    void setVolume(int volume);
    void setPaused(bool paused);
    PlayerSessionSnapshot snapshot() const;
    std::shared_ptr<const PlayerVideoFrame> frame() const;
    // Qt 主线程在对应新帧完成绘制后调用；重复重绘不重复采样，过期代次自动忽略。
    void notePresented(std::uint64_t sequence, std::int64_t capturedUnixUs);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace lsl::player
