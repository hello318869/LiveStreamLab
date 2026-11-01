# 音视频预处理（M03 / M04）

`src/publisher/preprocess/` 接收 `AVFrame` 与 `CapturedFrame::timestampUs`，返回零到多个独占帧。
组件由各自处理线程独占；构建见 [README](../README.md)，调用位置见 [函数调用顺序](function-call-order.md)。

## 视频 VideoPreprocessor

输出为 YUV420P、`video.width/height`、时间基 `1/video.fps`；libswscale 转换/缩放时处理平面、linesize，按需更新上下文。

| `preprocess.video` 字段 | 默认值 / 作用 |
|---|---|
| `scale_algorithm` | `bilinear`；另支持 nearest、bicubic、lanczos |
| `resize_mode` | `fit` 保宽高比留黑；`crop` 居中裁剪；`stretch` 拉伸，计算显示比例时计入输入 SAR |
| `rotation` / `mirror` | 0 / false；顺时针 0、90、180、270 度，再水平镜像；文件旋转元数据不自动应用 |
| `watermark` | 空；最多 80 个可打印 ASCII 字符，无外部字体依赖 |
| `timestamp_overlay` | false；显示直播经过时间 |

输入时间映射为输出 PTS；同槽/回退帧丢弃，缺槽引用上一帧补齐。单次最多补一秒旧画面；
开启经过时间叠加时还受 64 MiB 输出像素预算约束（至少允许当前帧）。长停顿保留 PTS 空隙，
不无限补帧。重复帧的结构独立、像素可共享，写像素前应 `av_frame_make_writable()`。
统计包括输入/输出/丢弃/重复帧、省略时间槽和媒体时间 FPS。

## 音频 AudioPreprocessor

libswresample 转换平面/交错格式、采样率及单/双声道，再由 `AVAudioFifo` 按编码帧长组帧。
主播端**先开 AAC 编码器，再按其实际格式与 frame_size 创建预处理器**；
`preprocess.audio.sample_format=fltp`、`frame_size=1024` 是偏好值，被覆盖时会记日志。
默认 AAC 路径为 48 kHz、双声道、FLTP、每声道 1024 样本。

- `gain_db`：默认 0，范围 -60..30 dB；重采样后、写 FIFO 前施加，也作用于重采样尾部。
  超幅饱和并计入 `clippedValues`，不修改设备音量。
- PTS 按累计输出样本推进，时间基 `1/sample_rate`。`discontinuity_threshold_ms` 默认 100：
  小误差视为抖动，短正向缺口补有限静音，大停顿结束旧片段并保留时间空隙，反向重叠裁去重复样本。
- 格式变化或结束时先排空重采样延迟。最终余数按 `padFinalFrame` 补静音或输出短尾帧；
  独立组件默认补齐，主播端根据 AAC 能力选择。时间跳变形成的中间片段始终补完整帧。
- 输入单帧最多一秒 PCM，每次调用最多返回 4096 帧；极小 frame_size 需缩小输入分片。
  `discardedSamples` 按输入采样率计数，输出/静音/填充按输出采样率，均为每声道样本；
  削波数按所有声道采样值计数。

## 共同时间轴与收尾

```cpp
lsl::preprocess::VideoPreprocessor video(videoOptions, originUs);
auto frames = video.process(*captured.frame, captured.timestampUs);
// 消费全部返回帧；音频以相同方式 process()。
auto tail = video.flush();
```

设备音视频使用会话启动时同一个 `av_gettime_relative()` 起点；文件起点为 0，沿用输入对齐后的媒体时间。
不得分别减去两轨首帧，也不使用 `capturedAtUs` 或系统墙钟生成媒体 PTS。

正常停止先读尽原始队列，再 flush 预处理、编码器及输出。视频 flush 仅释放前帧；
音频 flush 排空重采样器和 FIFO，返回值也必须编码。flush 可重复，之后 process 会报错；新会话创建新实例。
背压与取消见 [多线程流水线](threading.md)，AAC 尾帧约定见 [音频编码](audio-encoding.md)。

## 使用

以下命令从工程根目录运行：

```powershell
# 裁剪、旋转、镜像、叠字与增益；已有输出文件须换名
.\build\cmake\bin\Release\livestreamlab.exe --mode publisher --cli --video-file output/processed.mp4 --audio-file output/processed.m4a --resize-mode crop --rotation 90 --mirror true --watermark LiveStreamLab --timestamp-overlay true --audio-gain-db 10
```

尺寸/采样参数用 `--width/--height/--fps/--sample-rate/--channels`；其他叶字段可
`--set preprocess.audio.discontinuity_threshold_ms=100`。驱动能力查询见 [输入](capture.md)，实际格式/统计见日志。
