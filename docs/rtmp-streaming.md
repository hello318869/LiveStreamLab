# M07 FLV 封装与 RTMP 推流

M07 将已有 H.264/AAC 编码器的输出交给 `RtmpPublisher`，由一个专用线程完成 FLV 封装与
RTMP/RTMPS 网络写出。实现位于 `src/publisher/rtmp/RtmpPublisher.hpp/.cpp`，由
`src/publisher/pipeline/PublisherPipeline.cpp` 接入主播端，CMake 目标为 `lsl_rtmp`。
本地 MP4/M4A 写出器位于 `src/publisher/muxer/`，由独立的 `lsl_muxer` 目标提供。

默认 `output.streaming=false`，原来的独立 MP4/M4A 录制继续可用。
`config/publisher-stream.json` 开启推流，并将 `output.video_file` 与 `output.audio_file` 设为空。
这避免默认录制路径的旧文件妨碍推流。需要同时录制时可显式指定两个新的本地文件名。

## 使用

先自行准备可访问的 SRS RTMP 服务；本模块不部署或修改 SRS。以下命令均在工程根目录执行，
示例采用 Visual Studio Release 产物路径，本次未运行。

```powershell
# 默认 Windows 桌面、麦克风 → H.264/AAC → SRS
.\build\cmake\bin\Release\publisher.exe --config config\publisher-stream.json --url rtmp://127.0.0.1/live/livestream

# 可选摄像头取代桌面；设备 id 来自 --list-devices
.\build\cmake\bin\Release\publisher.exe --config config\publisher-stream.json --video-source camera --video-device "default"

# 文件解码、预处理、重新编码后实时推流；不能使用 --realtime false
.\build\cmake\bin\Release\publisher.exe --config config\publisher-stream.json --source file --input-file "D:\media\sample.mp4" --loop true

# Windows 桌面画面 + 默认输出设备的系统声音，取代麦克风
.\build\cmake\bin\Release\publisher.exe --config config\publisher-stream.json --audio-source system

# 单音频推流；单视频则使用 --capture-audio false
.\build\cmake\bin\Release\publisher.exe --config config\publisher-stream.json --capture-video false

# 同时保存两条独立本地轨道；已有文件拒绝覆盖
.\build\cmake\bin\Release\publisher.exe --config config\publisher-stream.json --video-file output/live-01.mp4 --audio-file output/live-01.m4a

# 外部播放器接收；URL 与主播端相同
ffplay rtmp://127.0.0.1/live/livestream
```

远程服务器应替换主机地址，应用名/流名应与服务端配置一致。
OBS 可在“媒体源”中取消“本地文件”，将相同 RTMP 地址填入输入地址。
自研 `player` 仍属于后续观众端模块，不能用它验证本次推流。
仅传 `--url` 会覆盖地址，开启推流还需要 `--stream true` 或配置中的 `output.streaming=true`。
设备视频默认 `capture.video_source=desktop`。桌面采集读取完整虚拟桌面与鼠标，随后缩放到
配置的编码尺寸；`--video-source camera` 才使用 `--video-device` 和 `--video-format`。
文件输入不受视频来源字段影响；本次桌面后端仅支持 Windows，Linux 设备输入须选择 `camera`。

## 配置

字段均位于 JSON `output` 对象中，沿用已有 `url`：

| 字段 | 默认值 | 范围与用途 |
|---|---|---|
| `streaming` | `false` | 是否建立 FLV/RTMP 输出 |
| `connect_timeout_ms` | `5000` | 100–60000 ms，连接及写头阶段超时 |
| `write_timeout_ms` | `5000` | 100–60000 ms，写包、写尾和关闭阶段超时 |
| `reconnect_attempts` | `5` | 0–100，失败后的最多重试次数；0 禁止重连 |
| `reconnect_delay_ms` | `1000` | 0–60000 ms，重连等待间隔 |
| `queue_capacity` | `256` | 8–4096，应用侧待复用编码包上限 |
| `max_interleave_delta_ms` | `500` | 1–5000 ms，交织等待控制 |

快捷参数为 `--stream`、`--connect-timeout`、`--write-timeout`、`--reconnect-attempts`、
`--reconnect-delay`、`--mux-queue-capacity`。所有字段也可用 `--set output.字段=值` 覆盖。
推流采用文件输入时要求 `capture.realtime=true`，防止离线编码速度持续超出实时网络输出。

## 接口与交接

`lsl::rtmp::RtmpPublisher` 由主播端持有，日志对象必须覆盖它的整个生命周期。

| 接口 | 约定 |
|---|---|
| `start(video, audio)` | 按实际存在的轨道调用一次 |
| `setVideoParameters` / `setAudioParameters` | 深复制编码参数，登记对应编码时间基；先于该轨的任何提交 |
| `submitVideo` / `submitAudio` | 移交独占编码包；同一轨保持编码器输出顺序 |
| `finishVideo` / `finishAudio` | 该轨的延迟包已全部提交，之后不再生产 |
| `finish()` | 总收尾预算内排空、写尾并回收线程，检查内部异常；幂等 |
| `requestStop()` | 请求立即中断网络并丢弃尾包；随后提交安静丢弃，允许本地编码刷新继续 |
| `checkError()` / `stats()` | 主线程检查复用线程异常、读取线程安全统计快照 |

两条轨可以从不同线程生产；每条轨内部的参数登记、提交、结束必须依序调用。
不能在提交后继续修改同一个 `AVPacket` 结构，不能把本地写出器消费过的包再交给网络。

## 输出流与时间戳

生产线程深复制编码参数后提交给复用器；FLV 输出只接收 H.264 和 AAC。
推流时编码器启用 global header，让 H.264 SPS/PPS 与 AAC AudioSpecificConfig 随参数传入。
复用线程使用 `avformat_alloc_output_context2(..., "flv", ...)` 创建输出上下文，
建立实际音视频流、复制编码参数，并写入封装头。文件输入按启用且实际存在的轨道建流；
没有被启用的轨道不会使复用器一直等待。

视频编码包使用 `1/fps`，AAC 包使用 `1/sample_rate`。输出流时间基可能在写头时被 FFmpeg 调整，
因此每次写包都用写头后的流时间基调用 `av_packet_rescale_ts()`，同步换算 PTS、DTS、duration。
选择写出顺序时比较 DTS，不能按 PTS 排序，否则 B 帧重排会破坏解码顺序。
无时间戳、时间基错误、负 duration、同轨 DTS 倒退等异常会被报告。

一场输出会话使用同一个音视频起点，不分别将两轨首包归零；这保留设备启动偏移和编码延迟。
AAC 首包因 priming 可以带负时间戳，H.264 开启 B 帧时 PTS 与 DTS 也可以不同。
起播等待受交织上限限制；共同起点一旦确定，晚到且早于该起点的音频包会被舍弃，避免输出负 DTS。
网络恢复时重新选择共同起点、清空旧会话状态，而不是沿用旧连接的输出 DTS。
FLV 不能提供与 MP4/M4A 相同的样本级 priming/尾部裁剪表示，不能由“保留 side data”推断
外部播放器一定实现无缝音频首尾裁剪。

## 线程、所有权与网络恢复

采集线程仍串行完成解码、预处理、编码与可选本地写出。推流包以独占 `PacketPtr` 交给有界队列，
由专用复用线程串行调用 `av_interleaved_write_frame()`；生产线程不并发操作输出上下文。
同时本地录制时，独立引用产生不同的 `AVPacket` 结构，某一路 rescale 或消费包不会修改另一路元数据。

网络慢或断线时队列不能无限增长。超过容量时舍弃积压包并恢复到可解码边界；
有视频的会话从关键帧恢复，并丢弃早于恢复边界的音频。恢复前可能短暂没有画面/声音，
不保证故障期间的内容完整保留。M14 的完整采集/编码流水线队列仍未在本阶段展开。

连接与写入均设置超时，`AVIOInterruptCB` 根据停止请求与当前 I/O 截止时间中断阻塞操作。
网络失败时关闭旧会话，按配置有限重试，并重新创建输出上下文、流和封装头。
编码器继续生产，复用器等待新的关键帧；最坏等候时间还受 GOP 和编码器延迟影响。
重试耗尽后异常回传到主线程，主线程停止采集、回收线程并经既有日志边界报告。
地址由 FFmpeg 解析，不在应用日志中输出含认证信息或流密钥的完整 URL。

正常 EOF 时，先排空预处理与编码器并标记各轨结束，再尝试排空复用队列、写尾并关闭输出。
`finish()` 使用总计 `write_timeout_ms` 的收尾预算，进入收尾后不再重连；超时则舍弃剩余网络包并记录日志。
此前已发生且尚未恢复的网络故障仍会报告；有编码输入但未成功交付任何包时也返回错误。
Ctrl+C 会立即请求中断网络 I/O 并舍弃网络队列尾包，采集线程仍排空编码器并尽力写完本地 MP4/M4A。
因此主动停止不保证最后一批编码包已经推送，不会为了等待断开的服务器而无限阻塞退出。
强制结束进程不保证写尾或本地文件完整性。

## 统计与完成边界

`stats()` 字段为 `sentBytes`、`sentPackets`、`droppedPackets`、`networkErrors`、`reconnects`、
`durationSeconds` 和 `averageBitrate`，分别报告成功交给 mux 的包字节与包数、丢弃包数、网络错误、
重连次数、经过时间（秒）与平均码率（bit/s）。字节统计使用编码 payload 长度，不含 FLV/RTMP/TCP 头，
不等于网卡流量，也不证明服务器或观众已经接收。平均码率使用相同字节口径计算。
主播端每秒及结束时输出 `sent_packets`、`sent_payload_bytes`、`average_bitrate_bps`、`duration_seconds`、
`dropped_packets`、`network_errors` 与 `reconnects`，对应同一份统计。

历史记录：2026-10-25 的 M07 实现已通过 Windows Release 的 CMake 完整编译，生成 `publisher.exe` / `player.exe`：

```powershell
cmake -S . -B build/cmake -DLSL_BUILD_TESTS=OFF -DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON
cmake --build build/cmake --config Release --parallel 1 -- /nr:false
```

第二条为本机 Visual Studio/MSBuild 的实际构建命令。未新增构建脚本。
该阶段按项目要求未运行测试、采集、录制或网络推流。编译通过不等于已经通过外部播放验收；
SRS 连续接收、OBS/ffplay 播放、断线恢复、长时间稳定性和端到端延迟仍待用户后续实际运行确认。

同日后续的默认桌面采集接入未构建、未运行测试、未做真实桌面采集或网络推流；
上述 M07 编译记录不作为桌面采集修改的验收结果。
