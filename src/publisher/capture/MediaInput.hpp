#pragma once

#include "common/config/Config.hpp"
#include "common/media/FFmpegResources.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace lsl::capture {

enum class Kind { Video, Audio };
const char* kindName(Kind kind) noexcept;

struct DeviceInfo {
    Kind kind;
    std::string id;          // 可直接作为 devices.video/audio 配置值
    std::string description;
};

std::vector<DeviceInfo> listDevices(Kind kind);
// 设备能力打印到 stderr。Windows 使用 dshow list_options；Linux 视频使用 V4L2。
void printDeviceOptions(Kind kind, const std::string& device);

struct CapturedFrame {
    media::FramePtr frame;
    Kind kind;
    AVRational timeBase;             // 原始 frame->pts / best_effort_timestamp 的时间基
    std::int64_t timestampUs = 0;     // 设备：单调采集时钟；文件：共用起点且跨循环连续
    std::int64_t capturedAtUs = 0;    // 解码输出时的 av_gettime_relative()
};

// open/read/close 必须在同一线程执行；若析构时仍持有媒体资源，析构也须在该线程。
// 每个输入独占解复用和解码上下文。
// 其他线程只允许 requestStop()；它不会并发释放 FFmpeg 正在使用的资源。
// readFrame 返回独占帧，可移动交给下游；nullopt 表示文件 EOF 或主动停止。
class MediaInput {
public:
    MediaInput();
    ~MediaInput();
    MediaInput(const MediaInput&) = delete;
    MediaInput& operator=(const MediaInput&) = delete;

    void openDevice(Kind kind, const config::PublisherConfig& config);
    void openFile(const config::PublisherConfig& config);
    std::optional<CapturedFrame> readFrame();
    // 已打开的实际设备或文件，供启动日志明确显示音源。
    std::string sourceDescription() const;
    void requestStop() noexcept;
    void close() noexcept; // 幂等，close 后可再次 open；已打开时再次 open 会报错

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace lsl::capture
