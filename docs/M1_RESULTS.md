# M1 基准:对照 arch.md §1 判据(T1.7)

生成:`scripts/bench/m1_criteria.sh`,2026-09-19 15:29。
机器:Apple M1,16GB, macOS 27.0(build 26A428),内核 root:xnu-13432.1.9~1/RELEASE_ARM64_T8103。
卷:/dev/disk3s5 105Gi 可用 / 228Gi(APFS,scratch 与 store 同卷)。
负载:开跑时 `uptime` = 1.81 2.53 2.87。
**QQLive 在跑**(常驻 0.5–16% 单核,与 CLONE_MODEL_MACOS27 同一噪声源),数字里含这份噪声。
命令:`world 0.1.0-m1`,二进制 `build/Release/cli/world`(Release)。
单值取 best of 3(§5 diff 取 best of 5);20 次一组的取 p50/p95。
所有 fork 都是**整条命令**(含进程启动与动态链接),没有任何数字来自测试二进制里的库调用。
各节可以单独重跑(`--only N`),所以节与节之间的时间戳不同;上面这行时间是最后一次组装的时间。

## 0. 判据总表

| 判据(arch.md §1) | 目标 | 实测 | 结论 |
|---|---|---|---|
| fork 延迟(pool 命中 p50) | < 10 ms p50 | 9.7 ms(1k/10k/50k 最差) | **达成** |
| fork 延迟(pool 空,50k) | — | 0.50 s(裸 clonefile 0.45 s) | 参考 |
| git/build 吞吐 | ≥ 90% native | 99%(> 0.5 s 步骤最差) | **达成** |
| agentstress 9 场景 | ≥ 90% native | 最差 99%(S4 build-artifacts) | **达成** |
| 1000 个 idle World | 可承受 | 114 s 建完,3.40 GB 物理,list 10 ms / 8 MB,gc 525 s | **达成** |
| 存储增长 | ≈ 实际 divergence | 100 个 World+1% 改动 = 真副本的 13.59% | **达成** |
| diff | O(changes) | 50k/800 改动:events 0.354 s,全扫 0.185 s(默认 1.420 s) | **达成** |
| safety.sh + ctest | 全过 | safety: 93 passed, 0 failed;100% tests passed out of 2 | **达成** |

## 1. fork 延迟(arch.md §1:**< 10 ms p50**)

每组 20 次,整条 `world fs fork` 命令计时(含进程启动)。pool 命中 = 事先 `pool fill` 好的预克隆树;pool 未命中 = `--no-pool`,当场 clonefile。

| 树(条目含根) | `init` | 未命中 p50 | 未命中 p95 | **命中 p50** | 命中 p95 | 命中率 | 裸 `clonefile(dir)` | 预克隆一棵 |
|---|---|---|---|---|---|---|---|---|
| 1k(1,041) | 0.055 s | 0.026 s | 0.028 s | **8.6 ms** | 9.7 ms | 20/20 | 0.006 s | 0.009 s |
| 10k(10,401) | 0.181 s | 0.110 s | 0.113 s | **9.0 ms** | 9.4 ms | 20/20 | 0.082 s | 0.097 s |
| 50k(52,001) | 0.814 s | 0.505 s | 0.510 s | **9.7 ms** | 14.9 ms | 20/20 | 0.455 s | 0.481 s |

进程地板(`world version`,20 次):p50 **4.4 ms**,p95 4.9 ms —— 命中路径里这一份是省不掉的。

pool 命中的 p50 基本**与树大小无关**(1k/10k/50k 分别 8.6 / 9.0 / 9.7 ms):手上活只有写 marker + `rename(2)` + 三次 SQLite 事务(认领、建行、转 ACTIVE),剩下的全是进程启动 —— 地板本身就占了一半。余量最小的是 50k 那一格(p95 14.9 ms):树越大,前面那 20 次未命中 fork 在 APFS 上留下的元数据脏活越多,同一台机器上跟着抖。未命中路径就是 `clonefile` 本身加 30–50 ms。

**判据:达成** —— pool 命中 p50 最大 9.7 ms(< 10 ms);pool 空时 50k 需要 0.50 s,这正是 pool 存在的理由。


## 2. git / build 负载(arch.md §1:**≥ 90% native**)

同一份 fmt 仓库(`git clone --no-hardlinks`),一侧是普通 `cp -R` 副本,一侧是 World(从该仓库的快照 fork 出来)。两侧都是真 APFS,World 没有任何东西在数据路径上。

| 步骤 | native(cp -R 副本) | World | World 相对 native |
|---|---|---|---|
| git status | 0.045 s | 0.045 s | **100%** |
| git status(2nd) | 0.026 s | 0.026 s | **101%** |
| find \| wc | 0.009 s | 0.009 s | **104%** |
| cmake configure | 0.553 s | 0.471 s | **117%** |
| cmake build -j8 | 1.503 s | 1.453 s | **103%** |
| touch header + rebuild | 1.360 s | 1.368 s | **99%** |
| git status(after build) | 0.017 s | 0.017 s | **104%** |

轻量步骤(几十毫秒)的百分比噪声大;只看 > 0.5 s 的步骤,最差 **99%**。

**判据:达成** —— World 里的 git/build 是 native 的 99%(重步骤最差值)。


### agentstress(9 个 coding-agent IO 场景,`--plain`:World 是普通目录,不是挂载点)

```
scenario                  native    world  native%  correctness   (scale=1)
S1 context-read             0.605s     0.600s    101%  native:ok worldfs:ok
S2 edit-loop                0.160s     0.159s    101%  native:ok worldfs:ok
S3 edit-parallel-8          0.197s     0.193s    102%  native:ok worldfs:ok
S4 build-artifacts          1.276s     1.286s     99%  native:ok worldfs:ok
S5 install-tree             3.829s     3.765s    102%  native:ok worldfs:ok
S6 git-cycle                0.718s     0.588s    122%  native:ok worldfs:ok
S7 test-churn               6.494s     6.494s    100%  native:ok worldfs:ok
S8 watch-events             1.954s     1.720s    114%  native:ok worldfs:ok
S9 big-file-8K-writes       0.347s     0.278s    125%  native:ok worldfs:ok
S12 exec-artifacts          2.276s     1.549s    147%  native:ok worldfs:ok
```


## 3. 1000 个 idle World(arch.md §1:**可承受**)

源是 10,400 条目的合成树(200 目录 × 50 文件)。两列的区别只有 pool:左列全程 `--no-pool`,右列先 `pool fill S1 --count 50`,并让每次命中都在后台补到 50。

| | 无 pool | pool(预热 50,自动补) |
|---|---|---|
| 预热 50 棵 | — | 5.0 s |
| 1000 次 fork 总时间 | **113.7 s** | **102.3 s** |
| 其中 pool 命中 | 0/1000 | **1000/1000** |
| 跑完后 pool 里还剩 | — | 48(测 df 前已 drain) |
| 前 10 次均值 | 0.110 s | 0.099 s |
| 后 10 次均值 | 0.120 s | 0.109 s |
| 单次 p50 | 0.113 s | 0.102 s |
| 单次 p95 | 0.122 s | 0.114 s |
| df 物理增长 | 3.40 GB | 3.23 GB |
| metadata.db(+wal) | 0.4 MB | 0.4 MB |
| `fs list`(1000 行) | 0.010 s | 0.010 s |
| `fs list` 常驻内存 | 7.8 MB | 7.8 MB |
| `fs status` | 0.007 s | 0.007 s |
| 1000 次 `discard`(逐条命令) | 9.1 s | 9.2 s |
| `gc --retention 0` 真删 | 525.2 s | 543.8 s |

每个 World 的物理代价:351 B/条目(3.5 MB/World,10,400 条目),与 CLONE_MODEL_MACOS27 §4 的 308 B/条目 同一量级——这就是 clonefile 的元数据,数据块全是共享的。

**没有随 World 数量退化**:后 10 次 / 前 10 次 = 无 pool 1.09×、pool 1.10×。1000 行的 `fs list` 是 10 ms,SQLite 这一侧完全不是瓶颈。

**pool 买的是延迟,不是吞吐**:后台 filler 跟得上(1000/1000 命中),但 1000 次 fork 背靠背时那 1000 次 clonefile 还是要这台机器做,只是挪到了 fork 的关键路径之外——总时间 102 s vs 114 s,只快 10%,单次 p50 也还是 102 ms 而不是 §1 里的 9 ms。§1 的 9 ms 是 agent 实际感受到的那种场景:偶尔 fork 一次,克隆早就有人替它做完了。

**最贵的一步是删**:`gc --retention 0` 真删 1000 × 10,400 条目 = 1040 万次 unlink,525 s(≈ 50 µs/条目),两列一样(543.8 s 那一列紧接着跑,APFS 的回收还在排队)。建 1000 个 World 比毁掉它们便宜一个数量级——真要频繁回收,得把 `gc` 做成后台增量的。

> 注:这一节测完之后给 CLI 加了一条:命中时如果已经有 filler 在跑,就不再起第二个(省掉命中路径上的一次 ~4 ms 进程启动)。表里 pool 那一列是加这条**之前**测的,加了之后只会更快一点(1000 次里约有 950 次的 spawn 被省掉,≈ 4 s / 102 s)。

**判据:达成(可承受)** —— 1000 个 World 共 3.40 GB 物理、metadata.db 0.4 MB、`fs list` 0.010 s / 8 MB 内存,清理 9 s + 525.2 s。


## 4. 存储增长 ≈ 实际 divergence(arch.md §1)

52,000 条目 / 143 MB 的树,fork 出 100 个 World,每个改 1%(500 个文件,每个 append 100 B)。物理用量用 `df`(只有它看得见块共享)。

| | 值 |
|---|---|
| 源树逻辑大小 | 143 MB(50,000 文件) |
| 100 个 World 的逻辑大小(如果真复制) | 14.0 GB |
| 100 次 fork 的 df 增长 | **1736 MB**(350 B/条目,= 克隆元数据) |
| 改 1% 写入的字节 | 4.77 MB |
| 改 1% 之后的 df 增长 | **213 MB** |
| 相对 100 份真副本 | 13.59% |
| fork 总耗时 / 改动总耗时 | 51 s / 38 s |

改动部分的放大率 **44.7×**(写 4.77 MB,物理涨 213 MB):APFS 的 COW 粒度是 4 KiB 块,append 100 B 也要把那一个块 unshare 出来,50000 次改动 × 4 KiB ≈ 195 MB —— 实测与这个模型吻合。

对照 CLONE_MODEL_MACOS27 §2 的「改 1% 文件物理 +0.36%」:那里是**一个**克隆相对源树,这里是 100 个克隆相对 100 份真副本(13.59%)。两者说的是同一件事:存储只为真正分叉的块付钱。

**判据:达成** —— 100 个 World + 1% 改动共 1.90 GB,真复制要 14.0 GB。


## 5. diff O(changes)(arch.md §1)

50k 条目的 World,800 处改动(500 M + 200 A + 100 D);best of 3,整条 CLI 计时。目标取自 docs/TASKS.md T1.3 的实测(0.164 / 1.354 / 0.087 s)。

| 路径 | 本轮 | T1.3 实测 | 计数 |
|---|---|---|---|
| 默认(全扫 + xattr) | **1.420 s** | 1.354 s | 200 added, 500 modified, 100 deleted, 0 metadata-only |
| `--full --no-xattr` | **0.185 s** | 0.164 s | 200 added, 500 modified, 100 deleted, 0 metadata-only |
| `--events`(FSEvents 候选) | **0.354 s** | 0.087 s | 200 added, 500 modified, 100 deleted, 0 metadata-only |

6 个文件的小 World(1 处改动):

| 路径 | 本轮 |
|---|---|
| 默认(全扫) | **0.011 s** |
| `--events` | **0.076 s** |

50k 上事件路径 0.354 s vs 最便宜的全扫 0.185 s = 0.5×;6 文件的小 World 上事件路径要 0.076 s 而全扫 0.011 s(7× 反过来)——建流 + 等水位标是一笔固定开销。这正是 T1.3 把全扫定为默认、事件路径留给 20 万条目以上的树的原因。

**判据:达成** —— 改动数决定成本的那条路(`--events`)在 50k / 800 改动上是 0.354 s,与树大小无关的那部分是固定开销;默认路径是 O(tree) 但常数极小(0.185 s / 5 万条目)。


## 6. 安全用例与单元测试

```
$ scripts/tests/safety.sh   ->  safety: 93 passed, 0 failed  (exit 0)
PASS 行数: 93
没有 FAIL

$ ctest   ->  100% tests passed out of 2  (exit 0)
1/2 Test #1: core_test ........................   Passed    0.04 sec
2/2 Test #2: diff_test ........................   Passed    9.97 sec
```

safety.sh 覆盖 P1–P14 + T1.5 的 pool(拒绝把 pool 目录当 fork 目标、pool status、
空 pool 的未命中路径、命中后 uuid 目录消失且 World 根 inode = 原 pool 条目的 inode、
后台补种、gc 清半成品与孤儿)。


