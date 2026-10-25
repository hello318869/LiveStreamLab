# 配置管理

配置为 UTF-8 JSON，允许只提供部分字段。加载顺序：默认值 → 文件 → 命令行 → 校验最终值。
不传 `--config` 使用默认值；相对路径以启动工作目录为准。示例见 `config/*.json`。

## 字段

| 主播端字段 | 默认值 | 校验 |
|---|---|---|
| `devices.video` / `devices.audio` | `default` | 非空白设备名，不含控制字符 |
| `output.url` | `rtmp://127.0.0.1/live/livestream` | RTMP/RTMPS，主机与流路径非空 |
| `output.streaming` | `false` | 启用 M07 FLV/RTMP 输出；`--url` 单独不会开启推流 |
| `output.connect_timeout_ms` | `5000` | 100–60000 ms，连接与封装头阶段 I/O 超时 |
| `output.write_timeout_ms` | `5000` | 100–60000 ms，包写入、写尾及关闭阶段 I/O 超时 |
| `output.reconnect_attempts` | `5` | 0–100，网络故障后的最多重试次数；0 禁用重连 |
| `output.reconnect_delay_ms` | `1000` | 0–60000 ms，重连等待间隔 |
| `output.queue_capacity` | `256` | 8–4096，复用交接队列的编码包上限 |
| `output.max_interleave_delta_ms` | `500` | 1–5000 ms，音视频交织等待上限 |
| `output.video_file` | `output/video.mp4` | M05 本地 MP4 视频路径，空字符串禁用；自动建目录，拒绝覆盖已有文件 |
| `output.audio_file` | `output/audio.m4a` | M06 本地 AAC 音频 M4A 路径，空字符串禁用；自动建目录，拒绝覆盖已有文件 |
| `video.width` / `video.height` | 1280 / 720 | 2–7680 / 2–4320，偶数 |
| `video.fps` | 25 | 1–120 |
| `video.bitrate` | 2000000 | 64000–100000000 bit/s |
| `video_encoder.gop_size` | 50 | 最大关键帧间隔，1–1200 帧 |
| `video_encoder.preset` | `veryfast` | x264 预设：ultrafast / superfast / veryfast / faster / fast / medium / slow / slower / veryslow / placebo |
| `video_encoder.low_latency` | `true` | 禁用 B 帧，x264 使用 zerolatency |
| `audio.sample_rate` | 48000 | 标准 AAC 采样率 |
| `audio.channels` | 2 | 1 或 2 |
| `audio.bitrate` | 128000 | 16000–512000 bit/s |
| `capture.source` | `devices` | `devices` 或 `file` |
| `capture.video_source` | `desktop` | `desktop` 或 `camera`，仅影响设备模式；本次桌面后端仅支持 Windows |
| `capture.audio_source` | `microphone` | `microphone` 或 `system`，仅影响设备模式；`system` 为 Windows 默认输出设备回采 |
| `capture.file` | `""` | 文件模式要求非空白 UTF-8 路径，打开时检查本地文件 |
| `capture.loop` | `false` | 文件 EOF 后循环，布尔值 |
| `capture.realtime` | `true` | 按文件时间戳控制交付节奏 |
| `capture.video` / `capture.audio` | `true` | 启用对应输入，至少启用一种 |
| `capture.timeout_ms` | `5000` | 100–60000，单次 I/O 超时毫秒数 |
| `capture.video_format` | `""` | 摄像头模式由驱动协商，或指定如 `yuyv422` / `mjpeg`；不用于桌面输入 |
| `preprocess.video.scale_algorithm` | `bilinear` | `nearest` / `bilinear` / `bicubic` / `lanczos` |
| `preprocess.video.resize_mode` | `fit` | 保持比例留黑 `fit` / 居中裁剪 `crop` / 拉伸 `stretch` |
| `preprocess.video.rotation` | `0` | 顺时针 0 / 90 / 180 / 270 度 |
| `preprocess.video.mirror` | `false` | 旋转后水平镜像 |
| `preprocess.video.watermark` | `""` | 最多 80 个可打印 ASCII 字符 |
| `preprocess.video.timestamp_overlay` | `false` | 叠加直播经过时间 |
| `preprocess.audio.sample_format` | `fltp` | PCM 格式偏好；M06 协商编码器支持格式，实际值见日志 |
| `preprocess.audio.frame_size` | `1024` | M04 独立使用时为 1–8192 个每声道样本；主播端由 AAC 实际 frame_size 覆盖 |
| `preprocess.audio.gain_db` | `0` | -60–30 dB，重采样后、编码前的软件 PCM 增益；0 保留原电平，不修改系统设备音量 |
| `preprocess.audio.discontinuity_threshold_ms` | `100` | 1–1000 ms，区分时间抖动与不连续 |

观众端 M09 输入、M11 音频与 M12 显示字段：

| 观众端字段 | 默认值 | 校验及含义 |
|---|---|---|
| `input.url` | `http://127.0.0.1:8080/live/livestream.flv` | RTMP/RTMPS 或 HTTP/HTTPS-FLV，主机与流路径非空 |
| `input.connect_timeout_ms` | `5000` | 100–60000 ms，连接与流信息探测 I/O 超时 |
| `input.read_timeout_ms` | `5000` | 100–60000 ms，单次读包 I/O 超时 |
| `input.reconnect_attempts` | `5` | 0–100，连续故障后的最多重试次数；收到媒体包后重置故障预算 |
| `input.reconnect_delay_ms` | `1000` | 0–60000 ms，可被停止操作中断的重试等待 |
| `input.queue_capacity` | `256` | 8–4096，每轨编码包数量上限 |
| `input.max_buffer_ms` | `2000` | 100–60000 ms，媒体时间跨度与排队墙钟年龄上限 |
| `input.max_queue_bytes` | `33554432` | 1048576–268435456，两轨合计包数据与 side data 字节上限 |
| `input.discontinuity_threshold_ms` | `1000` | 100–60000 ms，每轨明显时间戳跳变的判定阈值 |
| `video.width` / `video.height` | 1280 / 720 | 160–7680 / 90–4320，初始窗口大小，不限制输入视频分辨率 |
| `video.vsync` | `true` | 请求垂直同步；后端不支持时记录警告并尝试关闭 |
| `video.fullscreen` | `false` | 是否以全屏启动；运行时可切换 |
| `audio.device` | `default` | 默认输出设备或精确设备名；实际存在音轨时打开 |
| `audio.sample_rate` / `audio.channels` | 48000 / 2 | 输出 PCM 参数，标准 AAC 采样率 / 1 或 2 声道 |
| `audio.enabled` | `true` | 启用音频解码与播放；禁用后仍消费并释放音频包 |
| `audio.buffer_ms` | `500` | 100–2000 ms，PCM 环形缓冲目标深度，实际容量至少 4096 个每声道样本 |
| `audio.volume` | `100` | 0–100，只改变播放器音量 |
| `audio.muted` / `audio.paused` | `false` / `false` | 启动时静音 / 仅暂停音频 |

观众端音频按流参数解码，再转换为配置的输出采样率与声道数；输出设置不覆盖输入编码参数。
音画同步尚未实现；Space 仅暂停声音，M 静音，↑/↓ 每次调节 10% 音量。
网络缓冲见 [M09](network-demuxing.md)，视频节奏见 [M10/M12](video-playback.md)，
音频接口、缓冲与提交侧时钟口径见 [M11](audio-playback.md)。
主播端与观众端都接受 `video.width` / `video.height`，前者控制编码尺寸，后者只设置初始窗口尺寸。

AAC 采样率：7350、8000、11025、12000、16000、22050、24000、32000、44100、48000、
64000、88200、96000 Hz。默认 `capture.video_source=desktop` 在 Windows 通过 `gdigrab`
采集完整虚拟桌面与鼠标，`video.fps` 控制采集帧率，`video.width` / `video.height` 仅作为
M03 缩放与编码的目标尺寸，不作为桌面裁剪尺寸。
`capture.video_source=camera` 才使用 `devices.video` 和 `capture.video_format`，
宽高/帧率会请求摄像头支持的采集组合；麦克风的采样率/声道同样由驱动协商。
M03/M04 负责后续缩放和重采样。文件模式保留源文件参数，解码后再转换为配置的输出参数，
不受 `capture.video_source` 影响。本次桌面后端仅支持 Windows；Linux 设备视频输入须选择 `camera`。
设备模式可通过 `capture.audio_source` 选择麦克风或电脑系统声音。`microphone` 使用
`devices.audio` 指定的输入设备；`system` 使用 Windows 默认输出设备的 WASAPI 回采，
此时 `devices.audio` 不参与选择。两种来源每次选择一种，不进行混音。
文件模式始终读取文件音轨，不受 `capture.audio_source` 影响。非 Windows 平台暂不支持
设备模式的 `system`；文件模式仍可使用同一配置。
URL 检查协议、主机/流路径是否为空及空白字符；端口、IPv6、认证等细节由 FFmpeg 打开流时解析。
开启 `output.streaming` 且使用文件输入时，必须保留 `capture.realtime=true`。
`config/publisher-stream.json` 是推流示例，显式开启推流并把两个本地文件路径设为空，避免旧文件阻止启动。

两端共用：

| 字段 | 默认值 | 含义 |
|---|---|---|
| `logging.level` | `INFO` | DEBUG / INFO / WARN / ERROR |
| `logging.console` | `true` | stderr 日志开关，JSON 布尔值 |
| `logging.file` | `""` | UTF-8 路径，空字符串禁用文件输出 |

拒绝非法 JSON、未知字段、错误类型及整数溢出。采用 JSON 库默认的重复键处理：
最后出现的键生效；不再添加重复键检测、文件大小和嵌套层数的定制限制。

## 命令行

`--config PATH` 指定文件，`--set FIELD=VALUE` 覆盖任意叶字段；
`--check-config` 检查并退出，`--print-config` 输出完整 JSON 并退出，`--help` 查看参数。
两种动作同时指定时优先打印 JSON。带值参数支持 `--name value` 和 `--name=value`。

通用快捷参数：`--url`、`--audio-device`、`--sample-rate`、`--channels`、
`--log-level`、`--log-file`、`--log-console`。
主播端另有 `--video-device`、`--width`、`--height`、`--fps`、
`--video-bitrate`、`--audio-bitrate`；采集参数为 `--source`、`--input-file`、`--loop`、
`--realtime`、`--capture-video`、`--capture-audio`、`--video-source`、`--audio-source`、`--capture-timeout`、`--video-format`。
`--input-file` 只覆盖路径，选择文件输入时还须设置 `--source file`。
`--video-source desktop` 是默认视频来源；`--video-source camera --video-device "设备名"` 改用摄像头。
`--video-device` 不会隐式切换到摄像头，`--video-format` 也只用于摄像头模式。
`--audio-source microphone` 采集麦克风，可用 `--audio-device "设备名"` 选择具体输入；
`--audio-source system` 采集 Windows 默认输出设备正在播放的声音。系统声音不包含麦克风，
默认输出设备没有播放声音时，写出的音频也会静音。选择音源不会改变 Windows 音量、静音或默认设备。
预处理另有 `--scale-algorithm`、`--resize-mode`、`--rotation`、`--mirror`、
`--watermark`、`--timestamp-overlay`、`--audio-sample-format`、`--audio-frame-size`、`--audio-gain-db`。
详细策略与接口见 [M03/M04](preprocessing.md)。
视频编码另有 `--video-file`、`--gop-size`、`--preset`、`--low-latency`。
M05 默认自动保存只有视频轨的 MP4；默认关闭推流时，`output.url` 不连接；
路径、停止写尾和编码器选择见 [M05](video-encoding.md)。
音频编码另有 `--audio-file`；M06 默认独立保存 AAC 音频至 M4A，
`--audio-bitrate`、`--sample-rate`、`--channels` 沿用已有音频参数。
PCM 格式与帧长协商、尾帧和编码延迟见 [M06](audio-encoding.md)。
推流参数为 `--stream true|false`、`--connect-timeout MS`、`--write-timeout MS`、
`--reconnect-attempts N`、`--reconnect-delay MS` 和 `--mux-queue-capacity N`；
交织等待可通过 `--set output.max_interleave_delta_ms=500` 修改。用法与恢复边界见 [M07](rtmp-streaming.md)。
观众端支持 `--connect-timeout MS`、`--read-timeout MS`、`--reconnect-attempts N`、
`--reconnect-delay MS`、`--packet-queue-capacity N`、`--max-buffer-ms MS` 和 `--max-queue-bytes N`，
分别映射到 `input` 下的同名配置；`--connect-timeout`、`--reconnect-attempts`、`--reconnect-delay`
在两端作用于各自的输入或输出连接。时间戳跳变阈值使用
`--set input.discontinuity_threshold_ms=1000` 修改。
观众端另支持 `--width N`、`--height N`、`--vsync true|false`、`--fullscreen true|false`，
对应 `video` 显示字段。窗口缩放保持输入视频的显示宽高比；F、Alt+Enter 或双击切换全屏，
Esc、关闭窗口或 Ctrl+C 停止。
观众端音频支持 `--play-audio BOOL`、`--audio-buffer-ms MS`、`--volume N`、`--mute BOOL` 和
`--audio-paused BOOL`，分别覆盖 `audio.enabled`、`audio.buffer_ms`、`audio.volume`、`audio.muted`、`audio.paused`；
输出设备、采样率、声道沿用 `--audio-device`、`--sample-rate`、`--channels`。本次未实现音画同步。
`--list-devices` 枚举摄像头、麦克风，`--list-options video|audio` 查询所选摄像头/麦克风能力，
均不启动桌面采集；查询摄像头能力不要求先切换 `capture.video_source`。
布尔值只接受 `true` / `false`，整数使用完整十进制文本。

```powershell
.\build\cmake\bin\Release\publisher.exe --config config\publisher.json --fps 30 --set audio.channels=1 --print-config
.\build\cmake\bin\Release\player.exe --audio-device "中文扬声器" --check-config
.\build\cmake\bin\Release\player.exe --width 960 --height 540 --vsync true --fullscreen false
```

选择音源时可从工程根目录运行以下命令；音频默认保存到 `output/audio.m4a`，已有文件需换名：

```powershell
# 麦克风；指定名称来自 --list-devices，可省略 --audio-device 使用默认选择
.\build\cmake\bin\Release\publisher.exe --capture-video false --audio-source microphone --audio-device "麦克风设备名" --audio-file output/microphone.m4a
# 电脑系统声音；使用 Windows 默认输出设备
.\build\cmake\bin\Release\publisher.exe --capture-video false --audio-source system --audio-file output/system.m4a
# 输入麦克风电平偏低时，可手动增加 20 dB（采样幅度乘 10）
.\build\cmake\bin\Release\publisher.exe --capture-video false --audio-source microphone --audio-gain-db 20 --audio-file output/microphone-gain20.m4a
```

`--audio-gain-db` 支持整数 -60–30，正值放大，负值衰减。增益作用于麦克风、系统回采和
文件音轨的 PCM；不会把设备没有采集到的声音恢复出来。超出满幅的值会饱和截断，并累计
`clippedValues`（所有声道的采样值总数，与每声道样本数不同）；出现截断时应减小增益。
0 dBFS 表示数字满幅，PCM 峰值和 RMS 越接近 0 dBFS 声音越大；较小的 RMS 本身并不代表编码丢失。

Windows 从 Unicode 命令行转换为 UTF-8，支持中文配置路径与设备名。
退出码：`0` 成功（包括文件 EOF 和 Ctrl+C 停止），`2` 配置或命令行错误，
`1` 其他启动、采集、编码、推流或观众拉流、视频解码及显示错误。观众端默认把直播 EOF 作为断流重连；
禁用重连后正常 EOF 排空退出为 `0`，其他网络错误仍为 `1`。

C++ 接口位于 `src/common/config/Config.hpp`：
`parse*Config` 从字符串解析，`load*Config` 加载可选文件，
`validate` 检查结构，`toJson` 导出 JSON。错误为 `ConfigError`。
