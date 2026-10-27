#pragma once

#include "common/config/Config.hpp"
#include "player/audio/IAudioSink.hpp"

#include <chrono>
#include <cstdint>
#include <optional>

namespace lsl::sync {

enum class SyncAction { Wait, Render, Drop, Resynchronize };
enum class MasterClock { Waiting, Audio, External, Paused };
const char* clockName(MasterClock clock) noexcept;

struct SyncStats {
    std::uint64_t serial = 0;
    MasterClock master = MasterClock::Waiting;
    bool errorValid = false;
    std::int64_t errorUs = 0; // video PTS - audio estimated playback position
    std::uint64_t samples = 0; // 每个完成调度的视频帧最多计一次，不按主循环轮询计数。
    double meanAbsoluteErrorUs = 0;
    std::int64_t maxAbsoluteErrorUs = 0;
    std::uint64_t waits = 0;
    std::uint64_t droppedFrames = 0;
    std::uint64_t fallbacks = 0;
    std::uint64_t resets = 0;
    std::uint64_t recoveries = 0;
};

struct SyncInput {
    audio::AudioClock audio;
    bool audioExpected = false;
    bool audioFinished = false; // 已排空，不只是解码线程已收到 EOF。
    bool paused = false;
    bool hasNextFrame = false;
    bool canResynchronize = true; // 任一解码线程已经结束时不能再要求它处理 Reset。
};

// 由播放协调线程独占；只决定帧的去留，不拥有帧、不休眠、不调用设备或网络。
// 输入的两路时间戳已由解码器 av_rescale_q 到 AV_TIME_BASE 的微秒，不能分别归零。
class AVSynchronizer {
public:
    using Clock = std::chrono::steady_clock;
    explicit AVSynchronizer(const config::PlayerSyncConfig& options);
    void reset(std::uint64_t serial, Clock::time_point now);
    void setPaused(bool paused, Clock::time_point now); // 无视频帧时也必须观察暂停区间。
    void beginFrame(std::int64_t timestampUs, std::int64_t durationUs, Clock::time_point now);
    SyncAction evaluate(const SyncInput& input, Clock::time_point now);
    SyncStats stats() const noexcept { return stats_; }

private:
    void anchorExternal(std::int64_t timestampUs, Clock::time_point now);
    std::int64_t externalTime(Clock::time_point now) const;
    SyncAction complete(SyncAction action);
    SyncAction wait();

    config::PlayerSyncConfig options_;
    SyncStats stats_;
    Clock::time_point externalWall_{};
    Clock::time_point pendingSince_{};
    std::optional<Clock::time_point> unavailableSince_;
    std::optional<Clock::time_point> pausedSince_;
    std::optional<Clock::time_point> driftSince_;
    std::optional<Clock::time_point> audioProgressAt_;
    std::optional<std::uint64_t> audioRevision_;
    std::int64_t externalPts_ = 0;
    std::int64_t lastMasterPts_ = 0;
    std::int64_t framePts_ = 0;
    std::int64_t nextVideoPts_ = 0;
    std::int64_t lastVideoPts_ = AV_NOPTS_VALUE;
    std::int64_t audioProgressPts_ = AV_NOPTS_VALUE;
    MasterClock beforePause_ = MasterClock::Waiting;
    bool externalValid_ = false;
    bool masterValid_ = false;
    bool videoValid_ = false;
    bool frameHasPts_ = false;
    bool pending_ = false;
    bool countedWait_ = false;
    bool synchronized_ = false;
    bool videoJump_ = false;
    bool syntheticAudioAnchored_ = false;
};

} // namespace lsl::sync
