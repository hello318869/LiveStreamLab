# N01：SRS 与后端环境基线

记录日期：2026-10-28（Asia/Shanghai）。本次只读取现有发行版、包元数据、源码和配置，未启动/重启服务，未调用业务或媒体接口，未推拉流，未执行测试或数据库 SQL。

## 本机实际情况

WSL 发行版为 `Ubuntu`，系统为 Ubuntu 24.04.3 LTS。2026-10-28 检查时，Ubuntu 中未找到运行中的原生 `srs`、`mysqld` 进程；这不证明其他 Windows 服务、远端主机或 Docker 容器中没有同类服务。Ubuntu 的 Docker 命令提示未启用此发行版集成，未继续操作 Docker。

现有 SRS 位于 `/home/asusjian/srs`，Git HEAD 为
`89bdba34ba55175008a1f7749f11d842a90ca6b7`，`git describe` 为
`v8.0-d0-8-g89bdba34b`，检查时工作树无改动。版本选择文件
`trunk/src/core/srs_core_version.hpp` 包含 `srs_core_version8.hpp`，版本为 **8.0.58**。
`trunk/objs/srs` 可执行文件存在；`trunk/objs/srs.log` 最近一次已有启动记录为
2026-10-27，记录 `SRS/8.0.58` 并标注非稳定版本。本次没有执行该程序来重新获取版本。

N03 的首个 SRS 适配目标固定为上述提交与 8.0.58 字段，不用方案引用的 v6 文档推定现有环境。SRS 官方也将 [8.0 文档](https://ossrs.net/lts/en-us/docs/v8/doc/http-api)标为 Unstable；本阶段不升级、降级或替换现有 SRS。后续更换版本必须重新核对字段与行为。

读取的配置为 `/home/asusjian/srs/trunk/conf/srs.conf`。它是现有磁盘文件，检查时没有正在运行的原生进程，不能称为已经核实的运行中配置：

| 配置项 | 文件中内容 |
|---|---|
| RTMP | `rtmp { listen 1935; }`，使用该提交的 v8 配置组织 |
| HTTP API | 已启用，端口 `1985` |
| HTTP 文件服务 | 已启用，端口 `8080` |
| HTTP-FLV | 默认 vhost 开启 `http_remux`，挂载 `[vhost]/[app]/[stream].flv` |
| HLS | 默认 vhost 已启用 |
| RTC | 已启用，UDP `8000`；RTMP/RTC 互转关闭 |
| 回调 | 文件未配置 `http_hooks`，没有平台发布鉴权 |
| daemon | `on` |

本次不覆盖上述文件。[平台配置模板](../deploy/srs-platform.conf) 使用相同 v8 语法、RTMP/HTTP-FLV 和回环管理地址；N03 已加入回调地址，部署前须替换共享密钥占位符。HLS/RTC 按该版本默认关闭。N03 服务端鉴权已实现，但模板尚未应用，不能据此认定现有 SRS 已受平台控制。

## N03 的标识映射

以下根据本机固定提交的 `trunk/src/app/srs_app_http_api.cpp`、
`srs_app_http_hooks.cpp`、`srs_app_statistic.cpp` 核对，不是接口调用结果。

| 来源 | 原始字段 | 后端含义 |
|---|---|---|
| HTTP API 顶层 | `server` | `srs_server_id`，配置产生的服务器标识 |
| HTTP API 顶层 | `service` | `srs_service_id`，当前进程实例生成的标识 |
| 流对象 | `id` | `srs_stream_id` |
| 流对象 | `name`、`app`、`url` | 校验预期流名称、应用与 vhost/流路径 |
| 流对象 | `publish.active`、`publish.cid` | 当前是否有发布者及其连接标识 |
| 客户端对象 | `id`、`stream`、`publish` | 发布连接标识、流 ID、是否为发布连接 |
| 发布/停播回调 | `server_id`、`service_id`、`client_id` | 与 API 服务器、实例、发布连接对应 |
| 发布/停播回调 | `stream_id` | 若统计对象已存在才附带，首次回调可能没有 |
| 发布/停播回调 | `vhost`、`app`、`stream`、`param` | 校验目标并从查询参数提取推流凭证 |

全部 SRS 标识按不透明字符串处理。`server_id` 不能替代进程实例标识；
`service_id` 变化必须废弃原观察基线。对账和回调写入同时检查
`live_session_id + server_id + service_id + stream_id + client_id + connection_generation`。
首次合法 `on_publish` 可以先记录其可用标识，再通过预期流路径与发布连接查询补齐流 ID。
来源 IP 只用于回调入口的可信来源校验，不能充当发布连接身份。

只允许可信来源和正确共享密钥访问回调，未实现鉴权时拒绝发布。
SRS 回调响应使用原生数值 `code`，接受时 HTTP 200 + `{"code":0}`；拒绝时非零值，
不使用业务 API 的字符串错误码包装。确切提交的 `do_post` 支持该 JSON 格式，参见
[官方回调说明](https://ossrs.net/lts/en-us/docs/v8/doc/http-callback)。

该提交的 SRS 回调实现会在日志中记录回调 URL 和请求体，包含 `param`。
已交付针对本提交的 [定向脱敏补丁](../deploy/srs-http-hooks-redaction.patch)，处理回调 URL/正文、嵌套 HTTP 错误和 RTMP 连接参数中的凭证，保留事件与错误码。部署回调前需审阅应用并验证该补丁；本轮未改现有源码、未应用补丁，也未构建 SRS。仅脱敏后端日志不足以覆盖原版 SRS 的这条路径。

## 媒体进展口径

该提交的流对象提供 `audio_frames`、`video_frames`、`frames`、`recv_bytes`；
客户端对象提供 `recv_bytes`。接收线程按收到的音频/视频 RTMP 消息计数，统计层按差值累计；
流对象的 `frames` 为音频与视频计数之和。对应实现是
`srs_app_recv_thread.cpp::SrsPublishRecvThread::consume`、
`srs_app_rtmp_conn.cpp` 的发布循环及 `srs_app_statistic.cpp::SrsStatisticStream::dumps`。

N03 实现按以下口径判定，运行表现仍待授权验收：

1. 必须成功取得 HTTP 响应并验证 JSON 数值 `code=0`，校验版本、身份元组、预期流路径、`publish.active=true`、匹配的发布连接。HTTP 成功本身不表示已收媒体。
2. 对同一身份元组至少保留两次成功观察。`audio_frames` 或 `video_frames` 增长表示观察到媒体消息进展；纯音频、纯视频均可成立。结合非空的对应轨道信息确认至少一条受支持的音视频轨道，不要求两条轨道同时存在。
3. `recv_bytes` 是接收字节的辅助指标，包含协议流量；只增加字节或只有 TCP 连接不足以转为 `LIVE`。不使用 30 秒平滑码率作为 15 秒无媒体超时的唯一依据。
4. 换发布连接、换实例、流对象重建、连接代次变更或计数回退时重新建立基线，不能把旧累计值当作新连接的进展。按当前身份条件提交异步查询结果，拒绝旧响应覆盖新连接。
5. 默认约每 3 秒对账；连续成功查询且相同发布连接已超过 15 秒没有帧计数进展，才进入无媒体恢复规则。SRS 查询失败另记不可用，不推断所有会话已结束；恢复后的第一次成功观察重新建立超时基线，不能把不可观测的时间当作已证实停滞。
6. 这些计数证明接收到音视频消息，可能包括序列头，不证明关键帧可解码或观众已有声音画面；完整可播性留给 N07 授权后的真实链路验收。

N03 使用 `GET /api/v1/versions`、`GET /api/v1/streams/{id}` 和
`GET /api/v1/clients/{id}`；首次需要按流路径定位时使用分页流列表，不假定一页含全部流。
集合查询使用规范路径 `GET /api/v1/vhosts/` 和 `GET /api/v1/streams/`，保留末尾 `/`。
2026-11-01 短测确认 SRS 8.0.58 对缺少末尾 `/` 的这两个集合路径返回 HTTP 302；
Drogon HTTP 客户端不会自动跟随，会使快照对账失败并持续报告媒体服务不可用。
主动停止仅在匹配当前发布连接后使用 `DELETE /api/v1/clients/{id}`。
删除成功表示结束请求已受理，最终状态仍由回调或对账确认连接消失。
API 同时检查 HTTP 状态和 JSON 数值错误码，参见 [SRS HTTP API](https://ossrs.net/lts/en-us/docs/v8/doc/http-api)。

固定源码参考：

- [API 实现](https://github.com/ossrs/srs/blob/89bdba34ba55175008a1f7749f11d842a90ca6b7/trunk/src/app/srs_app_http_api.cpp)
- [回调实现](https://github.com/ossrs/srs/blob/89bdba34ba55175008a1f7749f11d842a90ca6b7/trunk/src/app/srs_app_http_hooks.cpp)
- [统计实现](https://github.com/ossrs/srs/blob/89bdba34ba55175008a1f7749f11d842a90ca6b7/trunk/src/app/srs_app_statistic.cpp)
- [接收消息计数](https://github.com/ossrs/srs/blob/89bdba34ba55175008a1f7749f11d842a90ca6b7/trunk/src/app/srs_app_recv_thread.cpp)

以上固定链接便于后续复查；本次对确切提交的核对依据是现有本地源码。

## 构建环境记录

| 项目 | Ubuntu 本机读取结果 |
|---|---|
| 工具 | CMake `3.28.3`、Ninja `1.11.1`；`gcc`、`g++`、`git`、`pkg-config` 存在 |
| MySQL 客户端及头文件 | `mysql-client`、`libmysqlclient-dev`、`libmysqlclient21` 均为 `8.0.46-0ubuntu0.24.04.4` |
| JsonCpp | 运行库 `libjsoncpp25` 为 `1.9.5-6build1`，开发包原先未安装 |
| libsodium | 运行库 `libsodium23` 为 `1.0.18-1ubuntu0.24.04.1`，开发包原先未安装 |
| 其他已安装开发包 | OpenSSL `3.0.13-0ubuntu3.16`、UUID `2.39.3-9ubuntu6.6`、zlib `1:1.3.dfsg-3.1ubuntu2.3` |
| Drogon | 原先未找到系统 Drogon 开发包与标准 CMake 安装路径 |
| MariaDB Connector/C | 原先未安装开发包；仓库候选 `libmariadb-dev` 为 `1:10.11.14-0ubuntu0.24.04.1` |

Drogon 固定版本所需的异步 MySQL 驱动依赖 MariaDB Connector/C。
这是连接 MySQL 服务的客户端库选择，不要求把数据库服务换成 MariaDB。
构建依赖如何隔离及最终锁定版本见 [平台部署说明](platform-deployment.md)。

以上 MySQL 版本是 Ubuntu 客户端/开发包版本，没有连接数据库获取服务端版本，
也没有核实 Windows MySQL 服务、授权或网络可达性；数据库服务端须满足 SQL 所要求的 MySQL 版本。
