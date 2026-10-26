# M05 视频编码与自动保存

主播端将 M03 输出的 YUV420P 帧送入 H.264 编码器，默认自动保存到
`output/video.mp4`。目录不存在时自动创建；相对路径以程序启动时的工作目录为准。
从 `D:\直播\LiveStreamLab` 启动时，默认文件为
`D:\直播\LiveStreamLab\output\video.mp4`。

输出是只有视频轨的 MP4。M06 已将音频编码为 AAC，独立保存为 `output/audio.m4a`；
`output.url` / `--url` 由 M07 使用；设置 `output.streaming=true` / `--stream true` 后进行 FLV/RTMP 推流，
默认仍关闭推流。当前推流用法见 [M07](rtmp-streaming.md)。下文保留 M05 阶段的实现与验证记录。

## 参数

| JSON 字段 | 默认值 | 含义 |
|---|---|---|
| `output.video_file` | `output/video.mp4` | 本地 MP4 路径；空字符串禁用本地写出 |
| `video.width` / `video.height` | 1280 / 720 | 编码尺寸，沿用 M03 的目标尺寸 |
| `video.fps` | 25 | 帧率与编码时间基 `1/fps` |
| `video.bitrate` | 2000000 | 目标码率，bit/s；不保证每秒严格等于该值 |
| `video_encoder.gop_size` | 50 | 最大关键帧间隔，单位为帧 |
| `video_encoder.preset` | `veryfast` | x264 速度预设，`ultrafast` 至 `placebo` |
| `video_encoder.low_latency` | `true` | 禁用 B 帧，x264 使用 `zerolatency` |

优先选择 FFmpeg 中的 `libx264`，没有时使用 `libopenh264`；两者都没有则报告错误，
不自动选择硬件编码器。OpenH264 使用自身默认编码策略，不应用 x264 的 preset，也不启用 B 帧。
关闭低延迟后，x264 允许两帧 B 帧重排，停止时会排空缓存。
码率、帧率、GOP 和 preset 在新会话初始化时读取，不在运行中热切换。

程序拒绝覆盖已有输出文件。需要再次保存时，移动原文件或指定新名字，例如
`--video-file output/video-02.mp4`。文件输入和输出使用不同路径。
关闭视频输入或输入只有音频时，不创建空视频文件。

## 构建与使用

只使用 CMake，不需要构建脚本：

```powershell
cd D:\直播\LiveStreamLab
cmake -S . -B build/cmake -DLSL_BUILD_TESTS=OFF
cmake --build build/cmake --config Release --target publisher --parallel
```

本机完整构建的 SDL 预编译头遇到中文路径编码错误，已通过 CMake 缓存选项
`-DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON` 避开；未修改 SDL 源码或添加构建脚本。

以下为使用命令，本次未执行：

```powershell
# 沿用采集配置；视频自动写入 output/video.mp4，Ctrl+C 正常停止并写尾
.\build\cmake\bin\Release\publisher.exe --config config\publisher.json

# 本地文件经过 M02 解码、M03 预处理和 M05 重编码；EOF 后写尾退出
.\build\cmake\bin\Release\publisher.exe --source file --input-file "D:\media\sample.mp4" --capture-audio false --video-file output/file-video.mp4

# 调整目标码率、GOP、速度预设与低延迟策略
.\build\cmake\bin\Release\publisher.exe --video-file output/video-02.mp4 --video-bitrate 3000000 --fps 30 --gop-size 60 --preset fast --low-latency true
```

JSON 中的 `output.video_file` 设为 `""`，或使用 `--set output.video_file=`，
可只执行编码和统计。没有实际输入帧时不会创建文件。

## 接口、所有权与停止顺序

```text
采集线程：MediaInput → CapturedFrame → 原始视频有界队列
处理线程：VideoPreprocessor → VideoEncoder → 独占参数/PacketPtr → 输出事件有界队列
输出线程：VideoFileWriter → output/video.mp4（按配置另交给 RTMP 网络队列）
```

`lsl_video_encoder` 仅包含 `encoder/VideoEncoder`；`muxer/VideoFileWriter` 属于 `lsl_muxer`。
两者均链接 `lsl_common`，由 `pipeline/PublisherPipeline` 组合调用。
M14 中视频预处理器与编码器由视频处理线程独占，本地写出器由唯一输出线程独占，
采集、处理和输出之间通过有界队列移交媒体对象。实时输入优先在编码前丢旧原始帧；
输出队列满时等待容量，不任意删除本地录制的编码包。两条输出路径使用独立包引用，
不共享可变 AVPacket。容量和线程生命周期见 [多线程流水线](threading.md)。

- `encode(const AVFrame&)` 返回零个、一个或多个编码包。send 返回 EAGAIN 时先 receive，
  然后重送尚未被接收的同一帧；不能把 EAGAIN 当作丢帧。
- 输出包保留编码器的 PTS、DTS、duration 和关键帧标志；旧编码器未填 duration 时补一个帧槽。
  输出时间基为 `1/fps`。开启 B 帧时不能用显示顺序覆盖 DTS。
- `parameters()` 返回独占编码参数及 SPS/PPS。MP4 模式启用 global header；
  首个预处理帧的色彩描述用于初始化编码上下文与输出流。
- 写出器复制流参数，写头后按实际流时间基调用 `av_packet_rescale_ts()`，
  消费传入的 `PacketPtr`，保留关键帧标志。
- EOF / Ctrl+C：停止输入、关闭输入和原始帧队列 → 排空已入队视频 →
  flush 预处理及编码器至 EOF → 输出线程排空事件、写完 MP4 尾部并关闭 I/O。
  `flush()` 与 `finish()` 均可重复调用；结束后不能继续提交数据。
- 编码或写文件失败会经现有异常边界报告；析构只尽力收尾并释放资源，失败文件不视为完整输出。
  强制结束进程可能来不及写 MP4 尾部，应使用正常停止。

统计日志新增编码输入帧数、编码包数、编码字节数和关键帧包数，
均为本地编码统计，不代表网络发送量。

## M05 阶段完成边界（2026-10-24）

M05 实现已接入 CMake、主播端、配置与命令行。当时 Windows Release 完整构建通过，
包括当时的 `lsl_video_encode`、`publisher`、SDL3 与 `player` 的编译、链接。
按要求没有运行测试、采集或录像；视频可解码性、设备运行表现与其他平台兼容性尚未运行验收。
