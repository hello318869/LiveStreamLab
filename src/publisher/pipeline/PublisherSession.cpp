#include "publisher/pipeline/PublisherSession.hpp"
#include "publisher/pipeline/PublisherPipeline.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace lsl::pipeline {

struct PublisherSession::Impl {
    using Clock = std::chrono::steady_clock;

    explicit Impl(logging::Logger& value) : logger(value) {}

    ~Impl() {
        requestStop();
        std::lock_guard<std::mutex> lifecycle(lifecycleMutex);
        if (worker.joinable()) worker.join();
    }

    bool start(const config::PublisherConfig& config, bool onlyPreview = false) {
        // 只有启动/回收线程需要串行；snapshot/requestStop 不等待 join。
        std::lock_guard<std::mutex> lifecycle(lifecycleMutex);
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (running) return false;
        }
        if (worker.joinable()) worker.join();
        try {
            auto ownedConfig = config;
            if (onlyPreview) {
                ownedConfig.capture.source = "devices";
                ownedConfig.capture.video = true;
                ownedConfig.capture.audio = false;
                ownedConfig.audioDevice = "default";
                ownedConfig.streaming.enabled = false;
                ownedConfig.videoFile.clear();
                ownedConfig.audioFile.clear();
                // 现有校验即使关闭推流也校验地址；预览不能依赖用户填写推流地址。
                ownedConfig.outputUrl = config::PublisherConfig{}.outputUrl;
            }
            config::validate(ownedConfig);
            {
                std::lock_guard<std::mutex> lock(mutex);
                view = PublisherSessionSnapshot{};
                view.state = PublisherSessionState::Starting;
                liveAt.reset();
                latestPreview.reset();
                previewOnly = onlyPreview;
                previousFrames = 0;
                previousCaptured = 0;
                previousBytes = 0;
                previousSample = Clock::now();
                stop.store(false, std::memory_order_relaxed);
                running = true;
            }
            worker = std::thread([this, ownedConfig = std::move(ownedConfig)] {
                run(ownedConfig);
            });
            return true;
        } catch (const std::exception& problem) {
            failStart(problem.what());
        } catch (...) {
            failStart("Unknown publisher startup failure");
        }
        return false;
    }

    void requestStop() noexcept {
        std::lock_guard<std::mutex> lock(mutex);
        if (!running) return;
        stop.store(true, std::memory_order_relaxed);
        view.state = PublisherSessionState::Stopping;
    }

    PublisherSessionSnapshot snapshot() const {
        std::lock_guard<std::mutex> lock(mutex);
        auto result = view;
        if (running && liveAt) result.liveSeconds = elapsed(*liveAt, Clock::now());
        return result;
    }

    std::shared_ptr<const PublisherPreviewFrame> preview() const {
        std::lock_guard<std::mutex> lock(mutex);
        return latestPreview;
    }

    void observePreview(std::shared_ptr<const PublisherPreviewFrame> frame) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!running || stop.load(std::memory_order_relaxed)) return;
        latestPreview = std::move(frame);
        if (previewOnly) {
            view.state = latestPreview ? PublisherSessionState::Previewing : PublisherSessionState::Starting;
        }
    }

    static double elapsed(Clock::time_point begin, Clock::time_point end) noexcept {
        return std::chrono::duration<double>(end - begin).count();
    }

    void observe(const PublisherPipelineStats& stats) {
        const auto now = Clock::now();
        std::lock_guard<std::mutex> lock(mutex);
        if (!running) return;
        if (!previewOnly && !liveAt && stats.sentPackets != 0) liveAt = now;
        auto performance = stats.performance;
        // 累计量/队列立即更新；速率至少积累 200 ms，避免启动回调的极短窗口尖峰。
        performance.sampled = view.performance.sampled;
        performance.captureFps = view.performance.captureFps;
        performance.encodeFps = view.performance.encodeFps;
        performance.sendBitrate = view.performance.sendBitrate;
        const double seconds = elapsed(previousSample, now);
        const bool rolledBack = performance.capturedVideoFrames < previousCaptured ||
            performance.encodedVideoFrames < previousFrames || performance.sentBytes < previousBytes;
        if (rolledBack) {
            performance.sampled = false;
            performance.captureFps = 0;
            performance.encodeFps = 0;
            performance.sendBitrate = 0;
        } else if (seconds >= 0.2) {
            performance.captureFps = static_cast<double>(performance.capturedVideoFrames - previousCaptured) / seconds;
            performance.encodeFps = static_cast<double>(performance.encodedVideoFrames - previousFrames) / seconds;
            performance.sendBitrate = static_cast<double>(performance.sentBytes - previousBytes) * 8 / seconds;
            performance.sampled = true;
        }
        if (rolledBack || seconds >= 0.2) {
            previousCaptured = performance.capturedVideoFrames;
            previousFrames = performance.encodedVideoFrames;
            previousBytes = performance.sentBytes;
            previousSample = now;
        }
        if (previewOnly) performance.sendBitrate = 0;
        if (stats.finished) {
            performance.captureFps = 0;
            performance.encodeFps = 0;
            performance.sendBitrate = 0;
        }
        view.performance = performance;
        view.videoFps = performance.encodeFps;
        view.bitrate = performance.sendBitrate;
        if (previewOnly) {
            view.liveSeconds = 0;
            view.bitrate = 0;
        } else if (liveAt) {
            view.liveSeconds = elapsed(*liveAt, now);
        }
        if (stop.load(std::memory_order_relaxed)) {
            view.state = PublisherSessionState::Stopping;
        } else if (previewOnly) {
            view.state = latestPreview ? PublisherSessionState::Previewing : PublisherSessionState::Starting;
        } else if (stats.networkState == network::ConnectionState::Reconnecting) {
            view.state = PublisherSessionState::Reconnecting;
        } else if (stats.networkState == network::ConnectionState::Streaming && stats.sentPackets != 0) {
            view.state = PublisherSessionState::Streaming;
        } else {
            view.state = PublisherSessionState::Starting;
        }
    }

    void run(const config::PublisherConfig& config) noexcept {
        try {
            runPublisherPipeline(config, logger, stop,
                [this](const PublisherPipelineStats& stats) { observe(stats); },
                [this](std::shared_ptr<const PublisherPreviewFrame> frame) { observePreview(std::move(frame)); });
            finish(nullptr);
        } catch (const std::exception& problem) {
            finish(problem.what());
        } catch (...) {
            finish("Unknown publisher worker failure");
        }
    }

    void finish(const char* problem) noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (liveAt) view.liveSeconds = elapsed(*liveAt, Clock::now());
            view.videoFps = 0;
            view.bitrate = 0;
            view.performance.captureFps = 0;
            view.performance.encodeFps = 0;
            view.performance.sendBitrate = 0;
            view.state = problem ? PublisherSessionState::Failed : PublisherSessionState::Stopped;
            // 即使错误文本分配失败，也保留终态，不能让异常逃出 std::thread。
            if (problem) {
                try { view.error = problem; } catch (...) { view.error.clear(); }
            }
            running = false;
        }
        if (problem) logger.log(logging::Level::Error, "publisher-session", problem);
    }

    void failStart(const char* problem) noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex);
            view.state = PublisherSessionState::Failed;
            view.liveSeconds = 0;
            view.videoFps = 0;
            view.bitrate = 0;
            view.performance = {};
            liveAt.reset();
            latestPreview.reset();
            running = false;
            try { view.error = problem; } catch (...) { view.error.clear(); }
        }
        logger.log(logging::Level::Error, "publisher-session", problem);
    }

    logging::Logger& logger;
    std::mutex lifecycleMutex;
    mutable std::mutex mutex;
    std::thread worker;
    std::atomic<bool> stop{false};
    PublisherSessionSnapshot view;
    bool running = false;
    bool previewOnly = false;
    std::shared_ptr<const PublisherPreviewFrame> latestPreview;
    std::optional<Clock::time_point> liveAt;
    Clock::time_point previousSample{};
    std::uint64_t previousFrames = 0;
    std::uint64_t previousCaptured = 0;
    std::uint64_t previousBytes = 0;
};

PublisherSession::PublisherSession(logging::Logger& logger) : impl_(std::make_unique<Impl>(logger)) {}
PublisherSession::~PublisherSession() = default;
bool PublisherSession::start(const config::PublisherConfig& config) { return impl_->start(config); }
bool PublisherSession::startPreview(const config::PublisherConfig& config) { return impl_->start(config, true); }
void PublisherSession::requestStop() noexcept { impl_->requestStop(); }
PublisherSessionSnapshot PublisherSession::snapshot() const { return impl_->snapshot(); }
std::shared_ptr<const PublisherPreviewFrame> PublisherSession::preview() const { return impl_->preview(); }

} // namespace lsl::pipeline
