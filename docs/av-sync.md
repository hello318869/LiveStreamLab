# 音画同步

`src/player/sync/AVSynchronizer.*` 通过 `IAudioSink::clock()` 估计音频位置并调度视频，默认音频为主时钟。CLI 在应用主线程调用，GUI 在 Session 后台协调线程调用，Qt 主线程只绘制同步后画面。构建见 [README](../README.md)，流程见[函数调用顺序](function-call-order.md)。

## 时间轴与时钟

视频取 `best_effort_timestamp`，缺失时取帧 PTS；音频以帧 PTS 和输出样本数定位。两轨用 `av_rescale_q()` 换算为微秒，保留原始相对偏移，不能分别把首 PTS 置零。视频缺 PTS 可按时长补时，但不计为可测音画误差。外部时钟用 `steady_clock` 映射媒体时间。

`SDLAudioSink::clock()` 从回调提交终点扣除 SDL 未消费数据和估计设备周期延迟，回调之间平滑推进；欠载停止媒体进度，暂停冻结，serial 换代清基准，同代新时间戳段用 revision 标识。静音/调音量继续消费媒体。此位置不是物理播放指针，SDL 缓冲的扣减口径与设备延迟见[音频播放](audio-playback.md)。

## 调度与恢复

协调循环最多持有一帧 pending，新帧调用 `beginFrame()`，等待期间反复调用 `evaluate()`；窗口事件与停止检查持续运行：

| 条件 | 动作 |
|---|---|
| 同 serial 音频时钟有效且推进 | 用音频为主时钟 |
| 视频提前超过容差 | Wait，保留 pending；已对齐后等待过久且有后继才淘汰候选 |
| 迟到超过阈值且有后继 | Drop 当前已解码帧，不破坏参考链 |
| 到期，或迟到但无后继 | Render |
| 音频未就绪、欠载或时钟停滞 | 有限等待后降级外部时钟；恢复后再跟随音频 |
| 无音轨、禁用声音或音频已结束 | 外部时钟继续推进视频 |

首次对齐时保留提前首帧，避免反复换成更远的未来帧。有效音频超过 `audio_wait_ms` 未推进也会降级。提前候选等待上限使用 `discontinuity_threshold_ms`；迟到阈值实际取 `min(sync.drop_threshold_ms, pipeline.max_video_age_ms)`，默认 100 ms。

重连、包积压和输入时间戳跳变会递增 serial，清旧解码器、帧、PCM、画面和同步基准，参见[拉流恢复](network-demuxing.md)。同代次内，曾经对齐后，视频 PTS 跳变或音频 revision 改变且实际误差超阈值，或大偏差持续一秒，可返回 Resynchronize：共同清缓冲、换代并等关键帧，通常不重建网络连接。只凭视频 PTS 跨度大不能判失步。

正常 EOF 保留排空路径，不再请求已结束解码线程处理 Reset；音频结束后视频走外部时钟。若只剩一帧且异常超前超过跳变阈值，则丢弃以免长期挂起。联合暂停冻结媒体节奏并扣除暂停时长，无 pending 时也维护暂停；长时间直播暂停仍可能因有界缓冲换代而跳内容。

## 参数与指标

| JSON 字段 | 默认值 | 约束 |
|---|---|---|
| `sync.tolerance_ms` | 40 | 1–100 ms |
| `sync.drop_threshold_ms` | 100 | 1–1000 ms，且不小于容差 |
| `sync.audio_wait_ms` | 500 | 100–5000 ms |
| `sync.discontinuity_threshold_ms` | 1000 | 500–10000 ms，且大于丢帧阈值 |

```powershell
.\build\cmake\bin\Release\livestreamlab.exe --mode player --cli --set sync.tolerance_ms=30 --set sync.drop_threshold_ms=100
.\build\cmake\bin\Release\livestreamlab.exe --mode player --cli --audio-paused true
```

`input.discontinuity_threshold_ms` 另管网络包跳变。控制和全部字段见[配置说明](configuration.md)。

每秒日志包含 `master`（audio/external/waiting/paused）、`av_error_ms`、`av_mean_abs_ms/av_max_abs_ms/av_samples`、`sync_waits/sync_drops`、`sync_fallbacks/sync_resets/sync_recoveries` 及 `audio_buffered_ms/audio_device_latency_ms`。

误差方向为“视频 PTS − 音频估计位置”，正值提前；无有效比较时为 n/a。每个完成调度且可比较的帧最多采样一次，含丢弃前偏差，等待轮询不重复计数；sync_drops 包含迟到和候选淘汰。日志是调度观测，不是物理声画同步测量，阈值配置也不是实测误差保证。界面指标见[性能统计](performance-statistics.md)。
