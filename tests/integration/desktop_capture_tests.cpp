#include "publisher/capture/MediaInput.hpp"
#include "publisher/pipeline/PublisherPipeline.hpp"
#include "publisher/pipeline/PublisherSession.hpp"
#include "common/stats/CaptureTimestamp.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

extern "C" {
#include <libavfilter/avfilter.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/hwcontext.h>
#include <libavutil/rational.h>
}

namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct ComRelease {
    template <typename T>
    void operator()(T* value) const noexcept { value->Release(); }
};
template <typename T> using ComPtr = std::unique_ptr<T, ComRelease>;

void checkDxgi(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        throw std::runtime_error(std::string(operation) + " failed (HRESULT " +
            std::to_string(static_cast<unsigned long>(result)) + ")");
    }
}

// Probe the SDK and the interactive desktop separately from MediaInput, so a project
// graph, download or timing regression cannot be treated as a missing prerequisite.
std::string desktopUnavailable() {
    if (!avfilter_get_by_name("ddagrab")) return "FFmpeg was built without ddagrab";
    AVBufferRef* rawDevice = nullptr;
    const int result = av_hwdevice_ctx_create(&rawDevice, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0);
    lsl::media::BufferRefPtr device{rawDevice};
    if (result == AVERROR_UNKNOWN || result == AVERROR(ENODEV) || result == AVERROR(ENOSYS)) {
        return "the default D3D11VA device is unavailable";
    }
    lsl::error::throwIfFFmpegFailed(result, "desktop-test", "create default D3D11VA device");
    const auto* context = reinterpret_cast<const AVHWDeviceContext*>(device->data);
    const auto* hardware = static_cast<const AVD3D11VADeviceContext*>(context->hwctx);

    IDXGIDevice* rawDxgiDevice = nullptr;
    checkDxgi(hardware->device->QueryInterface(IID_PPV_ARGS(&rawDxgiDevice)), "query IDXGIDevice");
    const ComPtr<IDXGIDevice> dxgiDevice{rawDxgiDevice};
    IDXGIAdapter* rawAdapter = nullptr;
    checkDxgi(dxgiDevice->GetAdapter(&rawAdapter), "get DXGI adapter");
    const ComPtr<IDXGIAdapter> adapter{rawAdapter};

    IDXGIOutput* rawOutput = nullptr;
    const auto outputResult = adapter->EnumOutputs(0, &rawOutput);
    if (outputResult == DXGI_ERROR_NOT_FOUND || outputResult == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE) {
        return "DXGI output 0 is unavailable on the default D3D11VA device";
    }
    checkDxgi(outputResult, "enumerate DXGI output 0");
    const ComPtr<IDXGIOutput> output{rawOutput};

    IDXGIOutput1* rawDuplicationOutput = nullptr;
    const auto interfaceResult = output->QueryInterface(IID_PPV_ARGS(&rawDuplicationOutput));
    if (interfaceResult == E_NOINTERFACE) return "DXGI output 0 does not expose Desktop Duplication";
    checkDxgi(interfaceResult, "query IDXGIOutput1");
    const ComPtr<IDXGIOutput1> duplicationOutput{rawDuplicationOutput};

    IDXGIOutputDuplication* rawDuplication = nullptr;
    const auto duplicateResult = duplicationOutput->DuplicateOutput(hardware->device, &rawDuplication);
    const ComPtr<IDXGIOutputDuplication> duplication{rawDuplication};
    if (duplicateResult == DXGI_ERROR_UNSUPPORTED || duplicateResult == E_ACCESSDENIED ||
        duplicateResult == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE || duplicateResult == DXGI_ERROR_SESSION_DISCONNECTED) {
        return "Desktop Duplication is unavailable for this output or session (HRESULT " +
            std::to_string(static_cast<unsigned long>(duplicateResult)) + ")";
    }
    checkDxgi(duplicateResult, "duplicate DXGI output 0");
    require(static_cast<bool>(duplication), "DXGI duplication succeeded without a duplication interface");
    return {};
}

lsl::config::PublisherConfig desktopConfig() {
    lsl::config::PublisherConfig options;
    options.capture.videoSource = "desktop";
    options.capture.audio = false;
    options.capture.timeoutMs = 3000;
    options.video.width = 320;
    options.video.height = 240;
    options.videoFile.clear();
    options.audioFile.clear();
    return options;
}

std::int64_t frameTimestamp(lsl::capture::MediaInput& input) {
    const auto captured = input.readFrame();
    require(captured && captured->kind == lsl::capture::Kind::Video && captured->frame,
        "desktop capture did not return a video frame");
    const auto& frame = *captured->frame;
    require(frame.width > 0 && frame.height > 0 && frame.format == AV_PIX_FMT_BGRA &&
        !frame.hw_frames_ctx && frame.data[0] && frame.linesize[0] >= static_cast<std::int64_t>(frame.width) * 4,
        "desktop capture did not download a valid CPU BGRA frame");
    require(captured->timeBase.num > 0 && captured->timeBase.den > 0 &&
        av_cmp_q(captured->timeBase, frame.time_base) == 0,
        "desktop capture returned invalid time base metadata");
    require(captured->timestampUs > 0 && captured->capturedAtUs > 0 &&
        lsl::stats::captureTimeUs(frame) > 0, "desktop capture did not stamp capture times");
    return captured->timestampUs;
}

void captureAndReopen() {
    using lsl::capture::Kind;
    lsl::capture::MediaInput input;
    const auto options = desktopConfig();
    input.openDevice(Kind::Video, options);
    require(input.sourceDescription().find("ddagrab") != std::string::npos,
        "desktop input did not select ddagrab");
    require(input.hasTrack(Kind::Video) && !input.hasTrack(Kind::Audio), "desktop input exposed the wrong tracks");
    auto previous = frameTimestamp(input);
    for (int index = 0; index < 5; ++index) {
        const auto current = frameTimestamp(input);
        require(current > previous, "desktop capture timestamps did not increase");
        previous = current;
    }
    input.close();
    require(!input.hasTrack(Kind::Video), "closing desktop capture did not clear the video track");
    input.openDevice(Kind::Video, options);
    require(frameTimestamp(input) > previous, "reopened desktop capture did not use the monotonic clock");
    input.close();
    std::cout << "PASS ddagrab BGRA frames, capture timestamps and reopen\n";
}

void cancellation() {
    lsl::capture::MediaInput input;
    auto options = desktopConfig();
    // A one-second frame period keeps the next read pending when the stop arrives.
    options.video.fps = 1;
    input.openDevice(lsl::capture::Kind::Video, options);
    static_cast<void>(frameTimestamp(input));

    Clock::time_point stoppedAt;
    std::thread stop([&] {
        std::this_thread::sleep_for(25ms);
        stoppedAt = Clock::now();
        input.requestStop();
    });
    std::optional<lsl::capture::CapturedFrame> captured;
    std::exception_ptr problem;
    try { captured = input.readFrame(); }
    catch (...) { problem = std::current_exception(); }
    const auto ended = Clock::now();
    stop.join();
    input.close();
    if (problem) std::rethrow_exception(problem);
    require(!captured && ended >= stoppedAt && ended - stoppedAt < 500ms,
        "stopping a pending desktop read did not promptly return EOF");
    std::cout << "PASS ddagrab read cancellation\n";
}

void publisherPipeline(int seconds) {
    auto options = desktopConfig();
    lsl::logging::Logger logger;
    std::atomic<bool> stop{false};
    std::atomic<bool> validPreview{false};
    lsl::pipeline::PublisherPipelineStats final;
    const auto stopAt = Clock::now() + std::chrono::seconds(seconds);
    const int result = lsl::pipeline::runPublisherPipeline(options, logger, stop,
        [&](const lsl::pipeline::PublisherPipelineStats& stats) {
            final = stats;
            if (Clock::now() >= stopAt) stop.store(true);
        },
        [&](const std::shared_ptr<const lsl::pipeline::PublisherPreviewFrame>& frame) {
            if (frame && frame->width > 0 && frame->height > 0 && !frame->pixels.empty()) {
                validPreview.store(true);
            }
        });
    require(result == 0 && final.finished && final.encodedVideoFrames > 0 && validPreview.load(),
        "desktop publisher did not produce preview and encoded frames before stopping normally");
    std::cout << "PASS ddagrab publisher pipeline, " << seconds << " seconds\n";
}
} // namespace

int main(int argc, char* argv[]) {
    try {
        int soakSeconds = 2;
        if (argc != 1) {
            require(argc == 3 && std::strcmp(argv[1], "--soak-seconds") == 0,
                "usage: desktop_capture_tests [--soak-seconds N]");
            std::size_t consumed = 0;
            soakSeconds = std::stoi(argv[2], &consumed);
            require(argv[2][consumed] == '\0' && soakSeconds >= 2 && soakSeconds <= 7200,
                "soak seconds must be an integer within 2..7200");
        }
        const auto unavailable = desktopUnavailable();
        if (!unavailable.empty()) {
            std::cout << "SKIP desktop capture: " << unavailable << '\n';
            return 77;
        }
        captureAndReopen();
        cancellation();
        publisherPipeline(soakSeconds);
        return 0;
    } catch (const std::exception& problem) {
        std::cerr << "FAIL desktop capture: " << problem.what() << '\n';
        return 1;
    }
}
#else
int main() {
    std::cout << "Windows desktop capture test is unavailable on this platform\n";
    return 77;
}
#endif
