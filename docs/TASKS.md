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

### M1 — clonefile World(方案 C,2026-09-19 用户确认切换;设计见 docs/M1_DESIGN.md)
- [x] T1.1 core 重构:schema v2、snapshot/world/trash、平台原语(clone_tree/protect/probe/free_space/count)
      C ABI 换成 snapshot + world 两类对象;M0 的 view/namespace API 移到 `core/include/worldfs/worldfs_fskit.h`,
      由 CMake 选项 `WFS_FSKIT`(默认 OFF)决定是否编译 `core/src/view.cpp` 与 `macos/fskit/`(ON 时已验证可编译、测试通过)。
      pool(T1.5)未做,fork 走当场 clone。
- [x] T1.2 CLI:init/fork/checkpoint/list/inspect/discard/restore/gc/status/verify/adopt + 防错规则,
      每条拒绝一行原因 + 一行"正确的命令";退出码 0 ok / 1 error / 2 usage / 3 refused-by-safety-rule。
- [x] T1.6a safety 测试套件 `scripts/tests/safety.sh`(P1/P2/P3/P4/P6/P7/P8/P9/P12/P13,35 条全过);
      core 单测 `core/tests/core_test.cpp` 覆盖同样的规则 + P11/P13 的 API 层。
      T1.1b/T1.4 之后扩到 71 条(加 P5/P14,P3 改写成 gate + --hard 两条路径)。

#### T1.1/T1.2 实测(2026-09-19,同一台 M1 Mac mini / macOS 27.0,best of 3,机器非空闲)

| 操作 | 1k 条目 | 10k 条目(10200) | 50k 条目(50993) |
|---|---|---|---|
| `world fs init`(count + probe + clonefile + protect + manifest + SQLite) | 0.037 s | 0.280 s | **1.363 s** |
| `world fs fork`(clonefile + unprotect + marker + rename + SQLite) | 0.040 s | 0.300 s | **1.350 s** |
| `world fs verify S<n>`(读清单 + 逐项 lstat + 计数) | 0.010 s | 0.041 s | 0.177 s |
| `world version`(进程 + 动态链接的地板) | 0.004 s | — | — |

分解(50k):

| 成分 | 实测 | 对照 |
|---|---|---|
| `clonefile(dir)` 本身 | 0.435 s | CLONE_MODEL §1.1 在空闲机上是 0.370 s;本轮机器更忙 |
| 源树一次 walk(P9 硬链接统计 + P11 条目数) | 0.047 s | 新增成本;克隆后再数是数不出硬链接的(§11) |
| `fs_protect_tree`(4 线程 chflags+chmod,不含 manifest) | **0.679 s** | M1_DESIGN 假设 ~0.4 s,**实际慢 1.7×** |
| `fs_unprotect_tree`(4 线程) | **0.730 s** | 同上 |
| manifest 写入 + rename + SQLite | ~0.20 s | |

线程数扫描(50k 克隆树,同一棵):

| 线程 | 1 | 2 | **4** | 8 | 16 |
|---|---|---|---|---|---|
| protect | 1.899 s | 1.031 s | **0.679 s** | 0.700 s | 0.728 s |
| unprotect | 1.649 s | 1.007 s | **0.730 s** | 0.654 s | 0.704 s |

与 §9.2 的 clonefile 结论一致:**4 线程就到顶**,瓶颈是 APFS 的元数据事务而不是 CPU。

**结论**:fork 的一半时间花在 unprotect 上(0.73 s / 1.35 s)。
当时的判断是"P3 的逐条目保护无法省掉",这一条在 T1.1b 被推翻(见下)。

- [x] **T1.1b 保护模型换成 gate 目录**(2026-09-19,架构师决定)
      默认保护 = 快照根目录本身 `chmod 0000`,树里的条目一个都不动。内核在解析任何子路径之前
      就挡住了 traverse/list/read/write/create/unlink,普通工具和 agent 连里面有什么都看不见;
      因为条目没被改过,**fork 不需要任何 unprotect 遍历**,克隆出来就是可写的正常树。
      clonefile 期间(fork / verify)core 自己把根临时开到 `0500`,用快照 `manifest` 文件上的
      独占 flock 串行化:同一快照的并发 fork **不共享窗口**,第二个等第一个关门后再开。
      逐条目 `UF_IMMUTABLE` 保留为显式选项 `init/checkpoint --hard`(`snapshots.hard=1`),
      从 hard 快照 fork 仍要走 unprotect 遍历;`verify` 两种都支持
      (gate 快照额外检查"根是不是被人留成敞开的")。
      P9 硬链接统计与 P11 条目数仍在克隆前的源树 walk 里(50k 上 0.047 s)。
- [x] T1.4 exec:`world exec W<n> [--no-sandbox|--require-sandbox] -- <cmd...>`(P5 + P14)
- [x] T1.6b safety 覆盖 P5/P14,并按 gate 模型重写 P3;core 单测同步。

#### T1.1b 实测(2026-09-19,同一台 M1 Mac mini / macOS 27.0,best of 3,负载 ~2–4,比 T1.1 那轮更忙)

同一棵树,gate 与 `--hard` 各自独立的 store(条目数含根):

| 操作 | 1 041 | 10 401 | 52 001 |
|---|---|---|---|
| `fork`(gate,**新默认**) | **0.048 s** | **0.141 s** | **0.615 s** |
| `fork --hard 快照`(= a937ce0 的行为) | 0.062 s | 0.322 s | 1.509 s |
| a937ce0 实测(50 993 条目,更空闲的机器) | 0.040 s | 0.300 s | 1.350 s |
| `init`(gate) | 0.055 s | 0.175 s | 0.785 s |
| `init --hard` | 0.064 s | 0.330 s | 1.507 s |
| `verify`(gate / hard) | 0.035 / 0.034 s | 0.068 / 0.069 s | 0.213 / 0.219 s |

对照:同一时刻同一棵 52 001 条目树的**裸 `clonefile(dir)`** best of 3 = **0.563 s**。
即 gate fork = clonefile + 0.05 s(probe + chmod + marker + rename + 两次 SQLite 事务 + 进程启动),
已经贴着 clonefile 的地板;50k 没有落到目标区间 0.45–0.5 s 纯粹是因为这轮机器上 clonefile 本身
就要 0.563 s(a937ce0 那轮是 0.435 s,CLONE_MODEL §1.1 空闲机是 0.370 s)。
1k 的 48 ms 里有 ~4 ms 是进程启动 + 动态链接。

**结论**:fork 的成本从"clonefile + 一次全树 chflags 遍历"降到"clonefile + O(1)"。
`fork < 10ms`(arch.md §1)仍然只有 T1.5 的 pool 能满足,但 pool 现在只需要预克隆,不再需要预 unprotect。

#### T1.4 exec(P5 + P14)

- 锁:`<store>/locks/W<n>.lock`(pid + 起始时间 + 命令行),flock 独占,`FD_CLOEXEC`,
  进程退出/被杀由内核释放。`discard` / `checkpoint` / 从该 World `fork` 检测到活锁就返回
  `WFS_E_WORLD_BUSY`(退出码 3,提示 `--force`);pid 已死或 flock 能拿到的锁算 stale,静默删除。
  锁放在 store 而不是 World 根:World 根是用户的项目树,锁文件会进 `git status`、会被克隆进子 World。
  (设计文档里写的是 `.world/lock`,但本实现里 `.world` 是文件不是目录。)
- 沙盒:默认用 `sandbox-exec -f <profile>`(27.0 上仍然可用,只是 deprecated)。profile 生成到
  `<store>/tmp/`,命令结束即删,`gc` 兜底清理超过 1 小时的残留。
  seatbelt **后匹配的规则赢**,所以 allow 全写在前、deny 全写在后:
  `(allow default)` → allow 本 World 根 / `$TMPDIR` / `/private/tmp` / agent 缓存目录 →
  `deny file-write*` 整个 store、每个别的 World 根,`deny file-read*` `<store>/snapshots`。
  profile 先拿 `/usr/bin/true` 试跑一次,跑不起来就降级为无沙盒并打印大写 WARNING,
  `--require-sandbox` 则改为拒绝。
- 退出码透传;子进程被信号杀死时返回 128+signo;SIGINT/SIGTERM/SIGHUP 转发给子进程,锁总是释放。

- [x] T1.3 diff:FSEvents 候选 + 全扫回退 + 逐项比对(P10);C ABI `wfs_world_diff` / `wfs_world_diff_ex`,
      CLI `world fs diff W<n> [--full|--events] [--stat] [--no-xattr] [--no-content]`。
      新文件:`core/src/diff.cpp`、`core/src/platform_darwin_events.cpp`、`core/src/events.h`、
      `core/src/snapshot_access.{h,cpp}`、`core/tests/diff_test.cpp`。详见下节。
- [ ] T1.5 pool:后台预克隆(现在只需预克隆,不需预 unprotect)
- [~] T1.6 safety 测试套件(P1–P14):P1/P2/P3/P4/P5/P6/P7/P8/P9/**P10**/P12/P13/P14 已覆盖
      (71 条 + 6 条 P10 全过;更细的精确集合断言在 `core/tests/diff_test.cpp`);
      P11 的"真实磁盘写满"仍只在 API 层验证
- [ ] T1.7 基准:fork 延迟、diff、1000 idle World、存储增长
      **附带一条优化**:全扫的走树现在对每个文件都发 `listxattr(2)`(两边各一次,APFS 上约 10 µs),
      5 万文件的全扫因此从 0.164 s 涨到 1.354 s——这是全扫最大的一块成本。
      `core/src/platform_posix.cpp` 的 walker 应该换成 `getattrlistbulk(2)`,
      用 `ATTR_CMNEXT_EXT_FLAGS` 拿 `EF_NO_XATTRS`,**没有 xattr 的文件直接跳过 listxattr**
      (绝大多数文件都没有),顺带一次系统调用批量拿到 stat 信息。
      做完之后默认的全扫应该能逼近 `--no-xattr` 的数字,默认选路的阈值要跟着复核。
- [ ] T1.8 文档:arch.md 增补章节、README

#### T1.3 diff 实测与结论(2026-09-19,M1 Mac mini / macOS 27.0)

**FSEvents 参数(先写探针实测再定,探针在 scratchpad,结论写进 `core/src/platform_darwin_events.cpp` 顶注释):**

| flag | 取 | 理由 |
|---|---|---|
| `kFSEventStreamCreateFlagFileEvents` | **是** | 不开就只有目录粒度,每次 diff 退化成目录扫描。实测 800 改动 → 800 条 file-level 路径 |
| `kFSEventStreamCreateFlagNoDefer` | **是** | 第一批立即投递而不是等 latency 窗口;latency 已经是 0,但这是 §6.2 测的那一套 |
| `kFSEventStreamCreateFlagIgnoreSelf` | **否** | 它压制的是"持流进程当场产生的事件",对**历史回放**没有意义(事件是别的进程很久以前记的)。而且没用:fork 自己的写全部发生在 `<target>.wfs-tmp` 下,**实测**不出现在 world 根的回放里(2000 文件克隆 + unprotect + rename,回放只有 21 条 = 20 改动 + world 根那一条 rename)。再说 P10 下多一个候选只值一次 lstat,少一个候选才是漏报 |
| `kFSEventStreamCreateFlagWatchRoot` | **否** | 那是给长命 live 流跟随被移动的根用的;这条流活几十毫秒,回放过去 |
| `kFSEventStreamCreateFlagUseCFTypes` | **否** | `char**` 路径,省掉每条事件一个 CFString |
| 消费者 | **专用 serial dispatch queue** | §6.2 的反例:消费者被阻塞才丢事件。回调只 memcpy 路径,校验全部在 join 之后做。用 dispatch queue 而不是 runloop,因为 `FSEventStreamScheduleWithRunLoop` 在 13.0 起 deprecated,两者实测结果逐字相同 |

**回退(任何一条都走全扫,且不打断调用方):**

| 触发 | 判据 |
|---|---|
| `WFS_DF_REQUESTED` | `--full` |
| `WFS_DF_FROM_WORLD` | World fork 自另一个 World(或 adopt 而来):cursor 是**本次** fork 的,盖不住父 World 在 fork 之前已经改掉的东西 |
| `WFS_DF_NO_CURSOR` | 行里没有 fsevents id |
| `WFS_DF_MUST_SCAN` | `kFSEventStreamEventFlagMustScanSubDirs` |
| `WFS_DF_DROPPED` | `UserDropped` / `KernelDropped` |
| `WFS_DF_WRAPPED` | `EventIdsWrapped`,或记录的 id 比 `FSEventsGetCurrentEventId()` 还大 |
| `WFS_DF_STALE` | `FSEventsCopyUUIDForDevice` 返回 NULL,或 cursor 比卷的 journal 还老 |
| `WFS_DF_TIMEOUT` | HistoryDone / 水位标在 2 s 内没回来 |

**`FSEventsGetLastEventIdForDeviceBeforeTime` 在 27.0 上吃的是 unix 秒,不是 CFAbsoluteTime**(签名写的是 `CFAbsoluteTime`)。
传真正的 CFAbsoluteTime 一律返回 0;传 unix 秒返回合理的 id,并在时间早于 journal 保留期时返回 0。
本机数据卷的保留期实测约 **18–24 h**(`now-64800` 还有 id,`now-86400` 是 0)。
→ 昨天 fork 的 World 今天 diff 会自动全扫,这正是想要的。
**陈旧 cursor 必须挡在建流之前**:实测 `sinceWhen` 早于 journal 时,FSEvents **一条事件都不投,也永远不发 HistoryDone**——
看起来和"没有任何改动"一模一样。这是本任务里最危险的一个坑。

**fseventsd 的 journal 延迟(新测,决定了这个特性的上限):**

| 改动类型 | 从改完到一条**新建**的流能回放到它(5 次) |
|---|---|
| write | 99 / 592 / 395 / 105 / 287 ms |
| chmod | 97 / 91 / 326 / 89 / 91 ms |
| setxattr | 91 / 90 / 155 / 296 / 91 ms |

§6.2 的"最后一条 +3.1/+11.8 ms"是**已经跑着的 live 流**;diff 是事后建流,必须先等 fseventsd 把事件写进 journal。
成批改动会立刻 flush(850 条的 fixture 从没漏过),**孤立的一次改动要等定时器**。
为此 diff 在 HistoryDone 之后还会等一个自己的水位标:建流前在 **store 目录**(绝不在 World 里,diff 不能改 World)
建一个 `.wfs-diff-<pid>`,并把 store 一起 watch;它绕回来就说明流是活的、journal 已经越过了 diff 开始的时刻。
**但它不能证明完整性**:journal 的 flush 不是全局有序的——实测后写的哨兵会比另一个目录里更早的改动先回来。
再等 150 ms 静默也试过,10k diff 变成 0.22–0.70 s,**比直接走两棵树还慢**,已回退。
→ **结论:最近 ~100–600 ms 内的孤立改动,事件路径可能看不到;`--full` 是确定的答案,而且很便宜。**
**合并时的新数据:成批改动也不是 100% 安全。**850 条改动的 fixture 在"改完立刻 diff"下,
27 次里有 2 次事件路径少报(精确集合断言当场失败);journal flush 不是全局有序的,
哨兵可能比这批里最后一个文件先回来。`diff_test` 因此在这一步也加了 `settle()`——
它测的是 diff,不是 fseventsd 的定时器。这条数据也是把全扫定为默认的理由之一。
`core/tests/diff_test.cpp` 里的 `settle()` 就是为此存在:它测的是 diff,不是 fseventsd 的定时器。

**比对(P10:事件只是候选,永远不是答案):**
两边都在 → 类型/大小/mode/mtime;大小相等而 mtime 不同就**读两边的字节**(克隆共享 extent,但用户态看不出来,也没有"这两个是不是同一批 extent"的调用);
内容相同而 mode/uid/gid/flags/mtime/xattr 不同 → `T`;只在 World → `A`;只在快照 → `D`;rename 在 M1 就是 `D` + `A`。
`UF_IMMUTABLE`/`UF_APPEND` 在比对时被掩掉——那是 P3 的保护,不是用户改的。
**候选是目录时必须展开**:实测目录改名只产生两条事件(旧名、新名),里面的文件一条都没有,
所以只在一侧存在的目录要在那一侧走一遍,把每个文件报出来;空目录报它自己。
只报文件,空目录的 A/D 除外。
读快照一侧一律经过 `snapshot_open_for_read()`(`core/src/snapshot_access.{h,cpp}`)。
**合并 T1.1b 之后这不再是 no-op**:`SnapGate` 从 `world.cpp` 搬进 `snapshot_access.{h,cpp}`,
fork / checkpoint / verify / diff 共用同一份实现、同一把 manifest 上的 flock,
`snapshot_open_for_read()` 按根目录当前是否可 traverse 决定要不要开门(`--hard` 快照 0555,什么都不用做)。
**门只在比对阶段开**:FSEvents 建流 + 等水位标(几百 ms)在门外做,
比对(50k 全扫 0.16–1.4 s)在门内做,排序和回调在门外做。代价要说清楚:
比对期间同一快照的 `fork` 会阻塞等这把 flock。RAII guard 保证任何错误路径出去时根都被 chmod 回 0000
(`diff_test` 里有一条专门制造"门开着时走树失败"的用例来验证这一点)。

**xattr 的代价(新发现):** `listxattr(2)` 在 APFS 上约 10 µs/次,两边各一次。
候选路径上几百个文件无所谓,全扫时是 10 万次系统调用:5 万文件全扫 **0.157 s → 1.339 s(8.6×)**。
默认仍然比(回退来的全扫必须至少和事件路径一样完整),`WFS_DIFF_NO_XATTR` / `--no-xattr` 可以关掉
(关掉后纯 xattr 改动会被当成没变)。

**实测(best of 3,机器非空闲):**

| 树 / 改动 | FSEvents | `--full` | `--full --no-xattr` |
|---|---|---|---|
| 10 100 条目 / 850 改动(500 M + 200 A + 100 D + 50 T) | **0.048 s** | 0.233 s | 0.035 s |
| 50 500 条目 / 800 改动(500 M + 200 A + 100 D) | **0.087 s** | 1.354 s | **0.164 s** |

(同一轮的 `init` 1.46 s / `fork` 1.47 s,与 T1.1 的 50k 数字一致。机器变忙时三列一起涨:
并发跑压力测试那一轮是 0.159 / 1.895 / 0.271。12 次连跑 `diff_test` 全过。)

对照 M1_DESIGN §1 的"全扫 0.8 s / 5 万":我们的 4 线程 C 走树 **0.157 s**,比设计假设快 5×
(§6.1 那 0.647 s 是单线程 python `os.scandir`)。
**于是 O(changes) 的优势比设计预期小得多**:事件路径 0.087 s vs 最便宜的全扫 0.164 s,只有 1.9×;
真正拉开差距的是"要不要比 xattr"(8.6×),不是"要不要用事件"。
树再大一个数量级时事件路径才重新变成决定性的。
更糟的是事件路径有一笔固定开销(建流 + 等水位标):**6 个文件的小 World,`--full` 0.002 s,事件路径 0.4 s**,
交叉点大约在几万文件。

**默认路径(2026-09-19 合并时架构师拍板,依据就是上面这些数字):**

- **全扫是默认。** 理由三条:(a) 50k 上事件路径只快 1.9×(0.087 vs 0.164),真正的差距在 xattr 而不在事件;
  (b) 事件路径的固定开销让小树慢两个数量级(0.002 s → 0.4 s);
  (c) fseventsd 的 journal flush 不是全局有序的,**最近 ~100–600 ms 内的孤立改动事件路径可能看不见**,
  而全扫永远是准的。精确优先。
- **事件路径只在两种情况下走**:World 记录的 `entries` 超过 `WFS_DIFF_EVENTS_MIN_ENTRIES`
  (常量,默认 **200000**,可用同名环境变量覆盖,0 = 总是先试事件),或者显式 `--events`
  (C ABI `WFS_DIFF_EVENTS`)。阈值取在实测交叉点(几万)之上一个数量级:全扫是准确答案,事件是优化。
- **`--full`(`WFS_DIFF_FULL`)压倒一切**,永远走全扫。
- 哨兵和**全部回退触发条件一条不动**:走事件路径时 MustScan / Dropped / Wrapped / Stale / Timeout /
  没有 cursor / fork 自 World 仍然静默退回全扫,并在 stderr 打一行原因。
  新增的 `WFS_DF_SMALL_TREE` 只是 stats 里"这次是按默认走的全扫"的标记,**不是**回退,CLI 不为它打提示。
- xattr 比对仍然默认开(精确优先),`--no-xattr` 仍然可以关。

这一条在 T1.7 里复核后写回 arch.md §25。

**测试(`core/tests/diff_test.cpp`,独立 ctest target,600 s 超时):**
10k fixture(100 目录 × 100 文件)+ 500 改 / 200 增 / 100 删 / 50 只改 mode,
每条断言都是**精确集合**:输出必须有序、无重复、每一行都等于 fixture 对那个路径做的事,四个计数分毫不差。
覆盖 (a) 事件路径、(b) `--full`、(c) 三种坏 cursor(stale / wrapped / 没有)——都必须静默回退且结果一致、
(d) 再叠 5000 次快速改写之后仍然精确(消费者是只 memcpy 的专用队列,§6.2 让探针丢事件的那种负载在这里不丢)、
(e) `--no-content`、(f) 回调提前返回能中断、(g) 未改动的 World 两条路径都是 0 行。
小 fixture 另外覆盖:空目录 A/D、目录改名 → `D` + `A`、symlink 改指向、内容相同只有 mtime 变 → `T`、
纯 xattr 改动(事件路径与 `--full` 都报 `T`,`--no-xattr` 故意报不出来)、
fork 自 World 的子 World 自动全扫、World 被移动后仍能 diff(P1)、
**来源快照没了(行不是 ACTIVE / 树被移走)→ `WFS_E_SOURCE_GONE`,退出码 3**。
合并 T1.1b 之后又加了两组:
**门**(gate 快照的根在 diff 前/后/出错后都必须是 0000;错误路径是在 World 里放一个 0000 的目录让走树失败;
出错之后紧接着还能从同一快照 fork,证明 flock 放掉了)、
**默认选路**(阈值取真实值时 `flags=0` 必须是 `full_scan=1 / fallback=WFS_DF_SMALL_TREE`,
`--events` 必须走事件,`--events --full` 仍然全扫,阈值调小则自动走事件)。
整个 `diff_test` 跑之前把 `WFS_DIFF_EVENTS_MIN_ENTRIES` 设成 0,否则 10k fixture 下事件路径一条都测不到。
`WFS_DIFF_BENCH=1` 另跑 5 万文件的基准。

**CLI:** `world fs diff W<n> [--full|--events] [--stat] [--no-xattr] [--no-content]`;
`--stat` 只打计数;回退时在 stderr 打一行原因(P10 是承诺,做不到就要说),但默认的全扫和 `--full` 不打;
trashed / dead World、来源快照没了、World 不在记录的路径上 → 退出码 3;**diff 不拿 World 锁**,
被人占用的 World 照样可以 diff(`WFS_E_WORLD_BUSY` 不是 diff 的拒绝理由)。

### 原 M1/M2/M3(FSKit 路线,已冻结,仅存档)
#### M1(旧)— Read-only Worlds
- [ ] T1.1 `WorldFSCore`:World DAG(SQLite WAL,`worlds`/`entries`/`inodes`/`objects` 表)
- [ ] T1.2 Resolver:overlay → ResolvedNameCache → ancestry;DirViewCache
- [ ] T1.3 `world fs init` / `fork` / `list` / `inspect` / `mount`
- [ ] T1.4 1 / 10 / 100 / 1000 Worlds:fork latency、metadata RAM、mount cost、page-cache 共享验证

#### M2(旧)— Lazy APFS COW
- [ ] T2.1 writable-open 触发 `clonefile()` → private backing(temp → finalize → metadata publish)
- [ ] T2.2 WHITEOUT / rename namespace-only / metadata-only override
- [ ] T2.3 `changed` 集合 + `world fs diff`(O(changes))
- [ ] T2.4 `world fs discard` + 后台 GC
- [ ] T2.5 100 GB 文件 × 100 Worlds × 8 KB 随机写:物理写入量、首写延迟

#### M3(旧)— Real Agent Workload
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

### macOS 27.0 实测(2026-09-19,build 26A428,同一份二进制/同一套 deprecated Operations API)

完整原始数据见 [`docs/MACOS27_MEASUREMENTS.md`](MACOS27_MEASUREMENTS.md)。27 SDK(Handler API)未安装,
所以这是"同一份代码换个系统"的复测,不是"用新 API 重写"的结果。

**门槛判定(两条都不满足,且都比 26.6.2 更差):**

| 门槛 | 阈值 | 26.6.2 | **27.0** | 判定 |
|---|---|---|---|---|
| create+unlink 往返次数 | ≤ 6 | 16.06 | **16.025** | **FAIL**(超 2.7×,零改善) |
| 单次往返(lookup-miss) | ≤ 40µs | 68.1µs | **73.6µs** | **FAIL**(超 1.8×,退步 8%) |

- 往返构成逐项不变:`getattr` 7.025、`lookup` 3.00、`getxattr`(全是 `com.apple.provenance`)2.00、
  `create`/`remove`/`reclaim`/`sync` 各 1.00。**mutation 后的 getattr 刷新和 provenance 探测都还在。**
- RT 涨价不是我们的问题:Apple 自带 msdos 模块的同一探针也从 72.6µs 涨到 78.0µs(+7%),我们仍比它快 6%。
- `-o` 挂载选项**仍然到不了扩展**(`FSTaskOptions.taskOptions` 恒为空数组),26.6.2 的这个 bug 原样存在。
- 单 mount 仍然串行:8 客户端时 99.5% 的样本在一条 `com.apple.NSXPCConnection...(serial)` 队列上,
  扩展 88.2% CPU、`fskitd` 107.9%。8 mount × 8 进程:lookup 46.5k ops/s、create+unlink 3.5k ops/s
  (26.6.2 是 51.7k / 3.8k),瓶颈仍是全机一份的 `fskitd`。
- 内核的否定名字缓存与属性缓存**依旧有效且与 native 持平**(同名 miss 0.78µs、已存在文件 lstat 0.91µs);
  `vfs.generic.lifs` 12 个键的值与 26.6.2 逐字相同,两个 meta cache 计数器是活的。
- 读/编译路径与 26.6.2 持平或更好:`cmake build -j8` 92%(26.6.2 90%)、open+close 106%、pread 104%、
  S12 exec-artifacts 112%、S8 watch 99%。agentstress 十个场景 correctness 全 ok。

**27 新出现的三个问题:**

1. **`readdir(3)` / `getattrlistbulk` 不再返回 `.` 和 `..`**:27 的内核不再合成 dot 项,
   而 26.6.2 的修复让 core 跳过了它们(Apple 的 msdos 模块是自己 pack 的)。
   `ls -fa` 仍正确(BSD ls 自己合成),find/git/tar/rm -rf 实测不受影响,但这是 POSIX 语义缺口,
   **需要在 `core` 的 `readdir_trampoline` 里把 dot/dotdot 重新 pack 回去。**
2. **FSKit 的 `-[FSModuleVolume(Project) getItemForFH:]` 会退化成线性扫描**:
   `sample` 显示 95% CPU 花在一个以 `FSFileHandle` 为 key 的 `NSMutableDictionary` 查找上,
   沿哈希桶链逐个 `isEqual:`。表现是**同一挂载点上按目录呈稳定双峰**(75µs vs 270–560µs),
   写路径(每次 create/remove 造新 handle)最坏被拖慢 3–12×:
   create+unlink 1046µs(新挂载)→ 3781–4577µs(老挂载),8 进程聚合从 1249 掉到 402 ops/s。
   劣化可逆(churn+reclaim 后自愈)。这是 FSKit 自己的代码。
   实测后果:agentstress 第一轮 S4 82.8s / S5 134.8s,第二轮恢复到 8.7s / 24.3s;
   `realwork` 的 "touch 头文件 + 增量编译" 从 66% native 稳定掉到 **24%**(两轮复现)。
3. **每轮 mutation 会打一条 error 级日志** `getStandardItemAttributesForItem ... error:70`(ESTALE),
   45 分钟测试产生 15 万条以上,落盘噪声。

**27 SDK 的新 API 确实存在于运行时**(`FSVolumeHandler`、`FSLookupItemResult(initWithFoundItem:itemName:itemAttributes:)`、
`FSCreateItemResult(... newItemAttributes:directoryAttributes:freeSpace:)`、`FSContext`、`FSVolumeDataCacheHandler` 全部 present;
旧的 `FSVolumeOperations` 在 27 的 FSKit 里已改名为 `FSVolumeCommonOperations`,我们的 appex 因为自带协议对象仍能工作)。
按 trace 估算,即使用它消掉全部 7 次 `getattr`,16.03 也只降到 ~9 次,**仍然过不了 ≤6 的门槛**,
而且 3 次 lookup 和 2 次 provenance getxattr 不在这套 API 的射程内;单次往返更是反向走了 8%。

**结论:T0.5 的 Go/No-Go 不因 macOS 27 改变。M1 维持 C(native-root clonefile World)主线;
27 SDK Handler API 的改造降级为"装好 Xcode 27 之后的可选实验",不作为路线依据。**
另外补三个 M0 收尾项(2026-09-19 已全部做完,见下节)。

### M0 收尾 — 2026-09-19(macOS 27.0 实测暴露的三个问题)

- [x] **T0.6 `readdir` 的 `.` / `..`**(`core/src/view.cpp` + `core/src/platform_posix.cpp`)
  内核版本不同,要求正好相反:Darwin ≤ 26 的 VFS 自己合成 dot 项(模块再 pack 就会重复),
  Darwin 27 不再合成(模块不 pack 就一个都没有)。没有能力位、`-o` 选项到不了扩展、C ABI 又不能改,
  所以判定放在 `wfs::fs_readdir_emits_dots(bool with_attrs)` 里,从 `sysctl kern.osrelease` 读一次主版本号并缓存
  (`static int cached = -1`,常量初始化,符合 §39 的"禁止静态对象动态初始化")。
  **第二个维度是枚举种类**:27 上模块 pack 的东西会原样流进两个目录系统调用,而它们的语义不一样——
  native APFS 的 `getdirentries(2)` 带 dot 项、`getattrlistbulk(2)` **从不**带(BSD `ls`/fts 正是因此自己合成 dot 行)。
  内核替我们把两者分开了:实测 `readdir(3)` 到达扩展时 `attrs=0`,`getattrlistbulk` / `ls` / `find` 是 `attrs=1`。
  因此只在"不带属性"的那次枚举里 pack dot 项,两个系统调用同时与 native 对齐。
  `.` 带目录自己的**逻辑** ino(挂载根是 `WFS_INO_ROOT`,不是 backing 的 st_ino),`..` 带父目录的逻辑 ino(根的父是根本身);
  want_attr 时 dot 项的属性走 `wfs_getattr(逻辑 ino)`,且**绝不 intern**(dot 名字不属于任何 inode)。
  cookie 仍然是 backing readdir 的下标,所以续传语义与改动前完全一致。
  实测(mnt4 vs native APFS):50 项目录 raw `getdirentries` **52 : 52**,100 / 1000 / 5000 项同样是 n+2 : n+2
  (跨多个缓冲区也只有一个 `.` 一个 `..`);`getattrlistbulk` 两侧都是 n 项 0 个 dot;`ls -fa` 恰好一个 `.` 一个 `..`;
  `os.scandir` / `os.listdir` 无 dot(libc 过滤);`.`/`..` 的 `d_ino` 与 `lstat` 一致,`d_type` 都是 `DT_DIR`;
  readdir 完整性 100 / 1000 / 5000 四路比对全 `identical=YES`。
  `core_test` 的期望值调用同一个 `wfs::fs_readdir_emits_dots(with_attrs)`,两个分支(带属性/不带属性)都覆盖。
- [x] **ESTALE 日志噪声**(`macos/fskit/WorldVolume.mm` + `WorldItem.h/.mm`)
  trace 确认序列是 `lookup → getattr(X) → remove(X) → **getattr(X)** → getattr(dir) → sync → reclaim(X)`,
  每轮 create+unlink 恰好一条 `getStandardItemAttributesForItem ... error:70`。
  两个方案都测了:**(b) 改回 ENOENT 没用**——op 数不变(16.30/轮),错误行数不变(20/20 轮),
  只是把 `error:70` 换成 `error:2`;**(a) 在 `WorldItem` 里存最后一次 getattr 的 `wfs_attr`、
  core 报 ESTALE 时拿它作答**——op 数同样不变(16.30/轮),错误行 **20 → 0**。选 (a)。
  这不是编造属性:那份快照就是同一轮里 remove 前几微秒的那次 getattr 的结果;
  缓存只到属性为止,**没有恢复 per-item fd 缓存**。
- [x] **`scripts/smoke.sh` 不能在同一个活挂载上连跑两次**
  收尾的清理从 backing 侧删文件(`rm -f "$B/smoke.txt" "$B/w.txt"`),view 的 ino→path 表看不见,
  留下悬空节点,下一轮 `echo > "$M/w.txt"` 解析到旧 inode 拿 ENOENT。改成一律经挂载点删除,
  并在脚本里写下复跑检查。实测同一个挂载点上**连跑三次全 ALL OK**。

**验收(mnt4,2026-09-19):** `smoke.sh` ×3 全 ALL OK;`ctest` 1/1 通过;
`check-deps.sh build/Release` 三个产物全是系统库;`agentstress.sh` scale=1 十个场景
correctness 全 `native:ok worldfs:ok`(S1 15% / S2 15% / S3 11% / S4 10% / S5 15% / S6 17% /
S7 59% / S8 108% / S9 69% / S12 134%,与 §6 的 mount 存活时间劣化同一量级的抖动)。
整个 agentstress 窗口内 `getStandardItemAttributesForItem` 的 error 行只剩 9 条,而且是 `error:22`(EINVAL,
FSKit 传进来的不是 `WorldItem`),与本次改动无关;`error:70` 一条都没有。

### FSKit 前端冻结(2026-09-19)

`macos/fskit/` 到此**冻结为实验性回退路径**,不再继续投入:

- 冻结的理由是 T0.5 / macOS 27 复测的结论——元数据写 6.5–8% native、单次 XPC 往返 73.6µs、
  create+unlink 16 次往返,而且 27 的 `getItemForFH:` 退化还会再乘 3–12×。M1 主线是 C(native-root clonefile World)。
- **保留它的理由**:有两类场景 clonefile World 不覆盖,只有一个真正的挂载点能做到——
  (1) **超大仓库**,`clonefile()` 也要复制一整棵目录树的元数据,当 World 数量或树的规模大到
  连 COW 克隆都嫌贵时,挂载点的"零拷贝视图"仍然是唯一解;
  (2) **跨卷 / 跨文件系统**,`clonefile()` 只在同一个 APFS 卷内有效,backing 与工作区不同卷时用不了。
  这两种情况下 FSKit passthrough 虽然慢,但语义正确、已验证(agentstress 十个场景 correctness 全过)。
- **不做的事**:不迁移到 macOS 27 的 Handler API(`FSVolume.Handler` / `FSLookupItemResult` /
  `FSCreateItemResult` …)。按 §3.1 的 trace 估算,即使消掉全部 7 次 `getattr`,16.03 也只降到 ~9 次,
  仍然过不了 ≤6 的门槛,而单次往返在 27 上反而涨了 8%。等 Xcode 27 SDK 装好之后可以作为可选实验复核,
  但不作为路线依据。
- 冻结不等于不维护:上面三个收尾项就是把它修到"正确且安静"的状态,以后只做正确性修复,不做性能改造。

**macOS 27 上的 clonefile / native-root World 成本模型复测(2026-09-19)**:见
[`docs/CLONE_MODEL_MACOS27.md`](CLONE_MODEL_MACOS27.md) —— C 方案在 27 上 fork 快 23–44%(5 万文件 0.37s)、
克隆内速度 99–102% native、FSEvents 实测能给 O(changes) 的 diff(800 改动 → 800 条 file-level 路径,0 丢事件),
唯一退化是 COW 首写惩罚 392µs → 969µs;目录整 `clonefile` 比 Apple 推荐的 `copyfile(3)` 递归克隆快 15×,
代价只是源树写者 p99 10–11ms、0 失败,主路径继续用它。
