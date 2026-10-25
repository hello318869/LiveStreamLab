#include "player/decoder/AudioDecoder.hpp"

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/mathematics.h>
#include <libavutil/samplefmt.h>
}

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace lsl::decoder {
namespace {

constexpr std::string_view module = "audio-decode";
constexpr AVRational microseconds{1, 1000000};
constexpr int maximumBlockSamples = 1024;
constexpr int maximumInputChannels = 64;
constexpr int maximumInputRate = 768000;
constexpr int maximumInputSamples = 1024 * 1024;
constexpr std::size_t maximumInputBytes = 64ULL * 1024 * 1024;
constexpr std::int64_t discontinuityUs = 100000;

[[noreturn]] void unexpected(std::string message) {
    throw error::AppError(std::string(module), error::Unexpected, std::move(message));
}

bool validTimeBase(AVRational value) { return value.num > 0 && value.den > 0; }

void checked(int result, std::string_view operation) {
    error::throwIfFFmpegFailed(result, module, operation);
}

std::int64_t addTime(std::int64_t origin, std::int64_t duration) {
    if (origin == AV_NOPTS_VALUE) return AV_NOPTS_VALUE;
    if (duration < 0 || origin > std::numeric_limits<std::int64_t>::max() - duration) {
        unexpected("audio sample timeline overflow");
    }
    return origin + duration;
}

std::int64_t subtractTime(std::int64_t timestamp, std::int64_t duration) {
    if (duration < 0 || timestamp < std::numeric_limits<std::int64_t>::min() + duration + 1) {
        unexpected("audio sample timestamp cannot be represented");
    }
    return timestamp - duration;
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

class DecodeTimer {
public:
    explicit DecodeTimer(AudioDecodeStats& value) : counters_(value) {}
    ~DecodeTimer() {
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start_).count();
        counters_.decodeTimeUs += static_cast<std::uint64_t>(std::max<std::int64_t>(0, elapsed));
    }
private:
    AudioDecodeStats& counters_;
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
};

} // namespace

struct AudioDecoder::Impl {
    const int outputRate;
    const int outputChannels;
    logging::Logger& logger;
    media::CodecContextPtr context;
    media::SwrContextPtr resampler;
    ChannelLayout outputLayout;
    ChannelLayout inputLayout;
    AudioDecodeStats counters;
    AVRational packetTimeBase{0, 1};
    std::uint64_t serial = 0;
    int inputRate = 0;
    AVSampleFormat inputFormat = AV_SAMPLE_FMT_NONE;
    bool flushSent = false;
    bool drained = false;
    // 每个重采样片段分别累计整数样本，统一换算，避免逐块四舍五入积累漂移。
    std::int64_t inputOriginUs = AV_NOPTS_VALUE;
    std::int64_t outputOriginUs = AV_NOPTS_VALUE;
    std::int64_t inputSamples = 0;
    std::int64_t outputSamples = 0;

    Impl(int sampleRate, int channels, logging::Logger& output)
        : outputRate(sampleRate), outputChannels(channels), logger(output) {
        if (sampleRate < 1 || sampleRate > 384000 || (channels != 1 && channels != 2)) {
            throw error::AppError(std::string(module), error::InvalidConfig,
                "audio output requires 1..384000 Hz and one or two channels");
        }
        av_channel_layout_default(&outputLayout.value, channels);
    }

    std::int64_t durationUs(std::int64_t samples, int sampleRate) const {
        const auto value = av_rescale_q(samples, AVRational{1, sampleRate}, microseconds);
        if (value < 0) unexpected("audio sample duration overflow");
        return value;
    }

    std::int64_t nextOutputUs() const {
        return addTime(outputOriginUs, durationUs(outputSamples, outputRate));
    }

    void resetResampler(bool countDiscarded) {
        if (countDiscarded && resampler) {
            const auto pending = swr_get_delay(resampler.get(), outputRate);
            if (pending > 0) counters.discardedSamples += static_cast<std::uint64_t>(pending);
        }
        resampler.reset();
        av_channel_layout_uninit(&inputLayout.value);
        inputRate = 0;
        inputFormat = AV_SAMPLE_FMT_NONE;
        inputOriginUs = outputOriginUs = AV_NOPTS_VALUE;
        inputSamples = outputSamples = 0;
    }

    void recover(std::string_view message, bool frameError) {
        ++counters.decodeErrors;
        if (frameError) ++counters.droppedFrames;
        else ++counters.droppedPackets;
        if (counters.decodeErrors <= 5 || counters.decodeErrors % 100 == 0) {
            logger.log(logging::Level::Warn, module, message);
        }
        avcodec_flush_buffers(context.get());
        resetResampler(true); // 不把损坏音频的滤波器尾部混入下一段 PCM。
        flushSent = drained = false;
    }

    void append(const float* samples, int count, std::vector<DecodedAudioBlock>& output,
                std::size_t& batchSamples) {
        const auto limit = static_cast<std::size_t>(outputRate) * 2;
        if (count < 0 || batchSamples > limit || static_cast<std::size_t>(count) > limit - batchSamples) {
            unexpected("decoded audio batch exceeds the two-second PCM limit");
        }
        for (int offset = 0; offset < count;) {
            const int size = std::min(maximumBlockSamples, count - offset);
            DecodedAudioBlock block;
            block.serial = serial;
            block.sampleRate = outputRate;
            block.channels = outputChannels;
            block.timestampUs = nextOutputUs();
            const auto first = static_cast<std::size_t>(offset) * outputChannels;
            const auto length = static_cast<std::size_t>(size) * outputChannels;
            block.samples.assign(samples + first, samples + first + length);
            // 非有限浮点 PCM 不应进入音频设备；正常过满幅值由输出端音量处理。
            for (auto& value : block.samples) if (!std::isfinite(value)) value = 0;
            output.push_back(std::move(block));
            if (outputSamples > std::numeric_limits<std::int64_t>::max() - size) {
                unexpected("audio output sample counter overflow");
            }
            outputSamples += size;
            counters.outputSamples += static_cast<std::uint64_t>(size);
            batchSamples += static_cast<std::size_t>(size);
            offset += size;
        }
    }

    void drainResampler(std::vector<DecodedAudioBlock>& output, std::size_t& batchSamples) {
        if (!resampler) return;
        // 每次只分配一个小块；完整尾部仍受 append 的两秒总量限制。
        std::vector<float> samples(static_cast<std::size_t>(maximumBlockSamples) * outputChannels);
        for (;;) {
            std::uint8_t* destination[]{reinterpret_cast<std::uint8_t*>(samples.data())};
            const int count = swr_convert(resampler.get(), destination, maximumBlockSamples, nullptr, 0);
            checked(count, "drain audio resampler");
            if (!count) break;
            append(samples.data(), count, output, batchSamples);
        }
    }

    void configure(const AVFrame& frame, AVChannelLayout& layout,
                   std::int64_t timestampUs, std::int64_t previousEndUs) {
        SwrContext* raw = nullptr;
        // 局部 RAII 对象接管候选 resampler，配置失败也不会泄漏。
        const int result = swr_alloc_set_opts2(&raw, &outputLayout.value, AV_SAMPLE_FMT_FLT, outputRate,
            &layout, static_cast<AVSampleFormat>(frame.format), frame.sample_rate, 0, nullptr);
        media::SwrContextPtr candidate{raw};
        checked(result, "configure audio playback resampler");
        if (!candidate) throw std::bad_alloc{};
        checked(swr_init(candidate.get()), "initialize audio playback resampler");
        inputLayout.copy(layout);
        resampler = std::move(candidate);
        inputRate = frame.sample_rate;
        inputFormat = static_cast<AVSampleFormat>(frame.format);
        inputSamples = outputSamples = 0;
        inputOriginUs = timestampUs != AV_NOPTS_VALUE ? timestampUs : previousEndUs;
        outputOriginUs = previousEndUs != AV_NOPTS_VALUE ? previousEndUs : timestampUs;
        logger.log(logging::Level::Info, module, "input=" + std::to_string(inputRate) + "Hz/" +
            std::to_string(layout.nb_channels) + "ch/" + av_get_sample_fmt_name(inputFormat) +
            " output=" + std::to_string(outputRate) + "Hz/" + std::to_string(outputChannels) + "ch/f32");
    }

    void convert(const AVFrame& frame, std::vector<DecodedAudioBlock>& output, std::size_t& batchSamples) {
        const auto format = static_cast<AVSampleFormat>(frame.format);
        const int bytesPerSample = av_get_bytes_per_sample(format);
        if (frame.sample_rate < 1 || frame.sample_rate > maximumInputRate || frame.nb_samples < 0 ||
            frame.nb_samples > maximumInputSamples ||
            static_cast<std::int64_t>(frame.nb_samples) > static_cast<std::int64_t>(frame.sample_rate) * 2 ||
            bytesPerSample <= 0 || frame.ch_layout.nb_channels < 1 ||
            frame.ch_layout.nb_channels > maximumInputChannels) {
            unexpected("invalid or oversized decoded audio frame parameters");
        }
        const auto bytes = static_cast<std::size_t>(frame.nb_samples) * bytesPerSample * frame.ch_layout.nb_channels;
        if (bytes > maximumInputBytes) unexpected("decoded audio frame exceeds the 64 MiB input limit");
        const bool planar = av_sample_fmt_is_planar(format) != 0;
        const int planes = planar ? frame.ch_layout.nb_channels : 1;
        const int stride = bytesPerSample * (planar ? 1 : frame.ch_layout.nb_channels);
        if (!frame.nb_samples) return;
        if (!frame.extended_data || frame.linesize[0] < static_cast<std::int64_t>(frame.nb_samples) * stride) {
            unexpected("decoded audio frame has invalid sample plane size");
        }
        for (int plane = 0; plane < planes; ++plane) {
            if (!frame.extended_data[plane]) unexpected("decoded audio frame contains a null sample plane");
        }
        ChannelLayout layout;
        if (frame.ch_layout.order == AV_CHANNEL_ORDER_UNSPEC) {
            av_channel_layout_default(&layout.value, frame.ch_layout.nb_channels);
        } else {
            if (!av_channel_layout_check(&frame.ch_layout)) unexpected("decoded audio channel layout is invalid");
            layout.copy(frame.ch_layout);
        }
        const auto pts = frame.pts != AV_NOPTS_VALUE ? frame.pts : frame.best_effort_timestamp;
        const auto timestampUs = pts == AV_NOPTS_VALUE ? AV_NOPTS_VALUE :
            av_rescale_q(pts, packetTimeBase, microseconds);
        const auto expectedUs = resampler ? addTime(inputOriginUs, durationUs(inputSamples, inputRate)) : AV_NOPTS_VALUE;
        const bool jumped = timestampUs != AV_NOPTS_VALUE && expectedUs != AV_NOPTS_VALUE &&
            std::abs(static_cast<long double>(timestampUs) - expectedUs) > discontinuityUs;
        const bool changed = resampler && (inputRate != frame.sample_rate || inputFormat != format ||
            av_channel_layout_compare(&inputLayout.value, &layout.value) != 0);
        std::int64_t previousEndUs = AV_NOPTS_VALUE;
        if (jumped) {
            ++counters.discontinuities;
            resetResampler(true);
            logger.log(logging::Level::Warn, module, "audio timestamp discontinuity; starting a new PCM segment");
        } else if (changed) {
            drainResampler(output, batchSamples);
            previousEndUs = nextOutputUs();
            resetResampler(false);
        }
        if (!resampler) configure(frame, layout.value, timestampUs, previousEndUs);
        else if (inputOriginUs == AV_NOPTS_VALUE && timestampUs != AV_NOPTS_VALUE) {
            // 首个可用 PTS 可能晚于首帧；由已接收样本反推本段起点，补偿 swr 内部延迟。
            inputOriginUs = subtractTime(timestampUs, durationUs(inputSamples, inputRate));
            outputOriginUs = inputOriginUs;
        }
        const auto maximumCapacity = static_cast<std::int64_t>(outputRate) * 2;
        const int chunkLimit = static_cast<int>(std::max<std::int64_t>(1,
            std::min<std::int64_t>(4096, static_cast<std::int64_t>(maximumBlockSamples) * inputRate / outputRate)));
        for (int offset = 0; offset < frame.nb_samples;) {
            const int count = std::min(chunkLimit, frame.nb_samples - offset);
            std::array<const std::uint8_t*, maximumInputChannels> source{};
            for (int plane = 0; plane < planes; ++plane) {
                source[static_cast<std::size_t>(plane)] = frame.extended_data[plane] +
                    static_cast<std::size_t>(offset) * stride;
            }
            const int capacity = swr_get_out_samples(resampler.get(), count);
            checked(capacity, "measure audio conversion output capacity");
            if (capacity > maximumCapacity) unexpected("audio resampler exceeds the two-second staging limit");
            // 非零空间允许滤波器接收短输入；swr 可暂时输出零，随后由后续输入或 EOF 排出。
            std::vector<float> samples(static_cast<std::size_t>(std::max(1, capacity)) * outputChannels);
            std::uint8_t* destination[]{reinterpret_cast<std::uint8_t*>(samples.data())};
            const int produced = swr_convert(resampler.get(), destination, std::max(1, capacity), source.data(), count);
            checked(produced, "convert decoded audio to interleaved f32");
            append(samples.data(), produced, output, batchSamples);
            if (inputSamples > std::numeric_limits<std::int64_t>::max() - count) {
                unexpected("audio input sample counter overflow");
            }
            inputSamples += count;
            offset += count;
        }
    }

    int receive(std::vector<DecodedAudioBlock>& output, std::size_t& batchSamples) {
        for (;;) {
            media::FramePtr frame{av_frame_alloc()};
            if (!frame) throw std::bad_alloc{};
            const int result = avcodec_receive_frame(context.get(), frame.get());
            if (result == AVERROR(EAGAIN)) return result;
            if (result == AVERROR_EOF) { drained = true; return result; }
            if (result == AVERROR_INVALIDDATA) {
                recover("invalid audio bitstream; discarded decoder/resampler pending data", true);
                return result;
            }
            checked(result, "receive decoded audio frame");
            if ((frame->flags & AV_FRAME_FLAG_CORRUPT) || frame->decode_error_flags) {
                recover("discarded damaged audio frame and pending samples", true);
                return AVERROR_INVALIDDATA;
            }
            ++counters.decodedFrames;
            convert(*frame, output, batchSamples);
        }
    }

    bool send(const AVPacket* packet, std::vector<DecodedAudioBlock>& output, std::size_t& batchSamples) {
        for (;;) {
            const int result = avcodec_send_packet(context.get(), packet);
            if (result == AVERROR(EAGAIN)) {
                const auto before = counters.decodedFrames;
                const int received = receive(output, batchSamples);
                if (received == AVERROR_INVALIDDATA) {
                    if (packet) ++counters.droppedPackets;
                    return false;
                }
                if (drained) unexpected("audio decoder reached EOF before accepting pending input");
                if (counters.decodedFrames == before) unexpected("audio decoder made no progress after EAGAIN");
                continue; // receive 释放内部空间后，仍重试原来的包。
            }
            if (result == AVERROR_EOF) {
                drained = true;
                if (packet) unexpected("cannot decode audio after EOF without Reset");
                return true;
            }
            if (result == AVERROR_INVALIDDATA) {
                recover("discarded invalid audio packet and pending samples", false);
                return false;
            }
            checked(result, packet ? "send audio packet" : "start audio decoder drain");
            if (!packet) flushSent = true;
            return true;
        }
    }
};

AudioDecoder::AudioDecoder(int sampleRate, int channels, logging::Logger& logger)
    : impl_(std::make_unique<Impl>(sampleRate, channels, logger)) {}
AudioDecoder::~AudioDecoder() = default;

void AudioDecoder::reset(const demuxer::TrackInfo& track, std::uint64_t serial) {
    auto& state = *impl_;
    state.context.reset();
    state.resetResampler(true);
    state.packetTimeBase = track.timeBase;
    state.serial = serial;
    state.flushSent = state.drained = false;
    ++state.counters.resets;
    if (track.index < 0 || !track.parameters) return;
    if (track.parameters->codec_type != AVMEDIA_TYPE_AUDIO || !validTimeBase(track.timeBase)) {
        throw error::AppError(std::string(module), error::InvalidConfig,
            "audio stream has invalid codec parameters or time base");
    }
    const auto* codec = avcodec_find_decoder(track.parameters->codec_id);
    if (!codec) throw error::AppError(std::string(module), error::NotImplemented, "no decoder is available for the audio codec");
    state.context.reset(avcodec_alloc_context3(codec));
    if (!state.context) throw std::bad_alloc{};
    checked(avcodec_parameters_to_context(state.context.get(), track.parameters.get()), "import audio stream parameters");
    state.context->pkt_timebase = track.timeBase;
    state.context->thread_count = 1;
    state.context->max_samples = maximumInputSamples;
    checked(avcodec_open2(state.context.get(), codec, nullptr), "open audio decoder");
    state.logger.log(logging::Level::Info, module, "opened decoder=" + std::string(codec->name) +
        " serial=" + std::to_string(serial));
}

std::vector<DecodedAudioBlock> AudioDecoder::decode(const AVPacket& packet, std::uint64_t serial) {
    auto& state = *impl_;
    if (!state.context || serial != state.serial) { ++state.counters.droppedPackets; return {}; }
    if (state.flushSent || state.drained) unexpected("cannot decode audio after drain without Reset");
    ++state.counters.inputPackets;
    DecodeTimer timer(state.counters);
    if (packet.size <= 0 || !packet.data) { ++state.counters.droppedPackets; return {}; }
    if (packet.flags & AV_PKT_FLAG_CORRUPT) {
        state.recover("discarded corrupt audio packet and pending samples", false);
        return {};
    }
    std::vector<DecodedAudioBlock> output;
    std::size_t batchSamples = 0;
    if (state.send(&packet, output, batchSamples)) state.receive(output, batchSamples);
    return output;
}

std::vector<DecodedAudioBlock> AudioDecoder::flush() {
    auto& state = *impl_;
    if (!state.context || state.drained) return {};
    DecodeTimer timer(state.counters);
    std::vector<DecodedAudioBlock> output;
    std::size_t batchSamples = 0;
    if (!state.flushSent && !state.send(nullptr, output, batchSamples)) {
        state.drained = true;
        return output;
    }
    if (!state.drained && state.receive(output, batchSamples) == AVERROR(EAGAIN)) {
        unexpected("audio decoder requested input after drain was accepted");
    }
    state.drainResampler(output, batchSamples);
    state.resetResampler(false);
    state.drained = true;
    return output;
}

AudioDecodeStats AudioDecoder::stats() const noexcept { return impl_->counters; }

} // namespace lsl::decoder
