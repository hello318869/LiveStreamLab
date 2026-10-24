#pragma once

#include "publisher/capture/MediaInput.hpp"

#include <atomic>
#include <memory>
#include <optional>
#include <string>

namespace lsl::capture {

// Windows 默认播放设备的 WASAPI shared-mode loopback。
// open/readFrame/close（包括尚未 close 的析构）必须在同一所属线程执行。
// 不修改系统默认设备、音量或静音状态；其他线程仅通过 stop 通知停止。
class SystemAudioInput {
public:
    SystemAudioInput();
    ~SystemAudioInput();
    SystemAudioInput(const SystemAudioInput&) = delete;
    SystemAudioInput& operator=(const SystemAudioInput&) = delete;

    void open();
    std::string deviceName() const;
    // 返回独占、引用计数的 48 kHz / 双声道 / packed float PCM。
    // 系统未播放声音时继续等待；stop 后先取出转换尾部，再返回 nullopt。
    std::optional<CapturedFrame> readFrame(const std::atomic<bool>& stop);
    void close() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace lsl::capture
