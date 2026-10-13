# 本地容灾备份与增量 replay

状态：LevelDB 存储核心、离线 CLI、故障注入回归。不是 restore 的别名，
也不是路径操作重放。目标是原仓库丢失后，靠独立保存的基线与后续提交恢复
整个仓库，包括 Worlds、revisions、CoW 对象、inode 内容/属性和历史关系。

## 使用与恢复点

```text
forkfsd backup SOURCE BASE.pack
# 输出基线提交序号，例如 123；此时开始保留后续事务。
forkfsd replay-export SOURCE 123 DELTA.pack
# 输出本包结束序号，例如 456；下次可从 456 导出。
forkfsd replay BASE.pack DESTINATION
forkfsd replay DELTA.pack DESTINATION
forkfsd check DESTINATION
forkfsd fs-check DESTINATION
```

先停止持有仓库的 forkfsd 或卸载 FSKit World。CLI 通过 LevelDB LOCK 独占
源/目标，不绕过现有所有权。备份/导出直接使用 LevelStore，不执行 Container
打开时的 orphan 清理；不改变逻辑提交序号，也不创建用户 revision。
导出的不是单个 revision，而是一个一致提交点上的整个仓库。该仓库里的任意
命名 revision 都能在恢复后继续 fork。

基线和增量必须另行复制到独立的故障域；留在原机器上的日志不能抵御整盘丢失。
RPO 为已成功导出并独立保存的最后提交点。当前没有云端 ACK、自动上传、持续
复制、自动故障接管或损坏源盘上的无备份抢救功能。本地原事务成功不表示远端已收到。

## 原子留存与格式兼容

首次 backup 在 writer lock 内用同步 WriteBatch 设置 replay 起点、随机链锚及
head，并将仓库格式从 FFLDB001 提升为 FFLDB002。旧二进制在打开时拒绝新版格式，
避免提交了数据却漏记 replay。升级不可由旧程序降级。未启用 replay 的仓库仍用
FFLDB001，不承担事务日志留存开销。首次备份失败时也可能已成功启用留存。

启用后，每笔事务把以下内容放在**同一个 sync WriteBatch**：

- 原有新对象、命名根变更和 sequence/object/root counters；
- `L/<8 字节大端提交序号>`：规范化事务记录；
- `M/replay/head`：该事务记录的域隔离哈希 H。

事务记录包含提交序号、上笔记录哈希及完整对象/根/counter 变更。新增对象带
内容，引用和删除保留原义；没有重新解析路径、读取源文件或重新运行文件系统命令。
每笔原事务已有 4096 mutations、16 MiB 新对象的限制，记录帧上限为 24 MiB。
本地 WAL 恢复后，原提交和 replay 记录同时存在或同时不存在。

日志保存在 LevelDB 中，常规 compaction 不删除它们。首版不提供日志裁剪；
不能在确认所有备份消费者的恢复点前删除日志。启用后有额外编码、哈希和存储开销，
尚未给出性能基准。未来可以增加远端确认水位及日志保留策略。

## 包格式与流式处理

FFRPLY01 包采用 little-endian 整数，包含固定 105 字节头：magic(8)、类型(1，
B=基线/I=增量)、仓库 UUID(16)、from/to sequence(各 8)、起点/终点链哈希(各 32)。
基线 from=to 且两个链哈希相等；增量包含 `(from,to]` 的连续提交。

哈希 H(x) 复用对象格式：SHA-256(`forkfs.object.v0` + u16le(4) +
u16le(0) + u64le(len(x)) + x)，不是直接对 payload 计算裸 SHA-256。
每帧为 length(u32)、payload、H(payload)。基线 B 帧是一条有序的
M/O/R 键值记录；增量 T 帧是 sequence、previous hash 和操作数组。
操作数组为 count(u32)，每项包含 put/delete(u8)、key/value length(u32)
及字节数据。M 元数据仅允许 sequence/objects/roots；O 仅允许按内容哈希
新增对象；R 允许更新或删除命名根。不接受任意内部键写入。

E 帧包含全包滚动摘要，摘要起点为 H(header)，每个数据帧推进为
H(previous_digest + frame_hash)。没有 E 帧、尾随字节、超限长度、
错误对象哈希、重复键、缺提交或乱序均被拒绝。按帧读取，不把整个仓库放入内存。
哈希用于意外损坏检测，不是签名；包必须来自可信来源。

备份在启用留存后获取 LevelDB snapshot，锁外扫描与导出，不阻塞整个导出过程中的
核心写者。每个包输出到 O_EXCL 新文件，成功前 fsync 文件及父目录；失败可能留下
无完整 footer 的文件，重试使用新路径，不能覆盖已有包。

## 应用与重试

先完整扫描包，检查 framing、内容哈希、footer 和增量链，再打开恢复目标。
应用时再次逐帧校验。输入应保持不变；若应用过程中被修改或发生 IO 错误，可能
已有完整事务提交，必须以目标实际水位为准重试。合法校验值不等于合法数据库状态，
目标引用和计数还会在各事务提交前检查。

基线先写入目标同目录下的私有 `DEST.restore-XXXXXX` 临时仓库，验证完整对象和
根引用后关闭、同步目录，再用不覆盖已有目标的原子 rename 发布。崩溃前目标不存在，
发布后目标为完整基线。未发布的临时目录可能残留，可在确认无人恢复时人工清理。
已存在的目标仅在相同仓库身份、相同基线链哈希且完整验证通过时作为重试接受；
其他已有目标一律拒绝，绝不覆盖。

增量在每笔提交中同时更新对象、根、计数、日志及恢复水位。检查仓库 UUID、
起点链哈希、连续序号和当前 head。已应用且哈希一致的前缀跳过，支持整个包重试、
重叠包和成功回复丢失。缺段、异源、已应用记录不同或本地写入造成的分叉被拒绝。
较新的基线不接受早于其保留起点的增量包。空增量合法。

恢复目标不应在增量追赶期间启动可写服务；正常写入（包括 orphan 清理）会生成
本地提交并可能造成分叉。追赶完成后才通过 Container/服务打开，执行正常 orphan
恢复并接管写入。内存句柄、RPC session、锁、目录游标不会从备份复活。
当前不会让已挂载 World 在线切换到 replay 后的树。

## 回归

`forkfsd_replay` 加入 metadata-regression，使用真实 LevelDB、生产 CLI 及专用
故障注入二进制，覆盖基线/增量、重叠与重复应用、revision/fork/restore、独立
恢复后再次 fork、错误源/分叉/缺段、截断/篡改/乱序、全量发布和增量提交前后
崩溃、原事务及日志的原子留存。故障钩子不进入生产二进制。
`forkfsd_replay_core` 另外覆盖并发备份的提交点一致性、引用/删除、inode 身份、
hardlink、uid/gid/时间/binary xattr，以及底层对象/日志损坏和格式保护。


### Corruption 回归矩阵

`forkfsd_replay_corruption` 的包由独立 Python 编码器生成，并用合法包逐字节
往返作为正对照。负例重算正确的对象、frame、链和 footer 哈希，检查明确的
错误原因，避免所有用例实际上只碰到最外层 checksum 检查。

- revision：长度、magic、零/缺失 manifest、缺失/错误类型父记录、父序号倒挂、
  未来序号、非法名称，以及真实 LevelDB 中的描述对象丢失/bit flip。
- CoW：manifest 格式、长度、零/缺失树根、节点格式/长度、零/缺失值、缺失子节点。
- replay：重复操作键、未知元数据字段、错误计数、缺失对象引用，以及坏 revision
  通过校验正确的增量事务到达目标的情况。
- 拒绝结果：基线不发布目标目录；坏增量事务不改变目标 sequence/根/数据，
  之后仍能应用原始正确增量。源损坏时 backup/export 拒绝，不生成有效恢复包。

恢复校验遍历所有命名 World、revision 和 World base 的引用，检查 revision
父链序号严格递减，复用 CoW/旧 run 格式解析器检查传递引用。增量在提交前以
“现有对象 + 本事务新对象”的只读视图检查变更的根。此检查增加离线备份/replay
扫描成本，不加入日常写事务路径。

这不替代 `fs-check` 的完整 inode/目录语义检查；例如格式与引用都合法但 link
count 不匹配的逻辑错误，仍需文件系统层 fsck。既有 inode corruption 回归继续
覆盖 inode/xattr 编码错误。容灾验收需要两层检查，不能仅凭包的哈希判定文件系统健康。
