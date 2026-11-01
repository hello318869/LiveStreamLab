# 网络拉流、解封装与恢复

`src/player/demuxer` 从 RTMP/RTMPS、HTTP/HTTPS-FLV 读取直播，向独立音视频消费者交接压缩包。支持纯视频、纯音频，不录制媒体。播放侧见[视频](video-playback.md)、[音频](audio-playback.md)和[音画同步](av-sync.md)。构建见 [README](../README.md)，线程和调用链见[线程模型](threading.md)、[调用顺序](function-call-order.md)。

## 使用与参数

SRS 须已有对应直播流；远程服务器需替换 URL 主机。示例在工程目录执行：

```powershell
.\build\cmake\bin\Release\livestreamlab.exe --mode player --cli --config config\player.json
.\build\cmake\bin\Release\livestreamlab.exe --mode player --cli --url rtmp://127.0.0.1/live/livestream
.\build\cmake\bin\Release\livestreamlab.exe --mode player --cli --reconnect-attempts -1 --reconnect-delay 1000 --reconnect-max-delay 30000
```

`--help`、`--print-config`、`--check-config` 仅查询，不连接网络。TLS 依赖 FFmpeg SDK 和服务器支持。以下 JSON 字段均位于 `input`：

| 字段 | 默认值 | 含义 |
|---|---|---|
| `connect_timeout_ms` / `read_timeout_ms` | 5000 / 5000 | 连接及探测共用预算 / 单次读预算 |
| `reconnect_attempts` | 5 | 连续故障预算，0 禁用、-1 持续 |
| `reconnect_delay_ms` / `reconnect_max_delay_ms` | 1000 / 30000 | 指数退避初值/上限 |
| `inactivity_timeout_ms` | 15000 | 无可交给下游的媒体时限，0 禁用 |
| `queue_capacity` | 256 | 每轨包数 |
| `max_queue_bytes` | 33554432 | 两轨 payload 与 side data 合计 |
| `max_buffer_ms` | 2000 | 媒体跨度与最旧包排队年龄 |
| `discontinuity_threshold_ms` | 1000 | 包时间戳跳变阈值 |

CLI 映射与约束见[配置说明](configuration.md)，字段可用 `--set input.字段=值` 覆盖。退避参数可为 0，上限不能小于初值；有效媒体时限应大于上游最长 GOP。

## 连接与所有权

```text
Demuxer::start → 网络线程
  → open：avformat_open_input → avformat_find_stream_info → 复制轨道参数 → buffer.reset
  → readUntilEof：av_read_frame → 过滤/识别轨道 → buffer.push
```

网络线程独占 `AVFormatContext`；`StreamInfo` 深拷贝 `AVCodecParameters` 和时间基，关闭输入后仍有效，不借用 `AVStream`。队列移动交接独占 `PacketPtr`；分发给多个消费者需另建 `referencePacket()`。

探测使用 `fps_probe_size=0`，保留 2 MiB / 3 秒分析上限及探测包，参数完整后不额外累计帧估算 FPS；分析上限不是固定预缓冲。`probe_ms` 仅计流探测，参见[延迟说明](latency.md)。

中断回调同时检查停止、I/O 截止时间和有效媒体截止时间，关闭与退避等待也可中断。EAGAIN/EINTR 不重置读预算；无关流、损坏包、空包及等关键帧时的 P 帧不延长有效媒体时限。`start()` 每对象只能一次，停止可重复调用。

## 缓冲与事件

`PacketBuffer` 用同一锁保护两路队列、快照及 `serial`，网络生产者不等待解码器。入/出队均检查包数、字节、媒体跨度和墙钟年龄；缺少 PTS/DTS 仍受年龄限制。超限（含过大单包）会清两轨、递增 `serial` 并等视频关键帧，随后拒绝早于该关键帧的音频；纯音频无需等关键帧。该策略允许直播内容跳跃，不适合无损文件读取。

`Demuxer::pop(track, timeoutMs)` 每轨只允许一个消费者；当前视频/音频工作线程最多短等 10 ms。禁用声音时仍消费音频包，PCM 环满时只背压音频线程。

| 事件 | 消费者动作 |
|---|---|
| `Reset` | 清旧解码器、帧、PCM 与时钟；有 `streams` 才重建，无快照继续等连接 |
| `Packet` | 按轨道时间基处理，并检查 `serial` |
| `Timeout` | 检查停止及控制后继续 |
| `End` | 本轨缓存排空；先 `checkError()`，正常 EOF 才刷新尾部 |
| `Stopped` | 立即结束，丢弃积压 |

`finish()` 允许排空后返回 End，`stop()` 清空并唤醒。旧包可能已被取出，因此解码输出、待显示帧和 PCM 入队同样检查代次。`StreamInfo.session` 标识连接，`serial` 标识播放代次，一次连接可换代多次。GUI 在后台协调并由 Qt 主线程绘制，CLI 在主线程处理 SDL 显示。

## 重连、时间戳与统计

连接依次为 CONNECTING → STREAMING。可重试故障时先置 RECONNECTING，`buffer.disconnect()` 撤销快照、清包并换代，再关闭旧上下文、退避、重开；空快照 Reset 让下游及时撤下旧画面和声音状态。每次连接重探轨道，参数及时间基可变。预算耗尽/不可恢复错误进入 ERROR；主动停止经过 STOPPING 回到 IDLE。

退避默认 1/2/4/8/16/30 秒，随后封顶；只有队列接受有效媒体才重置连续故障预算，仅连接或收流头成功不算恢复。内存、参数、协议/解封装器缺失及 HTTP 认证/权限错误直接结束。无有效媒体超时按 ETIMEDOUT 处理，与单次读超时独立。

默认 EOF 触发重连；`--reconnect-attempts 0` 下正常 EOF 排空并返回 0，运行错误返回 1，配置错误返回 2。包的 PTS/DTS/时间基保留；缓冲检测优先用 DTS，无 DTS 用 PTS，跳变超限时共同换代，跨连接不拼接时间轴。

`BufferStats` 提供包数、字节、时长、峰值、丢包/重置/超限和 serial；`DemuxStats` 增加接收量、忽略包、连接/重连/时间戳重置。`network` 快照含状态、连接/运行时长、payload 码率、空闲时间、失败预算和最近错误。payload 不含协议开销，策略丢弃的有效包仍计入，码率分母含重试等待；收包不等于已播放。统一状态口径见[网络可靠性](network-reliability.md)，线程异常通过 `checkError()` 传播。
