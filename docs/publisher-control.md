# M16.1 主播控制

统一程序 `livestreamlab` 的“我要开播”使用 C++17 + Qt Widgets，复用现有采集、预处理、
H.264/AAC 编码、FLV/RTMP 与 M14/M15 停止/重连路径。控制台行为通过
`livestreamlab --mode publisher --cli` 使用。
本文说明 M16.1 主播控制；观众控制与 M16.3 性能统计现已分别接入，见
[观众控制](player-control.md)和[性能统计](performance-statistics.md)。
界面按参考图组织为深色左导航、中间直播画面/统计卡片、右侧滚动设置和底部事件日志。
导航只定位已实现的主播控制区域；没有虚构房间服务、观众人数、WebRTC 或录制状态。

## 构建与启动

从工程根目录直接使用 CMake，无额外构建脚本：

```powershell
cmake -S . -B build/cmake -DLSL_BUILD_TESTS=OFF -DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON
cmake --build build/cmake --config Release --target livestreamlab --parallel
.\build\cmake\bin\Release\livestreamlab.exe --mode publisher
```

不带参数启动同一程序可先显示功能首页。GUI 默认读取程序旁 `config/publisher-stream.json`，
CMake 构建时复制配置；也可用 `--config` 指定其他文件，显式相对路径按当前工作目录解释。
单配置生成器的产物位于 `build/cmake/bin/`。`LSL_BUILD_GUI` 默认开启，
设为 `OFF` 可只构建同一程序的命令行功能。需要 Qt 6.5+ 的 Widgets SDK；本机使用官方
Qt 6.8.3 MSVC 2022 x64 的 `qtbase` 与配套 `qttranslations` 包，安装在 `external/qt/6.8.3/msvc2022_64`。
SDK 属于本机依赖，已被 Git 忽略。CMake 会自动查找工程内的 Windows MSVC SDK；
其他位置可在配置命令增加 `-DCMAKE_PREFIX_PATH="C:/Qt/6.8.3/msvc2022_64"`，
或设置 `Qt6_DIR` 指向 `lib/cmake/Qt6`。Windows 构建后由 CMake 调用 Qt 的
`windeployqt` 复制 Qt DLL 与 `platforms/qwindows.dll` 等依赖，不执行目标程序。
Qt 来源：https://download.qt.io/online/qtsdkrepository/windows_x86/desktop/qt6_683/。

GUI 沿用默认值 < JSON < 命令行覆盖的配置优先级；`--mode publisher --help`、`--check-config`、
`--print-config`、`--list-devices`、`--list-options` 保留查询行为。
配置初始化右侧设置；分辨率、目标 FPS、视频目标码率和低延迟选项可在窗口内调整，
GOP、音频参数、采集超时、RTMP 超时和重连参数继续从配置继承。

## 窗口操作

1. 选择桌面或摄像头；摄像头可选默认设备、枚举到的设备，或手动填写设备 ID。
2. 直接在“音频源”下拉列表选择“麦克风：默认设备”、“系统声音：默认播放设备”，
   或枚举到的任意具体麦克风，不再使用第二级设备输入框。刷新设备列表会保留当前音频源及设备。
   配置文件指定但未枚举到的麦克风会作为单独选项保留，悬停可查看设备 ID。
3. 可先“开启预览”查看真实画面；预览模式只采集视频，不打开音频或网络、不写本地文件。
4. 填写 RTMP/RTMPS 地址，确认输出规格并点击“开始直播”。开始后锁定配置。
   若正处于预览，界面先异步停止预览，清理完成后再启动直播，避免并发打开同一设备。
5. 查看启动、预览、直播、重连、停止或失败状态。状态由实际流水线/网络进展决定。
6. 点击停止，等待后台清理完成，再修改配置或重新开始。地址卡片可复制当前推流地址。
7. 点击“返回首页”或关闭窗口，等待当前会话停止并释放设备后回到首页，可切换到观看功能；关闭首页退出软件。

进入窗口不会打开采集设备或连接服务器。GUI 固定使用设备输入、启用音视频和 RTMP，
禁用独立本地 MP4/M4A 写出；需要文件输入、本地录制等行为时使用 `livestreamlab --mode publisher --cli`。
桌面采集与系统声音使用现有 Windows 实现；其他平台只提供已有支持的摄像头和麦克风路径。
系统声音为默认播放设备的 WASAPI 回采，与麦克风二选一，不进行混音。
设备枚举失败时可查看错误；摄像头仍可手动填写 ID，麦克风可通过配置文件或 `--audio-device` 指定。
枚举失败不阻止使用不依赖摄像头/麦克风的桌面/系统源。

界面使用 Qt 系统字体与输入法，Windows 优先使用 Microsoft YaHei UI，字体不随工程分发。
配置、设备名和日志在 Qt QString 与现有 UTF-8 接口之间转换。

## 显示口径

| 项目 | 计算方式 |
|---|---|
| 直播时长 | 首次采样到成功发出的媒体包时开始，以单调时钟累计，重连期间继续计时；首次采样误差约不超过 250 ms，受调度影响 |
| 实际视频 FPS | 最近采样窗口内实际提交编码器的视频帧数差值 / 窗口秒数，并非配置的目标 FPS |
| 推流码率 | 最近采样窗口内成功交给 `av_interleaved_write_frame` 的音视频编码 payload 字节差值 × 8 / 窗口秒数，界面以 kbps 显示 |
| 本地画面 | 视频处理后的 RGB24 预览副本，约 8 FPS，最长边按 960x540 范围等比缩小；独立于直播编码目标 FPS |

快照约每 250 ms 更新，停止/失败后冻结直播时长并清零即时 FPS/码率。
码率不含 FLV、RTMP、TCP/IP 头部或重传开销，也不证明服务器或观众已经收到。
启动或首次成功发包前，直播时长为零；连接中和重连中有独立状态。
预览模式沿用现有视频预处理/编码线程，以复用停止和资源所有权；仍有视频编码 CPU 开销，
但不创建 RTMP 发送器，时长和推流码率保持为零。预览仅保留最新一帧，不增加帧队列积压。
事件日志来自现有 Logger，有界缓存最多 512 条，界面最多保留 1000 个文本块；
FFmpeg 直接写入 stderr 的原始诊断不等同于此日志缓存。

## 线程与生命周期

`src/publisher/ui/PublisherControl.*` 负责 Qt 主线程、输入、布局和 QImage 显示；
`src/publisher/pipeline/PublisherSession.*` 负责每次启动的配置副本、后台线程、
独立停止标志与加锁状态快照；`PublisherPipeline` 继续拥有媒体线程和队列。

点击停止只提交请求，流水线执行输入/网络中断、关闭队列和 join，不在按钮回调中等待。
返回首页或关闭功能窗口同样请求停止，并继续处理窗口事件、展示停止状态，待会话结束后回到首页。
会话析构会停止并 join 工作线程；日志对象必须活到会话销毁之后。
Qt 控件不在采集、编码或网络线程更新；主线程用 QTimer 轮询不可变帧、状态和日志副本。
GUI 会话不安装进程信号处理器，原 CLI 的 Ctrl+C 行为保留。

## 验证范围

2026-10-27：统一 GUI 程序 `livestreamlab` 已通过 CMake 的 Windows Release 编译和链接，
运行依赖与默认配置已部署。本次未启动 GUI、未运行测试或真实采集/推流；
显示与模式切换仍待运行验收。下列记录属于整合前的独立入口构建。

2026-10-27：Qt Widgets 版本通过 CMake 配置及 Windows Release 的 `publisher_gui`、`publisher`、
`player` 编译和链接；Qt Core/Gui/Widgets DLL 和 `platforms/qwindows.dll` 已部署到产物目录，
`LSL_BUILD_TESTS=OFF`。本阶段按用户要求不运行测试、GUI、真实设备采集或推流；
真实字体/输入法、设备选择、连续启停和 SRS 推流表现仍需后续运行验收。

同日音频选择调整：枚举到的全部麦克风和系统声音入口统一列在“音频源”下拉框，
显示设备名并将源类型与设备 ID 一起提交，移除单独的可编辑麦克风设备框。
刷新保留当前选择，配置指定但未枚举到的设备不自动替换为默认设备。
本轮已通过 CMake 的 Release `publisher_gui` 编译和链接；按要求未运行测试、GUI
或实际设备采集，显示与设备选择效果尚未运行验证。
