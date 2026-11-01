# LiveStreamLab

C++17 + FFmpeg + SDL3 音视频直播工程：主播端负责采集、预处理、编码、推流与本地录制，
观众端负责拉流、解码、音画同步播放。统一软件 `livestreamlab.exe` 在首页选择“我要开播”或“我要观看”，
通过同一进程中的 Qt Widgets 界面使用两种功能。
模块划分与验收标准见上级目录 `方案.md`，实现细节见 [docs/](docs/architecture.md)。

## 功能

### 开播模式 `publisher`

- **视频源**：Windows 桌面（gdigrab，含鼠标）、摄像头、本地媒体文件（可循环、按时间戳节奏读取）
- **音频源**：麦克风、Windows 系统声音（WASAPI 回采）、文件音轨
- **预处理**：缩放/裁剪/留黑、旋转、镜像、水印、直播时长叠加；重采样、声道转换、软件增益
- **编码**：H.264（libx264，回退 libopenh264）+ AAC-LC，GOP、preset、低延迟可配
- **输出**：本地 MP4（纯视频）、M4A（纯音频）、FLV 经 RTMP/RTMPS 推送到 SRS
- **并发**：采集、音视频处理编码、本地封装、推流各自线程，用有界队列交接；
  实时视频队列满时丢旧帧，音频和文件转换走背压

### 观看模式 `player`

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
SDL3 与 nlohmann/json 已放在 `external/`；Qt 界面另需 Qt 6.5+ Widgets、Network（本机 `external/qt/6.8.3/msvc2022_64` 会被自动找到）。

```powershell
cd D:\直播\LiveStreamLab
cmake -S . -B build/cmake -DLSL_BUILD_TESTS=OFF
cmake --build build/cmake --config Release --target livestreamlab --parallel
```

- FFmpeg 自动在 `external/ffmpeg`、上级/盘根 `ffmpeg/ffmpeg-*` 查找，也可 `-DFFmpeg_ROOT="D:/ffmpeg/ffmpeg-9.0.1-full_build-shared"`
- 默认 `LSL_BUILD_GUI=ON`；只需命令行功能、不装 Qt：追加 `-DLSL_BUILD_GUI=OFF`，仍生成同一 `livestreamlab` 程序
- 程序产物在 `build/cmake/bin/Release/`（单配置生成器为 `build/cmake/bin/`）
- 中文路径下若 SDL 预编译头报 C4828，配置时加 `-DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON`
- 测试默认关闭；仅在明确要求测试时开启并执行

### 网络控制服务与平台客户端（N01—N06）

Linux/WSL 后端可使用 `src/control_server/CMakeLists.txt` 或项目顶层 CMake 构建，生成 `control_server`，
不依赖客户端的 Qt、FFmpeg 或 SDL3。已提供注册/登录/退出、认证、个人房间、直播申请/停止、
大厅与播放查询、SRS 回调鉴权和周期状态对账。N04/N05 客户端已接入直播大厅、
登录注册、个人房间设置及平台开播/下播；N06 补齐恢复意图、有限重试、退出回收和部署模板。
操作与边界见 [平台客户端](docs/platform-client.md)、[恢复行为](docs/platform-recovery.md)。
顶层只编译后端时设置 `LSL_BUILD_CONTROL_SERVER=ON`、`LSL_BUILD_MEDIA=OFF`。
固定依赖、CMake 命令与验证边界见 [后端构建与配置](docs/platform-deployment.md)，
接口见 [HTTP 契约](docs/platform-api.md)。

## 启动

构建后双击 `build/cmake/bin/Release/livestreamlab.exe`，或从工程目录执行：

```powershell
.\build\cmake\bin\Release\livestreamlab.exe
```

首页选择“我要观看”进入公开直播大厅，选择主播后自动播放，返回时先停止媒体再回到大厅。
选择“我要开播”先登录或注册，再设置房间标题、分类和采集参数；点击开始后向后端申请独立直播会话。
本地预览不申请开播，推流地址由后端提供。返回首页或关闭功能窗口先停止当前媒体并释放资源；关闭首页退出软件。

平台入口优先读取程序旁 `config/platform.local.json`，否则读取 `platform.json`，默认 `http://127.0.0.1:8090`。
本机覆盖文件不会被重新构建覆盖；修改地址后重启客户端。
业务后端、数据库迁移和 SRS 平台鉴权需先按 [部署说明](docs/platform-deployment.md) 配置运行。
客户端代码已接入不代表这些服务已部署；本轮没有启动、重启或修改运行中的服务。

CMake 将默认配置复制到程序旁的 `config/`。GUI 按可执行文件位置加载 `publisher-stream.json` 或
`player.json`，双击启动不依赖当前工作目录。显式 `--config` 覆盖默认配置，媒体参数须配合 `--mode`：

```powershell
# 直接进入原有地址工具界面（不经过平台登录或大厅）
.\build\cmake\bin\Release\livestreamlab.exe --mode publisher
.\build\cmake\bin\Release\livestreamlab.exe --mode player --url rtmp://127.0.0.1/live/livestream
```

推流、拉流前需运行 SRS（默认 RTMP 1935、HTTP 8080）。需要原有控制台工作方式时加 `--cli`：

```powershell
# 桌面 + 系统声音推流
.\build\cmake\bin\Release\livestreamlab.exe --mode publisher --cli --config config\publisher-stream.json

# 本地录制（默认桌面 + 系统声音），Ctrl+C 有序停止
.\build\cmake\bin\Release\livestreamlab.exe --mode publisher --cli --config config\publisher.json

# 摄像头输入
.\build\cmake\bin\Release\livestreamlab.exe --mode publisher --cli --video-source camera --video-device "default"

# 从本地文件推流，可循环
.\build\cmake\bin\Release\livestreamlab.exe --mode publisher --cli --config config\publisher-stream.json --source file --input-file "D:\media\sample.mp4" --loop true

# 枚举设备、查询设备能力
.\build\cmake\bin\Release\livestreamlab.exe --mode publisher --list-devices
.\build\cmake\bin\Release\livestreamlab.exe --mode publisher --list-options video --video-device "default"

# 控制台观看，使用 SDL3 播放窗口
.\build\cmake\bin\Release\livestreamlab.exe --mode player --cli --config config\player.json --volume 50

# 统一入口帮助 / 观看参数帮助 / 配置查询
.\build\cmake\bin\Release\livestreamlab.exe --help
.\build\cmake\bin\Release\livestreamlab.exe --mode player --help
.\build\cmake\bin\Release\livestreamlab.exe --mode player --print-config
```

`--help`、`--check-config`、`--print-config` 和设备查询直接输出结果，不进入 GUI。
这些查询沿用 CLI 的配置规则；要检查某个 GUI 配置文件，请同时显式指定 `--config`。
CLI 使用内置默认配置；开播模式默认仍为本地录制，需要推流时显式指定 `publisher-stream.json`。
`LSL_BUILD_GUI=OFF` 时必须指定 `--mode`，该模式自动使用 CLI，也可保留 `--cli`。

CLI 播放窗口中 `F` / `Alt+Enter` / 双击切换全屏，`Esc` 停止；`Space` 暂停/继续，
`M` 静音，`↑`/`↓` 调音量。GUI 的 `Esc` 仅退出全屏，停止播放使用停止按钮。

## 配置与产物

| 文件 | 用途 |
| --- | --- |
| `config/publisher.json` | 默认本地录制：桌面 + 系统声音，写 `output/video.mp4`、`output/audio.m4a` |
| `config/publisher-stream.json` | 推流：关闭本地文件，开启 `output.streaming` |
| `config/publisher-file.json` | 本地文件循环推流 |
| `config/player.json` | 观众端：HTTP-FLV 地址、超时重连、同步阈值、队列与日志 |
| `config/platform.json` | 平台 API 地址、HTTP 超时、大厅及当前房间状态刷新间隔 |
| 程序旁 `config/platform.local.json` | 各设备自己的平台入口覆盖文件，不纳入 Git、不被构建覆盖 |

录制产物写入 `output/`，目录不存在会自动创建；文件名已存在时拒绝覆盖。
字段含义与全部命令行参数见 [配置说明](docs/configuration.md)。
旧文档中的 `publisher`、`player`、`publisher_gui`、`player_gui` 程序名对应早期独立入口；
当前统一使用 `livestreamlab --mode publisher|player`，控制台场景再加 `--cli`。

2026-10-27：统一 GUI 程序已通过 CMake 的 Windows Release 编译和链接，运行依赖与默认配置已部署。
本次按要求未启动程序、未运行测试或真实推拉流；显示与切换效果仍待后续运行验收。

## 文档

- [当前架构](docs/architecture.md)
- 主播端：[采集](docs/capture.md)、[预处理](docs/preprocessing.md)、[视频编码](docs/video-encoding.md)、[音频编码](docs/audio-encoding.md)、[RTMP 推流](docs/rtmp-streaming.md)
- 观众端：[拉流解封装](docs/network-demuxing.md)、[视频播放](docs/video-playback.md)、[音频播放](docs/audio-playback.md)、[音画同步](docs/av-sync.md)
- 工程：[多线程与队列](docs/threading.md)、[网络可靠性](docs/network-reliability.md)、[性能统计](docs/performance-statistics.md)、[延迟优化](docs/latency.md)
- 界面：[主播控制](docs/publisher-control.md)、[观众控制](docs/player-control.md)
- 平台：[客户端流程](docs/platform-client.md)、[HTTP 契约](docs/platform-api.md)、[部署配置](docs/platform-deployment.md)、[恢复行为](docs/platform-recovery.md)、[部署模板](deploy/README.md)
