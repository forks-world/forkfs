# macOS 27.0 实测:FSKit passthrough 复测报告

2026-09-19,Mac mini M1(4P+4E),**macOS 27.0(build 26A428,xnu-13432.1.9)**,
对照基线是同一台机器上的 **macOS 26.6.2**(仓库 commit `78e55a7`,数据见
[`TASKS.md`](TASKS.md) 与 [`PERF_STUDY_RT_PARALLEL.md`](PERF_STUDY_RT_PARALLEL.md))。

**被测对象与 26.6.2 完全是同一份二进制/同一套 API**:仍然用 26.5 SDK 编译,仍然实现
`FSVolumeOperations` 等已被 27 标为 deprecated 的协议。**macOS 27 SDK(新的 Handler API)尚未安装**,
所以本轮只回答"同一份代码在新系统上跑得怎么样",不回答"用新 API 重写能有多快"。

挂载点:`$S/m27/base` → `$S/m27/mnt1`(10k 文件树 + `hello.txt` / `link` / `src/a.c`)。
native 侧目录一律在 `$S/nat27/` 下(不在 backing 里)。
机器状态:测量期间系统空闲 80–83%,load avg 1.8–2.9;QQLive 常驻约 16–18% 单核,已计入噪声。
所有微基准 n=20000(写类 n=2000),3 轮取最小值;表里同时给出 26.6.2 的对应数字。

---

## 0. 结论速览

| 问题 | 26.6.2 | 27.0 | 变化 |
|---|---|---|---|
| 单次 XPC 往返(lookup-miss) | 68.1µs | **73.6µs** | **慢 8%** |
| Apple msdos 同一探针(地板对标) | 72.6µs | **78.0µs** | 慢 7%(同向,证明是系统变慢不是我们变慢) |
| create+unlink 的往返次数 | 16.06 | **16.03** | **完全没变** |
| lookup 后必补 getattr | 是(7 次/轮) | **仍然是(7.025 次/轮)** | 没变 |
| `com.apple.provenance` getxattr 探测 | 2 次/轮 | **仍然 2 次/轮** | 没变 |
| `-o` 选项能否到达扩展 | 否(`taskOptions` 恒空) | **仍然否** | 没变 |
| 单 mount 是否串行 | 是(一条 serial 队列) | **仍然是** | 没变 |
| readdir 是否返回 `.` / `..` | 内核合成 | **不再合成,我们也没 pack → 一个都没有** | **回归** |
| 元数据写随 mount 存活时间劣化 | 未观察到 | **有,最坏 3–12×** | **新问题** |

**验收门槛(`TASKS.md`):create+unlink 往返 ≤ 6 次 且 单次往返 ≤ 40µs。**
实测 **16.03 次 / 73.6µs**,**两条都不满足,而且都比 26.6.2 更差**。

---

## 1. 正确性(全部通过,但发现一个 readdir 回归)

| 检查 | 结果 |
|---|---|
| `scripts/smoke.sh`(read/readdir/stat/write/append/truncate/mkdir/rename/unlink/rmdir/symlink/hardlink/chmod/xattr/mmap/fsync) | **ALL OK** |
| readdir 完整性 100 / 1000 / 5000 项(listdir / scandir / os.walk / scandir+lstat 四路与 backing 逐名比对) | **全部一致,无漏无重** |
| `ls -fa` 的 dot 项 | **恰好一个 `.` 一个 `..`**(与 26.6.2 相同) |
| 经挂载点 clang 编译的可执行文件 exec | ok(`v1` → 改源码 → `v2` → `cp` 后仍可执行) |
| 经挂载点编译的 dylib `dlopen` + 调函数 | ok(返回 7) |
| `xattr -l` 经挂载点看新产物 | **只有 `com.apple.provenance`,没有 `com.apple.quarantine`** → 隔离隐藏仍然有效,没有 AMFI 挂死风险 |
| `git init/add/commit` ×2 + `git status` + `git fsck` | ok(2 个 commit,工作区干净,fsck 无输出) |
| hardlink 兄弟 unlink(`mkdir d; echo x > d/f; ln d/f d/f.lnk; rm d/f; rm d/f.lnk`) | ok(`cat d/f.lnk` 仍是 `x`,两次 rm 都成功,`rmdir` 成功) |

### 1.1 回归:raw `readdir(3)` / `getattrlistbulk` 不再返回 `.` 和 `..`

26.6.2 的修复(`TASKS.md`「晚间修正」)让 core 在 `readdir_trampoline` 里跳过 dot/dotdot,
理由是"内核/FSKit 自己会合成 dot 项"。**在 27 上内核不再合成**:

| 50 个文件的目录 | raw `readdir(3)` 条数 | 其中 dot 项 |
|---|---|---|
| native APFS | 52 | `.` `..` |
| **worldfs** | **50** | **无** |
| msdos(Apple FSKit) | 102(含 50 个 `._`) | `.` `..` |

`getattrlistbulk` 同样:worldfs 返回 50 条 0 个 dot。1/2/5/10/20/30/40/50/100 项的目录全都如此,
不是缓冲区边界问题。`ls -fa` 依然正确(BSD `ls` 自己合成 dot 行),`find` / `rm -rf` / `git` /
`tar` / `cp -R` 实测都不受影响,所以影响面有限;但这是 **POSIX 语义缺口**,而且 Apple 自己的
msdos 模块是自己 pack dot 项的 —— 说明 27 上正确做法就是模块自己 pack。
**修复位置在 `core` 的 `readdir_trampoline`(本轮只测不改)。**

### 1.2 附带观察:backing 侧带外删除目录会留下陈旧节点

在挂载存活期间从 backing 侧 `rm -rf base/X`,之后经挂载点 `mkdir -p mnt1/X` 会**静默成功**
(挂载点上 `ls -ld` 还能看到旧 inode 的 link count 和 mtime),随后在里面建任何文件都 `ENOENT`。
这是 passthrough 的 ino→path 表没有失效通知导致的,**不是 27 引入的**,但会让"native 侧直接写 backing"
这类 harness(例如 `realwork.sh`)在重名目录上踩坑。规避办法:带外改动只用全新名字,或改动后重挂。

同一个机制让 **`scripts/smoke.sh` 在同一个活挂载上不能连跑两次**:它结尾用 `rm -f "$B/smoke.txt" "$B/w.txt"`
从 backing 侧删文件,下一次 `echo > "$M/w.txt"` 就会拿到 ENOENT(而 `ls -l $M/w.txt` 还能看到陈旧属性)。
修复办法:把结尾的清理改成经挂载点删除(`rm -f "$M/smoke.txt" "$M/w.txt"`)。本轮是先在 backing 侧
重建同名文件、再经挂载点删掉,来把节点表恢复干净的。

---

## 2. 单次往返的地板

### 表 2.1 单操作延迟(µs/op,n=20000,3 轮最小值)

| 操作 | native 27.0 | **worldfs 27.0** | msdos 27.0 | worldfs 26.6.2 | msdos 26.6.2 | native% |
|---|---|---|---|---|---|---|
| `lstat` 不存在、**每次换名** → 1 次 RT | 1.84 | **73.57** | 77.96 | 68.1 | 72.6 | **2.5%** |
| `getxattr("user.nope")` 缺失 → 1 次 RT | 9.30 | **93.71** | 35.74 | 83.6 | 33.2 | 9.9% |
| `lstat` 不存在、**同一个名字** | 0.73 | **0.78** | 0.81 | 0.86 | 0.56 | 94% |
| `lstat` 已存在文件(属性缓存命中) | 0.97 | **0.91** | 0.94 | 0.94 | 0.72 | 107% |
| `open`+`close`(`openCloseInhibited=YES`) | 11.28 | **10.68** | 10.64 | 9.3 | 7.3 | 106% |
| `lstat` 5000 个不同文件(真冷,每轮全新名字) | 1.91 | **122.53** | — | 108.9 | — | 1.6% |
| `pread` 4K(fd 已开,页缓存命中) | 0.48 | **0.46** | — | 0.6→0.8 | — | 104% |
| `open`+`read4K`+`close` | 12.24 | **11.44** | — | 12.0→11.1 | — | 107% |

**要点:**

- **RT 地板从 68.1µs 涨到 73.6µs(+8%)。** 这不是我们的锅:Apple 自己的 msdos 模块在同一台机器上
  做同一件事(在目录里查一个不存在的名字、回 ENOENT),也从 72.6µs 涨到 78.0µs(+7%)。
  **我们仍然比 Apple 自己的模块快 6%。**
- **内核的否定名字缓存和属性缓存仍然在工作**,而且和 26.6.2 一样好:同名 miss 0.78µs、
  已存在文件 0.91µs,都和 native 持平(94% / 107%),一次 XPC 都不发。
  **27 没有引入"额外的"缓存,也没有丢掉原有的缓存。**
- `vfs.generic.lifs.read_meta_cache_hit` / `write_meta_cache_hit` 在本轮测试后分别是
  **1,770,397 / 58,041**,计数器是活的 —— 内核侧确实有一层元数据缓存在命中,
  但它挡不住"每次换名字"的冷 lookup。
- 冷 lstat 从 108.9µs 涨到 122.5µs,比单次 RT 贵,是因为新目录的中间路径分量也要各查一次。

### 表 2.2 readdir 的固定成本与每项成本(`opendir`+`getdirentries`+`closedir`,n=500)

| 目录项数 | native | **worldfs 27.0** | msdos 27.0 | worldfs 26.6.2 | msdos 26.6.2 |
|---|---|---|---|---|---|
| 0 项 | 15.1 | **201.0** | 176.3 | 176.6 | 161.3 |
| 50 项 | 32.8 | **264.5** | 365.4 *(返回 102 项)* | 229.3 | 320.4 |
| 500 项 | 183.0 | **1138.3** | 2784.2 *(返回 1002 项)* | 1288.5 | 2649.0 |
| **固定开销(截距)** | ~15 | **~200** | ~176 | ~177 | ~161 |
| **每项边际成本** | 0.34 | **1.88** | 2.62 | 2.35 | 2.45 |

固定开销涨了(177→200µs,和 RT 涨价一致),**但每项成本反而降了**(2.35→1.88µs/项),
所以 500 项的大目录比 26.6.2 **快 12%**。msdos 仍然因为 FAT 存不下 xattr 而给每个文件
造一个 AppleDouble `._`,条目数翻倍。

### 2.3 msdos 校准的方法

`hdiutil create -size 256m -fs "MS-DOS FAT32"` 造的镜像,`hdiutil attach` 后
`mount` 行确认带 `fskit` 标记(即走 FSKit 模块而不是老的 msdos kext)。测完已 `hdiutil detach` 并删除镜像。

---

## 3. 每个操作发多少次往返

用 `log stream --level debug --predicate 'subsystem == "world.forks.fs"'` 抓扩展的 `op ...` 行
(必须写在脚本文件里执行,内联会报 "too many arguments")。

### 表 3.1 一轮 create+unlink(n=200)

| op | 27.0 次数/轮 | 26.6.2 次数/轮 |
|---|---|---|
| `getattr` | **7.025** | 7.06 |
| `lookup` | **3.00** | 3.00 |
| `getxattr`(全部是 `com.apple.provenance`) | **2.00** | 2.00 |
| `create` | 1.00 | 1.00 |
| `remove` | 1.00 | 1.00 |
| `reclaim` | 1.00 | 1.00 |
| `sync` | 1.00 | 1.00 |
| **合计** | **16.025** | **16.06** |

**逐条回答任务书的问题:**

- **mutation 之后的 getattr 刷新:仍然存在,一次没少**(7.025 次/轮)。`lookupItemNamed:` 的回复里
  依旧不带属性,内核每次 lookup 后必补一次 `getattr` —— 这正是 27 的新 `FSLookupItemResult`
  (带 `itemAttributes` 属性)想解决的问题,但**要 27 SDK 重写才能用上**。
- **`com.apple.provenance` 的 getxattr 探测:仍然每轮 2 次**,一次没少。
- 实测 1036.5µs ÷ 16.025 = **64.7µs/RT**,与表 2.1 的 73.6µs 同一量级(create/remove 这类
  handler 里 backing 系统调用更便宜,所以略低于 lookup 探针)。26.6.2 是 995 ÷ 16.06 = 61.9µs。

### 表 3.2 其它典型动作

| 动作 | 27.0 每轮/每文件的 op 数 | 明细 |
|---|---|---|
| tmp+fsync+rename 编辑一次(n=100) | **25.21** | getattr 12.08、lookup 4.06、getxattr 3.02、sync 2.01、write/reclaim/create 各 1.01、rename 1.00 |
| `tar xf` 20 个 4KB 文件 | **45.75 / 文件** | getxattr 13.50、getattr 12.45、setattr 5.20、lookup 4.25、listxattr 2.10、write/sync/create 各 2.05、remove/reclaim 各 1.05 |
| `ls -l` 一个 50 项目录(已预热) | **51 次总计** | `readdir` 1 次 + `listxattr` 50 次(属性由 readdir 打包带回,`ls` 的 `@` 标记逼出每文件一次 listxattr) |
| 只做 lookup-miss | 1.10 | 纯 `lookup` |

`tar xf` 的 45.75 次/文件 × 74µs ≈ 3.4ms/文件,实测 83.7ms ÷ 20 = **4.2ms/文件**,吻合。

### 3.3 `-o` 挂载选项仍然到不了扩展

`mount -F -t worldfs -o world=1,foo=bar <base> <mp>` 成功挂载,扩展日志:

```
load .../m27/b2 writable=0 opts=(
)
mount opts=(
)
```

`FSTaskOptions.taskOptions` **仍然是空数组**,`world=` / `store=` / `openclose=` 一个都收不到
(扩展因此走默认分支,给 b2 新建了一个 World W12)。**26.6.2 的这个 bug 在 27 上原样存在。**

### 3.4 新增:FSKit 每次 mutation 会打一条 error 级日志

`-[FSVolumeConnector getStandardItemAttributesForItem:replyHandler:]_block_invoke:reply:error:70`
(70 = `ESTALE`,来自 core 在 `remove` 之后 `path_of` 找不到链路)——
实测**每一轮 create+unlink 恰好 1 条**,lookup-miss 不产生。45 分钟的测试产生了 15 万条以上。
error 级 `os_log` 会落盘,属于纯开销,而且会淹没系统日志。26.6.2 的文档里没有这条记录。

---

## 4. 写路径

### 表 4.1 元数据写微基准(µs/op,3 轮最小值)

| 操作 | native 27.0 | **worldfs 27.0** | native% | worldfs 26.6.2 | msdos 27.0 |
|---|---|---|---|---|---|
| `open(O_CREAT)`+`close` | 45.09 | **561.79** | 8.0% | 532.3 | 2552 |
| `unlink` | 30.96 | **475.88** | 6.5% | 442.4 | 1957 |
| create+close+unlink(一轮) | 68.05 | **1036.52** | 6.6% | 995 | — |
| create+write4K+close+unlink | 97.37 | **1259.36** | 7.7% | 304.9 *(26.6.2 用的是 `microbench.py`,口径不同)* | — |
| tmp 写+fsync+rename 覆盖 | 160.60 | **2118.02** | 7.6% | — | — |

### 表 4.2 writebench 分阶段(µs/op,n=300,批式:先建完 300 个再统一写/关/截断/fsync/删)

| 阶段 | native | **worldfs** | native% |
|---|---|---|---|
| create 新文件 | 45.6 | **1242.2** | 3.7% |
| write 4K | 10.0 | **9.1** | 110% |
| close | 10.2 | **418.1** | 2.4% |
| open `O_TRUNC` | 34.4 | **832.0** | 4.1% |
| fsync | 0.37 | **318.8** | 0.1% |
| unlink | 36.8 | **1737.4** | 2.1% |

**这张表有一个反常结论,必须写清楚:整套 6 个阶段合计 4557µs/文件,但 trace 显示总共只有
22.04 次 XPC(≈1630µs)。差出来的 ~2900µs/文件不是往返,是内核侧的成本。**
对照:同样 6 个动作但**逐文件串行**做(create→write→close→unlink 一个个来),
只要 1259µs/文件。也就是说 **"同时开着几百个文件"这种形态会让 lifs 每个文件额外付约 3ms**,
与 n 无关(n=50 / 100 / 200 / 400 / 800 结果一致,不是 O(n²))。
最可能的原因是 lifs 的 per-vnode 写状态(`vfs.generic.lifs.max_write_blockmap_size` 那套 blockmap)
在大量文件同时打开时的建立/回收成本。26.6.2 没有这张表,**无法判断是不是 27 新增的**。

### 表 4.3 `tar xf` 20 个 4KB 文件(3 次最小值)

| | native | worldfs | native% |
|---|---|---|---|
| wall | 11.2ms | **83.7ms** | **13%** |

### 表 4.4 per-op 微基准总表(µs/op)

| op | native 27.0 | **worldfs 27.0** | native% | worldfs 26.6.2 | 26.6.2 native% |
|---|---|---|---|---|---|
| lstat same file(已缓存) | 0.97 | **0.91** | **107%** | 1.8 | 83% |
| lstat 5000 distinct(冷) | 1.91 | **122.53** | 1.6% | 108.9 | 3% |
| open+close | 11.28 | **10.68** | **106%** | 10.3 | 108% |
| pread 4K(fd 已开) | 0.48 | **0.46** | **104%** | 0.8 | 75% |
| open+read4K+close | 12.24 | **11.44** | **107%** | 11.1 | 108% |
| listdir(50 项) | 32.8 | **264.5** | 12% | 261.8 | 13% |
| scandir+stat(50 项) | 87.8 | **517.2** | 17% | 359.7 | 32% |
| create+write4K+close+unlink | 97.4 | **1259.4** | 7.7% | 304.9 | 12% |

(`scandir+stat` 三轮波动 517–906µs,取最小值;26.6.2 的 `microbench.py` 与本轮的 C 版本
在预热和 dot 项处理上口径不完全一致,这两行只能看趋势。)

---

## 5. 并行度

### 表 5.1 单 mount lookup-miss(每进程 8000 次,新挂载点,3 轮最好值)

| 进程数 | 27.0 延迟 | **27.0 聚合** | 26.6.2 聚合 | 27.0 native 聚合 |
|---|---|---|---|---|
| 1 | 74.6µs | **12,677 ops/s** | 14,321 | 179,338 |
| 2 | 84.1µs | **22,600 ops/s** | 24,093 | — |
| 4 | 114.0µs | **33,740 ops/s** | 38,654 | — |
| 8 | 227.9µs | **34,262 ops/s** | 39,358 | 296,841 |

### 表 5.2 单 mount create+unlink(每进程 1500 次)

| 进程数 | 27.0 延迟 | **27.0 聚合** | 26.6.2 聚合 | 27.0 native 聚合 |
|---|---|---|---|---|
| 1 | 1046µs | **938 ops/s** | 981 | 11,011 |
| 2 | 1731µs | **1,142 ops/s** | 1,159 | — |
| 4 | 3308µs | **1,193 ops/s** | 1,197 | — |
| 8 | 见 §5.4 | **402–429 ops/s**(劣化时) | 1,249 | 31,219 |

形状和 26.6.2 完全一样:**4 个进程撞顶,再加进程只是把延迟按比例摊长**;写更彻底,
1→4 进程只涨 27%。所有数字比 26.6.2 低 10–13%,与 RT 涨价 8% 一致。

### 表 5.3 8 个 mount × 8 个 1000 文件 base 副本(每进程一个 mount)

| 指标 | 27.0 | 26.6.2 |
|---|---|---|
| 扩展进程数 | **每挂一个多一个**(挂 8 个 → 11 个进程) | 同 |
| lookup 聚合 | **46,543 ops/s**(单 mount 8 进程的 1.36×) | 51,693(1.31×) |
| create+unlink 聚合 | **3,461 ops/s** | 3,821 |
| 每个扩展进程 CPU(lookup) | ~26% × 8 = 208% | ~36% × 8 = 288% |
| **`fskitd` CPU(lookup)** | **149%** | 225.9% |
| `fskitd` CPU(create+unlink) | 98.6% | — |
| 系统空闲 | 80% | 14.7% |

**结论没变:一个 World 一个 mount 仍然是唯一有效的杠杆,下一个瓶颈仍然是全机一份的 `fskitd`。**

### 5.4 串行点仍然在 FSKit,而且 27 新增了一条队列

8 个客户端打同一个 mount 时 `sample` 扩展进程:

```
2308 Thread_65198  DispatchQueue_68: com.apple.NSXPCConnection.user.anonymous.753  (serial)
   6 Thread_65198  DispatchQueue_66: FSModuleVolumeFileHandleQueue                 (serial)
   4 Thread_65198  DispatchQueue_13: com.apple.root.default-qos                    (concurrent)
```

**2308/2319 = 99.5% 的样本仍然落在同一条 serial 队列上**(peer pid 753 = `fskitd`),
扩展进程恒定 **88.2%**(一个核跑满),`fskitd` **107.9%** —— 与 26.6.2 的 98.8% / 117.9% 同形。
**单 mount 串行化在 27 上一点没变。**

27 新出现的队列:`FSModuleVolumeFileHandleQueue`(serial)和一组
`com.apple.fskit.FSWorkQueue.connector.<UUID>`(单进程 create+unlink 时能看到十几条),
但它们只承接很小一部分样本,没有改变"一条 serial 队列吃掉全部解码/编码"的结构。

---

## 6. 新问题:元数据写会随 mount 的存活时间劣化 3–12×

这是本轮最重要的**新**发现,26.6.2 的文档里没有。

### 6.1 现象

同一个挂载点上,**按目录**呈现稳定的双峰:

| `$S/m27/mnt1` 上的目录 | lookup-miss µs/op |
|---|---|
| 挂载点根 | 77 |
| `rt`(该 mount 上第一个被用的目录) | 75–81 |
| `par` / `nd1` / `nd2` / `src` / `tree` / `tree/pkg0000/src` / 新建的 `xa` `xb` `xc` | **267–312** |
| `fresh1` | **557** |

可复现、可交替测量(rt 75.2 / nd1 269.0 / rt 75.8 / nd1 267.8 / rt 75.8 / nd1 267.0),
和目录名、深度、条目数、是否新建都无关。**同一时刻新挂的 mount 上所有目录都是 76–82µs。**

### 6.2 根因:FSKit 27 的 `getItemForFH:` 退化成线性扫描

`sample` 慢目录时的栈(718/718 个样本):

```
-[FSVolumeConnector lookupIn:name:flags:auditToken:requestID:replyHandler:]
  -[FSModuleVolume(Project) getItemForFH:]
    __40-[FSModuleVolume(Project) getItemForFH:]_block_invoke
      684  -[__NSDictionaryM objectForKeyedSubscript:]
        139 -[FSFileHandle isEqual:] → objc_retain
        135 -[FSFileHandle isEqual:]
        119 -[FSFileHandle isEqual:] → objc_opt_class
        108 -[FSFileHandle isEqual:] → objc_release
        103 -[FSFileHandle isEqual:] → objc_opt_isKindOfClass
```

**95% 的 CPU 花在一个以 `FSFileHandle` 为 key 的 `NSMutableDictionary` 查找上,
退化成了沿哈希桶链逐个 `isEqual:`。** 同一次采样里,快目录(`rt`)的栈是

```
-[FSVolumeConnector lookupIn:...]  (178)
  -[WorldVolume lookupItemNamed:inDirectory:replyHandler:]  (120+54)
    wfs_lookup → wfs::fs_lstat → lstat(2)  (50)
```

`getItemForFH:` 在快目录里只有 1 个样本。**这是 FSKit 自己的代码,不是我们的。**
一个目录落在链的前面还是后面决定了它此后一直快还是一直慢。

### 6.3 对元数据写的影响

写路径每次 create/remove 都要造一个新 handle,所以受影响最大:

| 状态 | create+unlink µs/op | 同一目录的 lookup-miss |
|---|---|---|
| 新挂的 mount | **1046–1159** | 76–86µs |
| 跑过 15000 文件冷 stat / 8 进程 ×1500 create+unlink 之后 | **3781–4577** | 仍然 75–81µs |
| 单 mount 8 进程 create+unlink 劣化时 | **19,709µs/进程,聚合 402 ops/s** | — |

**注意 lookup 不受影响,只有写受影响** —— 因为读路径复用已有 handle,写路径不断造新的。
劣化是**可逆的**:大量 churn + reclaim 之后会自己恢复(见 §7.1 的 agentstress 两轮对比)。

### 6.4 这解释了 27 上最刺眼的两个回归

- `agentstress` 第一轮 S4 82.8s / S5 134.8s(26.6.2 分别是 6.9s / 22.5s),第二轮恢复到 8.7s / 24.3s。
- `realwork` 的 "touch 头文件 + 增量编译" 从 66% native 掉到 24% native,且两次复现一致。

---

## 7. 真实负载

### 7.1 agentstress(scale=1,native = `$S/nat27/st`,跑两轮)

| scenario | native | **worldfs 第 1 轮** | native% | **worldfs 第 2 轮** | native% | 26.6.2 native% |
|---|---|---|---|---|---|---|
| S1 context-read | 0.64–0.72s | 2.511s | 29% | 4.372s | 15% | 21% |
| S2 edit-loop | 0.161s | 0.920s | 18% | 1.042s | 15% | 17% |
| S3 edit-parallel-8 | 0.21–0.22s | **17.550s** | **1%** | 1.832s | 11% | 11% |
| S4 build-artifacts | 1.31–1.35s | **82.772s** | **2%** | 8.741s | 15% | 18% |
| S5 install-tree | 3.79–3.88s | **134.823s** | **3%** | 24.315s | 16% | 18% |
| S6 git-cycle | 0.65s | 7.592s | 9% | 3.796s | 17% | 19% |
| S7 test-churn | 6.53s | 11.170s | 58% | 11.131s | 59% | 61% |
| S8 watch-events | 1.73–1.93s | 1.748s | 110% | 1.745s | 99% | 100% |
| S9 big-file-8K-writes | 0.28–0.34s | 0.485s | 58% | 0.474s | 71% | 60% |
| S12 exec-artifacts | 2.22–2.27s | 1.887s | 117% | 2.020s | 112% | 106% |

**correctness:两轮十个场景全部 `native:ok worldfs:ok`。**

第 1 轮是在一个已经跑了一小时微基准的"老"挂载点上跑的(§6 的劣化态),
第 2 轮在同一挂载点紧接着跑,**自己恢复到了和 26.6.2 基本一致的水平**。
把第 2 轮当作稳态:S2/S3/S7/S8/S12 与 26.6.2 持平或更好,S4/S5/S6/S1 低 2–6 个百分点。

### 7.2 realwork(fmt 源码树,跑了两轮,第二轮只复核了异常项)

| workload | native | **worldfs 27.0** | native% | 26.6.2 native% |
|---|---|---|---|---|
| tar 解压源码树 | 0.135s | 0.753s | 18% | 17% |
| rm -rf 解压树 | 0.037s | 0.125s | 30% | 34% |
| git clone(本地) | 0.116s | 0.267s | 43% | 40% |
| git status(1st, racy) | 0.049s | 0.062s | 79% | — |
| git status(稳态) | 0.049s | 0.070s | 70% | 83% |
| find \| wc | 0.031s | 0.055s | 56% | 54% |
| cmake configure | 0.569s | 0.869s | 65% | 61% |
| cmake build libfmt -j8 | 1.567s | 1.698s | **92%** | 90% |
| **touch 头文件 + 增量编译** | 0.363s | **1.525s** | **24%** | **66%** |
| rm -rf 整棵树 | 0.044s | 0.214s | 21% | 26% |

(`rg` 未安装,该行两侧都没有数据。第二轮:cmake build 1.494 → 1.691s,
touch+rebuild 0.364 → 1.527s,**touch+rebuild 的回归可复现**。)

**编译主路径 92% native,比 26.6.2 略好;增量编译(touch 一个头文件)从 66% 掉到 24%,是本轮
唯一一个稳定复现的真实负载回归**,与 §6 的写路径劣化同源(ninja 重建时大量 temp 文件 churn)。

---

## 8. 系统侧的其它变化

### 8.1 `sysctl vfs.generic.lifs`:**12 个键的值与 26.6.2 逐字相同**

```
max_io_threads: 1                       max_read_blockmap_size: 1048576
max_inline_io_size: 262144              max_write_blockmap_size: 1048576
max_read_size: 8388608                  max_ssd_read_blockmap_size: 262144
max_write_size: 2097152                 max_ssd_write_blockmap_size: 262144
max_ssd_read_size: 8388608              read_meta_cache_hit:  (计数器,本轮后 1,770,397)
max_ssd_write_size: 8388608             write_meta_cache_hit: (计数器,本轮后    58,041)
```

没有新增键,`max_io_threads` 仍然是 1。`PERF_STUDY_RT_PARALLEL.md` §6 留给 root 的实验依然未执行
(需要密码),预期结论不变。

### 8.2 FSKit 27 的新 API 在**运行时**已经存在,但 SDK 还是 26.5

`xcrun --show-sdk-version` = **26.5**,`/Library/Developer/CommandLineTools/SDKs/` 里最新是
`MacOSX26.5.sdk`。但 dlopen `FSKit.framework` 之后反射得到:

| 名字 | 运行时 |
|---|---|
| `FSVolumeHandler` / `FSVolumeOpenCloseHandler` / `FSVolumeReadWriteHandler` / `FSVolumeXattrHandler` | **present** |
| `FSVolumeDataCacheHandler` / `FSVolumeRenameHandler` / `FSVolumeCloneHandler` / `FSVolumeAccessCheckHandler` / `FSVolumeNamedStreamsHandler` / `FSVolumeSeekRegionHandler` / `FSVolumeItemDeactivationHandler` / `FSVolumePreallocateHandler` / `FSVolumeKernelOffloadedIOHandler` | present |
| `FSVolumeHandlerResult` / `FSLookupItemResult` / `FSCreateItemResult` / `FSRenameItemResult` / `FSGetAttributesResult` / `FSEnumerateDirectoryResult` / `FSOpenItemResult` / `FSContext` | present |
| **`FSVolumeOperations`(我们现在声明的那个)** | **absent** —— 27 的 FSKit 二进制里改叫 `FSVolumeCommonOperations`;旧的各 `*Operations` 子协议(OpenClose/ReadWrite/Xattr/PathConf/ItemDeactivation/KernelOffloadedIO/Rename/AccessCheck/Preallocate)都还在 |

`arch.md` §7 里写的名字确实是 27 的这一套。关键签名:

```objc
-[FSLookupItemResult initWithFoundItem:itemName:itemAttributes:]
-[FSCreateItemResult initWithNewItem:newItemName:newItemAttributes:directoryAttributes:freeSpace:]
-[FSRenameItemResult initWithNewName:renamedItemAttributes:sourceDirectoryAttributes:
                     destinationDirectoryAttributes:overItemAttributes:freeSpace:]
-[FSGetAttributesResult initWithAttributes:]
-[FSVolumeHandlerResult freeSpace] / -getFreeSpaceWithReplyHandler:
-[FSContext initWithAuditToken:]
```

**`FSLookupItemResult` 确实能带 `itemAttributes`,`FSCreateItemResult` 还能同时带
新项属性 + 父目录属性。** 按 §3.1 的 trace,这理论上能把 create+unlink 的 7 次 `getattr`
基本消掉,16.03 → 9 左右;但**要 ≤6 次还得同时干掉 3 次 lookup 和 2 次 provenance getxattr,
后两者不在这套 API 的射程里。**

### 8.3 进程行为

- **每挂一个 worldfs 仍然多起一个 `WorldFSExtension` 进程**(挂 8 个 → 11 个进程),
  卸载后回收。基线状态 2 个进程。
- `fskitd`(pid 753,root)仍然是全机一份,仍然是所有扩展的 XPC 对端。
- 扩展 RSS:空闲 12.7MB,跑完 20000 文件的 lookup 后 19–21MB,没有观察到泄漏。
- **`-o` 选项到不了扩展(§3.3)**,`world fs mount W<n>` 仍然只能靠 base 路径反查。

### 8.4 日志

- **没有** 任何关于 FSKit 缓存/coherency/deferred close 的新日志。
- **没有** 关于 `*Operations` 协议 deprecated 的运行时告警(deprecation 只在编译期,
  而我们用 26.5 SDK 编译,连编译告警都不会有)。
- **有** 大量新的 error 级噪声:`getStandardItemAttributesForItem ... error:70`,
  每轮 create+unlink 1 条(见 §3.4)。
- `fskitd` 自己的日志只有连接管理(`About to get current agent for 501`、
  `Incomming connection, entitled 1`、xpc connection activate/invalidate),没有性能相关信息。

---

## 9. 门槛判定

`TASKS.md` 写的升级后验收门槛:

> create+unlink 往返 ≤ 6 次 **且** 单次往返 ≤ 40µs(→ 元数据写约 40–60%);
> 若两者都不满足,M1 维持 C(clonefile World)主线。

| 门槛 | 阈值 | 26.6.2 | **27.0 实测** | 判定 |
|---|---|---|---|---|
| create+unlink 往返次数 | ≤ 6 | 16.06 | **16.025** | **不满足**(超 2.7×,且毫无改善) |
| 单次往返 | ≤ 40µs | 68.1µs | **73.6µs**(msdos 78.0µs) | **不满足**(超 1.8×,且比 26.6.2 更差 8%) |

**两条都不满足,而且都是朝坏的方向走。元数据写实测 6.5–8% native(26.6.2 是 8–9%),
真实负载 15–18%(26.6.2 是 17–19%),外加一个新的、会把写路径再拖慢 3–12× 的
FSKit `getItemForFH:` 退化。**

用 27 SDK 的 Handler API 重写的**理论上限**(按 §3.1 的 trace 算):
消掉全部 7 次 `getattr` → 16.03 降到 ~9 次,元数据写从 ~7% 提到 ~12% native。
**仍然达不到 6 次的门槛,也达不到 §29 的 90%。** 而且这个数字还要乘以一个前提:
单次往返得从 73.6µs 降到 40µs 以下 —— 27 的实测是**反向**的。

**建议:M1 维持 C(native-root clonefile World)主线。** 把 27 SDK 的 Handler API 改造
降级为"等 Xcode 27 装好之后的一个可选实验",不作为路线依据。

---

## 附:本轮用到的脚本(都在 scratchpad `$S/perf27/`,未进仓库)

按 `PERF_STUDY_RT_PARALLEL.md` 的附录描述重建(重启后原件已丢失),并做了扩充:

| 文件 | 作用 |
|---|---|
| `rtbench.c` | 单进程微基准:`getxattr_miss` / `lookup_miss_same` / `lookup_miss_distinct` / `stat_cached` / `stat_cold` / `open_close` / `pread` / `open_read_close` / `create_only` / `unlink_only` / `create_unlink` / `create_write_close_unlink` / `tmp_rename` / `listdir` / `scandir_stat` / `w_*`(writebench 六阶段) |
| `round.sh` | 同一个 mode 跑 N 轮,报最小值与中位数 |
| `par.sh` | N 个进程并发跑同一个微基准(各自子目录),报聚合 ops/s 与每进程 µs/op |
| `par8m.sh` | 8 个进程分别打 8 个不同的 mount |
| `allocbench.mm` | 直接链 FSKit,测 `FSItemAttributes` / `FSFileName` / `NSLock` 的 ns/call |
| `trace_run.sh` / `optrace.sh` / `logstream.sh` / `fskitd_log.sh` / `errcount.sh` | `log stream` 抓 trace(必须在脚本文件里跑) |
| `samp.sh` / `cpu.sh` | 采样扩展进程的队列分布与 CPU |
| `probe2.m` | 反射 FSKit 运行时的协议/类清单 |
| `readdir_check.sh` / `rawdir.py` / `bulk.c` | readdir 完整性与 dot 项检查(含 `getattrlistbulk`) |
| `coldstat.py` / `handles.py` / `creatvar.c` | 冷 lstat、handle 数量与 RT 的关系、create 变体对比 |

### `allocbench` 在 27 上的结果(ns/call,对照 26.6.2)

| | 27.0 | 26.6.2 | 占 73.6µs RT |
|---|---|---|---|
| `FSItemAttributes new` + 15 个 setter | 421.0 | 332.3 | 0.57% |
| `FSItemAttributes new` 单独 | 50.7 | 78.9 | 0.07% |
| `FSFileName nameWithBytes:length:` | 196.8 | 214.1 | 0.27% |
| `FSFileName.string.UTF8String` | 319.0 | 420.8 | 0.43% |
| `@(ino)` + NSMutableDictionary 查找 | 109.8 | 56.2 | 0.15% |
| `NSLock` lock+unlock | 8.9 | 14.5 | 0.01% |
| `NSMutableData dataWithLength:32` | 82.8 | 81.8 | 0.11% |
| *参考:`lstat(2)`* | *974.9* | *1317* | *1.3%* |
| *参考:`getxattr(2)` 缺失* | *5873.7* | *6179* | *8.0%* |

结论不变:**把热路径分配全部消掉最多省 0.6%,不值得做。**

### 遗留状态

测试结束时:**只有 `$S/m27/mnt1` 一个 worldfs 挂载**,FAT32 镜像已 detach 并删除,
临时挂载点(`mnt2` / `mnt3` / `mm0`–`mm7`)与临时 base(`b2` / `b3` / `mb0`–`mb7`)已清理,
`$S/m27/base` 恢复为 `hello.txt` / `link` / `src` / `tree`(外加系统自建的 `.fseventsd`),
`scripts/smoke.sh` 收尾复跑 **ALL OK**。
`core/` 与 `macos/` **一行未改**(本轮纯测量,没有加过临时 tracing)。

---

## 附录 B:本报告发现的三个问题已修(2026-09-19,同日晚些时候)

本报告正文是**纯测量**的记录(测完时 `core/` 与 `macos/` 一行未改)。随后按 [`TASKS.md`](TASKS.md)
的「M0 收尾 — 2026-09-19」把其中三个问题修掉了,细节在那一节,这里只留指针和复测数字:

| 本报告的小节 | 问题 | 修法 | 复测(挂载点 `mnt4`) |
|---|---|---|---|
| §1.1 | `readdir(3)` 不再返回 `.` / `..` | `wfs::fs_readdir_emits_dots(with_attrs)`:`kern.osrelease` 主版本号(缓存)+ 枚举是否带属性 | 50 项目录 raw `getdirentries` **52 : 52**(native 也是 52);100/1000/5000 项均为 n+2;`getattrlistbulk` 两侧都是 n 项 0 dot;`ls -fa` 恰好一个 `.` 一个 `..` |
| §1.2 | `smoke.sh` 在同一个活挂载上不能连跑两次 | 收尾清理一律经挂载点删除 | 同一挂载点**连跑三次全 ALL OK** |
| §3.4 | 每轮 mutation 一条 `getStandardItemAttributesForItem ... error:70` | `WorldItem` 存最后一次 getattr 的 `wfs_attr`,core 报 ESTALE 时作答 | 20 轮 create+unlink:错误行 **20 → 0**,op 数 16.35 → **16.30/轮(不变)** |

`error:70` 换成 ENOENT 的方案也实测过:op 数不变、错误行数不变,只是变成 `error:2`,所以没有采用。

§6 的 `getItemForFH:` 线性扫描劣化是 FSKit 自己的代码,**没有修,也修不了**;
§2 / §4 / §5 的性能数字不受这三个改动影响(op 数一次没变)。
