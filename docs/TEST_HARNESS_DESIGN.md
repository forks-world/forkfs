# forkfs 测试 harness 设计

状态：设计；统一 runner、参考模型和平台适配器尚未实现。
适用基线：已合并的元数据、Namespace、CLI/RPC 和实验性只读 FSKit 实现。
当前 CTest、模型测试和进程崩溃测试继续保留，不能将设计中的能力计为已完成。

## 1. 目标与验收边界

用统一、可重放的场景检验元数据管理、常驻服务和真实文件系统。
一次失败必须能够回答：哪种执行层、哪条操作、期望什么、观察到什么、如何重现。
测试控制逻辑使用 Python 标准库；外部命令以 subprocess 参数列表执行，禁止 shell=True，
不依赖 Bash。引擎内部并发和故障注入仍使用 C++ 测试目标。

必须分别报告三个层级：

| 执行层 | 路径 | 能证明的性质 |
| --- | --- | --- |
| metadata | C++ → Container/Namespace/Journal | 事务、逻辑 inode、CoW、版本引用、恢复 |
| service | Python → 受限 RPC → forkfsd | 会话、租约、句柄、协议、进程生命周期 |
| mounted | Python/真实工具 → syscall → FSKit/FUSE | 实际挂载、内核缓存、POSIX、隔离和工作负载 |

metadata 或 service 通过不能替代 mounted 验收。只读 FSKit 回调单测不能算挂载通过。
本阶段先打通 metadata/service；实际挂载缺失时标记 unavailable，发布门槛必须失败。

## 2. 架构

```text
CI / 本地命令
    ↓
runner：配置、配额、能力检查、工件、生命周期
    ↓
scenario：操作轨迹 + 检查点 + 故障/并发调度
    ├── oracle：独立状态模型、允许的恢复结果
    └── adapter
          ├── C++ metadata driver
          ├── RPC service driver
          └── FSKit / FUSE mounted driver
```

平台管理与数据操作分开：Controller 创建 repository、服务、挂载和受限客户端；
Adapter 只执行测试数据操作。agent 隔离场景中的客户端不得持有 Controller 控制能力。

建议目录：

```text
tests/harness/
  runner.py                 # 参数、选择、超时、结果
  trace.py                  # 版本化操作轨迹
  model.py                  # 独立 namespace/handle/version 模型
  resources.py              # 独占 scratch、进程、服务生命周期
  adapters/                 # metadata、RPC、mounted
  scenarios/                # correctness、recovery、concurrency、isolation、workloads
  fixtures/                 # 小型固定历史/损坏格式 fixtures
  tests/                    # runner 与清理自身的单测
```

先复用现有 CTest 作为执行入口，不要求一轮迁移全部脚本。

## 3. 操作契约和能力

统一操作包括 lookup/stat/list、create/mkdir、read/write_at/append/truncate、
link/symlink/rename/unlink、open/dup/close、fsync、snapshot/fork、restart。
restore、revision 删除、GC、remote publish 和 mount revoke 是后续能力。

操作输入显式携带 World/revision、逻辑路径或 inode、handle、offset、payload、
预期错误类别和数据检查点。逻辑 handle 名映射到 adapter 的真实 token/fd；
trace 不记录随机服务 token、机器绝对路径或凭证。

各 adapter 声明能力：读写/只读、最大文件大小、文件名范围、hardlink、symlink、
xattr、mmap、句柄、durability、fault hooks、mount、agent isolation。
能力来自实现配置和预检，不能因测试失败而动态撤掉。

场景声明 required capabilities：

- 开发 smoke 可以显式跳过不支持的场景，并记录原因。
- 验收 profile 的必需能力缺失是失败，不能变成绿色 skip。
- adapter 的意外 unavailable、超时、崩溃或格式解析错误均不能被当成预期 errno。

当前内部错误多为 runtime_error，缺乏完整 errno。内部场景可要求精确的既有拒绝结果；
POSIX profile 必须得到真实规范 errno。不能用宽泛“抛了异常就通过”掩盖损坏或缺失功能。

## 4. 独立参考模型

模型只维护测试语义，不复用生产编码、树遍历或 path resolver。

- inode 表：测试逻辑 ID、类型、mode、link count、内容、必要时间戳。
- 目录项：(parent inode, name) → inode；hardlink 共用 inode。
- open description：inode 引用、访问模式、共享 offset、append、锁状态；dup 共用 description。
- descriptor：独立关闭状态；最终 unlink 后 open description 仍保持文件。
- World：当前模型根；revision：不可变模型快照；fork 从该快照生成独立可写状态。

模型不预测真实 UUID、纳秒时间、物理文件大小或 SHA 值。比较身份等价关系，
时间戳只验证契约要求的变化/不变性，不能把不确定字段简单全部删掉。

stat+read、目录项+inode、head+sequence 等多字段检查必须来自一次一致视图，
或在写入者停止后的检查点执行，避免将跨提交观察误判为原子性问题。

模型检验之外保留原始格式损坏测试、hash 验证、底层计数核对。
模型和引擎可能同时漏掉规则，需用固定规范用例与 native POSIX 对照交叉验证。

## 5. 可重放轨迹与失败缩减

随机 generator 使用固定版本、种子、明确的最大操作数和数据预算。
同时保存生成的完整操作轨迹：仅 seed 不能抵御 generator 版本变化。
轨迹是 JSONL，首行记录 schema、generator、seed、profile 和能力要求。
操作中的二进制数据存入独占工件目录，以长度和摘要引用；小数据可用 base64。

```json
{"schema":1,"generator":"namespace-v1","seed":61772,"profile":"metadata"}
{"step":1,"op":"create","world":"main","path":"/a","data":"AQI="}
{"step":2,"op":"snapshot","world":"main","revision":"r1"}
{"step":3,"op":"fork","revision":"r1","world":"work"}
```

支持 replay 指定轨迹，失败报告给出精确 replay 命令。轨迹损坏/版本不支持必须拒绝。
缩减器从最短失败前缀开始，再删除操作组和缩小 payload；每次候选使用新 repository。
必须保留创建/引用依赖、原错误指纹和故障调度。无法缩减就保留原轨迹，不能改弱判据。

## 6. 场景矩阵

| 类别 | 必测案例 | 主要判据 |
| --- | --- | --- |
| 事务 | mixed invalid batch、stale CAS、重复请求 | 无部分发布；计数/根/序号正确 |
| 版本 | snapshot、fork、分支分歧、compaction/reopen | 旧 revision 不变；World 独立 |
| 身份 | hardlink、rename、unlink、dup、最终 close | inode/description 身份和 orphan 生命周期 |
| I/O | 覆盖、补零、EOF、append、truncate | 内容与 inode size 同事务；边界不溢出 |
| 目录 | 分页、rename、空/非空删除、循环拒绝 | 无遗漏/重复；父子引用和 link count 正确 |
| 会话 | ownership、续租、过期、丢回复、配额 | 无跨 session 访问；释放保留引用 |
| 格式 | 截断、重复引用、错误 hash、非法树 | 明确拒绝；有效 head 不被修改 |
| 恢复 | 提交前/后退出、restart、ENOSPC | 符合确认边界；可重试；无混合状态 |
| 隔离 | backing、控制 socket、旧 fd、卸载 | 拒绝旁路；撤销后不能继续访问 |
| 工作负载 | editor save、git status/build/install | 正确输出；记录真实挂载延迟和资源 |

单文件 256 KiB、ASCII 名称等原型限制必须显式进入能力表。
大文件、任意字节文件名、xattr、mmap 尚未通过，不能通过缩小工作负载隐藏这些限制。

## 7. 崩溃与确认语义

故障只注入 harness 创建的测试进程。生产二进制不得包含 fault hooks。
引擎 hook ID 固定并版本化，例如 before_write / after_write；验证 fault executable
确实在指定 hook 退出，而不是把任意非零退出当成有效崩溃。

| 故障位置 | 恢复允许结果 |
| --- | --- |
| 已知提交前 hook | 完整旧状态 |
| 已知同步提交后 hook、响应前 | 完整新状态 |
| 无已知 hook 的执行中 kill | 完整旧或新状态，禁止部分状态 |
| 客户端已收到持久确认后 kill | 完整新状态 |

记录请求 ID、调用开始/响应、确认等级和最后成功检查点。响应丢失是 unknown outcome，
不能一律算失败或允许重复 append；有去重协议才检查重试幂等。
当前确认基于本地 sync，cloud ack 不进入本地 harness 的承诺。

每次恢复使用新进程；复制 LevelDB baseline 前必须关闭其唯一 owner。
保留 revision、head、inode/content、orphan 和对象计数检查，不能仅检查一个文件。
进程崩溃不等于断电；真实掉电验收另设明确环境和恢复预期。

## 8. 并发与调度

引擎层采用现有仅测试构建的 barrier hooks；不用 sleep 决定竞争胜者。
RPC/挂载层使用开始屏障与受控客户端，记录 invocation/completion 区间及返回值。

小型历史采用有界线性化检查：枚举满足实时先后约束的顺序，并与独立模型比较。
历史规模/搜索预算固定；预算耗尽报告 inconclusive，在必需验收中不能算通过。
CAS 竞争、append、不相交 World 写入、verify/compaction 期间发布分别检查。

目前 RPC 串行调度。并发客户端通过仅证明协议排队与一致性，不能证明引擎并行。
锁进展测试在引擎层验证，不用 RPC 吞吐数字替代锁粒度判据。

## 9. 生命周期与清理

创建带随机 ownership token 的独占 run 目录，只清理本轮创建的资源。
路径参数、marker、symlink 和包含关系在破坏性清理前核对；不删除用户挂载点或仓库。

每个阶段设置总期限及单操作超时。启动成功必须同时满足进程存活和真实健康请求；
socket 文件存在不代表服务可用。持续读取或将 stdout/stderr 导入文件，防止管道堵塞。
只终止自己启动且仍持有的进程/进程组；不得按进程名称或未知 PID 批量 kill。
先 TERM、限时等候，再 KILL；等待退出后才能移除自己创建的 socket。

mounted controller 记录实际设备/挂载身份，清理先正常卸载并确认移除。
设备忙/无法确认清理时保留映像和证据，返回失败；不强制卸载未知设备，不假装清理成功。
清理失败作为独立结果记录，保留最初测试失败；成功测试不能掩盖 cleanup failure。
runner 自身需测试启动失败、超时、丢响应、子进程残留、卸载失败和错误的 ownership marker。

## 10. 工件与报告

每轮独占输出目录，至少保存：

- manifest.json：schema、源码 SHA、base/head、实际 tested tree、构建选项、二进制摘要、
  平台/内核/文件系统、能力、profile、seed、generator 版本。
- trace.jsonl、events.jsonl：请求/返回、时间区间、fault/barrier、检查点、清理事件。
- result.json / JUnit：pass/fail/skipped/unavailable/inconclusive，区分语义、协议、环境、清理失败。
- 日志、最小重放轨迹、期望/实测模型差异和选定 workload 输出。

错误报告不能只有 assert 行号。记录相关 World、逻辑 inode/handle、操作序号和差异。
工件不得包含身份凭证、环境全文或用户文件；限量保存测试生成的数据和白名单环境信息。
性能记录保留原始样本、缓存/同步策略、并发度和数据规模；缺少样本时不报告可比较的 p95。
复用 mounted_fs.py 的原始输出，不把内部 RPC 延迟冒充 syscall/mounted 延迟。

## 11. CI 分层

| Profile | 执行 | 范围 |
| --- | --- | --- |
| PR smoke | 每次 push/PR，macOS/Linux | 固定回归、少量确定种子、协议、有限 crash hooks |
| filesystem matrix | 既有 XFS/Btrfs/ext4/macOS | 真正位于目标卷上的 fixture 和恢复测试 |
| extended | 显式/nightly，后续配置 | 更多种子、长并发历史、ENOSPC/大目录/版本保留 |
| mounted acceptance | 配置好前端的指定机器 | 实际挂载、POSIX、缓存、撤销和 agent 隔离 |
| performance | 指定稳定机器/显式触发 | native 对照、Git/build、冷热缓存与资源成本 |

当前只有既有 Release 矩阵和 metadata-regression 专用 job，后面三个 profile 尚未配置。
标签选择必须 --no-tests=error，额外校验发现的 suite IDs 符合 profile 清单，避免空标签或
误删测试变绿。发布检查保存 tested tree；base/head 改变后旧证据不自动继承。
mounted 能力缺失时 PR 核心测试可以继续，但发布验收不得通过。

CI 中 PR 代码在无发布凭据、无外部网络且无法写发布 checkout 的环境执行。
harness 的资源隔离不替代 PR 执行隔离；平台挂载所需权限必须单独声明。

## 12. 实施顺序

1. **统一 runner 和资源管理**：封装已有 CTest/Python 场景，生成结果/工件，
   提供清理与超时自测。首个版本不改存储格式，也不替换已有有效断言。
2. **共享 RPC adapter 与 trace/model**：迁移文件、revision、session 场景，
   增加 replay 和最短失败前缀。验证同轨迹在 metadata/RPC 下给出一致语义。
3. **故障与并发控制**：结构化 hooks/确认边界、独立恢复实例、小型历史检查和缩减。
4. **真实挂载 adapter**：完成安装/授权/身份预检后接 FSKit/FUSE，逐项打开
   POSIX、缓存、撤销、Git/build 和隔离门槛；无前端时明确 unavailable。

首个交付门槛：同一确定性场景可在现有 metadata 与 RPC 层运行；失败有可重放轨迹；
超时不会遗留子进程；损坏/错误配置不能变成 skip；现有 11 项 metadata-regression
测试继续通过。真实挂载和性能验收不属于首个交付的成功声明。
