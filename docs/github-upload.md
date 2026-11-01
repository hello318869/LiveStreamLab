# GitHub 上传与日常提交

在项目根目录操作，先核对仓库和登录账号。下列命令是操作参考；上传不需要重建 `.git` 或丢弃现有提交。

## 上传前检查

```powershell
git status --short
git remote -v
git branch -vv
gh auth status
git count-objects -vH
```

仓库已有 `.gitignore`，忽略 `build/`、`out/`、`external/qt/` 和 `output/` 产物。提交前排除 SDK 二进制、日志、媒体样本、令牌和密码。

`.gitignore` 使用 `/`，对已跟踪文件不生效。若构建目录已被跟踪，可先检查 `git ls-files build`，再用 `git rm -r --cached -- build/` 取消跟踪，保留本地文件；这不会删除历史中的大文件。历史清理应单独处理，推送前不要直接删除 `.git`。

## 首次关联远程

需要登录时在本机执行 `gh auth login`，然后 `gh auth setup-git`。从 GitHub 仓库页面复制准确地址，替换下方占位符：

```powershell
# 尚无 origin 时添加；已有 origin 时使用 git remote set-url origin <地址>
git remote add origin https://github.com/OWNER/REPOSITORY.git
git push -u origin HEAD
```

远程尚未创建时，也可选择创建私有仓库并推送已有本地提交：

```powershell
gh repo create LiveStreamLab --private --source=. --remote=origin --push
```

若远程已有提交，先检查两边历史，再拉取合并；不要用强制推送覆盖未知内容。

## 日常修改

```powershell
git status --short
git diff
git add docs/capture.md
git commit -m "docs: 更新采集说明"
git push
```

只暂存本次需要提交的文件。多人协作可先 `git switch -c feature/功能名`，推送后使用 `gh pr create --fill --web` 创建 PR。

## 常见问题

| 现象 | 处理方向 |
|---|---|
| Repository not found / 404 | 核对远程地址、登录账号和仓库权限 |
| Permission denied / 无法读取用户名 | 检查 `gh auth status`，必要时重新登录并执行 `gh auth setup-git` |
| `SEC_E_NO_CREDENTIALS` | 曾在本机遇到 schannel 凭据问题；可在仓库内设置 `git config http.sslBackend openssl` 后重试 |
| 连接超时 | 检查网络与现有代理设置，保持证书验证开启 |
| 大文件拒收 / 推送体积异常 | 检查已跟踪产物及提交历史；停止重复推送，先整理大文件 |
| 中文路径显示为转义字符 | `git config core.quotepath false` |

构建和运行步骤见 [项目 README](../README.md)。
