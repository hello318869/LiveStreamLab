# 网络可靠性与状态管理

对应《方案.md》M15。`common/utils/NetworkStatus.hpp` 提供状态、统计和退避；
`RtmpPublisher`、`Demuxer` 的所属线程管理实际会话，共享状态锁内不执行网络 I/O。
队列和回收约定见 [线程说明](threading.md)，函数顺序见 [调用说明](function-call-order.md)。

## 状态

| 状态 | 含义 |
|---|---|
| `IDLE` | 未启动或正常结束；每个网络对象只能启动一次 |
| `CONNECTING` | 建连、协商参数/写 FLV 头；主播也可能等待参数或关键帧 |
| `STREAMING` | 会话就绪，不保证远端已经播放 |
| `RECONNECTING` | 撤销旧会话、退避并重建，成功后回到 STREAMING |
| `STOPPING` | 取消/正常 EOF 收尾，禁止重连；迟到成功不能恢复运行 |
| `ERROR` | 致命错误、预算耗尽或收尾失败；清理保留错误状态 |

正常路径为 `IDLE → CONNECTING → STREAMING → STOPPING → IDLE`；
可恢复故障进入 `RECONNECTING`，致命故障进入 `ERROR`。
断线先更新状态/连接计时，再关闭协议。工作线程保存异常，协调层通过 `checkError()` 接收。

## 超时与重试

| 配置 | 默认值 | 预算范围 |
|---|---|---|
| 两端 `connect_timeout_ms` | 5000 ms | 观众 open + 探测；主播连接 + FLV 头 |
| `input.read_timeout_ms` | 5000 ms | 单次读包，含连续 EAGAIN/EINTR；协议关闭 |
| `output.write_timeout_ms` | 5000 ms | 单次写入/关闭；正常 EOF 的总网络排空 |
| 两端 `inactivity_timeout_ms` | 15000 ms | 无有效媒体进展；0 禁用 |

`AVIOInterruptCB` 只读原子停止标记与截止时间，不取状态锁/写日志。
截止中断按超时恢复，主动停止不重试；等待参数、关键帧、包及退避也可取消。
这些是协作式预算，不保证强制中断任意系统调用。

成功写入媒体包才恢复主播故障预算。观众仅将有效、非空、选定轨道的 payload 计入接收量，
只有 `PacketBuffer` 接受的包才续期媒体进展时限；损坏/忽略包、等待关键帧而拒绝的包不续期。
新连接/退避结束会给下一轮媒体等待预算，但连接成功本身不清零连续失败。
低帧率或长 GOP 应相应放宽无媒体时限；禁用它仍保留单次 I/O 超时与取消。

两端配置分别放在主播 `output`、观众 `input`：

- `reconnect_attempts=5`：初次连接之外，最多连续重试 5 次；`0` 禁用；`-1` 持续重试可恢复故障。
- `reconnect_delay_ms=1000`、`reconnect_max_delay_ms=30000`：1、2、4、8、16、30、30 秒；上限不得小于基础值，基础值 0 可立即重试。
- 有效媒体进展后恢复重试预算；计数/倍增饱和处理，等待可中断。

超时、连接拒绝/重置/中断、网络不可达、管道断开及适用服务端错误可重试；
配置、协议、认证/权限、内存等致命错误按具体错误退出。
观众直播 EOF 默认重连；禁用重连的真实 EOF 可排空退出，其他网络失败仍报错。
主播文件正常 EOF 收尾，不重连。

## 媒体恢复边界

主播清故障包并重建 FLV、AVIO、流与封装头；等待新视频关键帧，丢弃早于恢复边界的音频，
重建共同时间轴。编码器与本地录制继续工作；致命故障取消整条流水线。
网络故障期间的内容不保证保留，发送状态未知的当前包不会重发。
EOF 排空有总预算且不重连，停止/故障可丢网络尾包；磁盘故障独立处理。

观众 `PacketBuffer::disconnect()` 在协议关闭前清两轨、撤销旧 `StreamInfo`、递增 `serial`，
交付空快照 Reset，使消费者销毁旧解码状态并清 PCM、画面、时钟。
重连后重新探测流编号、参数、时间基并交付新快照，从可解码边界恢复。
同连接积压/PTS 重置使用 `invalidate()` / `resynchronize()`，保留当前参数快照。
取消或换代不继续播放旧代次延迟帧。

## 网络统计

`stats().network` 为共享 `ConnectionStats` 快照：

| 字段 | 口径 |
|---|---|
| `connections` | 参数/封装头协商成功的连接次数 |
| `reconnectAttempts` | 已安排重试次数，包含建连失败 |
| `consecutiveFailures`、`retryDelayMs` | 连续失败/当前退避，有效媒体进展后清零 |
| `networkErrors`、`lastErrorCode`、`lastErrorMessage` | 累计会话错误和最近错误；恢复后保留最近错误 |
| `bytes`、`averageBitrate` | 编码 payload 字节/bit/s；均值分母包含建连和退避 |
| `uptimeSeconds` | 网络对象 start 到结束的时间 |
| `connectionSeconds` | 当前连接时长；断开后保留上次值 |
| `connectedSeconds` | 各连接时长之和，开始关闭/停止时结束计时，不含退避 |
| `idleSeconds` | 距最近有效媒体进展的时间；新连接从就绪开始计 |

结束后时间快照冻结。正常排空尾包仍计发送量，但不能把 STOPPING 改回 STREAMING。
payload 不含协议头，不等于网卡流量，也不证明远端收到/播放。
主播旧外层 `reconnects` 计成功重建，观众旧外层字段计安排重试；跨端比较应使用
`network.reconnectAttempts`。速率、队列和延迟口径见 [性能统计](performance-statistics.md)。
构建及统一应用用法见 [README](../README.md)。
