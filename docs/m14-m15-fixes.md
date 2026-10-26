# M14/M15 运行故障修复与验证

日期：2026-10-26。用户本次明确允许测试。构建继续使用 CMake。

## 故障与修改

1. 主播端：截图中的 `gdigrab: Failed to capture image (error 5)` 导致读包返回 `EIO`，
   原采集线程立即中止整个流水线。现在仅对桌面输入的这类读包失败重建 GDI 输入及 BMP 解码器，
   在原始读包超时预算内重试，保持音视频共同时间轴；停止会唤醒等待。
   这是对临时采集故障的恢复，截图本身不足以确定其具体系统诱因。
2. 播放器：视频解码快于音频实际播放时，原队列按解码年龄和 PTS 跨度删掉尚未到显示时刻的帧，
   同步器又会超时替换等待中的首帧，导致视频不断追到更远的未来。
   已解码队列现在按容量及 64 MiB 内存上限背压，主线程按播放时钟判断迟到，首次对齐保留候选帧。
   网络换代和停止均可取消队列等待。

修改归属：`publisher/capture/MediaInput.cpp`、`player/pipeline/VideoFrameQueue.hpp`、
`player/pipeline/PlayerPipeline.cpp`、`player/sync/AVSynchronizer.cpp`。
主播端原始视频队列仍按实时年龄丢旧帧；播放器参数口径见 [配置说明](configuration.md)。

实现依据：[FFmpeg gdigrab 源码](https://github.com/FFmpeg/FFmpeg/blob/master/libavdevice/gdigrab.c)
中 `BitBlt` 失败返回 `AVERROR(EIO)`，输入关闭时释放持有的 DC 与位图。

## 验证结果

- Windows Release 的 `publisher`、`player` 及回归测试通过 CMake 编译、链接。
- CTest 四组全部通过：基础配置/日志、播放器队列/同步、桌面采集、CLI。
  播放器包含 13 项定向断言；桌面测试在当前测试进程内注入 `BitBlt` 错误 5，
  覆盖连续 3 次失败后恢复、单调时间戳、持续失败的总超时预算、等待中取消，
  以及真实桌面/系统音频完整流水线的恢复和正常停止。不会修改磁盘 DLL 或系统设置。
- 同一 HTTP-FLV 合成输入：修复前解码 437 帧，显示/上传 0 帧，音频仍播放，
  视频持续提前约 650–700 ms；修复后连续约 90 秒，显示/上传达到 2199 帧，
  稳态渲染约 25 FPS，无视频解码错误或视频帧丢弃，已目视确认动态画面。
- 自研 publisher → 本机 FFmpeg RTMP 接收/HTTP-FLV 转发 → 自研 player：
  约 30 秒正常推拉流，渲染约 25 FPS，解码错误为 0；已目视确认画面。
  随后主动关闭播放器，转发器失去唯一接收端并关闭上游，产生的断流错误属于本次结束操作。
- HTTP-FLV 断流后恢复同一地址：播放器由 `RECONNECTING` 回到 `STREAMING session=2`，
  重建两轨后继续显示约 25 FPS，已目视确认恢复画面；Escape 退出返回 0。
- 真实桌面（2560×1600）与系统音频流水线持续 660 秒：注入 3 次采集错误后恢复，
  最终编码视频 16,500 帧，输出 25 FPS，处理音频 31,680,480 样本/声道；
  SIGINT 后所有阶段正常结束，队列排空，进程返回 0。此项没有录像或网络输出。

原始记录在 `build/repair-validation/`：`before-player3.log`、`after-player.log`、
`rtmp2-publisher.log`、`rtmp2-player.log`、`reconnect-player.log`、`desktop-soak.log`；
CTest 记录在 `build/cmake/Testing/Temporary/LastTest.log`。

## 复查命令与边界

```powershell
cmake -S . -B build/cmake -DLSL_BUILD_TESTS=ON -DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON
cmake --build build/cmake --config Release --target publisher player common_tests player_playback_tests desktop_capture_tests --parallel 1 -- /nr:false
ctest --test-dir build/cmake -C Release --output-on-failure
.\build\cmake\bin\Release\desktop_capture_tests.exe --soak-seconds 660
```

持续采集故障超过 `capture.timeout_ms` 仍会明确失败；恢复机制不绕过系统权限。
本次本机网络验证使用 FFmpeg 接收/转发，未启动或重启 SRS。
未验证真实锁屏/安全桌面切换、摄像头热拔插、30 分钟/2 小时稳定性及竞态检测器。
