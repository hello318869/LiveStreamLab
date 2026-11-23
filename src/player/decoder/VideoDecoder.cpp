#include "player/decoder/VideoDecoder.hpp"

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
}

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace lsl::decoder {
namespace {

constexpr std::string_view module = "video-decode";
constexpr AVRational microseconds{1, 1000000};
constexpr std::int64_t maxPixels = 64LL * 1024 * 1024;
constexpr int maxDimension = 16384;
constexpr int maxFrameBytes = 256 * 1024 * 1024;
constexpr std::size_t maxBatchFrames = 32;
constexpr std::size_t maxBatchBytes = 128ULL * 1024 * 1024;

[[noreturn]] void unexpected(std::string message) {
    throw error::AppError(std::string(module), error::Unexpected, std::move(message));
}

bool validTimeBase(AVRational value) { return value.num > 0 && value.den > 0; }

class NvdecInitializationError final : public error::AppError {
public:
    NvdecInitializationError(int code, std::string message)
        : AppError(std::string(module), code, std::move(message)) {}
};

void checkNvdecInitialization(int result, std::string_view operation) {
    if (result == AVERROR(ENOMEM)) throw std::bad_alloc{};
    if (result < 0) {
        throw NvdecInitializationError(result, std::string(operation) + ": " + error::ffmpegErrorString(result));
    }
}

struct NvdecFormat {
    AVPixelFormat hardware = AV_PIX_FMT_NONE;
    bool allowSoftware = false;
    bool rejected = false;
};

AVPixelFormat selectNvdecFormat(AVCodecContext* context, const AVPixelFormat* formats) noexcept {
    auto& selection = *static_cast<NvdecFormat*>(context->opaque);
    AVPixelFormat software = AV_PIX_FMT_NONE;
    for (auto format = formats; *format != AV_PIX_FMT_NONE; ++format) {
        if (*format == selection.hardware) {
            selection.rejected = false;
            return *format;
        }
        const auto* descriptor = av_pix_fmt_desc_get(*format);
        if (software == AV_PIX_FMT_NONE && descriptor && !(descriptor->flags & AV_PIX_FMT_FLAG_HWACCEL)) {
            software = *format;
        }
    }
    // 若 CUDA 初始化失败，FFmpeg 会带着去掉 CUDA 的格式列表再次协商。
    selection.rejected = true;
    return selection.allowSoftware ? software : AV_PIX_FMT_NONE;
}

AVPixelFormat nvdecPixelFormat(const AVCodec* codec) {
    for (int index = 0; const auto* hardware = avcodec_get_hw_config(codec, index); ++index) {
        if (hardware->device_type == AV_HWDEVICE_TYPE_CUDA && hardware->pix_fmt == AV_PIX_FMT_CUDA &&
            (hardware->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) {
            return hardware->pix_fmt;
        }
    }
    throw NvdecInitializationError(error::NotImplemented,
        "NVDEC is unavailable for decoder " + std::string(codec->name) + " in this FFmpeg build");
}

media::CodecContextPtr makeContext(const AVCodec* codec, const demuxer::TrackInfo& track) {
    media::CodecContextPtr context{avcodec_alloc_context3(codec)};
    if (!context) throw std::bad_alloc{};
    error::throwIfFFmpegFailed(avcodec_parameters_to_context(context.get(), track.parameters.get()), module,
        "import video stream codec parameters");
    context->pkt_timebase = track.timeBase;
    context->max_pixels = maxPixels;
    // 只在同一帧内并行，避免帧级线程额外缓存后续帧；B 帧仍按码流正常重排。
    context->thread_count = 2;
    context->thread_type = FF_THREAD_SLICE;
    return context;
}

media::FramePtr downloadNvdecFrame(const AVFrame& frame) {
    media::FramePtr output{av_frame_alloc()};
    if (!output) throw std::bad_alloc{};
    error::throwIfFFmpegFailed(av_hwframe_transfer_data(output.get(), &frame, 0), module,
        "download NVDEC frame to system memory");
    // 下载只传输像素，显示时间戳、裁剪和采集时间 SEI 等属性单独复制。
    error::throwIfFFmpegFailed(av_frame_copy_props(output.get(), &frame), module,
        "copy downloaded NVDEC frame properties");
    return output;
}

class DecodeTimer {
public:
    explicit DecodeTimer(VideoDecodeStats& counters) : counters_(counters) {}
    ~DecodeTimer() {
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start_).count();
        counters_.lastDecodeTimeUs = static_cast<std::uint64_t>(std::max<std::int64_t>(0, elapsed));
        counters_.decodeTimeUs += counters_.lastDecodeTimeUs;
        counters_.maxDecodeTimeUs = std::max(counters_.maxDecodeTimeUs, counters_.lastDecodeTimeUs);
    }
private:
    VideoDecodeStats& counters_;
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
};

int frameBytes(const AVFrame& frame) {
    const auto format = static_cast<AVPixelFormat>(frame.format);
    if (frame.width <= 0 || frame.height <= 0 || frame.width > maxDimension || frame.height > maxDimension) {
        unexpected("decoded video dimensions must be within 1..16384");
    }
    error::throwIfFFmpegFailed(av_image_check_size2(static_cast<unsigned>(frame.width),
        static_cast<unsigned>(frame.height), maxPixels, format, 0, nullptr), module,
        "validate decoded video image size");
    const auto* descriptor = av_pix_fmt_desc_get(format);
    if (!descriptor || (descriptor->flags & AV_PIX_FMT_FLAG_HWACCEL)) {
        throw error::AppError(std::string(module), error::NotImplemented,
            "video playback requires a supported software pixel format");
    }
    const int bytes = av_image_get_buffer_size(format, frame.width, frame.height, 1);
    error::throwIfFFmpegFailed(bytes, module, "measure decoded video image");
    if (bytes > maxFrameBytes) unexpected("decoded video frame exceeds the 256 MiB safety limit");
    int minimumLines[4]{};
    error::throwIfFFmpegFailed(av_image_fill_linesizes(minimumLines, format, frame.width), module,
        "inspect decoded video image planes");
    const int planes = av_pix_fmt_count_planes(format);
    if (planes <= 0 || planes > 4) unexpected("decoded video has an unsupported plane layout");
    for (int plane = 0; plane < planes; ++plane) {
        if (!frame.data[plane] || std::abs(static_cast<std::int64_t>(frame.linesize[plane])) < minimumLines[plane]) {
            unexpected("decoded video contains a missing plane or an invalid linesize");
        }
    }
    return bytes;
}

std::size_t storageBytes(const AVFrame& frame) {
    // linesize 对齐、裁剪前的原图等仍由 AVBuffer 持有，不能只按可见宽高估计占用。
    // 同一帧多个平面可能引用同一个 AVBuffer，按底层缓冲去重。
    std::vector<const AVBuffer*> seen;
    std::size_t total = 0;
    const auto add = [&](const AVBufferRef* reference) {
        if (!reference || std::find(seen.begin(), seen.end(), reference->buffer) != seen.end()) return;
        seen.push_back(reference->buffer);
        if (total > maxBatchBytes || reference->size > maxBatchBytes - total) total = maxBatchBytes + 1;
        else total += reference->size;
    };
    for (const auto* reference : frame.buf) add(reference);
    for (int index = 0; index < frame.nb_extended_buf; ++index) add(frame.extended_buf[index]);
    return total ? total : static_cast<std::size_t>(frameBytes(frame));
}

int matrix(AVColorSpace colorspace, int height) {
    switch (colorspace) {
    case AVCOL_SPC_BT709: return SWS_CS_ITU709;
    case AVCOL_SPC_FCC: return SWS_CS_FCC;
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M: return SWS_CS_ITU601;
    case AVCOL_SPC_SMPTE240M: return SWS_CS_SMPTE240M;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL: return SWS_CS_BT2020;
    default: return height >= 720 ? SWS_CS_ITU709 : SWS_CS_ITU601;
    }
}

bool fullRange(const AVFrame& frame) {
    if (frame.color_range != AVCOL_RANGE_UNSPECIFIED) return frame.color_range == AVCOL_RANGE_JPEG;
    const auto format = static_cast<AVPixelFormat>(frame.format);
    const auto* descriptor = av_pix_fmt_desc_get(format);
    return (descriptor && (descriptor->flags & AV_PIX_FMT_FLAG_RGB)) ||
        format == AV_PIX_FMT_YUVJ420P || format == AV_PIX_FMT_YUVJ422P ||
        format == AV_PIX_FMT_YUVJ444P || format == AV_PIX_FMT_YUVJ440P;
}

} // namespace

struct VideoDecoder::Impl {
    logging::Logger& logger;
    const std::string backend;
    NvdecFormat nvdecFormat;
    media::BufferRefPtr cudaDevice;
    media::CodecContextPtr context;
    media::SwsContextPtr scaler;
    VideoDecodeStats counters;
    AVRational timeBase{0, 1};
    std::uint64_t serial = 0;
    bool flushSent = false;
    bool drained = false;
    bool awaitingKeyframe = true;
    std::int64_t previousDurationUs = 40000;
    int previousWidth = 0;
    int previousHeight = 0;
    int previousFormat = AV_PIX_FMT_NONE;
    bool reportedFormatFallback = false;
    std::optional<bool> usingNvdec;

    explicit Impl(logging::Logger& output, std::string value) : logger(output), backend(std::move(value)) {
        if (backend != "software" && backend != "nvdec" && backend != "auto") {
            throw error::AppError(std::string(module), error::InvalidConfig,
                "video decoder backend must be software, nvdec or auto");
        }
    }

    media::CodecContextPtr openNvdec(const AVCodec* codec, const demuxer::TrackInfo& track) {
        nvdecFormat = {nvdecPixelFormat(codec), backend == "auto", false};
        if (!cudaDevice) {
            AVBufferRef* device = nullptr;
            const int result = av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_CUDA, nullptr, nullptr, 0);
            media::BufferRefPtr candidate{device};
            checkNvdecInitialization(result, "create NVDEC CUDA device");
            cudaDevice = std::move(candidate);
        }
        auto candidate = makeContext(codec, track);
        candidate->hw_device_ctx = av_buffer_ref(cudaDevice.get());
        if (!candidate->hw_device_ctx) throw std::bad_alloc{};
        candidate->opaque = &nvdecFormat;
        candidate->get_format = selectNvdecFormat;
        // 在取回 CPU 帧后统一按现有逻辑裁剪，保留硬件帧无法应用的上、左边界。
        candidate->apply_cropping = 0;
        checkNvdecInitialization(avcodec_open2(candidate.get(), codec, nullptr), "open NVDEC video decoder");
        checkFormatNegotiation();
        return candidate;
    }

    void checkFormatNegotiation() {
        if (!nvdecFormat.rejected) return;
        if (!nvdecFormat.allowSoftware) {
            throw NvdecInitializationError(error::NotImplemented, "NVDEC cannot decode the negotiated video format");
        }
        if (!reportedFormatFallback) {
            logger.log(logging::Level::Warn, module, "NVDEC format negotiation failed, using software decoding");
            reportedFormatFallback = true;
        }
    }

    void warning(std::string_view message) {
        // 损坏数据可能持续出现；保留最初几次及周期性记录，避免日志反过来阻塞解码。
        if (counters.decodeErrors <= 5 || counters.decodeErrors % 100 == 0) {
            logger.log(logging::Level::Warn, module, message);
        }
    }

    void recover(std::string_view reason, bool frameError) {
        ++counters.decodeErrors;
        if (frameError) ++counters.droppedFrames;
        else ++counters.droppedPackets;
        warning(reason);
        // 损坏参考帧之后继续解 P 帧会放大花屏；同代内等下一个关键帧恢复。
        avcodec_flush_buffers(context.get());
        awaitingKeyframe = true;
        flushSent = false;
        drained = false;
    }

    media::FramePtr adapt(media::FramePtr frame) {
        frameBytes(*frame);
        if (frame->crop_left || frame->crop_right || frame->crop_top || frame->crop_bottom) {
            error::throwIfFFmpegFailed(av_frame_apply_cropping(frame.get(), AV_FRAME_CROP_UNALIGNED), module,
                "apply decoded frame cropping");
            frameBytes(*frame);
        }
        if (previousWidth != frame->width || previousHeight != frame->height || previousFormat != frame->format) {
            const auto* name = av_get_pix_fmt_name(static_cast<AVPixelFormat>(frame->format));
            logger.log(logging::Level::Info, module, "video format=" + std::string(name ? name : "unknown") +
                " size=" + std::to_string(frame->width) + "x" + std::to_string(frame->height) +
                " serial=" + std::to_string(serial));
            previousWidth = frame->width;
            previousHeight = frame->height;
            previousFormat = frame->format;
        }
        if (frame->format == AV_PIX_FMT_YUV420P || frame->format == AV_PIX_FMT_NV12) return frame;
        const auto inputFormat = static_cast<AVPixelFormat>(frame->format);
        if (!sws_isSupportedInput(inputFormat)) {
            throw error::AppError(std::string(module), error::NotImplemented,
                "decoded pixel format cannot be converted by libswscale");
        }
        media::FramePtr output{av_frame_alloc()};
        if (!output) throw std::bad_alloc{};
        output->format = AV_PIX_FMT_YUV420P;
        output->width = frame->width;
        output->height = frame->height;
        error::throwIfFFmpegFailed(av_frame_get_buffer(output.get(), 32), module,
            "allocate display-compatible YUV420P frame");
        error::throwIfFFmpegFailed(av_frame_copy_props(output.get(), frame.get()), module,
            "copy decoded video properties");
        scaler.reset(sws_getCachedContext(scaler.release(), frame->width, frame->height, inputFormat,
            output->width, output->height, AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr));
        if (!scaler) throw std::bad_alloc{};
        const bool full = fullRange(*frame);
        const auto* coefficients = sws_getCoefficients(matrix(frame->colorspace, frame->height));
        error::throwIfFFmpegFailed(sws_setColorspaceDetails(scaler.get(), coefficients, full ? 1 : 0,
            coefficients, full ? 1 : 0, 0, 1 << 16, 1 << 16), module, "configure video conversion colors");
        const int rows = sws_scale(scaler.get(), frame->data, frame->linesize, 0, frame->height,
            output->data, output->linesize);
        if (rows != output->height) unexpected("video pixel conversion produced an incomplete frame");
        output->color_range = full ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
        // RGB 的矩阵标识不能沿用到 YUV；未声明矩阵时与上方 swscale 选型一致。
        if (output->colorspace == AVCOL_SPC_RGB || output->colorspace == AVCOL_SPC_UNSPECIFIED) {
            output->colorspace = frame->height >= 720 ? AVCOL_SPC_BT709 : AVCOL_SPC_SMPTE170M;
        }
        output->crop_left = output->crop_right = output->crop_top = output->crop_bottom = 0;
        return output;
    }

    std::int64_t duration(const AVFrame& frame) {
        auto value = frame.duration > 0 ? av_rescale_q(frame.duration, timeBase, microseconds) : 0;
        if (value <= 0 && validTimeBase(context->framerate)) {
            value = av_rescale_q(1, av_inv_q(context->framerate), microseconds);
        }
        if (value <= 0) value = previousDurationUs;
        previousDurationUs = value;
        return value;
    }

    int receive(std::vector<DecodedVideoFrame>& output, std::size_t& outputBytes) {
        for (;;) {
            media::FramePtr frame{av_frame_alloc()};
            if (!frame) throw std::bad_alloc{};
            const int result = avcodec_receive_frame(context.get(), frame.get());
            checkFormatNegotiation();
            if (result == AVERROR(EAGAIN)) return result;
            if (result == AVERROR_EOF) {
                drained = true;
                return result;
            }
            if (result == AVERROR_INVALIDDATA) {
                recover("invalid video bitstream; waiting for the next keyframe", true);
                return result;
            }
            error::throwIfFFmpegFailed(result, module, "receive decoded video frame");
            if ((frame->flags & AV_FRAME_FLAG_CORRUPT) || frame->decode_error_flags) {
                recover("discarded damaged video frame; waiting for the next keyframe", true);
                return AVERROR_INVALIDDATA;
            }
            const bool hardware = frame->format == AV_PIX_FMT_CUDA;
            if (!hardware && backend == "nvdec") {
                unexpected("NVDEC returned a software frame while hardware decoding is required");
            }
            if (hardware) frame = downloadNvdecFrame(*frame);
            // pkt_timebase 与 TrackInfo::timeBase 相同；best_effort_timestamp 表示显示顺序，
            // 不拿 packet DTS 来覆盖帧 PTS，避免有 B 帧的直播流乱序显示。
            const auto pts = frame->best_effort_timestamp != AV_NOPTS_VALUE ?
                frame->best_effort_timestamp : frame->pts;
            frame->pts = pts;
            frame->time_base = timeBase;
            const auto durationUs = duration(*frame);
            const auto timestampUs = pts == AV_NOPTS_VALUE ? AV_NOPTS_VALUE :
                av_rescale_q(pts, timeBase, microseconds);
            frame = adapt(std::move(frame));
            if (!usingNvdec || *usingNvdec != hardware) {
                logger.log(logging::Level::Info, module, "decoding backend=" +
                    std::string(hardware ? "nvdec" : "software") + " serial=" + std::to_string(serial));
                usingNvdec = hardware;
            }
            ++counters.decodedFrames;
            const auto bytes = storageBytes(*frame);
            if (bytes > maxBatchBytes) {
                ++counters.droppedFrames;
                if (counters.droppedFrames <= 5 || counters.droppedFrames % 100 == 0) {
                    logger.log(logging::Level::Warn, module, "discarded video frame exceeding the 128 MiB batch limit");
                }
                continue;
            }
            // 单包可以产生多帧。返回批次也必须有界；只保留较新的帧供直播追赶。
            while (!output.empty() && (output.size() >= maxBatchFrames || outputBytes + bytes > maxBatchBytes)) {
                outputBytes -= storageBytes(*output.front().frame);
                output.erase(output.begin());
                ++counters.droppedFrames;
            }
            outputBytes += bytes;
            output.push_back(DecodedVideoFrame{std::move(frame), serial, timestampUs, durationUs});
        }
    }

    bool send(const AVPacket* packet, std::vector<DecodedVideoFrame>& output, std::size_t& outputBytes) {
        for (;;) {
            const int result = avcodec_send_packet(context.get(), packet);
            checkFormatNegotiation();
            if (result == AVERROR(EAGAIN)) {
                const auto before = counters.decodedFrames;
                const int received = receive(output, outputBytes);
                if (received == AVERROR_INVALIDDATA) {
                    if (packet) ++counters.droppedPackets; // 待重试包尚未被 codec 接收。
                    return false;
                }
                if (drained) unexpected("video decoder reached EOF before accepting pending input");
                if (counters.decodedFrames == before) {
                    unexpected("video decoder made no progress after send returned EAGAIN");
                }
                continue; // 未被接收的同一个包必须重试，不能提前释放或换成下一个包。
            }
            if (result == AVERROR_EOF) {
                drained = true;
                if (packet) unexpected("cannot decode video after EOF without a Reset");
                return true;
            }
            if (result == AVERROR_INVALIDDATA) {
                recover("discarded invalid video packet; waiting for the next keyframe", false);
                return false;
            }
            error::throwIfFFmpegFailed(result, module, packet ? "send video packet" : "start video decoder drain");
            if (!packet) flushSent = true;
            return true;
        }
    }
};

VideoDecoder::VideoDecoder(logging::Logger& logger, std::string backend)
    : impl_(std::make_unique<Impl>(logger, std::move(backend))) {}
VideoDecoder::~VideoDecoder() = default;

void VideoDecoder::reset(const demuxer::TrackInfo& track, std::uint64_t serial) {
    auto& state = *impl_;
    // Reset 有意丢弃旧会话延迟帧，不发送空包把旧画面带入新代。
    state.context.reset();
    state.scaler.reset();
    state.nvdecFormat = {};
    state.reportedFormatFallback = false;
    state.usingNvdec.reset();
    state.serial = serial;
    state.timeBase = track.timeBase;
    state.flushSent = state.drained = false;
    state.awaitingKeyframe = true;
    state.previousDurationUs = 40000;
    state.previousWidth = state.previousHeight = 0;
    state.previousFormat = AV_PIX_FMT_NONE;
    ++state.counters.resets;
    if (track.index < 0 || !track.parameters) return;
    if (track.parameters->codec_type != AVMEDIA_TYPE_VIDEO || !validTimeBase(track.timeBase)) {
        throw error::AppError(std::string(module), error::InvalidConfig, "video stream has invalid codec parameters or time base");
    }
    const auto* codec = avcodec_find_decoder(track.parameters->codec_id);
    if (!codec) {
        throw error::AppError(std::string(module), error::NotImplemented, "no decoder is available for the video stream codec");
    }
    if (state.backend != "software") {
        try {
            state.context = state.openNvdec(codec, track);
        } catch (const NvdecInitializationError& problem) {
            if (state.backend == "nvdec") throw;
            state.nvdecFormat = {};
            state.logger.log(logging::Level::Warn, module,
                "NVDEC initialization failed, using software decoding: " + std::string(problem.what()));
        }
    }
    if (!state.context) {
        auto context = makeContext(codec, track);
        error::throwIfFFmpegFailed(avcodec_open2(context.get(), codec, nullptr), module, "open video decoder");
        state.context = std::move(context);
    }
    state.logger.log(logging::Level::Info, module, "opened decoder=" + std::string(codec->name) +
        " serial=" + std::to_string(serial) + " requested_backend=" + state.backend);
}

std::vector<DecodedVideoFrame> VideoDecoder::decode(const AVPacket& packet, std::uint64_t serial) {
    auto& state = *impl_;
    if (!state.context || serial != state.serial) {
        ++state.counters.droppedPackets;
        return {};
    }
    if (state.flushSent || state.drained) unexpected("cannot decode video after drain without a Reset");
    ++state.counters.inputPackets;
    DecodeTimer timer(state.counters);
    // 空包在 send_packet API 中具有 drain 语义；正常数据路径不能因此提前结束解码。
    if (packet.size <= 0 || !packet.data) {
        ++state.counters.droppedPackets;
        return {};
    }
    if (packet.flags & AV_PKT_FLAG_CORRUPT) {
        state.recover("discarded corrupt video packet; waiting for the next keyframe", false);
        return {};
    }
    if (state.awaitingKeyframe) {
        if (!(packet.flags & AV_PKT_FLAG_KEY)) {
            ++state.counters.droppedPackets;
            return {};
        }
        state.awaitingKeyframe = false;
    }
    std::vector<DecodedVideoFrame> output;
    std::size_t outputBytes = 0;
    if (state.send(&packet, output, outputBytes)) state.receive(output, outputBytes);
    return output;
}

std::vector<DecodedVideoFrame> VideoDecoder::flush() {
    auto& state = *impl_;
    if (!state.context || state.drained) return {};
    DecodeTimer timer(state.counters);
    std::vector<DecodedVideoFrame> output;
    std::size_t outputBytes = 0;
    if (!state.flushSent && !state.send(nullptr, output, outputBytes)) {
        state.drained = true;
        return output;
    }
    if (!state.drained) {
        const int result = state.receive(output, outputBytes);
        if (result == AVERROR(EAGAIN)) unexpected("video decoder requested input after drain was accepted");
        // EOF 时若尾部损坏，只交付此前的完好帧；后续仍须 Reset 才能继续输入。
        state.drained = true;
    }
    return output;
}

VideoDecodeStats VideoDecoder::stats() const noexcept { return impl_->counters; }

} // namespace lsl::decoder
