# forkfs 确定性存储故障注入

状态：专用故障二进制、可选命中次数、crash/EIO/ENOSPC 注入及自动恢复矩阵。
正常 forkfsd 不解析这些变量，没有故障配置 RPC，也不暴露测试专用命令。
已有 `FORKFS_TEST_CRASH` 提交/compaction 崩溃入口保持兼容。

## 运行

使用标准 Python runner，不依赖 Bash 或 world 项目：

```sh
python3 tests/faults/storage.py --binary build/forkfsd/daemon/forkfsd --fault-binary build/forkfsd/daemon/forkfsd_leveldb_fault_test
python3 tests/faults/storage.py --binary build/forkfsd/daemon/forkfsd --fault-binary build/forkfsd/daemon/forkfsd_leveldb_fault_test --case restore-wal_after_sync-eio
```

runner 自己创建临时仓库与独立副本，不接受用户仓库。`--output` 指定不存在的
artifact 目录；已存在目录拒绝覆盖。保留 manifest（两个二进制 SHA256）、
JSONL 命令/返回码/输出证据、result.json、失败 traceback 和仓库副本。
每次运行应确认所有 case 通过；不能把故障命令返回非零本身当作通过。
所有 subprocess 有超时；resident 服务在 finally 中终止并 wait。
现有 CI 通过 metadata-regression 标签运行此矩阵。

## 注入接口

专用二进制使用以下环境变量；可用 Python subprocess 的 env 参数设置：

| 变量 | 值 |
| --- | --- |
| FORKFS_TEST_FAULT | 下表中的一个精确 point |
| FORKFS_TEST_FAULT_HIT | 从 1 开始的十进制命中次数；默认 1 |
| FORKFS_TEST_FAULT_ACTION | crash、eio、enospc；默认 crash |

未知 point、无效 action、零/溢出/格式错误次数，在 main 打开仓库前拒绝。
每个进程只选择一个 point，第 N 次命中触发一次；次数为进程级原子计数。
触发时 stderr 写一条 JSON 事件，包含 point、hit 和 action。crash 使用
`_exit(86)`，不执行析构或正常关闭。没有实际触发事件的 case 必须失败。

| Point | 边界及注入方式 |
| --- | --- |
| leveldb_before_write | batch 已准备、DB::Write 尚未调用；throw EIO/ENOSPC 或退出 |
| wal_append | 事务线程向 WAL WritableFile::Append 写入前；返回合成 LevelDB IOError 或退出 |
| wal_sync | WAL 实际 Sync 前；返回合成 IOError 或退出 |
| wal_after_sync | WAL Sync 成功后；模拟已持久化但同步结果报错/进程退出 |
| leveldb_after_write | sync WriteBatch 成功、计数更新后；throw 错误或退出 |
| compaction_stage_committed | staging batch 持久化后、World head 未切换 |
| compaction_before_publish | head 发布事务之前 |
| compaction_published | head 发布事务成功之后 |

WAL point 仅在事务 DB::Write 的线程局部作用域启用，打开数据库/recovery
不会消耗次数。wal_append 计数是物理 Append 调用数，不能解释成事务数。
WAL 返回的是带 EIO/ENOSPC 标签的合成 LevelDB IOError，不是实际填满磁盘。
compaction 三个边界由故障二进制专用 `test-compact-staged` 驱动真实
Compaction::prepare/build/stage/install；该命令不编入生产。当前 LevelDB
`fs-compact` 主要调用原生 compact，不能把未命中的注入当作发布边界覆盖。

## 恢复判据

write、rename、fork、restore 在五个存储点分别执行三种 action，共 60 个
故障场景。重启后比较完整文件/目录/硬链接身份、内容、分支/历史以及事务序号，
并执行存储/namespace verify。跨独立执行的时间戳不直接比较。

- 提交前错误：完整旧状态；已有成功事务不受影响。
- WAL append/sync 未确认：允许完整旧/新状态，不允许部分发布或引用损坏。
- 已成功 sync 后错误：重启必须得到完整新状态，客户端不能因错误推断未提交。
- 存储写入失败或提交后注入错误：当前 LevelStore poisoned，后续读写要求 reopen。
- 安全的提交前拒绝：未污染 LevelDB，第三次操作仍可成功。

另外 9 个 compaction 边界场景确认 staged objects 不提前改变 head、发布
后完整状态保持；5 个 resident 场景验证第二次命中、进程存活、拒绝受污染
存储的后续访问及重启；4 个场景验证生产忽略配置、测试入口严格校验。
总共 78 个确定性场景。已有 crash、句柄和 restore 回归继续保留。

## 后续发布门槛

这批完成 WAL 与逻辑发布故障验证，尚不覆盖真实 ENOSPC、随机读/SST/manifest
损坏、原生 LevelDB compaction 文件创建/rename/sync 失败、设备断电或 live
FSKit 内核缓存/写回中断。后续应逐项增加受控存储 Env 故障及真实挂载测试，
分别报告覆盖，不能因本矩阵通过宣称已完成全部用户交付验收。
