# M13 音画同步

观众端使用音频主时钟控制视频显示。实现位于 `src/player/sync/AVSynchronizer.*`，
由 `lsl_sync` 构建目标提供，主线程的 `PlayerPipeline` 负责调用；声音输出继续通过
`IAudioSink` 接口交接，不让同步模块直接操作 SDL 设备。

2026-10-25 已通过 CMake 配置与 Windows Release 的 `player` 编译、链接。
本次未运行测试、未启动播放器或连接 SRS。以下描述实现策略；实际音画误差、设备延迟、
长时间同步和断线恢复仍需运行验收，不能从编译或阈值配置推断已达到 100 ms 目标。

## 统一时间轴

视频解码器使用帧 `best_effort_timestamp`，缺失时使用 PTS；音频解码器使用音频帧 PTS，
并按输出样本数计算 PCM 块的媒体位置。两者通过 `av_rescale_q()` 将各自流时间基转换为
`AV_TIME_BASE` 对应的微秒，保留同一输入中的原始时间关系。

不分别把首个视频 PTS 和首个音频 PTS 置零，否则会丢掉真实的初始音画偏移。
外部时钟使用 `std::chrono::steady_clock` 把媒体时间映射到单调时间，避免系统时间调整影响节奏。
视频缺少 PTS 时可用帧时长补时，但这种补时不等于取得可测量的音画误差。

## 音频播放位置估计

`SDLAudioSink::clock()` 提供当前 `serial` 的估计播放位置。回调提交的媒体终点仅表示应用已交给 SDL，
同步时还要扣除 SDL 尚未消费的数据和设备周期延迟，并在回调之间平滑推进。
欠载时不能把设备补出的静音计入媒体进度，暂停时保持位置，重连或清缓存时丢弃旧基准。
同代次内出现新的音频时间戳段时，通过 `revision` 通知同步器重建基准。

`SDL_GetAudioStreamQueued()` 是未消费的输入字节数，`SDL_GetAudioStreamAvailable()` 是同一流可读的
转换后输出字节数；不能把两者当成独立缓冲相加。应用 PCM 环中的内容尚未提交给 SDL，
也不能再次从提交终点中扣除。静音和音量调整继续消费媒体，因此不停止时钟。

SDL 未提供本工程所需的扬声器精确播放位置。设备周期是延迟估计，系统混音、驱动和硬件仍可能带来
额外偏差；这里的 `AudioClock` 与同步日志不等于物理声画测量。

## 视频调度与降级

主线程保持最多一帧待显示帧，每次处理窗口事件后重新比较当前帧与主时钟：

| 情况 | 处理 |
|---|---|
| 同代次有效音频时钟 | 使用音频进度作为主时钟 |
| 视频提前超过允许范围 | 保留待显示帧，短等并继续处理事件与换代；等待过久且有后续帧时更新候选 |
| 视频迟到超过丢帧阈值，且有后续帧 | 丢弃当前原始帧追赶；不破坏解码参考帧链 |
| 视频已到期 | 显示当前帧；迟到但无后续帧时仍显示 |
| 初始音频尚未就绪、暂时欠载，或有效音频时钟长期停滞 | 有限等待，超时后使用外部时钟保持视频继续推进 |
| 无音轨或禁用声音输出 | 使用外部时钟 |
| 音频恢复 | 重新跟随当前代次的音频时钟，通过等待或丢帧调整画面 |

默认提前容差为 40 ms，迟到丢帧阈值为 100 ms。小幅抖动在容差内吸收，避免每次抖动都改变播放节奏。
音频有效但媒体位置超过 `sync.audio_wait_ms` 没有变化时，也按不可用时钟降级，
避免设备后端未报告耗尽而让视频一直等待。
提前帧在队列外等待达到 `sync.discontinuity_threshold_ms`，且队列已有后续帧时，
丢弃旧候选并改取后续帧；这类等待淘汰也计入 `sync_drops`。
外部时钟降级不是完成音画同步的保证；音频可用后仍需回到共同媒体时间轴。
正常 EOF 会排空媒体；音频结束后视频尾部可继续使用外部时钟，不会永远等待已结束的音轨。
EOF 后不能再重建解码流水线，若仅剩一帧且它异常超前超过跳变阈值，则丢弃这个孤立尾帧，
避免坏时间戳让退出长期挂起；正常范围内的尾帧仍按节奏显示。

Space 同时暂停或恢复声音与视频；没有音频输出时也可以暂停视频。
主循环每轮观察暂停状态，没有待显示帧的期间也冻结外部时钟；恢复时扣除暂停时长，
不把用户暂停当作网络失步或候选帧等待超时。
`audio.paused` 和 `--audio-paused` 沿用原配置名，现在表示初始音画暂停。
M 静音，↑/↓ 每次调整 10% 音量；这些操作不改变 Windows 系统音量。
直播输入仍保持有界缓冲，长时间暂停可能触发清缓存换代，恢复时会跳到较新的直播内容。

## 网络恢复与统计

M09 在重连、接收积压超限和明显时间戳跳变时递增 `serial`。主线程丢弃旧待显示帧和画面，
解码线程清除旧解码延迟与帧队列，音频输出清除旧 PCM 和 SDL 缓存，同步器重新建立基准。
已经交给系统或硬件的少量音频无法通过 SDL 清缓存撤回。
同代次音频出现新时间戳段时仍重建音频基准；视频相邻 PTS 大跨度可能只是缺帧后正常追上音频，
不能仅凭这个跨度清空两轨。已经对齐后，只有视频 PTS 跳变或音频 `revision` 改变，且当前实际
音画误差也超过 `sync.discontinuity_threshold_ms` 时，才立即请求两轨共同换代。
其他大于跳变阈值的实际音画偏差持续一秒时，也可请求共同换代，清除积压并等待新关键帧。
正常 EOF 已结束解码线程时，
不再请求无法被这些线程处理的 Reset，保留尾部排空路径。

每秒播放器日志记录当前主时钟、音画时间差、误差统计及等待、丢帧、降级、重建情况：

| 日志字段 | 含义 |
|---|---|
| `master` | `audio`、`external`、`waiting` 或 `paused` |
| `av_error_ms` | 当前可比较帧的音画差，无有效比较时为 `n/a` |
| `av_mean_abs_ms` / `av_max_abs_ms` / `av_samples` | 累计平均绝对误差、最大绝对误差与样本数 |
| `sync_waits` / `sync_drops` | 曾经等待的帧数 / 同步调度丢帧数，后者包含迟到丢帧及提前帧等待过久后的候选淘汰 |
| `sync_fallbacks` / `sync_resets` / `sync_recoveries` | 外部时钟降级、代次重置与主动恢复次数 |
| `audio_buffered_ms` / `audio_device_latency_ms` | 已提交媒体中估计尚未播放的时长 / 设备周期延迟估计 |

音画差值以“视频 PTS − 音频估计播放位置”为方向：正值表示视频提前，负值表示视频迟到。
无可比较音频时间戳、缺失视频 PTS 或使用外部时钟时，不把补时值算作真实音画误差。
每个完成调度且有有效比较的视频帧最多取一次样本，包含迟到丢帧和等待淘汰；不会按主循环轮询频率重复计数。
累计误差统计用于观察调度表现，包含丢帧前的偏差，不能代替实际已显示画面的设备端同步测量。

## 配置与构建

| 字段 | 默认值 | 约束 |
|---|---|---|
| `sync.tolerance_ms` | 40 | 1–100 ms |
| `sync.drop_threshold_ms` | 100 | 1–1000 ms，且不小于容差 |
| `sync.audio_wait_ms` | 500 | 100–5000 ms，音频未就绪、欠载或时钟停滞的等待上限 |
| `sync.discontinuity_threshold_ms` | 1000 | 500–10000 ms，且大于丢帧阈值；用于显著失步判定及提前帧候选等待上限 |

`config/player.json` 包含默认值。通用 `--set` 可覆盖，例如
`--set sync.tolerance_ms=30 --set sync.drop_threshold_ms=100`。
`input.discontinuity_threshold_ms` 仍控制 M09 包时间戳换代，两者分别作用于网络包与播放调度。

在工程目录直接使用 CMake，无额外构建脚本。本次成功的配置与构建命令为：

```powershell
cd D:\直播\LiveStreamLab
cmake -S . -B build/cmake -DLSL_BUILD_TESTS=OFF -DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON
cmake --build build/cmake --config Release --target player --parallel 1 -- /nodeReuse:false
```

`/nodeReuse:false` 是 Windows MSBuild 参数；其他生成器可去掉 `-- /nodeReuse:false`。
以下是使用命令，本次未启动：

```powershell
.\build\cmake\bin\Release\player.exe --config config\player.json
.\build\cmake\bin\Release\player.exe --url rtmp://127.0.0.1/live/livestream --set sync.tolerance_ms=30
.\build\cmake\bin\Release\player.exe --play-audio false
.\build\cmake\bin\Release\player.exe --audio-paused true
```
