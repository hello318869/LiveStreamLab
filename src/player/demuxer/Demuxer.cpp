#include "player/demuxer/Demuxer.hpp"
#include "player/demuxer/PacketBuffer.hpp"

extern "C" {
#include <libavutil/mathematics.h>
#include <libavutil/time.h>
}

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <limits>
#include <mutex>
#include <string>
#include <thread>

namespace lsl::demuxer {
namespace {
constexpr AVRational microseconds{1, AV_TIME_BASE};

void setOption(media::DictionaryPtr& options, const char* name, const std::string& value) {
    auto* raw = options.release();
    const int result = av_dict_set(&raw, name, value.c_str(), 0);
    options.reset(raw);
    error::throwIfFFmpegFailed(result, "demuxer", "set input option");
}

TrackInfo findTrack(AVFormatContext& context, AVMediaType type) {
    TrackInfo track;
    const int index = av_find_best_stream(&context, type, -1, -1, nullptr, 0);
    if (index == AVERROR_STREAM_NOT_FOUND) return track;
    error::throwIfFFmpegFailed(index, "demuxer", "locate media stream");
    const auto& stream = *context.streams[index];
    if (stream.time_base.num <= 0 || stream.time_base.den <= 0) {
        throw error::AppError("demuxer", error::InvalidConfig, "input stream has an invalid time base");
    }
    track.index = index;
    track.timeBase = stream.time_base;
    track.parameters.reset(avcodec_parameters_alloc());
    if (!track.parameters) throw std::bad_alloc{};
    error::throwIfFFmpegFailed(avcodec_parameters_copy(track.parameters.get(), stream.codecpar),
                              "demuxer", "copy input codec parameters");
    return track;
}

std::string describe(const TrackInfo& track) {
    if (!track.parameters) return "absent";
    const auto& p = *track.parameters;
    std::string text = "stream=" + std::to_string(track.index) + " codec=" + avcodec_get_name(p.codec_id);
    if (p.codec_type == AVMEDIA_TYPE_VIDEO) {
        text += " " + std::to_string(p.width) + "x" + std::to_string(p.height);
    } else {
        text += " " + std::to_string(p.sample_rate) + "Hz " + std::to_string(p.ch_layout.nb_channels) + "ch";
    }
    return text + " time_base=" + std::to_string(track.timeBase.num) + "/" + std::to_string(track.timeBase.den);
}

std::int64_t packetTime(const AVPacket& packet, AVRational timeBase) {
    const auto value = packet.dts != AV_NOPTS_VALUE ? packet.dts : packet.pts;
    if (value == AV_NOPTS_VALUE) return AV_NOPTS_VALUE;
    const long double scaled = static_cast<long double>(value) * timeBase.num * AV_TIME_BASE / timeBase.den;
    if (scaled <= static_cast<long double>(std::numeric_limits<std::int64_t>::min()) + 2 ||
        scaled >= static_cast<long double>(std::numeric_limits<std::int64_t>::max()) - 2) {
        throw error::FFmpegError("demuxer", AVERROR_INVALIDDATA, "convert input timestamp");
    }
    return av_rescale_q(value, timeBase, microseconds);
}

bool retryable(int code) noexcept {
    return code != AVERROR(ENOMEM) && code != AVERROR(EINVAL) &&
        code != AVERROR_PROTOCOL_NOT_FOUND && code != AVERROR_DEMUXER_NOT_FOUND &&
        code != AVERROR_HTTP_UNAUTHORIZED && code != AVERROR_HTTP_FORBIDDEN;
}
} // namespace

struct Demuxer::Impl {
    const config::PlayerConfig config;
    logging::Logger& logger;
    PacketBuffer buffer;
    std::thread worker;
    bool started = false; // start/join 由控制线程串行调用。
    std::atomic<bool> stopped{false};
    std::atomic<std::int64_t> ioDeadlineUs{0};
    std::atomic<std::int64_t> mediaDeadlineUs{0}; // 与单次 read 预算独立；垃圾包不能延长它。
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::exception_ptr failure;
    DemuxStats counters;
    network::ConnectionMonitor connection;

    // 仅网络线程访问；关闭上下文也只能由该线程执行。
    media::InputFormatPtr context;
    std::shared_ptr<const StreamInfo> streams;
    std::array<std::int64_t, 2> lastTimestamps{{AV_NOPTS_VALUE, AV_NOPTS_VALUE}};
    std::uint64_t session = 0;

    Impl(const config::PlayerConfig& options, logging::Logger& output)
        : config(options), logger(output), buffer(options.demux) {
        config::validate(options);
        static std::once_flag network;
        std::call_once(network, [] {
            error::throwIfFFmpegFailed(avformat_network_init(), "network", "initialize FFmpeg networking");
        });
    }

    ~Impl() {
        requestStop();
        if (worker.joinable()) worker.join();
    }

    static int interrupt(void* opaque) noexcept {
        const auto& self = *static_cast<Impl*>(opaque);
        if (self.stopped.load(std::memory_order_relaxed)) return 1;
        const auto now = av_gettime_relative();
        const auto deadline = self.ioDeadlineUs.load(std::memory_order_relaxed);
        const auto mediaDeadline = self.mediaDeadlineUs.load(std::memory_order_relaxed);
        return (deadline != 0 && now >= deadline) || (mediaDeadline != 0 && now >= mediaDeadline);
    }

    void deadline(int milliseconds) noexcept {
        ioDeadlineUs.store(av_gettime_relative() + static_cast<std::int64_t>(milliseconds) * 1000,
                           std::memory_order_relaxed);
    }

    void checkIo(int result, const char* operation) const {
        checkInactivity();
        const auto end = ioDeadlineUs.load(std::memory_order_relaxed);
        if (!stopped.load(std::memory_order_relaxed) &&
            end != 0 && av_gettime_relative() >= end) {
            throw error::FFmpegError("network", AVERROR(ETIMEDOUT), operation);
        }
        error::throwIfFFmpegFailed(result, "network", operation);
    }

    void renewMediaDeadline() noexcept {
        mediaDeadlineUs.store(config.demux.inactivityTimeoutMs == 0 ? 0 :
            av_gettime_relative() + static_cast<std::int64_t>(config.demux.inactivityTimeoutMs) * 1000,
            std::memory_order_relaxed);
    }

    void checkInactivity() const {
        const auto end = mediaDeadlineUs.load(std::memory_order_relaxed);
        if (!stopped.load(std::memory_order_relaxed) && end != 0 && av_gettime_relative() >= end) {
            throw error::FFmpegError("network", AVERROR(ETIMEDOUT), "live input produced no usable media within inactivity timeout");
        }
    }

    void close() noexcept {
        // 某些协议关闭也执行 I/O，仍保留超时/停止中断回调。
        mediaDeadlineUs.store(0, std::memory_order_relaxed);
        deadline(config.demux.readTimeoutMs);
        context.reset();
        ioDeadlineUs.store(0, std::memory_order_relaxed);
    }

    bool waitRetry(int milliseconds) {
        std::unique_lock<std::mutex> lock(mutex);
        return wake.wait_for(lock, std::chrono::milliseconds(milliseconds),
            [&] { return stopped.load(std::memory_order_relaxed); });
    }

    void open() {
        context.reset(avformat_alloc_context());
        if (!context) throw std::bad_alloc{};
        context->interrupt_callback = AVIOInterruptCB{interrupt, this};
        context->probesize = 2 * 1024 * 1024;
        context->max_analyze_duration = 3000000;
        // 按 PTS 调度直播，不必累计几十帧估算 FPS；仍完整发现编码参数并保留初始关键帧。
        context->fps_probe_size = 0;
        media::DictionaryPtr options;
        setOption(options, "rw_timeout", std::to_string(static_cast<std::int64_t>(config.demux.readTimeoutMs) * 1000));
        if (config.inputUrl.rfind("rtmp", 0) == 0) setOption(options, "rtmp_live", "live");
        const auto* flv = av_find_input_format("flv");
        if (!flv) throw error::FFmpegError("demuxer", AVERROR_DEMUXER_NOT_FOUND, "find FLV demuxer");

        deadline(config.demux.connectTimeoutMs);
        auto* raw = context.release();
        auto* dictionary = options.release();
        const int result = avformat_open_input(&raw, config.inputUrl.c_str(), flv, &dictionary);
        context.reset(raw); // 失败时 FFmpeg 可能已释放上下文并将 raw 设为 nullptr。
        options.reset(dictionary);
        checkIo(result, "open live input");
        // 打开和探测共享连接预算，避免无限等待缺失的轨道。
        const auto probeStartedUs = av_gettime_relative();
        checkIo(avformat_find_stream_info(context.get(), nullptr), "discover live streams");
        const auto probeElapsedMs = (av_gettime_relative() - probeStartedUs) / 1000;
        if (stopped.load(std::memory_order_relaxed)) return;

        auto info = std::make_shared<StreamInfo>();
        info->session = ++session;
        info->video = findTrack(*context, AVMEDIA_TYPE_VIDEO);
        info->audio = findTrack(*context, AVMEDIA_TYPE_AUDIO);
        if (info->video.index < 0 && info->audio.index < 0) {
            throw error::AppError("demuxer", error::InvalidConfig, "input has no audio or video stream");
        }
        streams = std::move(info);
        lastTimestamps.fill(AV_NOPTS_VALUE);
        buffer.reset(streams);
        connection.connected(); // 停止已发生时 monitor 不会重新进入 STREAMING。
        renewMediaDeadline();
        {
            std::lock_guard<std::mutex> lock(mutex);
            ++counters.connections;
        }
        logger.log(logging::Level::Info, "demuxer", std::string("input state=") +
            network::stateName(connection.stats().state) + " session=" + std::to_string(session) +
            " probe_ms=" + std::to_string(probeElapsedMs) +
            " video: " + describe(streams->video) + "; audio: " + describe(streams->audio));
        ioDeadlineUs.store(0, std::memory_order_relaxed);
    }

    // EOF/停止时返回；其他负返回码统一抛出给外层重连策略。
    void readUntilEof(int& consecutiveFailures) {
        deadline(config.demux.readTimeoutMs);
        while (!stopped.load(std::memory_order_relaxed)) {
            checkInactivity();
            media::PacketPtr packet{av_packet_alloc()};
            if (!packet) throw std::bad_alloc{};
            const int result = av_read_frame(context.get(), packet.get());
            if (stopped.load(std::memory_order_relaxed)) return;
            checkInactivity(); // 即使 read 不断成功返回非媒体数据，也检查连续无媒体时间。
            if (result == AVERROR(EAGAIN) || result == AVERROR(EINTR)) {
                // 不重置截止时间，持续 EAGAIN 也必须在读预算内结束。
                const auto end = ioDeadlineUs.load(std::memory_order_relaxed);
                if (av_gettime_relative() >= end) checkIo(AVERROR(ETIMEDOUT), "read live packet");
                if (waitRetry(5)) return;
                continue;
            }
            if (result == AVERROR_EOF) return;
            checkIo(result, "read live packet");
            deadline(config.demux.readTimeoutMs);

            TrackKind kind;
            AVRational timeBase;
            if (packet->stream_index == streams->video.index) {
                kind = TrackKind::Video;
                timeBase = streams->video.timeBase;
            } else if (packet->stream_index == streams->audio.index) {
                kind = TrackKind::Audio;
                timeBase = streams->audio.timeBase;
            } else {
                std::lock_guard<std::mutex> lock(mutex);
                ++counters.ignoredPackets;
                continue;
            }
            if ((packet->flags & AV_PKT_FLAG_CORRUPT) != 0 || packet->size < 0 ||
                (packet->size > 0 && !packet->data)) {
                buffer.invalidate(); // 损坏的预测包不能只单包丢弃后继续解码。
                std::lock_guard<std::mutex> lock(mutex);
                ++counters.ignoredPackets;
                continue;
            }
            if (packet->size == 0) {
                std::lock_guard<std::mutex> lock(mutex);
                ++counters.ignoredPackets;
                continue;
            }
            const auto timestamp = packetTime(*packet, timeBase);
            const auto index = kind == TrackKind::Video ? 0u : 1u;
            const auto previous = lastTimestamps[index];
            const auto threshold = static_cast<std::int64_t>(config.demux.discontinuityThresholdMs) * 1000;
            if (previous != AV_NOPTS_VALUE && timestamp != AV_NOPTS_VALUE &&
                (static_cast<long double>(timestamp) - previous > threshold ||
                 static_cast<long double>(previous) - timestamp > threshold)) {
                buffer.invalidate();
                lastTimestamps.fill(AV_NOPTS_VALUE);
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    ++counters.timestampResets;
                }
                logger.log(logging::Level::Warn, "demuxer", "input timestamp discontinuity; reset queues and clocks");
            }
            if (timestamp != AV_NOPTS_VALUE) lastTimestamps[index] = timestamp;
            packet->time_base = timeBase;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (kind == TrackKind::Video) ++counters.videoPackets;
                else ++counters.audioPackets;
                counters.bytes += static_cast<std::uint64_t>(packet->size);
            }
            // 实际收到的 payload 均计入吞吐量；只有能交给下游的媒体才表示恢复。
            // 等待关键帧期间持续出现 P 帧不能无限刷新无数据检测与连续失败预算。
            const auto receivedBytes = static_cast<std::uint64_t>(packet->size);
            const bool accepted = buffer.push(kind, std::move(packet), timestamp);
            connection.media(receivedBytes, accepted);
            if (accepted) {
                consecutiveFailures = 0;
                renewMediaDeadline();
            }
        }
    }

    void run() noexcept {
        bool terminalFailure = false;
        std::exception_ptr recordedError;
        try {
            int consecutiveFailures = 0;
            logger.log(logging::Level::Info, "network", "input state=CONNECTING");
            while (!stopped.load(std::memory_order_relaxed)) {
                try {
                    open();
                    if (stopped.load(std::memory_order_relaxed)) break;
                    readUntilEof(consecutiveFailures);
                    if (stopped.load(std::memory_order_relaxed)) break;
                    if (config.demux.reconnectAttempts == 0) {
                        connection.setState(network::ConnectionState::Stopping);
                        close();
                        buffer.finish();
                        connection.finish();
                        return;
                    }
                    throw error::FFmpegError("network", AVERROR_EOF, "live input disconnected");
                } catch (const error::FFmpegError& e) {
                    if (stopped.load(std::memory_order_relaxed)) break;
                    recordedError = std::current_exception();
                    connection.recordError(e.code, e.what());
                    const bool canRetry = retryable(e.code) && (config.demux.reconnectAttempts < 0 ||
                        consecutiveFailures < config.demux.reconnectAttempts);
                    if (!canRetry) terminalFailure = true;
                    connection.setState(canRetry ? network::ConnectionState::Reconnecting : network::ConnectionState::Error);
                    // 先撤销快照/代次，令音视频线程立即释放旧解码器、PCM 和待显示帧。
                    // 关闭协议可能仍进行 I/O，不能到 close 完毕后才清理播放侧。
                    buffer.disconnect();
                    streams.reset();
                    lastTimestamps.fill(AV_NOPTS_VALUE);
                    close();
                    if (!canRetry) throw;
                    if (stopped.load(std::memory_order_relaxed)) break;
                    if (consecutiveFailures < std::numeric_limits<int>::max()) ++consecutiveFailures;
                    const auto delay = network::reconnectDelayMs(config.demux.reconnectDelayMs,
                        config.demux.reconnectMaxDelayMs, static_cast<std::uint64_t>(consecutiveFailures));
                    connection.retry(consecutiveFailures, delay);
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        ++counters.reconnects;
                    }
                    logger.log(logging::Level::Warn, "network", "input state=RECONNECTING attempt=" +
                        std::to_string(consecutiveFailures) + "/" +
                        (config.demux.reconnectAttempts < 0 ? "unlimited" : std::to_string(config.demux.reconnectAttempts)) +
                        " delay_ms=" + std::to_string(delay) + ": " + e.what(), e.code);
                    if (waitRetry(delay)) break;
                }
            }
        } catch (...) {
            if (terminalFailure || !stopped.load(std::memory_order_relaxed)) {
                const auto problem = std::current_exception();
                terminalFailure = true;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    failure = problem;
                }
                if (problem != recordedError) {
                    try {
                        std::rethrow_exception(problem);
                    } catch (const error::AppError& e) {
                        connection.recordError(e.code, e.what());
                    } catch (const std::exception& e) {
                        connection.recordError(error::Unexpected, e.what());
                    } catch (...) {
                        connection.recordError(error::Unexpected, "unknown live input error");
                    }
                }
                connection.finish(true);
                buffer.disconnect();
                streams.reset();
                logger.log(logging::Level::Error, "network", "input state=ERROR: " +
                    connection.stats().lastErrorMessage, connection.stats().lastErrorCode);
            }
        }
        if (!terminalFailure) connection.setState(network::ConnectionState::Stopping);
        close();
        buffer.finish();
        connection.finish(terminalFailure);
    }

    void requestStop() noexcept {
        {
            // 与 waitRetry 的谓词共用锁，避免停止通知恰好发生在入睡前而丢失唤醒。
            std::lock_guard<std::mutex> lock(mutex);
            stopped.store(true, std::memory_order_relaxed);
        }
        connection.requestStop();
        buffer.stop();
        wake.notify_all();
    }
};

Demuxer::Demuxer(const config::PlayerConfig& config, logging::Logger& logger)
    : impl_(std::make_unique<Impl>(config, logger)) {}
Demuxer::~Demuxer() = default;

void Demuxer::start() {
    if (impl_->started || impl_->stopped.load(std::memory_order_relaxed)) {
        throw error::AppError("demuxer", error::InvalidConfig, "demuxer can only be started once");
    }
    impl_->started = true; // 线程创建失败也结束本对象的启动机会，调用方应创建新会话。
    impl_->connection.start();
    try {
        impl_->worker = std::thread([state = impl_.get()] { state->run(); });
    } catch (const std::exception& e) {
        impl_->connection.recordError(error::Unexpected, std::string("start live input worker: ") + e.what());
        impl_->connection.finish(true);
        impl_->buffer.finish();
        throw;
    }
}
PacketEvent Demuxer::pop(TrackKind track, int timeoutMs) { return impl_->buffer.pop(track, timeoutMs); }
bool Demuxer::resynchronize(std::uint64_t expectedSerial) {
    return impl_->buffer.resynchronize(expectedSerial);
}
void Demuxer::requestStop() noexcept { impl_->requestStop(); }
void Demuxer::join() { if (impl_->worker.joinable()) impl_->worker.join(); }
void Demuxer::checkError() const {
    std::exception_ptr saved;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        saved = impl_->failure;
    }
    if (saved) std::rethrow_exception(saved);
}
DemuxStats Demuxer::stats() const {
    DemuxStats result;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        result = impl_->counters;
    }
    result.buffer = impl_->buffer.stats();
    result.network = impl_->connection.stats();
    return result;
}

} // namespace lsl::demuxer
