# 网络控制服务与客户端：N01—N06 构建与部署

`control_server` 已提供 N01 基础、N02 账号/个人房间和 N03 直播会话/SRS 集成。服务面向 Linux/WSL；Windows 客户端已按 N04/N05 接入登录、大厅和平台开播页面。项目顶层 CMake 也可选择构建后端。客户端流程和 `platform.json` 配置见 [平台客户端](platform-client.md)。

## 目录与依赖边界

后端入口为 `src/apps/control_server_main.cpp`，可直接使用 `src/control_server/` 的 CMake，也可从项目顶层添加其子目录；后端不链接 `lsl_common`、Qt、FFmpeg 或 SDL3。

代码遵循 Controller → Service → Repository/SrsClient：控制器解析 HTTP，认证过滤器验证 Bearer，业务服务处理凭证与状态，Repository 使用同一个 Drogon 异步 MySQL 连接池，SrsClient 异步访问已固定版本的 SRS。密码哈希由 2 个工作线程执行，待处理队列最多 16 项；队列满返回 `503 AUTHENTICATION_BUSY`。

构建基线固定如下，不跟随主分支；系统安全补丁对应的发行包版本也记录在这里：

| 依赖 | 固定基线 |
|---|---|
| 语言与工具 | C++17，CMake ≥3.24；本机 GNU 13.3.0 / CMake 3.28.3 / Ninja 1.11.1 |
| Drogon | 1.9.11，提交 `a22956b82b6b221ceeff83913c3014ce0d048555` |
| Trantor | 上述 Drogon 的子模块提交 `43fd79b2dbac59608a819ebba167e8fe2c079d90` |
| JsonCpp | 1.9.5；Ubuntu 包 `1.9.5-6build1` |
| libsodium | 1.0.18；Ubuntu 安全更新包 `1.0.18-1ubuntu0.24.04.1`，不使用未经补丁的原始旧源码 |
| MariaDB Connector/C | pkg-config 版本 3.3.17；Ubuntu `libmariadb-dev/libmariadb3` 包 `1:10.11.14-0ubuntu0.24.04.1` |
| 传递系统依赖 | 本机 OpenSSL 包 `3.0.13-0ubuntu3.16`、UUID 包 `2.39.3-9ubuntu6.6`、zlib 包 `1:1.3.dfsg-3.1ubuntu2.3` |
| 数据库服务 | SQL 要求 Oracle MySQL ≥8.0.16；本轮未连接数据库，未声称已核实服务端版本 |
| SRS 适配目标 | 现有 8.0.58 / `89bdba34ba55175008a1f7749f11d842a90ca6b7`，详见 [版本与字段基线](platform-srs-baseline.md) |

MariaDB Connector/C 是 Drogon 异步 MySQL 驱动所用的客户端库，数据库服务仍按 MySQL 设计。现有 Oracle `libmysqlclient-dev` 不替代该依赖。代码通过 `drogon/config.h` 的 `USE_MYSQL` 强制检查 Drogon 已编入 MySQL 支持。

CMake 要求上述三个 pkg-config 版本，并默认查找精确版本 Drogon 1.9.11；`LSL_SERVER_FETCH_DROGON=ON` 则由 CMake 获取固定提交及其 Trantor 子模块。获取方式可通过 CMake 的 `FETCHCONTENT_SOURCE_DIR_DROGON` 指向已有完整源码以离线构建；它必须是该提交且含对应子模块。升级依赖时应明确更新版本约束和此表。

参考：[Drogon 固定版本构建定义](https://github.com/drogonframework/drogon/blob/a22956b82b6b221ceeff83913c3014ce0d048555/CMakeLists.txt)、[libsodium 安装与发行说明](https://doc.libsodium.org/installation)。

## 标准 CMake 构建

以下命令在 Linux/WSL 中、以 `LiveStreamLab` 为工作目录执行，前提是上述开发包已可被 pkg-config/CMake 找到。

使用已安装的 Drogon 1.9.11：

```bash
cmake -S src/control_server -B build/server -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build/server --target control_server --parallel 4
```

由 CMake 获取 Drogon 的固定源码：

```bash
cmake -S src/control_server -B build/server -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
  -DLSL_SERVER_FETCH_DROGON=ON
cmake --build build/server --target control_server --parallel 4
```

第三方示例、drogon_ctl、数据库生成器、测试、PostgreSQL、SQLite、Redis、YAML 和 Brotli 扩展不纳入此构建。输出为 `build/server/bin/control_server`。可选安装由 `cmake --install build/server --prefix <安装目录>` 完成；部署机器仍需匹配的动态运行库。

从项目顶层只构建后端时使用以下选项；依赖获取与 `CMAKE_PREFIX_PATH` 等参数仍按上文和下文配置：

```bash
cmake -S . -B build/platform -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
  -DLSL_BUILD_CONTROL_SERVER=ON -DLSL_BUILD_MEDIA=OFF \
  -DLSL_SERVER_FETCH_DROGON=ON
cmake --build build/platform --target control_server --parallel 4
```

Windows 客户端的默认构建保持原有配置；Linux 若同时具备媒体依赖，也可保持 `LSL_BUILD_MEDIA=ON` 编译相应目标。

没有新增 `.sh/.ps1/.bat` 构建脚本。下面的命令是部署说明，本轮没有执行安装、SQL 或服务操作。

## 本次使用的项目内隔离依赖

由于本机原先缺少部分开发包，本次用 Ubuntu 包元数据指定精确版本，将以下 `.deb` 下载并解压至 `build/server-deps/`，没有执行系统安装：

- `libjsoncpp-dev/libjsoncpp25=1.9.5-6build1`
- `libsodium-dev/libsodium23=1.0.18-1ubuntu0.24.04.1`
- `libmariadb-dev/libmariadb3=1:10.11.14-0ubuntu0.24.04.1`

Drogon 及 Trantor 源码同样位于该目录，`build/` 由现有 Git 忽略规则排除。解压后的 `.pc` 文件中 `prefix` 已改为其项目内实际 `root/usr` 路径，不把机器路径写进源代码或 CMake。项目移动后需要重新定位这些生成文件，或采用标准安装的开发包。

使用本次已有隔离依赖重新配置：

```bash
export PKG_CONFIG_PATH="$PWD/build/server-deps/root/usr/lib/x86_64-linux-gnu/pkgconfig"
cmake -S src/control_server -B build/server -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
  -DLSL_SERVER_FETCH_DROGON=ON \
  -DFETCHCONTENT_SOURCE_DIR_DROGON="$PWD/build/server-deps/drogon" \
  -DCMAKE_PREFIX_PATH="$PWD/build/server-deps/root/usr" \
  -DMYSQL_INCLUDE_DIRS="$PWD/build/server-deps/root/usr/include/mariadb" \
  -DMYSQL_LIBRARIES="$PWD/build/server-deps/root/usr/lib/x86_64-linux-gnu/libmariadb.so"
cmake --build build/server --target control_server --parallel 4
```

这些参数仅用于本机隔离依赖布局；不需要重新安装数据库或替换已有 MySQL/SRS。

## 配置与启动

`config/server.example.json` 默认只监听 `127.0.0.1:8090`，数据库连接关闭。复制为已忽略的 `config/server.local.json`，按实际环境启用数据库并填写已迁移的库及业务账号。数据库关闭时健康接口可用，业务接口按数据库不可用返回；不能把示例默认开关当作已部署的账号系统。启动必须显式指定配置路径，相对路径按启动工作目录解析：

```bash
build/server/bin/control_server --config config/server.local.json
```

本轮只构建，不执行以上启动命令。运行后 `GET /health` 仅表示进程存活；它不访问 SRS 或数据库，不代表平台已可开播。默认 SIGTERM 交由 Drogon 退出事件循环。

配置要求严格 JSON、最大 64 KiB；拒绝重复键、未知字段、缺字段、类型错误、非法监听 IP、端口和超范围计时值。配置错误仅输出固定字段名，不输出原始 JSON 或解析器摘录。启动参数错误或配置失败退出码 `2`，运行初始化失败为 `1`。

| 配置段 | 用途与约束 |
|---|---|
| `http` | 数字监听 IP、端口、1–64 个网络线程、请求体上限 1024–1048576 字节；默认 16384 |
| `database` | 开关、主机、端口、库、用户、环境变量名、连接数与查询超时；启用后创建 Drogon 异步连接池，不自动建表 |
| `addresses` | 业务公开入口、RTMP/播放公开入口、SRS 内部管理入口、应用名和 `srs_vhost`（默认 `__defaultVhost__`）；入口只含 scheme/host/可选端口，不带末尾 `/`、路径、账号、凭证、查询或 fragment |
| `srs` | 精确目标版本、回调密钥环境变量名、可信直接连接 IP 列表、请求超时和对账间隔；用于异步查询、回调鉴权和状态确认 |
| `lifecycle` | 首次接入 120 秒、重连宽限 20 秒、无媒体进展 15 秒、登录有效期 86400 秒；用于服务端时限判断 |

`database.enabled=true` 时进程从 `database.password_env` 指定的环境变量读取密码，未提供非空值则拒绝启动。示例仅保存变量名 `LSL_DB_PASSWORD`，不保存真实密码。部署使用独立业务账号而非把管理员密码写入示例、源码、命令或日志。

进程从 `LSL_SRS_HOOK_SECRET` 读取共享回调密钥，格式为随机 32 字节的无填充 base64url（43 字符），同一个值配置到 SRS 的两个回调 URL。未提供有效密钥时拒绝回调并保持媒体服务不可用，不发放开播或播放地址；账号接口仍可使用。IP 白名单只接受字面 IP，匹配直接连接来源并归一化 IPv4/IPv6，不信任代理头。

监听地址与公开地址是两类配置。让其他电脑访问时，业务监听可按需要改为 `0.0.0.0`，三个公开入口必须填写客户端实际可达的服务器地址；`0.0.0.0` 是监听地址，不能作为客户端入口。WSL、Windows、Docker 和另一台电脑的 `127.0.0.1` 各指向自身，示例回环地址只适合本机。公网登录入口需要 HTTPS。

`srs_vhost` 必须与 SRS 实际解析的 vhost 名一致。使用自定义 vhost 时，配置校验要求公开 RTMP/HTTP 地址的主机名都等于该 vhost，不接受会路由到默认 vhost 的 IP 地址组合；后端不会自动附加 vhost 覆盖参数。默认模板使用 `__defaultVhost__`。

## 数据库与 SRS 的实施边界

建表交付位于 [deploy/database](../deploy/database/README.md)，按版本 1、2 顺序迁移。版本 1 包含 `users/auth_sessions/rooms/live_sessions`，版本 2 增加发布连接接入标记。用户、房间和会话标识分开；唯一约束和房间行锁共同防止并发开播；当前会话采用同房间复合外键；停止/终态撤销推流摘要。所有写入时间用 UTC，SQL 不自动应用，本次没有连接或改动已有数据库。

[srs-platform.conf](../deploy/srs-platform.conf) 面向实际 SRS 8.0.58，没有覆盖 WSL 原配置。管理 API 监听本机 `127.0.0.1:1985`，媒体端口为 1935/8080，已配置发布/停播回调模板。部署前须替换两处共享密钥占位符，并核对从 SRS 到后端的可达地址；未替换模板拒绝发布。该模板已交付但未应用，不表示当前 SRS 已启用平台鉴权。

后端不启用普通访问日志；第三方诊断仅保留固定提示与源码位置，丢弃可能包含 SQL 绑定值、URL 或请求体的原始文案。另交付 [SRS 定向脱敏补丁](../deploy/srs-http-hooks-redaction.patch)，针对固定提交中的回调和 RTMP 日志隐藏 URL、凭证参数及正文，保留事件、连接标识、HTTP 状态与错误码，不关闭日志。实际部署前应在相同 SRS 源码版本审阅应用该补丁，并另行完成构建与日志验证；本轮未应用补丁或修改现有 SRS。

接口、状态、错误和分页的完整约定见 [platform-api.md](platform-api.md)。N02/N03 路由与回调均已注册，未注册路径仍按框架返回错误；回调校验失败使用 SRS 原生非零 `code`。静态文件扩展名白名单清空，并关闭隐式首页；不启用 Cookie 会话，应用 JSON 响应使用 `Cache-Control: no-store`。

统一 JSON 错误适用于应用处理器、异常边界和框架可定制的错误路径。Drogon HTTP 解析器在请求格式错误、请求体超限等情况下可直接发送无 JSON 体的 HTTP 错误并关闭连接，客户端必须先处理 HTTP 状态，再尝试解析可选业务错误码；不能假定所有 400/413 都有 JSON。没有改写第三方解析器。

## N06 原生单服务器部署

首版部署路线是 Linux 上各运行一个 MySQL、SRS 和 `control_server`，Windows 运行统一客户端。
[deploy/README.md](../deploy/README.md) 列出本轮新增的 systemd、环境变量及局域网配置模板。
`deploy/docker-compose.yml` 仍为空，不能作为可运行的容器方案。WSL 采用下面的同一套 Linux 进程配置，
但只有启用了 systemd 的发行版才能使用 unit；unit 不负责让关闭后的 Windows/WSL 自动启动。

### 依赖、安装目录与运行库

在 Ubuntu 24.04 中准备 `g++`、`cmake`、`ninja-build`、`pkg-config`、`git`，以及
`libssl-dev`、`uuid-dev`、`zlib1g-dev`、`libjsoncpp-dev`、`libsodium-dev`、`libmariadb-dev`。
这些是部署者之后安装的依赖，本轮没有安装系统软件。开发包必须满足前面的固定版本表；
发行版更新若改变 pkg-config 版本，需要明确更新兼容基线，不能删除 CMake 版本检查来静默通过。
MySQL 服务端与 MariaDB Connector/C 客户端库是不同依赖，已有 Windows MySQL 也不替代 Linux 客户端库。

完成上文 CMake Release 构建后，原生安装命令为：

```bash
sudo cmake --install build/server --prefix /opt/livestreamlab
```

后端位于 `/opt/livestreamlab/bin/control_server`，示例位于
`/opt/livestreamlab/share/livestreamlab/server.example.json`。
安装不会创建业务账号、覆盖 `/etc/livestreamlab` 或自动启用 unit。
N06 的部署、迁移和 systemd 示例同时安装到 `share/livestreamlab/deploy/`，
HTTP 契约、部署及恢复说明安装到 `share/livestreamlab/docs/`；只安装模板，不自动执行或填入本机配置。
部署机器需要匹配的动态运行库；项目内 `build/server-deps` 的临时 RPATH/解压目录不应成为长期运行依赖。
采用发行版运行库或单独管理的运行库目录，并在切换前检查安装产物的动态依赖；
不能只复制一个可执行文件就假定离开构建机后也能运行。

### 初次创建账号与本机配置

以下布局与交付的 unit 一致；如果机器已经存在同名业务用户或配置，保留已有项并人工合并。

```bash
sudo useradd --system --user-group --home-dir /nonexistent --no-create-home --shell /usr/sbin/nologin livestreamlab
sudo install -d -m 0750 -o root -g livestreamlab /etc/livestreamlab
sudo cp --no-clobber deploy/server.lan.example.json /etc/livestreamlab/server.json
sudo cp --no-clobber deploy/srs-platform.conf /etc/livestreamlab/srs-platform.local.conf
sudo chown root:livestreamlab /etc/livestreamlab/server.json /etc/livestreamlab/srs-platform.local.conf
sudo chmod 0640 /etc/livestreamlab/server.json /etc/livestreamlab/srs-platform.local.conf
```

`server.lan.example.json` 中的 `192.0.2.10` 是保留示例地址，部署前必须替换为实际可达的服务器地址。
该模板开启数据库、监听 `0.0.0.0:8090`；未调整地址、数据库和密钥前不应启用服务。
原有 `config/server.example.json` 继续保留仅本机监听且数据库关闭的安全默认值。

首次生成两个独立随机值可用以下命令，内容直接写入仅 root 可读的文件，不输出到终端；
`open(...,"x")` 在文件已存在时失败，防止升级时意外轮换密钥：

```bash
sudo python3 -c 'import os,secrets; os.umask(0o077); f=open("/etc/livestreamlab/control-server.env","x"); f.write("LSL_DB_PASSWORD="+secrets.token_urlsafe(32)+"\nLSL_SRS_HOOK_SECRET="+secrets.token_urlsafe(32)+"\n"); f.close()'
```

也可按 [control-server.env.example](../deploy/control-server.env.example) 人工创建文件，权限为 `root:root 0600`。
数据库密码对应下节独立业务账号；回调密钥对应 SRS 本机配置中两处 `key=`。
用本机受保护的编辑器完成填写，不把值放在命令行参数、聊天、版本库或普通日志中。
后端进程从 systemd 的 `EnvironmentFile` 读取变量；SRS 不会自行展开这个环境文件或替换配置中的占位符。

### MySQL 初始化与升级

由有建库权限的管理员在受保护终端中完成，使用交互密码提示，不把密码放在 `-p` 后面。
已有库先备份并读取 `schema_migrations`；以下是全新独立库的示意，不能直接重跑到已有平台库：

```bash
MYSQL_HISTFILE=/dev/null mysql --protocol=TCP -h 127.0.0.1 -P 3306 -u root -p
```

```sql
CREATE DATABASE livestreamlab_platform CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_ai_ci;
CREATE USER 'lsl_control'@'127.0.0.1' IDENTIFIED BY '<与本机环境文件一致的独立业务密码>';
GRANT SELECT, INSERT, UPDATE ON livestreamlab_platform.* TO 'lsl_control'@'127.0.0.1';
```

上面的尖括号内容是说明，必须在本机替换；管理员密码不保存到平台配置。
账号的 Host 要匹配 MySQL 实际看到的后端来源地址。数据库位于 Windows 或另一台主机时，
相应调整 `database.host`、账号来源限制和数据库防火墙，不使用面向任意来源的管理员账号。
运行服务只需要查询、新增和更新权限；结构迁移继续由管理员或独立迁移账号执行。

在 `LiveStreamLab` 目录中，对新建空库按顺序应用：

```bash
mysql --protocol=TCP -h 127.0.0.1 -P 3306 -u root -p --database=livestreamlab_platform < deploy/database/001_initial_schema.sql
mysql --protocol=TCP -h 127.0.0.1 -P 3306 -u root -p --database=livestreamlab_platform < deploy/database/002_publish_connection.sql
```

已经登记版本 1 的库仅执行版本 2；已经登记版本 2 的库不重跑这两份文件。
DDL 隐式提交，失败后先检查实际结构和版本记录，再处理部分执行结果。
升级不能靠清空库或 `IF NOT EXISTS` 掩盖结构差异。
后端不自动迁移数据库，详见 [数据库说明](../deploy/database/README.md)。

### 固定版 SRS 与脱敏补丁前提

SRS 必须采用固定提交 `89bdba34ba55175008a1f7749f11d842a90ca6b7` 的 8.0.58，
并让 [日志脱敏补丁](../deploy/srs-http-hooks-redaction.patch) 落实到实际部署的二进制。
部署者可以在独立源码副本中核对提交、审阅差异并应用补丁：

```bash
git -C /path/to/pinned-srs rev-parse HEAD
git -C /path/to/pinned-srs diff --stat
git -C /path/to/pinned-srs apply --check /path/to/LiveStreamLab/deploy/srs-http-hooks-redaction.patch
git -C /path/to/pinned-srs apply /path/to/LiveStreamLab/deploy/srs-http-hooks-redaction.patch
```

不要在正在使用的源码或原配置上直接覆盖操作。当前工程 CMake 只构建客户端和后端，
未提供 SRS 构建目标；这里要求另行准备由该补丁源码产生的 SRS 运行产物，
不能把应用补丁前留下的 `objs/srs` 当成已脱敏版本。本轮没有应用补丁或编译 SRS。
缺少这个运行产物时，部署前提尚未满足，不能按“只改配置即可上线”处理。

将已准备好的 SRS 树放到 `/opt/livestreamlab/srs`，实际程序为
`/opt/livestreamlab/srs/trunk/objs/srs`。复制安装树时保持原始二进制和所需运行库；
unit 使用普通用户运行，不依赖 `/home` 下的源码或符号链接。
本机配置保持 `daemon off`、`srs_log_tank console`；管理 API 仅监听 `127.0.0.1:1985`。
两处回调 URL 都直连同机 `127.0.0.1:8090/internal/srs/hooks`，使用与后端一致的密钥。
`srs.trusted_sources` 保持实际直连来源 IP；不以观众 IP、代理头或 Windows 主机地址替代同机回环来源。

[SRS unit](../deploy/systemd/livestreamlab-srs.service) 将工作目录设为 `/var/lib/livestreamlab-srs`，
由 systemd 创建状态目录，启动前创建其 `objs/nginx/html`。配置中的相对 PID/标识/静态目录
因此位于状态目录，安装树保持只读；首版只使用动态 HTTP-FLV，静态目录保持为空。
已有其他 SRS 实例占用 1935/8080/1985 时，先由部署者安排切换窗口，不并行抢占端口。

### systemd 安装、启动和升级顺序

配置和运行产物准备完成后，由部署者后续执行：

```bash
sudo cp --no-clobber deploy/systemd/livestreamlab-control.service /etc/systemd/system/livestreamlab-control.service
sudo cp --no-clobber deploy/systemd/livestreamlab-srs.service /etc/systemd/system/livestreamlab-srs.service
sudo systemctl daemon-reload
sudo systemctl start mysql.service
sudo systemctl start livestreamlab-srs.service
sudo systemctl start livestreamlab-control.service
```

`mysql.service` 是同机 Oracle MySQL 的示例服务名；使用已有 Windows/远端数据库时不执行这一行，
也不在 Linux 安装第二份数据库。unit 的 `After=` 只给同时启动的服务排序，不会自动安装、启动或证明依赖可用。
SRS 可以先启动，后端未就绪时新推流会被拒绝，随后后端通过对账开放业务；
后端也能在依赖临时不可用时保留进程和账号接口，不因 SRS 失联主动退出。

确认部署配置与运行结果后，部署者可再执行：

```bash
sudo systemctl enable livestreamlab-srs.service livestreamlab-control.service
```

服务以 `livestreamlab` 普通用户运行，读取配置，向 journal 输出已脱敏诊断。
后端 unit 通过 `StateDirectory` 创建可写的 `/var/lib/livestreamlab-control` 并作为工作目录，
容纳 Drogon 启动时创建的默认 `uploads/tmp` 树；不让 `ProtectSystem=strict` 下的进程尝试写入只读 `/opt` 安装目录。
`Restart=on-failure` 负责异常退出后的进程重启；一分钟最多启动五次，配置错误退出码 2 不循环重启。
正常停止使用 `systemctl stop`，不会触发失败重启；systemd 发送 SIGTERM，30 秒未退出才按系统策略强制结束。
这不是对媒体业务已下播的承诺，强制终止仍由后端对账和会话时限收敛。

升级时先在独立构建目录完成 CMake 构建，备份数据库和 `/etc/livestreamlab`；
需要新增迁移时先安排兼容的维护窗口。停止后端后安装新二进制，再启动后端。
不重新复制 env、本机 JSON、SRS 密钥配置，也不自动重启 SRS。unit 有变化时人工合并，再 `daemon-reload`。
仅后端升级无需强制中断仍正常运行的 SRS 媒体流；重启后的业务可用性由新的对账确认决定。

### Windows、WSL 与其他设备的地址映射

所有观众收到的是同一组后端生成的公开媒体地址，所以应选择所有目标客户端都能到达的地址。
WSL 网络模式和转发配置可参考 [Microsoft 官方说明](https://learn.microsoft.com/en-us/windows/wsl/networking)，
实际可用地址以部署设备的网络配置为准。

| 部署位置 | 客户端 `api_base` 与后端公开地址 | 后端访问 SRS / SRS 回调 |
|---|---|---|
| 独立 Linux 局域网服务器 | 服务器实际 LAN IP 或可解析域名，端口 8090/1935/8080 | 同机分别 `127.0.0.1:1985` 与 `127.0.0.1:8090` |
| Windows 仅本机访问同机 WSL | 可在确认 localhost 转发后使用 `127.0.0.1`；仅适合本机使用 | WSL 内两服务同机，仍用回环地址 |
| 其他电脑访问 Windows 上的 WSL NAT | Windows LAN IP，经已配置的 TCP 转发进入 WSL；三项公开地址都使用该 LAN IP | 两服务仍在 WSL，管理与回调不经过公网转发 |
| WSL 镜像网络 | 使用实际可达的 Windows/WSL LAN 地址，并核对 Windows、Hyper-V 和 Linux 的入站规则 | 两服务同一网络空间时保留回环地址 |

WSL NAT 的端口转发只需要 TCP 8090、1935、8080，目标为当前 WSL IP；
1985 和 3306 留在管理网络，不作为观众入口转发。端口转发和防火墙由部署者配置，
本轮没有改动 Windows 网络策略。WSL 重启可能改变 NAT IP，原转发需要重新核对；
不能将历史 WSL IP 写死后认定跨设备始终有效。客户端不能填写服务器自己的 WSL 内部名称或回环地址。

每台 Windows 客户端在可执行文件旁的 `config/platform.local.json` 设置自己的业务入口，例如：

```json
{
  "api_base": "http://192.0.2.10:8090",
  "request_timeout_ms": 8000,
  "lobby_refresh_ms": 5000,
  "room_refresh_ms": 5000
}
```

替换示例地址后重启客户端。程序优先读取本机覆盖文件，否则读取 `platform.json`；
CMake 重新部署默认配置不会覆盖 `platform.local.json`，也不把服务端本机 JSON/env 一起复制到客户端输出目录。
登录令牌和密码不写入这份配置。播放器和推流器的媒体 URL 继续由后端生成，无需逐台手填。

公网登录必须使用 HTTPS 业务入口。修改 `api_public_base` 为 HTTPS 不会给 Drogon 的 HTTP 监听器自动增加 TLS；
应单独部署受信任的 HTTPS 入口，将业务请求转到回环 HTTP，并让客户端直接使用最终入口，避免依赖重定向。
内部 SRS 回调仍直连受限入口，不能让反向代理的普通访问日志记录含共享密钥的查询参数。
首版公开直播的 HTTP-FLV/RTMP 未提供私密观看鉴权或自动 TLS 媒体部署。

## 状态恢复与容量边界

进程启动后先读取数据库非终态会话，与 SRS 核对后才开放开播与新播放信息。每轮对账不重叠，SRS 查询或数据库更新失败时标记不可用并清空连续媒体观察基线，保留原会话；恢复后重新观察媒体增量。持久化的 `LIVE` 在重启/故障恢复后也必须重新确认匹配发布连接的音频或视频计数增量，不能直接当成当前可用。首次合法 `on_publish` 只准入，只有匹配发布连接的 AAC/H.264 媒体计数出现进展才确认 `LIVE`。

账号密码哈希与网络循环分离；登录、注册和开播有限流。首版为单业务服务进程，限流和连续观测状态保存在进程内；不宣称多副本协调或分布式一致性。

SRS 快照最多读取 32 页、每页 100 条流，最多 256 个 vhost，超限或快照明显变动时按不可用处理。分页列表遗漏的已知流通过点查确认，不把分页遗漏直接当成断流。适配器限制并发、请求和整次操作超时；1 MiB 的 JSON 上限在 Drogon 接收响应后检查，不是传输层接收缓冲上限。管理 API 应保持在可信内部网络。

SRS 的删除客户端接口没有附带服务器实例的条件删除参数。适配器在删除前核对版本、实例、客户端和流，删除后仍等待回调/对账确认；远端在核对与删除之间瞬间重启的竞态无法由该接口原子消除。数据库的状态更新仍按会话、连接代次和版本检查，旧结果不能覆盖新连接。

断网、异常退出和各服务重启的处理及剩余限制见 [N06 恢复行为](platform-recovery.md)。

## 本阶段验证记录

2026-10-31：按本机配置授权，在当前 WSL 安装并启用了 `livestreamlab-srs.service` 和
`livestreamlab-control.service`。运行产物位于 `/opt/livestreamlab`，服务端 JSON 为
`/etc/livestreamlab/server.json`，SRS 使用 `/etc/livestreamlab/srs-platform.local.conf`。
密码及固定回调密钥保存在 root 所有、权限 `0600` 的
`/etc/livestreamlab/control-server.env`，同时配置独立运行库和 `MARIADB_PLUGIN_DIR`；
无需在终端手工 export。按用户要求使用现有 MySQL root 账号，已确认
`livestreamlab_platform` 的迁移版本为 1、2，未重新建库或执行旧迁移。

修正了 Drogon 1.9.11 对同一行多个带引号连接字段的解析问题：连接字段改用换行分隔。
SRS 的 systemd 模板允许枚举网卡所需的 `AF_NETLINK`，并将 `LimitNOFILE` 设置为 10000。
固定版 SRS 在独立副本应用现有日志脱敏补丁，通过本机 `build/local-srs-cmake` 的 CMake
入口仅构建服务器目标，再安装补丁后的二进制；原 `/home/asusjian/srs` 源码和配置保持原样。

本机配置使用回环地址：业务 API 8090、RTMP 1935、HTTP-FLV 8080，SRS 管理 API 1985。
服务已设置在 WSL 的 systemd 启动后自动运行；这不会主动启动关闭的 WSL。
以后在 WSL 中可使用：

```bash
sudo systemctl start livestreamlab-srs.service livestreamlab-control.service
sudo systemctl restart livestreamlab-control.service
sudo systemctl status livestreamlab-srs.service livestreamlab-control.service --no-pager
```

本次完成 CMake 构建/安装、配置、服务启动及进程/端口/数据库连接元数据检查；
未运行测试、请求业务接口或执行真实推拉流，不作为 N07 媒体闭环验收记录。

2026-10-29：N06 新增原生 Linux systemd、局域网配置和环境变量模板，整理跨设备地址、
版本化迁移、恢复与升级说明。后端已通过 WSL CMake Release 编译；
这些部署文件未安装/应用，未安装系统依赖、运行测试、启动/重启服务、执行 SQL 或访问业务/媒体接口。
Windows 客户端也已通过 CMake Release 编译、链接和依赖部署，`LSL_BUILD_TESTS=OFF`；
完整媒体闭环、systemd 运行与跨设备连通性仍未验收，N07 待明确授权。

2026-10-29：N04/N05 客户端通过 CMake Windows Release 编译、链接和依赖部署，
`LSL_BUILD_TESTS=OFF`；没有运行测试、GUI、业务接口或媒体链路，没有执行 SQL 或启动/重启服务。
后端和 SRS 的实际部署仍按本文件完成，不能将客户端默认回环地址理解为跨设备地址已配置。

2026-10-28：N01—N03 仅进行 CMake Release 配置、编译和静态复核，`BUILD_TESTING=OFF`。未启动后端，未请求健康或业务接口，未运行测试，未执行 SQL，也未推拉流或启动/重启现有服务。编译结果不替代接口行为、SQL 执行和真实媒体链路验收；这些留待用户明确授权。
