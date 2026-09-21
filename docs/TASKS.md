# BranchFS / World FS — 任务分解与进度

依据 `arch.md` v0.3。本文件是唯一的任务看板,状态用 `[ ]` / `[~]` / `[x]` 标记。

当前 macOS 支持范围、测试命令和剩余验收边界见
[`MACOS_VALIDATION.md`](MACOS_VALIDATION.md)。本文件按时间保留历史测试数量与测量结果;
旧的 93/142 等通过数不代表当前测试总数,应以当前 CI/测试输出为准。

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
      当时 P11 的"真实磁盘写满"仅在 API 层验证;后续新增独立
      `scripts/tests/disk_full.py` + `core/disk_full_test`,在私有 APFS 映像中验证真实 ENOSPC,
      不填宿主卷。范围和复现方式见 `MACOS_VALIDATION.md`。
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
- [x] T1.8 文档:arch.md §40、README 与 `MACOS_VALIDATION.md` 说明当前 clonefile 路线、验证边界和复现方式

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
- [x] P17 store 完整性:有树但库(`metadata3.db`;schema 2 是 `metadata.db`)没了 → `WFS_E_STORE_DAMAGED`,拒绝并说明出路
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

**风险**:store 目录还在、`snapshots/` 里还有树,但库(`metadata3.db`;schema 2 时叫 `metadata.db`)没了(误删、备份还原了半套、磁盘错误)。
从前 `wfs_store_open` 会**静默新建一个空库**——id 是从库里发的,新库再发一次 1,下一次 `init` 就往已经在磁盘上的
`snapshots/S1` 上写。

**做法**:`wfs_store_open` 在创建任何东西之前先判:库不存在 / 是空文件 / 读不了 /
打开后连 pragma 都执行不过(根本不是数据库),**而** `snapshots/`、`trash/`、`pool/` 里还有条目
→ `WFS_E_STORE_DAMAGED`(-1017)。检测只是对这三个目录各一次 readdir,**快照的 gate 一路关着也照样发现**
(不需要进 `S<n>/root`)。空目录不算损坏,那是新 store。

**CLI 把话说全**:这些树就是那个数据库的索引、id 会撞车、**`gc --reconcile` 帮不上忙**
(它要读数据库才知道哪些行的树没了,而这里没的正是数据库),出路只有两条——从备份恢复 `metadata3.db`,
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

#### PR #1 review 第三十七轮:方向不在磁盘上,在选了方向的那个进程里(2026-09-20)

第三十七轮,Codex 一条 P1。前三轮把升级协议的**每一个中间形状**都写成「可以接着做」的,这一轮指出:
**这套协议有两个方向,而磁盘上的形状分不出方向**——一个正在回滚的进程留下的形状,和一次没做完的
前进留下的形状,**逐字**是同一个;于是第二个 M2 进程把别人的回滚当成自己的续做,两个人一人删一个
名字,库的 inode 一个名字都不剩。

| # | 位置 | 问题 | 修法 | 提交 |
|---|---|---|---|---|
| P1 `PRRT_kwDOUf7jGc6kHIsy` | `core/src/store.cpp` `legacy_holders_gate()` / `wfs_store_open()`(~777) | A 在回滚:交换回来之后、`unlink(metadata3.db)` 之前。B 这一刻 open,看到「`VERSION` 还是 3 + 两个名字都是普通文件 + 同一个 inode」——上表里「崩在第 2、3 步之间」,**一律往前做**。B 于是交换进自己的空目录;A `unlink(metadata3.db)`;B `unlink` 掉它以为多出来的链接——**那是最后一个名字**。store 里只剩一个 0500 空目录 | 两条,一条防住一条兜住:① `<store>/upgrade.lock` 上的独占 `flock`,在**读判据之前**取,整段持有(前进的四步 + 闸门 + 迁移 + `VERSION` 复读,或者整个回滚),锁里再重读一遍布局;② **永远不删最后一条链接**——每次 `unlink` 库的名字之前比 `st_dev`/`st_ino` 并要求 `st_nlink >= 2`,否则拒绝并把两个名字都留下 | `be88326` |

##### 那条交错,和它的实测(先跑红)

```
A(回滚)                                   B(另一个 M2 进程 open)
  link(metadata3.db, stub_tmp)
  SWAP stub_tmp <-> metadata.db
  ── 窗口:metadata.db 和 metadata3.db 是同一个 inode,nlink=2 ──
                                            store_layout(): 两个名字都是普通文件、同一个
                                            inode → 「崩在第 2、3 步之间」→ 往前做
                                            mkdir stub_B; link 已在; SWAP stub_B <-> metadata.db
  unlink(metadata3.db)                      (metadata.db 现在是 B 的空目录,库在 stub_B)
                                            unlink(stub_B)   ← 最后一条链接
```

`scratchpad/r37/red37.c`,链接父提交 `bfbd38e` 编出来的 `libworldfs_core.a`(只加了一条回滚缝):

```
before: VERSION=2, metadata.db is the database, 2 rows in worlds
  A@window    metadata.db: file ino=312691916 nlink=2 | metadata3.db: file ino=312691916 nlink=2
B: has exchanged its stub into metadata.db (no lock stopped it)
  A@afterB    metadata.db: dir  ino=312691929 nlink=2 | metadata3.db: file ino=312691916 nlink=2
A: unlinked metadata3.db; letting B run its step 4
A: wfs_store_open rc=-1020   B: wfs_store_open rc=-1020
  after both  metadata.db: dir  ino=312691929 nlink=2 | metadata3.db: ABSENT
  VERSION=2
  the database inode: HAS NO NAME AT ALL -- the rows are gone
  a later open of the store: rc=-1017
```

两个进程都「按协议办事」,两次 open 都回 `WFS_E_STORE_BUSY`(看起来只是两次重试),而 store 里只剩
`dr-x------ metadata.db/` 和一个 `VERSION`。**方向不在磁盘上,在选了方向的那个进程里**——所以要么把
选方向这件事串起来,要么让那个致命的 `unlink` 自己长出眼睛。两样都做了。

##### 一、`upgrade.lock`:只在欠着一步的时候取

```
布局说欠着(搬库 / 补空目录 / 清扫 / VERSION 还是 2)?  → flock(LOCK_EX) → 锁里重读 VERSION + 布局
布局说什么都不欠(schema 3、空目录在、库一条链接)?    → 这个文件根本不打开
```

- **整段持有**:往前是「搬库四步 → 持有者闸门 → 迁移 → 最后那次 `VERSION` 复读」,往后是整个回滚。
- **双重检查**:等到的那个进程在锁里重读 `VERSION` 和布局——要么是已经做完的 schema 3 store(直接开),
  要么是被放回 schema 2 的 store(自己再跑一遍闸门:`WFS_E_STORE_BUSY`,或者接管)。
- **迟到的那道闸门也在锁里**,而且 `user_version` 要在锁里**重读**:锁外读到的 2xx 可能是我们正在等的
  那个进程迁移做到一半,拿它去跑闸门,会把别人刚做完的升级整段放回去(这一条是写完测试才发现的,
  测试当场就把它抓出来了)。
- **M1 不取这把锁,也不需要取**:M1 全程不取任何 store 级的锁——这一整节的前提就是这个。挡 M1 的是
  文件(第三十二轮)、空目录(第三十五 / 三十六轮)、持有者闸门(第三十四轮);这把锁是 **M2↔M2**,
  只管那三样管不了的那一种:另一个进程**反着**跑同一套协议。
- **它不会被当成库的持有者**:`proc_listpidspath(3)` 问的是 `metadata3.db` 的 vnode,`upgrade.lock`
  是另一个文件(`core_test` 里按住它的子进程,`wfs_store_holders()` 一个都列不出来)。
- **进程死了锁自动没**,`flock` 是内核维护的,没有「陈旧锁」要扫。

##### 二、`unlink_extra_link()`:要删的和要留的,必须是同一个 inode 而且 `nlink >= 2`

前进的第 4 步删 `stub_tmp`、回滚删 `metadata3.db`,两处都改成先问再删:`stat` 两个名字,
`st_dev`/`st_ino` 必须相同、而且 `st_nlink >= 2`,否则**原地拒绝**(`WFS_E_STORE_DAMAGED`),
两个名字都留着。留下的形状上一轮那张表都接得住,**丢掉 inode 接不住**。这一条让丢数据这个结局
**即使绕过锁也不成立**——老的 M2 二进制没这把锁、将来再出一个 bug、`flock` 在某个文件系统上是空操作,
都一样。

##### 三、第三十六轮那个疑问,现在可以关掉

`metadata.db.stub.` 的清扫(空目录 `rmdir`、和库同 inode 的普通文件 `unlink`)在**锁里**做,
并发的另一个升级者的 stub 已经不可能存在,所以这次清扫是安全的,原样保留。

##### 还剩的那一种:两个升级者互相拒绝(第三十四轮那笔交易,照旧)

两个 M2 进程同时**第一次**打开同一个 schema 2 store:一个取到锁往前做,另一个可能在**取锁之前**就已经
打开了库(布局看起来什么都不欠、`user_version` 才是 2xx 的那条路),于是前者的闸门把它列成持有者,
整段回滚 + `WFS_E_STORE_BUSY`。**一个 store 一生一次的重试,数据一行不少**——这正是第三十四轮写下的
那笔交易(「两个升级者互相拒绝,不过是一个 store 第一次 open 时重试一次」)。第三十一轮那个用例的断言
因此从「两个都回 0」改成「两个都只可能是 0 或 `WFS_E_STORE_BUSY`、都不留临时文件、随后单独一次 open
一定接管」——因为那个「第二个打开者」现在是**真的第二个进程**,而以前它是本进程的一次嵌套调用,
`fs_other_holders()` 按定义把自己的 pid 排除在外。

##### 新增测试

- 新缝 `wfs_test_in_revert(ctx, dir, phase)`(`worldfs.h`,非测试运行恒为 NULL):1 = 交换回来之后、
  `unlink(metadata3.db)` 之前;2 = 那个 unlink 之后、`VERSION` 放回 2 之前。
- 新缝 `wfs_test_in_upgrade_lock(ctx, dir)`:**等到锁的那个进程**刚拿到锁、还没读任何东西的那一刻——
  它在里面看到什么,就是这把锁的全部意义。
- 交错用例:持有者子进程(让闸门回滚)+ **B 进程**,B 在缝之前就起好、等一个管道字节。A 在窗口里
  断言「`metadata.db` 是普通文件、`nlink == 2`、和 `metadata3.db` 同一个 inode」,放 B 走,等 B 说
  「我进 `wfs_store_open` 了」,睡 250 ms,然后**非阻塞地**确认 B **既没有**交换、**也没有**做完——
  修之前这两件事都成立,而第二件就是那次致命的 unlink。B 在锁里报回来:`VERSION == 2`、
  `metadata.db` 是**普通文件**、`nlink == 1`、没有 `metadata3.db`;它自己那次 open 回
  `WFS_E_STORE_BUSY`(持有者还在)。收尾断言库还在、两行还在、没有多余的名字;放走持有者之后,
  同一个 store 被正常接管。
- 兜底那条单独测:在前进的第 2 步之后(交换完、还没 unlink)把 `metadata3.db` **从外面删掉**——
  正是回滚做的那件事——于是第 4 步要删的名字成了**最后一条链接**:`WFS_E_STORE_DAMAGED`,名字留着,
  那个文件还是那个库、两行还在。另一种形状:把私有名换成**别人的**文件(不同 inode)→ 同样拒绝、
  原样留着,而搬库本身已经完成,这次 open 照常把 store 开出来。
- `upgrade.lock` 不是库的持有者:一个子进程按住它,`wfs_store_holders()` 答 0 个。
- **测试里的「第二个进程」一律改成 `fork` + `execv` 自己**(`core_test --open-store` / `--m1-open`):
  这台机器上 `os_log` 的状态不是 fork-safe 的,一个 fork 出来的子进程调 `sqlite3_open()` 会在
  `libsystem_trace` 里段错误(实测 15 次跑挂 2 次,栈上没有我们一行代码)。`execv` 给它一个干净的地址
  空间,而且「第二个进程」本来就是这些用例要说的事。顺带把第三十六轮那两个 fork 出来的 M1 开库子进程
  也一起换掉了。

**验收**:`ctest`(WFS_FSKIT=OFF)**2/2**,`core_test` 连跑 12 次全绿;`safety.sh` **298 passed, 0 failed**;
`check-deps.sh` 全绿(5 个系统库)。(`m1_criteria.sh` 本轮跳过:没有碰到它量的任何东西。)

#### PR #1 review 第三十六轮:改名的那一瞬间,那个名字不能是空的(2026-09-20)

第三十六轮,Codex 一条 P1。上一轮把「能拒 M1 的只剩库本身」这件事做对了,但**做法本身开了一个更小、
更糟的窗口**:库先 `rename` 走、空目录后 `mkdir` 回来,这两个系统调用之间 `<store>/metadata.db`
**不存在**——而上一轮要拒的正是那个「已经验过票、还没推门」的进程,它那句 open 带着
`SQLITE_OPEN_CREATE`。**一个不存在的名字,对带 CREATE 的 open 来说不是拒绝,是邀请。**

| # | 位置 | 问题 | 修法 | 提交 |
|---|---|---|---|---|
| P1 `PRRT_kwDOUf7jGc6kHCa8` | `core/src/store.cpp` `db_move_to_schema3()` / `wfs_store_open()`(~495) | `rename(metadata.db → metadata3.db)` 和 `db_stub_make()` 之间那个名字是空的。已经被放行的 M1 进程醒在这里:`sqlite3_open_v2(metadata.db, READWRITE\|CREATE)` **新建一个空库**,M1 把它戳成 2、一行都读不到,它的 collector 于是把整个 store 的快照树和 trash 当孤儿删光;M2 随后在自己空目录的位置上看到一个普通文件,报 `WFS_E_STORE_DAMAGED` | 搬库和建空目录合成**一步**,那个名字全程有人占着:私有名 `metadata.db.stub.<pid>.<hex>` 先 `mkdir` → `link(metadata.db, metadata3.db)`(一个 inode 两个名字)→ `renameatx_np(..., RENAME_SWAP)` 把两个目录项**原子对调** → `unlink` 掉多出来的链接。回滚反着走同样四步 | `d42ac8d` |

##### 四步,和每一步里 M1 看得见什么

```
1. mkdir <store>/metadata.db.stub.<pid>.<hex>   0500 空目录;metadata.db 还一动没动
2. link  metadata.db -> metadata3.db            库拿到新名字、留着老名字:一个 inode,两个名字
3. SWAP  stub_tmp <-> metadata.db               两个目录项原子对调:metadata.db 从「是库」变成
                                                「是目录」,中间没有第三种状态
4. unlink stub_tmp                              去掉多出来的那条链接;库只剩 metadata3.db
```

| 时刻 | M1 的 `sqlite3_open_v2(metadata.db, RW\|CREATE)` | 谁来拒它 |
|---|---|---|
| 第 3 步之前(含 2 和 3 之间) | **成功**,拿到的是**真正的**库和真正的行 | 第三十四轮那道持有者闸门:`proc_listpidspath(3)` 把 `metadata3.db` 解析成同一个 vnode,找得到它 → 整体回滚 + `WFS_E_STORE_BUSY` |
| 第 3 步之后 | `SQLITE_CANTOPEN` | 空目录本身(第三十五轮的实测) |

没有第三种,而且两种里都不会凭空多出一个新库——这就是「红」那一行和「绿」那一行的全部差别。

**第 2 步为什么安全**:硬链接的是一个**关着**的 SQLite 库。上一轮的 (b) 已经用本 core 的连接跑过
`PRAGMA journal_mode=DELETE`、关掉、删掉三个 sidecar,所以链的是一个没人开着、旁边什么都没有的普通
文件;而硬链接是第二个**名字**、不是副本,没有第二个库可以分叉。(反过来说:`metadata3.db-wal` /
`-shm` / `-journal` 只要还在——只有「崩在 2 和 3 之间」的 store 才可能带着它——就说明有一个这个 core
不认识的进程按**新名字**开过它,而下面这次 checkpoint 走的是**老名字**、根本看不见那个 WAL,所以一律
`WFS_E_STORE_BUSY`、原样不动。)

**第 3 步先探针、后写代码**(scratchpad,APFS,本机):

```
before swap: metadata.db ino=311809154 nlink=2, metadata3.db ino=311809154
renameatx_np(dir <-> regfile, RENAME_SWAP) = 0 (ok)
after swap: metadata.db isdir=1 mode=500; stub_tmp isreg=1 size=14 ino=311809154 nlink=2
unlink(stub_tmp) = 0
metadata3.db nlink=1 size=14; metadata.db isdir=1
-- revert --
link(metadata3.db, tmp2) = 0
swap back = 0 (ok)
metadata.db isreg=1 size=14 ; tmp2 isdir=1
```

目录保住 0500,文件保住 inode / 大小 / 链接数,**两个方向都行**。本项目的 publish 本来就在用
`renameatx_np(RENAME_EXCL)`(P7/P8),这只是换一个 flag;为了不让唯一一处不可移植的调用散在
`store.cpp` 里,它封成 `wfs::fs_rename_swap()`(macOS `RENAME_SWAP`,Linux
`renameat2(RENAME_EXCHANGE)`,两样都没有就 `-ENOSYS` 并拒绝升级)。**没有「无缝的退路」**:任何
一串普通 `rename` 都有一个瞬间某个名字是空的——那正是这一轮要修的东西,所以退路只能是这个交换本身。

##### 回滚也反着走这四步

```
link(metadata3.db, stub_tmp)  ->  SWAP stub_tmp <-> metadata.db  ->  unlink(metadata3.db)
                              ->  rmdir(stub_tmp)  ->  VERSION := 2
```

每一步同样让 `metadata.db` 有人占着:交换之前它是那个空目录,交换之后它就是库(的第二个链接)。
任一步失败就停在原地,留下的仍然是下面那张表里可以接着做的形状;上一轮那条「库旁边还有
`-wal`/`-shm`/`-journal` 就**不回滚**」的规矩原样保留。

##### 崩溃态:两个新形状,判据还是布局

| `metadata.db` | `metadata3.db` | 是什么 | 这次 open 做什么 |
|---|---|---|---|
| 不在 | 不在 | 一个没有库的 store | 有树 → `WFS_E_STORE_DAMAGED`;没树 → 新建 |
| 普通文件 | 不在 | schema 2,或者崩在第 2 步之前 | `VERSION` 是 2 就先 (a),然后 1–4、(d) |
| 普通文件 | **同一个 inode** | 崩在第 2 步和第 3 步之间 | 同上;`link()` 发现自己那步已经做过了,接着从第 3 步走 |
| 普通文件 | 另一个 inode | 协议产生不出来 | `WFS_E_STORE_DAMAGED` |
| 不在 | 在 | 空目录被人删了的 schema 3 store | `VERSION` 是 2 就先 (a),然后补空目录、(d) |
| 目录 | 在 | schema 3;或者崩在 (d) 之前;或者崩在第 3、4 步之间(`st_nlink` 是 2) | `VERSION` 是 2 就先 (a),然后 (d);`st_nlink > 1` 就顺手把那条多余链接扫掉 |
| 目录 | 不在 | 空目录旁边没有库 | `WFS_E_STORE_DAMAGED`,绝不新建 |

**不变量**:凡是这个协议能产生的、**有库**的状态,`metadata.db` 都**存在**——要么是库本身,要么是库的
第二个链接,要么是那个空目录。表里两行「不在」是「还没有库的新 store」和「空目录被人删掉的 store」,
都不是这个 core 开出来的窗口(`db_move_to_schema3()` 里在交换之后还当场 `stat` 确认它是目录)。

##### 清扫:`metadata.db.stub.` 是第二个前缀

按 P18 那条规矩(在 `<store>` 里、不进子目录、前缀只有我们自己写):**空目录** `rmdir`(崩在第 3 步
之前那个没用上的),**inode 和 `metadata3.db` 一样的普通文件** `unlink`(崩在第 3、4 步之间那条多余
链接——这就是迟到的第 4 步),别的一概不碰。什么时候扫:`metadata.db` 还是普通文件,或者库的
`st_nlink > 1`——正常那条路两个都不成立,**一次 readdir 都不做**。并发的另一个升级者的 stub 有可能被
扫掉,那么它那次交换失败、它那次 open 重试,和第三十一轮 `VERSION.tmp` 那次清扫做的是同一笔交易。

##### 先跑红

老办法:把断言换成打印,跑在**修改前**的 core 上(`scratchpad/r36/red36.c`,链接 `64a64b1` 编出来的
`libworldfs_core.a`,并把缝临时挪到 `rename` 和 `mkdir` 之间)。库里先放两行 `worlds`:

```
before: VERSION=2, metadata.db user_version=200, 2 rows in worlds
seam ran: 1
M1-shaped sqlite3_open_v2("<store>/metadata.db", RW|CREATE): rc=0 (not an error)
  SELECT count(*) FROM worlds = -1   (-1 = no such table: a FRESH EMPTY database)
  PRAGMA user_version = 0
wfs_store_open rc=-1017 (the store has trees in it but no readable metadata3.db)
<store>/metadata.db  exists=1 isdir=0 size=0
<store>/metadata3.db exists=1 user_version=200
```

`count(*)` 那一行是这一轮的全部证据:一瞬间之前还有两行的库,在 M1 眼里连 `worlds` 表都没有——它
打开的是 SQLite **刚刚替它新建**的那个空库。修完之后同一个窗口(缝现在是三步制的第 1 步):
`rc=0`、`count(*)=2`、`user_version=200`,拿到的是**真库**,而这次 open 以 `WFS_E_STORE_BUSY` 整体
回滚;第 2、3 步那两个窗口是 `SQLITE_CANTOPEN`,升级照常做完。

##### 新增测试

- `core_test` 新缝 `wfs_test_between_db_steps(ctx, dir, phase)`(`worldfs.h`,非测试运行里恒为 NULL),
  在第 2、3、4 步之后各开一次火。缝里 **fork 一个子进程**做 M1 的那句 open
  (`READWRITE|CREATE|FULLMUTEX`)——因为升级下一步要问的正是「**别人**谁开着」,而
  `fs_other_holders()` 按定义排除自己的 pid;子进程把 `rc` / `SELECT count(*) FROM worlds` /
  `PRAGMA user_version` 通过管道报回来,然后**攥着句柄不放**,直到父进程放它走。
- 第 1 步(link 之后):`rc == SQLITE_OK`、`count(*) == 2`(**不是** 0、**不是** -1)、`user_version`
  是 200;这次 `wfs_store_open()` 回 `WFS_E_STORE_BUSY`,而且整体回滚——`VERSION` 回 2、库是
  `metadata.db` 上的普通文件、`nlink == 1`、两行还在、一个增量列都没加、没有 `metadata3.db`、
  `metadata.db.stub.` 前缀下一个都不剩;放走子进程之后同一句 open 照样把 store 接管过来。
- 第 2、3 步:`rc == SQLITE_CANTOPEN`,什么都没读到、什么都没建;这次 open 照常成功。
- 三种情况收尾都断言同一件事:`metadata.db` 是 0500 空目录、`metadata3.db` 是普通文件且 `nlink == 1`、
  两行 `worlds` 一行不少、`user_version` 3xx、`metadata.db.stub.` 和 `metadata.db-` 前缀都是 0 个。
- 手搓两个崩溃态:①「link 完没交换」(两个名字同一个 inode,外加一个没用上的 stub 空目录)→ 接着做完,
  stub 被扫掉;②「交换完没 unlink」(`nlink == 2`,`user_version` 戳回 200)→ 接着做完,多余链接被扫掉。
- 再加一个「M1 store 旁边一个陈年 stub 空目录」→ 升级照常,空目录被扫掉。
- 第三十四轮那个子进程持有者的用例补两条断言:回滚之后库的 `nlink == 1`、`metadata.db.stub.` 前缀下
  一个都不剩(回滚现在也走交换)。

**验收**:`ctest`(WFS_FSKIT=OFF)**2/2**;`safety.sh` **298 passed, 0 failed**;`check-deps.sh`
全绿(5 个系统库)。(`m1_criteria.sh` 本轮跳过:没有碰到它量的任何东西。)

#### PR #1 review 第三十五轮:库的名字就是 schema 的一部分、删不掉的临时名还是一条链接(2026-09-20)

第三十五轮,Codex 一条 P1 一条 P2。P1 把前两轮那道门补完:**一个只拦得住「还没进门」和「已经站在
屋里」的防线,漏掉的是「已经验过票、还没推门」的那个人**;P2 是同一种形状的另一头——**一个失败了
没人读的清理,让下一次重试变成了第二条链接**。

| # | 位置 | 问题 | 修法 | 提交 |
|---|---|---|---|---|
| P1 `PRRT_kwDOUf7jGc6kGzla` | `core/src/store.cpp` `wfs_store_open()`(~443) | 第三十二轮拦的是抬 `VERSION` **之后**才启动的 M1,第三十四轮拦的是**已经拿到描述符**的 M1。中间还有一种:读完 `VERSION` 拿到 2、被放行、**却还没跑到自己那句 `sqlite3_open_v2()`** 的进程。它手上什么都没开,`proc_listpidspath(3)` 看不见;它已经读过那唯一一个文件,抬文件也拒不到它。恢复执行之后,它打开的是刚刚被迁移过的库,把 `user_version` 戳回 2,然后跑 M1 的 collector | M1 之后不再读任何别的文件,所以能拒它的只剩**库本身**:schema 3 的库改名成 `<store>/metadata3.db`,M1 认的 `<store>/metadata.db` 变成一个**空目录(0500)**。实测(SQLite 3.54.0):对目录 `sqlite3_open_v2` 在 `READWRITE\|CREATE`、`READWRITE`、`READONLY` 三种模式下一律 `SQLITE_CANTOPEN`(14) | `7e978b7` |
| P2 `PRRT_kwDOUf7jGc6kGzld` | `core/src/hardlinks.cpp` `relink_at()` / `relink_under_lend()`(~547) | 克隆出来的目录带着 `UF_APPEND` 时,`linkat` 出临时名**成功**(加东西正是这个标志允许的),`renameat` 被拒,收尾的 `unlinkat` 同样被拒**而且返回值被丢掉**;第十八轮那条「借目录写位重试」换一个新临时名重新 link + rename,**成功了**。没有任何一步报错,快照照常发布,正身 inode 上比组的成员数多一条链接 → `hardlinks_verify_groups()` 要求 `nlink` **正好**等于成员数,于是这个 `snapshot create` 回 0 的快照永远 `WFS_E_SNAPSHOT_DIRTY` | `relink_at()` 在 unlink 也失败时把临时名**交回调用者**;借到写位之后**第一件事**是 `unlinkat` 掉它,然后才重试。连借着写位都删不掉就是致命错(`first_err` → 整棵克隆回退),而不是发布出去 | `2f6939f` |

##### 升级协议:五步,判据是磁盘布局而不是 `VERSION`(P1)

```
(a) VERSION := 3
(b) 打开老库 -> PRAGMA journal_mode=DELETE -> 关 -> unlink 三个 sidecar -> rename(metadata.db -> metadata3.db)
(c) mkdir(metadata.db, 0500)                    <- 空目录,M1 的 open 在这里 CANTOPEN
(d) fs_other_holders(metadata3.db) ? 整体回滚 + WFS_E_STORE_BUSY : 迁移 + 盖 3xx
```

**(b) 里 sidecar 是必须先处理的**:`main` 的 M1 每次 open 都跑
`PRAGMA journal_mode=WAL`(读过 `main:core/src/store.cpp` 的 `kPragmas` 和 `wfs_store_open()`),
所以一个活着的 M1 store 一定带着 `metadata.db-wal` / `metadata.db-shm`,被打断在设 WAL 之前的还可能
带一个回滚日志 `metadata.db-journal`。只 rename 库本身会把这三个留在一个**同名目录**旁边——一个
数据库已经不在了的热日志,是这件事里唯一会丢数据的形状。所以先用本 core 的连接打开老库(这一次打开
本身就让 SQLite 回放热日志),`PRAGMA journal_mode=DELETE` 把 WAL checkpoint 回主库并删掉
`-wal`/`-shm`;pragma 的答案要**读**:别人占着 WAL 时 SQLite 是用一个「wal」的答案、而不是错误码
告诉你没改成,不是 `delete` 就按 `WFS_E_STORE_BUSY` 拒,库还留在 M1 认得的名字上。关掉之后把三个
sidecar 名字一并 unlink(内容已经都在主库里),**然后**才 rename。

**(d) 问的是新名字,而它依然找得到旧名字上的持有者**——scratchpad 探针验过:

```
child 61627 opened old name: y
holders(metadata.db) before rename: 1 61627
holders(metadata3.db) AFTER rename: 1 61627      <- proc_listpidspath 比的是 vnode,不是名字
holders(metadata.db) after rename: -1 (ENOENT)   <- 所以问题必须问新名字
holders(metadata3.db) after stub dir: 1 61627
holders(metadata3.db) with child gone: 0
```

回滚按相反次序:`rmdir` 空目录 → `rename` 回去 → `VERSION` 放回 2。任一步失败就停在原地,留下
`VERSION` 3 压着一个 M1 两个名字都够不到的库——被 M1 拒、被下一次 M2 open 做完,和第三十二轮同一笔
交易里安全的那一半。库旁边只要还有 `-wal`/`-shm`/`-journal` 就**不回滚**:一个留在 schema 3 名字上的
WAL 配着一个在 schema 2 名字上的库,等于让 M1 读一个缺了整段已提交页的库,比任何一次拒绝都糟。

**崩在任何一步都能接着做**,而且判据是布局:

| `metadata.db` | `metadata3.db` | 是什么 | 这次 open 做什么 |
|---|---|---|---|
| 不在 | 不在 | 一个没有库的 store | 有树 → `WFS_E_STORE_DAMAGED`;没树 → 新建 |
| 普通文件 | 不在 | schema 2,或者崩在 (b) 之前 | `VERSION` 是 2 就先 (a),然后 (b)(c)(d) |
| 不在 | 在 | 崩在 (b) 和 (c) 之间 | `VERSION` 是 2 就先 (a),然后 (c)(d) |
| 目录 | 在 | schema 3——或者崩在 (d) 之前 | `VERSION` 是 2 就先 (a),然后 (d) |
| 目录 | 不在 | 空目录旁边没有库 | `WFS_E_STORE_DAMAGED`,绝不新建 |
| 普通文件 | 在 | rename 是原子的,协议产生不出来 | `WFS_E_STORE_DAMAGED` |

`VERSION` 不在表里,因为它不决定任何事:**布局决定**,文件只说 (a) 还欠不欠。唯一可能两读的方向是
「`VERSION` 还是 2 而 `metadata3.db` 已经在」(一次没做完的回滚),**一律往前做**——库已经搬过去了,
再搬回来是第二次无同步的 rename,而把文件抬到 3 才让 store 的两半重新一致。回滚方向只由**手里正握着
这次升级**的那个进程走(`legacy_holders_gate()`),之后读这些名字的 open 一律不走。

持有者闸门也只有欠着迁移的 store 付:`user_version` 还在 2xx、而且库在这次 open 开始前就已经在磁盘上,
才问 `proc_listpidspath(3)` 那 ~106 ms;盖了 3xx 的 store 和这次 open 自己刚建的库都不问。

##### M1 那边到底会发生什么(引 `main`)

`main:core/src/store.cpp` 的 `wfs_store_open()`:

```c++
String dbp(s->dir);
dbp.append("/metadata.db");
int rc = sqlite3_open_v2(dbp.c_str(), &s->db,
                         SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
if (rc != SQLITE_OK) { wfs_store_close(s); return -EIO; }
```

`SQLITE_OPEN_CREATE` 只创建**不存在**的库,它不会在一个目录旁边另起炉灶——路径上是目录,
`sqlite3_open_v2` 直接 `SQLITE_CANTOPEN`,这里就 `-EIO` 返回,句柄不发。M1 的 `wfs_gc()` 是
`wfs_store_open()` 成功之后才被 CLI 调用的(它的第一个参数就是那个 `wfs_store *`),所以 open
失败等于 collector 根本没有机会跑;M1 也没有 P17 那种「有树没库」的守卫——它连 `store_has_trees()`
都没有,`main` 的 open 里在 `sqlite3_open_v2` 之前只有 `check_version()` 和六个 `mkdir`。
也就是说:这条路径上 M1 不会删任何东西,它只是打不开。

##### 波及面

`.world` 标记和 FSKit 扩展都是通过 core 读 store 的(`wfs_store_open()` / `wfs_world_marker_*`),
库改名对它们不可见,本轮一行 FSKit 代码都没动。改到的是:`world fs status` 现在印
`schema: 3 (store <id>), database metadata3.db`;`WFS_E_STORE_DAMAGED` 的文案和 CLI 的出路提示改成
`metadata3.db`;`safety.sh` 里直接 `sqlite3` 戳库的 11 处、P17 那一段、`core_test`/`diff_test` 里
同样的 13 处、`scripts/bench/m1_criteria.sh` 的库大小统计,全部跟着改名。

##### 先跑红

两条都用「把断言换成打印、跑在修改前的 core 上」的老办法(`scratchpad/red_f1.c`、`red_f2.c`,
链接的是 `66524e8` 上编出来的 `libworldfs_core.a`):

```
##### F1 #####
before: VERSION=2, metadata.db user_version=200
M1-shaped sqlite3_open_v2("<store>/metadata.db", RW|CREATE) in the gap: rc=0 (not an error)
user_version after M1's `PRAGMA user_version=2`: 2
##### F2 #####
snapshot create rc=0 sid=1
snapshot d/a nlink=3 (want 2)
leftover .wfs-hl-* under the snapshot's d/: 1 .wfs-hl-62455-0-0d6bc0539ff1e512
verify rc=-1008 modified=2
```

修完之后同一个探针:F1 那一句 open 在同一个窗口里是 `rc=14 (unable to open database file)`、
什么都没戳到;F2 是 `verify rc=0`、`nlink=2`、`leftover 0`。

新增测试:

- `core_test` 版本那一段新增一个新缝 `wfs_test_after_db_move`(`worldfs.h`,非测试运行里恒为 NULL),
  它正好在 (c) 之后 (d') 之前(库已搬、空目录已建、持有者闸门已过、还没迁移)开火;缝里做的就是
  M1 的那句 open(`READWRITE|CREATE|FULLMUTEX`)加 `PRAGMA user_version=2`,断言它回
  `SQLITE_CANTOPEN`、什么都没戳到,而这次 open 本身照常成功、第二次 open 也照常成功。
- 布局断言:升级完之后 `metadata.db` 是一个 0500 的空目录、`metadata3.db` 是一个非空普通文件、
  `metadata.db-` 前缀的 sidecar 一个不剩。
- 崩溃态逐个走一遍:上表的六行,加上「`VERSION` 2 + `metadata3.db` 在」(往前做)和
  「`VERSION` 2 + 库已经是 3xx」(老次序留下的那种)。
- 第三十四轮那个子进程持有者的用例扩展成**整体回滚**的断言:`VERSION` 回到 2、库是**普通文件**
  回到 `metadata.db`、没有空目录挡路、没有 `metadata3.db`、一个增量列都没加;
  `wfs_store_holders()` 照样报得出那个 pid(它现在先问 `metadata3.db`、没有再问 `metadata.db`)。
- P17 那一段跟着改:空目录 + 没有 `metadata3.db` → `WFS_E_STORE_DAMAGED` 且什么都不建;
  把空目录也删掉之后才是第十三轮那个「`snapshots/` 读不出来 → 原样返回 errno」的形状。
- `core_test` 新的 `flag-dir-uappnd` 一段(F2):源树里一个带 `UF_APPEND` 的目录 `d/` 装着一对
  硬链接 `(a, b)` → `snapshot create` 成功、`verify` 干净、快照里 `d/a` 的 nlink **正好 2**、
  `d/` 下没有 `.wfs-hl-*`;从它 fork 出来的世界同样把这一对重建出来、也没有残骸。收尾把标志清掉。

**验收**:`ctest`(WFS_FSKIT=OFF)**2/2**;`safety.sh` **298 passed, 0 failed**;`check-deps.sh`
全绿(5 个系统库)。(`m1_criteria.sh` 本轮跳过,只跟着改了库名。)

#### PR #1 review 第三十四轮:没开起来的事务、已经在屋里的人、按字节读的路径(2026-09-20)

第三十四轮,Codex 两条 P1 一条 P2。两条 P1 是同一个主题的两半:**一个防线只有在它真的成立的时候
才是防线**——一个失败了没人读的 `BEGIN IMMEDIATE`,和一个只挡「后来者」的 `VERSION` 文件。

| # | 位置 | 问题 | 修法 | 提交 |
|---|---|---|---|---|
| P1 `PRRT_kwDOUf7jGc6kGghX` | `core/src/db.h` `Txn` 构造函数(~102) | 第十一轮把 `BEGIN IMMEDIATE` 的返回码记进 `begin_rc`,**只有 schema 迁移一个调用者会读**。busy timeout 熬完的 `SQLITE_BUSY`、库上的 `SQLITE_IOERR` 之后,调用者的语句全跑在 autocommit 里,析构那句 `ROLLBACK` 回滚的是「什么都没有」——「检查和改动是同一个写事务」(P12;第一、五、十五、十六、二十一轮)整体失效:`discard` 的引用计数和它的 UPDATE 成了两次写,一个 fork 的 CREATING 行正好塞得进中间 | `ok()`/`err()`,**41 个 `Txn` 构造点每一个**一行 `if (!t.ok()) return t.err();`(或那一处的「什么都不做」),41 处 `t.ok()`,可以 grep;`commit()` 返回 rc 且 `[[nodiscard]]`;BEGIN 失败的连接记进线程局部指针,`Stmt` 在它上面拒绝 prepare——忘了写检查的地方「什么都不写」,而不是「写在事务外」。`open_` 跟着 BEGIN 走。`err()` 把 BUSY 映射成 `-EBUSY` | `cce6759` |
| P1 `PRRT_kwDOUf7jGc6kGghY` | `core/src/store.cpp` `wfs_store_open()`(~724) | 第三十二轮把门关在了迁移之前,挡住的是**抬文件之后才启动**的 M1 进程。一个在抬文件**之前**刚刚 `wfs_store_open()` 完的 M1 进程,手上那个句柄照样能用——而 M1 全程不持有任何 store 级的锁,所以排他不能指望 M1 配合 | 抬文件之后、迁移之前,问操作系统谁还开着 `<store>/metadata.db`(`wfs::fs_other_holders()`;macOS 用 libproc 的 `proc_listpidspath(3)`,Linux `-ENOSYS` 桩)。有外来持有者就把 `VERSION` 放回 2 并返回新的 `WFS_E_STORE_BUSY`;CLI 把 pid 和可执行文件路径都说出来(`wfs_store_holders()`) | `49312cf` |
| P2 `PRRT_kwDOUf7jGc6kGghZ` | `macos/fskit/WorldVolume.mm`(~52)、`WorldVolumeHandler.mm` | marker 里的 store 路径用 `+[NSString stringWithUTF8String:]` 解码:合法但不是 UTF-8 的路径分量会让它返回 **nil**,下一行的 fallback 于是打开了扩展自己的默认 store——而 CLI 的挂载预检(第十七轮)是按**字节**比对的,它放行了 | 两处都改用 `-[NSFileManager stringWithFileSystemRepresentation:length:]`;连它都答 nil 就带 `EILSEQ` **让卷加载失败**。marker 里**有**路径时,永远不再退回默认 store | `ed37971` |

##### 次序即完备性(P1 第二条)

```
抬 VERSION 到 3  ->  列举谁开着 metadata.db  ->  有人 ? 放回 2 并拒绝
                                                : 迁移
```

在列举**之后**才出现的持有者,是在 rename 之后启动的,文件已经拒了它(第三十二轮);列举**列到的**
持有者,是在抬文件之前被放进来的——正是这条 finding 说的那一批。没有第三种。外来持有者一律同等对待:
另一个 M2 升级者和一个 M1 进程按 pid 分不出来,而两个升级者互相拒绝不过是「一个 store 第一次 open
时重试一次」。**「问不出来」算持有者**(平台报错,或者问不了的平台的 `-ENOSYS`),因为放进来才是
不可挽回的那一边。第三十一轮那个两句柄竞争测试**不用改**:两个句柄是同一个进程,而列举按定义排除
自己的 pid。libproc 先在 scratchpad 里探过:非特权可用,列得出本用户自己那个开着文件的子进程、
子进程回收之后就不再列出来,~900 个进程约 106 ms——只在一个 store 一生中接管它的那一次 open 上付。

##### 纵深防御:M1 的 `wfs_gc()` 到底会删掉 M2 的什么(Codex 让一并分析)

`main` 上的 `wfs_gc()` 里,「该留下的 trash 条目」(`keep`)**只**来自
`SELECT id, trash_path, trashed_at, path FROM worlds WHERE state=2`。`<store>/trash` 底下凡是
不在这张表里的一级条目,一律 `fs_remove_tree` 整棵删掉。于是:

| M2 的东西 | M1 会怎样 | 后果 |
|---|---|---|
| 快照的 trash 条目 `S<n>-<ts>`(T2.2) | M1 **从不**查 snapshots 表的 `trash_path`,所以每一个都是「没有行认领的孤儿」 | **不看保留期,立刻整棵删**。保留期内的快照再也 restore 不回来,从它 fork 出来的世界失去 diff/verify 的基线 |
| `state=4`(`WFS_ST_TRASHING`)的世界/快照条目 | M1 只查 `state=2`,所以这条行的 `trash_path` 不在 `keep` 里;rename 已经发生的话树就在 trash 里 | **删**。行永远停在 TRASHING 指着空气;之后 M2 的 `trashing_recover()` 读到「树两处都不在」,判成「rename 从没发生」,把世界放回 **ACTIVE**——一个树已经没了的 ACTIVE 世界 |
| `<entry>.deleting` | 世界那侧:行更新和 rename 在同一个事务里(第十五轮),所以 `keep` 里就是 `.deleting` 这个名字,保留期内**保得住**;快照那侧同上表第一行 | 不额外新增损失,但会把一个正在跑的 M2 collector 脚下的树删掉 |
| 跨卷 trash `<parent>/.wfs-trash/W<id>-<ts>` | M1 只 readdir `<store>/trash`,够不到 | 安全(过了保留期按行删是 M1/M2 一致的正确行为) |
| `state=0` 的快照行 + `S<n>.wfs-tmp` / `S<n>` | M1 照删,但**没有 owner 存活判定**(第三轮加的 `owner_pid`/`owner_start` 它不认),也没有最小年龄 | **删掉一个正在跑的 `snapshot create` 的半成品树**,并删掉它的行 |
| `state=0` 的世界行(正在跑的 fork) | M1 `remove_tmp(path)` 按 `<path>.wfs-tmp` 猜临时名(M2 用的是行上记的 `tmp_path`,名字是 `.wfs-fork-<hex>`,猜不中),但 `DELETE FROM worlds WHERE id=? AND state=0` 照删 | **树留下、行没了**:`--to` 上出现一棵没有任何行认领的树;pool 供的那种 fork 还会让 pool 条目变成无主,被 `pool_collect` 在活着的 fork 脚下删掉 |
| pool 的 `DRAINING`(`state=2`) | M1 的 `pool_collect` 是 `doomed = state != 1` | 删树删行——和 M2 drain 想要的结果一致,不算损失;但它同样会删掉一个**活着的** filler 正在克隆的 `state=0` 条目 |
| `<store>/tmp`、`<store>/VERSION.tmp.*` | M1 只清 `<store>/tmp` 里超过一小时的**非目录**;`VERSION.tmp.*` 在 `<store>` 根上,M1 不 readdir 那里 | 安全 |

**「把 M2 的 trash 挪到一个 M1 不扫的目录」够不够?** 不够,但挡得住最贵的那一半。M1 的孤儿扫描
只 readdir `<store>/trash` 这**一层**,并且跳过 `.` 开头的名字——所以放进 `<store>/trash/.m2/`
(或者 `<store>/trash2/`)之后,上表**第一、二行**(快照条目、TRASHING 条目)对 M1 **完全不可见**,
也就是不可挽回的那部分损失没有了。挡不住的是**创建那一半**:`state=0` 的世界行和快照行是按**行**
和 store 外的路径删的,跟 trash 放在哪里无关。而且这是一次磁盘布局变更(T2.2 的名字写在文档里、
出现在 `gc --status` 的输出和 `safety.sh` 的断言里),所以**记在这里作为后备方案**,不实现:
真正的防线是上面那道持有者闸门,它连「创建那一半」一起挡住。

**验收**:`ctest`(WFS_FSKIT=OFF)**2/2**;`safety.sh` **298 passed, 0 failed**;`check-deps.sh`
全绿(5 个系统库,libproc 属于 libSystem);FSKit 两处改动在单独的 `build/fskit-check`
(`-DWFS_FSKIT=ON`)里编过,没有打包、注册或挂载。(`m1_criteria.sh` 本轮跳过。)

**先跑红**(把断言换成打印,跑在修改前的 core 上):

```
[F1 a] discard rc=0 state=2 (TRASHED) tree_at_S<n>=0 trash/S*=1
[F1 b] fork    rc=0 world=1 tree=1 rows=1
[F1 c] gc      rc=0 armed=1 worlds_deleted=1 state=3 (DEAD) trash/W*=0
[F2  ] open    rc=0 handle=1 VERSION=3 user_version=304 cols=1
```

——F1:BEGIN 一失败,discard 照样把快照搬进了 trash、fork 照样发布了一整个世界、collector 照样
把 trash 条目改名删掉并把行埋了,三次都返回 0;F2:M1 那种持有者还活着,open 报成功、句柄照发,
store 被接管到 schema 3(`user_version=304`、列都加上了)。

新增测试:

- `core_test` 新的 `txn-store` 一段,新缝 `wfs_test_txn_fail_once`(`worldfs.h`,非测试运行里恒为 0):
  (a) `discard S<n>` → `-EIO`,快照仍 ACTIVE、树仍在 `snapshots/S<n>`、trash 里没有 `S*`;同一个
  调用把缝设成 `SQLITE_BUSY` 时是 `-EBUSY`。(b) `--no-pool` 的 fork → `-EBUSY`,没有世界、没有行、
  目标目录上没有临时树;撤掉缝同一个调用成功。(c) 一个在 trash 里的世界 + retention 0 的 `gc`,
  缝在 `wfs_test_before_trash_delete` 里装弹(它正好在 `gc_claim_deleting()` 之前)→ `worlds_deleted=0`、
  行仍 TRASHED、条目还在它自己的名字上(没有 `.deleting`);下一次不装弹的 wake 正常收掉。
- `core_test` 版本那一段新增 (e):一个 schema 2 的 store,一个**子进程**开着它的 `metadata.db`
  (通过管道确认真的开到了,并且用第二根控制管道保证父进程一走它就退出)→ `wfs_store_open` 返回
  `WFS_E_STORE_BUSY`、不发句柄、`VERSION` 回到 2、`user_version` 仍是 2xx、一个增量列都没加、
  没有 `VERSION.tmp` 残骸;`wfs_store_holders()` 报出这个子进程的 pid 和可执行文件路径;子进程
  收掉之后,同一个 open 正常接管(`VERSION` 3、`user_version` 3xx、列齐)。
- F3 没有可执行的测试:前端是冻结的、默认不编译,证据就是那次单独的编译检查。

#### PR #1 review 第三十三轮:帮手的错误就是调用者的错误(2026-09-20)

第三十三轮,Codex 两条 P2,形状一模一样:**一个返回 int 的帮手失败了,调用者把它的返回值扔掉,
然后报成功**。上一轮(第三十二轮)教会了这些帮手「读失败」和「没有行」不是一回事;这一轮管的是
它们把失败讲出来之后,**没有人听**。

| # | 位置 | 问题 | 修法 | 提交 |
|---|---|---|---|---|
| P2 `PRRT_kwDOUf7jGc6kGYCC` | `core/src/store.cpp` `wfs_store_open()`(~754) | `trashing_recover(s, nullptr, nullptr)` 的返回值被丢掉:recovery 的查询失败(I/O、损坏、熬过 busy timeout 的 BUSY)时,TRASHING 行原封不动留着,而 open **报成功**——正好违背它自己那句「这件事必须在这个进程读任何状态、给 trash 分类之前完成」 | 查 rc:非 0 就 `wfs_store_close(s)` 并把 errno 还回去(CLI 打印 errno / `WFS_E_STORE_DAMAGED`,用户重跑一次)。`wfs_gc_ex()` 开头那次同样的调用一并改掉——collector 更不能在一个它没能整理好的 store 上做分类 | `f215b77` |
| P2 `PRRT_kwDOUf7jGc6kGYCF` | `core/src/world.cpp` `wfs_gc_status()`(~4162) | `pool_stranded(...)` 的返回值被丢掉:pool 的分类查询失败时,报表里 `pool_stranded` 是 **0**——「<store>/pool 底下没有陈旧条目」,而真相是「没问出来」 | 传播。`gc --status` 是诊断命令,一句错的「干净」比一个能照着做的错误更糟,而且这里没有「局部」可以抢救:失败的是分类本身,不是四个目录里的一个 | `231f5d0` |

##### `SQLITE_BUSY` 要不要重试(Codex 在 P2 里专门问了)

**不要,busy timeout 已经把它盖住了**。`wfs_store_open()` 在 `sqlite3_open_v2` 之后、读任何东西
之前就 `sqlite3_busy_timeout(s->db, 10000)`,这个超时对 `trashing_recover()` 的两次 SELECT 和它
写回时那次 `BEGIN IMMEDIATE` **一样有效**(P12:写锁在 BEGIN 就拿,所以两个 `world` 进程是排队,
不是半路 BUSY)。熬过 10 秒争用的东西已经不是「瞬时」了,在它上面再套一个重试循环,等于在 SQLite
自己那个超时之上再加一个超时。另外 `trashing_recover()` 本身也不会返回 `-EBUSY`:它按第三十二轮的
写法一律答 `-EIO`,真正的 BUSY 是被 timeout 吃掉之后才会走到那里。而 store open 是这个 core 里
最便宜的可重复动作——失败就是让用户再敲一次命令。

##### 同形状的全路径审计(open / gc / gc --status / gc --pending / CLI)

| 位置 | 原来 | 现在 |
|---|---|---|
| `wfs_store_open()` → `trashing_recover()` | 丢 rc,报成功 | **传播**(P2 本体) |
| `wfs_gc_ex()` → `trashing_recover()` | 丢 rc,接着分类 | **传播**:读不出 TRASHING 行,它的树在下面那趟孤儿扫描里就是「没有行认领」,而无主孤儿是**立刻删**的 |
| `wfs_gc_status()` → `pool_stranded()` | 丢 rc,报 0 | **传播**(P2 本体) |
| `wfs_gc_status()` → `fs_free_space()` | 丢 rc,`volume: 0 B free` | **传播**(`wfs_store_status()` 里同一个调用从第一天起就是传播的) |
| `wfs_gc_ex()` → `rm_tmp_in_store_dir()` | 丢 rc | **传播**:它只在「这不是我扫的那个目录」时返回非 0,而一次什么都没扫的 run 不该报一个它没看过的干净 store |
| `wfs_gc_ex()` → `pool_collect()` | 丢 rc,报 `pool_removed 0` | **传播**:分类是它做的第一件事、也是唯一会失败的一件事,所以非 0 == 一根指头都没动 |
| `wfs_gc_ex()` → `--no-trash` 分支的 `trash_scan()` | `if (... == 0) {...}`,没有 else:失败时 rc 被丢掉且 `work_remains` **不置位** | **传播**:不置 `work_remains` 正好是唯一能让 worker 链不再回来的事 |
| `wfs_gc_pending()` → `trash_scan()` | 失败答 **0**(「没有待办」) | 失败答 **1**。这个函数是 bool,没有错误通道,所以按第三十二轮那条规矩倒向**什么都不丢**的一边:起一个 collector,让它把 errno 报出来;答 0 则是把扫不到的活彻底埋掉(第三轮那个 bug,只是这次从 EIO 走过来)。不是空转:这句话只在一个刚刚成功读过同一个库的命令之后才问 |
| CLI `cmd_gc()` 的「第二意见」`wfs_gc_status`(unread 那条 note) | 丢 rc,`strerror(0)` 打出 `Undefined error: 0` | 看 rc:读不回来就说「读不回来」,不编一个理由出来 |
| CLI `cmd_gc()` 另两处第二意见(blocked / foreign 的路径) | 丢 rc | **有意不看**:结构体先 memset,用到的只有一个路径字段,失败自然落到「没有路径」那句措辞上——这里没有可以报错的东西。注释写清楚 |
| CLI `cmd_status()` → `wfs_gc_status(-1)` | `== 0 && (...)`:失败时**整行不打**,读起来正好是「trash 空的、没有 worker」 | **标成局部**:`trash:     not counted: <errno>`。`status` 是这一圈里唯一值得局部打印的报表(上面每一行都已经成功读出来了),所以失败是**说出来**,不是传播,也不是沉默 |
| CLI `cmd_gc()` → `wfs_gc_pending(s, retention, &running)` | 返回值不用,只要出参 | 不变:要的是 `running`,返回值本来就不是这里的答案 |
| `wfs_gc_status()` / `wfs_gc_pending()` → `gc_worker_probe()` | 返回 bool | 不变:它是对锁文件的探测,不是 errno 通道 |
| `wfs_store_open()` 的其余帮手(`store_has_trees` / `check_version` / `version_upgrade` / `migrate_schema` / `version_read` / `meta_get` / `meta_set`) | — | 第十一/十三/二十四轮已经全部传播,本轮逐个确认过 |
| `gc_dirs_unreadable_pending()` | 读失败答 `false` | 不变,第三十二轮就是这么定的:「说不准」永远不唤醒 worker 链,而它答 true 会让每一次 fork 都起一个进程 |

**验收**:`ctest`(WFS_FSKIT=OFF)**2/2**;`safety.sh` **298 passed, 0 failed**;`check-deps.sh`
全绿。(`m1_criteria.sh` 本轮跳过。)

**先跑红**(同一个 `wfs_test_stmt_fail_sql` 缝,把断言换成打印,跑在修改前的 core 上):

```
[a] wfs_store_open rc=0 handle=yes world state=4 (4 = TRASHING) seam=fired
[b] wfs_gc_status rc=0 pool_stranded=0 seam=fired
[b] unarmed         rc=0 pool_stranded=1
```

——(a) open 报成功、句柄照发,而那条 TRASHING 行一动没动;(b) 报表把「问不出来」报成了「没有」,
而同一个 store 问得出来的时候是 1。

新增测试(`core_test`,新的 `rc-store` 一段):

- (a) 一个 owner 已死、树还在原处的 TRASHING 世界行 + recovery 的 `FROM worlds WHERE state=4`
  失败一次 → `wfs_store_open` 返回 `-EIO`、`*out` **没被写**、行还是 `state=4`;缝一撤,下一次
  open 把它放回 ACTIVE,树还在它从没离开过的路径上。
- (b) 一个快照已经不存在的 pool 行(树还在)+ `pool_scan` 的 `FROM pool ORDER BY id` 失败一次
  → `wfs_gc_status` 返回 `-EIO`(而不是 0 带一个 `pool_stranded == 0`);缝一撤,同一个调用数到
  那条陈旧条目。

#### PR #1 review 第三十二轮:先关门,再搬家具;查询失败不是查询没结果(2026-09-20)

第三十二轮,Codex 一条 P1 一条 P2。P1 打在第二十四轮那次 schema 2 → 3 的**次序**上:门关得太晚。
P2 打在一件比它更底层的事上——这个 core 从第一天起就把「`row()` 返回 false」读成「行走完了」,
而 SQLite 从来没答应过这件事。

##### P1:`VERSION` 抬到 3 必须发生在数据库迁移**之前**

第二十四轮的结论是对的:M2 改变了 collector 可以删什么,所以必须抬 schema 大版本,让 `main` 上的
M1 二进制**打不开**这个 store。但那一轮的实现是

```
check_version()(文件说 2 → legacy)→ 数据库迁移(事务,提交时盖 user_version 3xx)→ VERSION := 3
```

M1 只认那个文件。于是**提交之后、rename 之前**这一小段里起来的 M1 进程,读到 2,被放进来,然后:

```c
// main:core/src/store.cpp,WFS_STORE_SCHEMA == 2
if (user_version != WFS_STORE_SCHEMA) {
    ...kSchema...;
    sqlite3_exec(s->db, "PRAGMA user_version=2", ...);
}
```

它把 3xx **戳回 2**,而且它拿到的句柄在文件变成 3 之后照样有效——接着就是 M1 的 `wfs_gc()` 跑在
M2 的 trash 语义上:`state=2` 的快照行的树不在它保护的名单里,一个还在保留期里的快照被整棵删掉。

| # | 位置 | 问题 | 修法 | 提交 |
|---|---|---|---|---|
| P1 `PRRT_kwDOUf7jGc6kGMj-` | `core/src/store.cpp` `wfs_store_open()`(~675) | 升级的两半次序反了:**先迁移、后抬文件**,中间留出一个「库已经是 M2、文件还说是 M1」的窗口,M1 在这个窗口里起来就被放进来,而且会把 `user_version` 戳回 2 | ①**文件先抬**:`version_upgrade()` 挪到第二十四轮那条「百位比我们新就拒」的检查**之后**、写数据库的任何一步**之前**(连 `journal_mode` 都还没设),rename 一落地,M1 的 `check_version()` 就开始拒绝这个 store;②新的中间态「文件 3 + 库 2xx」被认成**升级做了一半**:百位检查只拒**比我们新**的,2xx 在 3 底下照常走迁移(第十一轮起按列在不在幂等),于是「抬完文件、还没提交迁移」的崩溃由下一次 M2 open 收尾;③迁移之后**再读一遍**文件并要求它是 3;④老次序崩出来的「文件 2 + 库 3xx」也一并救回来——抬文件这一步现在不看库戳,照抬 | `25a231e` |

**两种崩法换了位置**,这是这次改动的全部内容:老次序崩完留下「文件 2 + 库 3xx」——每一个 M1
二进制都打得开,而且没有任何 M2 代码看得出它和一个真正的 M1 store 有什么区别;新次序崩完留下
「文件 3 + 库 2xx」——M1 一律被拒,M2 接着做完。迁移**失败**的 store 现在也停在「文件 3」上:代价是
M1 从此打不开一个它本来还处理得了的 schema 2 store,而这是这笔交易里安全的那一半。

**边界,说清楚**:这条防线只挡「启动」,挡不住「已经在跑」。M1 全程不持有任何 store 级的锁
(`gc.lock` 是 M2 才加的;M1 只有 `pool.lock`、per-world 的 `W<n>.lock` 和快照 manifest 上那把),
所以**没有任何锁可以拿来等一个在飞的 M1 命令**,也没有任何文件能挡住一个在第一次 M2 open **之前**
就拿到句柄的 M1 进程。换新二进制之前,先把老二进制全停掉——这句话现在写在 M1_DESIGN.md P13 里。

**并发的两个 M2 首次 open**:两个都抬文件(私有临时名 + 幂等 rename,第三十一轮),两个都跑迁移
(`BEGIN IMMEDIATE` 序列化,后进来的那个发现列全在),谁也不多做什么——这一轮没有改变这件事。

##### P2:`row()` 的 false 里混着「读失败」

`Stmt::row()` 是 `sqlite3_step(s) == SQLITE_ROW`。prepare 成功之后,step 照样会失败:SQLITE_IOERR
(数据库文件本身的 I/O 错)、熬过 busy timeout 的 SQLITE_BUSY、SQLITE_NOMEM、SQLITE_CORRUPT——
这些和 SQLITE_DONE 一样,都是 false。于是 `while (q.row())` 出来的是一个**少数了几行**的计数,
`if (!q.row())` 出来的是一句**「没有任何行认领它」**,而这两句正好是这个 core 里每一个**破坏性**
步骤的前提:快照的引用计数、「还有行认领这个 trash 路径吗」、「这个 pool 条目还该死吗」。
一次 EIO 落在对的地方,gc 就会删掉活着的世界正在用的基线。

| # | 位置 | 问题 | 修法 | 提交 |
|---|---|---|---|---|
| P2 `PRRT_kwDOUf7jGc6kGMkB` | `core/src/world.cpp` `snapshot_refs_locked()`(~2531)以及所有「拿查询结果决定要不要删」的地方 | `row()` 把 step 期的错误报成「没有更多行」,判决因此可以凭一次读失败成立 | `Stmt` 记住最后一次 step 的结果:`row()` 对只要行的调用者一字不变,新增 `done()`(上一次 step 是 SQLITE_DONE)和 `err()`。然后**全仓审计 66 处**(`world.cpp` 36、`pool.cpp` 16、`store.cpp` 14;`diff.cpp` 一处 `Stmt` 都没有),统一成三个可 grep 的写法:决定什么的循环之后 `if (!q.done()) return -EIO;`;单行查找 `return q.done() ? -ENOENT : -EIO;`;答 bool 的帮手,失败倒向**什么也不删**的那一边 | `4177393` |

改到的几处要紧的:`snapshot_refs_locked()`(discard 的引用计数,少一行就是「没人用」);
`trashing_recover()` 的两个 select 和它里面那次引用计数(失败过去读成「没人要这个基线」,于是把
discard **做完**——活着的世界的基线留在 trash 里等收);`claim_trash_paths()`(改成返回 int:少几行
就等于凭空造出一个孤儿,而孤儿是**立刻删**的);`trash_path_claimed_locked()` / `pool_path_claimed_locked()`
(失败现在答 true——「说不准」永远不等于「删掉」);`pool_scan()` 里那次快照查找(失败过去把一个
快照活得好好的 pool 条目判成 doomed)和 `pool_row_still_doomed_locked()` 里对称的那次(失败过去答
`true` = 删);`has_column()`(改成 `int`:1/0/负——读不到 schema 不等于列不在,否则迁移会去跑一条
注定重复的 ALTER,或者把一个迁好的 store 判成没迁);`wfs_store_open()` 里读 `PRAGMA user_version`
那一次(读不出来不再当 0,按第十三轮的规矩给 `WFS_E_STORE_DAMAGED`);`wfs_world_next_id()`
(读不出来不再答 1 —— 那是一个 store 已经发过的 id);`gc_fail_bump()` / `gc_fail_get()`
(读不出来不再当「一次都没失败过」,否则 collector 会在同一棵树上打转)。另外 8 处**本来就**倒向
安全一侧(`snap_tmp_named_by_row()`、`trash_row_still_ours()`、`gc_tmp_is_removable()` 的两次查询、
`gc_dirs_unreadable_pending()`、`gc_fail_clear()` 的读、`pool_row_still_doomed_locked()` 的行重读、
`wfs_gc_pending()` 对 `trash_scan` 失败答「没有待办」),这次把这件事写进注释,让审计结果可复查。

**验收**:`ctest`(WFS_FSKIT=OFF)**2/2**;`safety.sh` **298 passed, 0 failed**;`check-deps.sh`
全绿。(`m1_criteria.sh` 本轮跳过。)

**先跑红**。P1(新缝 `wfs_test_after_version_bump`,在抬文件和迁移之间触发,从里面同时读文件和
库戳):老次序下把缝放在数据库提交之后,读出来是

```
[seam] VERSION=2 user_version=304
```

——库已经是 M2 的了,而 M1 的那道门还开着。修完之后同一个缝读出 `VERSION=3`、`user_version` 百位是 2。

P2(新缝 `wfs_test_stmt_fail_sql`:下一条 SQL 里含这个子串的语句,`row()` 报一次 SQLITE_IOERR;
库里恒为 NULL)。把 `world.cpp` 退回修前:

```
[a] discard rc=0 snapshot state=2 (TRASHED) tree at home=0      ← 还有一个 ACTIVE 世界是从它 fork 的
[b] after the recovery: snapshot state=2 (TRASHED) tree in trash=1 at home=0
[c] gc with the claim lookup failing: trash_orphans=1 orphan still there=0
```

新增测试(`core_test`):

- 第二十四轮那段 schema3-store 里新增 (d):抬文件和迁移之间的窗口里 `VERSION` 读 3、`user_version`
  百位读 2;再加两种崩溃形态——「文件 3 + 库 2xx」下一次 open 迁完,「文件 2 + 库 3xx」(老次序留下的)
  下一次 open 把门关上。
- 独立的一段 step-store:(a) 一个 ACTIVE 世界指着的快照,引用计数那条 SQL 的 step 失败一次 →
  `discard` 返回 `-EIO`、快照**还是 ACTIVE**、树还在原处;缝一撤,它按 `WFS_E_SNAPSHOT_IN_USE` 拒。
  (b) 用 `wfs_test_trash_crash` 造一个「rename 之后被打死」的 TRASHING 快照,再把一个 world 行改回
  ACTIVE 指着它,带缝重开 store:recovery 的引用计数失败 → 行**留在 TRASHING**,树留在 trash;
  缝撤掉再开一次,基线被搬回家、行回 ACTIVE。(c) `<store>/trash` 里一个没有行认领的目录 +
  认领查询失败一次 → `gc` 报 `trash_orphans == 0`、目录**还在**;缝撤掉再跑,它照常被收走。

#### PR #1 review 第三十一轮:升级 VERSION 的临时文件不能是共用的(2026-09-20)

第三十一轮,Codex 一条 P2,打在第二十四轮那次 schema 2 → 3 的**收尾动作**上。那一轮把
"schema 2 的 store 原地接管"写进了 `wfs_store_open`:`check_version()` 判 legacy,迁移提交之后
`version_upgrade()` 把 `VERSION` 重写成 3。问题是**这条路上没有锁**。`wfs_store_open()` 自己不加
任何 flock,整条路上唯一的互斥是迁移那次 `BEGIN IMMEDIATE`——它足够保住**数据库**(一个事务赢,
另一个进来发现列全在),对那个**文件**一点忙也帮不上。

于是两个进程同时第一次打开一个 schema 2 的 store:两个都把 `VERSION` 读成 2,两个都迁移,两个都走到
`version_upgrade()`,而它写的是**同一个** `<store>/VERSION.tmp`——两边 `O_TRUNC` 同一个文件,第一个
`rename` 把它消耗掉之后,第二个 `rename` 的源已经不在了,`ENOENT` 一路返回出去:一次**完全正常**的
`world` 调用,在一个**健康**的 store 上失败,而且只在它**第一次**被打开的时候失败——最没人盯着的那一次。

| # | 位置 | 问题 | 修法 | 提交 |
|---|---|---|---|---|
| P2 `PRRT_kwDOUf7jGc6kGFgG` | `core/src/store.cpp` `version_upgrade()`(~275) | 临时文件名是**共用**的 `<store>/VERSION.tmp`。两个 legacy open 并发时,后 `rename` 的那个源文件已被前一个搬走,`-ENOENT` 失败;两边还会互相 `O_TRUNC` 对方正在写的字节 | ①临时名**私有**:`VERSION.tmp.<pid>.<hex>`,hex 取自 store id 用的那把 `getentropy()`,建的时候 `O_CREAT\|O_EXCL`(不再 `O_TRUNC`),写、`fsync`、`rename`。两个升级者不可能拿到同一个名字,于是后落地的那次 `rename` 只是把一个写着 3 的 `VERSION` 换成另一个写着 3 的 `VERSION`——它本来就是幂等写。②`rename` 真失败了就**回头问文件本身**(新的 `version_read()`,和 `check_version()` 不同,它**从不写**):`VERSION` 已经是 3 就返回 0——不管是我们干的还是先到的人干的,调用者要的是盘上有一个 schema 3 的 store,不是这件事的署名权;只有"errno + `VERSION` 还是 2"才算失败。③崩在"建好临时文件还没 rename"之间的升级者留下的残骸,由下一次升级清掉 | `c8c1eb3` |

**清扫遵第二十轮那条 P18 的规矩**(只删这个 core 自己分配的名字):只在 `<store>` **本级**
(不进子目录,`unlink(2)` 不穿符号链接),只删 `VERSION.tmp` 前缀的名字——这个文件在那里写过的**全部**
命名空间就是它,新的 `VERSION.tmp.<pid>.<hex>` 和老版本用的那个共用 `VERSION.tmp`,别的东西不会在一个
store 目录里占这个前缀;而且**只在 `version_upgrade()` 里扫**,也就是只有升级者自己付这次 readdir,
普通 open(每次 `world` 调用都有一次)一点都不多花。清扫**可能**把另一个并发升级者的临时文件端掉——
那个升级者的 `rename` 于是失败、回头读 `VERSION` 读到 3、返回 0,正好是上面②那条路。

**为什么不加 flock**:`wfs_store_open()` 目前不持有任何锁,为了这件事引一把新锁要覆盖"迁移 + 重写
文件"整段,而数据库那半本来就是安全的(`BEGIN IMMEDIATE`),剩下的文件那半靠"私有临时名 + 幂等
rename + 回头读"就够了——这条路上没有任何需要序列化的**读改写**。老的共用名 `VERSION.tmp` 现在全仓
grep 不到第二处(只剩 `core_test` 的前缀断言和本节)。

**验收**:`ctest`(WFS_FSKIT=OFF)**2/2**;`safety.sh` **298 passed, 0 failed**;`check-deps.sh`
全绿。(`m1_criteria.sh` 本轮跳过。)

先跑红(`core_test` 第二十四轮那段 schema3-store 里新增 (c)):新的 seam
`wfs_test_before_version_rename` 在句柄 A 写完临时文件、还没 `rename` 的那一瞬间触发,在里面对同一个
store 跑一次**完整的** `wfs_store_open()`(句柄 B),B 迁移(空转)、升级、**先** rename。修之前 A 的
open 是 `core_test.cpp:5621: vrace_a -> -2 (No such file or directory)`。

新增测试:①A 的 open 回 0、B 的 open 回 0、`VERSION` 读出来是 3、`user_version` 的百位是 3、
store 目录里 `VERSION.tmp` 前缀的条目**一个不剩**;②崩掉的升级者:store 里先放一个
`VERSION.tmp.999.deadbeef`,再开一次(legacy),它**没了**,`VERSION` 是 3。

#### PR #1 review 第三十轮:借来的权限位要还回去(2026-09-20)

第三十轮,Codex 一条 P2,打在第二十七轮**的另一半**上。第二十七轮把认领里那次 `chmod 0700` 挪到了
身份核对**之后**,于是"一次拒绝不在被拒的东西上留痕迹";可它只说了**陌生人**那一半。另一半是:
这些位是**借**的——认领如果**不删这棵树**,就得还。

`wfs_world_restore()` 正是那个不删树的认领。一个 `0311` 的 World 根(可进、可写、**不可列**,
目录最寻常不过的一种模式)discard 时一根指头都没被碰过:整条 discard 路上没有人以读的方式打开过根
(`.world` 是**穿过**它读的,rename 是父目录的事)。restore 随后 `open(O_RDONLY|O_DIRECTORY)` 拿
EACCES → 核对身份 → `chmod 0700` → 重开 → rename 回家 → 返回 0,**而那 0700 从此再没人改回去**。
discard + restore 本该是一个**往返**,现在它成了一次静悄悄地改写用户目录权限、并且退出 0 的操作。

| # | 位置 | 问题 | 修法 | 提交 |
|---|---|---|---|---|
| P2 `PRRT_kwDOUf7jGc6kF7js` | `world.cpp` `trash_claim_open()`(`core/src/world.cpp` ~877)与 `wfs_world_restore()` | 打不开的条目被 `chmod 0700` 之后,**没有任何人记得它原来是什么模式**。collector 和 `--now` 无所谓(那棵树马上就没了),`restore` 把树搬回家却什么也不还:一个 `0311`(或 `0111`、`0711`…)的 World 根,discard 一次再 restore 一次就永久变成 `0700` | `TrashClaim` 记下身份 `lstat` 看到的那个模式(`lent` / `orig_mode`),`return_lend()` 用 **`fchmod(fd, orig_mode)`** 还——**打在描述符上,绝不打在名字上**(第二十六轮的规矩用在回程):①`~TrashClaim` 兜住**每一个**出口;②`restore` 在"树已到家且核对是我们的"那一刻**显式**还,让这件事是**成功路径**自己的属性,不是析构的副作用;③`trash_claim_verify_rename()` 在"rename 搬的是陌生人、原样撤回"那条拒绝里还(我们自己的树被 `mv` 挪走了,但 fd 找得到它)。**删树的那两条不欠**:`trash_unlink_claim()` 在树**真的没了**的那一刻 `forget_lend()`;而**超时只删了一半**的那棵树保留 `.deleting` 名字、**模式照还**,下一次 wake 的认领重新借——没删完的树必须和我们发现它时一模一样。`trash_claim_open()` 里两个**没有描述符可用**的出口(第二次 `open` 失败、其上的 `fstat` 失败)走 `trash_unlend_path()`:按名字还,但**只还给那个 inode**——重新 `lstat` 必须仍是我们借给的那一个,中间搬进来的陌生人保留他自己的模式(第二十七轮那条界限一字未变) | `6b9c91b` |

**其余"借位"一处一处查过了**:

| 位置 | 结论 |
|---|---|
| `gc_tmp_is_removable()`(被放弃的 fork 临时树) | **根本不 chmod**:那里的证据在树**里面**(marker),所以打不开的目录只能是"还没证明过任何事"的目录,原样留着、记 `undecided`(第十二轮)。不借,也就不用还 |
| `fs_remove_tree_fd()` 的 `rm_fd_unlock_dir` / `rm_fd_unlock_child` / `rm_fd_open_child` | **借在正被删的树上**:调用它的时候整棵树已经在删了,不还是对的(M1_DESIGN.md §3 P4) |
| `trash_unlink_claim()` 里 EPERM/EACCES 那次 `fchmod(c.parent, …|0700)` | 那是 trash **父目录**(`<store>/trash` 或用户的 `.wfs-trash`),不是被认领的树;本轮不动 |
| `wfs_world_discard()` | **确认不改 World 根的任何位**:`verify_identity` 只 `realpath`+`stat`+穿过根读 `.world`,`WorldLock` 打开的是 `.world` 本身,rename 要的是**父目录**的写权限。`SnapGate` / unprotect 那一套是快照的事,与 World 根无关 |

**验收**:`ctest`(WFS_FSKIT=OFF)**2/2**;`safety.sh` **298 passed, 0 failed**;`check-deps.sh`
全绿。(`m1_criteria.sh` 本轮跳过。)

先跑红(`core_test` 新增一段):World 根 `chmod 0311`,discard → restore。修之前三处全是 **0700**——
`RED: restored root mode 0700`、`RED: gc-refused tree mode 0700`、`RED: restore-refused tree mode 0700`。

新增测试(`core_test.cpp` 末尾一段,第三十轮):①`0311` 的根 discard **不被改动**(trash 条目仍是
`0311`);②restore 之后**根还是 `0311`**,而且它仍然是个 World——`wfs_world_verify()` 认得出、
marker 穿过 `0311` 读得出、里面的文件读得出;③两个**借了又拒**的窗口(`wfs_test_between_trash_claim`
把树挪走、放一个陌生目录进来):collector 判 `trash_foreign`、`restore` 回 `WFS_E_TRASH_FOREIGN`,
而**被借的那棵树(已被挪到一边)回到 `0311`**;④最后 collector 照常把这个 `0311` 条目删掉
(`worlds_deleted == 1`,`entries_freed == 2`)——那个认领本来就不欠。

#### PR #1 review 第二十九轮:报表也要把那四个目录都看一遍(2026-09-20)

第二十九轮,Codex 一条 P2,打在第二十八轮**只差一步**的地方。上一轮把"读不出来就报出来、按上限
重试"推到了 collector 扫的每一个目录,`<store>/tmp`(`world exec` 留下的沙箱配置)也在内;可是
`wfs_gc_status()` 的报表是从 `<store>/trash`(`trash_scan`)、`<store>/snapshots` 的 `*.wfs-tmp`
计数趟、`pool_stranded()` 这三处拼出来的,**没有第四处**。于是 `<store>/tmp` 一个 EACCES:`gc` 有
note,`gc --status` **一行都没有**。

两半合起来才是真正的洞:`gc` 那条 note 只有 collector 真跑了才印,而一个目录失败到 `kGcFailCap`
之后 collector 就**不再被唤醒**了——这是设计:过了上限它仍然"每次都被报出来",只是不再每两秒叫醒一个
worker(`internal.h`)。过了上限之后负责报的那个人就是 `gc --status`,而它对这一个目录恰好是哑的。
于是那些读不出来的沙箱配置落到了这一串 review 从头到尾就在防的那个状态里:**东西在盘上、没人收、
也没有任何一条命令叫得出它的名字**。

| # | 位置 | 问题 | 修法 | 提交 |
|---|---|---|---|---|
| P2 `PRRT_kwDOUf7jGc6kFyRB` | `world.cpp` `wfs_gc_status()`(`core/src/world.cpp` ~4035) | 报表扫 trash / snapshots / pool 三处,不扫 `<store>/tmp`。`unread:` 行因此永远不会提到它,`dirs_unreadable` 也不把它算进去;一旦重试上限停掉 worker 链,就再也没有任何东西会说出这个目录 | 在报表里**直接扫**一趟 `<store>/tmp`:`opendir` 只有 ENOENT 算"里面没有东西",其余 errno 记进同一个 `wfs::DirUnreadable`;逐条 `readdir` 之前 `errno = 0`,NULL 且 errno 非零同样记——中途停下的流正是这一轮要修的那种沉默。**一条都不数**(`wfs_trash_stat` 里没有放沙箱配置的字段,那是 collector 的 `tmp_removed`),它欠读者的只是"这个目录到底读不读得了",但循环照样走到流的尽头;并且和别的计数趟一样**一个字都不写库**——上限是 collector 的,提问不花重试。复用已有的 `note()` lambda,所以 CLI 的 `unread:` 行原样印出、计数原样加上,**不加字段、不动 ABI** | `cb20ff5` |

**为什么是直接扫,不是把 `gcfail:dir:<path>` 那条记录翻出来**:`gc --status` 回答的是**现在**,
而计数器只说"某一次 wake 在这里失败过"——一个已经好了一个星期的目录,在 collector 把它清零之前仍然
在计数器里。何况翻出来也**多不出一个目录**:计数器能装的每一条路径,这份报表现在都自己打开了一遍
(`<store>/trash`、`<store>/snapshots`、`<store>/pool` 及其 `S<n>`、`<store>/tmp`)。

**审计**(collector 碰的目录 vs 报表扫的目录):

| 目录 | 结论 |
|---|---|
| `<store>/trash` | `trash_scan`,两边都有(第二十八轮) |
| `<store>/snapshots` | 清扫 + 计数趟,两边都有(第二十八轮) |
| `<store>/pool` 及其 `S<n>` | `pool_collect` / `pool_stranded`,两边都有(第二十七轮) |
| `<store>/tmp` | collector 第二十八轮,**报表这一轮补上** |
| `<parent>/.wfs-trash`(EXDEV 世界的旁路 trash) | **确认不是 readdir**:里面每一条都由行的 `trash_path` 叫出名字(扫描的行循环、`trash_blocked_by`、`trash_identity`、`wfs_trash_blocked_path` / `wfs_trash_entry_path`),根本没有一次目录列举会失败,也就没有东西要记 |
| `<store>/snapshots/S<n>`、`S<n>.wfs-tmp` 这些 gate 目录 | **确认是 `lstat(2)` 不是 readdir**:结论由 `proven_gone()` 给(第十二轮);而且带 gate 的快照根是 0000,本来就不会被当目录打开 |

**验收**:`ctest`(WFS_FSKIT=OFF)**2/2**;`safety.sh` **298 passed, 0 failed**(+8);`check-deps.sh`
全绿。(`m1_criteria.sh` 本轮跳过。)

先跑红(`safety.sh`,新增 8 条):`<store>/tmp` `chmod 000`。修之前 **8 条里红了 7 条**——
`gc --status` 连 `unread:` 这一行都没有、没有目录名、数目是 0;绿的那 1 条是对照组(权限改回来之后
报表干净,本来就绿)。

新增测试(`safety.sh` 末尾一段,`PR29`):`gc --status` 说得出"1 directory … could not be read"、
**报出目录名**、在 `unread:` 行上、errno 是 `Permission denied`、`dirs_unreadable` 数得上它;**再问
一次还是这句**(计数趟不花重试);**连跑 10 次 `gc --now` 把上限用光之后,`gc --status` 照样说得出
这个目录**(这一条才是这个修复真正要的);`chmod 700` 之后报表干净。

#### PR #1 review 第二十八轮:读不出来的目录,一个都不算空的(2026-09-20)

第二十八轮,Codex 两条 P2,和第二十七轮那条 pool 的是**同一句话**,只是换了两个目录:`<store>/trash`
和 `<store>/snapshots`。**我们说"那里什么都没有"的时候,必须先有证据**——而 `opendir` 失败、
`readdir(3)` 中途停下,都不是证据,是**没看见**。

| # | 位置 | 问题 | 修法 | 提交 |
|---|---|---|---|---|
| P2 `PRRT_kwDOUf7jGc6kFfnz` | `world.cpp` `trash_scan()` 里 `<store>/trash` 的无行孤儿 readdir(collector / `gc --status` / `wfs_gc_pending()` 三条路共用) | `if (DIR *d = opendir(trashdir)) { while (readdir(d)) … }`:trash 目录一个 EACCES/EIO,或者 `readdir` 中途失败(它只用 errno 报自己的失败),都被读成"没有无行孤儿"。于是 `trash_scan` 成功返回、`gc` 一声不吭、`gc --status` 报"trash 是干净的",而 `wfs_gc_pending()` **就是这个扫描**,所以 fork 和 discard 都不会再起 collector——**worker 链停在一个谁都没看进去的目录上** | 只有 **ENOENT** 算"里面没有东西"(第十三轮的规矩)。其余 errno,以及 `errno = 0` 后逐条读、NULL 且 errno 非零的那次 readdir,都记进共享的 `wfs::DirUnreadable{count, err, path}`:**collector 那一趟**按**目录路径**bump 共享失败计数器(新前缀 `gcfail:dir:<path>`——读不出来的目录和删不掉的树不是一回事)、在 `kGcFailCap` 以内置 `work_remains`;读通了就 `gc_note_readable` 清零;**计数那两趟(`gc --status`、`wfs_gc_pending`)一个字都不写库**。`wfs_gc_pending()` 自己也认这件事了,链才不会断:trash 那一处它本来就扫,其余目录靠 `gc_dirs_unreadable_pending()`——meta 上一次带索引的 `GLOB` 范围查询,fork 那条路上**不多走一次 readdir**(每次 gc wake 都是新进程,计数器就是上一次 wake 留下的事实),另加 `gc_fail_get()` 读计数而**不花掉一次重试**(提问不是失败) | `266cbae` |
| P2 `PRRT_kwDOUf7jGc6kFfn2` | `world.cpp` `rm_tmp_in_store_dir()`,`<store>/snapshots` 的 `*.wfs-tmp` 清扫 | 同一处沉默的另一半:`if (!d) return 0;`——目录打不开就当"扫过了,里面没有东西";`readdir` 的 errno 同样从来没问过,中途停下就是目录读完了。于是一个无行的 `S<n>.wfs-tmp` 既没被删、也没被数、更没被重试 | 同上,一字不差。另外**这一轮顺手补齐的两处**:`wfs_gc_status()` 里数 `<store>/snapshots` 下 `*.wfs-tmp` 的那一趟(计数,不写库),和 `wfs_gc_ex()` 里 `<store>/tmp` 那个一小时的沙箱配置清扫(collector,照样 bump/重试) | `266cbae` |

**对外只说一次**:第二十七轮的 `pool_unreadable` / `pool_unreadable_path` / `pool_unreadable_errno`
换成 `wfs_gc_report.dirs_unreadable` 和 `wfs_trash_stat.dirs_unreadable` / `_path` / `_errno`——三个
目录三套字段读起来比一套差,而且读者要知道的事在三种情况下完全一样:**上面那些数少算了东西,少算的
那个目录叫什么、errno 是什么**。`gc` 在 stderr 上一条 note(哪个目录、什么 errno、会不会再来),
`gc --status` 一行 `unread:`(里面的东西没算进上面那个数)。

**全部 `opendir` / `readdir` 查过了**(core 里一个不漏):

| 位置 | 结论 |
|---|---|
| `world.cpp` `trash_scan()`(`<store>/trash`) | F1,已修 |
| `world.cpp` `rm_tmp_in_store_dir()`(`<store>/snapshots`) | F2,已修 |
| `world.cpp` `wfs_gc_status()` 里 `<store>/snapshots` 的 `*.wfs-tmp` 计数 | 同一处沉默,已修(计数趟,不写库) |
| `world.cpp` `wfs_gc_ex()` 里 `<store>/tmp` 的过期沙箱配置清扫 | 同一处沉默,已修(collector 趟) |
| `pool.cpp` `pool_sweep_orphans()`(收集 + 计数) | 第二十七轮修的,这轮换到共享结构上 |
| `store.cpp` `store_has_trees()` | 第十三轮就是这条规矩,不动 |
| `diff.cpp` `dir_is_empty()` | 第二十三轮就是这条规矩,而且不是 gc 的路 |
| `platform_posix.cpp` `rm_rec()` / `rm_fd_walk()` / bulk walk | **删除**的走法,不是扫描:每个目录清空之后都有一次 `rmdir`/`unlinkat(AT_REMOVEDIR)`,漏了一条就是 `ENOTEMPTY`;而"它没了"从第十二轮起由 `proven_gone()` 说了算,从来不是走法的返回值 |
| `platform_darwin.cpp` `find_regular()`(EXDEV 探测) | "先建再查"那一类,失败原样回 `-errno` |
| `view.cpp` `wfs_readdir()` | FSKit 视图的 ABI,不是 gc |
| gc 里那些 `S<n>` 编号目录的探测 | 是 `lstat(2)` 不是 readdir,第十二轮已经换成 `proven_gone()` |

**验收**:`ctest`(WFS_FSKIT=OFF)**2/2**;`safety.sh` **290 passed, 0 failed**(+12);`check-deps.sh`
全绿。(`m1_criteria.sh` 本轮跳过。)

先跑红(`safety.sh`,新增 11 条):trash 目录 `chmod 000`、里面放一个无行孤儿;`<store>/snapshots`
`chmod 000`、里面放一个无行的 `S999999.wfs-tmp`。修之前 **11 条里红了 9 条**——`gc` 只打印
`gc: 0 worlds deleted, …` 那一行,stderr 上一个字都没有,`gc --status` 照样报干净;绿的那 2 条是
对照组(权限改回来之后两样东西都会被收掉,本来就绿)。

新增测试(`safety.sh` 末尾一段):

- (a) `<store>/trash` 0000 + 无行孤儿:`gc --now` 报"store 下有 1 个目录读不出来"、**报出目录名**、
  说"会再来试"(`work_remains`);`gc --status` 不把这个 trash 说成干净的;`chmod 700` 之后下一次
  `gc` 把孤儿收掉。
- (a′) **链没断**:另起一个 store(gc 从没在里面跑过),trash 0000 + 无行孤儿,删掉
  `<store>/logs/gc.log`,然后跑一次 `fork`——`spawn_gc_worker()` 问的就是 `wfs_gc_pending()`,
  修之前它回"没事做",现在 worker 起来了,`gc.log` 有内容。
- (b) `<store>/snapshots` 0000 + 无行 `S999999.wfs-tmp`:`gc --now` 报出目录名,`gc --status` 不把
  它说成干净的;`chmod 700` 之后 `gc --now` 把那棵无行克隆收掉。
- (b′) **链没断**:同一个 store 里 `discard W1`(默认保留期,trash 里没有一条到期),删掉 `gc.log`
  之后 worker 照样起来——这一次 `wfs_gc_pending()` 的依据是 `gcfail:dir:` 计数器,正好把
  `<store>/snapshots`、`<store>/pool` 这些它不走 readdir 的目录也盖住了。

#### PR #1 review 第二十七轮:拒绝的时候不许留下痕迹,看不了的目录不算空的目录(2026-09-20)

第二十七轮,Codex 两条 P2,一条打在**认领的顺序**上,一条打在**扫描的沉默**上。两条其实是同一句话的
两面:**我们对别人的东西做的每一件事,都要先有证据;我们说"那里什么都没有"的时候,也要先有证据。**

| # | 位置 | 问题 | 修法 | 提交 |
|---|---|---|---|---|
| P2 `PRRT_kwDOUf7jGc6kFUbS` | `world.cpp` `trash_claim_open()`(gc / `--now` / `restore` 三条路共用) | 条目打不开(EACCES,比如一个 0000 的目录)时,认领先 `chmod 0700` 再 open——**而身份是在 open 之后的 `fstat` 上才比的**。于是一个被放在条目路径上的陌生目录(跨卷 discard 的条目就在用户自己的 `<parent>/.wfs-trash` 里,名字还能猜),在被判成 `WFS_E_TRASH_FOREIGN` 之前,模式已经被我们从 0000 改成了 0700。一次**拒绝**在被拒的东西身上留下了痕迹——而 0000 是它主人的意思 | 顺序反过来:`lstat` → 比行的 `dir_dev`/`dir_ino`(外加 `S_ISDIR`)→ 不符直接 `WFS_E_TRASH_FOREIGN`,**一个字节都不碰** → 符了才 `chmod 0700` → 再 open → open 之后的 `fstat` 在**真正要用的那个描述符**上把身份再证一遍(第二十六轮的协议一点没动)。`lstat` 失败时把 errno 还原成 open 自己那个,按老路返回。**先量了 (a) 再退到 (b)**:macOS 上 `open(path, O_EVTONLY\|O_DIRECTORY\|O_NOFOLLOW)` 打一个 0000 目录**照样 EACCES**(实测 Darwin 27 / APFS,文件属主自己跑),`O_EVTONLY` 去掉 `O_DIRECTORY` 也一样,而 macOS 没有 `O_PATH`——所以"先拿到一个不需要读权限的描述符"这条路在这个平台上不存在,只能在**名字**上先核对 | `6034e95` |
| P2 `PRRT_kwDOUf7jGc6kFUbV` | `pool.cpp` `pool_sweep_orphans()`(收集与计数两条路共用) | `opendir(<store>/pool)` 失败就 `return 0`、`opendir(<store>/pool/S<n>)` 失败就 `continue`、`readdir(3)` 自己的 errno 从来没问过——**一次 EACCES/EIO 于是回答"这里没有孤儿"**。崩掉的 fork 或 filler 留下的无行克隆被整个漏掉:`gc` 一声不吭,`gc --status` 报"0 stale pre-clone entries"(等于"pool 是干净的"),而 `wfs_gc_pending()` 只看 trash,worker 链也跟着停 | 只有 **ENOENT** 算"里面没有东西"(第十三轮给 store 扫描定的同一条规矩)。其余 errno——pool 根的、某个 `S<n>` 的、以及中途停下的 `readdir` 的(`errno = 0` 后逐条读,NULL 且 errno 非零就是读失败)——都记进新的 `PoolUnreadable{count, err, path}`:**收集那一趟**按**目录路径**bump 共享失败计数器、在 `kGcFailCap` 以内置 `work_remains`(worker 链因此会回来),**计数那一趟(`gc --status`)一个字都不写库**;读通了的目录 `gc_fail_clear` 把自己的计数清零。对外是 `wfs_gc_report.pool_unreadable` 和 `wfs_trash_stat.pool_unreadable` / `_path` / `_errno`:`gc` 在 stderr 上报"哪个目录、什么 errno、会不会再来",`gc --status` 单独一行说"里面的东西没算进上面那个数" | `b7f3998` |

**为什么 F1 选了 (b) 而不是 (a)**:(a) 是"先拿一个不需要读权限的描述符,核对完再 `fchmod` 那个已经证明
过的 inode"。在 macOS 上拿不到——写了个小程序,以属主身份对一个 0000 的目录:
`open(O_EVTONLY|O_DIRECTORY|O_NOFOLLOW)` → `EACCES`;去掉 `O_DIRECTORY` → 还是 `EACCES`;`O_PATH` 这个
平台没有。所以只剩 (b):先 `lstat` 按名字核对,再 `chmod`,再 `open` + `fstat` 二次核对。**界限写死**:
在 `lstat` 和 `chmod` 之间被换进来的陌生目录,能拿到的**全部**后果就是模式变成 0700,随即被 `fstat`
拒掉;**永远不可能是一次删除**——从 `fstat` 往后,rename、递归删、rmdir 全部由那个描述符说了算
(第二十六轮),名字再也决定不了任何不可逆的事。

**同一个形状的另外两处,查过了**:(1) `fs_remove_tree_fd()` 里的免疫标志/权限借位(第四/十八轮)是
`fchflags`/`fchmod` **打在子 fd 上**,而那个子 fd 是从已经核对过的父 fd `openat` 出来的——顺序本来就是
"先证明、后动手",不用改(第二十六轮就是这么排的)。(2) 被放弃的 fork 临时树走的 `gc_tmp_is_removable()`
**故意就不 chmod**:那里的证据在树**里面**(`.world` 标记),一个打不开的用户目录就是"还没证明任何事",
保持第十二轮的 `undecided`、把行和树都留着。

**F2 的旁证也查过了**:`pool_scan()`(行那一侧)不受影响——它只读数据库,`Stmt` prepare 不出来本来就
回 `-EIO`;而它喂给 `pool_collect()` / `pool_stranded()` 的树侧判断,第十二轮就已经从 `exists()` 换成
`proven_gone()`(只有 ENOENT/ENOTDIR 算"没了")。

**验收**:`ctest`(WFS_FSKIT=OFF)**2/2**;`safety.sh` **278 passed, 0 failed**(+4);`check-deps.sh` 全绿。
(`m1_criteria.sh` 本轮跳过。)

先跑红:

- **F1**(`core_test`,新增一块):把世界的树挪走,在条目路径上 `mkdir` 一个带 `user.txt` 的目录、
  `chmod 0000`。修之前 `wfs_gc()` 确实报 `trash_foreign == 1`(第二十五轮那条规矩在起作用),**但
  陌生人的模式回来是 0700**——`core_test.cpp:5898` 的 `CHECK(stat(mentry, &mst) == 0 &&
  (mst.st_mode & 07777) == 0)` 当场红。
- **F2**(`safety.sh`,新增四条):`<store>/pool/S1/` 下放一个无行的孤儿、pool 根 `chmod 000`。修之前
  `gc` 只打印 `gc: 0 worlds deleted, …, 0 pool entries` 就完了,stderr 上一个字都没有,`gc --status`
  也什么都不说——四条里红了三条(第四条"权限改回来之后孤儿会被收掉"两边都绿,它本来就是对照组)。

新增测试:

- `core_test`(F1,排在第二十六轮那块后面):0000 的陌生目录站在条目路径上 → (1) `wfs_gc()` 报
  `trash_foreign == 1`、`worlds_deleted == 0`、`entries_freed == 0`,**模式还是 0000**;(2)
  `wfs_world_discard(..., immediate=1, ...)` 回 `WFS_E_TRASH_FOREIGN`,模式还是 0000;(3)
  `wfs_world_restore()` 回 `WFS_E_TRASH_FOREIGN`、家目录没被创建出来,模式还是 0000;行始终 TRASHED、
  身份列没动。最后自己 `chmod 0700` 进去,陌生人的 `user.txt` 逐字节还在;把真树搬回来再收一次,
  `worlds_deleted == 1`,行 DEAD。
- `safety.sh`(F2,T1.5 那一段末尾):`gc` 报出"`<store>/pool` 下有 1 个目录读不出来(Permission
  denied)"并说"会再来试"(`work_remains`);`gc --status` 不把这个 pool 说成干净的;`chmod 700` 之后
  下一次 `gc` 把那个孤儿收掉。

#### PR #1 review 第二十六轮:核对过的那个身份,必须就是被删掉的那个身份(2026-09-20)

第二十六轮,Codex 一条 P1,打在**第二十五轮自己**身上。第二十五轮把"这条 trash 条目是不是这一行的
树"写成了一次 `lstat` + 比 `dev`/`ino`,然后——**换两次新的名字查找**去动它:`trash_mark_deleting()`
按路径 rename,`trash_unlink()` 按路径递归删。SQLite 那个 `BEGIN IMMEDIATE` 串行化的是**我们自己的
写者**;可这条洞里的另一方根本不是写者,是**那个目录的主人**——跨卷 discard 的条目就在用户自己的
`<parent>/.wfs-trash` 里,名字还可以猜。一条 `mv` 落在 `lstat` 和 `rename` 之间:核对通过的是世界的
树,被改名成 `.deleting`、被连内容删光的是别人的目录。第二十五轮把窗口从"整个保留期"缩到"两次系统
调用之间",但**没有关上它**。

**规矩写成一句**:**核对是在一个描述符上做的,动手也必须在同一个描述符上。** 一个名字每解析一次就是
一次新的提问,而 `open()` 之后的那个 fd 就是**那个 inode 本身**,别人在名字上做什么都换不掉它。

| # | 位置 | 问题 | 修法 | 提交 |
|---|---|---|---|---|
| P1 `PRRT_kwDOUf7jGc6kFGX_` | `world.cpp` `gc_claim_deleting()` / `trash_delete_now()`(`--now`)/ `wfs_world_restore()` / `gc_tmp_is_removable()`,`platform_posix.cpp` | 第二十五轮的 `trash_identity()`(lstat + 比 dev/ino)、随后的 `trash_mark_deleting()`(按路径 rename)、再随后的 `trash_unlink()`(按路径递归删)是**三次独立的文件系统操作**。事务挡得住我们自己的 collector/`--now`/`restore` 互相插队,挡不住一个在 lstat 与 rename 之间把真树挪走、在 `j.path` 上放另一个目录的用户:按路径的 rename 于是认领了陌生人,按路径的递归删把它删光。`restore` 同样——核对完再 rename 回家,回家的可以是别人的树 | **认领 = 描述符**(新结构 `TrashClaim`)。(1) `open(path, O_RDONLY\|O_DIRECTORY\|O_NOFOLLOW\|O_CLOEXEC)` + `fstat` 与行的 `dir_dev`/`dir_ino` 比对(不符 `WFS_E_TRASH_FOREIGN`,和第二十五轮一样),这个 fd **握到整件事做完**;(2) rename 照旧按名字(**rename 只搬目录,永远删不掉东西**,所以就算搬的是陌生人也没损失);(3) 打开 trash 父目录(`O_DIRECTORY\|O_NOFOLLOW`),`fstatat(父fd, "<leaf>.deleting", AT_SYMLINK_NOFOLLOW)` 必须等于 fd 的 dev/ino——不等就是刚才搬的是陌生人:`renameat` **原样搬回去**、报 foreign、这一条跳过(事务回滚,行一个字没改);(4) 内容**穿过 fd 删**——新增 `wfs::fs_remove_tree_fd(dirfd, threads, entries, deadline_us, partial)`:`fdopendir(dup(fd))`,每条 `fstatat(AT_SYMLINK_NOFOLLOW)`,文件/符号链接 `unlinkat`,子目录 `openat(O_DIRECTORY\|O_NOFOLLOW)` 递归后 `unlinkat(AT_REMOVEDIR)`,免疫标志与权限借位(第四/十八轮)也都打在子 fd 上(`fchflags`/`fchmod`),`deadline`/`partial` 语义与 `fs_remove_tree_parallel` 逐字相同;(5) 条目本体那次 rmdir:`fstatat(父fd, leaf.deleting)` 再对一次 fd 的身份,然后 `unlinkat(父fd, leaf.deleting, AT_REMOVEDIR)`。`--now` 同一套协议;`restore` 是 rename 回家之后用 `fstatat(家的父目录, leaf)` 对 fd,不符就 rename 回去、`WFS_E_TRASH_FOREIGN`,(c) 写回行的那一对就是 fd 上核对过的那一对 | `4f7437f`(remover)+ `8c7f76d`(协议) |
| 配套 | `world.cpp` `gc_tmp_is_removable()` + CREATING 收尾,`marker_read_at()` | 被放弃的 fork 临时树(第五/二十轮)是同一个形状:`.world` 标记**按路径读**、树**按路径删**。而这棵树就在**用户自己的**目标目录里,名字只有行记得 | 先 `open(O_DIRECTORY\|O_NOFOLLOW)`,标记穿过这个 fd 读(新 `marker_read_at()` = `openat(fd, ".world", O_NOFOLLOW)`,并把"根本没有标记"(-ENOENT)和"有但读不出来"分开,后者照第十二轮算 `undecided`),问数据库用的 inode 就是这个 fd 的,删除走 `trash_unlink_claim()`(同样的 fd + 最后那次带身份核对的 rmdir)。**这里故意不 chmod**:trash 那边是行的 dev/ino 先证明了树是我们的,这里的证据在树**里面**,一个打不开的用户目录就是"还没证明任何事",保持第十二轮的 `undecided` | `8c7f76d` |

**最后那次 rmdir 的界限,写明白**:整条协议里唯一还由名字决定的一步,是 `unlinkat(父fd, leaf,
AT_REMOVEDIR)`——它前面那次 `fstatat` 和它之间仍然有一个瞬间。可这一步的界限是**精确**的:
`AT_REMOVEDIR` 要么删掉一个**空目录**,要么什么都不删(非空是 `ENOTEMPTY`)。也就是说,在那个瞬间
被塞进来的东西,最多赔掉一个空目录,**一个字节的数据都丢不了**。代码注释和回帖里都把这句话说死。

**哪些地方仍走按名字的并行删除,为什么**:(a) **快照条目**——它的树永远在 `<store>/trash`、名字由
store 分配,而且 snapshots 表压根没有 `dir_dev`/`dir_ino`(第二十五轮已经解释过为什么不补),没有
身份可核对,也就没有身份需要一路带下去;它们还整棵 `UF_IMMUTABLE`(`--hard`),按名字那条路一趟
`fs_unprotect_tree` 就解开了。(b) **`<store>/trash` 下无行的孤儿**——没有行可以当它的树。(c) **早于
身份列的老行**(本 store 从来不产生):没记过身份就没得核对,这本来就是第二十五轮自己的规矩。这三类
继续用 4 线程的 `fs_remove_tree_parallel`,而它们正是"大树"最常出现的地方。

**代价量了**(50 000 个条目 = 100 个目录 × 500 个文件,APFS,同一台机器,各跑三遍取平均,
`fs_remove_tree_parallel` / `fs_remove_tree_fd` / `fs_remove_tree` 各删一棵一模一样的树):

| 删法 | 均值 | 相对 |
|---|---|---|
| 按名字,4 线程(`fs_remove_tree_parallel`,原路径) | **844 ms** | 1.00x |
| 按 fd,串行(`fs_remove_tree_fd`,本轮新增) | **2242 ms** | 2.66x |
| 按名字,串行(`fs_remove_tree`,老的单线程 remover) | **2216 ms** | 2.62x |

结论很干净:**贵的是并行度,不是描述符**——fd 版本比同样串行的按名字版本只慢 ~1%(2242 vs 2216 ms)。
并行版的 walker 把**路径**发给 worker、rmdir 又要按"最深优先"的路径表收尾,让它改成按描述符下降是把
walker 重写一遍,不是换个 remover;而 gc 的批次**deadline 逐条目检查**这条没变,所以代价的形状是
"同样的一秒唤醒删掉的条目变少、唤醒次数变多",不是"一次唤醒变长"。真要把这 2.6x 拿回来,是另开一轮
"按描述符下降的并行 walker"的事。

**验收**:`ctest`(WFS_FSKIT=OFF)**2/2**;`safety.sh` **274 passed, 0 failed**;`check-deps.sh` 全绿。
(`m1_criteria.sh` 本轮跳过。)`safety.sh` 没有新增:本轮这条竞态的另一方必须**恰好落在核对与 rename
之间**,只有库内的 seam 能造出来,CLI 造不出来——所以测试全在 `core_test`。

先跑红(新 seam `wfs_test_between_trash_claim`,库外恒为 NULL;在里面把真树 `mv` 走、在 `j.path` 上
`mkdir` 一个带 `user.txt` 的目录):

- `wfs_gc()`:陌生人的目录**和里面的 `user.txt` 一起没了**(`exists=0`),`entries_freed=1`,
  `worlds_deleted=1`,行落 **DEAD**——gc 报"删掉了一个世界",而世界自己的树好端端躺在被挪去的地方。
  断言 `CHECK(exists(g_swap_file))` 在 `core_test.cpp` 当场红。
- 修完之后同一条 seam:`trash_foreign=1`、`worlds_deleted=0`、`entries_freed=0`,`.deleting` 名字
  **压根没留下**(搬过去又搬回来了),用户的文件逐字节还在,真树也逐字节还在,行还是 TRASHED 且
  `dir_dev`/`dir_ino` 没变。

新增测试(`core_test` 末尾一块,和第二十五轮那块并排):

- **collector**:seam 里换掉 → `wfs_gc()` 报 `trash_foreign == 1`、`worlds_deleted == 0`、
  `entries_freed == 0`;陌生人的 `user.txt` 内容逐字节比对;`<条目>.deleting` 不存在(rename 被原样
  撤回);被挪走的真树里的 `sub/b.txt` 还在;行 TRASHED、身份列没动。
- **`--now`**:同一个 seam → `wfs_world_discard(..., immediate=1, ...)` 回 `WFS_E_TRASH_FOREIGN`,
  一样什么都没动。
- **`restore`**:同一个 seam → `wfs_world_restore()` 回 `WFS_E_TRASH_FOREIGN`,**家目录没有被创建出来**
  (陌生人没有被登记成这个世界),陌生人被 rename 回条目的名字上、文件还在,真树还在,行还是 TRASHED。
- **普通路径**:不装 seam 再收一次 → `worlds_deleted == 1`、`trash_foreign == 0`,
  `entries_freed == 4`(`a.txt`、`sub`、`sub/b.txt`、`.world`——条目本体不计),条目和 `.deleting`
  两个名字都没了,行 DEAD。**这一条就是新 remover 的计数契约**。
- deadline/partial 走的是既有的 `safety.sh` PR7 那块(120k 条目的被放弃 fork 树 + 一秒预算):它现在
  **正好穿过新的 fd remover**(CREATING 那条路本轮也改成了按 fd),仍旧一秒返回、留一棵可续的树、
  不报失败,下一次无预算的 gc 收干净。

#### PR #1 review 第二十五轮:名字不是所有权,inode 才是(2026-09-20)

第二十五轮,Codex 一条 P1,打在第二十轮那条规矩的**另一半**上。第二十轮写的是"**不是我们起的名字,
就不是我们留下的东西**":collector 不再把 `<条目>.deleting` 当成自己上一次没删完的折叠掉。可它那一趟
真正要删的那个东西——条目本身——依旧只问了一个问题:**行里还写着这个路径吗?** 而跨卷 discard 的
trash 就在用户自己的 `<parent>/.wfs-trash` 里,条目名 `W<id>-<时间戳>` 完全可以猜。保留期里用户把树
挪走、在同一个路径上放一个**别的**目录,行照样"写着这个路径":collector 把它改名成 `.deleting`、连
里面的文件一起递归删光;`discard --now` 同样删、而且退出 0 报成功;`restore` 更糟——它把那棵树 rename
回家,**再 stat 一遍、把它的 dev/ino 写进行里**,于是别人的目录成了这个 World,而世界自己的树没有任何
东西还记得它。

**规矩写成一句**:**一棵树归不归这一行,看的是 inode,不是名字。** World 行从发布那一刻起就带着
`dir_dev`/`dir_ino`(fork/adopt 写的),`wfs_world_discard` 在搬树之前用
`wfs_world_verify_identity()` 核对过它们、之后再没碰过这两列,而这条路上的每一次 rename 都是**同卷**
的(store 同卷时是 `<store>/trash`,撞上 EXDEV 时是世界旁边的 `.wfs-trash`)——同卷 rename 保 inode,
所以**条目只要还在,行记的那个 inode 就是它的身份**,`.deleting` 那个拼写也一样。这正是 P18 那句
"ownership is an invariant we wrote, not a lookalike name" 的字面意思:第二十轮管的是我们**放**出去的
名字,这一轮管的是我们**拿**回来的树。

| # | 位置 | 问题 | 修法 | 提交 |
|---|---|---|---|---|
| P1 `PRRT_kwDOUf7jGc6kE555` | `world.cpp` `gc_claim_deleting()` / `trash_delete_now()`(`--now`)/ `wfs_world_restore()` / `trash_is_deleting()`,`wfs_gc_status()`,`cli/main.cpp` | 认领一条 trash 条目只问"行里还写着这个路径吗"。跨卷 discard 的条目在用户自己的 `<parent>/.wfs-trash` 里、名字可以猜,所以保留期里"把树挪走、放一个同名目录"是用户一条 `mv` 就能造出来的状态——而它对这三条路径全都成立:`gc`(保留期一到就递归删掉用户的目录,报"1 worlds deleted",退出 0)、`discard --now`(同样删,同样报成功)、`restore`(把陌生人的树 rename 回家,**再 stat 一遍写进 `dir_dev`/`dir_ino`**,行从此指着别人的数据,`fs list` 里 HERE 是 yes) | **改名/删除/搬回家之前,先核对 `st_dev`/`st_ino` 和行里的 `dir_dev`/`dir_ino`**(新 helper `trash_identity()`,新错误码 `WFS_E_TRASH_FOREIGN`)。三处都在**决定那一刻所在的那个事务里**读行、在同一个事务里核对:collector 的 `gc_claim_deleting()`(第十五轮起 rename 和记名字就在这一个 `BEGIN IMMEDIATE` 里,所以身份和认领不可能分家)、`--now` 的 `trash_delete_now()`、`restore` 的 (a)。对不上就**一根指头都不碰**:行留在 TRASHED,collector 计进新的 `trash_foreign`、按共享失败计数器重试到上限(和第二十轮的 `trash_blocked` 同一套,因为同样只有操作者能解),`gc --status` 用新的 `foreign_path` **把那个路径说出来**,`--now` 和 `restore` 回 `WFS_E_TRASH_FOREIGN`,CLI 用新的 `wfs_trash_entry_path()` 报出路径并说清"这是别人的数据"。第十二轮那条照旧:**问不出来不算不在也不算不是**(probe 的 errno 原样返回、条目留着);而**真的不在**(ENOENT)仍旧走老路——行进 DEAD,因为树是真的没了。配套两处:(a) `restore` 的 (c) 不再拿 `stat(<家>)` 去喂行的 `dir_dev`/`dir_ino`,写回去的就是 (a) 核对过的那一对——同卷 rename 保 inode,合法情况下本来就是同一个数,而"从地上捡一个身份给行"正是这条 bug 的最后一步;(b) `trash_is_deleting()` 也核对身份:`.deleting` 上的那棵树是不是 collector 在删**这个世界**,同样要 inode 说了算,否则别人放的一个同名目录会永远替我们回答"collector 正在删它"(第二十轮刚把这句假话从另一半里拿掉) | `6e5dca9` |

**快照怎么办**:快照的 trash 永远在 store 里(`<store>/trash/S<n>-<ts>`),不在任何用户目录,名字也是
store 自己分配的;snapshots 表没有 `dir_dev`/`dir_ino` 两列(worlds 有、pool 有,snapshots 从来没有),
补这两列要一次迁移加一轮"在哪儿写、在哪儿核对"的审计,而它换来的不是本轮这条洞——本轮这条洞的前提
就是"**手伸进了用户可见的目录**"。所以身份检查**只对 World 成立**,snapshot 的条目照旧按行+路径认领。
同理,`<store>/trash` 底下**无行的孤儿**没有行可以比,它归的是既有那条规矩:**store 自己分配名字的
目录里,按名字模式判断才成立**(第二十轮 (b))。

**验收**:`ctest`(WFS_FSKIT=OFF)**2/2**;`safety.sh` **274 passed, 0 failed**(新增 8 条 PR25);
`check-deps.sh` 全绿。(`m1_criteria.sh` 本轮跳过。)

先跑红(用 `3f458bc` 的二进制,store 里 discard 一个世界,把条目 `mv` 走,在同一个路径上
`mkdir` 一个带 `keep/user.txt` 的目录):

- `world fs gc --now --retention 0` → `gc: 1 worlds deleted, …`,**退出 0**,`keep/user.txt` 连同那个
  目录一起没了。
- `world fs restore W1` → `W1 restored to …/w`,**退出 0**;`ls -R …/w` 印的是 `keep/user.txt`,
  `fs list` 说 W1 是 `active`、`HERE yes`——别人的目录被登记成了这个世界,而世界自己的树没人记得。
- `safety.sh` 的新块:8 条里 7 条红(最后一条"把树放回去再收"本来就该绿)。
- `core_test` 的新块:把 `trash_identity()` 短路成永远 0,第一条断言
  `frep.trash_foreign == 1` 就红。

新增测试:

- `safety.sh` 新块 **PR25**(8 条,走 CLI):discard 一个世界 → 把条目挪走、在同一路径放一个带
  `user.txt` 的目录 → `gc --now --retention 0` **不删它**、`gc` 说"could not be collected"、
  `gc --status` 有 `foreign:` 一行且**印出那个路径** → `discard W<n> --now` 拒绝并印出路径 →
  `restore W<n>` 拒绝、家目录没有被创建出来 → 把世界自己的树放回那个路径 → `restore` 成功、
  `f.txt` 在 → 再 discard 一次、`gc` 照常收掉,trash 空。**跨卷(EXDEV)那条 `.wfs-trash` 路径没法
  在单卷的测试机上造出来,而身份检查跑的是同一段代码——它比的是行,不是条目碰巧在哪儿。**
- `core_test` 末尾一块(foreign-store):同一个剧本,但断言直接读行——`dir_ino` 从
  `metadata.db` 里 `SELECT` 出来、和 `lstat(条目)` 的 `st_ino` 相等(discard 不动这两列、同卷 rename
  保 inode),换成陌生目录之后 `st_ino` 不等;于是 `wfs_gc()` 的 `trash_foreign == 1` 且
  `worlds_deleted == 0`、`<条目>.deleting` **压根没被建出来**、用户的文件原样在、行还是 TRASHED 且
  `dir_ino` 没变;`wfs_gc_status()` 的 `foreign_path` 正是那个条目;`wfs_trash_entry_path()` 也是;
  `--now` 和 `restore` 都回 `WFS_E_TRASH_FOREIGN`,家目录没被建出来;把真树放回去之后 `restore` 成功、
  行的 `dir_ino`/`dir_dev` 还是原来那一对(不是从地上捡的),最后 `wfs_gc()` 正常收掉、行进 DEAD。

#### PR #1 review 第二十四轮:能删什么,也是 schema 的一部分(2026-09-20)

第二十四轮,Codex 一条 P1,打在一句我们自己写下来、而且写了两次的话上:**增量列不动 `VERSION`**
(第十一轮),以及**`state` 是普通 INTEGER 列,不需要迁移、不动版本号**(第二十一轮给 pool 的
`DRAINING=2` 下的判词)。两句就**读**而言都对:M1 的 core 碰到一个多出来的列、碰到一个它不认识的
`state`,顶多是不用它。可 store 里还有另一半——`wfs_gc()`,它**删**。M1 的 collector 只认 M1 那张
地图,`trash/` 和 `snapshots/` 底下凡是它认不出来的都是孤儿。而 `VERSION` 一直是 2,所以 `main` 上
的二进制照样打得开我们的 store:`world fs gc` 一跑,保留期里的快照没了,TRASHING 的世界树没了,行
还在,指着一堆不存在的路径。**版本号不是记“库里有哪些列”的,是记“这个 store 该交给谁”的。**

**schema 3 是什么意思**(一次抬版本,管住下面所有这些——它们合起来就是 M1 的 collector 看不懂的
那部分 store):

- snapshot 也走 trash(T2.2):`trash/S<n>-<ts>/`、`snapshots.trash_path` / `trashed_at`;
- `WFS_ST_TRASHING = 4`(第五轮):行已提交、树在两个名字之一,任何收 trash 的人都不许把它当无主目录;
- `.deleting` 后缀(T2.1):collector 开删之前先改的名,`restore` 一律拒;
- pool 的 `POOL_DRAINING = 2`(第二十一轮):一棵正在出门的树,`claim` 发不出去、gc 无条件判死;
- `owner_pid` / `owner_start`(第三轮起):谁在建这行,好让 gc 分得清“崩了的生产者”和“还在克隆”;
- 硬链接清单 `hl_groups` / `hl_external`(T2.5):快照的一部分,少一个文件就是一次错误的 fork。

| # | 位置 | 问题 | 修法 | 提交 |
|---|---|---|---|---|
| P1 `PRRT_kwDOUf7jGc6kEwaq` | `core/include/worldfs/worldfs.h` 的 `WFS_STORE_SCHEMA`、`store.cpp` `check_version()` / `wfs_store_open()`、`world.cpp` `marker_read()` | M2 改的全是“增量”的东西,于是 `VERSION` 从头到尾是 2——`main` 上的 M1 二进制打开一个 M2 store,一路绿灯。它的 `wfs_gc()` 是按 M1 那张地图画的:保 `state=2` 的 world trash 路径,`trash/` 和 `snapshots/` 底下别的一律当孤儿递归删。于是 `world fs gc` 一跑,保留期里的快照(T2.2)、行还在 TRASHING 的世界树(第五轮)、`.deleting`(T2.1)、DRAINING 的 pool 树(第二十一轮)全没了,而 M2 的行还在,指着一堆不存在的路径。**增量列是“老 core 读不到它”,新 state 值是“老 core 认不出它”,两句都只管住了读;删是另一回事** | **schema 抬到 3**,并且把这条写进 P13:凡是改变了 collector 可以删什么、或者改变了某个 `state` 的取值,就抬大版本,因为老二进制的 collector 必须**拒绝**这个 store 而不是在上面跑。落地三处:(1) `check_version()` 现在是不对称的——`3` 放行,`2` 放行但记下 `*legacy`(schema 2 的 store 就是“这个 core 还没碰过”的 store,里面没有任何上面那些东西),别的按 P13 拒;列还是第十一轮那趟表驱动事务迁移加,`VERSION` **在迁移提交之后**才由 `version_upgrade()` 重写成 3,走临时文件 + `rename`(写了一半的 `VERSION` 是一个谁都打不开的 store,而它存在的意义正是让比写它的人老的二进制读得懂),迁移失败就不重写——那还是个 M1 处理得了的 store。(2) `PRAGMA user_version` 的百位也当 schema 用,比我们新就按 P13 拒,而且**这一问挪到了对数据库写任何东西之前**(连 `journal_mode` 都还没设),于是未来 schema 的 store 原封不动地回来,而不是被我们的迁移盖成 3xx——那一下是没法撤销的。(3) `.world` 标记里那个 `schema` 字段:它的语法 2 和 3 之间没变,原地升级的 store 里的世界还带着旧标记,所以读 `2` 放行、比 3 新照拒 | `6fdcd96` |

**M1 那边靠的是哪一行**(`git show main:core/src/store.cpp`,`check_version()`,那里的
`WFS_STORE_SCHEMA` 是 2):

```c
        long v = ::strtol(buf, nullptr, 10);
        return v == WFS_STORE_SCHEMA ? 0 : WFS_E_SCHEMA;
```

它在**打开数据库之前**读这个文件,而且是全等比较——所以 `VERSION` 里的那个 `3` 就是拦住它的全部,
`user_version` 那一道它根本走不到(走到了也没用:它比的是 `user_version != WFS_STORE_SCHEMA`,不是
百位)。同一个二进制的 `marker_read()` 也是全等比较,所以 M2 写的标记它一样拒。

**验收**:`ctest`(WFS_FSKIT=OFF)**2/2**;`safety.sh` **266 passed, 0 failed**(P13 那条用的是
`VERSION=99`,照旧被拒;`store status` 现在印 `schema: 3`);`check-deps.sh` 全绿。
(`m1_criteria.sh` 本轮跳过。)

先把它跑红——这一条的红比别的都直接,因为它是**现在这个二进制留在磁盘上的数字**:

- 用 `fcc19aa` 的 `world` 建一个 store,`world fs --store … gc --status` 跑完之后 `cat VERSION` 是
  **`2`**、`PRAGMA user_version` 是 **`204`**。把这两个数字放进 `main` 那行全等比较里,答案就是
  **0,放行**——一个满是 TRASHING、`.deleting`、DRAINING 的 store,M1 的 collector 可以直接在上面跑。
  打了补丁的二进制再开同一个目录:`VERSION` 变成 **`3`**、`user_version` 变成 **`304`**、旁边没有
  留下 `VERSION.tmp`。
- 只把常量抬到 3、`store.cpp` 不动:`core_test` 在第十一轮那块的第一次 `wfs_store_open` 上就红了
  (`-1006 store schema mismatch`)——M1 的 store 被**拒绝**而不是被接管,升级路径是真的要写的。
- 再让 `check_version()` 放 `2` 过去、但别的都不改:`user_version` 确实变成了 `304`,而 `VERSION`
  文件**还是 `2`**(测试打出来的就是这一行:`RED: VERSION file says 2`)——也就是说库升级了,而 M1
  唯一会看的那个文件没有升级,洞还在原处。同一次跑里第二条红:一个盖着 `400` 的库被**打开成功**
  (`rc 0`),而且顺手盖回了 `304`。

新增测试:`core_test` 末尾一块(schema3-store),三件事——

- (a) 一个 M1 store(`VERSION` 写 `2`、库按 schema 2 最初的样子造、13 个后加列一个都没有):打开之后
  13 个列全在、`user_version / 100 == 3`、`VERSION` 文件读出来是 `3`、`VERSION.tmp` 不在;再开一次
  什么都不变(`wfs_store_status` 报 `schema == 3`)。这一条就是从另一头写下来的那个不变量:M1 自己
  那行全等比较,现在对这个 store 的答案是 `WFS_E_SCHEMA`。
- (b) `VERSION` 是 `3`、库盖着 `400`:`wfs_store_open` 回 `WFS_E_SCHEMA`,`user_version` **还是 400**、
  13 个列**一个都没加**——原封不动地回来。
- (c) 同一个 store 把 `VERSION` 改成 `4`:在数据库被打开之前就按 P13 拒,`user_version` 依旧 400。

`make_v2_db()` 和那张 13 列的表从第十一轮那块里提到了文件作用域:schema 2 的库就是 M1 的 store,
两轮问的是同一个东西——抬版本**不许**让它丢列。

#### PR #1 review 第二十三轮:读不出来的那一半,不是空的那一半(2026-09-20)

第二十三轮,Codex 三条 P2,三条是同一句话——第十二轮那句"**问不出来不等于不在**"——在两个还没被它
管到的文件里的实例。第十二轮管的是 collector:它要毁东西、要写没法反悔的行,所以"树不在了"必须证明。
这一轮的两个文件不毁任何东西,可它们**报数**:`world fs diff` 报出来的那份清单是人拿去看代码改了什么
的,硬链接重放报出来的 0 是"这棵克隆和快照一模一样"。一份报错成"没有"的答案,和一次删错的树一样没法
事后发现——它退出 0,没有任何一处留下痕迹。

| # | 位置 | 问题 | 修法 | 提交 |
|---|---|---|---|---|
| P2 `PRRT_kwDOUf7jGc6kEbHC` | `core/src/diff.cpp` `expand_side()`(候选路径),以及 `platform_posix.cpp` 的 walker | 搬进/搬出 World 的**目录**是靠走一遍它来报的(FSEvents 对整棵搬进来的子树只给一条目录事件),而这趟 walk 的返回值**被丢掉了**:目录是 `0000`、或者中途 EIO,于是它一条不报、或者只报到断掉的地方,`world fs diff --events` 把这份残缺的清单印出来、退出 0。同一个 bug 往下一层还有一份:`dir_is_empty()` 是 `opendir(3)` 上的一个 bool,EACCES/EIO 在它眼里等于"非空",于是只在一侧存在的目录那一行**悄悄没了** | walk 的 errno 就是这次展开的结果:`expand_side()`/`report_one_side()`/`verify_candidate()` 一路往上传,候选循环遇到就停,`wfs_world_diff()` 回的是这个 errno 而不是半份 diff(CLI 照常印出来、非 0 退出)。`dir_is_empty()` 改成回 `0/-errno`,**只有真读到底的目录才算空**;`fs_gone()`(候选在我们看它的时候被删了)仍旧是"什么都不报",和原来一样。顺带把 walker 本身审了(全扫那条路也靠它):`fs_walk_tree_ex()` 的 readdir(3) 回退路把 NULL **一律当成目录读完了**,而 NULL 也是 readdir 报错的方式——一次 EIO 于是让整趟 walk 回 0、条目少几个:克隆少文件、清单少行、diff 少变更,全都报成功。现在每次调用前清 `errno`、NULL 之后读它。`getattrlistbulk(2)` 那条路、`open(2)`/`fstatat(2)`/`FS_DIRS_POST` 的收尾 `lstat(2)` 本来就是传 errno 的 | `7bef4c9` |
| P2 `PRRT_kwDOUf7jGc6kEbHD` | `core/src/diff.cpp` 候选的两侧查找、全扫的 `side_entry()` | 两侧的 `lstat` 都是 bool:0 是"在",别的一律是"不在"。父目录 EACCES、EIO、目录位置上现在是一个指向自己的符号链接(ELOOP)、卷没挂上——全被读成"不在",而 diff **把它报出去**:一侧失败是一条凭空的 A 或 D(谁都没碰过的文件,印成"已删除",退出 0),两侧都失败是这个候选**一声不吭地消失** | `side_lookup()` 一处问、三处用,errno 留着:`fs_gone()`(ENOENT/ENOTDIR——候选在我们看它的时候被删了、父路径上是个文件)是唯一的"不在",别的 errno 直接结束这次 diff。同一轮的审计还包括:`wfs_world_diff_ex()` 顶上快照根那次 `lstat` 原来把**任何**失败都读成 `WFS_E_SOURCE_GONE`(那是在叫人去别处重新 fork,对自己 store 里的一个 EACCES 是错的建议),改走 `fs_probe`/`fs_gone`;xattr 比对(第九轮)本来就把 `XA_ERROR` 和 `XA_EQUAL` 分开、报 `T`、计进 `stats.xattr_errors`、快照侧的 EACCES/EPERM 直接致命——确认无改;`content_equal()`/`link_target_equal()` 读不到时回"不相等",那不是"不在"的判词,而且它倒向**报出一处变更**(M)、从不倒向沉默,和第九轮给 xattr 选的方向一致,维持原样 | `3f8e58e` |
| P2 `PRRT_kwDOUf7jGc6kEbHE` | `core/src/hardlinks.cpp` `restore_group()` / `group_still_linked()` / `hardlinks_verify_groups()` | 重放里每一次查成员(`fstatat`/`openat`)失败都记成 `missing` 然后接着放。`missing` 是**故意**容忍的——第五轮那条:**活的**源可能在扫描和克隆之间变了。可 EACCES/EIO/ELOOP 不是源变了,是这个问题**没被回答**;而从一个 **IMMUTABLE 快照** fork 根本没有这个借口。fork 和 pool 填充只认 `first_err`、从不读 `broken`,于是克隆被发布出去、组里的名字是各自独立的 inode、fork 回 0 | `fs_gone()` 是重放里唯一的"不在",树根、找正身的那一趟、每个成员的父目录下降、每个成员的 `fstatat` 都照这条办;别的 errno 进 `first_err`,和第四轮起被拒的 `link(2)` 一样让快照创建 / fork / pool 填充整个回退。`group_still_linked()` 改成回 `0` + 一个 bool("这棵活树上这个组已经断了")或 `-errno`("这棵活树问不出来"),于是活源那条路留着它对**不在**的容忍、失去它对**错误**的容忍。`hardlinks_verify_groups()`(第十三轮)是同一件事的另一头:它对**任何** lstat 失败都回 `-EINVAL`,而每个调用者把 `-EINVAL` 读成 `WFS_E_SNAPSHOT_DIRTY`——那是在告诉用户快照坏了、去重新做一个,对"成员不在"是对的,对"这棵树读不出来"是错的。非 `fs_gone` 的 errno 原样返回,fork 和 pool filler 透传,只有 `-EINVAL` 还变 SNAPSHOT_DIRTY;`wfs_snapshot_verify` 仍旧把它计进 `modified`——它是一份报告不是一次拒绝,而且它自己两行之后对同一棵树的 walk 本来就会把那个 errno 返回来 | `50e0e5a` |

**新增的测试接缝**:`wfs_test_before_hl_replay`(fork 里克隆已经建好、一条 `link` 都还没做的那一瞬,
给的是临时树的路径)。开发机上没有别的办法造出"成员查不了但成员在"这种状态。

**FSEvents 有一条值得写下来的性质**(它决定了 P2 `…HD` 的 fixture 怎么搭):**重放时它会重新解析路径**。
把 World 里的 `a` 改成 `0000`,重放回来的就只剩 `a` 一条(实测),它自己底下的候选根本不出现——
于是"用 World 侧不可读的目录"去测候选查找是测不到东西的。所以那个 fixture 把**快照侧**的 `a` 关掉:
它完全不参与 FSEvents 的解析,而且它是在 gate 窗口里读的,正是一个坏掉的 store 该现形的地方。World
侧那一半由末尾的 ELOOP 用例覆盖。

**验收**:`ctest`(WFS_FSKIT=OFF)**2/2**;`safety.sh` **266 passed, 0 failed**;`check-deps.sh` 全绿。
(`m1_criteria.sh` 本轮跳过。)

先把三个都跑红过:

- P2 `…HC`:World 里新增目录 `d/`(里面一个文件),`d` 改 `0000`。**老代码** `--events` 回 **0、0 行**
  ——`A d/f.txt` 就这么没了。
- P2 `…HD`:快照侧的 `a` 改 `0000`(`a/b/h.txt` 两边都在、World 侧改过)。**老代码** `--events` 回
  **0**,印出一行 **`A a/b/h.txt`**——一个两棵树里都有的文件,报成"新增";同一趟里
  `a/b/gone.txt`(World 里建了又删,快照侧 ENOENT)被**悄悄丢掉**。ELOOP 那一半:World 的 `a/b` 换成
  指向自己的符号链接,**老代码** `--full` 回 **0**,印出 `M a/b` 和一条凭空的 `D a/b/h.txt`。
- P2 `…HE`:快照里一对 `(d/a, d/b)`,fork 时用新接缝把克隆的 `d` 改成 `0000`。**老代码** fork 回
  **0**,发布出 W26,里面 `d/a` ino 288091848、`d/b` ino 288091849、`nlink` 1——快照记的是一个 inode
  两个名字,发出去的是两个独立文件。

新增测试:

- `diff_test` `unreadable_dir()`(P2 `…HC`):上面那棵树,`--events` 和 `--full` 都要回 `-EACCES`
  **且一行都不报**(回调一次都不许进);把模式改回去,两条路都列出 `A d/f.txt`。
- `diff_test` `unreadable_lookup()`(P2 `…HD`):快照侧 `a` 关掉时两条路都回 `-EACCES`、一行不报,
  而且 gate 在两条出门的路上都回到 `WFS_GATE_CLOSED`;改回去两条路都列出 `M a/b/h.txt`。末尾是
  `side_entry()` 那一半:World 的 `a/b` 是指向自己的符号链接,`--full` 回 `-ELOOP`。
- `core_test`(P2 `…HE`,接在第十八轮那一块后面):新接缝把克隆的 `d` 关掉,fork 必须回 `-EACCES`,
  `--to` 上什么都没有、旁边不留 `.wfs-fork-` 临时树、World 行数一条没多;接缝清掉之后同一次 fork
  成功,`d/a` 和 `d/b` 是一个 inode、`nlink` 2。

#### PR #1 review 第二十二轮:我们要拿走的名字,不算别人身上的一条链接(2026-09-20)

第二十二轮,Codex 一条 P2,落在第十四轮那半条修法上。checkpoint 会把克隆里的 `.world` 标记删掉
(World 的身份不是快照的身份),所以第十四轮让扫描**跳过**这个名字——对的,但**只跳过了它自己那条
记录**:同一个 inode 上**留下来**的名字,`st_nlink` 里仍旧把标记算着。于是 `.world`、`m1`、`m2` 挂在
一个 inode 上(`nlink` 3)时,分组那一趟在树内只找到 2 个名字,"找到的名字比 `nlink` 少"这条判词把它
当成**伸到树外**的组——什么都不写。可它一点也没伸到树外:第三个名字是我们自己的,而且马上就要被删。
克隆已经把链接断开了、标记也删了,于是快照发布出去的 `m1` 和 `m2` 是**两个独立 inode**,而源 World
里它们是一个 inode 两个名字——P9 存在的意义就是不让这件事发生,而它**一声不吭**地发生了,还被这个
快照的每一次 fork 和每一个 pool 条目继承下去。

| # | 位置 | 问题 | 修法 | 提交 |
|---|---|---|---|---|
| P2 `PRRT_kwDOUf7jGc6kEVx_` | `core/src/hardlinks.cpp` `hardlinks_scan()`(排除名字的处理,第十四轮) | 排除只作用在**记录**上,不作用在 `nlink` 上。留下来的名字带着把被排除名字算在内的 `st_nlink`,分组那一趟的 `k != nlink` 于是把一个完全在树内的组判成外部组,一行都不写;克隆断链 + 标记被删,两个(或更多)幸存的名字在发布出去的快照里成了各自独立的文件。同一笔账还有两处要对齐:`hardlinks` 这个统计第十四轮只跳过了被排除的名字本身,以及 `hl_external` 不该把这种组算进去 | 扫描遇到被排除的名字时记下它的 **`(st_dev, st_ino)`**(一个 `Vec<ExclIno>`,正常情况下**一条都没有**——标记的 `nlink` 通常就是 1;有也只有一条,因为 `exclude` 是单个整名匹配)。分组那一趟读这个 inode 的 `nlink` 时**先减掉**落在它身上的排除名字数,于是三种情形正好是**发布出去那棵树**的三个事实:(1) **减完 `k == nlink` 且 `k >= 2`** → 真正的树内组,照常写进清单,组的 `nlink` 写成 `k`——那正是快照树将有的 `nlink`,而 `hardlinks_verify_groups()` 和清单读者都是拿"成员数 == `nlink`"去问那棵树的(没有排除的组,`k` 本来就等于源的 `nlink`,值不变);(2) **减完只剩一个名字** → 一个普通文件:不成组(组至少两个名字),**也不算外部**(这个 inode 的名字没有一个在树外),而且它在发布出去的树里只有一条链接,所以 walk 里给它记的那一笔 `hardlinks` 在这里**退回去**——整个 inode 的账要等到分组这一趟才看得全;(3) 其余仍旧是外部组,**真在树外**的名字才让一个组算外部。注意 `k == nlink == 1` 只可能来自排除:没有排除时,有记录就意味着 `nlink > 1` | `417a36d` |

**这一轮改了两个对外报出来的数字。** 第十四轮那种形状(标记 + **一个**普通名字)原来落在外部分支——
"计数而不认领",`hl_external` 记 1、`hardlinks` 把那个名字也数进去。按上面第 (2) 条,它现在是一个
普通文件:`hl_external` **0**,`hardlinks` **不数**它。发布出去的那棵树一个字节都没变(标记删掉、
那个名字是独立文件,一直如此),变的是快照行上的数字**终于在描述那棵树**。第十四轮的测试里这两条
断言(`hl_external == 1`、`hardlinks == 3`)因此改成 `0` 和 `2`,它关于树本身的断言一条没动。

**验收**:`ctest`(WFS_FSKIT=OFF)**2/2**;`safety.sh` **266 passed, 0 failed**;`check-deps.sh` 全绿。
(`m1_criteria.sh` 本轮跳过。)

先把测试跑红过:一个 World 里 `.world`、`m1`、`m2` 同一个 inode(`nlink` 3,做法和第十四轮一样——
先有 World 和它的标记,再 `link()` 两个普通名字挂上去),另有一对无关的控制组 `c1`/`c2`,然后
checkpoint。**老代码**:`hl_groups=1`(只有控制组)、`hl_external=2`、`hardlinks=4`,快照树里
`m1`/`m2` 是**两个 ino 各自 `nlink` 1**,fork 出来同样是两个独立 inode。**新代码**:`hl_groups=2`、
`hl_external=0`、`hardlinks=4`,快照树里 `m1`/`m2` 是**一个 inode、`nlink` 2**,`verify` 全 0,fork
出来也是一个 inode `nlink` 2,而 fork 自己的 `.world` 是另一个 inode、`nlink` 1。

新增测试:

- `core_test`(P2,接在第十四轮那一块后面):上面那一段——checkpoint 的三个计数、`verify`
  (`missing`/`modified`/`extra`/`unprotected` 全 0)、快照树里 `.world` 不在而 `m1`/`m2` 同 inode
  `nlink` 2、控制组照旧同 inode、fork 出来的 World 里这一对被重放回来且 fork 的标记与它们无关。
- 第十四轮那一块:两条计数断言按上面改掉(`hl_external` 1→0、`hardlinks` 3→2),其余不动——
  它那棵树的形状本来就是对的,这一轮改的是怎么数它。
#### PR #1 review 第二十一轮:正在被删的树,不能再发给任何人(2026-09-20)

第二十一轮,Codex 一条 P1。第十六轮把 `wfs_pool_drain()` 改成**先删树、证明没了才删行**——这样一个
EPERM 不会留下一棵没人认领的预克隆世界。可是**它留下的那一行还是一行普通的 READY 行**,而
`pool_claim()` **不拿 pool 锁**(也不能拿:fork 不许等 filler),只认 `state=1` + 快照还是那个快照。
于是 `fs_remove_tree()` 正在**遍历**那棵树的时候,一个 pool fork 可以把它认领走、`rename` 到用户的
`--to`、提交一个 ACTIVE 世界——`fork` 返回 0,世界里少文件。修法是把"这一行还算不算发得出去的条目"
写进行里:**两步,行先走,树后删。**

| # | 位置 | 问题 | 修法 | 提交 |
|---|---|---|---|---|
| P1 `PRRT_kwDOUf7jGc6kEE_b` | `pool.cpp` `wfs_pool_drain()`,以及每一处读 `pool.state` 的地方 | drain 先把行读出来,然后逐条 `fs_remove_tree(tmp)` + `fs_remove_tree(path)`,只有两个名字都 `proven_gone()` 才删行(第十六轮)。删除**不是一步**:它是一趟 walk,中间那棵树是"删了一半"的。而这段时间里行还是 READY,`pool_claim()` 在另一个 BEGIN IMMEDIATE 里照样能把它取走——claim 不拿 pool 锁,drain 的 flock 拦不住它。结果:fork 把半棵树 rename 到 `--to`,世界行记的 `entries` 是快照的数目,树里却少文件,而且 `fork` 退出 0。反过来那一半是对的:claim 先提交的话,行已经在 claim 的事务里被删掉了 | **加第三个状态 `POOL_DRAINING = 2`(`pool.h`),drain 改成三步。**(1) 一个 BEGIN IMMEDIATE 把行 `UPDATE pool SET state=2 WHERE id=? AND path=?` 并提交;`sqlite3_changes()` 就是答案——改到了,这棵树归 drain;**改到 0 行**,说明 fork 抢先一步把行连同 claim 一起提交了,那棵树现在归它的 CREATING 世界行,drain **跳过、一根指头都不碰**。claim 与这个事务都是 BEGIN IMMEDIATE,SQLite 的写锁让两者**必有先后**。(2) 删树,和第十六轮一模一样。(3) 只在两个名字都证明没了、并且行**还是**这次 drain 写的那行时才 `DELETE … WHERE id=? AND state=2 AND path=?`。删不掉 → 行留在 DRAINING、errno 原样返回(`discard --force` 照旧失败,快照不动)。**每一个读 `state` 的地方都过了一遍**:`pool_claim` 只认 `state=1`(DRAINING 天然发不出去,这就是整条修法的支点);`ready_count`(`pool ready`、`fork` 的 `pool_left`、`pool fill` 的目标数)同样只数 `state=1`,所以 DRAINING **不算 ready、也不挡 fill 补仓**——它是一棵正在出门的树;`pool_scan`(`pool_collect` / `pool_stranded`)把 DRAINING **无条件判死**,不再走"filler 是不是还活着"那条(drain 和 fill 拿同一把 pool 锁,不可能有活 filler),于是 gc 就是它的重试路径,按共享失败上限;第九轮那次"删之前重问一遍"的 `pool_row_still_doomed_locked()` 同样接受 `state=2`;`wfs_pool_status` 把它记进 `stale`(记成 `building` 等于说"有人在克隆",记成 `ready` 等于答应 fork 一个条目);`gc --status` 的 `pool_stranded` 因此自动数到它;`pool_return` 插的是**新行**(`state=1`),不会盖在 DRAINING 上;`snapshot_refs_locked` 数的是 `COUNT(*)`,DRAINING 照样是引用,所以不带 `--force` 的 discard 仍然拒绝。**schema 不动**:`state` 是普通 INTEGER 列,没有约束,不需要迁移、不动版本号;老版本 core 读到 2 只会当成"不是 READY",既发不出去、也会被它自己的 gc 当成 filler 死掉的行收走 | `6424b47` |

**验收**:`ctest`(WFS_FSKIT=OFF)**2/2**;`safety.sh` **266 passed, 0 failed**(新增 3 条 PR21,改写
第十六轮那条关于 `gc --status` 的断言);`check-deps.sh` 全绿。(`m1_criteria.sh` 本轮跳过。)

先把测试跑红过(新缝 `wfs_test_in_pool_drain`,phase 0 = 行变状态之前,phase 1 = 提交之后、删树之前):
phase 1 的钩子先把条目里的 `b.txt` unlink 掉——`fs_remove_tree()` 是一趟 walk,fork 撞进去看见的
正是这个形状——然后在**另一个 store 句柄**上 fork 一次。**老代码**:`fork` 返回 0、`from_pool=1`,
世界行记 `entries=4`(快照的数目),而 `b.txt` **不在树里**(`read_file` 回 `-ENOENT`)。**新代码**:
`from_pool=0`——fork 自己克隆一棵,`a.txt`/`b.txt`/`sub/c.txt` 三个文件都在,`entries` 与快照一致,
drain 照样把那个条目删掉(`removed=1`)。

新增测试:

- `core_test`(P1,一个块三段):(1) 上面那一段;(2) 镜像——phase 0 时 fork 先提交,drain 的
  `UPDATE` 改到 0 行、**跳过**这棵树,世界完整、`removed=0`;(3) 删不掉的那种——给条目里的 `sub`
  加 owner deny `delete,delete_child` 的 ACL,drain 回 errno、行 `state=2`(直接查库)、
  `pool ready` 是 0、`pool status` 记成 stale(不是 ready、不是 building)、`gc --status` 的
  `pool_stranded ≥ 1`、这期间的 fork 只会自己克隆、`discard S<n> --force` 回**同一个** errno 而快照
  仍是 ACTIVE;ACL 拿掉之后一趟 `gc` 把树和行都收走,`pool_stranded` 归零。
- `safety.sh`(3 条 PR21,接在第十六轮那组后面):行是 DRAINING(`sqlite3` 查 `pool.state`)、
  `gc --status` **数得到**它、这期间 `fork` 的输出里没有 `(pool)` 而树是完整的。第十六轮原来那条
  "`gc --status` 什么都不该报"正是本轮要改的行为:那棵树**确实**在等 collector,报出来才对。

#### PR #1 review 第二十轮:不是我们起的名字,就不是我们留下的东西(2026-09-20)

第二十轮,Codex 两条(一条 P1、一条 P2),都是同一条老规矩的漏网之鱼:**一棵树是不是我们的,
要有证据,不能靠名字长得像。** P1 在 trash 删除的第 0 步——`trash_fold_leftover()` 把 `<条目>.deleting`
当成"我们上一次没删完的",递归删掉;可是从第十五轮起 rename 和记名字在**同一个事务**里,两个名字
**按构造不可能同时是我们的**,所以那个兄弟目录永远是别人的——而跨卷 discard 的 trash 就在用户自己的
`<parent>/.wfs-trash` 里,名字 `W<id>-<时间戳>` 完全可以猜。P2 在 fork 失败的 unwind 上:临时树删不掉
的时候,那条 CREATING 行——树名字的**唯一**记录——照样被删掉了。**一条一个提交、一个测试,先验证过
"没有修复就会红"。**

| # | 位置 | 问题 | 修法 | 提交 |
|---|---|---|---|---|
| P1 `PRRT_kwDOUf7jGc6kDwWf` | `world.cpp` `trash_fold_leftover()`(collector 与 `--now` 两个调用点) | 删一条 trash 条目的第一步是一次 rename:`<条目>` → `<条目>.deleting`。这一步前面跑的 `trash_fold_leftover()` 把已经在那个名字上的东西**递归删掉**,理由是"只可能是我们自己上一次被打断的尝试"。从**第十五轮**起这个理由不成立了:`gc_claim_deleting()`(以及 `--now` 的 `trash_delete_now()`)在**一个事务**里重读行、rename、记下新名字,所以我们自己造成的状态里,两个名字**任何一刻只有一个存在**——rename 和提交之间崩掉是"行记 X、树在 X.deleting、X 不在",正是 `trash_follow_deleting()` 解的那个;而 discard 本身(第五轮:行、rename、行)从不写出 `.deleting` 名字。于是"X 和 X.deleting 同时在"**永远**意味着第二个名字是别人造的——而 World 与 store 不同卷时,trash 是**用户自己目录**里的 `<parent>/.wfs-trash`,条目名 `W<id>-<时间戳>` 完全可以猜。用户在那里放一个目录,`gc` 或者 `discard --now` 就把它连同里面的文件递归删光,而且 `--now` 还报成功退出 0 | **折叠整个删掉,并且只往"证明了不在"的名字上 rename。** `trash_mark_deleting()` 现在先 `fs_probe()` 那个 `.deleting` 名字:在 → `WFS_E_TRASH_BLOCKED`(新错误码),问不出来 → 把 errno 原样返回(第十二轮那条"证明不了不在就当它还在"),只有证明不在才 rename——`rename(2)` 本身会把一个**空**目录整个吞掉、对非空的回 ENOTEMPTY,所以这道检查不能省。collector 碰到这种条目:树和兄弟目录**一根指头都不碰**,计进新的 `trash_blocked`,按路径共享的失败计数器重试几次(兄弟目录可能是别人的临时名)、到上限就不再自己唤醒自己,`gc --status` 用新的 `trash_blocked`/`blocked_path` **把那个目录的路径说出来**,`gc` 的提示同理。`discard --now` 返回 `WFS_E_TRASH_BLOCKED`,CLI 用新的 `wfs_trash_blocked_path()` 把挡路的目录名报出来——跨卷那种情况下,要看的是用户**自己的**一个目录。**同一条规矩再往前一步**:`trash_is_deleting()` 原来拿一句 `exists(<名字>.deleting)` 判"collector 已经开始删了",于是那个兄弟目录会让 `restore` 一直回 `WFS_E_TRASH_DELETING`(而这句话是假的)。以前这个状态是瞬态的(折叠会把兄弟目录删掉),现在不是了,所以改成问 rename 真正留下的形状:树在 `.deleting` **且不在**自己的名字上。**顺带审计"按后缀折叠 / 删一个不是我们命名的兄弟"**:`rm_tmp_in_store_dir()` 只扫 `<store>/snapshots` 且逐个回 snapshots 表问(第十轮),pool 的孤儿清扫问 pool 行和 World 行(第九轮),trash 的孤儿判定只读 `<store>/trash` 且在 store 锁下重问活行(第五/九轮),`<store>/tmp` 按年龄清 store 内部的文件;剩下两处 `if (exists) remove` 都在 `<store>/…<id>.wfs-tmp` 上,名字由 store 自己分配。**trash 是唯一一处手能伸进用户可见目录的** | `c1eeb6a` |
| P2 `PRRT_kwDOUf7jGc6kDwWk` | `world.cpp` `wfs_world_create_ex()` 的 unwind | fork 在克隆已经建出来之后失败,unwind 先 `fs_remove_tree(tmp)`、再 `DELETE FROM worlds`——**删除的结果被扔掉**。而那条 CREATING 行是这棵树名字的**唯一**记录:临时树在**用户自己**的目标目录里,名字是 64 位随机数,gc **故意**从不按后缀扫用户目录(`rm_tmp_in_store_dir`,第三轮那条规矩)。于是一个 EPERM(ACL、只读父目录、瞬时 EIO)就留下一棵完整的半成品克隆,store 里没有任何东西叫得出它的名字:`gc --status` 数不到、collector 不会重试、`adopt` 也认不回来 | **`proven_gone()` 说了算**(第十二轮):证明没了 → 照旧删行;还在 → 行留着,保持 CREATING、`tmp_path` 不动,把**原始的那个错误**还给调用者。这正是"fork 在发布前被 kill"留下的那种行,所以下游本来就认:`gc_tmp_is_removable()` 问标记这棵树归谁、问行是不是还 CREATING 且还记着这个 `tmp_path`,生产者判死之后的那趟扫描按共享失败上限重试,`wfs_gc_status()` 计进 `creating_stranded`。pool 那条路(第十六/十八轮)不用改也是一致的:它的树是 `<store>/pool/S<n>/<uuid>`,store 内部的名字、pool 清扫自己问行就能找到;而树真的落在用户 `--to` 上的那一种,第十八轮已经改成留行 | `61a9f00` |

**验收**:`ctest`(WFS_FSKIT=OFF)**2/2**;`safety.sh` **264 passed, 0 failed**(新增 7 条 PR20);
`check-deps.sh` 全绿。(`m1_criteria.sh` 本轮跳过。)

先把测试跑红过:

- **P1**:一个 discard 进 trash 的 World,旁边手工造一个用户目录 `<条目>.deleting/keep/user.txt`。
  **老代码**:`world fs discard W1 --now` **退出 0、打印 "W1 deleted"**,而 `user.txt` 连同整个
  `.deleting` 目录**没了**(手工复现同样确认)。**新代码**:`--now` 退出 3 并把挡路的那个目录的
  完整路径说出来;`gc --now --retention 0` 两样都不碰、输出 "could not be collected";
  `gc --status` 打出 `blocked: … is in the way`;`restore W1` 仍然成功(不再谎称"正在被删除");
  把那个目录拿走之后,同一条 discard + gc 照常把条目收掉。
- **P2**:`wfs_test_before_fork_publish` 这次**返回 0**(不是 kill -9),在缝里给刚克隆出来的树里
  一个子目录加上 owner deny `delete,delete_child` 的 ACL(remover 会 chmod、会清 chflags,只有 ACL
  拦得住),再在 `--to` 上造一个目录,于是发布那次 rename 是 P7 的拒绝、unwind 带着树跑。
  **老代码**:树还在磁盘上,而 `wfs_world_info()` 回 **-ENOENT**——行没了。**新代码**:fork 回
  `-EEXIST`,行是 CREATING 且 `tmp_path` 还在,`gc --status` 的 `creating_stranded ≥ 1`,ACL 还在时
  gc 留着行并计 `tmp_failed`,把 ACL 去掉之后下一趟 gc 删树、行落 DEAD。

新增测试:

- `safety.sh`(P1,7 条 PR20):`--now` 的拒绝与它报出的路径、collector 两样都不动、`gc` 的提示、
  `gc --status` 的 `blocked:` 行、条目仍可 `restore`、清掉兄弟目录之后照常收。放在 CLI 上是因为
  这条 bug 的受害者是用户目录里的一个目录,CLI 正是他看见这件事的地方。(跨卷那一半没法在测试里
  造真的第二个卷,逻辑完全相同:`trash_mark_deleting()` 只认路径。)
- `core_test`(P2):上面那个缝跑一遍,断言行、树、`gc --status`、两趟 gc 的行为。

#### PR #1 review 第十九轮:分隔符只有一个空格,键必须是键(2026-09-20)

第十九轮,Codex 两条,都是 P2,而且是同一类毛病的两个化身:**用"找到这几个字节"代替"按语法读"**。
第一条在两份清单的读者上——`sscanf` 的数字后面跟了一个空白指令(`%llu %n`),而 scanf(3) 的空白指令
会把够得着的空格、制表符、CR、LF **全部**吃掉,不只是写者放的那**一个**分隔符;于是名字开头的空格
连同名字本身的第一段一起没了(` a` 读成 `a`)。第二条在 `.world` 标记的读者上——`json_find()` 用
`strstr` 找 `"<键>"`,而值是写在后面那些键**之前**的,值里一个转义引号就能把这几个字节凑出来,
匹配落进路径里、冒号那一关没过,函数**当作没有这个键**返回。**一条一个提交、一个测试,先验证过
"没有修复就会红"。**

| # | 位置 | 问题 | 修法 | 提交 |
|---|---|---|---|---|
| P2 `PRRT_kwDOUf7jGc6kDjTg` | `hardlinks.cpp` `hardlinks_manifest_read()` + `world.cpp` `wfs_snapshot_verify()` 的清单读者 | 两份清单的读者都用 `%llu %n` 收尾——**结尾那个空白指令**把分隔符和名字开头的空格/制表符一起吃掉了。两个写者都只写**一个**空格,后面是原样的名字:`fprintf(f, "hl %llu %llu ", …)` + `put_escaped()`,以及 `Manifest::line()` 的 `"%c %o %llu %lld.%ld %llu "` + 同一套转义。于是一个以空格或制表符开头的名字(文件名里的普通字节:下载、编辑器、`tar` 解出来的树都有)读回来就少了开头那一段:` a` 读成 `a`——要么是**隔壁那个文件**,要么谁也不是。代价是整个快照:`init` 刚做完的 `verify` 就报 5 处(4 条条目行 `lstat` 到了别的文件,再加硬链接段——第十三轮那道组检查拿着读错的名字去 `lstat`,落到两个不同的 inode 上),此后这个快照的**每一次 fork、每一次 pool 填充**都是 `WFS_E_SNAPSHOT_DIRTY`,而且是永久的:清单和树都原封不动地躺在那里 | **只在读者这边改:分隔符就是一个 `' '`,后面到行尾全是名字。** 两处都把格式改成 `%llu%n`(`verify` 那处是 `%llu%n`),`%n` 落在数字的末尾,然后**显式要求**那个偏移上正好是一个空格,名字从下一个字节开始,再走 `unescape()`。格式一个字没动,所以磁盘上已有的清单读回来一字不差——包括 `verify` 清单里根那一行:它的分隔符就是行尾最后一个字节,`rel` 依旧是空串。`#hl` 头那一行后面没有自由文本(五个数字读完就完了),不受影响;`core_test` 里那几个手工改清单的辅助函数也按同一条规矩解析了 | `0a8be3c` |
| P2 `PRRT_kwDOUf7jGc6kDjTk` | `core/src/world.cpp` `json_find()` | `json_find()` 拿 `strstr(js, "\"<键>\"")` 在**整份标记**里找,于是这几个字节最先出现的地方就赢——而每一个值都写在它后面那些键之前。`json_escape()` 把值里的 `"` 写成 `\"`,所以只要路径里有一个引号后面跟着那个词,文本里就出现了 `"world`,再加上这个值**自己的**收尾引号,`"world"` 就凑齐了:store 目录叫 `q"world` 时,`strstr` 落在 `store_path` 里,后面一个字节是 `,` 而不是 `:`,函数于是**返回没找到**——而那个键就在两行之下。`marker_read()` 把 world id 留成 0,这个 store 里的**每一个** World 对 `verify`、`mount`、`fork` 都是 `WFS_E_UNREGISTERED`:一个目录名里的引号,废掉整个 store。World 的**名字**对它后面每一个键是同样的效果(`w"snapshot` 吃掉 snapshot id) | **改成顶层键遍历。** 从对象的 `{` 开始,一对一对地走 `"键": 值`:字符串按转义走完(`\"`、`\\`,`\uXXXX` 的四个十六进制位在跳过反斜杠对之后就是普通字节),值按种类跳过(字符串;数字/`true`/`false`/`null` 走到下一个分隔符;标记虽然是平的,`{}`/`[]` 仍按深度计数走完,里面的字符串照样先跳过,免得括号被字符串里的字符骗到),键**整体**比较。`json_str()`/`json_u64()` 一个字没动、从同一个偏移开始读,写者没动,C ABI 没动:磁盘上已有的标记解析结果完全一样——变的只是"什么才算一个键" | `7b25d5e` |

**验收**:`ctest`(WFS_FSKIT=OFF)**2/2**;`safety.sh` **257 passed, 0 failed**(本轮没加,两条都在核心里);
`check-deps.sh` 全绿。(`m1_criteria.sh` 本轮跳过。)

先把测试跑红过:

- **名字以空格开头(P2 其一)**:源树里两对硬链接 `(" a", " b")`、`("\ta", "\tb")`,外加两个同样
  大小、同样 mtime 的诱饵 `a`、`b`。**老代码**:`wfs_snapshot_create` 成功,紧接着的
  `wfs_snapshot_verify` 回 **-1008**(`modified=5`、`missing=0`、`extra=0`),而
  `wfs_world_create_ex` 和 `wfs_pool_fill` 也都回 **-1008**——刚做出来的快照谁也用不了。
  (第十三轮那道"组必须是那棵树的组"把"焊错文件"挡在了前面,于是损坏的形状是**永久拒绝**而不是
  内容被盖;第十一轮那条"名字不许重复"同理:两组读错之后撞名也是 `-EINVAL`。)**新代码**:
  `verify` 全 0,fork 把两对都重建出来,诱饵 `a`、`b` 仍是两个独立 inode、内容各是各的;
  pool 填充 + 命中的 fork 同样。
- **标记里的键被值吃掉(P2 其二)**:store 开在 `<root>/q"world`,World 名字 `qw"snapshot`。
  **老代码**:`wfs_world_verify_identity` 回 **-1002**("unregistered copy of a world"),
  world id 读成 **0**。**新代码**:`registered=1`、world id / snapshot id / 名字全对,
  `wfs_world_marker_store()` 也认得出这就是本 store 的路径。

新增测试:

- `core_test`(P2 其一):`init` → `verify`(干净)→ `fork --no-pool`(两对都在、诱饵还是两个
  独立文件、内容各是各的)→ `pool fill` + 命中的 fork(同上)。
- `core_test`(P2 其二):带引号的那个 store 走一遍 `init`/`fork`/`verify identity`/
  `marker_store`;另外钉住两件事——目录分量**就叫** `world`(没有引号)这种本来就不会错的形状
  得继续对,以及别人 store 的 World 依旧是按 store id 判成 `WFS_E_FOREIGN_STORE`(键照样读得出来)。

#### PR #1 review 第十八轮:源树自己上的锁要借一下,回滚也是一次会失败的 rename(2026-09-20)

第十八轮,Codex 两条,都是 P2。第一条落在硬链接重放上:第四轮那个"只读目录借一下写位"借的只有
**目录**,而 `UF_IMMUTABLE`/`UF_APPEND` 是**用户**标志、`clonefile` 会原样复制到克隆的每一个名字
上,`link(2)` 和 `rename(2)` 对它们一律 EPERM——一棵源树上有人 `chflags uchg` 过一个硬链接文件,
`snapshot create`/`pool fill`/`fork` 就全都拒绝。第二条落在 pool 命中那条路的 unwind 上:把树从
`--to` 搬回 pool 名下的那次 rename**也会失败**,而它的返回值被扔掉了,于是 CREATING 行被删掉、
一棵带着 `.world` 标记的树留在用户目录里没有任何东西叫得出它的名字。**一条一个提交、一个测试,
先验证过"没有修复就会红"。**

| # | 位置 | 问题 | 修法 | 提交 |
|---|---|---|---|---|
| P2 `PRRT_kwDOUf7jGc6kDTs4` | `hardlinks.cpp` `LendStack` / `relink_under_lend()` | `UF_IMMUTABLE` 和 `UF_APPEND` 是**用户**标志——`chflags uchg`,vendor 进来的树、发布目录、被人冻住的 fixture 都可能带着——而 `clonefile(2)` 会把它们原样复制到克隆的**每一个名字**上(实测:源里一个 inode 两个名字带 `uchg`,克隆出来是两个 inode、两个都带 `0x2`)。重放那一对系统调用于是一个都跑不了:`link(2)` 对 immutable / append-only 的**源**回 EPERM,`rename(2)` 要顶掉一个 immutable 的**目标**也回 EPERM。第四轮那条重试只借**目录**的写位,帮不上忙;而第四轮起重放失败是致命的(`first_err` 一路返回,快照/fork/pool 填充全部 unwind),所以一棵完全正常的源树会让 `snapshot create`、`pool fill` 和每一次 fork 都回 `-EPERM`,而且是因为用户在树里某个文件上有意设的一个标志 | **那两个 inode 也照目录的办法借。** 重试路径上除了目录,还把这一对调用真正碰到的两个文件借下来:**正身文件**(每一个新名字最后都是它)和 **rename 要顶掉的那个名字**。fd 都是从第十七轮那个目录描述符 `openat(O_RDONLY\|O_NOFOLLOW)` 拿的——成员路径的叶子同样不许是符号链接,而且借的就是 link/rename 要进去的那个目录里的那个文件;immutable 的文件是允许只读打开的,清掉自己文件上的 `UF_` 标志是属主的权利。清标志 → link + rename → **一字不差地还回去**,成功和失败两条路都还(和目录模式同一个 `LendStack`,析构里倒着还)。一组只还一次:`link` 之后每一个名字就是同一个 inode 了,还它就是还整组。唯一不还的是被 rename 顶掉的那个 inode——它已经没有名字了,标志没有东西可穿。(`--hard` 快照不走这条:fork 会先 `fs_unprotect_tree` 把整棵克隆解开再重放。这一条管的是**源树自己**带的标志) | `1a92a21` |
| P2 `PRRT_kwDOUf7jGc6kDTs9` | `world.cpp` `wfs_world_create_ex()` pool 命中那条路的 unwind | pool 命中那条路的尾巴是 marker、rename、stat、UPDATE。stat 或 UPDATE 失败时树**已经在**用户的 `--to` 上了,所以 unwind 先把它 rename 回 pool 名下——而这次 rename 的返回值是**扔掉**的。`pool_return()` 于是 `stat` 到一个空的 pool 路径、回 `-ENOENT`,调用方把这个非零读成"条目没回去,那 CREATING 行只能自己走"并**删掉了那条行**:`--to` 上留着一棵带 `.world` 标记的完整世界,而 store 里没有任何东西叫得出它的名字——gc 的半成品 fork 清扫是从 CREATING 行的 `tmp_path` 出发的(第五、七轮),`world fs adopt` 要 marker 指着本店**有**的那个 world。更糟的是 fork 并不停:它接着掉进普通克隆那条路,在**同一个 `--to`** 上又克隆一棵树,然后报成功 | **回滚的结果要查。** 真回去了才进 `pool_return()` 和第十六轮那个"行出去和条目回来在同一个事务里"的 unwind;没回去就到此为止:行**留在 CREATING**,`tmp_path` 改写成这棵树现在真正的名字,也就是那个已经发布的 `--to`。这样的行正是"fork 在 publish 之前被 kill"留下的那一种——树在数据库里唯一的名字(P8)——所以 `gc_tmp_is_removable()` 原样接受它:它问 marker 这棵树是谁的(`.world` 里的 world id 就是这一行),问行是不是还写着 CREATING + 这个 `tmp_path`,两问都对得上;`world fs adopt` 也一样能收。调用方拿到的是**最初那个错误**,而不是一棵没人认领的树旁边一份新克隆。整段时间里快照始终有引用(那条 CREATING 行),第十六轮那条不变量照旧 | `84d22e8` |

**验收**:`ctest`(WFS_FSKIT=OFF)**2/2**;`safety.sh` **257 passed, 0 failed**(本轮没加,两条都在核心里);
`check-deps.sh` 全绿。(`m1_criteria.sh` 本轮跳过。)

先把测试跑红过:

- **源树带 `uchg`(P2 其一)**:`d/a`、`d/b` 一个 inode 两个名字,`chflags uchg d/a`。**老代码**:
  `wfs_snapshot_create` 回 **-1(Operation not permitted)**——快照根本做不出来。单独探过底层:
  `clonefile` 之后克隆里两个名字**各带 `0x2`**,`link(克隆的 a, 临时名)` → EPERM;把正身的标志清掉
  之后 link 过了,`rename(临时名, 克隆的 b)` 又 → EPERM(**被顶掉的那个名字自己也是 immutable 的**),
  所以两个 inode 都得借。`uappnd`(`0x4`)一模一样。**新代码**:快照做得出来、组记在案,gate 后面
  `d/a`、`d/b` 还是一个 inode 两个名字、标志还在;fork 和 pool 命中的 fork 都把这一对重建出来,
  标志还在,树里不留 `.wfs-hl-` 临时名。
- **回滚跑不了(P2 其二)**:新的 `wfs_test_after_pool_publish` 在 publish rename 之后、拥有这棵树的
  那条 UPDATE 之前把树搬到旁边一个名字上,于是 stat 和回滚 rename 都回 ENOENT。**老代码**:
  `wfs_world_create` 回 **0**(而且新建了 **W2**——在同一个 `--to` 上又克隆了一棵),
  `wfs_world_info(W1)` 回 **-2**(CREATING 行被删了),被发布出去的那棵树读回来是
  **"unregistered copy of a world"**(`registered=0`、`has_marker=1`)——没人认领,`pool ready=0`。
  **新代码**:fork 失败,行还是 CREATING、`tmp_path` 写着那个 `--to`,pool 里没有条目回去,
  `discard S<n>` 还是 `WFS_E_SNAPSHOT_IN_USE`,`world fs adopt` 把搬走的那棵树收编。

新增测试:

- `core_test`(P2 其一):`uchg` 和 `uappnd` 两趟,每趟三条路——`snapshot create`(组记在案、gate
  后面标志还在、`.wfs-hl-` 不留)、`fork --no-pool`(重建那一对、标志还在)、`pool fill` + 命中的
  fork(同上)。
- `core_test`(P2 其二):同一条 seam 的两个形状。**树被搬走**:回滚 ENOENT,行留 CREATING 且
  `tmp_path` 指着 `--to`(直接读 `metadata.db` 的那一列——没有任何公开结构带它),pool 没有条目
  回去,快照仍被引用,`world fs adopt` 收编搬走的那棵树。**树留在原地**:把 `--to` 的父目录 `chmod 0`,
  stat 和回滚 rename 都回 EACCES,树就留在发布出去的位置上——这正是 gc 要认的那一种,
  `wfs_test_fork_owner_pid` 把生产者写成一个死 pid 之后,`wfs_gc` 报 `tmp_removed`、树没了、行落 DEAD。

#### PR #1 review 第十七轮:分量必须是真名字,标记指着谁是判决不是提示(2026-09-20)

第十七轮,Codex 两条:一条 P1、一条 P2。P1 是第十一、十四轮那条"成员路径必须是树内相对路径"
最后没盖住的一层:那两轮管的都是**拼法**(`/` 开头、`..`、空分量、`.`),而 `s/x` 里的 `s` 是一条
**符号链接**时每一条拼法规则都满足——`lstat(2)` 只放过最后一个分量、前面的照跟,`link(2)` 和
`rename(2)` 一个都不放过。于是核对在快照那边跟着链接走,重放在克隆这边跟着**同一条相对链接**走到
另一个地方去了。P2 是 T2.3 那条"store 路径搭着 marker 旅行"的另一头:CLI 说"扩展会退回容器默认
值",而扩展在 marker 里的路径非空时根本不退。**一条一个提交、一个测试,先验证过"没有修复就会红"。**

| # | 位置 | 问题 | 修法 | 提交 |
|---|---|---|---|---|
| P1 `PRRT_kwDOUf7jGc6kDBZ4` | `hardlinks.cpp` `manifest_path_sane()` / `hardlinks_verify_groups()` / `restore_group()` / `group_still_linked()` | 路径检查是**纯字面**的,中间分量是符号链接时一路放行,而这一组用到的三个系统调用**全都跟着它走**:`lstat(2)` 只放过最后一个分量,`link(2)`、`rename(2)` 连最后一个也跟。相对链接在树的两份拷贝里落到**不同的地方**——快照根是 `<store>/snapshots/S<n>/root`,fork 的临时目录在 `--to` 的父目录下,深度和父目录都不一样——所以一份写着 `s/x`、`s/y` 的清单可以拿**快照旁边**种下的一对硬链接验过(两个成员 `lstat` 到同一个 inode、nlink 正好是 2),重放却在**克隆旁边**把两个完全不相干的文件 link + rename 焊在一起:其中一个的内容没了,而 fork 报成功。从活 World fork 那条路上更直接——那条路上没有 `hardlinks_verify_groups`,唯一的把关是 `group_still_linked()`,它同样跟着链接走 | **把成员路径当成它本来的样子走一遍**:从树根起,一个分量一个分量地 `openat(O_DIRECTORY\|O_NOFOLLOW)` 下去,叶子上的每一个动作都是相对父目录描述符的 `*at(2)`——`fstatat(AT_SYMLINK_NOFOLLOW)`、`linkat`、`renameat`、`unlinkat`。检查和动作因此**是同一条系统调用路径**:名字不会被解析第二遍,中间没有窗口,分量是符号链接就是 `ELOOP`,而且是在任何地方创建任何东西**之前**。第四轮那个"只读目录借一下写位"也搬到同一个描述符上(`fstat`/`fchmod`/`fchflags`),借的于是就是 link 要进去的那个目录。`hardlinks_verify_groups` 把这种成员判成 `-EINVAL`(调用方一律映射成 `WFS_E_SNAPSHOT_DIRTY`),重放那边把它当成"不去碰的名字"(`skipped`),组判 broken——快照创建于是把它从清单和 `hl_groups` 上去掉。写者这边确认过:walker 只对 `S_ISDIR`(来自 `AT_SYMLINK_NOFOLLOW` 的 `fstatat`/`lstat`)递归,符号链接在它眼里从来不是目录,所以扫描写不出这种路径,`exclude` 是整条 `rel` 的全等匹配、diff 的 walker 同理,都不受影响 | `76ee115` |
| P2 `PRRT_kwDOUf7jGc6kDBZ6` | `cli/main.cpp` `world fs mount`(+ 新的 `wfs_world_marker_store()` / `wfs_world_marker_refresh()`) | macOS 27 上 `-o` 选项到不了 FSKit 模块,所以扩展是从挂载源根目录的 `.world` 里读 store 路径的——而 `WorldVolume.mm` / `WorldVolumeHandler.mm` 只要读到的路径**非空就直接用**,只有一个路径都没拿到时才退回自己容器里的默认 store。CLI 却在路径对不上时打一句"扩展会退回容器默认值",然后照挂不误:store 搬过家、或者 marker 是从别人 store 里拷出来的,挂上来的就是**另一个 store**,`-o world=<n>` 里的 id 是拿它去解析的,而同一条命令里其它所有事都是对着 CLI 打开的那个 store 做的 | **问核心,而且按 P1/P2 已有的那条界线分两种答案。** 新的 `wfs_world_marker_store()` 报三件事:marker 到底带不带路径、它的 store id 是不是**这个** store 的、它的路径**解析之后**是不是这个 store 的目录(解析,不是比字符串——扩展要 `open(2)` 的就是它;解析不出来的路径当然不是这个 store)。`mount` 于是在非空且不是本 store 时**拒绝**,两个路径都说出来,并说清是哪一种;只有"marker 根本没带路径"那一种才还是提示,因为那时扩展的退路是真的。`macos/fskit` 一个字没动。出路是 `wfs_world_marker_refresh()`,命令是 `world fs verify <world> --refresh-marker`:只改路径,world id、名字、来源 snapshot、`created_at` 全部原样(`marker_read` 为此补了 `created_at` 的解析),而且只对**这个 store 真正拥有**的 World 动手——marker 的 store id 得是本店的、行和 inode 都得对得上,所以副本是 `WFS_E_UNREGISTERED`、别人的 World 是 `WFS_E_FOREIGN_STORE`,两样都是 `adopt` 的事。它按 P12 拿 World 锁(和 fork/checkpoint/discard 串行),新 marker 写在旁边再 rename 盖上去,崩在中间留下的是**完整的老 marker**。`fs verify` 自己也这么报:它本来就是"这个 World 是不是它自称的那个"这个问题 | `7e7cb57` |

**验收**:`ctest`(WFS_FSKIT=OFF)**2/2**;`safety.sh` **257 passed, 0 failed**(新增 9 条);
`check-deps.sh` 全绿。(`m1_criteria.sh` 本轮跳过。)

先把测试跑红过:

- **符号链接分量(P1)**:一棵树里有一对硬链接 `d/a`、`d/b`,外加一条 `s -> ../hlsym-side/d`;
  快照根旁边种一对真硬链接(让伪造的清单验得过),fork 临时目录旁边种两个**size 和 mtime 相同**
  的普通文件(让重放那道"还是不是扫描看见的那个文件"闸门也拦不住),然后把清单那一组的两个成员
  改写成 `s/x`、`s/y`。**老代码**:`wfs_snapshot_verify` 回 **0**(ok),`wfs_world_create` 回
  **0**,而 `<worlds>/hlsym-side/d/y`——完全在克隆之外的一个文件——回来时 inode 267451317、
  nlink 2、内容是 `x` 的,自己那五个字节没了。从活 World fork 那条路(没有任何 verify)再来一遍:
  `<root>/hlsym-side/d` 那一对同样被焊上(inode 267453124、nlink 2、内容 `farx`)。
  **新代码**:verify、fork、pool fill 全是 `WFS_E_SNAPSHOT_DIRTY`,`--to` 上什么都没发布、
  `.wfs-fork-` 一个不剩,外面那两个文件 inode 各是各的、nlink 1、内容原样;活 World 那条路把组
  丢掉(活树本来就允许变),fork 成功而外面纹丝不动;把清单换回快照自己写的那一份,fork 照常、
  `d/a`/`d/b` 还是一个 inode 两个名字。
- **marker 指着别的 store(P2)**:用**改之前**的二进制跑——把一个 World 的 marker `store_path`
  改写成另一个 store 的路径之后,`world fs verify <path>` **退出 0**、只报"world W1 'p' at …",
  一个字都没提这个 marker 会让挂载开到另一个 store 去;`--refresh-marker` 根本不存在(usage,
  退出 2);而扩展会去 `open(2)` 的那个路径,确确实实是**另一个 store**。**新代码**:退出 3,
  refusal 把两个 store 都说出来、并说是哪一种,`try:` 里给出 `--refresh-marker`。

新增测试:

- `core_test`(P1):上面那棵 `hlsym` 树的两条路——从快照 fork(verify/fork/pool fill 三处拒绝,
  外面的文件一个字节没动)和从活 World fork(`group_still_linked` 拒绝,组被丢掉,外面的文件
  同样没动)。
- `core_test`(P2):三个判决位(`has_path` / `same_store` / `same_path`)各自的取值——原样、
  换成另一个 store 的路径、换成一个解析不出来的路径、清空(T2.3 之前的 marker,那时退路是真的);
  refresh 之后身份、名字、来源 snapshot 一样都没变;别人 store 的 World 是 `WFS_E_FOREIGN_STORE`,
  marker 一个字节没改。
- `safety.sh`(P2,9 条):marker 指着另一个 store 时 `fs verify` 退 3、refusal 里两个 store 都在、
  `try:` 里有 `--refresh-marker`;`--refresh-marker` 改完之后 verify 干净、marker 里除了路径别的
  没变;marker 的 store id 是别人的时候拒绝、`--refresh-marker` 也不接管、marker 原封不动。
  (`fs mount` 只在 `-DWFS_FSKIT=ON` 的构建里存在,这个脚本从不挂载任何东西;它问的是和
  `fs verify` 同一个核心调用。)

#### PR #1 review 第十六轮:引用要在每一个瞬间都在,删不掉的树不算删掉了(2026-09-20)

第十六轮,Codex 两条,都是 P2,都落在 pool 上。第一条是第一轮那道"fork 和 `discard S<n>` 不能
都赢"的**另一半**:认领时那条 CREATING World 行把引用立起来了,可**归还**时是先把它删掉、再把条目
插回去的——夹在两次提交之间的那一格,**谁都没有引用这个快照**。第二条是第八轮、第十二轮那句
"删不掉的树不算删掉了"还没走到的最后一条命令:`discard S<n> --force` 的 drain。
**一条一个提交、一个测试,先验证过"没有修复就会红"。**

| # | 位置 | 问题 | 修法 | 提交 |
|---|---|---|---|---|
| P2 `PRRT_kwDOUf7jGc6kCz5H` | `world.cpp` pool fork 失败后的 unwind + `pool.cpp` `pool_return()` | 从 pool 领走一个条目的 fork,如果**领走之后**失败(marker 写不下去、`--to` 上凭空冒出一个目录于是 `RENAME_EXCL` 回 EEXIST、收尾那条条件 UPDATE 改不到行),unwind 是**两个事务**:先删掉自己那条 CREATING World 行,再 `pool_return()` 把条目插回去。中间那一格,`discard S<n>` 数引用**两样都数不到**——World 没了,pool 行还没回来——于是提交 TRASHING、把树搬进 trash;`pool_return()` 随后照插不误,插出一个 **READY 条目,属于一个已经在 trash 里的快照**,而且它自己既不看快照状态也不拿 pool 锁。`pool_collect()` 要到下一次唤醒才埋它,中间任何一次 fork 都会把这棵完整的陈旧克隆当活基线领走 | **行出去和条目回来是同一个事务。** `pool_return()` 多收一个 `PoolReturnHook`(`PoolClaimHook` 的镜像):它在归还自己的 `BEGIN IMMEDIATE` 里、插完 pool 行之后、提交之前跑,fork 的 `DELETE FROM worlds` 就放在那里。于是**从认领到归还的每一个瞬间都有一条引用在**——提交之前是 World 行,提交之后是 pool 行——`discard` 的 `BEGIN IMMEDIATE` 必然看见其中一条(本轮的测试里看见的是前者:`WFS_E_SNAPSHOT_IN_USE`)。顺带把第十五轮给 `build_one()` 的那道身份检查也给了 `pool_return()`:同一把写锁下重读快照行,要求 ACTIVE 且 `created_at` 正是条目带着的那个,对不上就不插——树删掉、非 0 返回,调用方再单独把自己那条行埋掉。`pool_return()` 因此从 `void` 变成返回 `int`("条目回去了没有"),调用方只有 unwind 这一处 | `c89c171` |
| P2 `PRRT_kwDOUf7jGc6kCz5J` | `pool.cpp` `wfs_pool_drain()`(`discard S<n> --force` 先调它) | drain 是**先删行、后删树**,而且两次 `fs_remove_tree()` 的返回值都扔掉,整个函数无论如何返回 0。一个删不掉的条目(ACL、EPERM、偶发 EIO)于是变成 `<store>/pool` 下一棵**完全没有行的**完整克隆——而 `--force` 拿着那个 0 继续往下走,把快照搬进了 trash。之后**再也没有人回来收**:孤儿清扫确实会找到它,但 `wfs_gc_pending()` 只扫 trash,而这个快照自己那条 trash 条目要过好几天才到期,所以连一个 worker 都不会被起起来 | 和第八轮给 `pool_collect()`、第十二轮给 `gc --reconcile` 的是同一条规矩,只是往前挪了一条命令:**树先删**(条目本体和它旁边的 `.wfs-tmp`),两个名字都 `proven_gone` 了才删行(而且和 `pool_collect()` 一样带上 `path`),删不掉就**留着行、把 errno 还给调用方**。`wfs_snapshot_discard --force` 于是在**碰快照之前**就失败了,条目留下来仍旧是它本来的样子——一条 ACTIVE 快照的普通 pool 行,所以 `gc --status` 什么都不报,因为确实什么都没坏;CLI 把挡路的那个目录和 errno 一起说出来,并且告诉用户修好权限之后**同一条 `--force` 就是重试**。另外确认了一遍:drain 全程在 store 的 pool 锁下,所以没有 filler 在旁边加条目;而万一有 fill 挤在 drain 和引用计数之间,那次计数在 discard 自己的 `BEGIN IMMEDIATE` 里、且**不分状态**地数 pool 行,照样拒绝;挤在 discard 提交之后的,撞上第十五轮 `build_one()` 那道重读,`-ESTALE` | `fa5a8c6` |

**验收**:`ctest`(WFS_FSKIT=OFF)**2/2**;`safety.sh` **248 passed, 0 failed**(新增 5 条);
`check-deps.sh` 全绿。(`m1_criteria.sh` 本轮跳过。)

先把测试跑红过:

- **归还(P2)**:新 seam `wfs_test_in_pool_unwind`——unwind 里、树已经改回 pool 名字之后、条目
  回去之前。让 hand-out 失败的办法是 `wfs_test_after_pool_claim` 里在 `--to` 上 `mkdir` 一个目录
  (P7:`RENAME_EXCL` 于是 EEXIST,谁也不许被盖掉),窗口里跑一整趟 `discard S<n>`(不带
  `--force`、不带 `--now`)。**老代码**:discard 回 **0**,快照落到 `WFS_ST_TRASHED`,
  `wfs_pool_ready()` 却还报**这个快照有 1 个 READY 条目**——正是 Codex 说的那棵无人认领的陈旧克隆。
  **新代码**:`WFS_E_SNAPSHOT_IN_USE`,快照还是 ACTIVE、树还在原处。
- **drain(P2)**:一个只有一个 pool 条目的快照,给条目的树挂上 `deny delete,delete_child,add_file`
  (和第五、第八、第十二轮同一个 ACL,`chmod`/`chflags` 都解不掉)。**老代码**:
  `discard S1 --force` **退出 0**,快照行变 TRASHED、pool 表被清空,而克隆还原样待在
  `<store>/pool/S1/<uuid>`,谁都不认识它(`gc --status` 随后把它算成一个 stale pre-clone entry
  ——store 自己报出来的损失)。**新代码**:退出 3,打出"a pre-cloned pool entry under
  .../pool/S1 could not be removed: Operation not permitted",快照还是 ACTIVE、行还在、
  `gc --status` 无话可说。

新增测试:

- `core_test`(归还,接第一轮那个 fork/discard 竞态):失败的那次 pool fork 之后,窗口里的
  discard 是 `WFS_E_SNAPSHOT_IN_USE`,快照 ACTIVE 且树在,`pool ready` 回到 1、
  `<store>/pool/S<n>` 下正好一个目录;而且它就是一个普通条目——不带 `--force` 的 `discard` 仍以
  `WFS_E_SNAPSHOT_IN_USE` 拒绝(那条失败的 fork 没留下任何东西在替它拒绝),带 `--force --now`
  则干净地把 pool 清空、快照删掉。
- `safety.sh`(drain,5 条):ACL 挂着时 `--force` 失败、快照 ACTIVE、pool 行还在、树没动、
  refusal 里有目录和 errno、`gc --status` 不报任何异常;ACL 拿掉之后**同一条命令**把 pool drain
  干净、快照进 trash,`<store>/pool` 下什么都不剩。

#### PR #1 review 第十五轮:认领是一次事务,不是事后的一条 WHERE(2026-09-20)

第十五轮,Codex 两条:一条 P1、一条 P2。两条是同一句话的两面:**一个"我可以动它"的判断,
必须和它授权的那个动作在同一个 `BEGIN IMMEDIATE` 里。** 第八轮到第十二轮把 collector 对**行**的
每一次写都变成了条件写,可这一轮的两处,不可逆的都不是那次写——P1 是一次 `rename(2)`(做完了
条件写才发现"这一条不归我了",而树已经改名了),P2 是一棵**克隆**(行插进去的时候快照已经在
去 trash 的路上了)。**一条一个提交、一个测试,先验证过"没有修复就会红"。**

| # | 位置 | 问题 | 修法 | 提交 |
|---|---|---|---|---|
| P1 `PRRT_kwDOUf7jGc6kCjHC` | `world.cpp` `gc_delete_one()`(以及 `--now` 的 `trash_delete_now()`) | 删一条 trash 用三步:`trash_row_still_ours()` 读行(TRASHED、还叫这个名字)→ `trash_mark_deleting()` 把树 rename 成 `<path>.deleting` → 条件 UPDATE 记下新名字 → unlink。`restore W<n>` 只要把它的 (a)(`BEGIN IMMEDIATE`:行 → TRASHING)插在**头两步之间**,树那一刻**还在 trash 里**:collector 的 rename 照样成功,它后面那条 `WHERE ... state=2 AND trash_path=?` 一行都没改到,**unlink 却照常往下走**。restore 随后的 rename 失败(树已经叫 `.deleting`),按既有逻辑回落 TRASHED——于是一条 TRASHED 行,还在邀请你 `restore`,而树已经被删光了。`--now` 里是同一个窗口:它的 rename 也在事务外,`-ESTALE` 是**树已经被改名之后**才报出来的 | **那次 rename 就是认领,所以它进事务**:`gc_claim_deleting()` 拿 store 锁 + `BEGIN IMMEDIATE`,在里面重读行(`state=2 AND trash_path=`排队时那个名字)、rename、记下新名字、提交。restore 的 (a) 也是 `BEGIN IMMEDIATE`,两者因此串行:要么 restore 在前——认领读到 TRASHING,**什么都不做**(树一根指头没碰);要么认领在前——restore 读到行写着 `.deleting`,回 `WFS_E_TRASH_DELETING`,树完好。事务里只握着一次 `rename(2)`(微秒级),那个会花上几分钟的"把上一次没删完的 `.deleting` 折进来"拆成了 `trash_fold_leftover()`、在锁外先做。rename 和提交之间崩掉 = 行记老名字、树在新名字,正是 `trash_follow_deleting()` 一直在解的状态。便宜的那次 `trash_row_still_ours()` 留着当快速跳过(不拿写锁),但**它不再作数**。无行的孤儿走同一个事务里的 `trash_path_claimed_locked()`——discard 是先提交 TRASHING 行、后搬树的,所以同一把写锁同样管得住。`--now` 照此改:先重读行,再 rename,`-ESTALE` 现在是一个**什么都没做**的 `-ESTALE` | `2c2bb14` |
| P2 `PRRT_kwDOUf7jGc6kCjHH` | `pool.cpp` `build_one()` 插 CREATING 行那一段 | `wfs_pool_fill` 在顶上 `snap_info()` 读一次快照行,`build_one()` 随后在自己的 `BEGIN IMMEDIATE` 里插 pool 行,**再也没问过快照**。夹在中间的 `discard S<n>`(不带 `--force`)数引用时,这一行还不存在——数出来 0 个 pool 条目、0 个 World——于是提交 TRASHING。它的树那一刻**还没搬走**(discard 是行先写、树后搬),filler 的 clonefile 照样成功,条目发布成 READY:一个**已经在去 trash 路上的快照**,有了个可以被领走的 pool 条目。`pool_collect()` 下一轮会埋掉它,可中间任何一次 fork 都会把它当活基线领走 | 插入的那个事务里**重读快照行**,要求 `state == ACTIVE` 且 `created_at` 正是条目要带的那个(`snapshot_id + snap_created_at` 就是 fork 认条目的那把钥匙);对不上就 `-ESTALE`。两边都是 `BEGIN IMMEDIATE`,于是串行:要么 discard 在前、这次插入看见快照不再 ACTIVE;要么插入在前、discard 数得到这一行(不带 `--force` 是拒绝,带 `--force` 是 drain)。`-ESTALE` 沿 `wfs_pool_fill` 的循环原样返回——CLI 打的还是那句"S<n> is not an active snapshot",和一开始就冲着一个已经进 trash 的快照填是同一句话。`pool_collect()` 那一头本来就会埋掉非 ACTIVE 快照的条目,这一条把窗口从另一头关上 | `b9fda0b` |

**验收**:`ctest`(WFS_FSKIT=OFF)**2/2**;`safety.sh` **243 passed, 0 failed**;`check-deps.sh` 全绿。
(`m1_criteria.sh` 本轮跳过。)

先把测试跑红过:

- **认领(P1)**:`wfs_test_before_trash_delete` 这个 seam 往后挪了一格——从"重读行之前"挪到
  **"重读行之后、认领之前"**,也就是这条 bug 真正的窗口里(原来那一格里 restore 已经把树搬回家了,
  collector 的 rename 回 `-ENOENT`,本来就不会出事)。窗口里跑一个**跑到一半的 restore**
  (`crash_at(2)`:行提交成 TRASHING,rename 没做,主人 pid 是个死的)。老代码:collector 把树
  rename 成 `.deleting` 并**删光**,`exists(g_half_path)` 红;恢复之后那个 World 再也回不来。
  新代码:认领读到 TRASHING,什么都不做。`--now` 那一半同样跑过红——把 `trash_delete_now()` 里
  的 rename 临时挪回事务外,`exists(g_nowhalf_path)` 就红。
- **填充(P2)**:新 seam `wfs_test_before_pool_insert`(读完快照行、插行之前)里跑一个
  **停在 phase 0 的 `discard S<n>`**(行已提交 TRASHING,树还没搬——这是唯一会造成损失的那个形状)。
  老代码:`wfs_pool_fill` 回 **0**,pool 里多出一个 READY 条目,而快照是 TRASHING。
  新代码:回 `-ESTALE`,`pool ready` 0,`<store>/pool/S<n>` 是空的。

新增测试:

- `core_test`(P1,接第八轮那个 collector 竞态):跑到一半的 restore 之后,树还在 trash 里、
  `.deleting` 这个名字不存在、内容还是 `one`,`worlds_deleted`/`trash_orphans`/`trash_failed`
  全是 0,行是 TRASHING;再 open 一次 store 让恢复收尾(树没动 → 回 TRASHED),`restore` 成功、
  World 回家、内容完好。**镜像顺序**也补了一条:在 restore 自己的窗口(phase 2)里跑一整趟 `wfs_gc`,
  它必须什么都不动,restore 随后正常收尾。
- `core_test`(P1,接第九轮那个 `--now` 竞态):同样那个跑到一半的 restore,`--now` 回 `-ESTALE`
  且**什么都没做**——树还在、没有 `.deleting`、内容完好,恢复之后 `restore` 照样把 World 领回家。
- `core_test`(P2):填充窗口里那趟停在 phase 0 的 discard 之后,`pool ready` 0、pool 目录空;
  被打断的 discard 按老规矩恢复成 ACTIVE(树从没搬过),同一个快照再填一次,条目正常发布。

#### PR #1 review 第十四轮:同一个文件的第二种拼法,和那个要被拿走的名字(2026-09-20)

第十四轮,Codex 两条,都是 P2,都落在 `core/src/hardlinks.cpp`。两条凑在一起是同一句话的两面:
**清单里的名字必须和树里的名字一一对应**。一条是名字可以有第二种拼法(`./d/a`、`d//a` 和
`d/a` 是同一个文件),前面十一轮那道"名字不许重复"于是形同虚设;另一条是树里那个名字
**本来就要被拿走**(checkpoint 会把克隆里的 `.world` 标记删掉),而扫描把它当成了树的一部分。
**一条一个提交、一个测试,先验证过"没有修复就会红"。**

| # | 位置 | 问题 | 修法 | 提交 |
|---|---|---|---|---|
| P2 `PRRT_kwDOUf7jGc6kCZpO` | `hardlinks.cpp` `manifest_path_sane()` | 第十一轮只拒了开头的 `/` 和 `..` 分量,于是同一个文件还剩两种拼法:`./d/a` 和 `d//a`,对任何一次系统调用都是 `d/a`,对清单上的每一道检查却是三个不同的字符串。这正是**第十一轮那种"成员重复"换了个拼法**——`(d/a, ./d/a)` 是两个成员、互不相同、组的 `nlink` 声明的就是 2,连第十三轮问树的那一道也放行:两个名字 `lstat` 到同一个 inode、那个 inode 的 `st_nlink` 确实是 2,**因为它们本来就是一个名字**。重放于是发现第二个成员已经在正身那个 inode 上,记成"已链好"就收工,真正的 `d/b` 在 fork 出来的 World 里还是一个独立文件——快照宣称的组,它自己的树里没有 | **每一个分量都必须是一个名字**:不许为空(这一条顺带管了 `a//b`、结尾的 `/`,和本来就拒掉的开头 `/`),也不许是 `.` 或 `..`。写者写不出别的东西来——成员就是 `fs_walk_tree` 的 `rel`,由 readdir 出来的名字拼接而成,而 `readdir(3)`(显式跳过点目录项)和 `getattrlistbulk(2)`(从来不返回它们)都不会给出 `.`、`..` 或空名字,所以**这个库写过的任何一份清单都不会因此读不回来**。`hardlinks_verify_groups()` 里不再加第二道机制:有了这条,一组里的两个成员不可能再是同一个文件的两种拼法,`lstat` 之后再做一次规范化比对已经无事可做 | `b15c7fc` |
| P2 `PRRT_kwDOUf7jGc6kCZpT` | `hardlinks.cpp` 扫描 + 重放里那个"成员不在"的分支 | checkpoint 克隆完活 World 之后会把克隆里的 `.world` 标记 `unlink` 掉——源 World 的身份不是这个快照的身份。可收集硬链接组的那一趟扫描是**连标记一起**走的:标记被人硬链接过(备份副本、内容寻址的存储、隔壁树的 `cp -al`;在工作区里再平常不过,World 的身份校验也不在乎),它就和它的孪生名字进了同一个组。重放随后发现标记在克隆里没了,记了一笔 `missing`,却**没有把 `whole` 置否**,于是这个组留在清单里、留在 `hl_groups` 上。快照发布了,而从那一刻起每一次 `verify`、每一次 fork 都会去 `lstat` 一个树里根本没有的成员:`WFS_E_SNAPSHOT_DIRTY`,永远,发生在一个从来没有损坏过的快照上 | **两半都补,因为它们管的不是一回事。**(1)`hardlinks_scan()` 多收一个"调用方待会儿要拿走的那个相对名",`wfs_snapshot_create` 传 `WFS_MARKER_NAME`——和它 `unlink` 的**正好是同一个名字**,所以子 World 里的 `.world`(不会被删)原样保留。标记于是既不进分组、也不计进 `hardlinks`;留下的那个孪生名字所在的 inode,链接数就比树内找到的名字多——那正是**外部组**,计数而不认领,和这个库一贯的做法一致。(2)重放时**不在克隆里的成员一律把组判为 broken**,`wfs_snapshot_create` 把它从清单和 `hl_groups` 上一起去掉(第五轮那条规矩:快照只说自己那棵树有的东西)。这一半管的是别的——快照创建从克隆里拿走的任何东西,以及活源树在"扫描"和"克隆"之间消失的成员(源端那道核对看不见它,因为去问源树时它还在)。fork 和 pool 填充从不读 `broken`,它们的克隆照发,所以除快照创建外没有任何行为变化 | `4e8f726` |

**验收**:`ctest`(WFS_FSKIT=OFF)**2/2**;`safety.sh` **243 passed, 0 failed**;`check-deps.sh` 全绿。
(`m1_criteria.sh` 本轮跳过。)

先把测试跑红过:

- **拼法(P2)**:一个 `(d/a, d/b)` 的快照,把清单里 `d/b` 那一行的路径分别换成 `./d/a`、
  `d//a`、`d/a/`(头、组号、nlink、行数一个都没动)。老代码:`./d/a` 和 `d//a` 两种拼法
  `verify` 都回 **0**、`wfs_world_create` 都回 **0**,发布出来的 World 里 `d/a` 和 `d/b` 是
  两个 inode、`nlink` 各为 1——快照宣称的那个组不在树里。新代码三种拼法都是
  `WFS_E_SNAPSHOT_DIRTY`。(`d/a/` 本来就被第十三轮那道问树的检查挡住了:对一个普通文件用
  结尾带斜杠的路径 `lstat`,回的是 ENOTDIR。)
- **标记(P2)**:一个把 `.world` 硬链接到普通名字 `m` 的 World,外加一对普通硬链接做对照。
  老代码:checkpoint **发布成功**,`hl_groups=2`、`hl_external=0`、`hardlinks=4`,紧接着
  `wfs_snapshot_verify` 和 `wfs_world_create` **都回 `-1008`**(`WFS_E_SNAPSHOT_DIRTY`)。
  新代码:`hl_groups=1`、`hl_external=1`、`hardlinks=3`,`verify` 干净,fork 成功。

新增测试:

- `core_test`(拼法):三种拼法各自要让 `verify`、`wfs_world_create`、`wfs_pool_fill` 三条路
  都回 `WFS_E_SNAPSHOT_DIRTY`,`--to` 上什么都没有、克隆没留下、世界表没多行、pool 还是空的;
  把清单换回原样,同一个快照照旧 fork 成功、那一对重新链上、内容还是 `aaaa`。
- `core_test`(标记):checkpoint 之后查 `hl_groups` / `hl_external` / `hardlinks` 三个数
  (这三个数把"扫描那一半"钉死),`verify` 干净;掀开 gate 看快照树本身——标记没了、`m` 是
  自己一个 inode、对照那一对是一个 inode 两个名字;再 fork(直接克隆和 pool 条目两条路都走),
  那一对重新链上,`m` 是 `nlink` 1 的普通文件,和 fork 自己那个新写的 `.world` 不是一个 inode。

#### PR #1 review 第十三轮:看不了不是空的,数对了不是真的(2026-09-20)

第十三轮,Codex 两条:一条 P1、一条 P2。两条各自是前面两轮的延伸,而且都是同一种错觉——
**把"我没查出问题"当成了"没有问题"**。P1 是第十二轮那条 `exists()` 规则的最后一个漏网之处,
只不过这次的系统调用是 `opendir(2)`:读不了的目录被当成了空目录。P2 是第八/九/十一轮那三道清单
检查的尽头:那些检查全是**数数**,而清单自己数得对并不等于它描述的是那棵树——查清单查不出清单
自己的错,得去问那棵不可变的树。
**一条一个提交、一个测试,先验证过"没有修复就会红"。**

| # | 位置 | 问题 | 修法 | 提交 |
|---|---|---|---|---|
| P1 `PRRT_kwDOUf7jGc6kCO5d` | `store.cpp` `wfs_store_open()` 里的 P17 守卫 | `metadata.db` 不在时,守卫靠对 `snapshots/`、`trash/`、`pool/` 各做一次 `opendir(2)` 来判断这个 store 是不是真的空的,而失败被静静跳过(`if (!d) continue;`)。EACCES、EIO、卷没挂上,于是统统读成"里面没东西":open 就在那些树旁边**新建了一个数据库和一个新的 store id**。这比没有守卫还糟——从那一刻起库**在**了,以后每一次 open 都走"库可读"那条路,守卫**再也不会跑第二遍**,id 从 1 重新发,下一次 `init` 写到已经在磁盘上的 `snapshots/S1` 上 | 第十二轮那条规则,这次落在 `opendir(2)` 上:只有 ENOENT 算"这个子目录不在,所以里面不可能有东西",别的 errno 都不是答案。`store_has_trees()` 从 bool 改成 `1 / 0 / -errno`(`readdir(3)` 自己那个 errno 也算进去——它只能这样报错),`wfs_store_open` 拿到负值就在**创建任何东西之前**原样返回。`metadata.db` 自己那次 `stat(2)` 同理:不是 ENOENT 的失败一律挡回去,而不是判成"文件不在"。文件本身从来不会被顶掉——`SQLITE_OPEN_CREATE` 只建不存在的库,不会截断已有的,所以"在但打不开"照旧是 `WFS_E_STORE_DAMAGED`。(ABI 里没有第二个建 store 的入口要审:`wfs_store_open` 就是唯一一个,守卫在它最前面。)CLI 对"看不了"那几个 errno 把话说全:store 路径、errno,以及**什么都没建**,因为读不了的 store 不是空 store(P17) | `7164947` |
| P2 `PRRT_kwDOUf7jGc6kCO5g` | `hardlinks.cpp` 清单组的校验 | 到这一轮为止,清单上的检查全是**数数**:`#hl` 头的组数/名字数(第八轮)、每组成员数对上它的 nlink(第九轮)、名字不重复、路径不越界(第十一轮)。有一种损坏把这些全保住了——**两个组之间互换一个成员**:`(a,b)`、`(c,d)` 写成 `(a,c)`、`(b,d)`,还是两个组、四个名字、每组 nlink 2、名字不重复、路径都在树内。重放于是把 `c` 链到 `a`、`d` 链到 `b`;而这四个文件是同一趟快照 walk 的克隆,size 和 mtime 一模一样,`restore_group()` 那道"它还是当初扫到的那个文件吗"的闸门也拦不住。fork 回 rc 0,World 里 `a`/`c` 焊成一个 inode、`c` 的内容彻底没了 | 清单查不出清单自己的错,因为出错的正是清单。**快照树才是不可变的原件**,所以拿它当权威:`hardlinks_verify_groups()` 在重放之前对每一组挨个 `lstat`——所有成员必须落在同一个 inode 上,且那个 inode 的 `st_nlink` **正好**等于组的成员数(正好而不是至少:快照发布时那个 inode 上只有这个组的这些名字)。第一个对不上的组就是 `-EINVAL` → `WFS_E_SNAPSHOT_DIRTY`。三个读清单的地方都做:从快照 fork、pool filler 的 `build_one()`、`wfs_snapshot_verify()`(运维就是从这里知道为什么)。前两处自己开 gate 窗口(gate 关着的根是 0000,底下什么名字都解析不了),失败就回滚、克隆不发布;`verify` 本来就在窗口里。**不能拿克隆去问**:clonefile 把每一条硬链接都断开了,克隆自己说不出哪些名字本来是一个 inode。从活 World fork 仍是第五轮那条(verify root = 那个 World,逐组核对、跳过并上报,而不是拒绝),活树本来就允许变。代价:每个硬链接名字一次 `lstat`——正是 `hardlinks.h` 一直给 verify 标的价;没有组的快照一次系统调用都不用加 | `92fcba4` |

**验收**:`ctest`(WFS_FSKIT=OFF)**2/2**;`safety.sh` **243 passed, 0 failed**;`check-deps.sh` 全绿。
(`m1_criteria.sh` 本轮跳过。)

先把测试跑红过:

- **P17(P1)**:一个有一个快照的 store,删掉 `metadata.db`,把 `snapshots/` `chmod 000`。老代码
  `wfs_store_open()` 回 **0**——一个崭新的数据库和一个崭新的 store id 就建在那棵快照树旁边;
  新代码回 `-EACCES`、`metadata.db` 没被建出来,权限还回去之后同一个 store 照旧报
  `WFS_E_STORE_DAMAGED`(这是对那条规则的收窄,不是撤退)。
- **组校验(P2)**:两对硬链接 `(a,b)`、`(c,d)` 的快照,四个文件 size 和 mtime 一样,清单 `hl`
  段的第 1、2 行互换路径(计数一个都没动)。老代码:`verify` 回 **0、modified=0**;fork 回
  **0**,World 里 `a`/`c` 一个 inode、`b`/`d` 一个 inode,四个文件读出来**全是 `aaaa`**——
  `cccc` 没了。新代码:`verify`、`wfs_world_create`、`wfs_pool_fill` 三个都回
  `WFS_E_SNAPSHOT_DIRTY`,`--to` 上什么都没有、克隆没留下、pool 还是空的。

新增测试:

- `core_test`(P17,接在原来那段 P17 测试后面):上面那个 `chmod 000` 的 `snapshots/`,
  `-EACCES` + 没有新 `metadata.db`,权限还回去之后 `WFS_E_STORE_DAMAGED` 照旧。
- `core_test`(组校验):互换之后 `verify`/fork/`pool_fill` 三条路径的拒绝、`--to` 干净、
  世界表没多行、pool 没多条;把清单换回来之后,**同一个带 gate 的快照**照样 fork 成功、两对都
  重新链上、各是各的内容——直接克隆和 pool 条目两条路都验(那也是"校验跑在 gate 窗口里"的对照)。

#### PR #1 review 第十二轮:问不出来不等于不在(2026-09-20)

第十二轮,Codex 四条:一条 P1、三条 P2。其中三条是**同一个缺陷**:`exists()`——`lstat(2)` /
`stat(2)` 上的一个 bool——被当成了"这棵树没了"的结论,可 EACCES(父目录权限)、EIO、卷没挂上、
ENAMETOOLONG 在它眼里统统等于"不在"。于是一次读不到,就换来一行 DEAD、一行被删、或者一棵
collector 从此再也不认识的树。**一个错误不是一次缺席。** 这一轮按第九轮 P18 那次的做法做了一遍
**全面审计**,并把判据收进一个地方:`wfs::fs_probe()` 保留 errno,`wfs::fs_gone()` 只认
ENOENT/ENOTDIR,`proven_gone()` 是"**能证明不在才算不在**"。
**一条一个提交、一个测试,先验证过"没有修复就会红"。**

| # | 位置 | 问题 | 修法 | 提交 |
|---|---|---|---|---|
| P1 `PRRT_kwDOUf7jGc6kB15x` | `world.cpp` `wfs_gc_ex()` 的 reconcile 扫描 | 判据是 `stat(2) == 0 && S_ISDIR`,别的一律当成"树没了"。`gc --reconcile` 于是把行标成 **DEAD**——而 DEAD 的 World `verify` 修不了、`adopt` 也认不回来:**一次瞬时错误就把一个活着的 World 永久注销**。父目录被 `chmod 000`、卷没挂上、一次 EIO,都够了。快照那一半同理 | 只有 ENOENT/ENOTDIR 算 dangling。其余一律计进新增的 `wfs_gc_report::{snapshots,worlds}_unreadable`(`wfs_store_stat` 同名字段跟上),**行原样留着**,由 `gc` / `gc --reconcile` / `status` 报出来。路径上放着一个**不是目录**的东西也归这一类:那是一个**损坏**的 World,不是一个不在的 World——埋了它就把"这里本该有什么"这条记录一起扔了,所以同样只报不埋。`wfs_store_status()` 用同一条规则,因为 `status` 那个数正是运维决定要不要跑 `--reconcile` 时看的数 | `4641ca7` |
| P2 `PRRT_kwDOUf7jGc6kB151` | `world.cpp` `wfs_snapshot_discard()` | "树已经没了"是 discard 可以走的捷径:行直接写 DEAD、不记 trash_path(没有东西要搬、要删)。这个判断也是一个 `exists()`,所以 EACCES / EIO 也走了捷径——而树还在 `S<n>`,**之后没有任何东西会再看它一眼**(reconcile 只扫 ACTIVE 行,后缀清扫只认 `*.wfs-tmp`):一整个快照,悄悄注销、永久留在磁盘上 | 只有 ENOENT/ENOTDIR 走捷径。别的 errno 原样返回给调用者——返回点在事务里面,`Txn` 析构回滚,**行一个字节都没动**——CLI 直接把原因打出来 | `65dbac3` |
| P2 `PRRT_kwDOUf7jGc6kB153` | `hardlinks.cpp` 清单读者 | 读者把行尾的 `\r` 连同 `\n` 一起剥掉,像是它自己刚写出来的这个文件可能是 CRLF 换行;而 `put_escaped()` 只转义反斜杠和 LF。于是名字末尾带 CR 的文件——对所有它跑的文件系统来说就是一个普通字节——原样写进去、读回来短一个字节:`a<CR>`/`b<CR>` 读成了 `a`/`b`,重放去动的是隔壁那两个真叫 `a`、`b` 的普通文件 | 写者转义 CR(`\\r`),读者只剥自己的终止符,`unescape()` 认回来。两份清单一起改:硬链接清单(`hardlinks.cpp`)和 `verify` 清单(写在 `platform_posix.cpp`、读在 `world.cpp`),它们本来就是同一对写者/读者。**老清单照样读得回来**——反斜杠一直写成两个、在看到它后面那个 `r` 之前就被成对吃掉,所以"反斜杠 + r"这个序列在老清单里不可能出现;而老写者写出来的、名字里带 CR 的清单本来就没有一种正确读法,那里没有兼容可言 | `a40fb45` |
| P2 `PRRT_kwDOUf7jGc6kB156` | `world.cpp` reconcile 之后删 `S<n>` 残株 | 行先标 DEAD,然后删 `<store>/snapshots/S<n>`,**结果丢掉**。删不掉(ACL、EPERM、一次没那么"瞬时"的 EIO)之后这棵树就永久泄漏:reconcile 只扫 ACTIVE 行、后缀清扫只认 `*.wfs-tmp`,而唯一还记得这个目录的那条行刚被埋了——不计数、不置 `work_remains`、`gc --status` 也看不见 | 第五、七、八轮那条规则的最后一处:**先删、确认没了才埋行**。删不掉就把行留在 ACTIVE(它本来就被报成 dangling,那正是它的状态),树计进 `tmp_failed`、按 `S<n>` 记进共享失败计数器(和 CREATING 那一半用同一个键——同一个目录,而两趟按状态天然不重叠)、在 `kGcFailCap` 以内置 `work_remains`。deadline 中途停下不算失败:置 `work_remains`、不计数、行原样 | `84adc39` |

**`exists()` 审计**(本轮的收尾,和第九轮 P18 那张表同一个性质):凡是 false 会导致"毁东西"或者
"写下一条没法反悔的行"的调用,全部改判。`world.cpp`:`check_path()` 沿祖先找 `.world` 标记
(读不到就等于放行,P7 唯一那道"别 fork 进别人的 World"的闸门自己站下了)、`trash_mark_deleting()`
(它的 `-ENOENT` 在两个调用者那里都是"埋行")、`trashing_reclaim()` 的 `-ESTALE`、
`wfs_world_restore()` 的 `-ENOENT`、`trashing_recover()`(读不到 trash_path 就把一条 TRASHING
行"恢复"成 ACTIVE——树在 trash 里、家里空着,一条天生 dangling 的 ACTIVE 行,下一次
`gc --reconcile` 正好埋掉它)、`wfs_world_verify()` 的 `WFS_E_WORLD_MISSING`(运维就是看着这句话
去跑 `--reconcile` 的)、后缀清扫的"它删掉了"、`gc_tmp_is_removable()` 的标记检查,以及
collector 埋 CREATING fork 树 / 半成品快照的那两处、`gc --status` 的三个计数。`pool.cpp`:
`pool_claim()`(读不到的条目现在让 fork 失败并回滚那条 DELETE,而不是把行删掉、克隆留在磁盘上)、
孤儿清扫的"删掉了"、过期行的删除、`pool_stranded()`。(`9bcf7ce`)

`gc_tmp_is_removable()` 那处要多说一句:光回 false 只做对了一半——调用者收到 false 就把行标 DEAD
并清空 `tmp_path`,而那正是第五轮修好的"永久搁浅"。所以"**我判断不了**"现在是它自己的一个答案
(`*undecided`):行留在 CREATING、树计进 `tmp_failed`、下一次唤醒再问。

**不改的**:纯粹"它在不在,我好建"的检查——fork 抽临时名、快照的 `tmpdir`、`PATH_TARGET`、
`restore` 的 `-EEXIST`、recovery 往家里搬之前看家里有没有东西。这些判断错了,代价是紧接着那个
系统调用回 EEXIST,不是一个 World。

**验收**:`safety.sh` **243 passed, 0 failed**(240 → 243,第四条新增 3 例);
`ctest`(WFS_FSKIT=OFF)**2/2**;`check-deps.sh` 全绿。

先把测试跑红过:

- **reconcile(P1)**:一个父目录被 `chmod 000` 的 World(`stat` 回 EACCES)、一个树被换成普通
  文件的 World。老代码:`unreadable=0 dangling=2 reconciled=2`,两条行**都是 state 3(DEAD)**。
  新代码:`unreadable=2 dangling=0 reconciled=0`,两条行都还 ACTIVE;权限还回去之后第一条恢复
  常态,而树真的没了的那条照样被 reconcile 成 DEAD(这是对那条规则的收窄,不是撤退)。
- **discard(P2)**:`<store>/snapshots` `chmod 000`。老代码 `wfs_snapshot_discard()` 回 **0**、
  行进 state 3,而 `S<n>` 还在磁盘上;新代码回 `-EACCES`、行还是 ACTIVE、树还在,权限还回去之后
  discard 正常完成。
- **CR(P2)**:一棵树里一对硬链接 `a<CR>`/`b<CR>`,外加两个 size 和 mtime 都一样的普通文件
  `a`、`b`。老代码:`verify` 对一个完好的快照回 **-1008、modified=2**;fork 报 `hardlinks=1`
  且返回成功,而 CR 那一对出来是**两个独立 inode**(nlink 1),`a`/`b` 反倒被焊在一个 inode 上、
  `b` 的内容没了。新代码:CR 那一对链上了,`a`/`b` 还是两个文件各自的内容,`verify` 干净——
  走 pool filler 的重放(它**根本没有 verify root**)也一样。
- **残株(P2)**:一个记录路径被删掉、`S<n>` 里留着一棵被 ACL 锁住的子树的快照。老代码:行
  state 3、残株留在磁盘上、gc 一个字都不说,ACL 去掉之后也没人回来找它。新代码:行留在 ACTIVE、
  gc 打出 "could not be removed",ACL 去掉之后下一次 `gc --reconcile` 删掉残株并埋掉行。
- **审计**:用户目录里一棵被遗弃的 fork 树,`.world` 标记 `lstat` 不到(树 `chmod 000`)。老代码
  把这棵树**删了**——连那道"这是不是我们的"检查都没做成。

新增测试:

- `core_test`(reconcile):上面那两个 World + 快照那一半(`<store>/snapshots` `chmod 000`),
  报表和 `wfs_store_status()` 都要数进 `*_unreadable`、行都要还 ACTIVE;权限还回去、树真删掉之后
  照样 reconcile 成 DEAD。
- `core_test`(discard):`-EACCES`、行 ACTIVE、`S<n>` 还在;权限还回去之后 discard 正常。
- `core_test`(CR):快照的 `hl_groups`/`hardlinks`、`verify` 干净、直接 fork 与 pool fork 两条
  路径上 CR 那一对的 inode/nlink 和 `a`/`b` 的独立性与内容。
- `core_test`(审计):标记读不到的遗弃 fork 树不许被删、要计进 `tmp_failed`、行留在 CREATING
  且 `tmp_path` 原样;权限还回去之后下一次 gc 删树埋行。
- `safety.sh`(残株):行不许变 DEAD、残株还在、stderr 说得出原因;`chmod -N` 之后下一次
  `gc --reconcile` 收尾。

#### PR #1 review 第十一轮:写下来的事情要真的做到了才算数(2026-09-20)

第十一轮,Codex 三条,全是 P2,而且是同一句话的三个位置:**一件事做没做成,要以它真的做成了
为准,不能以"我发了那条指令"为准。** 迁移发了 `ALTER TABLE` 就盖上版本戳、清单读回来只数数不查
名字、清扫删完了树不看树还在不在——三处都是把"我试过了"当成了"它成了"。
**一条一个提交、一个测试,先验证过"没有修复就会红"。**

| # | 位置 | 问题 | 修法 | 提交 |
|---|---|---|---|---|
| P2 `PRRT_kwDOUf7jGc6kBfvv` | `store.cpp` `wfs_store_open()` | 增量迁移的每条 `ALTER TABLE` 结果都被丢掉,然后照样把 `PRAGMA user_version` 盖成当前修订号。这只有在"ALTER 唯一可能的失败是 duplicate column name"时才成立,而它不是:EIO、磁盘满、熬过 10 s busy timeout 的 SQLITE_BUSY、SQLite 只能只读打开的库,失败的都是同一个调用。于是 store 被盖成"已迁移"而列是缺的——**而版本戳正是决定要不要跑迁移的唯一依据**,所以之后每一次打开都不会再试,凡是提到那个列的 prepare 从此永远失败,store 里没有任何东西说得出为什么 | 整个迁移放进**一个事务**,而且**结论来自 schema 而不是来自语句的返回值**:`kMigrations` 每条带上它要加的表名和列名;`PRAGMA table_info` 说列已经在了就**跳过**(列已经在不是"要容忍的失败",是"这一步没事可做",因此全程不匹配任何错误字符串);真正跑的每一步都查返回值;最后再拿 `table_info` 问一遍**每一个列都在**,才写版本戳。任何一步不对就整体回滚(建表 DDL 一起回滚,`PRAGMA user_version` 是库头的一次写,也跟着事务回滚),`wfs_store_open` 返回 `-EIO`,store 原封不动、**没有盖戳**,所以下一次打开就是重试。`Txn` 加 `begin_rc` / `commit_rc()` 给这唯一一个必须知道结果的调用者用 | `a321da3` |
| P2 `PRRT_kwDOUf7jGc6kBfvy` | `hardlinks.cpp` `hardlinks_manifest_read()` | 第八、第九轮加的清单校验全是**计数**:`#hl` 头里的组数/名字数,以及每组正好是它声明的 `nlink` 那么大。有一种损坏把这些全保住了——**某个成员重复出现**:重复在自己组里,或者顶掉邻组的一个成员。重放拿这个重复的名字没辙(它本来就是正身那个 inode,于是被记成"已经链好了"),而被它顶掉的那个成员根本不在清单里了,于是 fork 回 0、快照里是一个 inode 两个名字的地方,克隆出来是两个独立 inode。跨组更糟:被塞进来的名字会拿**另一个组**的正身去链,而两边的 size/mtime 都来自同一次克隆,重放那道"这还是扫描时看到的那个文件吗"的闸门放行,**克隆里的内容被覆盖** | 名字是一条 dirent,扫描只会记一次,所以重复**按构造就是损坏**。读回来之后把所有成员路径收进一个 `Vec`、排序、相邻相等就 `-EINVAL`(到调用者是 `WFS_E_SNAPSHOT_DIRTY`)——一次 qsort,而且绝大多数树里这个集合是空的。顺带按要求审了路径本身,又揪出一个洞:**成员路径从来没有被校验过是不是树内相对路径**。重放是拿这些名字对着克隆的根解析的,所以 `../escape` 就是用户自己目录里、fork 临时名旁边的一个文件,`link(2)` + `rename(2)` 会把组的正身 inode 盖上去。空路径、开头的 `/`、任何 `..` 分量现在一并拒绝 | `779ed84` |
| P2 `PRRT_kwDOUf7jGc6kBfv0` | `world.cpp` `rm_tmp_in_store_dir` | 清扫把 `fs_remove_tree` 的结果丢了。`<store>/snapshots` 下没有任何行认领的 `S<n>.wfs-tmp` 要是删不掉(ACL、EPERM、一次没那么"瞬时"的 EIO),它不进报表、不置 `work_remains`,而 `wfs_gc_pending()` 只给 trash 分类,于是**没有任何人会回来找它**:树一直留在 store 里,直到有人手动跑 gc。collector 里别的删不掉的东西从第五、第七、第八轮起都会说话,就剩这一处不说 | 试完之后树还在,就计进 `rep.tmp_failed`、给共享失败计数器加一、在 `kGcFailCap` 以内置 `work_remains`——和 trash 条目、半成品 fork 树、过期 pool 条目同一个上限、同一条"报出来,但永远不报成已经没了"的规则。它**没有行**可以做 key(没人认领正是它归清扫管的原因),所以按**路径**记,跟 `pool.cpp` 给自己那些无行树的做法一样。deadline 中途停下**不算失败**、不计数(下次唤醒它照样是棵无行的树);树没了就清掉计数器。`wfs_gc_status` 也数它们,就数在第七轮给半成品快照的那条 `abandoned:` 里:一次 readdir,由 `snap_tmp_named_by_row()` 判定,所以和上面那趟 CREATING 循环**按构造不重叠**。交互式 `gc` 本来就看 `rep.work_remains` 决定要不要起 worker(第六轮),链条原样接上;那条提示改成"任何认领它的行都会留着"——原来的"它的记录会留着"对这一种树从来不成立 | `8f25a2f` |

**验收**:`safety.sh` **240 passed, 0 failed**(236 → 240,第三条新增 4 例);
`ctest`(WFS_FSKIT=OFF)**2/2**;`check-deps.sh` 全绿。

先把测试跑红过:

- **迁移**:按 schema 2 最初的样子造一个库(基础表,后来加的 13 个列一个都没有),再把
  `snapshots` 换成一个 view(`CREATE TABLE IF NOT EXISTS` 于是静静地什么都不做,第一条 ALTER
  回 `Cannot add a column to a view`,而它后面那条 `PRAGMA` 成功)——这正是 EIO / 丢掉的
  SQLITE_BUSY 的形状,也是唯一一种测试能随时造出来的。老代码:`wfs_store_open()` 返回 **0**,
  `user_version` 从 **200 变成 204**,`snapshots` 的 8 个列一个都没加上,而碰巧能跑的 5 个
  自己加上了。
- **清单**:两个三成员组,六个文件同一个 size 同一个 mtime(免得重放那道闸门误打误撞救了场),
  三份损坏的清单 —— `a3` 换成第二个 `a1`、`b1` 换成 `a1`(a1 同时在两个组里)、某个成员换成
  `../escape` 且隔壁真有这么个文件。老代码三份**全是** verify 0、fork 0、pool fill 0:
  (a) `a1` 停在 nlink 2、`a3` 是个独立 inode;(b) `b2`/`b3` 被链到 `a1` 上——`a1` nlink 5,
  fork 里再也没有 `bbb`;(c) `a1` 被 rename 到隔壁那个文件上,它的内容读出来是 `aaa`。
- **清扫**:一个没有任何行的 `S999998.wfs-tmp`,里面一棵被 ACL 锁住的子树。老代码 4 例里红 3 例
  ——gc 什么都不说、`work_remains` 不置、`gc --status` 什么都不数,树还在;第 4 例是对照
  (ACL 去掉之后下一次 gc 删掉它并停止计数),前后都绿。

新增测试:

- `core_test`(迁移):(1) 老库打开就迁移、迁移完才盖戳,再打开一次什么都不变,`wfs_store_status`
  正常;(2) 上面那份"有一条 ALTER 必然失败"的库,`wfs_store_open` 必须回 `-EIO`、`user_version`
  停在 200、**13 个列一个都不能加上**(部分生效也是生效);(3) 把 view 换回真表,下一次打开
  照常迁移到底——这正是 (2) 不盖戳的意义。
- `core_test`(清单):上面三份损坏各自要 verify / fork / pool fill 三处全部 `WFS_E_SNAPSHOT_DIRTY`,
  `--to` 上不留东西、不留 `.wfs-fork-` 克隆、World 数不变,隔壁那个文件 inode、nlink、内容
  一概不动;把清单放回去,同一个快照照样 fork,两个组都重建、谁也没拿到对方的内容。
- `safety.sh`(清扫):`gc` 在 stderr 上报它、说"the collector will try again"、`gc --status`
  数到 1 棵;`chmod -N` 之后下一次 `gc` 删掉它并且不再数。

#### PR #1 review 第十轮:后缀清扫也得先问过行(2026-09-20)

第十轮,Codex 一条,P1,而且正是 **P18**(docs/M1_DESIGN.md §3)在 collector 里剩下的最后一处
缺口:**按名字模式下的判断也是一个关于活行的判断**。上一轮把"没有任何行认领这棵树"这句话在
trash、pool、reconcile 三处都改成了"在 store 锁下拿活行再问一遍",却漏了最古老也最粗的那一处
——`<store>/snapshots` 下的 `*.wfs-tmp` 后缀清扫,它一行都不问,见一个删一个。
**一条一个提交、一个测试,先验证过"没有修复就会红"。**

| # | 位置 | 问题 | 修法 | 提交 |
|---|---|---|---|---|
| P1 `PRRT_kwDOUf7jGc6kBXLy` | `world.cpp:2409` `rm_tmp_in_store_dir` | `wfs_gc_ex` 的 CREATING 那一趟对半成品快照很小心:**生产者还活着的行一律跳过**,因为 `S<n>.wfs-tmp` 这时候是一棵正在写的克隆。紧接着跑的后缀清扫却什么都不问,把那一趟刚刚放过的树当场删掉——`wfs_snapshot_create()` 正在往 `<tmp>/root` 里 `clonefile`,父目录没了,于是整个创建在克隆中途拿到 `-ENOENT` | 每个条目的 `S<n>` 解析回行 id,在 **store 锁下**、**紧挨着删除之前**问一遍 snapshots 表:**任何不是 DEAD 的行都算认领这棵树**。生产者活着的 CREATING 是正在建的克隆;生产者死了的 CREATING 归上面那一趟——它删不掉时会**故意**留着行按失败上限重试,清扫插进来只会把树删了、行留着变成谁也数不着的垃圾;ACTIVE/TRASHING/TRASHED 说明旁边的 `S<n>` 是真快照,这种 `.wfs-tmp` 本就不该存在,真有也宁可留一个空目录(下次用到这个 id 时 `wfs_snapshot_create()` 自己会清)。行读不出来同样算"别删"——清扫是 collector 里最靠后也最没信息的一趟,它的猜测不许压过行。另外这个清扫从此**只肯扫 `<store>/snapshots`**:它带的认领规则就是那张表,换个目录就不成立,新的调用点必须自己带规则(`pool.cpp` 那个清扫本来就是这么做的,上一轮已核) | `eb4b036` |

**其余按后缀扫的地方**:`pool.cpp` 的 `pool_sweep_orphans` 上一轮已经改成重查活行
(`path`、`path + .wfs-tmp`、fork 的 CREATING 行的 `tmp_path` 都算认领),本轮复核无误;
用户目录里一处都没有(第七轮的 `.wfs-tmp` 家族审计的结论未变)。

**验收**:`safety.sh` **236 passed, 0 failed**(本轮不变——这条竞态只有库内的 seam 能驱动,
CLI 层没有对应的检查点);`ctest`(WFS_FSKIT=OFF)**2/2**;`check-deps.sh` 全绿。

先把测试跑红过:把重查去掉 → `wfs_snapshot_create()` 当场返回 **-2(`-ENOENT`)**,
报表 `tmp_removed=1`,半棵克隆被删走(创建自己会回滚,所以发布不出半成品,但它**失败了**)。

新增测试(复用既有 seam,测试之外恒为 NULL):

- `wfs_test_before_snapshot_clone`:这个 seam 的位置正好——`S<n>.wfs-tmp` 已经 `mkdir` 好、行是
  CREATING 且生产者就是本进程、克隆还没开始。在里面拿**第二个 store 句柄**跑一整个
  `wfs_gc_ex`(保留期 0、不设 deadline,清扫最凶的那一档)。创建必须回 0,`verify` 干净,
  `fork` 出来的 `a.txt`/`sub/b.txt` 内容对得上,collector 那个句柄读到的也是 ACTIVE,
  报表 `tmp_removed=0`、`tmp_failed=0`、`snapshots_deleted=0`。
- 三个对照,把清扫剩下的行为钉在原地:没有任何行认领的 `S999999.wfs-tmp` 照样立刻删;
  根本不是 `S<n>` 的 `stray.wfs-tmp` 照样删;而一条 **ACTIVE** 行的 id 所对应的 `.wfs-tmp`
  留着不动。

#### PR #1 review 第九轮:collector 手里的那份快照已经旧了(2026-09-20)

第九轮,Codex 六条(四条 P1、两条 P2)。四条 P1 是同一条规则的最后四个缺口,这一轮把它写成
**P18**(docs/M1_DESIGN.md §3):**collector 只拥有它扫描那一刻看到的东西**——它对行的每一次写
都以"扫描时看到的状态和路径"为条件并查 `sqlite3_changes()`,它每一个"没有任何行认领这棵树"的
孤儿判断都要在**排队之前**和**真正开删之前**,在 store 锁下拿活行再问一遍。第五到第八轮修的是这
条规则的一个个实例;这一轮补齐剩下的,并且**按这条规则把 `wfs_gc_ex`、`pool_collect` /
`pool_sweep_orphans`、`trash_delete_now`、`trashing_reclaim` / `trashing_recover`、reconcile 和
`wfs_gc_status` 通审了一遍**,另外揪出四处(见下)。两条 P2 各自是老朋友的下一层:**读回来的东西
必须自洽**(这次是硬链接组的大小),以及**同一条规则要在每一条路径上成立**(这次是 xattr 名字太
多时那条 fallback)。
**一条一个提交、一条一个测试,六个测试都先验证过"没有修复就会红"。**

| # | 位置 | 问题 | 修法 | 提交 |
|---|---|---|---|---|
| P1 `PRRT_kwDOUf7jGc6kA-m3` | `world.cpp:2575` | `trash_scan()` 先读"哪些行认领了 trash 路径"(`claim_trash_paths()`),**放掉 store 锁**,然后才 readdir `<store>/trash`。正好挤在中间提交 TRASHING 并把树 rename 进来的 `discard`,留下一个这份快照从没听说过的目录——而**无主孤儿是立刻删的**:保留期不算数、`restore` 不可能、"这是谁的基线"也不问,而且旁边那个 discard 还在跑,它的树就这么没了 | 两个判断都改成**问活行**:排队之前(`trash_scan`)、以及开删之前(`gc_delete_one` → `trash_row_still_ours`,它原来对孤儿一律答"是我的")都在 store 锁下重查一次。"认领"沿用 `claim_trash_paths` 的定义——一条行的 `trash_path` 同时认领这个名字和它加上 `.deleting` 的那个(collector 先改名、后记名)。`wfs_gc_status()` 和 `wfs_gc_pending()` 共用 `trash_scan`,所以报表数的就是 collector 真会动的东西 | `baaed52` |
| P1 `PRRT_kwDOUf7jGc6kA-m4` | `world.cpp:1459` | `trash_delete_now()`(`discard --now` 的那个 helper)跟着树走到它现在的名字、删掉,然后写 `state=DEAD, trash_path=''`——**一个条件都不带**。`restore W<n>` 挤在它 `lstat` 和改名之间时:树搬回家、行写成 ACTIVE,于是这边的改名回 `-ENOENT`(它读成"已经被别人删了"),最后那条无条件 UPDATE 把一个**刚刚活过来、就在家里**的世界写成 DEAD | 两条写都带上这次调用所依据的状态和路径(`state=2 AND trash_path=?`)并查 `sqlite3_changes()`,**改不到行就是 `-ESTALE`,绝不是悄悄的 0**。记 `.deleting` 那一步同样带条件,对不上就停在那里:树此刻顶着一个 `restore` 不会碰的名字,下一个 collector 会收完它,而不是由这次调用去删一棵行已经不认的树。两个常规入口天然成立——(c) 之后行就是 TRASHED 且 `trash_path=trash`,"已经在 trash 里"那条路径的 `trash_path` 是**现读的**。CLI 把这条新拒绝在 `discard W<n>` 和 `discard S<n>` 上都说清楚(并且**重读**状态,而不是报调用之前那一眼) | `02fbc01` |
| P1 `PRRT_kwDOUf7jGc6kA-m5` | `pool.cpp:435` | `pool_scan()` 取一份 pool 行快照,`pool_collect()` 照它删树、并把 `<store>/pool` 下它没提到的目录当无主目录扫掉。扫描**之后**才插 CREATING 行的 filler,`.wfs-tmp` 被当场删掉(半棵树还会接着被发布成 READY);扫描之后才**建好**的条目被整棵删走,留下一条说 READY、树却没了的行 | 每一个"看起来无主"的目录在**删之前**(以及只计数的那一趟——`gc --status` 得说 collector 真会动的东西)在 store 锁下重查一遍活行:pool 行的 `path`、`path + .wfs-tmp`、以及 fork 的 CREATING World 行的 `tmp_path` 都算认领。**`pool_claim()` 本身不需要宽限期**:删 pool 行和插 fork 的 CREATING 行是同一个 `BEGIN IMMEDIATE`,rename 在它提交之后,所以树一刻也不会无行可依——重查会看 `worlds.tmp_path` 正是为此 | `b8e18f3` |
| P1 `PRRT_kwDOUf7jGc6kA-m9` | `world.cpp:2989` | `gc --reconcile` 扫出"这条 ACTIVE 行记的路径上没有目录"就**无条件** UPDATE 成 DEAD。可这句话对**被删掉的**和**只是被搬走的** World 同样成立,直到有人跑 `world fs verify <新路径>` 按 inode 把行挪过去——那次 verify 要是正好挤在扫描和 UPDATE 之间,它的成果就被埋了:一个 ACTIVE、就在新路径上的世界,被一条读着没人用的老路径的 reconcile 判死 | 两条 UPDATE 都带上扫描时看到的 `state=1 AND path=?`,`worlds_reconciled` / `snapshots_reconciled` 只数**真的被这条 UPDATE 改到**的行;快照那边,行没改到就连 `<store>/snapshots/S<n>` 也不动。`*_dangling` 不变——它报的是扫描看见了什么 | `7ec714b` |
| P2 `PRRT_kwDOUf7jGc6kA-nB` | `hardlinks.cpp:244` | 第八轮的清单自洽检查只要求"每组至少两个名字"。这挡得住**写到一半**的清单,挡不住**把一个成员改挂到邻组**:组数和名字数都不变,`#hl` 头依然自洽,而重放会把属于甲 inode 的名字 `link` 到乙组的正身上——克隆出来的内容被覆盖,`verify` 和 `fork` 都还报成功 | 读回来时**每一组还必须正好是它声明的 `nlink` 那么大**。**格式不动、版本不升、老清单照读**:先看了写的那一半——`hardlinks_scan()` 只在"这个 inode 的每一条链接都在树内"(`k == nlink`)时才把组写进 `groups`,名字不全在树内的组只进头里的 external 计数、一行 `hl` 都不写,所以 `nlink` **本来就是**这一组声明的树内成员数,不需要给清单加字段 | `98cfdec` |
| P2 `PRRT_kwDOUf7jGc6kA-nE` | `diff.cpp:350` | 一侧保留下来的 xattr 名字超过 4096 字节的有界缓冲时,比较落到堆上那条 fallback——而它**完全不过滤**:`com.apple.provenance` 就此回到比较里(内核分别盖章的两个文件上它是单边的),默认 `diff` 于是对一个谁都没动过的文件报 `T`;名字过了之后读的值也是没过滤的那一份,所以两侧都有、值不同的被忽略名字同样算一处改动 | `xattr_equal_raw()` 接过 diff 的 flags,**就地**把被忽略的名字从两份清单里去掉再比,随后读的值就是剩下那些的——一趟、不分配,`--all-xattrs` 照旧什么都比。provenance 本身没法从测试里驱动(内核给本进程建的每个文件都盖同一个值,`setxattr`/`removexattr` 对它**静默无效**,已实测),所以库里留一个 seam `wfs_test_xattr_ignore`:它指名的那个普通 xattr 被过滤规则当成 provenance 一样对待 | `a91d447` |

**P18 通审顺手揪出来的四处**(并入第六条之后的同一批改动,规则同上):`gc_tmp_is_removable()`
和 CREATING 行的收尾 UPDATE 都加上 `tmp_path` 条件(生产者被判死之后才发布的 fork,它的树不归
gc 删);`trashing_recover()` 的收尾 UPDATE 加上 `trash_path` 条件(那个判定正是拿这条路径
`lstat` 出来的),改不到行就不计数;`pool_collect()` 在删整棵克隆**之前**在锁下重问一遍"这一行
还该死吗";`DELETE FROM pool` 带上 `path`。

**验收**:`safety.sh` **236 passed, 0 failed**(上一轮 225 → 本轮 +11);`ctest`(WFS_FSKIT=OFF)
**2/2**;`check-deps.sh` 全绿(没有新的系统调用、没有新的库)。

六条都先把测试跑红过:

- trash 孤儿:把两处重查去掉 → `gc` 报 `trash_orphans=1`,窗口里那个 discard 的树被当场删掉。
- `--now`:把收尾 UPDATE 换回无条件版 → `--now` **返回 0**,而那个刚被 restore 回来的世界的行
  已经不是 ACTIVE 了。
- pool:把重查去掉 → `pool_removed=1`、pool 目录空了,而 `wfs_pool_ready()` 还答 1。
- reconcile:把 `path` 条件去掉 → `worlds_reconciled=1`,那个刚被 verify 挪过去的世界是 DEAD。
- 清单:把 `nlink` 检查去掉 → `verify` 回 0、`fork` 回 0,而 fork 出来的 `b1`/`b2`/`b3` 里装的是
  `a1` 的内容、nlink 4(`aaa`,`bbb` 没了)。
- xattr fallback:把过滤去掉 → `many names / --full: 2 lines, wanted 0`。

新增测试(三个新 seam,测试之外恒为 NULL):

- `wfs_test_before_trash_orphans`:在"认领快照取完、readdir 之前"那一刻,拿第二个 store 句柄跑
  一整个 `discard`。gc 必须什么都不删、不数、也不报成失败,行停在 TRASHED、树完整、`restore`
  拿得回来;而一个**真的**无主目录照样立刻被删。
- `wfs_test_trash_crash` 的 **phase 4**:在 `--now` 的 `lstat` 和改名之间跑一整个
  `wfs_world_restore()`。`--now` 必须回 `-ESTALE`,世界 ACTIVE 且在家、内容完整、还 diff 得动;
  常规的两个 `--now` 入口和快照的 `--now` 一并复核。
- `wfs_test_before_pool_sweep`:在 `pool_scan` 和删除之间,拿第二个句柄跑一整个 pool 后备 fork
  **和**一整个 `pool fill`。两棵树都必须留下,fork 的世界 ACTIVE 且内容完整,新条目仍是 READY
  且真的能被下一次 fork 领走(`from_pool == 1`),`gc --status` 的 `pool_stranded` 是 0;真的无主
  目录照样被删。
- `wfs_test_before_reconcile`:在扫描和 UPDATE 之间跑 `wfs_world_verify_identity(<新路径>)`。世界
  必须留在 ACTIVE、`path` 是新路径、还 diff 得动;树真的没了的世界照样被 reconcile。
- `core_test`(清单):两个三成员组 + 一个**真的有树外链接**的组(`nlink 3`,树内两个名字),六个
  文件统一长度和 mtime(免得重放那道"还是扫描时那个文件吗"的 size+mtime 检查替损坏兜底),把第
  一组的最后一个成员改挂到第二组(2+4,总数不变)。`verify` / `fork` / `pool fill` 全是
  `WFS_E_SNAPSHOT_DIRTY`,目标路径下什么都没发布、也不留半成品克隆;换回原样后连那个外部组一起
  照常工作。
- `diff_test`:两个各带 200 个名字(4800 字节,超过有界缓冲)的文件,把 fallback 的两半都驱动一
  遍——一个名字只有 world 那边有,一个名字两边都有而值不同。默认一条都不报,`--all-xattrs` 两条
  都报;把 seam 关掉,那个名字就是普通 xattr,两条又都报(这是"确实是过滤在起作用"的对照)。
- `safety.sh`(+11):`.deleting` 名字下的树仍然是它那条行的条目(删掉、行落 DEAD、**不**计成孤
  儿),真的无主目录照样立刻删;被搬走又 `verify` 过的世界扛得住 `--reconcile`,树真没了的照样被
  reconcile;硬链接组改挂之后 `verify` 和 `fork` 都拒、`--to` 下什么都不留,换回原样 fork 出来
  `b1` 还是 `bbb`、nlink 3。

#### PR #1 review 第八轮:正在进行的操作不是崩溃现场(2026-09-19)

第八轮,Codex 四条(两条 P1、两条 P2)。两条 P1 是同一个主题的**第三个面**:前几轮管的是"谁在
写锁下**决定**",这一轮管的是"**决定之后、树搬完之前**,别人怎么看这条行"。`trashing_recover()`
只会 `lstat`,而 `lstat` 分不清"死在 rename 之前"和"还差一微秒就 rename";collector 也一样,
它排队时那条行是 TRASHED,轮到它时可能已经不是了。两条 P2 还是那对老朋友:**删不掉不等于删掉了**
(这次轮到 pool),以及**读回来的东西必须自洽**(这次轮到硬链接清单的 `#hl` 头)。
**一条一个提交、一条一个测试,四个测试都先验证过"没有修复就会红"。**

| # | 位置 | 问题 | 修法 | 提交 |
|---|---|---|---|---|
| P1 `PRRT_kwDOUf7jGc6kAj4-` | `store.cpp:328` / `world.cpp` | `trashing_recover()` 跑在**每一次 store open** 和每一次 gc 开头,把**每一条** TRASHING 行都当成崩溃现场。于是一个**活着的** `discard W<n>` / `discard S<n>` / `restore W<n>` 卡在 (a) 和 rename 之间时,隔壁进程(脚本里的下一条命令就够)看到"树还在家",把行判回 ACTIVE、清掉 `trash_path`;这个 discard 随后把树搬进 trash,它那条 `WHERE state=4` 的 (c) **一行都没改到却返回 0**——留下一条 ACTIVE 行和一棵**没有任何行提到**的树,而无主孤儿是**立刻**删的:保留期不算数、restore 不可能,快照的话那是所有 fork 自它的世界的基线。`restore` 是镜像(行被判回 TRASHED,树却已经在家) | **TRASHING 行带主人**:(a) 把 `owner_pid` + `owner_start` 写上(和 CREATING 行同一对列、同一个 `producer_alive()`,它比的是进程启动时刻,所以 pid 复用骗不过它),(c) 和 `trashing_undo()` 清掉。`trashing_recover()` **跳过主人还活着的行**——那是在飞的操作,不是崩溃。第二层:(c) 和 restore 的收尾 UPDATE 都看 `sqlite3_changes()`,**0 行不算成功**。谁把行从我们手里判掉了,树和行也绝不许就此矛盾:`trashing_reclaim()` 在"树在 trash、行却说 ACTIVE"时把行重新写成 TRASHED(那是唯一一种不许留下的组合),树已经不在我们放的地方就回 `-ESTALE`;restore 的收尾同时认 `state=4` 和 `state=2`(一个 TRASHED 却躺在家里的世界只可能是这次 restore 造出来的),行已经是 ACTIVE 且没有 `trash_path` 就是别人替我们干完了,回 0 | `b577c2b` |
| P1 `PRRT_kwDOUf7jGc6kAj5D` | `world.cpp:2532` | collector 排队了一条 TRASHED 行,轮到它时 `restore` 已经抢先把树搬回家。`trash_mark_deleting()` 的 `-ENOENT` 此刻的意思是"**搬回家了**",不是"已经被删了",而那个分支**无条件** `mark_dead()`:一个刚刚恢复、就在家里、行是 ACTIVE 的 World 被写成 DEAD——没有任何回头路,行是那个目录之所以是 World 的全部理由 | collector 对行的**每一次**写都带上这条行的全部身份:`WHERE id=? AND state=2 AND trash_path=?`(排队时那条行的样子),并查 `sqlite3_changes()`。重命名成 `.deleting` **之前**还在 store 锁下把行重读一次,所以常见情况下它连树都不碰就跳过:TRASHING 是别人在飞的操作、ACTIVE 是被 restore 拿回去了、DEAD 是别人收了。`gc_delete_one()` 对这种任务返回 **1**,循环**不计数、不报告、不重试**。`TrashJob` 同时记住行自己的 `trash_path` 和跟过 `.deleting` 的那个:collector 先改名再记名,中间那条记录若被条件写挡掉,两个名字**本来就都属于这条行**(`claim_trash_paths`),下一趟扫描照样跟着改名找到树 | `4fd1a54` |
| P2 `PRRT_kwDOUf7jGc6kAj5G` | `pool.cpp:470` | `pool_collect()` 对一条过期 pool 行调 `fs_remove_tree()`,**把结果扔掉**然后删行。删不掉(ACL、EPERM、瞬时 EIO)就等于:一整棵快照克隆留在磁盘上、行没了、`gc` 还报告"1 pool entries"收掉了。旁边那条无主目录规则下次会看到这棵树,但它**没有失败计数也不置 `work_remains`**,所以没人会自己回来——一直泄漏到有人手动跑 `gc` | 和第五轮(fork 临时树)、第七轮(半成品快照)同一个修法:**树还在就留着行**。这不会把它交出去——`pool_claim()` 只匹配 `state=1` 且 `snapshot_id`/`snap_created_at` 正是被 fork 的那个 ACTIVE 快照的行,而这条行之所以过期恰恰是这个前提不成立了——只是下次再收一遍。新增 `wfs_gc_report::pool_failed`,CLI 照 `tmp_failed` 的样子打一条 note,`gc --status` 本来就数(`pool_stranded`),重试套同一个上限。那个上限顺手**并成一份**:`wfs::kGcFailCap` / `gc_fail_bump` / `gc_fail_clear` 搬进 `store.cpp`(它本来就是 meta 表里的一行),按字符串键;`world.cpp` 留 `TrashJob` 形状的壳,`pool.cpp` 按条目路径做键(uuid 抽一次不复用,所以行和它可能留下的无主目录**算同一件事**)。无主目录删不掉也照此办理。另外:一条过期行的树原来会被**数两遍**(一遍过期行、一遍无主目录),`gc --status` 把一个说成两个 | `a2f66e8` |
| P2 `PRRT_kwDOUf7jGc6kAj5K` | `hardlinks.cpp:206` | `#hl` 头里写着它下面那一段有多少组、多少名字,而 `hardlinks_manifest_read()` 把这两个数**解析完就扔**(只留外部计数)。于是**最后一个成员没落盘**的清单读回来:组数照旧(fork 和 pool filler 拿来和行上 `hl_groups` 对的就是它),某一组只剩一个名字——而一个名字的组被 `restore_group()` **一声不吭地跳过**。fork 出来是两个独立文件,而快照记的是一个 inode 两个名字:P9 存在的全部意义,就这么悄悄没了,事后也查不出来,因为下游再没人读这份清单 | `HardlinkSet` 记下头里说的数(`header_groups`/`header_names`/`header_seen`),读完对不上就 `-EINVAL`(调用方早就把它映射成 `WFS_E_SNAPSHOT_DIRTY`):有 `hl` 行却没有头、组数不符、名字数不符,或者**任何一组少于两个名字**——扫描从不写这种组(名字不全在树内的组只计外部数)。写的那一半不用动:头写在它描述的那些行之前、来自同一个 `HardlinkSet`,`hl_drop_broken()` 丢组时会重算 `names`,所以这套代码写出来的每一份清单都自洽 | `79da4e2` |

**验收**:`safety.sh` **225 passed, 0 failed**(上一轮 221 → 本轮 +4:pool 条目留行);
`ctest`(WFS_FSKIT=OFF)**2/2**;`check-deps.sh` 全绿(没有新的系统调用、没有新的库)。

四条都先把测试跑红过:

- 在飞的 discard:去掉主人判活 → 第二个进程把行判成 **state 1**(ACTIVE),discard 照样回 0,
  留下"行 ACTIVE、家里没树、trash 里有树",紧接着一次**默认保留期**的 `gc` 就把它当无主孤儿
  删了(`trash_orphans != 0`)。
- collector 的条件写:把两条 UPDATE 换回无条件版 → `gc` 报 `worlds_deleted=1`,而那个刚被
  restore 回来的世界是 **state 3(DEAD)、树就在家里**。
- pool:把"树还在就留行"拿掉 → `gc: … 1 pool entries`(报告收掉了)、行没了(rows 0)、树还在
  磁盘上、什么都没说、也没人会回来收。
- 清单:把自洽检查拿掉 → 三种损坏(丢最后一行、丢头、头多报一组)`verify` 全回 0、fork 全回 0,
  而丢最后一行那次 **fork 出来是两个独立文件**(same inode 0, nlink 1)。

新增测试:

- `core_test`(P1 之一):自己的一个 store。崩溃缝**返回 0**(不是 `-EINTR`),在窗口里开**第二个
  store 句柄**——open 会跑恢复,再跑一次什么都不留手的 `gc`——然后本进程的操作继续跑完。
  `discard W<n>`(phase 0)、`restore W<n>`(phase 2)、`discard S<n>`(phase 0)三处都要:第二个
  进程必须让行停在 TRASHING,操作回 0,行和树一致,而且随后一次默认保留期的 `gc` 也不把那棵树
  当孤儿。第四段是**崩溃本身仍然能恢复**:同一个窗口,但主人是一个**不存在的 pid**
  (`wfs_test_fork_owner_pid`,文件里其他"生产者已死"的用例用的是同一个),store open 照旧判回
  ACTIVE。第五轮和第七轮的崩溃用例也都改用这个 pid ——seam 的 `kill -9` 发生在一个**还活着的**
  测试进程里,而真正的崩溃留下的是一个死掉的主人。
- `core_test`(P1 之二):新的 collector 缝 `wfs_test_before_trash_delete`(测试之外恒为 NULL),
  在"排队完、改名前"那一刻跑一整个 `wfs_world_restore()`,gc 跑在另一个 store 句柄上。restore
  必须赢,世界 ACTIVE 且在家、树是完整的、还 diff 得动,gc **什么都没删、没埋、也没报成失败**
  (它没出错,这条条目只是不再是垃圾了);再 discard 一次,同一个 collector 照常收掉。
- `safety.sh`(pool):自己的一个 store,`pool fill` 出一条条目,把 `snap_created_at` 改掉让它
  过期(快照行不可变,对不上就说明这个 id 现在是另一个快照了),再用"删不掉的 trash 条目"那条
  用例同一个 `deny delete,delete_child` ACL 锁住条目里的一个子目录。gc 必须留着行、在 stderr
  说清楚、`gc --status` 数出来;撤掉 ACL,下一次 gc 把树和行一起收掉。
- `core_test`(清单):一份好清单,三种损坏各跑一遍 `verify` / `fork` / `pool fill`,都必须是
  `WFS_E_SNAPSHOT_DIRTY`,而且**目标路径下什么都没发布、也不留半成品克隆**;把清单换回原样,
  fork 出来那一对又是同一个 inode、nlink 2。

另外把第七轮那条"不带预算的 gc 收干净半成品快照"的等待循环改成**同时等行消失**:那次断言在后台
worker 正好并行收同一棵树时会假红(树先没、行差几毫秒),本轮遇到过一次。

#### PR #1 review 第七轮:谁在写锁下改状态,谁在期限下删树(2026-09-19)

第七轮,Codex 三条(一条 P1、两条 P2)。P1 还是那条同一个主题的下一环:**改状态的一方也得在
写锁下决定**——前六轮把 fork、pool、adopt 都搬进了写锁,`restore` 没有,它先读一眼"基线还
ACTIVE",再搬树,再另起一个事务把世界写成 ACTIVE,于是 `discard S<n>` 正好挤在中间时**两边都
成功**。两条 P2 都在 gc 的"便宜那一半"里,而且是第五轮和第六轮各自那条的**镜像**:半成品快照
删不掉却照样删行(第五轮给 fork 临时树修过的洞),以及那一半里三处整棵树的删除**不看表**
(第六轮给 pool 修过的洞)。
**一条一个提交、一条一个测试,三个测试都先验证过"没有修复就会红"。**

| # | 位置 | 问题 | 修法 | 提交 |
|---|---|---|---|---|
| P1 `4053506303` | `world.cpp:1483` | `wfs_world_restore()` 用一次**独立的 SELECT** 确认源快照还 ACTIVE,然后把树从 trash 搬回家,再用**另一个事务**把行写成 ACTIVE。`wfs_snapshot_discard` 的引用计数跑在 `BEGIN IMMEDIATE` 里,拒的是 ACTIVE 世界、CREATING 世界和 pool 条目——而此刻这个世界还是 **TRASHED,什么都不拒**。于是:restore 读到 ACTIVE → discard S<n> 通过、快照进 trash → restore 搬完树、把世界写成 ACTIVE。**两件事都成功了**,而只能成功一件:活着的世界没有基线,`diff` 和 `verify` 此后永远是 `WFS_E_SOURCE_GONE` | restore 改成 discard 那套**三步协议倒过来跑**:(a) 一个 `BEGIN IMMEDIATE`——行必须是 TRASHED、树没有在被删(`.deleting`)、`trash_path` 还是刚才看的那个、基线必须是 ACTIVE,**全部在事务里重读**,然后行进 `WFS_ST_TRASHING`(`trash_path` 不动,树还没搬);(b) rename 回家;(c) 一个事务:ACTIVE、清 `trash_path`、写 `dir_dev`/`dir_ino`。**让这个决定生效的是 TRASHING**:`snapshot_refs_locked()` 现在把 TRASHING 世界算成**硬引用**(原来和 TRASHED 混在一起,而 TRASHED 不拒),discard 照此拒绝——从那里看不出这条行是在去 trash 的路上还是在回家的路上,而两个方向都不能丢基线。崩溃**不需要新东西**:`trashing_recover()` 早就按 lstat 判 TRASHING 行——树在 `trash_path` → 回 TRASHED(rename 没发生,restore 也就没发生),树在家 → ACTIVE。它不必重新 stat `dir_dev`/`dir_ino`:世界的 trash 永远和世界同卷(`<store>/trash`,或者 discard 撞上 EXDEV 时世界旁边的 `.wfs-trash`),同卷 rename 保 inode。(b) 失败就用 `trashing_commit()` 把行放回 TRASHED——那正是这个 helper 的意思("树在 trash 里"),也正是恢复会给出的判定。崩溃缝多了 phase 2/3,**从 phase 2 返回 0** 就能让测试在窗口里跑完一整个 `discard S<n>` 再让 restore 继续 | `455db7b` |
| P2 `4053506313` | `world.cpp:2627` | 生产者已经死掉的 CREATING 快照行,gc 对 `S<n>.wfs-tmp` 和 `S<n>` 各调一次 `fs_remove_tree`,**把两个返回值都扔掉**,然后删行。删不掉的 `S<n>`(EPERM、ACL、瞬时 EIO)从此**永久泄漏**:旁边那趟后缀扫描(`rm_tmp_in_store_dir`)只看 `*.wfs-tmp`,而唯一记得这个目录的那条行刚刚被删了。整整一棵克隆,`gc --status` 也看不见 | 和第五轮给 fork 临时树的修法一样,就在它下面那个分支:**两棵树都确认不在了才删行**。否则行留着(仍是 CREATING),记进 `wfs_gc_report::tmp_failed`,并套上同一套每条目失败计数(`gc_fail_bump`/`gc_fail_clear`,键是 `S<n>`)——前几次唤醒设 `work_remains`,之后每次照报但不再自己叫醒 worker。`wfs_gc_status()` 也把这些树数进 `creating_stranded`:对运维来说它和搁浅的 fork 临时树是同一件事(被一条 CREATING 行独占的空间),所以共用那行 `abandoned:`,措辞从"half-built fork tree"改成"half-built tree",`gc` 打的那条 note 同理 | `d0c2701` |
| P2 `4053506322` | `world.cpp:2583` | gc 的"便宜那一半"在有期限的 trash 循环**前面**整个跑完,而里面有**三处**整棵树的删除从不看表:被遗弃的 fork 半成品克隆、半成品快照的 `S<n>`/`S<n>.wfs-tmp`,以及紧跟其后 `<store>/snapshots` 下的 `*.wfs-tmp` 后缀扫描。每一棵都是一整个工作区或一整棵源树的克隆——新用例里量到的,12 万条目要约 5 秒——于是两秒一轮的 worker、或者一次交互式 `gc`,可以在这里花掉几分钟。和第六轮 pool 那条是同一个洞 | 三处都收下本轮的期限:每条目之前看一次表,并把期限交给 `fs_remove_tree()`(它的遍历从第四轮起就是逐条目看表的),所以大树停在半路而不是只停在树与树之间。**超时不是失败,也不按失败处理**:行保持 CREATING 和它的 `tmp_path`,每条目失败计数**不动**(那是用来放弃"永远删不掉"的树的,是另一回事),`work_remains` 置位,并且**停下整个循环**而不是再开一棵同样做不完的树——和 `pool_collect()` 一个形状,留下的也正是后继会重新发现的形状。后缀扫描必须一起改,否则前两处白改:它删的就是 CREATING 循环刚停在上面的那些 `S<n>.wfs-tmp`,而且紧接着就跑、一口气删完。两个调用方本来就带着期限(worker 来自 `WORLD_GC_BATCH_SECS`,交互式 `gc`(不带 `--now`)从第六轮起也是同一个旋钮) | `521aba3` |

**验收**:`safety.sh` **221 passed, 0 failed**(上一轮 207 → 本轮 +14:半成品快照留行 5 条、
期限两处 9 条);`ctest`(WFS_FSKIT=OFF)**2/2**;`check-deps.sh` 全绿(没有新的系统调用、
没有新的库)。

三条都先把测试跑红过:

- restore:把"TRASHING 世界算硬引用"从 discard 的拒绝里拿掉 → `core_test:1727` 的
  `g_race_rc == WFS_E_SNAPSHOT_IN_USE` 红:discard 回 0、restore 也回 0,**两个都成功了**,
  这就是这个 bug 的一句话版本。
- 半成品快照:把"两棵树都没了才删行"换回老写法 → 五条里红四条,行没了(rows 0)而树还在磁盘上,
  正是那次泄漏。
- 期限:三处都不传期限 → 同一对一秒 wake 分别跑了 **4817 ms** 和 **4882 ms**(修好之后都是
  1032 ms),把两棵树整个删光、两条行都埋了、什么都没报,九条里红六条。

新增测试:

- `core_test`(P1):自己的一个 store。**窗口里的交错**——世界在 trash 里,restore 提交完
  TRASHING 行,就在那里跑一整个 `discard S<n>`:discard 必须是 `WFS_E_SNAPSHOT_IN_USE`,
  restore 回 0,世界 ACTIVE 且在位,快照仍 ACTIVE,而且这个世界**还 diff 得动**、快照还
  verify 得过。**反过来的顺序**:先 discard 快照,restore 就是 `WFS_E_SOURCE_GONE`,树还在
  trash 里、行还是 TRASHED、家里什么都没有。**两个 kill**:停在 (a) 之后 → TRASHING、树在
  trash、这期间快照**拿不走**,store open 判回 TRASHED;停在 (b) 之后 → TRASHING、树在家,
  store open 判成 ACTIVE 而且世界 diff 得动。
- `safety.sh`(半成品快照):把一条生产者已死的 CREATING 快照行直接写进一个**自己的 store**
  (别的 store 会被它占掉上面那些用例按名字点到的快照号),在 `<store>/snapshots/S<n>` 下放一个
  文件,用"删不掉的 trash 条目"那条用例同一个 `deny delete,delete_child` ACL 锁住。gc 必须
  留着行、说树删不掉、在 `gc --status` 里数出来;撤掉 ACL,下一次 gc 把树和行一起收掉。
- `safety.sh`(期限):两处各一个自己的 store,免得互相花对方的预算、或者被对方的 worker 收掉。
  一条生产者已死的 CREATING 世界行配一棵 **12 万条目**的 `tmp_path` 树,和一条 CREATING 快照行
  配一棵 12 万条目的 `S<n>.wfs-tmp`;一秒预算的 `gc` 要按时回来、行还是 CREATING、树还剩一部分、
  输出里说自己交班了、**并且不把超时报成删不掉**,然后一次不带预算的 `gc` 收干净。这两棵树是
  对目录做一次 `clonefile(2)` 克隆出来的(一秒,`cp -Rc` 要十五秒),源就是上面 pool 那条用例
  已经建好的那棵。

#### PR #1 review 第六轮:还得住的基线,还得算数的期限(2026-09-19)

第六轮,Codex 四条(一条 P1、三条 P2)。P1 还是"**谁在写锁下数引用**":fork 和 pool 上一轮都
已经在写锁里重读快照行了,`adopt` 没有——它只凭 marker 登记,把 marker 里的 snapshot id 原样
写进一条 ACTIVE 世界行,快照是死是活一概不问。三条 P2 是三处"**失败/中断被当成了正常结果**":
manifest 读不出来当成"没有硬链接"、`--now` 碰上 collector 改过的名字当成"树已经没了"、
gc 的期限管得住 trash 却管不住 pool。
**一条一个提交、一条一个测试,四个测试都先验证过"没有修复就会红"。**

| # | 位置 | 问题 | 修法 | 提交 |
|---|---|---|---|---|
| P1 `4053405511` | `world.cpp:1604` | `wfs_world_adopt()` 找原世界的行**不看状态**,把 `m.snapshot` 写进新行时也**不在写锁里**确认那个快照还在。于是:唯一一个注册世界被 discard 掉 → 快照引用数为 0、discard 通过 → 再 `adopt` 一份副本,store 里就多了一个 ACTIVE 世界,而它的基线躺在 trash 里(或者已经被 `--now` unlink 了)。它此后每一次 `diff` 都是 `WFS_E_SOURCE_GONE`,`restore` 也是;discard 的引用检查和 rename 之间那一瞬间同样是这个洞 | 把快照行**重读进 adopt 的插入事务**——`Txn` 就是 `BEGIN IMMEDIATE`,和 discard 第 (a) 步拿的是同一把写锁:要么快照还是 ACTIVE、这条新行就是 discard 接下来会数到的引用,要么 discard 先到,`adopt` 以 `WFS_E_SOURCE_GONE` 被拒。**除 ACTIVE 之外一律拒,TRASHING 也拒**:从这里看,"被 kill 在半路的 discard"和"下一步就要 unlink 的 `--now`"长得一模一样。marker 属于**别的 store** 的副本不受影响(这边没有父行,照旧以 snapshot_id 0 收编)。CLI 报出快照号,并告诉你副本还能怎么留(删掉 `.world`、当普通目录 `init`)。`trashing_recover()` 那句"adopt 会绕过这个论证"的注释改成:现在它不会了,那里的引用计数是**双保险** | `48b9c21` |
| P2 `4053405514` | `world.cpp:1139` | 行上的 `hl_groups` 说"这个快照有 n 组共享 inode 的名字",manifest 的那一节说"是哪些名字"。fork 和 pool 填充都按前者开门、按后者重放,而 `clonefile(2)` 已经把这些链接全断了——两处却都把"manifest 读不出来"和"读出来的组数比行上少"当成"没什么要重放的",然后**照常发布**:快照记着一个 inode 挂两个名字,世界(或者一个 READY 的 pool 条目)里是两个独立文件。没有任何声音,事后也查不出来,因为下游再没有人读 manifest | 两处都改成失败,码是 `WFS_E_SNAPSHOT_DIRTY`("行和 manifest 对不上"正是这个意思)。fork 沿着 do-block 里所有别的失败那条路回滚克隆和 CREATING 行;填充器的错误路删树删行,条目**永远不会变成 READY**。两个数是同一个 `HardlinkSet` 在同一个地方写下的(`wfs_snapshot_create`,而且是在上一轮把"没能完整留下的组"剔掉之后),所以对不上只可能是 manifest 被损坏了。**一个例外**,免得"优化"变成"拒绝":从**活 World** fork 时借的是它来源快照的候选组,现在只在那个快照还 ACTIVE 时借——基线被 trash 掉或被 reconcile 埋掉的世界仍然是世界,仍然 fork 得动,只是没有组可以重建。`wfs_snapshot_verify` 把这一节读回来对数,不一致按 modified 报(整份 manifest 不见是更早就报的错:它同时是门的锁文件) | `42a5f78` |
| P2 `4053405516` | `world.cpp:1330` | 删一个 trash 条目是两步:先改名成 `.deleting`,再 unlink;collector 把新名字记进行里是**改名之后**另一个事务的事。这中间——以及在这中间被打断之后——行里写的还是那个已经不存在的名字,`trash_mark_deleting()` 于是回 `-ENOENT`。World 的两条 `--now` 路**把它忽略了**:`deleting` 是空的、一次 unlink 都没跑,行标 DEAD、返回 0,而 collector 还在删那棵树。`discard W<n> --now` 承诺的空间没回来,而"树还在磁盘上"的唯一记录已经被写成"dead" | Snapshot 那条路上一轮已经有 `trash_follow_deleting()` 了。把它和 `snapshot_delete_now` 合成一个 `trash_delete_now(s, id, is_snapshot, trash)`(`is_snapshot` 开关的用法和 `trashing_undo`/`trashing_commit` 一致),三个调用点共用:World 的 TRASHED + `--now` 分支、普通 discard 的 `--now` 尾巴(行一变 TRASHED,collector 就可能接手)、以及 snapshot discard。`-ENOENT` 从此是它该有的意思——**两个名字下都没有**,别人已经删完了,行进 DEAD 是因为树真的没了 | `b3fd7b1` |
| P2 `4053405519` | `world.cpp:2657` | gc 的"便宜那一半"是几个 stat 加一趟 SQLite,所以它每次都整个跑完,跑在有期限的 trash 循环**前面**。`pool_collect()` 在里面——可 pool 条目一点都不便宜:它是**整棵快照的克隆**,一个 12 万条目的陈旧条目一口气删掉要约 5 秒(新用例里量到的)。于是两秒一轮的 worker、或者一次交互式 `gc`,可以在这里花掉几分钟而从不看表,正是 `max_secs` 要挡的前台争用(P16) | `pool_collect()` 收下本轮的期限和 report 的 `work_remains`:每棵树之前看一次表,并把期限交给 `fs_remove_tree()`(它的遍历上一轮起就是**逐条目**看表的),所以大树停在半路而不是只停在树与树之间。没删完的东西保持**后继能重新发现它**的形状:pool 行还在、无主目录还列得出来,下一轮照样被判 doomed。半删的树永远不会被当成 ready 条目发出去——`pool_claim()` 只认 `state=1` 且 `snapshot_id`/`snap_created_at` 属于某个 ACTIVE 快照的行,而 doomed 的行必定至少破一条。分类那一半拆成 `pool_scan`,于是 `gc --status` 能数而不删:`wfs_trash_stat` 多一个 `pool_stranded`(和上一轮的 `creating_stranded` 并排),CLI 多一行。交互式 `gc`(不带 `--now`——带了就是"整件事现在做")也收下 worker 的批次上限,剩下的按**这份 report** 起 worker,而不是问只懂 trash 的 `wfs_gc_pending()` | `091cb97` |

**验收**:`safety.sh` **207 passed, 0 failed**(上一轮 187 → 本轮 +20:adopt 3 条、manifest 7 条、
`--now` 跟名字 4 条、pool 期限 6 条);`ctest`(WFS_FSKIT=OFF)**2/2**;`check-deps.sh` 全绿
(没有新的系统调用、没有新的库)。

四条都先把测试跑红过:

- adopt:把那段状态检查编译掉 → `core_test:1481` 的 `wfs_world_adopt(...)` 回 0,要的是 `-1011`。
- manifest:把两处检查换回老写法 → `core_test:1716` 的 fork 和 `core_test:1723` 的 pool fill
  都回 0(要 `-1008`);`safety.sh` 那两条 pool 断言是 "exit 0, wanted 3"。
- `--now`:把 `trash_follow_deleting()` 从公共函数里拿掉 → "树真的没了""trash 空了"两条红,
  而退出码仍是 0、行仍然是 dead——**这就是这个 bug 的一句话版本**。
- pool 期限:不把期限传下去 → 同一次 wake 跑了 **4924 ms**(修好之后 1041 ms)、把整个条目删光、
  什么都没报,六条里红四条。

新增测试:

- `core_test`(P1):自己的一个 store。ACTIVE 时 adopt 成功、行带着基线、`diff` 跑得通,而且
  这个被收编的世界本身就让 `discard S<n>` 变成 `WFS_E_SNAPSHOT_IN_USE`(同一条不变量的另一半);
  把引用全撤掉再 discard 快照,TRASHED(不带 `--now`)和 DEAD(带 `--now`)两种状态下 adopt 都
  被拒、**什么都没写下去**(没有行、副本仍是未注册副本);另一个 store 的世界副本照旧收编得了。
  TRASHING 那一段窗口在第五轮那个崩溃缝的块里钉着:那里原来靠 adopt 制造"检查之后才出现的引用"。
- `core_test`(manifest):一对硬链接的快照,把 manifest 里的 `hl ` 行剥掉,要求 verify、fork、
  pool fill 三处都拒,`--to` 上没有树、没有 `.wfs-fork-` 残留、没有留下行、pool 里没有条目;
  把 manifest 放回去,三处都恢复正常、两个名字又是同一个 inode。"整份读不出来"那一半用
  `--hard` 快照来测(门控快照的 manifest 就是门的锁文件,删了会更早地报错)。
- `safety.sh`(`--now`):discard 一个世界,**手工替 collector 做它的第一步**改名
  (`W<n>-<t>` → `W<n>-<t>.deleting`,第二步故意不做),然后 `discard W<n> --now` 必须返回 0、
  树没了、trash 空了、行是 dead。
- `safety.sh`(pool 期限):预克隆一个 **12 万条目**的 pool 条目,再把它的快照从 store 底下抽走
  (于是行 dangling、`--reconcile` 埋掉它,条目就此陈旧),给一秒预算。这一轮 wake 要按时回来、
  把没做完的留在后继找得到的地方、在输出里说自己交班了、被 `gc --status` 数出来,
  而后继链要自己把它收干净。

#### PR #1 review 第五轮:崩溃窗口与"源是活的"(2026-09-19)

第五轮,Codex 四条(一条 P1、三条 P2)。P1 是 discard 的崩溃窗口——**行和树之间那一瞬间**,
谁先写决定了被 kill 之后 trash 里那棵树还有没有人认领;三条 P2 分别是:快照克隆时不校验活源、
临时树删不掉却照样埋行、`--now` 对已经在 trash 里的 snapshot 不认。
**一条一个提交、一条一个测试,四个测试都先验证过"没有修复就会红"。**

| # | 位置 | 问题 | 修法 | 提交 |
|---|---|---|---|---|
| P1 `4053320017` | `world.cpp:1435` | snapshot discard 把 `rename` 放在**事务里面**,commit 之前。中途被 kill:SQLite 回滚,行回到 ACTIVE,树却已经躺在 `trash/` 里——**没有任何行提到它**。下一个 collector 按"无主孤儿"规则**立刻删掉**(不看保留期、不能 restore),于是每一个 fork 自它的 World 都失去了 diff/verify 的基线(P4/P10)。World discard 是反过来的顺序(先 rename 后 commit),同一个洞 | **三步协议,两条路统一走**:(a) 一个事务做引用检查 + `state=WFS_ST_TRASHING` + `trash_path` = 树**即将**拥有的名字,提交;(b) rename;(c) 一个事务 `state=TRASHED`。World 的 EXDEV 回退先把旁路 trash 名提交上去再搬。**恢复**(`wfs::trashing_recover`,每次 `wfs_store_open` 和每次 `wfs_gc_ex` 开头,两条走索引的 SELECT):树还在老位置 → ACTIVE;树在 `trash_path` → TRASHED;**snapshot 还有引用 → 搬回去 + ACTIVE**(fork/pool 都在写锁下重读行,(a) 之后拿不到它,但 `adopt` 只凭 marker 登记并带上 snapshot id,所以按**现在**数而不是按这个论证信);两边都没有 → ACTIVE,交给 reconcile 那一半去报 dangling(比这里替谁都没删的树宣判死亡强)。**孤儿规则本身**:trash 里的目录只有"**任何状态**下都没有行提到它"才是孤儿,`.deleting` 拼写算同一个名字,行也因此要跟着 collector 的那次 rename 走 | `53d3ce6` |
| P2 `4053320021` | `world.cpp:661` | 快照/checkpoint 的重放拿到的 `verify_root` 是 `nullptr`,等于关掉唯一一个会去看源的检查。源是**用户的活目录**(checkpoint 更是活 World),scan 和 clone 是两趟遍历;这中间被换掉的组成员,只要**大小和 mtime 都一样**,就通过了 `restore_group()` 的"还是 scan 看见的那个文件"判定,被一条 link 覆盖掉——快照里两个名字装着同一份内容,一声不吭 | 把**活源**传成 verify root,`group_still_linked()` 于是在动克隆之前把整组名字在源里重新 lstat 一遍(代价:每个硬链接名一次 lstat,和头文件一直标的价钱一样)。**另一半**在下一环:fork 重放快照 manifest 时**不带 verify root**(快照是不可变的),所以 manifest 里留着一个快照自己都没有的组,等于让 fork 一步之后把这次拒绝覆盖的内容再覆盖一次。`hardlinks_restore()` 因此报出**哪些组没能完整留下**(`HardlinkRestore::broken`),`wfs_snapshot_create` 在写 manifest 和写 `hl_groups` 之前把它们剔掉——**快照说自己有什么,就得是自己树里真有什么** | `ef57559` |
| P2 `4053320025` | `world.cpp:2210` | fork 的临时树在**用户自己的目标父目录**里,名字是抽出来的,而且故意不被后缀扫描找到——CREATING 行是它唯一的记录。删树失败(EPERM、瞬时 EIO)之后那条 UPDATE 照样把行标 DEAD 并清空 `tmp_path`:**整棵克隆永久搁浅**,没有任何东西还知道它的名字,也没有哪次 gc 还能回来重试 | 删不掉而树还在 → **行留着**(仍是 CREATING,`tmp_path` 原样),记进 `wfs_gc_report::tmp_failed` 和 `gc --status`(一行 `abandoned:`——它不在 trash 里,但在等同一个 collector),并**套用 trash 条目那套失败计数与上限**:前几次唤醒设 `work_remains` 让后继重试,之后每次都报告但不再自己把 worker 叫醒。树确实已经不在了,照旧埋行 | `bc6b18d` |
| P2 `4053320027` | `cli/main.cpp:787` | 先 `discard S<n>` 再 `discard S<n> --now` 被拒,于是想在保留期之前把空间拿回来只能动全 store 的 `gc --now --retention 0`。World 一直是认的,`wfs_snapshot_discard` 的 API 契约也一直写着 `immediate` 会删树并把行留成 DEAD | TRASHED + `immediate` 时把删除提前:引用检查和搬迁都是历史,剩下的就是 collector 本来要做的那次 unlink——同样的两步(rename 成 `.deleting`、记下新名、unlink、行 DEAD),抽成一个两条路共用的函数;collector 已经在行底下改过名的条目跟着 `.deleting` 走而不是报找不到。不带 `--now` 仍是 `-EALREADY`,而那句拒绝现在会告诉你 `--now` 是干这个的 | `53e60a2` |

**验收**:`safety.sh` **187 passed, 0 failed**(上一轮 177 → 本轮 +10:临时树删不掉那条 5 个断言、
`--now` 那条 5 个);`ctest` 两个配置 WFS_FSKIT=OFF **2/2**、ON **3/3**(只剩 FSKit 那 4 条冻结 API 的
deprecated 警告);`check-deps.sh` 两个配置全绿。

四条都先把测试跑红过:

- P1:只认 TRASHED 行的 claim 列表 → `gc --status` 那条断言红;去掉恢复 → "回到 ACTIVE" 那条红。
- 活源:把 verify root 改回 `nullptr` → `hl_groups == 0` 红;把这条断言也关掉 → 快照里的 `b.txt`
  读出来是 `AAAA`,**数据丢失本身**。
- 临时树:把"树还在就留着行"那个分支关掉 → 行直接进 state 3,五条里红四条。
- `--now`:把 CLI 那个无条件拒绝放回去 → 三条红。

新增测试:

- `core_test`(P1):自己的一个 store,新测试缝 `wfs_test_trash_crash`(库里恒为 NULL)把 discard
  停在 phase 0(行已提交、树没动)或 phase 1(树已搬、行没提交)。四种情况:rename 之前被杀 →
  ACTIVE 且树没动过;rename 之后被杀、并且有一个 `adopt` 进来的 World 持有这个 snapshot →
  **树搬回去**、World 还能 diff;rename 之后被杀、没人持有 → TRASHED 然后照常被收走;
  以及一个 World 的两个方向,**由 store open 而不是 gc 解决**,`restore` 照样把它带回来。
- `core_test`(活源):新测试缝 `wfs_test_before_snapshot_clone` 在克隆前一刻把硬链接对的一个成员
  换成**不同内容、相同大小、相同 mtime**(`utimensat`)的文件。快照必须两份内容都留着、
  `hl_groups` 报 0 而 `hardlinks` 仍报源当时的 2、verify 干净、fork 出来也是两个文件;
  对照组(没人碰的同一棵树)照样把组连回去。
- `safety.sh`(临时树):直接往库里写一条 CREATING 行,树用和"删不掉的 trash 条目"同一条 ACL
  (`deny delete,delete_child`,chmod 和 chflags 都摘不掉)锁住。gc 要留下行和 `tmp_path`、
  在 stderr 说树删不掉、`gc --status` 数得出来;摘掉 ACL,下一次 gc 把树删掉、把行埋掉、不再数它。
- `safety.sh`(`--now`):`discard S2` 之后 `discard S2 --now` 要在返回前把树删掉、说 "S2 deleted"、
  行读作 dead、trash 空;不带 `--now` 的第二次 discard 仍被拒,而且拒绝里给出 `--now`。

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

#### P17 收窄:打不开的库不等于坏掉的库(2026-09-21)

实测(macOS 27 arm64,APFS):store 所在卷写满之后,**每一条** `world fs` 命令——连只读的
`status`/`list`,以及 `discard` 和 `gc`——都以退出码 3 报
「`its metadata3.db is missing or unreadable` … `restore metadata3.db from a backup, or move the
directory aside (`mv <store> <store>.damaged`)`」,而那个 40 960 字节的 `metadata3.db` 好好的。
store 是 WAL,SQLite 连**读**都要先建 `-shm`,满卷上建不出来(`SQLITE_IOERR_SHMOPEN`);
腾出 8 MiB,所有命令立刻恢复,一行数据都没丢。照着那句提示做的人,会为一个瞬时状态
把整个 store 里的每一个快照、每一个 world 变成孤儿。

这就是第三十二轮那条规矩再往前一步——**「查询失败不等于查到了空」,于是「库打不开不等于库坏了」**。

| # | 位置 | 问题 | 修法 | 提交 |
|---|---|---|---|---|
| P1 | `core/src/store.cpp` `wfs_store_open()`(~1428 / ~1450 / ~1520 / ~1530)、`db_move_to_schema3()`、`db_user_version_at()` | `sqlite3_open_v2()` 失败、`PRAGMA user_version` 读不出来、pragma 执行不了,一律 `WFS_E_STORE_DAMAGED`——**不问为什么**。满卷、EACCES、熬过 busy timeout 的 BUSY,拿到的都是那句「挪走重建」的提示 | 判据拆成两半,由 `open_failure_verdict()` 出结论。`WFS_E_STORE_DAMAGED` 从此只表示库**确实**不是一个能用的库:不在、不是普通文件、比 SQLite 那 100 字节文件头还短、头 16 字节不是 `SQLite format 3\0`,或者 SQLite 自己读完了报 `SQLITE_NOTADB` / `SQLITE_CORRUPT`。别的一律把**原因**还回去。这条路上一个字节都不写:文件头是一次 16 字节的 `read(2)`,库本身从来不会被顶掉(`SQLITE_OPEN_CREATE` 只建不存在的库) | `825589a` |
| P1 | 同上,`volume_out_of_space()` | 「是不是满了」——判据**不是** `avail == 0`。实测:`statfs(2)` 在一个 4 KiB 写、`mkdir(2)` 和 SQLite 那 32 KiB `-shm` 全部 ENOSPC 的卷上,仍然报 **11 247 616 字节可用**(512 MiB 镜像的 2.1%);`SQLITE_IOERR_SHMOPEN` 的 `sqlite3_system_errno()` 是 **3(ESRCH)**,完全帮不上忙 | 先问 `SQLITE_FULL`,再问 `sqlite3_system_errno()`(ENOSPC/EDQUOT),都问不出来才问卷:**可用空间低于 32 MiB** 算满。阈值是个尺寸而不是零,而且比实测留出余量:这个函数**只在** SQLite 已经在一个文件头完好的库上报了 I/O 类错误之后才被问到,它的两个答案(`-ENOSPC` / `-EIO`)**都变不成 `WFS_E_STORE_DAMAGED`**,所以往宽里猜是安全的——猜高,代价是一条「这个卷满了」说在一个 32 MiB 可用时出别的 I/O 错的 store 上;猜低,代价是 `-EIO`,也就是这条路原来的答案 | `825589a` |
| P2 | `core/src/db.h` `map_sqlite()` | `SQLITE_FULL` 落到 `default` 上,变成 `-EIO`:一个满卷上失败的事务,CLI 说的是「I/O 错误」 | `SQLITE_FULL` → `-ENOSPC`。调用者只有 `Txn::err()` 和 `Txn::commit()`,没有测试钉过它的 `-EIO` | `825589a` |
| P1 | `cli/main.cpp`(~1953 之后) | 那句致命的提示 | 新增 `-ENOSPC` 分支,说的正好是上面那句的**反面**:卷满了、库本身完好、这里什么都没建没改没删、**不要**把目录挪开、不要重开 store、不要从备份恢复,腾空间再跑一遍。退出码 **1**——3 是「被安全规则拒绝」,卷写满是环境,和这个函数里其它非拒绝的 open 错误一致。`WFS_E_STORE_DAMAGED` 的文案同时改准:「gone, or is a file that is not a database」 | `825589a` |

**先验证会红**(`2b46d30`,三条测试都先在未修复的代码上跑过):

- `core_test`:一个有树、`metadata3.db` 完好的 store,用新测试缝 `wfs_test_db_open_fail_once`
  (库里恒为 0,形状和 `wfs_test_txn_fail_once` 一样)让那次 open 因为**不是库的原因**失败。
  未修复时 `SQLITE_FULL` / `SQLITE_IOERR` / `SQLITE_BUSY` **三个都回 `-1017`**;
  修复后分别是 `-ENOSPC` / `-EIO` / `-EBUSY`。反方向一并钉死:库不在、名字上是个目录、
  截断到文件头以下、magic 不对、magic 对而后面是垃圾——**仍然**是 `WFS_E_STORE_DAMAGED`。
- `disk_full_test`:同一件事在**真正写满**的卷上(`disk_full.py` 那个私有 512 MiB APFS 镜像)。
  未修复:`wfs_store_open(...) -> -1017`。要求是 `-ENOSPC`、`metadata3.db` 前后**逐字节相同**、
  腾出空间之后每一行都还在。
- `disk_full_test` 的 CLI 那一半:满卷上跑 `world fs status`,退出码必须是 1,输出里不许出现
  `move the directory aside`、`mv `、`restore metadata3.db`,而且必须说要腾空间。
  wrapper 为此把同一次构建的 `world` 二进制作为 argv[2] 交给测试。

**验收**:`ctest` **2/2**;`check-deps.sh` 全绿(没有新依赖,`statfs(2)` 在 libSystem 里);
`safety.sh` **298 passed, 0 failed**;`test_disk_full_wrapper.py` **12/12**;
`disk_full.py` 通过。

**同一类的第二处(review 跟进,`5db1038` + `b01695c`)**:一个**全新的、还没有库的 store**,
在满卷上打开。主 open 带着 `SQLITE_OPEN_CREATE`,于是失败发生在**创建**上,而那个名字上此时
要么什么都没有,要么是 SQLite 的 `open(O_CREAT)` 在没空间之前留下的**零长度文件**——两者都是
「不在,或者短过文件头」,正是 P17 的形状;于是 `world fs init` 在满卷上说的是
「这个 store 还有树…把目录挪开」,而它是空的,也从来没坏过。
**「创建不出来的库,也不等于坏掉的库。」**

修法:`open_failure_verdict()` 多收一个 `db_existed`——`store_layout()` 在这次 open 动任何东西
**之前**记下的事实,**显式传进来**,不是事后去 stat 猜(事后已经晚了)。「确实不是一个库」这个判断
**只对本来就在那儿的库**成立;对一个这次 open 打算创建的库,「不在」和「零长度」是一次没做完的
创建留下的样子。`SQLITE_NOTADB` / `SQLITE_CORRUPT` 不动;schema 2→3 那两个 helper 的站点一律
传 `true`(它们只对「布局认定是普通文件的 `metadata.db`」调用,在那里「被人搬走了」仍然是损坏)。
P17 一点都没松:有树而库读不出来的 store,`store_layout()` 在 open 之前就已经挡掉了,
落到这四个站点上的只剩「既没树也没库」的那一种。CLI 的 `-ENOSPC` 文案原来断言
"the database itself is intact",对一个还没有库的 store 是假话——改成两种情况下都成立的说法:
这里什么都没建没改没删,**本来就在的** `metadata3.db` 没有被碰过。

先验证会红:`core_test.cpp:3151` 在未修复的代码上
`wfs_store_open(fstore, &f) -> -1017 … wanted -28`。
`disk_full_test` 里那条端到端的(满卷上打开一个空 store 目录)**修复前就是绿的**——真·满卷会让这次
open 停在 `snapshots/` 的第一个 `mkdir(2)` 上、根本走不到 SQLite 的 create,所以它钉的是 CLI 可见的
行为,不是判据本身;判据那一半由 `core_test` 的测试缝钉。失败的 create 留下的零长度库**不需要清理**:
`store_layout()` 把 size 0 读作「没有可读的库」,而一个没有树的 store 就只是个新 store,
下一次 open 直接把它变成一个普通的新 store(`core_test` 钉了这一条和它留下的那一个库)。

**同一类的第三处(PR #8 review,`ff72c7b` + `6bd79e9`)**:上面那个「零长度的 `metadata3.db`」
在**卷还满着**的时候没有消失,于是**第二条命令**踩了同一个坑。`store_layout()` 的
`lay.db_existed = (nrc == 0)` 问的是**名字在不在**——这是 2→3 升级协议要问的那个问题
(这里有没有一个 inode 要闸、要 link、要数链接数);判据借用了它,于是重试时读成「这儿本来有个库」,
看到一个短过文件头的文件,又报 `WFS_E_STORE_DAMAGED`:第一次 `world fs init` 说
"no space left on device",紧接着的第二次说「把目录挪开」。

修法:布局同时记两件事,名字分开。`db_existed` **一字不动**(升级路径和 M1 持有者闸门读的还是它),
新增 `db_had_content`——两个名字下的 `st_size > 0`,**在搬库之前**读,所以「宣布搬完」那个分支
不用对它说任何话;判据收的是 `db_had_content`。**零长度不是丢了的库,是没做完的创建**,
卷拒绝多少次它都还是这个。这掩盖不了真的损坏:`store_layout()` 本来就把零长度读作「库不可读」,
**有树**的 store 在这次 open 碰到 SQLite 之前就已经被那道守卫以 `WFS_E_STORE_DAMAGED` 拒了——
守卫原样保留,而 `db_had_content` 有意就是那道守卫自己的尺寸判据减去 `access(2)`。
**有字节**的文件判法不变:失败的创建留下的是零字节,绝不会是半个文件头(SQLite 第一次写就是
一整页),所以短文件和 magic 不对的文件,是别人的文件占了库的名字。

先验证会红:`core_test.cpp:3208`(第一次重试)在未修复的代码上
`wfs_store_open(rstore, &r) -> -1017 … wanted -28`。测试先断言第一次失败**确实**留下了那个
零长度文件(不然整条用例是空跑),然后把邻居一并盖住:再 `SQLITE_FULL` 三次(每次 `-ENOSPC`)、
空间充足时的 `SQLITE_IOERR`(`-EIO`)、失败消失后 open 成功且 id 从 1 发、**有树**时同一个零长度库
仍然 `WFS_E_STORE_DAMAGED`、空 store 里 16 字节和 magic 不对的文件仍然 `WFS_E_STORE_DAMAGED`。

**同一类的第四、第五处(PR #8 review 第二轮,`e02b5ee` + `ee1188d`)**:

- **EDQUOT 不是「卷满了」**。per-user / per-group 配额可以在卷半空的时候用光,
  「在这个卷上腾空间」对它毫无用处。`volume_out_of_space()` 原来把 EDQUOT 和 ENOSPC 并在一起;
  现在 `sqlite3_system_errno()` 说 EDQUOT 就**单独**回 `-EDQUOT`(而且只有这一条路),
  `SQLITE_FULL` 和 statfs 那条规矩照旧 `-ENOSPC`,`map_sqlite()` 不动。注释里记下:
  2026-09-21 实测的 APFS **卷**配额报的是 ENOSPC、从不报 EDQUOT,所以 EDQUOT 下剩的是经典的
  用户/组配额。`wfs_strerror` 通过 `strerror` 覆盖 `-EDQUOT`。驱动它需要第二条测试缝——
  已有的那条注入的是 SQLite 结果码,而这是系统 errno——`wfs_test_db_system_errno`,
  同样的规矩:读一次、清回 0、非测试运行里恒为 0。红:
  `core_test.cpp:3302: wfs_store_open(qstore, &q) -> -28 … wanted -69`。
- **文案不许说过头的话**。`-ENOSPC` 那段原来写「nothing here was created, changed or removed」,
  对一个**新的或做了一半的** store 是假话:open 失败之前它自己可能已经建了 store 目录、`VERSION`、
  那几个子目录、`upgrade.lock`,或者那个零长度的 `metadata3.db`。改成每种情况下都成立的说法:
  **本来就在的**东西一样没动(没有快照、没有 World、没有原本就在的 `metadata3.db`),
  而一个做了一半的新 store 不是要收拾的烂摊子,是**同一条命令有空间之后接着做完**的安装。
  其余性质全留:为什么失败、腾空间(或腾/抬配额)再跑一遍、绝不提挪开/重建/恢复、退出码 1。
  红:`disk_full_test.cpp:249: CHECK failed: strstr(out, "nothing here was created, changed or removed") == NULL`。
  同一趟顺手查出并改掉另外两处同类的话:`WFS_E_STORE_DAMAGED` 的 CLI 文案和它的 `wfs_strerror`
  串都写着 store「still holds trees」,而 `store_layout()` 有好几条路**没有树也会**判损坏
  (两个库名处在协议产生不出来的状态);两处都改成对所有这些情况都成立的说法,树那句保留下来
  当作「为什么 id 会撞车」的理由。

**同一类的第六处(PR #8 review 第三轮,`b75f133` + `e762df6`)**:`-ENOSPC` / `-EDQUOT` 那段
第二轮改成的「nothing that was already here was changed or removed」**还是说过头了**。
**两条**路让它不成立,不是一条:

- **schema 2 的老 store**。`wfs_store_open()` 先跑 `version_upgrade()`(`VERSION` 2→3),
  再跑 `db_move_to_schema3()`(改 journal mode、`link` + `RENAME_SWAP` 把库换到新名字、
  旧名字变成空目录),**然后**才轮到那次可能没空间的 open。到这一步为止,本来就在的
  `VERSION` 已经被重写了。
- **`trashing_recover()`**,每一次 open 的最后一步:它把被 kill 的 `discard` 留下的 state=4
  行**一行一个事务**地收尾,于是前面的行可能已经提交、后面的行才撞上 `SQLITE_FULL`。

迁移本身**不是**第三条:`migrate_schema()` 是**一个**事务,它唯一能走到这里的 `-ENOSPC` 是
COMMIT 失败——事务还开着,析构函数回滚,一行记录都没动。

于是保证改成按**「丢没丢」**说,这在每一条路上都成立:没有快照、没有 World、没有库里的记录
**丢失**,这里什么都没被删掉;已经做了的那部分(新 store 的目录和空文件、把老 store 往当前布局
搬的头几步、或者替一条被中断的 `discard` 收的尾)**原样留着就行**,同一条命令有空间之后
**从那儿接着做**。其余性质全留。两句「为什么失败」也有同样的毛病——老 store 那条路上用光空间的
可能是 `mkdir`、`link`、`rename` 或者 `VERSION` 的重写,而不是「打开数据库」——改成
「这条命令写不下它需要写的东西」,`-wal`/`-shm` 那句保留作为「连读都要先写」的理由。

**逐条核对过的路**(每一条都能从 `wfs_store_open()` 返回 `-ENOSPC`/`-EDQUOT`):新 store、
已经是 schema 3 的 store、老 store 在升级的每一步(`VERSION` 重写、搬库的 1~4 步、搬了一半、
迟到的持有者闸门、迁移事务、`kPragmas`)、以及最后那次 `trashing_recover()`。
**没有发现不可续的状态**:每一种都落在 `store_layout()` 那张表的某一行上,下一次 open 接着做,
既不会判 `WFS_E_STORE_DAMAGED` 也不会判 `WFS_E_STORE_BUSY`(`WFS_E_STORE_BUSY` 只在真有别的
进程开着库时出现,那是它本来的语义)。最长的那条由 `core_test` 钉住:用现成的 schema 2 夹具加
`wfs_test_after_db_move` 缝,在**搬完库、迁移还没提交**的那一刻让 open 撞上 `SQLITE_FULL`——
判据是 `-ENOSPC`,`metadata.db` 是空目录、`metadata3.db` 是普通文件、两行都在、`user_version`
还是 2xx、`VERSION` 已经是 3,而下一次 open 把迁移做完、两行一个不少。这条用例**本来就是绿的**
(判据的活前几轮已经做完),它新钉的是那句承诺所依赖的**可续性**。红的是文案那一条:
`disk_full_test.cpp:256: CHECK failed: strstr(out, "nothing that was already here was changed or removed") == NULL`。
