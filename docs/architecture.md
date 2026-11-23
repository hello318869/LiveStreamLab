# 当前架构

## 入口与控制层

CMake 只生成统一应用 `livestreamlab`，入口为 `src/apps/livestreamlab_main.cpp`。
无参数启动 Qt 直播大厅，右上角提供开播入口。`--mode publisher|player` 直接进入对应窗口，
加 `--cli` 运行控制台流水线。帮助、配置检查和设备查询直接返回结果。
`LSL_BUILD_GUI=OFF` 时仍使用同一入口，由 `--mode` 选择 CLI。

`ApplicationControl` 使用唯一 `QApplication`。返回直播大厅或关闭功能窗口时，先请求停止，
待工作线程回收后销毁窗口；日志活到会话和控件销毁之后。默认 GUI 配置位于可执行文件旁
`config/`，显式 `--config` 优先。构建与运行命令见 [README](../README.md)。

## 模块与数据流

```text
开播：设备/文件 → 输入解码 → 视频预处理 → H.264 ─┐
                         → 音频重采样/FIFO → AAC ┤
                                               ├→ MP4 / M4A
                                               └→ FLV / RTMP → SRS
观看：SRS → 解封装 → 视频包 → 解码 → 同步调度 → Qt / SDL 显示
                 → 音频包 → 解码/重采样 → PCM 环 → SDL 音频设备
```

| 目录 | 职责与主要对象 | 说明 |
|---|---|---|
| `common/config`、`logger`、`error`、`media` | 配置覆盖/校验、日志、异常、FFmpeg/SDL RAII | [配置](configuration.md)、[日志与错误](logging-and-errors.md) |
| `publisher/capture` | `MediaInput`、`SystemAudioInput`、设备枚举；桌面/摄像头/麦克风/系统声音及文件输入 | [采集](capture.md) |
| `publisher/preprocess` | `VideoPreprocessor`、`AudioPreprocessor`；像素转换、缩放、帧率、重采样、FIFO | [预处理](preprocessing.md) |
| `publisher/encoder` | `VideoEncoder`、`AudioEncoder`；H.264、AAC | [视频编码](video-encoding.md)、[音频编码](audio-encoding.md) |
| `publisher/muxer`、`rtmp` | `VideoFileWriter`、`AudioFileWriter`、`RtmpPublisher`；本地封装与推流 | [RTMP](rtmp-streaming.md) |
| `player/demuxer` | `Demuxer`、`PacketBuffer`；网络输入、轨道快照、压缩包缓冲 | [解封装](network-demuxing.md) |
| `player/decoder`、`renderer` | 视频解码/格式适配、`IVideoRenderer` 与 SDL 实现 | [视频播放](video-playback.md) |
| `player/audio`、`pipeline/AudioPlayback` | 音频解码、F32 PCM、`IAudioSink` 与 SDL 输出 | [音频播放](audio-playback.md) |
| `player/sync` | `AVSynchronizer`；音频主时钟与外部时钟降级 | [音画同步](av-sync.md) |
| 两端 `pipeline`、`ui` | 流水线编排、Session 生命周期/快照、Qt 控件 | [主播控制](publisher-control.md)、[观众控制](player-control.md) |
| `common/queue`、`utils/NetworkStatus.hpp`、`stats` | 有界交接、网络状态/退避、性能快照及采集时间戳 | [线程](threading.md)、[恢复](network-reliability.md)、[统计](performance-statistics.md) |

`publisher/`、`player/` 表示业务模块，不再分别生成可执行程序。
主播流水线组合采集、预处理、编码和输出，采集层不依赖下游。
CLI 内置默认值和 `config/publisher.json` 保存独立 MP4/M4A；GUI 默认加载的
`config/publisher-stream.json` 开启推流并禁用本地文件。
文件输入按实际音视频轨建立会话，可处理单轨输入。

## 线程与资源边界

FFmpeg 上下文由所属工作线程独占；跨线程移动独占帧/包结构，底层缓冲使用引用计数。
同时录制和推流时创建独立包引用。观众以 `serial` 撤销旧连接的包、帧、PCM 和同步状态。
完整线程、队列与停止约定见 [threading.md](threading.md)。

GUI 通过 Session 请求启停、读取状态和最新 RGB 副本，不直接访问媒体队列。
主播预览约 8 FPS、最大 960×540；观众后台协调线程复用解码/同步路径，Qt 主线程绘制同步后的最新帧。
SDL 音频子系统在主线程创建/销毁；CLI 的 SDL 事件、窗口和渲染留在主线程。
两端日志均通过有界内存副本供界面轮询。

函数的启动、逐帧处理、回调、停止及重连顺序见 [函数调用顺序](function-call-order.md)。
延迟默认值与限制见 [延迟](latency.md)。SRS 为外部流媒体服务；
[多人直播扩展方案](../../多人直播系统扩展方案.md)中的 N04/N05 已接入当前客户端调用链。

## 平台应用层（N04/N05）

`apps/platform/ApiClient` 持有 Qt Network 异步网络管理器、平台配置和内存登录态，统一处理
Bearer、HTTP 状态、JSON、超时、取消与固定错误文案。API 请求只在 Qt 主线程协调，不进入媒体线程。
直播大厅是默认入口。`--mode publisher|player` 保留直连工具。

`apps/ui/LobbyControl` 在可见时定时刷新大厅，点击卡片重新取得播放信息后创建平台播放器。点击「我要开播」进入共享 API 会话的主播界面。
播放器持有房间和直播场次上下文，低频查询当前房间；确认原场次结束或被替换时停止媒体，
查询失败与 `RECONNECTING` 保留当前播放恢复。关闭播放器或主播窗口后，返回同一大厅并刷新列表。

主播界面负责登录、个人房间和开播协调：先申请服务端会话，再将完整 URL 交给既有
`PublisherSession`；预览不申请会话，停止与退出同时发出本地停止和服务端下播意图。
服务端状态和本地媒体状态分开显示。旧请求通过取消、对象生命周期及代次检查失效。
`PublisherSession`、`PlayerSession` 和采集/编解码层均不依赖 Qt Network 或数据库。

凭证不持久化；配置打印隐藏 URL 凭证与查询串，应用日志和 FFmpeg 诊断在输出前脱敏。
详细使用方式与尚未执行的运行验收见 [平台客户端](platform-client.md)。

N06 在 `ApiClient` 中按用户保存不含凭证的发布恢复意图，跨工作窗口返回仍可核对原请求。
GET 的一次有限重试共享原总超时；停止重提先核对确切会话。正常应用退出先异步关闭工作窗口，
远端请求只保留有限处理时间，本地媒体完成回收后才结束 Qt 事件循环。
后端从故障恢复时重新观察匹配发布连接的媒体增量，第一份累计计数只建立基线。
本机配置使用 `platform.local.json` 覆盖，部署目录提供原生 Linux systemd 与 LAN 示例，
恢复规则及边界见 [N06 恢复行为](platform-recovery.md)。
