# Repository Replication / Failover API

状态：需求与接口设计提案，未实现。它是 [Sync API 总契约](REVISION_SYNC_API.md)
中的 `repository-replication/1` 与 `failover-control/1` profile。已有 mock 只验证
revision-closure 的传输与恢复，不验证本文件的写者接管协议。

## 1. 范围和不可变条件

首版 failover 单位为整个 repository，覆盖所有 World、命名 revision、World base、
分配及命名空间状态，以及尚未创建 revision 的持久提交。它是逻辑管理单位，不要求
每个 World 使用独立物理卷。跨仓库事务和多个活动写者不在此 profile 中。

必须保持：

1. 每个 group 同一时刻只有一个被授权的活动写者；备份接收成功不产生写权限。
2. 接管点是已验证且可恢复的完整事务边界，不能只给 revision ID 或最大 sequence。
3. 获得更高 epoch 不会自动阻止旧主在自己的 APFS/XFS 上写入；激活新主之前必须
   完成可验证的旧主停写/fencing。仅 timeout、ping 失败、DNS 更新或签名 token 不够。
4. `fsync=remote` 返回成功前，数据必须满足当时的持久性策略，且保护水位已由 authority
   记录；接管不得丢掉该策略承诺的提交。策略变更不能追溯降低既有承诺。
5. 计划、pin、fence、保护水位、接管证书及状态转换均持久化、幂等，崩溃后按事实恢复。
6. 原主回归先隔离；未同步尾部不能覆盖新主历史，也不能直接接上新主日志。

应用一致性仍需应用自行 checkpoint/quiesce。文件系统恢复不复活进程内存、RPC session、
打开句柄或文件锁，不能据此宣称 PostgreSQL 等应用已完成自身恢复。

## 2. 两种工作模式

| 模式 | 分区/离线写入 | 接管行为 |
| --- | --- | --- |
| `local-first-backup`（默认） | 本地继续工作，后台备份，按本地 fsync 合同返回 | 允许独立还原成新仓库；不宣传自动无分叉接管 |
| `managed-single-writer` | 仅有效写者可写；资格不确定、lease 过期则停写 | 通过 authority、fencing 和完整仓库复制实施受管接管 |

不能从 managed 模式静默降级成离线 local writer。若业务需要分区两侧继续写，必须
显式创建独立仓库/分支及新身份，之后由用户合并，而不是同时使用原 group 的写权限。

managed 模式需要独立、可线性化的 authority（例如满足该合同的共识服务）。一份
异步复制的本地 LevelDB、普通对象桶 listing 或一个无条件覆盖的 HEAD 文件不能承担
该角色。authority 不保存文件 payload；它保存组配置、写者状态、epoch、保护水位与
接管决策。备份存储服务可以同时提供它，但必须单独实现其一致性与故障域合同。

## 3. 身份和水位

JSON u64 均用十进制字符串。UUID 为 32 位小写 hex，hash 为 64 位小写 hex。

```json
{
  "group_id": "<stable-group-uuid>",
  "authority_term": "<authority-incarnation-uuid>",
  "writer_epoch": "42",
  "stream_id": "<stream-uuid>",
  "repository": "<logical-repository-uuid>",
  "sequence": "12345",
  "chain_hash": "<64-hex-hash>"
}
```

这是 `RecoveryPoint`。只有同一 group/term/stream 或有已验证 lineage certificate
连接的点才能比较。seq 更大、时间戳更晚或 descriptor.source_sequence 更大，都不
证明状态更新或覆盖。revision frontier 仍是 DAG 集合，不能替代事务水位。

- `group_id`：稳定的受管复制组身份；与特定主机、World 或挂载点无关。
- `repository`：完整仓库复制保留的逻辑仓库身份。
- `instance_id` + `boot_id`：具体副本及本次进程/启动身份；恢复副本不能复制主机身份
  凭证后自称原主。grant 绑定经过认证的实例、启动身份和 activation_id。
- `writer_epoch`：authority 分配的单调 fencing 代次；已预留/使用的值不回收。
- `stream_id`：每次激活的新写入流。原主的旧流不能冒充新流继续发布。
- `authority_term`：authority 自身恢复/更换后的身份。不能恢复旧 authority 快照并
  继续发放可能重复的旧 epoch；更换 term 前必须重新 fencing/核对所有潜在写者。

副本分别报告以下点，不能把它们统称为 synced：

| 点 | 条件 |
| --- | --- |
| `local_durable` | 当前写者本地事务及对象按本地存储契约持久化 |
| `stored` | 目标已持久保存 bytes；尚不证明完整性或可恢复 |
| `recovery_verified` | 独立验证基线、连续日志、对象及根图后，可恢复到该完整提交 |
| `applied` | standby 数据库已原子应用到该点并通过所需验证；没有普通本地写入 |
| `protected` | authority 已确认达到指定 policy 的恢复副本集合，允许相应 remote fsync ACK |

所有点必须包含完整身份/hash；空值表示没有该保证。readiness 还包含 codec 版本、
校验策略版本、损坏状态、最新验证时间及本地应用任务，不用一个布尔值掩盖落后或损坏。

## 4. 完整仓库复制通道

保留 revision 对象去重通道，同时增加仓库状态通道。它必须能够导出：

- 一致基线：全部逻辑 roots、对象、World/revision 绑定、分配/父关系及支持的控制状态。
- 连续增量：同流严格连续 `(from_point, to_point]` 的**完整事务**，包括未建 revision 的提交。
- 精确恢复：完整事务边界的截断、from/to 链验证、缺口检测和独立 namespace/对象验证。

实现起点可复用当前 `FFRPLY01` base/delta 的 canonical bytes；每个传输 manifest
额外绑定 group、term、epoch、stream、from/to point、archive 长度及 `artifact_sha256`
（对整个 archive bytes 的普通 SHA-256）。接收端同时校验外层授权/stream 和内部 archive
的 repository/sequence/chain。旧 archive 裸 header 没有 epoch，不能单独充当 managed
授权证书，也不能通过调用离线 replay 绕过 active writer 检查。

需要逐对象去重时可新增已协商的 archive codec，但不能拆事务后提前推进恢复水位。
当前 FFRPLY01 的白名单与物理格式不支持的新键/状态须明确升级；旧二进制必须拒绝
managed 仓库写入，不能忽略控制文件后按 standalone 模式打开。

### 源端接口

沿用 `R = /v1/repositories/{repository}`：

| 方法/路径 | 必要语义 |
| --- | --- |
| `POST R/replication/subscriptions` | 建立连续复制消费者，固定 group/config/policy，持久登记日志保留 pin |
| `GET R/replication/subscriptions/{id}` | 返回原始请求、状态、lease、可导出范围、已确认完整恢复点 |
| `POST R/replication/subscriptions/{id}/renew` | 续复制保留 lease；与 writer lease 不同 |
| `POST R/replication/checkpoints` | 在提交屏障下固定完整基线；返回有界后台任务，不长时间占 writer lock |
| `POST R/replication/subscriptions/{id}/exports` | 从精确 `after_point` 导出到已提交边界；返回 immutable manifest 与 artifact IDs |
| `GET R/replication/exports/{id}` | 元数据、固定 from/to、摘要、pin、准备/验证进度 |
| `GET R/replication/exports/{id}/artifacts/{artifact_id}` | 受 membership/lease 限制的 archive 数据，不能传入任意路径/URL |
| `POST R/replication/subscriptions/{id}/acks` | 分别登记 stored/verified/applied receipts；只按足够的持久恢复保证推进保留点 |
| `DELETE R/replication/subscriptions/{id}` | 显式过期消费者，声明后续可能必须重取基线；不删除既有远端备份 |

subscription 申请示例：

```json
{
  "consumer_id": "replica-b",
  "destination_id": "zone-b-storage",
  "group_id": "<uuid>",
  "expected_config_generation": "7",
  "after_point": null,
  "profile": "repository-replication/1",
  "lease_seconds": 900
}
```

`after_point=null` 请求基线，非 null 必须给完整 RecoveryPoint。增量导出请求还包含
`through_point`（可为空，表示捕获当前完整提交）、`max_bytes` 和 request_id。
单个事务超过当前预算须明确报错/协商更大预算，不能截成“半个可恢复提交”。

日志已裁剪返回 `BASE_REQUIRED` 和可用基线范围；不能跳过缺失区间。保留消费者恢复
需要校验原 receipt 和 from hash，而非猜测本地文件名或仅用 sequence。

archive 按 immutable artifact ID 提供有界 Range 重传；目标校验完整 archive hash、
内部 frame/footer 和所有依赖前不能确认 recovery_verified。传输中断时部分 bytes
只是 staging。它与 revision pack 的按对象续传是不同 codec 的两种恢复单位。

### 目标副本接口

`D = /v1/replicas/{instance_id}`：

| 方法/路径 | 必要语义 |
| --- | --- |
| `GET D/status` | role、boot_id、上述水位、校验/应用进度、错误及资源状态 |
| `POST D/apply-jobs` | 申请应用已授权 group 的 manifest/artifacts，带 expected_current_point 和 request_id |
| `GET D/apply-jobs/{id}` | 持久进度、实际完整提交水位与不可重试原因 |
| `GET D/receipts/{id}` | 可验证的不可变 stored/verified/applied receipt |
| `POST D/readiness` | 固定目标点和配置版本，验证后创建 takeover pin/readiness receipt；不授予写权限 |
| `POST D/activations` | 仅 authority 授权的控制者可安装精确 activation grant；绑定 transition、instance/boot、point、epoch/stream；持久保存后仍等待 ACTIVE |
| `GET D/activations/{id}` | 查询已安装 grant、持久恢复点和本地写入 gate；重试不得重复激活 |
| `DELETE D/readiness/{id}` | 释放已结束接管任务的 pin；不得回收当前激活所需数据 |

replica 只能拉取已登记连接里的授权 artifact ID，不执行客户端指定 URL/文件路径。
apply 期间为显式 standby/recovery 模式，禁止普通 Namespace 事务、orphan 清理、
后台逻辑 compaction 和其他改变根状态的维护。每次提交与实际 applied watermark
共同持久化；重复/重叠区间核对 hash 后可幂等重试，分叉或缺口必须拒绝。

`applied=q` 不能表示“异步任务曾读到 q”。ready receipt 绑定
`group/term/epoch/stream/point/instance/boot/config_generation/validator_version`、
根集合摘要、依赖 pins 及到期时间。源端丢失时，restore/readiness 不依赖再联系旧主。

## 5. ACK、RPO 和保护策略

组配置明确给出 `fsync_policy`、required destinations/quorum、故障域、验证等级、
保留与超时策略，并有不可变 `policy_version`。不同目标的“单盘 fsync”“对象后端完成”
“跨区 quorum”不是同一合同，故障域身份由配置授权，不由 receipt 自报。

### local

`fsync=local` 在本地持久提交后返回；异步远端可能落后。接管点若早于原主已 ACK 的
尾部，可以丢失该尾部。原主不可访问时，丢失范围可能未知，不能报告 RPO=0。

### remote

```text
本地 durable 到 q
 -> 所需目标持久接收并验证可恢复到 q
 -> authority 线性化提交 protected=q（绑定 epoch/stream/policy 和 receipt 集合）
 -> 向应用返回 remote fsync 成功
```

protected 是同一已验证链的连续前缀。可批量推进到较大 q 以合并多次 fsync 等待，
不要求每个文件一个云往返。authority 的保护记录自身必须满足其持久/共识合同。
该登记与封存旧 epoch 共同序列化：fencing 开始后旧流不能再推进普通 protected。
已经登记、但延迟到达应用的 ACK，其点已包含在封存 protected 中。

远端/authority 不可达或超时，不能把 remote fsync 降为 local 成功。返回明确的
`PROTECTION_UNAVAILABLE`/超时和 request_id；本地写入可能已经存在，重试/查询以
同一请求及真实水位对账，不能假设失败表示本地回滚。

RPO=0 的受管接管必须保留 seal 时的 protected 点及更早所有承诺，并在配置定义的
故障模型内至少有一份有效数据链。若所有保护副本不可访问/已损坏，则拒绝零丢失
接管，不能选择一个更旧、看似可用的副本后继续宣称零丢失。

`lag_seconds` 只作带采样时间/时钟不确定性的观测。没有可信旧主最新水位时，不能
把最后一次 heartbeat 的时间差当作严格丢失秒数。loss report 必须区分已知已保护
缺口、已知 local-only 尾部和不可观测尾部。

## 6. Authority 和 fencing

`A = /v1/replication-groups/{group_id}`：

| 方法/路径 | 合同 |
| --- | --- |
| `GET A/state` | 线性化组状态、配置代次、owner、epoch/term、stream、protected 和 route |
| `POST A/writer-leases/renew` | 仅当前 instance/boot/activation 能续租；不是抢主 API |
| `POST A/protection/commit` | 校验当前策略、授权 receipts 与链前缀后，持久推进 protected |
| `POST A/transitions` | 创建 switchover/failover/failback 计划，固定 request_id、expected epoch/config 和候选副本 |
| `GET A/transitions/{id}` | 返回持久状态、已完成步骤、sealing/fencing/readiness/loss 报告 |
| `POST A/transitions/{id}/prepare` | CAS 进入排空/fencing，预留新 epoch，固定恢复点并准备目标 |
| `POST A/transitions/{id}/accept-loss` | 仅恢复管理员授权，对精确 sealed_plan_hash/loss_report 明确接受丢失 |
| `POST A/transitions/{id}/activate` | 检查同一 sealed plan、fence/readiness、当前代次，登记唯一 activation grant |
| `POST A/transitions/{id}/activation-ready` | 目标报告已持久安装 grant 后，authority 发布 ACTIVE 与 route |
| `POST A/transitions/{id}/candidates` | fencing 后换候选副本；固定恢复点不变，重新生成 plan/readiness |
| `DELETE A/transitions/{id}` | 仅无停写/fence 副作用前可取消；之后不能自动恢复旧 epoch |

所有变更使用认证角色、Idempotency-Key 和 expected generation。备份读取/写入凭证
不含 activate、修改策略、accept-loss 或运行 fencer 的权限。网络探活只提供怀疑信号，
不能由 backup server 自行把“对方失联”升级成写者 grant。

源端另提供 `POST R/writer-transitions/{id}/quiesce` 和
`GET R/writer-transitions/{id}`：仅接受 authority 授权、绑定当前 epoch/boot 的
transition，持久排空后返回冻结证明与最终点。重复请求返回同一结果；不得通过取消
export 或重启清除 frozen 状态。普通备份凭证无权调用。

### Fencer 合同

只有以下证据可在激活前证明旧主不再写：

- **协作排空证明**：旧 owning daemon 已阻止全部新修改，排空已接受修改与后台任务，
  同步最终提交，持久写下 frozen 状态；重启必须先读 frozen 状态并向 authority
  获取新资格。证明绑定原 instance/boot/epoch、最终点和 transition_id。
- **外部 fencing 证明**：受信任控制系统完成主机停机、隔离其实际写入路径或等价硬
  fencing，且覆盖进程暂停/恢复、旧 VM 重启和已发出的 I/O。证明绑定具体资源与
  boot/epoch，authority 能验证。普通网络隔离、DNS 切换、源自己说“已死”不够。

外部 fencer adapter 至少提供 `POST /v1/fences` 和 `GET /v1/fences/{id}`。
请求绑定 request_id、transition_id、group、authority_term、原 epoch/instance/boot、
具体资源集合和 expected 配置版本；重复请求必须返回同一任务。完成证明携带执行者、
资源实际状态、完成时间、证据摘要和持久禁止旧资源重新写入的约束。authority 验证
身份、范围及约束仍有效；任务已接收/超时均不能作为完成证明。重启或解除隔离必须
走新的授权流程，不能让已消费的证明失效后旧主自行回归。

无法获得任一证明则返回 `NOT_FENCED` 并保持新主不可写。不存在绕过 fencing 的
`force=true` 安全接管选项。管理员可独立 restore-as-fork 到新身份，但不能把它路由
成原组的第二个活动写者。

writer lease 是有效性和停写约束，不是本地磁盘上的远程锁。旧进程可能在 lease
检查后暂停、在过期后继续 I/O，因此仅“等 lease 到期”不足以 fencing。若未来选择
共享提交网关代替外部 fencer，它必须在线性化的实际发布/应用 ACK 路径拒绝旧 epoch，
并证明不存在直写/缓存资格旁路；普通签名 token 或提交前检查不能替代这个保证。

## 7. 接管状态机和协议

```text
ACTIVE(A, E)
 -> PLANNED
 -> DRAINING（planned）或 FENCING（unplanned）
 -> FENCED + SEALED(point P, protected P_required)
 -> CATCHING_UP(target)
 -> READY(sealed_plan_hash, readiness, loss decision)
 -> ACTIVATING(B, E_new, stream_new)
 -> ACTIVE(B, E_new)
```

`PLANNED` 是观测性计划，不代表恢复点已冻结。prepare 后生成的 sealed plan 是
不可变事实，至少包含：group/term、旧/预留 epoch、旧写者身份、候选身份、精确恢复点、
封存 protected、基线/日志依赖、fence proof、策略/配置版本、loss report、pins 和有效期。
后续批准与激活绑定 sealed_plan_hash，不能在审批/激活时再解析“latest”。

### 7.1 计划切换 / failback 的切换阶段

1. 校验 expected epoch/config，确认旧主仍是 owner；预留单调新 epoch。
2. 请求旧主排空。新修改返回明确的 `DRAINING`，已接受操作完成/失败并对账；后台
   维护也通过同一 gate。获得 final barrier point Q 和持久 frozen 证明。
3. 使 Q 的完整依赖达到目标持久/验证策略，在 authority 原子封存旧流至 Q。
   排空期间普通保护记录可继续推进；封存之后不能再推进旧流。
4. 目标应用到 Q，验证完整仓库、目录/inode、控制状态，固定 readiness pin。
5. 按下面的两阶段 activation 发布新 owner。计划切换要求无已确认提交丢失。

### 7.2 非计划 failover

1. 检查 authority quorum 与策略，CAS 进入 FENCING 并封存当前 protected，拒绝旧 epoch
   的普通保护推进与 owner 续租；任何新主尚不可写。
2. 对旧主执行并确认外部 fencing。不可达只是原因，不是证据。
3. 从独立目标收集实际 verified/applied 水位、不可变 receipts 和完整依赖证据。
   零丢失策略至少恢复到封存 protected；缺数据则等待/修复或保持 BLOCKED。
4. async/local 模式或灾难中若选择更旧点，生成 loss report，写明受影响范围与
   `unobserved_tail`。受控恢复权限必须接受精确计划的损失；不能只传“允许一些丢失”。
5. 完成目标恢复/验证/pin，得到 READY，然后激活。所有动作按 request_id 幂等恢复。

示例 transition 请求（创建时还没有 sealed 恢复点）：

```json
{
  "request_id": "<caller-id>",
  "kind": "failover",
  "expected_authority_term": "<uuid>",
  "expected_writer_epoch": "42",
  "expected_config_generation": "7",
  "target_instance": "<replica-b-uuid>",
  "recovery_policy": {"require_zero_loss": true}
}
```

若 strict 策略不满足，任务返回 BLOCKED 的原因及证据。显式有损选择不能修改既有
sealed plan 来隐藏损失；产生新 plan_hash/loss_report，失效旧 readiness/授权。
接受低于 protected 的恢复点是明确放弃原零丢失承诺的灾难恢复决策，必须单独审计，
不能回写或降低历史 protected 来伪称原 remote ACK 已兑现；require_zero_loss=true
的计划始终不能激活该点。

### 7.3 两阶段 activation

1. authority 在一个线性化变更中记录 `ACTIVATING`、唯一 target、新 epoch/stream、
   activation_id、精确接管点和前序 lineage certificate。旧资格不可恢复。
2. 目标以专用控制接口持久安装 grant 和 managed role，确认 applied 点完全匹配、
   本地 boot_id 未变化；仍不接收普通修改，返回 activation-ready receipt。
3. authority 校验 receipt，原子记录 ACTIVE 和带 epoch 的 route。目标确认该 ACTIVE
   决策后才放行普通操作。route/DNS 发布失败不撤销或重复分配 epoch，重试同一激活。

authority 决策是 owner 的线性化点，但不能假设跨两台机器有原子 HTTP 提交。中间
崩溃通过 durable activation_id 查询/重试，既不让目标提前可写，也不让旧主复活。
ACTIVATING 目标失败后改用其他目标，须重新 fencing/撤销已授予目标、分配更高 epoch，
不能把同一 grant 复制给两个实例。已 ACTIVE 则必须创建下一次受控 transition。

取消仅在确认没有排空/fencing 副作用时安全；有副作用或结果未知则维持 BLOCKED/
FENCED，不能自动解除旧主冻结。目标更换须新 readiness 和 sealed plan，并失效旧
accept-loss/activation 请求。authority 不可用时不能“猜测上次激活没发生”。

## 8. 新流、旧主回归与 failback

完整 replay 的候选副本在切换前保持原 repository/sequence/chain。新 stream 的
`parent_point` 精确引用旧 stream 的接管点 Q，并由 authority 的不可变 lineage
certificate 连接。首个新事务从 Q.sequence+1、prev=Q.chain_hash 开始；不重写旧
archive/hash，也不因新 epoch 伪造空事务或改变原 revision ID。

跨 stream 恢复需要验证这份 certificate、旧流已封存的上界和新流授权；不能仅凭
两个 archive 的 sequence 恰好接得上。FFRPLY01 本身不编码 stream/epoch，这些验证
由新协商的控制 manifest 完成。控制状态必须持久恢复，不能只在进程内记住。

原主回归：

1. 在对外服务之前查询 authority，发现旧 epoch/boot/stream 后进入 QUARANTINED。
2. 旧尾部只读保留并可另存为有 provenance 的独立分支。不能自动覆盖当前组、修改
   protected，或把自己旧 stream 的增量发到新 stream。
3. 从当前已认证基线/lineage 重新 seed，或在证明共同前缀后丢弃隔离副本工作状态并
   重建 standby；保存旧尾部后才替换，重建过程不抢占当前主写权限。
4. 追赶新主并取得新 readiness。failback 再执行 planned switchover，分配更高 epoch
   和新 stream；不是重新启用故障前的 token。

会话与打开句柄不迁移。旧 session/epoch 的访问返回 FENCED/ESTALE 或适配的 I/O
错误，客户端重新发现 active route 并重新打开。挂载缓存、DNS TTL 和用户持有旧端点
不能产生写权限。immutable revision 的读取可继续用独立 read-only 授权。

## 9. 故障和恢复矩阵

| 情况 | 要求 |
| --- | --- |
| 原主活着但网络隔离 | 不能仅凭失联激活新主；确认 fencing，lease 不确定时旧主停写 |
| authority 无 quorum | 不续发写者资格、不激活；remote fsync 不返回成功；无自动 local 降级 |
| 原主本地 ACK 尾部未备份 | 明确 async 丢失风险/未知尾部；拒绝无依据的 RPO=0 |
| 目标 stored 到 Q、但未验证/未 apply | 不 READY；校验并追赶到精确 Q 后才激活 |
| 对象完整、事务链有缺口或错误 hash | 拒绝/重取基线，不能跳过坏事务 |
| 源 export/retention lease 过期 | 重建基线或 export；不会自动终止 writer lease，也不证明旧主已停写 |
| fence 已完成、目标准备失败 | 保持无人可写，恢复同一任务或更换候选并重做证明 |
| activation 响应丢失 | 查询 authority 的同一 activation_id，不重新发 epoch/写权限 |
| 新主已激活、原主重新启动 | 隔离旧主；保留尾部为独立历史，不能盲目合并 |
| 最后有效备份已损坏/失去保留 | readiness/eligible 失效；不能仅看旧 receipt 的 success 标记 |
| 目标存储已提交、源 ACK 丢失 | 数据可恢复，不等于 protected 已登记或 owner 已切换 |
| 权限/策略变更与 transition 并发 | expected config/policy CAS，旧批准不得套用新目标/更弱策略 |

## 10. 错误、状态与可观测性

复用总契约的错误结构；控制接口再定义：

- `STALE_EPOCH`、`NOT_PRIMARY`、`FENCED`、`DRAINING`：不能靠重放旧 token 修复。
- `WRONG_STREAM`、`WRONG_LINEAGE`、`LOG_GAP`、`BASE_REQUIRED`：重新规划完整恢复链。
- `PROTECTION_UNAVAILABLE`、`AUTHORITY_UNAVAILABLE`：不能降级 durability 成功。
- `NOT_FENCED`、`TARGET_NOT_READY`、`TARGET_CORRUPT`：不激活。
- `LOSS_ACCEPTANCE_REQUIRED`、`PLAN_STALE`、`POLICY_CHANGED`：返回精确计划/版本原因。
- `TRANSITION_NOT_CANCELLABLE`、`ACTIVATION_IN_PROGRESS`：按持久事实恢复，不复活旧资格。

数据面重复请求按对象/事务 idempotence；控制面幂等范围包含 group、principal、request_id
及规范化申请。相同 key 改参数返回冲突。查询终态不因 lease 过期消失，保留期须明示。

状态必须给出：全部水位、receipt/policy、当前 owner/epoch/term、route_generation、
复制缺口、fence/readiness 证据、loss_report、每步时间和阻塞原因。RTO 分解为检测、
fencing、下载/追赶、验证、激活及应用重新打开；不能把一次 mount 的 300ms 作为整体
failover RTO。RPO/RTO 的实测和上限分别报告。

## 11. 实施和验收要求

启用 managed 前必须：冻结现有 standalone owner -> 安装旧版不能忽略的格式/模式
保护 -> 建立完整基线和连续复制 -> authority 登记 genesis/policy -> 发放唯一 grant。
失败中断不得在未受保护的旧格式上提前宣告 managed 已可 failover。

验收采用独立状态模型、可控时钟和确定性网络/进程故障调度，并覆盖：

- local ACK、remote receipt、protected 登记、应用 ACK 各边界崩溃，验证承诺与实际恢复。
- 双向/单向分区、进程长暂停后恢复、旧 lease 过期但旧 I/O 仍在执行、主机重启与
  陈旧凭证；没有 fence proof 时任何新主都不能可写。
- checkpoint/delta/对象分开到达、缺日志/缺对象、重排/重复、错误链与损坏基线。
- 两个接管请求并发、重复/乱序 activate、authority leader 更换、CAS 响应丢失，始终
  不出现两个有效 owner；authority 恢复不能回滚 epoch/term。
- standby 误运行普通事务/orphan 清理、readiness 后对象被 GC、policy 改变后使用旧批准。
- 计划切换排空、非计划 fence、目标准备/ACTIVATING 阶段宕机，逐步对账与幂等重试。
- 原主有未同步尾部时归来，证明尾部保留、隔离、新主历史不被覆盖及 failback 新 epoch。
- 从 revision-only 备份请求 managed failover 必须拒绝，不静默变成“最新快照接管”。
- 本地先行、远端较慢时的队列/保留空间、前台延迟；remote fsync 超时不伪造成功。
- 原主及源盘都丢失后完整还原所有 World/未建 revision 的提交，并在新 epoch 上继续
  写、创建 revision、fork、再次同步和接管，验证数据与 inode 一致。

上线条件是上述协议、真实 fencing adapter、持久控制状态和故障演练同时完成。
当前 revision mock 的 20 个通过用例不满足这些 failover 验收条件。
