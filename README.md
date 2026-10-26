# LiveStreamLab

C++17 + FFmpeg + SDL3 音视频直播学习工程，详细模块见上级目录的 `方案.md`。
当前已实现 M01 基础框架、M02 音视频输入、M03/M04 音视频预处理、M05/M06 视频/音频编码、
M07 FLV/RTMP 推流、M09 网络拉流及解封装、M10 视频解码、M11 音频解码播放、M12 SDL3 视频显示、
M13 音画同步、M14 多线程有界流水线与 M15 网络可靠性/状态管理。主播端可以枚举设备、查看能力、
默认采集 Windows 桌面画面，摄像头可选；音频可选择麦克风或系统声音，也支持按时间戳读取、循环本地文件。
视频经过格式转换、缩放、帧率控制及可选画面处理；音频经过重采样、声道转换和固定帧长缓存。
视频继续编码为 H.264，默认自动保存到 `output/video.mp4`；目录不存在时自动创建。
音频继续编码为 AAC-LC，单独保存到 `output/audio.m4a`。
设备模式可选择麦克风或 Windows 系统声音，支持软件增益与音频电平日志。
MP4 仅含视频、M4A 仅含音频。启用 M07 后，独立复用线程将 H.264/AAC 合并为 FLV 并推送至 RTMP/RTMPS 地址；
包含超时、可中断 I/O、有限或持续重连与推流统计。观众端 `player` 已接入 RTMP/HTTP-FLV 收包、
流信息、音视频有界队列、重连和统计，并通过视频解码线程和 SDL3 主线程显示画面。
当前音频由独立解码线程和 SDL3 回调输出；视频跟随音频估计播放位置，支持等待、丢帧、外部时钟降级和换代恢复。
M14 将主播端采集、音视频预处理/编码和本地封装拆为独立工作线程，通过有界队列交接独占媒体对象，
网络发送继续由唯一 RTMP 线程负责。实时原始视频优先丢旧帧，音频和非实时文件转换使用背压；
观众端主线程继续负责 SDL 事件与视频显示。队列容量、关闭顺序与统计见 [M14 说明](docs/threading.md)。
M08 SRS 部署仍按原阶段单独安排。

主播端目录已按《方案.md》统一为 `capture`、`preprocess`、`encoder`、`muxer`、`rtmp`、`pipeline`。
`pipeline/PublisherPipeline` 组合各处理模块，采集模块仅负责输入。目录、命名空间与 CMake 职责对应见
[当前架构](docs/architecture.md)。此前目录整理已通过 Windows Release 的 CMake 完整编译，未运行测试；
2026-10-25 的桌面采集接入未构建、未运行测试，也未做真实桌面采集。

## 直接使用 CMake 构建

依赖 CMake ≥ 3.24、支持 C++17 的编译器，以及含 **libavdevice** 的 FFmpeg ≥ 6 开发 SDK。
SDL3 与 nlohmann/json 已放在 `external/`，不需要单独安装。Windows 安装 Visual Studio
的“使用 C++ 的桌面开发”组件后，在 PowerShell 中执行：

```powershell
cd D:\直播\LiveStreamLab
cmake -S . -B build/cmake -DLSL_BUILD_TESTS=OFF
cmake --build build/cmake --config Release --parallel
```

使用新的 `build/cmake` 目录可避开原有 Ninja 缓存。CMake 默认选择本机可用的生成器；
本机默认生成器是已安装的 VS 2026。若需要显式指定，第一条命令改为：

```powershell
cmake -S . -B build/cmake -G "Visual Studio 18 2026" -A x64 -DLSL_BUILD_TESTS=OFF
```

FFmpeg SDK 自动查找 `external/ffmpeg`、上级/盘根的 `ffmpeg/ffmpeg-*`；
也可加 `-DFFmpeg_ROOT="D:/ffmpeg/ffmpeg-9.0.1-full_build-shared"`。
Windows 会把 FFmpeg、SDL3 DLL 复制到程序旁边。
Visual Studio 生成器的程序位于 `build/cmake/bin/Release/`；
单配置生成器位于 `build/cmake/bin/`。Debug 将构建命令中的 `Release` 改成 `Debug`。

Linux 使用开发包提供的 libavdevice、libavformat、libavcodec、libavutil、libswscale、
libswresample 与 pkg-config：

```sh
cmake -S . -B build/cmake -DCMAKE_BUILD_TYPE=Release -DLSL_BUILD_TESTS=OFF
cmake --build build/cmake --parallel
```

已有 Ninja presets 仍可用，在配置好 MSVC 的 x64 Developer PowerShell 中执行
`cmake --preset windows-release` 与 `cmake --build --preset windows-release`。
中文 MSVC 使用 Ninja 时可先执行 `chcp 65001`。不提供额外构建脚本，测试目标默认关闭。
本机中文路径下若 SDL 的预编译头出现 C4828 / MSB8084 编码错误，
配置时加 `-DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON` 后重新构建即可；当前 `build/cmake` 已设置此选项。

## 使用主播端

以下命令在工程根目录执行，采用 Visual Studio 生成器的产物路径：

```powershell
# 枚举摄像头、麦克风，输出可用于配置的唯一 id
.\build\cmake\bin\Release\publisher.exe --list-devices

# 查询摄像头支持的格式、分辨率、帧率 / 麦克风参数
.\build\cmake\bin\Release\publisher.exe --list-options video --video-device "default"
.\build\cmake\bin\Release\publisher.exe --list-options audio --audio-device "default"

# 默认采集 Windows 完整桌面与麦克风，Ctrl+C 有序停止
.\build\cmake\bin\Release\publisher.exe --config config\publisher.json

# 改为摄像头；设备名称或 id 可从 --list-devices 获取
.\build\cmake\bin\Release\publisher.exe --video-source camera --video-device "default"

# 只有摄像头，使用设备实际支持的采集参数
.\build\cmake\bin\Release\publisher.exe --video-source camera --capture-audio false --width 640 --height 480 --fps 30

# 本地文件：正常节奏读取，EOF 后退出；加 --loop true 可持续循环
.\build\cmake\bin\Release\publisher.exe --source file --input-file "D:\media\sample.mp4"

# 指定另一个视频文件名；默认 output/video.mp4，拒绝覆盖已有文件
.\build\cmake\bin\Release\publisher.exe --video-file output/video-02.mp4 --gop-size 50 --preset veryfast --low-latency true

# 只处理文件音轨，编码为 AAC 并保存到 M4A
.\build\cmake\bin\Release\publisher.exe --source file --input-file "D:\media\sample.mp4" --capture-video false --audio-file output/audio-02.m4a

# 选择麦克风；输入偏小时加 20 dB 软件增益（不改变 Windows 设备音量）
.\build\cmake\bin\Release\publisher.exe --capture-video false --audio-source microphone --audio-gain-db 20 --audio-file output/microphone.m4a

# 选择电脑正在播放的声音，不混入麦克风
.\build\cmake\bin\Release\publisher.exe --capture-video false --audio-source system --audio-file output/system.m4a

# 配置与命令行仍可单独查看
.\build\cmake\bin\Release\publisher.exe --config config\publisher.json --print-config
.\build\cmake\bin\Release\player.exe --help
```

设备模式由 `capture.video_source` / `--video-source` 选择视频来源，默认是 `desktop`。
Windows 桌面使用 FFmpeg `gdigrab` 采集完整虚拟桌面及鼠标；`--fps` 控制采集帧率，
`--width` / `--height` 是后续缩放、编码的目标尺寸，不裁剪桌面。
`--video-source camera` 才使用 `--video-device` 与 `--video-format`；只传设备名不会切换视频来源。
`--list-devices` 与 `--list-options video` 仍用于枚举、查询摄像头。
Windows 麦克风的 `default` 跟随系统默认录音设备，摄像头的 `default` 选择枚举到的第一个设备。
本次桌面后端仅支持 Windows；Linux 设备模式须显式使用 `--video-source camera`，
通过 V4L2 采集摄像头，音频使用 ALSA `default`。摄像头请求的分辨率、帧率必须是驱动支持的组合。
Windows 摄像头与麦克风使用 DirectShow。设备占用、权限不足、断开、
不支持的采集格式与超时都会报告错误并清理资源。
文件模式只处理所启用且实际存在的音视频轨，支持只有音频或只有视频的文件；
`capture.video_source` 和 `capture.audio_source` 不改变文件中的音视频来源。
相对输出路径以启动工作目录为准；从本工程目录启动时，输出位于 `LiveStreamLab/output/`。
正常 EOF 或 Ctrl+C 会排空音视频编码器并写完文件尾部。再次运行时需使用新文件名或移走已有文件。
音频预处理按 AAC 编码器实际支持的 PCM 格式与 `frame_size` 组帧，协商结果在日志中报告。

## M07 推流

先准备一个可接收 RTMP 的 SRS 服务。以下为使用命令，本次未运行：

```powershell
# 默认桌面 + 麦克风；推流示例关闭本地 MP4/M4A 输出
.\build\cmake\bin\Release\publisher.exe --config config\publisher-stream.json --url rtmp://127.0.0.1/live/livestream

# 可选摄像头取代桌面
.\build\cmake\bin\Release\publisher.exe --config config\publisher-stream.json --video-source camera --video-device "default"

# 本地文件按时间戳重编码推流；EOF 后退出，加 --loop true 可循环
.\build\cmake\bin\Release\publisher.exe --config config\publisher-stream.json --source file --input-file "D:\media\sample.mp4"

# 桌面画面 + Windows 系统声音，不混入麦克风
.\build\cmake\bin\Release\publisher.exe --config config\publisher-stream.json --audio-source system

# 也可使用外部 ffplay 接收同一路直播
ffplay rtmp://127.0.0.1/live/livestream
```

SRS 在另一台机器时，将推流与播放 URL 中的 `127.0.0.1` 换成服务器地址。
默认 `publisher.json` 保持本地录制，只有 `--stream true` 或 `output.streaming=true` 才连接网络。
连接超时、重连、丢包恢复、统计口径与停止行为见 [M07 说明](docs/rtmp-streaming.md)。

## M09–M13 拉流、解码与同步播放

先准备正在推流的 SRS。以下是从工程根目录构建和启动的方法；启动命令本次未执行：

```powershell
# 只构建观众端；不运行测试
cmake -S . -B build/cmake -DLSL_BUILD_TESTS=OFF -DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON
cmake --build build/cmake --config Release --target player --parallel 1 -- /nodeReuse:false

# 默认 HTTP-FLV 输入，解码视频并在 SDL3 窗口显示，同时播放音频
.\build\cmake\bin\Release\player.exe --config config\player.json

# 同一路 RTMP 输入
.\build\cmake\bin\Release\player.exe --url rtmp://127.0.0.1/live/livestream

# 调整单次读包超时、重连次数与接收缓存时长
.\build\cmake\bin\Release\player.exe --read-timeout 5000 --reconnect-attempts 5 --max-buffer-ms 2000

# 设置初始窗口大小；也可 --fullscreen true 全屏启动
.\build\cmake\bin\Release\player.exe --width 960 --height 540 --vsync true

# 调整播放器音量；--play-audio false 可关闭音频输出
.\build\cmake\bin\Release\player.exe --volume 50
```

`player` 将视频包送入 FFmpeg 软件解码器，输出 YUV420P/NV12，由 SDL3 上传纹理并等比显示。
窗口可调整大小；按 F、Alt+Enter 或双击切换全屏，Esc、关闭窗口或 Ctrl+C 停止。
音频使用 FFmpeg 解码，经 libswresample 转为固定输出采样率/声道数的交错 F32 PCM，
通过有界 SPSC 环形缓冲交给 SDL3 音频流回调；支持只有视频或只有音频的输入，不保存媒体文件。
Space 同时暂停/恢复声音与视频，M 静音，↑/↓ 调节 10% 音量；不改变系统音量。
视频默认跟随音频主时钟，提前超过 40 ms 等待，迟到超过 100 ms 且有后续帧时丢帧；
无音轨或音频长时间未就绪时使用外部时钟。阈值可通过 `sync` 配置调整。
日志提供流编码参数、代次、解码/显示 FPS、耗时、上传次数、积压与丢帧信息。
缓冲超限会清理两路积压并等待下一视频关键帧；断线后重新发现流参数。
默认将网络 EOF 作为直播断流重连；`--reconnect-attempts 0` 禁用重连，此时正常 EOF 排空后退出。
解码帧队列默认最多 3 帧、64 MiB，主线程另保留最多一帧等待显示。
`pipeline.max_video_age_ms` 按播放时钟判断迟到，不按提前解码后的墙钟年龄删帧。
PCM 环默认容量时长为 100 ms；开流不额外等待视频帧率统计，避免启动积压被持续播放。
低延迟优化依据与边界见 [延迟回退优化](docs/latency.md)。
重连换代会清除旧解码器、排队帧、PCM、画面与同步时钟。
接口和网络缓冲见 [M09 说明](docs/network-demuxing.md)，播放路径与边界见 [M10/M12 说明](docs/video-playback.md)。

## M14 队列与并发控制

`src/common/queue/BoundedBlockingQueue.hpp` 提供有界阻塞队列，支持移动独占帧/包、超时交接、
关闭后排空、故障中止及积压峰值统计。主播端默认限制原始视频 8 帧、原始音频 32 块、
输出事件 256 项；输出事件包含编码参数、媒体包与轨道结束标记。
实时视频队列满时丢弃最旧原始帧，消费时年龄超过 200 ms 且仍有后续帧才跳过旧帧；
音频不任意丢样本，`capture.realtime=false` 的文件转换也不主动丢视频。

两端均支持 `--video-queue-capacity` 和 `--max-video-age-ms`；主播端另支持
`--audio-queue-capacity` 与 `--output-queue-capacity`，也可通过 JSON 的 `pipeline` 字段或 `--set` 修改。
主播端 Ctrl+C 中断采集与网络，排空已入队的处理数据并写完本地文件；阶段故障则中止所有队列，
唤醒等待者后回收线程。观众端停止时同时唤醒网络、解码与音频缓冲等待，再释放 SDL 资源。
具体参数、所有权和结束顺序见 [M14 说明](docs/threading.md)。

2026-10-26 M14 已通过 CMake 配置与 Windows Release 的 `publisher`、`player` 编译、链接。
按项目要求未运行测试、未启动程序、未采集设备或连接 SRS；真实退出、积压恢复和长时间稳定性待运行验收。

## M15 网络可靠性与状态

两端提供 `IDLE`、`CONNECTING`、`STREAMING`、`RECONNECTING`、`STOPPING` 和 `ERROR` 状态，
保留连接时长、重试次数、吞吐量和最近错误。连接/读写截止时间、无数据检测和主动取消共同限制网络等待；
断线后丢弃旧连接缓存并重新建立媒体会话，观众端继续通过 `serial` 清理旧解码帧、PCM 和时钟。

默认连续故障最多重试 5 次，首次等待 1 秒，之后按 2 倍退避至最多 30 秒。
两端均可用 `--reconnect-attempts -1` 持续重试，`0` 禁止重连；仅实际媒体进展重置失败预算。
`--reconnect-max-delay` 调整退避上限；`--inactivity-timeout` 默认 15000 ms，0 禁用，
主播端检查成功媒体发送进展，观众端检查有效音视频包进展。
低帧率、长 GOP 或源本来允许长时间无媒体时，应适当增大或禁用此时限。
状态、配置、统计口径与恢复边界见 [M15 网络可靠性说明](docs/network-reliability.md)。

2026-10-26 M15 已通过 CMake 配置及 Windows Release 两端编译、链接。
本次未运行测试、未启动程序、未连接或重启 SRS；实际断网恢复与退出时延仍待运行验收。

## 实现说明

- [M02 接口、生命周期与时间戳](docs/capture.md)
- [M03/M04 预处理、PTS、帧率和音频尾帧](docs/preprocessing.md)
- [M05 H.264 编码与自动保存](docs/video-encoding.md)
- [M06 AAC 编码、尾帧与自动保存](docs/audio-encoding.md)
- [M07 FLV 封装、RTMP 推流、时间戳与重连](docs/rtmp-streaming.md)
- [M09 网络拉流、解封装、接收缓冲与重连](docs/network-demuxing.md)
- [M10/M12 视频解码、SDL3 显示与播放节奏](docs/video-playback.md)
- [M11 音频解码、重采样、SDL3 播放与控制](docs/audio-playback.md)
- [M13 音频主时钟、音画调度与同步恢复](docs/av-sync.md)
- [M14 多线程流水线、有界队列、背压与停止顺序](docs/threading.md)
- [M15 网络状态机、超时、退避重连与故障清理](docs/network-reliability.md)
- [音源选择、弱音频修复与测试结果](docs/audio-diagnosis.md)
- [配置字段与命令行](docs/configuration.md)
- [日志、错误处理与清理](docs/logging-and-errors.md)
- [当前架构](docs/architecture.md)

M06 阶段已通过 Windows Release 的 CMake 完整构建，并经当时用户授权完成增益定向测试、
合成信号回归、真实麦克风和系统回采编码/解码检查，记录见 [音频诊断](docs/audio-diagnosis.md)。
M07 实现时已通过 Windows Release 的 CMake 完整编译；当时按项目要求未运行测试、采集或推流。
实际构建命令见 [M07 说明](docs/rtmp-streaming.md)，网络连接、接收播放和重连表现仍待用户运行验收。
2026-10-25 本次新增默认桌面采集与可选摄像头入口，未构建、未运行测试、未做真实桌面采集；
此前构建或测试记录不作为本次修改的运行验收结果。
2026-10-25 M09 阶段已通过 Windows Release 的 `player` 编译、链接；当时测试保持关闭，
未启动程序、未运行测试、未连接 SRS。持续收包、网络恢复和后续播放效果仍需另行运行验收，
实际成功的构建命令见 [M09 说明](docs/network-demuxing.md)。
2026-10-25 本次 M10/M12 已通过 CMake 配置与 Windows Release 的 `player` 编译、链接；
未启动程序、未运行测试、未连接 SRS，成功命令见 [M10/M12 说明](docs/video-playback.md)。
M09 的历史构建记录不代表此次解码、窗口显示、真实播放或重连效果已验证。

2026-10-25 M11 音频解码与 SDL3 播放已接入，CMake 配置与 Release 的 `player` 编译、链接通过。
本次未启动程序、未运行测试、未连接 SRS；声音、连续性和设备兼容性仍待实际运行验收。
该 M11 阶段尚未实现音画同步，详见 [M11 说明](docs/audio-playback.md)。

2026-10-25 M13 已接入 `player/sync`，提供音频播放位置估算、音画显示调度、外部时钟降级、
同步暂停、换代恢复与误差统计。已通过 CMake 配置与 Windows Release 的 `player` 编译、链接；
本次未运行测试、未启动播放器或连接 SRS；
实际同步误差和长时间稳定性仍待运行验收，详见 [M13 说明](docs/av-sync.md)。
