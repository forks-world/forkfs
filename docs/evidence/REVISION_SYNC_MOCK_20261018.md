# Revision sync mock：本地验证记录（2026-10-18）

环境：Apple M1、macOS 27.0.1、本地 APFS。真实 Release forkfsd 生成输入，source adapter 与 backup server 在独立进程中通过 loopback HTTP 通信。此记录是协议原型验证，不是生产部署验收。

## 结果

- 20/20 场景通过，0 failures、0 errors。CTest `forkfsd_revision_sync_mock` 通过，用时 25.82 秒。
- pack codec 另外执行固定 seed 的 48 个随机位翻转、48 个随机截断及 3 个固定坏包，并检查重算外层 hash 的 inventory 结构错误。
- 源进程退出、源 fixture 删除、真实仓库移出可用位置后，从目标独立还原并逐字节检查四个祖先/分支树、权限、hardlink 和 symlink。
- 缺依赖、错误对象 hash、错误 inventory、重新哈希的非法 inode 都没有产生完整 catalog/ACK。
- 发布前退出后只补剩余对象；发布后退出及 ACK 响应丢失没有产生重复备份。目标提交后源 lease 过期的备份仍能独立还原。

## 一次增量观测

| 项目 | 本次实测 |
| --- | ---: |
| 首次完整闭包 | 1,217 个对象 / 319,494 payload bytes |
| 初次 inventory | 17 个节点 |
| 一个文件变化及新 revision 的增量 | 29 个对象 / 4,075 payload bytes |
| 新下载 inventory 节点 | 13 |
| 两轮目标总接收 | 1,246 个对象 / 323,569 payload bytes |
| 相同请求重复同步 | 0 新对象、0 新 payload、0 新 inventory 节点 |

对象数/路径会随随机 inode ID 改变，测试断言精确集合差，而不固定上述数值或小样本复用比例。数据仅说明内容去重生效，不是吞吐或空间效率 benchmark。目标原型每对象一个文件并逐对象 fsync，不应据此选择生产存储布局。

## 从 mock 反馈到设计的约束

1. ACK 后释放 pin：原任务仍可查询/重试 ACK，但不能继续读取对象；补拉须新建 export。
2. 目标 committed 与 source acknowledgement 分离：lease 过期或 ACK 丢失不使已发布备份失效。
3. 过期/取消后的任务元数据和幂等申请重试仍返回终态，数据请求/续租则返回明确失效错误；重试不复活任务。
4. 同步进度是 frontier 集合：left/right 分支并存，完整 next 闭包才能覆盖二者。

## 产物与范围

- 原始结果和每用例数据：`/private/tmp/fw49-83uw91l6/build/revision-sync-mock-artifacts/sync-mock-077mar5g`。
- 压缩取证包：`/private/tmp/fw49-83uw91l6/build/revision-sync-mock-artifacts/sync-mock-077mar5g.tar.gz`。
- CTest 日志：`/private/tmp/fw49-83uw91l6/sync-mock-ctest-final.log`。
- Python byte compilation、CI workflow YAML 解析及 whitespace 检查通过。

尚未证明：线上 daemon 的冻结/pin/GC 集成、TLS/生产鉴权、并发多 server 的 catalog CAS、远端介质持久性、revision-only 导入后的可写 fork/mount，以及物理掉电。mock 读取离线真实对象；还原输出是原生测试目录。Linux/macOS 远端 CI 尚待运行。

运行与覆盖说明见 [REVISION_SYNC_MOCK.md](../REVISION_SYNC_MOCK.md)。
