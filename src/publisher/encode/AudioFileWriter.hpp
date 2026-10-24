#pragma once

#include "common/media/FFmpegResources.hpp"

#include <string>

namespace lsl::encode {

// M06 的单路 AAC 本地输出；M07 再处理音视频交织和网络推流。
// 单线程使用，接收独占 AVPacket；编码器必须启用 AV_CODEC_FLAG_GLOBAL_HEADER。
class AudioFileWriter {
public:
    // 路径使用 UTF-8；自动创建父目录，拒绝覆盖已有文件。
    AudioFileWriter(std::string path, const AVCodecParameters& parameters,
                    AVRational encoderTimeBase);
    ~AudioFileWriter() noexcept;
    AudioFileWriter(const AudioFileWriter&) = delete;
    AudioFileWriter& operator=(const AudioFileWriter&) = delete;

    // 转换 PTS/DTS/duration 后写入 M4A，保留编码延迟对应的负时间戳和包 side data。
    void write(media::PacketPtr packet);
    // 编码器 flush 的剩余包全部写入后显式调用；幂等，检查写尾及关闭错误。
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

} // namespace lsl::encode
