# 基于 revision 的备份与 replay 设计

状态：设计提案，基于 PR #54 已合并的离线备份与事务 replay。
本文中的 checkpoint、catalog、在线 RPC、日志裁剪和精确恢复目标尚未实现。
现有命令、FFRPLY01 格式及已实现保证见 [DISASTER_REPLAY.md](DISASTER_REPLAY.md)。

## 1. 决策与边界

采用三层模型：**revision 定义用户可见的文件树；checkpoint 定义仓库一致恢复点；
事务日志连接恢复点之间的每次已提交修改**。备份介质首版为本地目录，网络传输是后续
可替换模块。先实现单仓库、单写者，源端由 forkfsd 在线备份，目标端离线恢复。

每次 write 不必创建 revision，但每次持久提交必须进入 replay 日志。
仅保存 revision 间的树差异能恢复某个文件树，不能完整恢复仓库事务、所有 World、
命名 revision、inode 分配状态及其他控制元数据，因此不能取代事务 replay。

首版备份范围固定为整个仓库。单 revision 的便携导出可以以后单独实现，不能把它
伪装成可接续原仓库日志的基线。本文不设计多写者合并、跨仓库事务、挂载中的原地回滚，
也不承诺恢复进程句柄、文件锁、RPC session 或应用未提交到文件系统的内存数据。

## 2. 对象与恢复点

| 对象 | 身份与内容 | 作用 |
|---|---|---|
| revision | 不可变 descriptor 对象 ID，引用 manifest、父 revision、source_sequence | 固定一个 World 的逻辑文件树；名称只是可变索引 |
| checkpoint | 独立 ID，记录仓库 UUID、sequence、链哈希、World/revision 根清单及版本 | 固定整个仓库的一致状态，供备份和恢复定位 |
| base | 在 checkpoint 水位导出的完整仓库包 | 不依赖源仓库即可建立恢复目标 |
| delta | 同一 lineage 上 `(from_sequence, to_sequence]` 的连续事务 | 从已有状态推进至下一恢复点 |
| backup generation | base、后续 delta 和不可变 catalog manifests 的集合 | 管理一个独立可恢复的备份链及保留期限 |

恢复位置用 `(repository_uuid, sequence, chain_hash)` 标识，不能只比较 sequence。
不同副本可能在相同 sequence 写入不同内容；链哈希用于发现分叉，不负责选举写者。
正常 World fork 不创建新的仓库 lineage，只是仓库中的一次根引用事务。

现有 FFREV001 中的 `source_sequence` 是创建 revision **之前**的仓库序号，
不是 revision 描述记录的提交水位，不能直接拿它作为 replay 的终点。
即使当前 snapshot 路径通常下一笔就提交，也不把 `source_sequence + 1` 作为长期 API。

引入独立的 revision 创建事件索引：

```text
revision_event = {revision_id, world_id_or_name_at_creation,
                  creation_sequence, creation_chain_hash}
checkpoint = {checkpoint_id, repository_uuid, sequence, chain_hash,
              world_roots, revision_roots, format_versions}
```

同一 descriptor 可能被多个名称引用，事件以提交身份区分；请求必须解析为不可变 ID
及明确事件，不能在任务执行中反复解析名字。创建索引由持有 writer lock 的提交路径
获得实际分配的序号。chain_hash 在提交后由该日志记录算出，不将事务自身哈希写入
自身 payload，避免循环定义。索引是可重建缓存；恢复授权以实际事务及其根变更为准。
若用独立持久键保存索引，必须先扩展格式白名单及旧版本拒绝规则，不能悄悄增加现有
FFRPLY01 未支持的键。首阶段可将事件索引保存在备份 catalog 并由保留日志重建。

旧 revision 若缺少创建事务及可信索引，不承诺能恢复整个仓库到它的创建时刻。
仍可从包含它的基线恢复仓库，再从该 revision fork 出 World。

## 3. 两种 revision 恢复语义

必须在 API 和用户界面上区分：

1. **恢复文件树**：恢复一个包含目标 revision 的 checkpoint，再 fork 该 revision。
   不影响其他 World，也不要求仓库状态倒退到 revision 创建时。
2. **恢复整个仓库到 revision 创建点**：选择不晚于 creation_sequence 的基线，
   replay 到该事件的完整事务提交边界。恢复所有 World 当时的状态，而不只是该 revision。

例如 revision R 在事务 101 创建，基线在 120：基线 120 可用于恢复 R 的文件树，
但不能回退到仓库 101。只有基线 90 加连续日志 `(90,101]` 才能实现后者。
如果事务 102 又改了另一 World，停在 101 的恢复结果不应包含该改动。

用户请求“现在备份 World main”时，可以选择同时创建一个命名 revision，方便之后
浏览和 fork；后台定期仓库备份默认只建 checkpoint，避免自动制造大量用户 revision。
对于多 World 的统一恢复点，checkpoint 根清单必须来自同一个仓库 snapshot，不能
逐个创建 revision 后声称它们是同一时刻的应用状态。

文件系统一致不等于数据库等应用的一致。应用需要跨文件事务时，应自行 quiesce 或
执行它的 flush/checkpoint 协议，再请求 forkfs checkpoint；daemon 不能猜测事务边界。

## 4. 在线备份协议

由当前拥有 LevelDB 的 forkfsd 提供任务接口，备份客户端不再另开数据库。
现有 LevelStore 已支持锁内取 snapshot、锁外导出，但尚未提供这套在线控制协议。

1. 校验 request_id、目标目录权限、备份格式和空间预算；登记任务意图。
2. 在已有锁顺序下取得提交屏障：namespace/container 锁在前、store writer lock 在后。
   排空屏障前已接受的写请求，必要时创建用户请求的 revision。未来若增加延迟提交，
   该屏障必须显式 flush；不能把进入内核缓冲区当成备份持久化确认。
3. 在 writer lock 内启用日志留存，取得 LevelDB snapshot 和对应的 sequence/head，
   同时建立活动导出的日志保留 pin。创建 revision 的路径应返回提交凭证；若要求
   checkpoint 恰好等于该提交，应在同一串行协调区内捕获 snapshot，不能事后再读水位。
4. 释放锁，锁外扫描 snapshot，生成完整基线，检查对象哈希及传递引用。
   后续写入继续提交，并原子记录 replay 日志。
5. 写入备份介质的临时包，完成 footer、文件同步、校验和原子发布。
6. 发布 catalog，记录 checkpoint 与包身份；完成介质持久化确认后才更新任务为 durable。
7. 释放 snapshot 和活动 pin；备份策略如需连续增量，转由持久 retention lease 接管。

建立 pin 与取得 snapshot 必须在同一裁剪协调区内；失败、取消、daemon 重启需要恢复
或解除相应 pin。LevelDB snapshot 不是可跨进程重启恢复的备份句柄：重启后未完成的
基线任务重新取新 snapshot，使用新的 checkpoint/包，不续写旧的半包。

长时间持有 snapshot 会阻碍 LevelDB 回收旧版本。首版限制每仓库一个基线导出、设置
最长任务时间及磁盘阈值，支持取消；读吞吐、扫描缓存和校验工作需要限速。
revision 创建成本主要是根引用；完整备份仍是 O(仓库对象量)，不能宣称 O(1)。

## 5. 包与 catalog

首阶段复用 FFRPLY01 的 B/I 包，不修改既有事务语义；新增版本化 catalog 包装层。
catalog 独立于源数据库保存，源盘完全丢失后仍能选择恢复路径。

```text
backup-root/
  objects/<package_digest>.pack
  manifests/<generation_id>/<manifest_digest>.json
  generations/<generation_id>/CURRENT
```

包不可变，以整个包的长度和域隔离摘要定位。manifest 至少包含 schema version、
仓库 UUID、generation ID、前一个 manifest 摘要、基线位置、连续 delta 区间及两端
链哈希、checkpoint/revision 事件索引、每个包的长度/摘要、校验等级和 retention 信息。
时间戳用于展示和策略，不替代 sequence 排序。数据格式版本与 catalog 版本分别管理。

本地目录发布顺序：写临时包 → fsync 包 → 同文件系统原子发布不可变包 → fsync 父目录
→ 写并同步不可变 manifest → 同步更新 CURRENT 及父目录。任一步失败都不返回 durable。
CURRENT 损坏时允许扫描校验通过的 manifests，必须验证完整依赖链，不能盲选最新时间戳。

任务状态为 `requested → capturing → exporting → verifying → publishing → durable`，
并允许转入 failed/cancelled。request_id 与 generation 绑定，重复请求返回同一任务；
如果进程在发布后、回复前崩溃，通过已发布 manifest 对账，避免重复创建备份身份。
源端任务状态不是独立备份可恢复的证据，备份介质上的有效 manifest 和包才是。

## 6. 增量导出和 replay

增量从上一个 durable 水位开始，在固定 snapshot 上选择结束水位，导出连续的完整事务。
包可按大小或时间切分，但不能拆开单笔事务；日志中的完整新对象和根变更足以恢复，
不能只保存“写入某路径”等需重新执行的命令。先沿用现有对象编码，压缩、加密与跨包
去重作为后续可测量优化，不先增加第二套对象寻址和 GC 协议。

恢复流程：

1. 解析目标 checkpoint 或 revision 创建事件，生成确定的 restore plan。
2. 验证 catalog、所有必要包和基线/增量区间，检查 repository UUID 与链衔接。
   缺包、乱序、分叉、未知版本都明确拒绝，不自动跳过坏 revision。
3. 在目标同目录的 staging 中恢复基线；原有目标绝不覆盖。
4. 逐笔校验并原子应用事务，提交数据与恢复水位使用同一个 sync WriteBatch。
5. 若目标位于包中部，仅提交到指定 sequence，验证该处链哈希及 revision 事件。
   这是新增能力；现有 CLI 默认应用整个包。不得通过执行到包尾再修改水位伪造回退。
6. 验证对象/引用图，再对全部待交付 World 和 revision 执行 inode/目录语义检查。
7. 发布完整目标，输出包含实际水位、检查结果及包摘要的恢复报告，之后才允许挂载。

为保证多包恢复对用户可见时已完整，新增 restore job 将整个计划的目标放在私有 staging，
最终一次发布。底层 replay 仍允许失败时留下完整事务前缀以便重试，但该前缀不等于
整个 restore job 成功。当前单包 CLI 的发布行为保持不变，不能混淆两层保证。

恢复重试读取实际持久水位，接受同 lineage 的重复和重叠事务；拒绝本地写入产生的
分叉。恢复过程中禁止普通写服务及 orphan 清理。完成检查并显式接管后，才打开普通
Container 执行正常 orphan 恢复；内存句柄不复活。接管后的本地新提交不能再盲目追赶
原源端，后续复制需要显式重新建立基线或重新选择 lineage。

恢复输入必须是稳定、只读的已验证包。恢复缓存按摘要固定包，重新校验后使用；
不能让后台下载覆盖正在读取的路径。现有校验哈希仅用于损坏检测，不能证明来源可信。

## 7. 日志保留和 GC

区分三种独立的生命周期：源端 replay 日志、源端 CoW 对象、备份介质上的包。
对其中一类做 compaction 不代表另外两类可以删除。

每个连续备份消费者持有 lease，记录下一次导出起点 `q_i` 和已持久接收的链哈希。
活动导出也持有 pin。有效消费者需要 `(q_i, current]`，因此在所有约束满足时，
`L/s` 的候选删除条件为 `s <= min(q_i)`。额外保留窗口只能使删除边界更早，不能更晚。
无消费者时不得把空集最小值解释成无限大；是否关闭保留必须由明确策略控制。

推进 lease 前须验证该介质已拥有可独立恢复的基线及连续包，并确认 durable manifest。
只导出到源盘临时文件、上传结束但未确认发布、或仅持有 revision ID，都不能推进水位。
一个离线消费者可能阻止裁剪；超过容量阈值时报告并要求显式过期该 lease，随后该消费者
必须重新建立基线，不能静默丢弃日志后继续承诺旧恢复范围。

裁剪要保持现有 `replay_start/base/head` 语义：删除到 p 后，必须保留 H(p) 为新起点锚。
每一小批同时删除连续前缀记录并更新 start/base，使用同步原子批次；head 保持不变。
裁剪与事务提交、导出 pin、恢复校验协调，重启后仍有完整可验证后缀。
日志裁剪作为存储维护不增加逻辑 sequence，也不写入被复制的业务事务日志。
历史重叠重试若从已裁剪水位开始，明确返回需要新基线；不能未经验证地跳过未知前缀。

CoW 对象 GC 必须考虑活跃 World、命名 revision、World base、活动 checkpoint/export
以及现有句柄/快照生命周期。备份已导出的对象不要求永久留在源盘，但活动导出不能
读到被回收的对象。CoW 层日志记录自带对象内容，不依赖未来从已删除对象重建事务。
首版只做日志裁剪，不借此引入未经设计的 CoW 对象 GC。

备份端删除按完整依赖链计算：旧 base 仍被 delta 链依赖时不可删除；保留单个增量包
或单个 revision 标签没有恢复意义。先原子更新 catalog 的保留集合，再等待读者 pin
释放，最后回收无引用包；中断最多残留可回收文件，不使已发布 manifest 悬空。

## 8. 确认语义与故障域

| 确认 | 表示什么 | 不表示什么 |
|---|---|---|
| local commit/fsync | 按本地存储契约持久化事务和 replay 记录 | 独立备份已收到 |
| checkpoint captured | 固定了一致视图和导出 pin | 已完成基线导出 |
| backup durable | 指定介质上的 manifest 和全部依赖已持久发布 | 一定跨越源盘故障域 |
| remote durable（后续） | 配置的远端目标完成持久确认 | 多副本共识或自动写者接管 |

同一 APFS 盘上的另一个目录可用于误删恢复，不抵御整盘丢失；策略应标注介质故障域。
RPO 按目标介质最后确认水位计算，RTO 通过实际演练测量。未来 `fsync=cloud` 的确认需要
对应提交的全部恢复依赖均在远端 durable；超时必须报告未确认，不能静默降级为 local。
云确认不能把每次 fsync 都变成一次全量备份，应使用已持久基线加批量日志传送。
这些确认策略与传输均为后续设计实现，不改变当前本地 fsync 的含义。

## 9. 接口草案

以下为协议意图，不是当前可执行 CLI：

- `backup.create(request_id, target, optional_revision)`：返回 task_id。
- `backup.status(task_id)`：返回阶段、固定水位、字节进度、确认范围和错误。
- `backup.export_delta(lease_id, target)`：按 lease 导出并发布连续区间。
- `backup.cancel(task_id)`：取消当前任务，清理本次临时文件并释放 pin。
- `backup.list(target)` / `backup.verify(generation_id)`：发现和验证独立备份链。
- `restore.plan(target_checkpoint | revision_event, destination)`：列出包及准确终点。
- `restore.run(plan_id)`：离线恢复并生成报告；失败保留可验证进度。
- `backup.expire_lease(lease_id)`：显式放弃该消费者的连续追赶保证。

RPC 应返回结构化的 missing-range、wrong-lineage、corrupt-package、invalid-graph、
insufficient-space、unsupported-version、target-exists 等错误及实际持久水位。
源端重启后任务对账和 lease 状态恢复必须早于任何日志裁剪。

## 10. 验证与交付顺序

延续独立状态模型和 fuzz，不用生产元数据计算期望结果。随机交错写入、revision、
fork、restore、备份、导出、取消、裁剪和重启；随机选择保留目标并验证可恢复性。
除了正常随机 seed，还应记录并最小化失败的操作序列与实际包。

关键不变量和故障点：

- checkpoint 的根清单、sequence 和链哈希始终来自同一一致提交点；并发写入不混入基线。
- revision 文件树恢复与仓库创建点恢复结果分别符合独立模型；基线过新必须拒绝回退。
- 发布包、manifest、CURRENT、ACK、推进 lease 和裁剪每个边界注入进程退出/IO 失败。
- ACK 丢失、重复任务、daemon 重启、消费者过期和两代基线切换不造成日志缺口。
- 正确重算哈希的坏 revision/CoW 图、丢包、错链、磁盘满都不发布错误的完整目标。
- 裁剪前后任一承诺保留的恢复点都能在删除源仓库后重建；其他任务的临时文件不受清理影响。
- 压测记录导出期间前台操作 p95/p99 延迟、snapshot 持有时长、日志增长、峰值空间及恢复速度。
  先测无备份基线，再确定预算；此文不虚构吞吐指标。

交付分四阶段：

1. **在线一致 checkpoint 与任务控制**：提交凭证、事件索引、持久 pin、daemon RPC、取消/重启。
2. **本地备份 catalog 与恢复计划**：不可变发布、精确恢复终点、多包 staging、完整验证报告。
3. **保留策略与日志裁剪**：lease、容量限制、原子前缀裁剪、依赖链回收及随机故障演练。
4. **远端介质适配**：上传下载、条件发布、认证/加密、remote ACK；单独设计写者接管。

每阶段以本地故障测试通过为前提；阶段 1–3 不依赖网络功能。
