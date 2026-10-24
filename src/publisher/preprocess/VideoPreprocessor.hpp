#pragma once

#include "common/media/FFmpegResources.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace lsl::preprocess {

struct VideoPreprocessOptions {
    int width = 1280;
    int height = 720;
    int fps = 25;
    std::string scaleAlgorithm = "bilinear"; // nearest / bilinear / bicubic / lanczos
    std::string resizeMode = "fit";          // fit 留黑 / crop 居中裁剪 / stretch
    int rotation = 0;                        // 显式顺时针 0 / 90 / 180 / 270；不自动应用输入 display matrix
    bool mirror = false;                     // 旋转后的画面水平镜像
    std::string watermark;                   // 可打印 ASCII，最多 80 字符
    bool timestampOverlay = false;           // 叠加共用时间轴上的直播时长
};

struct VideoPreprocessStats {
    std::uint64_t inputFrames = 0;
    std::uint64_t outputFrames = 0;
    std::uint64_t droppedFrames = 0;
    std::uint64_t duplicatedFrames = 0;
    double inputFps = 0.0;
    double outputFps = 0.0;
    // 暂停等大跳跃最多补一秒旧画面；叠时长时，单批输出像素额外限 64 MiB
    // （至少允许当前帧一张）。其余槽保留 PTS 空隙并在此计数。
    std::uint64_t skippedTimelineFrames = 0;
};

// 单线程使用。输出为指定尺寸、方形像素的 YUV420P；PTS / duration 使用 1/fps。
// originUs 必须与音频预处理共用；第一帧保留距 originUs 的偏移，不补前导黑帧。
// 输入缓冲通过 FFmpeg 引用计数持有，调用者再次写入前仍须 make_writable。
// 输出清除输入 display matrix；尺寸变化、裁剪、旋转、镜像后清除依赖几何的 side data。
class VideoPreprocessor {
public:
    explicit VideoPreprocessor(VideoPreprocessOptions options, std::int64_t originUs = 0);
    ~VideoPreprocessor();
    VideoPreprocessor(const VideoPreprocessor&) = delete;
    VideoPreprocessor& operator=(const VideoPreprocessor&) = delete;

    std::vector<media::FramePtr> process(const AVFrame& frame, std::int64_t timestampUs);
    // 无未知时长的尾部补帧。flush 幂等；flush 后 process 报错。
    std::vector<media::FramePtr> flush();
    VideoPreprocessStats stats() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace lsl::preprocess
