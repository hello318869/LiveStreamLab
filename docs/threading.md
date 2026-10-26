# M14 多线程流水线与缓冲队列

实现对应上级《方案.md》M14.1–M14.6。通用队列位于
`src/common/queue/BoundedBlockingQueue.hpp`；线程编排分别位于
`src/publisher/pipeline/PublisherPipeline.cpp` 和 `src/player/pipeline/PlayerPipeline.cpp`。

## 通用队列的关闭约定

`lsl::queue::BoundedBlockingQueue<T>` 使用 `std::mutex` 和两个
`std::condition_variable`，支持多个生产者与消费者、最大容量，以及可移动的独占媒体对象。

| 操作 | 行为 |
|---|---|
| `push(T)` | 满时等待；关闭后返回 false，按值接管的对象随调用结束释放 |
| `pop(T&)` | 空时等待；关闭但仍有数据时继续读取，排空后返回 false |
| `tryPush(T&)` / `tryPop(T&)` | 不等待；写入失败时保留调用者的对象 |
| `pushFor` / `popFor` | 等待至超时、可操作或关闭；返回是否交接成功 |
| `tryPopIf` | 在锁内检查队首，满足谓词才取出；谓词不得重入队列或保留引用 |
| `close()` | 禁止新写入，允许排空，唤醒全部读写等待者 |
| `abort()` | 关闭并丢弃剩余对象，唤醒全部等待者；媒体析构在队列锁外完成 |
| `clear()` | 丢弃缓存但不关闭，唤醒等待容量的写入者 |
| `stats()` | 一致快照：当前大小、容量、历史最大积压、入队/出队/丢弃数和关闭状态 |

关闭和中止均幂等，队列不能重新打开。`clear()` 不代表新媒体代次，代次由调用者检查。
销毁队列前必须停止并 join 全部使用者。队列传递 `FramePtr`、`PacketPtr` 和拥有编码参数的事件，
不借用上游临时裸指针。

## 主播端线程与背压

```text
视频采集线程 → 原始视频有界队列 → 视频预处理/H.264 编码线程 ─┐
音频采集线程 → 原始音频有界队列 → 音频预处理/AAC 编码线程 ──┤
                                                         ↓
文件模式：单一解复用/输入解码线程按轨分发             输出事件有界队列
                                                         ↓
                                               唯一本地封装/输出线程
                                               ├─ MP4 / M4A 文件
                                               └─ RTMP 有界编码包队列
                                                         ↓
                                               唯一 FLV / 网络发送线程
```

每个 `MediaInput` 的打开、读取、输入解码、关闭均留在所属采集线程。
其他线程只能调用其 `requestStop()`，不会并发释放正在使用的 FFmpeg 上下文。
文件模式只打开一个输入，不让音视频线程并发读取同一个 `AVFormatContext`。
两路预处理器与编码器分别归属各自处理线程；两个本地文件上下文归属唯一输出线程；
FLV 上下文和网络写操作归属 `RtmpPublisher` 内部唯一线程。

输出队列承载 Start、编码参数、独占编码包和 End 事件。实际轨道的 Start 先于媒体数据；
每轨参数先于包，包先于 End。音频先与 AAC 编码器协商实际 PCM 格式和帧长；
仅在首次实际预处理输出后创建对应文件，支持缺少音轨或视频轨的文件。
同时本地保存与推流时，使用独立 `AVPacket` 引用交给两个输出路径。

| 缓冲 | 默认限制 | 满/过期时的处理 |
|---|---|---|
| 原始视频 | 8 帧，实时排队年龄 200 ms | 实时输入满时丢最旧原始帧；消费时过期且有后继帧则丢旧；非实时文件转换等待容量 |
| 原始音频 | 32 块 | 等待容量，保留整块 PCM，不随意删除样本 |
| 编码输出事件 | 两轨合计 256 个事件 | 等待容量，不任意丢弃本地录制编码包 |
| RTMP 待发送编码包 | 两轨合计 256 包 | 整体恢复：清积压、等待视频关键帧，音频跟随恢复时间边界 |

前三项容量和原始视频年龄通过 `pipeline` 配置，RTMP 缓冲通过 `output.queue_capacity` 配置。
视频优先在编码前丢弃，避免破坏 H.264 参考链；音频背压可能传播到文件解复用线程，
这是保留音频连续性与有界内存的取舍。严重网络积压允许两轨统一恢复，不能仅删除一个 P/B 包。
这些容量限制约束交接队列，不是进程总内存上限；输入解码、重采样 FIFO、编码器和批次另持有媒体资源。

主播端每秒记录当前队列大小、容量、`video_high`、`audio_high`、`output_high` 和
`dropped_raw_video`；RTMP 记录 `queued_video`、`queued_audio`、`queued_packets`、`queue_high`。
峰值在一次运行内累计，不因清理缓存而清零。

## 观众端线程与同步恢复

网络线程独占输入上下文，视频和音频工作线程分别独占各自解码器。
主线程消费已解码视频帧，负责 SDL 事件、同步调度、纹理上传和渲染。
音频线程产生固定格式 F32 PCM，经有界 SPSC 环交给 SDL 音频流回调；
回调不使用通用阻塞队列，不等待视频，不执行耗时日志。

视频交接用通用队列承载独占帧，外层 `VideoFrameQueue` 保留媒体专属策略：
默认最多 3 帧、64 MiB，主线程另持有至多一帧待显示。满时背压视频解码线程，
出队、换代和停止会唤醒等待，等待中每 10 ms 检查网络代次，避免阻塞后无法处理 Reset。
主线程按播放时钟丢迟到帧，阈值为 `min(sync.drop_threshold_ms, pipeline.max_video_age_ms)`；
不按解码后的墙钟年龄或队列 PTS 跨度删除尚未到显示时间的帧。解码后视频帧可丢弃，不影响解码器参考帧链。
外层策略锁到通用队列锁的方向固定，SDL 主线程不做阻塞式入队或出队。

压缩包队列保留 `PacketBuffer`：每轨数量由 `input.queue_capacity` 控制，
两轨共用 `input.max_queue_bytes` 与 `input.max_buffer_ms`。这层需协调
Reset/Packet/End 事件、`serial` 与关键帧恢复，不能直接替换成没有媒体语义的普通队列。
严重积压、断线重连和时间戳跳变清理两轨，以同一 `serial` 撤销旧包、解码帧、PCM 和同步基准。

PCM 环由 `audio.buffer_ms` 限制，至少 4096 个每声道样本帧。
环满仅等待音频处理线程；停止通知会唤醒等待，换代也会被短周期检查。
M13 音频估计播放时钟、Space 音画同步暂停、欠载时外部时钟降级保持生效；
该时钟仍不等于扬声器精确物理播放位置。

日志新增 `frame_queue_peak`、`frame_peak_bytes`、`packet_queue_video_peak`、
`packet_queue_audio_peak`、`packet_peak_bytes` 和 `audio_queue_peak`。
PCM 峰值单位为每声道样本帧，字节与媒体帧数不可直接相加。

## 启动、停止与异常清理

信号处理器只写无锁原子停止标志，主线程负责唤醒与回收；
简单运行标志使用原子变量，共享异常、复杂统计和队列状态受互斥锁保护。

主播端正常 EOF：输入关闭原始队列 → 处理线程排空队列、预处理器和编码器 →
各轨发送 End → 最后一个处理线程关闭输出队列 → 输出线程完成本地文件尾部 →
RTMP 在总计 `write_timeout_ms` 预算内排空/写尾 → join 全部线程。
禁用轨也会结束，避免缺少消费者或生产者导致等待。

Ctrl+C：中断采集与网络 I/O，关闭原始队列唤醒被背压的生产者；
处理线程继续排空已入队媒体并完成本地录制，网络尾包允许丢弃。
本地磁盘写入使用同步 FFmpeg 文件 I/O，文件系统永久卡住不属于网络中断回调可解决的范围。

主播端工作线程发生异常时，在异常产生处保存首个错误并立即通知全部阶段：
停止输入、abort 所有交接队列、中断 RTMP，再由主线程 join 并传播异常。
中途创建线程失败同样取消并回收已经启动的线程。
文件尾部只在正常排空路径尽力完成，故障路径不承诺输出文件完整。

观众端先向网络、音频和视频线程全部发送停止，再统一 join，最后关闭 SDL 音频流和视频资源。
网络读写由 `AVIOInterruptCB` 的停止标记与截止时间中断，重试等待也能被唤醒。
正常 EOF 且禁用重连时允许解码尾部排空；主动停止与换代不继续播放旧尾部。
音频流与回调 userdata 存活至工作线程结束；不可在持 stream 锁时反向获取 device 锁。

## 本次构建与验收边界

2026-10-26 使用直接 CMake 命令，保持测试关闭，不新增构建脚本：

```powershell
cd D:\直播\LiveStreamLab
cmake -S . -B build/cmake -DLSL_BUILD_TESTS=OFF -DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON
cmake --build build/cmake --config Release --target publisher --parallel 1 -- /nr:false
cmake --build build/cmake --config Release --target player --parallel 1 -- /nr:false
```

以上 CMake 配置及 Windows Release 两端编译、链接通过。
本次未运行测试、未启动程序、未采集设备、未连接 SRS。
静态检查和编译不能证明运行中绝无死锁、数据竞争或泄漏；
真实退出、长时间积压、音画同步和网络恢复仍待用户要求运行验收时验证。

相关配置见 [配置管理](configuration.md)，网络恢复见 [M07](rtmp-streaming.md)、
[M09](network-demuxing.md)，同步时钟口径见 [M13](av-sync.md)。
