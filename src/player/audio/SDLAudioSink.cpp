#include "player/audio/SDLAudioSink.hpp"
#include "common/error/Error.hpp"
#include "common/media/SDLResources.hpp"

#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <vector>

namespace lsl::audio {
namespace {
using Clock = std::chrono::steady_clock;
constexpr const char* module = "audio-sink";
static_assert(std::atomic<std::uint64_t>::is_always_lock_free &&
              std::atomic<std::int64_t>::is_always_lock_free && std::atomic<bool>::is_always_lock_free,
              "audio callback requires lock-free counters");

[[noreturn]] void failSDL(const char* operation) {
    throw error::AppError(module, error::Io, std::string(operation) + ": " + SDL_GetError());
}
void requireSDL(bool result, const char* operation) {
    if (!result) failSDL(operation);
}

class AudioSubsystem {
public:
    AudioSubsystem() {
        SDL_SetMainReady();
        requireSDL(SDL_InitSubSystem(SDL_INIT_AUDIO), "initialize SDL audio");
    }
    ~AudioSubsystem() { SDL_QuitSubSystem(SDL_INIT_AUDIO); }
    AudioSubsystem(const AudioSubsystem&) = delete;
    AudioSubsystem& operator=(const AudioSubsystem&) = delete;
};

class StreamLock {
public:
    explicit StreamLock(SDL_AudioStream* stream) : stream_(stream) {
        requireSDL(SDL_LockAudioStream(stream_), "lock SDL audio stream");
    }
    ~StreamLock() { SDL_UnlockAudioStream(stream_); }
    StreamLock(const StreamLock&) = delete;
    StreamLock& operator=(const StreamLock&) = delete;
private:
    SDL_AudioStream* stream_;
};

std::size_t capacityOf(const AudioSinkOptions& options) {
    if (options.sampleRate < 7350 || options.sampleRate > 192000 || options.channels < 1 || options.channels > 8 ||
        options.bufferMs < 10 || options.bufferMs > 5000 || options.volume < 0 || options.volume > 100 || options.device.empty()) {
        throw error::AppError(module, error::InvalidConfig, "invalid audio sink format, buffer, volume or device");
    }
    return std::max<std::size_t>(4096, (static_cast<std::size_t>(options.sampleRate) * options.bufferMs + 999) / 1000);
}

SDL_AudioDeviceID selectDevice(const std::string& wanted) {
    if (wanted == "default") return SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
    int count = 0;
    struct FreeDevices {
        void operator()(SDL_AudioDeviceID* devices) const noexcept { SDL_free(devices); }
    };
    std::unique_ptr<SDL_AudioDeviceID, FreeDevices> devices{SDL_GetAudioPlaybackDevices(&count)};
    if (!devices) failSDL("enumerate audio playback devices");
    SDL_AudioDeviceID selected = 0;
    for (int index = 0; index < count; ++index) {
        const auto* name = SDL_GetAudioDeviceName(devices.get()[index]);
        if (!name) failSDL("read audio playback device name");
        if (wanted != name) continue;
        if (selected) {
            throw error::AppError(module, error::InvalidConfig, "audio device name is ambiguous: " + wanted);
        }
        selected = devices.get()[index];
    }
    if (!selected) throw error::AppError(module, error::InvalidConfig, "audio playback device not found: " + wanted);
    return selected;
}

std::int64_t frameEndTimestamp(std::int64_t start, std::size_t frames, int sampleRate) {
    if (start == AV_NOPTS_VALUE) return AV_NOPTS_VALUE;
    const auto offset = static_cast<std::int64_t>(frames * 1000000ULL / static_cast<std::size_t>(sampleRate));
    if (start > std::numeric_limits<std::int64_t>::max() - offset) return std::numeric_limits<std::int64_t>::max();
    return start + offset;
}
} // namespace

struct SDLAudioSink::Impl {
    // 生命周期：stream 必须先于 callback 的 userdata 和 SDL 音频子系统销毁。
    AudioSubsystem audio;
    logging::Logger& logger;
    AudioSinkOptions options;
    const std::size_t capacityFrames;
    const std::size_t channels;
    const std::size_t frameBytes;
    std::vector<float> samples;
    std::vector<std::int64_t> endTimestamps;
    mutable std::mutex control;
    media::AudioStreamPtr stream;
    std::atomic<std::uint64_t> readPosition{0}; // 只有 callback 推进；清理时保持原值。
    std::atomic<std::uint64_t> writePosition{0};
    std::atomic<std::uint64_t> serial{0};
    std::uint64_t producerSerial = 0; // 受 control 保护；clear 不冒充生产者 reset。
    std::atomic<bool> ready{false};
    std::atomic<bool> stopped{false};
    std::atomic<bool> finished{false};
    std::atomic<bool> flushed{false};
    std::atomic<bool> clockValid{false};
    std::atomic<std::int64_t> clockTimestamp{AV_NOPTS_VALUE};
    std::atomic<std::uint64_t> submittedFrames{0};
    std::atomic<std::uint64_t> underruns{0};
    std::atomic<std::uint64_t> droppedFrames{0};
    std::atomic<std::uint64_t> callbacks{0};
    enum class CallbackFailure : int { None, Query, Put, Flush };
    std::atomic<CallbackFailure> callbackFailure{CallbackFailure::None};
    std::chrono::microseconds tailProtection{20000};
    mutable std::optional<Clock::time_point> emptySince;

    Impl(const AudioSinkOptions& requested, logging::Logger& output)
        : logger(output), options(requested), capacityFrames(capacityOf(requested)),
          channels(static_cast<std::size_t>(requested.channels)), frameBytes(channels * sizeof(float)),
          samples(capacityFrames * channels), endTimestamps(capacityFrames, AV_NOPTS_VALUE) {}

    void rememberFailure(CallbackFailure failure) noexcept {
        auto expected = CallbackFailure::None;
        callbackFailure.compare_exchange_strong(expected, failure, std::memory_order_release, std::memory_order_relaxed);
    }

    void checkFailure() const {
        const auto failure = callbackFailure.load(std::memory_order_acquire);
        if (failure == CallbackFailure::None) return;
        const char* operation = failure == CallbackFailure::Query ? "query queued input" :
            failure == CallbackFailure::Put ? "submit PCM media" : "flush resampling tail";
        throw error::AppError(module, error::Io, std::string("SDL audio callback failed to ") + operation);
    }

    void flushIfEmpty(SDL_AudioStream* target) noexcept {
        if (!finished.load(std::memory_order_acquire) || flushed.load(std::memory_order_relaxed) ||
            readPosition.load(std::memory_order_relaxed) != writePosition.load(std::memory_order_acquire)) return;
        if (!SDL_FlushAudioStream(target)) rememberFailure(CallbackFailure::Flush);
        else flushed.store(true, std::memory_order_release);
    }

    static void SDLCALL fill(void* userdata, SDL_AudioStream* target, int additionalAmount, int) noexcept {
        auto& self = *static_cast<Impl*>(userdata);
        self.callbacks.fetch_add(1, std::memory_order_relaxed);
        if (self.stopped.load(std::memory_order_acquire) || !self.ready.load(std::memory_order_acquire) ||
            self.callbackFailure.load(std::memory_order_relaxed) != CallbackFailure::None) return;

        // SDL 在调用回调前已持有 stream 锁。这里从不取 control、不分配应用内存、不写日志。
        // additionalAmount 是输入格式的约略字节数，可能高估；只交付实际媒体，欠载由设备补静音。
        const auto wanted = additionalAmount > 0 ?
            (static_cast<std::size_t>(additionalAmount) + self.frameBytes - 1) / self.frameBytes : 0;
        auto read = self.readPosition.load(std::memory_order_relaxed);
        const auto write = self.writePosition.load(std::memory_order_acquire);
        const auto available = static_cast<std::size_t>(write - read);
        const int queued = SDL_GetAudioStreamQueued(target);
        if (queued < 0) { self.rememberFailure(CallbackFailure::Query); return; }
        const auto maximumBytes = self.capacityFrames * self.frameBytes;
        const auto queuedBytes = static_cast<std::size_t>(queued);
        const auto room = queuedBytes < maximumBytes ? (maximumBytes - queuedBytes) / self.frameBytes : 0;
        auto remaining = std::min({wanted, available, room});
        const auto selected = remaining;
        while (remaining > 0) {
            const auto offset = static_cast<std::size_t>(read % self.capacityFrames);
            const auto count = std::min(remaining, self.capacityFrames - offset);
            if (!SDL_PutAudioStreamData(target, self.samples.data() + offset * self.channels,
                                       static_cast<int>(count * self.frameBytes))) {
                self.rememberFailure(CallbackFailure::Put);
                return;
            }
            const auto timestamp = self.endTimestamps[offset + count - 1];
            self.clockTimestamp.store(timestamp, std::memory_order_relaxed);
            self.clockValid.store(timestamp != AV_NOPTS_VALUE, std::memory_order_release);
            self.submittedFrames.fetch_add(count, std::memory_order_relaxed);
            read += count;
            // Put 已复制数据，才允许生产者重新使用这些环形槽位。
            self.readPosition.store(read, std::memory_order_release);
            remaining -= count;
        }
        if (selected < wanted && !self.finished.load(std::memory_order_acquire)) {
            self.underruns.fetch_add(1, std::memory_order_relaxed);
        }
        self.flushIfEmpty(target); // EOF 不补无限静音，让 SDL 的重采样尾部真正排空。
    }

    void ensureOpen() {
        if (stream) return;
        const SDL_AudioSpec spec{SDL_AUDIO_F32, options.channels, options.sampleRate};
        stream.reset(SDL_OpenAudioDeviceStream(selectDevice(options.device), &spec, fill, this));
        if (!stream) failSDL("open audio playback device stream");
        requireSDL(SDL_SetAudioStreamGain(stream.get(), options.muted ? 0.0f : options.volume / 100.0f), "set audio stream gain");
        SDL_AudioSpec deviceFormat{};
        int deviceFrames = 0;
        if (SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(stream.get()), &deviceFormat, &deviceFrames) &&
            deviceFrames > 0 && deviceFormat.freq > 0) {
            const auto protection = 2LL * deviceFrames * 1000000LL / deviceFormat.freq;
            tailProtection = std::chrono::microseconds(std::clamp<std::int64_t>(protection, 10000, 250000));
        }
        logger.log(logging::Level::Info, module, "SDL float32 playback device=" + options.device +
            " rate=" + std::to_string(options.sampleRate) + " channels=" + std::to_string(options.channels) +
            " ring_frames=" + std::to_string(capacityFrames) + "; clock tracks callback submissions, not hardware playback");
    }

    void clearRing() noexcept {
        const auto read = readPosition.load(std::memory_order_relaxed);
        const auto write = writePosition.load(std::memory_order_relaxed);
        droppedFrames.fetch_add(write - read, std::memory_order_relaxed);
        writePosition.store(read, std::memory_order_release);
    }

    void changeEpoch(std::uint64_t generation) {
        // 调用者持 control，先停 device，之后才能锁 stream；设备线程的顺序是 device -> stream。
        // 绝不能在 StreamLock 中调用 Pause/Resume/Destroy，否则可形成反向锁序。
        if (stream) {
            requireSDL(SDL_PauseAudioStreamDevice(stream.get()), "pause audio for generation reset");
            StreamLock freeze(stream.get());
            const auto queued = SDL_GetAudioStreamQueued(stream.get());
            if (queued < 0) failSDL("query audio before generation reset");
            droppedFrames.fetch_add(static_cast<std::uint64_t>(queued) / frameBytes, std::memory_order_relaxed);
            requireSDL(SDL_ClearAudioStream(stream.get()), "clear obsolete SDL audio");
            clearRing();
            ready.store(false, std::memory_order_release);
            serial.store(generation, std::memory_order_release);
            finished.store(false, std::memory_order_release);
            flushed.store(false, std::memory_order_release);
            clockValid.store(false, std::memory_order_release);
            clockTimestamp.store(AV_NOPTS_VALUE, std::memory_order_relaxed);
        } else {
            clearRing();
            ready.store(false, std::memory_order_release);
            serial.store(generation, std::memory_order_release);
            finished.store(false, std::memory_order_release);
            flushed.store(false, std::memory_order_release);
            clockValid.store(false, std::memory_order_release);
            clockTimestamp.store(AV_NOPTS_VALUE, std::memory_order_relaxed);
        }
        emptySince.reset();
        // 无音轨的新会话保持设备暂停；下一次有效 reset 才按用户的暂停状态恢复。
    }
};

SDLAudioSink::SDLAudioSink(const AudioSinkOptions& options, logging::Logger& logger)
    : impl_(std::make_unique<Impl>(options, logger)) {}
SDLAudioSink::~SDLAudioSink() { stop(); }

void SDLAudioSink::reset(std::uint64_t serial) {
    std::lock_guard<std::mutex> lock(impl_->control);
    if (impl_->stopped.load(std::memory_order_acquire) || serial < impl_->serial.load(std::memory_order_relaxed)) return;
    impl_->checkFailure();
    if (serial > impl_->serial.load(std::memory_order_relaxed)) impl_->changeEpoch(serial);
    if (impl_->ready.load(std::memory_order_relaxed)) return;
    impl_->ensureOpen(); // SDL 新流初始暂停，尚无回调与生产者竞争。
    impl_->producerSerial = serial;
    impl_->finished.store(false, std::memory_order_release);
    impl_->flushed.store(false, std::memory_order_release);
    impl_->ready.store(true, std::memory_order_release);
    impl_->emptySince.reset();
    if (!impl_->options.paused) requireSDL(SDL_ResumeAudioStreamDevice(impl_->stream.get()), "start audio playback");
}

bool SDLAudioSink::enqueue(const decoder::DecodedAudioBlock& block) {
    std::lock_guard<std::mutex> lock(impl_->control);
    impl_->checkFailure();
    if (block.sampleRate != impl_->options.sampleRate || block.channels != impl_->options.channels ||
        block.samples.size() % impl_->channels != 0) {
        throw error::AppError(module, error::InvalidConfig, "PCM block does not match audio sink format");
    }
    const auto count = block.samples.size() / impl_->channels;
    if (impl_->stopped.load(std::memory_order_acquire) || !impl_->ready.load(std::memory_order_acquire) ||
        block.serial != impl_->serial.load(std::memory_order_relaxed)) {
        impl_->droppedFrames.fetch_add(count, std::memory_order_relaxed);
        return true; // 过时代次已处理；不能让生产者为它无限重试。
    }
    if (impl_->finished.load(std::memory_order_acquire)) {
        throw error::AppError(module, error::Unexpected, "cannot enqueue audio after EOF without a new reset");
    }
    if (count > impl_->capacityFrames) {
        throw error::AppError(module, error::InvalidConfig, "PCM block exceeds ring capacity; decoder must split large blocks");
    }
    const auto write = impl_->writePosition.load(std::memory_order_relaxed);
    const auto read = impl_->readPosition.load(std::memory_order_acquire);
    if (count > impl_->capacityFrames - static_cast<std::size_t>(write - read)) return false;
    // 这里只复制，不等待空位。false 返回前自动释放 control，暂停/换代/停止均可及时进入。
    for (std::size_t frame = 0; frame < count; ++frame) {
        const auto offset = static_cast<std::size_t>((write + frame) % impl_->capacityFrames);
        std::memcpy(impl_->samples.data() + offset * impl_->channels,
                    block.samples.data() + frame * impl_->channels, impl_->frameBytes);
        impl_->endTimestamps[offset] = frameEndTimestamp(block.timestampUs, frame + 1, impl_->options.sampleRate);
    }
    impl_->writePosition.store(write + count, std::memory_order_release);
    impl_->emptySince.reset();
    return true;
}

void SDLAudioSink::finish() {
    std::lock_guard<std::mutex> lock(impl_->control);
    impl_->checkFailure();
    if (impl_->stopped.load(std::memory_order_acquire) || !impl_->ready.load(std::memory_order_acquire) ||
        impl_->producerSerial != impl_->serial.load(std::memory_order_relaxed)) return;
    impl_->finished.store(true, std::memory_order_release);
    StreamLock freeze(impl_->stream.get());
    impl_->flushIfEmpty(impl_->stream.get());
    impl_->emptySince.reset();
    impl_->checkFailure();
}

bool SDLAudioSink::drained() const {
    std::lock_guard<std::mutex> lock(impl_->control);
    impl_->checkFailure();
    if (!impl_->stream || !impl_->ready.load(std::memory_order_acquire) || impl_->stopped.load(std::memory_order_acquire)) return true;
    if (!impl_->finished.load(std::memory_order_acquire) || !impl_->flushed.load(std::memory_order_acquire) || impl_->options.paused ||
        impl_->readPosition.load(std::memory_order_acquire) != impl_->writePosition.load(std::memory_order_relaxed)) {
        impl_->emptySince.reset();
        return false;
    }
    const auto queued = SDL_GetAudioStreamQueued(impl_->stream.get());
    const auto available = SDL_GetAudioStreamAvailable(impl_->stream.get());
    if (queued < 0 || available < 0) failSDL("query audio drain progress");
    if (queued != 0 || available != 0) {
        impl_->emptySince.reset();
        return false;
    }
    // SDL 已排空不等于声卡已经发声完毕。保留约两个设备周期作尾声保护，仍是近似值。
    const auto now = Clock::now();
    if (!impl_->emptySince) impl_->emptySince = now;
    return now - *impl_->emptySince >= impl_->tailProtection;
}

void SDLAudioSink::clear(std::uint64_t serial) {
    std::lock_guard<std::mutex> lock(impl_->control);
    if (impl_->stopped.load(std::memory_order_acquire) || serial <= impl_->serial.load(std::memory_order_relaxed)) return;
    impl_->checkFailure();
    impl_->changeEpoch(serial);
}

void SDLAudioSink::setPaused(bool paused) {
    std::lock_guard<std::mutex> lock(impl_->control);
    impl_->checkFailure();
    if (impl_->stopped.load(std::memory_order_acquire)) return;
    if (impl_->stream && impl_->ready.load(std::memory_order_acquire)) {
        requireSDL(paused ? SDL_PauseAudioStreamDevice(impl_->stream.get()) : SDL_ResumeAudioStreamDevice(impl_->stream.get()),
                   paused ? "pause audio playback" : "resume audio playback");
    }
    impl_->options.paused = paused;
    impl_->emptySince.reset();
}

void SDLAudioSink::setMuted(bool muted) {
    std::lock_guard<std::mutex> lock(impl_->control);
    impl_->checkFailure();
    if (impl_->stopped.load(std::memory_order_acquire)) return;
    if (impl_->stream) requireSDL(SDL_SetAudioStreamGain(impl_->stream.get(), muted ? 0.0f : impl_->options.volume / 100.0f), "mute audio stream");
    impl_->options.muted = muted;
}

void SDLAudioSink::setVolume(int volume) {
    std::lock_guard<std::mutex> lock(impl_->control);
    impl_->checkFailure();
    if (impl_->stopped.load(std::memory_order_acquire)) return;
    volume = std::clamp(volume, 0, 100);
    if (impl_->stream) requireSDL(SDL_SetAudioStreamGain(impl_->stream.get(), impl_->options.muted ? 0.0f : volume / 100.0f), "set audio stream volume");
    impl_->options.volume = volume;
}

AudioSinkStats SDLAudioSink::stats() const {
    std::lock_guard<std::mutex> lock(impl_->control);
    AudioSinkStats result;
    result.submittedFrames = impl_->submittedFrames.load(std::memory_order_relaxed);
    result.underruns = impl_->underruns.load(std::memory_order_relaxed);
    result.droppedFrames = impl_->droppedFrames.load(std::memory_order_relaxed);
    result.callbacks = impl_->callbacks.load(std::memory_order_relaxed);
    result.serial = impl_->serial.load(std::memory_order_relaxed);
    result.queuedFrames = static_cast<std::size_t>(impl_->writePosition.load(std::memory_order_relaxed) -
                                                 impl_->readPosition.load(std::memory_order_acquire));
    result.capacityFrames = impl_->capacityFrames;
    result.paused = impl_->options.paused;
    result.muted = impl_->options.muted;
    result.volume = impl_->options.volume;
    result.finished = impl_->finished.load(std::memory_order_acquire);
    result.opened = static_cast<bool>(impl_->stream);
    if (impl_->stream) {
        const int queued = SDL_GetAudioStreamQueued(impl_->stream.get());
        const int available = SDL_GetAudioStreamAvailable(impl_->stream.get());
        if (queued < 0 || available < 0) failSDL("query audio stream statistics");
        result.sdlQueuedBytes = static_cast<std::size_t>(queued);
        result.sdlAvailableBytes = static_cast<std::size_t>(available);
    }
    return result;
}

AudioClock SDLAudioSink::clock() const {
    std::lock_guard<std::mutex> lock(impl_->control);
    AudioClock result;
    result.serial = impl_->serial.load(std::memory_order_relaxed);
    result.valid = impl_->clockValid.load(std::memory_order_acquire);
    result.timestampUs = impl_->clockTimestamp.load(std::memory_order_relaxed);
    result.valid = result.valid && result.timestampUs != AV_NOPTS_VALUE;
    return result;
}

void SDLAudioSink::checkError() const {
    std::lock_guard<std::mutex> lock(impl_->control);
    impl_->checkFailure();
}

void SDLAudioSink::stop() noexcept {
    std::lock_guard<std::mutex> lock(impl_->control);
    if (impl_->stopped.exchange(true, std::memory_order_acq_rel)) return;
    if (impl_->stream) {
        // 此处不持有 stream 锁；禁用回调会等待已在执行的回调结束，再关闭设备。
        SDL_PauseAudioStreamDevice(impl_->stream.get());
        SDL_SetAudioStreamGetCallback(impl_->stream.get(), nullptr, nullptr);
    }
    impl_->clearRing();
    impl_->ready.store(false, std::memory_order_release);
    impl_->clockValid.store(false, std::memory_order_release);
    impl_->stream.reset();
    impl_->emptySince.reset();
}

} // namespace lsl::audio
