# BranchFS / World FS — 任务分解与进度

依据 `arch.md` v0.3。本文件是唯一的任务看板,状态用 `[ ]` / `[~]` / `[x]` 标记。

## 环境事实(2026-09-18, Mac mini M1, macOS 26.6.2)

- 只有 Command Line Tools,**没有 Xcode.app**。SwiftPM(Swift 6.3)可以直接链接 FSKit.framework,已验证。
- FSKit 扩展是 ExtensionKit appex(`EXExtensionPointIdentifier = com.apple.fskit.fsmodule`),
  本仓库用 `scripts/bundle.sh` 手工组装 `.app/Contents/Extensions/*.appex`,ad-hoc 签名,`pluginkit` 注册。
- 路径型 backing 通过 `FSPathURLResource`(macOS 26+),挂载命令:
  `mount -F -t worldfs <backing-dir> <mountpoint>`。
- 系统自带 msdos/ftp 模块带 `com.apple.developer.fskit.fsmodule` entitlement。实测:ad-hoc 签名、无该 entitlement 的
  appex 可以被 LaunchServices 注册到 `com.apple.fskit.fsmodule` 扩展点,`mount -F` 也能找到它
  (报 `Module world.forks.fs.extension is disabled!`)。模块能否真正启动仍待启用后验证(R1 部分解除)。
- **appex 必须开启 App Sandbox**:pkd 日志 `rejecting; Ignoring mis-configured plugin ...: plug-ins must be sandboxed`。
  未沙盒的 appex 会被 pkd 静默丢弃,表现为 `mount` 报 "No extension with fsShortName found"。
  解决:entitlements 里 `com.apple.security.app-sandbox=true`,plist `FSRequiresSecurityScopedPathURLResources=true`,
  扩展在 loadResource 里 `startAccessingSecurityScopedResource`。元数据 store 因此位于扩展 container:
  `~/Library/Containers/world.forks.fs.extension/Data/Library/Application Support/World/fs`,CLI 默认同路径。
- 用 `pluginkit -a` 注册过的临时副本会留下指向已删除路径的记录,要用 `pluginkit -r <path>` 撤销;`lsregister -u` 对不存在的路径无效。
- `bundle.sh` 改为 rsync 原地更新 + `open -g -j` 启动一次宿主 app(macOS 在 app 启动时登记其扩展)。
- **沙盒扩展创建的文件会被打 `com.apple.quarantine`**(agent 字段是 WorldFSExtension)。隔离 + ad-hoc 签名的
  可执行文件/dylib 在 exec/dlopen 时 AMFI 校验失败("no CMS blob"),随后**内核在 lifs 的失败路径上挂死**:
  进程不可杀、`umount -f` 也卡在 U 态,只能重启清理。`LSFileQuarantineEnabled=false` 对沙盒 appex 无效;
  扩展进程 `removexattr` 也被拒(EPERM),`LSFileQuarantineEnabled=false` 无效——这是 fskitd 对文件系统模块的
  固定策略(外来文件系统写入的文件默认隔离)。**解决**:内核是通过我们的 getxattr 得知隔离状态的,所以 core 的
  namespace 层隐藏 agent 字段等于本进程名的 quarantine 记录(`fs_xattr_is_own_quarantine`),其他来源的隔离标记
  照常可见。验证:经挂载点写入的 ad-hoc 可执行文件与 dylib 均可 exec/dlopen。
  这一点对 agent 场景致命:cargo/go 产物、venv 里的 .so、node 原生模块都是"经挂载点写入的 ad-hoc 二进制"。
- 直接放在 backing 目录里的 ad-hoc 二进制,经挂载点 exec/dlopen 正常(默认和 `openclose=forward` 两种模式都正常)。
- **启用开关是 entitlement 门控的**:`FSClient setEnabledStateForIdentifier:` 从普通进程调用返回 EPERM,
  `pluginkit -e use` 无效。必须在 系统设置 > 通用 > 登录项与扩展 > 文件系统扩展 里手动打开 WorldFS(R2 结论)。
- 纯 CLT 下 `swift test` 既没有 XCTest 也没有 Testing 模块,测试 target 已暂时移除。
  解决办法二选一:装 Xcode.app,或装 swift.org 的 macOS toolchain(自带 XCTest/Testing)。M1 开始需要单元测试,届时补。

## 工程约束(arch.md §39)

- core C++23,依赖 libc + 标准 C++ 运行时 + SQLite + header-only 的 smallstring / Containa / Arena / fmt(submodule);对外 C ABI。
- 不用 std::string / unordered_map / std::mutex / iostream;core 自身不 throw。
- CLT 的 `usr/include/c++/v1` 目录不完整会遮住 SDK 的 libc++ 头,CMake 里用 `-nostdinc++ -isystem <SDK>/usr/include/c++/v1` 绕开。
- 只有 `macos/fskit/`(Objective-C++)接触 FSKit;`cli/` C 风格 C++。
- `scripts/check-deps.sh` 检查产物不链接 libc++/libstdc++。
- 构建用 CMake(已 `brew install cmake`);Mac 上不需要 Xcode。`world` CLI 已支持 init / fork / list / inspect / mount / discard(mount 走 `-o world=N`)。

## 里程碑(对应 arch.md §29–§32)

### M0 — FSKit passthrough(不做 branching)
- [x] T0.1 仓库骨架:Package.swift,`WorldFSCore` / `WorldFSExtension` / `world` 三个 target(release 编译通过)
- [~] T0.2 Passthrough volume:lookup/getattr/readdir/read/write/create/unlink/rename/mkdir/symlink/setattr/xattr 已写完,未经挂载验证;`scripts/smoke.sh` 为验收脚本
- [x] T0.3 打包/签名/安装/注册/启用/挂载全部打通(2026-09-18 20:02 首次 mount 成功);`scripts/smoke.sh` 全过(含 mmap、fsync、xattr、hardlink)
- [x] T0.4 benchmark harness(`scripts/bench/`);首轮结果见下表
- [~] T0.4b frontend 优化:已做 `openCloseInhibited=YES`(open/close 从 146µs 降到 10µs,与 native 持平)+ 每次 read/write 现开现关(fd 缓存会 EMFILE);待做 readdir 走 `getattrlistbulk`、小文件写路径
      跑 git status / find / rg / cargo check / npm / pytest / PostgreSQL build
- [~] T0.5 Go/No-Go:passthrough ≥ 90% native——编译/读达标,元数据写 17–40%,见下表与结论;等用户拍板

### M0 首轮性能(10k 文件树,M1 Mac mini,3 次取中位数,2026-09-18)

| workload | native | worldfs | native% |
|---|---|---|---|
| find(10k 文件) | 0.056s | 0.135s | 41% |
| stat 10k | 0.079s | 0.153s | 51% |
| cat 10k 小文件 | 0.296s | 0.357s | 83% |
| git status(稳态) | ~0.03–0.06s | ~0.11–0.15s | ~40% |
| 1000 × 4KB 新文件(shell) | 1.72s | 9.13s | 19% |
| 64MB 顺序写 | 0.046s | 0.065s | 71% |

微基准(µs/op,native → worldfs):lstat 已缓存 1.5 → 1.9;lstat 冷 3.0 → 108(一次 lookup XPC);
open+close 11 → 10;pread 页缓存命中 0.6 → 0.6;listdir 50 项 32 → 269;create+write+close+unlink 41 → 321。
结论:内核层的名字/属性缓存和页缓存都在起作用,剩余开销集中在 readdir 和 create/write 的 XPC 往返以及扩展内每项目录条目的处理成本。

### M0 真实负载(fmt 源码树,`scripts/bench/realwork.sh`,2026-09-18)

| workload | native | worldfs | native% |
|---|---|---|---|
| tar 解压源码树(元数据写) | 0.122s | 0.698s | 17% |
| rm -rf 解压树 | 0.039s | 0.114s | 34% |
| git clone(本地,objects+checkout) | 0.098s | 0.248s | 40% |
| git status(稳态) | 0.048s | 0.058s | 83% |
| find \| wc | 0.028s | 0.052s | 54% |
| cmake configure | 0.508s | 0.829s | 61% |
| cmake build libfmt -j8 | 1.497s | 1.666s | **90%** |
| touch 头文件 + 增量编译 | 0.346s | 0.525s | 66% |
| rm -rf 整棵树 | 0.048s | 0.188s | 26% |

### M0 结论(T0.5 Go/No-Go 待用户决定)

- 读/编译/git status 这类 agent 主负载:83–90%,达到或接近 §29 目标。
- 元数据写密集(解压、clone、批量删除):17–40%。原因是**结构性的**:trace 显示内核对每个
  mutation 发 5–7 次 XPC(create = lookup + create + getattr×2 + getxattr(com.apple.provenance)×2;
  unlink = 7 次),每次往返约 50–90µs,而 core 侧每次操作只需 1–10µs。
- 已排除的路子:fd 缓存(EMFILE)、抑制 xattr(内核退回 AppleDouble `._` 文件,更慢)。
- 可继续做的 frontend 优化(预计再提 10–20%):readdir 用 `getattrlistbulk` 并复用 FSItemAttributes 对象;
  减少 create 后的 lookup;把 core 的 readdir 从 48µs/50 项压到 ~15µs。
- 结构性上限只能靠换前端(NFS loopback / macFUSE 同样是 per-op RPC,不一定更好)或等 FSKit 增加
  批量/缓存能力。建议:按 M1 继续做 branching(它不改变 frontend 成本),同时保留 frontend 优化清单。
- **REDIRECT(把写密集目录移出 FSKit)与 native-root clonefile World 的实测数据见
  [`docs/REDIRECT_EXPERIMENT.md`](REDIRECT_EXPERIMENT.md)**:S4/S5/S7 从 8–20% 提到 93–99%、源文件编辑不受益(仍 13–18%)、
  clonefile 约 10–20µs/文件(50k 文件 0.48s,不满足 fork<10ms)、`python -m venv` 拒绝 symlink 的 `.venv` 是硬阻断,
  且 REDIRECT 必须对内核可见(symlink)才有加速——做成 core 内部的 `entries.operation` 则 XPC 一次不少。
- **单次往返成本与单挂载点串行化的专项测量见 [`docs/PERF_STUDY_RT_PARALLEL.md`](PERF_STUDY_RT_PARALLEL.md)**:
  对标 Apple 自带 msdos FSKit 模块,68µs 的 RT 是 FSKit 地板(msdos 72.6µs),其中只有 ~1.3% 是我们的代码;
  单 mount 的请求全部排在 FSKit 建的一条 serial XPC 队列上(严格 round-robin,扩展进程恒定跑满一个核),
  QoS 提升与并发队列 offload 实测均为负收益(已回退);一个 World 一个 mount 可把聚合吞吐提到
  lookup +31% / create+unlink +206%,但下一个瓶颈是全机一份的 `fskitd`。

### M0 findings — 2026-09-18 晚间修正

**先前怀疑的 "edit-loop 数据损坏" 是测试程序自身的产物,不是文件系统 bug。** 两个原因叠加:
(1) `agentstress.sh` 的 `gen_tree` 把 body 写成 `line * 64`,kb=8 时只有 3328 字节而不是 8192,
再叠加 `str.replace("x * 3", ..., 1)` 会命中 token 内部(`x * 3` 也是 `x * 30`/`x * 39` 的前缀),
校验串对不上;(2) harness 别名:上一轮把 native 目录指进了挂载点的 **backing 目录**,
于是"两侧"写的是同一份文件,互相覆盖,校验结果全是假的。两处都已修(body 精确 kb*1024;
harness 现在用 `mount | grep " <mountpoint> "` 解析 backing 路径,native 目录落在挂载点或 backing 里就
打印原因并 `exit 2`)。

**真正找到并修掉的 bug(三个):**

1. `wfs_rename` 的窗口:重命名不是原子的,旧名已经消失、新名还没建立的瞬间,内核发来的回写
   拿到 ENOENT 被**静默丢弃**(写入的数据就此丢失)。现在 rename 在 view 锁下一次完成,
   路径解析与目录项更新对其他操作不可见中间态。
2. `wfs_unlink` 删完不清 node 记录:被删名字的 inode 记录留在表里,同一 inode 的 hardlink 兄弟
   之后解析到已经不存在的路径,报 **ENOMSG**。现在 unlink 丢掉陈旧记录,下次 lookup 会用活着的
   名字重新 intern。效果:S5 install-tree 从 **1200 次 ENOMSG / 5131 个残留文件降到 0**,
   整套 agentstress 十个场景 correctness 全 ok。
3. **chmod 成只读后回写失败(EACCES)**:因为 `openCloseInhibited=YES`,每次 read/write 现开现关
   backing 文件;而 POSIX 的 fd 在 open 之后保留权限,内核也已经先做过权限检查,所以
   "写 → fchmod 0444 → close" 这种模式(**git 写 loose object 就是这样**)在我们这里退化成
   `open(O_RDWR)` 拿 EACCES。表现是 `git commit` 在挂载点上必败:
   `fatal: error when closing loose object file: Permission denied`。现在 `openFDFor:writable:`
   在 EACCES 时临时补上 owner 写位、open 之后立刻用 `fchmod(fd, 原 mode)` 还原。
   这个 bug 之前被 S6 的 `s6_check() { [ -d .git ]; }` 掩盖了(git 全盘失败它也判 ok),
   现在改成检查 `work` 分支上确实有 2 个 commit。

**readdir 的 `.` / `..` 重复已修**:内核/FSKit 自己会合成 dot 项,core 又把 backing opendir 返回的
那两项也 pack 进去,`ls -fa` 于是显示 `. . .. .. x`。`readdir_trampoline` 现在跳过 dot/dotdot,
cookie 仍然用 backing readdir 的下标,因此单调且可续传(带 cookie 的续读既不重发也不漏发)。
100 / 1000 / 5000 项目录的完整性对比(挂载点 listdir / scandir+stat / find vs backing)全部通过,
`ls -fa` 恰好一个 `.` 和一个 `..`,`scripts/smoke.sh` 全绿。

#### agentstress(scale=1,native = 挂载点之外的真 APFS 目录,M1 Mac mini,安静机器)

| scenario | native | worldfs | native% | correctness |
|---|---|---|---|---|
| S1 context-read | 0.924s | 4.441s | 21% | ok / ok |
| S2 edit-loop | 0.148s | 0.886s | 17% | ok / ok |
| S3 edit-parallel-8 | 0.195s | 1.840s | 11% | ok / ok |
| S4 build-artifacts | 1.219s | 6.927s | 18% | ok / ok |
| S5 install-tree | 4.097s | 22.480s | 18% | ok / ok |
| S6 git-cycle | 0.643s | 3.337s | 19% | ok / ok |
| S7 test-churn | 6.242s | 10.157s | 61% | ok / ok |
| S8 watch-events | 1.711s | 1.720s | 100% | ok / ok |
| S9 big-file-8K-writes | 0.274s | 0.455s | 60% | ok / ok |
| S12 exec-artifacts | 2.371s | 2.232s | 106% | ok / ok |

(上一轮同配置:S1 43%、S2 15%、S3 9%、S4 15%、S5 14%、S6 20%、S7 62%、S8 104%、S9 77%、S12 124%;
S1/S4 的抖动来自 mds 索引新生成的文件。)

#### 微基准(µs/op,`microbench.py`,200×50 的 4KB 文件树)

| op | native | worldfs | native% |
|---|---|---|---|
| lstat same file(已缓存) | 1.5 | 1.8 | 83% |
| lstat 5000 distinct files(冷) | 3.0 | 108.9 | 3% |
| open+close | 11.1 | 10.3 | 108% |
| pread 4K(fd 已开) | 0.6 | 0.8 | 75% |
| open+read4K+close | 12.0 | 11.1 | 108% |
| listdir(50 项) | 33.1 | 261.8 | 13% |
| scandir+stat(50 项) | 116.8 | 359.7 | 32% |
| create+write4K+close+unlink | 36.5 | 304.9 | 12% |

结论没变:页缓存/名字缓存命中的路径与 native 持平,冷 lookup、readdir、create/unlink 仍然被
每次操作 5–7 次 XPC 往返(每次 ~50–90µs)主导。

### M1 — Read-only Worlds
- [ ] T1.1 `WorldFSCore`:World DAG(SQLite WAL,`worlds`/`entries`/`inodes`/`objects` 表)
- [ ] T1.2 Resolver:overlay → ResolvedNameCache → ancestry;DirViewCache
- [ ] T1.3 `world fs init` / `fork` / `list` / `inspect` / `mount`
- [ ] T1.4 1 / 10 / 100 / 1000 Worlds:fork latency、metadata RAM、mount cost、page-cache 共享验证

### M2 — Lazy APFS COW
- [ ] T2.1 writable-open 触发 `clonefile()` → private backing(temp → finalize → metadata publish)
- [ ] T2.2 WHITEOUT / rename namespace-only / metadata-only override
- [ ] T2.3 `changed` 集合 + `world fs diff`(O(changes))
- [ ] T2.4 `world fs discard` + 后台 GC
- [ ] T2.5 100 GB 文件 × 100 Worlds × 8 KB 随机写:物理写入量、首写延迟

### M3 — Real Agent Workload
- [ ] T3.1 PostgreSQL 源码树,100 Worlds,各改 1–5 文件 + 增量编译 + 测试,对比 native ≥ 90%
- [ ] T3.2 1000 sibling Worlds 读同一源码/依赖,验证无 1000× 物理读 / page-cache 放大

## 已知风险
- R1 entitlement/签名:第三方 FSKit 模块 ad-hoc 签名是否被 fskitd 接受。
- R2 FSKit 路径资源是否需要 root 才能 mount;是否需要在系统设置里手动启用扩展。
- R3 FSKit 每次 I/O 走 XPC,passthrough 性能可能达不到 90%。M0 的目的就是把这个数字测出来。

## macOS 27 FSKit 变化(2026-09-19 文档调研,未实测)

- `FSVolume.Operations / OpenCloseOperations / ReadWriteOperations / XattrOperations …`(我们现在用的)在 27 标为 deprecated,
  替代品是 `FSVolume.Handler` 及各 `*Handler` 协议,回复对象是 `FSVolumeHandlerResult` 子类(`FSLookupItemResult`、
  `FSCreateItemResult`、`FSRenameItemResult` …),文档原话:"add the ability to reply with `FSItem.Attributes` and free space"。
  → lookup/create/rename/remove 可以把属性一并回给内核,理论上能去掉 create+unlink 里 16 次往返中的 7 次 getattr。
  arch.md §7 写的就是这套 27 的名字。
- 新增 `FSContext`(带调用方 uid/gid),可按调用者限制访问 → 文件系统层的 World 隔离成为可能。
- 新增 `FSVolume.DataCacheHandler`:内核数据缓存模式协商(noCache/readCache/writeThrough/writeBack)、deferred close
  (关闭后内核仍保留缓存状态)、`setCacheState(for:cacheMode:coherencyType:action:)` 做降级/失效。
- macFUSE 5.4.0(2026-09-07)已适配:"item attribute caching based on the validity timeouts returned by the file system server",
  需要 Xcode 27 SDK。
- Apple DTS 在论坛承认 FSKit 开销主要是 "repeated XPC traffic" 且 "performance is definitely being looked at"(2025-05)。
- 未提及/未知:单次 XPC 往返成本是否下降、fskitd 是否仍是每挂载单队列、provenance xattr 探测是否仍每次发。
- macFUSE #1192:26.6.2 与 27.0 beta 6 上 fskitd 不枚举已注册启用的模块——与我们踩过的注册问题同类。

升级后的验收门槛(决定 A 是否值得重投):create+unlink 往返 ≤ 6 次且单次往返 ≤ 40µs(→ 元数据写约 40–60%);
若两者都不满足,M1 维持 C(clonefile World)主线。
