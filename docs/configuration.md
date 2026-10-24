# 配置管理

配置为 UTF-8 JSON，允许只提供部分字段。加载顺序：默认值 → 文件 → 命令行 → 校验最终值。
不传 `--config` 使用默认值；相对路径以启动工作目录为准。示例见 `config/*.json`。

## 字段

| 主播端字段 | 默认值 | 校验 |
|---|---|---|
| `devices.video` / `devices.audio` | `default` | 非空白设备名，不含控制字符 |
| `output.url` | `rtmp://127.0.0.1/live/livestream` | RTMP/RTMPS，主机与流路径非空 |
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
| `capture.audio_source` | `microphone` | `microphone` 或 `system`，仅影响设备模式；`system` 为 Windows 默认输出设备回采 |
| `capture.file` | `""` | 文件模式要求非空白 UTF-8 路径，打开时检查本地文件 |
| `capture.loop` | `false` | 文件 EOF 后循环，布尔值 |
| `capture.realtime` | `true` | 按文件时间戳控制交付节奏 |
| `capture.video` / `capture.audio` | `true` | 启用对应输入，至少启用一种 |
| `capture.timeout_ms` | `5000` | 100–60000，单次 I/O 超时毫秒数 |
| `capture.video_format` | `""` | 驱动协商，或指定如 `yuyv422` / `mjpeg` |
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

观众端使用 `input.url`（默认 `http://127.0.0.1:8080/live/livestream.flv`，支持
RTMP/RTMPS/HTTP/HTTPS），以及 `audio.device`、`audio.sample_rate`、`audio.channels`。
设备、采样率、声道的默认值及规则同上；这些音频参数用于输出。

AAC 采样率：7350、8000、11025、12000、16000、22050、24000、32000、44100、48000、
64000、88200、96000 Hz。设备模式的宽高/帧率/采样率/声道用于请求驱动支持的采集参数；
M03/M04 负责缩放和重采样。文件模式保留源文件参数，解码后再转换为配置的输出参数。
设备模式可通过 `capture.audio_source` 选择麦克风或电脑系统声音。`microphone` 使用
`devices.audio` 指定的输入设备；`system` 使用 Windows 默认输出设备的 WASAPI 回采，
此时 `devices.audio` 不参与选择。两种来源每次选择一种，不进行混音。
文件模式始终读取文件音轨，不受 `capture.audio_source` 影响。非 Windows 平台暂不支持
设备模式的 `system`；文件模式仍可使用同一配置。
URL 检查协议、主机/流路径是否为空及空白字符；端口、IPv6、认证等细节由 FFmpeg 打开流时解析。

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
`--realtime`、`--capture-video`、`--capture-audio`、`--audio-source`、`--capture-timeout`、`--video-format`。
`--input-file` 只覆盖路径，选择文件输入时还须设置 `--source file`。
`--audio-source microphone` 采集麦克风，可用 `--audio-device "设备名"` 选择具体输入；
`--audio-source system` 采集 Windows 默认输出设备正在播放的声音。系统声音不包含麦克风，
默认输出设备没有播放声音时，写出的音频也会静音。选择音源不会改变 Windows 音量、静音或默认设备。
预处理另有 `--scale-algorithm`、`--resize-mode`、`--rotation`、`--mirror`、
`--watermark`、`--timestamp-overlay`、`--audio-sample-format`、`--audio-frame-size`、`--audio-gain-db`。
详细策略与接口见 [M03/M04](preprocessing.md)。
视频编码另有 `--video-file`、`--gop-size`、`--preset`、`--low-latency`。
M05 默认自动保存只有视频轨的 MP4，`output.url` 暂不连接；
路径、停止写尾和编码器选择见 [M05](video-encoding.md)。
音频编码另有 `--audio-file`；M06 默认独立保存 AAC 音频至 M4A，
`--audio-bitrate`、`--sample-rate`、`--channels` 沿用已有音频参数。
PCM 格式与帧长协商、尾帧和编码延迟见 [M06](audio-encoding.md)。
`--list-devices` 枚举设备，`--list-options video|audio` 查询所选设备能力，均不启动采集。
布尔值只接受 `true` / `false`，整数使用完整十进制文本。

```powershell
.\build\windows-release\bin\publisher.exe --config config\publisher.json --fps 30 --set audio.channels=1 --print-config
.\build\windows-release\bin\player.exe --audio-device "中文扬声器" --check-config
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
`1` 其他启动/采集错误或尚未实现的观众播放。

C++ 接口位于 `src/common/config/Config.hpp`：
`parse*Config` 从字符串解析，`load*Config` 加载可选文件，
`validate` 检查结构，`toJson` 导出 JSON。错误为 `ConfigError`。
