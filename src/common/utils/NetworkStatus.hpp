#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace lsl::network {

enum class ConnectionState { Idle, Connecting, Streaming, Reconnecting, Stopping, Error };

inline const char* stateName(ConnectionState state) noexcept {
    switch (state) {
    case ConnectionState::Idle: return "IDLE";
    case ConnectionState::Connecting: return "CONNECTING";
    case ConnectionState::Streaming: return "STREAMING";
    case ConnectionState::Reconnecting: return "RECONNECTING";
    case ConnectionState::Stopping: return "STOPPING";
    case ConnectionState::Error: return "ERROR";
    }
    return "ERROR";
}

struct ConnectionStats {
    ConnectionState state = ConnectionState::Idle;
    std::uint64_t connections = 0;
    std::uint64_t reconnectAttempts = 0;
    std::uint64_t networkErrors = 0;
    std::uint64_t bytes = 0;
    int consecutiveFailures = 0;
    int retryDelayMs = 0;
    int lastErrorCode = 0;
    std::string lastErrorMessage;
    double uptimeSeconds = 0;
    double connectionSeconds = 0; // 当前连接时长；断开后保留上一连接时长。
    double connectedSeconds = 0;  // 各次已连接时长累计，不包含重连等待。
    double idleSeconds = 0;       // 距最近媒体包；新连接尚无媒体时从连接成功计时。
    double averageBitrate = 0;    // payload bit/s；分母包含连接和重连等待时间。
};

// 状态与统计共用一把锁，网络线程、控制线程及日志查询不会读到撕裂快照。
// 不持有 FFmpeg 资源，也不负责等待或网络 I/O；调用者在自己的线程执行这些操作。
class ConnectionMonitor {
public:
    void start() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (startedAt_ || terminal_ || counters_.state == ConnectionState::Stopping ||
            counters_.state == ConnectionState::Error) return;
        startedAt_ = Clock::now();
        counters_.state = ConnectionState::Connecting;
    }

    void setState(ConnectionState state) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (terminal_ || counters_.state == ConnectionState::Error ||
            (counters_.state == ConnectionState::Stopping && isWorking(state))) return;
        if (state != ConnectionState::Streaming) closeConnection(Clock::now());
        counters_.state = state;
    }

    void connected() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!active()) return;
        const auto now = Clock::now();
        closeConnection(now);
        connectionAt_ = now;
        lastMediaAt_ = now;
        ++counters_.connections;
        counters_.state = ConnectionState::Streaming;
        counters_.retryDelayMs = 0;
        // 只有 media() 才清零连续失败预算，不能仅因 TCP/封装头打开成功而重置。
    }

    // progress=false 只累计收到/发送的 payload；例如等关键帧时被下游拒绝的包。
    void media(std::uint64_t bytes, bool progress = true) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (terminal_ || counters_.state == ConnectionState::Error) return;
        counters_.bytes += bytes;
        if (!progress) return;
        lastMediaAt_ = Clock::now();
        if (counters_.state != ConnectionState::Stopping) {
            counters_.consecutiveFailures = 0;
            counters_.retryDelayMs = 0;
        }
        // STOPPING 期间成功排空的尾包仍属于实际吞吐量，但不能恢复工作状态。
        // 迟到的成功回调不改变状态，尤其不能把 STOPPING 恢复成 STREAMING。
    }

    void retry(int failures, int delayMs) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!active()) return;
        closeConnection(Clock::now());
        ++counters_.reconnectAttempts;
        counters_.consecutiveFailures = failures;
        counters_.retryDelayMs = delayMs;
        counters_.state = ConnectionState::Reconnecting;
    }

    void recordError(int code, std::string message) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (terminal_) return;
        ++counters_.networkErrors;
        counters_.lastErrorCode = code;
        counters_.lastErrorMessage = std::move(message);
    }

    void requestStop() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (terminal_ || counters_.state == ConnectionState::Error) return;
        closeConnection(Clock::now());
        counters_.state = ConnectionState::Stopping;
        counters_.retryDelayMs = 0;
    }

    void finish(bool failed = false) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (terminal_) return;
        const auto now = Clock::now();
        closeConnection(now);
        endedAt_ = now;
        terminal_ = true;
        counters_.state = failed || counters_.state == ConnectionState::Error
            ? ConnectionState::Error : ConnectionState::Idle;
        counters_.retryDelayMs = 0;
    }

    ConnectionStats stats() const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto result = counters_;
        const auto now = endedAt_ ? *endedAt_ : Clock::now();
        if (startedAt_) result.uptimeSeconds = elapsed(*startedAt_, now);
        result.connectionSeconds = connectionAt_ ? elapsed(*connectionAt_, now) : lastConnectionSeconds_;
        result.connectedSeconds = connectedSeconds_ + (connectionAt_ ? elapsed(*connectionAt_, now) : 0);
        const auto idleFrom = lastMediaAt_ ? lastMediaAt_ : startedAt_;
        if (idleFrom) result.idleSeconds = elapsed(*idleFrom, now);
        if (result.uptimeSeconds > 0) {
            result.averageBitrate = static_cast<double>(result.bytes) * 8 / result.uptimeSeconds;
        }
        return result;
    }

private:
    using Clock = std::chrono::steady_clock;

    static bool isWorking(ConnectionState state) noexcept {
        return state == ConnectionState::Connecting || state == ConnectionState::Streaming ||
            state == ConnectionState::Reconnecting;
    }
    bool active() const noexcept {
        return !terminal_ && counters_.state != ConnectionState::Stopping &&
            counters_.state != ConnectionState::Error;
    }
    static double elapsed(Clock::time_point begin, Clock::time_point end) noexcept {
        return std::max(0.0, std::chrono::duration<double>(end - begin).count());
    }
    void closeConnection(Clock::time_point now) noexcept {
        if (!connectionAt_) return;
        lastConnectionSeconds_ = elapsed(*connectionAt_, now);
        connectedSeconds_ += lastConnectionSeconds_;
        connectionAt_.reset();
    }

    mutable std::mutex mutex_;
    ConnectionStats counters_;
    std::optional<Clock::time_point> startedAt_;
    std::optional<Clock::time_point> endedAt_;
    std::optional<Clock::time_point> connectionAt_;
    std::optional<Clock::time_point> lastMediaAt_;
    double connectedSeconds_ = 0;
    double lastConnectionSeconds_ = 0;
    bool terminal_ = false;
};

// attempt 从 1 起：base、2*base、4*base……，达到上限后停止相乘，避免整数溢出。
inline int reconnectDelayMs(int baseMs, int maximumMs, std::uint64_t attempt) noexcept {
    if (baseMs <= 0 || maximumMs <= 0) return 0;
    int delay = std::min(baseMs, maximumMs);
    while (attempt > 1 && delay < maximumMs) {
        if (delay > maximumMs / 2) return maximumMs;
        delay *= 2;
        --attempt;
    }
    return delay;
}

} // namespace lsl::network
