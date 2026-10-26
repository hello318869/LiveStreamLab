#include "player/pipeline/VideoFrameQueue.hpp"
#include "player/sync/AVSynchronizer.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
using namespace std::chrono_literals;
using lsl::player::VideoFrameQueue;
int checks = 0;

void require(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}

lsl::decoder::DecodedVideoFrame frame(std::uint64_t serial, std::int64_t timestamp) {
    lsl::media::FramePtr value{av_frame_alloc()};
    if (!value) throw std::bad_alloc{};
    value->format = AV_PIX_FMT_YUV420P;
    value->width = value->height = 16;
    lsl::error::throwIfFFmpegFailed(av_frame_get_buffer(value.get(), 32), "test", "allocate frame");
    return {std::move(value), serial, timestamp, 40000};
}

void bufferedFrames() {
    lsl::config::PlayerConfig options;
    options.pipeline.videoQueueCapacity = 2;
    VideoFrameQueue queue(options);
    queue.reset(1);
    queue.push(frame(1, 0), [] { return true; });
    queue.push(frame(1, 2000000), [] { return true; });
    require(queue.snapshot().queued == 2 && queue.snapshot().dropped == 0,
            "future PTS span must not expire frames before their presentation time");
    std::this_thread::sleep_for(220ms);
    require(queue.snapshot().queued == 2, "decoding age must not expire buffered future frames");

    auto producer = std::async(std::launch::async, [&] {
        queue.push(frame(1, 2040000), [] { return true; });
    });
    const bool blocked = producer.wait_for(30ms) == std::future_status::timeout;
    auto first = queue.pop();
    const bool resumed = producer.wait_for(500ms) == std::future_status::ready;
    if (!resumed) queue.stop();
    producer.get();
    require(blocked, "a full queue must apply backpressure rather than replace the earliest frame");
    require(first && first->timestampUs == 0, "the earliest buffered frame must survive decoder lookahead");
    require(resumed, "popping a frame must wake the producer");
    queue.finish();
    auto second = queue.pop();
    auto third = queue.pop();
    require(second && third && second->timestampUs == 2000000 && third->timestampUs == 2040000,
            "normal EOF must drain frames in presentation order");
    require(!queue.pop() && queue.snapshot().bytes == 0, "draining must release all frame storage");
}

void cancellation() {
    lsl::config::PlayerConfig options;
    options.pipeline.videoQueueCapacity = 1;
    VideoFrameQueue queue(options);
    queue.reset(1);
    queue.push(frame(1, 0), [] { return true; });
    std::atomic<std::uint64_t> serial{1};
    auto obsolete = std::async(std::launch::async, [&] {
        queue.push(frame(1, 40000), [&] { return serial.load() == 1; });
    });
    serial.store(2);
    const bool cancelled = obsolete.wait_for(500ms) == std::future_status::ready;
    if (!cancelled) queue.stop();
    obsolete.get();
    require(cancelled, "a network generation change must cancel a producer waiting for room");
    queue.reset(2);
    queue.push(frame(2, 80000), [] { return true; });
    auto blocked = std::async(std::launch::async, [&] {
        queue.push(frame(2, 120000), [] { return true; });
    });
    queue.stop();
    require(blocked.wait_for(500ms) == std::future_status::ready, "stop must unblock a full frame queue");
    blocked.get();
    require(queue.snapshot().queued == 0 && queue.snapshot().bytes == 0,
            "stop must clear stale generation frames");
}

void futureFirstFrame() {
    using lsl::sync::SyncAction;
    const auto start = lsl::sync::AVSynchronizer::Clock::now();
    lsl::sync::AVSynchronizer sync(lsl::config::PlayerSyncConfig{});
    sync.reset(1, start);
    sync.beginFrame(2000000, 40000, start);
    lsl::sync::SyncInput input;
    input.audioExpected = input.hasNextFrame = true;
    input.audio.serial = 1;
    input.audio.valid = true;
    input.audio.timestampUs = 0;
    require(sync.evaluate(input, start) == SyncAction::Wait, "future first frame must wait for audio");
    input.audio.timestampUs = 1100000;
    require(sync.evaluate(input, start + 1100ms) == SyncAction::Wait,
            "initial alignment must not replace a waiting frame with ever newer video");
    input.audio.timestampUs = 2000000;
    require(sync.evaluate(input, start + 2s) == SyncAction::Render,
            "first frame must render once audio reaches its timestamp");
}
} // namespace

int main() {
    try {
        bufferedFrames();
        cancellation();
        futureFirstFrame();
        std::cout << checks << " playback regression checks passed\n";
        return 0;
    } catch (const std::exception& problem) {
        std::cerr << "playback regression failed: " << problem.what() << '\n';
        return 1;
    }
}
