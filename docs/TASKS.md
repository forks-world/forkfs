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
  `~/Library/Containers/world.forks.fs.extension/Data/Library/Application Support/World/fs`。
  **订正(T2.3,2026-09-19):"CLI 默认同路径"是错的。** CLI 的默认 store 是 `~/Library/Application Support/World/fs`
  ——扩展因为被沙盒,`NSApplicationSupportDirectory` 解析到自己的 container 里,两者是两个不同的目录,
  而且沙盒**拒绝**扩展读 `~/Library/Application Support`。于是 `world fs mount W<n>` 会以 `WFS_E_FOREIGN_STORE`
  (`mount: POSIX error 1009`)失败,除非 `WORLD_STORE` 指向 container store。修法见下面的 T2.3。
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

**状态(2026-09-19 收尾)**:T1.1–T1.7 全部完成,T1.8 文档(arch.md 增补 + README)由架构师进行中。
门禁:`safety.sh` **93/93**;`ctest` 默认配置 2/2(core_test、diff_test)、`-DWFS_FSKIT=ON` 3/3(加 fskit_test);`check-deps.sh` 两个 build 目录只链系统库。
**arch.md §1 五条判据全部 PASS**(详见 [`docs/M1_RESULTS.md`](M1_RESULTS.md)):fork 延迟 pool 命中 p50 **8.6 / 9.0 / 9.7 ms**(1k/10k/50k,< 10 ms);
git/build **99%** native(> 0.5 s 的步骤最差)、agentstress 10 场景 99–147%;1000 个 idle World **3.40 GB / 建完 114 s / `fs list` 10 ms**;
存储 = 100 份真副本的 **13.6%**(100 个 50k World 各改 1%);diff O(changes),50k/800 改动全扫 **0.185 s**(`--events` 0.354 s)。
FSKit 前端**冻结**在 macOS 27 Handler API 上(`WorldVolumeH` 为默认,旧 `FSVolumeOperations` 以 marker 保留为回退),门槛**仍不达标**:create+unlink **11.04** 次往返(≤ 6)、单次往返 **74.64µs**(≤ 40µs),见 [`docs/FSKIT_HANDLER_API_MACOS27.md`](FSKIT_HANDLER_API_MACOS27.md)。

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
- [x] T1.5 pool:预克隆池(见下节"T1.5 pool 实现")
- [~] T1.6 safety 测试套件(P1–P14):P1/P2/P3/P4/P5/P6/P7/P8/P9/**P10**/P12/P13/P14 已覆盖
      (**93 条全过**,含 T1.5 pool 的 13 条;更细的精确集合断言在 `core/tests/diff_test.cpp`);
      P11 的"真实磁盘写满"仍只在 API 层验证
- [x] T1.7 基准:`scripts/bench/m1_criteria.sh`(六节,可 `--only N` 单独重跑)
      → [`docs/M1_RESULTS.md`](M1_RESULTS.md)。**arch.md §1 五条判据全部达成**:

      | 判据 | 目标 | 实测 | |
      |---|---|---|---|
      | fork 延迟 | < 10 ms p50 | pool 命中 **8.6 / 9.0 / 9.7 ms**(1k/10k/50k,含进程启动;地板 4.4 ms) | 达成 |
      | | | pool 空:0.026 / 0.110 / 0.505 s(裸 `clonefile` 0.006 / 0.082 / 0.455 s) | 参考 |
      | git/build | ≥ 90% native | 99–117%(> 0.5 s 的步骤最差 99%);agentstress 10 场景 99–147% | 达成 |
      | 1000 idle World | 可承受 | 建完 114 s、3.40 GB 物理(351 B/条目)、metadata.db 0.4 MB、`fs list` 10 ms / 7.8 MB RSS | 达成 |
      | 存储 ≈ divergence | — | 100 个 50k World + 各改 1% = 100 份真副本的 **13.6%**;改动部分放大 44×(4 KiB COW 块) | 达成 |
      | diff | O(changes) | 50k/800 改动:`--events` 0.354 s(只比对 800 个候选)、`--full --no-xattr` 0.185 s、默认 1.420 s | 达成 |
      | safety + ctest | 全过 | safety 93/93,ctest 2/2 | 达成 |

      三条值得记下来的结论:
      1. **pool 买的是延迟,不是吞吐**。1000 次背靠背 fork,pool 命中 1000/1000,但总时间只从 114 s 降到 102 s:
         那 1000 次 clonefile 还是这台机器做的,只是挪出了 fork 的关键路径。单次 p50 在这种打法下是 102 ms 而不是 9 ms。
         9 ms 是 agent 真实的场景——偶尔 fork 一次,克隆早被别人做完了。
      2. **最贵的一步是删**。`gc --retention 0` 真删 1000 个 1 万条目的 World = 1040 万次 unlink,**525 s**(≈50 µs/条目),
         比建它们贵 4.6×。要频繁回收就得把 gc 做成后台增量的(M2)。
      3. **事件路径这一轮 0.354 s,比 T1.3 那次的 0.087 s 慢 4×**,而且同一棵树逐次波动 0.14–0.49 s:
         波动在建流 + 等 fseventsd 水位标,与改动数无关。全扫则稳定。T1.3 把全扫定为默认的决定,本轮复核**不变**。

      ~~**仍然挂着的一条优化**:全扫的走树现在对每个文件都发 `listxattr(2)`(两边各一次,APFS 上约 10 µs),
      5 万文件的全扫因此从 0.185 s 涨到 1.420 s——这是全扫最大的一块成本。
      `core/src/platform_posix.cpp` 的 walker 应该换成 `getattrlistbulk(2)`,
      用 `ATTR_CMNEXT_EXT_FLAGS` 拿 `EF_NO_XATTRS`,**没有 xattr 的文件直接跳过 listxattr**
      (绝大多数文件都没有),顺带一次系统调用批量拿到 stat 信息。
      做完之后默认的全扫应该能逼近 `--no-xattr` 的数字,默认选路的阈值要跟着复核。~~
      → **T2.4 已做**,见下面的 M2 小节。结论与预期有一处重要偏差:macOS 27 给本机进程新建的
      **每一个**文件都盖 `com.apple.provenance`(且删不掉),所以合成 fixture 上 `EF_NO_XATTRS` 一次都不触发;
      真实树(96% 无 xattr)上默认全扫确实逼近了 `--no-xattr`。
- [ ] T1.8 文档:arch.md 增补章节、README

#### T1.5 pool 实现(2026-09-19)

**一个 pool 条目 = 一个"等着被领走的 World"**:`<store>/pool/S<n>/<uuid>/` 是快照的一次完整
`clonefile`(经同一把 `SnapGate`,根已经 chmod 回源树自己的 mode),**没有 `.world` 标记、
`worlds` 表里也没有行**。所以它还不是 World,外面也够不着它:P7 拒绝 store 里的任何路径
(这次连"路径还不存在"的情况也拒绝了 —— `<store>/pool/S1/mine` 以前会先撞上 ENOENT),
`world exec` 的 seatbelt profile 本来就 deny 整个 store。

**领取 = 原有 publish 顺序的尾巴**,条目扮演 `<target>.wfs-tmp` 的角色:
插入 CREATING 行 → 写 marker → `rename(2)` 到目标 → 提交 ACTIVE。
认领本身是一条 `BEGIN IMMEDIATE` 里的 `DELETE`,两个并发 fork 拿到的一定是不同条目;
中间任何一步失败就把条目放回去(`pool_return`)并退回当场 clone,调用方看不出区别。
条目按 **snapshot id + 该快照的 `created_at`** 建键:快照行不可变,所以对不上就说明这个 id
现在是另一个快照,永远不发放,`gc` 收走。

| 命令 | 作用 |
|---|---|
| `world fs pool fill S<n> [--count K]` | **补到** K 个 ready(不是加 K;默认 2) |
| `world fs pool status` | 每个快照的 ready / building / stale / 条目数 |
| `world fs pool drain S<n>\|--all` | 删掉这些预克隆,把空间还回去 |
| `world fs fork ... --no-pool` | 无视 pool,当场 clone(基准测试用) |

**自动补种**:命中的那次 fork 用 setsid + 双 fork 起一个游离的 `world fs pool fill S<n>`,
stdin 走 `/dev/null`,stdout/stderr 追加到 `<store>/logs/pool.log`,所以下一次 fork 还是快的,
而这一次不为它买单。`$WORLD_POOL_TOPUP` 改目标数(0 = 关掉自动补种,基准测试用);
store 级 `flock`(`<store>/locks/pool.lock`,**非阻塞**)保证同时只有一个 filler,
抢不到的直接返回 `WFS_E_POOL_BUSY` 而不是排队再克隆一棵。
**pool 是 opt-in 的**:不 `fill` 就一棵都不预克隆 —— 每个条目要占一棵树的 APFS 元数据
(~308 B/条目),不能替用户默认决定这笔开销。

`verify S<n>` 顺带检查等着的条目(还在不在、根 mtime 有没有比克隆那一刻新 —— 每条一次 `lstat`,
不走全树,否则 verify 会和 fill 一样贵);`gc` 收快照已经没了/换了身份的条目、filler 被杀留下的
`*.wfs-tmp`、以及 `<store>/pool` 下没有任何行认领的目录;`fs status` 多一行 pool 计数。

**顺带的两处优化**:
(a) `wfs_store_open()` 以前每次都执行一遍建表 DDL。改成用 `PRAGMA user_version` 记录 schema,
已经初始化过的 store 直接跳过 DDL(PRAGMA 仍然每次执行,`synchronous` 是 per-connection 的)。
命中一次 pool 总共才 ~9 ms,这一步省下来的 ~1 ms 不是小数。
(b) 命中后先问一句 `wfs_pool_filling()`(store 的 pool flock 探一下):已经有 filler 在跑就不再起第二个,
否则那个子进程除了撞锁退出什么也不干,而这次 fork 白付一次 ~4 ms 的进程启动。
(M1_RESULTS §3 的 pool 那一列是加 (b) 之前测的。)

数字见 [`docs/M1_RESULTS.md`](M1_RESULTS.md) §1。

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

### Handler API 实测(2026-09-19 晚,Command Line Tools 27.0 SDK 已装,真的用新 API 重写了一遍)

上一节说的"可选实验"当天就做了:`macos/fskit/WorldVolumeHandler.{h,mm}` 新增 `WorldVolumeH`,
实现 `FSVolumeHandler` / `FSVolumeXattrHandler` / `FSVolumeReadWriteHandler` /
`FSVolumeDataCacheHandler`,每个回复都用带 `FSItem.Attributes` 的结果对象,并把该类
`requestedAttributes` 要求的 14 个属性全部填满(运行时反射确认 21 个结果类要的是同一组
`0x3fff`,core 的 `wfs_attr` 本来就够)。冻结的 `WorldVolume` 一字未改,两个类共存在同一个
appex 里,靠 store 目录下的 marker 文件(`wfs_api_old` / `wfs_nodatacache`)**按挂载点**切换,
所以下表的"旧 API"列是同一天、同一台机器、同一个二进制、间隔几分钟测出来的真对照。
完整数据见 [`docs/FSKIT_HANDLER_API_MACOS27.md`](FSKIT_HANDLER_API_MACOS27.md)。

**门槛判定(两条仍然都不满足):**

| 门槛 | 阈值 | 26.6.2 | 27.0 旧 API | **27.0 Handler API** | 判定 |
|---|---|---|---|---|---|
| create+unlink 往返次数 | ≤ 6 | 16.06 | 16.04 | **11.04** | **FAIL**(超 1.84×) |
| 单次往返(lookup-miss) | ≤ 40µs | 68.1µs | 74.27µs | **74.64µs** | **FAIL**(超 1.87×) |
| (派生)元数据写 native% | 40–60% | 6.6% | 5.5% | **6.1%** | **FAIL** |

- **`getattr` 刷新彻底消失**:7.04 → 2.06 次/轮,而且剩下的 2 次全是对**父目录**的。
  `FSLookupItemResult` / `FSCreateItemResult` / `FSRemoveItemResult` 里带的属性内核确实缓存了。
  冷 `lstat` 从每文件 3.08 次往返(lookup 2.04 + getattr 1.04)降到 **2.04 次(纯 lookup)**。
- **`lookup` 3.00 和 provenance `getxattr` 2.00 一次没少。** 新发现:macOS 27 对一个**未缓存的名字
  固定发两次 lookup**(冷 lstat 5000 个不同名字 = 10201 次 lookup,两套 API 完全一样,
  把目录改名成 `*.noindex` 也一样),这一条约占 create+unlink 全部往返成本的 9%,不在模块 API 射程内。
- **`FSVolumeDataCacheHandler` 是净亏,必须 `dataCacheInhibited = YES`。**
  实测 open/close **每一次都真的发到扩展**(2000 次 open+close → 2051 open + 2051 close),
  **没有 deferred close、没有合并**;读写行为一点没变(冷读仍每个页缓存 miss 一次 `read`,热读零往返)。
  代价是 `open`+`close` 从 11.0µs 涨到 **141.0µs(13×)**、`ls -l` 50 项从 52 次往返涨到 104 次、
  create+unlink 从 11.04 涨到 13.04。关掉它等价于 M0 的 `openCloseInhibited=YES`(逐项实测相同)。
- **墙钟收益只有往返次数收益的三分之一**:次数 -31%,create+unlink 延迟只 -9.4%
  (1097.9 → 995.2µs)。原因是**被消掉的 getattr 是最便宜的往返**:旧 API 68.4µs/往返,
  Handler 90.1µs/往返 —— 省掉的 5 次每次只值约 20µs。
- 有收益的地方(Handler+noDC vs 旧 API):tmp+rename **-22.6%**、create+write+close+unlink **-10.5%**、
  冷 lstat **-12.4%**、并发 create+unlink 吞吐 **+9%(P=1)→ +26%(P=8)**;
  `realwork` 的 **"touch 头文件 + 增量编译" 1.533s → 0.433–0.522s(快 3×,24% → 68–82% native)**,
  把上一节 §7.2 唯一那个稳定复现的真实负载回归补回来了。
- 读路径、缓存命中路径**逐项不变**(stat_cached 0.92µs / pread 0.45µs / open+read+close 11.60µs,
  都是 native 的 105–108%),**单次 XPC 往返成本一动没动**(74.27 → 74.64µs)。
- 正确性:两套配置各跑一遍 `smoke.sh`(同一活挂载连跑两次)、readdir 完整性 100/1000/5000、
  `ls -fa` dot 项、exec + dlopen、git init/add/commit、硬链接兄弟 unlink、agentstress 十场景 —— **全过**;
  quarantine 隐藏仍然有效(新产物只有 `com.apple.provenance`)。

**订正上一节的两处数据(在 `4582150` 上复现不出来):**

- 上一节问题 2 的"写随 mount 存活时间劣化 3–12×":**50,000 次 create+unlink churn 之后,
  两套 API 都只变化 ±2%**(旧 API 1103→1129µs,Handler 1012→1003µs)。最可能的原因是
  `d1a2a3c` 把"删除后那次 getattr"从回 `ESTALE` 改成回属性快照,不再让 FSKit 攒住 `FSFileHandle`。
  **偶发的多倍尖峰仍在**(agentstress 三轮里 S4 一次 6.4→34.8s、S5 一次 24.4→75.0s),
  但微基准量不到,Handler API 没有解决。
- `MACOS27_MEASUREMENTS` §4.2 的 writebench 表(create 1242 / close 418 / open_trunc 832 /
  fsync 319 / unlink 1737 µs)是劣化态下测的:今天旧 API 是 **599 / 173 / 241 / 66 / 493**,
  Handler+noDC 是 **563 / 143 / 230 / 66 / 435**。由它推出的"几百个文件同时打开每个额外付 ~3ms"
  的反常结论同样需要带这个注脚。

**结论:T0.5 的 Go/No-Go 仍然不变,M1 维持 C(native-root clonefile World)主线。**
这一轮的额外价值是把"再等 FSKit 一版"的期待关掉了:属性缓存是新 API 里最被寄予厚望的一条路,
走完之后 create+unlink 仍有 11 次往返,剩下的每一次(2 次冷名字 lookup、1 次热 lookup、
2 次 provenance getxattr、create/remove/sync/reclaim 各 1 次、2 次父目录 getattr)都在内核里,
没有一条是模块能动的;单次往返 74.6µs 更是 27 相对 26.6.2 涨了 8%(Apple 自己的 msdos 同向涨)。
Handler 版作为冻结前端的新默认留在仓库里(旧 `FSVolumeOperations` 在 27 上已 deprecated),
默认 `dataCacheInhibited = YES`。

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

### M2 — 运维与规模(2026-09-19 用户确认按此顺序)

> **M2 状态(2026-09-19 收口,macOS 27.0 / M1 Mac mini):T2.1–T2.5 全部完成,T2.6 按用户决定推迟到 Linux 机器。**
>
> | 任务 | 头条数字 |
> |---|---|
> | T2.1 后台增量 gc | 1000 个 10.4k 条目的 World 在后台排空 663.6 s(真删 339.9 s,30,601 条目/s,比 M1 前台同步 gc 的 525.2 s / 19,800 条目/s **快 1.55×**);期间 200 次并发 fork p50 **0.200 s**,空闲时 0.202 s → **−1.0%**(P16 目标是不超过 ~10%);`discard` 9.1 → 11.2 ms |
> | T2.2 `discard S<n>` + 对账 | 快照走与 World 相同的 trash/保留期/后台删除;有 ACTIVE World 引用时 `--force` 也拒绝;`gc --reconcile` 只有在"行还在、树没了"时才标 DEAD |
> | T2.3 store 路径 | store 路径写进 `.world` marker(`store_path`),沙盒 appex 从挂载源根目录读它;CLI 默认 store 与扩展 container store **是两个目录**,mount 前自查并给可执行建议(不再是 `POSIX error 1009`) |
> | T2.4 `getattrlistbulk(2)` + `EF_NO_XATTRS` | walker 快 1.20–1.23×(syscall 省 6×);50k 默认全扫 1.40 → 0.96 s,真实树 0.21 → 0.17 s |
> | T2.4 后续:忽略 `com.apple.provenance` | 50k 默认全扫 **0.96 → 0.231 s**(`--all-xattrs` 1.303 s = 老默认,`--no-xattr` 0.126 s);10k **0.157 → 0.047 s**;整条 CLI 计时的 §5 复核 1.420 → **0.267 s** |
> | T2.5 树内硬链接 | 1000 对硬链接的树 fork +274 ms = **0.27 ms/条**(4 线程);pool 命中 +2.6 ms(重放在填充时做完) |
> | P17 store 完整性 | 有树没数据库 → `WFS_E_STORE_DAMAGED`,绝不静默重建 |
>
> 验收:`ctest` 2/2(WFS_FSKIT=OFF)与 3/3(ON),两个构建目录干净重建、`check-deps.sh` 全绿;
> `safety.sh` **142 passed, 0 failed**;`diff_test` 连跑 10 次 10/10;
> `m1_criteria.sh --only 1` fork 命中 p50 9.2/9.4/9.5 ms(M1 是 8.6/9.0/9.7,门槛 <10 ms,无退化);
> 真实 FSKit 挂载 + `smoke.sh` **ALL OK**,卸载后无残留挂载。

- [x] T2.1 后台增量 gc:discard 保持毫秒级,物理删除由后台分批完成(1000 个 10k 树的 World 实测 gc 525s)
- [x] T2.2 `discard S<n>` + 悬空快照对账;快照有活 World/池条目引用时拒绝
- [x] T2.3 store 路径统一:沙盒 appex 与 CLI 默认 store 不同(container vs ~/Library/Application Support),`world fs mount` 把 store 路径写进 `.world` marker 传给扩展;修正任务板中"CLI 默认同路径"
- [x] T2.4 diff 扫描改 `getattrlistbulk` + `EF_NO_XATTRS`(2026-09-19,分支 `m2/t2.4-bulk-walker`;真实树默认全扫 0.21 → 0.17 s,合成 50k 1.40 → 0.96 s)
  - [x] T2.4 后续:`com.apple.provenance` 不再参与比较(架构决定,见下;合成 50k 0.96 → **0.231 s**)
- [x] P17 store 完整性:有树但 `metadata.db` 没了 → `WFS_E_STORE_DAMAGED`,拒绝并说明出路
- [x] T2.5 fork 后按 (dev, ino) 恢复树内硬链接(P9 从警告变为修复;实测 0.27 ms/条,pool 命中不受影响)
- [ ] T2.6 Linux 平台层:overlayfs + mount namespace(fork O(1)、upper 目录即 changed-set)——**不在这台 Mac 上做**(用户决定),等 Linux 机器

#### T2.1 后台增量 gc(2026-09-19 实现 + 实测)

**为什么**:`discard` 本来就是一次 rename,毫秒级;贵的是它欠下的那笔 unlink。M1 实测
1000 个 10,400 条目的 World = **1040 万次 unlink、525 s**(M1_RESULTS §3),是建它们的 4.6 倍。
这笔账不能挂在任何一条命令的关键路径上,也不能跟前台抢盘(P16)。

**设计**(实现在 `core/src/world.cpp` 的 `wfs_gc_ex` / `wfs_gc_status` / `wfs_gc_pending`,
调度在 `cli/main.cpp`):

- **崩溃安全先于一切**:真删之前先 `rename(<trash>/X, <trash>/X.deleting)`。这一步是原子的,
  之后无论发生什么,那棵树都"一眼不是 World"。被 kill 的 worker 因此绝不会留下一棵
  *看起来可以 restore、实际少了一半文件* 的树:`restore` 以 `WFS_E_TRASH_DELETING` 拒绝,
  下一次唤醒把 `*.deleting` **排在最前面**删完(不看保留期)。
- **一次唤醒 = 一批**:`WORLD_GC_BATCH`(默认 64 个 trash 条目)或 `WORLD_GC_BATCH_SECS`
  (默认 2 s)先到者为准,然后进程退出。还有活就**自己起一个新的游离后继进程**
  (`spawn_detached`,和 T1.5 的 pool filler 同一套 setsid + double fork),
  所以没人再敲命令 trash 也会排空;`discard` / `fork` / `gc` 发现有到期的活而没人干时也会起一个。
- **全店一个 worker**:`<store>/locks/gc.lock` 上的非阻塞 `flock`。这个锁文件**永远不 unlink**——
  `flock` 锁的是 inode,删了再建会让两个 worker 同时"持有"同一把锁。worker 每删完一条就把
  `pid/start/done/remaining` 重写进这个文件,`gc --status` 因此不需要任何 IPC 就能报进度。
- **删除用 4 线程**(`fs_remove_tree_parallel`,复用 clone 用的那个并行 walker):文件在并行阶段
  unlink,目录在它本来就有的"最深优先"串行尾巴里 rmdir。任何一步出错都退回单线程 `rm_rec`,
  宁可慢也不留半棵树。
- **前台命令的语义**:`world fs gc` 只做便宜的一半(半成品树、过期 seatbelt profile、死快照的
  pool 条目、对账报告)然后把 trash 交给 worker;`world fs gc --now` 是同步跑完的老行为
  (safety.sh 的 P4 用例改用它);`--status` 报 trash 大小与 worker;`--reconcile` 见 T2.2。

**4 线程到底值不值 / 怎么才不抢前台**(30 个 10.4k World 一轮,M1 Mac mini / 27.0,
fork p50 = 并发跑 `world fs fork --no-pool` 的中位数):

| 线程 | io policy / 占空比 | drain | 条目/s | 并发 fork p50 | 相对空闲 |
|---|---|---|---|---|---|
| 1 | throttle,连续 | 15.6 s | 19,941 | 0.161 s | **+15%** |
| 2 | throttle,连续 | 11.2 s | 27,880 | 0.163 s | **+16%** |
| 3 | throttle,连续 | 9.0 s | 34,635 | 0.211 s | **+51%** |
| 4 | normal,连续 | 8.4 s | 37,071 | 0.221 s | **+57%** |
| 4 | throttle,连续 | 8.4 s | 37,314 | 0.221 s | +57% |
| 4 | utility,连续 | 8.4 s | 37,199 | 0.221 s | +57% |
| **4** | **干 2 s / 停 2 s** | 16.1 s | 19,332 | 0.145 s | **+5%** |

- **4 线程确实有用**:37k 条目/s,比 M1 单线程 `rm_rec` 的 19.8k(1040 万 / 525 s)快 **1.9×**,
  和克隆那边"4 线程是 APFS 元数据事务甜点"的结论一致。
- **`setiopolicy_np()` 一点用都没有**(normal / utility / throttle 三档逐项相同)。
  瓶颈不是磁盘带宽也不是 CPU,是 APFS 的元数据事务——Spotlight/Time Machine 那套 IO 节流对它无效。
- **真正管用的是占空比**。连续跑时哪怕只用 1 个线程,并发 fork 也要慢 15%(那是"同一个卷上
  有人在 unlink"的地板);**干 2 s 停 2 s** 把争抢窗口砍掉一半,前台代价降到 ~5%,
  代价是 drain 大约翻倍。背景工作,等得起——于是 `WORLD_GC_BATCH_SECS=2` / `WORLD_GC_PAUSE_MS=2000`
  成了默认值。

**1000 个 World 的完整一轮**(和 M1_RESULTS §3 同一棵 10,400 条目的树,同一台机器):

| | M1(同步 `gc`) | T2.1(后台 collector) |
|---|---|---|
| 1000 次 fork(`--no-pool`) | 113.7 s | 122.6 s |
| 1000 次 `discard` | 9.1 s(9.1 ms/次) | **11.2 s(11.2 ms/次)** |
| trash 里的内容 | — | 1000 条目 / 1040 万 tree entries / ~3.0 GB(估) |
| 真删的墙钟 | **525.2 s**(前台,命令一直卡着) | **663.6 s**(后台;158 次唤醒,其中真正在删 339.9 s,其余是每轮之间 2 s 的故意停顿) |
| worker CPU | —(全在前台) | 850.7 s(干活时约 2.5 个核),共 158 次唤醒 |
| 期间并发 fork p50 | —(不可能并发) | 0.200 s,空闲时 0.202 s → **-1.0%** |

- `discard` 从 9.1 → 11.2 ms:多出来的 2 ms 是"顺手看一眼 trash 里有没有到期的活"。
  第一版用 `wfs_gc_status()` 做这件事,它要把 trash 里每个目录和每个 trashed 行比对一遍
  (trash 一千条时是 100 万次 strcmp),于是换成 `wfs_gc_pending()`:两条带索引的 count
  加一次在第一个 `*.deleting` 就停的 readdir。
- **P16 达成**:collector 全程在跑的情况下,200 次并发 fork 的 p50 是 **0.200 s**,
  空闲时是 **0.202 s** —— **-1.0%**,在噪声里(目标是"不超过 ~10%")。
- 真正在删的那 339.9 s 里是 **30,601 条目/s**(比上面 30 个 World 那轮的 37k 低,
  因为每次唤醒都要重新开 store、扫一遍 trash——1000 条目的 trash 扫描不是免费的)。
  对比 M1 同步 `gc` 的 1040 万 / 525.2 s = 19,800 条目/s,**快 1.55×**;
  而这 663.6 s 里没有任何一条命令在等它。

#### T2.2 `discard S<n>` 与对账(2026-09-19)

- `wfs_snapshot_discard(store, id, force)` + `world fs discard S<n>`。
  **有 ACTIVE World 从它 fork 出来就拒绝,`--force` 也不行**——`diff` 和 `verify` 都要拿它当基线,
  绝不能让一个活着的 World 失去来源。拒绝信息会把是哪几个 World 列出来。
  有 pool 条目时同样拒绝,`--force` 先 drain 再删(pool 条目只是预克隆,丢了只是再克隆一次)。
- trash 里的 World **不算**引用(它们本来就在路上了);但它之后 `restore` 会以
  `WFS_E_SOURCE_GONE` 拒绝,并说明"恢复出来的 World 没有基线可 diff / verify"。
- 快照走和 World 完全相同的 trash / 保留期 / 后台删除路径。搬的是 `<store>/snapshots/S<n>`
  整个目录(`manifest` 要跟着走),**gate 一路关着**——rename 不需要进树里——
  由 deleter 在真删时 `chmod 0700` 打开。
- **对账**:`world fs gc` 和 `fs status` 检查每个 ACTIVE 行的树是否还在
  (快照看 `snapshots.path`,World 看 `worlds.path`,一行一次 `stat`),有就报告;
  `gc --reconcile` 才真的把它们标成 DEAD,并让 `pool_collect` 顺手清掉死快照的 pool 条目。
  **默认只报不动**,因为一个只是被 `mv` 走的 World 从 store 这边看起来一模一样,
  那种情况的正解是 `world fs verify <新路径>`(P1),不是标死。

**两个遗留 store 的实际处理**(用户点名的那两个 `m1final` 快照):

| store | 里面有什么 | `gc --reconcile` 的结论 | 处理 |
|---|---|---|---|
| `~/Library/Application Support/World/fs` | `S1 m1final`(1040 条目),0 个活 World | **没有悬空行**:`snapshots/S1/root` 还在,只是没人引用 | `discard S1` → `gc --reconcile --now --retention 0` 删掉;store 现在是空的 |
| `~/Library/Containers/world.forks.fs.extension/.../World/fs` | `S1 s27` + `W1/W2/W3`,以及 `S2 m1final` | 同上,0 悬空 | 只 `discard S2` → 同一条命令删掉;**`S1 s27` 和 `W1/W2/W3` 一动没动** |

**订正任务描述**:这两个快照并不是"悬空"(dangling)——它们的目录都还在磁盘上,
所以 `gc --reconcile` 看它们是完全正常的行,什么也不会做。它们只是**没人引用的遗留快照**,
对应的新命令是 `discard S<n>`(T2.2),不是 `--reconcile`。`--reconcile` 的真实用例
(行还在、树没了)由 safety.sh 的两个用例覆盖。

#### T2.3 store 路径(2026-09-19)

两条硬事实决定了一切:

1. **`-o` 选项到不了 macOS 27 的 FSKit 模块**(options 数组是空的)。T0.6 已经踩过一次。
2. **appex 必须沙盒**(pkd 直接丢弃非沙盒 appex),于是它只能碰自己的 container 和那个
   security-scoped 的挂载源;`~/Library/Application Support` 被拒,而且
   `NSApplicationSupportDirectory` 解析进 container —— 扩展的"默认 store"和 CLI 的
   **是两个目录**。这就是 `world fs mount W<n>` 报 `POSIX error 1009`(`WFS_E_FOREIGN_STORE`)的原因。

修法:**store 的路径写进 `.world` marker**(新字段 `store_path`),扩展从挂载源根目录的 marker 里读它
——那个根目录正是它被交付的 resource,是唯一一条一定到得了的通道。没有 `store_path` 的旧 World
退回 container 默认值。打不开时日志里说清楚是哪个 store、为什么(沙盒),而不是漏一个裸 errno 出去。
`world fs mount` 自己先查一遍,store 不在 container 里就直接以可执行的建议拒绝。

`wfs_marker_store_path()` 是为此加的一个不需要先打开 store 的读 marker 接口。

**真挂载验收(2026-09-19,`scripts/bundle.sh Release` 之后):**

| | store | 结果 |
|---|---|---|
| A | CLI 默认 `~/Library/Application Support/World/fs` | `world fs mount W2 <mnt>` **exit 3**,打印"扩展被沙盒、读不到这个 store、`-o` 到不了模块",并给出 `WORLD_STORE=<container>` 的可执行建议——不再是 `POSIX error 1009` |
| B | container store | `.world` 里有 `store_path`;`mount` exit 0,`mount(8)` 里能看到 `(worldfs, local, ..., fskit)`;`scripts/smoke.sh` **ALL OK**(read/readdir/stat/write/append/truncate/mkdir/rename/unlink/rmdir/symlink/hardlink/chmod/xattr/mmap/fsync);`umount` 后 `mount | grep worldfs` 为空 |

收尾:两个 store 都被恢复成测试前的样子(container store 里 `S1 s27` + `W1/W2/W3` 原封不动),
结束时没有任何 worldfs 挂载。

#### T2.4 diff 扫描改 `getattrlistbulk(2)` + `EF_NO_XATTRS`(2026-09-19,M1 / macOS 27.0)

分支 `m2/t2.4-bulk-walker`。改的是 `core/src/platform_darwin.cpp`(新增 `fs_bulk_dir` / `fs_lstat_xattr`)、
`core/src/platform_posix.cpp`(walker)、`core/src/diff.cpp`、`core/src/internal.h`、`core/tests/diff_test.cpp`。
`fs_walk_tree` 的签名**没变**(它现在是 `fs_walk_tree_ex` 的一层包装),所以 clone / protect / count / verify
一行都没改就跟着快了。

**要的属性**(`FSOPT_ATTR_CMN_EXTENDED`,**不带** `FSOPT_PACK_INVAL_ATTRS`):

```
commonattr  RETURNED_ATTRS | NAME | DEVID | OBJTYPE | CRTIME | MODTIME | CHGTIME | ACCTIME
            | OWNERID | GRPID | ACCESSMASK | FLAGS | FILEID
fileattr    LINKCOUNT | ALLOCSIZE | DATALENGTH
forkattr    ATTR_CMNEXT_EXT_FLAGS          ← EF_NO_XATTRS 从这里来
```

四条实测结论,都是先写探针再定的:

1. **`ATTR_CMN_ACCESSMASK` 带 `S_IFMT`**:目录回来是 `0o41755`、符号链接 `0o120755`、FIFO `0o10644`。
   所以类型和权限一次拿全,`OBJTYPE` 只当交叉校验(两者不一致就整条退回 `fstatat`)。
2. **目录不走 bulk**:`ATTR_DIR_LINKCOUNT` 是"指向该目录的硬链接数"(实测 1,而 `lstat` 的 `st_nlink` 是 3),
   `ATTR_DIR_DATALENGTH` 也不是 `st_size`。目录只占一棵树 ~1%,而且 walker 本来就要把它打开,
   所以目录多花一次 `fstatat(2)`,它的 `struct stat` 与从前**逐字节相同**——这很重要,
   `Manifest::line` 会把它写进快照清单,`snapshot verify` 要读回来比。
   实测:用旧二进制建的 store 用新二进制 `fs verify` 干净通过,反之亦然。
3. **不要 `FSOPT_PACK_INVAL_ATTRS`**:带上它时,文件系统答不出的属性**仍然**会把
   `ATTR_CMN_RETURNED_ATTRS` 的位置上,只是塞个 0 进去——那就是一个悄悄错掉的 mtime 或 size。
   不带它,位是清的,解包器当场发现,该条目退回 `fstatat`/`lstat`。
   实测:devfs 上 353 条目里 352 条在不带标志时报告 `EXT_FLAGS` **缺失**,带标志时报告"有,值为 0";
   APFS 上带不带两者 144,133 条目逐字段相同。
4. **`EF_NO_XATTRS` 只会"否认",不会"承认"**:137,665 条目(/Applications + /usr/share)里 117,168 条带这个位,
   其中 `listxattr(2)` 返回非空的有 **0** 条;另有 5,809 条没带这个位但 `listxattr` 是空的——
   也就是说它保守的方向恰好是安全的那一侧,漏判只多花一次 `listxattr`,绝不会给出错的答案。

**回退逻辑**:`fs_bulk_dir` 在**交出第一条目之前**失败一律返回 `-ENOTSUP`,walker 收到后
`lseek(fd,0)` + `fdopendir` 重走 `readdir(3)` + `fstatat(2)` 老路——因此不可能重复上报。
交出第一条之后再失败就是真错误,按 `-errno` 中止整个 walk(和老 walker 一样)。
非 Darwin 平台 `fs_bulk_dir` 直接是 `-ENOTSUP` 的桩,走的就是老路。

**顺带改掉的一处**:`xattr_equal()` 从前对每个 name 发 4 次 `getxattr`(两边各"问大小 + 取值"),
现在直接用 1 KiB 栈缓冲取值,一边一次,`ERANGE` 才退回老写法。实测 55 µs/对 → 35 µs/对。

**单次系统调用成本(M1 / 27.0 / APFS,热,60,001 个路径)**:

| 调用 | µs |
|---|---|
| `lstat(2)` | 1.41 |
| `getattrlist(2)`(上面那张表) | 1.78 |
| `lstat` + `listxattr` | 2.56 |
| `listxattr(2)` | 2.1 |
| **`getxattr(2)`** | **14.0** |
| `getattrlistbulk(2)`,摊到每条目 | 0.23 |

`getxattr` 才是大头,`listxattr` 不是——旧注释里"listxattr 约 10 µs"这个数不准,
真正贵的是它后面跟着的 4 次 `getxattr`。

**walker 本身**(`fs_count_entries`,4 线程,热,best of 7):

| 树 | 旧 | 新 | 倍数 |
|---|---|---|---|
| 平铺 50,500(500 目录 × 100 文件) | 1,490,597 条目/s | **1,793,834** | 1.20× |
| Keynote.app 克隆出的 World,37,174 条目 | 948,851 | **1,168,038** | 1.23× |
| `/Applications`,134,641 条目 / 10,302 目录 | 846,766 | **1,034,563** | 1.22× |

syscall 省了 6×,总时间只省 1.2×:walker 自己那份(每条目两个 `String`、队列、锁)现在才是大头。
要再往下压得先把路径拼接改成不分配的写法,那是另一件事。

**diff 实测。先说一条会影响所有数字的环境事实**:macOS 27 给本机进程新建的**每一个**文件盖
`com.apple.provenance`,`removexattr` 删不掉(实测 `xattr -d` 静默失败)。所以
**`diff_test` 造的合成 fixture 里没有任何一个文件能拿到 `EF_NO_XATTRS`**——合成树上这条捷径一次都不触发,
省下来的全是 walker 和 `getxattr` 那两笔。而 `clonefile(2)` 会原样保留"没有 xattr"这个状态
(实测源树 35,548/37,072 带 `EF_NO_XATTRS`,克隆出来一模一样),所以真实树上它是实打实生效的。

50k / 800 改动(`WFS_DIFF_BENCH=1 diff_test`,库内计时,合成 fixture,**每个文件都有 provenance**):

| 路径 | T1.7 | 本次 base(297fe5a) | 本次 new | |
|---|---|---|---|---|
| 默认(全扫 + xattr) | 1.420 s | 1.402–1.410 s | **0.956–0.986 s** | 1.45× |
| `--full --no-xattr` | 0.185 s | 0.165–0.166 s | **0.129–0.139 s** | 1.25× |
| `--events` | 0.354 s | 0.048–0.122 s | 0.044–0.086 s | — |

10k / 850 改动(同上,10 次里取最好):

| 路径 | base | new | |
|---|---|---|---|
| `--full` | 0.232 s | **0.158 s** | 1.47× |
| `--full --no-xattr` | 0.034 s | **0.028 s** | 1.21× |

**真实树**(`clonefile /Applications/Keynote.app` 当源,37,174 条目、94% 无 xattr,
同样 500 改 + 200 增 + 100 删),整条 CLI 计时 best of 3:

| 路径 | base | new |
|---|---|---|
| 默认(全扫 + xattr) | 0.210 s | **0.171–0.191 s** |
| `--full --no-xattr` | 0.167 s | **0.144–0.158 s** |

**默认全扫在真实树上已经和 `--no-xattr` 同一个量级**(0.17 vs 0.14,差的是那 6% 真有 xattr 的文件),
T1.7 里 7× 的那道口子在真实树上合上了。合成 fixture 上还剩 7×,原因只有一个:provenance。

`fs verify S1`(37,073 条目,与 walker 共用):0.180 s → **0.170 s**;
它的大头是按清单逐行 `lstat`,不是 walker。

**测试**:`diff_test` 原有断言一字未改、全部精确集合通过,连跑 10 次 10/10;
新增 `xattr_shortcut()` 一节,把四种组合摆在一棵树里——两边都没有、两边都有(值不同)、
只有 World 有、**只有快照有**。最后一种是"只看 World 那一侧就跳过"的写法一定会漏的那种,
现在 `--full` 与 `--events` 两条路都报 `T`,`--no-xattr` 一条都不报。
`core_test` 通过,`safety.sh` 93/93,`check-deps.sh` 三个二进制全绿。

**留给以后的两条**:
1. ~~`com.apple.provenance` 是内核记的"谁建的这个文件",不是用户数据,却要每文件 2 次 `listxattr` + 2 次
   `getxattr`(28 µs)才能确认它两边一样。把它(以及别的纯系统 xattr)排除在比较之外能把合成树上剩下的
   0.97 s 直接打掉,但那是语义变更,得单独提。~~
   → **已提、已做**,见下面"T2.4 后续"一节:默认 0.96 → 0.231 s,`--all-xattrs` 保留老行为。
2. walker 每条目两次 `String` 分配,见上面 1.2× 那一段。

#### T2.5 树内硬链接的恢复(2026-09-19 实现 + 实测)

`clonefile(dir)` 和所有替代方案一样会把树内硬链接打断(`docs/CLONE_MODEL_MACOS27.md` §11):
nlink 掉到 1,每个名字变成一个独立克隆。对 pnpm/uv 的 store、git 的 pack、cargo 的 target 缓存
来说这是"两份文件"而不是"一份文件两个名字",而且往任一个名字写一下,存储就默默地分叉了。
M1 只数了个数并警告(P9),T2.5 把它修掉。

**做法**(新文件 `core/src/hardlinks.{h,cpp}`,没有碰任何 walker):

1. **扫**:init / checkpoint 本来就要走一趟源树数条目(P11 要条目数),这趟顺带把
   `S_ISREG && nlink>1` 的条目按 (dev, ino) 分组。名字全在树内的组(组内名字数 == nlink)
   可以重建;有名字在树外的组重建不了(克隆里没有可以链接的对象),只计数。
   目录天生 nlink>1、symlink 用 `lstat` 看,两者都不是候选。
2. **清单**:组写在快照 `manifest` 末尾,**纯增量格式**——
   `#hl 1 <组> <名字> <树外组> <树外名字>` 加每个名字一行 `hl <组号> <nlink> <相对路径>`。
   老的 manifest 读者(`wfs_snapshot_verify`)要求 `line[1] == ' '` 才解析,`#hl` 和 `hl ` 都不满足,
   一律被跳过,所以 `verify`、`checked`、`extra` 全都不受影响。转义沿用清单自己的(`\\`、`\n`)。
   另加两列 `snapshots.hl_groups` / `hl_external`(schema rev 2 的 additive ALTER):
   `hl_groups == 0` 时 fork 连 manifest 都不用读。
3. **重放**:每一次克隆——fork、checkpoint、pool 填充——在 **rename 之前**(`--hard` 的
   unprotect 之后)按组重放:第一个存在的名字是正身,其余 `link` 到正身的临时名再 `rename` 覆盖。
   用 `link`+`rename` 而不是 `unlink`+`link`:名字一刻也不消失,link 失败也丢不了文件;
   崩在中间只留一棵 `.wfs-tmp`(P8)。组之间互不相干,所以 4 线程跑(APFS 元数据事务的老规矩)。
4. **不动的东西**:名字在克隆里不存在(活 World 在扫和克隆之间被改了)→ 跳过,容忍 ENOENT;
   大小或 mtime 和正身对不上(名字在这中间被换掉了)→ 跳过,绝不 unlink 别人的数据。
5. **从活 World fork**:活树没有清单,用**来源快照的组当候选**,重放前逐个名字在活树上 `lstat`,
   要求仍是同一个 inode,否则整组跳过。代价是每个名字一次 `lstat`,不是一趟 walk。
   换来的缺口是:agent 在 World 里**新建**的硬链接不会被带到它的 fork 里(找它们只能全树扫);
   `checkpoint` 会重新扫源树,所以走一次 checkpoint 就记上了。
6. **pool**:池条目也是克隆,重放放在**填充时**(后台 filler 里),所以领取仍然是"写 marker + rename"的 O(1)。

**实测**(M1 Mac mini,macOS 27.0;两棵除了硬链接以外完全一样的树,各 11 100 个条目、100 个目录,
其中 1000 对硬链接 = 2000 个名字共享 1000 个 inode;对照组是同样 11 100 个各自独立的文件):

| | 对照(无硬链接) | 1000 对硬链接 | 差 |
|---|---|---|---|
| `init`(扫 + clone + 重放 + manifest) | 197.6 ms | 462.5 ms | **+265 ms** |
| `fork --no-pool` | 120.7 ms | 395.0 ms | **+274 ms** |
| `fork`(pool 命中) | 9.0 ms | 11.6 ms | +2.6 ms(重放在填充时做完了) |

- **0.27 ms/条**(4 线程)。单线程实测 `link`+`rename` 0.62 ms/条、`unlink`+`link` 0.32 ms/条
  ——`rename` 比 `unlink` 贵一倍,但它买到的是"名字不消失",所以留着它,靠 4 线程(2.4×)补回来。
- **清单读**:11 100 条目的 manifest(528 KB)2.1 ms,50 000 条目的(2.2 MB)也是 2.1 ms
  ——非 `hl ` 行三个字节就被拒掉,整趟是 memcpy 速度;而且 `hl_groups == 0` 时根本不读。
- **清单变大**:2000 个名字 = 41.8 KB(487.8 KB → 528.4 KB,+8%),一个名字一行。
- **一对硬链接的树**:fork 相对同样大小的对照树看不出差别(<1 ms),固定开销是噪声级。

**测试**:`core_test` 新增一整节(3 个组:2/3/5 个名字,外加一个有名字在树外的组)——
snapshot 行的 `hl_groups`/`hl_external`、fork 后的 `st_ino` 相同与 nlink 正确、
穿过一个名字写另一个名字看得见、源树不受影响、`verify` 不被新行干扰、checkpoint 的重放、
pool 命中的重放(`fr.hardlinks == 0`,因为填充时就做完了)、从活 World fork 时被破坏的组会被跳过。
`safety.sh` 新增 5 条 CLI 级用例(`ln` → init → fork → `stat -f %i` 相同、写穿、pool 路径、
树外组只留独立副本、init 的措辞),全套 **135 passed, 0 failed**(T2.3 时是 130)。

#### T2.4 后续:`com.apple.provenance` 退出比较(2026-09-19,架构决定)

T2.4 结尾留的第一条"以后再说"在 M2 收口时被采纳了。**这是语义变更,不是优化**,所以单独记:

**事实**(实测,macOS 27.0):

1. 本机进程**新建的每一个文件**都被盖 `com.apple.provenance`(11 字节)。
2. **删不掉**:`removexattr` 和 `setxattr` 对这个名字都返回 0 而**什么也没改**(`xattr -d` 同样静默)。
3. 盖章的时机不只是创建:**`rename(2)` 也算**——把一个没盖章的文件 `mv` 进自己的目录,它就被盖上了。
4. **只有整目录 `clonefile(2)` 原样保留"没有 xattr"这个状态**;逐文件 `clonefile` 不保留(那个克隆是本进程创建的文件,照盖)。
   这条同时解释了 T2.4 里"真实树 94% 命中 `EF_NO_XATTRS`、合成树一次都不命中"的差别:真实树是整目录克隆来的。
5. 本机上它的值是常数(同一台机器、不同签名标识、sandbox-exec 里、shell 重定向,取到的 11 字节完全一样)。

它是**内核记的"哪个 app 建了这个文件"**,不是工作区状态。于是:

- **默认不比较它**,并且**在"要不要比较"的判断里也不算数**:`EF_NO_XATTRS` 没置位时,先 `listxattr` 拿名字、
  把 provenance 滤掉再判——**只剩 provenance 的文件等同于"一个 xattr 都没有"**,和文件系统自己置了位一样。
- `world fs diff --all-xattrs`(`WFS_DIFF_ALL_XATTRS`)把它放回来。
- **`com.apple.quarantine` 和其他所有名字照常比较**——那些是工作区身上发生的事。

**实测**(库内计时,合成 fixture,每个文件都有 provenance,所以捷径一次都不触发;best of 3):

| `diff --full` | 50k / 800 改动 | 10k / 850 改动 |
|---|---|---|
| 本次之前(= 现在的 `--all-xattrs`) | 1.303–1.310 s | 0.157–0.159 s |
| **默认(忽略 provenance)** | **0.231–0.240 s** | **0.047–0.048 s** |
| `--no-xattr` | 0.123–0.130 s | 0.026–0.027 s |

确认这一个内核写的属性两边一样,占了默认全扫的 **5/6**:每个"其他都一样"的文件 2 次 `listxattr` + 4 次 `getxattr`
(`getxattr` 14 µs 才是大头)。剩下高出 `--no-xattr` 的那 0.11 s,是还得问一次名字的那 2 次 `listxattr`。
整条 CLI 计时的复核(`m1_criteria.sh --only 5`,50k/800):默认 1.420 → **0.267 s**,`--no-xattr` 0.185 → 0.154 s。

**测试**(`diff_test` 新增 `provenance` 一节,5 条断言)。要造一个"只差 provenance"的差异,得有一个内核没盖章的文件,
而按上面第 4 条,唯一的来路是整目录 `clonefile`:fixture 在候选目录里找一个自己没有任何 xattr 的小目录
(`~/Library`、`/Library`、`/Applications`,找不到就跳过并说明),整目录克隆**直接克隆进源树**(不能 `mv`,见第 3 条),
只留那一个文件;然后在 World 里用一个字节相同的新文件覆盖它并把 mode/时间戳放回去——两边 `stat(2)` 看到的一切都相同,
唯一的差别就是内核刚盖上的那个 provenance。结果:默认**不报**,`--all-xattrs` 报 `T`,`--no-xattr` 一条不报,
全扫和候选(FSEvents)两条路一致。同一棵树里给邻居文件加一个 `com.apple.quarantine`,两种模式都报 `T`
——证明这不是"忽略 `com.apple.*`"。

#### P17 store 完整性(2026-09-19)

**风险**:store 目录还在、`snapshots/` 里还有树,但 `metadata.db` 没了(误删、备份还原了半套、磁盘错误)。
从前 `wfs_store_open` 会**静默新建一个空库**——id 是从库里发的,新库再发一次 1,下一次 `init` 就往已经在磁盘上的
`snapshots/S1` 上写。

**做法**:`wfs_store_open` 在创建任何东西之前先判:`metadata.db` 不存在 / 是空文件 / 读不了 /
打开后连 pragma 都执行不过(根本不是数据库),**而** `snapshots/`、`trash/`、`pool/` 里还有条目
→ `WFS_E_STORE_DAMAGED`(-1017)。检测只是对这三个目录各一次 readdir,**快照的 gate 一路关着也照样发现**
(不需要进 `S<n>/root`)。空目录不算损坏,那是新 store。

**CLI 把话说全**:这些树就是那个数据库的索引、id 会撞车、**`gc --reconcile` 帮不上忙**
(它要读数据库才知道哪些行的树没了,而这里没的正是数据库),出路只有两条——从备份恢复 `metadata.db`,
或者把整个目录挪开(`mv <store> <store>.damaged`)重开一个。**不提供 `store repair --scan`**:
从磁盘上的树反推出 id、名字、来源快照、fork 时的 FSEvents 游标是猜,猜错比拒绝更坏。

**测试**:`core_test` 一节(拒绝、拒绝之后**没有**留下新库、"文件在但不是数据库"同样拒绝、空目录照常打开);
`safety.sh` 新增 7 条 CLI 用例(含"树一动没动"和那句 `gc --reconcile` 的解释),全套 **142 passed, 0 failed**。

#### M2 验收(T2.1–T2.5 + P17,2026-09-19)

- `scripts/tests/safety.sh`:**142 passed, 0 failed**(M1 结束时 93 → T2.3 时 130 → T2.5 时 135 → P17 的 7 条)。
- `ctest`:全新配置的两个构建目录,WFS_FSKIT=OFF **2/2**、WFS_FSKIT=ON **3/3**;
  FSKit 那侧只剩 4 条 `FSVolume*Operations` 的 deprecated 警告(冻结的旧 API,与本轮无关)。
- `scripts/check-deps.sh`:两个构建目录的全部产物都只链接系统库。
- `diff_test` 连跑 **10 次 10/10**。
- `m1_criteria.sh --only 1`(fork 延迟):pool 命中 p50 **9.2 / 9.4 / 9.5 ms**(1k/10k/50k),M1 那轮是 8.6 / 9.0 / 9.7
  ——最差格反而好了 0.2 ms,另两格 +0.4/+0.6 ms 在机器负载噪声里(开跑时 uptime 2.86),门槛 <10 ms 仍然达成。
- `m1_criteria.sh --only 5`(diff):默认全扫 1.420 → **0.267 s**,`--no-xattr` 0.185 → **0.154 s**,
  `--events` 0.354 → 0.228 s。结论不变:50k 上全扫仍然比事件路径快,默认不动。
- 真实 FSKit 挂载复核(`bundle.sh Release` → `world fs mount W2` → `smoke.sh` → `umount`):
  `mount(8)` 里看得到 `(worldfs, local, …, fskit)`,smoke **ALL OK**(read/readdir/stat/write/append/truncate/
  mkdir/rename/unlink/rmdir/symlink/hardlink/chmod/xattr/mmap/fsync),卸载后 `mount | grep worldfs` 为空,
  container store 里 `S1 s27` + `W1/W2/W3` 一动没动。

#### PR #1 review 修复(2026-09-19,Codex 四条)

PR [#1](https://github.com/forks-world/forkfs/pull/1) 的机器评审提了四条,全部认领并修掉,
一条一个提交、一条一个测试:

| # | 位置 | 问题 | 修法 | 提交 |
|---|---|---|---|---|
| P1 | `world.cpp` 快照 discard | 池化 fork 先 claim、后插 world 行,中间那一瞬 `discard S<n>` 既看不到活 World 也看不到池条目,于是把快照扔进 trash——随后发布的 World 没有基线可 diff/verify/restore | claim 和 CREATING world 行**在同一个 BEGIN IMMEDIATE 里提交**(`pool_claim()` 收一个在事务内跑的 hook);普通克隆路径在插行的同一事务里复查快照状态;`wfs_snapshot_discard()` 把**引用检查 + 状态迁移 + rename 整体放进一个 BEGIN IMMEDIATE**,并把 CREATING 的 World 也算作引用 | `250a846` |
| P2 | `cli/main.cpp:844` | CLI 明明写着 `discard W<n>\|S<n> [--now]`,快照分支却把 `now` 丢了,只搬进 trash | `wfs_snapshot_discard(s, id, immediate, force)`,和 World 同一位置同一语义:`.deleting` 改名 → 真删 → 行 DEAD;CLI 打印 `S<n> deleted` 且不再起 worker | `ad9de61` |
| P2 | `world.cpp` gc worker | 时限只在**两棵树之间**检查,一棵大 World 就能把"2 秒"的唤醒拖成几分钟,正好毁掉 T2.1 的占空比(P16) | `fs_remove_tree_parallel()` 收一个 `fs_mono_us()` deadline,**逐条目**检查;超时返回 `-ECANCELED` 中止 walk(目录尾巴一并跳过),`partial` 上报给 worker,树保持 `.deleting` 名字等后继进程接手 | `32d9f28` |
| P2 | `world.cpp` gc worker | `gc_delete_one` 失败只是 `continue`,`work_remains` 可能是 0,于是日志写"trash empty"、不起后继——一次瞬时 unlink 错误就让一棵不可 restore 的树留在那里没人管 | 新增 `wfs_gc_report::trash_failed`(非零就绝不能读成"trash 空");失败的**连续唤醒次数记在 store 的 meta 表**里(每次唤醒都是新进程),前 5 次照常 `work_remains=1` 重试,之后只报告不再自唤醒;删成功即清零 | `2e225c6` |

**验收**:`safety.sh` **158 passed, 0 failed**(P17 那轮的 142 → 本轮新增 16 条 `PR1` 用例);
`ctest` 两个全新配置的构建目录 WFS_FSKIT=OFF **2/2**、ON **3/3**;`check-deps.sh` 两个目录全绿。

新增测试都验证过"没有修复就会红":

- P1:`core_test` 用一个测试缝(`wfs_test_after_pool_claim`,库里恒为 NULL)在 claim 与发布之间跑一次
  `discard`,要求 `WFS_E_SNAPSHOT_IN_USE`,再确认 fork 出来的 World 基线还在;不数 CREATING 行时它返回 0(红)。
- 时限:12 万条目的 trash 树 + `WORLD_GC_BATCH_SECS=1`,唤醒 **1.03 s** 返回、留下 41,647 条目、
  后继链把它删完;老代码是 1.77 s 一口气删完并报告 trash 空(红 2 条)。
- 失败重试:用 **ACL**(`deny delete_child`,chmod 和 chflags 都解不掉,正是 deleter 遇到 EPERM 时的两招)
  造一个真删不掉的条目,`gc --status` 一直数得到它,worker 日志绝不出现 "trash empty",5 次之后
  改口"不再自动重试",ACL 一撤下次 gc 就删干净;老代码红 5 条。

#### PR #1 review 第二轮修复(2026-09-19,Codex 两条)

同一个 PR 的第二轮机器评审又提了两条,都是**"把一次失败读成一个肯定的答案"**这类错误,
一条一个提交、一条一个测试:

| # | 位置 | 问题 | 修法 | 提交 |
|---|---|---|---|---|
| P1 | `hardlinks.cpp:284` | 硬链接重放把临时名字写死成 `<path>.wfs-tmp`,EEXIST 时**直接 unlink**。可是快照装的是别人的工作区:一个叫 `b.wfs-tmp` 的普通文件挨着硬链接的 `b`,每次 fork / checkpoint / pool 填充都会把它悄悄删掉 | 临时名字改成同目录下的 `.wfs-hl-<pid>-<计数>-<getentropy 16 位十六进制>`;`link(2)` 本身就是排他创建(EEXIST 而不是覆盖),**成功的 link 就是占位**,和 `open(2)` 的 `O_EXCL` 一个道理;EEXIST 就换个名字重来,**任何不是我们自己的东西一律不 unlink** | `59a0ad9` |
| P2 | `diff.cpp:244` | `listxattr`/`getxattr` 失败(EACCES、EIO、ERANGE 重试分配不到内存)一律当成"没有扩展属性"。对面也是空的时候,两个文件就被判**相等**——一次根本没发生的比较,报出来是"干净" | xattr 这条腿从两值答案改成三值:只有**成功返回 0 长度**才算"没有";失败是 `XA_ERROR`,条目报 `T`(绝不报相等)并记进新的 `wfs_diff_stats::xattr_errors`(`diff --stat` 会打一行 note)。**快照那一侧失败根本不算 diff 结果**:那棵树是我们克隆的、在 SnapGate 窗口里开着门读的,EACCES/EPERM 直接让整个调用返回该 errno | `665a868` |

`EF_NO_XATTRS` 那条捷径**保留**:它是文件系统**成功**地说"这个文件一个属性都没有",和一次失败的
`listxattr` 不是一回事。但只要有一侧没这个位,另一侧就老老实实 `listxattr`,失败就是错误、不是空表。
顺带修掉同一类的两处静默相等:`malloc` 失败(原来 `return true`)和名字超过 4096 字节的
`xattr_equal_raw` 回退路径(原来两边都当 0 长度,于是相等)。

**`.wfs-tmp` 家族审计**(评审要求的那一条):`WFS_TMP_SUFFIX` 一共六处,按"名字在谁的地盘上"分两类——

| 位置 | 名字 | 判定 |
|---|---|---|
| `hardlinks.cpp` 重放 | `<用户树里的名字>.wfs-tmp` | **不安全,本轮已修** |
| `world.cpp:576` 快照创建 | `<store>/snapshots/S<n>.wfs-tmp` | 安全:store 内部,名字由新行 id 决定 |
| `world.cpp:2055` gc 清快照 | 同上 | 安全:同一个 store 内部名字 |
| `pool.cpp:154/365/520` | `<store>/pool/S<n>/<uuid>.wfs-tmp` | 安全:store 内部 + uuid |
| trash 的 `.deleting` | `<store>/trash/<name>.deleting` | 安全:trash 全归我们,条目名字是 discard 自己起的 |
| `world.cpp:937` fork 目标 | `<用户选的 target>.wfs-tmp` | **仍不安全**,见下(第三轮 `fd673df` 已修) |

后两处需要单独一轮(本轮不动,以免把 publish 顺序和 gc 的清扫规则一起改了):

- `wfs_world_create()` 在 `<target>.wfs-tmp` 下建树,开头是
  `if (exists(tmp)) fs_remove_tree(tmp)`。`target` 是用户给的路径(`--to ~/w/a`),
  所以 `~/w/a.wfs-tmp` 要是用户自己的文件或目录,fork 会**整棵删掉**。
- `world.cpp:1688` 的 `rm_tmp_in_dir()` 更宽:gc 会把**每个 World 的父目录**(用户目录!)扫一遍,
  凡是以 `.wfs-tmp` 结尾的条目一律 `fs_remove_tree`。`~/w/notes.wfs-tmp` 就这么没了。
- 修法方向:临时名字同样改成唯一名(`link`/`clonefile` 都是排他创建,不必先删),
  并且把它**记进 CREATING 那一行**,让 gc 只删自己记下来的路径,而不是按后缀猜。

**验收**:`safety.sh` **161 passed, 0 failed**(上一轮 158 → 本轮新增 3 条 `PR1` 用例);
`ctest` 两个全新配置的构建目录 WFS_FSKIT=OFF **2/2**、ON **3/3**(只剩 FSKit 那 4 条冻结 API 的
deprecated 警告);`check-deps.sh` 两个目录全绿(ACL 用的是 libSystem,没引进新库);
`diff_test` 连跑 **8 次 8/8**。

两条新测试都验证过"没有修复就会红":

- P1:`core_test` 在硬链接组 `g2` / `sub/g2-b` 旁边各放一个 `*.wfs-tmp` 普通文件,要求 fork 之后
  三者都在、内容一字不差、两个名字仍共享 inode,而且树里**不留**任何 `.wfs-hl-` 临时名;
  pool 路径同样要过。老代码红在 `core_test.cpp:724`(`sub/g2-b.wfs-tmp` 直接不见了)。
  `safety.sh` 用 CLI 把同一件事再做一遍(`a` / `b` / `a.wfs-tmp` / `b.wfs-tmp`)。
- P2:`diff_test` 用 **ACL**(`deny readextattr`)让 world 侧一个文件的 `listxattr` 返回 EACCES
  ——ACL 正好不碰 diff 比的任何东西:`st_mode` 还是 0644、`st_flags` 还是 0、`lstat` 照样能用,
  所以条目是**带着其余全部相等**走到 xattr 这条腿上的,这是老 bug 唯一看得见的地方。要求
  `--full` 和事件路径都报 `T` 且 `xattr_errors == 1`,`--no-xattr` 0 行,ACL 一撤回到 1 行、
  属性删掉回到 0 行;再把 ACL 挪到**快照**那一侧,要求整个 diff 以 `-EACCES` 失败。
  老代码红在 "xattr unreadable / --full: 0 lines, wanted 1"。

#### PR #1 review 第三轮:用户目录里的临时名(2026-09-19)

上一轮那张审计表留了两处"仍不安全"的尾巴,本轮先把它们做掉;做到一半 Codex 又给同一个 PR
提了四条,全部落在同一片代码里,于是一起收。**一条一个提交、一条一个测试。**

架构决定(本轮的总纲):**store 在用户的目录里从不靠猜**。名字要么是我们抽出来的、并且记在行上,
要么就不是我们的;凡是"以某后缀结尾就删"的扫描,只允许发生在 store 内部。

| # | 位置 | 问题 | 修法 | 提交 |
|---|---|---|---|---|
| 尾巴 1 | `world.cpp` fork | 克隆建在 `<target>.wfs-tmp`,开头 `if (exists(tmp)) fs_remove_tree(tmp)`。`target` 是用户给的(`--to ~/w/a`),于是 `~/w/a.wfs-tmp`——用户自己的文件或整棵目录——每次 fork 都被删掉 | 临时名改成目标父目录里抽出来的 `.wfs-fork-<pid>-<计数>-<getentropy 16 位十六进制>`;`clonefile(2)` 自己创建目标、已存在返回 EEXIST 而不是覆盖,**克隆成功就是占位**(和 `open(2)` 的 `O_EXCL` 一个道理),EEXIST 就换名字重来,**任何不是我们建的东西一律不删**。名字在**克隆开始之前**写进 CREATING 行的新列 `tmp_path`(additive ALTER,`user_version` rev 2→3),崩溃之后那一行就是那棵树唯一的名字来源 | `fd673df` |
| 尾巴 2 | `world.cpp` gc | `rm_tmp_in_dir()` 让 gc 把**每个 World 的父目录**扫一遍,凡是 `.wfs-tmp` 结尾一律 `fs_remove_tree`。一次例行 `world fs gc` 就能吃掉 `~/w/notes.wfs-tmp` | gc 只删 CREATING 行**记下来的那一条路径**,而且要先和"现在"对一遍:仍是目录、没有活行认领它的 inode、`.world` 标记若存在必须写着这一行的 world id(publish 顺序是先写标记后 rename,所以我们自己的半成品是带标记的)。删完把行标成 **DEAD**(不再 DELETE,证据留着)。按后缀扫只剩 `<store>/snapshots`,函数改名 `rm_tmp_in_store_dir()` 并加 `under_dir(store)` 守卫 | `fd673df` |
| 顺手 | `world.cpp` / `pool.cpp` publish rename | P7 只在 `check_path` 那一刻看过"目标不存在";`rename(2)` 会**替换一个已存在的空目录**,所以从检查到 rename 之间任何人建出目标目录,就是一次静默删除 | 两条 publish 路径(普通克隆、pool 领取)都换成 `fs_rename_excl()` = `renameatx_np(..., RENAME_EXCL)`,目标存在就是 EEXIST,由 P7 去解释,绝不替换 | `fd673df` |
| P1 `4053200895` | `cli/main.cpp:562` | gc 的 cheap pass 把**每一条 `state=0` 的行**(world / snapshot / pool)都当成"生产者死了",删树删行。可是大树克隆比自动 worker 那两秒的停顿长得多:worker 能把**正在进行的 fork** 的树和行一起删掉,fork 随后那条 UPDATE 一行都没匹配上却照样返回成功,`--to` 上就留下一个没人登记的目录 | CREATING 行记 `owner_pid` + **该进程自己的启动时间**(pid 复用就是另一个人);行只有在"生产者确实没了"**且**"行已经老过 `WORLD_GC_CREATING_MIN_AGE`(默认 60 s)"时才收。`kill(pid,0)` 说不清就当活着——不收是安全的那一边。pool 同理:还在填的条目和它的 `<uuid>.wfs-tmp` 都不动;**被 fork 领走的条目根本没有 pool 行**(claim 就删了),所以 pool 那趟"没有行认领的目录"扫描以前会把活 fork 正要 rename 的树删掉——现在 CREATING world 行把它记在 `tmp_path` 里,pool_collect 一律当作有主。另一半:fork 最后那条 UPDATE 加 `AND state=CREATING` 并校验 `sqlite3_changes()==1`,行没了就把树搬回临时名删掉(pool 路径则搬回池里)并返回 `-ESTALE` | `c9899bc` |
| P2 `4053200896` | `world.cpp:1986` | discard 在"rename 进 trash"和"提交行"之间被杀,留下的是一个普通的 `W<n>-<t>` 目录:没有行,也没有 `.deleting` 后缀。`trash_scan()` 把它算作立即到期,`gc` 于是 `work_remains=1`,可 `wfs_gc_pending()` 另起炉灶只查两张表 + 找 `.deleting`,答案是"没活儿"——CLI 说"已交给后台",却一个 worker 都没起,后面每次 fork/discard 都同样判断,孤儿就一直躺着 | `wfs_gc_pending()` 直接跑 `trash_scan()`,从 `deleting` / `due` 给答案。**决策和干活用同一套分类**,代价还是一次 readdir + 两张表 | `b8d28e4` |
| P2 `4053200899` | `world.cpp:1282` | 快照目录已经不在时,不带 `--now` 的 `discard S<n>` 把 `trash_path` 清空却仍把行提交成 TRASHED。这种行 `trash_scan()` 跳过(没路径),reconcile 又只看 ACTIVE——没有任何后续 collector 能推动它,`status` 里就永远"trashed" | 树已经没了就在同一个 BEGIN IMMEDIATE 里**直接 reconcile 成 DEAD** 并返回:没东西可搬、没东西可删、也不用起 worker。pool 条目不用额外处理(有引用就拒绝 discard,`--force` 事先 drain,填回来的由 pool_collect 收)。CLI 改口"its tree was already gone; the row is now dead" | `073a9ef` |
| P2 `4053200900` | `hardlinks.cpp:412` | `pthread_create` 可能这一轮失败、下一轮成功。句柄写在 `th[i]`,`started` 却只是计数:join 循环于是 join 一个**从没写过**的 `pthread_t`,同时**漏掉**真正活着的那个 worker——它可以在调用者返回之后继续读栈上的 `RestoreJob` | 抽一个 `threads_start()`:**真起来的句柄连续写进 `th[0..n)`**,返回 n。核心里一共就两处 `pthread_create`——`hardlinks_restore`(P9 重放)和 `fs_walk_tree`(也就是所有并行遍历:克隆回退、protect/unprotect、scan、count、并行删除,pool 填充和 gc deleter 也都走它)——两处都换过去 | `7f0bb20` |

**验收**:`safety.sh` **167 passed, 0 failed**(上一轮 161 → 本轮净 +6:PR3 七条新用例,减掉那条"gc 扫掉
`*.wfs-tmp`"的旧断言——它断言的正是本轮删掉的行为);`ctest` 两个全新配置的构建目录
WFS_FSKIT=OFF **2/2**、ON **3/3**(只剩 FSKit 那 4 条冻结 API 的 deprecated 警告);
`check-deps.sh` 两个目录全绿(新用到的 `renameatx_np`、`sysctl`、`getentropy` 都在 libSystem 里)。

新增测试都验证过"没有修复就会红":

- 尾巴 1/2:`core_test` 在 fork 目标旁边放一个普通文件 `wtmp.wfs-tmp`(正好是旧代码给
  `--to .../wtmp` 起的临时名)和一个普通目录 `notes.wfs-tmp`,要求 fork 之后、gc 之后两者
  分毫不动,而且 fork 不留任何 `.wfs-fork-` 名字。`safety.sh` 用 CLI 把同一件事再做一遍。
- 崩溃那一刻:新测试缝 `wfs_test_before_fork_publish`(库里恒为 NULL)在"克隆已建好、已记在行上"
  和"publish rename"之间把 fork 停住,什么都不回滚——正是 `kill -9` 留下的样子。gc 必须删掉
  **行记下的那条路径**、别的一样不动,并把行标成 DEAD;行记的树要是早被人手删了,就只标 DEAD、
  `tmp_removed == 0`。
- P1:同一个缝跑两遍。第一遍生产者是**本进程**(活的),`WORLD_GC_CREATING_MIN_AGE=0` 把年龄那条
  规则关掉,要求 gc 把树和行原样留下;第二遍用 `wfs_test_fork_owner_pid` 写一个**不存在的 pid**,
  要求 gc 删树 + 标 DEAD。`safety.sh` 用 `sqlite3` 直接往 store 里写一行 CREATING(除了跟 fork 赛跑,
  没有别的办法让一行在一整趟 gc 期间保持打开):`owner_pid=$$` 时树和行都在,换成死 pid 之后
  树没了、行是 3。
- P2 `…896`:`core_test` 造一个没有行、也没有 `.deleting` 后缀的 `W9999-1` 孤儿,要求
  `wfs_gc_pending()` 在 7 天保留期下答 1(老代码答 0),随后那次 gc 把它删掉并计进 `trash_orphans`;
  `safety.sh` 走 CLI:孤儿放进 trash,跑一次普通 `fork`,它起的 worker 必须把孤儿清掉。
- P2 `…899`:`core_test` 建一个快照,绕过 store 把 `<store>/snapshots/S<n>` 删掉,然后**不带 `--now`**
  discard,要求行是 DEAD、store 的 trashed-snapshot 计数只剩旁边那个正经 trashed 的。
- P2 `…900`:这个 bug 本身是 UB,从调用者那边看不看得见全凭运气,所以测的是抽出来的那个 helper:
  `wfs_test_thread_fail_mask` 拒掉指定的 slot,`wfs_test_threads_start()` 先把句柄数组清零
  (于是"没写过的 slot"必然 join 不成),再直接查合同——写了 n 个句柄、join 成功 n 次、跑起来的
  worker 数等于 join 掉的数。老循环 + 拒掉 slot 0 时它返回 `-EIO`(起了 3 个、join 成 2 个、跑了 3 个)。
  另外 `core_test` 还拿一棵 12 组硬链接的树在 slot 0 被拒的情况下真做一遍 init + fork,要求每组
  都还连着、树里不留 `.wfs-hl-` 临时名。

**`.wfs-tmp` 家族审计**(上一轮那张表的收尾):`WFS_TMP_SUFFIX` 现在只出现在 store 内部——
`world.cpp` 快照创建与 gc 清快照(`<store>/snapshots/S<n>.wfs-tmp`,名字由行 id 决定)、
`pool.cpp` 三处(`<store>/pool/S<n>/<uuid>.wfs-tmp`)、trash 的 `.deleting`。用户目录里的两处
(fork 目标、gc 扫父目录)本轮都没了,硬链接重放那处上一轮已改成 `.wfs-hl-<…>`。

#### PR #1 review 第四轮:两条"失败了却当没事"(2026-09-19)

同一个 PR 的第四轮,Codex 两条 P2,主题是一样的:**一次失败被当成了一个可以接受的结果**——
一次是重放硬链接失败了照样发布,一次是并行删除失败了就退回到一条没有期限的慢路。
**一条一个提交、一条一个测试,两个测试都先验证过"没有修复就会红"。**

| # | 位置 | 问题 | 修法 | 提交 |
|---|---|---|---|---|
| P2 `4053275996` | `world.cpp:658` | 硬链接组落在一个**没有 owner-write 位**的目录里(`0555`:vendor 进来的树、生成的 fixture、`chmod -R a-w` 过的发布目录),克隆把这个模式一起带过去,于是重放的 `link(2)`/`rename(2)` 全是 EACCES。而这里**把返回值丢掉了**:快照照样发布,manifest 和 `hl_groups` 行都声称有这个组,树里却没有——之后每一个 fork、每一个 pool 条目都继承了这份不一致 | 两半都做。**借**:`link`/`rename` 吃到 EACCES/EPERM 时,把目标所在目录借出 owner write(和 search),**只借这两个调用的工夫**,然后把**原样的 mode** 还回去(以及 `UF_IMMUTABLE`/`UF_APPEND`,如果它本来有)。借出记在一个栈上,由析构函数**逆序**归还,成功路径和每一条错误路径都走同一个出口。两个组可能落在同一个目录里、重放又是四线程的,所以整个"借→link→rename→还"用调用自己的一把锁串起来:常见情况下它一次都不会跑,代价是零。**败**:`hardlinks_restore()` 从此返回 `first_err`——`missing`/`skipped` 仍然容忍(那是活源在克隆底下变),但**文件系统拒绝过的 link 不行**,快照创建删掉半成品树并删行、fork 回滚克隆和行、pool 填充丢掉这个条目。顺手:`rm_rec()` 能打开 `0555` 目录却不能在里面 unlink(`discard --now` 因此 EACCES),现在进门就把模式摘掉——和并行删除的 `rm_entry` 一直在做的一样,**我们本来就是在删它** | `c0b5e2f` |
| P2 `4053276002` | `platform_posix.cpp:852` | 并行删除每条目检查期限,可**只要错误不是 `-ECANCELED` 就退回 `fs_remove_tree()`**——而它一个期限都没收到:一趟 O(树) 的 unprotect 遍历加一趟 O(树) 的深度优先 unlink。大 trash 树里一个读不动的目录,就能把"两秒一轮"的 worker 拖成几分钟,正是 `max_secs` 要挡的前台争用 | `fs_remove_tree()` 收下同一对 `(deadline_us, partial)`,带默认值,所有旧调用点一字不改:`rm_rec()` 每条目读一次时钟(和 `rm_entry` 一样),超时以 `-ECANCELED` 退出并且**回程上不 rmdir 任何东西**,剩下的是一棵 `.deleting` 树,天生可续;unprotect 遍历也拿到期限(`fs_unprotect_tree` 多一个可选时间戳,回调超时返回 `-ECANCELED`)——一个能把整份预算花在清 `UF_IMMUTABLE` 上的回退路,不过是换个名字的无界 wake;`rm_entry` 的 ENOTEMPTY 补救把自己的期限传下去,被期限打断时报 `-ECANCELED` 而不是失败 | `5f5dfd3` |

**验收**:`safety.sh` **177 passed, 0 failed**(上一轮 167 → 本轮净 +10:0555 那条 7 个断言、
回退期限那条 4 个断言,减去合并的计数差);`ctest` 两个配置 WFS_FSKIT=OFF **2/2**、ON **3/3**
(只剩 FSKit 那 4 条冻结 API 的 deprecated 警告);`check-deps.sh` 全绿(没有新的系统调用)。

两条都先把测试跑红过:

- 0555:关掉"借"这一半,`core_test` 在 `wfs_snapshot_create` 就 `-13`(以前是静静发布一棵
  和 manifest 对不上的树,现在至少会失败),`safety.sh` 五条断言全红。
- 回退期限:把 `fs_remove_tree(root, deadline_us, partial)` 改回 `fs_remove_tree(root)`,
  一次 `WORLD_GC_BATCH_SECS=1` 的 wake 跑了 **4713 ms**、把整棵树删光、一句 "work remains" 都没有。

新增测试:

- `core_test`(P2 `…996`):`0555` 目录里的一对硬链接,在**快照、fork、pool 发出来的 world**
  三处都要重新连上,而且三处的目录模式都要是 `0555`、里面不留 `.wfs-hl-` 临时名。另一半用新测试缝
  `wfs_test_hardlink_restore_err`(库里恒为 0)强制重放报错,钉死回滚:**没有 `S<n>`、没有
  `S<n>.wfs-tmp`、没有行、`--to` 上没有树、pool 里没有条目**;缝一清,同一个快照照样建得出来。
- `safety.sh`(P2 `…996`):同一件事走 CLI 再做一遍,快照(开一下门看)、fork、pool 三条路。
- `safety.sh`(P2 `…002`):一棵 12 万条目的 trash 树,**全部内容压在一个 `0000` 目录底下**——
  并行遍历的 `opendir` 在删掉任何一个条目之前就 EACCES,回退路**确定地**每次都会走到。
  `WORLD_GC_BATCH_SECS=1` 下 wake 要在 ~1 s 回来、报 "work remains, handing over"、
  留下 `.deleting` 和大半棵树;删除器进门时把那个 `0000` 摘掉(它本来就是在删它),
  于是后继链走回并行路并把树收干净。
