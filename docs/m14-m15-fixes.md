# 采集恢复与播放器黑屏修复

本文记录 2026-10-26 的实现和验证。当时的桌面后端是 `gdigrab`。当前桌面采集见 [音视频输入](capture.md)。

## 修复机制

- **桌面采集 EIO**：`gdigrab` 临时读包失败时，在原 `capture.timeout_ms` 预算内重建 GDI 输入和 BMP 解码器，保留共同时间轴，停止可取消等待。超过预算仍明确失败；错误 5 的具体系统诱因尚未确定，恢复不绕过权限。
- **播放器黑屏**：旧策略按解码年龄/PTS 跨度删除尚未到显示时间的帧，并不断替换首个同步候选。现在解码帧队列按容量和 64 MiB 上限背压，协调循环按播放时钟丢迟到帧，首次对齐保留候选；换代与停止可打断等待。

代码位于 `publisher/capture/MediaInput.cpp`、`player/pipeline/VideoFrameQueue.hpp`、`PlayerPipeline.cpp` 和 `player/sync/AVSynchronizer.cpp`。主播原始视频仍允许丢旧帧，详见 [线程与队列](threading.md)、[音画同步](av-sync.md)。

## 历史验证（2026-10-26，已获当时授权）

| 项目 | 结果 |
|---|---|
| Release 构建、CTest | 当时的 publisher/player 编译链接成功，基础、播放器、桌面采集、CLI 四组通过 |
| 故障注入 | 覆盖连续 3 次错误 5 后恢复、时间戳单调、持续失败总超时、恢复等待中取消 |
| 同一 HTTP-FLV 合成输入 | 修复前解码 437 帧、显示 0 帧；修复后约 90 秒显示 2199 帧，约 25 FPS，目视确认动态画面 |
| 本机 RTMP 接收/HTTP-FLV 转发 | 自研推拉流约 30 秒、约 25 FPS、无解码错误；断流后重连到 session=2 并恢复画面 |
| 桌面 + 系统声音持续采集 | 660 秒、3 次注入后恢复；编码 16500 帧、音频 31680480 样本/声道，SIGINT 后正常退出 |

历史日志目录为 `build/repair-validation/`，关键记录包括 `before-player3.log`、`after-player.log`、`rtmp2-publisher.log`、`rtmp2-player.log`、`reconnect-player.log`、`desktop-soak.log`；CTest 记录曾位于 `build/cmake/Testing/Temporary/LastTest.log`。构建目录可能被清理，这些路径不是当前文件存在性保证。

该次网络验证使用本机 FFmpeg 接收/转发，未启动或重启 SRS；660 秒采集项没有录像或网络输出。真实锁屏/安全桌面切换、摄像头热拔插、30 分钟/2 小时稳定性和竞态检测器尚未覆盖。上述记录不代表后续统一 GUI 已通过运行验收。
