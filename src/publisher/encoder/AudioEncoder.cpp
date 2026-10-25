#include "publisher/encoder/AudioEncoder.hpp"

extern "C" {
#include <libavcodec/version.h>
#include <libavutil/channel_layout.h>
#include <libavutil/rational.h>
#include <libavutil/samplefmt.h>
}

#include <cerrno>
#include <cstdint>
#include <limits>
#include <string_view>
#include <utility>

namespace lsl::encoder {
namespace {

constexpr std::string_view module = "audio-encode";

[[noreturn]] void invalid(std::string message) {
    throw error::AppError(std::string(module), error::InvalidConfig, std::move(message));
}

[[noreturn]] void unexpected(std::string message) {
    throw error::AppError(std::string(module), error::Unexpected, std::move(message));
}

void validateOptions(const AudioEncoderOptions& options) {
    if (options.sampleRate <= 0) invalid("AAC encoding sample rate must be positive");
    if (options.channels < 1 || options.channels > 2) invalid("AAC encoding supports mono or stereo");
    if (options.bitrate <= 0) invalid("AAC encoding bitrate must be positive");
    if (av_get_bytes_per_sample(options.preferredSampleFormat) <= 0) {
        invalid("preferred AAC input sample format is invalid");
    }
}

const AVCodec* findEncoder() {
    // 优先 FFmpeg 原生软件 AAC，避免随安装环境隐式切到硬件编码器。
    if (const auto* codec = avcodec_find_encoder_by_name("aac")) return codec;
    if (const auto* codec = avcodec_find_encoder(AV_CODEC_ID_AAC)) {
        if (!(codec->capabilities & AV_CODEC_CAP_HARDWARE)) return codec;
    }
    throw error::AppError(std::string(module), error::NotImplemented,
                          "no AAC software encoder is available in this FFmpeg build");
}

AVSampleFormat chooseSampleFormat(const AVCodec* codec, AVSampleFormat preferred) {
    const AVSampleFormat* formats = nullptr;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 13, 100)
    const void* values = nullptr;
    error::throwIfFFmpegFailed(avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT,
                                                           0, &values, nullptr),
                               module, "query AAC input sample formats");
    formats = static_cast<const AVSampleFormat*>(values);
#else
    formats = codec->sample_fmts;
#endif
    // 空列表表示任意格式都支持，实际初始化仍会检查。
    if (!formats) return preferred;
    if (formats[0] == AV_SAMPLE_FMT_NONE) invalid("AAC encoder has no supported input sample format");
    for (const auto* format = formats; *format != AV_SAMPLE_FMT_NONE; ++format) {
        if (*format == preferred) return preferred;
    }
    return formats[0];
}

void validateSupportedRateAndLayout(const AVCodec* codec, const AVCodecContext& context) {
    const int* rates = nullptr;
    const AVChannelLayout* layouts = nullptr;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 13, 100)
    const void* values = nullptr;
    error::throwIfFFmpegFailed(avcodec_get_supported_config(&context, codec, AV_CODEC_CONFIG_SAMPLE_RATE,
                                                           0, &values, nullptr),
                               module, "query AAC sample rates");
    rates = static_cast<const int*>(values);
    values = nullptr;
    error::throwIfFFmpegFailed(avcodec_get_supported_config(&context, codec, AV_CODEC_CONFIG_CHANNEL_LAYOUT,
                                                           0, &values, nullptr),
                               module, "query AAC channel layouts");
    layouts = static_cast<const AVChannelLayout*>(values);
#else
    rates = codec->supported_samplerates;
    layouts = codec->ch_layouts;
#endif
    if (rates) {
        bool found = false;
        for (const auto* rate = rates; *rate != 0; ++rate) found = found || *rate == context.sample_rate;
        if (!found) invalid("requested sample rate is unsupported by the AAC encoder");
    }
    if (layouts) {
        bool found = false;
        for (const auto* layout = layouts; layout->nb_channels != 0; ++layout) {
            found = found || av_channel_layout_compare(layout, &context.ch_layout) == 0;
        }
        if (!found) invalid("requested channel layout is unsupported by the AAC encoder");
    }
}

void validateFrame(const AVFrame& frame, const AVCodecContext& context,
                   bool smallLastFrame, bool hasPreviousPts, std::int64_t nextPts) {
    if (frame.format != context.sample_fmt || frame.sample_rate != context.sample_rate ||
        av_channel_layout_check(&frame.ch_layout) != 1 ||
        av_channel_layout_compare(&frame.ch_layout, &context.ch_layout) != 0) {
        invalid("AAC input format, sample rate and channel layout must match the encoder");
    }
    if (frame.nb_samples <= 0 || frame.nb_samples > context.frame_size ||
        (!smallLastFrame && frame.nb_samples != context.frame_size)) {
        invalid("AAC input sample count must match frame_size, except for a supported short final frame");
    }
    int minimumLineSize = 0;
    error::throwIfFFmpegFailed(av_samples_get_buffer_size(&minimumLineSize, context.ch_layout.nb_channels,
                                                         frame.nb_samples, context.sample_fmt, 1),
                               module, "validate AAC input sample buffer size");
    if (!frame.extended_data || frame.linesize[0] < minimumLineSize) {
        invalid("AAC input has no sample planes or its audio linesize is too small");
    }
    const int planes = av_sample_fmt_is_planar(context.sample_fmt) ? context.ch_layout.nb_channels : 1;
    for (int plane = 0; plane < planes; ++plane) {
        if (!frame.extended_data[plane]) invalid("AAC input is missing a sample plane");
    }
    if (frame.pts == AV_NOPTS_VALUE) invalid("AAC input PTS is missing");
    // 容许采集停顿造成的正向跳变，但不能倒退或让两帧的样本时间区间重叠。
    if (hasPreviousPts && frame.pts < nextPts) invalid("AAC input sample timestamps must not overlap or regress");
    if (frame.pts > std::numeric_limits<std::int64_t>::max() - frame.nb_samples) {
        invalid("AAC input sample timestamps overflow int64");
    }
    if (frame.duration < 0 || (frame.duration != 0 && frame.duration != frame.nb_samples)) {
        invalid("AAC input duration must equal its sample count");
    }
    if (frame.time_base.num != 0 &&
        (frame.time_base.num < 0 || frame.time_base.den <= 0 || av_cmp_q(frame.time_base, context.time_base) != 0)) {
        invalid("AAC input time base must match 1/sample_rate");
    }
}

} // namespace

struct AudioEncoder::Impl {
    media::CodecContextPtr context;
    AudioEncoderStats counters;
    bool flushSent = false;
    bool drained = false;
    bool shortFrameSent = false;
    bool hasPreviousPts = false;
    std::int64_t nextPts = 0;

    explicit Impl(const AudioEncoderOptions& options) {
        validateOptions(options);
        const auto* codec = findEncoder();
        context.reset(avcodec_alloc_context3(codec));
        if (!context) throw std::bad_alloc{};
        context->sample_rate = options.sampleRate;
        context->sample_fmt = chooseSampleFormat(codec, options.preferredSampleFormat);
        context->time_base = AVRational{1, options.sampleRate};
        context->bit_rate = options.bitrate;
        av_channel_layout_default(&context->ch_layout, options.channels);
#ifdef AV_PROFILE_AAC_LOW
        context->profile = AV_PROFILE_AAC_LOW;
#else
        context->profile = FF_PROFILE_AAC_LOW;
#endif
        if (options.globalHeader) context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        validateSupportedRateAndLayout(codec, *context);
        error::throwIfFFmpegFailed(avcodec_open2(context.get(), codec, nullptr), module, "open AAC encoder");
        if (context->sample_rate != options.sampleRate || context->ch_layout.nb_channels != options.channels ||
            av_cmp_q(context->time_base, AVRational{1, options.sampleRate}) != 0) {
            unexpected("AAC encoder changed the requested sample rate, channel count or time base");
        }
        // AAC-LC 是固定帧长；不能以预处理器的默认 1024 代替编码器报告的值。
        if (context->frame_size <= 0) unexpected("AAC encoder did not provide a positive frame_size");
    }

    bool supportsSmallLastFrame() const noexcept {
        return (context->codec->capabilities & AV_CODEC_CAP_SMALL_LAST_FRAME) != 0;
    }

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
            error::throwIfFFmpegFailed(result, module, "receive encoded AAC packet");
            // AAC 的首包可能因编码延迟带负 PTS。完整保留 PTS / DTS / duration 及
            // AV_PKT_DATA_SKIP_SAMPLES 等 side data，由容器正确表示 priming / 尾部补齐。
            packet->time_base = context->time_base;
            ++counters.outputPackets;
            counters.outputBytes += static_cast<std::uint64_t>(packet->size);
            output.push_back(std::move(packet));
        }
    }

    void send(const AVFrame* frame, std::vector<media::PacketPtr>& output) {
        for (;;) {
            const int result = avcodec_send_frame(context.get(), frame);
            if (result == AVERROR(EAGAIN)) {
                const auto before = output.size();
                receive(output);
                if (drained) unexpected("AAC encoder reached EOF before accepting pending input");
                if (output.size() == before) unexpected("AAC encoder made no progress after send returned EAGAIN");
                continue; // 该帧尚未被消费，取包后重试同一帧。
            }
            if (result == AVERROR_EOF) {
                drained = true;
                if (frame) unexpected("cannot encode audio after the AAC encoder reached EOF");
                return;
            }
            error::throwIfFFmpegFailed(result, module, frame ? "send AAC input frame" : "start AAC encoder flush");
            if (!frame) flushSent = true;
            return;
        }
    }
};

AudioEncoder::AudioEncoder(AudioEncoderOptions options)
    : impl_(std::make_unique<Impl>(options)) {}

AudioEncoder::~AudioEncoder() = default;

std::vector<media::PacketPtr> AudioEncoder::encode(const AVFrame& frame) {
    auto& state = *impl_;
    if (state.flushSent || state.drained || state.shortFrameSent) invalid("cannot encode audio after flush or a short final frame");
    validateFrame(frame, *state.context, state.supportsSmallLastFrame(), state.hasPreviousPts, state.nextPts);
    auto input = media::referenceFrame(frame);
    input->time_base = state.context->time_base;
    input->duration = input->nb_samples;
    std::vector<media::PacketPtr> output;
    state.send(input.get(), output);
    ++state.counters.inputFrames;
    state.counters.inputSamples += static_cast<std::uint64_t>(frame.nb_samples);
    state.hasPreviousPts = true;
    state.nextPts = frame.pts + frame.nb_samples;
    state.shortFrameSent = frame.nb_samples < state.context->frame_size;
    state.receive(output);
    return output;
}

std::vector<media::PacketPtr> AudioEncoder::flush() {
    auto& state = *impl_;
    if (state.drained) return {};
    if (state.counters.inputFrames == 0) {
        state.flushSent = true;
        state.drained = true;
        return {};
    }
    std::vector<media::PacketPtr> output;
    if (!state.flushSent) state.send(nullptr, output);
    if (!state.drained && state.receive(output) == AVERROR(EAGAIN)) {
        unexpected("AAC encoder requested new input after flush was accepted");
    }
    return output;
}

AVRational AudioEncoder::timeBase() const noexcept { return impl_->context->time_base; }

media::CodecParametersPtr AudioEncoder::parameters() const {
    media::CodecParametersPtr parameters{avcodec_parameters_alloc()};
    if (!parameters) throw std::bad_alloc{};
    error::throwIfFFmpegFailed(avcodec_parameters_from_context(parameters.get(), impl_->context.get()),
                               module, "copy AAC encoder parameters");
    return parameters;
}

AVSampleFormat AudioEncoder::sampleFormat() const noexcept { return impl_->context->sample_fmt; }
int AudioEncoder::sampleRate() const noexcept { return impl_->context->sample_rate; }
int AudioEncoder::channels() const noexcept { return impl_->context->ch_layout.nb_channels; }
int AudioEncoder::frameSize() const noexcept { return impl_->context->frame_size; }
bool AudioEncoder::supportsSmallLastFrame() const noexcept { return impl_->supportsSmallLastFrame(); }
std::string AudioEncoder::codecName() const { return impl_->context->codec->name; }
AudioEncoderStats AudioEncoder::stats() const noexcept { return impl_->counters; }

} // namespace lsl::encoder
