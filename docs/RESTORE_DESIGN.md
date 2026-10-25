# World restore 与访问撤销

状态：CLI/RPC 首版实现；真实挂载与内核缓存屏障尚未实现。

## 契约

`fs-restore CONTAINER REVISION [--world WORLD]` 将已存在的 World 整体恢复到
命名 revision 的不可变 manifest。省略 World 表示 main。RPC 同名命令。
恢复文件内容、目录、inode 身份、hardlink 拓扑、symlink 和 inode 属性；
World 名称保持不变，已保存的 revision 和其他 World 保持不变。
恢复后可以继续写入并创建新 revision，历史基线改为目标 revision。
恢复不自动保存被替换的 live 状态；需要保留时，应先显式 snapshot。

成功后，目标 World 的全部 live 文件句柄（包含只读、dup 和 open-unlink
句柄）、whole-file/range 锁及目录 cursor 被撤销。旧 token 不能读、写、
seek、stat 或继续分页；close 旧 token 返回拒绝。session 本身仍有效，可打开
新句柄。历史 revision 句柄/cursor 和其他 World 的访问继续有效。
即使目标 manifest 与当前 head 相同，也执行撤销并递增 generation。

## 持久化与原子发布

复用现有 LevelDB 命名对象事务，不复制目录树，不重写内容对象。一个
`Journal::transact` 原子发布：

- `\0forkfs-history/world/<world>`：引用目标 manifest；
- `\0forkfs-history/base/<world>`：目标 revision descriptor 的对象 ID；
- `\0forkfs-history/generation/<world>`：八字节 little-endian 单调 generation；
- 删除目标 World 的全部 `orphan/<world>/...` retention roots。

未设置 generation 的已有 World 视为 0。损坏的 generation 长度或 uint64
溢出拒绝操作。该字段属于服务控制元数据，不进入被恢复的文件树。
删除 orphan retention roots 不删除不可变对象，也不回收仍可见的 inode。

首版只支持 LevelDB，并保留一次事务最多 4096 项的约束：最多允许 4093
个 orphan roots，加上三个发布字段；超过时完整拒绝，不分批删除。
之后可用 generation 分区的 orphan 索引替代扫描及该限制，但不能在原子
发布前删除仍由旧句柄使用的 roots。

本地 sync WriteBatch 成功是持久化确认边界。没有远端确认或云端持久化承诺。
缺失 revision、非法名称、目标 World 不存在、revision 只读视图或超限均在
发布前拒绝，树和访问保持原样。底层 IO 错误与确认丢失仍采用现有存储层
契约；不能把所有 IO 错误推断为必然未提交。成功提交后进程崩溃，重启读取
新树；崩溃前未提交则保留旧树。客户端没有收到回应时必须重新读取状态。

## 服务屏障与后台工作

Namespace::restore 使用 Container mutex，与 handle IO、目录操作和 head
发布共用线性化锁。锁内先验证并准备 batch，持久化成功后删除 live handles
和 open_counts。撤销步骤不需要新分配；释放 handle 也释放共享锁状态。
DirectoryCursor 捕获打开时的 generation；read/rewind 在锁内检查当前值，
因此即使 core API 的 cursor 不归 RPC 管理，也不能继续访问恢复前的树。
普通 write/rename 不改变 generation，仍保留原有分页快照语义。

RPC 当前串行 dispatch。在返回 restore 成功前清除所有 session 中目标 World
的句柄和 cursor ownership；不会再调用 close 已撤销的引擎 token。清理不
移除其他 World 或 revision 项，session expiry/shutdown 可继续正常执行。

Compaction::prepare 同时捕获 head 和 generation 对象引用；stage/install
同时比较两者。即使恢复到了相同 head，也不能发布恢复前准备的候选结果。
已经 staged 的不可变对象可留存，不能覆盖新 head。当前没有跨进程写者。
未来并行 RPC 必须让发布和 ownership 撤销处于同一调度屏障；仅靠 session
清理不能替代引擎锁与 generation 校验。

## 挂载边界

目前可用 FSKit 适配是固定 revision 的实验性只读访问，restore 不改变这些
revision。没有实现 live World 挂载的恢复。未来 live mount 请求必须携带
World generation；restore 需要停止新请求，处理已提交/未提交写回，撤销旧
inode/lookup/handle，并使内核缓存失效后才能确认挂载层恢复完成。
不能把一次根指针更新或 RPC 回归通过当作已实现这个屏障。

## 回归证据

RV005 比较恢复前后的完整树、保留后续历史、恢复后独立写入和指定 World。
RV006 覆盖多 session、dup、orphan、锁释放、旧 cursor 拒绝，以及 revision/
其他 World 访问存活。RV007 在 compaction/restore 后重启服务读取历史和
恢复结果。C++ core 回归检查 cursor generation、失败不撤销、同 head restore
使 compaction 失效、orphan roots 释放和重新打开存储。
故障二进制在 LevelDB write 前/本地 sync 后退出，重启验证完整旧/新树、
提交序号及保留历史。真实内核挂载、断电硬件、云同步和完整 POSIX 验收
仍不属于这些测试的证明范围。

## Managed mounted Worlds

Restore now returns POSIX `EBUSY` while the target World has a durable mount
registration, including expired/revoked sessions and registrations surviving a
daemon restart. Stop the managed agent, verify OS unmount, and retire that exact
registration before restore. See [managed mount lifecycle](MOUNT_LIFECYCLE.md).
This prevents an in-place restore from publishing new metadata beneath old
kernel caches; it does not implement live cache invalidation or hot restore.
