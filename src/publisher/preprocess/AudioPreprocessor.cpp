#include "publisher/preprocess/AudioPreprocessor.hpp"

#include "common/error/Error.hpp"

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/mathematics.h>
#include <libavutil/samplefmt.h>
}

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>

namespace lsl::preprocess {
namespace {

constexpr AVRational microseconds{1, AV_TIME_BASE};
// 停顿超过 250 ms 时只跳跃 PTS，避免按照任意大的外部时间戳分配静音缓冲。
constexpr int maximumSilenceGapMs = 250;
constexpr int maximumConversionSamples = 65536;
constexpr std::size_t maximumOutputFrames = 4096;

[[noreturn]] void fail(std::string message) {
    throw error::AppError("audio_preprocess", error::Unexpected, std::move(message));
}

void checked(int result, const char* operation) {
    error::throwIfFFmpegFailed(result, "audio_preprocess", operation);
}

std::int64_t addSamples(std::int64_t first, std::int64_t second) {
    if (second < 0 || first > std::numeric_limits<std::int64_t>::max() - second) {
        fail("audio sample timeline overflow");
    }
    return first + second;
}

struct ChannelLayout {
    AVChannelLayout value{};
    ~ChannelLayout() { av_channel_layout_uninit(&value); }
    ChannelLayout() = default;
    ChannelLayout(const ChannelLayout&) = delete;
    ChannelLayout& operator=(const ChannelLayout&) = delete;

    void copy(const AVChannelLayout& source) {
        av_channel_layout_uninit(&value);
        checked(av_channel_layout_copy(&value, &source), "copy audio channel layout");
    }
};

template <typename Sample>
void gainValues(std::uint8_t* data, std::size_t count, long double multiplier,
                std::uint64_t& clippedValues) {
    auto* values = reinterpret_cast<Sample*>(data);
    for (std::size_t i = 0; i < count; ++i) {
        if constexpr (std::is_floating_point_v<Sample>) {
            const auto scaled = static_cast<long double>(values[i]) * multiplier;
            // 浮点 PCM 的 0 dBFS 对应幅度 1；超过满幅时饱和，避免编码前产生过载。
            if (std::isnan(scaled)) { values[i] = 0; ++clippedValues; }
            else if (scaled > 1) { values[i] = 1; ++clippedValues; }
            else if (scaled < -1) { values[i] = -1; ++clippedValues; }
            else values[i] = static_cast<Sample>(scaled);
        } else {
            // U8 的静音值是 128；其余有符号 PCM 以 0 为中心。
            constexpr int bias = std::is_unsigned_v<Sample> ? 128 : 0;
            const auto minimum = static_cast<long double>(std::numeric_limits<Sample>::lowest()) - bias;
            const auto maximum = static_cast<long double>(std::numeric_limits<Sample>::max()) - bias;
            const auto upperExclusive = std::ldexp(1.0L, static_cast<int>(sizeof(Sample) * 8) - 1);
            const auto scaled = (static_cast<long double>(values[i]) - bias) * multiplier;
            // MSVC 的 long double 与 double 同精度，S64 最大值可能舍入成 2^63。
            // 显式检查上界，绝不把 2^63 转回 int64_t，防止溢出回绕为负数。
            if (scaled > maximum || scaled >= upperExclusive) {
                values[i] = std::numeric_limits<Sample>::max();
                ++clippedValues;
            } else if (scaled < minimum) {
                values[i] = std::numeric_limits<Sample>::lowest();
                ++clippedValues;
            } else {
                const auto rounded = std::round(scaled) + bias;
                if (rounded - bias >= upperExclusive) values[i] = std::numeric_limits<Sample>::max();
                else values[i] = static_cast<Sample>(rounded);
            }
        }
    }
}

} // namespace

struct AudioPreprocessor::Impl {
    AudioPreprocessOptions options;
    std::int64_t originUs;
    AVRational timeBase;
    ChannelLayout outputLayout;
    ChannelLayout inputLayout;
    media::SwrContextPtr resampler;
    media::AudioFifoPtr fifo;
    AudioPreprocessStats statistics;
    long double gainMultiplier = 1;
    AVSampleFormat inputFormat = AV_SAMPLE_FMT_NONE;
    int inputRate = 0;
    // segmentInputSamples 采用输入采样率累计后一次换算，避免逐帧四舍五入造成漂移。
    std::int64_t segmentInputSamples = 0;
    std::int64_t segmentStartPts = 0;
    std::int64_t nextOutputPts = 0;
    bool started = false;
    bool flushed = false;

    Impl(AudioPreprocessOptions requested, std::int64_t origin)
        : options(requested), originUs(origin), timeBase{1, requested.sampleRate} {
        if (options.sampleRate <= 0 || options.sampleRate > 384000 ||
            (options.channels != 1 && options.channels != 2) ||
            av_get_bytes_per_sample(options.sampleFormat) <= 0 ||
            options.frameSize < 1 || options.frameSize > 8192 ||
            options.discontinuityThresholdMs < 1 || options.discontinuityThresholdMs > 1000 ||
            options.gainDb < -60 || options.gainDb > 30 ||
            originUs == AV_NOPTS_VALUE) {
            fail("invalid audio preprocessing options");
        }
        gainMultiplier = std::pow(10.0L, static_cast<long double>(options.gainDb) / 20);
        av_channel_layout_default(&outputLayout.value, options.channels);
        fifo.reset(av_audio_fifo_alloc(options.sampleFormat, options.channels, options.frameSize));
        if (!fifo) throw std::bad_alloc{};
    }

    void applyGain(AVFrame& frame, int samples) {
        if (!options.gainDb || !samples) return;
        const bool planar = av_sample_fmt_is_planar(options.sampleFormat) != 0;
        const int planeCount = planar ? options.channels : 1;
        const auto valueCount = static_cast<std::size_t>(samples) * (planar ? 1 : options.channels);
        for (int plane = 0; plane < planeCount; ++plane) {
            auto* data = frame.extended_data[plane];
            switch (av_get_packed_sample_fmt(options.sampleFormat)) {
            case AV_SAMPLE_FMT_U8: gainValues<std::uint8_t>(data, valueCount, gainMultiplier, statistics.clippedValues); break;
            case AV_SAMPLE_FMT_S16: gainValues<std::int16_t>(data, valueCount, gainMultiplier, statistics.clippedValues); break;
            case AV_SAMPLE_FMT_S32: gainValues<std::int32_t>(data, valueCount, gainMultiplier, statistics.clippedValues); break;
            case AV_SAMPLE_FMT_S64: gainValues<std::int64_t>(data, valueCount, gainMultiplier, statistics.clippedValues); break;
            case AV_SAMPLE_FMT_FLT: gainValues<float>(data, valueCount, gainMultiplier, statistics.clippedValues); break;
            case AV_SAMPLE_FMT_DBL: gainValues<double>(data, valueCount, gainMultiplier, statistics.clippedValues); break;
            default: fail("unsupported audio gain sample format");
            }
        }
    }

    media::FramePtr allocateFrame(int samples) const {
        media::FramePtr frame{av_frame_alloc()};
        if (!frame) throw std::bad_alloc{};
        frame->format = options.sampleFormat;
        frame->sample_rate = options.sampleRate;
        frame->nb_samples = samples;
        frame->time_base = timeBase;
        checked(av_channel_layout_copy(&frame->ch_layout, &outputLayout.value), "set output audio layout");
        checked(av_frame_get_buffer(frame.get(), 0), "allocate output audio samples");
        return frame;
    }

    std::int64_t bufferedEndPts() const {
        return addSamples(nextOutputPts, av_audio_fifo_size(fifo.get()));
    }

    std::int64_t expectedInputPts() const {
        if (!resampler) return bufferedEndPts();
        return addSamples(segmentStartPts,
                          av_rescale_q(segmentInputSamples, AVRational{1, inputRate}, timeBase));
    }

    std::int64_t sourcePts(std::int64_t timestampUs) const {
        if (timestampUs == AV_NOPTS_VALUE) return started ? expectedInputPts() : 0;
        if ((originUs > 0 && timestampUs < std::numeric_limits<std::int64_t>::min() + originUs) ||
            (originUs < 0 && timestampUs > std::numeric_limits<std::int64_t>::max() + originUs)) {
            fail("audio timestamp exceeds the supported range");
        }
        // 首帧早于会话起点时钳制到零；后续 PTS 由样本数推进，不受小幅采集抖动影响。
        const auto relative = std::max<std::int64_t>(0, timestampUs - originUs);
        const auto result = av_rescale_q(relative, microseconds, timeBase);
        if (result < 0) fail("audio timestamp cannot be represented in the output time base");
        return result;
    }

    void emitSamples(int samples, std::vector<media::FramePtr>& output) {
        if (samples < 1 || samples > options.frameSize) fail("invalid audio FIFO output frame size");
        if (output.size() >= maximumOutputFrames) fail("audio output frame collection limit exceeded");
        auto frame = allocateFrame(samples);
        void* planes[2]{frame->extended_data[0], nullptr};
        if (av_sample_fmt_is_planar(options.sampleFormat) && options.channels == 2) {
            planes[1] = frame->extended_data[1];
        }
        const int count = av_audio_fifo_read(fifo.get(), planes, samples);
        checked(count, "read preprocessed audio FIFO");
        if (count != samples) fail("audio FIFO returned an incomplete encoder frame");
        frame->pts = nextOutputPts;
        frame->duration = samples;
        nextOutputPts = addSamples(nextOutputPts, samples);
        ++statistics.outputFrames;
        statistics.outputSamples += samples;
        output.push_back(std::move(frame));
    }

    void emitReady(std::vector<media::FramePtr>& output) {
        while (av_audio_fifo_size(fifo.get()) >= options.frameSize) {
            emitSamples(options.frameSize, output);
        }
    }

    void appendSamples(const AVFrame& frame, int samples, std::vector<media::FramePtr>& output) {
        if (!samples) return;
        const int buffered = av_audio_fifo_size(fifo.get());
        if (samples < 0 || samples > maximumConversionSamples ||
            buffered > maximumConversionSamples + options.frameSize - samples) {
            fail("audio FIFO capacity limit exceeded");
        }
        checked(av_audio_fifo_realloc(fifo.get(), buffered + samples), "grow audio FIFO");
        void* planes[2]{frame.extended_data[0], nullptr};
        if (av_sample_fmt_is_planar(options.sampleFormat) && options.channels == 2) {
            planes[1] = frame.extended_data[1];
        }
        const int count = av_audio_fifo_write(fifo.get(), planes, samples);
        checked(count, "write preprocessed audio FIFO");
        if (count != samples) fail("audio FIFO accepted fewer samples than requested");
        emitReady(output);
    }

    void appendSilence(int samples, bool padding, std::vector<media::FramePtr>& output) {
        while (samples > 0) {
            const int count = std::min(samples, options.frameSize);
            auto silence = allocateFrame(count);
            checked(av_samples_set_silence(silence->extended_data, 0, count, options.channels,
                                           options.sampleFormat), "fill audio silence");
            appendSamples(*silence, count, output);
            if (padding) statistics.paddedSamples += count;
            else statistics.silenceSamples += count;
            samples -= count;
        }
    }

    void finishFifo(std::vector<media::FramePtr>& output, bool padTail) {
        emitReady(output);
        const int remainder = av_audio_fifo_size(fifo.get());
        if (!remainder) return;
        if (padTail) appendSilence(options.frameSize - remainder, true, output);
        else emitSamples(remainder, output);
    }

    int conversionCapacity(int samples) const {
        // delay 以输入采样率计量；向上取整后同时参考 swr 的上界，避免缓存被截断。
        const auto delay = swr_get_delay(resampler.get(), inputRate);
        if (delay < 0 || delay > std::numeric_limits<std::int64_t>::max() - samples) {
            fail("invalid audio resampling delay");
        }
        const auto needed = av_rescale_rnd(delay + samples, options.sampleRate, inputRate, AV_ROUND_UP);
        const int upperBound = swr_get_out_samples(resampler.get(), samples);
        checked(upperBound, "calculate resampling output capacity");
        const auto capacity = std::max<std::int64_t>({needed, upperBound, 1});
        if (capacity > maximumConversionSamples) fail("audio resampling buffer capacity limit exceeded");
        return static_cast<int>(capacity);
    }

    void drainResampler(std::vector<media::FramePtr>& output) {
        if (!resampler) return;
        for (;;) {
            auto converted = allocateFrame(conversionCapacity(0));
            const int count = swr_convert(resampler.get(), converted->extended_data, converted->nb_samples,
                                          nullptr, 0);
            checked(count, "flush audio resampler");
            if (!count) break;
            applyGain(*converted, count);
            appendSamples(*converted, count, output);
        }
    }

    void configureResampler(const AVFrame& input, AVChannelLayout& layout) {
        // 先让局部 RAII 对象接管，即使 alloc_set_opts2 或 init 失败也不会泄漏。
        SwrContext* raw = nullptr;
        const int result = swr_alloc_set_opts2(&raw, &outputLayout.value, options.sampleFormat,
                                               options.sampleRate, &layout,
                                               static_cast<AVSampleFormat>(input.format),
                                               input.sample_rate, 0, nullptr);
        media::SwrContextPtr candidate{raw};
        checked(result, "configure audio resampler");
        if (!candidate) throw std::bad_alloc{};
        checked(swr_init(candidate.get()), "initialize audio resampler");
        inputLayout.copy(layout);
        resampler = std::move(candidate);
        inputRate = input.sample_rate;
        inputFormat = static_cast<AVSampleFormat>(input.format);
        segmentInputSamples = 0;
        segmentStartPts = bufferedEndPts();
    }

    std::vector<media::FramePtr> process(const AVFrame& input, std::int64_t timestampUs) {
        if (flushed) fail("audio process called after flush");
        // 单次输入限制为一秒，返回的帧集合也因此有界；异常大 PCM 应由上游分片交接。
        if (input.sample_rate <= 0 || input.sample_rate > 768000 || input.nb_samples < 0 ||
            input.nb_samples > input.sample_rate ||
            av_get_bytes_per_sample(static_cast<AVSampleFormat>(input.format)) <= 0 ||
            (input.ch_layout.nb_channels != 1 && input.ch_layout.nb_channels != 2)) {
            fail("invalid input audio frame parameters");
        }
        ChannelLayout layout;
        if (input.ch_layout.order == AV_CHANNEL_ORDER_UNSPEC) {
            av_channel_layout_default(&layout.value, input.ch_layout.nb_channels);
        } else {
            if (!av_channel_layout_check(&input.ch_layout)) fail("invalid input audio channel layout");
            layout.copy(input.ch_layout);
        }
        const bool planar = av_sample_fmt_is_planar(static_cast<AVSampleFormat>(input.format)) != 0;
        const int planeCount = planar ? input.ch_layout.nb_channels : 1;
        if (input.nb_samples > 0) {
            if (!input.extended_data) fail("input audio frame has no sample planes");
            for (int plane = 0; plane < planeCount; ++plane) {
                if (!input.extended_data[plane]) fail("input audio frame contains a null sample plane");
            }
        }
        ++statistics.inputFrames;
        statistics.inputSamples += input.nb_samples;
        std::vector<media::FramePtr> output;
        if (!input.nb_samples) return output;

        const auto desiredPts = sourcePts(timestampUs);
        const auto expectedPts = started ? expectedInputPts() : desiredPts;
        if (!started) {
            nextOutputPts = desiredPts;
            started = true;
        }
        const auto threshold = av_rescale_q(options.discontinuityThresholdMs, AVRational{1, 1000}, timeBase);
        // 两个 PTS 均非负，差值不会溢出 int64_t。
        const auto delta = desiredPts - expectedPts;
        const bool forwardGap = delta > threshold;
        const bool backwardGap = delta < -threshold;
        const bool changed = resampler &&
            (input.sample_rate != inputRate || input.format != inputFormat ||
             av_channel_layout_compare(&layout.value, &inputLayout.value) != 0);

        if (forwardGap || backwardGap) ++statistics.discontinuities;
        if (changed || forwardGap) {
            // 切换格式和时间片前先取出旧滤波器的延迟，旧 PCM 不会遗失或混入新状态。
            drainResampler(output);
            resampler.reset();
        }
        if (forwardGap) {
            const auto maximumGap = av_rescale_q(maximumSilenceGapMs, AVRational{1, 1000}, timeBase);
            const auto remainingGap = desiredPts - bufferedEndPts();
            if (delta <= maximumGap && remainingGap >= 0 && remainingGap <= maximumGap) {
                appendSilence(static_cast<int>(remainingGap), false, output);
            } else {
                // 大停顿结束旧片段。补齐尾帧单独计数，跳过的长间隔不制造虚假 PCM。
                // 中间片段始终补齐；短帧只允许出现在整个编码流的最终尾部。
                finishFifo(output, true);
                nextOutputPts = std::max(nextOutputPts, desiredPts);
            }
        }
        if (!resampler) configureResampler(input, layout.value);

        int skip = 0;
        if (backwardGap) {
            // 回退意味着这段输入与已接收音频重叠，只裁掉重复部分，不回退输出 PTS。
            const auto overlap = av_rescale_rnd(-delta, input.sample_rate, options.sampleRate, AV_ROUND_UP);
            // av_rescale_rnd 在极端外部时间戳溢出时返回 INT64_MIN；此时整帧均被覆盖。
            skip = overlap < 0 ? input.nb_samples :
                static_cast<int>(std::min<std::int64_t>(input.nb_samples, overlap));
            statistics.discardedSamples += skip;
        }
        const int stride = av_get_bytes_per_sample(static_cast<AVSampleFormat>(input.format)) *
                           (planar ? 1 : input.ch_layout.nb_channels);
        // 每次按约 4096 个输出样本切小块，FIFO 每次写入后立即输出完整帧。
        const int chunkLimit = static_cast<int>(std::min<std::int64_t>(4096,
            std::max<std::int64_t>(1, static_cast<std::int64_t>(4096) * input.sample_rate /
                                       options.sampleRate)));
        for (int offset = skip; offset < input.nb_samples;) {
            const int count = std::min(chunkLimit, input.nb_samples - offset);
            const std::uint8_t* planes[2]{};
            for (int plane = 0; plane < planeCount; ++plane) {
                planes[plane] = input.extended_data[plane] + static_cast<std::size_t>(offset) * stride;
            }
            auto converted = allocateFrame(conversionCapacity(count));
            const int produced = swr_convert(resampler.get(), converted->extended_data,
                                             converted->nb_samples, planes, count);
            checked(produced, "convert audio samples");
            applyGain(*converted, produced);
            appendSamples(*converted, produced, output);
            segmentInputSamples = addSamples(segmentInputSamples, count);
            offset += count;
        }
        return output;
    }

    std::vector<media::FramePtr> flush() {
        std::vector<media::FramePtr> output;
        if (flushed) return output;
        drainResampler(output);
        finishFifo(output, options.padFinalFrame);
        resampler.reset();
        flushed = true;
        return output;
    }
};

AudioPreprocessor::AudioPreprocessor(AudioPreprocessOptions options, std::int64_t originUs)
    : impl_(std::make_unique<Impl>(options, originUs)) {}

AudioPreprocessor::~AudioPreprocessor() = default;

std::vector<media::FramePtr> AudioPreprocessor::process(const AVFrame& input, std::int64_t timestampUs) {
    return impl_->process(input, timestampUs);
}

std::vector<media::FramePtr> AudioPreprocessor::flush() { return impl_->flush(); }

AudioPreprocessStats AudioPreprocessor::stats() const noexcept { return impl_->statistics; }

} // namespace lsl::preprocess
