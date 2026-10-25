# M10 视频解码与 M12 SDL3 显示

观众端现已接通 M09 拉流 → M10 视频解码 → M12 SDL3 窗口显示。
输入使用 RTMP/RTMPS 或 HTTP/HTTPS-FLV，视频按输入流的实际编码参数解码，按视频 PTS 控制显示节奏。
M11 音频解码与声音输出已接入独立线程及 SDL3 音频流回调，见 [音频播放](audio-playback.md)。
M13 音画同步仍未实现，视频沿用独立 PTS 节奏；只有音频的输入保留黑色窗口并输出声音。

2026-10-25 已通过 CMake 配置与 Windows Release 的 `player` 编译、链接，
生成 `build/cmake/bin/Release/player.exe`。本次未启动程序、未运行测试、未连接 SRS；
真实解码、显示效果、窗口操作和断线恢复仍需实际运行验收。

## 构建与使用

在工程根目录使用 CMake，无额外构建脚本。以下是本次成功的 Windows 构建命令：

```powershell
cd D:\直播\LiveStreamLab
cmake -S . -B build/cmake -DLSL_BUILD_TESTS=OFF -DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON
cmake --build build/cmake --config Release --target player --parallel 1 -- /nodeReuse:false
```

首次并行构建在 MSBuild 的 `Checking File Globs` 后未继续推进，已中断；上述串行、
禁用节点复用的命令成功。`/nodeReuse:false` 是 Windows MSBuild 参数；其他生成器可使用
`cmake --build build/cmake --config Release --target player --parallel`。

先准备正在推流的 SRS，再执行下面的启动命令；本次未执行：

```powershell
# 默认 HTTP-FLV 输入
.\build\cmake\bin\Release\player.exe --config config\player.json

# 同一路 RTMP 输入；SRS 在其他机器时替换 127.0.0.1
.\build\cmake\bin\Release\player.exe --url rtmp://127.0.0.1/live/livestream

# 初始窗口 960x540，保留垂直同步
.\build\cmake\bin\Release\player.exe --width 960 --height 540 --vsync true

# 直接全屏启动
.\build\cmake\bin\Release\player.exe --fullscreen true

# 禁用重连；正常 EOF 时排空包队列与解码尾帧后退出
.\build\cmake\bin\Release\player.exe --reconnect-attempts 0
```

F、Alt+Enter 或鼠标左键双击切换全屏；Esc、关闭窗口或 Ctrl+C 停止。
窗口可自由缩放，视频结合像素宽高比等比显示并留黑。
`video.width` / `video.height` 仅设置初始窗口大小，不改变解码分辨率；
`video.vsync` 默认 `true`，`video.fullscreen` 默认 `false`。完整配置见 [配置说明](configuration.md)。

## 模块和线程

| 位置 | 职责 |
|---|---|
| `src/player/decoder/VideoDecoder.*` | 解码器初始化、视频包解码、显示格式适配、解码统计 |
| `src/player/renderer/IVideoRenderer.hpp` | 事件、显示、清屏和渲染统计接口 |
| `src/player/renderer/SDLVideoRenderer.*` | SDL3 视频子系统、窗口、纹理和渲染后端 |
| `src/player/pipeline/PlayerPipeline.*` | 网络/解码生命周期、原始帧队列、视频显示时序和日志 |

```text
网络线程：AVFormatContext → av_read_frame → 视频/音频 PacketBuffer
解码线程：视频包 → VideoDecoder → DecodedVideoFrame → VideoFrameQueue
音频线程：音频包 → AudioDecoder → swresample / F32 → SPSC 环 → SDL3 音频回调
主线程：  SDL 窗口事件 → 取待显示帧 → 视频 PTS 调度 → IVideoRenderer → SDL3
```

网络线程独占输入上下文，解码线程独占视频解码上下文。所有 SDL 视频操作和资源释放都在主线程执行。
`DecodedVideoFrame` 独占 `AVFrame` 结构，携带 `serial`、微秒媒体 PTS 和帧时长，
像素缓冲通过 FFmpeg 引用计数保活。每层在换代时撤销旧资源与输出，不借用输入上下文中的 `AVStream`。

## 解码与显示

`VideoDecoder::reset(track, serial)` 根据快照中的 `AVCodecParameters` 查找软件解码器，
导入流参数和包时间基后打开 `AVCodecContext`。`decode(packet, serial)` 使用 send/receive API：
send 返回 EAGAIN 时先接收输出，再重试同一个尚未接受的包；receive 持续读取，覆盖一包多帧和延迟输出。
`flush()` 通过空包排空正常 EOF 的尾帧。

帧时间戳优先采用 `best_effort_timestamp`，其次采用帧 PTS，不以包 DTS 覆盖显示时间。
帧时长依次使用解码帧时长、解码器帧率、上一帧时长；初始缺省为 40 ms。
YUV420P/NV12 软件帧可直接交接，其他支持的软件格式通过 libswscale 转为 YUV420P，
保留时间信息、色彩范围和像素宽高比。帧尺寸和像素格式变化按实际输出处理。
损坏包或损坏帧触发解码器刷新，等待下一关键帧恢复；严重初始化、资源或 API 错误向顶层报告。

SDL3 优先尝试硬件渲染后端，失败后尝试 software；垂直同步不受支持时记录警告并尝试关闭。
YUV420P 使用 `SDL_UpdateYUVTexture`，NV12 使用 `SDL_UpdateNVTexture`；
各平面保留独立 pitch，负 pitch 先复制到正向临时帧，支持奇数尺寸的色度平面。
尺寸、像素格式或色彩空间变化会重建纹理；显示通过逻辑分辨率与 letterbox 保持比例。
窗口缩放、重新显示或全屏切换时，即使没有新视频帧，也会重绘已有纹理。

BT2020 NCL 按帧内的色域、传递函数和色度位置设置 SDL 元数据，避免把 SDR、PQ 与 HLG 混用。
HDR 显示仍取决于 SDL 后端和设备；本轮未实现 HDR 色调映射或高位深保真，
BT2020 CL 与 SDL 无法表示的色度位置会明确报告不支持。

## 缓冲、时序和停止

网络包缓冲继续沿用 [M09](network-demuxing.md) 的包数、字节数、媒体跨度和墙钟年龄限制。
视频解码后的交接另外限制为：

- 单次解码返回批次最多保留 32 帧、128 MiB，超限丢弃较旧的输出。
- `VideoFrameQueue` 最多 8 帧、64 MiB、500 ms 媒体时间跨度及墙钟排队年龄，超限优先丢弃较旧原始帧。
- 主线程在队列外另持有最多一帧等待显示。

字节统计按所持有的 FFmpeg 缓冲计算，帧队列对共享缓冲采用保守计数。
这些是各层交接限额，不是整个播放器的内存或端到端延迟保证；解码参考帧、转换临时帧与 SDL 纹理另占资源。
丢弃已解码帧不会破坏解码器的参考帧链。

`VideoSchedule` 将视频 PTS 增量映射到单调墙钟；缺失 PTS 时用帧时长补时。
PTS 回退或向前跳变超过 500 ms 时重新锚定，避免等待异常的远期时间戳。
到期后若迟到超过 100 ms 且队列仍有后续帧，丢弃当前帧追赶；没有后续帧时仍显示该帧。
这是独立视频时钟，尚未使用音频主时钟，不构成 M13 音画同步。

重连、包缓冲超限或网络时间戳跳变递增 `serial`；解码线程重开解码器并清空帧队列，
主线程丢弃旧待显示帧、清屏并重置时钟。网络线程已换代而 Reset 尚未被解码线程取到时，
旧代次的包和解码输出同样会被丢弃。
首次连接失败而尚无流参数快照时，Reset 只清理状态并继续等待 M09 重连，
实际探测到输入仅包含音频时，由 M11 输出声音，视频线程保持无帧状态。

默认把网络 EOF 作为断流重连；仅 `--reconnect-attempts 0` 下的正常 EOF 排空解码尾帧后退出。
网络失败先检查并传播错误；停止或换代不会把旧连接的延迟帧刷到窗口。
用户退出时中断网络 I/O、停止帧交接、等待解码与网络线程退出，再在主线程释放 SDL 资源。

每秒日志包含解码/显示 FPS、累计帧数、平均解码与显示耗时、纹理上传次数、帧队列字节数、
各层丢帧/丢包、解码错误、网络缓冲与重连次数。显示耗时包含垂直同步等待；
解码耗时包含 send/receive 和像素适配。编译成功及这些统计接口的存在不代表已取得真实播放性能数据。
