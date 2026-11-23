#include "publisher/capture/MediaInput.hpp"
#include "publisher/capture/DeviceManager.hpp"
#include "publisher/capture/SystemAudioInput.hpp"
#include "common/stats/CaptureTimestamp.hpp"

extern "C" {
#include <libavcodec/codec_desc.h>
#ifdef _WIN32
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavutil/hwcontext.h>
#endif
#include <libavutil/pixdesc.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>
}

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <mutex>

namespace lsl::capture {
namespace {
constexpr AVRational microseconds{1, 1000000};

#ifdef _WIN32
using FilterGraphPtr = std::unique_ptr<AVFilterGraph, media::detail::IndirectDeleter<avfilter_graph_free>>;

bool desktopCanRecover(int code) {
    return code == AVERROR_EXTERNAL || code == AVERROR_OUTPUT_CHANGED || code == AVERROR(EIO) ||
        code == AVERROR(EBUSY) || code == AVERROR(EPERM) || code == AVERROR(EACCES);
}
#endif

void setOption(media::DictionaryPtr& options, const char* key, const std::string& value) {
    auto* raw = options.release();
    const int result = av_dict_set(&raw, key, value.c_str(), 0);
    options.reset(raw);
    error::throwIfFFmpegFailed(result, "capture", key);
}
} // namespace

struct MediaInput::Impl {
    struct Decoder {
        int streamIndex;
        Kind kind;
        media::CodecContextPtr codec;
        bool flushSent = false;
        bool drained = false;
        std::int64_t nextUs = 0;
        std::int64_t lastUs = AV_NOPTS_VALUE;
    };

    media::InputFormatPtr format;
    std::unique_ptr<SystemAudioInput> systemAudio;
#ifdef _WIN32
    FilterGraphPtr desktopGraph;
    AVFilterContext* desktopSink = nullptr;
    std::int64_t nextDesktopPullUs = 0;
#endif
    std::string sourceDescription;
    std::vector<Decoder> decoders;
    media::PacketPtr packet;
    std::atomic<bool> stopped{false};
    std::atomic<std::int64_t> deadlineUs{0};
    std::mutex waitMutex;
    std::condition_variable wake;
    int timeoutMs = 5000;
    int desktopFps = 0;
    int active = -1;
    bool packetPending = false;
    bool inputEnded = false;
    bool finished = false;
    bool file = false;
    bool loop = false;
    bool realtime = true;
    bool loopHadFrame = false;
    std::int64_t originUs = AV_NOPTS_VALUE;
    std::int64_t loopOffsetUs = 0;
    std::int64_t loopEndUs = 0;
    std::int64_t wallStartUs = 0;

    static int interrupt(void* opaque) {
        const auto& self = *static_cast<Impl*>(opaque);
        const auto deadline = self.deadlineUs.load();
        return self.stopped.load() || (deadline && av_gettime_relative() >= deadline);
    }

    void armTimeout() { deadlineUs = av_gettime_relative() + static_cast<std::int64_t>(timeoutMs) * 1000; }

    void waitForInput(std::int64_t deadline, std::chrono::microseconds delay) {
        const auto remaining = std::chrono::microseconds(std::max<std::int64_t>(0, deadline - av_gettime_relative()));
        std::unique_lock<std::mutex> lock(waitMutex);
        wake.wait_for(lock, std::min(delay, remaining), [&] { return stopped.load(); });
    }

    void requireClosed() const {
        if (format || systemAudio || desktopFps) {
            throw error::AppError("capture", error::Io, "input already open; close it before reopening");
        }
        if (stopped) throw error::FFmpegError("capture", AVERROR_EXIT, "input was stopped; close it before reopening");
    }

    void open(const std::string& url, const AVInputFormat* backend, media::DictionaryPtr options,
              AVCodecID videoCodec = AV_CODEC_ID_NONE) {
        AVFormatContext* raw = avformat_alloc_context();
        if (!raw) throw std::bad_alloc{};
        raw->interrupt_callback = {interrupt, this};
        // dshow 不响应 AVIOInterruptCB 的阻塞等帧；NONBLOCK 使调用方可检查停止/超时。
        if (!file) raw->flags |= AVFMT_FLAG_NONBLOCK;
        if (videoCodec != AV_CODEC_ID_NONE) raw->video_codec_id = videoCodec;
        auto* rawOptions = options.release();
        armTimeout();
        const int result = avformat_open_input(&raw, url.c_str(), backend, &rawOptions);
        deadlineUs = 0;
        format.reset(raw);
        options.reset(rawOptions);
        error::throwIfFFmpegFailed(result, "capture",
            "open input (check device name, permissions, busy device and requested format)");
        if (options && av_dict_count(options.get())) {
            const auto* unknown = av_dict_get(options.get(), "", nullptr, AV_DICT_IGNORE_SUFFIX);
            throw error::AppError("capture", error::InvalidConfig, std::string("unsupported input option: ") + unknown->key);
        }
        if (file) {
            armTimeout();
            const int info = avformat_find_stream_info(format.get(), nullptr);
            deadlineUs = 0;
            error::throwIfFFmpegFailed(info, "capture", "read media stream information");
        }
        packet.reset(av_packet_alloc());
        if (!packet) throw std::bad_alloc{};
    }

    bool addDecoder(Kind kind, bool required) {
        const auto type = kind == Kind::Video ? AVMEDIA_TYPE_VIDEO : AVMEDIA_TYPE_AUDIO;
        const AVCodec* codec = nullptr;
        const int index = av_find_best_stream(format.get(), type, -1, -1, &codec, 0);
        if (index == AVERROR_STREAM_NOT_FOUND && !required) return false;
        error::throwIfFFmpegFailed(index, "capture", "find input audio/video stream and decoder");
        // 封面图不是本地文件的直播视频轨。
        if (file && (format->streams[index]->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
            if (required) throw error::AppError("capture", error::Io, "input contains only cover art");
            return false;
        }
        media::CodecContextPtr context{avcodec_alloc_context3(codec)};
        if (!context) throw std::bad_alloc{};
        error::throwIfFFmpegFailed(avcodec_parameters_to_context(context.get(), format->streams[index]->codecpar),
            "capture", "copy decoder parameters");
        context->pkt_timebase = format->streams[index]->time_base;
        error::throwIfFFmpegFailed(avcodec_open2(context.get(), codec, nullptr), "capture", "open input decoder");
        decoders.push_back({index, kind, std::move(context)});
        return true;
    }

    int decoderFor(int streamIndex) const {
        for (std::size_t i = 0; i < decoders.size(); ++i) {
            if (decoders[i].streamIndex == streamIndex) return static_cast<int>(i);
        }
        return -1;
    }

#ifdef _WIN32
    std::int64_t desktopFrameIntervalUs() const { return (1000000LL + desktopFps - 1) / desktopFps; }

    void openDesktop() {
        const auto* sourceFilter = avfilter_get_by_name("ddagrab");
        const auto* sinkFilter = avfilter_get_by_name("buffersink");
        if (!sourceFilter || !sinkFilter) {
            throw error::AppError("capture", error::NotImplemented,
                "FFmpeg was built without the ddagrab or buffersink filter");
        }
        FilterGraphPtr graph{avfilter_graph_alloc()};
        if (!graph) throw std::bad_alloc{};
        graph->nb_threads = 1;
        AVFilterContext* source = nullptr;
        AVFilterContext* sink = nullptr;
        // ddagrab 采集一个 DXGI 显示输出的完整画面。编码分辨率仍由预处理控制。
        // 桌面静止时使用滤镜默认的重复帧行为，兼容尚未提供 dup_frames 选项的 FFmpeg 6.0。
        const auto options = "output_idx=0:draw_mouse=1:output_fmt=bgra:framerate=" + std::to_string(desktopFps);
        error::throwIfFFmpegFailed(avfilter_graph_create_filter(&source, sourceFilter, "desktop", options.c_str(),
            nullptr, graph.get()), "capture", "create ddagrab desktop source");
        error::throwIfFFmpegFailed(avfilter_graph_create_filter(&sink, sinkFilter, "desktop_frames", nullptr,
            nullptr, graph.get()), "capture", "create desktop frame sink");
        error::throwIfFFmpegFailed(avfilter_link(source, 0, sink, 0), "capture", "link ddagrab desktop graph");
        error::throwIfFFmpegFailed(avfilter_graph_config(graph.get(), nullptr), "capture",
            "open ddagrab desktop (check interactive Windows session and DXGI display access)");
        desktopGraph = std::move(graph);
        desktopSink = sink;
        nextDesktopPullUs = av_gettime_relative() + desktopFrameIntervalUs();
    }

    void releaseDesktop() noexcept {
        desktopSink = nullptr;
        desktopGraph.reset();
        nextDesktopPullUs = 0;
    }

    std::optional<CapturedFrame> readDesktop() {
        const auto deadline = av_gettime_relative() + static_cast<std::int64_t>(timeoutMs) * 1000;
        bool recovering = false;
        while (!stopped && av_gettime_relative() < deadline) {
            try {
                if (!desktopGraph) openDesktop();
                // 在采集线程可唤醒地等到帧间隔，避免低帧率时在滤镜内部等整帧。
                const auto waitUs = nextDesktopPullUs - av_gettime_relative();
                if (waitUs > 0) waitForInput(deadline, std::chrono::microseconds(waitUs));
                if (stopped || av_gettime_relative() >= deadline) break;
                media::FramePtr texture{av_frame_alloc()};
                if (!texture) throw std::bad_alloc{};
                const auto pullUs = av_gettime_relative();
                nextDesktopPullUs = std::max(nextDesktopPullUs + desktopFrameIntervalUs(), pullUs);
                const int result = av_buffersink_get_frame(desktopSink, texture.get());
                if (stopped) break;
                if (result == AVERROR(EAGAIN)) {
                    waitForInput(deadline, std::chrono::milliseconds(5));
                    continue;
                }
                error::throwIfFFmpegFailed(result, "capture", "read ddagrab frame");
                media::FramePtr frame{av_frame_alloc()};
                if (!frame) throw std::bad_alloc{};
                frame->format = AV_PIX_FMT_BGRA;
                error::throwIfFFmpegFailed(av_hwframe_transfer_data(frame.get(), texture.get(), 0), "capture",
                    "download ddagrab frame to system memory");
                error::throwIfFFmpegFailed(av_frame_copy_props(frame.get(), texture.get()), "capture",
                    "copy ddagrab frame properties");
                const auto capturedUs = av_gettime_relative();
                if (stopped || capturedUs >= deadline) break;
                const auto timeBase = av_buffersink_get_time_base(desktopSink);
                frame->time_base = timeBase;
                // 跨桌面重开延续与其他设备共用的单调采集时钟。
                static_cast<void>(stats::stampCaptureTime(*frame));
                if (recovering) av_log(nullptr, AV_LOG_INFO, "ddagrab desktop capture recovered.\n");
                return CapturedFrame{std::move(frame), Kind::Video, timeBase, capturedUs, capturedUs};
            } catch (const error::FFmpegError& problem) {
                if (stopped) break;
                if (!desktopCanRecover(problem.code)) throw;
                if (!recovering) av_log(nullptr, AV_LOG_WARNING,
                    "Desktop duplication temporarily unavailable; rebuilding ddagrab within the capture timeout.\n");
                recovering = true;
                releaseDesktop();
                waitForInput(deadline, std::chrono::milliseconds(100));
            }
        }
        if (stopped) return std::nullopt;
        throw error::FFmpegError("capture", AVERROR(ETIMEDOUT), "read ddagrab desktop frame");
    }
#endif

    int readPacket() {
        armTimeout();
        const auto readDeadline = deadlineUs.load();
        for (;;) {
            if (stopped) { deadlineUs = 0; return AVERROR_EXIT; }
            if (av_gettime_relative() >= readDeadline) { deadlineUs = 0; return AVERROR(ETIMEDOUT); }
            int result = av_read_frame(format.get(), packet.get());
            if (result != AVERROR(EAGAIN)) {
                if (result == AVERROR_EXIT && !stopped && interrupt(this)) result = AVERROR(ETIMEDOUT);
                deadlineUs = 0;
                return result;
            }
            av_packet_unref(packet.get());
            waitForInput(readDeadline, std::chrono::milliseconds(5));
        }
    }

    CapturedFrame stamp(Decoder& decoder, media::FramePtr frame) {
        const auto* stream = format->streams[decoder.streamIndex];
        const auto timeBase = stream->time_base;
        const auto capturedUs = av_gettime_relative();
        frame->time_base = timeBase;
        // 设备采集解码输出的应用时刻，不是摄像头曝光时间；文件保持原媒体时间语义。
        if (!file && decoder.kind == Kind::Video) static_cast<void>(stats::stampCaptureTime(*frame));
        std::int64_t timestampUs = capturedUs;
        if (file) {
            auto pts = frame->best_effort_timestamp;
            if (pts == AV_NOPTS_VALUE) pts = frame->pts;
            timestampUs = decoder.nextUs;
            if (pts != AV_NOPTS_VALUE) {
                const auto sourceUs = av_rescale_q(pts, timeBase, microseconds);
                if (originUs == AV_NOPTS_VALUE) originUs = sourceUs;
                timestampUs = loopOffsetUs + std::max<std::int64_t>(0, sourceUs - originUs);
            }
            // 异常回退的时间戳不倒退；缺失 PTS 时按样本数/帧率连续生成。
            if (decoder.lastUs != AV_NOPTS_VALUE) timestampUs = std::max(timestampUs, decoder.lastUs);
            std::int64_t durationUs = 0;
            if (decoder.kind == Kind::Audio && frame->sample_rate > 0) {
                durationUs = av_rescale_q(frame->nb_samples, AVRational{1, frame->sample_rate}, microseconds);
            } else if (frame->duration > 0) {
                durationUs = av_rescale_q(frame->duration, timeBase, microseconds);
            } else {
                const auto fps = av_guess_frame_rate(format.get(), format->streams[decoder.streamIndex], frame.get());
                if (fps.num > 0 && fps.den > 0) durationUs = av_rescale_q(1, av_inv_q(fps), microseconds);
            }
            if (durationUs <= 0) durationUs = decoder.kind == Kind::Video ? 40000 : 1;
            decoder.lastUs = timestampUs;
            decoder.nextUs = timestampUs + durationUs;
            loopEndUs = std::max(loopEndUs, decoder.nextUs);
            loopHadFrame = true;
            if (realtime) {
                if (!wallStartUs) wallStartUs = capturedUs;
                const auto waitUs = wallStartUs + timestampUs - av_gettime_relative();
                if (waitUs > 0) {
                    std::unique_lock<std::mutex> lock(waitMutex);
                    wake.wait_for(lock, std::chrono::microseconds(waitUs), [&] { return stopped.load(); });
                }
            }
        }
        return {std::move(frame), decoder.kind, timeBase, timestampUs, capturedUs};
    }

    void rewind() {
        if (!loopHadFrame) throw error::AppError("capture", error::Io, "cannot loop a file with no decoded frames");
        armTimeout();
        const auto start = format->start_time == AV_NOPTS_VALUE ? 0 : format->start_time;
        const int result = avformat_seek_file(format.get(), -1, std::numeric_limits<std::int64_t>::min(),
            start, start, AVSEEK_FLAG_BACKWARD);
        deadlineUs = 0;
        error::throwIfFFmpegFailed(result, "capture", "seek to start for file loop");
        loopOffsetUs = loopEndUs;
        for (auto& decoder : decoders) {
            avcodec_flush_buffers(decoder.codec.get());
            decoder.flushSent = decoder.drained = false;
            decoder.nextUs = loopOffsetUs;
            decoder.lastUs = AV_NOPTS_VALUE;
        }
        av_packet_unref(packet.get());
        packetPending = inputEnded = loopHadFrame = false;
        active = -1;
    }

    std::optional<CapturedFrame> read() {
        if (!format) throw error::AppError("capture", error::Io, "input is not open");
        if (finished) return std::nullopt;
        for (;;) {
            if (stopped) return std::nullopt;
            if (active >= 0) {
                auto& decoder = decoders[static_cast<std::size_t>(active)];
                media::FramePtr frame{av_frame_alloc()};
                if (!frame) throw std::bad_alloc{};
                const int result = avcodec_receive_frame(decoder.codec.get(), frame.get());
                if (result >= 0) {
                    auto captured = stamp(decoder, std::move(frame));
                    if (stopped) return std::nullopt;
                    return captured;
                }
                if (result == AVERROR_EOF) decoder.drained = true;
                else if (result != AVERROR(EAGAIN)) throw error::FFmpegError("capture", result, "decode input frame");
                else if (decoder.flushSent) throw error::AppError("capture", error::Io, "decoder requested input after flush");
                active = -1;
            }
            // send 返回 EAGAIN 时保留原包，先 receive，成功 send 后才 unref。
            if (packetPending) {
                active = decoderFor(packet->stream_index);
                const int result = avcodec_send_packet(decoders[static_cast<std::size_t>(active)].codec.get(), packet.get());
                if (result == AVERROR(EAGAIN)) continue;
                error::throwIfFFmpegFailed(result, "capture", "send input packet to decoder");
                av_packet_unref(packet.get());
                packetPending = false;
                continue;
            }
            if (inputEnded) {
                for (std::size_t i = 0; i < decoders.size(); ++i) {
                    auto& decoder = decoders[i];
                    if (decoder.drained) continue;
                    active = static_cast<int>(i);
                    if (!decoder.flushSent) {
                        const int result = avcodec_send_packet(decoder.codec.get(), nullptr);
                        if (result == AVERROR_EOF) { decoder.drained = true; active = -1; continue; }
                        if (result != AVERROR(EAGAIN)) {
                            error::throwIfFFmpegFailed(result, "capture", "flush input decoder");
                            decoder.flushSent = true;
                        }
                    }
                    break;
                }
                if (active >= 0) continue;
                if (file && loop) { rewind(); continue; }
                finished = true;
                return std::nullopt;
            }
            const int result = readPacket();
            if (stopped) return std::nullopt;
            if (result == AVERROR_EOF && file) { inputEnded = true; continue; }
            error::throwIfFFmpegFailed(result, "capture", "read input packet (device disconnected or read timed out)");
            if (decoderFor(packet->stream_index) < 0) { av_packet_unref(packet.get()); continue; }
            packetPending = true;
        }
    }
};

MediaInput::MediaInput() : impl_(std::make_unique<Impl>()) {}
MediaInput::~MediaInput() { close(); }

void MediaInput::openDevice(Kind kind, const config::PublisherConfig& config) {
    impl_->requireClosed();
    config::validate(config);
    impl_->timeoutMs = config.capture.timeoutMs;
    if (kind == Kind::Video && config.capture.videoSource == "desktop") {
#ifdef _WIN32
        try {
            impl_->desktopFps = config.video.fps;
            impl_->openDesktop();
            impl_->sourceDescription = "desktop: ddagrab (DXGI output 0, cursor included)";
        } catch (...) { close(); throw; }
        return;
#else
        throw error::ConfigError("capture.video_source: desktop capture currently requires Windows; use --video-source camera");
#endif
    }
    if (kind == Kind::Audio && config.capture.audioSource == "system") {
        try {
            impl_->systemAudio = std::make_unique<SystemAudioInput>();
            impl_->systemAudio->open();
            impl_->sourceDescription = "system playback: " + impl_->systemAudio->deviceName();
        } catch (...) { close(); throw; }
        return;
    }
    const auto* backend = detail::deviceFormat(kind);
    const auto url = detail::deviceUrl(kind, kind == Kind::Video ? config.videoDevice : config.audioDevice);
    impl_->sourceDescription = url;
    // 将默认设备解析后的唯一 ID 对应回友好名称，避免用户不知道实际录制了哪一路。
    if (kind == Kind::Audio) {
        try {
            for (const auto& device : listDevices(kind)) {
                if (url == "audio=" + device.id || url == device.id) {
                    impl_->sourceDescription = "microphone: " + device.description;
                    break;
                }
            }
        } catch (const error::AppError&) {
            // ALSA 等后端可能不能枚举；友好名称只是日志，不阻断显式设备打开。
        }
    }
    media::DictionaryPtr options;
    AVCodecID videoCodec = AV_CODEC_ID_NONE;
    if (kind == Kind::Video) {
        setOption(options, "video_size", std::to_string(config.video.width) + "x" + std::to_string(config.video.height));
        setOption(options, "framerate", std::to_string(config.video.fps));
        if (!config.capture.videoFormat.empty()) {
#ifdef _WIN32
            if (av_get_pix_fmt(config.capture.videoFormat.c_str()) != AV_PIX_FMT_NONE) {
                setOption(options, "pixel_format", config.capture.videoFormat);
            } else {
                const auto* codec = avcodec_descriptor_get_by_name(config.capture.videoFormat.c_str());
                if (!codec || codec->type != AVMEDIA_TYPE_VIDEO) throw error::ConfigError("capture.video_format: unknown pixel format or codec");
                videoCodec = codec->id;
            }
#else
            setOption(options, "input_format", config.capture.videoFormat);
#endif
        }
    } else {
        setOption(options, "sample_rate", std::to_string(config.audio.sampleRate));
        // 新版 ALSA 使用 ch_layout，DirectShow 和旧版 ALSA 使用 channels。
        const AVClass* optionsClass = backend->priv_class;
        if (optionsClass && av_opt_find(&optionsClass, "ch_layout", nullptr, 0, AV_OPT_SEARCH_FAKE_OBJ)) {
            setOption(options, "ch_layout", config.audio.channels == 1 ? "mono" : "stereo");
        } else {
            setOption(options, "channels", std::to_string(config.audio.channels));
        }
    }
    try {
        impl_->open(url, backend, std::move(options), videoCodec);
        impl_->addDecoder(kind, true);
    } catch (...) { close(); throw; }
}

void MediaInput::openFile(const config::PublisherConfig& config) {
    impl_->requireClosed();
    config::validate(config);
    const auto path = std::filesystem::u8path(config.capture.file);
    if (!std::filesystem::is_regular_file(path)) {
        throw error::AppError("capture", error::Io, "not a local media file: " + config.capture.file);
    }
    impl_->file = true;
    impl_->sourceDescription = "file: " + config.capture.file;
    impl_->loop = config.capture.loop;
    impl_->realtime = config.capture.realtime;
    impl_->timeoutMs = config.capture.timeoutMs;
    try {
        impl_->open(config.capture.file, nullptr, {});
        if (config.capture.video) impl_->addDecoder(Kind::Video, false);
        if (config.capture.audio) impl_->addDecoder(Kind::Audio, false);
        if (impl_->decoders.empty()) throw error::AppError("capture", error::Io, "file has no enabled audio/video stream");
        impl_->originUs = impl_->format->start_time;
        if (impl_->originUs == AV_NOPTS_VALUE) {
            for (const auto& decoder : impl_->decoders) {
                const auto* stream = impl_->format->streams[decoder.streamIndex];
                if (stream->start_time == AV_NOPTS_VALUE) continue;
                const auto start = av_rescale_q(stream->start_time, stream->time_base, microseconds);
                if (impl_->originUs == AV_NOPTS_VALUE || start < impl_->originUs) impl_->originUs = start;
            }
        }
    } catch (...) { close(); throw; }
}

std::optional<CapturedFrame> MediaInput::readFrame() {
    if (impl_->systemAudio) return impl_->systemAudio->readFrame(impl_->stopped);
#ifdef _WIN32
    if (impl_->desktopFps) return impl_->readDesktop();
#endif
    return impl_->read();
}

std::string MediaInput::sourceDescription() const { return impl_->sourceDescription; }

bool MediaInput::hasTrack(Kind kind) const noexcept {
    if (impl_->systemAudio && kind == Kind::Audio) return true;
    if (impl_->desktopFps && kind == Kind::Video) return true;
    return std::any_of(impl_->decoders.begin(), impl_->decoders.end(),
        [kind](const Impl::Decoder& decoder) { return decoder.kind == kind; });
}

void MediaInput::requestStop() noexcept {
    impl_->stopped = true;
    impl_->wake.notify_all();
}

void MediaInput::close() noexcept {
    impl_->systemAudio.reset();
#ifdef _WIN32
    impl_->releaseDesktop();
#endif
    impl_->packet.reset();
    impl_->decoders.clear();
    impl_->format.reset();
    impl_->deadlineUs = 0;
    impl_->desktopFps = 0;
    impl_->active = -1;
    impl_->packetPending = impl_->inputEnded = impl_->finished = false;
    impl_->file = impl_->loop = impl_->loopHadFrame = false;
    impl_->originUs = AV_NOPTS_VALUE;
    impl_->loopOffsetUs = impl_->loopEndUs = impl_->wallStartUs = 0;
    impl_->stopped = false;
    impl_->sourceDescription.clear();
}
} // namespace lsl::capture
