#include "player/demuxer/PacketBuffer.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <utility>

namespace lsl::demuxer {
namespace {
using Clock = std::chrono::steady_clock;

std::size_t indexOf(TrackKind kind) { return kind == TrackKind::Video ? 0 : 1; }

// 有符号时间戳可能跨过零，直接 high-low 会溢出。先使用无符号差值，
// 转换为毫秒后再饱和到有符号监控值；不改变媒体包内的 PTS/DTS。
std::int64_t spanMs(std::int64_t low, std::int64_t high) {
    if (low == AV_NOPTS_VALUE || high == AV_NOPTS_VALUE || high <= low) return 0;
    const auto delta = static_cast<std::uint64_t>(high) - static_cast<std::uint64_t>(low);
    const auto milliseconds = delta / 1000;
    return static_cast<std::int64_t>(std::min(milliseconds,
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())));
}
} // namespace

struct PacketBuffer::Impl {
    struct Item {
        media::PacketPtr packet;
        std::int64_t timestampUs = AV_NOPTS_VALUE;
        Clock::time_point arrived;
        std::size_t bytes = 0;
    };
    struct Queue {
        std::deque<Item> items;
        bool resetPending = false;
    };

    const config::DemuxConfig options;
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::array<Queue, 2> queues;
    std::shared_ptr<const StreamInfo> streams;
    BufferStats counters;
    std::size_t bytes = 0;
    bool finished = false;
    bool stopped = false;
    bool waitingVideoKey = false;
    std::int64_t audioCutoffUs = AV_NOPTS_VALUE;

    explicit Impl(const config::DemuxConfig& settings) : options(settings) {
        if (options.queueCapacity <= 0 || options.maxBufferMs <= 0 || options.maxQueueBytes <= 0) {
            throw error::AppError("demuxer", error::InvalidConfig, "packet buffer limits must be positive");
        }
    }

    void clearLocked() {
        for (auto& queue : queues) {
            counters.droppedPackets += queue.items.size();
            queue.items.clear();
        }
        bytes = 0;
    }

    void resetLocked() {
        clearLocked();
        ++counters.serial;
        ++counters.resets;
        for (auto& queue : queues) queue.resetPending = true;
        waitingVideoKey = streams && streams->video.index >= 0;
        audioCutoffUs = AV_NOPTS_VALUE;
    }

    std::int64_t timestampSpan(const Queue& queue, std::int64_t extra = AV_NOPTS_VALUE) const {
        std::int64_t low = extra;
        std::int64_t high = extra;
        for (const auto& item : queue.items) {
            if (item.timestampUs == AV_NOPTS_VALUE) continue;
            if (low == AV_NOPTS_VALUE || item.timestampUs < low) low = item.timestampUs;
            if (high == AV_NOPTS_VALUE || item.timestampUs > high) high = item.timestampUs;
        }
        return spanMs(low, high);
    }

    std::int64_t bufferedMsLocked(Clock::time_point now) const {
        std::int64_t result = 0;
        for (const auto& queue : queues) {
            if (!queue.items.empty()) {
                result = std::max(result, static_cast<std::int64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(now - queue.items.front().arrived).count()));
                result = std::max(result, timestampSpan(queue));
            }
        }
        return result;
    }

    // 在出队路径也检查墙钟年龄；生产者断网后，不会继续把陈旧尾包
    // 交给慢消费者。即便没有有效 DTS/PTS，排队时间仍然有硬上限。
    bool expireLocked(Clock::time_point now) {
        if (bufferedMsLocked(now) < options.maxBufferMs) return false;
        ++counters.overflows;
        resetLocked();
        changed.notify_all();
        return true;
    }

    std::size_t packetBytes(const AVPacket& packet) const {
        const auto maximum = static_cast<std::size_t>(options.maxQueueBytes);
        if (packet.size < 0 || packet.side_data_elems < 0) return maximum + 1;
        auto total = static_cast<std::size_t>(packet.size);
        if (total > maximum) return maximum + 1;
        for (int i = 0; i < packet.side_data_elems; ++i) {
            const auto side = static_cast<std::size_t>(packet.side_data[i].size);
            if (side > maximum - total) return maximum + 1;
            total += side;
        }
        return total;
    }

    PacketEvent event(TrackKind kind, EventKind type) const {
        PacketEvent result;
        result.kind = type;
        result.track = kind;
        result.streams = streams;
        result.serial = counters.serial;
        return result;
    }
};

PacketBuffer::PacketBuffer(const config::DemuxConfig& options) : impl_(std::make_unique<Impl>(options)) {}
PacketBuffer::~PacketBuffer() { stop(); }

void PacketBuffer::reset(std::shared_ptr<const StreamInfo> streams) {
    if (!streams) throw error::AppError("demuxer", error::InvalidConfig, "missing stream snapshot");
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->stopped) return;
        impl_->streams = std::move(streams);
        impl_->finished = false;
        impl_->resetLocked();
    }
    impl_->changed.notify_all();
}

void PacketBuffer::invalidate() {
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->stopped) return;
        impl_->resetLocked();
    }
    impl_->changed.notify_all();
}

bool PacketBuffer::push(TrackKind kind, media::PacketPtr packet, std::int64_t timestampUs) {
    if (!packet) throw error::AppError("demuxer", error::InvalidConfig, "cannot queue a null packet");
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto drop = [&] { ++impl_->counters.droppedPackets; return false; };
    if (impl_->stopped || impl_->finished) return drop();
    if (!impl_->streams) {
        throw error::AppError("demuxer", error::InvalidConfig, "stream parameters must precede packets");
    }
    const auto index = indexOf(kind);
    const auto& track = index == 0 ? impl_->streams->video : impl_->streams->audio;
    if (track.index < 0) return drop();

    const auto now = Clock::now();
    impl_->expireLocked(now);
    const auto packetBytes = impl_->packetBytes(*packet);
    const auto maximumBytes = static_cast<std::size_t>(impl_->options.maxQueueBytes);
    auto& queue = impl_->queues[index];
    if (packetBytes > maximumBytes ||
        queue.items.size() >= static_cast<std::size_t>(impl_->options.queueCapacity) ||
        packetBytes > maximumBytes - impl_->bytes ||
        impl_->timestampSpan(queue, timestampUs) >= impl_->options.maxBufferMs) {
        // 不能只删除一个 P/B 帧：整体换代后必须重新从关键帧建立参考链。
        ++impl_->counters.overflows;
        impl_->resetLocked();
        impl_->changed.notify_all();
        if (packetBytes > maximumBytes) return drop();
    }

    if (impl_->waitingVideoKey) {
        if (kind != TrackKind::Video || !(packet->flags & AV_PKT_FLAG_KEY)) return drop();
        impl_->waitingVideoKey = false;
        impl_->audioCutoffUs = timestampUs;
    }
    if (kind == TrackKind::Audio && timestampUs != AV_NOPTS_VALUE &&
        impl_->audioCutoffUs != AV_NOPTS_VALUE && timestampUs < impl_->audioCutoffUs) return drop();

    queue.items.push_back(Impl::Item{std::move(packet), timestampUs, now, packetBytes});
    impl_->bytes += packetBytes;
    ++impl_->counters.acceptedPackets;
    impl_->changed.notify_all();
    return true;
}

PacketEvent PacketBuffer::pop(TrackKind kind, int timeoutMs) {
    std::unique_lock<std::mutex> lock(impl_->mutex);
    const auto deadline = Clock::now() + std::chrono::milliseconds(std::max(0, timeoutMs));
    auto& queue = impl_->queues[indexOf(kind)];
    for (;;) {
        if (impl_->stopped) return impl_->event(kind, EventKind::Stopped);
        const auto now = Clock::now();
        impl_->expireLocked(now);
        if (queue.resetPending) {
            queue.resetPending = false;
            return impl_->event(kind, EventKind::Reset);
        }
        if (!queue.items.empty()) {
            auto result = impl_->event(kind, EventKind::Packet);
            auto& first = queue.items.front();
            result.packet = std::move(first.packet);
            result.timestampUs = first.timestampUs;
            impl_->bytes -= first.bytes;
            queue.items.pop_front();
            return result;
        }
        if (impl_->finished) return impl_->event(kind, EventKind::End);
        if (now >= deadline) return impl_->event(kind, EventKind::Timeout);

        // 等待期间也在最老包到期时醒来；另一轨无人消费时同样能触发清理。
        auto wakeAt = deadline;
        for (const auto& other : impl_->queues) {
            if (!other.items.empty()) {
                wakeAt = std::min(wakeAt, other.items.front().arrived +
                    std::chrono::milliseconds(impl_->options.maxBufferMs));
            }
        }
        impl_->changed.wait_until(lock, wakeAt);
    }
}

void PacketBuffer::finish() {
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->finished = true;
    }
    impl_->changed.notify_all();
}

void PacketBuffer::stop() noexcept {
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->stopped = true;
        impl_->clearLocked();
    }
    impl_->changed.notify_all();
}

BufferStats PacketBuffer::stats() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto result = impl_->counters;
    result.videoPackets = impl_->queues[0].items.size();
    result.audioPackets = impl_->queues[1].items.size();
    result.bytes = impl_->bytes;
    result.bufferedMs = impl_->bufferedMsLocked(Clock::now());
    return result;
}

} // namespace lsl::demuxer
