# 网络控制服务 HTTP 契约（N01—N03）

本文件定义《多人直播系统扩展方案》N01—N03 已实现的服务端接口。
账号、个人房间、直播会话、SRS 回调及周期对账已接入 `control_server`。
接口示例不是运行验收结果，本轮未启动服务、执行 SQL 或运行测试。
后端可单独构建或从项目顶层 CMake 构建；N04/N05 客户端已接入这些接口，见 [平台客户端](platform-client.md)。

## 统一规则

- 业务入口为 `/api/v1`，请求和响应使用 UTF-8 JSON；有请求体时必须为 JSON 对象并声明 `Content-Type: application/json`。
- 对外 ID 都是字符串：用户/房间 ID 为无前导零的正十进制 `BIGINT UNSIGNED`；登录会话和直播会话 ID 为 32 位小写十六进制。
- `client_request_id` 为规范小写 UUID 字符串，例如 `20c76c80-a012-4a41-90e7-dddc92a26cf1`；同一次开播操作的重试必须复用原值。
- 时间统一为 UTC ISO 8601，使用 `Z` 后缀，允许最多六位小数；尚未发生的时间为 `null`。
- JSON 中的 `null` 表示无值；不得以空字符串或数字 `0` 代替。分页数值和布尔值使用对应 JSON 类型。
- 受保护接口使用 `Authorization: Bearer <access_token>`。不接受 query 中的登录令牌，不使用 cookie 登录态。
- 响应不包含密码哈希、令牌摘要、数据库配置或 SRS 管理地址；异常消息使用固定公开文案，不回显 SQL、异常内容或凭证。
- 参数字段只接受本文规定的类型、范围和枚举；未知字段、格式错误、缺少必填字段返回 `400 INVALID_ARGUMENT`。

普通成功响应：

```json
{"code":"OK","message":"","data":{}}
```

普通失败响应使用对应 HTTP 状态码，`data` 通常为 `null`；开播冲突允许携带本文定义的会话信息：

```json
{"code":"INVALID_ARGUMENT","message":"Invalid request","data":null}
```

此 JSON 约定适用于进入应用响应处理的请求。Drogon 在 HTTP 解析阶段拒绝的畸形报文、
超长请求行、超大请求体等可能只返回 HTTP 状态并关闭连接，响应体可以为空，无法由业务错误处理器统一包装。
客户端必须先检查 HTTP 状态；仅在响应体为有效业务 JSON 时读取 `code`，不得假定每个 `400/413/414` 都有 JSON。

涉及房间/直播状态的响应额外包含 `meta`，成功和相关失败响应均适用：

```json
{"media_service_status":"AVAILABLE","last_confirmed_at":"2026-10-28T04:00:00Z"}
```

`media_service_status` 只有 `AVAILABLE`、`UNAVAILABLE`；服务尚未确认 SRS 或查询失败时为 `UNAVAILABLE`。
`last_confirmed_at` 是后端最后成功确认 SRS 状态的 UTC 时间，尚未确认时为 `null`，不是本次响应时间。
SRS 不可用时保留最后确认的会话状态，不把旧 `LIVE` 当成新近确认的在线结果；客户端显示状态暂不可确认。

## 字段限制与公共对象

| 字段 | 首版约束 |
|---|---|
| `username` | 3–32 个 ASCII 字符，正则 `[a-z0-9_]{3,32}`；精确小写，不自动改写大小写 |
| `nickname` | 1–32 个 Unicode 标量值，不能全为空白或包含控制字符 |
| `password` | UTF-8 编码后 8–128 字节，不 trim、不做 Unicode 规范化、不回传 |
| `title` | 1–120 个 Unicode 标量值，不能全为空白或包含控制字符 |
| `category` | 固定为 `general`、`gaming`、`music`、`education`、`technology` 之一 |
| `client_request_id` | 36 字符小写规范 UUID，仅标识一次开播操作，不是登录凭证 |
| `page` | 十进制正整数，默认 `1`；超过结果末页返回空数组 |
| `page_size` | 十进制整数 `1..100`，默认 `20`；超范围拒绝，不静默截断 |

服务端先验证 UTF-8，再按 Unicode 标量值计算显示字段长度。未规定的字符串不自动裁剪或截断。
注册时默认房间标题为 `<nickname>的直播间`，分类为 `general`；注册事务同时创建唯一个人房间。

`User` 对象仅包含：

```json
{"id":"1001","username":"alice_01","nickname":"小林","created_at":"2026-10-28T04:00:00Z"}
```

`Room` 对象：

```json
{
  "room_id":"2001",
  "owner":{"id":"1001","nickname":"小林"},
  "title":"一起学习 C++",
  "category":"technology",
  "cover_url":null,
  "current_session_id":null,
  "status":"OFFLINE",
  "started_at":null,
  "updated_at":"2026-10-28T04:00:00Z"
}
```

`cover_url` 首版固定为 `null`，客户端显示内置默认封面，不为大厅卡片拉流。
`status` 由当前会话计算；无当前会话时为 `OFFLINE`，有当前会话时使用下文六种会话状态。
`OFFLINE` 是房间展示值，不写入 `live_sessions.status`。不维护另一份 `rooms.is_live`。
`started_at` 对应当前会话首次确认媒体就绪的时间；未开始或无当前会话时为 `null`。
公开房间对象不包含登录令牌、推流 URL、推流凭证、`client_request_id` 或 SRS 连接标识。

## 进程健康接口

### `GET /health`

公开的进程存活接口，HTTP `200`：

```json
{"code":"OK","message":"","data":{"service":"control_server","stage":"N03","status":"ok"}}
```

只说明 HTTP 进程能处理请求，不探测 MySQL/SRS，不表示数据库已迁移、媒体已就绪或系统可供开播。
未注册的业务路径返回统一的 `404`。
框架对目录访问或路径穿越请求可能返回 `403`；服务不提供静态文件下载。

## N02 账号与个人房间接口

| 方法与路径 | 请求 | 成功状态与 `data` |
|---|---|---|
| `POST /api/v1/auth/register` | `{username,nickname,password}` | `201`，`{user: User, room: Room}`；注册不自动登录 |
| `POST /api/v1/auth/login` | `{username,password}` | `200`，`{user: User, access_token, token_type:"Bearer", expires_at}` |
| `POST /api/v1/auth/logout` | 已登录，无请求体 | `200`，`{}`；撤销当前令牌 |
| `GET /api/v1/me` | 已登录 | `200`，`User` |
| `GET /api/v1/me/room` | 已登录 | `200`，`Room`，附状态 `meta` |
| `PATCH /api/v1/me/room` | 已登录，`{title?,category?}` 至少一个字段 | `200`，更新后的 `Room`，附状态 `meta` |

一个用户只拥有一个固定房间；`GET /me/room` 不因重试再创建房间。注册的用户与房间写入一次事务。
昵称、用户名暂不提供编辑接口。`PATCH` 只修改提供的字段，不能修改房间归属、会话或状态。
重复用户名返回 `409 USERNAME_EXISTS`；密码错误与用户不存在都返回同样的 `401 INVALID_CREDENTIALS`。
令牌失效或已撤销返回 `401 UNAUTHENTICATED`，客户端清理内存登录态；重复携带已撤销令牌退出也遵循此规则。

登录令牌使用 libsodium 安全随机数生成 32 字节，编码为无填充 base64url；数据库只保存 SHA-256 摘要。
有效期默认 24 小时，以响应 `expires_at` 为准；不引入刷新令牌。密码使用 libsodium Argon2id 存储哈希。
哈希工作使用有界工作线程，不能在 Drogon 网络事件循环中执行；客户端只在内存中保存令牌。
登录、注册请求分别受频率限制，超限返回 `429 RATE_LIMITED`，不泄露账号是否存在。

## N03 公开房间与播放接口

### `GET /api/v1/rooms?status=live&page=1&page_size=20`

匿名可访问。`status` 省略时默认为 `live`，首版只接受此值。返回 `200`，附状态 `meta`：

```json
{"items":[],"page":1,"page_size":20,"has_more":false}
```

`items` 为 `Room` 数组，只包含房间当前会话状态为 `LIVE` 的记录。
按 `live_sessions.started_at DESC, rooms.id DESC` 排序，ID 使用数据库数值排序。
`has_more` 说明本次查询后是否还有记录；不返回未经定义的在线人数或总容量。
分页期间房间上下播可能改变结果，首版不承诺跨请求快照，客户端按 `room_id` 识别房间。
SRS 不可用时允许返回最后确认的 `LIVE` 记录，但 `meta` 必须为 `UNAVAILABLE`；大厅必须明确显示状态未知。

### `GET /api/v1/rooms/{room_id}`

匿名可访问。存在时返回 `200` 和 `Room`，附状态 `meta`；不存在返回 `404 ROOM_NOT_FOUND`。
房间不是一次性直播会话，停播不会删除房间。查询到 `RECONNECTING` 不能当成直播已结束。

### `GET /api/v1/rooms/{room_id}/playback`

匿名可访问。每次点击卡片重新请求，不复用列表缓存的媒体地址。返回 `200`，附状态 `meta`：

```json
{
  "room_id":"2001",
  "live_session_id":"1a4f15823c8c41ef8560a9d550b33167",
  "status":"LIVE",
  "protocol":"http-flv",
  "playback_url":"http://media.example:8080/live/s_1a4f15823c8c41ef8560a9d550b33167.flv"
}
```

这里只返回后端确认仍可观看的当前 `LIVE` 会话。房间未直播或重连中返回 `409 ROOM_NOT_LIVE`；
SRS 不可用或后端恢复对账尚未完成返回 `503 MEDIA_SERVICE_UNAVAILABLE`，不得发放旧播放地址。
媒体 URL 使用配置的客户端可达媒体入口；`media.example` 为示例，不代表本机部署地址。
首版公开直播不在播放 URL 附登录或推流令牌。

## N03 主播直播会话接口

### `POST /api/v1/me/room/live-sessions`

已登录房间所有者提交：

```json
{"client_request_id":"20c76c80-a012-4a41-90e7-dddc92a26cf1"}
```

成功创建返回 `201`，附状态 `meta`：

```json
{
  "room_id":"2001",
  "live_session_id":"1a4f15823c8c41ef8560a9d550b33167",
  "client_request_id":"20c76c80-a012-4a41-90e7-dddc92a26cf1",
  "status":"PREPARING",
  "publish_url":"rtmp://media.example:1935/live/s_1a4f15823c8c41ef8560a9d550b33167?token=<publish-token>",
  "prepare_deadline":"2026-10-28T04:02:00Z"
}
```

`publish_url` 中 `<publish-token>` 只是文档占位符，真实值由后端安全随机生成。
每场直播使用唯一 `s_<32位小写十六进制会话ID>` 流名称，不能以账号或固定 `livestream` 共享流。
推流凭证同样使用 32 随机字节、无填充 base64url 和 SHA-256 摘要，**只在创建成功响应中发放一次**。
首版仅保存摘要，查询和重试不能恢复明文凭证。

后端事务锁定 `rooms` 对应行，检查当前有效会话、插入新会话并设置 `current_session_id` 后一次提交。
同一房间存在 `PREPARING/LIVE/RECONNECTING/STOPPING` 时，不创建第二场直播，返回
`409 LIVE_SESSION_EXISTS`，`data` 为：

```json
{
  "room_id":"2001",
  "live_session_id":"1a4f15823c8c41ef8560a9d550b33167",
  "client_request_id":"20c76c80-a012-4a41-90e7-dddc92a26cf1",
  "status":"PREPARING"
}
```

同一个 `client_request_id` 已对应终态会话时返回 `409 LIVE_REQUEST_ALREADY_USED` 和上述对象，
不重启旧会话；新的一次用户开播操作必须生成新请求 ID。房间内该字段唯一。
客户端用冲突响应区分自己的响应丢失与其他客户端的直播；`409` 不自动触发下播。
只有用户明确取消，或能确认是本次尚未启动媒体的准备会话时，才取消后重新申请。
开播申请受频率限制；SRS 不可用或恢复对账未完成时返回 `503 MEDIA_SERVICE_UNAVAILABLE`。

### `GET /api/v1/live-sessions/{session_id}`

仅会话所属房间的所有者可访问。成功返回 `200`，附状态 `meta`：

```json
{
  "room_id":"2001",
  "live_session_id":"1a4f15823c8c41ef8560a9d550b33167",
  "client_request_id":"20c76c80-a012-4a41-90e7-dddc92a26cf1",
  "status":"PREPARING",
  "allow_publish":true,
  "created_at":"2026-10-28T04:00:00Z",
  "prepare_deadline":"2026-10-28T04:02:00Z",
  "started_at":null,
  "ended_at":null,
  "reconnect_deadline":null,
  "end_reason":null
}
```

`end_reason` 为固定机器码或 `null`，首版取值为 `OWNER_STOPPED`、`PREPARE_TIMEOUT`、
`RECONNECT_TIMEOUT`、`PUBLISH_FAILED`；不直接暴露内部异常或凭证。
会话不存在返回 `404 LIVE_SESSION_NOT_FOUND`，属于他人返回 `403 FORBIDDEN`。
这里不再次返回 `publish_url`，也不把旧会话 ID 当成房间当前会话。

### `POST /api/v1/live-sessions/{session_id}/stop`

仅会话所有者，无请求体。撤销 `allow_publish` 并进入 `STOPPING`，成功返回 `200` 和上述会话对象，附状态 `meta`。
媒体已确认消失时可直接返回 `ENDED`。对 `STOPPING/ENDED/FAILED` 的重复请求返回当前状态，不重复创建结束记录。
先撤销再次发布权限，再请求 SRS 关闭**当前已记录的发布连接**；结束确认后按当前会话 ID 条件清理房间关联。
后端不可用不阻止客户端停止本地采集/推流；SRS 不可用时可以接受下播意图并保持 `STOPPING`，等待恢复确认。
停止旧会话不能停止房间后来创建的新会话。

## 会话状态与确认口径

| 状态 | 接入及大厅行为 | 主要后继 |
|---|---|---|
| `PREPARING` | 等待合法推流与媒体进展，不入大厅 | `LIVE`、`STOPPING`、`FAILED` |
| `LIVE` | 当前连接匹配且有媒体接收进展，入大厅 | `RECONNECTING`、`STOPPING` |
| `RECONNECTING` | 宽限期内允许同会话重连，不供新观众进入 | `LIVE`、`STOPPING`、`ENDED` |
| `STOPPING` | 已撤销推流权限并撤下大厅，等待媒体消失 | `ENDED` |
| `ENDED` | 终态，凭证失效 | 无 |
| `FAILED` | 终态，准备超时或开播失败，凭证失效 | 无 |

首次接入等待默认 120 秒、重连宽限默认 20 秒、无媒体进展超时默认 15 秒，默认每 3 秒与 SRS 对账。
计时参数以服务配置为准，不能根据一次客户端超时自行宣告直播结束。
`on_publish` 通过仅代表允许接入；SRS 流、发布连接标识和接收计数匹配并出现进展后才进入 `LIVE`。
媒体进展按核实的 SRS 8.0.58 流字段 `audio_frames + video_frames` 的正增量判断，
支持纯音频和纯视频，不要求两条轨道同时存在；连接必须匹配，单纯接收字节增加不足以确认媒体就绪。
具体字段及采样条件见 [SRS 版本与计数基线](platform-srs-baseline.md)。
API 查询失败只更新 SRS 可用性，不推进“无媒体进展”判断，不把全部直播改成结束。

每次新发布连接都记录 SRS `server_id`、进程实例 `service_id`、流和客户端标识，并推进连接代次。
回调和异步对账结果更新必须带会话及当前连接/代次条件；旧停播通知、旧查询不得覆盖新连接。
非法凭证只拒绝该连接，不把被冒用流的合法会话改成 `FAILED`。
后端重启先恢复非终态记录并对账，在完成前暂停发放新播放地址与开播结果。

## N03 SRS 回调协议

`POST /internal/srs/hooks?key=<hook-secret>` 独立于 `/api/v1`，只接受配置中启用的 `on_publish`、`on_unpublish`。
SRS 必须能从自身网络环境访问该地址。回调入口先检查直接连接来源地址白名单和回调密钥，
不信任客户端提供的 `X-Forwarded-For`。密钥通过环境变量配置；回调 URL、查询串和消息体不得写入普通访问日志。

回调请求使用 SRS 原生 JSON，包括 `action`、`server_id`、`service_id`、`client_id`、`vhost`、`app`、`stream`、`param` 等版本适配字段。
`stream_id` 在该版本的回调中可能省略，不能将它当成全部事件的必填字段；需要通过流查询补齐。
`param` 中的 `token` 必须与当前有效会话摘要匹配，同时校验流名称、有效期和 `allow_publish`。
`on_unpublish` 只作用于当前匹配连接；重复事件幂等，未知或迟到的停播事件安全忽略。
实际安装的 SRS 版本、字段与接收计数判定在 [SRS 版本与计数基线](platform-srs-baseline.md) 中记录，
部署步骤见 [部署说明](platform-deployment.md)。N01 核对时没有正在运行的 SRS，源码核对不等于接口运行验收。

**此路径不使用普通业务包装。** 按 SRS 协议以 HTTP `200` 和原生 JSON 响应：

```json
{"code":0}
```

`code:0` 表示接受/成功处理，非零（首版固定 `1`）表示拒绝。凭证非法、来源不可信、缺少所需字段或内部处理失败时拒绝发布。
处理器已经注册。共享密钥未配置、来源不可信或数据库不可用时拒绝发布；没有“总是成功”的占位回调。SRS 配置模板仍须按部署文档替换密钥并安排应用。

## HTTP 状态与机器错误码

| HTTP | `code` | 含义与客户端处理 |
|---|---|---|
| `400` | `INVALID_ARGUMENT` | 参数、JSON 或字段格式错误，修正请求 |
| `401` | `UNAUTHENTICATED` | 缺失/过期/已撤销令牌，重新登录 |
| `401` | `INVALID_CREDENTIALS` | 登录失败，不区分账号不存在和密码错误 |
| `403` | `FORBIDDEN` | 已登录但无资源操作权限 |
| `404` | `NOT_FOUND` | 未注册的 HTTP 路径 |
| `404` | `ROOM_NOT_FOUND`、`LIVE_SESSION_NOT_FOUND` | 对应业务资源不存在 |
| `405` | `METHOD_NOT_ALLOWED` | 已注册路径不支持该方法 |
| `409` | `USERNAME_EXISTS` | 用户名冲突 |
| `409` | `CONFLICT` | 框架基础冲突响应，业务接口优先使用下方具体错误 |
| `409` | `LIVE_SESSION_EXISTS` | 房间已有有效直播；检查响应会话，不能盲目下播 |
| `409` | `LIVE_REQUEST_ALREADY_USED` | 开播请求 ID 已用于终态会话，新操作需要新 ID |
| `409` | `ROOM_NOT_LIVE` | 当前不能观看，返回房间/大厅状态 |
| `413` | `PAYLOAD_TOO_LARGE` | 请求体超过服务配置上限 |
| `415` | `UNSUPPORTED_MEDIA_TYPE` | 请求体媒体类型不支持 |
| `429` | `RATE_LIMITED` | 频率超限，遵守 `Retry-After` |
| `500` | `INTERNAL_ERROR` | 固定通用错误，不暴露内部原因 |
| `501` | `NOT_IMPLEMENTED` | 保留码；未来业务接口尚未注册时仍按实际 `404` 返回 |
| `503` | `DATABASE_UNAVAILABLE` | 数据库不可用，允许用户稍后重试 |
| `503` | `AUTHENTICATION_BUSY` | 密码哈希队列已满或正在关闭，稍后重试 |
| `503` | `MEDIA_SERVICE_UNAVAILABLE` | SRS 未确认或不可用；暂停开播/新播放信息 |
| `503` | `SERVICE_UNAVAILABLE` | 框架基础服务不可用响应 |
| 其他 `4xx` | `INVALID_REQUEST` | 进入应用错误处理器的其他请求错误 |

业务代码使用上述对应状态及机器码；依赖不可用和参数错误不会回显内部异常。
对所有失败都检查 HTTP 状态，有业务 JSON 时进一步检查 `code`，不得把 HTTP `200` 中的任意 JSON 当成业务成功。
SRS 回调按上一节的独立协议处理。

## 实施与验收边界

N02 实现账号、认证与个人房间；N03 实现直播状态机、SRS 鉴权、媒体进展对账和公开查询。
N04/N05 已接入大厅、观看和开播客户端，同时保留原媒体直连功能；客户端运行验收尚未执行。
N06 补齐恢复与部署收口：故障恢复后的旧 `LIVE` 必须重新观察到媒体计数增量后才恢复可用性，
客户端按原请求 UUID 和确切会话核对未完成操作，详见 [恢复行为](platform-recovery.md)。
N01—N03 本轮未运行测试、未导入 SQL，未进行注册、登录、回调、开播或多客户端运行验收。
运行验收仅在用户明确要求后执行。
