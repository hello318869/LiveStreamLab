#include "common/stats/CaptureTimestamp.hpp"

extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include <libavutil/time.h>
}

#include <array>
#include <cstddef>
#include <cstring>
#include <limits>

namespace lsl::stats {
namespace {

// 本 UUID 定义 v1 载荷格式；未来格式变更使用不同 UUID，不借用 x264 的版本 SEI。
constexpr std::array<std::uint8_t, 16> captureUuid{
    0x5a, 0x66, 0x7b, 0xf2, 0x43, 0x9c, 0x4e, 0x37,
    0x9f, 0x9a, 0x23, 0xc8, 0x11, 0x6b, 0xa4, 0x01};
constexpr std::size_t payloadSize = captureUuid.size() + sizeof(std::uint64_t);

bool hasCaptureUuid(const AVFrameSideData* data) noexcept {
    return data && data->type == AV_FRAME_DATA_SEI_UNREGISTERED && data->data &&
        data->size >= captureUuid.size() &&
        std::memcmp(data->data, captureUuid.data(), captureUuid.size()) == 0;
}

AVBufferRef* timestampBuffer(std::uint64_t timestamp) noexcept {
    auto* buffer = av_buffer_alloc(payloadSize);
    if (!buffer) return nullptr;
    std::memcpy(buffer->data, captureUuid.data(), captureUuid.size());
    for (std::size_t i = 0; i < sizeof(timestamp); ++i) {
        buffer->data[captureUuid.size() + i] =
            static_cast<std::uint8_t>(timestamp >> (8 * (sizeof(timestamp) - 1 - i)));
    }
    return buffer;
}

} // namespace

bool stampCaptureTime(AVFrame& frame) noexcept {
    const auto now = av_gettime();
    if (now <= 0) return false;
    bool replaced = false;
    for (int i = 0; i < frame.nb_side_data; ++i) {
        auto* data = frame.side_data[i];
        if (!hasCaptureUuid(data)) continue;
        // av_frame_ref/copy_props 会共享 side-data 的底层缓冲；使用独立新缓冲，
        // 不能直接覆盖 data->data，否则会污染解码器参考帧或已有帧副本。
        auto* buffer = timestampBuffer(static_cast<std::uint64_t>(now));
        if (!buffer) return false;
        av_buffer_unref(&data->buf);
        data->buf = buffer;
        data->data = buffer->data;
        data->size = buffer->size;
        replaced = true;
    }
    if (replaced) return true;
    auto* buffer = timestampBuffer(static_cast<std::uint64_t>(now));
    if (!buffer) return false;
    if (!av_frame_new_side_data_from_buf(&frame, AV_FRAME_DATA_SEI_UNREGISTERED, buffer)) {
        av_buffer_unref(&buffer);
        return false;
    }
    return true;
}

std::int64_t captureTimeUs(const AVFrame& frame) noexcept {
    for (int i = 0; i < frame.nb_side_data; ++i) {
        const auto* data = frame.side_data[i];
        if (!hasCaptureUuid(data) || data->size != payloadSize) continue;
        std::uint64_t timestamp = 0;
        for (std::size_t byte = captureUuid.size(); byte < payloadSize; ++byte) {
            timestamp = (timestamp << 8) | data->data[byte];
        }
        if (timestamp != 0 && timestamp <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return static_cast<std::int64_t>(timestamp);
        }
    }
    return 0;
}

} // namespace lsl::stats
