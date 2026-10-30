# 安全审查报告 / Security Audit

审查对象：`aimgr` 1.0.0（`src/`、`gui/`、`packaging/`、`tests/`）
审查日期：2026-10-30

审查方法：

- 人工通读全部本机代码与状态转换。
- 针对性攻击用例，固化为 `tests/e2e.sh` 与 `tests/unit_tests.cpp`。
- GCC AddressSanitizer + UndefinedBehaviorSanitizer 跑单元测试与离线端到端测试。
- Clang Static Analyzer（`clang-tidy` / `scan-build`）。
- 依赖面、外部进程、网络与文件系统边界梳理。
- 仓库密钥模式扫描。

## 1. 威胁模型

| 资产 | 说明 |
|---|---|
| 用户磁盘上的 AppImage | 被错误替换/删除会造成不可用，甚至执行恶意程序 |
| 版本备份 | 回滚的唯一依靠 |
| `config.json` | 更新源、被管理文件路径、备份/缓存根目录 |
| journal、锁文件、自更新状态 | 崩溃恢复与并发控制 |
| 终端 / 图形界面 | 可能被远端字符串注入转义序列 |
| `AIMGR_GITHUB_TOKEN` | 可能被泄露到第三方主机 |

| 攻击者 | 可控输入 |
|---|---|
| 恶意或被攻破的发布者 | tag、资产文件名、资产字节、HTML/JSON 响应体、重定向目标 |
| 本地其它用户 / 共享目录 | 缓存与临时目录中的符号链接、FIFO、竞态 |
| 并发的第二个 aimgr 进程 | 同时更新同一个文件或写配置 |
| 被篡改的配置 / journal / 扫描目录 | id、desktop 字段、journal 路径、超长文件名 |
| 用户填写的版本页 / 校验和页 | 任意 HTTPS URL 的响应内容 |

信任边界：

- `AIMGR_*` 环境变量、命令行参数、`$HOME` 属主视为**操作者可信**。
- `config.json` 的 `path`、`source`、备份/缓存根目录属于**功能性配置**，决定要管理哪些文件，必须由操作者自己填写；不要直接使用来源不明的配置。
- 即使配置文件被篡改，以下输入仍按不可信处理：app `id`、`.desktop` 字段、journal 中的路径、扫描到的文件名、远端 tag/资产名/响应体。

## 2. 发现与处置

| ID | 严重度 | 问题 | 处置 | 回归测试 |
|---|---|---|---|---|
| A1 | **高** | GUI 用 `std::system()` 拼接参数，`;`/`$( )` 可被 shell 执行 | 改为 `execv()`，完全去掉 shell | `gui_smoke.sh` + 代码审查 |
| A2 | **高** | AppImage 路径或 desktop 字段含换行可注入额外 `Exec=` | `escape_desktop_exec()` 转义 `" \ $ \`` 与 `%`；`sanitize_desktop_value()` 压成单行并去掉控制字符；文件 0644 | e2e desktop 注入用例 + 单测 |
| A3 | **高** | app id 直接作为目录/文件名，`--id ../../evil` 可越界写入 | `is_valid_id()` 仅允许 ASCII `[A-Za-z0-9._-]`、长度 1..64、禁止 `..` 与前导 `.`/`-`；派生路径使用 `sanitize_component()` | e2e「path-traversal id is rejected」 |
| A4 | **中** | 抓版本号时把 GitHub token 发给了用户填写的任意 `version_url` | 版本页/校验和页请求不携带认证头；token 仅发往配置的 API base | e2e token 泄漏用例 |
| A5 | **中** | 全链路接受明文 HTTP，可被中间人投毒 | 默认仅 HTTPS，重定向链同样受限；`AIMGR_ALLOW_INSECURE_HTTP=1` 才放开；显式启用证书/主机名校验 | e2e「plain http is refused」 |
| A6 | **中** | `.part` 下载文件与锁文件跟随符号链接 | 下载与锁使用 `O_NOFOLLOW`；原子写使用 `mkstemp()`；FIFO 用 `O_NONBLOCK` + `fstat` 拒绝 | e2e 全流程 + 单测 |
| A7 | **中** | 未校验就替换目标：404 页面、截断文件都能覆盖正常 AppImage | 替换前校验 ELF + `AI\x01/\x02`、大小、可选 SHA-256；失败时目标不变 | e2e 坏包/校验和用例 |
| A8 | **中** | 并发执行两次更新会互相踩踏 | 跨进程 `flock` + 进程内递归计数，所有写操作在锁内 | 代码审查 |
| A9 | **低** | 远程字符串可直接进日志，造成终端转义序列注入 | 日志出口统一 `strip_control()`（CLI 与 GUI） | 单测 `strip_control` |
| A10 | **低** | 下载与响应体无上限 | 单文件默认 8 GiB 上限；JSON 8 MiB / HTML 4 MiB；JSON 深度 64；本地 `read_file` 上限 64 MiB | 代码审查 |
| A11 | **低** | `uninstall --delete-file` 可删除非 AppImage 文件 | 删除前要求 ELF + AppImage 魔数，否则拒绝 | e2e「refuses non-AppImage files」 |
| A12 | **低** | journal 可驱动把任意文件当作备份还原 | 恢复前校验 target 在 app 目录内、backup 在备份根内，并验证备份本身是 AppImage | e2e recover 用例 |
| A13 | **低** | `$PATH` 劫持外部辅助程序 | 只从 `/usr/bin`、`/bin`、`/usr/local/bin` 取绝对路径并 `execv()` | 代码审查 |
| A14 | **信息** | 用户配置的正则可能造成 ReDoS | 只作用于短资产名/版本字符串，异常被捕获；仍建议只使用自己的配置 | 单测非法正则 |
| A15 | **中** | `deintegrate` 可删除配置里的任意 `desktop_entry` 路径 | 删除路径只由已校验的 app id 重新推导；配置里的旧路径不作为删除依据 | e2e「deintegrate only removes aimgr's own path」 |
| A16 | **低** | 远端版本号/标签直接拼进缓存目录 | app id 与版本均过 `sanitize_component()`；加载配置时再次校验所有 app id，非法 id 整体拒绝 | e2e 非法 id 配置 |
| A17 | **中** | 原子写先 `mkstemp` 再按路径重新 `open`，共享目录中可被符号链接竞态替换；复制源也可能被换成符号链接 | `mkstemp()` 的 fd 保持打开并直接写入；复制源使用 `O_NOFOLLOW` + `fstat` 校验普通文件；SHA-256 与 AppImage 检查同样 `O_NOFOLLOW` | ASan/UBSan + e2e 全流程 |
| A18 | **中** | 配置/陈旧记录可让 update、uninstall、rollback 操作非 AppImage 目标 | 普通更新拒绝替换非 AppImage 目标，只有显式 `--force` 才允许；uninstall/rollback 同样校验；restore 验证备份结构 | e2e「normal update refuses to replace a non-AppImage target」 |
| A19 | **中** | GitHub 网页回退接受页面中的跨源绝对资产链接，可能把下载引到其它主机 | 只接受与 `AIMGR_GITHUB_WEB` 同源、且路径属于当前 `owner/repo/releases/download/` 的链接；GH repo 语法同步收紧 | e2e web fallback 包含恶意链接 |
| A20 | **低** | 超长扫描文件名可生成超过 64 字节的 id，使下一次加载拒绝整个配置 | 扫描 id 截断到 64 字节、去尾部分隔符、冲突后缀保持长度合法 | e2e「long scan filename yields a short safe id」 |
| A21 | **低** | `HOME`/`XDG_DATA_HOME` 缺失时曾回退到可预测的 `/tmp/.local/...` | desktop 写入与自启动器项在缺少用户目录时直接报错，不再写共享临时目录 | 代码审查 |

## 3. 仍然存在的风险

1. **发布方被攻破**：如果 GitHub Release 或下载站本身被替换，TLS 与 SHA-256 不能证明内容善意。需要时请使用 GPG/Minisign 在外部校验。
2. **不验证 AppImage 内部签名**：`aimgr` 只保证文件结构合法且与发布方声明一致，不保证内容善意；它**从不执行**下载的文件。
3. **首次信任**：第一次登记某个仓库或 URL 时需要人工确认来源正确。
4. **功能性配置路径是可信输入**：`path`、`source`、`backup_dir`、`download_dir` 决定读写位置。不要加载来源不明的配置文件。
5. **本地同权限攻击者**：能改 `~/.config/aimgr/` 或 `~/.local/state/aimgr/` 的进程已经拥有用户权限，可做更糟的事。
6. **FUSE / AppImage 运行时**本身不在审查范围内。

## 4. 使用者加固建议

```bash
export AIMGR_GITHUB_TOKEN=...        # 只读 token，避免匿名限流
# 不要设置 AIMGR_ALLOW_INSECURE_HTTP=1，除非确认网络与镜像可信
chmod 700 ~/.config/aimgr ~/.local/state/aimgr ~/.cache/aimgr
```

- 高价值 AppImage 建议配置 `--sha256` / `--sha256-url`。
- `aimgr check --fail-on-update` 可放入 systemd timer。
- 备份目录应纳入系统快照/备份策略。
- 桌面项中的 `Icon=` 与附加参数只影响显示和启动参数，不能改变受管理的 `Exec` 路径。

## 5. 复现审查结论

```bash
# 1) 构建（仓库无编译警告）
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"

# 2) 单元 + 端到端 + GUI 冒烟
ctest --test-dir build --output-on-failure
# unit: 93 assertions
# e2e: 108 checks
# gui-smoke: pass（无显示环境自动 SKIP）

# 3) ASan + UBSan
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer -g' \
  -DCMAKE_C_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer -g' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined'
cmake --build build-asan -j"$(nproc)"
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  ./build-asan/aimgr-unit-tests
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  ./tests/e2e.sh ./build-asan/aimgr
# 结果：无 ASan/UBSan 报告，无 aimgr 代码泄漏

# 4) 静态分析
clang-tidy -p build-tidy -checks='clang-analyzer-*' src/*.cpp gui/app.cpp tests/unit_tests.cpp
scan-build --status-bugs cmake --build build-scan
# 结果：No bugs found
```

GUI 在 `detect_leaks=1` 下会报告 Mesa/llvmpipe 图形栈内部约 240 字节的分配，调用栈位于 `<unknown module>`，不在 `aimgr` 代码中。关闭第三方图形栈泄漏检测后，GUI 冒烟测试在 ASan/UBSan 下通过。

仓库密钥扫描覆盖 `ghp_*`、`github_pat_*`、AWS access key、私钥头、`password=` / `token=` 等模式；除测试使用的假 token 字符串 `supersecrettoken123` 外无命中。
