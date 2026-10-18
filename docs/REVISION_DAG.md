# Revision DAG：格式与验证

Revision 是不可变的、按内容哈希寻址的对象，指向一个 namespace manifest 和有序的父 revision 列表。manifest 描述文件系统状态；父边描述历史。二者是不同的图，记录多个父节点不执行文件内容合并。

## FFREV002

新 snapshot 写入以下二进制格式，所有整数均为 little-endian：

| 偏移 | 长度 | 内容 |
| --- | --- | --- |
| 0 | 8 | ASCII `FFREV002` |
| 8 | 32 | namespace manifest 对象 ID |
| 40 | 8 | `source_sequence`，创建前的本地日志水位 |
| 48 | 4 | 父节点数量 N，0–64 |
| 52 | 32 × N | 有序的父 revision 对象 ID |

总长度必须恰好为 `52 + 32 × N`。对象 ID 使用 `Journal::object_id` 对完整编码计算带类型/长度 framing 的 SHA-256
（不是裸 payload 的 SHA-256，见 [同步对象身份](REVISION_SYNC_API.md#2-身份对象和格式)）；父顺序参与哈希，父 0 为主线。根 revision 的父列表为空。禁止零 manifest、零父 ID、自引用和重复父 ID。

继续读取原始 80 字节 FFREV001：manifest 位于 8，单父位于 40，source_sequence 位于 72；零父表示根。不会重写旧对象或改变其 ID，新旧节点可出现在同一 DAG 中。旧二进制不能读取新格式，写入 FFREV002 后不能假定可降级。

## 创建

普通 `fs-snapshot REPO NAME [--world WORLD]` 自动以 World 当前 base revision 为单父；没有 base 时创建根。

显式父列表入口：

```
forkfsd fs-snapshot-parents REPO NAME PARENT[,PARENT...] [--world WORLD]
forkfsd fs-snapshot-parents REPO NAME - [--world WORLD]
```

父参数是当前仓库中的 revision 名称，按输入顺序解析；`-` 表示空列表。RPC 使用相同命令，省略 REPO。descriptor、名称绑定和 World base 更新在同一事务中提交。快照内容始终来自所选 World；调用者必须先完成所需的内容合并，再记录多父历史。

## 验证

1. 检查 descriptor 哈希与引用 ID 一致、magic、精确长度及父列表约束。
2. 检查所有可达父对象存在且可解码，并验证各节点引用的 namespace manifest。
3. 使用显式栈与三色标记遍历：遇到灰节点为环，黑节点为已完成的共享祖先。不能将重复访问共享祖先视为环。
4. 不以 `source_sequence` 大小判断祖先关系。它是仓库本地日志水位，跨来源历史不能比较。恢复点定位仍须找到实际创建事务，并校验它与该本地事务的关系；DAG 合法不代表某个日志恢复点存在。
5. 默认最多验证 100,000 个节点，遍历状态累计预算 64 MiB。超限返回 `revision DAG validation incomplete: resource limit`，不报告验证成功，也不声称数据损坏。预算约束 DAG 状态，不是整个 namespace 校验的内存上限。

验证器在一次验证上下文内复用已完成节点；失败时清空状态。snapshot 的验证上下文保存在打开的 Container 中，受现有互斥锁保护，避免连续 snapshot 重复扫描全部祖先。缓存只包含已验证的不可变对象，重开仓库会清空；预算也包含缓存。备份、replay 和恢复校验使用各自的上下文，独立检查输入。

## 测试与范围

回归覆盖 codec 边界、新旧格式混合、菱形共享祖先、多父快照与恢复、缺失父对象、重复父、深链、资源限额及 RPC。随机生成 DAG 验证共享祖先与循环检测；循环注入使用合成 ID，避免把构造 SHA-256 固定点误当成可行的损坏测试手段。

此实现提供历史 DAG、存储兼容和校验，不包含文件内容自动 merge、远端同步协议或来源仓库身份字段。元数据 CoW 图和 revision DAG 分别验证。

外部 backup server 的目标同步协议见 [Revision Sync API v1](REVISION_SYNC_API.md)，
目前仍是设计提案，不改变现有 DAG 格式。
