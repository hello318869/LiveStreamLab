# 音视频输入（M02）

采集实现位于 `src/publisher/capture/`，由 `PublisherPipeline` 组合。构建见 [README](../README.md)，以下命令从工程根目录运行。

## 输入选择

| 来源 | 行为与边界 |
|---|---|
| Windows 桌面 | `capture.source=devices`、`video_source=desktop`。FFmpeg `ddagrab` 通过 DXGI Desktop Duplication 采集首个显示输出（`output_idx=0`）和鼠标，按 `video.fps` 请求帧率。`video.width/height` 决定后续预处理的输出尺寸。 |
| 摄像头 | `video_source=camera`；Windows 使用 DirectShow，Linux 使用 V4L2；`devices.video` 可填设备 ID，Windows 也接受友好名称 |
| 麦克风 | `audio_source=microphone`；Windows `default` 按默认录音端点 GUID 匹配 DirectShow 设备，Linux 使用 ALSA |
| Windows 系统声音 | `audio_source=system`；WASAPI 回采默认播放端点，统一输出 48 kHz 双声道 float；与麦克风二选一，不混音、不修改系统音量，无声时允许正常等待 |
| 本地文件 | `source=file`、`capture.file` 指定路径；自动识别容器，为允许且存在的音视频轨解码，文件模式忽略设备来源字段 |

无配置时音源默认 `microphone`；仓库 `publisher.json` 和 `publisher-stream.json` 均选择 `system`。
桌面与系统回采仅支持 Windows。Linux 设备视频须选择 `camera`。
`--video-device` / `--video-format` 仅影响摄像头。

桌面源按单个显示输出采集。`ddagrab` 产生 D3D11 帧，采集端将帧下载为 CPU 内存中的 BGRA，交给现有缩放和像素转换。静止时仍按采集帧率交付画面。Windows SDK 需要包含 `libavfilter`，运行时需支持 `ddagrab` 和 DXGI 桌面复制。

```powershell
# 按仓库配置录制桌面与系统声音；输出文件已存在时需换名
.\build\cmake\bin\Release\livestreamlab.exe --mode publisher --cli --config config/publisher.json

# 摄像头与默认麦克风
.\build\cmake\bin\Release\livestreamlab.exe --mode publisher --cli --video-source camera --video-device default --audio-source microphone

# 设备枚举与摄像头能力；查询不会启动桌面采集
.\build\cmake\bin\Release\livestreamlab.exe --mode publisher --cli --list-devices
.\build\cmake\bin\Release\livestreamlab.exe --mode publisher --cli --list-options video --video-device default
```

Windows 分别枚举摄像头、麦克风；DirectShow 能力查询输出至 stderr，不受 `logging.file` 重定向。
Linux 视频能力查询使用 V4L2 ioctl，ALSA 在打开时协商参数。枚举/能力支持取决于驱动和 FFmpeg 后端；
实际输入格式见首帧日志。文件实时推流示例见 [RTMP 推流](rtmp-streaming.md)。

## 接口与时间轴

```cpp
lsl::capture::MediaInput input;
input.openFile(config); // 或 openDevice(Kind::Video/Audio, config)
while (auto captured = input.readFrame()) {
    // 将独占的 CapturedFrame 移动给下游。
}
input.close();
```

`readFrame()` 返回 `optional<CapturedFrame>`：EOF/停止为空，其他错误抛 `AppError/FFmpegError`。
帧不会被下一次读取覆盖，关闭输入后仍有效；引用副本共享底层缓冲，修改像素/样本前需 make_writable。
`CapturedFrame` 包含帧、媒体种类、原始时间基、`timestampUs` 和 `capturedAtUs`：

- 设备使用共同单调时钟。系统回采时间先校准到该时钟域。`capturedAtUs` 表示应用取得采集帧的时刻。
- 文件以容器/轨道起点为共同原点，保留音视频偏移；优先 `best_effort_timestamp`，缺失时按样本数/帧率补齐。
- 文件默认按时间戳节奏交付；`--realtime false` 可离线处理，但推流要求实时模式。
- EOF 先排空解码器；`--loop true` seek 回起点并刷新解码器，时间轴接续上一轮。不支持 seek 或没有解码帧时不能循环。

预处理如何使用时间轴见 [音视频预处理](preprocessing.md)。

## 生命周期与故障恢复

设备各有采集线程；文件共用一个解复用线程后按轨分流。输入的 open/read/close 必须在同一线程执行，
只有 `requestStop()` 可跨线程调用；不得在另一个线程读取期间直接 close。重复 close 安全，close 后可重新 open。

正常停止先请求输入停止并关闭原始队列，下游排空预处理和编码，再写本地文件尾部并回收线程；
异常会中止队列并停采集/网络。线程、队列与完整收尾顺序见 [多线程流水线](threading.md)。
非阻塞读包与中断回调支持停止/超时，但故障驱动的内部初始化调用可能不响应中断。

桌面读取遇到可恢复的 DXGI 错误时，在原采集线程尝试重建 `ddagrab` 采集图，共用本次 `capture.timeout_ms`（默认 5000 ms）预算。恢复期间沿用公共媒体时钟，音频继续运行，停止请求可唤醒重试等待。持续失败或首次打开失败会报告错误。FFmpeg 内部调用返回后才可检查停止和超时。
