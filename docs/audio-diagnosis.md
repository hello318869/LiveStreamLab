# 音源选择与弱音频修复（2026-10-24）

## 本次问题与证据

用户报告系统播放器播放生成的 M4A 时听不到声音。原文件位于
`build/cmake/bin/Release/output/audio.m4a`，AAC-LC、48 kHz、双声道、约 5.52 秒。
FFmpeg 能正常解码，平均电平约 -50.02 dBFS，峰值 -29.07 dBFS，文件含非零信号。
直接读取默认麦克风得到约 -50.1 dBFS 的平均电平，与原文件接近。
因此本次有证据的问题是输入电平很低，未复现 AAC 编码将正常信号变为静音。

读取时 Windows 默认录音设备为 Realtek 麦克风，未静音，设备音量约 90%；
默认输出为 Realtek 耳机，未静音，设备音量 20%。这些是当时的设备状态，不保证以后不变。
检查时没有系统播放器会话，无法确认用户此前播放时该应用自身的音量。
没有修改 Windows 默认设备、音量或静音设置。

原文件保留，已通过本项目的文件输入、预处理增益和 AAC 编码链路另存
`output/audio-fixed.m4a`，增益 +20 dB。处理前后 PCM 平均电平分别为 -50.02 / -30.02 dBFS，
峰值提高到约 -9.07 dBFS，处理时削波计数为 0；重新解码后的平均电平约 -30.2 dBFS。
放大只能提升已有信号，不能恢复音源中没有采集到的内容。

## 使用方式

在 `D:\直播\LiveStreamLab` 目录执行以下命令；文件已存在时请使用其他名称。
Ctrl+C 正常停止并写完 M4A 尾部。音源二选一，当前不做同时混音。

```powershell
# 麦克风；default 跟随 Windows 默认录音设备
.\build\cmake\bin\Release\publisher.exe --capture-video false --audio-source microphone --audio-gain-db 20 --audio-file output/microphone.m4a

# 可先枚举，再通过 --audio-device 指定麦克风名称或唯一 ID
.\build\cmake\bin\Release\publisher.exe --list-devices

# 电脑正在播放的音乐、视频、游戏声音：WASAPI 默认播放端点回采
.\build\cmake\bin\Release\publisher.exe --capture-video false --audio-source system --audio-file output/system.m4a
```

JSON 对应 `capture.audio_source` 为 `microphone` / `system`，`preprocess.audio.gain_db` 为
-60..30 的整数，默认 0。文件输入始终读取文件音轨。系统声音当前仅支持 Windows。
音源空闲或程序静音时，系统回采可能只有零样本包或暂时没有包，这是正常现象。

启动日志显示 `Opened source` 的实际设备名称，处理日志增加输入与输出的 RMS dBFS、输出峰值、
`clipped_audio_values`。输出持续很小时会提示检查音源、设备或增益；削波增多时应降低增益。

相对 `output` 路径以启动工作目录为准。从 exe 所在目录启动会保存在该目录下的 `output`；
从工程根目录启动会保存在工程的 `output`。需要固定位置时可给 `--audio-file` 传绝对路径。

## 验证结果

| 检查 | 实际结果 |
|---|---|
| Windows Release CMake 构建 | publisher、音源组件和定向测试工具编译链接通过 |
| PCM 软件增益测试 | 3732 项检查通过，覆盖全部 12 种 packed/planar 格式、正负增益、饱和与重采样尾部 |
| 已知 997 Hz、48 kHz 双声道、3 秒输入 | 解码 144000 样本/声道，RMS -24.10 dBFS，时长和音调正常 |
| 44.1 kHz 单声道、1.23 秒重采样输入 | 解码 59040 样本/声道，尾帧裁剪和 AAC priming 正常 |
| 真实麦克风 +20 dB | 输入约 -52.8 / -51.9 dBFS，输出约 -32.8 / -31.9 dBFS，0 削波，M4A 解码成功 |
| WASAPI 回采短测试音 | 145440 样本/声道，预处理前后 RMS 同为 -17.426 dBFS；编码后解码约 -17.429 dBFS |

以上为当时的验证记录。按用户要求，本轮临时诊断样本、测试源码、手工探针及其构建产物已清理；
保留正式音源选择、增益功能和修复后的音频文件。硬件结果对应当时设备和环境，
未以自动化替代用户在系统播放器中的最终听感确认。
日常构建保持 `-DLSL_BUILD_TESTS=OFF`，没有新增构建脚本。
