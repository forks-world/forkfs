# 文件系统 API 兼容目标与验收矩阵

当前目标：macOS 26+ 上，单用户、私有挂载的 revision-backed 文件系统，
可以承载普通开发工具的文件操作。**尚未达到完整 POSIX 或 APFS 兼容**。
Linux 目前只有 daemon 测试，不等于存在 Linux 挂载前端。本文仅针对
`forkfsd` + `macos/forkfs`；历史 passthrough、`world` CLI 的测试不能作为证据。

兼容的单位是应用实际调用及其语义，不是 FSKit selector 数量。
Apple 的 [FSVolume](https://developer.apple.com/documentation/fskit/fsvolume)
包含必要操作和可选协议；例如
[AccessCheckOperations](https://developer.apple.com/documentation/fskit/fsvolume/accesscheckoperations)
用于由卷自行执行访问检查。没有实现某个可选协议，不足以证明对应 POSIX
调用不可用；反过来，存在回调也不足以证明应用可用。mmap 和原生文件锁必须
通过内核挂载验证，daemon RPC 的锁测试不能替代它们。

状态含义：

- **挂载已测**：有当前 revision 后端的真实 syscall/应用验收，限于列出的场景。
- **回调/RPC 已测**：实现存在，但不能据此宣称所有内核路径兼容。
- **未验收**：尚无足够证据；不等同于已证实失败。
- **缺失/受限**：明确的实现缺口或产品限制。

## API 清单

| 应用接口/行为 | FSKit 或内核路径 | 当前状态与验收边界 |
| --- | --- | --- |
| `mount/umount`、只读挂载、后台服务启动/退出 | activate/mount/unmount/deactivate + launcher/session | 挂载已测；服务损失后卸载，不能透明重连旧 inode |
| `open/read/write/pread/pwrite/close` | OpenCloseOperations、ReadWriteOperations → inode handles | 挂载已测；单文件逻辑上限 1 TiB，RPC 单次最多 256 KiB，回调分块 |
| `O_CREAT/O_EXCL/O_TRUNC/O_APPEND/O_NOFOLLOW/O_DIRECTORY`、`openat` 系列 | 内核路径解析 + lookup/create/open/setattr | 挂载已测排他/类型/nofollow 拒绝、dangling link 创建、截断身份、umask、并发 append、基本 `*at`；全部组合与极限并发仍未验收 |
| `dup`、打开后 rename/unlink、同名路径重用 | 内核 fd + daemon orphan roots | 挂载已测；包含普通文件、符号链接元数据 fd 和 mmap |
| `stat/lstat/fstat`、`chmod/fchmod`、`utime/utimens`、birth time | get/setAttributes | 挂载已测；时间纳秒精度，读取不更新 atime，模式仅 0777 |
| `st_blocks`、`du` | allocated_size → allocSize | 挂载已测；空洞为零、数据 payload 每位置向上取整到 512 字节，不是去重后独占仓库空间 |
| `chown/fchown`、setuid/setgid/sticky、ACL | inode metadata + access checks | 缺失；uid/gid 来自服务用户，拒绝更换属主，无 ACL，不是多用户文件系统 |
| `access/faccessat`、目录搜索权限、只读 fd 权限 | 内核权限判断；未接 AccessCheckOperations | 挂载已测单用户 mode 000/0400、access、目录搜索/写权限、拒绝 truncate/创建/unlink、chmod 不撤销已开 fd；faccessat/组权限/多用户未验收 |
| `mkdir/rmdir/unlink`、普通 `rename`、原子保存 | create/remove/rename → 原子 namespace 提交 | 挂载已测；失败 errno 与状态不变需按操作继续扩充。直接 RPC 错误码修复另见 PR #95 |
| `renamex_np/renameatx_np(RENAME_EXCL/RENAME_SWAP)`、交换/排他 rename | FSKit 平台限制 | 原生实测 SWAP、EXCL 到不存在目标均 ENOTSUP；EXCL 目标已存在时内核先返回 EEXIST。旧 Operations 和新 Handler 都不提供 flags 回调，不能只补 daemon 或迁移 Handler 解决 |
| `link/symlink/readlink`、dangling symlink、`O_SYMLINK` | createLink/createSymbolicLink/readSymbolicLink | 挂载已测；包含链接身份、nofollow 时间/模式、`cp -Pp` 和二进制 xattr |
| `readdir/getdirentries`、目录游标与属性 | enumerateDirectory → captured cursor | 挂载已测 66,000 项及 telldir/seekdir；前端按页读取，无旧 65,536 上限；64 个快照流，回调已测跨页删除/回退和淘汰游标 |
| UTF-8、大小写、NFC/NFD、长名字 | FSFileName → namespace validation | 挂载已测；区分大小写及 NFC/NFD，255 字节/组件、4096 字节/路径；非法 UTF-8/控制符拒绝 |
| `get/set/list/remove xattr`、fd/nofollow xattr | XattrOperations | 挂载已测；每值 262143 字节、每 inode 128 项、总值 8 MiB；ResourceFork 整值已测，`cp -p`/位置读取仍 EINVAL，不能称完整 fork 兼容 |
| `mmap(MAP_SHARED/MAP_PRIVATE)`、`msync`、`munmap` | 内核缓存 + ReadWriteOperations | macOS 27.2 单挂载已测；映射/`pread/pwrite` 一致性、private 隔离、open-unlink、flush 后 revision 隔离 |
| `fcntl(F_SETLK)`、`flock` | 内核原生 advisory locks；与 RPC 锁分开 | macOS 27.2 跨进程互斥、关闭/退出释放已测；byte-range 不相交范围可同时持有。新增挂载验收共享锁并存/排他冲突、阻塞唤醒、dup 关闭释放差异和 F_SETLKW 信号 EINTR；公平性/死锁/OFD locks 未验收 |
| `fsync`、目录同步、`F_FULLFSYNC`、断电恢复 | synchronize → inode-sync + 存储提交 | 挂载已测文件/元数据和目录 fsync、目录改名后的 fd、原子保存和被替换旧 fd；F_FULLFSYNC/F_BARRIERFSYNC 请求返回成功。全硬件屏障、断电/内核崩溃仍无完整保证 |
| `ftruncate`、稀疏文件、`lseek` | setattr size + bounded chunk I/O | 挂载已测零填充、shrink/extend、64 MiB/4 GiB/1 TiB 边界；macOS 27 已挂载验收 `SEEK_DATA/SEEK_HOLE`（64 KiB chunk 粒度）；macOS 26 保留保守 fallback |
| C++ `Namespace::seek`、daemon `fs-hseek` | namespace open-description cursor / RPC | 实现约定：负位置为 `EINVAL`，超出 `INT64_MAX` 为 `EOVERFLOW`；失败不移动共享 cursor。修复回归与构建证据见[seek error regression record](evidence/2026-11-14-handle-seek-errors.json)，当前验证状态以记录为准。**此项不验收原生 `lseek(2)` errno 或新增挂载能力。** 独立 APFS syscall 对照见 [seek reference](evidence/2026-11-14-seek-apfs-reference.json) |
| `F_PREALLOCATE`、`posix_fallocate`、hole punch | 可选 Preallocate/数据变更接口 | 未实现相应协议；不能声称空间预留或洞区间查询兼容 |
| `statfs/statvfs/pathconf` | volumeStatistics、PathConfOperations | 挂载已测容量/单位/标志、路径与 fd 查询、rename/unlink/路径重用和 GC 后只读 revision；子目录/文件 fd 的 `PC_PATH_MAX` 返回 EINVAL，不能宣称完整 pathconf。空间来自宿主卷，不是 forkfs 配额或预留 |
| `chflags/fchflags/lchflags`、BSD flags | get/setAttributes → revision inode | 挂载已测 hidden/nodump 的路径/fd/nofollow、硬链接、cp -p、孤儿 fd 和历史隔离；user immutable 及 user append-only 的数据/元数据/目录约束已接入；nounlink/系统保护标记仍拒绝 |
| `mkfifo/mknod`、Unix socket 路径 | create item types | 缺失；当前只支持 regular/directory/symlink，不适合依赖这些节点的运行目录 |
| `copyfile/clonefile/fcopyfile/sendfile` | 内核 fallback 或可选复制能力 | 新增原生验收 `copyfile(DATA|STAT|XATTR)`、`fcopyfile(DATA)` 和 sendfile 区间/EOF/unlink fd；clone 请求可回退，强制 clone 返回 ENOTSUP；ResourceFork、ACL、递归/完整 flags 仍未补齐 |
| `kqueue` vnode 通知、文件监控、FSEvents | 内核 vnode 事件 | macOS 27.2 小仓库已测 WRITE/EXTEND/ATTRIB/LINK/RENAME/DELETE、父目录事件、跨进程写及路径重用隔离；FSEvents、递归监控、revoke 未验收 |
| 持久 file ID、file handles、卷改名、大小写折叠 | 可选卷能力/身份协议 | persistent IDs 未提供（不宣告）；ID 只在当前 mount 稳定；卷名支持 revision 元数据和原生改名，大小写折叠未提供/未验收 |

### Seek error codes

The compatibility claim above is limited to the C++ `Namespace::seek` method and
the raw daemon `fs-hseek` RPC. A negative computed position produces `EINVAL`;
a computed position above `INT64_MAX` produces `EOVERFLOW`. Both preserve the
shared open-description cursor. The APFS reference artifact records a separate
native `lseek(2)` run on macOS 27.2.1 arm64 with APFS; it is host-kernel context,
not a forkfs mount result, and does not establish any new mount capability.
Its `EOVERFLOW` value is the Darwin errno observed in that run.
The baseline-vs-fixed C++ and raw-RPC regression record is maintained separately
in [seek error regression evidence](evidence/2026-11-14-handle-seek-errors.json);
it distinguishes the baseline failures from the completed fixed-build validation.

当前类使用 macOS 26 的 Operations 协议，具体实现见
[`RevisionVolume.h`](../macos/forkfs/RevisionVolume.h) 和
[`RevisionVolume.mm`](../macos/forkfs/RevisionVolume.mm)。已接入 Operations、
PathConf、OpenClose、ReadWrite、Xattr。macOS 27 Handler 迁移、AccessCheck、
Preallocate、DataCache 等独立评估；迁移 API 不自动补齐上表语义。

inode RPC 的路径查找可复用已观察到的路径提示；每次都在当前 World/revision
重新解析并核对 inode ID，最终符号链接不跟随。目录提示若经过祖先符号链接也
回退真实目录遍历，避免长别名挤占子项路径长度。提示失效时仍回退遍历，不以旧
路径决定写入目标。缓存全局最多 4,096 项，已测目录改名、硬链接、路径重用、
祖先符号链接改向/成环、World/revision 隔离和淘汰；不等同于持久 inode 索引
或任意冷查询的常数复杂度。见 [路径提示验收](evidence/2026-11-12-inode-path-hints.json)。

## 应用验收：可重复执行

已有基础 syscall 验收：

```sh
python3 scripts/tests/forkfs_mount.py /path/to/mount \
  --daemon build/Release/daemon/forkfsd --socket /path/to/runtime/control.sock --checkpoint
```

新增 [`filesystem_compat.py`](../scripts/tests/filesystem_compat.py) 验收：

```sh
python3 scripts/tests/filesystem_compat.py /path/to/mount \
  --daemon build/Release/daemon/forkfsd --socket /path/to/runtime/control.sock
# 相同用例在 APFS 上运行，作为宿主环境对照：
python3 scripts/tests/filesystem_compat.py /private/tmp --reference-apfs
```

必须使用可丢弃的 repository。脚本验证实际文件系统类型，只在唯一临时目录内
写入并清理。forkfs 模式还保留一个命名 revision，验证 mmap 数据已进入 daemon，
以及后续写入不能修改历史；不自动删除该 revision。各组用例有子进程超时，
输出 JSON，任一失败返回非零。Git 用例禁用用户/系统配置、签名、hooks、模板，
不连接远端。

| 用例 | 验证内容 |
| --- | --- |
| mmap | shared/private/read-only、页边界、pread/pwrite 一致性、另一进程读取、unlink 后映射与同名新文件隔离 |
| lockf / flock | 另一进程争锁必须失败、关闭及进程退出释放；lockf 另测互不相交范围 |
| SQLite DELETE / WAL | 显式模式确认、二进制数据、并发读与写锁冲突、rollback、进程未 close 即退出后的事务恢复、integrity_check、WAL checkpoint |
| Git | init/add/commit、分支 checkout、可执行位/符号链接、fast-forward merge、repack、strict fsck、历史二进制内容 |

这证明普通 Git 程序可使用被测挂载，不表示已恢复废弃的 `world` Git 集成。
SQLite 的 [锁和同步要求](https://www.sqlite.org/lockingv3.html) 说明为什么不能
只以建表成功判断兼容；这里的进程退出恢复也不等于断电安全。
本轮 [APFS 与 forkfs 对照结果](evidence/2026-11-10-filesystem-api-compatibility.json)
限于 macOS 27.2/列出的应用版本、小文件、单挂载。

2026-11-11 增加四组回归，APFS 与 forkfs 全部通过：

| 用例 | 验证内容 |
| --- | --- |
| open-flags | 12 组失败 flags 的精确 errno 与文件/目录不变性、dangling symlink 创建、保留 inode 的 truncate、Darwin `O_RDONLY|O_TRUNC`、umask |
| append | 4 进程各追加 4 个 300 KiB 记录（超过 256 KiB RPC 上限），无丢失/交错/重复；lseek 不改变 append，Darwin pwrite 忽略 O_APPEND |
| permissions | 单用户文件 mode/access、目录搜索/写入检查；已开 fd 在 chmod 000 后仍可读写，拒绝写截断及目录变更时不改数据 |
| relative-paths | 目录 fd 在目录改名后仍支持 openat/fstatat/linkat/renameat/unlinkat/mkdirat/readlinkat 路径语义 |

脚本另外输出 `capability_probes`，与通过的功能用例分开。
rename extensions 探针确认 forkfs 的 SWAP 和 EXCL-to-missing 返回 ENOTSUP，
失败不改 inode 或数据；APFS 对照可完成这些操作。EXCL-to-existing 的 EEXIST
只是提前拒绝，不能据此宣称支持排他 rename。
[Apple 的 Handler rename 文档](https://developer.apple.com/documentation/fskit/fsvolume/handler/renameitem%28_%3Aindirectory%3Anamed%3Ato%3Aindirectory%3Aoveritem%3Acontext%3Areplyhandler%3A%29)
明确说明 FSKit 在进入回调之前拒绝带 flags 的 rename。SDK 27.2 的 Operations
接口也有相同限制。因此完整 APFS API 兼容在现有公开 FSKit 表面上有平台阻碍；
不能用多次普通 rename 冒充原子交换，也不能宣称 Handler 迁移能解决此问题。
完整结果见 [open/append/权限/相对路径证据](evidence/2026-11-11-open-semantics.json)。

大文件扩展的逻辑上限为 1 TiB；验收覆盖 65 MiB + 17 字节密集文件全量哈希、
跨旧上限 mmap、64 MiB/4 GiB/1 TiB 稀疏位置、orphan/截断/历史隔离，以及
GC、服务重启后只读重挂载。见 [大文件证据](evidence/2026-11-11-large-files.json)。
密集文件回收改为 CoW 范围删除及两个 orphan 保留根后，已额外验收两个 320 MiB、
各 5,120 个已分配 chunk 的文件。open-unlink/dup/孤儿写入和截断、路径重用、
最后关闭及闭合文件截断均通过，并在 GC/只读重挂载后校验历史完整哈希。
见 [密集文件回收证据](evidence/2026-11-11-chunk-retirement.json)。这不是全范围
1 TiB 密集文件性能资格。1 TiB 不预留磁盘空间。

分配统计现按每个数据位置的已存 payload 向上取整到 512 字节；空洞为零，
FSKit `allocSize`、原生 `st_blocks` 和 `du` 已挂载验收。它不包含元数据/WAL，
也不是去重后独占物理字节。真实 256 MiB APFS 测试镜像已复现 ENOSPC，
故障后保活和 sync 拒绝成功；释放空间并重启后校验完整旧/新发布和历史隔离。
WAL append/sync 的 ENOSPC、EDQUOT、EIO 另有注入测试；没有实现配额保证或
完整断电资格。`FFLDB002` 索引升级后旧 daemon 会拒绝打开。
见 [分配及空间不足证据](evidence/2026-11-11-allocation-enospc.json)。

LevelDB 写失败后的失效检查现抛出明确的 EIO，普通 fs-* 的 handle/路径读取、
写重试及保留 revision 读取均通过现有 errno+1 wire code 返回，不再混用通用
错误码 1。首次写失败仍保留 ENOSPC/EDQUOT/EIO；移除故障注入并不修复已打开
的失效实例，必须停止服务后重开。status/storage-profile 仍可诊断；失效后的
拒绝不推进提交/对象/根计数，也不命中已预热的对象/根缓存。

12 组 WAL append/sync × ENOSPC/EDQUOT/EIO × 写入/截断故障继续验证完整旧/新
发布恢复和保留 revision 不变，存储单测另覆盖失效后的校验、压缩和 GC 拒绝。
这是 daemon/C++ 和 RPC 的错误契约补全；原生前端此前已将同一通用异常映射为
EIO，因此不提升原生同步故障传播或内核缓存失效的验收等级。见
[存储失效 EIO 证据](evidence/2026-11-13-poisoned-store-eio.json)。

2026-11-12 对最新 `main` 重新集成了 #100 的分配统计和存储故障处理，保留
#99 的 flat 布局 open-unlink/rename 替换修复。#100 原先合入已完成主线合并的
功能分支，因此单看 PR 的 merged 状态不足以证明功能进入主线。本次组合重新
通过 55 项隔离测试、原生分配/基础 syscall、真实磁盘满及 GC 后只读重挂载。
证据见 [主线 API 集成验收](evidence/2026-11-12-main-api-integration.json)。
后续仍按下述缺口推进，不把这些容量接口的完成等同于完整文件系统兼容。

单值 xattr 上限扩展到 262143 字节（256 KiB 减 1 字节），在现有不可变对象上限内，并与 FSKit 的原生可用边界
保持一致，旧的 64 KiB 值无需迁移。验收包含普通属性和 ResourceFork 的整值
读写，以及普通大属性的 `cp -p`。ResourceFork 复制和位置读取实测 EINVAL，
APFS 对照成功；这些仍是明确缺口。数量上限现为 128 项，每 inode 总值预算 8 MiB，
包含系统自动添加的属性；相同 payload ID 仍按每个属性名计入。预算 ENOSPC
在发布前拒绝，释放名称/字节预算后可立即重试，不触发存储故障重开。
见 [数量/预算和 ResourceFork EOF 证据](evidence/2026-11-12-xattr-capacity-resource-fork.json)。
对照脚本与范围见 [大 xattr 验收](evidence/2026-11-12-large-xattrs.json)。

`hidden`/`nodump` 标记已贯通 daemon、RPC 与 FSKit，flags-only diff 即使时间戳
相同也能识别变化。非零标记使用 `FFINOD04`，旧版本 daemon 拒绝该记录；
清除当前 World 的标记不等于清除历史版本。hidden/nodump 不影响数据写入；
后续 user immutable 的范围见下文，append-only 等其他保护类标记仍拒绝。
见 [BSD 元数据标记验收](evidence/2026-11-12-file-flags.json)。

macOS 27 的 `SeekRegionHandler` 已接入现有 Operations 前端，原生
`SEEK_DATA/SEEK_HOLE` 可查询稀疏 chunk 区间，不扫描空洞或读取 payload。
验收覆盖 1 TiB 稀疏文件、EOF/空文件 ENXIO、dup/open-unlink、路径重用、
revision 隔离及 GC 后只读重挂载；macOS 26 构建不宣称支持该新协议。
这是逻辑数据区间查询，不提供物理 extent、预分配或 hole punch。
见 [稀疏 seek 验收](evidence/2026-11-12-seek-regions.json)。

普通 Namespace 提交现复用按不可变节点 ID 缓存的子树高度，避免每个新编辑器
重复做完整高度扫描；缓存命中仍按新深度检查 256 层上限，GC 后清空。
4,000 项空文件的单次 Namespace API 对照从 45.9 秒降到 30.1 秒；这不等于
原生挂载的创建吞吐资格。冷缓存、inode 到路径查找和其他元数据成本仍需
单独处理。见 [高度缓存验收](evidence/2026-11-12-metadata-height-cache.json)。

原生锁的补充验收直接调用内核 `flock/lockf/fcntl`，与 daemon RPC 锁分开。
共享锁可并存，排他请求冲突；阻塞等待者在解锁后唤醒。Darwin 的 flock 在最后
一个共享描述符关闭后释放，经典 fcntl/lockf 则在持锁进程关闭该文件的任一 fd
后释放。原始 `F_SETLKW` 调用关闭 SA_RESTART 后被 SIGUSR1 打断返回 EINTR，
不释放另一进程仍持有的锁。用例使用子进程握手和超时，不以单次上锁成功作证。
见 [共享/阻塞锁证据](evidence/2026-11-13-native-locking.json)。

## 补全顺序与完成条件

每批功能必须同时更新此矩阵、自动化用例及真实挂载证据。明确不支持的功能应
在修改前返回合适错误；不能返回成功却丢弃请求。不能根据接口存在把“未验收”
改成“挂载已测”。

1. **基础语义完整性**：在本轮已验收的 flags/`*at`/append/单用户权限上继续补
   flag 组合、组权限、阻塞与共享 fd、目录并发遍历、容量边界的 APFS 对照；发现
   差异后分别修复 namespace、RPC 或 FSKit 层。明确 errno、失败不变性、readonly、
   revision/GC 后仍保持语义。rename 扩展需单独评估平台路线或明确兼容范围，
   不能当作普通 daemon 修复任务。
2. **容量与大文件**：完善空间不足恢复，继续处理 128 项/8 MiB xattr 和目录项上限中的应用阻断；
   扩充更大密集文件的性能/内存资格、旧仓库冷查询性能、边界短写及空间不足故障组合、
   GC/重启和历史隔离验收。预分配必须兑现空间保证，不能只改逻辑长度。
3. **元数据与安全**：设计持久 uid/gid、特殊模式位、ACL、file flags 的 revision
   格式与授权模型；所有 write/truncate/link/rename/unlink 路径执行约束。先完成
   私有单用户权限验收，再考虑多用户；不能只增加 AccessCheck selector。
4. **工作负载闭环**：扩大 SQLite WAL 并发和进程故障、Git 大仓库/构建器/编辑器
   atomic-save、copyfile/资源 fork、文件监控。特殊节点和应用运行目录需求独立验证。
5. **故障与平台资格**：长时间缓存/写回、daemon/bridge/extension 异常、lease 失效
   后缓存读取边界、重挂载、目录 fsync 故障传播、硬件 flush、磁盘满与断电恢复；分别记录
   macOS 26/27，Linux 挂载需要独立前端及完整验收。

达到上述目标意味着约定的 macOS 应用兼容范围通过验收；POSIX 全标准一致性、
完整 APFS 功能和 Linux 文件系统支持仍是不同目标，需要各自的清单与测试。

容量接口验收直接调用 `statfs/fstatfs/statvfs/fstatvfs/pathconf/fpathconf`，
核对稳定几何参数、每次返回的计数边界与 fd 卷身份，不要求两次调用之间的空闲
计数完全相等。只读 revision 仍报告共享宿主空间，由 readonly 标志和 EROFS
表明不可写；空闲 inode 数不是 forkfs 自身的 inode 配额。`PC_FILESIZEBITS=42`
是包含 1 TiB 端点所需的有符号位数；实际 1 TiB 稀疏文件可写最后一字节，
增加一字节的 truncate 返回 EFBIG 且保留内容。`PC_PATH_MAX` 在本次子目录及
文件 fd 上返回 EINVAL；公开 PathConfOperations 没有路径总长属性，daemon 的
4096 字节限制不能当作该 syscall 的成功结果。

本轮同时修复超长路径/组件及符号链接目标返回 EINVAL 的问题，现返回
ENAMETOOLONG，其他非法名称仍为 EINVAL，拒绝操作不产生提交。组件上限按
UTF-8 字节计：255 字节可用，256 字节拒绝；APFS 本次接受 256 字节的多字节
名称，不能声称相同 Unicode 名称容量。见
[容量与名称限制证据](evidence/2026-11-13-volume-capacity.json)。


2026-11-12：目录枚举前端改为每个流缓存一页（最多 512 项加 dot 项），
不再一次加载完整目录或在 65,536 项处失败。游标保持原始快照；跨页查询和
回退不会重新读取 live World。保留最多 64 个流，淘汰时关闭 daemon 游标，
旧 verifier 返回 ESTALE。挂载生命周期内的 inode→file ID 映射仍随观察到的
inode 增长，不能把页缓存上限当成整个进程的常数内存保证。
见 [大目录验收](evidence/2026-11-12-directory-streams.json)。

该验收的 fixture 使用批量元数据准备，不代表逐文件创建性能。普通逐文件
创建期间已观察到 `MetadataTree::height` 的每编辑器全树检查占主要耗时；
高度校验复用/创建吞吐将作为独立后续项处理。


User immutable (`UF_IMMUTABLE`) 已在 daemon 的路径写入、写打开、元数据、
硬链接及目录变更入口执行 EPERM 限制，并保留在 revision/孤儿 inode 中。
遵循 Darwin 的已授予权限语义：设置前打开的写 fd 仍可写和截断，设置后不能
新开写 fd；它不撤销旧 fd/mmap 权限，也不是多用户授权边界。标志本身可由
私有服务用户清除。旧 daemon 拒绝 FFINOD04 的新增标志位，包括历史版本。
见 [user immutable 验收](evidence/2026-11-13-user-immutable.json)。

链接容量：daemon 与 FSKit 共用 32,767 上限，新增链接及子目录/跨父目录移动
增长超限时返回 EMLINK，失败不发布；删除后容量立即复用，替换空目录的净零
增长在上限仍可完成。原生文件 fixture 包含真实 32,767 个引用，并通过 GC 后
只读 revision 回读。APFS 对照的 pathconf 同样报 32,767，但实际允许下一个
链接；forkfs 不宣称相同额外容量。目录边界的快速单测使用合成计数，恢复真实
计数后做完整校验，不把它当作 32,765 个子目录的性能验收。
见 [链接容量证据](evidence/2026-11-13-link-limits.json)。

User append-only (`UF_APPEND`) 允许从 EOF 追加，禁止覆写、空洞写入、截断、
mode/时间变更及删除/改名/新增硬链接；目录允许新增子项和移动进入，禁止删除、
替换或移出。xattr 按 APFS 实测保持可修改。普通 RPC 写入严格要求 EOF；
FSKit 专用写回先比较已有前缀，只有完全相同的重写和 EOF 后追加才可成功，
避免未满数据页的缓存写回使 fsync 失败。旧 fd 的覆写/截断也受 append 约束，
区别于 immutable 的已授予数据权限。此功能不是不可撤销日志或多用户授权边界。
见 [append-only 验收](evidence/2026-11-13-append-flags.json)。

卷名改名通过 FSKit RenameOperations 和 `ATTR_VOL_NAME` 接入，名称是 World
元数据树中的独立叶子，快照/fork/restore/GC 均保留，不改文件身份、World 名、
repository ID 或挂载路径。缺失名称记录时仍为 ForkFS；相同名称不产生提交。
支持 1–255 字节合法 UTF-8，保留大小写/NFC/NFD，不接受空名称、点名称、斜杠
或控制字符。revision/只读挂载改名返回 EROFS；label-only diff 单独报告卷名变化，
不伪造文件内容变化。APFS 对照仅操作新建可丢弃镜像。
见 [卷名验收](evidence/2026-11-13-volume-name.json)。

原生复制/传输补充验收直接调用 Darwin libc，而不是 Python 文件复制或
`socket.sendfile` 的回退。502000 字节二进制文件跨 256 KiB RPC 边界，验证
`copyfile(DATA|STAT|XATTR)` 的数据/mode/mtime/hidden/nodump/20 KiB 普通
xattr，以及 `fcopyfile(DATA)` 和源/目标均 unlink 后同名路径重用的 fd 身份。
`sendfile` 通过有期限的 loopback 接收线程检查区间、count=0 到 EOF、超 EOF
零字节及源 fd 偏移不变，并累计非阻塞短传输/EAGAIN 的实际发送字节。

`COPYFILE_CLONE` 在 forkfs 上成功回退普通复制，state 明确报告未 clone；
`COPYFILE_CLONE_FORCE` 返回 ENOTSUP。APFS 对照两个分支都实际 clone。
`COPYFILE_EXCL` 返回 EEXIST 后，目标数据/身份/mode/mtime/flags 保持不变，
但两种文件系统均观察到 ctime 更新；不能把这个复合库调用当作单次原子提交。
三份复制文件通过 daemon revision 数据回读和 GC/只读重挂载的元数据/xattr
校验。macOS CTest 自动执行 APFS reference，原生 FSKit 挂载仍单独验收。
见 [复制与传输验收](evidence/2026-11-13-copy-transfer.json)。不宣称 ACL、
ResourceFork、递归复制、稀疏分配保留、header/trailer、任意负载或零拷贝性能。

CLI `request ... fs-hwriteback HANDLE OFFSET INPUT_FILE` 已与其他写请求一样
读取二进制文件内容；此前会把文件名字符串作为 payload，导致合法 append-only
写回失败，普通可写文件也可能收到文件名文本。回归覆盖含 NUL 的写回、空输入
不提交、非法前缀拒绝和缺失/超过 256 KiB 文件在 dispatch 前失败。此修复针对
CLI 包装层，不改变 wire protocol 或原生 FSKit 能力等级。
见 [CLI 写回输入验收](evidence/2026-11-13-cli-writeback.json)。

同步验收以 1,028,347 字节二进制内容跨越 256 KiB RPC 边界。文件数据与
mode/mtime 的 fsync、创建文件/硬链接/符号链接后的目录 fsync、目录改名后
旧目录 fd 的 fsync、写临时文件后 rename 替换并同步父目录均通过。被替换文件
的旧 fd 仍可写入/同步，且不影响新文件；干净文件/目录的重复同步不产生提交。
在创建 snapshot 之前，daemon 已回读确认同步后的内容和元数据。保留的 revision
还通过服务退出后新进程回读、离线 GC 和只读重挂载的内容/mode/mtime/链接校验。

APFS 与 forkfs 均接受本轮 F_FULLFSYNC 和 F_BARRIERFSYNC 请求，但返回成功
不能证明每次调用都执行了设备级屏障，亦不能替代断电/内核崩溃测试。当前
inode-sync 检查存储健康状态，而实际变更由同步提交完成；LevelDB 的 macOS
Sync 会尝试 full sync，失败可回退普通 sync。因此本轮只提升已列出的调用与
正常退出/重开的验收等级，不提供新的硬件持久性保证或故障传播资格。见
[同步与原子保存证据](evidence/2026-11-13-native-sync.json)。
