# M06 AAC 音频编码与自动保存

主播端将输入音频解码为 PCM，经 M04 重采样和 FIFO 组帧后送入 AAC-LC 编码器。
默认自动保存为 `output/audio.m4a`，目录不存在时创建，已有文件拒绝覆盖。
相对路径以启动工作目录为准；从 `D:\直播\LiveStreamLab` 启动时，输出位于
`D:\直播\LiveStreamLab\output\audio.m4a`。

当前视频保存为 `output/video.mp4`，音频保存为独立 M4A（内部编码为 AAC）。
M07 再进行 H.264/AAC 合并封装与推流，本阶段不连接 `output.url`。

## 编码参数与预处理协商

| 字段 | 默认值 | 含义 |
|---|---|---|
| `audio.sample_rate` | 48000 | AAC 采样率，时间基为 `1/sample_rate` |
| `audio.channels` | 2 | 单声道或双声道 |
| `audio.bitrate` | 128000 | 目标码率，bit/s |
| `output.audio_file` | `output/audio.m4a` | 本地音频文件路径；空字符串禁用写出 |
| `capture.audio_source` | `microphone` | 设备模式选择麦克风或 Windows 系统声音 `system`；文件模式仍用文件音轨 |
| `preprocess.audio.gain_db` | 0 | -60..30 dB 软件增益，0 保留原始电平，不修改 Windows 音量 |
| `preprocess.audio.sample_format` | `fltp` | PCM 格式偏好；编码器不支持时改用其支持的格式 |
| `preprocess.audio.frame_size` | 1024 | M04 独立使用时的帧长；主播端按 AAC 实际 `frame_size` 组帧 |

先打开 AAC 编码器，再用其实际采样格式、采样率、声道数和帧长创建音频预处理器。
优先使用 FFmpeg 原生 `aac` 编码器，使用 AAC-LC 配置；不支持指定采样率或声道布局时报告错误。
初始化日志打印实际参数，PCM 偏好被覆盖时另有提示。

常规 PCM 帧含完整的 `frame_size` 个每声道样本。结束时先排空重采样器，再处理 FIFO 余数：

- 编码器支持 `AV_CODEC_CAP_SMALL_LAST_FRAME`：输出一次实际余数长度的最终帧，之后只能刷新编码器。
- 不支持短尾帧：由 M04 补静音到完整帧长，并计入 `paddedSamples`。
- 采集停顿形成的中间片段始终补齐完整帧，允许随后继续编码；短帧只用于整段流的最后。

M04 独立使用时，`AudioPreprocessOptions::padFinalFrame` 默认仍为 `true`。
主播端根据编码器能力设置此值，无需手工指定新的配置。

## 时间戳、延迟与所有权

```text
MediaInput → CapturedFrame → AudioEncoder 参数协商
  → AudioPreprocessor（重采样 → FIFO → PCM AVFrame）
  → AudioEncoder → 独占 AAC AVPacket → AudioFileWriter → output/audio.m4a
```

PCM 的 PTS 按累计样本推进，保留与视频共用时间轴上的起始偏移；不把音频首帧单独归零。
输入帧时间区间不能重叠或倒退，采集停顿造成的正向 PTS 跳变允许保留。

AAC 编码有内部延迟，一次输入可能产生零个或多个包。send 返回 EAGAIN 时先取出已有包，
再重送同一帧；结束时发送空帧并 receive 至 EOF。`flush()` 幂等，无输入时不会生成音频包。

输出包保留编码器产生的 PTS、DTS、duration 和 side data。首包因 priming 可能有负时间戳，
不会擅自裁掉或移到零；`parameters()` 返回的独占参数保留 AudioSpecificConfig 和
`initial_padding`。M4A 写出器复制编码参数，保留包 side data，按流时间基换算时间戳，
由容器处理编码延迟。这些信息同样可供后续 M07 与视频合并输出。

每轨的预处理、编码和本地写出在所属输入线程内串行完成，包通过 `PacketPtr` 移动交接。
M14 再加入有界队列流水线，当前没有共享可变 AVPacket。

正常 EOF / Ctrl+C 的顺序：停止输入 → 排空预处理 → 排空编码器 → 写完文件尾部并关闭。
`finish()` 幂等并检查写尾、刷新和关闭错误；异常文件不会报告为完整输出。
关闭音频输入或源文件没有音频时，不创建空音频文件。

## 构建与使用

```powershell
cd D:\直播\LiveStreamLab
cmake -S . -B build/cmake -DLSL_BUILD_TESTS=OFF
cmake --build build/cmake --config Release --parallel
```

以下为使用命令，本次未执行：

```powershell
# 沿用配置，分别自动保存视频 MP4 与 AAC 音频 M4A；Ctrl+C 正常结束
.\build\cmake\bin\Release\publisher.exe --config config\publisher.json

# 只处理本地文件的音轨，EOF 后写尾退出
.\build\cmake\bin\Release\publisher.exe --source file --input-file "D:\media\sample.mp4" --capture-video false --audio-file output/file-audio.m4a

# 设置音频码率；已有文件需换一个输出名称
.\build\cmake\bin\Release\publisher.exe --audio-bitrate 192000 --audio-file output/audio-02.m4a
```

JSON 中设置 `output.audio_file` 为 `""`，或使用 `--set output.audio_file=`，可只编码和统计。
统计日志包含 `encoded_audio_frames`、`encoded_audio_samples`、`aac_packets` 和 `aac_bytes`；
样本数指每声道样本数，编码字节数不代表网络发送量。

Windows Release 的 CMake 完整构建通过，包括 `lsl_audio_encode`、`lsl_preprocess`、
`lsl_capture`、`publisher` 和 `player` 的编译、链接。
M06 初次接入时只进行了编译。本轮用户允许测试后，已完成增益定向测试、已知音调文件回归，
以及短时麦克风和 WASAPI 系统声音采集后的 AAC/M4A 解码检查。
音源选择、弱输入处理和实测证据见 [音频诊断](audio-diagnosis.md)。
