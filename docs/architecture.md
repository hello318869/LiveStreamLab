# 当前架构

M01 提供配置、日志、错误处理，以及 FFmpeg/SDL3 的 RAII 所有权封装。
M02 的 `lsl_capture` 提供设备枚举、能力查询、设备/本地文件输入与解码。
M03/M04 的 `lsl_preprocess` 提供视频预处理与音频重采样/FIFO，独立链接 `lsl_common`。
M05 的 `lsl_video_encoder` 提供 H.264 软件编码，M06 的 `lsl_audio_encoder` 提供 AAC-LC 编码。
`lsl_muxer` 提供 `VideoFileWriter` / `AudioFileWriter`，负责单路 MP4/M4A 本地封装输出。
M07 的 `lsl_rtmp` 提供独立复用线程、FLV 封装、RTMP/RTMPS 输出与会话重建。
这些模块各自链接 `lsl_common`，由 `lsl_publisher_pipeline` 组合；`lsl_capture` 不依赖下游编码或输出模块。
设备音源可选 DirectShow/ALSA 麦克风，或 Windows `SystemAudioInput` 的 WASAPI 系统回采。
M09 的 `lsl_demuxer` 提供网络输入、媒体流快照及音视频有界包队列。
M10 的 `lsl_video_decoder` 提供 FFmpeg 视频软件解码和显示格式适配，
M12 的 `lsl_video_renderer` 提供 `IVideoRenderer` 接口及 SDL3 实现，链接 `lsl_common` 和 `SDL3::SDL3`。
M11 的 `lsl_audio_decoder` / `lsl_audio_sink` 提供音频解码、固定格式 PCM、SDL3 输出及估计播放时钟。
M13 的 `lsl_sync` 提供 `AVSynchronizer`，按音频进度调度视频，并在无可用音频时降级为外部时钟。
`lsl_player_pipeline` 组合网络输入、解码工作线程、帧交接和主线程显示，并接入 `player` 入口。

2026-10-25 按《方案.md》统一目录与命名空间，运行逻辑保持原样：

```text
src/publisher/
├── capture/       # lsl::capture：设备与媒体输入
├── preprocess/    # lsl::preprocess：音视频预处理
├── encoder/       # lsl::encoder：AudioEncoder、VideoEncoder
├── muxer/         # lsl::muxer：AudioFileWriter、VideoFileWriter
├── rtmp/          # lsl::rtmp：RtmpPublisher
└── pipeline/      # lsl::pipeline：PublisherPipeline，统一编排 runPublisher
```

`RtmpPublisher` 当前保留 FLV 与网络会话的完整实现；本轮只整理归属，不强拆内部逻辑。
`publisher` 入口链接 `lsl_publisher_pipeline`，后者组合采集、预处理、编码、本地封装与推流模块。
目录调整后已通过 Windows Release 的 CMake 完整编译；本轮未运行测试、采集或推流。

```text
publisher_main
  └─ runConfigApp：命令行 → JSON → 校验 → 日志 → 顶层异常边界
       └─ pipeline/PublisherPipeline：runPublisher
            ├─ 枚举 / 能力查询后退出
            ├─ 摄像头线程 → 独占设备上下文 → 视频解码 → CapturedFrame
            ├─ 音频线程 → 麦克风解码 / WASAPI 系统回采 → CapturedFrame
            └─ 或文件线程 → 解复用 → 音视频解码 → 时间戳节奏 → CapturedFrame

CapturedFrame
  ├─ 视频 → VideoPreprocessor → YUV420P / 目标宽高 / 1/fps PTS
  └─ 音频 → AudioPreprocessor → swresample → FIFO → 固定样本数 / 1/sample_rate PTS
       └─ EOF / 正常停止：排空重采样延迟，按编码器能力输出短尾帧或补齐

视频预处理输出 → VideoEncoder → 独占 AVPacket → VideoFileWriter → output/video.mp4
音频预处理输出 → AudioEncoder → 独占 AVPacket → AudioFileWriter → output/audio.m4a

启用 output.streaming：
视频 / 音频编码包 → 独占 PacketPtr 有界队列 → RtmpPublisher 复用线程
  → 按 DTS 选包 → av_packet_rescale_ts → av_interleaved_write_frame → FLV / RTMP → SRS
```

输入、预处理、音视频编码和本地写出都在所属输入工作线程内串行运行。
音频先打开编码器协商 PCM 格式及帧长，再创建预处理器；音视频继续共用时间轴。
开启推流时，生产线程交接独占编码包，复用线程独占输出上下文与网络 I/O。
同时保存本地文件时，先创建独立 AVPacket 引用，再交给两个各自拥有包结构的写出路径。
文件输入按实际存在的轨道建立输出流，支持仅音频或仅视频；设备输入按启用的轨道建立会话。
默认 `output.streaming=false`，沿用独立 MP4/M4A 输出；`config/publisher-stream.json` 显式开启推流并禁用本地文件。
观众音频解码播放 M11 与音画同步 M13 已接入；M08 SRS 管理和 M14 完整队列流水线仍属后续模块。

## M09–M13 观众端播放链路

```text
src/player/
├── demuxer/       # lsl::demuxer：Demuxer、PacketBuffer、媒体事件与流快照
├── decoder/       # lsl::decoder：VideoDecoder、AudioDecoder、独占视频帧/PCM块
├── renderer/      # lsl::renderer：IVideoRenderer、SDLVideoRenderer
├── audio/         # lsl::audio：IAudioSink、SDLAudioSink、有界 SPSC 环形缓冲
├── sync/          # lsl::sync：AVSynchronizer、音频主时钟与外部时钟降级
└── pipeline/      # lsl::player：runPlayer、VideoWorker、AudioPlayback、视频队列与调度

player_main
  └─ runConfigApp：命令行 → JSON → 校验 → 日志 → 顶层异常边界
       └─ pipeline/PlayerPipeline：runPlayer
            ├─ Demuxer 网络线程独占 AVFormatContext
            │    → avformat_open_input → avformat_find_stream_info
            │    → 选择视频/音频流 → codecpar 深拷贝快照
            │    → av_read_frame → 按 stream_index 分发
            │         ├─ 视频 PacketPtr 有界队列
            │         └─ 音频 PacketPtr 有界队列
            ├─ VideoWorker 解码线程独占 AVCodecContext
            │    ├─ 视频 Reset → 按新 codecpar 重开 VideoDecoder
            │    └─ 视频 Packet → send/receive → YUV420P / NV12 → VideoFrameQueue
            ├─ AudioPlayback 音频线程独占音频 AVCodecContext 与 SwrContext
            │    → 音频 Packet → send/receive → 固定采样率/声道 F32 → IAudioSink/SPSC 环
            ├─ SDL3 音频回调：从环取 PCM → SDL_PutAudioStreamData → 系统音频设备
            └─ 主线程：SDL 事件 → AVSynchronizer/IAudioSink::clock → IVideoRenderer → SDL3 纹理显示
```

`AVPacket` 结构通过 `PacketPtr` 转移独占所有权；媒体缓冲由 FFmpeg 引用计数管理。
`StreamInfo` 拥有独立的 `AVCodecParameters`，不会把输入上下文里的 `AVStream` 借给下游。
`DecodedVideoFrame` 转移独占帧结构，包含 `serial`、微秒媒体 PTS 和帧时长；
YUV420P/NV12 可直接交接，其他支持的软件格式通过 libswscale 转为 YUV420P。
每个解码批次最多保留 32 帧、128 MiB；`VideoFrameQueue` 另限 8 帧、64 MiB、
500 ms 媒体跨度与墙钟排队年龄，主线程另持有最多一帧等待显示。
这些是各交接层的限额，不代表整个进程的内存上限；解码器参考帧和 SDL 纹理另外占用资源。

每路队列首先交付 `Reset`，随后交付同一 `serial` 的 `Packet`。
重连、积压清空和明显时间戳跳变都会清空两路队列并递增 `serial`。
M10 在 `Reset` 时重开解码器和清空帧队列；主线程发现换代后丢弃待显示帧、撤下旧画面并重建视频时钟。
已经取出的包、解码输出和待显示帧也根据当前 `serial` 丢弃，避免跨会话显示。
当前 `AVSynchronizer` 按同代次音频估计播放位置安排视频；PTS 缺失时用帧时长补时，
默认提前超过 40 ms 等待，迟到超过 100 ms 且仍有后续帧时丢弃当前帧。
音频未就绪、欠载或有效时钟停滞超过 500 ms 后降级到单调外部时钟，恢复后重新跟随音频；
提前候选帧等待过久且有后续帧时更新候选，相邻视频 PTS 大跨度不会单独触发两轨清空。
每轮主循环观察暂停状态，无待显示帧时也冻结外部时钟；
同步误差与等待、丢帧、时钟降级及重建信息进入播放器日志。
M11 按输入流实际音频格式解码，再重采样为用户配置的交错 F32；音频环满时只等待音频线程，
不阻塞视频解码。回调只取预分配环中已有样本，欠载不虚增媒体位置；
M13 从提交终点扣除 SDL 未消费缓存与设备周期延迟，得到估计播放位置，仍不代表精确物理位置。
没有音轨或禁用音频时不打开输出设备；纯音频流保留黑色窗口并播放声音。

停止时先请求中断网络 I/O、停止并清空帧队列，等待解码线程和网络线程退出，
最后在主线程释放 SDL 纹理、渲染器、窗口及视频子系统。正常 EOF 只有在禁用重连时才排空解码器尾帧；
用户停止、网络失败和重连换代都不继续播放旧尾部。

接口和缓冲规则见 [M09 说明](network-demuxing.md)、[M10/M12 说明](video-playback.md)、
[M11 说明](audio-playback.md) 与 [M13 说明](av-sync.md)。
M09 阶段曾通过 Windows Release 的 `player` 编译、链接，未启动程序、未运行测试、未连接 SRS。
本次 M10/M12 已通过 CMake 配置与 Windows Release 的 `player` 编译、链接，
未启动程序、未运行测试、未连接 SRS；编译通过不代表真实播放已验证。

M11 音频解码、SDL3 输出与独立音频线程已接入，并通过本次 CMake 配置和 Release 的 `player` 编译、链接。
该 M11 阶段未启动程序、未运行测试、未连接 SRS，当时尚未实现 M13 音画同步。

M13 现已接入同步器和音频估计播放时钟，并通过 CMake 配置与 Windows Release 的 `player` 编译、链接。
本次未运行测试、未启动播放器或连接 SRS；
真实同步误差、连续播放和故障恢复仍待运行验收。
