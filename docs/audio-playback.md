# M11 音频解码与 SDL3 播放

观众端在 M09 拉流、M10 视频解码与 M12 窗口显示之外，新增音频解码和声音输出。
音频按输入流的实际编码参数解码，通过 libswresample 转为配置采样率和声道数的交错
float32 PCM，再由 SDL3 音频流回调播放。默认播放音频，输出采样率 48000 Hz、立体声，
PCM 环形缓冲容量时长默认 100 ms，实际容量至少 4096 个每声道样本。

M13 已接入音频主时钟与视频调度，见 [音画同步](av-sync.md)。Space 与初始 `audio.paused`
现在同时暂停声音和视频；静音继续消费媒体并推进时钟。

2026-10-25 M11 阶段已通过 CMake 配置与 Windows Release 的 `player` 编译、链接。当时未启动程序、未运行测试、未连接 SRS；
实际声音、设备兼容性、连续播放和断线恢复尚未运行验收。

## 构建和使用

在工程目录直接使用 CMake，不增加构建脚本。以下为本次成功的构建命令：

```powershell
cd D:\直播\LiveStreamLab
cmake -S . -B build/cmake -DLSL_BUILD_TESTS=OFF -DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON
cmake --build build/cmake --config Release --target player --parallel 1 -- /nodeReuse:false
```

`/nodeReuse:false` 是 Windows MSBuild 参数，其他生成器可使用
`cmake --build build/cmake --config Release --target player --parallel`。

先准备正在推流的 SRS，以下启动命令本次均未执行：

```powershell
# 默认 HTTP-FLV，同时输出视频和声音
.\build\cmake\bin\Release\player.exe --config config\player.json

# 同一路 RTMP
.\build\cmake\bin\Release\player.exe --url rtmp://127.0.0.1/live/livestream

# 调整声音：50% 音量，输出 48 kHz 立体声
.\build\cmake\bin\Release\player.exe --volume 50 --sample-rate 48000 --channels 2

# 静音启动，或以音画暂停状态启动
.\build\cmake\bin\Release\player.exe --mute true
.\build\cmake\bin\Release\player.exe --audio-paused true

# 仅播放视频；音频包仍及时消费并释放
.\build\cmake\bin\Release\player.exe --play-audio false

# 更改 PCM 环形缓冲的容量时长，单位毫秒
.\build\cmake\bin\Release\player.exe --audio-buffer-ms 300

# 使用系统默认输出设备，或指定 SDL 枚举到的输出设备名
.\build\cmake\bin\Release\player.exe --audio-device default
```

| 操作 | 行为 |
|---|---|
| Space | 同时暂停或恢复声音与视频 |
| M | 切换静音 |
| 上/下方向键 | 每次增加或减少 10% 音量，限制在 0% 至 100% |
| F、Alt+Enter、双击 | 沿用 M12 的全屏切换 |
| Esc、关闭窗口、Ctrl+C | 停止全部播放并退出 |

| 命令行 | 作用 |
|---|---|
| `--play-audio true\|false` | 启用或禁用音频设备输出 |
| `--audio-device NAME` | SDL 输出设备名；`default` 使用系统默认播放设备 |
| `--sample-rate HZ` | 应用侧 PCM 输出采样率，设备侧由 SDL 协商 |
| `--channels N` | 应用侧 PCM 声道数，1 或 2 |
| `--audio-buffer-ms N` | PCM 环形缓冲目标时长，100 至 2000 ms；实际容量至少 4096 帧 |
| `--volume N` | 音量，0 至 100 |
| `--mute true\|false` | 静音状态 |
| `--audio-paused true\|false` | 初始音画暂停状态，沿用旧参数名 |

这些开关分别对应 JSON 的 `audio.enabled`、`audio.device`、`audio.sample_rate`、
`audio.channels`、`audio.buffer_ms`、`audio.volume`、`audio.muted` 和 `audio.paused`。
`--play-audio false` 禁用声音输出；静音仍按正常速率消费音频，防止静音期间积压。
环形缓冲容量不等于保证的播放延迟，网络包、解码器、重采样器及设备还各有缓冲。
单个解码块最多 1024 个每声道样本，单次解码或刷新返回最多两秒 PCM；
环容量为 `max(4096, ceil(采样率 * buffer_ms / 1000))` 个每声道样本。

## 模块与线程

```text
网络线程：AVFormatContext → av_read_frame → 视频/音频 PacketBuffer
视频线程：视频包 → VideoDecoder → VideoFrameQueue
音频线程：音频包 → AudioDecoder → libswresample → float32 PCM → SPSC ring
SDL 回调：SPSC ring → SDL_PutAudioStreamData → SDL 转换/混音 → 系统输出设备
主线程：  SDL 事件、视频 PTS 调度与显示、音频控制、停止及错误传播
```

| 位置 | 职责 |
|---|---|
| `src/player/decoder/AudioDecoder.*` | 根据流参数创建解码器、send/receive、PCM 转换、正常 EOF 排空 |
| `src/player/audio` | 音频输出接口、SDL3 设备与回调、PCM 环形缓冲、控制与统计 |
| `src/player/pipeline/AudioPlayback.*` | 独立音频工作线程、PCM 写入背压、音频结束与错误交接 |
| `src/player/pipeline/PlayerPipeline.*` | 视频工作线程、主线程编排、代次、停止与输入生命周期 |

音频工作线程独占音频解码上下文和重采样器，视频工作线程独占视频解码上下文。
PCM 交接使用一个生产者和一个消费者的有界环形缓冲；生产者不在 SDL 回调中执行解码、
分配大型缓冲、记录日志或等待网络。音频回调只做短暂的 PCM 交接和原子统计。
SDL 视频资源仍由主线程创建和释放。

## 解码、格式和缓冲

`AudioDecoder` 从 `StreamInfo` 快照的 `AVCodecParameters` 初始化解码器并导入包时间基。
send 返回 EAGAIN 时先 receive，再重试同一个尚未接受的包；持续 receive 处理一包多帧及延迟输出。
缺少音轨时不创建有效音频解码状态，也不打开声音输出设备，视频仍可继续播放。
只有音频的输入可以输出声音，SDL 视频窗口保留黑屏；没有实现专用的音频播放器界面。

解码 PCM 的采样格式、采样率或声道布局可以随输入变化。libswresample 将其转换为固定的
交错 float32、配置采样率及单声道或立体声，使应用侧输出格式稳定；SDL 再适配实际设备格式。
媒体样本时间以输入时间基换算，正常结束先排空解码器，再排空重采样器，避免遗漏尾部样本。

网络包队列仍使用 [M09 的限制和换代规则](network-demuxing.md)。PCM 交接缓冲按
`--audio-buffer-ms` 限制容量，音频工作线程与视频工作线程独立，音频输出背压不占用视频解码线程。
暂停、停止和换代需要及时解除生产者等待；不能让主线程等待一个永远无法继续写入的音频线程。
音画暂停时间过长仍可能触发直播输入的缓存清理，恢复后可能跳到较新的直播内容。

## 重连、EOF 与退出

网络重连、包缓冲超限或时间戳跳变会递增 `serial`。音频工作线程撤销旧解码器和重采样状态，
清空旧 PCM 与 SDL 流缓存，重新建立当前代次的时间基准。旧代次的解码输出不能写入新代次缓冲。
清 SDL 流缓存无法撤回已经提交给操作系统或设备的声音，切换边界仍可能存在少量旧设备缓存。

默认网络 EOF 按 M09 策略重连。只有禁用重连后的正常 EOF 才排空解码器、重采样器和音频缓冲；
故障、主动停止或代次变化直接清理旧内容，不把旧连接尾部继续送入设备。
`SDL_FlushAudioStream` 让 SDL 重采样器中保留的尾部数据可读取，不负责等待物理设备播放结束。
输入缓存已经排空也不代表扬声器已经输出完最后一个样本。
如果正常 EOF 到来时音频仍处于暂停状态，播放器会等待恢复后排空尾部；
也可直接关闭窗口、按 Esc 或 Ctrl+C 停止，不需要先恢复声音。

关闭窗口或 Ctrl+C 会请求停止网络 I/O，撤销 PCM 写入等待，停止音视频工作线程并 join，
随后销毁 SDL 音频流和设备，最后释放 SDL 视频资源。音频流销毁前，回调 userdata 及其缓冲必须保持存活。
音频工作线程和回调中的失败通过受保护状态传回主线程，不能让异常穿过 SDL 的 C 回调边界。

## 音频媒体位置与 M13 时钟的口径

M11 记录媒体起始时间与回调实际提交的媒体样本帧数；M13 在此基础上扣除 SDL 未消费数据与
设备周期延迟，提供 `IAudioSink::clock()` 估计播放位置供视频同步调度。
欠载由静音补足的部分不累计为媒体样本，静音开关本身仍消费正常媒体样本；
暂停、清缓存和代次变化按各自生命周期维护或重置该基准。

这不是物理播放位置。工程内 `external/SDL/include/SDL3/SDL_audio.h` 的 GetCallback 文档说明，
回调在流取数前执行，`additional_amount` 是可能高估的输入字节需求；
`external/SDL/src/audio/SDL_audiocvt.c` 先调用回调，再取流数据和转换；
`external/SDL/src/audio/SDL_audio.c` 在取流后才调用设备提交函数。
因此回调计数只精确表示应用交给 SDL 的样本量，不能消除系统或设备缓冲延迟。

`SDL_GetAudioStreamQueued` 返回未消费的输入字节，`SDL_GetAudioStreamAvailable` 返回可读的
转换后输出字节；二者都不直接报告扬声器当前播放位置。
M13 使用估计位置驱动视频，欠载时停止媒体时钟推进，并有限等待后降级到外部时钟。
`Queued` 与 `Available` 表示同一流的输入/输出视角，不能重复相加扣减；设备周期仍只是估计，
不宣称取得扬声器精确播放位置或已经验证真实音画同步。细节见 [M13](av-sync.md)。
