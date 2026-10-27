#pragma once

#include "common/config/Config.hpp"
#include "common/queue/BoundedBlockingQueue.hpp"
#include "player/decoder/VideoDecoder.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <optional>

namespace lsl::player {

// 已解码帧可能比音频播放位置超前；容量不足时背压解码线程，不能依据
// 解码后的墙钟年龄或队列 PTS 跨度丢掉尚未到显示时间的帧。
// 主线程通过 A/V 同步丢迟到帧；锁序固定为 mutex_ -> queue_。
class VideoFrameQueue {
public:
    struct Snapshot {
        decoder::VideoDecodeStats decode;
        std::uint64_t consumedPackets = 0;
        std::uint64_t dropped = 0;
        std::size_t queued = 0;
        std::size_t highWaterMark = 0;
        std::size_t bytes = 0;
        std::size_t peakBytes = 0;
        bool finished = false;
    };
    explicit VideoFrameQueue(const config::PlayerConfig& options)
        : queue_(static_cast<std::size_t>(options.pipeline.videoQueueCapacity)) {}

    void reset(std::uint64_t serial) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state_.dropped += queue_.stats().size;
            queue_.clear();
            state_.bytes = 0;
            serial_ = serial;
        }
        writable_.notify_all();
    }

    // current() 检查网络代次；短等期间主动检查它，避免视频线程在满队列里
    // 等待而无法取得后续 Reset。current 不得重入本队列。
    template<typename Current>
    void push(decoder::DecodedVideoFrame frame, Current current) {
        std::unique_lock<std::mutex> lock(mutex_);
        const auto bytes = frameBytes(*frame.frame);
        if (bytes > maxBytes) { ++state_.dropped; return; }
        Item item{std::move(frame), bytes};
        for (;;) {
            if (stopped_ || state_.finished || item.frame.serial != serial_ || !current()) {
                ++state_.dropped;
                return;
            }
            if (bytes <= maxBytes - state_.bytes && queue_.tryPush(item)) {
                state_.bytes += bytes;
                state_.peakBytes = std::max(state_.peakBytes, state_.bytes);
                return;
            }
            writable_.wait_for(lock, std::chrono::milliseconds(10));
        }
    }

    std::optional<decoder::DecodedVideoFrame> pop() {
        std::optional<decoder::DecodedVideoFrame> result;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            Item item;
            while (queue_.tryPop(item)) {
                state_.bytes -= item.bytes;
                if (item.frame.serial != serial_) { ++state_.dropped; continue; }
                result = std::move(item.frame);
                break;
            }
        }
        writable_.notify_one();
        return result;
    }
    void consumed() {
        std::lock_guard<std::mutex> lock(mutex_);
        ++state_.consumedPackets;
    }
    void update(const decoder::VideoDecodeStats& stats) {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.decode = stats;
    }
    void finish(std::exception_ptr failure = {}) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            failure_ = failure;
            state_.finished = true;
            queue_.close(); // 正常 EOF 仍允许主线程排空末尾帧。
        }
        writable_.notify_all();
    }
    void stop() noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopped_ = true;
            state_.dropped += queue_.stats().size;
            queue_.abort();
            state_.bytes = 0;
        }
        writable_.notify_all();
    }
    Snapshot snapshot(bool checkFailure = true) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (checkFailure && failure_) std::rethrow_exception(failure_);
        auto result = state_;
        const auto queue = queue_.stats();
        result.queued = queue.size;
        result.highWaterMark = queue.highWaterMark;
        return result;
    }

private:
    static constexpr std::size_t maxBytes = 64 * 1024 * 1024;
    struct Item {
        decoder::DecodedVideoFrame frame;
        std::size_t bytes = 0;
    };
    static std::size_t frameBytes(const AVFrame& frame) {
        std::size_t result = 0;
        const auto add = [&](const AVBufferRef* buffer) {
            if (buffer && result <= maxBytes) result += std::min(buffer->size, maxBytes + 1 - result);
        };
        for (const auto* buffer : frame.buf) add(buffer);
        for (int i = 0; i < frame.nb_extended_buf; ++i) add(frame.extended_buf[i]);
        return result;
    }
    mutable std::mutex mutex_;
    std::condition_variable writable_;
    queue::BoundedBlockingQueue<Item> queue_;
    Snapshot state_;
    std::exception_ptr failure_;
    std::uint64_t serial_ = 0;
    bool stopped_ = false;
};

} // namespace lsl::player
