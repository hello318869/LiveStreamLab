#include "publisher/pipeline/PublisherPipeline.hpp"
#include "publisher/capture/MediaInput.hpp"
#include "common/queue/BoundedBlockingQueue.hpp"
#include "publisher/encoder/AudioEncoder.hpp"
#include "publisher/muxer/AudioFileWriter.hpp"
#include "publisher/encoder/VideoEncoder.hpp"
#include "publisher/muxer/VideoFileWriter.hpp"
#include "publisher/rtmp/RtmpPublisher.hpp"
#include "publisher/preprocess/AudioPreprocessor.hpp"
#include "publisher/preprocess/VideoPreprocessor.hpp"

extern "C" {
#include <libavutil/pixdesc.h>
#include <libavutil/samplefmt.h>
#include <libavutil/time.h>
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <csignal>
#include <cstring>
#include <exception>
#include <iostream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <thread>

namespace lsl::pipeline {
namespace {
static_assert(std::atomic<bool>::is_always_lock_free, "signal handler requires a lock-free stop flag");
std::atomic<bool> stopRequested{false};
void signalStop(int) { stopRequested.store(true, std::memory_order_relaxed); }

// 信号处理器只写无锁原子变量，实际停止/唤醒由主线程执行。
class SignalScope {
public:
    SignalScope() {
        stopRequested = 0;
        oldInterrupt_ = std::signal(SIGINT, signalStop);
        oldTerminate_ = std::signal(SIGTERM, signalStop);
    }
    ~SignalScope() {
        if (oldInterrupt_ != SIG_ERR) std::signal(SIGINT, oldInterrupt_);
        if (oldTerminate_ != SIG_ERR) std::signal(SIGTERM, oldTerminate_);
    }
private:
    using Handler = void (*)(int);
    Handler oldInterrupt_;
    Handler oldTerminate_;
};

std::string describe(const AVFrame& frame, capture::Kind kind) {
    if (kind == capture::Kind::Video) {
        const auto* format = av_get_pix_fmt_name(static_cast<AVPixelFormat>(frame.format));
        return std::to_string(frame.width) + "x" + std::to_string(frame.height) + " " + (format ? format : "unknown");
    }
    const auto* format = av_get_sample_fmt_name(static_cast<AVSampleFormat>(frame.format));
    return std::to_string(frame.sample_rate) + "Hz " + std::to_string(frame.ch_layout.nb_channels)
        + "ch " + (format ? format : "unknown");
}

// 仅统计电平，不改采样值。输入和预处理输出分别统计，可区分弱音源与处理链问题。
struct AudioLevels {
    long double squareSum = 0;
    double peak = 0;
    std::uint64_t values = 0;

    template<typename T>
    static double readValue(const std::uint8_t* data, double divisor, double bias = 0) {
        T value{};
        std::memcpy(&value, data, sizeof(value));
        return (static_cast<double>(value) - bias) / divisor;
    }

    void add(const AVFrame& frame) {
        const auto format = static_cast<AVSampleFormat>(frame.format);
        const auto packed = av_get_packed_sample_fmt(format);
        const int bytes = av_get_bytes_per_sample(format);
        const bool planar = av_sample_fmt_is_planar(format) != 0;
        if (!frame.extended_data || bytes <= 0) return;
        for (int channel = 0; channel < frame.ch_layout.nb_channels; ++channel) {
            for (int sample = 0; sample < frame.nb_samples; ++sample) {
                const auto index = planar ? sample : sample * frame.ch_layout.nb_channels + channel;
                const auto* data = frame.extended_data[planar ? channel : 0] + static_cast<std::size_t>(index) * bytes;
                double value = 0;
                switch (packed) {
                case AV_SAMPLE_FMT_U8: value = readValue<std::uint8_t>(data, 128.0, 128.0); break;
                case AV_SAMPLE_FMT_S16: value = readValue<std::int16_t>(data, 32768.0); break;
                case AV_SAMPLE_FMT_S32: value = readValue<std::int32_t>(data, 2147483648.0); break;
                case AV_SAMPLE_FMT_S64: value = readValue<std::int64_t>(data, 9223372036854775808.0); break;
                case AV_SAMPLE_FMT_FLT: value = readValue<float>(data, 1.0); break;
                case AV_SAMPLE_FMT_DBL: value = readValue<double>(data, 1.0); break;
                default: return;
                }
                if (!std::isfinite(value)) continue;
                squareSum += static_cast<long double>(value) * value;
                peak = std::max(peak, std::abs(value));
                ++values;
            }
        }
    }

    static std::string db(double value) {
        if (value <= 0) return "-inf";
        std::ostringstream text;
        text << std::fixed << std::setprecision(2) << 20.0 * std::log10(value);
        return text.str();
    }
    double rms() const { return values ? std::sqrt(static_cast<double>(squareSum / values)) : 0; }
};

preprocess::VideoPreprocessOptions videoOptions(const config::PublisherConfig& config) {
    preprocess::VideoPreprocessOptions options;
    options.width = config.video.width;
    options.height = config.video.height;
    options.fps = config.video.fps;
    options.scaleAlgorithm = config.preprocess.video.scaleAlgorithm;
    options.resizeMode = config.preprocess.video.resizeMode;
    options.rotation = config.preprocess.video.rotation;
    options.mirror = config.preprocess.video.mirror;
    options.watermark = config.preprocess.video.watermark;
    options.timestampOverlay = config.preprocess.video.timestampOverlay;
    return options;
}

preprocess::AudioPreprocessOptions audioOptions(const config::PublisherConfig& config,
                                               const encoder::AudioEncoder& encoder) {
    preprocess::AudioPreprocessOptions options;
    // M06 先打开 AAC 编码器，再用它实际接受的格式与 frame_size 组织 PCM。
    options.sampleRate = encoder.sampleRate();
    options.channels = encoder.channels();
    options.sampleFormat = encoder.sampleFormat();
    options.frameSize = encoder.frameSize();
    options.padFinalFrame = !encoder.supportsSmallLastFrame();
    options.gainDb = config.preprocess.audio.gainDb;
    options.discontinuityThresholdMs = config.preprocess.audio.discontinuityThresholdMs;
    return options;
}

encoder::AudioEncoderOptions audioEncoderOptions(const config::PublisherConfig& config) {
    encoder::AudioEncoderOptions options;
    options.sampleRate = config.audio.sampleRate;
    options.channels = config.audio.channels;
    options.bitrate = config.audio.bitrate;
    options.preferredSampleFormat = av_get_sample_fmt(config.preprocess.audio.sampleFormat.c_str());
    options.globalHeader = !config.audioFile.empty() || config.streaming.enabled;
    return options;
}

encoder::VideoEncoderOptions encoderOptions(const config::PublisherConfig& config, const AVFrame& firstFrame) {
    encoder::VideoEncoderOptions options;
    options.width = config.video.width;
    options.height = config.video.height;
    options.fps = config.video.fps;
    options.bitrate = config.video.bitrate;
    options.gopSize = config.videoEncoder.gopSize;
    options.preset = config.videoEncoder.preset;
    options.lowLatency = config.videoEncoder.lowLatency;
    // 编码会话的颜色描述与首个预处理帧一致，并写入 H.264 / MP4 流参数。
    options.colorRange = firstFrame.color_range;
    options.colorSpace = firstFrame.colorspace;
    options.colorPrimaries = firstFrame.color_primaries;
    options.colorTransfer = firstFrame.color_trc;
    options.chromaLocation = firstFrame.chroma_location;
    // MP4 与 FLV 都需要从编码参数取得 SPS/PPS，即使禁用本地文件也必须启用。
    options.globalHeader = !config.videoFile.empty() || config.streaming.enabled;
    return options;
}
} // namespace

int runPublisher(const config::PublisherConfig& config,
                 const config::CommandLineOptions& options, logging::Logger& logger) {
    if (options.listDevices) {
        bool failed = false;
        for (const auto kind : {capture::Kind::Video, capture::Kind::Audio}) {
            try {
                const auto devices = capture::listDevices(kind);
                std::cout << capture::kindName(kind) << " devices: " << devices.size() << '\n';
                for (const auto& device : devices) {
                    std::cout << "  " << device.description << "\n    id: " << device.id << '\n';
                }
            } catch (const error::AppError& e) {
                logger.log(logging::Level::Error, e.module, e.what(), e.code);
                failed = true;
            }
        }
        return failed ? 1 : 0;
    }
    if (!options.deviceOptions.empty()) {
        const auto kind = options.deviceOptions == "video" ? capture::Kind::Video : capture::Kind::Audio;
        capture::printDeviceOptions(kind, kind == capture::Kind::Video ? config.videoDevice : config.audioDevice);
        return 0;
    }

    SignalScope signals;
    using Clock = std::chrono::steady_clock;
    struct RawFrame {
        capture::CapturedFrame captured;
        Clock::time_point queuedAt;
    };
    struct OutputEvent {
        enum class Type { Start, Parameters, Packet, End };
        Type type = Type::Packet;
        capture::Kind kind = capture::Kind::Video;
        bool video = false;
        bool audio = false;
        media::CodecParametersPtr parameters;
        AVRational timeBase{0, 1};
        media::PacketPtr packet;
    };
    queue::BoundedBlockingQueue<RawFrame> videoFrames(config.pipeline.videoQueueCapacity);
    queue::BoundedBlockingQueue<RawFrame> audioFrames(config.pipeline.audioQueueCapacity);
    queue::BoundedBlockingQueue<OutputEvent> output(config.pipeline.packetQueueCapacity);
    const bool realtime = config.capture.source != "file" || config.capture.realtime;
    const auto originUs = config.capture.source == "file" ? std::int64_t{0} : av_gettime_relative();
    std::atomic<std::uint64_t> droppedVideo{0};
    std::atomic<std::size_t> done{0};
    std::atomic<unsigned> encodersLeft{2};
    std::atomic<bool> cancelling{false};
    std::atomic<bool> failed{false};
    std::mutex stateMutex;
    std::condition_variable changed;
    std::exception_ptr failure;
    std::vector<std::unique_ptr<capture::MediaInput>> inputs;
    std::vector<capture::Kind> kinds;
    std::vector<std::thread> workers;
    if (config.capture.source == "file") {
        inputs.push_back(std::make_unique<capture::MediaInput>());
        kinds.push_back(capture::Kind::Video); // 一个文件只由一个线程解复用，随后分流两轨。
    } else {
        for (const auto kind : {capture::Kind::Video, capture::Kind::Audio}) {
            if (kind == capture::Kind::Video ? !config.capture.video : !config.capture.audio) continue;
            inputs.push_back(std::make_unique<capture::MediaInput>());
            kinds.push_back(kind);
        }
    }
    std::unique_ptr<rtmp::RtmpPublisher> streaming;
    if (config.streaming.enabled) streaming = std::make_unique<rtmp::RtmpPublisher>(config, logger);

    // 正常停止保留已入队数据，错误停止丢弃缓存；两条路径都唤醒所有队列等待者。
    const auto stopAll = [&](bool abort) {
        cancelling.store(true);
        for (auto& input : inputs) input->requestStop();
        if (abort) {
            videoFrames.abort();
            audioFrames.abort();
            output.abort();
        } else {
            videoFrames.close();
            audioFrames.close();
        }
        // 网络尾包允许舍弃，独立本地文件仍由输出线程写完尾部。
        if (streaming) streaming->requestStop();
        changed.notify_all();
    };
    const auto recordFailure = [&](std::exception_ptr problem) {
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            if (!failure) failure = std::move(problem);
        }
        failed.store(true);
        stopAll(true);
    };
    const auto joinAll = [&] {
        for (auto& worker : workers) if (worker.joinable()) worker.join();
    };
    // 工作线程统一异常边界；异常产生处立即中断其他阶段，避免主线程 join 时才发现阻塞。
    const auto launch = [&](auto work) {
        workers.emplace_back([&, work = std::move(work)]() mutable {
            try { work(); }
            catch (...) { recordFailure(std::current_exception()); }
            ++done;
            changed.notify_all();
        });
    };
    const auto reportQueues = [&] {
        const auto video = videoFrames.stats();
        const auto audio = audioFrames.stats();
        const auto packets = output.stats();
        logger.log(logging::Level::Info, "pipeline", "queues video=" + std::to_string(video.size)
            + "/" + std::to_string(video.capacity) + " video_high=" + std::to_string(video.highWaterMark)
            + " audio=" + std::to_string(audio.size) + "/" + std::to_string(audio.capacity)
            + " audio_high=" + std::to_string(audio.highWaterMark)
            + " output=" + std::to_string(packets.size) + "/" + std::to_string(packets.capacity)
            + " output_high=" + std::to_string(packets.highWaterMark)
            + " aborted_raw_video=" + std::to_string(video.discarded)
            + " aborted_raw_audio=" + std::to_string(audio.discarded)
            + " aborted_output_events=" + std::to_string(packets.discarded)
            + " dropped_raw_video=" + std::to_string(droppedVideo.load()));
        if (streaming) {
            const auto stats = streaming->stats();
            logger.log(logging::Level::Info, "network", "RTMP sent_packets=" + std::to_string(stats.sentPackets)
                + " sent_payload_bytes=" + std::to_string(stats.sentBytes)
                + " average_bitrate_bps=" + std::to_string(stats.averageBitrate)
                + " duration_seconds=" + std::to_string(stats.durationSeconds)
                + " dropped_packets=" + std::to_string(stats.droppedPackets)
                + " queued_video=" + std::to_string(stats.videoQueued)
                + " queued_audio=" + std::to_string(stats.audioQueued)
                + " queued_packets=" + std::to_string(stats.queuedPackets)
                + " queue_high=" + std::to_string(stats.queueHighWaterMark)
                + " network_errors=" + std::to_string(stats.networkErrors)
                + " reconnects=" + std::to_string(stats.reconnects)
                + " state=" + network::stateName(stats.network.state)
                + " connections=" + std::to_string(stats.network.connections)
                + " retry_attempts=" + std::to_string(stats.network.reconnectAttempts)
                + " consecutive_failures=" + std::to_string(stats.network.consecutiveFailures)
                + " retry_delay_ms=" + std::to_string(stats.network.retryDelayMs)
                + " connection_seconds=" + std::to_string(stats.network.connectionSeconds)
                + " connected_seconds=" + std::to_string(stats.network.connectedSeconds)
                + " idle_seconds=" + std::to_string(stats.network.idleSeconds)
                + " last_error_code=" + std::to_string(stats.network.lastErrorCode)
                + " last_error=" + stats.network.lastErrorMessage);
        }
    };
    const auto sendParameters = [&](capture::Kind kind, media::CodecParametersPtr parameters, AVRational timeBase) {
        OutputEvent event;
        event.type = OutputEvent::Type::Parameters;
        event.kind = kind;
        event.parameters = std::move(parameters);
        event.timeBase = timeBase;
        return output.push(std::move(event));
    };
    const auto sendPackets = [&](capture::Kind kind, std::vector<media::PacketPtr> packets) {
        for (auto& packet : packets) {
            OutputEvent event;
            event.kind = kind;
            event.packet = std::move(packet);
            if (!output.push(std::move(event))) return false;
        }
        return true;
    };
    const auto finishTrack = [&](capture::Kind kind) {
        OutputEvent event;
        event.type = OutputEvent::Type::End;
        event.kind = kind;
        output.push(std::move(event));
        // 最后一个编码线程关闭输出队列；消费者继续读取已排队的尾包和 End。
        if (encodersLeft.fetch_sub(1) == 1) output.close();
    };

    logger.log(logging::Level::Info, "pipeline", "M14 bounded capture -> preprocessing/encoding -> mux/output pipeline started. Ctrl+C stops processing.");
    try {
        // 本地两个封装上下文只在此线程创建、写入、写尾与销毁。
        // RTMP 则由已有的唯一发送线程执行 FLV 和网络 I/O。
        launch([&] {
            std::unique_ptr<muxer::VideoFileWriter> videoFile;
            std::unique_ptr<muxer::AudioFileWriter> audioFile;
            OutputEvent event;
            while (output.pop(event)) {
                if (failed.load()) break;
                switch (event.type) {
                case OutputEvent::Type::Start:
                    if (streaming) streaming->start(event.video, event.audio);
                    break;
                case OutputEvent::Type::Parameters:
                    if (event.kind == capture::Kind::Video) {
                        if (streaming) streaming->setVideoParameters(*event.parameters, event.timeBase);
                        if (!config.videoFile.empty()) {
                            videoFile = std::make_unique<muxer::VideoFileWriter>(config.videoFile, *event.parameters, event.timeBase);
                            logger.log(logging::Level::Info, "video_encode", "Saving H.264 video to " + config.videoFile);
                        }
                    } else {
                        if (streaming) streaming->setAudioParameters(*event.parameters, event.timeBase);
                        if (!config.audioFile.empty()) {
                            audioFile = std::make_unique<muxer::AudioFileWriter>(config.audioFile, *event.parameters, event.timeBase);
                            logger.log(logging::Level::Info, "audio_encode", "Saving AAC audio to " + config.audioFile);
                        }
                    }
                    break;
                case OutputEvent::Type::Packet:
                    if (event.kind == capture::Kind::Video) {
                        if (streaming) streaming->submitVideo(videoFile ? media::referencePacket(*event.packet) : std::move(event.packet));
                        if (videoFile) videoFile->write(std::move(event.packet));
                    } else {
                        if (streaming) streaming->submitAudio(audioFile ? media::referencePacket(*event.packet) : std::move(event.packet));
                        if (audioFile) audioFile->write(std::move(event.packet));
                    }
                    break;
                case OutputEvent::Type::End:
                    if (event.kind == capture::Kind::Video) {
                        if (videoFile) {
                            videoFile->finish();
                            logger.log(logging::Level::Info, "video_encode", "Video file completed: " + config.videoFile);
                        }
                        if (streaming) streaming->finishVideo();
                    } else {
                        if (audioFile) {
                            audioFile->finish();
                            logger.log(logging::Level::Info, "audio_encode", "Audio file completed: " + config.audioFile);
                        }
                        if (streaming) streaming->finishAudio();
                    }
                    break;
                }
                // 尽早释放上一个事件的编码参数/媒体包，避免空队列等待时继续持有尾包。
                event = OutputEvent{};
            }
            if (!failed.load() && streaming) streaming->finish();
        });
        launch([&] {
            std::unique_ptr<preprocess::VideoPreprocessor> video;
            std::unique_ptr<encoder::VideoEncoder> encoder;
            bool reported = false;
            std::uint64_t inputFrames = 0;
            const auto consume = [&](std::vector<media::FramePtr> frames) {
                for (const auto& frame : frames) {
                    if (failed.load()) return;
                    if (!reported) {
                        logger.log(logging::Level::Info, "preprocess", "video output: " + describe(*frame, capture::Kind::Video)
                            + " pts=" + std::to_string(frame->pts) + " time_base="
                            + std::to_string(frame->time_base.num) + "/" + std::to_string(frame->time_base.den));
                        reported = true;
                    }
                    if (!encoder) {
                        encoder = std::make_unique<encoder::VideoEncoder>(encoderOptions(config, *frame));
                        if (!sendParameters(capture::Kind::Video, encoder->parameters(), encoder->timeBase())) return;
                        logger.log(logging::Level::Info, "video_encode", "encoder=" + encoder->codecName()
                            + " bitrate=" + std::to_string(config.video.bitrate)
                            + " gop=" + std::to_string(config.videoEncoder.gopSize));
                    }
                    if (!sendPackets(capture::Kind::Video, encoder->encode(*frame))) return;
                }
            };
            RawFrame raw;
            while (videoFrames.pop(raw)) {
                if (failed.load()) break;
                // 仅原始视频可以安全丢旧帧；文件非实时转换始终背压并保留全部输入。
                const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - raw.queuedAt).count();
                if (realtime && age > config.pipeline.maxVideoAgeMs && videoFrames.stats().size > 0) {
                    ++droppedVideo;
                    continue;
                }
                if (!video) video = std::make_unique<preprocess::VideoPreprocessor>(videoOptions(config), originUs);
                ++inputFrames;
                consume(video->process(*raw.captured.frame, raw.captured.timestampUs));
                raw = RawFrame{};
            }
            if (!failed.load()) {
                if (video) consume(video->flush());
                if (encoder) sendPackets(capture::Kind::Video, encoder->flush());
                finishTrack(capture::Kind::Video);
            }
            std::string stats = "video processing ended: input_frames=" + std::to_string(inputFrames);
            if (video) {
                const auto s = video->stats();
                stats += " video_out=" + std::to_string(s.outputFrames) + " dropped=" + std::to_string(s.droppedFrames)
                    + " duplicated=" + std::to_string(s.duplicatedFrames)
                    + " skipped_timeline=" + std::to_string(s.skippedTimelineFrames)
                    + " input_fps=" + std::to_string(s.inputFps) + " output_fps=" + std::to_string(s.outputFps);
            }
            if (encoder) {
                const auto s = encoder->stats();
                stats += " encoded_frames=" + std::to_string(s.inputFrames) + " encoded_packets=" + std::to_string(s.outputPackets)
                    + " encoded_bytes=" + std::to_string(s.outputBytes) + " key_packets=" + std::to_string(s.keyPackets);
            }
            logger.log(logging::Level::Info, "video_encode", stats);
        });
        launch([&] {
            std::unique_ptr<preprocess::AudioPreprocessor> audio;
            std::unique_ptr<encoder::AudioEncoder> encoder;
            AudioLevels inputLevels;
            AudioLevels outputLevels;
            bool parametersSent = false;
            std::uint64_t inputFrames = 0;
            std::uint64_t inputSamples = 0;
            const auto consume = [&](std::vector<media::FramePtr> frames) {
                for (const auto& frame : frames) {
                    if (failed.load()) return;
                    outputLevels.add(*frame);
                    if (!parametersSent) {
                        logger.log(logging::Level::Info, "preprocess", "audio output: " + describe(*frame, capture::Kind::Audio)
                            + " pts=" + std::to_string(frame->pts) + " samples=" + std::to_string(frame->nb_samples));
                        if (!sendParameters(capture::Kind::Audio, encoder->parameters(), encoder->timeBase())) return;
                        parametersSent = true;
                    }
                    if (!sendPackets(capture::Kind::Audio, encoder->encode(*frame))) return;
                }
            };
            RawFrame raw;
            while (audioFrames.pop(raw)) {
                if (failed.load()) break;
                ++inputFrames;
                inputSamples += static_cast<std::uint64_t>(raw.captured.frame->nb_samples);
                inputLevels.add(*raw.captured.frame);
                if (!encoder) {
                    encoder = std::make_unique<encoder::AudioEncoder>(audioEncoderOptions(config));
                    audio = std::make_unique<preprocess::AudioPreprocessor>(audioOptions(config, *encoder), originUs);
                    const auto* format = av_get_sample_fmt_name(encoder->sampleFormat());
                    logger.log(logging::Level::Info, "audio_encode", "encoder=" + encoder->codecName()
                        + " sample_rate=" + std::to_string(encoder->sampleRate())
                        + " channels=" + std::to_string(encoder->channels())
                        + " sample_format=" + (format ? format : "unknown")
                        + " frame_size=" + std::to_string(encoder->frameSize())
                        + " bitrate=" + std::to_string(config.audio.bitrate)
                        + " gain_db=" + std::to_string(config.preprocess.audio.gainDb));
                    if (config.preprocess.audio.sampleFormat != (format ? format : "unknown") ||
                        config.preprocess.audio.frameSize != encoder->frameSize()) {
                        logger.log(logging::Level::Info, "audio_encode", "PCM sample format and frame size follow the opened AAC encoder, overriding preprocessing preferences.");
                    }
                }
                consume(audio->process(*raw.captured.frame, raw.captured.timestampUs));
                raw = RawFrame{};
            }
            if (!failed.load()) {
                if (audio) consume(audio->flush());
                if (encoder) sendPackets(capture::Kind::Audio, encoder->flush());
                finishTrack(capture::Kind::Audio);
                if (outputLevels.values >= static_cast<std::uint64_t>(config.audio.sampleRate) * config.audio.channels
                    && outputLevels.rms() < std::pow(10.0, -45.0 / 20.0)) {
                    logger.log(logging::Level::Warn, "capture", "Audio signal is very quiet (rms_dbfs="
                        + AudioLevels::db(outputLevels.rms())
                        + "). Check --audio-source, --audio-device, microphone input level, or --audio-gain-db.");
                }
            }
            std::string stats = "audio processing ended: input_frames=" + std::to_string(inputFrames)
                + " input_samples=" + std::to_string(inputSamples)
                + " input_audio_rms_dbfs=" + AudioLevels::db(inputLevels.rms())
                + " audio_rms_dbfs=" + AudioLevels::db(outputLevels.rms())
                + " audio_peak_dbfs=" + AudioLevels::db(outputLevels.peak);
            if (audio) {
                const auto s = audio->stats();
                stats += " audio_out=" + std::to_string(s.outputFrames) + " output_samples=" + std::to_string(s.outputSamples)
                    + " silence_samples=" + std::to_string(s.silenceSamples) + " padded_samples=" + std::to_string(s.paddedSamples)
                    + " discarded_input_samples=" + std::to_string(s.discardedSamples)
                    + " discontinuities=" + std::to_string(s.discontinuities)
                    + " clipped_audio_values=" + std::to_string(s.clippedValues);
            }
            if (encoder) {
                const auto s = encoder->stats();
                stats += " encoded_audio_frames=" + std::to_string(s.inputFrames)
                    + " encoded_audio_samples=" + std::to_string(s.inputSamples)
                    + " aac_packets=" + std::to_string(s.outputPackets) + " aac_bytes=" + std::to_string(s.outputBytes);
            }
            logger.log(logging::Level::Info, "audio_encode", stats);
        });
        if (config.capture.source != "file") {
            OutputEvent start;
            start.type = OutputEvent::Type::Start;
            start.video = config.capture.video;
            start.audio = config.capture.audio;
            output.push(std::move(start));
            if (!config.capture.video) videoFrames.close();
            if (!config.capture.audio) audioFrames.close();
        }
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            launch([&, i] {
                auto& input = *inputs[i];
                std::uint64_t videoCount = 0;
                std::uint64_t audioCount = 0;
                const auto closeQueues = [&] {
                    if (config.capture.source == "file" || kinds[i] == capture::Kind::Video) videoFrames.close();
                    if (config.capture.source == "file" || kinds[i] == capture::Kind::Audio) audioFrames.close();
                };
                try {
                    // dshow COM、解复用、输入解码和释放均留在所属采集线程。
                    if (config.capture.source == "file") input.openFile(config);
                    else input.openDevice(kinds[i], config);
                    if (config.capture.source == "file") {
                        OutputEvent start;
                        start.type = OutputEvent::Type::Start;
                        start.video = input.hasTrack(capture::Kind::Video);
                        start.audio = input.hasTrack(capture::Kind::Audio);
                        output.push(std::move(start));
                        if (!input.hasTrack(capture::Kind::Video)) videoFrames.close();
                        if (!input.hasTrack(capture::Kind::Audio)) audioFrames.close();
                    }
                    logger.log(logging::Level::Info, "capture", "Opened source: " + input.sourceDescription());
                    while (!cancelling.load()) {
                        auto captured = input.readFrame();
                        if (!captured || cancelling.load()) break;
                        auto& count = captured->kind == capture::Kind::Video ? videoCount : audioCount;
                        if (count++ == 0) logger.log(logging::Level::Info, "capture",
                            std::string(capture::kindName(captured->kind)) + " input: " + describe(*captured->frame, captured->kind));
                        RawFrame raw{std::move(*captured), Clock::now()};
                        if (raw.captured.kind == capture::Kind::Audio) {
                            // 音频整帧背压；不从队列中任意剔除 PCM，避免持续破音。
                            if (!audioFrames.push(std::move(raw))) break;
                        } else if (!realtime) {
                            if (!videoFrames.push(std::move(raw))) break;
                        } else if (!videoFrames.tryPush(raw)) {
                            if (cancelling.load()) break;
                            RawFrame old;
                            if (videoFrames.tryPop(old)) ++droppedVideo;
                            // 单生产者；消费者只会释放容量。关闭与此处竞争时直接停止。
                            if (!videoFrames.tryPush(raw)) break;
                        }
                    }
                } catch (const error::FFmpegError& e) {
                    if (!(cancelling.load() && e.module == "capture" && e.code == AVERROR_EXIT)) {
                        input.close();
                        throw;
                    }
                } catch (...) {
                    input.close();
                    throw;
                }
                input.close();
                closeQueues();
                logger.log(logging::Level::Info, "capture", "capture ended: video_frames=" + std::to_string(videoCount)
                    + " audio_frames=" + std::to_string(audioCount));
            });
        }
        const auto expected = workers.size();
        auto nextReport = Clock::now() + std::chrono::seconds(1);
        while (done.load() != expected) {
            if (streaming) streaming->checkError();
            if (failed.load()) stopAll(true);
            else if (stopRequested.load() && !cancelling.load()) stopAll(false);
            if (Clock::now() >= nextReport) {
                reportQueues();
                nextReport = Clock::now() + std::chrono::seconds(1);
            }
            std::unique_lock<std::mutex> lock(stateMutex);
            changed.wait_for(lock, std::chrono::milliseconds(50), [&] {
                return done.load() == expected;
            });
        }
        joinAll();
    } catch (...) {
        stopAll(true);
        joinAll();
        throw;
    }
    if (failure) std::rethrow_exception(failure);
    reportQueues();
    return 0;
}
} // namespace lsl::pipeline
