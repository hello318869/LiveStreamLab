#include "player/pipeline/PlayerPipeline.hpp"
#include "player/pipeline/AudioPlayback.hpp"
#include "player/audio/SDLAudioSink.hpp"
#include "player/decoder/VideoDecoder.hpp"
#include "player/demuxer/Demuxer.hpp"
#include "player/renderer/SDLVideoRenderer.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <deque>
#include <exception>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace lsl::player {
namespace {
using Clock = std::chrono::steady_clock;
using Microseconds = std::chrono::microseconds;
static_assert(std::atomic<bool>::is_always_lock_free, "signal handler requires a lock-free stop flag");
std::atomic<bool> stopRequested{false};
void signalStop(int) { stopRequested.store(true, std::memory_order_relaxed); }

class SignalScope {
public:
    SignalScope() {
        stopRequested.store(false, std::memory_order_relaxed);
        interrupt_ = std::signal(SIGINT, signalStop);
        terminate_ = std::signal(SIGTERM, signalStop);
    }
    ~SignalScope() {
        if (interrupt_ != SIG_ERR) std::signal(SIGINT, interrupt_);
        if (terminate_ != SIG_ERR) std::signal(SIGTERM, terminate_);
    }
private:
    using Handler = void (*)(int);
    Handler interrupt_;
    Handler terminate_;
};

// 视频交接保留在 player/pipeline；完整通用队列属于 M14。
// 原始帧可以直接丢弃，不破坏解码器内部的 H.264 参考帧链。
class VideoFrameQueue {
public:
    struct Snapshot {
        decoder::VideoDecodeStats decode;
        std::uint64_t consumedPackets = 0;
        std::uint64_t dropped = 0;
        std::size_t queued = 0;
        std::size_t bytes = 0;
        bool finished = false;
    };
    void reset(std::uint64_t serial) {
        std::lock_guard<std::mutex> lock(mutex_);
        discardAll();
        serial_ = serial;
    }
    void push(decoder::DecodedVideoFrame frame) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_ || frame.serial != serial_) { ++state_.dropped; return; }
        const auto bytes = frameBytes(*frame.frame);
        if (bytes > maxBytes) { ++state_.dropped; return; }
        const auto now = Clock::now();
        expire(now);
        while (!queue_.empty() && (queue_.size() >= maxFrames ||
               bytes > maxBytes - state_.bytes || spanExceeded(frame.timestampUs))) dropFront();
        queue_.push_back(Item{std::move(frame), now, bytes});
        state_.bytes += bytes;
    }
    std::optional<decoder::DecodedVideoFrame> pop() {
        std::lock_guard<std::mutex> lock(mutex_);
        expire(Clock::now());
        if (queue_.empty()) return std::nullopt;
        auto result = std::move(queue_.front().frame);
        state_.bytes -= queue_.front().bytes;
        queue_.pop_front();
        return result;
    }
    void consumed() {
        std::lock_guard<std::mutex> lock(mutex_);
        ++state_.consumedPackets;
    }
    void noteDropped() {
        std::lock_guard<std::mutex> lock(mutex_);
        ++state_.dropped;
    }
    void update(const decoder::VideoDecodeStats& stats) {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.decode = stats;
    }
    void finish(std::exception_ptr failure = {}) {
        std::lock_guard<std::mutex> lock(mutex_);
        failure_ = failure;
        state_.finished = true;
    }
    void stop() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        stopped_ = true;
        discardAll();
    }
    Snapshot snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (failure_) std::rethrow_exception(failure_);
        auto result = state_;
        result.queued = queue_.size();
        return result;
    }
private:
    static constexpr std::size_t maxFrames = 8;
    static constexpr std::size_t maxBytes = 64 * 1024 * 1024;
    static constexpr auto maxAge = std::chrono::milliseconds(500);
    struct Item {
        decoder::DecodedVideoFrame frame;
        Clock::time_point arrived;
        std::size_t bytes;
    };
    static std::size_t frameBytes(const AVFrame& frame) {
        // 实际引用缓冲大小包含 stride 对齐；共享缓冲重复计数是保守上界。
        std::size_t result = 0;
        const auto add = [&](const AVBufferRef* buffer) {
            if (buffer) result += std::min(buffer->size, maxBytes + 1);
        };
        for (const auto* buffer : frame.buf) add(buffer);
        for (int i = 0; i < frame.nb_extended_buf; ++i) add(frame.extended_buf[i]);
        return result;
    }
    bool spanExceeded(std::int64_t timestamp) const {
        const auto first = queue_.front().frame.timestampUs;
        return timestamp != AV_NOPTS_VALUE && first != AV_NOPTS_VALUE &&
            static_cast<long double>(timestamp) - first > 500000.0L;
    }
    void dropFront() {
        state_.bytes -= queue_.front().bytes;
        queue_.pop_front();
        ++state_.dropped;
    }
    void discardAll() {
        state_.dropped += queue_.size();
        queue_.clear();
        state_.bytes = 0;
    }
    void expire(Clock::time_point now) {
        while (!queue_.empty() && now - queue_.front().arrived >= maxAge) dropFront();
    }
    mutable std::mutex mutex_;
    std::deque<Item> queue_;
    Snapshot state_;
    std::exception_ptr failure_;
    std::uint64_t serial_ = 0;
    bool stopped_ = false;
};

// 解码上下文只属于此工作线程。析构覆盖正常停止、网络错误和 SDL 异常路径。
class VideoWorker {
public:
    VideoWorker(demuxer::Demuxer& input, VideoFrameQueue& frames, logging::Logger& logger)
        : input_(input), frames_(frames), logger_(logger), thread_([this] { run(); }) {}
    ~VideoWorker() { stop(); }
    void stop() noexcept {
        stopping_.store(true, std::memory_order_relaxed);
        input_.requestStop();
        frames_.stop();
        if (thread_.joinable()) thread_.join();
    }
private:
    void publish(std::vector<decoder::DecodedVideoFrame> output) {
        for (auto& frame : output) {
            // 网络线程可能已经换代，Reset 还没被本线程取到。
            if (stopping_.load(std::memory_order_relaxed)) return;
            if (frame.serial == input_.stats().buffer.serial) frames_.push(std::move(frame));
            else frames_.noteDropped();
        }
    }
    void run() noexcept {
        try {
            decoder::VideoDecoder decoder(logger_);
            bool initialized = false;
            std::uint64_t serial = 0;
            while (!stopping_.load(std::memory_order_relaxed)) {
                auto event = input_.pop(demuxer::TrackKind::Video, 10);
                if (event.kind == demuxer::EventKind::Reset) {
                    if (event.serial != input_.stats().buffer.serial) continue;
                    frames_.reset(event.serial);
                    serial = event.serial;
                    initialized = false;
                    // 首次连接失败也会换代，但此时尚无任何流快照；继续交给 M09 重试。
                    if (!event.streams) {
                        decoder.reset({}, serial);
                        continue;
                    }
                    decoder.reset(event.streams->video, event.serial);
                    initialized = event.streams->video.index >= 0;
                    logger_.log(logging::Level::Info, "player", "video reset serial=" +
                        std::to_string(serial) + " session=" + std::to_string(event.streams->session));
                } else if (event.kind == demuxer::EventKind::Packet) {
                    frames_.consumed();
                    if (initialized && event.serial == serial && serial == input_.stats().buffer.serial) {
                        publish(decoder.decode(*event.packet, serial));
                    }
                } else if (event.kind == demuxer::EventKind::End) {
                    input_.checkError(); // 网络失败不能当作正常 EOF 刷出旧连接的延迟帧。
                    if (initialized && serial == input_.stats().buffer.serial) publish(decoder.flush());
                    frames_.update(decoder.stats());
                    break;
                } else if (event.kind == demuxer::EventKind::Stopped) {
                    break;
                }
                frames_.update(decoder.stats());
                input_.checkError();
            }
            frames_.update(decoder.stats());
            frames_.finish();
        } catch (...) {
            frames_.finish(std::current_exception());
            input_.requestStop();
        }
    }
    demuxer::Demuxer& input_;
    VideoFrameQueue& frames_;
    logging::Logger& logger_;
    std::atomic<bool> stopping_{false};
    std::thread thread_;
};

// M10/M12 使用独立的视频 PTS 时钟；M13 接入后再替换为音频主时钟。
class VideoSchedule {
public:
    Clock::time_point due(const decoder::DecodedVideoFrame& frame, Clock::time_point now) {
        const auto duration = std::clamp<std::int64_t>(frame.durationUs, 1000, 1000000);
        auto result = now;
        if (initialized_) {
            result = next_;
            if (frame.timestampUs != AV_NOPTS_VALUE && lastPts_ != AV_NOPTS_VALUE) {
                const auto delta = static_cast<long double>(frame.timestampUs) - lastPts_;
                if (delta >= 0 && delta <= 500000) {
                    result = previous_ + Microseconds(static_cast<std::int64_t>(delta));
                } else {
                    result = now; // 时间轴异常时重建锚点，避免等待极端 PTS。
                }
            }
        }
        previous_ = result;
        next_ = result + Microseconds(duration);
        lastPts_ = frame.timestampUs;
        initialized_ = true;
        return result;
    }
    void reset() { initialized_ = false; lastPts_ = AV_NOPTS_VALUE; }
private:
    Clock::time_point previous_{};
    Clock::time_point next_{};
    std::int64_t lastPts_ = AV_NOPTS_VALUE;
    bool initialized_ = false;
};

void report(logging::Logger& logger, const demuxer::DemuxStats& network,
            const VideoFrameQueue::Snapshot& frames, const renderer::VideoRenderStats& display,
            const AudioPlaybackStats& audioDecode, const audio::AudioSinkStats& audioOutput,
            const VideoFrameQueue::Snapshot& previousFrames, const renderer::VideoRenderStats& previousDisplay,
            double seconds) {
    const auto decoded = frames.decode.decodedFrames - previousFrames.decode.decodedFrames;
    const auto rendered = display.renderedFrames - previousDisplay.renderedFrames;
    logger.log(logging::Level::Info, "player", "received=" + std::to_string(network.videoPackets) +
        "/" + std::to_string(network.audioPackets) + " packets consumed=" +
        std::to_string(frames.consumedPackets) + "/" + std::to_string(audioDecode.consumedPackets) +
        " decode_fps=" + std::to_string(decoded / seconds) + " render_fps=" + std::to_string(rendered / seconds) +
        " decoded=" + std::to_string(frames.decode.decodedFrames) +
        " shown=" + std::to_string(display.renderedFrames) +
        " decode_us/frame=" + std::to_string(frames.decode.decodedFrames ?
            frames.decode.decodeTimeUs / frames.decode.decodedFrames : 0) +
        " render_us/frame=" + std::to_string(display.renderedFrames ?
            display.renderTimeUs / display.renderedFrames : 0) +
        " uploads=" + std::to_string(display.textureUploads) +
        " frame_queue=" + std::to_string(frames.queued) + " frame_bytes=" + std::to_string(frames.bytes) +
        " frame_drops=" + std::to_string(frames.dropped + display.droppedFrames + frames.decode.droppedFrames) +
        " decode_errors=" + std::to_string(frames.decode.decodeErrors) +
        " decoder_packet_drops=" + std::to_string(frames.decode.droppedPackets) +
        " packet_drops=" + std::to_string(network.buffer.droppedPackets) +
        " audio_decoded=" + std::to_string(audioDecode.decode.decodedFrames) +
        " audio_samples=" + std::to_string(audioDecode.decode.outputSamples) +
        " audio_errors=" + std::to_string(audioDecode.decode.decodeErrors) +
        " audio_submitted=" + std::to_string(audioOutput.submittedFrames) +
        " audio_queued=" + std::to_string(audioOutput.queuedFrames) +
        " audio_underruns=" + std::to_string(audioOutput.underruns) +
        " audio_dropped=" + std::to_string(audioOutput.droppedFrames) +
        " audio_paused=" + std::to_string(audioOutput.paused) +
        " audio_muted=" + std::to_string(audioOutput.muted) +
        " volume=" + std::to_string(audioOutput.volume) +
        " buffer_ms=" + std::to_string(network.buffer.bufferedMs) +
        " retries=" + std::to_string(network.reconnects));
}
} // namespace

int runPlayer(const config::PlayerConfig& config, const config::CommandLineOptions&, logging::Logger& logger) {
    SignalScope signals;
    // worker 先停止/join，网络和帧资源释放后，SDL 在主线程最后销毁。
    renderer::SDLVideoRenderer sdl({config.video.width, config.video.height,
                                  config.video.vsync, config.video.fullscreen}, logger);
    renderer::IVideoRenderer& display = sdl;
    demuxer::Demuxer input(config, logger);
    VideoFrameQueue frames;
    std::unique_ptr<audio::SDLAudioSink> sdlAudio;
    if (config.audioPlayback.enabled) {
        audio::AudioSinkOptions options;
        options.device = config.audioDevice;
        options.sampleRate = config.sampleRate;
        options.channels = config.channels;
        options.bufferMs = config.audioPlayback.bufferMs;
        options.volume = config.audioPlayback.volume;
        options.muted = config.audioPlayback.muted;
        options.paused = config.audioPlayback.paused;
        sdlAudio = std::make_unique<audio::SDLAudioSink>(options, logger);
    }
    audio::IAudioSink* audioOutput = sdlAudio.get();
    input.start();
    VideoWorker worker(input, frames, logger);
    AudioPlayback audioWorker(config, input, audioOutput, logger);
    logger.log(logging::Level::Info, "player",
        "M09-M12 playback; F/Alt+Enter/double click fullscreen, Space audio pause, M mute, Up/Down volume, Escape/close/Ctrl+C stop. A/V sync (M13) is not implemented.");
    std::optional<decoder::DecodedVideoFrame> pending;
    VideoSchedule schedule;
    Clock::time_point due{};
    auto lastReport = Clock::now();
    auto previousFrames = frames.snapshot();
    auto previousDisplay = display.stats();
    std::uint64_t visibleSerial = 0;
    bool stoppedByUser = false;
    for (;;) {
        if (stopRequested.load(std::memory_order_relaxed) || !display.pollEvents()) {
            stoppedByUser = true;
            break;
        }
        input.checkError();
        if (audioOutput) {
            audioOutput->checkError();
            const auto controls = display.takeAudioControls();
            const auto settings = audioOutput->stats();
            if (controls.togglePaused) audioOutput->setPaused(!settings.paused);
            if (controls.toggleMuted) audioOutput->setMuted(!settings.muted);
            if (controls.volumeDelta) audioOutput->setVolume(std::clamp(settings.volume + controls.volumeDelta, 0, 100));
        } else {
            static_cast<void>(display.takeAudioControls());
        }
        const auto serial = input.stats().buffer.serial;
        if (serial != visibleSerial) {
            if (pending) display.noteDroppedFrames(1);
            pending.reset();
            schedule.reset();
            display.clear();
            if (audioOutput && audioOutput->stats().serial != serial) audioOutput->clear(serial);
            visibleSerial = serial;
        }
        if (!pending) {
            pending = frames.pop();
            if (pending) due = schedule.due(*pending, Clock::now());
        }
        auto state = frames.snapshot();
        const auto audioState = audioWorker.stats();
        const auto now = Clock::now();
        if (pending) {
            if (pending->serial != input.stats().buffer.serial) {
                display.noteDroppedFrames(1);
                pending.reset();
                schedule.reset();
            } else if (now >= due) {
                // 超过100ms的帧让位给后续画面；无后续帧时仍显示，避免长时间黑屏。
                if (now - due > std::chrono::milliseconds(100) && state.queued > 0) {
                    display.noteDroppedFrames(1);
                } else {
                    display.render(*pending->frame);
                }
                pending.reset();
            }
        }
        if (now - lastReport >= std::chrono::seconds(1)) {
            const auto currentDisplay = display.stats();
            report(logger, input.stats(), state, currentDisplay, audioState,
                   audioOutput ? audioOutput->stats() : audio::AudioSinkStats{}, previousFrames, previousDisplay,
                   std::chrono::duration<double>(now - lastReport).count());
            lastReport = now;
            previousFrames = state;
            previousDisplay = currentDisplay;
        }
        if (state.finished && audioState.finished && !pending && state.queued == 0 &&
            (!audioOutput || audioOutput->drained())) break;
        // 短暂让出 CPU，保持窗口事件、停止和网络换代检查及时。
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    audioWorker.stop();
    worker.stop();
    input.join();
    if (audioOutput) audioOutput->stop();
    if (!stoppedByUser) {
        input.checkError();
        static_cast<void>(frames.snapshot());
        static_cast<void>(audioWorker.stats());
    }
    return 0;
}

} // namespace lsl::player
