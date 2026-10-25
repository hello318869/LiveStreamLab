#include "player/sync/AVSynchronizer.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace lsl::sync {
namespace {
using Microseconds = std::chrono::microseconds;

// PTS 可来自网络输入；减法先扩大范围，避免极端时间戳造成有符号溢出。
std::int64_t bounded(long double value) {
    // MSVC long double 与 double 同精度，INT64_MAX 转成浮点后会向上取整。
    // 先判断边界，不能 clamp 完后直接把该浮点边界转回整数。
    if (value <= static_cast<long double>(std::numeric_limits<std::int64_t>::min() + 1))
        return std::numeric_limits<std::int64_t>::min() + 1;
    if (value >= static_cast<long double>(std::numeric_limits<std::int64_t>::max()))
        return std::numeric_limits<std::int64_t>::max();
    return static_cast<std::int64_t>(value);
}
std::int64_t difference(std::int64_t a, std::int64_t b) {
    return bounded(static_cast<long double>(a) - b);
}
std::int64_t advance(std::int64_t pts, std::int64_t elapsed) {
    return bounded(static_cast<long double>(pts) + elapsed);
}
} // namespace

const char* clockName(MasterClock clock) noexcept {
    switch (clock) {
    case MasterClock::Audio: return "audio";
    case MasterClock::External: return "external";
    case MasterClock::Paused: return "paused";
    default: return "waiting";
    }
}

AVSynchronizer::AVSynchronizer(const config::PlayerSyncConfig& options) : options_(options) {}

void AVSynchronizer::reset(std::uint64_t serial, Clock::time_point now) {
    stats_.serial = serial;
    stats_.master = MasterClock::Waiting;
    stats_.errorValid = false;
    stats_.errorUs = 0;
    ++stats_.resets;
    externalWall_ = pendingSince_ = now;
    unavailableSince_.reset();
    pausedSince_.reset();
    driftSince_.reset();
    audioRevision_.reset();
    audioProgressAt_.reset();
    audioProgressPts_ = AV_NOPTS_VALUE;
    externalValid_ = masterValid_ = videoValid_ = pending_ = synchronized_ = videoJump_ = false;
    syntheticAudioAnchored_ = false;
    lastVideoPts_ = AV_NOPTS_VALUE;
}

void AVSynchronizer::setPaused(bool paused, Clock::time_point now) {
    if (paused) {
        if (!pausedSince_) {
            pausedSince_ = now;
            beforePause_ = stats_.master;
        }
        stats_.master = MasterClock::Paused;
        stats_.errorValid = false;
    } else if (pausedSince_) {
        const auto elapsed = now - *pausedSince_;
        externalWall_ += elapsed;
        pendingSince_ += elapsed;
        if (unavailableSince_) *unavailableSince_ += elapsed;
        if (driftSince_) *driftSince_ += elapsed;
        if (audioProgressAt_) *audioProgressAt_ += elapsed;
        pausedSince_.reset();
        stats_.master = beforePause_;
    }
}

void AVSynchronizer::anchorExternal(std::int64_t timestampUs, Clock::time_point now) {
    externalPts_ = timestampUs;
    externalWall_ = now;
    externalValid_ = true;
}

std::int64_t AVSynchronizer::externalTime(Clock::time_point now) const {
    return advance(externalPts_, std::chrono::duration_cast<Microseconds>(now - externalWall_).count());
}

void AVSynchronizer::beginFrame(std::int64_t timestampUs, std::int64_t durationUs, Clock::time_point now) {
    // 暂停期间新取得的首帧也锚定到冻结时刻，恢复时统一平移到当前墙钟。
    now = pausedSince_.value_or(now);
    const auto duration = std::clamp<std::int64_t>(durationUs, 1000, 1000000);
    frameHasPts_ = timestampUs != AV_NOPTS_VALUE;
    videoJump_ = false;
    if (frameHasPts_ && lastVideoPts_ != AV_NOPTS_VALUE) {
        const auto delta = difference(timestampUs, lastVideoPts_);
        videoJump_ = delta < -static_cast<std::int64_t>(options_.toleranceMs) * 1000 ||
            delta > static_cast<std::int64_t>(options_.discontinuityThresholdMs) * 1000;
    }
    // 无 PTS 的视频仍可按帧时长播放，但这部分不能参与真实 AV 误差统计。
    framePts_ = frameHasPts_ ? timestampUs : (videoValid_ ? nextVideoPts_ :
        (masterValid_ ? lastMasterPts_ : 0));
    if (!externalValid_ || (frameHasPts_ && lastVideoPts_ == AV_NOPTS_VALUE)) {
        anchorExternal(framePts_, now);
    }
    if (videoJump_ && !synchronized_) anchorExternal(framePts_, now);
    if (frameHasPts_) lastVideoPts_ = timestampUs;
    nextVideoPts_ = advance(framePts_, duration);
    videoValid_ = pending_ = true;
    countedWait_ = false;
    pendingSince_ = now;
}

SyncAction AVSynchronizer::wait() {
    if (!countedWait_) { ++stats_.waits; countedWait_ = true; }
    return SyncAction::Wait;
}

SyncAction AVSynchronizer::complete(SyncAction action) {
    if (stats_.errorValid) {
        const auto magnitude = bounded(std::abs(static_cast<long double>(stats_.errorUs)));
        ++stats_.samples;
        stats_.meanAbsoluteErrorUs += (static_cast<double>(magnitude) - stats_.meanAbsoluteErrorUs) /
            static_cast<double>(stats_.samples);
        stats_.maxAbsoluteErrorUs = std::max(stats_.maxAbsoluteErrorUs, magnitude);
    }
    if (action == SyncAction::Drop) ++stats_.droppedFrames;
    pending_ = false;
    return action;
}

SyncAction AVSynchronizer::evaluate(const SyncInput& input, Clock::time_point now) {
    stats_.errorValid = false;
    setPaused(input.paused, now);
    if (!pending_) return SyncAction::Wait;
    if (input.paused) return wait();

    const bool sameAudio = input.audio.serial == stats_.serial;
    const bool validAudio = input.audioExpected && !input.audioFinished && sameAudio && input.audio.valid;
    bool runningAudio = validAudio && !input.audio.starved && !input.audio.paused;
    bool changedAudio = false;
    if (validAudio) {
        if (audioRevision_ && *audioRevision_ != input.audio.revision) {
            changedAudio = true;
            driftSince_.reset();
            audioProgressAt_.reset();
            anchorExternal(input.audio.timestampUs, now);
        }
        audioRevision_ = input.audio.revision;
    }
    if (runningAudio) {
        if (!audioProgressAt_ || input.audio.timestampUs != audioProgressPts_) {
            audioProgressPts_ = input.audio.timestampUs;
            audioProgressAt_ = now;
        } else if (now - *audioProgressAt_ >= std::chrono::milliseconds(options_.audioWaitMs)) {
            // 即便设备后端未报告耗尽，停滞的媒体时钟也不能让视频永久等待。
            runningAudio = false;
            if (!unavailableSince_) unavailableSince_ = audioProgressAt_;
        }
    }

    std::int64_t master = 0;
    if (runningAudio) {
        unavailableSince_.reset();
        master = input.audio.timestampUs;
        if (!frameHasPts_ && lastVideoPts_ == AV_NOPTS_VALUE && !syntheticAudioAnchored_) {
            // 视频完全无 PTS 时，只能用首个主时钟位置建立合成节奏，不能拿任意零点比较。
            nextVideoPts_ = advance(master, difference(nextVideoPts_, framePts_));
            framePts_ = master;
            syntheticAudioAnchored_ = true;
        }
        stats_.master = MasterClock::Audio;
        // 随时为将来的外部时钟降级准备连续的锚点，而非另从零起步。
        anchorExternal(master, now);
    } else {
        if (input.audioExpected && !input.audioFinished) {
            if (!unavailableSince_) unavailableSince_ = now;
            if (now - *unavailableSince_ < std::chrono::milliseconds(options_.audioWaitMs)) {
                stats_.master = MasterClock::Waiting;
                return wait(); // 启动/欠载先等一小段，窗口和停止检查仍继续执行。
            }
        }
        if (stats_.master != MasterClock::External) {
            ++stats_.fallbacks;
            // 欠载期间的等待不计入媒体进度；从最近已知位置开始继续。
            if (masterValid_) anchorExternal(lastMasterPts_, now);
            else anchorExternal(framePts_, now);
        }
        stats_.master = MasterClock::External;
        master = externalTime(now);
        if (videoJump_) anchorExternal(master = framePts_, now);
    }
    masterValid_ = true;
    lastMasterPts_ = master;

    const auto error = difference(framePts_, master);
    stats_.errorValid = runningAudio && frameHasPts_;
    stats_.errorUs = stats_.errorValid ? error : 0;
    const auto magnitude = std::abs(static_cast<long double>(error));
    // 视频缺帧造成的 PTS 大跨度可能刚好追上音频；只有实际偏差也很大才共同换代。
    if ((videoJump_ || changedAudio) && synchronized_ && runningAudio && input.canResynchronize &&
        magnitude > static_cast<std::int64_t>(options_.discontinuityThresholdMs) * 1000) {
        ++stats_.recoveries;
        pending_ = false;
        return SyncAction::Resynchronize;
    }
    if (stats_.errorValid && magnitude <= static_cast<std::int64_t>(options_.dropThresholdMs) * 1000) {
        synchronized_ = true;
    }
    // 只在曾经对齐后才检测持续大偏差，合法的初始音画偏移仍按同一媒体时间轴等待。
    if (stats_.errorValid && synchronized_ &&
        magnitude > static_cast<std::int64_t>(options_.discontinuityThresholdMs) * 1000) {
        if (!driftSince_) driftSince_ = now;
        if (input.canResynchronize && now - *driftSince_ >= std::chrono::seconds(1)) {
            ++stats_.recoveries;
            pending_ = false;
            return SyncAction::Resynchronize;
        }
    } else {
        driftSince_.reset();
    }
    if (error > static_cast<std::int64_t>(options_.toleranceMs) * 1000) {
        // EOF 后无法再请求关键帧恢复；孤立的异常远期尾帧不能阻止正常排空退出。
        if (input.audioFinished && !input.canResynchronize && !input.hasNextFrame &&
            error > static_cast<std::int64_t>(options_.discontinuityThresholdMs) * 1000) {
            return complete(SyncAction::Drop);
        }
        // 待显示帧已离开有界队列，也要限制它独占等待槽位的时间；有新帧时更新候选。
        if (input.hasNextFrame && now - pendingSince_ >=
            std::chrono::milliseconds(options_.discontinuityThresholdMs)) return complete(SyncAction::Drop);
        return wait();
    }
    if (error < -static_cast<std::int64_t>(options_.dropThresholdMs) * 1000 && input.hasNextFrame) {
        return complete(SyncAction::Drop);
    }
    return complete(SyncAction::Render);
}

} // namespace lsl::sync
