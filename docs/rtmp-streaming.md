# FLV 封装与 RTMP 推流（M07 / M15）

`src/publisher/rtmp/RtmpPublisher` 由专用线程将 H.264/AAC 封装为 FLV，经 RTMP/RTMPS 写出。
本地 MP4/M4A 由 `src/publisher/muxer/` 独立处理。构建与产物路径见 [README](../README.md)。

CLI 无配置时默认只录制；`config/publisher-stream.json` 开启推流并关闭两个本地文件，
避免旧文件阻止启动。该配置当前选择 Windows 桌面与系统声音。
`--url` 只改地址，启用网络还须 `--stream true` 或配置 `output.streaming=true`。
GUI 行为见 [主播控制](publisher-control.md)。

## 使用

先准备可访问的 SRS 服务；客户端不负责部署 SRS。以下从工程根目录运行，远程服务器需替换主机、应用名和流名。

```powershell
# 桌面与系统声音推流
.\build\cmake\bin\Release\livestreamlab.exe --mode publisher --cli --config config/publisher-stream.json --url rtmp://127.0.0.1/live/livestream

# 文件按原时间节奏重编码并循环推流
.\build\cmake\bin\Release\livestreamlab.exe --mode publisher --cli --config config/publisher-stream.json --source file --input-file "D:/media/sample.mp4" --loop true

# 推流并保存两个独立本地文件，已有文件拒绝覆盖
.\build\cmake\bin\Release\livestreamlab.exe --mode publisher --cli --config config/publisher-stream.json --video-file output/live-01.mp4 --audio-file output/live-01.m4a
```

`--capture-video false` 为单音频，`--capture-audio false` 为单视频。
文件推流要求 `capture.realtime=true`；音源/平台边界见 [输入](capture.md)。
观众使用统一程序的观看模式，见 [README](../README.md)；外部播放器也可使用相同 RTMP 地址。

## output 配置

| 字段 | 默认值 | 范围 / 作用 |
|---|---|---|
| `streaming` | false | 是否启用网络输出 |
| `connect_timeout_ms` | 5000 | 100–60000 ms；打开连接与写 FLV 头共用预算 |
| `write_timeout_ms` | 5000 | 100–60000 ms；单次写入及总收尾预算 |
| `reconnect_attempts` | 5 | -1–100；连续故障重试数，-1 持续重试，0 禁止 |
| `reconnect_delay_ms` | 1000 | 0–60000 ms；首次退避，随后翻倍 |
| `reconnect_max_delay_ms` | 30000 | 0–60000 ms，且不小于首次间隔 |
| `inactivity_timeout_ms` | 15000 | 100–600000 ms，无媒体发送进展时限；0 禁用 |
| `queue_capacity` | 256 | 8–4096，待复用音视频编码包合计上限 |
| `max_interleave_delta_ms` | 500 | 1–5000 ms，交织等待上限 |

字段可 `--set output.字段=值` 覆盖，如 `--set output.max_interleave_delta_ms=200`；
队列快捷参数为 `--mux-queue-capacity`，其他映射见 [配置](configuration.md)。

## 接口、封装与时间戳

| 接口 | 约定 |
|---|---|
| `start(video,audio)` | 按实际存在的轨道启动一次；开启线程，尚未立即连接 |
| `setVideoParameters` / `setAudioParameters` | 深复制参数及编码时间基，先于该轨首包 |
| `submitVideo` / `submitAudio` | 移交独占包，同轨保持编码器顺序 |
| `finishVideo` / `finishAudio` | 该轨含尾包在内已全部提交 |
| `finish()` | 限时排空、写尾并 join，检查后台异常；幂等 |
| `requestStop()` | 中断网络、清队列；后续提交直接丢弃，不妨碍本地刷新 |
| `checkError()` / `stats()` | 检查后台异常 / 读取线程安全快照 |

日志须覆盖发送器生命周期。两轨可分别生产，但每轨参数/包/结束必须有序；
实际 `PublisherPipeline` 用唯一输出线程提交。网络线程独占 FLV 上下文：等参数和关键帧 → 建流/复制参数
→ `avio_open2()` → 写头 → 按 DTS 交织写包。同时录制时使用独立 AVPacket 结构，禁止提交后继续修改。

只接受 H.264/AAC，编码器开启 global header 提供 SPS/PPS、AudioSpecificConfig。
包须有有效 PTS/DTS、时间基及非负 duration，同轨 DTS 严格递增；写包使用写头后的实际流时间基换算。
按 DTS 而非 PTS 选择顺序，保留 B 帧重排。

音视频共用一个会话起点，保留起始偏移与 AAC priming；首次交织等待有时限，
起点确定后迟到且早于起点的包舍弃，避免负输出 DTS。恢复会话重新定基，不沿用旧输出 DTS。
FLV 不具备 MP4/M4A 相同的样本级 priming/尾部裁剪语义，不能保证外部播放器无缝首尾裁剪。
更多时间戳规则见 [协议](protocol.md)。

## 缓冲、恢复与停止

网络队列满时清积压，随后等待新视频关键帧，并舍弃早于恢复点的音频；故障期间内容不保证保留。
上游原始队列、输出事件队列和网络包队列分属不同阶段，见 [多线程流水线](threading.md)。

网络失败关闭旧会话后退避重建；只有成功写媒体包才重置连续失败预算。
等待参数/关键帧/包也检查无媒体进展时限，退避结束建立下一轮基准。
低帧率、大 GOP 或可能长时间无声时可调整该时限。重试耗尽则停止采集并回收线程；
日志不主动打印含认证信息的完整 URL。中断、超时分类、状态及退避见 [网络可靠性](network-reliability.md)。

正常 EOF：排空输入/预处理/编码 → 标记两轨结束 → `finish()` 在总计 write_timeout_ms 内尝试排空、写尾、关闭，
此时不再重连，超时舍弃剩余网络包。有编码输入却始终未发出包、或已有故障尚未恢复时报告错误。
Ctrl+C/GUI 停止立即中断网络并丢尾包，下游仍尽力写完本地文件；强杀不保证写尾。

`stats()` 包含发送 payload 字节/包、丢包、错误、成功重连、时长/平均码率及 `network` 连接状态快照。
`reconnects` 是成功重建数，`network.reconnectAttempts` 是安排重试数；恢复后保留最近错误描述。
码率不含 FLV/RTMP/TCP 头或重传，也不证明服务器/观众已收到；字段口径见 [性能统计](performance-statistics.md)。
已有采集/网络修复的运行范围见 [修复记录](m14-m15-fixes.md)。
