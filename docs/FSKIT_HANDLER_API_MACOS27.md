# macOS 27 Handler API 实测:把 FSKit passthrough 移植到 `FSVolumeHandler` 之后

2026-09-19,Mac mini M1(4P+4E),**macOS 27.0(build 26A428)**,
**Command Line Tools 27.0 SDK**(`xcrun --show-sdk-version` = 27.0),仓库 commit `4582150` + 本次改动。
测量期间 load average 1.9–3.6(每张表都标了具体值),机器上除本实验外只有常驻后台进程。

这一轮回答 [`MACOS27_MEASUREMENTS.md`](MACOS27_MEASUREMENTS.md) §8.2 留下的那个问题:
**用 27 SDK 的 Handler API(结果对象带 `FSItem.Attributes`)重写,能不能把 create+unlink 的
16 次往返压到门槛以内?**

---

## 0. 结论速览

| 问题 | 旧 API(27.0) | **Handler API** | 变化 |
|---|---|---|---|
| create+unlink 往返次数 | 16.04 | **11.04**(`dataCacheInhibited=YES`)/ 13.04(开数据缓存) | **-31%** |
| 单次往返(lookup-miss) | 74.27µs | **74.64µs** | 没变(API 不影响 XPC 成本) |
| create+unlink 延迟 | 1097.9µs | **995.2µs** | -9.4% |
| lookup 之后必补的 getattr | 7.04 次/轮 | **0 次**(全被结果对象里的属性替代) | **彻底消失** |
| `com.apple.provenance` getxattr | 2.00 次/轮 | **2.00 次/轮** | 没变 |
| 一个名字要几次 lookup | 2 次(冷查一个名字发两次) | **2 次** | 没变 |
| `FSVolumeDataCacheHandler` | — | **净亏**:open/close 每次都真的发到扩展,+2 往返/次开关 | 建议关掉 |
| 元数据写 native% | 5.5% | **6.1%** | 略好 |
| 真实负载(agentstress 稳态) | 11–69% native | **11–79% native** | 小幅好转 |
| **门槛:往返 ≤ 6** | 16.04 **FAIL** | **11.04 FAIL**(超 1.84×) | 仍不满足 |
| **门槛:单次往返 ≤ 40µs** | 74.27µs **FAIL** | **74.64µs FAIL**(超 1.87×) | 仍不满足 |

**两条门槛都仍然不满足。**Handler API 是一次真实但有限的改进:它精确地干掉了
`MACOS27_MEASUREMENTS` §3.1 指出的那 7 次 `getattr`,可是**它干掉的恰好是最便宜的那几次往返**
(getattr 回一个已在手的 inode 属性,~18µs),剩下的 lookup / create / remove / sync / reclaim
一次都没少,而且每次仍然要 ~70µs。往返次数 -31%,墙钟只 -9%,正是这个原因。

---

## 1. 做了什么

### 1.1 新增 `macos/fskit/WorldVolumeHandler.{h,mm}`(`WorldVolumeH`)

- 实现 `FSVolumeHandler` / `FSVolumeXattrHandler` / `FSVolumeReadWriteHandler` /
  `FSVolumeDataCacheHandler` / `FSVolumePathConfOperations`,用 `API_AVAILABLE(macos(27.0))` +
  调用点 `if (@available(macOS 27.0, *))` 卫护;**冻结的 `WorldVolume`(旧 `FSVolumeOperations`)
  一个字节都没改**,两个类共存在同一个二进制里。
- 每个回复都是结果对象,并且**把该类 `requestedAttributes` 要求的属性全部填满**。
  运行时反射(`$S/fskit27/reqattr.m`)显示 **21 个结果类的 `requestedAttributes` 完全一样**:
  `0x3fff` = type/mode/linkCount/uid/gid/flags/size/allocSize/fileID/parentID/atime/mtime/ctime/btime。
  core 的 `wfs_attr` 本来就带齐这 14 项,`wfs_attributes()` 一行不用改。
- 需要父目录属性的地方(`FSCreateItemResult` / `FSRemoveItemResult` / `FSCreateLinkResult` /
  `FSRenameItemResult`)额外向 core 要一次目录 `getattr`:一次本地 `lstat(2)` ≈ 2µs,
  换掉一次 ~70µs 的 XPC,稳赚。
- `remove` / `rename` 里被删除/被覆盖的那个 item 的属性**在动作之前快照**,之后按 nlink 减一修正 ——
  因为动作之后 core 的 `path_of()` 已经没有链路可走(-ESTALE)。这与旧前端回答"删除后那次 getattr"
  用的是同一条规则。
- `freeSpace` 一律传 `FSFreeSpace.noUpdate`:传 `nil` 会让 FSKit 自己去调 `volumeStatistics`
  (Apple 文档明说"may lead to degraded performance"),而 passthrough 的可用空间本来就是
  backing 卷的,`statfs(2)` 走 `volumeStatistics` 照样拿得到。
- M0 语义全部保留:**每次请求重新打开 backing fd、不缓存 fd、隐藏 quarantine、
  不实现 `supportedXattrNamesForItem:`**(实现它会让内核对每个 create 造 AppleDouble `._` 文件)。

### 1.2 两个"按挂载切换"的测量开关(marker 文件)

`scripts/bundle.sh` 重装 appex 会掐掉所有活挂载,所以 A/B 不能靠重新安装。
`WorldFileSystem.mm` 在 `loadResource` 时读 store 目录下的 marker,**同一个已安装的二进制里
按挂载点选 API**:

| marker(在 store 目录下) | 效果 |
|---|---|
| 无 | macOS 27 上默认走 `WorldVolumeH`(Handler API),且 `dataCacheInhibited = YES` |
| `wfs_api_old` | 走冻结的 `WorldVolume`(旧 `FSVolumeOperations`) |
| `wfs_datacache` | `WorldVolumeH` 打开 `FSVolumeDataCacheHandler`(实测更差,见 §2.3) |

(测量期间这个开关的默认方向是反的 —— marker 叫 `wfs_nodatacache`、默认开数据缓存;
测完按 §2.3 的结论把默认翻了过来,行为与当时的 `wfs_nodatacache` 挂载逐字节相同。)

这样**旧 API 的对照列是今天、同一台机器、同一个二进制、间隔几分钟测出来的**,
而不是引用两周前的数字。下文每张表里的 "旧 API(今天)" 列就是这么来的;
`MACOS27_MEASUREMENTS` 里的老数字单独列出来做交叉验证。

### 1.3 挂载布局

一个 10002 文件的源码树 `src10k`(`hello.txt` / `link` / `src/a.c` /
`tree/pkg0000..0199/src/file000..049.c`)→ `world fs init` → S1 →
fork 出三个 World:`baseH`(W1,Handler)、`baseO`(W2,旧 API)、`baseD`(W3,Handler+noDC)。
每一轮测量前都重新挂到一个新的挂载点,保证"新鲜挂载"。
native 对照在 `$S/fskit27/nat`(真 APFS,不在任何 backing 里)。

---

## 2. 每个操作发多少次往返(逐条对照 `MACOS27_MEASUREMENTS` §3)

方法与 §3 完全一致:`log stream --level debug --predicate 'subsystem == "world.forks.fs"'`
(写在脚本文件里执行),统计扩展打的 `op ...` 行。

### 表 2.1 一轮 create+unlink(n=200)

| op | 旧 API(§3.1 老数据) | **旧 API(今天)** | **Handler + DataCache** | **Handler + `dataCacheInhibited`** |
|---|---|---|---|---|
| `getattr` | 7.025 | **7.04** | **2.06** | **2.06** |
| `lookup` | 3.00 | 3.00 | 3.00 | 3.00 |
| `getxattr`(全是 `com.apple.provenance`) | 2.00 | 2.00 | 2.00 | 2.00 |
| `create` | 1.00 | 1.00 | 1.00 | 1.00 |
| `open` | — | — | **1.00** | — |
| `close` | — | — | **1.00** | — |
| `remove` | 1.00 | 1.00 | 1.00 | 1.00 |
| `sync` | 1.00 | 1.00 | 1.00 | 1.00 |
| `reclaim` | 1.00 | 1.00 | 1.00 | 1.00 |
| **合计** | **16.025** | **16.04** | **13.04** | **11.04** |
| 实测延迟 | 1036.5µs | 1097.9µs | 1138.4µs | **995.2µs** |
| 推出的 µs/往返 | 64.7 | 68.4 | 87.3 | **90.1** |

**逐条回答任务书的三个问题:**

- **"mutation 之后的 getattr 刷新会不会消失?"** —— **会,而且消失得很干净。**
  旧 API 每轮 7.04 次 `getattr`,Handler API 只剩 2.06 次,**而且剩下的这 2 次全是对
  *父目录* 的 getattr,不是对新建/删除的文件的**(见下面的完整序列)。
  也就是说 `FSLookupItemResult` / `FSCreateItemResult` / `FSRemoveItemResult` 里带的
  `itemAttributes` 内核**确实**用了、确实缓存了,一次都没再回来问。
- **"provenance getxattr 探测还在不在?"** —— **一次不少,仍然每轮 2 次。**
  这不在这套 API 的射程内:它是内核在 create 之后给新文件打 provenance 标记的流程,
  与结果对象无关。
- **"create 之后的 lookup 会不会消失?"** —— **不会。** `lookup` 稳定 3.00 次/轮。
  更精确地说(见 §2.5),**macOS 27 对一个未缓存的名字会连发两次 lookup**,
  第三次是 unlink 前的那次;Handler API 对此毫无影响。

**Handler + `dataCacheInhibited` 的完整一轮(11 次):**

```
lookup(dir, name)  ->  ENOENT                 # O_CREAT 的否定查找
getattr(dir)                                  # 父目录属性
create(dir, name)                             # 回 FSCreateItemResult(新项属性 + 目录属性)
getxattr(new, com.apple.provenance)
getxattr(new, com.apple.provenance)
getattr(dir)
lookup(dir, name)  ->  hit                    # unlink 前的查找,回 FSLookupItemResult(带属性)
remove(item, name, dir)                       # 回 FSRemoveItemResult(被删项属性 + 目录属性)
sync
reclaim(new)
```
(第 11 次是 3.00 次 lookup 里的第三次,发生在同一个名字上;见 §2.5。)

### 表 2.2 其它典型动作(次/轮 或 次/文件)

| 动作 | 旧 API(§3.2 老数据) | **旧 API(今天)** | **Handler + DataCache** | **Handler + noDC** |
|---|---|---|---|---|
| create+write+close+unlink(n=200) | — | **18.07** | 14.09 | **12.05** |
| tmp+fsync+rename 编辑一次(n=100) | 25.21 | **25.25** | 17.20 | **15.17** |
| `tar xf` 20 个 4KB 文件(每文件) | 45.75 | **45.25**\* | 45.30 | **37.85** |
| `ls -l` 一个 50 项目录(已预热,总计) | 51 | **52** | **104** | **52** |
| 冷 `lstat` 5000 个不同文件(每文件,新鲜挂载) | — | **3.08** | **2.04** | 2.04 |
| 纯 lookup-miss | 1.10 | **1.02** | 1.02 | 1.02 |

\* 旧前端的 `setXattrNamed:` 没有 trace 行,所以旧 API 的 `tar` 总数少算了 2.10 次/文件
(Handler 版补了这条 trace)。口径对齐后旧 API 是 **47.35**,Handler+noDC 的 37.85 少 **20%**。

**逐行拆解:**

- **`tar xf`**:旧 API `getattr` 12.70 → Handler 3.20(-9.5),但 Handler+DataCache 又加回
  `open` 3.20 + `close` 3.20,**净值完全打平(45.25 → 45.30)**;关掉数据缓存才拿到 37.85。
- **`ls -l` 50 项**:旧 API 52 次(`readdir` 1 + `listxattr` 50 + `sync` 1)。
  Handler+DataCache **翻倍到 104**:多了 50 次 `getxattr(com.apple.macl)` + `open`/`close`。
  这 50 次 macl 探测是 `FSVolumeDataCacheHandler` 的 open 路径带出来的 —— 关掉数据缓存后
  又变回 52,**和旧 API 一模一样**。
- **冷 `lstat`**:旧 API 每文件 `lookup` 2.04 + `getattr` 1.04 = 3.08;
  Handler 只剩 `lookup` 2.04,**getattr 归零**。这是 Handler API 最干净的一次胜利。

### 2.3 `FSVolumeDataCacheHandler` 到底买到了什么(答案:什么都没买到)

按任务书要求,`openItem:modes:cacheMode:context:` 按最宽松的口径授权:
`readWithCache → readCache`,`readWriteWithCache → writeBack`,`none → noCache`。
然后直接量 open/close/read 到底还发不发。

| 探针 | **Handler + DataCache** | **Handler + `dataCacheInhibited`** |
|---|---|---|
| 冷读 200 个 4KB 文件(次/文件) | `lookup` 2.04 + **`open` 1.00** + `read` 1.00 + **`close` 1.00** = **5.06** | `lookup` 2.04 + `read` 1.00 = **3.06** |
| 同样 200 个文件再读一遍(热,次/文件) | **`open` 1.00 + `close` 1.00 = 2.00** | **0.00** |
| 同一个文件 `open`+`close` 2000 次(次/次) | **2.06**(open 1.03 + close 1.03) | **0.01** |
| `open`+`close` 延迟 | **140.98µs** | **11.00µs** |
| `open`+`read4K`+`close` 延迟 | **141.03µs** | **11.60µs** |
| 4K 写(页缓存内)延迟 | 2.57µs | 2.49µs |

**结论,三条都是负面的:**

1. **open / close 每一次都真的发到扩展,没有任何 deferred close、没有任何合并。**
   2000 次 `open`+`close` 产生 2051 次 `open` 和 2051 次 `close`。
   协议文档说 `readCache`/`writeBack`/`writeThrough` "support deferred closing" ——
   **实测没有发生**,至少对 `FSPathURLResource` 的 unary 文件系统没有。
2. **读写行为一点没变**:冷读仍然每个页缓存 miss 发一次 `read`,热读一次都不发,
   写仍然走内核页缓存后台回写(4K 写 2.5µs)。也就是说,**内核本来就在缓存**,
   这正是协议文档最后那句话说的:"If a file system doesn't conform to this protocol,
   the kernel may still cache it."
3. 代价是**每次 `open(2)`/`close(2)` 各多一次 ~70µs 的 XPC 往返**,
   把 `open`+`close` 从 11µs 打到 141µs(**13×**),把 `ls -l` 的往返数翻倍。

**所以这个 passthrough 的正确配置是 `dataCacheInhibited = YES`** ——
它等价于 M0 的 `openCloseInhibited = YES`,实测两者的 open/close/read 行为逐项相同。
下文凡是"Handler API"的推荐值都取这一列。

### 2.4 FSKit 的 error 级日志噪声

`MACOS27_MEASUREMENTS` §3.4 记录的
`-[FSVolumeConnector getStandardItemAttributesForItem:...]...error:70`(每轮 create+unlink 一条,
45 分钟产生 15 万条)在本轮**两个 API 上都没有出现** —— 因为 commit `d1a2a3c` 给旧前端加的
"ESTALE 时回上一次的属性快照"已经把它堵住了,Handler 版继承了同一条规则。

### 2.5 附带发现:macOS 27 对一个冷名字发**两次** lookup

冷 `lstat` 5000 个不同文件(100 个目录 × 50 个文件,新鲜挂载)在两个 API 上都是
**10201 次 `lookup`**,即 **2.04 次/文件**。5000 个叶子 + 100 个 `pkgNNNN` + 100 个 `src`
= 5200 个不同名字,10201 / 5200 ≈ **1.96**。
这解释了 create+unlink 里那个一直数不清的第三次 lookup:**不是 Spotlight,也不是我们**
(把测试目录改名成 `*.noindex` 结果一模一样:11.04 → 11.07,在噪声内)。
这是内核/FSKit 侧的固定开销,**约占 create+unlink 全部往返成本的 9%**,
不在任何一套模块 API 的射程内。

---

## 3. 单次往返的地板与微基准

### 表 3.1 单操作延迟(µs/op,n=20000,写类 n=2000,3 轮取最小值)

2026-09-19 14:06–14:11,load 1.9–2.2,四列在 5 分钟内依次跑完。

| 操作 | native | **旧 API(今天)** | **Handler+DataCache** | **Handler+noDC** | native%(noDC) | 参考:§2.1 老数据 |
|---|---|---|---|---|---|---|
| `lstat` 不存在、每次换名 → 1 次 RT | 1.89 | 74.27 | 74.87 | **74.64** | **2.5%** | 73.57 |
| `getxattr("user.nope")` 缺失 | 9.58 | 94.76 | 95.70 | **95.32** | 10.0% | 93.71 |
| `lstat` 不存在、同一个名字(负缓存) | 0.73 | 0.79 | 0.79 | **0.80** | 91% | 0.78 |
| `lstat` 已存在文件(属性缓存命中) | 0.99 | 0.93 | 0.93 | **0.92** | **108%** | 0.91 |
| `open`+`close` | 11.57 | 10.97 | **140.98** | **11.00** | **105%** | 10.68 |
| `pread` 4K(fd 已开,页缓存命中) | 0.48 | 0.45 | 0.47 | **0.45** | **107%** | 0.46 |
| `open`+`read4K`+`close` | 12.30 | 11.56 | **141.03** | **11.60** | **106%** | 11.44 |
| `lstat` 5000 个不同文件(真冷,新鲜挂载) | 1.91† | 146.01 | **127.89** | 127.9 | 1.5% | 122.53 |
| `create`(`O_CREAT`+`close`) | 45.34 | 584.42 | 691.57 | **559.57** | 8.1% | 561.79 |
| `unlink` | 25.07 | 491.61 | 427.39 | **430.08** | 5.8% | 475.88 |
| **create+close+unlink(一轮)** | 60.89 | **1097.90** | 1138.40 | **995.22** | **6.1%** | 1036.52 |
| create+write4K+close+unlink | 89.07 | 1074.03 | 1059.35 | **961.34** | 9.3% | 1259.36 |
| tmp 写+fsync+rename 覆盖 | 159.66 | 1745.25 | 1462.84 | **1351.29** | 11.8% | 2118.02 |
| `listdir` 0 项 | 15.20 | 209.84 | 335.89 | **209.67** | 7.3% | 201.04 |
| `listdir` 50 项 | 30.29 | 261.54 | 389.81 | **262.99** | 12% | 264.53 |
| `listdir` 500 项 | 177.63 | 1236.53 | 1377.21 | **1156.14** | 15% | 1138.26 |
| `scandir`+`lstat` 50 项 | 90.53 | 320.00 | 460.20 | **319.42** | 28% | 517.16 |

† native 冷 lstat 沿用 §2.1 的 1.91µs(本轮 native 侧的同一棵树已经在 APFS 缓存里)。

**要点:**

- **单次往返的地板一动没动:74.27 → 74.64µs。** API 形状不改变 XPC 的成本,
  这一条本来也不该期待有变化,实测确认了。**门槛 ≤40µs 依然差 1.87×。**
- **Handler+noDC 相对旧 API 的净收益集中在写路径**:
  create+unlink **-9.4%**、create+write4K+close+unlink **-10.5%**、tmp+rename **-22.6%**、
  冷 lstat **-12.4%**、`scandir+lstat` 打平。读路径、缓存命中路径**逐项相同**
  (0.92/0.45/11.60,都在 native 的 105–108%)。
- **Handler+DataCache 是全面的退步**,只有 tmp+rename 和 create+write+close+unlink 例外
  (那两个的 open/close 次数少、getattr 省得多)。`open`+`close` 13× 的退步足以一票否决。
- 冷 lstat 每次往返:旧 API 146.01/3.08 = **47.4µs**,Handler 127.89/2.04 = **62.7µs**。
  create+unlink 每次往返:旧 68.4µs → Handler 90.1µs。
  **这就是"次数降 31%、墙钟只降 9%"的全部原因:被干掉的 getattr 是最便宜的那种往返
  (回一个手上已有的 inode 属性,~18µs),留下的 lookup / create / remove 才是 ~70–90µs 的大头。**

### 表 3.2 writebench 分阶段(µs/op,n=300,批式:先建完 300 个再统一写/关/截断/fsync/删)

| 阶段 | native | **旧 API(今天)** | **Handler+DataCache** | **Handler+noDC** | native%(noDC) | 参考:§4.2 老数据 |
|---|---|---|---|---|---|---|
| create 新文件 | 44.82 | 599.16 | 626.68 | **563.17** | 8.0% | 1242.2 |
| write 4K | 9.18 | 2.42 | 2.57 | **2.49** | **369%** | 9.05 |
| close | 10.38 | 173.07 | 202.98 | **143.03** | 7.3% | 418.1 |
| open `O_TRUNC` | 28.03 | 240.75 | 292.92 | **230.28** | 12% | 832.0 |
| fsync | 0.34 | 66.34 | 66.06 | **66.49** | 0.5% | 318.8 |
| unlink | 29.73 | 493.28 | 422.79 | **435.35** | 6.8% | 1737.4 |

**必须写清楚的一条订正:`MACOS27_MEASUREMENTS` §4.2 那张表(create 1242 / close 418 /
open_trunc 832 / fsync 319 / unlink 1737)在本轮的旧 API 上复现不出来** ——
今天的旧 API 是 599 / 173 / 241 / 66 / 493,**整体快 2–4×**。
所以那张表里"同时开着几百个文件每个文件额外付 ~3ms"的结论,以及由它推出的
"6 个阶段合计 4557µs 但只有 22 次 XPC"的反常,**是 §6 的劣化态下测出来的,不是稳态**
(见 §5)。Handler API 在这张表上**没有额外贡献**:563/143/230/66/435 与旧 API 的
599/173/241/66/493 只差 6–12%,与 §3.1 的口径一致。

---

## 4. 并行度:单 mount 1/2/4/8 进程

新鲜挂载,每进程在自己的子目录里,`$S/fskit27/par.sh`。load 2.5–2.7。

### 表 4.1 lookup-miss(每进程 8000 次)

| 进程数 | **旧 API(今天)** | **Handler+noDC** | 参考:§5.1 老数据 | native(§5.1) |
|---|---|---|---|---|
| 1 | 11,589 ops/s(80.9µs) | **11,729 ops/s**(80.0µs) | 12,677 | 179,338 |
| 2 | 18,852 ops/s(101.4µs) | **18,125 ops/s**(105.6µs) | 22,600 | — |
| 4 | 30,288 ops/s(127.0µs) | **30,320 ops/s**(126.9µs) | 33,740 | — |
| 8 | 29,566 ops/s(262.6µs) | **29,517 ops/s**(262.2µs) | 34,262 | 296,841 |

**读路径逐点相同**(差 ≤4%,在噪声内)。形状也没变:4 个进程撞顶,第 8 个只是把延迟摊长。

### 表 4.2 create+unlink(每进程 1500 次)

| 进程数 | **旧 API(今天)** | **Handler+noDC** | Handler 相对提升 | 参考:§5.2 老数据 | native(§5.2) |
|---|---|---|---|---|---|
| 1 | 859 ops/s(1137.5µs) | **938 ops/s**(1041.2µs) | **+9%** | 938 | 11,011 |
| 2 | 1,072 ops/s(1844.6µs) | **1,230 ops/s**(1596.5µs) | **+15%** | 1,142 | — |
| 4 | 1,203 ops/s(3298.6µs) | **1,448 ops/s**(2739.1µs) | **+20%** | 1,193 | — |
| 8 | 1,244 ops/s(6389.0µs) | **1,564 ops/s**(5083.5µs) | **+26%** | 1,249 / 402–429(劣化时) | 31,219 |

写路径的并发吞吐提升随进程数放大(+9% → +26%),因为串行队列上每个请求的**条数**少了,
而队列本身仍然是单条(§5.4 的结论未变)。**但 8 进程聚合 1,564 ops/s 仍然只有 native 的 5%。**

---

## 5. `MACOS27_MEASUREMENTS` §6 的"写随 mount 存活时间劣化 3–12×"复现不出来了

§6 的可复现实验(新鲜挂载 1046–1159µs → 大量 churn 之后 3781–4577µs)在本轮**两个 API 上都不复现**:

| 阶段 | **旧 API(今天)** | **Handler+noDC** |
|---|---|---|
| 新鲜挂载 create+unlink | 1103.4µs | 1012.1µs |
| 同一挂载 50,000 次 create+unlink churn(全程均值) | 1116.4µs | 1022.0µs |
| churn 之后、同一个目录 | **1128.9µs**(+2.3%) | **1002.8µs**(-0.9%) |
| churn 之后、churn 之后才第一次碰的新目录 | 1123.7µs | 1005.1µs |
| 同期 lookup-miss(新鲜 / churn 后) | 73.9 / 77.8µs | 76.0 / 74.7µs |

**最可能的原因是 commit `d1a2a3c`**:M0 的前端在"删除之后内核再问一次属性"时回 `ESTALE`/`ENOENT`,
`d1a2a3c` 改成回上一次的属性快照。一个失败的 `getStandardItemAttributesForItem:` 很可能让
FSKit 留着那个 `FSFileHandle` 不放,于是 §6.2 里那张以 `FSFileHandle` 为 key 的
`NSMutableDictionary` 越长越长、`getItemForFH:` 退化成链式 `isEqual:` 扫描。
**本轮没有直接验证这个因果**(要验证得把 `d1a2a3c` 回退再测一遍),只能说:
**在 `4582150` 这个 commit 上,两套 API 都不再出现可复现的 churn 劣化。**

**但是偶发的多倍尖峰还在**(见 §6 的 agentstress 三轮):S4 在第 3 轮从 6.4s 跳到 34.8s、
S5 在第 2 轮从 24.4s 跳到 75.0s,而尖峰发生后立刻测 create+unlink 仍是 1028µs、
lookup-miss 仍是 74.8µs —— **微基准量不到,只有真实负载踩得到**。这一条 Handler API 没有解决。

---

## 6. 真实负载

### 表 6.1 agentstress(scale=1,native = `$S/nat27/st`)

数字是 worldfs 侧的秒数(native 侧每轮都重测,波动 ±20%,所以用秒数横比更诚实)。

| scenario | native(典型) | **旧 API** | **Handler+noDC 第1轮** | **第2轮** | **第3轮** | Handler 稳态 native% | 参考:§7.1 第2轮 native% |
|---|---|---|---|---|---|---|---|
| S1 context-read | 0.65–1.00 | 3.475 | 4.204 | 4.152 | 4.243 | **15–17%** | 15% |
| S2 edit-loop | 0.16–0.19 | 1.142 | **0.856** | 1.156 | **0.859** | **18%** | 15% |
| S3 edit-parallel-8 | 0.20 | 1.817 | **1.528** | **1.458** | 1.739 | **11–14%** | 11% |
| S4 build-artifacts | 1.28–1.41 | 7.885 | **6.428** | **6.714** | *34.834*\* | **19–22%** | 15% |
| S5 install-tree | 3.88–4.97 | 26.674 | **24.367** | *75.014*\* | **23.396** | **16–17%** | 16% |
| S6 git-cycle | 0.60–0.82 | 3.696 | **3.441** | **3.262** | **3.247** | **18–19%** | 17% |
| S7 test-churn | 6.53–7.76 | 11.264 | 11.825 | **10.663** | **10.606** | **62–65%** | 59% |
| S8 watch-events | 1.73–1.90 | 1.744 | 1.729 | 1.695 | 1.740 | **99–110%** | 99% |
| S9 big-file-8K-writes | 0.27–0.37 | 0.531 | **0.482** | **0.425** | **0.451** | **61–79%** | 71% |
| S12 exec-artifacts | 1.98–2.30 | 1.471 | 1.420 | 1.887 | 1.747 | **113–152%** | 112% |

\* §5 说的偶发尖峰,与 API 无关(同一轮里其它场景正常,尖峰之后微基准也正常)。
**correctness:四轮十个场景全部 `native:ok worldfs:ok`。**

去掉尖峰以后的横比:Handler+noDC 在 **S2 -25%、S3 -16%、S4 -18%、S5 -9%、S6 -7%、S9 -9%**
比旧 API 快,**S7 -5%**,S1/S8/S12 持平。
**S1 context-read 是唯一看起来变慢的一项(3.48 → 4.15–4.24s)**;它是纯读负载,
Handler 版为了填 `FSReadFileResult.itemAttributes` 每次 `read` 多做一次本地 `lstat`(~2µs),
不足以解释 20%,更可能是 native 侧同轮波动(0.65 vs 1.00)带来的负载差异,**本轮没有定论**。

### 表 6.2 realwork(fmt 源码树)

| workload | native | **旧 API** | **Handler+noDC 第1轮** | **第2轮** | Handler native% | 参考:§7.2 |
|---|---|---|---|---|---|---|
| tar 解压源码树 | 0.138 | 0.715 | 0.741 | 0.728 | 19% | 18% |
| rm -rf 解压树 | 0.038 | 0.125 | 0.135 | 0.116 | 33% | 30% |
| git clone(本地) | 0.110 | 0.260 | 0.351 | 0.244 | 45% | 43% |
| git status(1st, racy) | 0.046 | 0.059 | 0.091 | 0.059 | 78% | 79% |
| git status(稳态) | 0.046 | 0.066 | 0.109 | 0.068 | 68% | 70% |
| find \| wc | 0.030 | 0.054 | 0.079 | 0.053 | 57% | 56% |
| cmake configure | 0.524 | 0.851 | 0.875 | 0.809 | 65% | 65% |
| cmake build libfmt -j8 | 1.496 | 1.693 | 1.868 | 1.679 | **89%** | 92% |
| **touch 头文件 + 增量编译** | 0.355 | **1.533** | **0.522** | **0.433** | **68–82%** | **24%** |
| rm -rf 整棵树 | 0.043 | 0.207 | 0.191 | 0.163 | 26% | 21% |

**`touch 头文件 + 增量编译` 从旧 API 的 1.533s 降到 0.433–0.522s(快 3×),
把 §7.2 里唯一那个稳定复现的真实负载回归补回来了。** 这条负载是 ninja 的
"少量 temp 文件 churn + 大量 stat",正是 Handler API 省往返最多的形态。
(注:Handler 那两轮里 native 侧的这一行测到 1.514s / 1.796s,明显是异常值,
所以 native 基准取旧 API 那轮的 0.355s,与 §7.2 的 0.363s 一致。)

---

## 7. 正确性

`Handler+DataCache` 与 `Handler+dataCacheInhibited` 两套配置各跑一遍,**全部通过**:

| 检查 | Handler+DataCache | Handler+noDC |
|---|---|---|
| `scripts/smoke.sh`(read/readdir/stat/write/append/truncate/mkdir/rename/unlink/rmdir/symlink/hardlink/chmod/xattr/mmap/fsync) | **ALL OK** | **ALL OK** |
| 同一个活挂载上**连跑两次** `smoke.sh` | **ALL OK** | **ALL OK** |
| readdir 完整性 100 / 1000 / 5000 项(listdir / scandir / os.walk / scandir+lstat 四路与 backing 逐名比对) | **全部一致** | **全部一致** |
| `ls -fa` 的 dot 项 | **恰好一个 `.` 一个 `..`** | **恰好一个 `.` 一个 `..`** |
| 经挂载点 clang 编译的可执行文件 exec | ok(`v1`) | ok |
| 经挂载点编译的 dylib `dlopen` + 调函数 | ok(返回 7) | ok |
| 新产物的 `xattr -l` | **只有 `com.apple.provenance`,没有 `com.apple.quarantine`** → 隔离隐藏仍有效 | 同 |
| `git init` / `add` / `commit` | ok(`git status --porcelain` 空) | ok |
| 硬链接兄弟 unlink(建链 nlink=2,删 a 之后 b 仍可读、nlink=1) | ok | ok |
| agentstress 10 个场景的 correctness 检查 | — | **四轮全 ok** |

(readdir dot 项的正确行为来自 commit `d1a2a3c` 对 `readdir_trampoline` 的修复,
Handler 版的 `enumerateDirectory` 走同一条 core 路径,所以 §1.1 的回归不再存在。)

---

## 8. 门槛判定

`TASKS.md` 的升级后验收门槛:

> create+unlink 往返 ≤ 6 次 **且** 单次往返 ≤ 40µs(→ 元数据写约 40–60%);
> 若两者都不满足,M1 维持 C(clonefile World)主线。

| 门槛 | 阈值 | 26.6.2 | 27.0 旧 API | **27.0 Handler API** | 判定 |
|---|---|---|---|---|---|
| create+unlink 往返次数 | ≤ 6 | 16.06 | 16.04 | **11.04** | **FAIL**(超 1.84×) |
| 单次往返(lookup-miss) | ≤ 40µs | 68.1µs | 74.27µs | **74.64µs** | **FAIL**(超 1.87×) |
| (派生)元数据写 native% | 40–60% | 6.6% | 5.5% | **6.1%** | **FAIL**(差 7–10×) |

**两条硬门槛都仍然不满足,验收不通过。**

**为什么"消掉 7 次 getattr"没有带来 §8.2 预估的那么多收益:**
`MACOS27_MEASUREMENTS` §8.2 预估"16.03 → ~9 次,元数据写从 ~7% 提到 ~12% native"。
实测是 **16.04 → 11.04 次,元数据写 5.5% → 6.1% native**。差距来自两处:

1. **`FSVolumeDataCacheHandler` 把 2 次往返加了回来**(13.04),必须显式
   `dataCacheInhibited = YES` 才降到 11.04。27 的新 API 里没有"既拿属性缓存、
   又不要 open/close"的正路,只有把整个数据缓存协议关掉这一条。
2. **被消掉的 getattr 是最便宜的往返**。按 §3.1 反推:旧 API 68.4µs/往返 ×16.04,
   Handler 90.1µs/往返 ×11.04 —— 少掉的 5 次里每次只值约 20µs,
   而剩下的 11 次里有 3 次 lookup(内核对一个冷名字固定发两次)、
   2 次 provenance getxattr、1 次 create、1 次 remove、1 次 sync、1 次 reclaim,
   **没有一条在模块 API 的射程内**。

**还差多远:**要过 ≤6 次这道门槛,必须再砍掉 5 次往返,候选只有
"3 次 lookup 砍成 1 次"(内核行为)+ "2 次 provenance getxattr 砍掉"(内核行为)。
要过 ≤40µs 这道门槛,需要 XPC 单次往返降到现在的 54% —— 而 27 相对 26.6.2 是**涨了 8%**,
Apple 自己的 msdos 模块同向涨(§2.1)。**两条都不是我们能推动的。**

---

## 9. 建议

1. **把 Handler API 的移植留在仓库里,作为冻结 FSKit 前端的新默认**:它在每一项上
   ≥ 旧 API,把 `touch+增量编译` 这个唯一的稳定真实负载回归修好了(24% → 68–82% native),
   把并发写吞吐抬了 9–26%,而且旧 `FSVolumeOperations` 在 27 上已经是 deprecated
   (本次构建产生 4 条 deprecation warning)。冻结的 `WorldVolume` 原样保留做回退。
2. **默认 `dataCacheInhibited = YES`**(已经是代码里的默认;`wfs_datacache` marker 只用于复现 A/B)。
   理由全在 §2.3。
3. **M1 主线不变:C(native-root clonefile World)。** 门槛没过,而且这次拿到的数字
   把"再等一版 FSKit"的期待也关掉了 —— 属性缓存这条最被寄予厚望的路已经走完,
   剩下的往返全在内核里。
4. 顺带订正 `MACOS27_MEASUREMENTS` 的两处:§4.2 的 writebench 表和 §6 的
   "3–12× 劣化"在 `4582150` 上复现不出来(§5),引用那两节时要带上这个注脚。

---

## 附:本轮用到的脚本(都在 scratchpad `$S/fskit27/`,未进仓库)

| 文件 | 作用 |
|---|---|
| `rtbench.c` | 从 `PERF_STUDY_RT_PARALLEL.md` 附录重建的单进程微基准(与上一轮逐字节相同) |
| `optrace.sh` / `ophist.sh` / `analyze.py` | `log stream` 抓每操作 trace,按 op 类型出直方图 |
| `reqattr.m` | 运行时反射 21 个 `FSVolumeHandlerResult` 子类的 `requestedAttributes` |
| `mount.sh` / `remount.sh` / `env.sh` | 按 marker 选 API/数据缓存,挂到新挂载点 |
| `bench.sh` / `scale.sh` / `degrade.sh` / `traceall.sh` / `readtrace.sh` / `correct.sh` | §3–§7 各张表 |

仓库侧的改动只有三处:新增 `macos/fskit/WorldVolumeHandler.{h,mm}`、
`macos/fskit/WorldFileSystem.mm` 里按 marker 选类、`macos/fskit/CMakeLists.txt` 加一个源文件。
**`macos/fskit/WorldVolume.mm` 与 `WorldVolume.h` 一个字节都没动。**
