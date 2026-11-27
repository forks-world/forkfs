# ForkFS 文件系统兼容性路线图

以 revision-backed `forkfsd` 和 macOS 26+ 的单用户私有 FSKit 挂载为第一阶段。验收对象是系统调用和应用可观察行为。Linux daemon、APFS 参考、FSKit 回调和真实挂载分别记录结果。历史挂载结果保留其原有系统版本和构建范围。

## 盘点基线

以 main `3e7ea34` 为盘点基线。`tests/fs_contract/coverage.json` 有 78 个测试族、37 条尚未覆盖的契约（POSIX 34 条、RPC 3 条，当前均为非强制项）、81 条外部测试引用（57 个不同脚本）。76 个用例按重叠 profile 统计为 daemon 71、APFS 参考 53、mounted 51。外部引用在共享 harness 中默认显示 `not_run`。这些数字衡量覆盖范围，已确认的实现缺陷和待验收项以各阶段的实际结果为准。

路线图有 33 个工作单元。1–22 面向私有 macOS 挂载基线，23–33 涉及可选能力、兼容策略或平台扩展。当前独立 PR #159 和 #160 处理符号链接及硬链接创建错误码，列为在途工作。原生 POSIX 没有 revision 快照接口，仓库快照继续由 RPC 契约验收。

## 私有 macOS 挂载基线

| 顺序 | 工作单元 | 验收条件 |
| --- | --- | --- |
| 1 | FSKit 无效 UTF-8 | 名称和 symlink 目标的回调返回 `EINVAL`。直接和 loopback 回归验证拒绝前后的 inode、目录项和提交序号。 |
| 2 | 当前候选的原生挂载 | 用同一提交构建、签名并挂载可丢弃的仓库。记录提交、扩展与 daemon 构建身份、OS/SDK 和 APFS/forkfs 成对报告。 |
| 3 | 打开标志与路径错误 | 逐项比对打开标志组合、链接循环和尾斜杠，以及创建、删除、重命名错误优先级。失败时检查命名空间、数据和句柄状态。 |
| 4 | 被删除或改名的目录 fd | 验证 fstat、xattr、`*at`、readdir、fchdir、路径重用及父目录 nlink。记录孤儿目录 nlink 的文件系统差异。 |
| 5 | 目录流与 cookie | 并发目录变更下验证分页、rewind、telldir/seekdir、游标淘汰和回收。 |
| 6 | 名称与路径边界 | 验证控制字符、换行、引号、UTF-8 字节上限、原样 Unicode 名称和 symlink 目标。记录 APFS 比较结果。 |
| 7 | 私有挂载权限与元数据 | 验证 mode、sticky、faccessat、目录搜索、目录尺寸拒绝、fd 元数据和私有挂载根权限。 |
| 8 | xattr 原生语义 | 验证存在性 errno、路径/fd/nofollow、孤儿描述符、只读拒绝、值大小和配额边界。 |
| 9 | 并发 append 与命名空间 | 多进程验证 append 记录原子性、共享描述符偏移、rename/unlink 与 I/O 竞争。覆盖 daemon RPC 对应行为。 |
| 10 | BSD 文件标志 | 验证 mixed flags、目录规则、旧 fd 和写回例外、只读与历史版本隔离。 |
| 11 | mmap 与缓存一致性 | 跨进程验证 shared/private 映射、页边界、同步、截断和 unlink 后的内容与生命周期。 |
| 12 | 内核文件锁 | 验证 flock 与 fcntl 的释放、阻塞、信号和死锁边界。OFD 结果按系统能力记录。 |
| 13 | 容量和配置接口 | 运行 statfs/statvfs/pathconf/st_blocks 的路径、fd 和 revision 对照。记录 `PC_PATH_MAX` 与宿主空间差异。 |
| 14 | 大文件、稀疏与规模 | 验证大偏移、短 I/O、稀疏零填充及 SEEK_DATA/SEEK_HOLE 的 26/27 行为。加入 GC 后全量校验和规模预算。 |
| 15 | ENOSPC、写回与 fsync 失败 | 通过可丢弃的有界镜像及故障注入检查拒绝、恢复、旧或新状态以及错误传播。 |
| 16 | 原生挂载生命周期 | 验证只读挂载、卸载和重挂载、daemon/lease/扩展丢失、缓存隔离与恢复边界。 |
| 17 | FIFO IPC | 实挂载验证创建、阻塞/非阻塞、EOF、ESPIPE 和 unlink/rename 后已开 fd。记录内核路由结果。 |
| 18 | Unix-domain socket | 实挂载验证 bind/listen/connect、冲突、unlink/rebind 和只读拒绝。确认活连接的生命周期由内核管理。 |
| 19 | 复制与传输 | 验证递归 copyfile、普通复制 fallback、flags 和稀疏行为，以及 sendfile 的区间、非阻塞、headers/trailers 和 EOF。 |
| 20 | 文件监控 | 验证 kqueue 和 FSEvents 的应用可见事件、递归、路径重用、revoke、卸载与事件丢失。 |
| 21 | 开发工具工作流 | 验证 Git worktree、构建、编辑器原子保存和 SQLite WAL 竞争。核对文件数据及 revision 回读。 |
| 22 | macOS 26/27 发布验收 | 对两代系统的精确候选运行所选 daemon、回调、APFS、挂载及外部套件，汇总可追溯证据与剩余边界。 |

## 独立扩展和平台边界

| 顺序 | 工作单元 | 验收条件 |
| --- | --- | --- |
| 23 | ResourceFork | 验证位置读取、范围、EOF 和 `cp -p`。先确认 FSKit 请求有足够信息传递 offset。 |
| 24 | 原生排他 rename 和交换 | 找到 OS 路由后验证 `RENAME_EXCL`、`RENAME_SWAP` 的原子性。平台拒绝时保留精确 errno 与状态证据。 |
| 25 | 强制 CoW clone | 验证 `clonefile`、forced clone、独立 inode 与写入隔离。普通复制继续验证 fallback。 |
| 26 | 原生 hole punch | 验证 syscall 路由、清零区间和 allocation 变化。物理 extent 另行评估。 |
| 27 | 原生预分配 | 验证容量预留、并发竞争和失败原子性。用可写容量证明预留。 |
| 28 | 持久文件 ID | 定义跨重新打开及 GC 的身份语义，验证碰撞、引用失效、只读历史和持久句柄。 |
| 29 | 所有权、ACL 与多用户 | 先定权限模型，再验证持久 uid/gid、set-ID、组权限、ACL、sticky 和 BSD 系统保护。 |
| 30 | 可选大小写折叠与 Unicode 等价 | 明确卷级策略、存量迁移、名称冲突和仅大小写 rename，验证精确名称模式兼容。 |
| 31 | `mknod` 与设备节点策略 | 确认产品和 OS 支持边界。为选定行为验证类型、授权或稳定拒绝。 |
| 32 | 硬件屏障与断电 | 定义文件数据和目录项写入顺序，在受控断电或设备故障下验证旧或新状态及错误传播。 |
| 33 | Linux 挂载前端 | 独立实现及验收 Linux mount、syscall、应用工作流和平台差异。 |

## 执行与证据

每个工作单元独立提 PR。Sol 定义行为与验收，再审核实际提交。Terra 实现并补回归。修复项优先在旧构建复现，使用相同用例验证新构建。验收项记录 OS、构建身份、执行层级、实际结果和缺口。新用例应复用现有 harness 或对应专用脚本。失败时保留 errno 与状态证据，平台入口无法路由时保留探针及依赖，供后续判断。

第 2 项为依赖真实挂载的单元提供当前候选证据。签名或 OS 路由阻塞时，继续推进可以通过 daemon、APFS 和直接回调可信验收的工作。每项完成后更新[兼容性矩阵](FILESYSTEM_API_COMPATIBILITY.md)及对应[契约清单](../tests/fs_contract/coverage.json)中的证据范围。

## 首次挂载基线

macOS 27.2.1 上，用 main `3e7ea34` 构建 daemon，并使用本机已注册的 2026-11-13 扩展运行可丢弃的仓库。已验证挂载类型为 forkfs。APFS 配对用例通过 51/51，forkfs 通过 48/51，三个用例因错误前的状态指纹遇到 `ESTALE` 而阻塞。

- `open_directory_rmdir_retains_identity`
- `directory_handle_relative_create_survives_rename_and_path_reuse`
- `rename_cross_parent_replaces_empty_directory`

独立探针在旧扩展上确认，`rmdir` 后原目录 fd 的 `fstat` 仍保留 inode，原路径查询返回 `ENOENT`，`flistxattr` 和 `mkdirat` 返回 `ESTALE`。新版扩展在 `codesign` 阶段返回 `errSecInternalComponent`，当时第 2 项仍待用同一提交的签名扩展和 daemon 验收。

本次配对报告的案例哈希为 `7cc8f670eb7fe89d7362eee0e6091a408756e4683db71a04363cbb7d6f93c370`，清单哈希为 `ab8d81682d8824f1aef2f214fcaf103740965bf99c290ce05d391b7f700ba7ba`。

## 同提交签名挂载验收

2026-11-26 在 macOS 27.2.1 上完成第 2 项。FSKit 扩展和 daemon 均从 main
`02e1a12` 构建。Apple Development 签名在本机图形登录会话中通过，实挂载时
核对了运行中扩展的可执行文件 inode。可写挂载和只读 revision 重挂载通过，
冒烟测试通过 13/13，CTest 通过 87/87。

两次契约对照均为 APFS 54/54、ForkFS 53/54。剩余差异属于第 4 项。
通过已删除的目录 fd 调用 `mkdirat` 时，APFS 返回 `ENOENT`，ForkFS 返回
`ESTALE`，失败前后的状态指纹一致。构建、报告哈希和失败步骤见
[签名部署记录](evidence/2026-11-26-signed-fskit-deployment.json)。
