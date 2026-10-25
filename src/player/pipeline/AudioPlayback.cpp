#include "player/pipeline/AudioPlayback.hpp"
#include "player/audio/IAudioSink.hpp"
#include "player/demuxer/Demuxer.hpp"

#include <atomic>
#include <chrono>
#include <exception>
#include <mutex>
#include <string>
#include <thread>

namespace lsl::player {

struct AudioPlayback::Impl {
    demuxer::Demuxer& input;
    audio::IAudioSink* sink;
    logging::Logger& logger;
    int sampleRate;
    int channels;
    std::atomic<bool> stopping{false};
    mutable std::mutex mutex;
    AudioPlaybackStats counters;
    std::exception_ptr failure;
    std::thread worker;

    Impl(const config::PlayerConfig& options, demuxer::Demuxer& source,
         audio::IAudioSink* output, logging::Logger& log)
        : input(source), sink(output), logger(log), sampleRate(options.sampleRate), channels(options.channels) {
        worker = std::thread([this] { run(); });
    }
    ~Impl() { stop(); }

    void stop() noexcept {
        stopping.store(true, std::memory_order_relaxed);
        input.requestStop();
        if (worker.joinable()) worker.join();
    }
    void update(const decoder::AudioDecodeStats& stats) {
        std::lock_guard<std::mutex> lock(mutex);
        counters.decode = stats;
    }
    void publish(const std::vector<decoder::DecodedAudioBlock>& blocks) {
        for (const auto& block : blocks) {
            for (;;) {
                if (stopping.load(std::memory_order_relaxed) ||
                    block.serial != input.stats().buffer.serial) return;
                sink->checkError();
                if (sink->enqueue(block)) break;
                // 不任意删音频采样。独立线程可等待，而视频解码与主线程继续运行。
                // 每次短等后检查停止/换代，暂停或断流不会使 join 永久等待。
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
    }
    void run() noexcept {
        try {
            decoder::AudioDecoder decoder(sampleRate, channels, logger);
            bool initialized = false;
            std::uint64_t serial = 0;
            while (!stopping.load(std::memory_order_relaxed)) {
                auto event = input.pop(demuxer::TrackKind::Audio, 10);
                if (event.kind == demuxer::EventKind::Reset) {
                    if (event.serial != input.stats().buffer.serial) continue;
                    serial = event.serial;
                    initialized = sink && event.streams && event.streams->audio.index >= 0;
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        counters.serial = serial;
                        counters.activeTrack = initialized;
                    }
                    if (initialized) {
                        decoder.reset(event.streams->audio, serial);
                        sink->reset(serial);
                        logger.log(logging::Level::Info, "player-audio", "audio reset serial=" +
                            std::to_string(serial) + " session=" + std::to_string(event.streams->session));
                    } else {
                        // 首次连接失败尚无快照、流无音频、或用户禁用音频时不打开设备。
                        decoder.reset({}, serial);
                        if (sink) sink->clear(serial);
                    }
                } else if (event.kind == demuxer::EventKind::Packet) {
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        ++counters.consumedPackets;
                    }
                    if (initialized && event.serial == serial && serial == input.stats().buffer.serial) {
                        publish(decoder.decode(*event.packet, serial));
                    }
                } else if (event.kind == demuxer::EventKind::End) {
                    input.checkError();
                    if (initialized && serial == input.stats().buffer.serial) {
                        publish(decoder.flush());
                        if (!stopping.load(std::memory_order_relaxed) && serial == input.stats().buffer.serial) {
                            sink->finish();
                        }
                    }
                    update(decoder.stats());
                    break;
                } else if (event.kind == demuxer::EventKind::Stopped) {
                    break;
                }
                update(decoder.stats());
                input.checkError();
                if (sink) sink->checkError();
            }
            update(decoder.stats());
            std::lock_guard<std::mutex> lock(mutex);
            counters.finished = true;
        } catch (...) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                failure = std::current_exception();
                counters.finished = true;
            }
            input.requestStop();
        }
    }
};

AudioPlayback::AudioPlayback(const config::PlayerConfig& options, demuxer::Demuxer& input,
                             audio::IAudioSink* sink, logging::Logger& logger)
    : impl_(std::make_unique<Impl>(options, input, sink, logger)) {}
AudioPlayback::~AudioPlayback() = default;
void AudioPlayback::stop() noexcept { impl_->stop(); }
AudioPlaybackStats AudioPlayback::stats() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->failure) std::rethrow_exception(impl_->failure);
    return impl_->counters;
}

} // namespace lsl::player
