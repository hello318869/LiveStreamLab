# 日志与错误处理

## 日志

`common/logger/Logger.hpp` 提供一个线程安全的 `Logger`：
DEBUG / INFO / WARN / ERROR 过滤、stderr 与文件输出。
文件追加写入，自动创建父目录；每条记录包含 UTC 时间、级别、模块名、线程 ID、错误码和消息。
消息中的换行转义，输出后立即刷新。配置字段见 [configuration.md](configuration.md)。

```cpp
#include "common/logger/Logger.hpp"

lsl::logging::Logger log({lsl::logging::Level::Info, true, "logs/player.log"});
log.log(lsl::logging::Level::Info, "decoder", "decoder opened");
log.log(lsl::logging::Level::Error, "decoder", "decode failed", -1);
```

`log` 输出失败返回 `false`，不向外抛异常。日志文件打不开时抛出 `AppError`；
程序入口会输出诊断。stdout 留给 JSON 和命令结果。同步写盘不适合实时音频回调。

## 错误

`common/error/Error.hpp` 保留基础 `AppError`、配置异常 `ConfigError` 和
`FFmpegError`。错误记录模块名、原始错误码、消息及 `recoverable`。
已有模块按需定义类型，后续模块实现时再扩展。

`ffmpegErrorString` 使用 `av_strerror` 转换错误码；
`throwIfFFmpegFailed` 仅在返回值小于零时抛出 `FFmpegError`。

```cpp
lsl::error::throwIfFFmpegFailed(result, "decoder", "avcodec_open2");
```

EAGAIN/EINTR 标记为可恢复；明确标为 `network` 的超时、连接重置/拒绝、管道断开、
HTTP 服务端错误也标记为可恢复。其他错误默认不可恢复。
标记供调用方决定后续行为，目前没有实现重试或重连。
send/receive 等 API 的 EAGAIN、EOF，以及主动取消，需先在调用点处理正常控制流程。

应用错误码：1001 配置错误，1003 I/O，1004 功能待实现，1005 未预期异常。
入口统一捕获异常并记录诊断，配置错误退出码为 2，其余为 1。

## 清理与验证

文件使用 `ifstream/ofstream`，Windows 参数缓冲使用带删除器的 `unique_ptr`，
依靠析构在正常返回和异常展开时释放资源。
M01.5 的 `common/media/FFmpegResources.hpp` 与 `SDLResources.hpp` 已提供媒体对象 RAII。
`referenceFrame/referencePacket` 通过 FFmpeg 引用计数创建独立帧/包结构。
M02 每个输入独占格式与解码上下文，停止后在所属线程释放。

历史记录（2026-10-11）：精简后 Windows Release / Debug 均构建成功，各 2/2 CTest 通过。
单元测试覆盖 600 条并发日志完整性、追加/过滤、FFmpeg 错误元数据以及异常时实际
AVDictionary 资源释放；入口测试覆盖中文路径、stdout/stderr、配置覆盖和错误退出码。
本次 M01/M02 更新按要求未构建、未运行测试；历史结果不作为本次修改的验证结果。
直接 CMake 构建命令见 [README](../README.md)。
