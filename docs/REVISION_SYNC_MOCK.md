# Revision sync：mock server 可用性验证

状态：测试原型，验证 [Revision Sync API 设计](REVISION_SYNC_API.md) 的核心数据流。
不启用生产 listener，也未提供可直接部署的 backup server 或正式 SDK。

## 运行

仅需 Python 标准库及已经构建的 `forkfsd`：

```sh
python3 scripts/tests/test_revision_sync_mock.py build/metadata/daemon/forkfsd \
  --output build/sync-mock-results
ctest --test-dir build/metadata -L revision-sync --output-on-failure
```

测试创建私有临时仓库和随机 loopback 端口。源 HTTP adapter 与 backup server 是
**独立子进程**；目标只能通过 HTTP 获取源数据，使用自己的 CAS、inventory cache、
manifest 和持久 catalog。HTTP server 不做 DNS 查询。用例启动失败也会清理子进程。
全程不挂载、不格式化、不修改用户目录。

## 输入和独立校验

真实 daemon 创建 160 个源码文件、空文件、150,000 字节二进制文件、magic-like 文件
内容、hardlink、symlink 和不同权限，再生成分叉及多父 DAG：

```text
base -> left  --+-> merge -> next
     -> right -+
```

测试源 adapter 从现有便携备份中读取 canonical 对象，作为冻结的离线输入；不会让
外部 backup server 读取 LevelDB/segment。FFREV001 + FFREV002 混合及最大 u64
source_sequence 的额外历史由测试生成并保留原始对象 ID。

mock 的显式 revision ID 选择范围是当前授权 refs 及其保留祖先，包含没有直接名称的
祖先。仅存在于 fixture CAS、或仅被旧 export pin 保留，不自动授予新建 export 的权限。
已有 export 仍按固定 plan 提供其闭包；refs 变化不会改写它。过期后的新申请必须重新
满足当前授权历史范围，不能把任意可找到的对象当作可导出的 revision。

源端 encoder/closure walker 与目标 decoder/namespace validator 分开实现。目标
验证 hash、inventory radix/count、pack framing、精确闭包、DAG、CoW 树顺序/priority/
depth、inode 类型/大小/权限/link count、目录父关系等。文件内容以魔数开头也保持
叶对象语义，不按内容猜测引用。

目标还原为原生测试目录后，按独立的预期文件表逐字节比较每个祖先 revision，检查
hardlink inode、symlink 文本和 mode。此时源 HTTP 进程已退出、源 fixture 已删除、
真实源仓库也已移出可用位置。目标还原过程不访问源端。

原生还原器通过目录文件描述符逐组件操作，避免给合法长路径追加测试目录前缀后
超过宿主 `PATH_MAX`；硬链接按 revision 内的 inode ID 保留，包括 symlink 硬链接。
它仍受宿主文件系统的表示能力约束，例如原生 symlink target 长度可能小于 ForkFS
允许的 4096 字节。协议校验保持 ForkFS 的格式边界，不因宿主限制拒绝合法备份；
目标无法表示的内容需要真正的 ForkFS import/mount，不能把原生目录还原当作通用导入。

## 场景

| 类别 | 验证 |
| --- | --- |
| 全量/增量 | 全祖先闭包、重复请求零对象重传、后续 revision 仅发送缺失对象 |
| 大文件 | 700,000 字节文件、FFINOD05/FFFILE01/FFEXT001 typed 闭包与独立还原；native 适配器单文件物化上限 8 MiB |
| Inventory | 多层 radix、精确缺失集、节点缓存、hash/count 损坏、重新计算外层哈希的结构错误 |
| 数据传输 | 分批 pack、完整对象 prefix 持久化、中断重试、随机截断/位翻转、footer/EOF |
| 目标独立验证 | footer 正确但 payload 损坏、hash 全正确但 inode 非法、inventory 漏依赖均不发布/ACK |
| 缓存失效 | inventory 已缓存但 CAS 对象丢失，重新 pin 并只补拉该对象 |
| 固定根 | 源进程重启、名称变更后原 plan 不变、expected-ID 和 request-key 冲突 |
| Lease/访问 | 续租、模拟时钟过期、闭包外对象拒绝、取消幂等、ACK 后关闭数据访问 |
| 发布/确认 | 目标发布前退出后复用已落盘对象；发布后退出只保留一份 catalog；ACK 丢失重试、发布后 lease 过期仍可还原 |
| 多分支 | frontier 同时保留 left/right，完整同步 next 后才可覆盖它们 |
| 协商/格式 | 不兼容版本先拒绝、旧新 revision 混合、不把 source_sequence 当作本地排序 |
| Namespace 边界 | 重新计算 hash 的非法记录集合、名称/target、inode 时间戳及 xattr 长度均拒绝；合法边界仍接受 |
| 重试与 ACK | 非法 receipt 不污染重启后的状态、跨 export receipt ID 冲突、repository 幂等范围、已确认状态不倒退 |
| 规模与还原 | 千层目录、长 revision 历史和接近路径长度边界的原生还原 |

pack/结构测试包含固定 seed 的随机截断与位翻转，不只依赖几个固定坏包。

staging 传输会周期续租 export，并在恢复尚未提交的任务时先核对/续租。单次阻塞
操作或离线暂停若超过 lease，不能靠协作续租保证不失效：原 request 保留固定 plan
和已验证 CAS，返回 `EXPORT_EXPIRED`，不会重新解析其名称或复活原 export。调用者
应建立新 request，并显式选择原 plan 中仍有授权的 revision IDs，复用已验证对象。
已经 committed 的备份不依赖续租；已知 acknowledged 的重试不再联系源端。

mock 同时执行 100,000 个唯一 revision 和 64 MiB 的 DAG 遍历预算，按当前 64 位
canonical `RevisionDag` 的 frame/parent charge 计算。该预算不是完整 namespace、
对象缓存或进程的内存上限；超限是 incomplete，不能发布完整 receipt。


## 观测和产物

每个用例保留 source/backup 的状态、日志、对象和还原目录。结束后输出 `result.json`
和 `.tar.gz` 压缩包，压缩包便于 CI 上传大量小文件。常规 metadata CI 已登记此用例，
本地通过不等于 Linux/macOS 远端矩阵已通过。

目标统计 `received_objects`、`received_payload_bytes`、`inventory_downloads`；测试精确
断言增量对象数与字节等于新闭包减旧闭包，重复请求不改变这些计数。新随机 inode ID
会改变 CoW 路径，测试不对小样本武断要求固定复用比例。

这是功能/故障原型，不是性能基准。目标暂时每对象一个文件，便于故障定位；该布局
及测试逐对象 fsync 不作为生产小文件存储或传输吞吐方案。

## 尚未验证的生产边界

- 源 adapter 读取离线真实对象，尚未接入 owning daemon 的在线捕获、异步 planning、
  durable PinRegistry 和真实 GC。模拟 lease/restart 不证明线上 GC 安全。
- 使用仅 loopback 的 HTTP 和固定测试 bearer，不验证 TLS/mTLS、生产 ACL 或多 tenant。
- 目标以单进程锁和本地 fsync/原子文件更新串行发布 catalog；不模拟多个 backup server
  的 CAS 争用、S3 持久性或跨故障域复制，也不证明物理掉电行为。
- prototype decoder 仅支持列出的 FFTREE01/inode/revision profile，有显式遍历预算；
  不宣称覆盖全部旧 namespace codec、所有扩展属性或任意深度历史。
- 还原输出是原生测试目录。revision-only 的新仓库导入、可写 fork 和实际 mount 仍需
  按设计实现 import profile，不能把本次结果当作已有的生产恢复接口。

协议修正：ACK 持久确认后源端会释放 pin。已 acknowledged export 仅保留查询/同 ACK
重试能力，数据读取和续租返回 `EXPORT_CLOSED`。目标若需要补拉已丢失对象，必须创建
新 export 重新保护闭包，而不能使用已释放的租约。该约束已写回 API 设计并纳入回归。

本地实测与产物见 [验证记录](evidence/REVISION_SYNC_MOCK_20261018.md)。
