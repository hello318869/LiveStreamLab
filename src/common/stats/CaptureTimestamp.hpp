#pragma once

#include <cstdint>

struct AVFrame;

namespace lsl::stats {

// 设备视频解码输出、进入原始帧队列前调用。时间为 Unix 微秒，不是曝光时间或媒体 PTS。
// 写入本应用版本 UUID + 8 字节大端时间，共 24 字节 UDU SEI；失败不抛异常。
// 调用者独占 AVFrame 结构；共享的像素与其他帧的 side-data 缓冲不会被修改。
bool stampCaptureTime(AVFrame& frame) noexcept;

// 遍历全部 UDU SEI，只接受本 UUID、精确长度及正的 int64 时间；缺失/无效返回 0。
std::int64_t captureTimeUs(const AVFrame& frame) noexcept;

} // namespace lsl::stats
