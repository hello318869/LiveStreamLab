# FLV / RTMP 与时间戳

主播把 H.264 + AAC 经 FFmpeg FLV muxer 写入 RTMP/RTMPS，地址为 `output.url`，须显式启用 `output.streaming`。SRS 在客户端进程之外接收、分发；观众支持 RTMP/RTMPS 和 HTTP/HTTPS-FLV。

- **序列头**：编码器提供 H.264 SPS/PPS、AAC AudioSpecificConfig；复用器创建输出流、复制参数、写容器头，每次重连重建完整会话。
- **排序与时间基**：队列按 DTS 选包，经 `av_interleaved_write_frame()` 写出。视频编码时间基为 `1/fps`，音频为 `1/sample_rate`；写头后使用实际流时间基换算 PTS、DTS 和 duration。
- **共同时间轴**：音视频使用同一会话起点并保留偏移；重连/溢出后以视频关键帧恢复，音频跟随同一边界。
- **统计口径**：发送字节是成功写出路径接受的编码 payload，不含协议头，不等于服务器确认接收量。FLV 不具有与 M4A 相同的样本级首尾裁剪语义。

使用和恢复见 [RTMP 推流](rtmp-streaming.md)、[网络解封装](network-demuxing.md)；播放时间调度见 [音画同步](av-sync.md)。
