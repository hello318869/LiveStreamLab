#include "player/pipeline/PlayerPipeline.hpp"
#include "player/pipeline/AudioPlayback.hpp"
#include "player/audio/SDLAudioSink.hpp"
#include "player/decoder/VideoDecoder.hpp"
#include "player/demuxer/Demuxer.hpp"
#include "player/renderer/SDLVideoRenderer.hpp"
#include "player/sync/AVSynchronizer.hpp"
#include "player/pipeline/VideoFrameQueue.hpp"
#include "common/stats/CaptureTimestamp.hpp"

extern "C" {
#include <libavutil/time.h>
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <exception>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace lsl::player {
namespace {
using Clock = std::chrono::steady_clock;
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

// 解码上下文只属于此工作线程。析构覆盖正常停止、网络错误和 SDL 异常路径。
class VideoWorker {
public:
    VideoWorker(demuxer::Demuxer& input, VideoFrameQueue& frames, logging::Logger& logger)
        : input_(input), frames_(frames), logger_(logger), thread_([this] { run(); }) {}
    ~VideoWorker() { stop(); }
    void requestStop() noexcept {
        stopping_.store(true, std::memory_order_relaxed);
        input_.requestStop();
        frames_.stop();
    }
    void join() noexcept {
        if (thread_.joinable()) thread_.join();
    }
    void stop() noexcept { requestStop(); join(); }
    bool hasVideo(std::uint64_t serial) const {
        std::lock_guard<std::mutex> lock(trackMutex_);
        return serial == trackSerial_ && activeTrack_;
    }
private:
    void publish(std::vector<decoder::DecodedVideoFrame> output) {
        for (auto& frame : output) {
            // 网络线程可能已经换代，Reset 还没被本线程取到。
            if (stopping_.load(std::memory_order_relaxed)) return;
            const auto serial = frame.serial;
            frames_.push(std::move(frame), [&] {
                return !stopping_.load(std::memory_order_relaxed) && serial == input_.stats().buffer.serial;
            });
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
                    {
                        std::lock_guard<std::mutex> lock(trackMutex_);
                        trackSerial_ = serial;
                        activeTrack_ = event.streams && event.streams->video.index >= 0;
                    }
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
    mutable std::mutex trackMutex_;
    std::uint64_t trackSerial_ = 0;
    bool activeTrack_ = false;
    std::thread thread_;
};

void report(logging::Logger& logger, const demuxer::DemuxStats& network,
            const VideoFrameQueue::Snapshot& frames, const renderer::VideoRenderStats& display,
            const AudioPlaybackStats& audioDecode, const audio::AudioSinkStats& audioOutput,
            const VideoFrameQueue::Snapshot& previousFrames, const renderer::VideoRenderStats& previousDisplay,
            const sync::SyncStats& timing, const audio::AudioClock& audioClock, double seconds) {
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
        " frame_queue=" + std::to_string(frames.queued) +
        " frame_queue_peak=" + std::to_string(frames.highWaterMark) +
        " frame_bytes=" + std::to_string(frames.bytes) +
        " frame_peak_bytes=" + std::to_string(frames.peakBytes) +
        " frame_drops=" + std::to_string(frames.dropped + display.droppedFrames + frames.decode.droppedFrames) +
        " decode_errors=" + std::to_string(frames.decode.decodeErrors) +
        " decoder_packet_drops=" + std::to_string(frames.decode.droppedPackets) +
        " packet_drops=" + std::to_string(network.buffer.droppedPackets) +
        " packet_queue_video=" + std::to_string(network.buffer.videoPackets) +
        " packet_queue_audio=" + std::to_string(network.buffer.audioPackets) +
        " packet_queue_video_peak=" + std::to_string(network.buffer.videoHighWaterMark) +
        " packet_queue_audio_peak=" + std::to_string(network.buffer.audioHighWaterMark) +
        " packet_peak_bytes=" + std::to_string(network.buffer.peakBytes) +
        " audio_decoded=" + std::to_string(audioDecode.decode.decodedFrames) +
        " audio_samples=" + std::to_string(audioDecode.decode.outputSamples) +
        " audio_errors=" + std::to_string(audioDecode.decode.decodeErrors) +
        " audio_submitted=" + std::to_string(audioOutput.submittedFrames) +
        " audio_queued=" + std::to_string(audioOutput.queuedFrames) +
        " audio_queue_peak=" + std::to_string(audioOutput.highWaterMarkFrames) +
        " audio_underruns=" + std::to_string(audioOutput.underruns) +
        " audio_dropped=" + std::to_string(audioOutput.droppedFrames) +
        " audio_paused=" + std::to_string(audioOutput.paused) +
        " audio_muted=" + std::to_string(audioOutput.muted) +
        " volume=" + std::to_string(audioOutput.volume) +
        " master=" + sync::clockName(timing.master) +
        " av_error_ms=" + (timing.errorValid ? std::to_string(timing.errorUs / 1000.0) : "n/a") +
        " av_mean_abs_ms=" + std::to_string(timing.meanAbsoluteErrorUs / 1000.0) +
        " av_max_abs_ms=" + std::to_string(timing.maxAbsoluteErrorUs / 1000.0) +
        " av_samples=" + std::to_string(timing.samples) +
        " sync_waits=" + std::to_string(timing.waits) +
        " sync_drops=" + std::to_string(timing.droppedFrames) +
        " sync_fallbacks=" + std::to_string(timing.fallbacks) +
        " sync_resets=" + std::to_string(timing.resets) +
        " sync_recoveries=" + std::to_string(timing.recoveries) +
        " audio_buffered_ms=" + std::to_string(audioClock.bufferedUs / 1000.0) +
        " audio_device_latency_ms=" + std::to_string(audioClock.deviceLatencyUs / 1000.0) +
        " buffer_ms=" + std::to_string(network.buffer.bufferedMs) +
        " retries=" + std::to_string(network.reconnects) +
        " network_state=" + lsl::network::stateName(network.network.state) +
        " connection_seconds=" + std::to_string(network.network.connectionSeconds) +
        " connected_seconds=" + std::to_string(network.network.connectedSeconds) +
        " network_uptime_seconds=" + std::to_string(network.network.uptimeSeconds) +
        " input_payload_bitrate_bps=" + std::to_string(network.network.averageBitrate) +
        " input_idle_seconds=" + std::to_string(network.network.idleSeconds) +
        " network_errors=" + std::to_string(network.network.networkErrors) +
        " retry_delay_ms=" + std::to_string(network.network.retryDelayMs) +
        " consecutive_failures=" + std::to_string(network.network.consecutiveFailures) +
        " last_network_error_code=" + std::to_string(network.network.lastErrorCode) +
        " last_network_error=" + network.network.lastErrorMessage);
}
} // namespace

std::unique_ptr<audio::IAudioSink> createPlayerAudioSink(const config::PlayerConfig& config,
                                                      logging::Logger& logger) {
    if (!config.audioPlayback.enabled) return {};
    audio::AudioSinkOptions options;
    options.device = config.audioDevice;
    options.sampleRate = config.sampleRate;
    options.channels = config.channels;
    options.bufferMs = config.audioPlayback.bufferMs;
    options.volume = config.audioPlayback.volume;
    options.muted = config.audioPlayback.muted;
    options.paused = config.audioPlayback.paused;
    return std::make_unique<audio::SDLAudioSink>(options, logger);
}

int runPlayer(const config::PlayerConfig& config, const config::CommandLineOptions&, logging::Logger& logger) {
    SignalScope signals;
    // 所有 SDL 子系统的初始化/销毁留在主线程；核心先停止并 join 媒体线程。
    renderer::SDLVideoRenderer display({config.video.width, config.video.height,
                                      config.video.vsync, config.video.fullscreen}, logger);
    auto audioOutput = createPlayerAudioSink(config, logger);
    logger.log(logging::Level::Info, "player",
        "F/Alt+Enter/double click fullscreen, Space A/V pause, M mute, Up/Down volume, Escape/close/Ctrl+C stop.");
    return runPlayerPipeline(config, logger, display, stopRequested, audioOutput.get());
}

int runPlayerPipeline(const config::PlayerConfig& config, logging::Logger& logger,
                      renderer::IVideoRenderer& display, const std::atomic<bool>& stop,
                      audio::IAudioSink* audioOutput, const PlayerPipelineCallback& callback,
                      const PlayerControlsReader& controlsReader) {
    if (stop.load(std::memory_order_relaxed)) return 0;
    demuxer::Demuxer input(config, logger);
    VideoFrameQueue frames(config);
    std::unique_ptr<VideoWorker> worker;
    std::unique_ptr<AudioPlayback> audioWorker;
    // 在任何线程启动前建立兜底；覆盖第二/第三个线程构造失败与循环内异常。
    struct Shutdown {
        demuxer::Demuxer& input;
        std::unique_ptr<VideoWorker>& videoThread;
        std::unique_ptr<AudioPlayback>& audioThread;
        audio::IAudioSink* sink;
        renderer::IVideoRenderer& display;
        bool finished = false;
        void stopAndJoin() noexcept {
            if (finished) return;
            finished = true;
            input.requestStop();
            // 异常路径也先撤下画面，不能让网络关闭/join 的等待期间残留断线旧图。
            try { display.clear(); } catch (...) {}
            if (audioThread) audioThread->requestStop();
            if (videoThread) videoThread->requestStop();
            if (audioThread) audioThread->join();
            if (videoThread) videoThread->join();
            try { input.join(); } catch (...) {}
            if (sink) sink->stop();
        }
        ~Shutdown() { stopAndJoin(); }
    } shutdown{input, worker, audioWorker, audioOutput, display};
    input.start();
    worker = std::make_unique<VideoWorker>(input, frames, logger);
    audioWorker = std::make_unique<AudioPlayback>(config, input, audioOutput, logger);
    logger.log(logging::Level::Info, "player",
        "Playback started; bounded handoff, network recovery and audio master with estimated device latency.");
    std::optional<decoder::DecodedVideoFrame> pending;
    auto syncOptions = config.sync;
    // 最大视频年龄按播放时钟衡量，而不是按提前解码的时间衡量。
    syncOptions.dropThresholdMs = std::min(syncOptions.dropThresholdMs, config.pipeline.maxVideoAgeMs);
    sync::AVSynchronizer schedule(syncOptions);
    auto lastReport = Clock::now();
    auto previousFrames = frames.snapshot();
    auto previousDisplay = display.stats();
    std::uint64_t visibleSerial = 0;
    PlayerPipelineControls settings;
    settings.paused = config.audioPlayback.paused;
    settings.muted = config.audioPlayback.muted;
    settings.volume = config.audioPlayback.volume;
    bool haveExternalControls = false;
    bool stoppedByUser = false;
    bool generationProgress = false;
    std::uint64_t previousSubmitted = 0;
    std::optional<Clock::time_point> lastVideoRender;
    std::int64_t lastVideoDurationUs = 40000;
    auto nextStatus = Clock::now();
    auto performanceAt = Clock::now();
    std::uint64_t previousDecoded = 0, previousReceived = 0;
    stats::PlayerPerformance performance;
    stats::LatencyStatus cliLatencyStatus = stats::LatencyStatus::Inactive;
    double cliLatencyMs = 0;
    std::optional<Clock::time_point> lastSyncSample;
    const auto samplePerformance = [&](bool terminal) {
        const auto current = Clock::now();
        const auto video = frames.snapshot(false); // 统计读取不能覆盖原工作线程错误。
        const auto network = input.stats();
        const auto pcm = audioOutput ? audioOutput->stats() : audio::AudioSinkStats{};
        const auto displayStats = display.stats();
        performance.decodedVideoFrames = video.decode.decodedFrames;
        performance.receivedBytes = network.bytes;
        performance.videoPackets = {network.buffer.videoPackets, static_cast<std::size_t>(config.demux.queueCapacity), network.buffer.videoHighWaterMark};
        performance.audioPackets = {network.buffer.audioPackets, static_cast<std::size_t>(config.demux.queueCapacity), network.buffer.audioHighWaterMark};
        performance.videoFrames = {video.queued, static_cast<std::size_t>(config.pipeline.videoQueueCapacity), video.highWaterMark};
        performance.pcm = {pcm.queuedFrames, pcm.capacityFrames, pcm.highWaterMarkFrames};
        performance.pcmMilliseconds = static_cast<double>(pcm.queuedFrames) * 1000 / config.sampleRate;
        performance.droppedVideoFrames = video.dropped + video.decode.droppedFrames + displayStats.droppedFrames;
        performance.droppedPackets = network.buffer.droppedPackets + video.decode.droppedPackets;
        const double seconds = std::chrono::duration<double>(current - performanceAt).count();
        if (terminal) {
            performance.decodeFps = performance.receiveBitrate = 0;
        } else if (seconds >= 0.2) {
            performance.decodeFps = video.decode.decodedFrames >= previousDecoded
                ? static_cast<double>(video.decode.decodedFrames - previousDecoded) / seconds : 0;
            performance.receiveBitrate = network.bytes >= previousReceived
                ? static_cast<double>(network.bytes - previousReceived) * 8 / seconds : 0;
            previousDecoded = video.decode.decodedFrames;
            previousReceived = network.bytes;
            performanceAt = current;
            performance.sampled = true;
        }
        const auto timing = schedule.stats();
        const auto clock = audioOutput ? audioOutput->clock() : audio::AudioClock{};
        const auto recentUs = std::max<std::int64_t>(500000, std::min<std::int64_t>(lastVideoDurationUs, 10000000) * 3);
        performance.syncValid = !terminal && !settings.paused && timing.errorValid && timing.serial == visibleSerial &&
            clock.valid && !clock.starved && clock.serial == visibleSerial && lastSyncSample &&
            current - *lastSyncSample < std::chrono::microseconds(recentUs);
        performance.syncErrorMs = performance.syncValid ? timing.errorUs / 1000.0 : 0;
        performance.latencyStatus = terminal || settings.paused || !lastVideoRender ||
            current - *lastVideoRender >= std::chrono::seconds(5) ? stats::LatencyStatus::Inactive : cliLatencyStatus;
        performance.endToEndMs = performance.latencyStatus == stats::LatencyStatus::Estimated ? cliLatencyMs : 0;
        return performance;
    };
    const auto finalSnapshot = [&] {
        if (!callback) return;
        PlayerPipelineSnapshot status;
        status.serial = visibleSerial;
        status.controls = settings;
        status.performance = samplePerformance(true);
        callback(status);
    };
    try {
    for (;;) {
        if (stop.load(std::memory_order_relaxed) || !display.pollEvents()) {
            stoppedByUser = true;
            break;
        }
        input.checkError();
        bool controlsChanged = false;
        if (controlsReader) {
            const auto requested = controlsReader();
            if (!haveExternalControls || requested.revision != settings.revision) {
                settings = requested;
                settings.volume = std::clamp(settings.volume, 0, 100);
                haveExternalControls = true;
                controlsChanged = true;
            }
        }
        const auto controls = display.takeAudioControls();
        if (controls.togglePaused) {
            settings.paused = !settings.paused;
            controlsChanged = true;
        }
        if (controls.toggleMuted) {
            settings.muted = !settings.muted;
            controlsChanged = true;
        }
        if (controls.volumeDelta) {
            settings.volume = std::clamp(settings.volume + controls.volumeDelta, 0, 100);
            controlsChanged = true;
        }
        if (audioOutput) {
            audioOutput->checkError();
            if (controlsChanged) {
                audioOutput->setPaused(settings.paused);
                audioOutput->setMuted(settings.muted);
                audioOutput->setVolume(settings.volume);
            }
        }
        const auto serial = input.stats().buffer.serial;
        bool generationChanged = false;
        if (serial != visibleSerial) {
            if (pending) display.noteDroppedFrames(1);
            pending.reset();
            schedule.reset(serial, Clock::now());
            display.clear();
            if (audioOutput && audioOutput->stats().serial != serial) audioOutput->clear(serial);
            visibleSerial = serial;
            generationProgress = false;
            lastVideoRender.reset();
            lastSyncSample.reset();
            cliLatencyStatus = stats::LatencyStatus::Inactive;
            generationChanged = true;
        }
        schedule.setPaused(settings.paused, Clock::now());
        if (!pending) {
            pending = frames.pop();
            if (pending) schedule.beginFrame(pending->timestampUs, pending->durationUs, Clock::now());
        }
        auto state = frames.snapshot();
        const auto audioState = audioWorker->stats();
        const auto audioClock = audioOutput ? audioOutput->clock() : audio::AudioClock{};
        const auto now = Clock::now();
        bool progressBegan = false;
        if (pending) {
            if (pending->serial != input.stats().buffer.serial) {
                display.noteDroppedFrames(1);
                pending.reset();
            } else {
                sync::SyncInput timing;
                timing.audio = audioClock;
                // 音频工作线程尚未收到 Reset 时给它启动宽限；确认本代无音轨后立即降级。
                timing.audioExpected = audioOutput && (audioState.serial != serial || audioState.activeTrack);
                timing.audioFinished = audioState.finished && (!audioOutput || audioOutput->drained());
                timing.paused = settings.paused;
                timing.hasNextFrame = state.queued > 0;
                timing.canResynchronize = !state.finished && !audioState.finished;
                const auto action = schedule.evaluate(timing, now);
                if (schedule.stats().errorValid) lastSyncSample = now;
                else lastSyncSample.reset();
                if (action == sync::SyncAction::Resynchronize) {
                    const bool reset = input.resynchronize(serial);
                    logger.log(logging::Level::Warn, "sync", reset ?
                        "persistent A/V drift or timestamp discontinuity; reset both tracks and wait for a keyframe" :
                        "synchronization recovery superseded by a newer generation or EOF");
                    display.noteDroppedFrames(1);
                    pending.reset();
                    // 成功换代由下一轮公共分支统一重置；EOF 竞争失败只撤销本地基准。
                    if (!reset && input.stats().buffer.serial == visibleSerial) schedule.reset(visibleSerial, now);
                } else if (action == sync::SyncAction::Drop) {
                    display.noteDroppedFrames(1);
                    pending.reset();
                } else if (action == sync::SyncAction::Render) {
                    // 再核对一次，减少网络换代发生在时钟查询与上传之间时的旧帧窗口。
                    if (pending->serial == input.stats().buffer.serial) {
                        display.render(*pending->frame);
                        // CLI 显示后采样；Qt GUI 另在 paintEvent 完成后覆盖为真实前端提交端点。
                        const auto captureUs = stats::captureTimeUs(*pending->frame);
                        const auto presentedUs = av_gettime();
                        if (captureUs <= 0) cliLatencyStatus = stats::LatencyStatus::NoTimestamp;
                        else if (captureUs > presentedUs || presentedUs - captureUs > 86400000000LL)
                            cliLatencyStatus = stats::LatencyStatus::ClockError;
                        else {
                            cliLatencyStatus = stats::LatencyStatus::Estimated;
                            cliLatencyMs = (presentedUs - captureUs) / 1000.0;
                        }
                        progressBegan = !generationProgress;
                        generationProgress = true;
                        lastVideoRender = now;
                        lastVideoDurationUs = std::max<std::int64_t>(1, pending->durationUs);
                    } else {
                        display.noteDroppedFrames(1);
                    }
                    pending.reset();
                }
            }
        }
        const auto outputState = audioOutput ? audioOutput->stats() : audio::AudioSinkStats{};
        const bool audioProgress = outputState.serial == serial && outputState.submittedFrames > previousSubmitted;
        if (audioProgress) {
            progressBegan = progressBegan || !generationProgress;
            generationProgress = true;
        }
        previousSubmitted = outputState.submittedFrames;
        if (callback && (generationChanged || controlsChanged || progressBegan || now >= nextStatus)) {
            const auto network = input.stats();
            PlayerPipelineSnapshot status;
            status.serial = serial;
            status.controls = settings;
            status.hasVideo = worker->hasVideo(serial);
            status.hasAudio = audioState.serial == serial && audioState.activeTrack;
            status.performance = samplePerformance(false);
            if (network.network.state == network::ConnectionState::Reconnecting) {
                status.state = PlayerPipelineState::Reconnecting;
            } else if (network.network.connections == 0) {
                status.state = PlayerPipelineState::Connecting;
            } else if (settings.paused) {
                status.state = PlayerPipelineState::Paused;
            } else {
                // 低帧率视频保留其正常帧间隔；音频时钟已欠载且无视频进展时回到缓冲。
                const auto graceUs = std::max<std::int64_t>(1000000,
                    std::min<std::int64_t>(lastVideoDurationUs, 10000000) * 3);
                const bool recentVideo = status.hasVideo && lastVideoRender &&
                    now - *lastVideoRender < std::chrono::microseconds(graceUs);
                const bool audioAdvancing = status.hasAudio && audioClock.serial == serial &&
                    audioClock.valid && !audioClock.starved && !audioClock.paused;
                status.state = generationProgress && (recentVideo || audioAdvancing || audioProgress)
                    ? PlayerPipelineState::Playing : PlayerPipelineState::Buffering;
            }
            callback(status);
            nextStatus = now + std::chrono::milliseconds(100);
        }
        if (now - lastReport >= std::chrono::seconds(1)) {
            const auto metrics = samplePerformance(false);
            logger.log(logging::Level::Info, "performance", "M16 player decode_fps=" + std::to_string(metrics.decodeFps)
                + " received_payload_bps=" + std::to_string(metrics.receiveBitrate)
                + " dropped_video_frames=" + std::to_string(metrics.droppedVideoFrames)
                + " dropped_packets=" + std::to_string(metrics.droppedPackets)
                + " sync_valid=" + std::to_string(metrics.syncValid)
                + " sync_error_ms=" + std::to_string(metrics.syncErrorMs)
                + " capture_to_backend_ms=" + (metrics.latencyStatus == stats::LatencyStatus::Estimated
                    ? std::to_string(metrics.endToEndMs) : std::string("unavailable")));
            const auto currentDisplay = display.stats();
            report(logger, input.stats(), state, currentDisplay, audioState,
                   outputState, previousFrames, previousDisplay,
                   schedule.stats(), audioClock, std::chrono::duration<double>(now - lastReport).count());
            lastReport = now;
            previousFrames = state;
            previousDisplay = currentDisplay;
        }
        if (state.finished && audioState.finished && !pending && state.queued == 0 &&
            (!audioOutput || audioOutput->drained())) break;
        // 短暂让出 CPU，保持窗口事件、停止和网络换代检查及时。
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    } catch (...) {
        if (pending) { display.noteDroppedFrames(1); pending.reset(); }
        shutdown.stopAndJoin();
        try { finalSnapshot(); } catch (...) {}
        throw;
    }
    if (pending) { display.noteDroppedFrames(1); pending.reset(); }
    shutdown.stopAndJoin();
    finalSnapshot();
    if (!stoppedByUser) {
        input.checkError();
        static_cast<void>(frames.snapshot());
        static_cast<void>(audioWorker->stats());
    }
    return 0;
}

} // namespace lsl::player
