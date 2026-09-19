# macOS 27.0 复测:clonefile / native-root World 成本模型

2026-09-19,Mac mini M1(4P+4E,16GB),**macOS 27.0(build 26A428,xnu-13432.1.9)**,
APFS Data 卷 `/dev/disk3s5`(可用 106GB)。

对照基线是同一台机器上的 **macOS 26.6.2**,数据来自
[`REDIRECT_EXPERIMENT.md`](REDIRECT_EXPERIMENT.md) 的 §4(E4)、§5(E6a–E6e)与 §6(E5)。
方法学完全沿用当时写的两个脚本 `scripts/bench/clone_cost.sh` 与 `scripts/bench/clone_world.sh`
(本轮原样重跑),另外新增四组当时没做的实验(§9 逐文件克隆、§10 源树阻塞、§11 语义等价性、
§6.2 FSEvents 实测)。

**工作根**:`$S/nat27clone/`,全部在普通 APFS 上;**没有碰** `$S/m27/` 下的 worldfs 挂载。
**机器状态**:测量期间 load avg 1.8–4.2;QQLive 常驻 0.9–16% 单核,已计入噪声。
每个时间点至少跑 2 次(§9 跑 3 次),表里给 **best-of-N**;离群值单独标注
(Spotlight 会对新建的树建索引,这是本轮唯一可复现的离群源)。

---

## 0. 结论速览

| 问题 | 26.6.2 | 27.0 | 变化 |
|---|---|---|---|
| 目录 `clonefile()` 的单位成本 | 9.6–12.9 µs/条目 | **6.6–8.2 µs/条目** | **快 25–45%** |
| 50k 文件树 fork 延迟 | 0.4812 s | **0.3699 s** | **快 23%** |
| 单文件 `clonefile` 延迟 | 178–195 µs | **76–96 µs** | **快 ~57–61%** |
| COW 首写(unshare)惩罚 | +392 µs | **+969 µs** | **慢 2.5×(唯一的退化)** |
| 克隆里跑 agent 负载 | native 的 95–110% | **99–102%** | 更干净,仍然 = native |
| stat-walk diff(5 万) | 0.776 s | 0.647 s | 快 17% |
| `git status`(5 万,800 改动) | 0.149 s | 0.116 s | 快 22% |
| **FSEvents 能否给 O(changes) 的 diff** | 未实测(只记录为选项) | **能:800 处改动 → 800 条 file-level 路径,0 丢事件,0 MustScanSubDirs** | **新结论** |
| 跨卷 `clonefile` | EXDEV(errno 18),`st_dev` 骗人 | **完全一样** | 没变 |
| 目录 `clonefile` vs 逐文件克隆 | 未实测 | **目录整克隆快 5.5–17×;逐文件 8 线程也只到 1/6** | **新结论** |
| 克隆活的源树会不会卡写者 | 未实测 | **会:p99 从 0.13ms → 10–11ms,最坏 15.5ms,但 0 失败** | **新结论** |

**一句话**:C 方案(native-root clonefile World)在 27 上**整体变好了**,唯一变差的是
**每个被写克隆文件的一次性 COW 惩罚翻了 2.5 倍**(392µs → 969µs);
而 FSEvents 在 27 上**实测可以把 diff 做成 O(changes)**,这补上了 26.6.2 结论里 C 方案最大的一个洞。

---

## 1. 目录树 `clonefile()`

### 1.1 整目录一次调用(内核递归)

合成树:1–8KB 混合,约 200 文件/目录。best-of-N。

| 树 | 条目数 | 26.6.2 秒(µs/file) | 27.0 秒(µs/file) | Δ 时间 |
|---|---|---|---|---|
| synthetic 1k | 1 000 | 0.0097(9.7) | **0.0066(6.6)** | **−32%** |
| synthetic 10k | 10 000 | 0.1290(12.9) | **0.0720(7.2)** | **−44%** |
| synthetic 20k | 20 000 | 0.1988(9.9) | **0.1459(7.3)** | **−27%** |
| synthetic 50k | 50 000 | 0.4812(9.6) | **0.3699(7.4)** | **−23%** |
| `third_party/fmt`(`.git` 是 gitfile) | 171 | 0.0022(12.9) | **0.0013(7.6)** | **−41%** |
| forkfs repo + fmt 源码,含完整 `.git` | 516 / **686**(27 上仓库长大了) | 0.0067(13.0) | **0.0053(7.7)** | µs/条目 **−41%** |
| **`fmt` + 真 `.git`**(`git clone --no-hardlinks` 本地 submodule) | — / **214** | 未测 | **0.0016(7.5)** | 新增 |
| 50k 文件的 **git 仓库**(含 `.git`) | ~50 000 | 0.88(≈19) | **0.5739(11.5)** | **−35%** |

20k 的两组独立采样:`clone_cost.sh` 0.1660 / 0.1720,`perfile.sh` 0.1459 / 0.1495 / 0.1480;
取全局 best 0.1459。**µs/条目在 27 上稳定在 6.6–8.2,比 26.6.2 的 9.6–12.9 低一档,而且抖动更小**。

### 1.2 `cp -c -R` / `cp -R` 对照

| 操作 | 树 | 26.6.2 秒(µs/file) | 27.0 秒(µs/file) | Δ |
|---|---|---|---|---|
| `cp -c -R` | 1k | 0.2687(268.7) | **0.1572(157.2)** | −41% |
| `cp -c -R` | 10k | 1.7169(171.7) | **1.3008(130.1)** | −24% |
| `cp -c -R` | **20k** | 4.4863(224.3) | **2.5553(127.8)** | **−43%** |
| `cp -c -R` | 50k | 8.6263(172.5) | **6.3228(126.5)** | −27% |
| `cp -R`(真实复制) | 20k | 10.1107(505.5) | **4.9107(245.5)** | **−51%** |
| `clonefile` 比 `cp -c -R` 快 | 20k | 22–27× | **17.5×** | 差距缩小(两边都变快,`cp` 变得更快) |

`du -sk` 在两个系统上都**看不出块共享**:原树 / clonefile / `cp -c -R` / `cp -R` 全是 125268k。

---

## 2. 单文件 `clonefile` 延迟与目录粒度

### 2.1 延迟 vs 文件大小(20 次中位数,两轮取好的一轮)

| 文件大小 | 26.6.2 中位数 | 27.0 中位数(轮1 / 轮2) | Δ |
|---|---|---|---|
| 4 KiB | 187.6 µs | **77.5 µs**(96.4 / 77.5) | **−59%** |
| 64 KiB | 183.0 µs | **75.9 µs**(94.3 / 75.9) | **−59%** |
| 1 MiB | 178.1 µs | **76.5 µs**(90.4 / 76.5) | **−57%** |
| 64 MiB | 195.1 µs | **76.3 µs**(83.0 / 76.3) | **−61%** |
| 2 GB 单文件(第1次 / 第2次) | 0.0899 s / 0.0001 s | **0.0001 s / 0.0001 s** | 首次不再有 0.09s 的坑 |
| `cp` 真实复制 2GB | 1.7775 s | **1.1274 s** | −37% |

**结论不变且更强:单文件 clonefile 的延迟与文件大小完全无关**,27 上从 ~180µs 降到 ~76µs。

### 2.2 目录粒度(5 次中位数)

| 目录内文件数 | 26.6.2 | 27.0 | Δ | 27.0 µs/file |
|---|---|---|---|---|
| 1 | 0.250 ms | **0.123 ms** | −51% | 123.1 |
| 10 | 0.343 ms | **0.183 ms** | −47% | 18.3 |
| 100 | 2.090 ms | **0.721 ms** | −65% | 7.2 |
| 1 000 | 16.624 ms | **6.427 ms** | −61% | 6.4 |
| 10 000 | 136.431 ms | **98.492 ms** | −28% | 9.8 |

懒克隆的目录粒度代价从 2–4ms(200 文件/目录)降到 **~1.4ms**。

---

## 3. COW 首写惩罚(**唯一的退化**)

8 KiB `pwrite` + `fsync` 打进一个 64MiB 文件,10 次中位数,两轮。

| 目标 | 26.6.2 第1次 | 27.0 第1次(轮1/轮2) | Δ | 26.6.2 第2次 | 27.0 第2次 | Δ |
|---|---|---|---|---|---|---|
| 普通文件(非克隆) | 47.9 µs | **41.5 µs**(40.3 / 41.5) | −13% | 41.1 µs | **62.8 µs** | **+53%** |
| 克隆出来的文件(首写 = unshare) | 439.6 µs | **1010.6 µs**(1010.6 / 1041.2) | **+130%** | 34.3 µs | **38.9 µs** | +13% |
| **净 unshare 惩罚** | **+391.7 µs** | **+969.1 µs** | **慢 2.5×** | — | — | — |

两轮完全一致(1010.6 / 1041.2),不是噪声。**每个第一次被原地写的克隆文件现在要多付约 1ms**,
之后恢复正常。

为什么 §5 的 agent 负载里看不到它:S2 的 edit-loop 走的是 **temp 文件 + rename**,
写的是一个全新的 inode,**不触发 unshare**;S4 写的也是新建的产物文件。所以 500 次编辑并没有
付 500 × 0.97ms。**真正会吃到这 1ms 的是"原地改写一个既有文件"的路径**:
in-place 写(`O_WRONLY` 直接 pwrite)、SQLite / 数据库文件、`.git/index` 的原地更新、
大 lockfile。一个 World 里有 1000 个这样的文件,第一次写就是额外 1 秒。
这是本轮唯一一项比 26.6.2 差的指标。

---

## 4. 存储

50 000 文件树(`du -sk` 313824k ≈ 306MB)。

| 指标 | 26.6.2 | 27.0 | Δ |
|---|---|---|---|
| 原始 `du -sk` | 313824k | 313824k | = |
| 克隆刚做完 `du -sk` | 313824k | 313824k | =(`du` 数逻辑块,看不出共享) |
| **克隆本身的 `df` 可用空间消耗** | 未测 | **15396k**(50000 文件 + 501 目录 ≈ **308 B/条目**) | 新增 |
| 改动:500 文件(1%)各 append 100B | 48.8 KiB 新数据 | 48.8 KiB 新数据 | = |
| 该改动的 `df` 消耗 | 1112k | **504k / 3296k**(两次独立测量) | 同量级,`df` 噪声大 |
| 改动后克隆的 `du -sk` | 313920k(+96k) | 313920k(+96k) | = |
| 200 个未改文件逐字节 `filecmp` | 0 处不一致 | **0 处不一致** | = |
| 被改文件对应的**原文件**尺寸也变了的个数 | 0 | **0** | = |
| 共享文件的 inode | 不同 inode,共享 extent | **不同 inode,共享 extent** | = |

**新数字**:克隆一棵 5 万文件的树,物理上要付 **15MB 元数据**(~308 B/条目)。
1000 个 World = 15GB 纯目录项/inode,与 26.6.2 结论里"1000 个 World 要 1000 份目录项"一致,
现在有了量级。1% 编辑的物理增长仍然是 **0.2%–1%** 区间(`df` 的读数受系统后台写影响,
两次分别读到 504k 与 3296k;26.6.2 读到 1112k,三者同量级)。

---

## 5. 克隆里的速度(E6c 原样重跑)

同一轮里背靠背跑"clonefile 克隆根"与"`cp -R` 出来的普通 native 副本",两轮 best-of-2。
`native%` = native 副本耗时 / 克隆里耗时。

| 配置 | S2 edit-loop | S4 build-artifacts | configure | build −j8 | 增量编译 | `rm -rf build` |
|---|---|---|---|---|---|---|
| **26.6.2** clone-root | 0.1949 | 1.5217 | 0.6616 | 2.0203 | 0.4189 | 0.0356 |
| **26.6.2** native 副本 | 0.1901 | 1.4941 | 0.6281 | 1.9967 | 0.4326 | 0.0392 |
| **26.6.2 native%** | 98% | 98% | 95% | 99% | 103% | 110% |
| **27.0** clone-root | **0.1551** | **1.2373** | **0.5031** | **1.4761** | **0.3527** | **0.0288** |
| **27.0** native 副本 | **0.1554** | **1.2632** | **0.5076** | **1.4749** | **0.3498** | **0.0288** |
| **27.0 native%** | **100%** | **102%** | **101%** | **100%** | **99%** | **100%** |
| **Δ(绝对耗时)** | −20% | −19% | −24% | −27% | −16% | −19% |

fork 本身(691 条目的工作区):

| | 26.6.2 | 27.0 | Δ |
|---|---|---|---|
| `clonefile` | 0.094 s | **0.0631 s** | −33% |
| `cp -R` | 0.271 s | **0.1974 s** | −27% |

Base 完整性检查通过(base 里 0 个残留 `.o`,158 行未被改动的标记行)。
**离群值**:rep1 的 native 副本增量编译 = 1.3693s(rep2 为 0.3498s),典型的 Spotlight 索引干扰,
已用 best-of-2 剔除;其余 11 个数对的两轮差异都在 3% 以内。

→ **27 上克隆里的速度更加"就是 native 速度"**:六项 native% 全部落在 99–102%(26.6.2 是 95–110%),
而且所有绝对耗时比 26.6.2 快 16–27%(这是系统整体变快,不是克隆变快)。

---

## 6. changed-set 的成本

### 6.1 走树的办法(O(tree))

50 000 文件树,克隆后做了 500 modified / 200 added / 100 deleted。

| 方法 | 26.6.2(2 次) | 27.0(2 次) | Δ(best) |
|---|---|---|---|
| stat-walk 双树对比(`os.scandir`,比 size/mtime_ns/ino/ctime_ns) | 0.957 / 0.776 s | **0.665 / 0.647 s** | **−17%** |
| 只单边 walk 克隆树(不比对) | 0.365 / 0.367 s | **0.308 / 0.312 s** | −16% |
| `git status --porcelain`(5 万文件 git 仓库,800 改动) | 0.191 / 0.149 s | **0.1177 / 0.1160 s** | **−22%** |
| `git status --porcelain -uno` | 0.145 / 0.112 s | **0.0863 / 0.0860 s** | **−23%** |

都快了 ~20%,但**复杂度级别没变,仍然是 O(tree)**。

### 6.2 FSEvents 实测(26.6.2 只记录为选项,27.0 实测)

探针:`FSEventStreamCreate` + `kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagNoDefer`,
`latency = 0`,监听一个刚 `clonefile` 出来的 5 万文件 World 根;在上面做 **800 处改动**
(500 改 / 200 增 / 100 删),runloop 在独立线程上持续 drain。

| 指标 | 26.6.2 | 27.0 live(两次) | 27.0 replay(`sinceWhen = fork 时的 event id`) |
|---|---|---|---|
| 是否实测 | **否**(只写进文档当作选项) | **是** | **是** |
| 投递的事件条数 | — | 816 / 823 | 799 |
| **唯一路径数(应 = 800)** | — | **800 / 800** | **799**(一个文件既被改又被删,合并成一条) |
| 是否 **per-file 路径** | 文档推测"目录粒度,除非开 FileEvents" | **是,800/800 全是 file-level(`ItemIsFile`),dir-only 条目 0 个** | 798 file-level |
| `MustScanSubDirs`(要求回退全扫) | 文档假设"会出现" | **一次都没有** | **一次都没有** |
| `UserDropped` / `KernelDropped` | 文档假设"会丢事件" | **0 / 0** | **0 / 0** |
| 投递延迟 | — | **流式**:第一条比最后一处改动还早 120–136 ms 到达,最后一条在改动结束后 **+3.1 / +11.8 ms** | 建流后 **21 ms 内**放完全部历史(`HistoryDone`) |
| 事件 flag | — | `ItemCreated` / `ItemRemoved` / `ItemModified` / `ItemXattrMod` / `ItemIsFile` | 同上 + `ItemInodeMetaMod` + `HistoryDone` |

**重要的反例(方法学)**:同一个探针,如果 **消费者不及时 drain**(把改动放在 runloop 所在的线程上做),
立刻出现 `UserDropped` **和** `MustScanSubDirs`,800 处改动只收到 546 条唯一路径。
也就是说 **"会丢事件"不是 FSEvents 的固有属性,而是消费者被阻塞的症状** ——
只要 World 守护进程有一条专门的 runloop 线程,27 上 800 处改动一条不丢。

→ **`world fs diff` 在 native-root World 上可以是 O(changes)**:fork 时记下
`FSEventsGetCurrentEventId()`,之后 `FSEventStreamCreate(sinceWhen = 那个 id)` 就能把改动路径全量取回。
仍然要保留一条 `MustScanSubDirs` / `*Dropped` 的兜底全扫路径,但实测它在正常负载下不触发。

---

## 7. 跨卷约束(完全没变)

| 检查 | 26.6.2 | 27.0 | Δ |
|---|---|---|---|
| `$S`(工作根)的 `st_dev` | 16777229 | 16777232 | 只是这次启动的编号不同 |
| `~/Library/Containers` 的 `st_dev` | 同上 | **同上(16777232)** | = |
| `clonefile("/usr/bin/true", $S/…)` | errno **18** Cross-device link | **errno 18 Cross-device link** | = |
| `clonefile("/System/…/SystemVersion.plist", …)` | errno 18 | **errno 18** | = |
| `clonefile("/bin/sh", …)` | 未测 | **errno 18** | = |
| 两侧 `st_dev` 是否相等 | **相等,但照样 EXDEV** | **相等(16777232 = 16777232),照样 EXDEV** | = |
| `cp -c` 跨这条界线 | 静默退化成真实复制 | **静默退化成真实复制(133184 字节实拷)** | = |

> 工程结论原样保留:**不能用 `st_dev` 相等来预判 clonefile 能不能成功**,必须实际调用并在 `EXDEV` 上降级。

---

## 8. 这个系统上有没有新东西

| 项 | 26.6.2 | 27.0 | Δ |
|---|---|---|---|
| `man clonefile` 的 flags | `CLONE_NOFOLLOW` / `CLONE_NOOWNERCOPY` / `CLONE_ACL`(文档引用过前两个) | `CLONE_NOFOLLOW`、`CLONE_NOOWNERCOPY`、`CLONE_ACL`、`CLONE_NOFOLLOW_ANY`、`CLONE_RESOLVE_BENEATH` | **没有 27 专属新 flag**;man 页页脚仍是 `June 3, 2021`,26.6.2 的原页没有存档,无法证明 `LIMITATIONS` 段是新加的 |
| `man clonefile` 的 **LIMITATIONS** | 当时未引用 | **"Cloning directories with these functions is strongly discouraged. Use copyfile(3) to clone directories instead."**;DESCRIPTION 里再说一遍:"If src names a directory, the directory hierarchy is cloned as if each item was cloned individually. However, the use of clonefile(2) to clone directory hierarchies is strongly discouraged." | 见 §9 |
| `man clonefile` 的原子性承诺 | 未引用 | "The clonefile(), clonefileat() and fclonefileat() functions are expected to be atomic i.e. the system call will result all new objects being created successfully or no new objects will be created." | 这正是**目录整克隆唯一不可替代的性质** |
| `man copyfile` 的递归克隆 | 未引用 | "A recursive clone operation invokes copyfile() with COPYFILE_CLONE on every entry found in the source file-system object. Because copyfile() does not allow the cloning of directories, a recursive clone will instead **copy** any directory it finds (while cloning its contents)." + `COPYFILE_CLONE` 蕴含 `COPYFILE_EXCL`,目标必须不存在 | Apple 推荐的替代品**在实现上就是逐文件克隆**,所以慢 15×(§9) |
| `sysctl vfs.generic.apfs.*` | 未记录 | 共 7 个键:`rage_purgeable_extents`、`rage_inode`、`unwritten_freeze_threshold`、`proc_free_blocks_threshold`、`rangelist_verification_mode`、`allocated`、`kern.apfsprebootuuid` | **没有一个与克隆有关** |
| `cp -c` 单文件 | 可用 | **1GiB 文件 0.0246s,`df` 可用空间变化 −840k(即 0,噪声)** | 仍然真克隆 |
| `cp`(无 `-c`)单文件 | — | 1GiB **0.4309s**,`df` 消耗 **1048584k** | 真实复制,对照组 |
| **`ditto`** | 未测 | **默认就克隆**:1GiB **0.0325s**,`df` 消耗 **8k**;并且有显式的 `--clone` / `--noclone` 开关 | 新增。`ditto` 是命令行里唯一"默认克隆且能显式控制"的工具 |

---

## 9. 目录整克隆 vs 逐文件克隆(新增)

Apple 明确不推荐用 `clonefile()` 克隆目录树,所以把四条路线放在同一棵树上量。
`clonetool`(C,`$S/nat27clone/src/clonetool.c`)实现:
`dirclone` = 一次 `clonefile(dir)`;`perfile` = 单线程 `readdir` + 每项一次 `clonefileat(CLONE_NOFOLLOW)`
+ 每个目录一次 `mkdirat` 与 `fcopyfile(COPYFILE_METADATA)`;`perfileN` = 同样的算法,N 个工作线程,
一个目录一个任务(动态队列);`copyfilerec` = `copyfile(src, dst, NULL, COPYFILE_CLONE|COPYFILE_RECURSIVE)`。
3 次取 best。

### 9.1 20 000 文件(201 目录,125268k)

| 方法 | 26.6.2 | 27.0 best(3 次) | µs/file | 相对目录整克隆 |
|---|---|---|---|---|
| **`clonefile(dir)` 整目录** | 0.1988 s | **0.1459 s** | **7.3** | **1.0×** |
| 逐文件 `clonefileat`,单线程 | 未测 | 2.0994 s | 105.0 | **慢 14.4×** |
| 逐文件,**4 线程** | 未测 | **0.8093 s** | 40.5 | 慢 5.5× |
| 逐文件,**8 线程** | 未测 | 0.8792 s | 44.0 | 慢 6.0× |
| 逐文件,16 线程 | 未测 | 0.8675 s | 43.4 | 慢 5.9× |
| `copyfile(3)` `CLONE\|RECURSIVE` | 未测 | 2.2802 s | 114.0 | 慢 15.6× |
| `cp -c -R` | 4.4863 s | 2.5553 s | 127.8 | 慢 17.5× |

### 9.2 50 000 文件(501 目录,313824k)

| 方法 | 26.6.2 | 27.0 best(3 次) | µs/file | 相对目录整克隆 |
|---|---|---|---|---|
| **`clonefile(dir)` 整目录** | 0.4812 s | **0.3699 s** | **7.4** | **1.0×** |
| 逐文件,单线程 | 未测 | 5.1980 s | 104.0 | **慢 14.1×** |
| 逐文件,**4 线程** | 未测 | **2.0308 s** | 40.6 | 慢 5.5× |
| 逐文件,8 线程 | 未测 | 2.4425 s | 48.9 | 慢 6.6× |
| 逐文件,16 线程 | 未测 | 2.5041 s | 50.1 | 慢 6.8× |
| `copyfile(3)` `CLONE\|RECURSIVE` | 未测 | 5.7005 s | 114.0 | 慢 15.4× |
| `cp -c -R` | 8.6263 s | 6.3228 s | 126.5 | 慢 17.1× |

### 9.3 `fmt` + 真 `.git`(214 条目)

| 方法 | 27.0 best(3 次) | µs/条目 |
|---|---|---|
| `clonefile(dir)` | **0.0016 s** | 7.5 |
| 逐文件单线程 | 0.0259 s | 121.0 |
| 逐文件 8 线程 | 0.0107 s | 50.0 |
| `copyfile` `CLONE\|RECURSIVE` | 0.0328 s | 153.3 |
| `cp -c -R` | 0.0509 s | 237.7 |

**读法**:
- **逐文件克隆的地板是 ~104 µs/file(单线程)**,与 §2.1 的单文件 `clonefile` 延迟(~76µs)+ `readdir`/
  `mkdir` 开销吻合。**目录整克隆是 7.3 µs/file,比它快 14×** —— 内核在一次调用里批量建 inode,
  省掉了每个文件一次 syscall 往返和一次独立事务。
- **并行只能拿回 2.6×**,而且 **4 线程就到顶**(8/16 线程反而略差):瓶颈是 APFS 的元数据事务,不是 CPU。
- Apple 推荐的 `copyfile(3)` 递归克隆(= `cp -c -R` 的实现)是 **15×**,比自己写的单线程逐文件循环还慢一点
  (它每个文件多走一遍 `fts` + `copyfile` 的 state 机)。
- **所以"按 Apple 说的做"要为一次 5 万文件的 fork 付 5.7 秒,而不是 0.37 秒。**

---

## 10. 源树阻塞测试:能不能对**活的** World 做目录整克隆(新增)

一个写者进程以 **1 ms 一次**的节奏向 **源树里的 100 个文件** append 4KB,记录每次 `write(2)` 的延迟;
在它跑到第 2.5 秒时开始克隆整棵 5 万文件的源树。两轮,完全可复现。

| 场景 | 克隆窗口 | 窗口内 p50 | 窗口内 **p99** | 窗口内 **max** | >5ms 的 op 数 | 写失败 |
|---|---|---|---|---|---|---|
| **基线**(没有任何克隆) | 2.01 s | 40 / 55 µs | **105 / 98 µs** | 127 / 974 µs | **0 / 0** | **0** |
| **`clonefile(dir)` 整目录** | 0.475 / 0.483 s | 11 / 11 µs | **11129 / 10407 µs** | **15482 / 14293 µs** | **8 / 9** | **0** |
| 逐文件克隆,8 线程 | 2.44 / 2.52 s | 24 / 28 µs | **2327 / 2875 µs** | 8743 / 9125 µs | 10 / 13 | **0** |
| `copyfile` `CLONE\|RECURSIVE` | 5.80 / 5.98 s | 8 / 7 µs | **78 / 111 µs** | 12431 / 13005 µs | 10 / 10 | **0** |

(26.6.2 未做此实验,整张表都是新增。)

**读法**:
- **目录整克隆确实会卡住源树里的写者**:窗口内 p99 从基线的 ~0.1ms 跳到 **10–11ms**,最坏 **15.5ms**,
  0.48 秒的窗口里有 8–9 次 op 被挡住 5ms 以上;写者在窗口里只完成了 388–395 次(预期 ~475),
  **丢了约 17% 的节拍**。
- **但它不是"整个 0.48 秒全程锁死"**:p50 反而降到 11µs,绝大多数写照常通过,阻塞是若干次 10–15ms 的尖峰。
  man 页的措辞会让人以为整棵源树被锁住整个克隆时长,**实测不是**。
- **也没有任何写失败**(errno 全 0),语义上写者看不到错误,只看到延迟。
- 逐文件 8 线程把最坏尖峰从 15.5ms 压到 **9ms**、p99 从 11ms 压到 **2.5ms**,代价是窗口长 5 倍
  (2.5s vs 0.48s)——**总的"被打扰时间"反而更大**。
- `copyfile` 递归几乎不打扰写者(p99 78–111µs),但要 **6 秒**。
- **另一条硬差别**:`clonefile(dir)` 是**原子**的(man 页承诺),逐文件循环不是 ——
  对一棵正在被写的源树,逐文件克隆拿到的是**撕裂的快照**(不同文件来自不同时刻),
  而目录整克隆拿到的是一个一致的点。对 "fork 一个正在跑 agent 的 World" 这件事,这是决定性的。

---

## 11. 逐文件克隆与目录整克隆的语义等价性(新增)

构造一棵"什么怪东西都有"的树:普通文件、4MiB 大文件、64MiB **稀疏文件**、
相对 symlink / 绝对 symlink / **悬空 symlink**、同目录 hardlink、跨目录 hardlink、
3000 字节的大 xattr、目录上的 xattr、`0400`/`0751`/`0700` 等模式、空目录、**FIFO**、
一个 2020 年的 mtime。逐项对比 kind / mode / uid / gid / size / sha256 / xattr / symlink 目标 / mtime。

| 检查 | `clonefile(dir)` | 逐文件(单线程 / 8 线程) | `copyfile CLONE\|RECURSIVE` |
|---|---|---|---|
| 条目数(源 55) | **55** | **55**(补了 FIFO 的 `mkfifoat` 回退后) | **50 —— 失败** |
| 结果 | 成功 | 成功 | **`ERRNO 45 Operation not supported`,在 FIFO 上中止,留下半棵树** |
| 文件内容(sha256) | 全同 | 全同 | (未跑完) |
| mode / uid / gid | 全同 | 全同 | — |
| xattr(含 3000B 的大 xattr、目录 xattr) | 全同 | 全同 | — |
| symlink(相对/绝对/悬空) | 保持为 symlink | 保持为 symlink | — |
| 稀疏文件 | 保持 | 保持 | — |
| FIFO | **内核自己处理了** | `clonefileat` 返回失败,**必须自己 `mkfifoat` 兜底** | **直接失败** |
| **tree 内的 hardlink** | **断开(nlink 1,变成两个独立克隆)** | **断开** | — |
| 文件 mtime | 保留 | 保留 | — |
| 目录 mtime | **有子项的目录不保留**(`a`、`a/b`、`a/b/c` 取了克隆时刻;空目录保留) | **保留**(靠 `fcopyfile(COPYFILE_METADATA)`) | — |

**读法**:
- **`clonefile(dir)` 也不保留 hardlink** —— 这一点上两种方法没有差别,
  都需要上层自己做 (dev,ino) 去重才能保住 `node_modules` 里 pnpm/uv 的 hardlink store。
- 逐文件克隆要**额外实现**:FIFO/socket/设备节点的兜底、目录元数据的复制。写全了之后
  与目录整克隆**逐字节等价**(唯一差别是目录 mtime,逐文件版本反而更忠实)。
- **Apple 推荐的 `copyfile(3)` 递归克隆在一个 FIFO 上就会中止并留下半棵树**,
  它既不原子也不健壮;真要走逐文件路线,得自己写,不能直接用 `copyfile`。

### 11.1 结论:目录整克隆 vs 逐文件克隆,选哪个

| 维度 | `clonefile(dir)` | 逐文件(自己写,4–8 线程) |
|---|---|---|
| 5 万文件 fork 延迟 | **0.37 s** | 2.0–2.4 s(**慢 5.5×**) |
| 原子性 | **内核承诺原子** | 无,活树上得到撕裂快照 |
| 源树写者的 p99 | 10–11 ms(最坏 15.5ms) | **2.3–2.9 ms**(最坏 9ms) |
| 源树写者被打扰的总时长 | 0.48 s 窗口 | 2.5 s 窗口(**更长**) |
| 写失败 | 0 | 0 |
| 特殊文件 / 元数据 | 内核全包 | 要自己补 FIFO、目录元数据 |
| hardlink | 断开 | 断开(都要上层去重) |
| Apple 的态度 | **"strongly discouraged"** | `copyfile(3)`(但它在 FIFO 上就挂) |

**建议:主路径用 `clonefile(dir)`,把逐文件循环留作降级路径。**
理由:(1) 快 5.5×,而 fork 延迟是这个架构唯一的硬伤;(2) 原子快照是"从活 World fork"的正确性前提,
逐文件循环给不了;(3) 实测的代价——源树写者 15ms 的尖峰、0 失败——对一个 fork 动作完全可以接受,
远没有 man 页措辞暗示的那么严重。
**但要做三件事**:(a) 在 fork 前对目标 World **静默写者**(暂停 agent / `fsync` 一次),
把 15ms 尖峰变成无人观察;(b) 保留逐文件实现,用于 `clonefile` 返回 `EXDEV` / `ENOTSUP` 的卷,
以及将来 Apple 真的把目录克隆去掉的情况;(c) 两条路径都要自己做 hardlink 的 (dev,ino) 去重。

---

## 12. 判断:C 方案在 27 上变好了还是变坏了

**变好了,而且是在最关键的两个维度上。**

1. **fork 成本全线下降 23–44%**,µs/条目从 9.6–12.9 降到 6.6–8.2。
   1k 文件 **0.0066s**(26.6.2 是 0.0097s,现在真的卡进 10ms 了)、10k **0.072s**、50k **0.370s**、
   5 万文件的 git 仓库 **0.574s**。arch.md 的 `fork < 10ms` 仍然只有 ≤1000 文件的仓库能满足,
   但"10 万文件的仓库 1 秒内 fork 完"这条现在有 ~40% 的余量。
2. **克隆里的速度从"95–110%"收紧到"99–102%"**,六项负载没有一项能测出克隆的代价。
3. **FSEvents 实测可用,diff 可以是 O(changes)**:开 `kFSEventStreamCreateFlagFileEvents`,
   800 处改动 → 800 条 file-level 路径,**0 丢事件、0 `MustScanSubDirs`**,
   live 模式下最后一条事件在改动结束后 3–12ms 到达,replay 模式用 fork 时记下的 event id
   能在 21ms 内放完全部历史。26.6.2 的文档把 FSEvents 写成"目录粒度 + 会丢事件"的将就方案,
   **这个判断在 27 上被实测推翻了**(前提是消费者有独立的 runloop 线程去 drain;
   把 drain 堵住就立刻出现 `UserDropped` + `MustScanSubDirs`)。
   这补上了 26.6.2 结论里 C 方案最大的一个洞 —— arch.md §25 的 O(changes) 要求**不再是 C 的死穴**。
4. **唯一变差的是 COW 首写:+392µs → +969µs(2.5×)**。影响面是"原地改写既有文件"的路径;
   新建文件 + rename(agent 的 apply_patch、编译产物)完全不吃这个代价,所以 §5 的 native% 没被它拉下来。
   要注意的是 `.git/index`、SQLite、大 lockfile 这类原地更新的大文件,每个第一次写多付 1ms。
5. **没变的**:跨卷仍然 `EXDEV`,`st_dev` 相等仍然不能作判据,`cp -c` 仍然静默退化成真实复制,
   `du` 仍然看不出块共享,COW 隔离仍然 0 污染。

**新增的两条工程结论**(§9/§10/§11):Apple 在 man 页里"强烈不建议"用 `clonefile` 克隆目录,
但实测目录整克隆比一切替代方案快 **5.5–17×**,而它的真实代价——源树写者 p99 10–11ms、最坏 15.5ms、
**0 失败**——是可以接受的;Apple 推荐的 `copyfile(3)` 递归克隆不但慢 15×,还会在一个 FIFO 上中止
并留下半棵树。**主路径继续用 `clonefile(dir)`,逐文件 4 线程版本作为 EXDEV / ENOTSUP 的降级路径。**

---

## 附:脚本与原始输出

| 内容 | 脚本 |
|---|---|
| §1 §2(2GB) §4 §6.1 §7 | `scripts/bench/clone_cost.sh`(26.6.2 原样重跑) |
| §1(真 repo) §5 §6.1(git) | `scripts/bench/clone_world.sh`(26.6.2 原样重跑) |
| §2.1 §2.2 §3 | `$S/nat27clone/src/probes.py`(两轮) |
| §9 §10 §11 | `$S/nat27clone/src/{clonetool.c,perfile.sh,stall.sh,rich.sh,correctness.py,writer.c}` |
| §6.2 | `$S/nat27clone/src/fsevents_probe.py` |
| §4 的 df-before-clone | `$S/nat27clone/src/storage.sh` |
| §7 §8 | `$S/nat27clone/src/misc.sh` |

原始日志在 `$S/nat27clone/out/`(本次会话的 scratchpad,不入库)。
