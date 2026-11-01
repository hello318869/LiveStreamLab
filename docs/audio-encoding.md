# AAC 编码与音频文件（M06）

`src/publisher/encoder/AudioEncoder` 将预处理 PCM 编码为 AAC-LC；
`src/publisher/muxer/AudioFileWriter` 保存独立 M4A。CLI 默认 `output/audio.m4a`，
GUI 禁用本地文件；合并 H.264/AAC 推流见 [RTMP 推流](rtmp-streaming.md)，构建见 [README](../README.md)。

## 参数与协商

| JSON 字段 | 默认值 | 作用 |
|---|---|---|
| `audio.sample_rate` | 48000 | Hz，时间基 `1/sample_rate` |
| `audio.channels` / `bitrate` | 2 / 128000 | 单/双声道；目标码率 bit/s |
| `output.audio_file` | `output/audio.m4a` | 空字符串禁用本地写出 |
| `preprocess.audio.sample_format` | `fltp` | PCM 格式偏好，以编码器实际接受的格式为准 |
| `preprocess.audio.frame_size` | 1024 | 独立预处理帧长；编码链路按 AAC 实际 frame_size 组帧 |
| `preprocess.audio.gain_db` | 0 | -60..30 dB 软件增益，不修改设备音量 |

优先 FFmpeg 原生 `aac`，不支持请求的采样率/布局时报错。
先打开编码器，再用实际格式、采样率、声道和帧长创建 `AudioPreprocessor`；日志报告协商参数。
音源选择与配置默认值区别见 [输入](capture.md)，增益/时间跳变见 [预处理](preprocessing.md)。

## 时间戳、尾帧与接口

```text
原始音频队列 → AudioPreprocessor → AudioEncoder::encode()
  → 输出事件队列 → AudioFileWriter::write() / RtmpPublisher::submitAudio()
```

PCM PTS 按累计每声道样本推进，保留与视频的共同起始偏移；编码输入区间不能倒退或重叠，
允许正向时间跳变。AAC 有内部延迟，`encode()` 返回零到多个独占包；EAGAIN 时取包后重送同一帧。

正常结束先排空重采样器、FIFO：编码器支持 `AV_CODEC_CAP_SMALL_LAST_FRAME` 时输出一次短尾帧，
随后只能 flush；否则补静音到完整帧。采集中间断点始终补完整帧。独立预处理默认
`padFinalFrame=true`，主播端按编码器能力设置，相关统计见 [预处理](preprocessing.md)。

`flush()` 送空帧并 receive 至 EOF，幂等，无输入不产包。包保留 PTS/DTS/duration/side data；
priming 可能产生负时间戳。`parameters()` 保留 AudioSpecificConfig、initial_padding；
M4A 写出器按实际流时间基换算，由容器表示编码延迟，不单独把首包归零。

正常排空尾包后 `finish()` 写尾、刷新并关闭 I/O，检查错误且幂等。
相对路径基于启动目录，目录自动创建，已有文件拒绝覆盖，无音频帧不建空文件。
异常或强杀不能保证文件完整。所有权/背压见 [多线程流水线](threading.md)。

## 使用

以下命令从工程根目录运行：

```powershell
# 只将文件音轨重编码为 M4A
.\build\cmake\bin\Release\livestreamlab.exe --mode publisher --cli --source file --input-file "D:/media/sample.mp4" --capture-video false --audio-file output/file-audio.m4a
```

系统声音用 `--audio-source system`，码率用 `--audio-bitrate`；`--set output.audio_file=` 禁用本地写出。
日志 `encoded_audio_frames/samples`、`aac_packets/bytes`
表示编码量；样本数按每声道计，字节数不是网络流量。弱输入诊断及已有实测证据见 [音频诊断](audio-diagnosis.md)。
