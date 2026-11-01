# 音源选择与弱音频排查

## 日常排查

先确认音源：`microphone` 读取指定或默认麦克风；`system` 回采 Windows 默认播放端点，不含麦克风。当前二选一、不混音；文件输入读取文件音轨。具体命令见 [采集](capture.md)。

查看启动日志中的实际设备名，以及处理日志的输入/输出 RMS、峰值和 `clipped_audio_values`。RMS 越接近 0 dBFS，数字信号电平越高；低 RMS 不等于编码静音。系统音源空闲时可能只有静音包或暂时无包。

软件增益 `preprocess.audio.gain_db` 默认 0，范围 -60–30 dB，不改变系统设备音量。以下命令在工程根目录录制麦克风并放大 20 dB；文件已存在时换名，Ctrl+C 正常写尾：

```powershell
.\build\cmake\bin\Release\livestreamlab.exe --mode publisher --cli --capture-video false --audio-source microphone --audio-gain-db 20 --audio-file output/microphone-gain20.m4a
```

增益只能放大已有信号；削波计数增加时应减小增益。相对输出路径以启动工作目录为准，固定位置请使用绝对路径。

## 历史诊断记录（2026-10-24）

当时“生成的 M4A 听不到声音”的文件能正常解码；原文件保留，另存了 +20 dB 的 `output/audio-fixed.m4a`。证据指向输入电平偏低，未复现 AAC 把正常信号编码为静音。

| 检查 | 当时结果 |
|---|---|
| 原文件，约 5.52 秒、48 kHz 双声道 | RMS -50.02 dBFS，峰值 -29.07 dBFS，存在非零信号 |
| 直接读取默认麦克风 | RMS 约 -50.1 dBFS，与原文件接近 |
| 修复文件 | 处理后 RMS -30.02 dBFS，峰值约 -9.07 dBFS，0 削波；AAC 重新解码约 -30.2 dBFS |
| PCM 增益检查 | 3732 项通过，覆盖 12 种 packed/planar 格式、正负增益、饱和及尾部 |
| 合成输入 | 997 Hz、3 秒解码 144000 样本/声道；44.1 kHz 单声道重采样解码 59040 样本/声道 |
| 真实设备 | 麦克风 +20 dB、0 削波；WASAPI 短测试音编码前后 RMS 约 -17.43 dBFS |

诊断没有修改 Windows 默认设备、音量或静音。临时诊断样本、探针及测试工具已清理；上述结果对应当时环境，最终系统播放器听感未由自动检查确认。
