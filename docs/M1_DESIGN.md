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
| `discard W` | rename 到 trash,保留期后 GC 真删 | 毫秒 |

数据面零介入:World 里所有读写都是原生 APFS。

## 2. 存储布局

```text
<store>/                         默认 ~/Library/Application Support/World/fs/;必须与项目同一 APFS 卷
├── VERSION                      schema 版本;新版本拒绝旧 core 打开
├── metadata.db                  SQLite WAL
├── snapshots/S<n>/root/         gate 保护:根目录 0000(`--hard` 时改为逐条目 uchg)
├── locks/W<n>.lock              `world exec` 的锁(pid + flock,P5)
├── tmp/                         `world exec` 生成的 seatbelt profile
├── pool/S<n>/<uuid>/            预克隆的 World,尚未分配
├── trash/W<n>-<ts>/             discard 后的 World,保留期(默认 7 天)内可 restore
└── worlds/W<n> -> <user path>   仅记录,World 真身在用户路径
~/worlds/W<n>/<name>/            World 根;含 .world 标记文件
```

store 目录加 `.noindex`(Spotlight)并 `tmutil addexclusion`(Time Machine),无需 root。

## 3. 防出错设计(逐条,每条对应 safety 测试)

| # | 风险 | 设计 |
|---|---|---|
| P1 | World 目录被移动/改名 | 身份不靠路径:根目录 `.world` 标记(world id、store id、snapshot id)+ metadata 记 dir inode。命令执行时按 inode 反查并自动修正路径 |
| P2 | World 被 `cp -R` 复制出未登记副本 | 标记存在但 inode 不符 → 视为"未登记副本",所有破坏性命令拒绝,提示 `world fs adopt` 或 `fork` |
| P3 | 快照被改动,污染后代 | **默认**:快照根目录 `chmod 0000`(gate),里面的条目一律不动;clonefile 期间才临时开到 0500,由 `manifest` 上的 flock 串行化。fork 因此不需要 unprotect 遍历(T1.1b)。`--hard`:每个文件/目录 `chflags uchg` + 目录去掉写位,fork 时在克隆上并行 `nouchg`。`world fs verify S<n>` 用清单校验两种模式,gate 模式额外检查根是否被留成敞开 |
| P4 | 误删 World | `discard` = rename 进 trash,`world fs restore W<n>` 可恢复;保留期后 `gc` 真删;`--now` 才立即删 |
| P5 | 对正在使用的 World 做 discard/checkpoint/fork | `world exec` 写 `<store>/locks/W<n>.lock`(pid + flock;`.world` 在实现里是文件不是目录,而且锁不该进项目树);discard / checkpoint / 从该 World fork 遇到活锁一律 `WFS_E_WORLD_BUSY`,`--force` 继续;pid 已死的锁静默清理 |
| P6 | 跨卷 clonefile EXDEV(`st_dev` 相同也可能) | init 前实际探针克隆一个临时文件;失败则明确报错,提供 `--store <同卷路径>` 或 `--copy`(真实复制,提示耗时) |
| P7 | 在危险路径上 init/fork | 拒绝 `/`、`$HOME`、store 自身、已是 World/Snapshot 的目录、另一个 World 内部;fork 目标不能在任何 World 或 Snapshot 内 |
| P8 | fork 中途崩溃留下半棵树 | 克隆到 `<target>.wfs-tmp` → 成功后 rename → 再写 metadata(arch.md §27 的 publish 顺序);gc 清理 `.wfs-tmp` |
| P9 | 树内硬链接被克隆断开 | init/checkpoint 的保护遍历顺带统计 nlink>1,写入清单并警告;fork 后按 (dev, ino) 恢复为硬链接(M1 只警告,M2 恢复) |
| P10 | diff 漏报 | 事件只当候选,最终以 stat/内容比对为准;收到 Dropped/MustScanSubDirs 立即全树 walk;`diff --full` 强制全扫 |
| P11 | 磁盘写满 | fork/init 前检查剩余空间 ≥ 条目数 × 1KB + 阈值;`world fs status` 用 df 差值报告真实占用(du 看不出块共享) |
| P12 | 并发命令互踩 | metadata 用 `BEGIN IMMEDIATE`;每条命令幂等;World 级操作先拿 flock |
| P13 | 用旧版 CLI 打开新 store | `VERSION` 文件 + schema 版本检查,拒绝并提示升级 |
| P14 | agent 越界写 | `world exec` 套 seatbelt profile:允许写 World 根、agent 配置/缓存目录、tmp;拒绝 store、快照、其他 World。不带沙盒的 agent 也至少得到 P3 的快照保护 |
| P15 | 原地改写大文件的 COW 首写惩罚(~1ms) | 文档说明;不做特殊处理 |

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
- T1.5 pool:后台预克隆(launchd agent 或 `world fs pool fill`),fork 命中率统计
- T1.6 safety 测试套件 `scripts/tests/safety.sh`:P1–P14 每条一个用例,全部必须过
- T1.7 基准:fork 延迟(带/不带保护)、diff O(changes) 验证、1000 idle World、存储增长;对照 arch.md §1 判据
- T1.8 文档:arch.md 增补测量结论与转向章节;README 改写

顺序:T1.1 → T1.2 → T1.3/T1.4/T1.5 → T1.6/T1.7 → T1.8。

## 6. 对 arch.md §1 判据的预期

| 判据 | 预期 |
|---|---|
| fork < 10ms p50 | pool 命中时满足;pool 空时 0.07–0.4s(10k–50k 文件) |
| git/build ≥ 90% | 99–102% |
| 1000 idle branches | 1000 个目录,元数据约 15GB(5 万文件树) |
| 存储 ≈ divergence | 改 1% 文件物理 +0.36% |
| diff O(changes) | FSEvents 正常时满足;丢事件时退化为 O(tree) |

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
