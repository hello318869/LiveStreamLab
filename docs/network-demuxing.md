# M09 网络拉流与解封装

M09 从 RTMP/RTMPS 或 HTTP/HTTPS-FLV 地址读取直播数据，解析媒体流信息，并把视频、音频
`AVPacket` 分别送入有界队列。实现位于 `src/player/demuxer`，应用编排位于
`src/player/pipeline/PlayerPipeline.*`。当前 `player` 已通过 M10 解码视频，交给 M12 的 SDL3 窗口显示。
M11 的独立音频线程解码音频包并经 SDL3 音频回调输出；关闭音频时消费后释放。
M13 已接入音频主时钟同步；不保存媒体文件。音频接口见 [M11 说明](audio-playback.md)，
同步与降级见 [M13 说明](av-sync.md)。
视频链路、窗口操作和帧缓冲见 [M10/M12 说明](video-playback.md)。

M09 阶段已通过 Windows Release 的 `player` 编译、链接；当时测试保持关闭，未启动程序、未运行测试、
未连接 SRS。本次 M10/M12 已通过 CMake 配置与 Windows Release 的 `player` 编译、链接，
未启动程序、未运行测试、未连接 SRS，成功命令见 [M10/M12 说明](video-playback.md)。
历史构建记录不代表此次真实解码播放和断线恢复已经验证。下面启动命令未执行。

## 构建和启动

在已安装 C++17 编译器与 FFmpeg 开发 SDK 的环境中，直接使用 CMake：

```powershell
cd D:\直播\LiveStreamLab
cmake -S . -B build/cmake -DLSL_BUILD_TESTS=OFF -DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON
cmake --build build/cmake --config Release --target player --parallel

# SRS 已有对应直播流时，接收 HTTP-FLV 并显示视频
.\build\cmake\bin\Release\player.exe --config config\player.json

# 改为同一路 RTMP
.\build\cmake\bin\Release\player.exe --url rtmp://127.0.0.1/live/livestream

# 调整连接/读取超时及重连预算
.\build\cmake\bin\Release\player.exe --connect-timeout 5000 --read-timeout 5000 --reconnect-attempts 5 --reconnect-delay 1000

# 调整接收缓冲，每轨 256 包、两轨合计 32 MiB、最多 2 秒
.\build\cmake\bin\Release\player.exe --packet-queue-capacity 256 --max-buffer-ms 2000 --max-queue-bytes 33554432
```

2026-10-25 M09 阶段本机实际成功的构建命令如下；当时 CMake 缓存中 `LSL_BUILD_TESTS=OFF`、
`CMAKE_DISABLE_PRECOMPILE_HEADERS=ON`：

```powershell
cmake --build build/cmake --config Release --target player --parallel 1 -- /p:BuildInParallel=false /nodeReuse:false
```

SRS 在另一台机器时，将 URL 主机替换为服务器地址。`--check-config`、`--print-config`
和 `--help` 仍单独执行配置动作，不启动网络连接。HTTPS/RTMPS 是否可用还取决于所用 FFmpeg
SDK 的协议支持与服务器配置。

## 连接、流信息和所有权

`Demuxer` 启动独立网络线程，在线程内创建并独占 `AVFormatContext`：

```text
avformat_open_input
  → avformat_find_stream_info
  → 选择视频流和音频流，复制 codecpar 与 time_base
  → av_read_frame
       ├─ video stream_index → 视频队列
       └─ audio stream_index → 音频队列
```

M09 解封装支持只有音频或只有视频的直播；当前 M10/M12 播放入口要求存在视频流，
纯音频输入由 M11 播放声音，窗口保持黑色。其他流的数据包不送入音视频队列。
编码格式、分辨率、采样率和声道等参数来自输入流，使用 `StreamInfo` 快照交付给下游。
快照拥有深拷贝的 `AVCodecParameters`，输入上下文关闭后仍然有效；下游不持有借用的 `AVStream`。
队列移动交接 `PacketPtr`，每个 `AVPacket` 结构只有一个所有者；需要分发给多个消费者时，应
另行使用 `media::referencePacket()` 创建独立包引用。

连接和探测使用 `input.connect_timeout_ms`，单次读取使用 `input.read_timeout_ms`。
FFmpeg 中断回调检查停止标志及当前 I/O 截止时间；重试等待也可以中断。
`Ctrl+C` 由主线程转为停止请求，网络线程负责释放自己的上下文，退场时等待线程结束。
`start()` 只允许调用一次；停止操作可重复调用。

## 缓冲和积压处理

`PacketBuffer` 分别保存视频和音频包，使用同一把锁保护两路队列、流快照、事件顺序和代次。
生产者不等待解码器，以免网络数据在应用外继续积压。接收缓存同时限制：

- 每轨包数：`input.queue_capacity`，默认 256。
- 两轨包数据和 side data 总字节数：`input.max_queue_bytes`，默认 33554432。
- 每轨媒体时间跨度，以及最旧包的墙钟排队年龄：`input.max_buffer_ms`，默认 2000 ms。

入队和出队都会检查时长上限。即使输入包没有可用 DTS/PTS，墙钟年龄仍限制排队时间。
超过上限时清空两路旧包、递增 `serial`、交付 `Reset`，并等待下一视频关键帧重新开始。
等待期间丢弃音视频包；关键帧到来后，丢弃时间戳早于关键帧的音频，避免旧音频拖延恢复。
只有音频的输入不等待视频关键帧。单包大于字节上限也会丢弃并重置预测链。
此策略会造成内容跳跃，目标是在直播中限制积压；不适合作为完整无损文件读取器。

`BufferStats` 提供当前视频/音频包数、总字节数、缓冲时长和 `serial`，并累计接收、丢包、
重置与超限次数。`DemuxStats` 另外统计两路已读包数、忽略包数、流量、连接、重连和时间戳重置。
应用日志报告这些指标；收到包只证明接收链路，不能据此认定解码或播放正常。

## 重连和时间戳

网络故障后先清空旧队列，按 `input.reconnect_delay_ms` 等待，再重新打开输入。
`input.reconnect_attempts` 是连续故障的重试预算；成功收到媒体包后重置预算。
每次新连接重新解析媒体流，流编号、编码参数或时间基可以变化，新的快照随 `Reset` 交付。
默认将网络 EOF 当作直播断流并重连。设置 `--reconnect-attempts 0` 后，正常 EOF 允许缓存排空
并以 `0` 退出；其他网络错误以 `1` 退出。配置错误仍以 `2` 退出。

包内原始 PTS、DTS 和流时间基保留，`timestampUs` 仅供监控使用。每轨优先观察 DTS，无 DTS
时使用 PTS；超过 `input.discontinuity_threshold_ms` 的明显时间戳跳变触发队列重置。
跨连接的时间戳不强行拼接，重连后由新的代次重新建立播放时间轴。

## 下游接入约定

`Demuxer::pop(track, timeoutMs)` 返回 `PacketEvent`；每轨允许一个消费者。
当前 `VideoWorker` 与 `AudioPlayback` 分别消费视频和音频事件，每次无包最多等待 10 ms。
音频 PCM 环满时仅音频线程短暂等待并持续检查停止/代次；视频线程仍可继续解码。
禁用音频播放时，音频线程继续及时消费并释放包，避免音频积压触发全局换代。
视频解码输出通过有界原始帧队列交给 SDL 主线程，窗口事件不在网络或解码线程处理。

| 事件 | 下游动作 |
|---|---|
| `Reset` | 保存 `serial` 与 `streams`；清理旧解码器、解码帧队列和播放时钟，按新参数重新建立状态 |
| `Packet` | 根据 `track` 和快照的流时间基处理 `packet`，保留其 `serial` |
| `Timeout` | 暂时没有事件，继续处理停止或应用控制 |
| `End` | 本轨缓存已排空；先检查网络错误，仅正常 EOF 且禁用重连时刷新解码尾帧后结束 |
| `Stopped` | 立即停止，不继续播放尾部积压 |

重连、积压清理和时间戳跳变都会产生新 `serial`。某个消费者可能已取出旧包，因此 M10
除了清空包队列，还会丢弃旧 `serial` 的包、解码输出和主线程待显示帧，重新建立解码器与视频时钟。
主线程撤下旧画面；M11 同时清除旧代次的解码延迟、PCM 环和 SDL 流，M13 按同一代次重建同步基准。
`StreamInfo.session` 标识输入连接，`serial` 标识队列与播放状态代次，一次连接可以发生多次重置。
M09 本身不拥有解码器，通过事件明确下游的清理和重建边界。

`finish()` 的队列行为是排空后交付 `End`；`stop()` 立即清空并唤醒消费者。
停止和队列结束可重复调用。`Demuxer::checkError()` 将网络工作线程失败重新抛给应用顶层，
避免工作线程异常静默丢失。
