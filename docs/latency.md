# M14/M15 后的本机直播延迟优化

日期：2026-10-26。目标是尽量恢复用户报告的约 0.5 秒延迟。本轮不运行测试、程序或推拉流，
不把下面的源码分析和历史日志写成优化后的实测结果。

## 定位依据

对比基线为 Git `cf1e22d`（音视频同步）。M14/M15 及此前故障修复仍是工作区已有改动，继续保留。

既有 `build/repair-validation/rtmp2-player.log` 第 14 行起，压缩视频约 30 包、音频约 56 包，
`buffer_ms` 约 1.2 秒；应用 PCM 环约 23000 / 48000，即约 0.48 秒；解码视频队列为 8 帧。
这些积压持续到该记录末尾，而不是仅在开流瞬间出现。音视频两路并行，不能把视频 8 帧再加到
音频缓存上；同一路的包缓存与解码后缓存可以串联估算。它们是应用内排队情况，不是端到端测量。
同批主播端队列很浅，RTMP 队列峰值只有 3 包；该旧场景优先指向播放侧，不能代表用户当前所有输入。

旧日志开流约 1.6 秒。FFmpeg 的 [流探测源码](https://raw.githubusercontent.com/FFmpeg/FFmpeg/n8.0/libavformat/demux.c)
在需要估算视频帧率时默认观察 20 帧，毫秒时间基会加倍；40 / 25 FPS = 1.6 秒，
与旧记录量级吻合。`fps_probe_size` 可覆盖这个数量。探测期间的包被保留，随后按正常播放速度输出；
M14 的有界背压保护了未到显示时间的帧，也使启动积压更容易持续存在。
这是源码与旧日志支持的主要定位，尚未通过本轮运行验证唯一因果关系。

## 本次调整

| 位置 | 调整 | 目的 |
|---|---|---|
| `player/demuxer/Demuxer.cpp` | `fps_probe_size=0` | 轨道参数完整后不额外等待 FPS 统计 |
| 同上 | INFO 日志增加 `probe_ms` | 区分流信息探测耗时与连接、播放排队 |
| `common/config/Config.hpp`、`player.json`、`IAudioSink.hpp` | PCM 默认容量时长 500 → 100 ms | 收紧解码后允许保留的音频量 |
| `common/config/Config.hpp`、`player.json` | 解码帧队列默认 8 → 3 帧 | 减少显示前允许保留的帧数 |
| `player/decoder/VideoDecoder.cpp` | 只启用 `FF_THREAD_SLICE` 帧内并行 | 避免帧级并行额外保留后续帧 |

保留首关键帧和所有探测包，流参数仍使用原 2 MiB / 3 秒的分析上限及连接超时。
这些是安全上限；正常 H.264 + AAC 两轨参数完整后可以提前结束探测。
不完整、缺轨或特殊输入仍可能探测更久。B 帧仍正常重排，音频继续作为主时钟，
解码帧队列满时仍背压；没有恢复按解码墙钟年龄删除未来帧的旧逻辑。

`input.max_buffer_ms=2000` 继续作为异常积压的两轨恢复阈值，而不是预缓冲目标。
超限会清理两轨并等待关键帧；默认主播 GOP 为 2 秒，直接把这个阈值压得很小可能增加黑屏恢复次数。
本轮不修改主播端 GOP、SRS 或已实现的网络超时、重连与代次清理。

## 使用与检查边界

本轮已通过 CMake 配置和 Windows Release 的 `publisher`、`player` 编译、链接，
`LSL_BUILD_TESTS=OFF`，未构建测试目标、未运行测试或启动程序。实际完成的命令为：

```powershell
cmake -S . -B build/cmake -DLSL_BUILD_TESTS=OFF -DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON
cmake --build build/cmake --config Release --target publisher player --parallel 1 -- /nr:false
```

新默认值同时作用于无配置启动与 `config/player.json`。显式使用其他 JSON 或命令行覆盖时，
检查是否仍设置了 `audio.buffer_ms=500` 或 `pipeline.video_queue_capacity=8`。

```powershell
.\build\cmake\bin\Release\livestreamlab.exe --mode player --cli --config config\player.json

# 沿用其他配置时显式覆盖，--url 可按实际 RTMP / HTTP-FLV 地址填写
.\build\cmake\bin\Release\livestreamlab.exe --mode player --cli --audio-buffer-ms 100 --video-queue-capacity 3
```

以上启动命令本轮未执行。PCM 环至少容纳 4096 个每声道样本；SDL 缓存和设备延迟另计。
帧内并行可能降低部分码流的并行解码能力，实际吞吐量仍需在使用环境确认。
本机 0.5 秒目标不能由容量配置保证，仍受采集、编码、SRS、网络突发、系统调度和音频设备影响。
后续经用户授权实测时，可同时观察 `probe_ms`、`buffer_ms`、`audio_queued`、`frame_queue`，
并对照源画面时间与显示画面时间确认端到端结果。
