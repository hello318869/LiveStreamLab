# LiveStreamLab

C++17 + FFmpeg + SDL3 音视频直播工程：主播端负责采集、预处理、编码、推流与本地录制，
观众端负责拉流、解码、音画同步播放，两端各带一套 Qt Widgets 控制界面。
模块划分与验收标准见上级目录 `方案.md`，实现细节见 [docs/](docs/architecture.md)。

## 功能

### 主播端 `publisher`

- **视频源**：Windows 桌面（gdigrab，含鼠标）、摄像头、本地媒体文件（可循环、按时间戳节奏读取）
- **音频源**：麦克风、Windows 系统声音（WASAPI 回采）、文件音轨
- **预处理**：缩放/裁剪/留黑、旋转、镜像、水印、直播时长叠加；重采样、声道转换、软件增益
- **编码**：H.264（libx264，回退 libopenh264）+ AAC-LC，GOP、preset、低延迟可配
- **输出**：本地 MP4（纯视频）、M4A（纯音频）、FLV 经 RTMP/RTMPS 推送到 SRS
- **并发**：采集、音视频处理编码、本地封装、推流各自线程，用有界队列交接；
  实时视频队列满时丢旧帧，音频和文件转换走背压

### 观众端 `player`

- **输入**：RTMP/RTMPS、HTTP/HTTPS-FLV
- **视频**：FFmpeg 软解 → SDL3 窗口或 Qt 控件显示，等比缩放、可全屏
- **音频**：解码重采样 → SDL3 输出，支持暂停、静音、音量
- **音画同步**：以音频播放位置为主时钟，无音轨或欠载时降级为外部时钟，支持等待、丢帧和重连恢复
- **网络**：连接/读取超时、无数据检测、指数退避重连

### 两端共有

- JSON 配置 + 命令行覆盖，`--print-config` 回显配置、`--check-config` 校验
- 网络状态机（IDLE / CONNECTING / STREAMING / RECONNECTING / STOPPING / ERROR）与连接统计
- 性能统计：采集/编码/解码 FPS、码率、各级队列积压、丢帧、同步误差、端到端延迟估计

## 构建

需要 CMake ≥ 3.24、支持 C++17 的编译器，以及含 **libavdevice** 的 FFmpeg ≥ 6 开发 SDK。
SDL3 与 nlohmann/json 已放在 `external/`；Qt 界面另需 Qt 6.5+ Widgets（本机 `external/qt/6.8.3/msvc2022_64` 会被自动找到）。

```powershell
cd D:\直播\LiveStreamLab
cmake -S . -B build/cmake -DLSL_BUILD_TESTS=OFF
cmake --build build/cmake --config Release --parallel
```

- FFmpeg 自动在 `external/ffmpeg`、上级/盘根 `ffmpeg/ffmpeg-*` 查找，也可 `-DFFmpeg_ROOT="D:/ffmpeg/ffmpeg-9.0.1-full_build-shared"`
- 只构建命令行程序、不装 Qt：追加 `-DLSL_BUILD_PUBLISHER_GUI=OFF -DLSL_BUILD_PLAYER_GUI=OFF`
- 程序产物在 `build/cmake/bin/Release/`（单配置生成器为 `build/cmake/bin/`）
- 中文路径下若 SDL 预编译头报 C4828，配置时加 `-DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON`
- 测试默认关闭，需要时改用 `-DLSL_BUILD_TESTS=ON`

## 启动

以下命令都在 `LiveStreamLab` 目录下执行。推流/拉流前先在 `127.0.0.1` 运行 SRS（RTMP 1935、HTTP 8080）。

```powershell
# 1. 主播端：桌面 + 系统声音，推流到 SRS
.\build\cmake\bin\Release\publisher.exe --config config\publisher-stream.json --url rtmp://127.0.0.1/live/livestream

# 2. 观众端：默认 HTTP-FLV 拉流，SDL3 窗口播放
.\build\cmake\bin\Release\player.exe --config config\player.json

# 或用外部播放器验证同一路流
ffplay rtmp://127.0.0.1/live/livestream
```

图形界面（选中设备、预览画面、开始/停止直播、查看统计）：

```powershell
.\build\cmake\bin\Release\publisher_gui.exe --config config\publisher-stream.json
.\build\cmake\bin\Release\player_gui.exe --config config\player.json
```

常用命令行开关：

```powershell
# 本地录制（默认桌面 + 系统声音），Ctrl+C 有序停止
.\build\cmake\bin\Release\publisher.exe --config config\publisher.json

# 改用摄像头 / 仅摄像头
.\build\cmake\bin\Release\publisher.exe --video-source camera --video-device "default"
.\build\cmake\bin\Release\publisher.exe --video-source camera --capture-audio false --width 640 --height 480 --fps 30

# 从本地文件推流，可循环
.\build\cmake\bin\Release\publisher.exe --config config\publisher-stream.json --source file --input-file "D:\media\sample.mp4" --loop true

# 枚举设备、查询设备能力
.\build\cmake\bin\Release\publisher.exe --list-devices
.\build\cmake\bin\Release\publisher.exe --list-options video --video-device "default"

# 观众端：RTMP 输入、调整超时与重连、窗口大小与音量
.\build\cmake\bin\Release\player.exe --url rtmp://127.0.0.1/live/livestream
.\build\cmake\bin\Release\player.exe --read-timeout 5000 --reconnect-attempts 5 --max-buffer-ms 2000
.\build\cmake\bin\Release\player.exe --width 960 --height 540 --fullscreen false --volume 50
```

快捷键：播放窗口里 `F` / `Alt+Enter` / 双击切换全屏，`Esc` 退出；`Space` 暂停/继续，`M` 静音，`↑`/`↓` 调音量。

## 配置与产物

| 文件 | 用途 |
| --- | --- |
| `config/publisher.json` | 默认本地录制：桌面 + 系统声音，写 `output/video.mp4`、`output/audio.m4a` |
| `config/publisher-stream.json` | 推流：关闭本地文件，开启 `output.streaming` |
| `config/publisher-file.json` | 本地文件循环推流 |
| `config/player.json` | 观众端：HTTP-FLV 地址、超时重连、同步阈值、队列与日志 |

录制产物写入 `output/`，目录不存在会自动创建；文件名已存在时拒绝覆盖。
字段含义与全部命令行参数见 [配置说明](docs/configuration.md)。

## 文档

- [当前架构](docs/architecture.md)
- 主播端：[采集](docs/capture.md)、[预处理](docs/preprocessing.md)、[视频编码](docs/video-encoding.md)、[音频编码](docs/audio-encoding.md)、[RTMP 推流](docs/rtmp-streaming.md)
- 观众端：[拉流解封装](docs/network-demuxing.md)、[视频播放](docs/video-playback.md)、[音频播放](docs/audio-playback.md)、[音画同步](docs/av-sync.md)
- 工程：[多线程与队列](docs/threading.md)、[网络可靠性](docs/network-reliability.md)、[性能统计](docs/performance-statistics.md)、[延迟优化](docs/latency.md)
- 界面：[主播控制](docs/publisher-control.md)、[观众控制](docs/player-control.md)
