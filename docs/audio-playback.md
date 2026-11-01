# 音频解码与 SDL3 播放

`AudioDecoder` 按流参数解码，经 libswresample 转为配置采样率/声道数的交错 float32 PCM，再由 SDL3 音频回调输出。默认 48000 Hz、立体声，启用声音。GUI 与 CLI 共用此链路，视频显示区别见[观众控制](player-control.md)。构建见 [README](../README.md)，完整线程和调用链见[线程模型](threading.md)、[函数调用顺序](function-call-order.md)。

## 使用与参数

```powershell
.\build\cmake\bin\Release\livestreamlab.exe --mode player --cli --config config\player.json
.\build\cmake\bin\Release\livestreamlab.exe --mode player --cli --volume 50 --sample-rate 48000 --channels 2
.\build\cmake\bin\Release\livestreamlab.exe --mode player --cli --play-audio false
```

| CLI | JSON 字段 | 默认值 / 含义 |
|---|---|---|
| `--play-audio` | `audio.enabled` | true；false 时仍消费并释放音频包 |
| `--audio-device` | `audio.device` | default，或 SDL 输出设备名 |
| `--sample-rate` / `--channels` | `audio.sample_rate` / `audio.channels` | 48000 / 2；声道 1 或 2，实际设备格式由 SDL 适配 |
| `--audio-buffer-ms` | `audio.buffer_ms` | 100；允许 100–2000 ms，为 PCM 环容量时长 |
| `--volume` / `--mute` | `audio.volume` / `audio.muted` | 100 / false；音量 0–100，仅影响本播放器 |
| `--audio-paused` | `audio.paused` | false；初始音画联合暂停 |

CLI 中 Space 联合暂停/恢复，M 静音，↑/↓ 每次调 10%，Esc/关窗/Ctrl+C 停止。静音继续消费媒体、推进时钟，与关闭声音输出不同。完整采样率约束见[配置说明](configuration.md)。

## 解码、回调和背压

```text
网络 PacketBuffer
  ⇒ AudioPlayback 独立线程：AudioDecoder → libswresample → PCM 环
  ⇒ SDL 音频回调 fill：SDL_PutAudioStreamData → SDL/设备输出
```

`src/player/decoder/AudioDecoder.*` 独占解码器和重采样器；`reset()` 导入快照参数及包时间基，`decode()` 用 send/receive（EAGAIN 后取输出、重送原包），`flush()` 先排解码器再排重采样器。输入格式、采样率和布局可变，应用侧 PCM 保持固定格式。无音轨或禁用输出时不打开设备，纯音频仍可播放，GUI 显示音频状态、CLI 保留空视频窗口。

`src/player/audio/SDLAudioSink.*` 实现 `IAudioSink`，首次有效音轨 Reset 时打开设备。`enqueue()` 写单生产者/单消费者环；回调只交接预分配 PCM 和原子统计，不解码、不等待网络、不写日志。容量为 `max(4096, ceil(sample_rate × buffer_ms / 1000))` 个每声道样本；单块最多 1024 个，单次解码/刷新最多两秒 PCM。SDL 流还有限制，容量不等于保证延迟。

`AudioPlayback::publish()` 在环满时短等并检查停止/serial，只背压音频线程。网络队列仍受[包缓冲规则](network-demuxing.md)约束，长暂停可触发换代，恢复跳到较新内容。

## 播放位置

`IAudioSink::clock()` 为[音画同步](av-sync.md)提供估计位置：回调提交的媒体终点扣除 SDL 未消费数据和估计设备周期延迟，并在回调间平滑推进。欠载补静音不累计媒体进度，静音仍消费媒体，暂停冻结，换代清基准。

`SDL_GetAudioStreamQueued` 是未消费的输入字节，`Available` 是同一流转换后的可读输出，不能重复相加扣减；应用 PCM 环尚未提交，也不能再从提交终点扣除。回调发生在流取数和设备提交之前，`additional_amount` 可高估，因此回调计数不等于扬声器物理播放指针。设备/驱动仍有额外延迟。

## 换代、EOF 与退出

重连、包超限或时间戳跳变使 serial 递增；清旧解码/重采样状态、PCM、SDL 流和时钟，拒绝旧输出。清 SDL 缓冲不能撤回已经交给系统/硬件的少量声音。

默认 EOF 重连；禁用重连的正常 EOF 才排空解码器、重采样器及设备缓冲。`finish()` 使用 `SDL_FlushAudioStream` 排 SDL 尾部，不能证明物理设备已经播完。若仍暂停则等待恢复排空，也可直接停止。

主动停止、故障和换代丢弃旧内容；统一退出先停网络与工作线程、解除背压并 join，再销毁音频流和设备。GUI 的 SDL 音频子系统在应用主线程构造/销毁，回调 userdata 存活至流销毁。工作线程错误和回调失败经受保护状态传给协调循环，异常不穿过 SDL C 回调边界。
