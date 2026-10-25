# 当前架构

M01 提供配置、日志、错误处理，以及 FFmpeg/SDL3 的 RAII 所有权封装。
M02 的 `lsl_capture` 提供设备枚举、能力查询、设备/本地文件输入与解码。
M03/M04 的 `lsl_preprocess` 提供视频预处理与音频重采样/FIFO，独立链接 `lsl_common`。
M05 的 `lsl_video_encoder` 提供 H.264 软件编码，M06 的 `lsl_audio_encoder` 提供 AAC-LC 编码。
`lsl_muxer` 提供 `VideoFileWriter` / `AudioFileWriter`，负责单路 MP4/M4A 本地封装输出。
M07 的 `lsl_rtmp` 提供独立复用线程、FLV 封装、RTMP/RTMPS 输出与会话重建。
这些模块各自链接 `lsl_common`，由 `lsl_publisher_pipeline` 组合；`lsl_capture` 不依赖下游编码或输出模块。
设备音源可选 DirectShow/ALSA 麦克风，或 Windows `SystemAudioInput` 的 WASAPI 系统回采。

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
观众播放、M08 SRS 管理和 M14 采集/处理/编码完整队列流水线仍属后续模块。
