# 日志与错误处理

## 日志

`common/logger/Logger.hpp` 提供线程安全的 DEBUG / INFO / WARN / ERROR 过滤，
输出到 stderr、追加文件及可选有界内存日志。自动创建文件父目录；
记录 UTC 时间、级别、模块、线程 ID、错误码和消息，换行转义、输出立即刷新。
stdout 留给 JSON/命令结果，配置见 [配置管理](configuration.md)。

```cpp
lsl::logging::Logger log({lsl::logging::Level::Info, true, "logs/player.log"});
log.log(lsl::logging::Level::Error, "decoder", "decode failed", result);
```

`log()` 输出失败返回 false，不向外抛异常；日志文件打不开时构造器抛 `AppError`。
GUI 通过 `enableMemoryLog()` 与 `linesSince(cursor)` 轮询日志。
同步日志/写盘不用于实时音频回调。

## 错误与清理

`common/error/Error.hpp` 定义 `AppError`、`ConfigError`、`FFmpegError`，
携带模块、原错误码、消息和 `recoverable`。
`throwIfFFmpegFailed(result, module, operation)` 在负返回值时抛 `FFmpegError`，
内部用 `av_strerror` 转换消息。

EAGAIN/EINTR 与明确标为 network 的部分超时/连接/服务端错误可恢复；
标记本身不重试，由调用方执行 [网络恢复](network-reliability.md)。
send/receive 的 EAGAIN、EOF 与主动取消先按调用点的正常控制流程处理。
应用错误码：1001 配置、1003 I/O、1004 待实现、1005 未预期异常。
入口捕获并记录，配置错误退出码 2，其余 1。

媒体 RAII 位于 `common/media/FFmpegResources.hpp`、`SDLResources.hpp`。
`referenceFrame/referencePacket` 创建独立结构、引用底层缓冲；正常返回/异常展开由析构释放。
输入、编解码与封装上下文在所属线程停止后释放，完整回收顺序见 [线程说明](threading.md)。
构建见 [README](../README.md)。
