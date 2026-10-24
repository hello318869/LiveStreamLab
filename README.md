# LiveStreamLab

C++17 + FFmpeg + SDL3 音视频直播学习工程，详细模块见上级目录的 `方案.md`。
当前已实现 M01 基础框架、M02 音视频输入、M03/M04 音视频预处理与 M05/M06 视频/音频编码。主播端可以枚举设备、查看能力、
采集并解码摄像头/麦克风，以及按时间戳读取、循环本地文件。
视频经过格式转换、缩放、帧率控制及可选画面处理；音频经过重采样、声道转换和固定帧长缓存。
视频继续编码为 H.264，默认自动保存到 `output/video.mp4`；目录不存在时自动创建。
音频继续编码为 AAC-LC，单独保存到 `output/audio.m4a`。
设备模式可选择麦克风或 Windows 系统声音，支持软件增益与音频电平日志。
当前 MP4 仅含视频、M4A 仅含音频；M07 的音视频合并封装推流与观众播放尚待实现。

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

# 查询所选设备支持的格式、分辨率、帧率 / 音频参数
.\build\cmake\bin\Release\publisher.exe --list-options video --video-device "default"
.\build\cmake\bin\Release\publisher.exe --list-options audio --audio-device "default"

# 摄像头与麦克风采集，Ctrl+C 有序停止
.\build\cmake\bin\Release\publisher.exe --config config\publisher.json

# 只有摄像头，使用设备实际支持的采集参数
.\build\cmake\bin\Release\publisher.exe --capture-audio false --width 640 --height 480 --fps 30

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

Windows 麦克风的 `default` 跟随系统默认录音设备，摄像头选择枚举到的第一个设备。
Linux 摄像头同理，音频使用 ALSA `default`。摄像头请求的分辨率、帧率必须是驱动支持的组合。
Windows 使用 DirectShow，Linux 使用 V4L2 / ALSA。设备占用、权限不足、断开、
不支持的采集格式与超时都会报告错误并清理资源。
文件模式只处理所启用且实际存在的音视频轨，支持只有音频或只有视频的文件。
相对输出路径以启动工作目录为准；从本工程目录启动时，输出位于 `LiveStreamLab/output/`。
正常 EOF 或 Ctrl+C 会排空音视频编码器并写完文件尾部。再次运行时需使用新文件名或移走已有文件。
音频预处理按 AAC 编码器实际支持的 PCM 格式与 `frame_size` 组帧，协商结果在日志中报告。

## 实现说明

- [M02 接口、生命周期与时间戳](docs/capture.md)
- [M03/M04 预处理、PTS、帧率和音频尾帧](docs/preprocessing.md)
- [M05 H.264 编码与自动保存](docs/video-encoding.md)
- [M06 AAC 编码、尾帧与自动保存](docs/audio-encoding.md)
- [音源选择、弱音频修复与测试结果](docs/audio-diagnosis.md)
- [配置字段与命令行](docs/configuration.md)
- [日志、错误处理与清理](docs/logging-and-errors.md)
- [当前架构](docs/architecture.md)

M06 接入后的工程已通过 Windows Release 的 CMake 完整构建，包括 publisher 与 player。
本次编译记录见 [音频编码说明](docs/audio-encoding.md)。
本轮经用户授权，已通过增益定向测试、合成信号端到端回归，以及真实麦克风和系统回采编码/解码检查。
具体结果与播放验收边界见音频诊断说明。
