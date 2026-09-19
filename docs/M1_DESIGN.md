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
| P1 | World 目录被移动/改名 | 身份不靠路径:根目录 `.world` 标记(world id、store id、snapshot id)+ metadata 记 dir inode。命令执行时按 inode 反查并自动修正路径。**标记里那条 store 路径(T2.3)是一个判决,不是提示(PR #1 review 第十七轮)**:`-o` 选项到不了 FSKit 模块,所以扩展是从挂载源根目录的 `.world` 里读 store 路径的,而且**只要非空就直接用**,一个路径都没拿到时才退回容器默认值。于是 store 搬过家之后,`world fs mount` 原来那句"扩展会退回容器默认值"是假的——挂上来的会是 marker 指着的那个 store。现在 `wfs_world_marker_store()` 报三件事(带不带路径、store id 是不是本店的、路径**解析之后**是不是本店的目录),`mount` 在非空且不是本店时拒绝并把两个路径都说出来;出路是 `world fs verify <world> --refresh-marker`(`wfs_world_marker_refresh()`):**只改路径**,world id、名字、来源 snapshot、`created_at` 原样,按 P12 拿 World 锁,新 marker 写在旁边再 rename 盖上去 |
| P2 | World 被 `cp -R` 复制出未登记副本 | 标记存在但 inode 不符 → 视为"未登记副本",所有破坏性命令拒绝,提示 `world fs adopt` 或 `fork`。**收编带着基线**(PR #1 review 第六轮):副本的 marker 里写着原世界的 snapshot,`adopt` 把它写进新行,所以那个 snapshot 不是 ACTIVE 时(已 discard、正在 discard、已不在 store)`adopt` 以 `WFS_E_SOURCE_GONE` 拒绝——检查和插入在同一个 `BEGIN IMMEDIATE` 里,也就是 `discard S<n>` 数引用的那把写锁。**`--refresh-marker` 不是收编(PR #1 review 第十七轮)**:它只改 store 路径,而且只对这个 store 真正拥有的 World 动手(marker 的 store id 是本店的、行和 inode 都对得上),所以未登记副本是 `WFS_E_UNREGISTERED`、别人 store 的 World 是 `WFS_E_FOREIGN_STORE`——两样都还是 `adopt` 的事,marker 一个字节都不会被改 |
| P3 | 快照被改动,污染后代 | **默认**:快照根目录 `chmod 0000`(gate),里面的条目一律不动;clonefile 期间才临时开到 0500,由 `manifest` 上的 flock 串行化。fork 因此不需要 unprotect 遍历(T1.1b)。`--hard`:每个文件/目录 `chflags uchg` + 目录去掉写位,fork 时在克隆上并行 `nouchg`。`world fs verify S<n>` 用清单校验两种模式,gate 模式额外检查根是否被留成敞开 |
| P4 | 误删 World / Snapshot | `discard` = rename 进 trash(一次 rename,毫秒级,与树大小无关),`world fs restore W<n>` 可恢复;保留期后由**后台 collector** 真删,`--now` 才在前台立即删。**T2.2 起 Snapshot 走同一条路**:`world fs discard S<n>`,有 ACTIVE World 引用时拒绝(`--force` 也不行,绝不让 World 失去 diff/verify 的基线),有 pool 条目时 `--force` 先 drain;trash 里的 World 不算引用,但它之后 `restore` 会以 `WFS_E_SOURCE_GONE` 拒绝。**崩溃安全(T2.1)**:真删之前先 rename 成 `<name>.deleting`,被 kill 的 worker 留下的东西一眼就不是 World,`restore` 以 `WFS_E_TRASH_DELETING` 拒绝,下一次唤醒继续删完。**崩溃安全的另一半(PR #1 review 第五轮):discard 本身是三步,行先写、树后搬**——(a) 一个事务:引用检查 + `state=WFS_ST_TRASHING` + `trash_path` 写成**树即将拥有的那个名字**,提交;(b) rename;(c) 一个事务:`state=TRASHED`。中间任何一刻被 kill,树只可能在**老位置或 `trash_path` 两者之一**,`lstat` 就能断定:还在老位置 → 回 ACTIVE(discard 没发生),已在 trash → 完成到 TRASHED,**除非还有 World/pool 引用这个 snapshot,那就搬回去**(第六轮起 `adopt` 也在写锁下拒绝非 ACTIVE 的 snapshot,所以这里的引用计数是**双保险**:按现在数,而不是按"谁都不可能再加引用"这个论证信)。恢复在**每次 store open 和每次 gc 开头**跑(两条走索引的 SELECT,平时一行都不返回)。配套:**trash 里的目录只有"任何状态下都没有行提到它"才算无主孤儿**,`.deleting` 拼写算同一个名字。`--now` 对**已经在 trash 里的 snapshot** 也成立:把删除提前,行落 DEAD(和 World 一致);不带 `--now` 才是 `-EALREADY`。**`restore` 是同一套三步反着跑(PR #1 review 第七轮)**:(a) 一个 `BEGIN IMMEDIATE` 里重读——行必须 TRASHED、树没在被删、基线必须 ACTIVE(否则 `WFS_E_SOURCE_GONE`)——并把行写成 `WFS_ST_TRASHING`;(b) rename 回家;(c) 一个事务写回 ACTIVE。于是**一个 TRASHING 的 World 行是快照的硬引用**:从那里看不出它是在去 trash 的路上还是在回家的路上,两个方向都不能丢基线,`discard S<n>` 一律拒。崩溃由同一个 `trashing_recover()` 按 lstat 收尾(树在 `trash_path` → TRASHED,树在家 → ACTIVE);世界的 trash 永远与世界同卷,所以 `dir_dev`/`dir_ino` 不必重新 stat。**一条 TRASHING 行带主人(PR #1 review 第八轮)**:(a) 把 pid 和该进程自己的启动时刻写进 `owner_pid`/`owner_start`(和 CREATING 行同一对列、同一套 `producer_alive()` 判活),(c) 和 undo 清掉。`trashing_recover()` **跳过主人还活着的行**——`lstat` 分不清"死在 rename 之前"和"还差一微秒就 rename",而每一次 `world fs ...` 都会 open store、每一次 open 都跑这套恢复,所以脚本里的下一条命令就足以把一个**正在进行**的 discard 判成没发生:行回 ACTIVE、`trash_path` 清空,discard 随后把树搬进 trash、(c) 改不到任何行却返回 0——一条 ACTIVE 行,一棵**无主孤儿**树,下一次 collector 立刻删掉;`restore` 是镜像。配套:(c) 和 restore 的收尾 UPDATE 都查 `sqlite3_changes()`,**改不到行就不算成功**——行和树在这之后绝不许互相矛盾。**collector 对行的每一次写也都是条件写(第八轮)**:`WHERE id=? AND state=2 AND trash_path=?`(排队时那条行的样子),重命名成 `.deleting` 之前还在 store 锁下再读一次行;`restore` 抢先把树搬回家时,collector 的 `-ENOENT` 意思是"搬回家了"而不是"已经被删了",原来那条无条件的 `mark_dead()` 会给一个刚刚恢复、就在家里的 World 写上 DEAD。**那次 rename 本身就是认领,所以它必须和行在同一个事务里(PR #1 review 第十五轮)**:条件写只能**事后**发现"这一条已经不归我了",而 rename 是**文件系统**那一步,发现的时候它已经做完了。`restore W<n>` 只要在 collector "再读一次行"和 rename 之间提交它的 (a)(行 → TRASHING,树还在 trash 里没动),collector 的 rename 就**成功**了——后面那条条件 UPDATE 一行都没改到,unlink 照样往下走;restore 的 rename 随后失败(树已经叫 `.deleting` 了),回落 TRASHED:一个还差一次 `restore` 就能回来的 World,没了。现在 `gc_claim_deleting()` 在**一个 `BEGIN IMMEDIATE` 里**重读行、rename、记下新名字、提交;restore 的 (a) 也是 `BEGIN IMMEDIATE`,两者因此串行——要么 restore 在前、认领读到 TRASHING 于是什么都不做,要么认领在前、restore 读到行已经写着 `.deleting`(`WFS_E_TRASH_DELETING`)而树一根指头没被碰过。事务里只握着一次 `rename(2)`(微秒级);会花上几分钟的那一步——把上一次没删完的 `.deleting` 折进来——挪到锁外先做(`trash_fold_leftover()`)。rename 和提交之间崩掉,等于行记着老名字、树在新名字,正是 `trash_follow_deleting()` 一直在解的那个状态(两个名字都属于这一行)。`--now`(`trash_delete_now()`)是同一个窗口、同一个修法:它的 `-ESTALE` 现在是一个**什么都没做**的 `-ESTALE`。无行的孤儿走同一个事务里的 `trash_path_claimed_locked()`——discard 是先提交 TRASHING 行、后把树搬进 trash 的。**`--force` 先跑的那次 drain 同样归“删不掉的树不算删掉了”管(PR #1 review 第十六轮)**:`wfs_pool_drain()` 原来先删行、后删树,两次删除的结果都扔掉,一个删不掉的条目于是变成 `<store>/pool` 下一棵**无行的完整克隆**,而 `--force` 拿着那个 0 继续把快照搬进了 trash——从此没有人会回来收它(`wfs_gc_pending()` 只扫 trash,而这个快照自己那条 trash 条目要过好几天才到期)。现在**树先删,条目本体和 `.wfs-tmp` 两个名字都 `proven_gone` 了才删行**,删不掉就留着行、把 errno 一路还给 `discard`:整条 `discard S<n> --force` 在碰快照之前就失败,条目仍旧是一条 ACTIVE 快照的普通 pool 行(所以 `gc --status` 什么都不报,因为确实什么都没坏),CLI 说出挡路的那个目录和 errno,权限修好之后同一条 `--force` 就是重试 |
| P5 | 对正在使用的 World 做 discard/checkpoint/fork | `world exec` 写 `<store>/locks/W<n>.lock`(pid + flock;`.world` 在实现里是文件不是目录,而且锁不该进项目树);discard / checkpoint / 从该 World fork 遇到活锁一律 `WFS_E_WORLD_BUSY`,`--force` 继续;pid 已死的锁静默清理 |
| P6 | 跨卷 clonefile EXDEV(`st_dev` 相同也可能) | init 前实际探针克隆一个临时文件;失败则明确报错,提供 `--store <同卷路径>` 或 `--copy`(真实复制,提示耗时) |
| P7 | 在危险路径上 init/fork | 拒绝 `/`、`$HOME`、store 自身、已是 World/Snapshot 的目录、另一个 World 内部;fork 目标不能在任何 World 或 Snapshot 内 |
| P8 | fork 中途崩溃留下半棵树 | 克隆到**目标父目录里一个抽出来的临时名** `.wfs-fork-<pid>-<计数>-<getentropy 16 位十六进制>` → 成功后 rename(`renameatx_np` + `RENAME_EXCL`,目标已存在就是 EEXIST,绝不覆盖)→ 再写 metadata(arch.md §27 的 publish 顺序)。**这个名字在克隆开始之前先写进 CREATING 行的 `tmp_path`**:父目录是用户的,不能靠后缀猜谁是我们的东西,崩溃之后**行是那棵树唯一的名字来源**。gc 只删 CREATING 行记下的那一条路径(再核对:仍是目录、没有活行认领它的 inode、`.world` 标记若在必须写着这一行的 world id),删完把行标成 DEAD;按后缀扫目录只剩 store 内部(`<store>/snapshots/S<n>.wfs-tmp`、`<store>/pool/S<n>/<uuid>.wfs-tmp`、trash 的 `.deleting`),用户目录一律不扫。**且只在生产者确实死了之后**:CREATING 行记 `owner_pid` + 该进程的启动时间(pid 复用检测),生产者还活着的行 gc 一律不碰,死了还要再过 `WORLD_GC_CREATING_MIN_AGE`(默认 60 s);fork 最后那条 UPDATE 校验 `sqlite3_changes()==1`,行没了就把树搬回临时名删掉并返回 `-ESTALE`,绝不留一个没人登记的目录在 `--to` 上。**删不掉的一律报出来、按上限重试,绝不报成已经没了(PR #1 review 第五、七、八、十一、十二轮)**:半成品 fork 树、半成品快照、过期 pool 条目各自留着自己的行,而 `<store>/snapshots` 下没有任何行认领的 `*.wfs-tmp` 没有行可留,就按**路径**记进同一个共享失败计数器——都进 `tmp_failed` / `pool_failed`、都被 `gc --status` 数着、都在 `kGcFailCap` 以内置 `work_remains` 让 worker 链回来找。第十二轮补上最后一处:`gc --reconcile` 埋掉一条悬空快照行时顺手删的 `<store>/snapshots/S<n>` 残株**先删、确认没了才埋行**——原来是先埋行再删、结果丢掉,而 DEAD 的快照行之后没有任何扫描会再看它一眼(reconcile 只扫 ACTIVE,后缀清扫只认 `*.wfs-tmp`),删不掉就永久泄漏;删不掉就把行留在 ACTIVE(它本来就被报成 dangling),按 `S<n>` 记失败计数 |
| P9 | 树内硬链接被克隆断开 | **T2.5 已修复**。init/checkpoint 扫源树的那一趟顺带按 (dev, ino) 分组:全部名字都在树内的组写进清单(`#hl` / `hl` 行,老读者一律跳过),有名字在树外的组只计数(克隆里没有可链接的对象)。之后每一次克隆——fork、checkpoint、pool 填充——在 rename 之前重放这些组:第一个名字是正身,其余 `link` 到它再 `rename` 覆盖(名字一刻也不消失,崩溃只留 `.wfs-tmp`)。代价 O(硬链接数),实测 0.27 ms/条(4 线程);pool 条目在填充时就做完,命中仍是 O(1)。从活 World fork 时用来源快照的组当候选,逐个名字在活树上 `lstat` 核对后才动手。**清单必须自洽(PR #1 review 第八轮)**:`#hl` 头里的组数和名字数写在它描述的那些行之前,读回来时必须对得上,且没有任何一组少于两个名字(扫描从不写这种组——名字不全在树内的组只计外部数)。否则 `-EINVAL` → `WFS_E_SNAPSHOT_DIRTY`。原来只留头里的外部计数,于是**最后一个成员没落盘**的清单读回来组数照旧、某一组只剩一个名字,而一个名字的组被重放**悄悄跳过**:fork 出来是两个独立文件,而快照记的是一个 inode 两个名字。**每一组还必须正好是它声明的那么大(PR #1 review 第九轮)**:`hl <组号> <nlink> <路径>` 行只描述"所有名字都在树内"的组(名字不全在树内的组只在头里计数,一行都不写),所以一组的成员数**就是**它的 `nlink`——**格式不动、版本不升,老清单照样读得回来**。只要求"至少两个名字"漏掉的是**把一个成员改挂到邻组**这种损坏:组数和名字数都不变,`#hl` 头依然自洽,而重放会把属于甲 inode 的名字 link 到乙组的正身上——克隆出来的内容被覆盖,`verify` 和 `fork` 都还报成功。**任何一个名字都不许在整段里出现两次(PR #1 review 第十一轮)**:这是最后一种把上面那些计数全保住的损坏——成员重复在自己组里,或者顶掉邻组的一个成员。重放对重复的名字什么也不做(它就是正身那个 inode,于是被记成已经链好),而被它顶掉的那个成员根本不在清单里了,于是 fork 回 0、克隆里是两个独立 inode;跨组的那一种更糟,被塞进来的名字会拿**另一个组**的正身去链,克隆里的内容被覆盖。名字是一条 dirent、扫描只记一次,所以重复按构造就是损坏:读回来把所有成员路径排个序,相邻相等就 `-EINVAL`。**成员路径还必须是树内相对路径**:空路径、开头的 `/`、任何 `..` 分量一律拒绝——重放是拿这些名字对着克隆的根解析的,`../escape` 就是用户自己目录里、fork 临时名旁边的一个文件,`link(2)` + `rename(2)` 会把组的正身盖上去。**名字里的字节只有三个需要转义:反斜杠、LF、CR(PR #1 review 第十二轮)**——读者原来会把行尾的 `\r` 当成 CRLF 一起剥掉,而写者只转义反斜杠和 LF,于是 `a\r` 写进去原样、读回来是 `a`,重放去链的是隔壁那个真叫 `a` 的文件(两个文件 size 和 mtime 一样时,重放那道闸门也拦不住,克隆里的内容就被覆盖了)。两份清单(硬链接清单、`verify` 清单)用的是同一对写者/读者,一起改;老清单照样读得回来——反斜杠一直是写成两个的,成对吃掉,所以"反斜杠 + r"这个序列在老清单里根本不会出现。**清单自洽还不够,组必须是那棵树的组(PR #1 review 第十三轮)**:清单上的检查全是数数,而两个组之间互换一个成员——`(a,b)`、`(c,d)` 写成 `(a,c)`、`(b,d)`——组数、名字数、每组的 nlink、名字不重复、路径不越界全都对得上,重放于是把 `a` 和 `c` 焊在一起、`c` 的内容没了(四个文件 size 和 mtime 一样,重放那道闸门也拦不住)。清单查不出清单自己的错,所以问**快照树**(它才是不可变的原件):重放之前每组挨个 `lstat`,所有成员必须落在同一个 inode 上、且该 inode 的 `st_nlink` 正好等于组的成员数,不符就是 `WFS_E_SNAPSHOT_DIRTY`。fork、pool 填充、`verify` 三处都做,前两处在 gate 窗口里做(根是 0000,不开门什么都 `lstat` 不到),没通过的克隆不发布。不能拿克隆去问:clonefile 把每一条硬链接都断开了,克隆自己说不出哪些名字本来是一个 inode。从活 World fork 仍走第五轮那条(核对活树、逐组跳过并上报),活树本来就允许变。**成员路径的每一个分量都必须是一个名字(PR #1 review 第十四轮)**:不许为空(连带管住 `a//b`、结尾的 `/`)、不许是 `.` 或 `..`——否则同一个文件还剩第二种拼法,`(d/a, ./d/a)` 就是第十一轮那种“成员重复”换了个写法,名字不重复、组数对、nlink 对,连问树那一道也放行(两个名字 `lstat` 到同一个 inode、nlink 确实是 2,因为它们本来就是一个名字),而重放把第二个成员当成已经链好,真正的 `d/b` 在 fork 出来的 World 里还是独立文件;写者写不出这种路径来(`rel` 由 readdir 的名字拼接,`.`/`..`/空名字都不会出现),所以老清单照样读得回来。**checkpoint 要从克隆里拿走的那个名字不进扫描(同一轮)**:`.world` 标记被人硬链接过时,扫描原本会把它和它的孪生名字收进一个组,克隆里的标记随后被删,重放只记了一笔 `missing` 就放过——组留在清单和 `hl_groups` 上,快照发布之后每一次 `verify` 和 fork 都因为这个树里没有的成员报 `WFS_E_SNAPSHOT_DIRTY`。`hardlinks_scan()` 现在收一个“要被拿走的相对名”(传的正是 `WFS_MARKER_NAME`,和 `unlink` 的是同一个名字,子 World 里的 `.world` 不受影响),剩下那个孪生名字自然落进**外部组**:计数而不认领。另外,**不在克隆里的成员一律把组判为 broken**,快照创建把它从清单和 `hl_groups` 上一并去掉——快照只说自己那棵树有的东西。**每一个分量还必须是一条真目录,不能是符号链接(PR #1 review 第十七轮)**:上面那些都是**拼法**上的规矩,而 `s/x` 里的 `s` 是符号链接时每一条都满足——`lstat(2)` 只放过最后一个分量、前面的照跟,`link(2)`/`rename(2)` 一个都不放过。相对链接在树的两份拷贝里落到不同的地方(快照根和 fork 的临时目录深度、父目录都不一样),所以核对可以在**快照旁边**种的一对硬链接上通过,而重放在**克隆旁边**把两个不相干的文件焊在一起,内容没了、fork 还报成功;从活 World fork 那条路上没有 `hardlinks_verify_groups`,唯一的把关 `group_still_linked()` 同样跟着链接走。修法是**把路径当成它本来的样子走**:从树根起逐个分量 `openat(O_DIRECTORY\|O_NOFOLLOW)`,叶子上的动作全部是相对父目录描述符的 `fstatat(AT_SYMLINK_NOFOLLOW)`/`linkat`/`renameat`/`unlinkat`——检查和动作是同一条系统调用路径,分量是符号链接就是 `ELOOP`,而且在任何地方创建任何东西之前(第四轮那个"借只读目录的写位"也搬到同一个描述符上)。验证那边判 `-EINVAL` → `WFS_E_SNAPSHOT_DIRTY`,重放那边判成"不去碰的名字"并把组判 broken。扫描写不出这种路径:walker 只对 `S_ISDIR` 递归,符号链接在它眼里从来不是目录 |
| P10 | diff 漏报 | 事件只当候选,最终以 stat/内容比对为准;收到 Dropped/MustScanSubDirs 立即全树 walk;`diff --full` 强制全扫 |
| P11 | 磁盘写满 | fork/init 前检查剩余空间 ≥ 条目数 × 1KB + 阈值;`world fs status` 用 df 差值报告真实占用(du 看不出块共享) |
| P12 | 并发命令互踩 | metadata 用 `BEGIN IMMEDIATE`;每条命令幂等;World 级操作先拿 flock |
| P13 | 用旧版 CLI 打开新 store | `VERSION` 文件 + schema 版本检查,拒绝并提示升级。同一个 schema 内部的**增量列**不动 `VERSION`,由 `PRAGMA user_version`(`SCHEMA*100 + 修订号`)记到哪一步。**迁移本身是一个事务,而且结论来自 schema(PR #1 review 第十一轮)**:每条迁移带着它要加的表名和列名,`PRAGMA table_info` 说列已经在了就跳过(因此全程不匹配任何错误字符串),真正跑的每一步都查返回值,最后再拿 `table_info` 问一遍每个列都在,**才**写版本戳;任何一步不对就整体回滚(`PRAGMA user_version` 是库头的一次写,跟着事务回滚),`wfs_store_open` 返回 `-EIO`,store 没有盖戳,下一次打开就是重试。原来是发完 ALTER 就盖戳,于是 EIO、磁盘满、熬过 busy timeout 的 SQLITE_BUSY、只能只读打开的库,都会留下一个"已迁移"但列是缺的 store,而且**永远不会再迁移** |
| P14 | agent 越界写 | `world exec` 套 seatbelt profile:允许写 World 根、agent 配置/缓存目录、tmp;拒绝 store、快照、其他 World。不带沙盒的 agent 也至少得到 P3 的快照保护 |
| P15 | 原地改写大文件的 COW 首写惩罚(~1ms) | 文档说明;不做特殊处理 |
| P16 | gc 与前台争抢(T2.1) | 物理删除是全系统最贵的操作(实测 1000 个 10k 树 = 1040 万次 unlink)。它只在**游离的后台 worker** 里做:store 级非阻塞 flock `<store>/locks/gc.lock` 保证全店一个;每次唤醒只做有限一批(N 条目或 T 秒)就退出,还有活就交给新起的后继进程;4 线程 unlink 比单线程快 1.9×,但实测会让并发 `fork` 慢 51–57%,**真正管用的是占空比**——干 2 s 停 2 s,前台代价降到 ~5%,drain 变成约两倍时长(背景工作,等得起)。`setiopolicy_np(IOPOL_THROTTLE)` 实测毫无作用:瓶颈是 APFS 元数据事务,不是磁盘带宽 |
| P17 | store 有树但 `metadata.db` 没了(M2) | **拒绝打开,绝不重建**。id 是从数据库里发的:新建一个空库就会再发一次 1,下一次 `init` 于是往已经在磁盘上的 `snapshots/S1` 上写。`wfs_store_open` 在创建任何东西之前先看:`metadata.db` 不存在、是空文件、读不了、或者打开后连 pragma 都执行不了(不是一个数据库),而 `snapshots/` / `trash/` / `pool/` 里还有条目 → `WFS_E_STORE_DAMAGED`。检测只是对这三个目录各做一次 readdir,快照的 gate 一路关着也不影响(不需要进 `S<n>/root`)。CLI 把话说全:这些树就是那个数据库的索引、id 会撞车、**`gc --reconcile` 帮不上忙**(它要读数据库才知道哪些行的树没了,而这里没的正是数据库),出路只有两条——从备份恢复 `metadata.db`,或者把整个目录挪开(`mv <store> <store>.damaged`)重开一个。空目录不算损坏,那是新 store。**但"我看不了"既不算空、也不算有(PR #1 review 第十三轮)**:那三次 `opendir(2)` 里只有 ENOENT 算"这个子目录不在,所以里面不可能有东西",EACCES / EIO / 卷没挂上一律把 open 连同那个 errno 一起挡回去(`metadata.db` 自己那次 `stat(2)` 同理)——原来它们被静静跳过,于是一个读不了的 store 被判成空的,数据库和 store id 就建在那些树旁边,而且从此**再也没有人会问第二遍**(库在了,守卫不跑了) |
| P18 | collector 按扫描时的快照行事,而 store 在动(M2,PR #1 review 第九轮) | **collector 只拥有它扫描那一刻看到的东西。** 它对行的**每一次写**都以"扫描时看到的状态和路径"为条件(`WHERE id=? AND state=? AND <路径列>=?`,再查 `sqlite3_changes()`);它做的**每一个"没有任何行认领这棵树"(孤儿)判断**,都要在树被排队之前、以及真正开删之前,**在 store 锁下拿活行再问一遍**。改不到行、或者现在有行认领了这个名字,就说明这一条已经不归它了:跳过,那棵树一根指头也不碰。第五到第八轮修的是这条规则的一个个实例(discard 的三步、TRASHING 行带主人、`mark_dead()`/`set_trash_path()` 的条件写),第九轮把剩下的补齐——(a) **trash 的孤儿判定**:`claim_trash_paths()` 取的是快照,随后的 readdir 不是,正好在两者之间提交 TRASHING 并 rename 的 discard 会留下一个快照没听说过的目录,而无主孤儿是**立刻删**的(保留期、`restore`、"这是谁的基线"全跳过);(b) **`--now` 的收尾 UPDATE**:原来一个条件都不带,`restore` 抢先把树搬回家时它把一个刚刚活过来的 World 写成 DEAD——现在改不到行就返回 `-ESTALE`,CLI 直说"它在这条 discard 跑的时候动过了";(c) **pool 的孤儿清扫**:扫描之后才插行的 filler,`.wfs-tmp` 被当场删掉(半棵树还会接着被发布成 READY),抢到条目的 fork 则被抽走了正要 rename 的树;pool 行的 `path`、`path + .wfs-tmp`、以及 fork 的 CREATING World 行的 `tmp_path` 都算"认领"。(`pool_claim()` 本身不需要宽限期:删 pool 行和插 fork 的 CREATING 行是同一个 `BEGIN IMMEDIATE`,rename 在它提交之后,树一刻也不会无行可依);(d) **`gc --reconcile`**:"这一行记的路径上没有树"对**被删掉的**和**只是被搬走的** World 同样成立,直到有人跑 `world fs verify <新路径>` 按 inode 把行挪过去——原来那条无条件 UPDATE 会把这份工作直接埋掉,现在带上扫描时看到的 `state=1 AND path=?`,`worlds_reconciled`/`snapshots_reconciled` 只数**真的被这条 UPDATE 改到**的行。同一轮顺带审出来的另外四处也一并按这条规则写死:CREATING 行的 `gc_tmp_is_removable()` 与收尾 UPDATE 加 `tmp_path` 条件(生产者被判死之后才发布的 fork,它的树不归 gc 删)、`trashing_recover()` 的收尾 UPDATE 加 `trash_path` 条件(判断是拿这条路径 `lstat` 出来的)、`pool_collect()` 在删整棵克隆之前在锁下重问一遍"这一行还该死吗"、`DELETE FROM pool` 带上 `path`。`gc --status` 与 `gc --pending` 和 collector 共用同一套判定,所以报表报的就是 collector 真会动的东西。**第十轮**补上这条规则在 collector 里的最后一处缺口(PR #1 review 第十轮):`<store>/snapshots` 下的 `*.wfs-tmp` **后缀清扫**——它一行都不问,见一个删一个,删掉的正是上面那趟 CREATING 扫描刚刚放过的、生产者还活着的 `S<n>.wfs-tmp`,于是 `wfs_snapshot_create()` 在克隆中途拿到 `-ENOENT`。"没有任何行认领这棵树"同样是一个关于活行的判断,所以现在每个条目的 `S<n>` 都解析回行 id,在 store 锁下问一遍 snapshots 表,**任何不是 DEAD 的行都算认领这棵树**:生产者活着的 CREATING 是正在建的克隆;生产者死了的 CREATING 归上面那趟(它删不掉时会**故意**留着行按失败上限重试,清扫插进来只会把行弄丢);ACTIVE/TRASHING/TRASHED 说明旁边的 `S<n>` 是真快照。解析不出 `S<n>.wfs-tmp` 的名字照旧删(没有谁会写出这种名字),而这个清扫从此只肯扫 `<store>/snapshots` ——它带的认领规则**就是**那张表,换个目录就不成立了。至此 collector 里每一处靠名字模式下判断的地方都归在这条规则下面了。**第十二轮**再加一句,管的是这些判断所依据的**证据**(PR #1 review 第十二轮):**"树不在了"必须是证明出来的,不能是问不出来的。** `exists()` 是 `lstat(2)`/`stat(2)` 上的一个 bool,而 EACCES(父目录权限)、EIO、卷没挂上、ENAMETOOLONG 在它眼里全等于"不在"——于是 `gc --reconcile` 把一个活着的 World 标成 DEAD(DEAD 的 World `verify` 修不了、`adopt` 也认不回来)、`snapshot discard` 把行直接埋掉而树还在 `S<n>`、collector 悄悄丢掉一棵它其实没删成的树。现在统一走 `wfs::fs_probe()`(保留 errno),`wfs::fs_gone()` **只认 ENOENT/ENOTDIR**;凡是会毁东西或者会写下一条没法反悔的行的判断,一律按"**能证明不在才算不在**"(`proven_gone`),证明不了就当它还在:行原样留着、计进 `tmp_failed`/`pool_failed`/新增的 `{snapshots,worlds}_unreadable`、由 `gc`/`gc --status`/`status` 报出来;有调用者可以交代的地方(`discard`、`restore`、`verify`、`check_path`)直接把 errno 还回去。路径上放着一个**不是目录**的东西,算**损坏**不算不在——照样不埋。纯粹"它在不在,我好建"的检查不变(建的时候自己会 EEXIST)。**第十五轮**再补两句(PR #1 review 第十五轮)。(a) **条件写补不了那一次 rename**:collector 对行的每一次写都是条件写,可真正不可逆的是 rename 成 `.deleting` 那一步,它做完之后才轮到条件写去发现"这一条不归我了"——所以**认领(rename)和行的重读必须在同一个 `BEGIN IMMEDIATE` 里**(`gc_claim_deleting()`,`--now` 的 `trash_delete_now()` 同),详见 P4。(b) **引用也必须在同一把写锁下产生**:`wfs_pool_fill` 在顶上读一次快照行,`build_one()` 随后在自己的事务里插 CREATING pool 行,却不再问一次快照——夹在中间的 `discard S<n>` 数引用时这一行还不存在,于是提交 TRASHING;而 filler 的源树那一刻还没搬走,克隆照做,一个正在进 trash 的快照就这么有了 READY 条目(`pool_collect()` 要到下一轮才埋它,中间的 fork 会把它当活基线领走)。现在插入的那个事务里重读快照行,要求 **ACTIVE 且 `created_at` 正是条目要带的那个**,否则 `-ESTALE`、停止填充——反过来,只要这一行先插进去,discard 就数得到它(不带 `--force` 是拒绝,带 `--force` 是 drain)。**第十六轮**再紧一格(PR #1 review 第十六轮):引用不只要在同一把写锁下**产生**,还必须**在每一个瞬间都存在**。从 pool 领走一个条目的 fork,认领时写下的那条 CREATING World 行就是这个快照的引用;可它失败之后的 unwind 是先删行、再把条目插回去两个事务,夹在中间的那一格 `discard S<n>` 两样都数不到,于是提交 TRASHING,归还随后插出一个**属于 trash 中快照的 READY 条目**。现在**行出去和条目回来在同一个 `BEGIN IMMEDIATE` 里**(`pool_return()` 的 `PoolReturnHook`,`PoolClaimHook` 的镜像),归还自己也在那把写锁下重读一次快照行(ACTIVE 且 `created_at` 正是条目带着的那个,否则不插:删树,由调用方单独埋掉自己那条行) |

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
