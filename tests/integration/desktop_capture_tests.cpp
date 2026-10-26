#include "publisher/capture/MediaInput.hpp"
#include "publisher/pipeline/PublisherPipeline.hpp"

extern "C" {
#include <libavdevice/avdevice.h>
}

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>

namespace {
using Clock = std::chrono::steady_clock;
using BitBltFunction = BOOL(WINAPI*)(HDC, int, int, int, int, HDC, int, int, DWORD);

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// Only this test process's avdevice import slot changes. The SDK DLL on disk,
// system GDI functions, and other processes are unaffected.
class BitBltFailures {
public:
    BitBltFailures() {
        const auto name = "avdevice-" + std::to_string(avdevice_version() >> 16) + ".dll";
        const auto module = GetModuleHandleA(name.c_str());
        require(module != nullptr, "loaded avdevice DLL was not found");
        auto* base = reinterpret_cast<unsigned char*>(module);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        require(dos->e_magic == IMAGE_DOS_SIGNATURE, "invalid avdevice DOS header");
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        require(nt->Signature == IMAGE_NT_SIGNATURE, "invalid avdevice PE header");
        const auto directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        require(directory.VirtualAddress != 0, "avdevice has no import directory");
        auto* imported = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
        for (; imported->Name && !slot_; ++imported) {
            auto* addresses = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imported->FirstThunk);
            if (!imported->OriginalFirstThunk) continue;
            const auto* names = reinterpret_cast<const IMAGE_THUNK_DATA*>(base + imported->OriginalFirstThunk);
            for (std::size_t index = 0; names[index].u1.AddressOfData; ++index) {
                if (IMAGE_SNAP_BY_ORDINAL(names[index].u1.Ordinal)) continue;
                const auto* symbol = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(
                    base + names[index].u1.AddressOfData);
                if (std::strcmp(reinterpret_cast<const char*>(symbol->Name), "BitBlt") == 0) {
                    slot_ = reinterpret_cast<void**>(&addresses[index].u1.Function);
                    break;
                }
            }
        }
        require(slot_ != nullptr, "avdevice does not import BitBlt; failure injection unavailable");
        original_ = reinterpret_cast<BitBltFunction>(*slot_);
        require(original_ != nullptr, "avdevice BitBlt import is empty");
        DWORD previous = 0;
        require(VirtualProtect(slot_, sizeof(*slot_), PAGE_READWRITE, &previous) != FALSE,
            "cannot make test import slot writable");
        InterlockedExchangePointer(slot_, reinterpret_cast<void*>(&invoke));
        DWORD ignored = 0;
        VirtualProtect(slot_, sizeof(*slot_), previous, &ignored);
    }

    ~BitBltFailures() {
        remaining_.store(0);
        DWORD previous = 0;
        if (slot_ && VirtualProtect(slot_, sizeof(*slot_), PAGE_READWRITE, &previous)) {
            InterlockedExchangePointer(slot_, reinterpret_cast<void*>(original_));
            DWORD ignored = 0;
            VirtualProtect(slot_, sizeof(*slot_), previous, &ignored);
        }
    }

    BitBltFailures(const BitBltFailures&) = delete;
    BitBltFailures& operator=(const BitBltFailures&) = delete;

    void failNext(int count) {
        injected_.store(0);
        remaining_.store(count); // -1 means every capture fails until disabled.
    }
    int injected() const { return injected_.load(); }

private:
    static BOOL WINAPI invoke(HDC destination, int x, int y, int width, int height,
        HDC source, int sourceX, int sourceY, DWORD operation) {
        const auto remaining = remaining_.load();
        if (remaining != 0) {
            if (remaining > 0) remaining_.fetch_sub(1);
            injected_.fetch_add(1);
            SetLastError(ERROR_ACCESS_DENIED); // gdigrab reports "Failed to capture image (error 5)".
            return FALSE;
        }
        return original_(destination, x, y, width, height, source, sourceX, sourceY, operation);
    }

    inline static BitBltFunction original_ = nullptr;
    inline static std::atomic<int> remaining_{0};
    inline static std::atomic<int> injected_{0};
    void** slot_ = nullptr;
};

lsl::config::PublisherConfig desktopConfig(int timeoutMs) {
    lsl::config::PublisherConfig options;
    options.capture.videoSource = "desktop";
    options.capture.audio = false;
    options.capture.timeoutMs = timeoutMs;
    options.videoFile.clear();
    options.audioFile.clear();
    return options;
}

std::int64_t frameTimestamp(lsl::capture::MediaInput& input) {
    const auto captured = input.readFrame();
    require(captured.has_value(), "desktop capture ended unexpectedly");
    require(captured->kind == lsl::capture::Kind::Video && captured->frame &&
        captured->frame->width > 0 && captured->frame->height > 0,
        "desktop capture returned an invalid video frame");
    return captured->timestampUs;
}

void temporaryFailure(BitBltFailures& failure) {
    failure.failNext(0);
    lsl::capture::MediaInput input;
    input.openDevice(lsl::capture::Kind::Video, desktopConfig(2000));
    auto previous = frameTimestamp(input);
    failure.failNext(3);
    auto current = frameTimestamp(input);
    require(failure.injected() == 3, "temporary capture errors were not exercised");
    require(current > previous, "recovered capture timestamp did not advance");
    previous = current;
    for (int index = 0; index < 5; ++index) {
        current = frameTimestamp(input);
        require(current > previous, "desktop timestamps are not monotonic after recovery");
        previous = current;
    }
    input.close();
    std::cout << "PASS temporary BitBlt error 5 and monotonic recovered frames\n";
}

void permanentFailure(BitBltFailures& failure) {
    failure.failNext(0);
    lsl::capture::MediaInput input;
    input.openDevice(lsl::capture::Kind::Video, desktopConfig(400));
    static_cast<void>(frameTimestamp(input));
    failure.failNext(-1);
    const auto started = Clock::now();
    bool timedOut = false;
    try {
        static_cast<void>(input.readFrame());
    } catch (const lsl::error::FFmpegError& problem) {
        require(problem.module == "capture" && problem.code == AVERROR(ETIMEDOUT),
            "persistent desktop failure did not report a capture timeout");
        timedOut = true;
    }
    const auto elapsed = Clock::now() - started;
    failure.failNext(0);
    input.close();
    require(timedOut, "persistent desktop failure unexpectedly returned a frame");
    require(elapsed >= std::chrono::milliseconds(300) && elapsed < std::chrono::milliseconds(1500),
        "desktop recovery restarted or exceeded the original read timeout budget");
    std::cout << "PASS persistent BitBlt failure respects the read timeout\n";
}

void cancellation(BitBltFailures& failure) {
    failure.failNext(0);
    lsl::capture::MediaInput input;
    input.openDevice(lsl::capture::Kind::Video, desktopConfig(3000));
    static_cast<void>(frameTimestamp(input));
    failure.failNext(-1);
    std::atomic<bool> returned{false};
    bool stopSent = false;
    Clock::time_point stoppedAt;
    std::thread stop([&] {
        const auto limit = Clock::now() + std::chrono::seconds(2);
        while (!returned.load() && !failure.injected() && Clock::now() < limit) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (returned.load()) return;
        // Stop while the recovery path is between failed capture attempts.
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        stoppedAt = Clock::now();
        stopSent = true;
        input.requestStop(); // This is the only cross-thread MediaInput operation.
    });
    std::optional<lsl::capture::CapturedFrame> result;
    std::exception_ptr problem;
    try { result = input.readFrame(); }
    catch (...) { problem = std::current_exception(); }
    const auto ended = Clock::now();
    returned.store(true);
    stop.join();
    failure.failNext(0);
    input.close();
    if (problem) std::rethrow_exception(problem);
    require(stopSent && !result, "cancelled desktop recovery did not stop normally");
    require(ended - stoppedAt < std::chrono::milliseconds(500), "desktop recovery cancellation was not prompt");
    std::cout << "PASS recovery wait can be cancelled\n";
}

void publisherPipeline(BitBltFailures& failure, int seconds) {
    failure.failNext(0);
    auto options = desktopConfig(2000);
    options.capture.audio = true;
    options.capture.audioSource = "system";
    lsl::logging::Logger logger;
    std::atomic<bool> returned{false};
    int signalResult = -1;
    // An early pipeline exit restores this fallback before the helper joins.
    // A simultaneous late raise must not invoke the process default handler.
    const auto previous = std::signal(SIGINT, [](int) {});
    require(previous != SIG_ERR, "cannot install pipeline test signal fallback");
    std::thread control;
    std::exception_ptr problem;
    int result = -1;
    try {
        control = std::thread([&] {
            const auto started = Clock::now();
            const auto waitUntil = [&](Clock::time_point end) {
                while (!returned.load() && Clock::now() < end)
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                return !returned.load();
            };
            if (!waitUntil(started + std::chrono::milliseconds(300))) return;
            failure.failNext(3);
            if (waitUntil(started + std::chrono::seconds(seconds))) signalResult = std::raise(SIGINT);
        });
        result = lsl::pipeline::runPublisher(options, {}, logger);
    } catch (...) { problem = std::current_exception(); }
    returned.store(true);
    if (control.joinable()) control.join();
    const auto injected = failure.injected();
    failure.failNext(0);
    std::signal(SIGINT, previous);
    if (problem) std::rethrow_exception(problem);
    require(result == 0 && signalResult == 0, "publisher pipeline did not stop normally on SIGINT");
    require(injected == 3, "publisher pipeline did not recover from three BitBlt failures");
    std::cout << "PASS desktop/system-audio publisher pipeline, " << seconds << " seconds\n";
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
        BitBltFailures failure;
        temporaryFailure(failure);
        permanentFailure(failure);
        cancellation(failure);
        publisherPipeline(failure, soakSeconds);
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
