# 配置管理

配置使用 UTF-8 JSON，可只写需要覆盖的字段。优先级：**内置默认值 → JSON 文件 → 命令行覆盖 → 校验**。相对配置、输入、输出及日志路径均相对启动工作目录。

## 入口与示例配置

统一程序为 `livestreamlab`。无参数打开直播大厅，`--mode publisher|player` 直达手填地址的工具，增加 `--cli` 运行控制台流水线。无 GUI 构建必须指定模式，自动使用 CLI。构建见 [项目 README](../README.md)。

| 配置来源 | 行为 |
|---|---|
| GUI 未指定 `--config` | 从程序旁 `config/` 加载 `publisher-stream.json` 或 `player.json` |
| CLI 未指定 `--config` | 使用结构体默认值；开播默认桌面、麦克风、本地 MP4/M4A，推流关闭 |
| `config/publisher.json` | 桌面 + 系统声音，本地录制 |
| `config/publisher-stream.json` | 桌面 + 系统声音，启用推流，关闭本地文件 |
| `config/publisher-file.json` | 文件循环推流 |
| `config/player.json` | HTTP-FLV 播放、重连与同步参数 |

`--help`、`--check-config`、`--print-config` 和设备查询直接返回，不进入 GUI，也不自动加载 GUI 默认 JSON。查看某个文件须显式指定 `--config`。

## 主播字段

下表默认值指结构体默认值，示例 JSON 可以覆盖它们。

| 字段 | 默认值 | 约束/作用 |
|---|---|---|
| `devices.video` / `devices.audio` | `default` | 非空设备名；仅摄像头/麦克风使用 |
| `capture.source` | `devices` | `devices` / `file` |
| `capture.video_source` | `desktop` | `desktop` / `camera`；桌面仅 Windows |
| `capture.audio_source` | `microphone` | `microphone` / `system`；系统回采仅 Windows |
| `capture.file` | `""` | 文件模式必填，UTF-8 本地路径 |
| `capture.loop` / `capture.realtime` | `false` / `true` | 文件循环 / 按媒体时间实时交付 |
| `capture.video` / `capture.audio` | `true` | 至少启用一种轨道 |
| `capture.timeout_ms` | `5000` | 100–60000 ms，采集 I/O 超时 |
| `capture.video_format` | `""` | 摄像头像素格式；空值由驱动协商 |
| `video.width` / `video.height` | `1280` / `720` | 偶数；2–7680 / 2–4320 |
| `video.fps` | `25` | 1–120 |
| `video.bitrate` | `2000000` | 64000–100000000 bit/s |
| `video_encoder.backend` | `software` | `software` 软件编码，`nvenc` 指定硬件编码，`auto` 优先 NVENC、初始化失败则回退软件编码 |
| `video_encoder.gop_size` | `50` | 1–1200 帧 |
| `video_encoder.preset` | `veryfast` | x264 预设，从 `ultrafast` 到 `placebo` |
| `video_encoder.low_latency` | `true` | 禁用 B 帧并启用所选编码器的低延迟设置 |
| `audio.sample_rate` / `audio.channels` | `48000` / `2` | AAC 标准采样率 / 1 或 2 声道 |
| `audio.bitrate` | `128000` | 16000–512000 bit/s |
| `output.url` | `rtmp://127.0.0.1/live/livestream` | RTMP/RTMPS 地址 |
| `output.streaming` | `false` | `--url` 本身不会开启推流 |
| `output.video_file` / `output.audio_file` | `output/video.mp4` / `output/audio.m4a` | 空字符串禁用；自动建目录，拒绝覆盖 |
| `output.queue_capacity` | `256` | 8–4096 个待发送编码包 |
| `output.max_interleave_delta_ms` | `500` | 1–5000 ms，音视频交织等待上限 |
| `pipeline.video_queue_capacity` | `8` | 1–64 个原始视频帧 |
| `pipeline.audio_queue_capacity` | `32` | 1–256 个原始音频块，满时背压 |
| `pipeline.packet_queue_capacity` | `256` | 8–4096 个编码输出事件 |
| `pipeline.max_video_age_ms` | `200` | 20–5000 ms，实时原始视频排队年龄 |

桌面采集 Windows 首个显示输出与鼠标，`video.width/height` 用于后续缩放。`camera` 使用摄像头设备名及格式。系统声音回采 Windows 默认播放端点。文件模式使用文件音视频轨，文件推流必须保留 `capture.realtime=true`。详见 [采集](capture.md)、[推流](rtmp-streaming.md)。

### 预处理

| 字段 | 默认值 | 约束/作用 |
|---|---|---|
| `preprocess.video.scale_algorithm` | `bilinear` | `nearest` / `bilinear` / `bicubic` / `lanczos` |
| `preprocess.video.resize_mode` | `fit` | 留黑 `fit` / 居中裁剪 `crop` / 拉伸 `stretch` |
| `preprocess.video.rotation` / `preprocess.video.mirror` | `0` / `false` | 顺时针 0/90/180/270 度；旋转后水平镜像 |
| `preprocess.video.watermark` | `""` | 最多 80 个可打印 ASCII 字符 |
| `preprocess.video.timestamp_overlay` | `false` | 叠加直播经过时间 |
| `preprocess.audio.sample_format` | `fltp` | PCM 格式偏好，编码链以 AAC 实际格式为准 |
| `preprocess.audio.frame_size` | `1024` | 独立预处理为 1–8192 样本/声道，编码链以 AAC 实际帧长为准 |
| `preprocess.audio.gain_db` | `0` | -60–30 dB，软件增益，不改系统音量 |
| `preprocess.audio.discontinuity_threshold_ms` | `100` | 1–1000 ms，音频时间不连续阈值 |

转换、补帧、FIFO 和尾部处理见 [预处理](preprocessing.md)，弱音量与削波见 [音频诊断](audio-diagnosis.md)。

## 观众字段

| 字段 | 默认值 | 约束/作用 |
|---|---|---|
| `input.url` | `http://127.0.0.1:8080/live/livestream.flv` | RTMP/RTMPS 或 HTTP/HTTPS-FLV |
| `input.queue_capacity` | `256` | 8–4096 个编码包/轨 |
| `input.max_buffer_ms` | `2000` | 100–60000 ms，媒体跨度和排队年龄上限 |
| `input.max_queue_bytes` | `33554432` | 1–256 MiB，两轨编码数据及 side data 合计 |
| `input.discontinuity_threshold_ms` | `1000` | 100–60000 ms，输入时间戳跳变阈值 |
| `video.width` / `video.height` | `1280` / `720` | 160–7680 / 90–4320，CLI 初始窗口尺寸，非解码尺寸 |
| `video.vsync` / `video.fullscreen` | `true` / `false` | CLI 请求垂直同步 / 全屏启动 |
| `video_decoder.backend` | `software` | `software` 软件解码，`nvdec` 指定 NVIDIA 硬解，`auto` 优先 NVDEC、初始化或格式协商失败则回退 |
| `audio.device` | `default` | 默认或精确设备名，存在且启用音轨时打开 |
| `audio.sample_rate` / `audio.channels` | `48000` / `2` | 输出 PCM 参数，AAC 标准采样率 / 1 或 2 声道 |
| `audio.enabled` | `true` | 禁用后仍消费并释放音频包 |
| `audio.buffer_ms` | `100` | 100–2000 ms，PCM 环容量，至少 4096 样本/声道 |
| `audio.volume` | `100` | 0–100，仅影响当前播放器 |
| `audio.muted` / `audio.paused` | `false` / `false` | 静音 / 同时暂停音画 |
| `sync.tolerance_ms` | `40` | 1–100 ms，允许的视频提前量 |
| `sync.drop_threshold_ms` | `100` | 1–1000 ms，迟到丢帧阈值，不小于容差 |
| `sync.audio_wait_ms` | `500` | 100–5000 ms，音频就绪/欠载恢复等待上限 |
| `sync.discontinuity_threshold_ms` | `1000` | 500–10000 ms，异常失步/候选等待阈值，大于丢帧阈值 |
| `pipeline.video_queue_capacity` | `3` | 1–64 个解码帧，另限 64 MiB |
| `pipeline.max_video_age_ms` | `200` | 20–5000 ms，相对播放时钟的迟到上限 |

音频按输入参数解码，再转换到输出 PCM 格式。视频队列满时背压；迟到判定取 `sync.drop_threshold_ms` 与 `pipeline.max_video_age_ms` 较小值，有后继帧时才丢弃。详见 [视频播放](video-playback.md)、[音频播放](audio-playback.md)、[音画同步](av-sync.md)。

## 共享网络与日志字段

以下网络字段分别位于主播 `output.*`、观众 `input.*`：

| 字段后缀 | 默认值 | 约束/作用 |
|---|---|---|
| `connect_timeout_ms` | `5000` | 100–60000 ms，连接与写头/探测阶段 |
| `write_timeout_ms`（仅 output） | `5000` | 100–60000 ms，写包/写尾/关闭 |
| `read_timeout_ms`（仅 input） | `5000` | 100–60000 ms，单次读包 |
| `reconnect_attempts` | `5` | -1–100；0 禁用，-1 持续重试 |
| `reconnect_delay_ms` | `1000` | 0–60000 ms，基础退避间隔 |
| `reconnect_max_delay_ms` | `30000` | 0–60000 ms，且不小于基础间隔 |
| `inactivity_timeout_ms` | `15000` | 0 禁用，否则 100–600000 ms，无有效媒体进展时限 |

成功传输有效媒体后才重置连续失败预算；退避、取消与恢复细节见 [网络可靠性](network-reliability.md)。

| 共用日志字段 | 默认值 | 作用 |
|---|---|---|
| `logging.level` | `INFO` | DEBUG / INFO / WARN / ERROR |
| `logging.console` | `true` | stderr 输出 |
| `logging.file` | `""` | 空值禁用文件输出 |

## 命令行速查

完整别名定义见 `src/common/config/CommandLine.cpp`，模式 `--help` 可列出全部参数；任意叶字段均可用 `--set FIELD=VALUE` 覆盖。

| 用途 | 常用参数 |
|---|---|
| 配置/查询 | `--config PATH`、`--set FIELD=VALUE`、`--check-config`、`--print-config` |
| 两端通用 | `--url`、`--audio-device`、`--sample-rate`、`--channels`、`--log-level`、`--log-file`、`--log-console` |
| 设备/文件 | `--source`、`--input-file`、`--video-source`、`--video-device`、`--audio-source`、`--capture-video`、`--capture-audio`、`--loop`、`--realtime` |
| 编码/输出 | `--width`、`--height`、`--fps`、`--video-bitrate`、`--audio-bitrate`、`--gop-size`、`--preset`、`--low-latency`、`--stream`、`--video-file`、`--audio-file` |
| 网络 | `--connect-timeout`、`--write-timeout`（主播）、`--read-timeout`（观众）、`--reconnect-attempts`、`--reconnect-delay`、`--reconnect-max-delay`、`--inactivity-timeout` |
| 观看控制 | `--video-decoder`、`--play-audio`、`--audio-buffer-ms`、`--volume`、`--mute`、`--audio-paused`、`--vsync`、`--fullscreen` |

```powershell
# 查询配置，不启动媒体；显式选择要查看的文件
.\build\cmake\bin\Release\livestreamlab.exe --mode publisher --config config\publisher-stream.json --print-config
# 任意字段覆盖；同步参数不需要专用别名
.\build\cmake\bin\Release\livestreamlab.exe --mode player --cli --set sync.tolerance_ms=30 --volume 50
# 禁用本地音频文件；空字符串使用 --set 形式
.\build\cmake\bin\Release\livestreamlab.exe --mode publisher --cli --set output.audio_file=
```

参数支持 `--name value` 和 `--name=value`；布尔值只接受 `true/false`，整数为完整十进制文本。`--input-file` 仅设置路径，须配合 `--source file`；`--video-device` 不会自动切换为摄像头。设备查询为主播模式的 `--list-devices` 或 `--list-options video|audio`。

拒绝非法 JSON、未知字段、错误类型及整数溢出；重复 JSON 键或重复覆盖项以后者为准。标准 AAC 采样率为 7350、8000、11025、12000、16000、22050、24000、32000、44100、48000、64000、88200、96000 Hz。

Windows 参数转换为 UTF-8，支持中文路径/设备名。退出码：`0` 成功或正常停止，`2` 配置/参数错误，`1` 运行错误。观众正常 EOF 仅在禁用重连后排空退出；允许重连时按断流处理。

C++ 接口：`Config.hpp` 的 `parse*Config()`、`load*Config()`、`validate()`、`toJson()`；配置错误类型为 `ConfigError`。
