# M15 网络可靠性与状态管理

实现对应上级《方案.md》M15.1–M15.5。共享状态、统计与退避工具位于
`src/common/utils/NetworkStatus.hpp`，实际网络会话仍由
`publisher/rtmp/RtmpPublisher` 和 `player/demuxer/Demuxer` 各自的唯一工作线程管理。
M14 队列、媒体所有权和线程停止约定见 [多线程流水线](threading.md)。

## 状态与生命周期

| 状态 | 含义与去向 |
|---|---|
| `IDLE` | 尚未启动，或正常结束/取消后已经收尾；每个网络对象仍只允许启动一次 |
| `CONNECTING` | 首次建立协议连接并协商流参数或写入 FLV 头；主播端也可能等待首批编码参数/关键帧 |
| `STREAMING` | 会话与流参数已就绪；不代表服务器、解码器或扬声器已成功播放 |
| `RECONNECTING` | 可恢复故障，撤销旧连接并退避、重新建立会话；成功后回到 STREAMING |
| `STOPPING` | 用户取消或正常 EOF 收尾；禁止重连，也不允许迟到的连接成功恢复运行状态 |
| `ERROR` | 不可恢复错误、重试耗尽或收尾失败；错误快照保留，后续清理不会改成正常 IDLE |

典型路径为 `IDLE → CONNECTING → STREAMING → RECONNECTING → STREAMING`；
正常停止进入 `STOPPING → IDLE`，致命故障进入 `ERROR`。
断线时先更新状态并结束连接计时，再执行可能阻塞的协议关闭。
工作线程保存异常，主线程通过 `checkError()` 接收，沿用既有顶层日志与退出码边界。

`ConnectionMonitor` 仅保存线程安全的状态和数值，不在共享锁内执行网络 I/O。
中断回调只读取原子停止标记与截止时间，不取状态锁，也不执行日志。
网络上下文只由所属工作线程创建、读取/写入和销毁。

## 超时、取消与无媒体检测

| 配置 | 默认值 | 作用 |
|---|---|---|
| 两端 `connect_timeout_ms` | 5000 ms | 观众端 open + 流探测共享预算，主播端连接 + FLV 头共享预算 |
| 观众端 `input.read_timeout_ms` | 5000 ms | 单次读包，包括连续 EAGAIN/EINTR 重试；协议关闭也设置此预算 |
| 主播端 `output.write_timeout_ms` | 5000 ms | 单次写入/关闭，以及正常 EOF 的总网络排空预算 |
| 两端 `inactivity_timeout_ms` | 15000 ms | 连续没有可用媒体进展时触发超时；0 禁用此项 |

FFmpeg 的 `AVIOInterruptCB` 检查主动停止、当前操作截止时间和媒体进展截止时间；
截止中断统一按超时错误进入恢复策略，主动停止不作为断网重试。
等待包、参数、关键帧或重试间隔时也检查取消；停止会通知条件变量并唤醒工作线程。
这些是协作式中断与超时预算，不是强制终止任意系统调用的硬实时保证。

主播端无媒体检测覆盖等待编码参数、关键帧、包以及实际网络写入；
成功写入媒体包才恢复故障预算。退避结束和新连接建立会给予下一轮媒体等待预算，
但单纯连接成功不清零连续失败次数。

观众端把收到字节与可交付媒体进展分开：有效、非空、选定音视频流的 payload 计入接收量；
只有被 `PacketBuffer` 接受的包才续期媒体进展时限并清零连续失败次数。
忽略流、损坏包、空包，以及等待关键帧时被拒绝的预测包都不能无限延长等待。
低帧率或长 GOP 的流应让此时限大于正常帧/关键帧间隔，或设置为 0。
禁用此项仍保留单次读写超时和用户取消。

## 重试预算与指数退避

两端使用相同参数语义，分别放在主播端 `output` 和观众端 `input`：

- `reconnect_attempts=5`：初次尝试之外，允许连续失败后的最多 5 次重试。
- `reconnect_attempts=0`：不重试；观众端真实 EOF 可排空后正常退出，其他网络失败仍报错。
- `reconnect_attempts=-1`：持续重试可恢复网络故障，直到用户停止；不重试配置、认证或内存等致命错误。
- `reconnect_delay_ms=1000`：第一次退避间隔；之后每次乘 2。
- `reconnect_max_delay_ms=30000`：退避上限，不能小于基础间隔；默认序列为 1、2、4、8、16、30、30 秒。

成功传输可用媒体后恢复重试预算和退避级别，不以 TCP 打开成功单独判断恢复。
乘法和持续重试计数采用饱和处理；等待可中断，基础间隔为 0 时允许立即重试。
超时、连接拒绝/重置/中断、管道断开、网络不可达及适用的服务端错误属于可恢复故障；
协议、参数、权限/认证、内存和媒体上下文初始化失败按具体错误退出。
观众端直播 EOF 默认作为断流重连，主播端正常文件 EOF 则收尾且不再重连。

下面是后续使用方法，本次未执行这些运行命令：

```powershell
# 主播端持续重试，退避最长 30 秒
.\build\cmake\bin\Release\publisher.exe --config config\publisher-stream.json --reconnect-attempts -1 --reconnect-max-delay 30000

# 观众端持续重试；较长 GOP 可适当增大媒体等待时间
.\build\cmake\bin\Release\player.exe --config config\player.json --reconnect-attempts -1 --inactivity-timeout 30000
```

## 断开后的媒体清理

主播端清理故障网络包，关闭并重建 FLV、AVIO、输出流和封装头。
恢复时等待新视频关键帧，并丢弃早于该恢复边界的音频，重新建立共同时间轴。
编码器继续在各自线程工作，通过有界队列限制积压；本地录制继续由独立输出线程处理。
若故障不可恢复，主线程取消整条流水线并回收资源。
错误期间的网络内容不保证保留，也不会把发送状态未知的当前包重复发送。

观众端 `PacketBuffer::disconnect()` 在协议关闭前清空两轨、撤销旧 `StreamInfo` 并递增 `serial`，
通知视频/音频线程销毁旧解码状态、清理 PCM、画面与同步时钟。
此时 Reset 的流快照为空，消费者不会按旧参数重新创建解码器。
重新连接后重新探测流编号、编码参数与时间基，交付新快照，再从可解码边界恢复。
同一连接内的积压/PTS 重置仍保留当前流快照，使用原有 `invalidate()` / `resynchronize()` 路径。

EOF 的编码/解码尾部只在正常结束时排空；取消和换代不继续播放旧代次延迟帧。
主播端 EOF 网络排空仍有总预算，期间不重连；停止/故障时网络尾包可以丢弃。
本地文件故障与网络恢复分开处理，磁盘 I/O 不由网络中断回调取消。

## 状态统计口径

两端的 `stats().network` 为共享 `ConnectionStats` 快照：

| 字段 | 含义 |
|---|---|
| `state` | 当前状态，正常结束为 IDLE，故障结束保留 ERROR |
| `connections` | 流参数/封装头已协商成功的连接次数 |
| `reconnectAttempts` | 已安排的重试次数，包含未能成功建立连接的尝试 |
| `consecutiveFailures` / `retryDelayMs` | 本轮连续失败及当前退避间隔，成功媒体进展后清零 |
| `networkErrors` | 累计会话错误，包含最终错误 |
| `lastErrorCode` / `lastErrorMessage` | 最近错误；恢复成功后保留，方便诊断 |
| `bytes` / `averageBitrate` | 编码 payload 字节及 bit/s，不含 FLV/RTMP/HTTP/TCP 头；平均值分母包含建连和重试等待 |
| `uptimeSeconds` | 此次网络对象从 start 到结束的时间 |
| `connectionSeconds` | 当前连接时长，断开后保留上次连接时长 |
| `connectedSeconds` | 各次连接时长总和，不含重试等待，从开始关闭/停止时结束连接计时 |
| `idleSeconds` | 距最近有效媒体进展的时间；新连接未有媒体时从连接就绪开始计时 |

正常排空阶段成功写入的尾包仍计入发送 payload，不会把 STOPPING 状态改回 STREAMING。
结束后时间快照冻结。实际收发字节不等于网卡流量，不证明远端接收、解码或播放成功。
主播端外层 `reconnects` 保留成功重建次数口径；观众端外层 `reconnects` 保留安排重试次数口径，
跨两端比较时使用共享 `network.reconnectAttempts`，不要混淆旧字段。

两端周期日志包含状态、连接/累计连接时长、退避间隔、连续失败数、最近错误及媒体空闲时间；
观众端还报告接收 payload 码率，主播端沿用成功发送 payload 码率。

## 本次构建与验收边界

直接使用 CMake，保持 `LSL_BUILD_TESTS=OFF`，不新增构建脚本：

```powershell
cd D:\直播\LiveStreamLab
cmake -S . -B build/cmake -DLSL_BUILD_TESTS=OFF -DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON
cmake --build build/cmake --config Release --target publisher player --parallel 1 -- /nr:false
```

2026-10-26 以上 CMake 配置及 Windows Release 两端编译、链接通过。
本次未运行测试、未启动程序、未连接/重启 SRS、未采集设备。
编译检查仅能确认代码与链接；服务器重启、网络中断、长时间稳定性及实际退出时延仍待用户要求运行验收时验证。
