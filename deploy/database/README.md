# N01—N03 数据库结构

`001_initial_schema.sql` 面向 Oracle MySQL **8.0.16 及以上**；依赖 InnoDB、实际执行的 CHECK 约束和降序索引。迁移不会创建数据库、用户、授权或示例账号，后端也不会自动执行迁移。本轮未执行 SQL。

按顺序应用 `001_initial_schema.sql`、`002_publish_connection.sql`。已经应用版本 1 的数据库只追加版本 2，不重跑版本 1。版本 2 新增 `publish_connected` 和 `publish_accepted_at`，区分已鉴权的发布连接与断开后等待重连的记录，并记录接入时间；状态对账确认后才开放新开播。N02/N03 的运行需要两次迁移都完成，否则相关业务接口返回数据库不可用。

由部署者选择一个独立的空数据库后，按版本顺序应用 SQL。文件不使用 `IF NOT EXISTS`，重复执行会明确失败，避免用“跳过已存在表”掩盖结构差异。MySQL DDL 隐式提交，失败可能留下部分表；应先检查已有对象和迁移记录，再修复或在另一个空库重新部署，不能当作事务回滚或直接重跑。`schema_migrations` 只在全部 DDL 完成后登记版本 1。后续修改新增 SQL 版本，不改写已经部署的文件。

## 字段约定

- `user_id`、`room_id` 在库中是 `BIGINT UNSIGNED`，HTTP JSON 中使用无前导零的正十进制字符串。直播会话和登录会话主键使用随机生成的 32 位小写十六进制；`client_request_id` 使用标准小写 UUID；`stream_name` 使用 `s_` 加本场直播会话 ID。格式由后续 Repository 入口校验，定长列不替代格式校验。
- 用户名为 3–32 个小写 ASCII 字母、数字或下划线，包含大写的输入直接拒绝，不自动改写；昵称 1–32 个 Unicode 标量值，标题 1–120 个 Unicode 标量值，禁止全空白输入及控制字符。分类固定为 `general/gaming/music/education/technology`。应用层先验证 UTF-8，再检查 Unicode 长度、空白和控制字符，SQL 的 `TRIM` 只提供基础空格检查。
- `password_hash` 保存 libsodium Argon2id 编码哈希。`auth_sessions.token_hash`、`live_sessions.publish_token_hash` 保存 32 字节令牌摘要；不保存原始令牌或带凭证的推流 URL。后续凭证生成使用 libsodium 安全随机数，不以 ID 充当凭证。
- 所有 `DATETIME(6)` 都表示 UTC；应用写入时显式绑定 UTC 值，每个数据库连接也应配置 UTC。不要依赖数据库主机本地时区或会话默认时区。
- `prepare_deadline/reconnect_deadline/end_reason` 与 HTTP 字段同名。准备截止时间由 `lifecycle.prepare_seconds` 计算；终止原因仅允许 API 规定的 `OWNER_STOPPED/PREPARE_TIMEOUT/RECONNECT_TIMEOUT/PUBLISH_FAILED` 或 `NULL`，不保存原始异常信息。
- SRS 的 `server_id/service_id/stream_id/client_id` 归一化为字符串；`service_id` 识别同一服务器重启后的服务实例。发布回调先保存服务器、实例和客户端标识，流标识允许等待 HTTP API 查询补齐；确认 `LIVE` 时四者必须完整。`publish_generation` 在接受新发布连接时递增，`revision` 用于异步结果的条件更新。
- `srs_recv_bytes/srs_audio_frames/srs_video_frames` 保存同一已确认连接的累计观测基线，分别对应核实版本的流 API `recv_bytes/audio_frames/video_frames`。N03 依据音频或视频帧计数的增量确认媒体进展；不要求两条轨同时增长，不能把接收字节或 TCP 连接存在单独当成音视频就绪。换服务器实例、换发布连接、换代次或计数回退必须清空/重建基线，不能跨连接计算增量。详细 SRS 版本、源码依据与判定口径见平台部署文档。

## 后续事务与并发约定

注册在一个事务中创建用户及其唯一房间。开播申请在同一事务内先 `SELECT ... FROM rooms WHERE owner_id = ? FOR UPDATE`，再检查 `(room_id, client_request_id)` 的既有请求及 `current_session_id`，最后插入 `PREPARING` 会话并更新房间指针。存在有效会话时返回 `409` 及其标识，不另建会话、不撤销正在使用的直播。房间内请求 ID 的唯一索引覆盖终态历史，重复旧请求也不得创建新会话。

`active_room_id` 是生成列，唯一索引使每个房间最多拥有一条 `PREPARING/LIVE/RECONNECTING/STOPPING` 记录。它是并发兜底，不能替代房间行锁、重复请求识别和明确的业务错误映射。状态结束前仍是 `STOPPING`，因此新开播须等待旧会话确认结束。

主动下播要在一次更新中设 `STOPPING`、`allow_publish=0`、`publish_token_hash=NULL`；终态必须同时写入 `ended_at`。状态转移、连接标识、发布代次和版本必须放进同一个条件更新；旧回调与旧查询结果不能覆盖新连接。非终态由数据库约束禁止写入 `ended_at`，进入或退出 `RECONNECTING` 时分别设置或清空 `reconnect_deadline`。

结束事务按旧会话 ID 条件清理房间指针，例如 `UPDATE rooms SET current_session_id=NULL, updated_at=? WHERE id=? AND current_session_id=?`，并将会话置为终态。房间指针通过复合外键只能指向本房间的直播会话。所有外键默认限制删除；历史清理、账号删除和数据保留策略留到有明确需求时再实现，不自动级联删除历史。

大厅通过 `rooms.current_session_id` 连接 `live_sessions`，按 `live_sessions.started_at DESC, rooms.id DESC` 排序，房间 ID 使用数值顺序。`ix_live_sessions_lobby` 中的 `room_id` 与连接后的 `rooms.id` 相等。SRS 可用时展示已确认的 `LIVE`；SRS 不可用时可返回最后确认的 `LIVE` 记录，但必须通过响应 `meta` 标记 `UNAVAILABLE`，不把缓存当成刚确认的在线结果，且不能发放新的播放地址。没有 `rooms.is_live` 字段；后端失联不能把会话批量结束。N02/N03 的账号、直播状态与 SRS 服务代码已实现，迁移仍需在实际部署时另行应用。

约束依据：[MySQL CHECK 文档](https://dev.mysql.com/doc/refman/8.0/en/create-table-check-constraints.html)、[MySQL 外键文档](https://dev.mysql.com/doc/refman/8.0/en/create-table-foreign-keys.html)。本轮未应用 SQL、未运行测试，不能据此认定迁移已经通过数据库执行验收。
