# 主播控制（M16.1）

统一程序的“我要开播”使用 Qt Widgets，复用现有采集、编码、RTMP 与重连链路。
构建、Qt SDK 查找与程序路径见 [README](../README.md)；控制台用法见 [RTMP 推流](rtmp-streaming.md)。

## 启动与配置

双击 `livestreamlab.exe` 后，在直播大厅右上角点击“我要开播”，进入平台登录和开播流程。 [平台客户端](platform-client.md)
也可以用 `livestreamlab --mode publisher` 打开手填推流地址的直连主播工具。
**进入主播窗口时会枚举设备，点击预览或开始直播后才启动媒体。**
GUI 默认读取程序旁 `config/publisher-stream.json`，显式 `--config` 的相对路径按工作目录解释。
配置优先级是默认值 < JSON < 命令行；`--help/--check-config/--print-config/--list-devices/--list-options`
保留查询行为。

窗口可调整分辨率、FPS、视频目标码率和低延迟；GOP、音频、采集/RTMP 超时、重连参数从配置继承。
GUI 固定设备输入、音视频与推流，禁用本地 MP4/M4A。文件输入或本地录制使用
`livestreamlab --mode publisher --cli`，详见 [输入](capture.md)。
GUI 需要 Qt 6.5+ Widgets；非默认 SDK 可设置 CMAKE_PREFIX_PATH 或 Qt6_DIR。
Windows 由 CMake 调用 windeployqt 部署 DLL 与 platforms/qwindows.dll，不启动程序。

## 窗口操作

1. 选择桌面或摄像头；摄像头可枚举选择或手填 ID。
2. 在一个“音频源”列表选择默认/具体麦克风或默认播放设备的系统声音。
   刷新保留源及设备，配置指定但未枚举到的麦克风仍单独保留，悬停查看 ID。
3. 可先开启预览：仅开视频，关闭音频、网络和文件输出；仍走视频预处理/编码链，有编码 CPU 开销。
4. 直连时填写 RTMP/RTMPS 地址，平台开播时使用服务端分配的地址。确认规格后开始直播，启动后锁定设置。
   正在预览时会先异步停止并释放设备，再开始直播。
5. 停止后等待后台清理，再调整设置/重新启动；地址卡片可复制推流地址。
   返回或关闭功能窗口时也会先停止会话。平台主播窗口返回直播大厅，关闭大厅退出软件。

桌面/系统声音仅支持 Windows；其他平台沿用摄像头/麦克风路径。
系统声音与麦克风二选一，不混音；不依赖摄像头/麦克风的桌面/系统源不因枚举失败而禁止启动。
未列出的麦克风可在配置或 `--audio-device` 指定。
界面使用 Qt 系统字体/输入法，Windows 优先 Microsoft YaHei UI；接口以 UTF-8 与 QString 互转。

## 画面、状态与统计

| 项目 | 口径 |
|---|---|
| 本地画面 | 预处理后 RGB24 副本，最多约 8 FPS，等比缩小至 960×540 范围；只保留最新帧 |
| 直播时长 | 首次采样到已发送媒体包时起算，重连继续累计；采样约 250 ms，受调度影响 |
| 视频 FPS | 最近采样窗口提交编码器的帧数/秒，区别于目标 FPS |
| 推流码率 | 最近窗口成功写 mux 的编码 payload bit/s，界面显示 kbps |

连接/重连/失败由真实会话快照驱动。开始成功发包前时长为零；预览时长和码率为零；
停止/失败后冻结时长、清零即时速率。码率不含协议头/重传，也不证明观众已收到。
完整指标见 [性能统计](performance-statistics.md)。

Qt 定时读取不可变帧、状态和日志副本，不在媒体线程更新控件。
Logger 内存最多 512 条，界面最多 1000 个文本块；FFmpeg 原始 stderr 不属于该缓存。

## 线程与退出

`PublisherControl` 负责主线程控件与 QImage 显示，`PublisherSession` 保存配置副本、后台工作线程、
停止标志和加锁快照，`PublisherPipeline` 拥有媒体线程/队列。
按钮只提交停止请求，关窗期间继续处理事件，媒体清理后关闭主播窗口。
Session 析构兜底停止并 join，Logger 必须晚于 Session 释放。GUI 不安装进程信号处理器，CLI 保留 Ctrl+C。
详见 [函数调用顺序](function-call-order.md)和[多线程流水线](threading.md)。
