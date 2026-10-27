# M16.2 观众控制

统一程序 `livestreamlab` 的“我要观看”使用 Qt Widgets，沿用主播控制的深色风格。视频嵌入同一窗口，
复用现有网络解复用、视频/音频解码、SDL3 音频和 M13 音画同步，控制台方式通过
`livestreamlab --mode player --cli` 使用。

## 构建与启动

在工程根目录直接使用 CMake，不新增构建脚本：

```powershell
cmake -S . -B build/cmake -DLSL_BUILD_TESTS=OFF -DLSL_BUILD_GUI=ON -DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON
cmake --build build/cmake --config Release --target livestreamlab --parallel
.\build\cmake\bin\Release\livestreamlab.exe --mode player
```

需要 Qt 6.5+ Widgets SDK，本机复用已安装的 `external/qt/6.8.3/msvc2022_64`。
其他位置通过 `CMAKE_PREFIX_PATH` 或 `Qt6_DIR` 指定。Windows 构建后，CMake 复制 FFmpeg、
SDL3 DLL，并用 `windeployqt` 部署 Qt DLL 与 `platforms/qwindows.dll`，不执行播放器。
单配置生成器的程序位于 `build/cmake/bin/`。如只构建 CLI，设置
`-DLSL_BUILD_GUI=OFF` 可完全跳过 Qt 查找，仍使用 `livestreamlab --mode player`。
不带参数启动 GUI 程序时先显示功能首页。GUI 默认读取程序旁 `config/player.json`，
CMake 构建时复制配置；显式 `--config` 优先，其相对路径按当前工作目录解释。

`--config`、`--url`、初始音量/静音/暂停、网络超时、重连、缓存和同步参数继续使用现有配置规则。
这些模式参数需配合 `--mode player`；`--help`、`--print-config`、`--check-config` 保持查询行为，不进入播放窗口。

## 窗口操作

1. 输入 RTMP/RTMPS 或 HTTP/HTTPS-FLV 直播地址，点击“连接直播”。进入窗口不会自动拉流。
2. 查看连接、缓冲、播放、暂停、重连、停止或失败状态；连接成功本身不等同于已经播放。
3. 用静音按钮和 0–100 音量滑块控制当前播放器，不修改系统音量；静音与音量是独立设置。
4. 暂停/继续沿用现有音视频联合暂停能力。直播继续在服务器端运行，恢复时仍受现有缓存/换代策略约束。
5. 使用全屏按钮、F、Alt+Enter 或双击视频画面切换全屏。Esc 退出全屏，恢复之前窗口状态。
   在地址或日志输入控件中操作时，普通字母快捷键不会吞掉输入文字。
6. 点击停止后等待后台回收完成，再修改地址并重新连接。点击“返回首页”或关闭窗口也先请求停止，
   等待清理后回到首页，可切换开播功能；关闭首页退出软件。

音频关闭或音轨不存在时不打开实际音频设备。只有音频的流可播放声音，视频区域显示相应状态；
无可用新代视频帧时清除旧连接画面。终态保留错误信息供查看，不把失败伪装成正常停止。
可展开运行日志；日志使用现有 Logger 的有界内存副本，最多显示 1000 个文本块。

## 视频与线程边界

`src/player/ui/PlayerControl.*` 负责 Qt 主线程中的窗口、输入、QImage 和状态显示。
`pipeline/PlayerSession.*` 负责配置副本、独立停止/控制状态、后台协调线程和线程安全快照。
`runPlayerPipeline` 接收 `IVideoRenderer` 与音频输出接口，复用原有解码与音画同步调度；
CLI 包装仍在主线程创建 SDL 视频窗口、处理 SDL 事件和信号，GUI 后台不创建 SDL 视频窗口。

SDL 音频子系统在 Qt 主线程构造和销毁；后台协调线程应用音量、静音和暂停命令，
输入、音频与视频工作线程终止并 join 后才允许销毁音频对象。
Qt 定时器同时在主线程排空 SDL 音频/设备事件，以执行默认设备切换或断开所需的主线程回调；
不创建 SDL 视频窗口，也不把 Qt 输入转发给 SDL。
停止按钮只提交请求，Qt 定时器继续处理窗口事件和显示停止状态。
每次重新连接建立新的媒体资源和状态，旧会话完成回收后才允许启动新会话。

同步器决定一帧应显示时，后台把视频转换为 RGB24 不可变副本；按像素宽高比修正显示比例，
等比限制在 1920×1080 内。会话只保留最新一帧，Qt 主线程复制成 QImage 并等比绘制，
不直接读取 FFmpeg 帧或媒体队列。转换跟随实际播放调度，不使用主播预览的 8 FPS 节流。
换代、重连、停止时撤销旧画面，修订号保证界面可观察到清帧。

状态根据实际网络阶段与媒体播放进展判断，不依靠按钮点击猜测。
界面时长为首次实际媒体输出后的会话播放经过时间，暂停时冻结，重连期间继续累计；
它不是流内的媒体 PTS 或逐样本播放位置。
画面刷新受 Qt 事件调度和 RGB 转换成本影响；本阶段不把后台帧提交数当作实际屏幕刷新 FPS，
不显示未经测量的观众人数。后续 M16.3 已新增帧/吞吐/队列统计，以及通过捕获时间戳测量的
端到端估计；真实屏幕刷新率仍不由后台提交数推断，详见[性能统计](performance-statistics.md)。

## 验证范围

2026-10-27：统一 GUI 程序 `livestreamlab` 已通过 CMake 的 Windows Release 编译和链接，
运行依赖与默认配置已部署。本次未启动 GUI、未运行测试或真实拉流；
显示与模式切换仍待运行验收。下列记录属于整合前的独立入口构建。

2026-10-27：通过 CMake 配置及 Windows Release 的 `player_gui`、`player`、`publisher_gui`
编译和链接；Qt Core/Gui/Widgets DLL、Windows 平台插件、SDL3 和 FFmpeg DLL 已部署至程序目录，
`LSL_BUILD_TESTS=OFF`。本阶段按用户要求不运行测试、播放器、真实拉流或 SRS 验证；
真实画面、声音、全屏、连续启停和断流恢复仍需后续运行验收。
