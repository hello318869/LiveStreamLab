# 推流全链路总览：采集 → SRS

一页看完直播推流这条线：有几个线程、几个队列、数据在每一段是什么形态、会在哪里丢、坏了怎么恢复。

---

## 1. 全景图

```text
[采集线程 v]──▶ videoFrames(8)  ──▶ [视频线程: 预处理+编码] ─┐
                                                             ├─▶ output(256) ─▶ [输出线程]
[采集线程 a]──▶ audioFrames(32) ──▶ [音频线程: 预处理+编码] ─┘                    │ submit()
                                                                                 ▼
                                                          track.queue(两轨共 256) ─▶ [RTMP 线程]
                                                                                 │
                                                          take() → 重定基 → 交织 → write()
                                                                                 ▼
                                                    av_interleaved_write_frame → FLV tag → RTMP chunk → TCP → SRS
```

关键结构特征：

- **每两个处理阶段之间有一个队列**，唯一例外是"预处理 → 编码"之间没有（同一线程顺序调用）
- **只有 RTMP 线程碰网络**，其他所有线程都不会阻塞在 socket 上
- **两个生产者、一个发送者**：视频/音频各自编码，RTMP 线程按 dts 交织成一条流

---

## 2. 线程与队列

### 线程（6 个媒体线程 + 1 个协调线程）

| # | 线程 | 数量 | 职责 | 创建位置 |
|---|---|---|---|---|
| 1 | 采集(video) | 1 | `readFrame()` 取帧 → push | `PublisherPipeline.cpp:725` |
| 2 | 采集(audio) | 1 | 同上（麦克风/系统回采） | 同上 |
| 3 | 视频线程 | 1 | 预处理 + H.264 编码 | `PublisherPipeline.cpp:568` |
| 4 | 音频线程 | 1 | 重采样/FIFO + AAC 编码 | `PublisherPipeline.cpp:637` |
| 5 | 输出线程 | 1 | 消费 `output`，分发到 RTMP | `PublisherPipeline.cpp:513` |
| 6 | RTMP 线程 | 1 | 交织 + FLV 封装 + socket 写 + 重连 | `RtmpPublisher.cpp:664` |
| — | 协调线程 | 1 | join、每 50 ms 轮询 `checkError()`，不碰媒体 | `PublisherSession.cpp:62` |

例外：**文件源时采集线程只有 1 个**（一个线程解复用后分流两轨），设备源才是 2 个。

### 队列（4 个有界对象 / 5 条物理 FIFO）

| # | 队列 | 容量 | 元素 | 生产者 | 消费者 | 满时策略 |
|---|---|---|---|---|---|---|
| ① | `videoFrames` | 8 | `RawFrame` | 采集线程 | 视频线程 | 实时：**丢最旧**，采集不阻塞 |
| ② | `audioFrames` | 32 | `RawFrame` | 采集线程 | 音频线程 | **阻塞背压**，绝不丢 PCM |
| ③ | `output` | 256 | `OutputEvent` | 编码线程 | 输出线程 | **阻塞背压** |
| ④ | `track.queue` | 两轨共 256 | `Item` | 输出线程 | RTMP 线程 | **全清 + 等新关键帧** |

④ 实现上是 `std::array<Track,2>`，每轨一个 `std::deque`，共享一个容量预算。

统一实现见 `src/common/queue/BoundedBlockingQueue.hpp`：`std::deque` + mutex + 双条件变量，容量构造时固定。
`close()` = 正常停止、允许排空；`abort()` = 丢弃剩余并唤醒所有等待者，媒体资源在锁外析构。

### 各阶段的容错姿态对比

| 边界 | 协议 | 满/坏时 |
|---|---|---|
| ① 视频原始帧 | 丢最旧，主动丢 | 保延迟，可观测计数 |
| ② 音频原始块 | 阻塞背压 | 保完整，宁可卡住采集 |
| ③ 编码事件 | 阻塞背压 | 保顺序，不允许乱序丢 |
| ④ 推流包 | 全清 + 等关键帧 | 保解码链可恢复 |

**从"保延迟"到"保完整"再到"保可恢复"，四层策略各不相同，这是整条链路最重要的设计。**

---

## 3. 数据形态演变

| 阶段 | 形态 | 关键点 |
|---|---|---|
| 设备出帧 | 平台 buffer（NV12/YUYV/MJPEG、PCM） | 格式由**初始化协商**决定，跨设备不统一 |
| 采集出口 | `CapturedFrame` | 自带所有权的拷贝 + 采集时间戳；驱动 buffer 立即归还 |
| 预处理出口 | `AVFrame` | 未压缩，`format/width/height` 由目标（编码器）倒推 |
| 编码出口 | `AVPacket` | 压缩码流，新增 `dts`（不再只有 `pts`） |
| 入队③ | `OutputEvent{ .packet }` | 信封带**类型标签**，与 Start/Parameters/End 共用有序通道 |
| 出队③ → `submit()` | 仍是 `AVPacket` | 包不动，另算 `dtsUs` 副本存进 Item |
| 入队④ | `Item{ packet, dtsUs, queuedAtUs }` | 多挂两个 `int64`；均为**副本**，不改包 |
| 出队④ → `write()` | `AVPacket`，时间字段被改 | rescale + 重定基 + stream_index；**码流字节不动** |
| `av_interleaved_write_frame` | FLV tag → RTMP chunk → 字节 | 真正的字节级变形在这里发生 |

两次入队是**换信封**而非叠加：`AVPacket` 自始至终是同一个对象，被 `std::move` 三次，底层码流零拷贝。

---

## 4. 三个时间尺度（复杂度的真正来源）

| 尺度 | 在解决什么 | 对应机制 |
|---|---|---|
| 毫秒 | 两轨的包按 dts 正确交错 | `take()` 里 `av_compare_ts` 选轨 |
| 秒 | 这段流能否从任意入点开始解码 | `awaitingKey`、`cutoffUs`、`sessionOriginUs` |
| 十秒~分钟 | 会话是否还活着、要不要重建 | `recover()`、退避、`inactive()` |

数据格式层面的事几乎不复杂；复杂度全部来自"时间"和"故障"。

---

## 5. 时基变换链

```text
编码器时基 {1, fps}  →  FLV 毫秒  →  RTMP（无时基，就是毫秒整数）  →  播放端流时基
              write() 里 rescale            线上传输                 demuxer 里 rescale
```

- **延迟定基**：队列里存的是编码器时基下的原始 dts，重定基留到 `write()` 最后一刻
- 原因：`sessionOriginUs` 要等**关键帧确定**才知道；提前改写会导致重连后无法重新归零
- 重连后从新关键帧的 dts 重新起算，播放端才不会看到巨大的时间跳变

---

## 6. 六处可丢点（排障时按此顺序查）

| # | 位置 | 计数 | 触发条件 |
|---|---|---|---|
| 1 | 采集 → `videoFrames` | `rawVideoDrops` | 队列满，丢最旧 |
| 2 | 视频线程出队时 | `rawVideoDrops` | 实时输入且排队年龄 > `max_video_age_ms`(200)，且队列仍有剩余 |
| 3 | 预处理内部 | `preprocessVideoDrops` | `pts <= lastPts`（时间戳回退） |
| 4 | `output` 满 | 反压 | 不丢，阻塞编码线程 |
| 5 | `submit()` 时队列溢出 | `droppedPackets` | 超 256 → `clearQueues()` 全清 |
| 6 | RTMP 等关键帧 / 越过恢复点 | `droppedPackets` | `awaitingKey` 期间的非 key 包、`dts < cutoffUs` 的包 |

音频的隐性丢点在**驱动环形缓冲**里（背压导致采集回调被拖住），应用层统计不到。

---

## 7. 失败与恢复

### 三重超时，全部通过 `AVIOInterruptCB` 生效

| 超时 | 默认值 | 作用 |
|---|---|---|
| `connect_timeout_ms` | 5000 | 建连阶段 |
| `write_timeout_ms` | 5000 | 单次 socket 写；收尾排空也用这个预算 |
| `inactivity_timeout_ms` | 15000 | 无成功媒体发送的时限 |

### 可重试判定（很克制）

```text
recoverable || AVERROR_EOF || AVERROR_EIO || (AVERROR_EXIT && 超时)
```

配置/内存/协议错误**不重试**，直接上抛终止整条流水线。

### 恢复动作（顺序固定）

1. `clearQueues()` —— 重连期间产出的包全部丢弃
2. `closeSession(false)` —— **不写 trailer**，避免在旧上下文上继续写（连接上可能有半个 FLV tag）
3. 指数退避 `1s → 2s → 4s → … → 30s` 上限，最多 `reconnect_attempts`(5) 次；`-1` 为无限
4. 重置 `recovering / awaitingKey / hasSessionOrigin`
5. 回到 `take()` **等新的关键帧**，用它重建时间原点

> 取舍：宁可在断点处黑屏几秒，也不发一帧无法独立解码的 P 帧。

### 收尾兜底

```text
submittedPackets != 0 && sentPackets == 0  →  报错"ended without publishing any encoded packets"
```

避免"看起来在推流、实际一个包都没发出去"却静默成功。

---

## 8. 延迟预算

排队本身不是延迟，**只有消费者变慢时队列才涨**。稳态下各队列水位接近 0，延迟主要来自：

| 来源 | 量级 | 可控性 |
|---|---|---|
| 驱动 buffer | 1~2 帧 | 低（设备参数） |
| 预处理 + 编码 | 1~2 帧 | 中（`preset`、`lowLatency`） |
| 队列堆积（异常时） | 0 → 数百 ms | 高（容量 + 丢弃策略） |
| 网络 RTT/2 | 数 ms ~ 数十 ms | 低 |
| 内核 socket 缓冲 | 弱网时数百 KB | **不可观测** |

默认编码参数（`Config.hpp`）：1280×720、25 fps、2 Mbps、GOP 50（= 2 秒）、`lowLatency=true`（禁用 B 帧、zerolatency）。

- **无 B 帧** → `pts == dts`，FLV 的 CompositionTime 恒为 0（那段 24 位范围校验是为非低延迟模式留的）
- **GOP 2 秒** → 重连后最坏等待约 2 秒就能等到关键帧，这是"断流恢复时间"的下界

---

## 9. 四个关键设计取舍

| 取舍 | 选择 | 代价 |
|---|---|---|
| 预处理与编码 | **同线程融合** | 编码器零锁、异常直接冒泡；但无法区分"编码慢"还是"预处理慢" |
| 视频/音频满队列 | 视频丢旧、音频背压 | 保住了 PCM 不破音；但音频的丢发生在驱动层，不可观测 |
| 时间戳重定基 | **延迟到 write()** | 支持重连归零；但 `submit()` 里必须多算一个 `dtsUs` 副本 |
| 推流队列满 | 全清 + 等关键帧 | 保住解码链；代价是断点处必须黑屏换 IDR |

---

## 10. 排障速查

| 现象 | 先看 | 常见原因 |
|---|---|---|
| 画面卡顿、声音正常 | `rawVideoDrops`、`preprocessVideoDrops` | 视频线程慢（编码参数过重 / CPU 抢占） |
| 声音断续 | 驱动层（应用无计数） | `audioFrames` 背压 → 采集回调被拖住 |
| 观众端花屏后恢复 | `droppedPackets` + 关键帧日志 | 推流队列溢出后等 IDR，GOP 越长恢复越慢 |
| 断流重连失败 | `networkErrors`、`consecutiveFailures` | 超出 `reconnect_attempts` 或不可重试错误 |
| 连上但观众看不到 | `sentPackets`、`submittedPackets` | 等不到参数/关键帧；收尾会显式报错 |
| 延迟越跑越大 | 各队列 `highWaterMark` | 某段消费者持续慢于生产者 |

---

## 11. 相关文档

- 架构与模块：[architecture.md](architecture.md)
- 线程模型：[threading.md](threading.md)
- 队列与交接：[threading.md](threading.md)
- RTMP 细节：[rtmp-streaming.md](rtmp-streaming.md)
- 网络可靠性：[network-reliability.md](network-reliability.md)
- 延迟：[latency.md](latency.md)
- 采集：[capture.md](capture.md)
- 预处理：[preprocessing.md](preprocessing.md)
- 编码：[video-encoding.md](video-encoding.md)、[audio-encoding.md](audio-encoding.md)
- 统计口径：[performance-statistics.md](performance-statistics.md)
