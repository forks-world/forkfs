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
| `chown/fchown`、setuid/setgid/sticky、ACL | inode metadata + access checks | 缺失；uid/gid 来自服务用户，拒绝更换属主，无 ACL，不是多用户文件系统 |
| `access/faccessat`、目录搜索权限、只读 fd 权限 | 内核权限判断；未接 AccessCheckOperations | 挂载已测单用户 mode 000/0400、access、目录搜索/写权限、拒绝 truncate/创建/unlink、chmod 不撤销已开 fd；faccessat/组权限/多用户未验收 |
| `mkdir/rmdir/unlink`、普通 `rename`、原子保存 | create/remove/rename → 原子 namespace 提交 | 挂载已测；失败 errno 与状态不变需按操作继续扩充。直接 RPC 错误码修复另见 PR #95 |
| `renamex_np/renameatx_np(RENAME_EXCL/RENAME_SWAP)`、交换/排他 rename | FSKit 平台限制 | 原生实测 SWAP、EXCL 到不存在目标均 ENOTSUP；EXCL 目标已存在时内核先返回 EEXIST。旧 Operations 和新 Handler 都不提供 flags 回调，不能只补 daemon 或迁移 Handler 解决 |
| `link/symlink/readlink`、dangling symlink、`O_SYMLINK` | createLink/createSymbolicLink/readSymbolicLink | 挂载已测；包含链接身份、nofollow 时间/模式、`cp -Pp` 和二进制 xattr |
| `readdir/getdirentries`、目录游标与属性 | enumerateDirectory → captured cursor | 基础挂载已测、游标 RPC 已测；单目录最多 65,536 项，64 个流，变更中遍历压力未验收 |
| UTF-8、大小写、NFC/NFD、长名字 | FSFileName → namespace validation | 挂载已测；区分大小写及 NFC/NFD，255 字节/组件、4096 字节/路径；非法 UTF-8/控制符拒绝 |
| `get/set/list/remove xattr`、fd/nofollow xattr | XattrOperations | 挂载已测；每值 64 KiB、每 inode 32 项；大 resource fork 不兼容 |
| `mmap(MAP_SHARED/MAP_PRIVATE)`、`msync`、`munmap` | 内核缓存 + ReadWriteOperations | macOS 27.2 单挂载已测；映射/`pread/pwrite` 一致性、private 隔离、open-unlink、flush 后 revision 隔离 |
| `fcntl(F_SETLK)`、`flock` | 内核原生 advisory locks；与 RPC 锁分开 | macOS 27.2 跨进程互斥、关闭/退出释放已测；byte-range 不相交范围可同时持有。共享锁/阻塞唤醒/信号打断未验收 |
| `fsync`、目录同步、`F_FULLFSYNC`、断电恢复 | synchronize → inode-sync + 存储提交 | 基础文件 fsync/提交与 daemon 恢复已测；目录语义、全硬件屏障、断电/内核崩溃没有完整保证 |
| `ftruncate`、稀疏文件、`lseek` | setattr size + bounded chunk I/O | 挂载已测零填充、shrink/extend、64 MiB/4 GiB/1 TiB 边界；`SEEK_DATA/SEEK_HOLE` 未验收 |
| `F_PREALLOCATE`、`posix_fallocate`、hole punch | 可选 Preallocate/SeekRegion 协议 | 未实现相应协议；不能声称空间预留或洞区间查询兼容 |
| `statfs/statvfs/pathconf` | volumeStatistics、PathConfOperations | 回调/RPC 已测；空间来自宿主卷，不是 forkfs 配额，剩余空间不保证下一次写入成功 |
| `chflags`、immutable/append-only/hidden 等 | setattr flags + 所有变更点执行约束 | 缺失；flags 更新拒绝，不能仅保存位而不执行约束 |
| `mkfifo/mknod`、Unix socket 路径 | create item types | 缺失；当前只支持 regular/directory/symlink，不适合依赖这些节点的运行目录 |
| `copyfile/clonefile/fcopyfile/sendfile` | 内核 fallback 或可选复制能力 | 普通 `cp` 部分场景已测；CoW clone、资源 fork 大值、完整 copyfile flags、sendfile 未验收 |
| `kqueue` vnode 通知、文件监控、FSEvents | 内核事件及卷能力 | 未验收；daemon revision history 不等于应用文件监控事件 |
| 持久 file ID、file handles、卷改名、大小写折叠 | 可选卷能力/身份协议 | persistent IDs 未提供（不宣告）；ID 只在当前 mount 稳定；其余未提供/未验收 |

当前类使用 macOS 26 的 Operations 协议，具体实现见
[`RevisionVolume.h`](../macos/forkfs/RevisionVolume.h) 和
[`RevisionVolume.mm`](../macos/forkfs/RevisionVolume.mm)。已接入 Operations、
PathConf、OpenClose、ReadWrite、Xattr。macOS 27 Handler 迁移、AccessCheck、
Preallocate、SeekRegion、DataCache 等独立评估；迁移 API 不自动补齐上表语义。

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
这不是全范围密集文件资格：现有事务容量仍可能拒绝大量 chunk 的 open-unlink
或 truncate；物理分配统计也尚未完善。1 TiB 不预留磁盘空间。

## 补全顺序与完成条件

每批功能必须同时更新此矩阵、自动化用例及真实挂载证据。明确不支持的功能应
在修改前返回合适错误；不能返回成功却丢弃请求。不能根据接口存在把“未验收”
改成“挂载已测”。

1. **基础语义完整性**：在本轮已验收的 flags/`*at`/append/单用户权限上继续补
   flag 组合、组权限、阻塞与共享 fd、目录并发遍历、容量边界的 APFS 对照；发现
   差异后分别修复 namespace、RPC 或 FSKit 层。明确 errno、失败不变性、readonly、
   revision/GC 后仍保持语义。rename 扩展需单独评估平台路线或明确兼容范围，
   不能当作普通 daemon 修复任务。
2. **容量与大文件**：验证更大密集文件的事务容量，并解除 32 项 xattr、目录项上限中的应用阻断；
   完善大规模 chunk 回收，并扩充密集文件、边界短写、ENOSPC、
   GC/重启和历史隔离验收。预分配必须兑现空间保证，不能只改逻辑长度。
3. **元数据与安全**：设计持久 uid/gid、特殊模式位、ACL、file flags 的 revision
   格式与授权模型；所有 write/truncate/link/rename/unlink 路径执行约束。先完成
   私有单用户权限验收，再考虑多用户；不能只增加 AccessCheck selector。
4. **工作负载闭环**：扩大 SQLite WAL 并发和进程故障、Git 大仓库/构建器/编辑器
   atomic-save、copyfile/资源 fork、文件监控。特殊节点和应用运行目录需求独立验证。
5. **故障与平台资格**：长时间缓存/写回、daemon/bridge/extension 异常、lease 失效
   后缓存读取边界、重挂载、目录 fsync、硬件 flush、磁盘满与断电恢复；分别记录
   macOS 26/27，Linux 挂载需要独立前端及完整验收。

达到上述目标意味着约定的 macOS 应用兼容范围通过验收；POSIX 全标准一致性、
完整 APFS 功能和 Linux 文件系统支持仍是不同目标，需要各自的清单与测试。
