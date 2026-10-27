#include "player/pipeline/PlayerSession.hpp"
#include "player/pipeline/PlayerPipeline.hpp"
#include "player/audio/IAudioSink.hpp"
#include "player/renderer/IVideoRenderer.hpp"
#include "common/media/FFmpegResources.hpp"
#include "common/stats/CaptureTimestamp.hpp"

extern "C" {
#include <libavutil/time.h>
}

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace lsl::player {

struct PlayerSession::Impl {
    using Clock = std::chrono::steady_clock;

    // 此适配器只产生 RGB 副本，不创建 SDL 窗口、不持有任何 Qt 对象。
    class MailboxRenderer final : public renderer::IVideoRenderer {
    public:
        explicit MailboxRenderer(Impl& owner) : owner_(owner) {}
        bool pollEvents() override { return true; }
        renderer::AudioControls takeAudioControls() override { return {}; }

        void render(const AVFrame& source) override {
            const auto begin = Clock::now();
            if (source.width <= 0 || source.height <= 0) {
                throw error::AppError("player-renderer", error::Io, "invalid video frame size");
            }
            const double sar = source.sample_aspect_ratio.num > 0 && source.sample_aspect_ratio.den > 0
                ? av_q2d(source.sample_aspect_ratio) : 1.0;
            const double displayWidth = source.width * sar;
            const double scale = std::min({1.0, 1920.0 / displayWidth, 1080.0 / source.height});
            auto result = std::make_shared<PlayerVideoFrame>();
            result->capturedUnixUs = stats::captureTimeUs(source);
            result->width = std::clamp(static_cast<int>(displayWidth * scale), 1, 1920);
            result->height = std::clamp(static_cast<int>(source.height * scale), 1, 1080);
            result->stride = (result->width * 3 + 31) & ~31;
            // 每帧最多 6,220,864 字节；后台只替换最新一帧，不给 UI 排队。
            result->pixels.resize(static_cast<std::size_t>(result->stride) * result->height
                + AV_INPUT_BUFFER_PADDING_SIZE);
            scaler_.reset(sws_getCachedContext(scaler_.release(), source.width, source.height,
                static_cast<AVPixelFormat>(source.format), result->width, result->height,
                AV_PIX_FMT_RGB24, SWS_BILINEAR, nullptr, nullptr, nullptr));
            if (!scaler_) throw error::AppError("player-renderer", error::Io, "cannot create RGB video scaler");
            int colorspace = SWS_CS_ITU601;
            if (source.colorspace == AVCOL_SPC_BT709) colorspace = SWS_CS_ITU709;
            else if (source.colorspace == AVCOL_SPC_BT2020_NCL || source.colorspace == AVCOL_SPC_BT2020_CL) {
                colorspace = SWS_CS_BT2020;
            }
            const auto* coefficients = sws_getCoefficients(colorspace);
            error::throwIfFFmpegFailed(sws_setColorspaceDetails(scaler_.get(), coefficients,
                source.color_range == AVCOL_RANGE_JPEG ? 1 : 0, coefficients, 1,
                0, 1 << 16, 1 << 16), "player-renderer", "configure RGB video colors");
            std::array<std::uint8_t*, 4> pixels{result->pixels.data(), nullptr, nullptr, nullptr};
            std::array<int, 4> strides{result->stride, 0, 0, 0};
            if (sws_scale(scaler_.get(), source.data, source.linesize, 0, source.height,
                          pixels.data(), strides.data()) != result->height) {
                throw error::AppError("player-renderer", error::Io, "incomplete RGB video frame");
            }
            {
                std::lock_guard<std::mutex> lock(owner_.mutex);
                if (!owner_.running || owner_.stop.load(std::memory_order_relaxed)) return;
                result->sequence = ++owner_.view.frameRevision;
                owner_.latestFrame = std::move(result);
            }
            ++stats_.renderedFrames;
            stats_.renderTimeUs += static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - begin).count());
        }

        void clear() override {
            std::lock_guard<std::mutex> lock(owner_.mutex);
            owner_.latestFrame.reset();
            ++owner_.view.frameRevision;
            owner_.minimumPresentedRevision = owner_.view.frameRevision;
            owner_.presentationAt.reset();
        }
        void noteDroppedFrames(std::uint64_t count) override { stats_.droppedFrames += count; }
        renderer::VideoRenderStats stats() const override { return stats_; }

    private:
        Impl& owner_;
        media::SwsContextPtr scaler_;
        renderer::VideoRenderStats stats_;
    };

    explicit Impl(logging::Logger& value) : logger(value), ownerThread(std::this_thread::get_id()) {}
    ~Impl() {
        requestStop();
        std::lock_guard<std::mutex> lifecycle(lifecycleMutex);
        if (worker.joinable()) worker.join();
        // AudioSubsystem 的 SDL_QuitSubSystem 在应用主线程执行。
        audioOutput.reset();
    }

    bool start(const config::PlayerConfig& config) {
        std::lock_guard<std::mutex> lifecycle(lifecycleMutex);
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (running) return false;
        }
        if (std::this_thread::get_id() != ownerThread) {
            failStart("PlayerSession::start must run on its application main thread");
            return false;
        }
        if (worker.joinable()) worker.join();
        audioOutput.reset();
        try {
            auto ownedConfig = config;
            config::validate(ownedConfig);
            // 仅初始化 AUDIO 子系统；真正打开流由既有 AudioPlayback/reset 路径执行。
            audioOutput = createPlayerAudioSink(ownedConfig, logger);
            {
                std::lock_guard<std::mutex> lock(mutex);
                const auto revision = view.frameRevision + 1;
                view = PlayerSessionSnapshot{};
                view.frameRevision = revision;
                view.state = PlayerSessionState::Connecting;
                view.muted = ownedConfig.audioPlayback.muted;
                view.paused = ownedConfig.audioPlayback.paused;
                view.volume = ownedConfig.audioPlayback.volume;
                desired.muted = view.muted;
                desired.paused = view.paused;
                desired.volume = view.volume;
                ++desired.revision;
                latestFrame.reset();
                minimumPresentedRevision = revision;
                lastPresentedSequence = 0;
                presentationAt.reset();
                presentationStatus = stats::LatencyStatus::Inactive;
                outputStarted = false;
                clockPaused = view.paused;
                clockAt = Clock::now();
                stop.store(false, std::memory_order_relaxed);
                running = true;
            }
            worker = std::thread([this, ownedConfig = std::move(ownedConfig)] { run(ownedConfig); });
            return true;
        } catch (const std::exception& problem) {
            failStart(problem.what());
        } catch (...) {
            failStart("Unknown player startup failure");
        }
        audioOutput.reset();
        return false;
    }

    void requestStop() noexcept {
        std::lock_guard<std::mutex> lock(mutex);
        if (!running) return;
        stop.store(true, std::memory_order_relaxed);
        view.state = PlayerSessionState::Stopping;
        view.performance.syncValid = false;
    }
    void setMuted(bool value) {
        std::lock_guard<std::mutex> lock(mutex);
        desired.muted = value;
        view.muted = value;
        ++desired.revision;
    }
    void setVolume(int value) {
        std::lock_guard<std::mutex> lock(mutex);
        desired.volume = std::clamp(value, 0, 100);
        view.volume = desired.volume;
        ++desired.revision;
    }
    void setPaused(bool value) {
        std::lock_guard<std::mutex> lock(mutex);
        desired.paused = value;
        view.paused = value;
        if (value) view.performance.syncValid = false;
        ++desired.revision;
    }
    PlayerPipelineControls controls() const {
        std::lock_guard<std::mutex> lock(mutex);
        return desired;
    }
    PlayerSessionSnapshot snapshot() const {
        std::lock_guard<std::mutex> lock(mutex);
        auto result = view;
        if (running && outputStarted && !clockPaused) {
            result.playbackSeconds += std::chrono::duration<double>(Clock::now() - clockAt).count();
        }
        const bool currentPresentation = running && !desired.paused && view.state == PlayerSessionState::Playing &&
            presentationAt && Clock::now() - *presentationAt < std::chrono::seconds(5);
        result.performance.syncValid = result.performance.syncValid && running && !desired.paused &&
            view.state == PlayerSessionState::Playing;
        result.performance.latencyStatus = currentPresentation ? presentationStatus : stats::LatencyStatus::Inactive;
        result.performance.endToEndMs = currentPresentation && presentationStatus == stats::LatencyStatus::Estimated
            ? presentationMs : 0;
        return result;
    }
    void notePresented(std::uint64_t sequence, std::int64_t capturedUnixUs) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!running || stop.load(std::memory_order_relaxed) || sequence <= minimumPresentedRevision ||
            sequence > view.frameRevision || sequence <= lastPresentedSequence) return;
        lastPresentedSequence = sequence;
        if (desired.paused || view.state != PlayerSessionState::Playing) return;
        presentationAt = Clock::now();
        const auto nowUs = av_gettime();
        if (capturedUnixUs <= 0) presentationStatus = stats::LatencyStatus::NoTimestamp;
        else if (capturedUnixUs > nowUs || nowUs - capturedUnixUs > 86400000000LL)
            presentationStatus = stats::LatencyStatus::ClockError;
        else {
            presentationStatus = stats::LatencyStatus::Estimated;
            presentationMs = (nowUs - capturedUnixUs) / 1000.0;
        }
    }
    std::shared_ptr<const PlayerVideoFrame> frame() const {
        std::lock_guard<std::mutex> lock(mutex);
        return latestFrame;
    }
    void updateClock(Clock::time_point now) noexcept {
        if (outputStarted && !clockPaused) {
            view.playbackSeconds += std::chrono::duration<double>(now - clockAt).count();
        }
        clockAt = now;
    }

    void observe(const PlayerPipelineSnapshot& status) {
        const auto now = Clock::now();
        std::lock_guard<std::mutex> lock(mutex);
        updateClock(now);
        if (!outputStarted && status.state == PlayerPipelineState::Playing) outputStarted = true;
        clockPaused = status.controls.paused;
        view.hasVideo = status.hasVideo;
        view.hasAudio = status.hasAudio;
        view.performance = status.performance;
        // GUI 的端点是 paintEvent，而非后台 RGB 交接；snapshot() 只发布绘制完成样本。
        view.performance.latencyStatus = stats::LatencyStatus::Inactive;
        view.performance.endToEndMs = 0;
        if (desired.revision == status.controls.revision) {
            view.muted = status.controls.muted;
            view.paused = status.controls.paused;
            view.volume = status.controls.volume;
        }
        if (stop.load(std::memory_order_relaxed)) {
            view.state = PlayerSessionState::Stopping;
            view.performance.syncValid = false;
            return;
        }
        switch (status.state) {
        case PlayerPipelineState::Connecting: view.state = PlayerSessionState::Connecting; break;
        case PlayerPipelineState::Buffering: view.state = PlayerSessionState::Buffering; break;
        case PlayerPipelineState::Playing: view.state = PlayerSessionState::Playing; break;
        case PlayerPipelineState::Paused: view.state = PlayerSessionState::Paused; break;
        case PlayerPipelineState::Reconnecting: view.state = PlayerSessionState::Reconnecting; break;
        }
    }

    void run(const config::PlayerConfig& config) noexcept {
        try {
            MailboxRenderer renderer(*this);
            runPlayerPipeline(config, logger, renderer, stop, audioOutput.get(),
                [this](const PlayerPipelineSnapshot& status) { observe(status); },
                [this] { return controls(); });
            finish(nullptr);
        } catch (const std::exception& problem) {
            finish(problem.what());
        } catch (...) {
            finish("Unknown player worker failure");
        }
    }

    void finish(const char* problem) noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex);
            updateClock(Clock::now());
            latestFrame.reset();
            ++view.frameRevision;
            minimumPresentedRevision = view.frameRevision;
            presentationAt.reset();
            view.performance.decodeFps = view.performance.receiveBitrate = 0;
            view.performance.videoPackets.size = view.performance.audioPackets.size = 0;
            view.performance.videoFrames.size = view.performance.pcm.size = 0;
            view.performance.pcmMilliseconds = 0;
            view.performance.syncValid = false;
            view.state = problem ? PlayerSessionState::Failed : PlayerSessionState::Stopped;
            if (problem) {
                try { view.error = problem; } catch (...) { view.error.clear(); }
            }
            running = false;
        }
        if (problem) logger.log(logging::Level::Error, "player-session", problem);
    }
    void failStart(const char* problem) noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex);
            latestFrame.reset();
            ++view.frameRevision;
            view.playbackSeconds = 0;
            view.hasVideo = false;
            view.hasAudio = false;
            view.state = PlayerSessionState::Failed;
            view.performance = stats::PlayerPerformance{};
            outputStarted = false;
            running = false;
            try { view.error = problem; } catch (...) { view.error.clear(); }
        }
        logger.log(logging::Level::Error, "player-session", problem);
    }

    logging::Logger& logger;
    const std::thread::id ownerThread;
    std::mutex lifecycleMutex;
    mutable std::mutex mutex;
    std::thread worker;
    std::unique_ptr<audio::IAudioSink> audioOutput;
    std::atomic<bool> stop{false};
    PlayerSessionSnapshot view;
    PlayerPipelineControls desired;
    std::shared_ptr<const PlayerVideoFrame> latestFrame;
    std::uint64_t minimumPresentedRevision = 0, lastPresentedSequence = 0;
    std::optional<Clock::time_point> presentationAt;
    stats::LatencyStatus presentationStatus = stats::LatencyStatus::Inactive;
    double presentationMs = 0;
    bool running = false;
    bool outputStarted = false;
    bool clockPaused = false;
    Clock::time_point clockAt{};
};

PlayerSession::PlayerSession(logging::Logger& logger) : impl_(std::make_unique<Impl>(logger)) {}
PlayerSession::~PlayerSession() = default;
bool PlayerSession::start(const config::PlayerConfig& config) { return impl_->start(config); }
void PlayerSession::requestStop() noexcept { impl_->requestStop(); }
void PlayerSession::setMuted(bool value) { impl_->setMuted(value); }
void PlayerSession::setVolume(int value) { impl_->setVolume(value); }
void PlayerSession::setPaused(bool value) { impl_->setPaused(value); }
PlayerSessionSnapshot PlayerSession::snapshot() const { return impl_->snapshot(); }
std::shared_ptr<const PlayerVideoFrame> PlayerSession::frame() const { return impl_->frame(); }
void PlayerSession::notePresented(std::uint64_t sequence, std::int64_t capturedUnixUs) {
    impl_->notePresented(sequence, capturedUnixUs);
}

} // namespace lsl::player
