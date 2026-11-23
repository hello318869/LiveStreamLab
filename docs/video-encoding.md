# H.264 编码与视频文件（M05）

`src/publisher/encoder/VideoEncoder` 将预处理后的 YUV420P 编码为 H.264；
`src/publisher/muxer/VideoFileWriter` 保存单视频轨 MP4。CLI 默认 `output/video.mp4`，
GUI 禁用本地文件；推流配置见 [RTMP 推流](rtmp-streaming.md)。构建统一见 [README](../README.md)。

## 参数

| JSON 字段 | 默认值 | 作用 |
|---|---|---|
| `output.video_file` | `output/video.mp4` | 空字符串禁用本地写出 |
| `video.width` / `height` | 1280 / 720 | 与预处理输出一致的偶数尺寸 |
| `video.fps` / `bitrate` | 25 / 2000000 | 时间基 `1/fps`；目标码率 bit/s，不保证每秒恒定 |
| `video_encoder.backend` | `software` | `software`、`nvenc` 或 `auto` |
| `video_encoder.gop_size` | 50 | 最大关键帧间隔，单位帧 |
| `video_encoder.preset` | `veryfast` | x264 预设，ultrafast 至 placebo |
| `video_encoder.low_latency` | true | 禁用 B 帧并启用所选编码器的低延迟设置 |

- `software` 优先 `libx264`，其次 `libopenh264`。x264 关闭低延迟时允许两帧重排，OpenH264 始终禁用 B 帧。
- `nvenc` 使用 FFmpeg 的 `h264_nvenc`。需要支持 NVENC 的 NVIDIA GPU、驱动以及包含该编码器的 FFmpeg。初始化失败时报告错误。
- `auto` 优先尝试 NVENC。初始化失败时记录原因并打开软件编码器，启动日志中的 `encoder=` 显示实际后端。

主播界面从「推流设置 → 输出设置 → 视频编码方式」选择。每次开播按所选方式初始化，单独预览走软件编码。NVENC 接收预处理输出的 CPU 内存 YUV420P 帧，采用 `p4` 预设，低延迟模式使用 `ll` 并减少输出缓冲。两种 NVENC 模式均禁用 B 帧。

## 接口与输出约定

```text
原始视频队列 → VideoPreprocessor → VideoEncoder::encode()
  → 输出事件队列 → VideoFileWriter::write() / RtmpPublisher::submitVideo()
```

- 首个预处理帧用于初始化编码器的色彩描述。`encode()` 返回零到多个独占 `PacketPtr`；
  send 遇 EAGAIN 时先 receive，再重送同一帧。
- 保留编码器 PTS/DTS/duration/关键帧标志；旧编码器缺 duration 时补一个帧槽，
  开 B 帧时不能用显示顺序覆盖 DTS。`parameters()` 返回独占参数及 SPS/PPS；MP4/推流启用 global header。
- 写出器复制参数，按写头后流时间基 rescale；包所有权与背压见 [多线程流水线](threading.md)。
- EOF/正常停止先排空原始帧和预处理，再 `VideoEncoder::flush()` 至 EOF，消费尾包后
  `VideoFileWriter::finish()` 写容器尾。二者幂等，结束后不能继续提交。

文件目录自动创建，相对路径基于启动工作目录；已有文件拒绝覆盖，输入输出路径必须不同。
无实际视频帧时不创建空文件。异常或强杀不能保证 MP4 完整写尾；析构仅尽力清理。
编码帧数、包数、字节数和关键帧数是编码统计，不代表网络发送量。

## 使用

以下命令从工程根目录运行：

```powershell
# 文件视频重编码；仅生成视频 MP4
.\build\cmake\bin\Release\livestreamlab.exe --mode publisher --cli --source file --input-file "D:/media/sample.mp4" --capture-audio false --video-file output/file-video.mp4
```

用 `--video-encoder nvenc` 选择硬件编码，或用 `--video-encoder auto` 启用自动回退。
用 `--video-bitrate`、`--fps`、`--gop-size`、`--preset` 和 `--low-latency` 调整编码参数。
用 `--set output.video_file=` 禁用本地写出，只编码和统计，或按配置推流。
音频独立文件见 [AAC 编码](audio-encoding.md)。
