# 视频解码与显示

视频按输入流实际参数解码，经[音画同步](av-sync.md)调度后显示。GUI 用 Qt，CLI 用 SDL3 窗口，共用解码、帧队列及 `PlayerPipeline`。构建见 [README](../README.md)，线程和详细流程见[线程模型](threading.md)、[函数调用顺序](function-call-order.md)。

## 使用

```powershell
.\build\cmake\bin\Release\livestreamlab.exe --mode player --cli --config config\player.json
.\build\cmake\bin\Release\livestreamlab.exe --mode player --cli --video-decoder nvdec
.\build\cmake\bin\Release\livestreamlab.exe --mode player --cli --width 960 --height 540 --vsync true
.\build\cmake\bin\Release\livestreamlab.exe --mode player --cli --fullscreen true
```

CLI 的 F/Alt+Enter/双击切全屏，Space 暂停，M/↑/↓ 调声音，Esc/关窗/Ctrl+C 停止。GUI 见[观众控制](player-control.md)。`video.width/height` 仅设置 CLI 初始窗口，不改解码分辨率；默认 1280×720、`vsync=true`、`fullscreen=false`。输入地址与重连见[拉流说明](network-demuxing.md)，全部配置见[配置说明](configuration.md)。

观众界面的「视频解码方式」在连接前选择，平台直播间可以停止播放后切换，再重新连接。配置项 `video_decoder.backend` 与 CLI 的 `--video-decoder` 都支持以下值。

| 模式 | 行为 |
|---|---|
| `software` | 默认使用软件解码。 |
| `nvdec` | 要求兼容的 NVIDIA GPU、驱动、FFmpeg 和视频码流。无法使用 NVDEC 时报告错误。 |
| `auto` | 优先 NVDEC。设备初始化、解码器打开或格式协商失败时使用软件解码。 |

首帧成功解码后，日志中的 `decoding backend=` 显示实际使用的后端。NVDEC 通过 FFmpeg 的 CUDA 硬件上下文解码，帧取回 CPU 内存时保留时间戳、色彩和 SEI，再交给现有帧队列和渲染器。 [FFmpeg 硬件解码示例](https://ffmpeg.org/doxygen/9.0/hw_decode_8c-example.html)

## 解码与帧交接

`src/player/decoder/VideoDecoder.*` 的主要接口：

- `reset(track, serial)`：按快照的 codec 参数、包时间基和所选后端重建解码器，丢弃旧连接延迟帧，等待关键帧。
- `decode(packet, serial)`：send/receive；EAGAIN 先取输出再重送原包，支持一包多帧及延迟输出。
- `flush()`：仅正常 EOF 发送空包排出尾帧。损坏包/帧则刷新解码器并等下一关键帧，严重资源/API 错误向上报告。

软件解码请求 `FF_THREAD_SLICE` 帧内并行，B 帧按码流正常重排。显示时间优先 `best_effort_timestamp`，其次帧 PTS，不用包 DTS 覆盖；时长依次取帧时长、解码器帧率、上帧时长，初值 40 ms。`DecodedVideoFrame` 独占帧结构，带 serial、微秒 PTS 和时长，底层缓冲由引用计数保活。

裁剪后 YUV420P/NV12 可直通，其余支持的软件格式经 libswscale 转 YUV420P，保留时间、色彩范围和像素宽高比，跟随实际尺寸/格式变化。

| 层次 | 限制与处理 |
|---|---|
| 单次解码批次 | 最多 32 帧、128 MiB，超限保留较新输出 |
| `VideoFrameQueue` | 默认 3 帧、64 MiB；`pipeline.video_queue_capacity` 调帧数，满时背压解码线程 |
| 协调循环 | 非阻塞取帧，队列外最多保留一帧待显示 |

等待检查停止与 serial，换代会中断旧帧入队。字节数按 FFmpeg 缓冲保守统计，解码参考帧、临时帧和显示资源另占内存；各层上限不是总内存或延迟保证。**不按解码后的墙钟年龄或队列 PTS 跨度删除提前帧**，避免首帧不断追逐未来画面。迟到判断依据播放时钟：超过 `min(sync.drop_threshold_ms, pipeline.max_video_age_ms)`（默认 100 ms）且有后续帧时可丢弃，不破坏参考链。

## 两种显示后端

CLI 的 `src/player/renderer/SDLVideoRenderer.*` 在应用主线程创建和释放视频资源：

```text
render → 检查平面/pitch → ensureTexture → 必要时转正 pitch
       → SDL_UpdateYUVTexture / SDL_UpdateNVTexture
       → RenderClear → RenderTexture → RenderPresent
```

优先硬件后端，失败尝试 software；vsync 不支持时警告并尝试关闭。纹理跟随尺寸、格式和色彩空间重建，支持独立 pitch、负 pitch 及奇数色度尺寸。结合 SAR 等比留黑，窗口缩放/全屏时可重绘旧纹理。BT2020 NCL 依据帧元数据区分 SDR/PQ/HLG；不提供 HDR 色调映射或高位深保真，BT2020 CL 与 SDL 无法表示的色度位置报不支持。

GUI 的 `PlayerSession::MailboxRenderer` 在后台同步决定 Render 后转 RGB24，按 SAR 修正并限制在 1920×1080，只替换最新不可变帧。Qt 主线程取副本转 QImage 绘制，不创建 SDL 视频窗口；实际绘制完成后才回报端到端延迟样本。

## 恢复、结束与指标

重连、积压超限或时间戳跳变会换 serial；清包、解码器、帧队列、待显示帧和旧画面，重建时钟。即使 Reset 尚未被消费，旧代次输出也被拒绝。无流快照时只清状态并等待；纯音频输入交给[音频模块](audio-playback.md)，不产生视频帧。

默认 EOF 重连；仅禁用重连时正常 EOF 才排空尾帧。停止/故障/换代丢弃旧内容；所有工作线程收到停止后 join，随后回收输出资源。

每秒日志报告解码/显示 FPS、耗时、纹理上传、各层积压/峰值、丢帧/丢包与错误。解码耗时含像素适配，CLI 显示耗时含 vsync 等待；GUI 后台提交数不等于屏幕刷新率。统一指标见[性能统计](performance-statistics.md)。
