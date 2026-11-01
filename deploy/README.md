# 单服务器原生部署交付

本目录交付部署模板，不自动安装软件、迁移数据库、填入密钥或启动服务。
完整步骤见 [构建与部署](../docs/platform-deployment.md)，异常处理见 [恢复行为](../docs/platform-recovery.md)。

| 文件 | 用途 |
|---|---|
| `server.lan.example.json` | Linux 同机 MySQL/SRS、对外提供 API 的配置示例；`192.0.2.10` 是保留示例地址，须替换为客户端实际可达地址 |
| `control-server.env.example` | 后端环境变量名；复制后的本机文件由 root 保管，不能提交 |
| `srs-platform.conf` | SRS 8.0.58 平台专用配置；两处回调密钥占位值必须替换为与后端相同的 43 字符密钥 |
| `srs-http-hooks-redaction.patch` | 固定 SRS 提交的日志脱敏补丁；必须落实到实际部署的 SRS 二进制 |
| `systemd/livestreamlab-control.service` | 原生后端前台进程、失败重启和有序停止模板 |
| `systemd/livestreamlab-srs.service` | 使用已打补丁的固定版 SRS；运行文件放 `/var/lib/livestreamlab-srs` |
| `database/` | 仅供人工按顺序执行的版本 1、2 迁移；升级不重跑旧版本 |

默认安装布局为 `/opt/livestreamlab/bin/control_server`、
`/opt/livestreamlab/srs/trunk/objs/srs` 和 `/etc/livestreamlab/` 中的本机配置。
systemd 模板不会自动启动 MySQL、执行迁移或确认媒体已就绪；启动排序不能替代后端对账门禁。
后端的 CMake 安装会把本目录列明的模板复制到 `share/livestreamlab/deploy/`，
不会复制真实 `.env` 或本机配置，也不会自动启用服务。

保留现有 SRS 配置和服务。部署者选择好切换窗口后再安装、启用本目录的独立配置；
同一端口只能有一个进程监听。此处 `docker-compose.yml` 仍为空，未交付容器部署路线。

N06 不包含 N07 的实际部署和运行验收。本轮未安装这些 unit、未应用 SRS 补丁、
未执行 SQL、未启动/重启服务或运行测试。
