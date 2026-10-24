#include "publisher/capture/DeviceManager.hpp"

extern "C" {
#include <libavdevice/avdevice.h>
}

#include <cerrno>
#include <algorithm>
#include <iostream>
#include <mutex>

#ifdef _WIN32
#include <windows.h>
#include <dshow.h>
#include <mmdeviceapi.h>
#endif

#ifdef __linux__
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace lsl::capture {
const char* kindName(Kind kind) noexcept { return kind == Kind::Video ? "video" : "audio"; }

#ifdef _WIN32
namespace {
void checkCom(HRESULT result, const char* operation) {
    if (FAILED(result)) throw error::AppError("capture", static_cast<int>(result), operation);
}

struct ComApartment {
    HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ComApartment() {
        if (result != RPC_E_CHANGED_MODE) checkCom(result, "initialize COM for device enumeration");
    }
    ~ComApartment() { if (SUCCEEDED(result)) CoUninitialize(); }
};
struct ComRelease { void operator()(IUnknown* ptr) const noexcept { ptr->Release(); } };
template <typename T> using ComPtr = std::unique_ptr<T, ComRelease>;
struct VariantOwner {
    VARIANT value{};
    ~VariantOwner() { VariantClear(&value); }
};
struct PropertyVariantOwner {
    PROPVARIANT value{};
    ~PropertyVariantOwner() { PropVariantClear(&value); }
};

std::string utf8(const wchar_t* value) {
    if (!value) return {};
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, nullptr, 0, nullptr, nullptr);
    if (!size) throw error::AppError("capture", error::Io, "invalid Unicode in capture device name");
    std::string text(static_cast<std::size_t>(size), '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, text.data(), size, nullptr, nullptr)) {
        throw error::AppError("capture", error::Io, "cannot convert capture device name to UTF-8");
    }
    text.pop_back();
    return text;
}

std::string propertyString(IPropertyStore* properties, const PROPERTYKEY& key) {
    PropertyVariantOwner property;
    if (FAILED(properties->GetValue(key, &property.value))) return {};
    if (property.value.vt == VT_LPWSTR) return utf8(property.value.pwszVal);
    if (property.value.vt == VT_BSTR) return utf8(property.value.bstrVal);
    return {};
}

std::string lowerAscii(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    }
    return text;
}

std::string windowsDefaultAudioDevice(const std::vector<DeviceInfo>& devices) {
    ComApartment apartment;
    IMMDeviceEnumerator* rawEnumerator = nullptr;
    checkCom(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
        __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&rawEnumerator)),
        "resolve Windows default audio input; specify --audio-device if unavailable");
    const ComPtr<IMMDeviceEnumerator> enumerator{rawEnumerator};
    IMMDevice* rawEndpoint = nullptr;
    checkCom(enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &rawEndpoint),
        "get Windows default audio input; specify --audio-device if unavailable");
    const ComPtr<IMMDevice> endpoint{rawEndpoint};
    IPropertyStore* rawProperties = nullptr;
    checkCom(endpoint->OpenPropertyStore(STGM_READ, &rawProperties),
        "read Windows default audio input properties; specify --audio-device if unavailable");
    const ComPtr<IPropertyStore> properties{rawProperties};
    // SDK 中 PKEY_AudioEndpoint_GUID / PKEY_Device_FriendlyName 的属性键。
    // 局部常量避免为仅仅读取属性额外链接 Propsys 或定义全局 SDK GUID。
    constexpr PROPERTYKEY endpointGuidKey{
        {0x1da5d803, 0xd492, 0x4edd, {0x8c, 0x23, 0xe0, 0xc0, 0xff, 0xee, 0x7f, 0x0e}}, 4};
    constexpr PROPERTYKEY friendlyNameKey{
        {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
    const auto guid = propertyString(properties.get(), endpointGuidKey);
    if (!guid.empty()) {
        // DirectShow 的设备 moniker 含 wave_{端点 GUID}，枚举先后不代表系统默认。
        const auto marker = "wave_" + lowerAscii(guid);
        const DeviceInfo* match = nullptr;
        for (const auto& device : devices) {
            if (lowerAscii(device.id).find(marker) == std::string::npos) continue;
            if (match) throw error::AppError("capture", error::Io,
                "Windows default audio input matches multiple DirectShow devices; specify --audio-device");
            match = &device;
        }
        if (match) return match->id;
    }
    // 部分驱动的 moniker 不提供端点 GUID；仅当友好名唯一时才允许回退。
    const auto friendlyName = propertyString(properties.get(), friendlyNameKey);
    if (!friendlyName.empty()) {
        const DeviceInfo* match = nullptr;
        for (const auto& device : devices) {
            if (device.description != friendlyName) continue;
            if (match) throw error::AppError("capture", error::Io,
                "Windows default audio input name is ambiguous in DirectShow; specify --audio-device");
            match = &device;
        }
        if (match) return match->id;
    }
    throw error::AppError("capture", error::Io,
        "cannot match Windows default audio input to DirectShow; specify --audio-device");
}

std::vector<DeviceInfo> windowsDevices(Kind kind) {
    // 分别枚举两个类别，避免 FFmpeg dshow 在另一类别不存在时丢弃整个列表。
    ComApartment apartment;
    ICreateDevEnum* rawDevices = nullptr;
    checkCom(CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER,
        IID_ICreateDevEnum, reinterpret_cast<void**>(&rawDevices)), "create DirectShow device enumerator");
    const ComPtr<ICreateDevEnum> devices{rawDevices};
    IEnumMoniker* rawList = nullptr;
    const auto& category = kind == Kind::Video ? CLSID_VideoInputDeviceCategory : CLSID_AudioInputDeviceCategory;
    const auto result = devices->CreateClassEnumerator(category, &rawList, 0);
    const ComPtr<IEnumMoniker> list{rawList};
    if (result == S_FALSE) return {};
    checkCom(result, "enumerate DirectShow device category");
    std::vector<DeviceInfo> output;
    for (;;) {
        IMoniker* rawMoniker = nullptr;
        const auto next = list->Next(1, &rawMoniker, nullptr);
        const ComPtr<IMoniker> moniker{rawMoniker};
        if (next == S_FALSE) break;
        checkCom(next, "read capture device moniker");
        IBindCtx* rawBinding = nullptr;
        checkCom(CreateBindCtx(0, &rawBinding), "create device binding context");
        const ComPtr<IBindCtx> binding{rawBinding};
        wchar_t* rawName = nullptr;
        const auto named = moniker->GetDisplayName(binding.get(), nullptr, &rawName);
        const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> name{rawName, CoTaskMemFree};
        checkCom(named, "read capture device unique name");
        auto id = utf8(name.get());
        // 与 FFmpeg dshow 的唯一设备名规则一致。
        std::replace(id.begin(), id.end(), ':', '_');
        std::string description = id;
        IPropertyBag* rawBag = nullptr;
        const auto bound = moniker->BindToStorage(nullptr, nullptr, IID_IPropertyBag, reinterpret_cast<void**>(&rawBag));
        const ComPtr<IPropertyBag> bag{rawBag};
        if (SUCCEEDED(bound)) {
            VariantOwner friendly;
            if (SUCCEEDED(bag->Read(L"FriendlyName", &friendly.value, nullptr)) && friendly.value.vt == VT_BSTR) {
                description = utf8(friendly.value.bstrVal);
            }
        }
        output.push_back({kind, std::move(id), std::move(description)});
    }
    return output;
}
} // namespace
#endif

namespace detail {
void registerDevices() {
    static std::once_flag once;
    std::call_once(once, [] { avdevice_register_all(); });
}

const AVInputFormat* deviceFormat(Kind kind) {
    registerDevices();
    const char* name = nullptr;
#ifdef _WIN32
    static_cast<void>(kind);
    name = "dshow";
#elif defined(__linux__)
    name = kind == Kind::Video ? "video4linux2" : "alsa";
#else
    static_cast<void>(kind);
    throw error::AppError("capture", error::NotImplemented, "device capture supports Windows and Linux");
#endif
    const auto* format = av_find_input_format(name);
    if (!format) throw error::AppError("capture", error::NotImplemented,
        std::string("FFmpeg was built without input device: ") + name);
    return format;
}

std::string deviceUrl(Kind kind, const std::string& name) {
    std::string id = name;
#ifdef __linux__
    // ALSA 的 default 由系统配置决定，无需枚举即可打开。
    if (kind == Kind::Audio) return id;
#endif
    if (id == "default") {
        const auto devices = listDevices(kind);
        if (devices.empty()) throw error::AppError("capture", error::Io,
            std::string("no ") + kindName(kind) + " capture device found");
#ifdef _WIN32
        id = kind == Kind::Audio ? windowsDefaultAudioDevice(devices) : devices.front().id;
#else
        id = devices.front().id;
#endif
    }
    // 显式 ID/友好名称直接交给驱动，避免枚举不完整时拒绝有效设备。
#ifdef _WIN32
    return std::string(kindName(kind)) + "=" + id;
#else
    return id;
#endif
}
} // namespace detail

std::vector<DeviceInfo> listDevices(Kind kind) {
#ifdef _WIN32
    return windowsDevices(kind);
#else
    const auto* format = detail::deviceFormat(kind);
    AVDeviceInfoList* raw = nullptr;
    const int result = avdevice_list_input_sources(format, nullptr, nullptr, &raw);
    const std::unique_ptr<AVDeviceInfoList, media::detail::IndirectDeleter<avdevice_free_list_devices>> list{raw};
    error::throwIfFFmpegFailed(result, "capture", "enumerate capture devices");
    std::vector<DeviceInfo> devices;
    if (!list) return devices;
    const auto type = kind == Kind::Video ? AVMEDIA_TYPE_VIDEO : AVMEDIA_TYPE_AUDIO;
    for (int i = 0; i < list->nb_devices; ++i) {
        const auto* device = list->devices[i];
        bool matches = false;
        for (int j = 0; j < device->nb_media_types; ++j) matches |= device->media_types[j] == type;
        // Linux 的后端各自只返回一种设备，旧后端可能不填 media_types。
        matches |= device->nb_media_types == 0;
        if (matches && device->device_name) {
            devices.push_back({kind, device->device_name,
                device->device_description ? device->device_description : device->device_name});
        }
    }
    return devices;
#endif
}

void printDeviceOptions(Kind kind, const std::string& device) {
    const auto* format = detail::deviceFormat(kind);
    const auto url = detail::deviceUrl(kind, device);
#ifdef _WIN32
    AVDictionary* rawOptions = nullptr;
    error::throwIfFFmpegFailed(av_dict_set(&rawOptions, "list_options", "true", 0), "capture", "set list_options");
    AVFormatContext* raw = nullptr;
    const int result = avformat_open_input(&raw, url.c_str(), format, &rawOptions);
    const media::DictionaryPtr options{rawOptions};
    const media::InputFormatPtr input{raw};
    // list_options 的预期结束码是 AVERROR_EXIT，并不代表采集失败。
    if (result != AVERROR_EXIT) error::throwIfFFmpegFailed(result, "capture", "query device capabilities");
#elif defined(__linux__)
    static_cast<void>(format);
    if (kind == Kind::Audio) {
        std::cerr << "ALSA negotiates sample rate/channels on open; the first captured frame reports the actual PCM format.\n";
        return;
    }
    const int fd = ::open(url.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd < 0) throw error::FFmpegError("capture", AVERROR(errno), "open V4L2 device for capability query");
    struct FdOwner { int fd; ~FdOwner() { ::close(fd); } } owner{fd};
    v4l2_capability capability{};
    if (ioctl(fd, VIDIOC_QUERYCAP, &capability) < 0) {
        throw error::FFmpegError("capture", AVERROR(errno), "query V4L2 capabilities");
    }
    const auto caps = capability.capabilities & V4L2_CAP_DEVICE_CAPS
        ? capability.device_caps : capability.capabilities;
    v4l2_fmtdesc pixel{};
    pixel.type = caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE
        ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE : V4L2_BUF_TYPE_VIDEO_CAPTURE;
    for (; ioctl(fd, VIDIOC_ENUM_FMT, &pixel) == 0; ++pixel.index) {
        std::cerr << reinterpret_cast<const char*>(pixel.description) << '\n';
        v4l2_frmsizeenum size{};
        size.pixel_format = pixel.pixelformat;
        for (; ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &size) == 0; ++size.index) {
            if (size.type != V4L2_FRMSIZE_TYPE_DISCRETE) {
                std::cerr << "  size " << size.stepwise.min_width << 'x' << size.stepwise.min_height
                    << " .. " << size.stepwise.max_width << 'x' << size.stepwise.max_height
                    << " step " << size.stepwise.step_width << 'x' << size.stepwise.step_height << '\n';
                continue;
            }
            std::cerr << "  " << size.discrete.width << 'x' << size.discrete.height;
            v4l2_frmivalenum interval{};
            interval.pixel_format = pixel.pixelformat;
            interval.width = size.discrete.width;
            interval.height = size.discrete.height;
            for (; ioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &interval) == 0; ++interval.index) {
                if (interval.type == V4L2_FRMIVAL_TYPE_DISCRETE && interval.discrete.numerator) {
                    std::cerr << " " << static_cast<double>(interval.discrete.denominator) / interval.discrete.numerator << "fps";
                } else if (interval.type != V4L2_FRMIVAL_TYPE_DISCRETE) {
                    std::cerr << " interval " << interval.stepwise.min.numerator << '/' << interval.stepwise.min.denominator
                        << " .. " << interval.stepwise.max.numerator << '/' << interval.stepwise.max.denominator << "s";
                }
            }
            std::cerr << '\n';
        }
    }
#else
    static_cast<void>(url);
#endif
}
} // namespace lsl::capture
