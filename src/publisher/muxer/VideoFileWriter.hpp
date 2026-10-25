#pragma once

#include "common/media/FFmpegResources.hpp"

#include <string>

namespace lsl::muxer {

// M05 的单路 H.264 本地输出；不承担 M07 的音视频交织或网络推流职责。
// 单线程使用，接收独占 AVPacket；编码器必须启用 AV_CODEC_FLAG_GLOBAL_HEADER。
class VideoFileWriter {
public:
    // 路径使用 UTF-8；自动创建父目录，拒绝覆盖已有文件。
    // M05 编码器时间基为 1/fps，因此以其倒数标记恒定帧率。
    VideoFileWriter(std::string path, const AVCodecParameters& parameters,
                    AVRational encoderTimeBase);
    ~VideoFileWriter() noexcept;
    VideoFileWriter(const VideoFileWriter&) = delete;
    VideoFileWriter& operator=(const VideoFileWriter&) = delete;

    // 转换 PTS/DTS/duration 后写入 MP4；包所有权在此消费，关键帧标志保持不变。
    void write(media::PacketPtr packet);
    // 显式写尾并检查刷新/关闭错误；幂等，结束后不能继续 write。
    // 应在编码器 flush 的剩余包全部写入后调用。
    void finish();
    const std::string& path() const noexcept { return path_; }

private:
    std::string path_;
    AVRational encoderTimeBase_;
    media::OutputFormatPtr context_;
    AVStream* stream_ = nullptr; // 由 context_ 拥有。
    bool finished_ = false;
    bool failed_ = false;
};

} // namespace lsl::muxer
