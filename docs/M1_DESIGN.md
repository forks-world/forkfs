# M1 设计:clonefile World(方案 C)

依据:`docs/REDIRECT_EXPERIMENT.md`、`docs/CLONE_MODEL_MACOS27.md`、`docs/MACOS27_MEASUREMENTS.md`。
决定(2026-09-19,用户确认):M1 主线切到 clonefile World;FSKit 前端冻结为实验性备选。
**设计一等目标:性能等于 native,且用户/agent 很难把自己搞坏。**

## 1. 模型

```text
Snapshot  S<n>   不可变的整树快照(init / checkpoint 的产物),内部路径,根目录 0000 gate 保护
                 (`--hard` 改为 per-file uchg;T1.1b)
World     W<n>   可写工作区 = 某个 Snapshot 或 World 的 clonefile 克隆,用户可见路径(默认 ~/worlds/W<n>/<name>)
DAG              SQLite:谁从谁 fork、来源快照、fork 时的 FSEvents id
```

四个原语:

| 原语 | 实现 | 成本(27.0 实测) |
|---|---|---|
| `init <dir>` | `clonefile(dir)` → S1;原目录不动、不接管 | 50k 条目 0.79s(clone + manifest walk;保护是一次 chmod) |
| `fork [--from W\|S]` | 从 pool 领预克隆的 World(快照来源)或当场 `clonefile(dir)`(活 World 来源) | pool 命中 <10ms;当场 0.07s/10k、0.37s/50k |
| `checkpoint W` | `clonefile(W.root)` → S<n+1>,W 继续可写 | 同上 |
| `diff W` | FSEvents(since fork id)候选 → 与来源快照 stat/内容比对;Dropped/MustScanSubDirs 时全树 walk | O(changes);全扫 0.8s/50k |
| `discard W` / `discard S`(T2.2) | rename 到 trash,保留期后由后台 collector 真删(T2.1) | rename 毫秒级;真删 4 线程 ~37k 条目/s |

数据面零介入:World 里所有读写都是原生 APFS。

## 2. 存储布局

```text
<store>/                         默认 ~/Library/Application Support/World/fs/;必须与项目同一 APFS 卷
├── VERSION                      schema 版本;新版本拒绝旧 core 打开
├── metadata.db                  SQLite WAL
├── snapshots/S<n>/root/         gate 保护:根目录 0000(`--hard` 时改为逐条目 uchg)
├── locks/W<n>.lock              `world exec` 的锁(pid + flock,P5)
├── locks/pool.lock              后台 filler 的 store 级锁(T1.5)
├── locks/gc.lock                后台 collector 的 store 级锁 + 进度(pid/start/done/remaining;T2.1)
├── tmp/                         `world exec` 生成的 seatbelt profile
├── pool/S<n>/<uuid>/            预克隆的 World,尚未分配(无 marker、无 worlds 行;T1.5)
├── logs/pool.log                后台 filler 的输出(T1.5)
├── logs/gc.log                  后台 collector 的输出,每次唤醒一行(wall/cpu;T2.1)
├── trash/W<n>-<ts>/             discard 后的 World,保留期(默认 7 天)内可 restore
├── trash/S<n>-<ts>/             discard 后的 Snapshot(T2.2),gate 仍关着,由 deleter 开到 0700
├── trash/*.deleting/            collector 已经开始删的条目;restore 一律拒绝(T2.1)
└── worlds/W<n> -> <user path>   仅记录,World 真身在用户路径
~/worlds/W<n>/<name>/            World 根;含 .world 标记文件
```

store 目录加 `.noindex`(Spotlight)并 `tmutil addexclusion`(Time Machine),无需 root。

## 3. 防出错设计(逐条,每条对应 safety 测试)

| # | 风险 | 设计 |
|---|---|---|
| P1 | World 目录被移动/改名 | 身份不靠路径:根目录 `.world` 标记(world id、store id、snapshot id)+ metadata 记 dir inode。命令执行时按 inode 反查并自动修正路径 |
| P2 | World 被 `cp -R` 复制出未登记副本 | 标记存在但 inode 不符 → 视为"未登记副本",所有破坏性命令拒绝,提示 `world fs adopt` 或 `fork`。**收编带着基线**(PR #1 review 第六轮):副本的 marker 里写着原世界的 snapshot,`adopt` 把它写进新行,所以那个 snapshot 不是 ACTIVE 时(已 discard、正在 discard、已不在 store)`adopt` 以 `WFS_E_SOURCE_GONE` 拒绝——检查和插入在同一个 `BEGIN IMMEDIATE` 里,也就是 `discard S<n>` 数引用的那把写锁 |
| P3 | 快照被改动,污染后代 | **默认**:快照根目录 `chmod 0000`(gate),里面的条目一律不动;clonefile 期间才临时开到 0500,由 `manifest` 上的 flock 串行化。fork 因此不需要 unprotect 遍历(T1.1b)。`--hard`:每个文件/目录 `chflags uchg` + 目录去掉写位,fork 时在克隆上并行 `nouchg`。`world fs verify S<n>` 用清单校验两种模式,gate 模式额外检查根是否被留成敞开 |
| P4 | 误删 World / Snapshot | `discard` = rename 进 trash(一次 rename,毫秒级,与树大小无关),`world fs restore W<n>` 可恢复;保留期后由**后台 collector** 真删,`--now` 才在前台立即删。**T2.2 起 Snapshot 走同一条路**:`world fs discard S<n>`,有 ACTIVE World 引用时拒绝(`--force` 也不行,绝不让 World 失去 diff/verify 的基线),有 pool 条目时 `--force` 先 drain;trash 里的 World 不算引用,但它之后 `restore` 会以 `WFS_E_SOURCE_GONE` 拒绝。**崩溃安全(T2.1)**:真删之前先 rename 成 `<name>.deleting`,被 kill 的 worker 留下的东西一眼就不是 World,`restore` 以 `WFS_E_TRASH_DELETING` 拒绝,下一次唤醒继续删完。**崩溃安全的另一半(PR #1 review 第五轮):discard 本身是三步,行先写、树后搬**——(a) 一个事务:引用检查 + `state=WFS_ST_TRASHING` + `trash_path` 写成**树即将拥有的那个名字**,提交;(b) rename;(c) 一个事务:`state=TRASHED`。中间任何一刻被 kill,树只可能在**老位置或 `trash_path` 两者之一**,`lstat` 就能断定:还在老位置 → 回 ACTIVE(discard 没发生),已在 trash → 完成到 TRASHED,**除非还有 World/pool 引用这个 snapshot,那就搬回去**(第六轮起 `adopt` 也在写锁下拒绝非 ACTIVE 的 snapshot,所以这里的引用计数是**双保险**:按现在数,而不是按"谁都不可能再加引用"这个论证信)。恢复在**每次 store open 和每次 gc 开头**跑(两条走索引的 SELECT,平时一行都不返回)。配套:**trash 里的目录只有"任何状态下都没有行提到它"才算无主孤儿**,`.deleting` 拼写算同一个名字。`--now` 对**已经在 trash 里的 snapshot** 也成立:把删除提前,行落 DEAD(和 World 一致);不带 `--now` 才是 `-EALREADY`。**`restore` 是同一套三步反着跑(PR #1 review 第七轮)**:(a) 一个 `BEGIN IMMEDIATE` 里重读——行必须 TRASHED、树没在被删、基线必须 ACTIVE(否则 `WFS_E_SOURCE_GONE`)——并把行写成 `WFS_ST_TRASHING`;(b) rename 回家;(c) 一个事务写回 ACTIVE。于是**一个 TRASHING 的 World 行是快照的硬引用**:从那里看不出它是在去 trash 的路上还是在回家的路上,两个方向都不能丢基线,`discard S<n>` 一律拒。崩溃由同一个 `trashing_recover()` 按 lstat 收尾(树在 `trash_path` → TRASHED,树在家 → ACTIVE);世界的 trash 永远与世界同卷,所以 `dir_dev`/`dir_ino` 不必重新 stat。**一条 TRASHING 行带主人(PR #1 review 第八轮)**:(a) 把 pid 和该进程自己的启动时刻写进 `owner_pid`/`owner_start`(和 CREATING 行同一对列、同一套 `producer_alive()` 判活),(c) 和 undo 清掉。`trashing_recover()` **跳过主人还活着的行**——`lstat` 分不清"死在 rename 之前"和"还差一微秒就 rename",而每一次 `world fs ...` 都会 open store、每一次 open 都跑这套恢复,所以脚本里的下一条命令就足以把一个**正在进行**的 discard 判成没发生:行回 ACTIVE、`trash_path` 清空,discard 随后把树搬进 trash、(c) 改不到任何行却返回 0——一条 ACTIVE 行,一棵**无主孤儿**树,下一次 collector 立刻删掉;`restore` 是镜像。配套:(c) 和 restore 的收尾 UPDATE 都查 `sqlite3_changes()`,**改不到行就不算成功**——行和树在这之后绝不许互相矛盾。**collector 对行的每一次写也都是条件写(第八轮)**:`WHERE id=? AND state=2 AND trash_path=?`(排队时那条行的样子),重命名成 `.deleting` 之前还在 store 锁下再读一次行;`restore` 抢先把树搬回家时,collector 的 `-ENOENT` 意思是"搬回家了"而不是"已经被删了",原来那条无条件的 `mark_dead()` 会给一个刚刚恢复、就在家里的 World 写上 DEAD |
| P5 | 对正在使用的 World 做 discard/checkpoint/fork | `world exec` 写 `<store>/locks/W<n>.lock`(pid + flock;`.world` 在实现里是文件不是目录,而且锁不该进项目树);discard / checkpoint / 从该 World fork 遇到活锁一律 `WFS_E_WORLD_BUSY`,`--force` 继续;pid 已死的锁静默清理 |
| P6 | 跨卷 clonefile EXDEV(`st_dev` 相同也可能) | init 前实际探针克隆一个临时文件;失败则明确报错,提供 `--store <同卷路径>` 或 `--copy`(真实复制,提示耗时) |
| P7 | 在危险路径上 init/fork | 拒绝 `/`、`$HOME`、store 自身、已是 World/Snapshot 的目录、另一个 World 内部;fork 目标不能在任何 World 或 Snapshot 内 |
| P8 | fork 中途崩溃留下半棵树 | 克隆到**目标父目录里一个抽出来的临时名** `.wfs-fork-<pid>-<计数>-<getentropy 16 位十六进制>` → 成功后 rename(`renameatx_np` + `RENAME_EXCL`,目标已存在就是 EEXIST,绝不覆盖)→ 再写 metadata(arch.md §27 的 publish 顺序)。**这个名字在克隆开始之前先写进 CREATING 行的 `tmp_path`**:父目录是用户的,不能靠后缀猜谁是我们的东西,崩溃之后**行是那棵树唯一的名字来源**。gc 只删 CREATING 行记下的那一条路径(再核对:仍是目录、没有活行认领它的 inode、`.world` 标记若在必须写着这一行的 world id),删完把行标成 DEAD;按后缀扫目录只剩 store 内部(`<store>/snapshots/S<n>.wfs-tmp`、`<store>/pool/S<n>/<uuid>.wfs-tmp`、trash 的 `.deleting`),用户目录一律不扫。**且只在生产者确实死了之后**:CREATING 行记 `owner_pid` + 该进程的启动时间(pid 复用检测),生产者还活着的行 gc 一律不碰,死了还要再过 `WORLD_GC_CREATING_MIN_AGE`(默认 60 s);fork 最后那条 UPDATE 校验 `sqlite3_changes()==1`,行没了就把树搬回临时名删掉并返回 `-ESTALE`,绝不留一个没人登记的目录在 `--to` 上 |
| P9 | 树内硬链接被克隆断开 | **T2.5 已修复**。init/checkpoint 扫源树的那一趟顺带按 (dev, ino) 分组:全部名字都在树内的组写进清单(`#hl` / `hl` 行,老读者一律跳过),有名字在树外的组只计数(克隆里没有可链接的对象)。之后每一次克隆——fork、checkpoint、pool 填充——在 rename 之前重放这些组:第一个名字是正身,其余 `link` 到它再 `rename` 覆盖(名字一刻也不消失,崩溃只留 `.wfs-tmp`)。代价 O(硬链接数),实测 0.27 ms/条(4 线程);pool 条目在填充时就做完,命中仍是 O(1)。从活 World fork 时用来源快照的组当候选,逐个名字在活树上 `lstat` 核对后才动手。**清单必须自洽(PR #1 review 第八轮)**:`#hl` 头里的组数和名字数写在它描述的那些行之前,读回来时必须对得上,且没有任何一组少于两个名字(扫描从不写这种组——名字不全在树内的组只计外部数)。否则 `-EINVAL` → `WFS_E_SNAPSHOT_DIRTY`。原来只留头里的外部计数,于是**最后一个成员没落盘**的清单读回来组数照旧、某一组只剩一个名字,而一个名字的组被重放**悄悄跳过**:fork 出来是两个独立文件,而快照记的是一个 inode 两个名字 |
| P10 | diff 漏报 | 事件只当候选,最终以 stat/内容比对为准;收到 Dropped/MustScanSubDirs 立即全树 walk;`diff --full` 强制全扫 |
| P11 | 磁盘写满 | fork/init 前检查剩余空间 ≥ 条目数 × 1KB + 阈值;`world fs status` 用 df 差值报告真实占用(du 看不出块共享) |
| P12 | 并发命令互踩 | metadata 用 `BEGIN IMMEDIATE`;每条命令幂等;World 级操作先拿 flock |
| P13 | 用旧版 CLI 打开新 store | `VERSION` 文件 + schema 版本检查,拒绝并提示升级 |
| P14 | agent 越界写 | `world exec` 套 seatbelt profile:允许写 World 根、agent 配置/缓存目录、tmp;拒绝 store、快照、其他 World。不带沙盒的 agent 也至少得到 P3 的快照保护 |
| P15 | 原地改写大文件的 COW 首写惩罚(~1ms) | 文档说明;不做特殊处理 |
| P16 | gc 与前台争抢(T2.1) | 物理删除是全系统最贵的操作(实测 1000 个 10k 树 = 1040 万次 unlink)。它只在**游离的后台 worker** 里做:store 级非阻塞 flock `<store>/locks/gc.lock` 保证全店一个;每次唤醒只做有限一批(N 条目或 T 秒)就退出,还有活就交给新起的后继进程;4 线程 unlink 比单线程快 1.9×,但实测会让并发 `fork` 慢 51–57%,**真正管用的是占空比**——干 2 s 停 2 s,前台代价降到 ~5%,drain 变成约两倍时长(背景工作,等得起)。`setiopolicy_np(IOPOL_THROTTLE)` 实测毫无作用:瓶颈是 APFS 元数据事务,不是磁盘带宽 |
| P17 | store 有树但 `metadata.db` 没了(M2) | **拒绝打开,绝不重建**。id 是从数据库里发的:新建一个空库就会再发一次 1,下一次 `init` 于是往已经在磁盘上的 `snapshots/S1` 上写。`wfs_store_open` 在创建任何东西之前先看:`metadata.db` 不存在、是空文件、读不了、或者打开后连 pragma 都执行不了(不是一个数据库),而 `snapshots/` / `trash/` / `pool/` 里还有条目 → `WFS_E_STORE_DAMAGED`。检测只是对这三个目录各做一次 readdir,快照的 gate 一路关着也不影响(不需要进 `S<n>/root`)。CLI 把话说全:这些树就是那个数据库的索引、id 会撞车、**`gc --reconcile` 帮不上忙**(它要读数据库才知道哪些行的树没了,而这里没的正是数据库),出路只有两条——从备份恢复 `metadata.db`,或者把整个目录挪开(`mv <store> <store>.damaged`)重开一个。空目录不算损坏,那是新 store |

## 4. C ABI 变化

`core/include/worldfs/worldfs.h` 保留 `wfs_store_*`、`wfs_world_*`、`wfs_diff`;新增 snapshot/pool/trash/exec-lock/verify 函数;
`wfs_view_*` 与 namespace 操作移到 `worldfs_fskit.h`(仅 FSKit 前端使用,CMake 选项 `WFS_FSKIT=OFF` 默认不编)。
平台层新增:`fs_clone_tree`(clonefile(dir),EXDEV/ENOTSUP 降级到 4 线程逐文件)、`fs_protect_tree`/`fs_unprotect_tree`(并行 chflags)、
`fs_clone_probe`、`fs_events_*`(Darwin)、`fs_free_space`。Linux 对应层留接口(overlayfs)。

## 5. 任务拆分

- [x] T1.1 core 重构:schema v2、snapshot/world/pool/trash 生命周期、平台原语、P6/P8/P12/P13
- [x] T1.2 CLI:init/fork/checkpoint/list/inspect/discard/restore/gc/status/verify/adopt,错误信息可读,P1/P2/P4/P7/P11
- T1.3 diff:FSEvents 层 + 全扫回退 + 比对,P10;输出格式同 arch.md §25
- [x] T1.4 exec:cwd/env/lock/seatbelt profile,P5/P14
- [x] T1.5 pool:预克隆(`world fs pool fill/status/drain`;命中后由 fork 自己起一个游离的后台 filler,
      不用 launchd agent —— 没有常驻进程要管,store 级 flock 保证只有一个 filler)
- T1.6 safety 测试套件 `scripts/tests/safety.sh`:P1–P14 每条一个用例,全部必须过
- [x] T1.7 基准:`scripts/bench/m1_criteria.sh` → [`docs/M1_RESULTS.md`](M1_RESULTS.md),六节全部对照 arch.md §1
- T1.8 文档:arch.md 增补测量结论与转向章节;README 改写

顺序:T1.1 → T1.2 → T1.3/T1.4/T1.5 → T1.6/T1.7 → T1.8。

## 6. 对 arch.md §1 判据的预期

| 判据 | 预期 | T1.7 实测(docs/M1_RESULTS.md) |
|---|---|---|
| fork < 10ms p50 | pool 命中时满足;pool 空时 0.07–0.4s(10k–50k 文件) | 命中 8.7–9.3 ms(与树大小无关),未命中 0.025/0.111/0.514 s |
| git/build ≥ 90% | 99–102% | 99–117%(重步骤最差 99%),agentstress 10 场景 99–147% |
| 1000 idle branches | 1000 个目录,元数据约 15GB(5 万文件树) | 1 万条目树 × 1000 = 3.40 GB 物理、351 B/条目;`fs list` 10 ms;`gc` 真删要 525 s |
| 存储 ≈ divergence | 改 1% 文件物理 +0.36% | 100 个 50k World + 各改 1% = 100 份真副本的 13.6%(克隆元数据占大头) |
| diff O(changes) | FSEvents 正常时满足;丢事件时退化为 O(tree) | 事件路径只比对 800 个候选(0.354 s);但全扫常数太小(0.185 s),默认仍是全扫 |

## 7. 快照保护的修正(2026-09-19,T1.1b)

per-file `uchg` 实测 50k 文件保护 0.68s、解保护 0.73s(4 线程已是 APFS 元数据事务上限),fork 一半时间花在解保护上。
改为**门目录保护**:快照根目录 mode 0000,内部文件不动;只在 clonefile 窗口内临时 0500。保护不会被克隆进 World,
fork = clonefile + 标记 + rename。per-file `uchg` 保留为 `--hard`。威胁模型是误操作,两种方式都挡不住 owner 主动解除。

实现(T1.1b 已完成,T1.3 合并后 `SnapGate` 搬到 `core/src/snapshot_access.{h,cpp}`):
开关门在 core 里(`SnapGate`),不在 CLI —— 这样每个调用方(fork / verify / diff / 将来的 pool)
都拿到同一套串行化;`diff` 经 `snapshot_open_for_read()` 用的就是这把门,且**只在比对阶段开**
(建流、排序、回调都在门外),因为门开着的时候同一快照的 fork 要等这把 flock。窗口用快照 `manifest` 文件上的独占 flock 保护,跨进程串行;克隆出来的根会被 chmod 回源树自己的
mode(记在 `snapshots.root_mode`),所以 World 里看不到 0500 的痕迹。实测见 docs/TASKS.md 的 T1.1b 一节。

## 8. 其他平台对应层(结论记录)

| | macOS | Linux | Windows |
|---|---|---|---|
| fork | `clonefile(dir)`,O(files) 但 7µs/文件 | overlayfs upper 目录,O(1) | ReFS 块克隆逐文件(需 Dev Drive);或 ProjFS 惰性投影,O(1) |
| changed-set | FSEvents(候选)+ 比对 | upper 目录,内核维护 | USN Journal(内核维护,不丢)或 ProjFS placeholder 状态 |
| 快照保护 | 门目录 0000 / `uchg` | lower 只读 | 只读属性 + ACL |
| 隔离 | seatbelt / uid | mount namespace | 每 agent 用户 + ACL;AppContainer |
| 前提 | 同一 APFS 卷 | XFS reflink / btrfs(否则 copy-up 整文件) | Windows 11 + Dev Drive(ReFS);Win10 已停服,不单独测 |

Windows 测试基线:一台 Windows 11 24H2,C:(NTFS,测 ProjFS)+ 一个 Dev Drive VHDX(ReFS,测块克隆与 Defender 豁免)。
