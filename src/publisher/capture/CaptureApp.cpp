#include "publisher/capture/CaptureApp.hpp"
#include "publisher/capture/MediaInput.hpp"
#include "publisher/encode/AudioEncoder.hpp"
#include "publisher/encode/AudioFileWriter.hpp"
#include "publisher/encode/VideoEncoder.hpp"
#include "publisher/encode/VideoFileWriter.hpp"
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

namespace lsl::capture {
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

std::string describe(const AVFrame& frame, Kind kind) {
    if (kind == Kind::Video) {
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
                                               const encode::AudioEncoder& encoder) {
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

encode::AudioEncoderOptions audioEncoderOptions(const config::PublisherConfig& config) {
    encode::AudioEncoderOptions options;
    options.sampleRate = config.audio.sampleRate;
    options.channels = config.audio.channels;
    options.bitrate = config.audio.bitrate;
    options.preferredSampleFormat = av_get_sample_fmt(config.preprocess.audio.sampleFormat.c_str());
    options.globalHeader = !config.audioFile.empty();
    return options;
}

encode::VideoEncoderOptions encoderOptions(const config::PublisherConfig& config, const AVFrame& firstFrame) {
    encode::VideoEncoderOptions options;
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
    // MP4 的 SPS/PPS 存在流参数中；后续 M07 可按实际输出格式选择此标志。
    options.globalHeader = !config.videoFile.empty();
    return options;
}
} // namespace

int runPublisher(const config::PublisherConfig& config,
                 const config::CommandLineOptions& options, logging::Logger& logger) {
    if (options.listDevices) {
        bool failed = false;
        for (const auto kind : {Kind::Video, Kind::Audio}) {
            try {
                const auto devices = listDevices(kind);
                std::cout << kindName(kind) << " devices: " << devices.size() << '\n';
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
        const auto kind = options.deviceOptions == "video" ? Kind::Video : Kind::Audio;
        printDeviceOptions(kind, kind == Kind::Video ? config.videoDevice : config.audioDevice);
        return 0;
    }

    SignalScope signals;
    std::vector<std::unique_ptr<MediaInput>> inputs;
    std::vector<Kind> kinds;
    if (config.capture.source == "file") {
        inputs.push_back(std::make_unique<MediaInput>());
        kinds.push_back(Kind::Video); // 文件线程同时处理所启用的音视频轨。
    } else {
        for (const auto kind : {Kind::Video, Kind::Audio}) {
            if ((kind == Kind::Video && !config.capture.video) || (kind == Kind::Audio && !config.capture.audio)) continue;
            inputs.push_back(std::make_unique<MediaInput>());
            kinds.push_back(kind);
        }
    }
    std::mutex stateMutex;
    std::condition_variable changed;
    std::atomic<std::size_t> done{0};
    std::exception_ptr failure;
    std::atomic<bool> failed{false};
    std::atomic<bool> cancelling{false};
    std::vector<std::thread> workers;
    // 两个设备使用同一个时钟原点，保留启动偏移；文件的 M02 时间戳已经共用零点。
    const std::int64_t originUs = config.capture.source == "file" ? 0 : av_gettime_relative();
    const auto stopAll = [&] {
        cancelling = true;
        for (auto& input : inputs) input->requestStop();
    };
    const auto joinAll = [&] { for (auto& worker : workers) if (worker.joinable()) worker.join(); };
    logger.log(logging::Level::Info, "capture", "M02-M06 capture, preprocessing and H.264/AAC encoding started. Ctrl+C stops processing.");
    try {
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            workers.emplace_back([&, i] {
                auto& input = *inputs[i];
                std::uint64_t videoFrames = 0;
                std::uint64_t audioFrames = 0;
                std::uint64_t audioSamples = 0;
                std::unique_ptr<preprocess::VideoPreprocessor> video;
                std::unique_ptr<preprocess::AudioPreprocessor> audio;
                // 每轨的预处理、编码与本地写出都在所属输入线程串行执行。
                // M14 再引入有界队列；交接的包结构已由 PacketPtr 独占。
                std::unique_ptr<encode::VideoEncoder> encoder;
                std::unique_ptr<encode::VideoFileWriter> videoFile;
                std::unique_ptr<encode::AudioEncoder> audioEncoder;
                std::unique_ptr<encode::AudioFileWriter> audioFile;
                AudioLevels inputAudioLevels;
                AudioLevels outputAudioLevels;
                bool reportedVideo = false;
                bool reportedAudio = false;
                auto lastReportUs = av_gettime_relative();
                const auto counts = [&] {
                    auto result = "video_frames=" + std::to_string(videoFrames) + " audio_frames=" + std::to_string(audioFrames)
                        + " audio_samples=" + std::to_string(audioSamples);
                    if (video) {
                        const auto s = video->stats();
                        result += " video_out=" + std::to_string(s.outputFrames)
                            + " dropped=" + std::to_string(s.droppedFrames)
                            + " duplicated=" + std::to_string(s.duplicatedFrames)
                            + " skipped_timeline=" + std::to_string(s.skippedTimelineFrames)
                            + " input_fps=" + std::to_string(s.inputFps)
                            + " output_fps=" + std::to_string(s.outputFps);
                    }
                    if (audio) {
                        const auto s = audio->stats();
                        result += " audio_out=" + std::to_string(s.outputFrames)
                            + " output_samples=" + std::to_string(s.outputSamples)
                            + " silence_samples=" + std::to_string(s.silenceSamples)
                            + " padded_samples=" + std::to_string(s.paddedSamples)
                            + " discarded_input_samples=" + std::to_string(s.discardedSamples)
                            + " discontinuities=" + std::to_string(s.discontinuities)
                            + " clipped_audio_values=" + std::to_string(s.clippedValues)
                            + " input_audio_rms_dbfs=" + AudioLevels::db(inputAudioLevels.rms())
                            + " audio_rms_dbfs=" + AudioLevels::db(outputAudioLevels.rms())
                            + " audio_peak_dbfs=" + AudioLevels::db(outputAudioLevels.peak);
                    }
                    if (encoder) {
                        const auto s = encoder->stats();
                        result += " encoded_frames=" + std::to_string(s.inputFrames)
                            + " encoded_packets=" + std::to_string(s.outputPackets)
                            + " encoded_bytes=" + std::to_string(s.outputBytes)
                            + " key_packets=" + std::to_string(s.keyPackets);
                    }
                    if (audioEncoder) {
                        const auto s = audioEncoder->stats();
                        result += " encoded_audio_frames=" + std::to_string(s.inputFrames)
                            + " encoded_audio_samples=" + std::to_string(s.inputSamples)
                            + " aac_packets=" + std::to_string(s.outputPackets)
                            + " aac_bytes=" + std::to_string(s.outputBytes);
                    }
                    return result;
                };
                const auto writePackets = [&](std::vector<media::PacketPtr> packets) {
                    for (auto& packet : packets) {
                        if (videoFile) videoFile->write(std::move(packet));
                        // 禁用本地输出时只做编码统计，后续 M07 在此接收独占编码包。
                    }
                };
                const auto writeAudioPackets = [&](std::vector<media::PacketPtr> packets) {
                    for (auto& packet : packets) {
                        if (audioFile) audioFile->write(std::move(packet));
                        // M07 后续在此交接 AAC 包；当前每个 AVPacket 仍保持独占所有权。
                    }
                };
                const auto consume = [&](Kind kind, std::vector<media::FramePtr> frames) {
                    auto& reported = kind == Kind::Video ? reportedVideo : reportedAudio;
                    for (const auto& frame : frames) {
                        if (!reported) {
                            logger.log(logging::Level::Info, "preprocess", std::string(kindName(kind))
                                + " output: " + describe(*frame, kind) + " pts=" + std::to_string(frame->pts)
                                + " time_base=" + std::to_string(frame->time_base.num) + "/"
                                + std::to_string(frame->time_base.den)
                                + " samples=" + std::to_string(frame->nb_samples));
                            reported = true;
                        }
                        if (kind == Kind::Video) {
                            // 首个视频帧到来时才建文件，音频文件输入不会留下空 MP4。
                            if (!encoder) {
                                encoder = std::make_unique<encode::VideoEncoder>(encoderOptions(config, *frame));
                                if (!config.videoFile.empty()) {
                                    const auto parameters = encoder->parameters();
                                    videoFile = std::make_unique<encode::VideoFileWriter>(
                                        config.videoFile, *parameters, encoder->timeBase());
                                    logger.log(logging::Level::Info, "video_encode", "Saving H.264 video to " + config.videoFile);
                                }
                                logger.log(logging::Level::Info, "video_encode", "encoder=" + encoder->codecName()
                                    + " bitrate=" + std::to_string(config.video.bitrate)
                                    + " gop=" + std::to_string(config.videoEncoder.gopSize));
                            }
                            writePackets(encoder->encode(*frame));
                        } else {
                            outputAudioLevels.add(*frame);
                            // 音频输入先完成编码参数协商；只有实际 PCM 输出才创建文件。
                            if (!audioFile && !config.audioFile.empty()) {
                                const auto parameters = audioEncoder->parameters();
                                audioFile = std::make_unique<encode::AudioFileWriter>(
                                    config.audioFile, *parameters, audioEncoder->timeBase());
                                logger.log(logging::Level::Info, "audio_encode", "Saving AAC audio to " + config.audioFile);
                            }
                            writeAudioPackets(audioEncoder->encode(*frame));
                        }
                    }
                };
                try {
                    // dshow 的 COM 初始化与释放、设备读包都留在同一工作线程。
                    if (config.capture.source == "file") input.openFile(config);
                    else input.openDevice(kinds[i], config);
                    logger.log(logging::Level::Info, "capture", "Opened source: " + input.sourceDescription());
                    if (config.capture.video && (config.capture.source == "file" || kinds[i] == Kind::Video)) {
                        video = std::make_unique<preprocess::VideoPreprocessor>(videoOptions(config), originUs);
                    }
                    while (auto captured = input.readFrame()) {
                        auto& count = captured->kind == Kind::Video ? videoFrames : audioFrames;
                        if (count++ == 0) logger.log(logging::Level::Info, "capture",
                            std::string(kindName(captured->kind)) + " input: " + describe(*captured->frame, captured->kind));
                        if (captured->kind == Kind::Audio) audioSamples += static_cast<std::uint64_t>(captured->frame->nb_samples);
                        if (captured->kind == Kind::Video) {
                            consume(Kind::Video, video->process(*captured->frame, captured->timestampUs));
                        } else {
                            inputAudioLevels.add(*captured->frame);
                            if (!audioEncoder) {
                                audioEncoder = std::make_unique<encode::AudioEncoder>(audioEncoderOptions(config));
                                audio = std::make_unique<preprocess::AudioPreprocessor>(
                                    audioOptions(config, *audioEncoder), originUs);
                                const auto* format = av_get_sample_fmt_name(audioEncoder->sampleFormat());
                                logger.log(logging::Level::Info, "audio_encode", "encoder=" + audioEncoder->codecName()
                                    + " sample_rate=" + std::to_string(audioEncoder->sampleRate())
                                    + " channels=" + std::to_string(audioEncoder->channels())
                                    + " sample_format=" + (format ? format : "unknown")
                                    + " frame_size=" + std::to_string(audioEncoder->frameSize())
                                    + " bitrate=" + std::to_string(config.audio.bitrate)
                                    + " gain_db=" + std::to_string(config.preprocess.audio.gainDb));
                                if (config.preprocess.audio.sampleFormat != (format ? format : "unknown") ||
                                    config.preprocess.audio.frameSize != audioEncoder->frameSize()) {
                                    logger.log(logging::Level::Info, "audio_encode",
                                        "PCM sample format and frame size follow the opened AAC encoder, overriding preprocessing preferences.");
                                }
                            }
                            consume(Kind::Audio, audio->process(*captured->frame, captured->timestampUs));
                        }
                        const auto now = av_gettime_relative();
                        if (now - lastReportUs >= 1000000) {
                            logger.log(logging::Level::Info, "capture", counts() + " timestamp_us=" + std::to_string(captured->timestampUs));
                            lastReportUs = now;
                        }
                    }
                    // EOF 或正常停止时，排空重采样延迟与 FIFO；错误退出时直接 RAII 清理。
                    if (!failed.load()) {
                        if (video) consume(Kind::Video, video->flush());
                        if (audio) consume(Kind::Audio, audio->flush());
                        // 先排空编码器延迟包，再写 MP4 尾部；Ctrl+C 与文件 EOF 路径一致。
                        if (encoder) writePackets(encoder->flush());
                        if (audioEncoder) writeAudioPackets(audioEncoder->flush());
                        if (videoFile) {
                            videoFile->finish();
                            logger.log(logging::Level::Info, "video_encode", "Video file completed: " + config.videoFile);
                        }
                        if (audioFile) {
                            audioFile->finish();
                            logger.log(logging::Level::Info, "audio_encode", "Audio file completed: " + config.audioFile);
                        }
                        if (outputAudioLevels.values >= static_cast<std::uint64_t>(config.audio.sampleRate) * config.audio.channels
                            && outputAudioLevels.rms() < std::pow(10.0, -45.0 / 20.0)) {
                            logger.log(logging::Level::Warn, "capture",
                                "Audio signal is very quiet (rms_dbfs=" + AudioLevels::db(outputAudioLevels.rms())
                                + "). Check --audio-source, --audio-device, microphone input level, or --audio-gain-db.");
                        }
                    }
                } catch (const error::FFmpegError& e) {
                    // 只忽略采集打开/读包被主动中断；编码和写尾失败仍须报告。
                    if (!(cancelling.load() && e.module == "capture" && e.code == AVERROR_EXIT)) {
                        std::lock_guard<std::mutex> lock(stateMutex);
                        if (!failure) failure = std::current_exception();
                        failed = true;
                    }
                } catch (...) {
                    std::lock_guard<std::mutex> lock(stateMutex);
                    if (!failure) failure = std::current_exception();
                    failed = true;
                }
                input.close();
                logger.log(logging::Level::Info, "capture", "processing ended: " + counts());
                ++done;
                changed.notify_one();
            });
        }
        std::unique_lock<std::mutex> lock(stateMutex);
        while (done.load() != inputs.size()) {
            if (stopRequested || failed.load()) stopAll();
            changed.wait_for(lock, std::chrono::milliseconds(50));
        }
        lock.unlock();
        joinAll();
    } catch (...) {
        stopAll();
        joinAll();
        throw;
    }
    if (failure) std::rethrow_exception(failure);
    return 0;
}
} // namespace lsl::capture
