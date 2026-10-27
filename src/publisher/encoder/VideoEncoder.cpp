#include "publisher/encoder/VideoEncoder.hpp"

extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/rational.h>
}

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <string_view>
#include <utility>

namespace lsl::encoder {
namespace {

constexpr std::string_view module = "video-encode";

[[noreturn]] void invalid(std::string message) {
    throw error::AppError(std::string(module), error::InvalidConfig, std::move(message));
}

[[noreturn]] void unexpected(std::string message) {
    throw error::AppError(std::string(module), error::Unexpected, std::move(message));
}

void validateOptions(const VideoEncoderOptions& options) {
    if (options.width < 2 || options.height < 2 || (options.width & 1) || (options.height & 1) ||
        options.width > 16384 || options.height > 16384 ||
        static_cast<std::int64_t>(options.width) * options.height > 64LL * 1024 * 1024) {
        invalid("YUV420P encoding dimensions must be even, at least 2, and within 16384 / 64 megapixels");
    }
    error::throwIfFFmpegFailed(av_image_check_size(options.width, options.height, 0, nullptr),
                               module, "validate encoding image dimensions");
    if (options.fps < 1 || options.fps > 240) invalid("video encoding fps must be in [1, 240]");
    if (options.bitrate <= 0) invalid("video encoding bitrate must be positive");
    if (options.gopSize <= 0) invalid("video encoding GOP size must be positive");
    if (options.preset.empty()) invalid("video encoding preset must not be empty");
}

const AVCodec* findSoftwareEncoder() {
    // 不通过 avcodec_find_encoder(H264) 隐式选到 NVENC / QSV 等硬件编码器。
    if (const auto* codec = avcodec_find_encoder_by_name("libx264")) return codec;
    if (const auto* codec = avcodec_find_encoder_by_name("libopenh264")) return codec;
    throw error::AppError(std::string(module), error::NotImplemented,
                          "no H.264 software encoder is available; FFmpeg needs libx264 or libopenh264");
}

void setOption(media::DictionaryPtr& options, const char* name, const char* value) {
    auto* raw = options.release();
    const int result = av_dict_set(&raw, name, value, 0);
    options.reset(raw);
    error::throwIfFFmpegFailed(result, module, "set H.264 encoder option");
}

void validateFrame(const AVFrame& frame, const VideoEncoderOptions& options,
                   AVRational timeBase, bool hasPreviousPts, std::int64_t previousPts) {
    if (frame.format != AV_PIX_FMT_YUV420P || frame.width != options.width || frame.height != options.height) {
        invalid("video encoder requires YUV420P frames matching its configured dimensions");
    }
    if (frame.crop_left || frame.crop_right || frame.crop_top || frame.crop_bottom) {
        invalid("video encoder input cropping must be applied by the preprocessor");
    }
    for (int plane = 0; plane < 3; ++plane) {
        const int minimum = plane == 0 ? options.width : options.width / 2;
        const auto stride = static_cast<std::int64_t>(frame.linesize[plane]);
        if (!frame.data[plane] || std::abs(stride) < minimum) {
            invalid("video encoder input is missing a YUV420P plane or has an invalid linesize");
        }
    }
    if (frame.pts == AV_NOPTS_VALUE) invalid("video encoder input PTS is missing");
    if (hasPreviousPts && frame.pts <= previousPts) invalid("video encoder input PTS must strictly increase");
    if (frame.duration < 0) invalid("video encoder input duration must not be negative");
    // 未声明 time_base 的手工帧按本模块时间基解释；预处理器会显式填入 1/fps。
    if (frame.time_base.num != 0 &&
        (frame.time_base.num < 0 || frame.time_base.den <= 0 || av_cmp_q(frame.time_base, timeBase) != 0)) {
        invalid("video encoder input time base must match 1/fps");
    }
}

} // namespace

struct VideoEncoder::Impl {
    VideoEncoderOptions options;
    media::CodecContextPtr context;
    VideoEncoderStats counters;
    bool flushSent = false;
    bool drained = false;
    bool hasPreviousPts = false;
    std::int64_t previousPts = 0;

    explicit Impl(VideoEncoderOptions value) : options(std::move(value)) {
        validateOptions(options);
        const auto* codec = findSoftwareEncoder();
        context.reset(avcodec_alloc_context3(codec));
        if (!context) throw std::bad_alloc{};
        context->width = options.width;
        context->height = options.height;
        context->pix_fmt = AV_PIX_FMT_YUV420P;
        context->time_base = AVRational{1, options.fps};
        context->framerate = AVRational{options.fps, 1};
        context->sample_aspect_ratio = AVRational{1, 1};
        context->color_range = options.colorRange;
        context->colorspace = options.colorSpace;
        context->color_primaries = options.colorPrimaries;
        context->color_trc = options.colorTransfer;
        context->chroma_sample_location = options.chromaLocation;
        context->bit_rate = options.bitrate;
        context->gop_size = options.gopSize;
        const bool x264 = std::string_view(codec->name) == "libx264";
        // OpenH264 不支持 B 帧；x264 的非低延迟模式允许两帧重排。
        context->max_b_frames = options.lowLatency || !x264 ? 0 : 2;
        if (options.lowLatency) context->flags |= AV_CODEC_FLAG_LOW_DELAY;
        if (options.globalHeader) context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
#ifdef AV_CODEC_FLAG_FRAME_DURATION
        // 新版 FFmpeg 显式要求该标记，才会将 AVFrame.duration 传给编码包。
        context->flags |= AV_CODEC_FLAG_FRAME_DURATION;
#endif
        media::DictionaryPtr codecOptions;
        if (x264) {
            setOption(codecOptions, "preset", options.preset.c_str());
            if (options.lowLatency) setOption(codecOptions, "tune", "zerolatency");
            if (options.captureTelemetry) setOption(codecOptions, "udu_sei", "1");
        }
        auto* raw = codecOptions.release();
        const int result = avcodec_open2(context.get(), codec, &raw);
        codecOptions.reset(raw);
        error::throwIfFFmpegFailed(result, module, "open H.264 software encoder");
        if (const auto* unused = av_dict_get(codecOptions.get(), "", nullptr, AV_DICT_IGNORE_SUFFIX)) {
            invalid(std::string("unsupported H.264 encoder option: ") + unused->key);
        }
    }

    // 每次 receive 都使用新的 AVPacket。返回的包结构及其引用计数缓冲不被本模块复用。
    // 返回 EAGAIN 表示需要更多输入，EOF 则表示刷新已经结束。
    int receive(std::vector<media::PacketPtr>& output) {
        for (;;) {
            media::PacketPtr packet{av_packet_alloc()};
            if (!packet) throw std::bad_alloc{};
            const int result = avcodec_receive_packet(context.get(), packet.get());
            if (result == AVERROR(EAGAIN)) return result;
            if (result == AVERROR_EOF) {
                drained = true;
                return result;
            }
            error::throwIfFFmpegFailed(result, module, "receive encoded video packet");
            // 保留编码器输出的 PTS / DTS / duration / flags；不能用输入序号重写 DTS，
            // 否则开启 B 帧时会破坏解码顺序。旧编码器未提供 duration 时采用固定帧率的一槽。
            packet->time_base = context->time_base;
            if (packet->duration <= 0) packet->duration = 1;
            ++counters.outputPackets;
            counters.outputBytes += static_cast<std::uint64_t>(packet->size);
            if (packet->flags & AV_PKT_FLAG_KEY) ++counters.keyPackets;
            output.push_back(std::move(packet));
        }
    }

    void send(const AVFrame* frame, std::vector<media::PacketPtr>& output) {
        for (;;) {
            const int result = avcodec_send_frame(context.get(), frame);
            if (result == AVERROR(EAGAIN)) {
                const auto before = output.size();
                receive(output);
                if (drained) unexpected("H.264 encoder reached EOF before accepting pending input");
                // FFmpeg 保证 send / receive 不会同时返回 EAGAIN；失约时终止而不是忙循环。
                if (output.size() == before) unexpected("H.264 encoder made no progress after send returned EAGAIN");
                continue; // 同一帧尚未被接收，排空已有包后重试，不能跳过该帧。
            }
            if (result == AVERROR_EOF) {
                drained = true;
                if (frame) unexpected("cannot encode video after the encoder reached EOF");
                return;
            }
            error::throwIfFFmpegFailed(result, module, frame ? "send video frame" : "start H.264 encoder flush");
            if (!frame) flushSent = true;
            return;
        }
    }
};

VideoEncoder::VideoEncoder(VideoEncoderOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

VideoEncoder::~VideoEncoder() = default;

std::vector<media::PacketPtr> VideoEncoder::encode(const AVFrame& frame) {
    auto& state = *impl_;
    if (state.flushSent || state.drained) invalid("cannot encode video after flush");
    validateFrame(frame, state.options, state.context->time_base, state.hasPreviousPts, state.previousPts);
    // 只修改独立帧结构的元数据，原始像素通过 FFmpeg 引用计数持有。
    auto input = media::referenceFrame(frame);
    input->time_base = state.context->time_base;
    input->pict_type = AV_PICTURE_TYPE_NONE; // GOP / 场景切换由编码器统一决定。
    if (input->duration == 0) input->duration = 1;
    std::vector<media::PacketPtr> output;
    state.send(input.get(), output);
    ++state.counters.inputFrames;
    state.previousPts = frame.pts;
    state.hasPreviousPts = true;
    state.receive(output);
    return output;
}

std::vector<media::PacketPtr> VideoEncoder::flush() {
    auto& state = *impl_;
    if (state.drained) return {};
    std::vector<media::PacketPtr> output;
    if (!state.flushSent) state.send(nullptr, output);
    if (!state.drained && state.receive(output) == AVERROR(EAGAIN)) {
        // 成功发送空帧以后应一直 receive 到 EOF；不能漏尾帧或重复发送输入来解围。
        unexpected("H.264 encoder requested new input after flush was accepted");
    }
    return output;
}

AVRational VideoEncoder::timeBase() const noexcept { return impl_->context->time_base; }

media::CodecParametersPtr VideoEncoder::parameters() const {
    media::CodecParametersPtr parameters{avcodec_parameters_alloc()};
    if (!parameters) throw std::bad_alloc{};
    error::throwIfFFmpegFailed(avcodec_parameters_from_context(parameters.get(), impl_->context.get()),
                               module, "copy H.264 encoder parameters");
    return parameters;
}

VideoEncoderStats VideoEncoder::stats() const noexcept { return impl_->counters; }

std::string VideoEncoder::codecName() const { return impl_->context->codec->name; }

} // namespace lsl::encoder
