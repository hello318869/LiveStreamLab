# 项目上传 GitHub 教程

> 适用环境：Windows + Git 2.5x + PowerShell；示例项目为本仓库（C++/CMake，含 `external/` 预置依赖）。
> 文中的数字是 2026-10-24 在本机实测得到的。

## 0. 先说结论：这次上传为什么没成功

本地 `D:\直播\LiveStreamLab` 已经有 `.git`、`main` 分支和 3 次提交，但**一次都没有推送成功**（`git branch -r` 为空、`main` 没有 upstream）。
卡住的原因有四个，前两个是"根本推不上去"，后两个是"即使推上去也是个烂仓库"：

| # | 现象 | 原因 | 处理 |
|---|---|---|---|
| 1 | `git ls-remote` / `gh api` 返回 **404 Not Found** | 远程地址写的是 `jian25916-ai/LiveStreamLab`，而本机登录的账号是 **hello318869**，该仓库不存在 | 用正确的账号名重建远程，或从网页复制真实地址 |
| 2 | `fatal: ... schannel: AcquireCredentialsHandle failed: SEC_E_NO_CREDENTIALS` | Git for Windows 默认用 Windows 自带的 schannel 做 TLS，本机这套凭据链不可用 | `git config --global http.sslBackend openssl`（实测立刻可用） |
| 3 | 仓库里有 **949 个 `build/` 文件、约 904 MB**，单个 `avfilter-12.dll` 就有 **98.4 MB** | 第一次 `git add` 时还没有 `.gitignore`；`.gitignore` 里写的是 `\build`（反斜杠被当成转义符），也不生效 | 修正忽略规则 + 把 `build/` 从历史里清掉 |
| 4 | `.git` 目录已经 **143 MB** | 上面那堆 DLL 被打进了 pack 文件 | 重新初始化仓库（远程还没建，历史没有保留价值） |

清理后仓库体积会从 965 MB 降到约 **61 MB**（主要是 `external/SDL` 的 53 MB）。

---

## 1. 上传前的体检（10 分钟，能省 2 小时）

### 1.1 这些东西不要进仓库

- 构建产物：`build/`、`out/`、`*.obj`、`*.pdb`、`*.dll`、`*.exe`、`*.lib`、`CMakeFiles/`
- 运行产物：`output/*.mp4`、`output/*.m4a`、日志
- 第三方 SDK 的二进制：FFmpeg 的 DLL、`.lib`
- 编辑器/系统垃圾：`.vs/`、`*.user`、`.DS_Store`、`Thumbs.db`
- 密钥：token、密码、账号配置文件

**为什么必须管**：GitHub 单个文件硬上限 **100 MiB**（超过直接拒绝推送，报 `GH001`），50 MiB 开始警告；仓库长期建议 < 1 GB。
更要命的是 Git 把每次提交都存进历史——文件删了也还在历史里，仓库永远瘦不下来。

### 1.2 体检命令（在项目根目录执行）

```powershell
cd D:\直播\LiveStreamLab

# 有多少文件被跟踪
(git ls-files).Count

# 最大的 10 个已跟踪文件（MB）
git ls-files | ForEach-Object {
    [pscustomobject]@{ MB = [math]::Round((Get-Item -LiteralPath $_).Length / 1MB, 1); File = $_ }
} | Sort-Object MB -Descending | Select-Object -First 10 | Format-Table -AutoSize

# 仓库自身有多大
git count-objects -vH

# 某个目录是不是被忽略了（有输出=被忽略，无输出=没忽略）
git check-ignore -v build
```

### 1.3 `.gitignore` 的正确写法（本仓库可直接用）

```gitignore
# ---- 构建产物 ----
/build/
/build-*/
/out/
CMakeFiles/
CMakeUserPresets.json
.vs/
*.user
*.suo

# ---- 运行产物 ----
/output/*
!/output/.gitkeep

# ---- 系统/编辑器 ----
.DS_Store
Thumbs.db
```

几个容易踩的点：

- 行首 `/` 表示"只匹配项目根目录"，`build/` 末尾的 `/` 表示"只匹配目录"。
- **不要写 `\build`**：反斜杠在 `.gitignore` 里是转义符，不是 Windows 路径分隔符。统一用 `/`。
- **`.gitignore` 对已经跟踪的文件无效**，必须配合 `git rm -r --cached <路径>`。
- 改完用 `git status --ignored` 或 `git check-ignore -v <路径>` 验证。

### 1.4 推荐再加一个 `.gitattributes`（解决 LF/CRLF 和二进制识别）

```gitattributes
* text=auto
*.bat  text eol=crlf
*.cmd  text eol=crlf
*.ps1  text eol=crlf
*.sln  text eol=crlf
*.vcxproj text eol=crlf

*.dll  binary
*.exe  binary
*.lib  binary
*.pdb  binary
*.a    binary
*.so   binary
*.dylib binary
*.png  binary
*.jpg  binary
*.ico  binary
*.mp4  binary
*.m4a  binary
```

---

## 2. 一次性环境准备

### 2.1 安装

```powershell
winget install --id Git.Git -e
winget install --id GitHub.cli -e
```

装完**新开一个终端**，确认：

```powershell
git --version    # git version 2.54.0.windows.1
gh --version
```

### 2.2 设置提交身份（作者名/邮箱会写进每个提交）

```powershell
git config --global user.name  "hello318869"
git config --global user.email "214953126+hello318869@users.noreply.github.com"
git config --global core.quotepath false     # 中文文件名不再显示成 \344\270\255 这种转义
```

### 2.3 TLS 与代理（本机就是这里坏掉的）

```powershell
# 关键修复：不用 Windows schannel，改用 Git 自带的 OpenSSL
git config --global http.sslBackend openssl
```

验证：

```powershell
git ls-remote https://github.com/git/git HEAD
# 能打印一串 40 位哈希就说明 TLS 通了
```

如果公司/校园网要代理：

```powershell
git config --global http.proxy  http://127.0.0.1:7897
git config --global https.proxy http://127.0.0.1:7897
# 取消代理
git config --global --unset http.proxy
git config --global --unset https.proxy
```

注意：`openssl` 后端用的是 Git 自带的 CA 证书包，公司做 HTTPS 拦截时可能报证书错误；这时要么让管理员给根证书（`git config --global http.sslCAInfo <证书路径>`），要么临时 `git -c http.sslVerify=false push`（不推荐长期用）。

### 2.4 登录 GitHub（推荐 gh，省掉手动配 token）

```powershell
gh auth login
# 选 GitHub.com → HTTPS → Login with a web browser，浏览器里输入终端显示的一次性代码

gh auth status        # 看到 ✓ Logged in to github.com account hello318869
gh auth setup-git     # 让 git 推送时自动用 gh 的令牌（本机已配好这一项）
```

不想用 gh 的话，去 <https://github.com/settings/tokens> 建一个 **classic PAT**（勾 `repo`），
推送时用户名填账号、密码填 token，Windows 凭据管理器会记住。
GitHub 早已不支持用账号密码推送（会报 `Password authentication is not supported`）。

---

## 3. 三种上传方式

### 方式 A：`gh repo create` 一条命令（推荐）

在**已有本地提交**的项目根目录：

```powershell
cd D:\直播\LiveStreamLab
gh repo create LiveStreamLab --public --source=. --remote=origin --push
```

它会：在当前账号下创建仓库 → 添加 `origin` → 推送当前分支并设置 upstream。
想建私有库把 `--public` 换成 `--private`。

### 方式 B：网页建库 + 本地关联（图形化，适合新手）

1. 打开 <https://github.com/new>，填仓库名（**不要**勾 "Add a README"、"Add .gitignore"，否则会和本地历史冲突）。
2. 创建后复制仓库地址，在本地执行：

```powershell
cd D:\直播\LiveStreamLab
git init -b main
git add .
git commit -m "feat: LiveStreamLab M1-M6 音视频采集与编码"
git remote add origin https://github.com/hello318869/LiveStreamLab.git
git push -u origin main
```

`-u` 把本地 `main` 和 `origin/main` 绑定，以后直接 `git push` 即可。

### 方式 C：本地已有历史，要保留

```powershell
git remote set-url origin https://github.com/hello318869/LiveStreamLab.git
git push -u origin main
```

前提是**历史里没有大文件**。有的话先用第 4 节的方法清历史，否则推上去也删不掉。

---

## 4. 本项目：清理后重新上传（可直接照抄）

`hello318869/LiveStreamLab` 现在还不存在（`gh api` 返回 404），本地只有 3 次提交，历史里塞了 900 MB 构建产物。
**最省事也最干净的做法是重做仓库**，而不是花时间清历史。

```powershell
cd D:\直播\LiveStreamLab

# ---- 1. 修 TLS（一次即可，前面 2.3 已配可跳过）----
git config --global http.sslBackend openssl

# ---- 2. 改 .gitignore（按 1.3 的内容），再补 .gitattributes（1.4）----
notepad .gitignore

# ---- 3. 重做历史（远程还没建，3 次旧提交没有保留价值）----
#        .gitignore 对已跟踪的文件无效，所以直接换一个全新的 .git 最省事
Remove-Item -Recurse -Force .git
git init -b main
git add .
git status --short | Measure-Object -Line    # 确认列表里没有 build/ 下的 DLL

# ---- 4. 确认体积合理（约 60 MB；明显偏大说明还有东西没被忽略）----
git count-objects -vH

# ---- 5. 首次提交 ----
git commit -m "feat: LiveStreamLab M1-M6 音视频采集、预处理与编码"
```

推之前再检查一次"有没有混进大文件"：

```powershell
git ls-files | ForEach-Object {
    [pscustomobject]@{ MB = [math]::Round((Get-Item -LiteralPath $_).Length / 1MB, 1); File = $_ }
} | Sort-Object MB -Descending | Select-Object -First 5
```

然后建远程并推送：

```powershell
gh auth setup-git
gh repo create LiveStreamLab --public --source=. --remote=origin --push
```

看到类似输出就成功了：

```
✓ Created repository hello318869/LiveStreamLab on GitHub
✓ Added remote https://github.com/hello318869/LiveStreamLab.git
✓ Pushed commits to https://github.com/hello318869/LiveStreamLab.git
```

验收：

```powershell
git branch -vv                 # main 后面应出现 [origin/main]
git ls-remote --heads origin   # 有一行 refs/heads/main
gh repo view --web             # 浏览器打开仓库
```

### 如果一定要保留那 3 次提交

用 `git filter-repo` 从历史里剜掉 `build/`：

```powershell
pip install git-filter-repo
git filter-repo --path build --invert-paths
git remote add origin https://github.com/hello318869/LiveStreamLab.git
git push -u origin main
```

（`filter-repo` 会要求在一个"新鲜"克隆上运行，并且会删掉 `origin`，所以后面要重新 `remote add`。）

---

## 5. 日常：改代码 → 提交 → 推送

```powershell
cd D:\直播\LiveStreamLab

git status                 # 看改了什么
git diff                   # 看具体改动
git add src/publisher/capture/VideoInput.cpp docs/capture.md
# 或者 git add -A    全部加入
git commit -m "fix: 修复摄像头断连后的资源泄漏"
git push
```

提交信息建议用 `类型: 说明` 的格式，一眼能看懂改了什么：

| 前缀 | 含义 |
|---|---|
| `feat:` | 新功能 |
| `fix:` | 修 bug |
| `docs:` | 只改文档 |
| `refactor:` | 重构，不改行为 |
| `chore:` | 构建、依赖、配置 |
| `perf:` | 性能优化 |

### 分支 + PR（多人协作或想让改动被审一遍时）

```powershell
git switch -c feature/m07-muxer     # 开新分支
git add -A
git commit -m "feat: M07 音视频合并封装"
git push -u origin feature/m07-muxer
gh pr create --fill --web
```

PR 合并后回主分支更新：

```powershell
git switch main
git pull
git branch -d feature/m07-muxer
```

### 常用命令速查

| 目的 | 命令 |
|---|---|
| 看远程地址 | `git remote -v` |
| 改远程地址 | `git remote set-url origin <新地址>` |
| 看提交树 | `git log --oneline --graph --all` |
| 撤销未提交的修改 | `git restore <文件>` |
| 撤销 `git add` | `git restore --staged <文件>` |
| 改最后一次提交信息 | `git commit --amend -m "新信息"` |
| 临时存一下改动 | `git stash` / `git stash pop` |
| 只提交部分改动 | `git add -p` |

---

## 6. 报错对照表

| 报错 | 原因 | 解决 |
|---|---|---|
| `schannel: AcquireCredentialsHandle failed: SEC_E_NO_CREDENTIALS` | Windows TLS 后端凭据链异常 | `git config --global http.sslBackend openssl` |
| `remote: Repository not found` / `404` | 仓库名或账号名不对；私有库没权限 | `gh api repos/<owner>/<name>` 验证；`git remote -v` 核对 |
| `remote: Permission to X denied to Y` | 推送账号和目标仓库所有者不一致 | `gh auth status` 看当前账号；改远程地址或换账号 |
| `GH001: Large files detected` | 单个文件 > 100 MiB | 从提交里移除；已在历史里就用 `filter-repo` |
| `RPC failed; HTTP 413` / push 中断 | 单次推送体积过大 | 分批推送，或先清掉大文件 |
| `Failed to connect ... port 443: Timed out` | 网络/代理 | 配 `http.proxy`；`Test-NetConnection github.com -Port 443` |
| `Password authentication is not supported` | 用了账号密码 | 改用 PAT 或 `gh auth login` |
| `could not read Username for 'https://github.com'` | 没有可用的凭据助手（或在限制管道的环境里运行） | `gh auth setup-git`；或建 PAT；换普通终端执行 |
| `fatal: refusing to merge unrelated histories` | 网页建库时勾了 README，两边历史无关 | `git pull --allow-unrelated-histories`，或删掉远程重新推 |
| 中文文件名显示成 `\344\270\255` | `core.quotepath` | `git config --global core.quotepath false` |
| 提交后满屏 `LF will be replaced by CRLF` | 换行符自动转换 | 正常现象；用 `.gitattributes` 统一 |
| 推送很慢、卡在 `Writing objects` | 仓库太大 | `git count-objects -vH` 查体积，清历史或改用 LFS |

---

## 7. 传上去之后建议补的几件事

1. **加 `LICENSE`**：公开仓库没有许可证，别人在法律上不能用。学习项目常用 MIT。
2. **README 开头写清三件事**：这是什么、怎么构建、怎么运行（本仓库的 `README.md` 已经写得不错，补一张界面/输出截图更好）。
3. **仓库描述和 Topics**：`gh repo edit --description "C++17 + FFmpeg + SDL3 直播学习项目" --add-topic ffmpeg,sdl3,livestream,cpp`
4. **`.gitignore` 用 GitHub 官方模板兜底**：<https://github.com/github/gitignore/blob/main/C%2B%2B.gitignore>
5. **需要放大文件时用 Git LFS**：`git lfs install` + `git lfs track "*.mp4"`，注意 LFS 有流量配额。
6. **CI（可选）**：`.github/workflows/build.yml` 里跑 `cmake -S . -B build && cmake --build build`，每次 push 自动验证能编译。

---

## 8. 一页速查（新建项目从零到上传）

```powershell
# 1. 环境（只做一次）
git config --global user.name  "你的名字"
git config --global user.email "你的邮箱"
git config --global http.sslBackend openssl
gh auth login

# 2. 忽略规则（.gitignore 先写好，再 git add）
#    /build/  /output/*  .vs/  *.user  *.dll

# 3. 提交
#    注意：先 commit 再 push，仓库里不能有 >100 MiB 的单个文件
cd 项目目录
git init -b main
git add .
git commit -m "feat: 初始提交"

# 4. 建库并推送
gh repo create 仓库名 --public --source=. --remote=origin --push

# 5. 验收
git branch -vv
gh repo view --web
```
