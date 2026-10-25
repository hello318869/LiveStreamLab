# M02 音视频输入

## 输入路径

`publisher_main → runConfigApp → runPublisher → MediaInput`。
`lsl_capture` 独立于入口程序，通过 CMake 链接已有 `lsl_common` 与 FFmpeg。
采集模块只保留设备管理和媒体输入；完整主播流程由 `lsl_publisher_pipeline` 组合，
入口实现位于 `src/publisher/pipeline/PublisherPipeline.cpp`，函数为 `lsl::pipeline::runPublisher`。

- Windows 桌面：设备模式默认 `capture.video_source=desktop`，通过 FFmpeg `gdigrab` 读取
  完整虚拟桌面及鼠标。采集帧率使用 `video.fps`，随后由 M03 缩放到 `video.width` / `video.height`；
  目标输出宽高不传给桌面后端作为裁剪尺寸。
- Windows 摄像头与音频：`--video-source camera` 才打开摄像头。
  DirectShow COM 按类别枚举摄像头与麦克风，唯一 ID 和友好名称均可配置。
  分别枚举避免缺少麦克风时影响摄像头列表；媒体采集仍通过 FFmpeg libavdevice 完成。
  `--list-options video|audio` 使用 FFmpeg 的 DirectShow `list_options`，能力信息直接输出到 stderr。
  麦克风 `default` 解析 Windows 默认录音端点，并按端点 GUID 匹配 DirectShow 设备。
  `--audio-source system` 改用默认播放设备的 WASAPI loopback；不混入麦克风、不修改系统音量。
  系统回采输出统一为 48 kHz、双声道 float，支持正常空闲等待和主动停止。
- Linux：V4L2 枚举摄像头，ALSA 枚举麦克风。视频能力通过 V4L2 ioctl 输出像素格式、
  分辨率与帧间隔；ALSA 在打开时协商采样率/声道，首帧日志报告实际 PCM 格式。
  本次桌面后端仅支持 Windows，Linux 设备输入须显式设置 `--video-source camera`。
- 文件：libavformat 自动识别本地文件，分别为选中的音视频轨创建解码上下文。
  MP4 中的压缩视频/音频与设备提供的 MJPEG、PCM 等统一解码为 AVFrame。

设备能力接口依赖驱动与 FFmpeg 构建启用的后端。FFmpeg 不支持枚举或缺少对应后端时会报告错误。
设备能力查询使用 FFmpeg 默认 stderr 日志，不受应用 `logging.file` 的重定向控制。
`--list-devices` / `--list-options video` 查询的仍是摄像头，不会启动桌面采集。
`--video-device` 与 `--video-format` 只用于 `camera`；文件输入不受 `capture.video_source` 影响。

在工程根目录执行以下命令，默认音频仍是麦克风；可通过 `--audio-source system` 改为系统声音：

```powershell
# Windows 默认完整桌面与麦克风
.\build\cmake\bin\Release\publisher.exe --config config\publisher.json

# 可选摄像头
.\build\cmake\bin\Release\publisher.exe --video-source camera --video-device "default"
```

上述命令会沿用 M05/M06 本地录制设置，默认保存 `output/video.mp4` 与 `output/audio.m4a`，
已有同名文件会拒绝覆盖。要再次录制，需指定新路径或先移走旧文件。

## C++ 接口与所有权

```cpp
lsl::capture::MediaInput input;
input.openFile(config); // 或 openDevice(Kind::Video/Audio, config)
while (auto captured = input.readFrame()) {
    // captured->frame 为独占 AVFrame，可将整个 CapturedFrame 移动给下游。
}
input.close();
```

`readFrame()` 返回 `optional<CapturedFrame>`；文件 EOF、主动停止返回空值，其他错误抛出
AppError/FFmpegError。返回的帧由调用方独占，不会被下一次读取覆盖，输入关闭后仍可继续持有。
复用帧或包时使用 `referenceFrame/referencePacket`，内部调用 `av_frame_ref/av_packet_ref`。
引用共享底层缓冲，修改前先调用 FFmpeg 的 make_writable 接口。

当前入口在 `pipeline/PublisherPipeline.cpp` 的读取循环将帧交给 M03/M04，继续交给 M05/M06 编码并分别自动保存。
参见 [预处理接口与时间轴](preprocessing.md)、[视频编码与文件输出](video-encoding.md)
和 [AAC 音频编码](audio-encoding.md)。
后续 M14 再建立完整的有界队列与流水线。

## 时间戳与文件节奏

`CapturedFrame` 包含原始 `AVFrame`、媒体种类、时间基、`timestampUs` 与 `capturedAtUs`。

- AVFrame 的 PTS 保留原始媒体时间；`timeBase` 用于换算原始 PTS。
- 设备帧的 `timestampUs` 使用 `av_gettime_relative()`，桌面、摄像头与麦克风共用单调时钟域。
  `capturedAtUs` 记录解码输出时刻；这是应用收到帧的时间，不能当作传感器曝光时刻。
- 文件帧以容器起点/轨道起点为共同原点，保留音视频起始偏移。
  `best_effort_timestamp` 优先于 PTS；缺失时间戳时按音频样本数或视频帧率补齐。
- 默认按照文件时间戳等待后再交付，`capture.realtime=false` 可取消等待。
- EOF 后先向所有解码器送空包并收完延迟帧，再退出或 seek 回起点。
  循环会 flush 解码器；每次循环在上一轮最后一帧结束时间后继续，保持输出时间轴连续。
  不可 seek 的文件和没有解码帧的循环会报告错误。

这里的节奏控制用于提供文件直播源，不代表已完成 M13 音画播放同步。

## 启停与线程边界

一个设备对应一个工作线程；文件的音视频轨共用一个解复用线程。
每个输入的 open/read/close 在同一线程执行，防止 DirectShow COM 与上下文跨线程释放。
已在所属线程 close 的空输入对象可在主线程析构；仍持有媒体资源的对象须在所属线程析构。
只有 `requestStop()` 可由其他线程调用：它设置原子停止标志并唤醒文件节奏等待。

禁止在读取时从另一个线程直接 close。停止顺序为：requestStop → 读取返回 → 工作线程 close → join。
重复 close 安全；已打开的输入再次 open 会报错，close 后允许重新打开。
工作线程发生错误后停止其他输入，所有线程退出后由原有顶层异常处理记录错误并返回 1。

DirectShow 读包采用 `AVFMT_FLAG_NONBLOCK`，无数据时短暂等待并检查停止/超时；
文件 I/O 配置 `AVIOInterruptCB`。Linux V4L2/ALSA 同样使用非阻塞请求。
驱动在设备初始化等内部调用中可能不响应 FFmpeg 中断，不能据此保证任意故障驱动都能限时退出。

实现依据：[FFmpeg gdigrab 桌面设备说明](https://ffmpeg.org/ffmpeg-devices.html#gdigrab)、
[gdigrab 读包实现](https://github.com/FFmpeg/FFmpeg/blob/master/libavdevice/gdigrab.c)、
[FFmpeg DirectShow 设备与读包实现](https://ffmpeg.org/doxygen/trunk/dshow_8c_source.html)、
[FFmpeg V4L2 后端](https://github.com/FFmpeg/FFmpeg/blob/master/libavdevice/v4l2.c)、
[FFmpeg ALSA 后端](https://github.com/FFmpeg/FFmpeg/blob/master/libavdevice/alsa_dec.c)。

## 本次完成边界

历史接入已补齐 M01 媒体引用接口及 M02 采集实现、入口和配置，并接入后续预处理、编码与输出。
此前 publisher 的 Windows Release 编译记录不作为本次修改的验证结果。
2026-10-25 本次新增默认 Windows 桌面采集与可选摄像头；按项目要求未构建、未运行测试，
也未做真实桌面采集。桌面尺寸、鼠标、停止行为及录制/推流表现仍待实际运行验收。
