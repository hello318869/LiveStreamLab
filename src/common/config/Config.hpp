#pragma once

#include "common/logger/Logger.hpp"
#include "common/error/Error.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lsl::config {

using ConfigError = error::ConfigError;

struct LoggingConfig {
    logging::Level level = logging::Level::Info;
    bool console = true;
    std::string file; // UTF-8 路径；空字符串禁用文件输出。
};

struct VideoConfig {
    int width = 1280;
    int height = 720;
    int fps = 25;
    int bitrate = 2000000; // bit/s
};

struct VideoEncoderConfig {
    int gopSize = 50;                   // 关键帧间隔，单位为帧
    std::string preset = "veryfast";   // libx264 编码预设
    bool lowLatency = true;            // 禁用 B 帧并使用 zerolatency 调优
};

struct AudioConfig {
    int sampleRate = 48000; // Hz
    int channels = 2;
    int bitrate = 128000; // bit/s
};

struct CaptureConfig {
    std::string source = "devices"; // devices 或 file
    std::string videoSource = "desktop"; // 设备模式：Windows desktop 或可选 camera
    std::string audioSource = "microphone"; // 设备模式：microphone 或 Windows system 回采
    std::string file;              // UTF-8 本地媒体路径
    bool loop = false;
    bool realtime = true;          // 文件按原始时间戳节奏读取
    bool video = true;
    bool audio = true;
    int timeoutMs = 5000;          // 单次设备读包/打开的超时
    std::string videoFormat;       // 仅摄像头：空值让驱动协商；如 yuyv422、mjpeg
};

struct VideoPreprocessConfig {
    std::string scaleAlgorithm = "bilinear"; // nearest / bilinear / bicubic / lanczos
    std::string resizeMode = "fit";          // fit 留黑 / crop 居中裁剪 / stretch 拉伸
    int rotation = 0;                       // 顺时针 0 / 90 / 180 / 270 度
    bool mirror = false;                    // 旋转后水平镜像
    std::string watermark;                  // 内置点阵字体，可打印 ASCII
    bool timestampOverlay = false;          // 直播经过时间，不使用系统墙钟
};

struct AudioPreprocessConfig {
    std::string sampleFormat = "fltp"; // 预处理独立使用时的默认值；编码链路以编码器实际格式为准
    int frameSize = 1024;              // 每声道样本数；编码链路以编码器实际 frame_size 为准
    int discontinuityThresholdMs = 100;
    int gainDb = 0;                    // 软件 PCM 增益，-60..30 dB；不修改系统设备音量
};

struct PreprocessConfig {
    VideoPreprocessConfig video;
    AudioPreprocessConfig audio;
};

struct StreamingConfig {
    bool enabled = false;              // 显式启用 M07；原有本地录制默认行为保留。
    int connectTimeoutMs = 5000;
    int writeTimeoutMs = 5000;
    int reconnectAttempts = 5;          // 每次连接故障最多重试次数；0 禁用重连。
    int reconnectDelayMs = 1000;
    int queueCapacity = 256;            // 待复用编码包数量上限。
    int maxInterleaveDeltaMs = 500;     // 音视频交织的最大等待跨度。
};

struct PublisherConfig {
    std::string videoDevice = "default";
    std::string audioDevice = "default";
    std::string outputUrl = "rtmp://127.0.0.1/live/livestream"; // M07 推流地址
    std::string videoFile = "output/video.mp4"; // M05 本地 H.264 视频，无音频；空值禁用
    std::string audioFile = "output/audio.m4a"; // M06 本地 AAC 音频，无视频；空值禁用
    VideoConfig video;
    VideoEncoderConfig videoEncoder;
    AudioConfig audio;
    CaptureConfig capture;
    PreprocessConfig preprocess;
    StreamingConfig streaming;
    LoggingConfig logging;
};

struct DemuxConfig {
    int connectTimeoutMs = 5000;
    int readTimeoutMs = 5000;
    int reconnectAttempts = 5;          // 连续故障后的最多重试次数；0 禁用重连。
    int reconnectDelayMs = 1000;
    int queueCapacity = 256;            // 每个音视频包队列的数量上限。
    int maxBufferMs = 2000;             // 时间戳跨度与排队时长上限。
    int maxQueueBytes = 33554432;       // 两个队列合计的媒体数据字节上限。
    int discontinuityThresholdMs = 1000;
};

struct PlayerVideoConfig {
    int width = 1280;        // 初始窗口大小；不改变视频解码分辨率。
    int height = 720;
    bool vsync = true;
    bool fullscreen = false;
};

struct PlayerAudioConfig {
    bool enabled = true;
    int bufferMs = 500; // 应用 PCM 环形缓冲目标深度，至少容纳一个解码块。
    int volume = 100;   // 0..100；只影响此播放器，不改变系统音量。
    bool muted = false;
    bool paused = false; // 仅暂停音频；M13 音画同步尚未实现。
};

struct PlayerConfig {
    std::string inputUrl = "http://127.0.0.1:8080/live/livestream.flv";
    std::string audioDevice = "default";
    int sampleRate = 48000;
    int channels = 2;
    DemuxConfig demux;
    PlayerVideoConfig video;
    PlayerAudioConfig audioPlayback;
    LoggingConfig logging;
};

// 路径使用 JSON 的点分字段名；字符串为原始文本，整数为十进制文本。
// 同一字段重复覆盖时，最后一次生效。
using Overrides = std::vector<std::pair<std::string, std::string>>;

PublisherConfig parsePublisherConfig(std::string_view json, const Overrides& overrides = {});
PlayerConfig parsePlayerConfig(std::string_view json, const Overrides& overrides = {});
PublisherConfig loadPublisherConfig(
    const std::optional<std::filesystem::path>& file = std::nullopt,
    const Overrides& overrides = {});
PlayerConfig loadPlayerConfig(
    const std::optional<std::filesystem::path>& file = std::nullopt,
    const Overrides& overrides = {});

void validate(const PublisherConfig& config);
void validate(const PlayerConfig& config);
std::string toJson(const PublisherConfig& config);
std::string toJson(const PlayerConfig& config);

} // namespace lsl::config
