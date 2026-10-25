#include "publisher/rtmp/RtmpPublisher.hpp"

extern "C" {
#include <libavutil/mathematics.h>
#include <libavutil/time.h>
}

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace lsl::rtmp {
namespace {
constexpr AVRational microseconds{1, AV_TIME_BASE};

[[noreturn]] void invalid(const std::string& message) {
    throw error::AppError("mux", error::InvalidConfig, message);
}

std::int64_t rescale(std::int64_t value, AVRational from, AVRational to,
                     AVRounding rounding = AV_ROUND_NEAR_INF) {
    // av_rescale_q_rnd 以 INT64_MIN 表示溢出，事先检查还能避免把它当合法时间戳。
    const long double scaled = static_cast<long double>(value) * from.num * to.den / from.den / to.num;
    if (scaled <= static_cast<long double>(std::numeric_limits<std::int64_t>::min()) + 2 ||
        scaled >= static_cast<long double>(std::numeric_limits<std::int64_t>::max()) - 2) {
        invalid("packet timestamp overflows during time-base conversion");
    }
    const auto result = av_rescale_q_rnd(value, from, to, rounding);
    if (result == AV_NOPTS_VALUE) invalid("packet timestamp conversion overflow");
    return result;
}

std::int64_t subtract(std::int64_t value, std::int64_t offset) {
    if ((offset > 0 && value < std::numeric_limits<std::int64_t>::min() + offset) ||
        (offset < 0 && value > std::numeric_limits<std::int64_t>::max() + offset)) {
        invalid("packet timestamp overflows while rebasing the session");
    }
    return value - offset;
}

void setOption(media::DictionaryPtr& options, const char* name, const std::string& value) {
    auto* raw = options.release();
    const int result = av_dict_set(&raw, name, value.c_str(), 0);
    options.reset(raw);
    error::throwIfFFmpegFailed(result, "mux", "set output option");
}
} // namespace

struct RtmpPublisher::Impl {
    struct Item {
        media::PacketPtr packet;
        std::int64_t dtsUs = 0;
        std::int64_t queuedAtUs = 0;
    };
    struct Track {
        bool enabled = false;
        bool ended = false;
        media::CodecParametersPtr parameters;
        AVRational timeBase{0, 1};
        std::deque<Item> queue;
        std::int64_t inputDts = AV_NOPTS_VALUE;
    };

    const config::StreamingConfig options;
    const std::string url;
    logging::Logger& logger;
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::array<Track, 2> tracks; // 0 视频，1 音频。
    std::thread worker;
    bool started = false;
    bool finishing = false;
    std::exception_ptr failure;
    PublisherStats counters;
    std::uint64_t submittedPackets = 0; // mutex 保护；区分空输入与未能发布任何已编码内容。
    std::uint64_t queueGeneration = 0;
    std::int64_t startedAtUs = 0;
    std::int64_t endedAtUs = 0;
    std::atomic<bool> stopped{false};
    std::atomic<std::int64_t> ioDeadlineUs{0};
    std::atomic<std::int64_t> drainDeadlineUs{0};

    // 以下字段只由复用线程访问；生产者绝不释放 FFmpeg 正在使用的上下文。
    media::OutputFormatPtr context;
    std::array<int, 2> streamIndices{{-1, -1}};
    std::array<std::int64_t, 2> outputDts{{AV_NOPTS_VALUE, AV_NOPTS_VALUE}};
    bool awaitingKey = true;
    bool recovering = false;
    bool headerWritten = false;
    bool hasSessionOrigin = false;
    bool reopening = false;
    std::exception_ptr pendingNetworkFailure;
    std::int64_t sessionOriginUs = 0;
    std::int64_t cutoffUs = std::numeric_limits<std::int64_t>::min();
    std::uint64_t observedGeneration = 0;
    int consecutiveFailures = 0;

    Impl(const config::PublisherConfig& config, logging::Logger& output)
        : options(config.streaming), url(config.outputUrl), logger(output) {
        char protocol[16]{};
        char host[1024]{};
        char path[4096]{};
        int port = -1;
        av_url_split(protocol, sizeof(protocol), nullptr, 0, host, sizeof(host), &port,
                     path, sizeof(path), url.c_str());
        if ((std::string(protocol) != "rtmp" && std::string(protocol) != "rtmps") ||
            host[0] == '\0' || path[0] != '/' || path[1] == '\0' || port == 0 || port > 65535) {
            invalid("RTMP output requires a valid rtmp(s) host, optional port and application/stream path");
        }
        // 进程级网络初始化不随会话重连反复开关，以免干扰其他 FFmpeg 输入。
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
        const auto& state = *static_cast<Impl*>(opaque);
        if (state.stopped.load(std::memory_order_relaxed)) return 1;
        const auto now = av_gettime_relative();
        const auto operation = state.ioDeadlineUs.load(std::memory_order_relaxed);
        const auto draining = state.drainDeadlineUs.load(std::memory_order_relaxed);
        return (operation != 0 && now >= operation) || (draining != 0 && now >= draining);
    }

    void deadline(int milliseconds) noexcept {
        ioDeadlineUs.store(av_gettime_relative() + static_cast<std::int64_t>(milliseconds) * 1000,
                           std::memory_order_relaxed);
    }

    void clearQueues() {
        for (auto& track : tracks) {
            counters.droppedPackets += track.queue.size();
            track.queue.clear();
        }
    }

    void requestStop() noexcept {
        stopped.store(true, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(mutex);
            clearQueues();
        }
        changed.notify_all();
    }

    void checkError() const {
        std::exception_ptr saved;
        {
            std::lock_guard<std::mutex> lock(mutex);
            saved = failure;
        }
        if (saved) std::rethrow_exception(saved);
    }

    void setParameters(std::size_t index, const AVCodecParameters& parameters, AVRational timeBase) {
        if (stopped.load(std::memory_order_relaxed)) return;
        checkError();
        if (timeBase.num <= 0 || timeBase.den <= 0) invalid("encoder time base must be positive");
        if (parameters.codec_id != (index == 0 ? AV_CODEC_ID_H264 : AV_CODEC_ID_AAC) ||
            parameters.codec_type != (index == 0 ? AVMEDIA_TYPE_VIDEO : AVMEDIA_TYPE_AUDIO)) {
            invalid("FLV streaming accepts only H.264 video and AAC audio");
        }
        if (!parameters.extradata || parameters.extradata_size <= 0) {
            invalid("FLV needs encoder global headers (H.264 SPS/PPS or AAC AudioSpecificConfig)");
        }
        media::CodecParametersPtr copy{avcodec_parameters_alloc()};
        if (!copy) throw std::bad_alloc{};
        error::throwIfFFmpegFailed(avcodec_parameters_copy(copy.get(), &parameters), "mux", "copy encoder parameters");
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (stopped.load(std::memory_order_relaxed)) return;
            auto& track = tracks[index];
            if (!started || !track.enabled || track.ended || track.parameters) {
                invalid("encoder parameters must be registered once for an active output track");
            }
            track.parameters = std::move(copy);
            track.timeBase = timeBase;
        }
        changed.notify_all();
    }

    void submit(std::size_t index, media::PacketPtr packet) {
        if (!packet) invalid("cannot submit an empty encoded packet");
        std::lock_guard<std::mutex> lock(mutex);
        if (stopped.load(std::memory_order_relaxed)) {
            ++counters.droppedPackets;
            return;
        }
        if (failure) std::rethrow_exception(failure);
        auto& track = tracks[index];
        if (!started || finishing || !track.enabled || track.ended || !track.parameters) {
            invalid("packet submitted before parameters or after the track ended");
        }
        if (packet->pts == AV_NOPTS_VALUE || packet->dts == AV_NOPTS_VALUE || packet->duration < 0 ||
            packet->size <= 0 || !packet->data) {
            invalid("encoded packet requires data, PTS/DTS and non-negative duration");
        }
        if (track.inputDts != AV_NOPTS_VALUE && packet->dts <= track.inputDts) {
            invalid("encoder DTS must be strictly increasing within each track");
        }
        if (packet->time_base.num != 0 && (packet->time_base.num <= 0 || packet->time_base.den <= 0 ||
            av_cmp_q(packet->time_base, track.timeBase) != 0)) {
            invalid("packet time base does not match its registered encoder");
        }
        const auto dtsUs = rescale(packet->dts, track.timeBase, microseconds, AV_ROUND_DOWN);
        static_cast<void>(rescale(packet->pts, track.timeBase, microseconds));
        static_cast<void>(rescale(packet->duration, track.timeBase, microseconds));
        track.inputDts = packet->dts;
        ++submittedPackets;
        const auto queued = tracks[0].queue.size() + tracks[1].queue.size();
        if (queued >= static_cast<std::size_t>(options.queueCapacity)) {
            // 不能阻塞生产者：本地文件的同一线程还要继续读出另一个轨道。
            // 丢掉积压后由复用线程等待下一关键帧，避免输出断裂的预测链。
            clearQueues();
            ++queueGeneration;
        }
        track.queue.push_back(Item{std::move(packet), dtsUs, av_gettime_relative()});
        changed.notify_all();
    }

    void finishTrack(std::size_t index) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            tracks[index].ended = true;
        }
        changed.notify_all();
    }

    bool ready() const {
        for (const auto& track : tracks) {
            if (track.enabled && !track.parameters && !track.ended) return false;
        }
        return true;
    }

    bool allEnded() const {
        return (!tracks[0].enabled || tracks[0].ended) && (!tracks[1].enabled || tracks[1].ended);
    }

    bool expired() const noexcept {
        const auto end = drainDeadlineUs.load(std::memory_order_relaxed);
        return end != 0 && av_gettime_relative() >= end;
    }

    int closeSession(bool writeTrailer) noexcept {
        if (!context) return 0;
        int trailerResult = 0;
        deadline(options.writeTimeoutMs);
        if (writeTrailer && headerWritten && !stopped.load(std::memory_order_relaxed) && !expired()) {
            const int result = av_write_trailer(context.get());
            trailerResult = result;
            if (result < 0 && !stopped.load(std::memory_order_relaxed) && !expired()) {
                logger.log(logging::Level::Warn, "network", "FLV trailer could not be completed", result);
            }
        }
        // 显式收集 flush/close 错误；正常 EOF 不能把失败的网络写尾报告为成功。
        // callback 及其 owner 一直保留到关闭结束，普通 RAII 只负责剩余上下文释放。
        if (context->pb) {
            if (writeTrailer && !stopped.load(std::memory_order_relaxed) && !expired()) avio_flush(context->pb);
            if (trailerResult >= 0 && context->pb->error < 0) trailerResult = context->pb->error;
            const int closed = avio_closep(&context->pb);
            if (trailerResult >= 0 && closed < 0) trailerResult = closed;
        }
        context.reset();
        ioDeadlineUs.store(0, std::memory_order_relaxed);
        headerWritten = false;
        streamIndices = {{-1, -1}};
        outputDts = {{AV_NOPTS_VALUE, AV_NOPTS_VALUE}};
        return trailerResult;
    }

    void openSession() {
        AVFormatContext* raw = nullptr;
        error::throwIfFFmpegFailed(avformat_alloc_output_context2(&raw, nullptr, "flv", nullptr),
                                   "mux", "allocate FLV output context");
        if (!raw) throw std::bad_alloc{};
        context.reset(raw);
        context->interrupt_callback = AVIOInterruptCB{interrupt, this};
        context->max_interleave_delta = static_cast<std::int64_t>(options.maxInterleaveDeltaMs) * 1000;
        context->flags |= AVFMT_FLAG_FLUSH_PACKETS;
        context->avoid_negative_ts = AVFMT_AVOID_NEG_TS_DISABLED;
        {
            std::lock_guard<std::mutex> lock(mutex);
            for (std::size_t index = 0; index < tracks.size(); ++index) {
                const auto& track = tracks[index];
                if (!track.enabled || !track.parameters) continue;
                const int supported = avformat_query_codec(context->oformat, track.parameters->codec_id, FF_COMPLIANCE_NORMAL);
                if (supported == 0) invalid("FLV muxer rejects the configured encoder codec");
                auto* stream = avformat_new_stream(context.get(), nullptr);
                if (!stream) throw std::bad_alloc{};
                error::throwIfFFmpegFailed(avcodec_parameters_copy(stream->codecpar, track.parameters.get()),
                                           "mux", "copy FLV stream parameters");
                stream->codecpar->codec_tag = 0;
                stream->time_base = track.timeBase;
                streamIndices[index] = stream->index;
            }
        }
        media::DictionaryPtr networkOptions;
        setOption(networkOptions, "rw_timeout", std::to_string(static_cast<std::int64_t>(options.writeTimeoutMs) * 1000));
        setOption(networkOptions, "rtmp_live", "live");
        setOption(networkOptions, "tcp_nodelay", "1");
        auto* dictionary = networkOptions.release();
        deadline(options.connectTimeoutMs);
        const int opened = avio_open2(&context->pb, url.c_str(), AVIO_FLAG_WRITE,
                                     &context->interrupt_callback, &dictionary);
        networkOptions.reset(dictionary);
        error::throwIfFFmpegFailed(opened, "network", "connect RTMP output");
        media::DictionaryPtr muxOptions;
        // 直播不可 seek；不尝试回填 FLV duration/filesize。
        setOption(muxOptions, "flvflags", "no_duration_filesize");
        dictionary = muxOptions.release();
        deadline(options.connectTimeoutMs);
        const int header = avformat_write_header(context.get(), &dictionary);
        muxOptions.reset(dictionary);
        error::throwIfFFmpegFailed(header, "network", "write FLV header");
        headerWritten = true;
        ioDeadlineUs.store(0, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (reopening) ++counters.reconnects;
        }
        reopening = false;
        logger.log(logging::Level::Info, "mux", "RTMP session ready: FLV, H.264/AAC encoder headers; stream time bases negotiated");
        for (const auto streamIndex : streamIndices) {
            if (streamIndex < 0) continue;
            const auto* stream = context->streams[streamIndex];
            const auto* parameters = stream->codecpar;
            const bool video = parameters->codec_type == AVMEDIA_TYPE_VIDEO;
            const auto format = video ? std::to_string(parameters->width) + "x" + std::to_string(parameters->height)
                : std::to_string(parameters->sample_rate) + "Hz " + std::to_string(parameters->ch_layout.nb_channels) + "ch";
            logger.log(logging::Level::Info, "mux", std::string(video ? "video" : "audio")
                + " stream=" + std::to_string(streamIndex) + " codec=" + avcodec_get_name(parameters->codec_id)
                + " format=" + format + " time_base=" + std::to_string(stream->time_base.num)
                + "/" + std::to_string(stream->time_base.den));
        }
    }

    bool take(Item& result, std::size_t& index) {
        std::unique_lock<std::mutex> lock(mutex);
        for (;;) {
            if (stopped.load(std::memory_order_relaxed) || expired()) return false;
            if (!ready()) {
                changed.wait_for(lock, std::chrono::milliseconds(20));
                continue;
            }
            if (queueGeneration != observedGeneration) {
                observedGeneration = queueGeneration;
                awaitingKey = true;
                recovering = true;
                logger.log(logging::Level::Warn, "mux", "Mux queue overflow: dropped backlog, waiting for a new video keyframe");
            }
            auto& video = tracks[0];
            auto& audio = tracks[1];
            const bool hasVideo = video.enabled && video.parameters;
            if (awaitingKey) {
                if (hasVideo) {
                    while (!video.queue.empty() && !(video.queue.front().packet->flags & AV_PKT_FLAG_KEY)) {
                        video.queue.pop_front();
                        ++counters.droppedPackets;
                    }
                    if (video.queue.empty()) {
                        if (video.ended) {
                            // 视频已结束且找不到新关键帧，无法建立可独立解码的新会话。
                            counters.droppedPackets += audio.queue.size();
                            audio.queue.clear();
                            if (allEnded()) return false;
                        }
                        changed.wait_for(lock, std::chrono::milliseconds(20));
                        continue;
                    }
                    // 初次建流容纳 AAC priming 的负 DTS；重连/丢积压从该关键帧同步恢复。
                    const auto keyDts = video.queue.front().dtsUs;
                    if (!recovering && audio.enabled && audio.parameters && audio.queue.empty() && !audio.ended &&
                        av_gettime_relative() - video.queue.front().queuedAtUs <
                            static_cast<std::int64_t>(options.maxInterleaveDeltaMs) * 1000) {
                        changed.wait_for(lock, std::chrono::milliseconds(10));
                        continue;
                    }
                    cutoffUs = recovering ? keyDts : std::numeric_limits<std::int64_t>::min();
                    if (!hasSessionOrigin) {
                        sessionOriginUs = keyDts;
                        if (!recovering && !audio.queue.empty()) sessionOriginUs = std::min(sessionOriginUs, audio.queue.front().dtsUs);
                        hasSessionOrigin = true;
                    }
                    if (!recovering) cutoffUs = sessionOriginUs;
                } else {
                    if (audio.queue.empty()) {
                        if (allEnded()) return false;
                        changed.wait_for(lock, std::chrono::milliseconds(20));
                        continue;
                    }
                    cutoffUs = audio.queue.front().dtsUs;
                    if (!hasSessionOrigin) {
                        sessionOriginUs = cutoffUs;
                        hasSessionOrigin = true;
                    }
                }
                awaitingKey = false;
            }
            // 未来迟到的音频也必须跨过恢复点；不能各轨分别清零破坏音画偏移。
            for (auto& track : tracks) {
                while (!track.queue.empty() && track.queue.front().dtsUs < cutoffUs) {
                    track.queue.pop_front();
                    ++counters.droppedPackets;
                }
            }
            if (video.queue.empty() && audio.queue.empty()) {
                if (allEnded()) return false;
                changed.wait_for(lock, std::chrono::milliseconds(20));
                continue;
            }
            index = video.queue.empty() ? 1 : audio.queue.empty() ? 0 :
                (av_compare_ts(video.queue.front().packet->dts, video.timeBase,
                               audio.queue.front().packet->dts, audio.timeBase) <= 0 ? 0 : 1);
            const auto other = 1 - index;
            auto& selected = tracks[index];
            const auto& missing = tracks[other];
            if (missing.enabled && missing.parameters && !missing.ended && missing.queue.empty() &&
                av_gettime_relative() - selected.queue.front().queuedAtUs <
                    static_cast<std::int64_t>(options.maxInterleaveDeltaMs) * 1000) {
                changed.wait_for(lock, std::chrono::milliseconds(10));
                continue;
            }
            result = std::move(selected.queue.front());
            selected.queue.pop_front();
            return true;
        }
    }

    void write(Item& item, std::size_t index) {
        auto& packet = item.packet;
        const auto source = tracks[index].timeBase; // 参数发布之后不可修改。
        const auto streamIndex = streamIndices[index];
        if (streamIndex < 0) invalid("encoded packet has no FLV output stream");
        const auto target = context->streams[streamIndex]->time_base; // write_header 后重新读取。
        if (target.num <= 0 || target.den <= 0) invalid("FLV returned an invalid stream time base");
        const auto offset = rescale(sessionOriginUs, microseconds, target, AV_ROUND_DOWN);
        const auto expectedPts = subtract(rescale(packet->pts, source, target), offset);
        const auto expectedDts = subtract(rescale(packet->dts, source, target), offset);
        static_cast<void>(rescale(packet->duration, source, target));
        // 使用 FFmpeg 标准包转换接口，保留 duration 及编码包 side data。
        av_packet_rescale_ts(packet.get(), source, target);
        packet->pts = expectedPts;
        packet->dts = expectedDts;
        packet->time_base = target;
        packet->stream_index = streamIndex;
        packet->pos = -1;
        if (packet->dts < 0 || (outputDts[index] != AV_NOPTS_VALUE && packet->dts <= outputDts[index])) {
            invalid("FLV DTS is negative or not strictly increasing after rescaling");
        }
        // FLV AVC composition offset 是带符号 24 位毫秒；允许正常 B 帧的 PTS/DTS 差。
        if (index == 0) {
            const auto composition = rescale(subtract(packet->pts, packet->dts), target, AVRational{1, 1000});
            if (composition < -8388608 || composition > 8388607) invalid("H.264 composition timestamp exceeds FLV range");
        }
        const auto bytes = static_cast<std::uint64_t>(packet->size);
        const auto dts = packet->dts;
        deadline(options.writeTimeoutMs);
        const int written = av_interleaved_write_frame(context.get(), packet.get());
        error::throwIfFFmpegFailed(written, "network", "write interleaved FLV packet");
        ioDeadlineUs.store(0, std::memory_order_relaxed);
        outputDts[index] = dts;
        consecutiveFailures = 0;
        pendingNetworkFailure = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex);
            ++counters.sentPackets;
            counters.sentBytes += bytes;
        }
    }

    bool recover(const error::FFmpegError& problem) {
        if (stopped.load(std::memory_order_relaxed) || expired()) return false;
        const auto ioDeadline = ioDeadlineUs.load(std::memory_order_relaxed);
        // 本模块 interrupt callback 的 deadline 以 AVERROR_EXIT 报告；服务器正常关闭
        // 可能映射为 EOF/EIO。这些网络状态同样可重连，配置/内存/协议错误不重试。
        const bool timedOut = ioDeadline != 0 && av_gettime_relative() >= ioDeadline;
        const bool retryable = problem.recoverable || problem.code == AVERROR_EOF ||
            problem.code == AVERROR(EIO) || (problem.code == AVERROR_EXIT && timedOut);
        bool finishingNow;
        {
            std::lock_guard<std::mutex> lock(mutex);
            ++counters.networkErrors;
            finishingNow = finishing;
            clearQueues();
        }
        // 连接可能残留半个 FLV tag；绝不在旧上下文上继续写下一会话。
        closeSession(false);
        if (!retryable || finishingNow || consecutiveFailures >= options.reconnectAttempts) throw problem;
        ++consecutiveFailures;
        recovering = true;
        reopening = true;
        awaitingKey = true;
        hasSessionOrigin = false;
        cutoffUs = std::numeric_limits<std::int64_t>::min();
        logger.log(logging::Level::Warn, "network", "RTMP I/O failed; rebuilding session, retry " +
                   std::to_string(consecutiveFailures) + "/" + std::to_string(options.reconnectAttempts), problem.code);
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait_for(lock, std::chrono::milliseconds(options.reconnectDelayMs), [&] {
            return stopped.load(std::memory_order_relaxed) || finishing;
        });
        return !stopped.load(std::memory_order_relaxed) && !finishing;
    }

    void run() noexcept {
        try {
            for (;;) {
                Item item;
                std::size_t index = 0;
                if (!take(item, index)) break;
                try {
                    if (!context) openSession();
                    write(item, index);
                } catch (const error::FFmpegError& e) {
                    if (e.module != "network") throw;
                    pendingNetworkFailure = std::make_exception_ptr(e);
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        ++counters.droppedPackets; // 当前包已被 mux 消费或其发送状态未知，不重发。
                    }
                    if (!recover(e)) break;
                }
            }
            // EOF 可能发生在重连等关键帧期间；尚未恢复的网络故障不能伪装成成功结束。
            if (pendingNetworkFailure && !stopped.load(std::memory_order_relaxed)) {
                std::rethrow_exception(pendingNetworkFailure);
            }
            const int trailer = closeSession(true);
            if (trailer < 0 && !stopped.load(std::memory_order_relaxed) && !expired()) {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    ++counters.networkErrors;
                }
                throw error::FFmpegError("network", trailer, "finish FLV output");
            }
            bool noOutput;
            {
                std::lock_guard<std::mutex> lock(mutex);
                noOutput = submittedPackets != 0 && counters.sentPackets == 0;
            }
            if (noOutput && !stopped.load(std::memory_order_relaxed)) {
                throw error::AppError("mux", error::Io,
                    "stream ended without publishing any encoded packets; no usable keyframe or output deadline reached");
            }
        } catch (...) {
            // 主线程轮询 checkError，能够立即停止两个采集线程，不能只把失败藏在后台。
            {
                std::lock_guard<std::mutex> lock(mutex);
                failure = std::current_exception();
                stopped.store(true, std::memory_order_relaxed);
            }
            closeSession(false);
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (expired()) logger.log(logging::Level::Warn, "mux", "Network drain deadline reached; dropping remaining streaming packets");
            clearQueues();
            endedAtUs = av_gettime_relative();
        }
        changed.notify_all();
    }
};

RtmpPublisher::RtmpPublisher(const config::PublisherConfig& config, logging::Logger& logger)
    : impl_(std::make_unique<Impl>(config, logger)) {}
RtmpPublisher::~RtmpPublisher() = default;

void RtmpPublisher::start(bool video, bool audio) {
    auto& state = *impl_;
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.stopped.load(std::memory_order_relaxed)) return;
    if (state.started || (!video && !audio)) invalid("start RTMP publishing once with at least one actual track");
    state.tracks[0].enabled = video;
    state.tracks[1].enabled = audio;
    state.startedAtUs = av_gettime_relative();
    state.started = true;
    state.worker = std::thread([&state] { state.run(); });
}

void RtmpPublisher::setVideoParameters(const AVCodecParameters& p, AVRational t) { impl_->setParameters(0, p, t); }
void RtmpPublisher::setAudioParameters(const AVCodecParameters& p, AVRational t) { impl_->setParameters(1, p, t); }
void RtmpPublisher::submitVideo(media::PacketPtr packet) { impl_->submit(0, std::move(packet)); }
void RtmpPublisher::submitAudio(media::PacketPtr packet) { impl_->submit(1, std::move(packet)); }
void RtmpPublisher::finishVideo() { impl_->finishTrack(0); }
void RtmpPublisher::finishAudio() { impl_->finishTrack(1); }
void RtmpPublisher::requestStop() noexcept { impl_->requestStop(); }
void RtmpPublisher::checkError() const { impl_->checkError(); }

void RtmpPublisher::finish() {
    auto& state = *impl_;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (!state.finishing) {
            state.finishing = true;
            state.tracks[0].ended = state.tracks[1].ended = true;
            state.drainDeadlineUs.store(av_gettime_relative() + static_cast<std::int64_t>(state.options.writeTimeoutMs) * 1000,
                                        std::memory_order_relaxed);
        }
    }
    state.changed.notify_all();
    if (state.worker.joinable()) state.worker.join();
    state.checkError();
}

PublisherStats RtmpPublisher::stats() const {
    const auto& state = *impl_;
    std::lock_guard<std::mutex> lock(state.mutex);
    auto result = state.counters;
    if (state.startedAtUs != 0) {
        const auto end = state.endedAtUs == 0 ? av_gettime_relative() : state.endedAtUs;
        result.durationSeconds = static_cast<double>(end - state.startedAtUs) / AV_TIME_BASE;
        if (result.durationSeconds > 0) result.averageBitrate = static_cast<double>(result.sentBytes) * 8 / result.durationSeconds;
    }
    return result;
}

} // namespace lsl::rtmp
