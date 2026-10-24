# 当前架构

M01 提供配置、日志、错误处理，以及 FFmpeg/SDL3 的 RAII 所有权封装。
M02 的 `lsl_capture` 提供设备枚举、能力查询、设备/本地文件输入与解码。
M03/M04 的 `lsl_preprocess` 提供视频预处理与音频重采样/FIFO，独立链接 `lsl_common`。
M05 的 `lsl_video_encode` 提供 H.264 软件编码与单路 MP4 本地写出，独立链接 `lsl_common`。
M06 的 `lsl_audio_encode` 提供 AAC-LC 编码与单路 M4A 本地写出，独立链接 `lsl_common`。
设备音源可选 DirectShow/ALSA 麦克风，或 Windows `SystemAudioInput` 的 WASAPI 系统回采。

```text
publisher_main
  └─ runConfigApp：命令行 → JSON → 校验 → 日志 → 顶层异常边界
       └─ runPublisher
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
```

输入、预处理、音视频编码和本地写出都在所属输入工作线程内串行运行。
音频先打开编码器协商 PCM 格式及帧长，再创建预处理器；音视频继续共用时间轴。
M07 音视频封装推流、观众播放和 M14 完整队列流水线仍未实现。
当前已提供的并发仅用于同时读取摄像头和麦克风，以及有序停止输入。
