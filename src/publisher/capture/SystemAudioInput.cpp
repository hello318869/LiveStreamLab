#include "publisher/capture/SystemAudioInput.hpp"

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/mathematics.h>
#include <libavutil/time.h>
}

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <thread>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#endif

namespace lsl::capture {
namespace {
constexpr const char* module = "capture";

[[noreturn]] void fail(std::string message) {
    throw error::AppError(module, error::Io, std::move(message));
}

#ifdef _WIN32
constexpr int outputRate = 48000;
constexpr AVRational outputTimeBase{1, outputRate};
constexpr AVRational microseconds{1, 1000000};

void checked(int result, const char* operation) {
    error::throwIfFFmpegFailed(result, module, operation);
}

void checkWindows(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        throw error::AppError(module, static_cast<int>(result),
                              std::string(operation) + " (WASAPI HRESULT=" +
                              std::to_string(static_cast<unsigned long>(result)) + ")");
    }
}

struct ComRelease {
    template <typename T> void operator()(T* pointer) const noexcept { pointer->Release(); }
};
template <typename T> using ComPtr = std::unique_ptr<T, ComRelease>;

struct ChannelLayout {
    AVChannelLayout value{};
    ~ChannelLayout() { av_channel_layout_uninit(&value); }
};

std::string utf8(const wchar_t* value) {
    if (!value) return {};
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, nullptr, 0, nullptr, nullptr);
    if (count <= 0) fail("cannot read system-audio device name");
    std::string text(static_cast<std::size_t>(count), '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, text.data(), count, nullptr, nullptr)) {
        fail("cannot convert system-audio device name to UTF-8");
    }
    text.pop_back();
    return text;
}

// 每个成功 GetBuffer 的包都必须归还；即使 AVFrame 分配或转换抛出也不占用设备缓冲。
struct CaptureBuffer {
    IAudioCaptureClient* client;
    UINT32 frames;
    ~CaptureBuffer() { if (client) client->ReleaseBuffer(frames); }
    void release() {
        auto* owner = client;
        client = nullptr;
        checkWindows(owner->ReleaseBuffer(frames), "release system-audio capture packet");
    }
};

media::FramePtr allocateAudioFrame(int samples, int rate, AVSampleFormat format,
                                   const AVChannelLayout& layout) {
    media::FramePtr frame{av_frame_alloc()};
    if (!frame) throw std::bad_alloc{};
    frame->format = format;
    frame->sample_rate = rate;
    frame->nb_samples = samples;
    checked(av_channel_layout_copy(&frame->ch_layout, &layout), "set system-audio channel layout");
    checked(av_frame_get_buffer(frame.get(), 0), "allocate system-audio PCM");
    return frame;
}
#endif
} // namespace

struct SystemAudioInput::Impl {
    std::string name;
#ifdef _WIN32
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> device;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
    media::SwrContextPtr resampler;
    ChannelLayout inputLayout;
    ChannelLayout outputLayout;
    AVSampleFormat inputFormat = AV_SAMPLE_FMT_NONE;
    int inputRate = 0;
    int inputChannels = 0;
    int bytesPerFrame = 0;
    bool packed24 = false;
    bool uninitializeCom = false;
    bool started = false;
    bool hadPacket = false;
    std::int64_t qpcOffsetUs = 0;
    std::int64_t fallbackNextUs = 0;
    std::thread::id owner;

    void activateClient() {
        IAudioClient* raw = nullptr;
        checkWindows(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                       reinterpret_cast<void**>(&raw)), "activate system-audio client");
        client.reset(raw);
    }

    void setInputFormat(const WAVEFORMATEX& format) {
        if (!format.nSamplesPerSec || format.nSamplesPerSec > 384000 ||
            !format.nChannels || format.nChannels > 32) {
            fail("unsupported system-audio mix sample rate or channel count");
        }
        WORD tag = format.wFormatTag;
        DWORD mask = 0;
        if (tag == WAVE_FORMAT_EXTENSIBLE) {
            if (format.cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
                fail("incomplete system-audio extensible format");
            }
            const auto& extended = reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(format);
            mask = extended.dwChannelMask;
            if (IsEqualGUID(extended.SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) tag = WAVE_FORMAT_IEEE_FLOAT;
            else if (IsEqualGUID(extended.SubFormat, KSDATAFORMAT_SUBTYPE_PCM)) tag = WAVE_FORMAT_PCM;
            else fail("unsupported system-audio mix encoding");
        }
        packed24 = false;
        if (tag == WAVE_FORMAT_IEEE_FLOAT && format.wBitsPerSample == 32) inputFormat = AV_SAMPLE_FMT_FLT;
        else if (tag == WAVE_FORMAT_IEEE_FLOAT && format.wBitsPerSample == 64) inputFormat = AV_SAMPLE_FMT_DBL;
        else if (tag == WAVE_FORMAT_PCM && format.wBitsPerSample == 8) inputFormat = AV_SAMPLE_FMT_U8;
        else if (tag == WAVE_FORMAT_PCM && format.wBitsPerSample == 16) inputFormat = AV_SAMPLE_FMT_S16;
        else if (tag == WAVE_FORMAT_PCM && format.wBitsPerSample == 24) {
            inputFormat = AV_SAMPLE_FMT_S32;
            packed24 = true;
        } else if (tag == WAVE_FORMAT_PCM && format.wBitsPerSample == 32) inputFormat = AV_SAMPLE_FMT_S32;
        else fail("unsupported system-audio PCM bit depth");
        inputRate = static_cast<int>(format.nSamplesPerSec);
        inputChannels = format.nChannels;
        bytesPerFrame = format.nBlockAlign;
        if (bytesPerFrame != inputChannels * (format.wBitsPerSample / 8)) {
            fail("invalid system-audio PCM block alignment");
        }
        av_channel_layout_uninit(&inputLayout.value);
        if (mask) {
            checked(av_channel_layout_from_mask(&inputLayout.value, mask), "read system-audio speaker layout");
            if (inputLayout.value.nb_channels != inputChannels) fail("system-audio speaker mask does not match channel count");
        } else {
            av_channel_layout_default(&inputLayout.value, inputChannels);
        }
        if (!av_channel_layout_check(&inputLayout.value)) fail("unsupported system-audio channel layout");
        av_channel_layout_default(&outputLayout.value, 2);
        SwrContext* raw = nullptr;
        const int configured = swr_alloc_set_opts2(&raw, &outputLayout.value, AV_SAMPLE_FMT_FLT, outputRate,
                                                    &inputLayout.value, inputFormat, inputRate, 0, nullptr);
        resampler.reset(raw);
        checked(configured, "configure system-audio PCM conversion");
        if (!resampler) throw std::bad_alloc{};
        checked(swr_init(resampler.get()), "initialize system-audio PCM conversion");
    }

    void open() {
        owner = std::this_thread::get_id();
        const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (initialized != RPC_E_CHANGED_MODE) checkWindows(initialized, "initialize COM for system audio");
        uninitializeCom = SUCCEEDED(initialized);
        IMMDeviceEnumerator* rawEnumerator = nullptr;
        checkWindows(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&rawEnumerator)),
                     "create system-audio endpoint enumerator");
        enumerator.reset(rawEnumerator);
        IMMDevice* rawDevice = nullptr;
        checkWindows(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &rawDevice),
                     "find Windows default playback device for system audio");
        device.reset(rawDevice);
        IPropertyStore* rawProperties = nullptr;
        checkWindows(device->OpenPropertyStore(STGM_READ, &rawProperties), "read system-audio device properties");
        const ComPtr<IPropertyStore> properties{rawProperties};
        struct PropertyValue {
            PROPVARIANT value{};
            ~PropertyValue() { PropVariantClear(&value); }
        } friendly;
        checkWindows(properties->GetValue(PKEY_Device_FriendlyName, &friendly.value),
                     "read system-audio device name");
        name = friendly.value.vt == VT_LPWSTR ? utf8(friendly.value.pwszVal) : "Windows default playback device";
        activateClient();

        // 请求固定输出格式，让 Windows 在共享模式中完成必要的混音/重采样。
        // 部分驱动不支持该组合，届时重新激活 client 并按原生 mix format 采集。
        WAVEFORMATEX requested{};
        requested.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        requested.nChannels = 2;
        requested.nSamplesPerSec = outputRate;
        requested.wBitsPerSample = 32;
        requested.nBlockAlign = requested.nChannels * requested.wBitsPerSample / 8;
        requested.nAvgBytesPerSec = requested.nSamplesPerSec * requested.nBlockAlign;
        const auto result = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
            1000000, 0, &requested, nullptr);
        if (SUCCEEDED(result)) {
            setInputFormat(requested);
        } else {
            client.reset();
            activateClient();
            WAVEFORMATEX* rawMix = nullptr;
            checkWindows(client->GetMixFormat(&rawMix), "read system-audio mix format");
            const std::unique_ptr<WAVEFORMATEX, decltype(&CoTaskMemFree)> mix{rawMix, CoTaskMemFree};
            setInputFormat(*mix);
            checkWindows(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                                              1000000, 0, mix.get(), nullptr), "initialize system-audio loopback");
        }
        IAudioCaptureClient* rawCapture = nullptr;
        checkWindows(client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&rawCapture)),
                     "get system-audio capture service");
        capture.reset(rawCapture);

        // WASAPI QPC 位置以 100 ns 表示；与 FFmpeg 单调时钟校准一次，保持音视频共用时钟域。
        LARGE_INTEGER frequency{}, qpc{};
        if (!QueryPerformanceFrequency(&frequency) || !QueryPerformanceCounter(&qpc) || frequency.QuadPart <= 0) {
            fail("cannot calibrate system-audio performance clock");
        }
        qpcOffsetUs = av_gettime_relative() - av_rescale(qpc.QuadPart, 1000000, frequency.QuadPart);
        checkWindows(client->Start(), "start system-audio loopback");
        started = true;
    }

    std::optional<CapturedFrame> drainTail() {
        if (!hadPacket) return std::nullopt;
        const auto delayUs = av_rescale_q(swr_get_delay(resampler.get(), inputRate),
                                          AVRational{1, inputRate}, microseconds);
        const int capacity = swr_get_out_samples(resampler.get(), 0);
        checked(capacity, "calculate system-audio conversion tail capacity");
        if (!capacity) return std::nullopt;
        if (capacity > outputRate + 4096) fail("invalid system-audio conversion tail capacity");
        auto output = allocateAudioFrame(capacity, outputRate, AV_SAMPLE_FMT_FLT, outputLayout.value);
        const int produced = swr_convert(resampler.get(), output->extended_data, capacity, nullptr, 0);
        checked(produced, "flush system-audio PCM conversion");
        if (!produced) return std::nullopt;
        const auto timestampUs = fallbackNextUs - delayUs;
        output->nb_samples = produced;
        output->time_base = outputTimeBase;
        output->pts = av_rescale_q(timestampUs, microseconds, outputTimeBase);
        output->duration = produced;
        return CapturedFrame{std::move(output), Kind::Audio, outputTimeBase, timestampUs, av_gettime_relative()};
    }

    std::optional<CapturedFrame> readFrame(const std::atomic<bool>& stop) {
        if (!started || !capture) fail("system-audio input is not open");
        if (owner != std::this_thread::get_id()) fail("system-audio input must be read on its owning thread");
        while (!stop.load()) {
            UINT32 available = 0;
            checkWindows(capture->GetNextPacketSize(&available), "query system-audio capture packet");
            if (!available) {
                // 没有播放声音时可能完全没有回采包；这是正常空闲，不是断线或读超时。
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            BYTE* data = nullptr;
            UINT32 samples = 0;
            DWORD flags = 0;
            UINT64 position = 0, qpcPosition = 0;
            const auto result = capture->GetBuffer(&data, &samples, &flags, &position, &qpcPosition);
            if (result == AUDCLNT_S_BUFFER_EMPTY) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            checkWindows(result, "read system-audio capture packet");
            CaptureBuffer buffer{capture.get(), samples};
            if (!samples) {
                buffer.release();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            if (samples > static_cast<UINT32>(inputRate)) fail("system-audio packet exceeds one second");
            auto input = allocateAudioFrame(static_cast<int>(samples), inputRate, inputFormat, inputLayout.value);
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                checked(av_samples_set_silence(input->extended_data, 0, input->nb_samples, inputChannels,
                                               inputFormat), "fill silent system-audio packet");
            } else {
                if (!data) fail("system-audio packet has no PCM data");
                if (packed24) {
                    // Windows 的 packed signed 24-bit 先无符号拼接，再高位对齐到 S32，
                    // 避免负数左移的未定义行为，并保留与 libswresample 相符的幅值。
                    auto* destination = reinterpret_cast<std::uint32_t*>(input->data[0]);
                    const auto count = static_cast<std::size_t>(samples) * inputChannels;
                    for (std::size_t i = 0; i < count; ++i) {
                        destination[i] = (static_cast<std::uint32_t>(data[i * 3]) << 8) |
                                         (static_cast<std::uint32_t>(data[i * 3 + 1]) << 16) |
                                         (static_cast<std::uint32_t>(data[i * 3 + 2]) << 24);
                    }
                } else {
                    std::memcpy(input->data[0], data, static_cast<std::size_t>(samples) * bytesPerFrame);
                }
            }
            buffer.release();
            if (hadPacket && (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY)) {
                // 设备真实中断后不让旧重采样延迟混入新时间片。
                swr_close(resampler.get());
                checked(swr_init(resampler.get()), "restart interrupted system-audio PCM conversion");
            }
            const auto nowUs = av_gettime_relative();
            std::int64_t packetUs = 0;
            if (!(flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) && qpcPosition > 0 &&
                qpcPosition / 10 <= static_cast<UINT64>(std::numeric_limits<std::int64_t>::max())) {
                packetUs = static_cast<std::int64_t>(qpcPosition / 10) + qpcOffsetUs;
            } else {
                const auto durationUs = av_rescale_q(samples, AVRational{1, inputRate}, microseconds);
                packetUs = hadPacket ? fallbackNextUs : nowUs - durationUs;
            }
            fallbackNextUs = packetUs + av_rescale_q(samples, AVRational{1, inputRate}, microseconds);
            hadPacket = true;
            const auto delayUs = av_rescale_q(swr_get_delay(resampler.get(), inputRate),
                                              AVRational{1, inputRate}, microseconds);
            const int capacity = swr_get_out_samples(resampler.get(), static_cast<int>(samples));
            checked(capacity, "calculate system-audio conversion capacity");
            if (capacity <= 0 || capacity > outputRate + 4096) fail("invalid system-audio conversion capacity");
            auto output = allocateAudioFrame(capacity, outputRate, AV_SAMPLE_FMT_FLT, outputLayout.value);
            const std::uint8_t* planes[]{input->data[0]};
            const int produced = swr_convert(resampler.get(), output->extended_data, capacity,
                                             planes, input->nb_samples);
            checked(produced, "convert system-audio PCM");
            if (!produced) continue;
            output->nb_samples = produced;
            output->time_base = outputTimeBase;
            output->pts = av_rescale_q(packetUs - delayUs, microseconds, outputTimeBase);
            output->duration = produced;
            return CapturedFrame{std::move(output), Kind::Audio, outputTimeBase, packetUs - delayUs, nowUs};
        }
        return drainTail();
    }

    void close() noexcept {
        if (client && started) client->Stop();
        started = false;
        capture.reset();
        client.reset();
        device.reset();
        enumerator.reset();
        resampler.reset();
        av_channel_layout_uninit(&inputLayout.value);
        av_channel_layout_uninit(&outputLayout.value);
        if (uninitializeCom) CoUninitialize();
        uninitializeCom = false;
        hadPacket = false;
        fallbackNextUs = 0;
        name.clear();
        owner = {};
    }
#else
    void open() { fail("system-audio loopback currently requires Windows WASAPI"); }
    std::optional<CapturedFrame> readFrame(const std::atomic<bool>&) {
        fail("system-audio loopback currently requires Windows WASAPI");
    }
    void close() noexcept {}
#endif
};

SystemAudioInput::SystemAudioInput() : impl_(std::make_unique<Impl>()) {}
SystemAudioInput::~SystemAudioInput() { close(); }

void SystemAudioInput::open() {
#ifdef _WIN32
    // 已有会话的重复 open 不应进入失败清理并关闭原会话。
    if (impl_->owner != std::thread::id{}) fail("system-audio input is already open");
#endif
    try { impl_->open(); }
    catch (...) { impl_->close(); throw; }
}

std::string SystemAudioInput::deviceName() const { return impl_->name; }

std::optional<CapturedFrame> SystemAudioInput::readFrame(const std::atomic<bool>& stop) {
    return impl_->readFrame(stop);
}

void SystemAudioInput::close() noexcept { impl_->close(); }

} // namespace lsl::capture
