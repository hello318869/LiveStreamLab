# 当前 FLV / RTMP 输出

M07 使用 FFmpeg 的 FLV muxer，将 H.264 视频与 AAC 音频封装后通过 RTMP/RTMPS 写出。
RTMP 地址沿用 `output.url`，显式打开 `output.streaming` 才建立网络连接。
SRS 接收与多观众分发的部署管理属于 M08；当前自研 `player` 尚未实现网络播放。

编码器以 global header 提供 H.264 SPS/PPS 和 AAC AudioSpecificConfig。
复用器先创建输出流、复制编码参数，再写 FLV 头和序列头；每次重连重建完整会话。
队列按 DTS 协调顺序，最终使用 `av_interleaved_write_frame()`；不能按视频 PTS 排列 B 帧。

编码时间基分别为视频 `1/fps` 与音频 `1/sample_rate`。`avformat_write_header()` 可能调整流时间基，
写包前以实际流时间基 rescale PTS、DTS 与 duration。音视频共享会话起点，保留相对偏移，
不会让每条轨单独从零开始。重连和溢出恢复时从视频关键帧开始，并让音频使用相同边界。

推流字节统计为成功写出路径接受的编码 payload；它不包含协议头，也不是服务器确认接收量。
FLV 的音频首尾延迟表达能力与 M4A 不同，不能将本地 M4A 的样本级裁剪行为等同于直播播放。
使用、超时、恢复策略和未运行验收边界见 [M07 说明](rtmp-streaming.md)。
