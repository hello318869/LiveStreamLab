# LiveStreamLab

C++17 + FFmpeg + SDL3 音视频直播学习工程，详细模块见上级目录的 `方案.md`。
当前已实现 M01 基础框架、M02 音视频输入、M03/M04 音视频预处理、M05/M06 视频/音频编码与 M07 FLV/RTMP 推流。主播端可以枚举设备、查看能力、
默认采集 Windows 桌面画面，摄像头可选；音频可选择麦克风或系统声音，也支持按时间戳读取、循环本地文件。
视频经过格式转换、缩放、帧率控制及可选画面处理；音频经过重采样、声道转换和固定帧长缓存。
视频继续编码为 H.264，默认自动保存到 `output/video.mp4`；目录不存在时自动创建。
音频继续编码为 AAC-LC，单独保存到 `output/audio.m4a`。
设备模式可选择麦克风或 Windows 系统声音，支持软件增益与音频电平日志。
MP4 仅含视频、M4A 仅含音频。启用 M07 后，独立复用线程将 H.264/AAC 合并为 FLV 并推送至 RTMP/RTMPS 地址；
包含超时、可中断 I/O、有限重连与推流统计。观众端播放和 SRS 部署属于后续模块。

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

# 使用外部 ffplay 接收同一路直播；自研 player 尚未实现播放
ffplay rtmp://127.0.0.1/live/livestream
```

SRS 在另一台机器时，将推流与播放 URL 中的 `127.0.0.1` 换成服务器地址。
默认 `publisher.json` 保持本地录制，只有 `--stream true` 或 `output.streaming=true` 才连接网络。
连接超时、重连、丢包恢复、统计口径与停止行为见 [M07 说明](docs/rtmp-streaming.md)。

## 实现说明

- [M02 接口、生命周期与时间戳](docs/capture.md)
- [M03/M04 预处理、PTS、帧率和音频尾帧](docs/preprocessing.md)
- [M05 H.264 编码与自动保存](docs/video-encoding.md)
- [M06 AAC 编码、尾帧与自动保存](docs/audio-encoding.md)
- [M07 FLV 封装、RTMP 推流、时间戳与重连](docs/rtmp-streaming.md)
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
