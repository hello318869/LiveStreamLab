#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace lsl::queue {

struct QueueStats {
    std::size_t size = 0;
    std::size_t capacity = 0;
    std::size_t highWaterMark = 0;
    std::uint64_t pushed = 0;
    std::uint64_t popped = 0;
    std::uint64_t discarded = 0;
    bool closed = false;
    bool aborted = false;
};

// 有界多生产者/多消费者队列；T 可以是独占 AVFrame/AVPacket 的可移动类型。
// close 禁止新写入，允许消费者排空；abort 同时丢弃剩余值。两者均唤醒所有等待者。
// 队列不能重新打开，销毁前由调用者停止并 join 所有使用它的线程。
template<typename T>
class BoundedBlockingQueue {
public:
    explicit BoundedBlockingQueue(std::size_t capacity) : capacity_(capacity) {
        if (capacity == 0) throw std::invalid_argument("queue capacity must be positive");
    }
    BoundedBlockingQueue(const BoundedBlockingQueue&) = delete;
    BoundedBlockingQueue& operator=(const BoundedBlockingQueue&) = delete;

    // 按值接管所有权；关闭时返回 false，传入的值随调用结束释放。
    bool push(T value) {
        std::unique_lock<std::mutex> lock(mutex_);
        writable_.wait(lock, [&] { return closed_ || items_.size() < capacity_; });
        if (closed_) return false;
        enqueue(std::move(value));
        lock.unlock();
        readable_.notify_one();
        return true;
    }

    // tryPush/pushFor 仅在成功时移动 value，超时或关闭时由调用者保留它。
    bool tryPush(T& value) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (closed_ || items_.size() == capacity_) return false;
        enqueue(std::move(value));
        lock.unlock();
        readable_.notify_one();
        return true;
    }

    template<typename Rep, typename Period>
    bool pushFor(T& value, const std::chrono::duration<Rep, Period>& timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!writable_.wait_for(lock, timeout,
                [&] { return closed_ || items_.size() < capacity_; }) || closed_) return false;
        enqueue(std::move(value));
        lock.unlock();
        readable_.notify_one();
        return true;
    }

    // 关闭但尚有数据时仍成功；关闭且排空后返回 false，不改动 value。
    bool pop(T& value) {
        std::unique_lock<std::mutex> lock(mutex_);
        readable_.wait(lock, [&] { return closed_ || !items_.empty(); });
        if (items_.empty()) return false;
        dequeue(value);
        lock.unlock();
        writable_.notify_one();
        return true;
    }

    bool tryPop(T& value) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (items_.empty()) return false;
        dequeue(value);
        lock.unlock();
        writable_.notify_one();
        return true;
    }

    // 在锁内检查队首后再移出，便于按媒体时间或排队年龄淘汰过期帧。
    // predicate 不得调用本队列的方法或保留元素引用；抛异常时尚未修改队列。
    template<typename Predicate>
    bool tryPopIf(T& value, Predicate predicate) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (items_.empty() || !predicate(static_cast<const T&>(items_.front()))) return false;
        dequeue(value);
        lock.unlock();
        writable_.notify_one();
        return true;
    }

    template<typename Rep, typename Period>
    bool popFor(T& value, const std::chrono::duration<Rep, Period>& timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!readable_.wait_for(lock, timeout,
                [&] { return closed_ || !items_.empty(); }) || items_.empty()) return false;
        dequeue(value);
        lock.unlock();
        writable_.notify_one();
        return true;
    }

    void close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        readable_.notify_all();
        writable_.notify_all();
    }

    void abort() {
        std::deque<T> discarded;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
            aborted_ = true;
            discarded_ += items_.size();
            discarded.swap(items_);
        }
        readable_.notify_all();
        writable_.notify_all();
        // 媒体资源在队列锁外析构，避免释放开销延长临界区。
    }

    // 清空不是关闭；适合在外部代次检查的保护下清理旧媒体数据。
    void clear() {
        std::deque<T> discarded;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            discarded_ += items_.size();
            discarded.swap(items_);
        }
        writable_.notify_all();
    }

    QueueStats stats() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return {items_.size(), capacity_, highWaterMark_, pushed_, popped_, discarded_, closed_, aborted_};
    }

private:
    // 只在 mutex_ 已持有时调用。
    void enqueue(T&& value) {
        items_.push_back(std::move(value));
        ++pushed_;
        highWaterMark_ = std::max(highWaterMark_, items_.size());
    }
    void dequeue(T& value) {
        value = std::move(items_.front());
        items_.pop_front();
        ++popped_;
    }

    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable readable_;
    std::condition_variable writable_;
    std::deque<T> items_;
    std::size_t highWaterMark_ = 0;
    std::uint64_t pushed_ = 0;
    std::uint64_t popped_ = 0;
    std::uint64_t discarded_ = 0;
    bool closed_ = false;
    bool aborted_ = false;
};

} // namespace lsl::queue
