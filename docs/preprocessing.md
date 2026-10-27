# M03 / M04 音视频预处理

`lsl_preprocess` 提供两个独立组件，输入为 M02 解码得到的 `AVFrame` 和
`CapturedFrame::timestampUs`，输出为独占的 `media::FramePtr`。
主播端已经在 `pipeline/PublisherPipeline.cpp` 的读取循环调用预处理，视频输出继续交给 M05 编码并保存，
音频输出继续交给 M06 编码为 AAC 并单独保存到 M4A，参见 [音频编码](audio-encoding.md)。

## 视频 M03

`VideoPreprocessor` 将 libswscale 支持的 CPU 像素格式（包括 RGB、NV12、YUV420P）
转换为 YUV420P，输出尺寸取 `video.width/height`，时间基为 `1/video.fps`。
按各平面及 linesize 访问数据；缓存转换上下文，输入参数变化时更新。

- `scale_algorithm`：`nearest`、`bilinear`、`bicubic`、`lanczos`。
- `resize_mode`：`fit` 保持显示宽高比并留黑；`crop` 保持显示宽高比并居中裁剪；
  `stretch` 按目标宽高拉伸。非方形输入像素的 SAR 纳入显示宽高比计算。
- `rotation`：顺时针 0 / 90 / 180 / 270 度；`mirror` 在旋转后水平镜像。
  输出方向由这些显式选项决定；文件的显示旋转元数据不会自动应用，需按需要设置 rotation。
- `watermark`：内置点阵文字水印，支持最多 80 个可打印 ASCII 字符。
  `timestamp_overlay` 显示直播经过时间。两项默认关闭，无外部字体依赖。

按输入时间戳映射输出 PTS，丢弃同一输出时间槽中多余的输入，缺帧时引用上一幅画面补帧。
单次最多补一秒旧画面；开启时间戳叠加时，还按 64 MiB 的输出像素预算减少补帧数量
（至少允许当前帧）。长时间停顿保留 PTS 空隙，统计省略的时间槽，避免大量分配或延迟堆积。
重复帧拥有独立 AVFrame 结构，像素缓冲通过 FFmpeg 引用计数共享；下游写像素前应调用
`av_frame_make_writable()`。

统计包括输入/输出帧数、丢帧数、补帧数、省略的时间槽，以及按媒体时间计算的输入/输出 FPS。
这些值用于观察预处理节奏，不代表端到端直播性能。

## 音频 M04

`AudioPreprocessor` 使用 libswresample 与 `AVAudioFifo`，支持平面/交错样本、
单声道/双声道输入、采样率和声道布局转换。
M04 独立组件使用 `AudioPreprocessOptions` 中的采样率、声道、采样格式及帧长。
主播端先打开 M06 AAC 编码器，再按其实际接受的格式和 `frame_size` 配置 M04；
`preprocess.audio.sample_format/frame_size` 为偏好值，以实际协商结果为准，日志会报告覆盖。
默认 AAC 路径为 48000 Hz、双声道、FLTP，每帧每声道 1024 个样本。
`preprocess.audio.gain_db` 在重采样后、写入 FIFO 前调整 PCM 电平，范围 -60..30 dB，默认 0。
增益包含重采样尾部；超幅时饱和并计入 `clippedValues`（所有声道的采样值数量）。
它不会修改系统设备音量。输入过弱时可用 `--audio-gain-db 20`；削波计数增长时应降低增益。

重采样输出容量包含内部延迟；FIFO 累积不足一帧的样本，达到 `frame_size` 后输出。
输入采样参数变化时先排空旧重采样器；EOF 或正常停止时再次排空。
`padFinalFrame` 默认为 `true`，不足完整帧的尾部补静音；主播端根据 AAC 编码器支持短尾帧的能力
设置为 `false` 时，最终余数按实际样本数输出一次，之后刷新编码器。
大时间跳变形成的中间片段仍补齐完整帧，不能让 AAC 把该片段当作整段流结束。

输出时间基为 `1/sample_rate`，PTS 以累计输出样本推进，每帧设置 duration。
小于 `discontinuity_threshold_ms` 的输入时间误差视为采集抖动；超过阈值时处理不连续：
短正向缺口补有限静音，较长停顿先结束旧片段，再保留新的 PTS 空隙；反向重叠裁去已覆盖的输入。
默认阈值为 100 ms。补静音、尾部填充、丢弃样本与不连续次数分别计数。
`discardedSamples` 按输入采样率计数；输出、静音与填充样本数按输出采样率计数，均为每声道样本数。
单帧输入最多一秒 PCM，每次调用最多返回 4096 帧；超限会报错。
使用极小输出 frame_size 时，调用方需要缩小输入分片；默认 1024 样本的 AAC 帧长不受影响。

## 调用和生命周期

```cpp
lsl::preprocess::VideoPreprocessOptions videoOptions;
lsl::preprocess::AudioPreprocessOptions audioOptions;
lsl::preprocess::VideoPreprocessor video(videoOptions, originUs);
lsl::preprocess::AudioPreprocessor audio(audioOptions, originUs);

// 每帧按 kind 选择一个组件，返回 0 到多个可编码帧。
auto frames = video.process(*captured.frame, captured.timestampUs);
// 正常结束时分别调用 flush()，接收并消费其输出。
auto finalAudio = audio.flush();
```

设备模式的 `originUs` 是会话启动时的 `av_gettime_relative()`，音视频使用同一值；
文件模式设为 0，沿用 M02 已对齐的媒体时间轴及循环偏移。
禁止分别减去两轨自己的首帧时间，否则会丢失原本的起始偏移。
`capturedAtUs` 是接收时刻，预处理使用 `timestampUs`，不使用系统墙钟生成媒体 PTS。

两个组件分别在视频、音频处理线程构造、调用和销毁；采集线程通过有界原始帧队列移交独占帧，
不再执行预处理或编码，不支持同一组件实例并发调用。
`flush()` 可重复调用；flush 后继续 process 会报错，新会话应创建新实例。
控制顺序为：请求停止输入 → 读取循环结束、关闭输入和原始帧队列 →
处理线程排空已入队帧、flush 预处理及编码器 → 输出线程排空事件、写完 MP4/M4A → join。
视频实时积压优先丢旧原始帧；音频队列满时等待容量，避免任意删除 PCM 样本。
具体容量、取消与故障清理见 [M14 多线程流水线](threading.md)。
异常退出依靠 RAII 释放资源；不会把未处理完成的帧当作成功输出。

## 使用示例

从工程根目录运行已有 CMake 产物：

```powershell
# 文件的实际格式/尺寸/采样率在解码后由 M03/M04 转为目标参数
.\build\cmake\bin\Release\livestreamlab.exe --mode publisher --cli --source file --input-file "D:\media\sample.mp4" --width 1280 --height 720 --fps 25 --sample-rate 48000 --channels 2

# 居中裁剪、旋转、镜像及文字叠加
.\build\cmake\bin\Release\livestreamlab.exe --mode publisher --cli --source file --input-file "D:\media\sample.mp4" --resize-mode crop --rotation 90 --mirror true --watermark "LiveStreamLab" --timestamp-overlay true

# PCM 格式和帧长偏好（M06 会按 AAC 实际支持值协商）；--set 可以覆盖任意 JSON 叶字段
.\build\cmake\bin\Release\livestreamlab.exe --mode publisher --cli --source file --input-file "D:\media\sample.mp4" --audio-sample-format s16 --audio-frame-size 1024 --set preprocess.audio.discontinuity_threshold_ms=100
```

设备模式仍用 `video.*` 和 `audio.*` 请求驱动支持的采集参数；不支持的设备组合需要先用
`--list-options` 查询。预处理输出的格式与帧长可通过首帧日志查看，周期和结束日志记录累计统计。
示例供用户实际运行时使用；本次不运行测试或真实采集，不据此声明编码、推流或运行验收通过。

## 编译记录（2026-10-24）

使用 CMake 4.3.1、MSVC 19.51 和本机 FFmpeg 9.0.1 开发 SDK，
`lsl_common`、`lsl_preprocess`、`lsl_capture` 及 `publisher` 的 Windows Release 编译、链接完成。
在独立临时目录构建，避免改写工程内已有构建产物；未创建构建脚本。

```powershell
cmake -S . -B "$env:TEMP\LiveStreamLab-M03-M04-01a16d2b" -DLSL_BUILD_TESTS=OFF
cmake --build "$env:TEMP\LiveStreamLab-M03-M04-01a16d2b" --config Release --target publisher --parallel
```

MSBuild 提示临时目录可能影响增量构建；源码编译和链接成功。
日常构建使用 README 中的 `build/cmake` 目录。仅确认本机编译层面，未运行项目测试或采集程序，
也未验证 Linux / FFmpeg 6 环境、设备兼容性、画面效果或声音连续性。
