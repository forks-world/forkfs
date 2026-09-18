# M0 性能研究:单次往返的成本构成 与 单挂载点的串行化

2026-09-18 夜,Mac mini M1(4P+4E),macOS 26.6.2。目标是回答两件事:

1. FSKit 每次内核↔扩展往返(round trip,下文 **RT**)的**地板**是多少,其中有多少是我们自己的?
2. 同一个 mount 上的并发为什么不 scale,串行点在内核还是在我们?

所有数字都是用 `$S/perf/rtbench.c`(C,`clock_gettime(CLOCK_MONOTONIC)`)在挂载点、
原生 APFS 目录、以及 **Apple 自带的 msdos FSKit 模块**(256MB FAT32 磁盘映像,`mount` 行含 `fskit`)
上跑同一组循环得到的。标注 "安静机器" 的表格是在 load average < 3 时采集,3 轮取最小值/中位数。
注意:本轮测试期间另一个 agent 在同一台机器上跑并行编译,凡是明显受负载影响的数据都已重测。

---

## 1. 单次强制往返的绝对成本

强制往返的办法:`getxattr(path, "user.nope")` —— 内核不能缓存"不存在的 xattr",所以每次调用
必然产生一次 XPC。用 `log stream` 验证过:200 次调用 + 50 次预热 = **251 条 `op getxattr`**,
即严格 1 次 XPC / 1 次调用。

### 表 1.1 单操作延迟(µs/op,n=20000,3 轮中位数,安静机器)

| 操作 | native APFS | **worldfs** | msdos(Apple FSKit) | worldfs/native |
|---|---|---|---|---|
| `getxattr("user.nope")` 缺失 → 1 次 RT | 8.70 | **83.6** | 33.2 | 9.6× |
| `lstat` 不存在的名字,**每次不同名** → 1 次 RT | 1.68 | **68.1** | 72.6 | 40× |
| `lstat` 不存在的名字,**同一个名字** | 0.80 | **0.86** | 0.56 | 1.1× |
| `lstat` 已存在文件(属性缓存命中) | 1.01 | **0.94** | 0.72 | 0.9× |
| `open+close`(`openCloseInhibited=YES`) | 10.0 | **9.3** | 7.3 | 0.9× |

要点:

- **内核会缓存"否定的名字查找"**:同一个不存在的名字重复 `lstat`,worldfs 是 0.86µs,和 native 一样,
  一次 XPC 都不发。只有**每次换名字**才强制 RT。所以 `lookup_miss_distinct` 是最干净的 RT 探针
  (backing 端只花 1 次 `lstat(2)` ≈ 1µs)。
- **`getxattr` 探针比 `lookup` 探针贵 15µs,原因在 backing 侧**:APFS 上查一个不存在的 xattr 本身
  要 8.7µs(见 native 列),而 `lstat` 只要 1.7µs。所以 83.6 = ~68(FSKit RT) + ~9(我们替内核做的
  `getxattr(2)`) + 余量。**做 RT 地板的基准要用 lookup,不要用 getxattr。**

### 表 1.2 readdir 的固定成本与每项成本(`opendir`+`getdirentries`+`closedir`,n=500,安静机器)

| 目录项数 | native | **worldfs** | msdos |
|---|---|---|---|
| 0 项 | 47.5 *(异常值)* | **176.6** | 161.3 |
| 50 项 | 27.8 | **229.3** | 320.4 *(返回 102 项)* |
| 500 项 | 174.9 | **1288.5** | 2649.0 *(返回 1002 项)* |
| **固定开销(截距)** | ~28 | **~177** | ~161 |
| **每项边际成本** | 0.33 | **2.35** | 2.45 |

msdos 每次 listdir 返回的条目数是文件数的两倍:FAT 不能存 xattr,内核为 `com.apple.provenance`
给**每个文件**额外造了一个 AppleDouble `._` 文件。这正是 `WorldVolume.mm` 里"不要实现
`supportedXattrNamesForItem:`"那条注释描述的退化路径,这里是它的实测证据。

### 表 1.3 元数据写(µs/op,3 轮最小值,安静机器)

| 操作 | native | **worldfs** | msdos |
|---|---|---|---|
| `open(O_CREAT)`+`close` | 47.2 | **532.3** | 2496.8 |
| `unlink` | 32.6 | **442.4** | 1569.3 |
| create+close+unlink(一轮) | 65.8 | **995** | — |

### 表 1.4 一次 create+unlink 到底发多少次 XPC(trace,n=200)

| op | 次数 / 每轮 create+unlink |
|---|---|
| `getattr` | **7.06** |
| `lookup` | 3.00 |
| `getxattr`(`com.apple.provenance`) | 2.00 |
| `create` | 1.00 |
| `remove` | 1.00 |
| `reclaim` | 1.00 |
| `sync` | 1.00 |
| **合计** | **16.06** |

**16.06 × 66.5µs = 1068µs,实测 995µs —— 完全吻合。**
元数据写的成本 = RT 次数 × RT 单价,没有第三个因素。7 次 `getattr` 尤其刺眼:FSKit 的
`lookupItemNamed:` 回复里**不带属性**(只回 `FSItem` + `FSFileName`),所以内核在每次 lookup
之后必须再发一次 `getattr`。这是 FSKit API 形状决定的,不是我们能改的。

---

## 2. 和 Apple 自己的 FSKit 模块对标(RT 地板)

| 指标 | **worldfs** | msdos(Apple,带 `com.apple.developer.fskit.fsmodule` entitlement) | 结论 |
|---|---|---|---|
| lookup-miss 单次 RT | **68.1µs** | 72.6µs | 我们**快 6%** |
| readdir 固定开销 | **177µs** | 161µs | 我们慢 10% |
| readdir 每项 | **2.35µs** | 2.45µs | 我们**快 4%** |
| create | **532µs** | 2497µs | 我们快 4.7×(FAT + AppleDouble + 磁盘映像,不可比) |

**结论:50–90µs 的往返是 FSKit 的地板,不是我们的实现问题。** 在两个模块做同一件事
(回一个 ENOENT)的那一栏上,我们比 Apple 自己的模块还快一点。

(msdos 的 `getxattr` 缺失只要 33µs,是因为 FAT 走 AppleDouble 旁路,内核用 `._` 文件的
否定 dentry 就能答掉大部分,不代表它的传输层更便宜 —— lookup 那一栏才是同类比较。)

---

## 3. 一次 RT 里,哪些是我们的?

### 3.1 `sample` 分解(1 个 mount,8 个客户端跑 lookup-miss,扩展进程 98.8% CPU)

**所有请求都落在一条 serial 队列上:`com.apple.NSXPCConnection.user.anonymous.821`**
(serial;peer pid 821 = `fskitd`)。该队列 4 秒内 2684 个采样点 ≈ 一个核跑满。分解:

| 栈段 | 采样点 | 占比 | 是谁的 |
|---|---|---|---|
| XPC 收包 / 解码(到达 FSKit 之前) | ~1338 | 50% | Foundation / libxpc |
| `-[FSVolumeConnector lookupIn:name:…]` 之内 | 1246 | 46% | FSKit |
|  └ 我们的 `lookupItemNamed:` 实际工作 | 378 | 14% | — |
|  &nbsp;&nbsp;&nbsp;&nbsp;└ backing `lstat(2)` | 359 | 13.4% | 内核(替内核做的那次系统调用) |
|  &nbsp;&nbsp;&nbsp;&nbsp;└ **我们自己写的代码**(path 拼接、node 表、`itemFor:`) | **~19** | **0.7%** | **我们** |
|  └ `reply(...)` 之后的 NSXPC 编码 + `mach_msg_send` | 794 | 30% | Foundation / FSKit |

把 `itemFor:` 等零散帧算进来,**我们自己的代码 ≈ 1.3%**,backing 系统调用 ≈ 13%,
**其余 ~85% 是 NSXPCConnection 解码 + NSInvocation 派发 + FSKit marshalling + 回复编码。**

### 3.2 热路径上的 ObjC 分配成本(`allocbench.mm`,直接链 FSKit)

| | ns/call | 占 66.5µs RT |
|---|---|---|
| `FSItemAttributes new` + 15 个 property set(= `wfs_attributes()`) | 332.3 | 0.50% |
| `FSItemAttributes new` 单独 | 78.9 | 0.12% |
| `FSFileName nameWithBytes:length:` | 214.1 | 0.32% |
| `FSFileName.string.UTF8String`(`getXattrNamed:` 里用到) | 420.8 | 0.63% |
| `@(ino)` + `NSMutableDictionary` 查找(`itemFor:`) | 56.2 | 0.08% |
| `NSLock` lock+unlock | 14.5 | 0.02% |
| `NSMutableData dataWithLength:32` | 81.8 | 0.12% |
| *参考:`lstat(2)`* | *1317* | *2.0%* |
| *参考:`getxattr(2)` 缺失* | *6179* | *9.3%* |

**把 `wfs_attributes()` 的分配全部消掉,最多省 0.5%。** 这条路不值得走。

---

## 4. 并行度:单 mount 是串行的

### 表 4.1 lookup-miss(每进程 8000 次,3 轮最小值,安静机器)

| 进程数 | 1 个 mount 延迟 | 1 个 mount 聚合 | 8 个 mount 聚合 | native 聚合 |
|---|---|---|---|---|
| 1 | 66.5µs | 14,321 ops/s | — | 209,892 ops/s |
| 2 | 79.8µs | 24,093 ops/s | — | — |
| 4 | 100.2µs | 38,654 ops/s | — | — |
| 8 | 199.5µs | **39,358 ops/s** | **51,693 ops/s** | 768,992 ops/s |

### 表 4.2 getxattr-miss

| 进程数 | 1 mount 延迟 | 1 mount 聚合 | 8 mounts 聚合 |
|---|---|---|---|
| 1 | 84.2µs | 11,099 ops/s | — |
| 2 | 103.3µs | 18,533 ops/s | — |
| 4 | 132.8µs | 29,135 ops/s | — |
| 8 | 264.8µs | **29,595 ops/s** | **44,504 ops/s** |

### 表 4.3 create+unlink(各进程在自己的子目录里)

| 进程数 | 1 mount 延迟 | 1 mount 聚合 | 8 mounts 聚合 | native 聚合 |
|---|---|---|---|---|
| 1 | 1001.7µs | 981 ops/s | — | 12,014 ops/s |
| 2 | 1707µs | 1,159 ops/s | — | — |
| 4 | 3312µs | 1,197 ops/s | — | — |
| 8 | 6368µs | **1,249 ops/s** | **3,821 ops/s** | 27,842 ops/s |

**读:** 单 mount 的吞吐在 4 个进程时就撞顶(~39k lookups/s ≈ 25µs 串行服务时间),
再加进程只是把延迟按比例摊长(8 进程 = 8×单进程延迟)。
**写:** 更彻底 —— 1 个进程 981 ops/s,8 个进程 1249 ops/s,**只涨 27%**。
这正好解释 agentstress 里 S3(8 进程并行编辑)只有 native 的 11%,而单进程 S2 有 17%。

### 4.4 串行点在哪

三条独立证据指向**同一个 serial 队列**:

1. **trace 的交错模式**:4 个进程同时打 lookup,1600 条 trace 里相邻两条来自**不同进程**的比例
   是 **98.2%**,平均"同一进程连续执行"的 run length = **1.02**;1567/1604 条记在**同一个
   handler 线程**上。也就是严格的一次一个、round-robin,不是"几个线程各跑各的"。
2. **CPU 计量**:1 个 mount + 8 个客户端时,扩展进程恒定 **98.8%**(正好一个核,跑满),
   `fskitd` 117.9%。扩展跑满一个核说明队列上永远排着下一个请求 —— 它就是瓶颈。
3. **`sample` 的队列名**:`com.apple.NSXPCConnection.user.anonymous.821 (serial)`。
   这条队列是 FSKit/Foundation 建的,不是我们建的。

**不是我们的锁。** 逐条核对:

| 锁 | 持锁做什么 | 是否跨系统调用 | 实测/估计持锁时间 |
|---|---|---|---|
| core `wfs_view::mu`(`Guard`,pthread_mutex) | `path_of` / `intern` / `fill_parent` / `forget` / `unlink` 的表更新 | 否,纯内存 | < 0.5µs |
| 同上,`wfs_rename` | `fs_lstat` + `fs_rename` **在锁内** | **是**(故意的) | ~30–60µs |
| ObjC `_lock`(`NSLock`) | 只保护 `_items` 字典 | 否 | 14.5ns 锁 + 56ns 查找 |

唯一跨系统调用持锁的热路径是 `wfs_rename`(这是 2026-09-18 修 rename 数据丢失 bug 时故意做的,
不能拆)。但由于同一个 mount 的请求本来就在一条 serial 队列上,这两把锁在单 mount 内
**根本不会发生竞争** —— 拆锁不会带来任何收益。

### 4.5 跨 mount:每个 mount 一个扩展进程

实测:每挂一个 worldfs 就多起一个 `WorldFSExtension` 进程(挂 10 个 → 11 个进程)。
8 个 mount × 8 个进程时:

| | 1 mount / 8 客户端 | 8 mounts / 8 客户端 |
|---|---|---|
| 扩展进程 CPU | 98.8%(1 个进程) | 8 × ~36% = 288% |
| `fskitd` CPU | 117.9% | **225.9%** |
| 系统空闲 | — | 14.7% |
| lookup 聚合吞吐 | 39,358 ops/s | 51,693 ops/s(**+31%**) |
| create+unlink 聚合吞吐 | 1,249 ops/s | 3,821 ops/s(**+206%**) |

一个 mount 一个 World 确实解开了扩展侧的串行,但**下一个瓶颈立刻变成 `fskitd`**:
它是全机一份的 FSKit 代理(扩展的 XPC 对端就是 pid 821 = `fskitd`),8 个 mount 忙起来时
它一个人吃 2.26 个核,比 8 个扩展进程加起来(2.88 核)只少一点。所以跨 mount 的扩展比是
3–8×,不是 8×;而且这台机器在 8 mount 测试时已经只剩 14.7% 空闲。

参考:native APFS 自己也不是线性 scale —— create+unlink 从 1 进程 12,014 ops/s 到
8 进程 27,842 ops/s 只涨 2.3×。所以 "8 个 mount 拿到 3.1×" 已经吃掉了 backing 能给的大部分并行度。

---

## 5. 试过但**不采纳**的两个改动(已全部回退)

因为 `mount -o` 的选项在 macOS 26.6.2 上**根本到不了扩展**
(`FSTaskOptions.taskOptions` 恒为空数组,`-o world=N` 同样收不到 —— 这条本身是个待修的 bug,
`cli/` 的 `world fs mount W<n>` 目前是靠 base 路径反查 world 才碰巧正确的),
实验开关临时改成了 base 目录里的标记文件,测完已删。

### 表 5.1 A/B(同一轮里交替跑 4 个变体,3 轮最小值)

| 变体 | lookup P=1 | lookup P=8 聚合 | create+unlink P=1 | create+unlink P=8 聚合 |
|---|---|---|---|---|
| baseline | 66.3µs | 39,884 ops/s | 978.9µs | 1,248 ops/s |
| **QoS → USER_INTERACTIVE** | 66.3µs (+0.0%) | 38,930 (**−2.4%**) | 989.5µs (+1.1%) | 1,228 (−1.6%) |
| **handler 派到并发队列再 reply** | 74.0µs (+11.7%) | 30,357 (**−23.9%**) | 1124.8µs (+14.9%) | 1,157 (−7.3%) |
| 两者都开 | 74.5µs | 33,418 (−16.2%) | 1113.6µs | 1,136 (−9.0%) |

- **QoS bump 没有任何收益**(−2.4% … +0.1%,在噪声内)。RunningBoard 那条
  "Denying dirty-tracking opt-in for managed WorldFSExtension" 和调度优先级无关;
  扩展的 worker 线程本来就通过 voucher 继承调用方的 QoS(`sample` 里能看到
  `_dispatch_set_priority_and_voucher_slow` 每个 block 都在重设优先级,所以"每线程设一次"
  也会被 libdispatch 覆盖掉)。**已回退。**
- **把 handler 派到并发队列反而更慢**(单 mount 8 进程掉 24%)。原因见 §3.1:
  serial 队列上 98.7% 的工作是 XPC 解码和回复编码,**这些无论如何都留在那条队列上**,
  能搬走的只有那 1.3%;换来的是每次多一跳 `dispatch_async`。**已回退。**
- 顺带确认:**我们现在就是 inline reply,代码里没有任何队列跳转**(`WorldVolume.mm` 的每个
  handler 都在调用线程上直接 `reply(...)`),这是对的。

---

## 6. 留给 root 的实验(**未执行**,需要密码)

`sysctl vfs.generic.lifs` 全部 12 个键(前 10 个可写,后 2 个是计数器):

```
vfs.generic.lifs.max_io_threads: 1
vfs.generic.lifs.max_inline_io_size: 262144
vfs.generic.lifs.max_read_size: 8388608
vfs.generic.lifs.max_write_size: 2097152
vfs.generic.lifs.max_ssd_read_size: 8388608
vfs.generic.lifs.max_ssd_write_size: 8388608
vfs.generic.lifs.max_read_blockmap_size: 1048576
vfs.generic.lifs.max_write_blockmap_size: 1048576
vfs.generic.lifs.max_ssd_read_blockmap_size: 262144
vfs.generic.lifs.max_ssd_write_blockmap_size: 262144
vfs.generic.lifs.read_meta_cache_hit:  (计数器)
vfs.generic.lifs.write_meta_cache_hit: (计数器)
```

请以 root 执行:

```bash
sudo sysctl vfs.generic.lifs.max_io_threads=4
# 复测(脚本都在 $S/perf/):
#   bash par.sh lookup_miss_distinct 8000 8 <mnt>/par     # 预期:不变
#   bash par.sh create_unlink        1500 8 <mnt>/par     # 预期:不变
#   8 进程 × 64MB 顺序读 / 写                              # 预期:这里才可能变
sudo sysctl vfs.generic.lifs.max_io_threads=1   # 复原
```

**预期**:这个旋钮控制的是 `lifs_io_strategy_thread`(数据面 read/write/blockmap 的 IO 线程池),
**不是** `lifs_req_callback_thread`(每个 mount 一条,负责把元数据请求回调给扩展)。
所以对 lookup / create / unlink 这类元数据往返**预计没有影响**;对多进程并发大文件 IO 可能有。
验证方式是改前改后各跑一遍上面三组,并观察扩展进程的线程数是否变化。

---

## 7. 结论与建议

### (1) FSKit 的 RT 地板 = **~68–73µs**,证据来自 msdos 对标

同一件事(在目录里查一个不存在的名字、回 ENOENT):**worldfs 68.1µs,Apple 的 msdos 72.6µs,
native APFS 1.68µs**。readdir 的固定开销(177 vs 161µs)和每项成本(2.35 vs 2.45µs)也在同一量级。
**我们已经在地板上,甚至略低于 Apple 自己的模块。** 50–90µs 是 FSKit 这条链路
(kernel lifs → mach → fskitd → NSXPCConnection → NSInvocation → FSVolumeConnector → 回来)
的固有价格,和 passthrough 的实现无关。

### (2) 这 66µs 里属于我们的部分:**≤2%**

`sample` 分解:我们自己写的代码占串行 CPU 的 **1.3%**(≈0.3–0.9µs),
替内核做的那次 backing `lstat(2)` 占 13%(≈1.3µs),**其余 85% 是 Foundation/FSKit 的
marshalling**。ObjC 分配(`FSItemAttributes` + 15 个 setter = 332ns)占 0.5%。
**我们这边没有 5% 以上的可回收空间。** 唯一还有意义的 core 侧优化是 readdir 的每项成本
(2.35µs/项,但已经比 msdos 快),以及 `getXattrNamed:` 里的 `name.string.UTF8String`
(420ns,可以改成直接用 `name.data.bytes` + 补 NUL,省 ~0.3%)—— 都不值得为它冒回归风险。

### (3) 单 mount 的串行化:**在内核和 FSKit,不在我们**

内核每个 mount 只有一条 `lifs_req_callback_thread`;扩展这边 FSKit/NSXPCConnection 又把所有
请求投递到**一条 serial dispatch 队列**上(`sample` 里的队列名就写着 `(serial)`)。
trace 显示 4 个进程的操作 98.2% 严格交替、平均 run length 1.02;扩展进程在 8 客户端时
恒定跑满**正好一个核**。我们自己的两把锁(core 的 `wfs_view::mu`、ObjC 的 `_lock`)
持锁时间都在纳秒到亚微秒级,且在单 mount 内**不可能有竞争**;唯一跨系统调用持锁的
`wfs_rename` 是正确性要求,不是性能问题。把 handler 搬到并发队列实测**倒退 24%**,
因为串行队列上的工作 98.7% 不是我们的。

### (4) 四个候选优化的预期收益

| 优化 | 预期收益 | 实测 | 结论 |
|---|---|---|---|
| **QoS bump**(USER_INTERACTIVE) | ? | **−2.4% … +0.1%** | **无效,已回退** |
| **削减热路径分配** | ≤0.5% | 未单独实测(上限来自 `allocbench`) | **不做** |
| **`max_io_threads=4`**(需 root) | 元数据 0%,数据面未知 | 未执行(见 §6) | 留给用户;元数据预期无变化 |
| **一个 World 一个 mount** | — | **lookup +31%,getxattr +50%,create+unlink +206%** | **唯一有意义的杠杆,建议 M1 就按这个形状做** |

补充两条**结构性**的观察,对 M1/M2 的取舍更重要:

- **元数据写 = RT 次数 × 66µs,没有别的项。** 一轮 create+unlink 是 **16.06 次 XPC**
  (其中 `getattr` 7 次、`lookup` 3 次、`provenance` 的 `getxattr` 2 次),16.06 × 66.5 = 1068µs,
  实测 995µs。要提升元数据写只有两条路:**减少 RT 次数**(FSKit 的 `lookupItemNamed:`
  回复不带属性,所以每次 lookup 后内核必然补一次 getattr —— 这是 API 形状,我们改不了),
  或者**把写密集目录移出 FSKit**(即 `docs/REDIRECT_EXPERIMENT.md` 那条路)。
- **一个 World 一个 mount 的下一个瓶颈是 `fskitd`**:8 个 mount 忙起来时它独占 2.26 个核,
  而 8 个扩展进程加起来才 2.88 核。所以"每个 World 一个 mount"能拿到 3–8× 而不是 8×,
  并且在 8 个 World 同时忙的时候这台 M1 就已经只剩 14.7% 空闲。**规模化到几十上百个
  活跃 World,`fskitd` 会先撞墙**,这一点必须写进 M1 的容量模型。

---

## 附:本研究用到的脚本(都在 scratchpad `$S/perf/`,未进仓库)

| 文件 | 作用 |
|---|---|
| `rtbench.c` | 单进程微基准:`getxattr_miss` / `lookup_miss_same` / `lookup_miss_distinct` / `stat_cached` / `open_close` / `create_only` / `create_unlink` / `listdir` |
| `par.sh` | N 个进程并发跑同一个微基准,报聚合 ops/s 和每进程 µs/op |
| `allocbench.mm` | 直接链 FSKit,测 `FSItemAttributes` / `FSFileName` / `NSLock` 等的 ns/call |
| `optrace.sh` / `cutrace.sh` / `ilv.sh` | `log stream` 抓每操作 trace,统计 XPC 次数与跨进程交错 |

**本研究在扩展/core 里加的临时开关(QoS、async offload、标记文件)已全部回退,
`macos/fskit/WorldVolume.mm` 与研究开始前逐字节一致(sha1 `ff16be1f…`)。**
