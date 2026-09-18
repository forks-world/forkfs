# Coding-agent 文件系统负载画像与压力测试设计

目的:BranchFS 的用户是 coding agent(Claude Code / Codex / Cursor / Aider 一类),
它们的文件系统行为和人类开发者不同:**更高频、更机械、更并行**。本文列出最常见场景、
每个场景实际触发的 syscall 模式,以及对应的压力测试(`scripts/bench/agentstress.sh`)。

## 1. 场景清单

| # | 场景 | 典型工具 | 文件系统模式 | 敏感点 |
|---|---|---|---|---|
| A | 读代码/找上下文 | `rg`、`grep -r`、`find`、glob、`cat`/`read_file` | 全树 readdir + stat + 小文件读,重复多次 | readdir、冷 lookup、页缓存 |
| B | 编辑文件 | apply_patch / write_file:多数实现是**整文件重写**(temp + rename)或原地 truncate+write | create/truncate/write/rename/unlink,单文件 4–200KB | create/rename/unlink 的 XPC 次数 |
| C | 构建 | tsc、vite、cargo、go build、cmake/ninja、javac | 读几千源文件,写几百到几万个中间产物(.o/.rlib/.pyc/.d),硬链接、mmap | 小文件写、目录创建、mmap |
| D | 测试 | pytest、jest/vitest、cargo test、go test | 读 + 临时文件 + 覆盖率/缓存目录写 + 大量进程 spawn(cwd 在挂载点) | tmp 文件 churn、并发 |
| E | 依赖安装 | npm/pnpm/yarn、pip/uv、cargo、go mod | 解压成千上万小文件;pnpm/uv 用 hardlink / `clonefile` / reflink;pip 写 .pyc | 元数据写洪水、link/clonefile 语义 |
| F | git | status/diff/add/commit/stash/checkout/rebase | index 读写、对象写(temp+rename)、全树 lstat、checkout 批量 create/unlink | lstat 缓存、racy-clean 重读、rename |
| G | Lint/format | eslint/prettier/ruff/black/gofmt | 读全部源文件,改写其中一部分 | 同 A+B |
| H | LSP / 索引 | tsserver、pyright、rust-analyzer、gopls | 启动时全树扫描,随后 watch;rust-analyzer mmap 大量 .rlib/.rmeta | 冷启动 readdir+read,FSEvents/kqueue |
| I | Dev server / watch | vite、nodemon、`cargo watch`、`fs.watch` | 依赖 FSEvents 或 kqueue 收到变更 | 事件是否透传、延迟 |
| J | 多 agent 并行 | 10–1000 个 World 同时跑 A–F | 同一 base 的读共享;各自私有写 | 页缓存共享、扩展进程并发、锁 |
| K | 长时会话 | 一个 agent 连续几小时 | 文件句柄/inode 表增长、fd 泄漏、内存 | 扩展进程 RSS、EMFILE |
| L | 大文件 | 数据库文件、日志、模型权重 | 随机 8KB 写、追加、mmap 读 | COW 粒度(M2)、写放大 |

## 2. 工具链实测(2026-09-18)

| 步骤 | native | worldfs | 备注 |
|---|---|---|---|
| npm install express(冷缓存,含网络) | 5.56s | 5.69s | 网络主导,IO 差异被掩盖 |
| node require express | 0.12s | 0.23s | 纯 JS,正常 |
| python -m venv | 2.13s | 2.67s | 大量 symlink + 小文件 |
| pip install requests | 7.21s | 6.63s | 网络主导 |
| python import requests | 1.6s | **挂死** | 加载 `charset_normalizer/*.so`(经挂载点写入的 ad-hoc .so)时 dyld `fcntl(F_ADDFILESIGS)` 永不返回 |

根因:沙盒扩展创建的文件被内核打上 `com.apple.quarantine`,隔离的 ad-hoc 二进制被 Gatekeeper/AMFI 拒绝后内核挂死。修复:namespace 层隐藏本进程打的隔离标记(见 `docs/TASKS.md`)。结论:凡是"在工作区内产生并执行的二进制"都会踩到,
所以 S4/S7 之外必须增加 **S12 exec-artifacts**:在挂载点编译一个 C 程序和一个 dylib,执行 + dlopen + 再改再编。

补测(2026-09-18 晚间,无网络,故 npm/pip 两行没有新数):`S12 exec-artifacts`(clang 在挂载点编译
可执行文件 + dylib → 执行 / dlopen → 改源码重编 → 再执行)native 2.371s / worldfs 2.232s = **106%**,
correctness ok——quarantine 内核挂死的回归测试通过。同一轮还修掉了两个会让 agent 直接失败的 bug:
`git commit` 在挂载点上必败(写 loose object 是 "write → fchmod 0444 → close",现开现关的 backing
open 拿到 EACCES)、`ls -a` 里 `.` / `..` 各出现两次(内核已经合成 dot 项,core 又 pack 了一份)。
细节见 `docs/TASKS.md` 的「2026-09-18 晚间修正」。

## 3. 压力测试设计(`scripts/bench/agentstress.sh`)

原则:**不碰网络,只测 IO**。依赖安装类场景用本地生成的树 + tar 解压模拟,不跑 npm/pip 联网安装。

每个测试同时在 native 目录和 worldfs 挂载点跑,输出 `name, native_s, worldfs_s, native%`,
并检查**正确性**(退出码、内容校验),不只看时间。

```text
S1 context-read       : rg 三次 + find + 随机读 2000 个文件(模拟 agent 找上下文)
S2 edit-loop          : 500 轮 "读文件 → 改一行 → temp+rename 写回",单线程
S3 edit-loop-parallel : 8 个进程各 200 轮 S2,不同文件
S4 build-artifacts    : 生成 5000 个 .o 风格文件(mkdir 深目录 + 4–64KB 写)再全部删除
S5 install-tree       : tar 解压 node_modules 风格树(20k 文件)、hardlink 一半、clonefile 一半、删除
S6 git-cycle          : git add -A / commit / status / stash / checkout -b / 改 50 文件 / diff / reset
S7 test-run-churn     : 200 次 "spawn 进程 + 写临时文件 + 删除"(模拟 pytest/jest)
S8 watch-latency      : fs.watch / kqueue 收到写事件的延迟与丢失率
S9 long-session       : 30 分钟循环 S2+S1,记录扩展进程 RSS / fd 数 / p99
S10 sibling-worlds    : N 个 World(1/10/100)各自跑 S1,验证页缓存共享与吞吐(M1 后)
S11 big-file          : 1GB 文件 1000 次随机 8KB 写 + mmap 读(M2 后验证 COW)
S12 exec-artifacts    : clang 在挂载点编译可执行文件与 dylib → 执行 / dlopen → 修改源码重编 → 再执行(验证 quarantine 不再出现)
```

每项设一个通过线:时间 ≥ 80% native(读类 ≥ 90%),正确性 100%。
