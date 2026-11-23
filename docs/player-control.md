# 观众控制

无参数启动 `livestreamlab` 直接打开直播大厅，匿名选择房间后自动开始播放，返回时回到大厅。平台播放器与直连工具共用网络、解码、SDL3 音频与[音画同步](av-sync.md)。构建和运行依赖统一见 [README](../README.md)。

```powershell
# 手填播放地址的直连 GUI
.\build\cmake\bin\Release\livestreamlab.exe --mode player
# CLI 使用 SDL 视频窗口
.\build\cmake\bin\Release\livestreamlab.exe --mode player --cli --config config\player.json
```

GUI 默认读取程序旁 `config/player.json`；显式 `--config` 优先，相对路径按当前工作目录解释。`--url`、声音、超时/重连、缓存和同步沿用[配置规则](configuration.md)；模式参数配合 `--mode player`。`--help/--print-config/--check-config` 只查询。

## 窗口操作

1. 直连工具中输入 RTMP(S) 或 HTTP(S)-FLV 地址，点击连接。平台播放器使用大厅选房后取得的地址自动连接。状态依实际网络及输出进展区分连接、缓冲、播放、暂停、重连、停止和失败。
2. 静音与 0–100 音量独立设置，仅影响本播放器。暂停/继续联合控制音视频；长暂停后可能按直播缓冲策略跳到新内容。
3. 全屏按钮、F、Alt+Enter、双击切换全屏，Esc 恢复原窗口状态；在地址/日志输入框中普通字母保留输入用途。
4. 停止后等后台回收，再重新连接。返回或关窗时先停止播放并等待清理，从平台播放器返回后显示直播大厅。

无音轨或禁用声音时不打开实际音频设备；纯音频显示相应状态，换代时撤下旧画面。错误保留供查看。运行日志按需展开，显示最多 1000 个文本块。

## 线程、显示与状态

`src/player/ui/PlayerControl.*` 在 Qt 主线程管理控件、QImage 和定时刷新；`PlayerSession` 持配置副本、停止/控制状态及快照，在后台调用 `runPlayerPipeline()`。SDL 音频子系统在应用主线程构造/销毁，媒体线程终止并 join 后才释放；Qt 定时器同时泵送 SDL 音频/设备事件。CLI 在主线程管理 SDL 视频与事件，GUI 后台不创建 SDL 视频窗口。

同步决定 Render 后，后台按 SAR 修正比例，转为最大 1920×1080 的 RGB24 不可变副本。Session 只保留最新一帧；Qt 每 16 ms 轮询，复制 QImage 等比绘制，paintEvent 完成后回报首次新帧的端到端延迟样本。换代/停止更新修订号以通知清帧；不沿用主播预览 8 FPS 节流。

停止按钮只发请求，定时器继续处理窗口和停止状态；前次回收结束才可重连。界面时长从首次实际媒体输出开始，暂停冻结、重连累计，并非媒体 PTS。后台提交数不等于屏幕刷新率；吞吐/队列、同步及采集到绘制的估计见[性能统计](performance-statistics.md)。详细边界见[线程模型](threading.md)、[函数调用顺序](function-call-order.md)。
