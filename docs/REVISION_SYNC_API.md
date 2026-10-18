# Revision Sync API v1：面向外部 backup server

状态：设计提案，尚未实现。当前 daemon 的 Unix RPC、在线 backup/replay 与本地
catalog 不提供本文的网络 API。本文约定的是可独立实现的 backup server 契约。

## 1. 目标与恢复边界

采用 **backup server 主动拉取、内容寻址去重、验证后发布** 的模型。
`forkfsd` 负责固定 revision、保护依赖对象并导出；外部服务负责目标存储、保留策略、
独立验证、备份目录发布及 durable receipt。通过拟提供的 codec/validator SDK 接入，
不要求 backup server 打开源 LevelDB、读取 segment 偏移或持有本地文件路径。

首版同步单位为 `revision-closure`：所选 revision、全部祖先 revision，以及这些
revision 各自完整文件树所依赖的对象。包含 inode、目录、文件内容及受支持属性。
父边有多个时全部保留；只记录父关系不执行 merge。v1 不支持 shallow history。

| 能力 | revision sync v1 | 现有 checkpoint/replay |
| --- | --- | --- |
| 独立保存、浏览及恢复某个 revision 文件树 | 是，含全部祖先的树 | 包含目标 revision 时可恢复 |
| 去重同步多个 World 产生的 revision DAG | 是，按对象及完整闭包共享 | 按仓库包/日志链 |
| 恢复所有 World、名称、分配状态到某次事务 | 不提供此承诺 | 按 repository/sequence/chain_hash 定位 |
| 保存尚未创建 revision 的写入 | 不包含 | 保留日志覆盖的提交可以包含 |

只修改文件而未产生 revision，revision sync 不会自动保护这些写入。创建 revision
由源端策略或应用调用现有 snapshot 完成，外部只读备份凭证不能修改 World。
需要更低事务级 RPO 时另外传输 checkpoint/replay；不能以 `source_sequence` 或
`source_sequence + 1` 推断创建事务，也不能据此实现 `fsync=cloud`。

该协议不做多写者选举、自动合并、远端在线挂载、源端删除或原地回滚。

## 2. 身份、对象和格式

所有 JSON 中的 u64 均使用无前导零的十进制字符串，避免客户端数字精度损失。
对象 ID 为 64 位小写十六进制；repository ID 为 32 位小写十六进制。
名字仅用于发现与捕获时解析，之后以不可变 ID 为准。

| 字段 | 含义 |
| --- | --- |
| `source_repository` | 这次导出的来源仓库身份，不是 DAG 全局排序键 |
| `revision_id` | 原始 FFREV001/FFREV002 descriptor 的 canonical object ID |
| `manifest_id` | 原始 namespace manifest 对象 ID |
| `export_id` | 源端生成的不可猜测任务 ID，承担 lease 与授权范围 |
| `plan_hash` | 固定根集合、来源及完整对象 inventory 的内容承诺 |
| `inventory_root` | 同步 inventory radix tree 的根哈希，和文件系统 CoW 树不同 |
| `consumer_id` / `destination_id` | 经授权的备份消费者及其目标存储身份 |
| `backup_id` / `receipt_id` | 目标生成的不可变备份及持久确认身份 |

对象 ID 必须复用当前 `Journal::object_id(payload)`：

```text
SHA256(ASCII("forkfs.object.v0") || LE16(4) || LE16(0)
       || LE64(payload_length) || payload)
```

这里 domain 后没有 NUL。不能把 `SHA256(payload)` 当作对象 ID。
传输原始 canonical payload，不重编码 inode、revision 或 CoW 节点，不改变其 ID。
FFOBJ002、C/索引、segment ID/offset、LevelDB SST/WAL 不进入协议。
当前单逻辑对象最大 256 KiB；更大的文件/extent 格式将另行协商。

closure walker 按**引用上下文**识别类型：

```text
revision -> ordered parent revisions + namespace manifest
FFTREE01 manifest -> tree root
FFTRNO01 node -> left/right tree nodes + value object
legacy manifest/run -> run objects + referenced PUT value objects
value object -> leaf bytes
```

逻辑 inode ID、目录项里的 inode ID 不是对象哈希；文件内容偶然以 `FFREV002`
等 magic 开头也仍是叶对象。同一对象可在多个上下文出现：网络只传一份 bytes，
验证器必须验证每一种实际引用角色。旧 run 中被遮蔽的 PUT 引用也必须存在并校验。
可选 xattr/ACL 等只在 codec、导出与还原三方协商支持后列入承诺。

## 3. 对外 API

建议 HTTPS + JSON 控制面、二进制 pack 数据面。网络 listener 是独立、显式启用的
export adapter，调用 owning daemon；不直接暴露当前完整管理 RPC。
URI 前缀统一为 `/v1/repositories/{repo}`，以下简称 `R`。

| 方法与路径 | 用途 |
| --- | --- |
| `GET R/sync-capabilities` | 协议、对象/namespace codec、限制、源身份 |
| `GET R/revision-refs?cursor=...&limit=...` | 分页发现命名 revision 与稳定 ID |
| `POST R/exports` | 固定 revision 集合并创建持久 lease，异步构建 inventory |
| `GET R/exports/{export_id}` | 状态、计划、进度、lease 期限、错误 |
| `POST R/exports/{export_id}/renew` | 续租，不改变根集合和 plan |
| `GET R/exports/{export_id}/inventory/{node_hash}` | 读取此计划可达的 inventory 节点 |
| `POST R/exports/{export_id}/packs` | 批量读取此闭包中的对象 |
| `POST R/exports/{export_id}/acknowledgements` | 提交目标已发布的 durable receipt |
| `DELETE R/exports/{export_id}` | 幂等取消/释放 lease，不删除远端备份 |
| `GET R/consumers/{consumer_id}/coverage` | 查询本地记录的 receipts 和 revision frontier |

除 capabilities 外均要求 repository 与 consumer 范围授权；capabilities 本身也可以
限制在同一认证边界。capabilities 必须列出 protocol、hash suite、支持的 revision/namespace/inode codec、
单对象/pack/lease 限制和 revision-only restore/import 支持状态；不能仅以协议版本相同
就推断能还原所有属性。客户端传入的 ID、cursor 或 hash 都不是访问凭证。
不存在把任意文件路径、上传 URL、shell 命令传给源端的接口。

### 3.1 发现 revision

refs 返回 `name, revision_id, manifest_id, source_sequence`，并提供有界 listing view
的 opaque cursor、到期时间及 ETag。分页固定的是名称视图，**不固定内容存活**。
源端重启或 view 到期返回 `REF_VIEW_EXPIRED`，重新发现即可。

导出申请才是权威捕获点：名字可以附 `expected_revision_id`，解析结果变化返回
`REF_CHANGED`，不能悄悄同步新版本。直接 ID 选择必须仍属于调用者授权的可导出历史。
发现后已被 GC 的对象返回明确错误，不以空列表或最新 revision 替代。

### 3.2 创建和查询导出

以下 JSON 为模板，尖括号值需要替换成真实 ID。

```http
POST R/exports
Idempotency-Key: <consumer-owned-request-id>
Content-Type: application/json
```

```json
{
  "consumer_id": "backup-main",
  "destination_id": "backup-cluster-a",
  "scope": "revision-closure",
  "history": "full",
  "selectors": [
    {"name": "release-2026-10", "expected_revision_id": "<64-hex-id>"},
    {"revision_id": "<another-64-hex-id>"}
  ],
  "lease_seconds": 900
}
```

返回 `202 Accepted`、`Location` 和 `export_id/state/expires_at`。任务 ready 后：

```json
{
  "export_id": "<opaque-id>",
  "state": "ready",
  "plan": {
    "protocol": "forkfs.revision-sync/1",
    "source_repository": "<32-hex-id>",
    "observed_sequence": "12345",
    "scope": "revision-closure",
    "history": "full",
    "selected_revision_ids": ["<sorted-distinct-64-hex-ids>"],
    "inventory_root": "<64-hex-inventory-hash>",
    "object_count": "83421",
    "payload_bytes": "190238172",
    "plan_hash": "<64-hex-plan-hash>"
  },
  "expires_at": "<UTC-time>"
}
```

同一次请求中的 selectors 在同一提交边界解析并固定；任一选择失败则整体失败。
根 ID 去重、按原始 32 字节升序排序。`observed_sequence` 是捕获时本地观察水位，
**不是** revision 创建凭证，也不是可直接 replay 的恢复点。源端继续写入不改变 plan。
名称视图的原始绑定可作为旁路说明保存，不参与恢复授权或改变 revision ID。

幂等范围为 `(principal, repository, consumer_id, Idempotency-Key)`；重试不创建第二份
pin。相同 key 的不同规范化申请字段返回 `IDEMPOTENCY_CONFLICT`；同字段重试直接返回
持久任务，不能重新解析名字覆盖旧任务。幂等记录及终态 tombstone 默认保存 30 天，期限在响应中明确给出。

### 3.3 租约和任务状态

```text
accepted -> planning -> ready -> acknowledged
                |        |
                +--------+-> failed / cancelled / expired
```

`accepted` 之前必须持久登记根 pin。`planning` 可续租；后台遍历和 pack 读取不持有
整个 Container writer lock。默认 lease 15 分钟，可续租，任务总期限默认 24 小时。
续租只接受新期限；不能修改 selectors、consumer、destination 或 plan。

源端控制 catalog 保存原始规范化请求、固定根、任务状态和计划节点。进程重启后
先恢复所有未到期 pin，再允许 GC；planning 可从持久进度或固定根重新构建。
已 ready 的计划字节与哈希必须不变。控制 catalog 损坏时停止导出及相关 GC，
不能把无法读取的 pin 当作不存在。不得依赖进程内 LevelDB snapshot/cursor 续传。

过期或取消后释放源端 pin，后续数据请求返回 `EXPORT_EXPIRED`/`EXPORT_CANCELLED`。
客户端此前已校验的对象仍可用于新任务。源端任务结束不删除目标端已发布备份。

## 4. 增量发现：Merkle inventory

源端提供覆盖**完整闭包**的内容寻址 inventory，而不是依赖“上次同步到 seq=N”。
backup server 同时维护对象 CAS 和已验证的 inventory 节点；比较哈希相同的子树，
只获取变化的 inventory 节点，然后精确查询本地缺失对象。

inventory 使用对象 ID 的 hex 前缀 radix tree，每层最多 16 个 child：

- 叶子至多 1,024 条 `(object_id, payload_length)`，按 ID 严格升序。
- 超过 1,024 条则按下一 hex nibble 分支；最大深度 64。
- 分区基于稳定 hash 前缀，新增对象不会像固定条数排序分页那样移动后面所有页。
- 空分支不编码；重复 ID、长度不一致、前缀不匹配、错误 count/hash 一律拒绝。
- inventory 只描述逻辑对象，不暴露本机对象位置。

### 4.1 Inventory 节点规范编码

所有整数 little-endian；节点 bytes 为：

```text
"FFSIN001"                         8 bytes
kind                                u8: 0 leaf, 1 branch
prefix_nibbles                      u8: 0..64
prefix                              prefix_nibbles bytes, ASCII lowercase hex
subtree_object_count                u64
if leaf:
    entry_count                     u32, equals subtree_object_count, <=1024
    entries[entry_count]             object_id[32] + payload_length[u32]
if branch:
    child_bitmap                    u16
    child_hashes[popcount(bitmap)]   32-byte hashes, ascending nibble order
```

leaf 的 ID 必须匹配 prefix；branch 必须 `count > 1024`、`prefix_nibbles < 64`，
child 的 prefix 必须为父 prefix 加对应 nibble，父 count 等于子 count 总和；对象长度总和必须等于 plan 的 payload_bytes。
所有加法/长度计算先检查溢出和资源上限。
只允许零前缀的空 root leaf；真实 revision 导出的闭包非空。必须精确消费整个编码。

```text
inventory_node_hash = SHA256(ASCII("forkfs.sync.inventory.v1") || NUL || node_bytes)
plan_hash = SHA256(ASCII("forkfs.sync.plan.v1") || NUL
                  || source_repository[16] || LE64(observed_sequence)
                  || LE32(selected_root_count) || sorted_revision_ids[32 each]
                  || inventory_root[32] || LE64(object_count) || LE64(payload_bytes)
                  || LE32(1))
```

最后的 flags 固定为 1，表示完整 revision 历史及完整树；其他值需新协商版本。
协议 ID、scope 和 history 在本版本是固定常量；未知字段值不能按兼容成功处理。
JSON 排版、对象键顺序、任务 ID、lease 时间和别名不参与 hash。

缓存 inventory 节点不等于它的对象仍存在。目标 GC/损坏可能让旧的“已拥有”集合失效：
只有被当前备份/恢复/staging pin 保护的有效闭包证书才能跳过相同子树；否则仍须检查
它列出的对象。Bloom filter 只能辅助探测，不能直接证明完整性或决定最终发布。

首版源端可流式遍历后外排构建 inventory，首次成本 O(完整闭包)，不宣称 O(changes)。
后续可按 `(role, object_id, codec_version)` 缓存已验证闭包，复用未变化子树；只有在
相关对象仍有可证明的保留关系时才复用。网络差量与源端构建成本分开计量。

## 5. 对象批量传输和断点续传

```http
POST R/exports/{export_id}/packs
Idempotency-Key: <pack-request-id>
Content-Type: application/json
```

```json
{"object_ids": ["<sorted-distinct-object-id>"]}
```

v1 限制每批最多 1,024 个 ID、canonical payload 总量不超过 16 MiB。
源端按 inventory 验证成员关系，不能把 endpoint 用作任意对象读取 oracle。
整批在发送响应头前完成范围与限制检查。返回 `application/vnd.forkfs.sync-pack`：

```text
"FFSPK001"                         8 bytes
plan_hash                           32 bytes
record_count                        u32
records[record_count]:
    object_id                       32 bytes, strictly ascending
    payload_length                  u32, matches inventory, <= negotiated limit
    canonical_payload               payload_length bytes
footer                              32 bytes
```

`footer = SHA256(ASCII("forkfs.sync.pack.v1") || NUL || all_preceding_pack_bytes)`。
接收端检查精确记录数、每个 object hash/长度、plan_hash、footer 和 EOF。
中途源端读到损坏必须中断流并使任务失败；不得跳过记录，也不能补发一个假的完整 footer。
只收到 HTTP 200/EOF、只校验 pack hash 或只收到 revision descriptor 都不构成备份成功。

断点单位为**已持久校验的对象**，不使用本机 segment offset 或在压缩流中任意 Range。
连接中断后，目标重新计算 missing set，发新 pack；完整通过对象 hash 的前缀可留在
staging，但不发布为完整备份。相同 pack key/相同 ID 集合重试返回相同 canonical bytes；
不同 ID 集合返回冲突。直到 lease 到期，inventory 与 plan 都保持可重读。

目标可在一批对象落盘后统一 fsync/提交索引，无需每对象一个 fsync/HTTP 请求。
掉电时允许重传尚未持久的批次，不能仅凭内存里的 received bit 跳过对象。
首版传完整逻辑对象；64 KiB chunk 缺块传输可作为后续 codec，不能直接导出 FFOBJ002
并假定另一端已具有这些 chunk。

## 6. 目标端验证、发布和 receipt

目标端状态：`staging -> verifying -> publishing -> committed`；失败/取消的 staging
不出现在可恢复目录中。所有对象的 hash 和完整闭包验证都由目标独立完成。

发布前必须验证：

1. inventory hash/count/length、plan_hash、每个 canonical object ID；对象存在不等于已验证。
2. 所选 revision 与全部祖先满足 FFREV001/002 格式和 DAG 规则；共享祖先允许，环拒绝。
3. 每个 revision 的 manifest、CoW 树/run、叶值和 namespace/inode/目录语义完整。
4. 实际可达对象集合与 inventory 一致；没有用未声明的缺失对象或以截断 DAG 假装完整。
5. 使用已发布闭包证书作优化时，证书依赖已被当前 job pin，且验证版本和授权范围匹配。

只有在目标对象、索引和 immutable backup manifest 均完成目标存储的持久化契约，
并且可恢复 catalog 经条件发布成功后，才能生成 receipt：

```json
{
  "receipt_id": "<destination-issued-immutable-id>",
  "consumer_id": "backup-main",
  "destination_id": "backup-cluster-a",
  "source_repository": "<32-hex-id>",
  "plan_hash": "<64-hex-id>",
  "inventory_root": "<64-hex-id>",
  "selected_revision_ids": ["<sorted-distinct-64-hex-ids>"],
  "backup_id": "<destination-backup-id>",
  "backup_manifest_hash": "<64-hex-id>",
  "catalog_generation": "19",
  "durability": "destination-committed",
  "retention_expires_at": "<UTC-time-or-null>"
}
```

`backup_manifest_hash` 是目标协议定义的不可变 manifest 摘要；目标 SDK/adapter 负责
解释实际位置与存储校验，源端不执行 receipt 中的任意 URL。`destination-committed`
表示已满足该 destination 配置的持久性合同；单盘落盘、对象存储确认和多副本确认
不是自动等价的，合同在配置中明示，不能由请求自报更高等级。

本地目标顺序：objects/pack fsync -> 索引持久化 -> manifest 原子发布及目录 sync ->
catalog CAS/持久化 -> receipt。对象存储目标先完成并验证所有依赖，再按该后端合同
条件发布 manifest/catalog；仅完成 multipart 上传或对象可列举不足以发 receipt。

源端 `POST .../acknowledgements` 必须核对 authenticated destination、固定 plan、
根集合和 inventory。receipt 及 acknowledged 状态一起持久化后才返回 200，再释放
源端 pin。receipt_id 同内容重试返回相同确认，异内容返回冲突。ACK 响应丢失可重试。

目标先发布成功、源端 ACK 丢失时，目标备份仍有效，源端显示“确认未完成”。lease
已经到期且之前未确认则返回 410，不能假装更新本地覆盖状态；保留的远端备份不受影响。
源端仅验证 receipt 来自被信任的目标并与任务匹配，不能通过网络声明独立证明对方磁盘
已落盘。mTLS/签名也不能把虚假持久化声明变成物理持久性。

并发任务完成顺序不一定等于创建顺序：目标以 catalog_generation CAS 发布，源端
以幂等 receipt 集合记录覆盖。不能用旧任务覆盖较新目录，也不能仅存一个“最后 seq”。catalog CAS 冲突后必须
重读并保留其他已发布备份，再重试条件更新，而不是用旧快照覆盖整个 catalog。

## 7. Frontier、保留与 GC

同步进度表示为有 provenance 的 **revision frontier 集合**。revision A 为 B 的祖先，
且 B 的完整闭包已保护时，可用 B 覆盖 A；互不为祖先的分支必须同时保留。
不能按 source_sequence、时间戳或 revision ID 大小选择“最新一个”代替所有分支。

coverage 响应包含目标、有效 receipts、frontier、保留期限和确认时间。
它表达源端最后收到的持久声明；目标失联、数据损坏或保留已过期时不能仍展示为实时
可恢复保证。只删除/重命名源端 revision 标签不会要求目标删除备份。

源端 GC 根必须包括活跃 export pins；目标 GC 根必须包括已发布 manifests、staging
任务、正在发布的任务和 restore leases。发布与 GC 需要共同的 fencing/epoch 或锁：
不能在检查对象存在后、catalog 发布前，让 GC 把对象删除。删除远端备份须独立授权并
通过目标 retention/catalog 流程完成，普通同步 ACK 不带删除权限。

lease 不得无限保护任意历史：并发任务、总 pinned bytes、任务时间和 staging 空间
都有配置限额。失败、中断和过期任务必须可枚举，清理须遵守读者 pin 与 grace period。
测试已观察到本地历史与元数据增长；同步协议不会自动为源 ObjectStore 提供 GC。

## 8. 还原和跨仓库导入

目标备份的最低交付为：plan、完整 inventory、所有对象、验证版本、receipt 及可下载
的不可变 backup manifest。还原工具必须能在源仓库彻底丢失后验证并取回完整闭包。

revision-only 还原建立**新的仓库身份与本地日志 lineage**，在私有 staging 中重建
对象存储、位置索引和所需逻辑根，验证后一次发布；不往已挂载 World 原地覆盖。
保留原始 revision bytes/IDs、父顺序及来源信息，不把历史 descriptor 的
source_sequence 当作新仓库本地事件，更不能填充虚构空事务以凑到这个序号。

当前 `revision_point()` 假定本地创建事件并检查 source_sequence，FFRPLY01 又限定
其可接受键和仓库恢复语义。因此需要显式的 revision import profile、来源绑定和
`creation_not_local`/不可用恢复凭证语义，另行版本化实现；不能把这个包直接冒充
FFRPLY01 base，或声称现在已有导入 CLI。若完整重建祖先命名视图，工具分配不冲突的
名字；原名称仅作为来源标签。所有恢复测试必须验证目录父关系、inode 分配状态和
后续可写 fork，而不只验证对象 hash。

需要原仓库精确事务恢复的 backup server，应另外实现 checkpoint/replay 传输通道。
它的 ACK 包含完整恢复点 `(repository, sequence, chain_hash)`，与本协议的
`revision-closure` receipt 分开管理。

## 9. 安全、错误与资源限制

生产网络使用 TLS、受信任身份到 repository/consumer/destination 的映射，以及独立
的只读导出权限。默认不开放 TCP；也可由本机受控 broker 代理到 daemon。远端上传、
源端文件写入、任意 source delete、World 操作不属于这份凭证。跨 tenant 不暴露对象
存在性；即使内部去重相同 hash，外部查询仍受导出 membership 和 tenant 授权约束。

| HTTP | code | 客户端处理 |
| --- | --- | --- |
| 400 | INVALID_ARGUMENT / UNSUPPORTED_VERSION | 修正请求，未知字段/模式不可默默忽略 |
| 401/403 | UNAUTHENTICATED / FORBIDDEN | 不启动传输，不泄露其他仓库对象存在性 |
| 404 | REVISION_NOT_FOUND / EXPORT_NOT_FOUND | 重新发现或人工处理，不能改成导出最新值 |
| 409 | REF_CHANGED / IDEMPOTENCY_CONFLICT / RECEIPT_MISMATCH | 不发布，重新选择或修正请求 |
| 410 | REF_VIEW_EXPIRED / EXPORT_EXPIRED / EXPORT_CANCELLED | 重新发现/建任务，复用目标已校验对象 |
| 413/422 | LIMIT_EXCEEDED / UNSUPPORTED_CODEC | 拆批/提高授权预算/升级，结果为 incomplete |
| 429 | RESOURCE_EXHAUSTED | 按 Retry-After 退避，保留已持久进度 |
| 500 | SOURCE_CORRUPTION | 导出失败，保留诊断，不能推进覆盖或静默回退 |
| 503 | TEMPORARILY_UNAVAILABLE | 幂等重试；失效 lease 必须重新建立 |

错误体固定为 `code, message, retryable, export_id?, details?`；message 不是机器匹配键。
限制初值：refs 每页 256（上限 1,024），selectors 数量 1–256，单批 1,024 对象/16 MiB，
单对象 <=256 KiB，DAG revision 节点 <=100,000，单仓库同时两个活动 export。
对象总数、pin 字节、staging 和校验内存均设管理员可调硬限额；当前遍历器的 DAG
预算不能冒充完整 namespace/closure 的内存上限。源/目标按请求与响应字节限流。

源端捕获只在短锁区解析根、登记 pin 并持久化意图；全闭包扫描、哈希、外排和传输
在受限后台 worker 中进行。当前 RPC dispatcher 为同步执行，因此不能把全量导出
直接放进它的 dispatch 调用。I/O、CPU、队列等待、foreground p95、pin 空间和缓存
miss 分别测量；不能用提高后台吞吐掩盖前台写入延迟恶化。

## 10. Backup server 参考流程

```text
negotiate capabilities and codecs
list/choose refs (optional expected IDs)
create export with persistent request ID; renew while planning/transferring
fetch plan and inventory root
walk changed inventory subtrees; pin any reused target closure certificates
for each bounded batch missing from verified durable CAS:
    fetch pack; verify; persist objects/index and staging progress
independently validate full requested closure and namespace semantics
publish immutable backup manifest and catalog under GC fencing
persist receipt, then POST source acknowledgement (retry on lost response)
release staging pins; expose committed backup through restore API
```

backup server 自己选择本地磁盘、S3 等 backend；协议不要求其复制源数据库格式。
最小接入 SDK 提供 object hash、inventory/pack codec、typed closure walker、DAG/
namespace validator 及恢复 reader adapter。SDK 的验证能从目标字节运行，不能仅把源端
的“verified”布尔值当作校验结果。

## 11. 实施与验收

1. 实现 typed ObjectWalker/目标 SDK、canonical 编码测试向量和独立模型测试。
2. 实现持久 SyncCatalog、PinRegistry、异步 export 与 inventory/pack reader。
   控制 catalog 不把 payload 放入 LevelDB；若增加持久键/格式，需明确版本及旧版拒绝，
   也可复用独立控制文件模式，但 pin 的崩溃恢复必须与 GC 启动共同协调。
3. 接入只读 HTTP/broker 与参考 backup server 的本地后端，完成发布/ACK 闭环。
4. 实现 revision import profile 和源端丢失后的真实还原，再接对象存储后端。
5. 另行扩展事务 checkpoint/replay 通道与 remote-fsync，保持 ACK 语义独立。

测试至少覆盖：

- 随机多父 DAG、共享祖先、相同 descriptor 多名、分支 frontier、旧格式混合。
- 导出中继续写/rename 标签/新建 revision，plan 不变；ref expected-ID 冲突。
- 漏对象、漏任一父树、错误 inventory count/prefix/hash、magic-like 文件内容、未知 codec。
- 内容 hash 正确但结构损坏、inode/目录关系错误、校验预算超限：均不发完整 receipt。
- Bloom 假阳性、陈旧 inventory/闭包证书、目标 GC 删除复用对象：必须发现缺失。
- 创建 pin、ready、续租、目标 fsync、catalog CAS、源 ACK 任意点崩溃、断网和响应丢失。
- 多客户端重复请求、不同参数同 key、并发乱序完成、已过期任务及 ACK 的幂等行为。
- 源/目标 GC 与发布竞态、lease 过期清理、暂停恢复者、磁盘满和配额耗尽。
- 1/10/100 个相近 revision 的全量及增量字节、inventory 变化量、前台 p95、后台内存、
  source pinned bytes、目标实际占用；源码/产物 churn 后不能无限保留失败 staging。
- 删除源仓库之后从备份恢复每个祖先树、创建可写 World，并验证 inode/数据一致性。

关联设计：[revision DAG](REVISION_DAG.md)、[本地对象存储](CONTENT_SEGMENTS.md)、
[事务备份与 replay](REVISION_BACKUP_DESIGN.md)、[研发 workload](DEVELOPER_WORKLOAD_TESTS.md)。
