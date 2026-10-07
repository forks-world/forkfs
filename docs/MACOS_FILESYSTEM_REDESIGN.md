# macOS 主线重设计：挂载式 World 与不可绕过的访问边界

> 2026-10-08 整体方案见 [用户态版本化文件系统设计](USERSPACE_FILESYSTEM_DESIGN.md)，重点为
> 元数据服务、CoW revision/fork、本地事务及 S3 发布。本文保留平台选型过程和实测证据；
> APFS 映像优先等阶段性结论不代表统一用户态核心已定型或已通过性能门槛。

状态：2026-10-07 的新设计方向，尚未实现。用户要求 macOS 不能继续提供普通文件夹式
World，必须作为真正的文件系统工作，并防止 agent 绕过。本文取代 M1 中“native-root
为主线、FSKit 冻结为备选”的后续架构决策；历史实现、测量与兼容性记录仍然有效。

## 1. 产品契约

World 是具有稳定身份、隔离命名空间和明确挂载生命周期的受控文件系统视图，**不要求
每个 World 是独立块卷，也不要求用户分区、格式化或配置存储池**。用户可以用
Finder、终端、编辑器和普通 POSIX 程序访问挂载点；`fork --to <path>` 中的 path 在新模式下
是挂载位置，不再是可直接写入的 backing tree。卸载后该路径不提供工作区内容，也不允许
agent 把写入静默落到挂载点下面的普通目录。`inspect/status` 区分 World 身份、挂载状态、
访问策略和后端健康状态；路径不是权限凭据。

核心约束：

- Linux/XFS 同样要求真正的文件系统挂载；普通目录及仅对目录做 bind mount 不作为
  新模式的 World 后端。OverlayFS 的私有 lower/upper 可以是目录，用户与 agent 只访问合并挂载。
- 工作区 I/O 必须经过受控的 VFS 挂载入口，包括任意二进制和 agent 的全部后代进程。
- backing、snapshot、历史对象、数据库、锁和管理接口不向 agent 提供直接访问权。
- agent 只能写获授的 World 和会话私有临时空间；主机源项目、其他 World 和宿主配置
  默认不可写。额外读取、网络和凭据访问由独立策略授权。
- 挂载、隔离、身份校验或授权失败时，拒绝启动；运行中失效则拒绝后续访问并终止会话。
  不自动回退到目录或无沙箱模式。
- 性能在这些约束内优化，并作为发布硬门槛；性能不达标时换实现，不放松安全边界。
- 易用性约束：用户已明确放弃 macFUSE，macOS 不再将其作为产品候选或部署依赖。
  不要求用户进入恢复模式、降低启动安全策略或启用第三方内核扩展。

“真正的文件系统”与“受限制的执行环境”都必须成立。挂载本身不会阻止程序打开主机上的
另一个绝对路径；仅限制 `cwd`、提示词或工具调用也不会阻止任意程序执行系统调用。

## 2. 当前实现为什么不满足

| 现有路径 | 问题 | 新设计要求 |
| --- | --- | --- |
| M1 clonefile World 在用户指定目录直接可写 | 数据访问完全不经过 forkfs 前端 | clonefile 只作为内部存储机制 |
| 可选 FSKit 透传，同一 backing 仍以目录存在 | 从挂载点改走 backing 即可绕过前端 | backing 对 agent 的身份不可访问 |
| `cli/main.cpp` 的 macOS profile 使用 `(allow default)` | 其他主机路径和用户配置仍可能被修改 | 会话采用默认拒绝、显式授权 |
| sandbox 探测失败默认继续执行 | 隔离失效后依然启动 agent | 新模式无降级出口 |
| `.world` 标记携带 store 路径，CLI 直接打开 store | 可发现路径与管理能力耦合；不能用标记作授权 | broker 管理身份和能力，CLI 不直接拥有私有 store |
| snapshot 根目录 `0000`、可选用户 immutable 标志 | 防误操作不能代替独立权限边界 | agent 无权修改保护或借用管理者能力 |
| FSEvents 与事后 Git 检查 | 观察或报告发生在修改后 | 用强制访问控制阻止越界；观察只作补充 |

以上基于 `core/src/view.cpp`、`macos/fskit/WorldFileSystem.mm`、`cli/main.cpp` 和现有
README。现有 safety 测试仍有价值，但不证明新契约已经满足。

## 3. 威胁模型

把 agent、它运行的 shell、编译器、测试、依赖安装脚本和项目提供的配置视作不可信代码。
假设它知道所有磁盘路径、会自行调用系统 API、构造符号链接、传递文件描述符、启动后台
进程，并尝试调用 `world` 或本机其他服务。不能依赖它遵守启动提示。

承诺范围是通过 forkfs 受控启动器启动的会话及其后代。一个已经拥有宿主用户完整权限的
外部 agent，可以直接修改源项目；单独安装文件系统无法把它追溯地关进隔离环境。
因此受保护模式必须控制启动链；无法接入的 agent 不得显示为已隔离。

不承诺抵御内核漏洞、管理员主动解除保护或已被攻陷的可信管理进程。宿主用户可以管理
World，但这些权限不能因为 agent 与用户共享登录会话就自动传给 agent。

## 4. 目标架构与技术选择

```text
可信用户 / 管理 CLI ── 经认证的控制请求 ── World broker
                                              │
受限 agent → macOS VFS → forkfs volume → 私有存储服务
                                              │
                                     APFS clone / snapshot / history
```

控制面负责创建、fork、挂载、checkpoint、导出、discard 和恢复。数据面只服务挂载卷的
文件操作。两者的协议、权限和授权范围分开；拿到 World 写权限不等于可以创建新挂载、
改策略、删除历史或发布到源项目。agent 如需请求 checkpoint，授予该 World 的单独能力。

**同时验证原生 APFS 映像和 FSKit 前端；不预先锁定发布前端。** FSKit 提供用户态文件系统扩展，
`FSUnaryFileSystem` 可以把一个资源呈现为一个卷；这与现有代码的基础吻合，但不说明
当前透传实现已具备隔离能力。见 Apple 的 [FSKit 文档](https://developer.apple.com/documentation/fskit)
和 [FSUnaryFileSystem 文档](https://developer.apple.com/documentation/fskit/fsunaryfilesystem)。

先验证每个 World 的受控挂载视图与会话授权，不要求独立块设备或独立文件系统实例。
底层可以是私有目录、共享文件系统中的子卷或自动管理的映像；用户只提供源路径和目标
路径。agent 对其他 World 仍须被执行策略拒绝；卷 UUID 和路径本身都不是授权凭据。

存储服务应使用 agent 无权控制的身份或经过验证的系统强制隔离边界。独立服务身份是
优先验证方案；FSKit 扩展如何通过受限资源或 IPC 连接它、签名/安装要求、是否需要
特权 helper，都必须先做可运行探针。不能假设 FSKit 扩展能直接访问任意服务目录，
也不能把 App Sandbox 容器路径当成对所有同用户进程的保密机制。
Apple 的 [App Sandbox 说明](https://developer.apple.com/documentation/security/protecting-user-data-with-app-sandbox)
描述的是应用访问限制，不能直接推导出我们的完整 agent 隔离契约。

FSKit 不满足安全性、必要的 POSIX 语义或 §6 的性能门槛时，优先评估自动管理的原生 APFS
映像；macFUSE 已因安装负担被用户排除，不作为替代或回退。换前端不免除 backing 隔离
与执行隔离。安全模型不可实现的系统版本不支持受保护模式。
旧性能报告是预算输入，不是重新开放 native-root 旁路的理由。

## 5. 必须落实的边界

### 存储与路径

私有 store 不在 agent 可写目录内；其父目录、服务配置、二进制和 IPC 端点同样受保护。
不把 backing fd、管理 socket、宿主 cwd 或可写日志 fd 继承给 agent。启动前关闭非授权
描述符，清理环境中的管理凭据，给会话分配私有 HOME、TMPDIR 和可写缓存；共享工具链
只读，凭据通过范围受限的服务提供，不整体暴露宿主 agent 配置目录。
私有映像不能仅靠路径名、`0600` 或与 agent 相同 UID 的 `0700` 父目录保护：同 UID
进程仍有所有者权限。应由 agent 不具备的系统身份持有映像及其全部硬链接，并阻止
agent 经设备节点或服务代理取得同一内容；运行环境的默认拒绝策略再提供会话边界。

后端用固定根描述符和验证过的对象身份处理请求，限制路径解析在指定 World 内。
不信任用户传入的 backing 路径、`.world`、symlink 或重命名后的旧字符串。
工作区可包含 symlink，但解析到主机路径后的访问仍由执行策略拒绝；跨卷 hardlink
拒绝。clone、rename、xattr、权限修改和特殊文件创建均不得取得后台服务身份的权限。

### 管理与代理调用

broker 校验操作系统提供的调用者身份和会话能力；不信任请求中的 uid、pid、World ID。
同一 UID、可执行文件签名、知道 socket 路径三者都不能单独证明管理授权。能力绑定
World、允许的操作和会话代次，可以撤销，不能借 `--force` 或嵌套 CLI 升权。

限制 agent 调用宿主 LaunchServices、Apple Events、launchd、远程 shell、本地 socket
等能让未隔离进程代执行的通路。确需开放的代理逐项定义权限，不把“请求被允许”扩展成
代理的完整宿主权限。编辑器 task、插件和终端如果执行项目代码，也必须进入会话。

### 会话与失效

启动顺序是：建立受保护挂载点 → 挂载并核对卷身份/代次 → 建立执行策略 → 运行正向与
拒绝探针 → 启动 agent。仅 `/usr/bin/true` 成功不足以证明隔离生效。

挂载点下层目录及其父目录由可信方控制；卸载、重挂载、挂载点替换不能产生可写的
普通目录窗口。不能只轮询发现卸载后再杀进程。旧 fd、cwd、映射及缓存的撤销语义
必须实测；无法可靠撤销时必须先停止并回收全部访问者，再改变卷代次或恢复 checkpoint。
agent 不能卸载、替换挂载或取得宿主管理员能力。

`--no-sandbox` 不能用于新的受保护 World。旧目录模式若为迁移保留，必须显式标为
legacy/unprotected，不能被当成启动失败时的 fallback。

本机 macOS 27.0.1 的初步探针：`sandbox-exec` 使用 `(deny default)`，仅给一个临时目录
`file-write*`、给执行进程必要的读/启动权限，`/bin/sh` 在允许目录写入成功，在另一个
临时目录写入返回 `Operation not permitted`。这只证明简单规则在这台机器上生效，
不证明任意 agent 的后代、宿主服务代理、描述符转交或未来 macOS 版本满足本节契约。
后续 launcher 必须使用实际运行方式做完整 §8 绕过测试。
同机补充的路径别名探针还发现：对一个 `0600` 文件仅按 literal 路径拒读时，直接路径
及 symlink 路径拒绝，但预先存在的 hardlink 可以从另一个名字读出；受限进程现场尝试
创建 hardlink 则返回 `Operation not permitted`。因此拒绝规则必须覆盖**对象所有权、
所有名字和能力转交**，不能仅靠记录 backing 的一个路径；该探针也不证明未来版本语义。
在另一临时映像上，受上述最小 profile 约束的 `hdiutil detach` 返回
`Operation not permitted`，映像仍挂载；可信进程随后正常卸载并清理。这只覆盖该命令
在当前系统上的一条代理路径，`diskutil`、其他系统服务和已有 fd 仍需 §8 的完整验证。

## 6. 文件系统语义、历史与性能

正确性原型可以对私有完整 APFS clone 做受控透传，不要求立即实现 overlay/whiteout 或
重新实现 APFS。原型同时测量 fork 延迟、缓存与元数据往返，不把性能问题推迟到架构定型后。

上线前明确并验证：稳定 inode/卷身份、open-unlink、rename 原子性、hardlink、权限和
ACL、xattr、大小写行为、文件锁、mmap、fsync、崩溃恢复、缓存一致性，以及 Git/build/
编辑器常用调用。未支持的操作返回明确错误，不能成功返回却遗漏语义。
checkpoint 必须先建立写入屏障，处理脏缓存、mmap 和在途操作，发布持久的存储状态后
恢复写入。旧的协作 `flock` 不能约束任意挂载访问者。

连续历史设计随之调整：文件系统可提供变化来源，FSEvents 不再是唯一入口。但内核写回
可能合并多个用户态写入，不能宣称“挂载就能保存每次编辑”。revision 的发布边界、
fsync 持久性、磁盘满时行为和审计失败策略要单独定义；必须记账的操作不得在记账失败
时静默成功。现有观察历史继续按已有较弱保证描述。

### 6.1 “足够好”的首版性能预算

以下是工程验收目标，**不是已有结果或性能承诺**。参考机器为 Apple M1 / 16 GiB / 本地
SSD；记录系统、SDK、文件系统配置与仓库 fixture。native 基线使用同盘等价 APFS clone，
保持相同内容、工具版本、构建并行度和持久性要求。新模式必须开启完整隔离与默认历史策略。
用 `T` 表示同一场景的墙钟耗时；逐项比较对应的 p50/p95，不用综合平均掩盖最慢场景。

| 场景 | 首版验收目标 |
| --- | --- |
| 编辑保存：4 KiB 临时文件 + fsync + rename，同目录已预热 | p95 ≤ 10 ms |
| 热读：open/read/close 4 KiB；属性缓存命中的 stat | p95 ≤ max(1.5 × native p95, native p95 + 20 µs) |
| 50k 文件 Git status：干净、800 处修改分别测量 | 热态 p95 ≤ 500 ms；新挂载首轮 p95 ≤ 2 s |
| 增量 build + test、无变化 rebuild | T p50/p95 ≤ max(1.25 × native 对应值, native 对应值 + 100 ms) |
| 完整 build、读取源码上下文、全树搜索 | T p50/p95 ≤ max(1.5 × native 对应值, native 对应值 + 100 ms) |
| 本地依赖安装/解包、Git checkout、大量生成文件 | T p50/p95 ≤ max(2 × native 对应值, native 对应值 + 100 ms)；不含下载等待 |
| 已有 World 的挂载：请求到卷可访问，不含首次安装或导入 | p50 ≤ 300 ms，p95 ≤ 400 ms |
| pool 命中：fork 请求到挂载、隔离探针完成并可启动命令 | p50 ≤ 400 ms，p95 ≤ 600 ms；不能只量领出 clone |
| pool 未命中：50k 文件完整可用 World | p95 ≤ 2 s；含挂载和策略就绪 |
| 1 GiB 顺序读/写并完成相应同步 | 吞吐 ≥ 同口径 native 的 80%，读写分别验收 |
| 8 个活跃 World，同等总 CPU 并行度和负载 | 聚合任务吞吐 ≥ native 的 70%；交互保存 p95 ≤ 20 ms |
| 1000 个已创建但未挂载 World，后台维护完成后 | 服务总增量常驻内存 ≤ 256 MiB，稳态 CPU < 单核 1% |

1000 个未挂载 World 不代表承诺 1000 个同时挂载；同时挂载的 1/8/32 个 World 另报扩展、
broker 和可归因的系统进程内存、fd 数及 CPU。资源不足应限制活跃数或拒绝启动，不能绕过
挂载。初次安装扩展与首次导入不计入 fork 指标，但必须单独报告，不能隐藏在预热步骤里。

对持续时间很短的微操作至少取 1000 个样本；交互与生命周期至少 100 个样本；长构建/
安装场景发布验收至少 20 轮，报告全部样本与尾延迟，样本不足不声称 p95 已达标。
交错运行 native/候选，分别测新挂载与重复访问、pool 命中与耗尽、首次 CoW 写与后续写。
新挂载只代表名字/卷缓存冷启动，不冒称物理磁盘冷缓存。禁止用 best-of-N 作为发布成绩。
首个数据面采样工具是 [`scripts/bench/mounted_fs.py`](../scripts/bench/mounted_fs.py)：
它在两个已存在的不同设备目录内建临时 fixture、交错取样并输出原始 JSONL 与 p50；
每项每侧满 1000 个样本才输出 p95，否则标记 `insufficient_samples`，p95 值为 `null`。
它不核验 forkfs 卷身份，也不包含挂载、隔离、broker、历史或构建/安装的完整场景，
因此只能作为上述验收的一部分。

### 6.2 已知瓶颈决定优化顺序

仓库的 [Handler API 测量](FSKIT_HANDLER_API_MACOS27.md) 在 2026-09-19 记录了：
lookup-miss 往返约 74.64 µs，create+unlink 约 995 µs、11.04 次回调，热读和热 stat 则接近
native。它是旧透传原型的结果，不是新隔离架构的成绩。[往返研究](PERF_STUDY_RT_PARALLEL.md)
还观察到单挂载并发瓶颈。串行链延迟可按“各回调耗时之和 + 后端工作 + 排队”分解；
内存元数据缓存不能消除内核仍然发出的请求，增加 worker 也未必能消除 FSKit 的串行点。

因此按以下顺序实施，并对每项记录前后对照：

1. **先避免增加第二层同步往返。** broker 管生命周期和授权，不为每次 read/stat 再查库
   或审批。优先验证受信数据面能否持有范围受限的 backing 根句柄，在既定授权下直接
   执行 I/O；句柄只交给可信扩展，绝不交给 agent。如果系统权限要求数据面再访问独立
   服务，分别测 RPC 成本、批处理和数据传输成本，计入总预算，不能假定为零。
2. **减少前端回调与后端系统调用。** 复用 Handler 返回属性，目录枚举批量返回可用属性，
   使用有上限的 inode/fd 缓存，避免每个请求重新拼完整路径、open/close。这些优化必须
   先保持 hardlink、rename、open-unlink 和引用寿命正确；身份不能只靠旧路径字符串。
3. **让热访问留在内核缓存。** 属性/名字/数据缓存只在能兑现一致性和撤销协议时启用，
   不靠无限 TTL。broker 修改 backing 也必须经过数据面的屏障和缓存失效协议，不能构成
   不可见的第二写者。旧原型开启 `FSVolumeDataCacheHandler` 反而增加 open/close 往返，
   因此按真实工作负载 A/B，不能把“开启缓存”直接当成优化结果。
4. **减少全局串行化。** 读路径不取全局数据库写锁；World 之间的队列、锁与资源预算分开。
   同 World 使用满足 rename/目录语义的细粒度同步。分别测单挂载多进程和多挂载并发，
   一个慢 World 的 GC/checkpoint 不应阻塞其他 World 的交互访问。
5. **保持 APFS CoW 和有界预热。** 起步使用同一私有 APFS 卷内的整树 clone 与小规模
   pool，预建内容可加速 fork，但完整计入挂载/授权时间。复用挂载实例必须先证明旧 fd、
   缓存和会话能力已失效，不能只更换 World ID。pool 按空间、命中率和后台 I/O 预算补充。
6. **历史和维护不堵塞常规 I/O。** 变化索引替代每次全树扫描；内容哈希、压缩、GC 可后台
   处理，但必须先保住相应版本的不可变内容或可靠日志。仅把路径排进异步队列会丢中间版本。
   fsync、checkpoint 和约定的持久化边界不能提前报成功；队列饱和时背压或明确拒绝。

构建产物与依赖仍须在受控挂载中。允许经过验证的只读依赖缓存或有版本的内部批量导入，
但任意工具的普通 create/write 路径也必须达标；不能靠把 node_modules/build 放回宿主
可写目录、特殊识别 benchmark 或去掉历史策略来取得成绩。

### 6.3 前端选型必须同时过安全与性能门槛

边界原型阶段就运行保存、Git status、元数据安装和增量构建四组测试，再评估 pool/多
World。现有 FSKit 数字已提示元数据密集负载有明显风险，不能承诺靠局部优化追到 native。
若热点主要在不可减少的系统往返，停止继续微调对象分配，把相同 fixture 和隔离契约用于
替代方案的对照实验：

| 候选 | 要证明的收益与代价 |
| --- | --- |
| FSKit + 私有 APFS clone | 现有代码复用；必须解决元数据尾延迟、服务连接与多挂载资源成本 |
| macFUSE | 已排除：用户不接受安装负担，不再投入该路线的集成或性能原型；历史调研仅作参考 |
| 由 broker 管理的独立 APFS 卷或磁盘映像 | 原生内核数据路径的候选；评估 Apple 的 sparse/raw 映像与 shadow/overlay，同时证明块设备/映像/挂载管理不可绕过，并解决 fork、历史和恢复成本 |

原生 APFS 卷路线不能直接复用“同卷 clone 很快”的推论：Apple 的
[APFS 说明](https://developer.apple.com/documentation/foundation/about-apple-file-system)
把快速文件克隆限定在同卷，卷间共享空闲空间不等于共享克隆；仓库
[跨卷实测](CAPACITY_QUOTA_FEASIBILITY.md) 也记录了同容器 `EXDEV`。独立映像若通过宿主
APFS 克隆映像文件，必须先卸载或取得受支持的一致性屏障，验证身份冲突、稀疏分配、
挂载延迟与写放大；不能对活跃映像直接 clone 并宣称获得一致 fork。

原生卷仍是真实挂载，但不天然提供逐操作用户态历史；若采用此路线，安全由内核权限与
会话隔离执行，历史按实际可证明的 checkpoint/观察保证声明，不承诺拦截每次写入。

候选必须同时通过 §8 安全矩阵、POSIX 语义和本节性能预算才能作为默认。都不通过则
继续实验并明确未就绪；不自动降低门槛，也不回退成普通目录。

### 6.4 第一轮架构判定（2026-10-07）

**现有 FSKit passthrough：发布方案 No-Go，研究样本保留。**
[完整负载测量](FSKIT_HANDLER_API_MACOS27.md#6-真实负载)中的 Handler API 在 agentstress
编辑循环仅为 native 的约 18%，生成文件约 19–22%，安装树约 16–17%；新增独立 broker
尚未计入。与 §6.1 的 1.25–2 倍目标有数倍差距。减少本地对象分配无法弥补已测到的
内核/FSKit 往返。若将来更换调用模式或 macOS 实现使完整负载达标，可以重评；当前不能
直接把透传前端接上私有 store 就称为性能方案。

**原生 APFS 映像：保留为性能优先的可行性候选，尚未通过 fork/安全验收。**运行
[`scripts/bench/apfs_image_probe.py`](../scripts/bench/apfs_image_probe.py) 在本机 macOS 27.0.1
/ arm64 / 内部 APFS 卷，创建一个一次性 128 MiB sparse image；写入、卸载后在宿主同卷
用 `cp -c` 克隆映像，挂载副本修改，再挂载原件确认未变化。一次探针：创建 1015 ms，
`cp -c` 14 ms，三次 attach 为 258/234/269 ms，detach 为 234/290/229 ms。两个映像各报告
约 8 MiB 已分配。这证明**该次离线映像克隆后内容独立**，不证明活跃映像一致性、物理
块共享量、真实项目 fork 时间、p95、卸载可靠性、并发挂载或 agent 隔离。探针完成后已
卸载并清理私有目录。输出是单样本观察，不能套用为产品预算。

用户确认约 300 ms 的挂载可接受，因此 §6.1 将**挂载**与**完整 fork 到可运行**分开计时，
并把旧 p95 250 ms 的完整 fork 目标改为 600 ms。补充在同机另一临时 128 MiB 映像上
连续执行 20 次 attach/detach：attach p50 258.7 ms、p95 268.8 ms（最慢 272.7 ms），
detach p50 207.6 ms、p95 240.6 ms（最慢 241.0 ms）。所有卷均卸载并删除。该短探针
支持把原生映像继续作为候选，但没有测到 broker、隔离探针、真实项目、首次挂载或
多 World 竞争；§6.1 的 400/600 ms 仍是待验证的目标。

下一道判定按依赖顺序进行：

1. 在完全由 broker 持有的映像上，用真实 10k/50k 文件项目测 native I/O 和挂载成本；
   执行隔离仍需另验。确认映像文件、挂载与设备节点不能被 agent 直接打开或替换。
2. 把单写者的保守 fork 路径扩展为**全部写者**的可证明一致点。验证暂停、同步、卸载、
   clone、重挂载的完整时间和失败恢复；若不能满足交互预算，验证受支持的快照/映像机制，
   不对 mounted sparse image 直接 `cp -c`。记录原 World 与子 World 的 UUID、缓存、
   文件锁、inode 身份及开放 fd 行为。
3. 单独计算 1000 个 idle World 的映像元数据、设备/挂载资源和恢复扫描成本；测试
   8 个同时工作的 World，以及内存、低空间、异常卸载和重启。离线镜像小样本不能外推。
4. 映像候选若因活跃 fork、历史或安全边界失败，继续比较能通过 §6.1 的挂载前端；
   不靠缩小真实负载或在用户目录暴露 backing 来过关。

**单写者活跃 fork 探针已通过，尚未完成第 2 项。** Luna worker 实现
[`scripts/bench/apfs_active_fork_probe.py`](../scripts/bench/apfs_active_fork_probe.py)，Sol 审查
了写入进度握手、完整记录校验及异常设备清理，并保存
[五轮原始结果](evidence/APFS_ACTIVE_FORK_20261007.json)。在同机 160 MiB 临时 APFS 映像上，
每轮都有持续 append 写者；停止写者并 `fsync`/关闭 fd 后执行全局 `sync`，卸载源卷，
`cp -c` 克隆映像，重新挂载源卷并恢复写者，再挂载子卷。

| 指标 | 五轮范围 | 中位数 |
| --- | --- | --- |
| 源卷暂停写入（停写到源卷重新挂载） | 402–558 ms | 536 ms |
| 停写到源卷与子卷均挂载 | 619–762 ms | 747 ms |
| `cp -c` 本身 | 3.3–5.6 ms | 5.3 ms |

五轮都验证了连续序号与每条记录的 hash，子卷内容逐字节等于暂停点，恢复后的源卷继续写入
且与子卷相互独立；每轮恢复后新增 55–59 条记录。探针退出时无附加设备，临时目录已清理。
这证明**单写者在干净停止/卸载后可一致地 fork**，也显示主要时间花在停止、同步和
卸载/重挂，而非 CoW 复制。五个样本不足以报告 p95；全局 `sync` 会受机器上其他写入
影响。真实项目的多进程写者、未关闭 fd、mmap、异常断电、volume UUID 冲突、broker
和会话隔离均未覆盖。原生映像继续作为下一阶段首要候选，尚不能宣称探索完成或发布就绪。

映像文件的**外层格式**可选择，APFS 只规定卷内的数据结构。Apple 的
[Disk Utility 映像格式说明](https://support.apple.com/en-ie/guide/disk-utility/dskutl1002/mac)
列出 raw、sparse 和 sparse bundle；新的
[DiskImageKit 文档](https://developer.apple.com/documentation/DiskImageKit)还描述 ASIF 的
overlay 层，但主要以虚拟机为使用示例，宿主卷是否能用该 API 的栈仍待实测。
本机 `hdiutil attach -shadow <file>` 的临时 APFS 映像探针成功生成独立 shadow 文件：
在 shadow 挂载内写入后，卸载并单独挂载 base 看不到该写入。这只证明现成 shadow 能
隔离 base，不证明它能对**正在写入**的 base 安全分叉，也没有证明 shadow 链的性能、
崩溃恢复、跨版本兼容。另一次 `diskutil image attach --shadow` 探针没有生成指定 shadow，
写入落在 base；在查明该命令参数语义前不得用它承载隔离保证。优先验证受支持的
现成容器/层叠机制；如果自定义外层格式，还必须提供 macOS 认可的块设备或映像接入层，
不能让一个任意 `.img` 文件直接被内核 APFS 当成卷。

### 6.5 借鉴 Btrfs 的分叉模型

[Btrfs 设计文档](https://btrfs.readthedocs.io/en/stable/dev/dev-btrfs-design.html)明确描述了
可写 snapshot：多个 subvolume 最初共享树根和 extent，写入时复制相关树路径，源与
snapshot 随后独立。forkfs 可以借鉴**共享不可变底层块 + 每个 World 自己的可写块映射**，
而让卷内 APFS 继续负责文件、目录、ACL 和 POSIX 行为。一个具体模型是：

```text
同一只读基底映像 B
  ├── World W1: B + 可写层 L1
  └── World W2: B + 可写层 L2
```

fork 时先取得卷的一致点，封存当前写层，再给父子各接一个新写层；读请求沿层查找最近
写入的块，写请求只落当前层。Apple 的
[DiskImageKit 层叠映像](https://developer.apple.com/documentation/DiskImageKit)
与这一模型相近，但其公开示例主要针对虚拟机，宿主 APFS 卷能否用同一条链路尚未证明。
本机 `hdiutil -shadow` 只验证了静态 base 的写隔离。

外层块映射不会自动得到 Btrfs 的一致性保证：APFS 仍可能有脏页、在途元数据写入和
打开的 fd；封存写层前要有可验证的同步/暂停协议。两层 CoW 还可能放大 APFS 元数据
写入，层数增长会增加读放大和恢复复杂度，所以必须测 Git/build/安装真实负载、层深度
与合并成本。可以借鉴 Btrfs 的**树根引用、代际封存与引用计数**，但不要假定把 Btrfs
整体搬进自定义 img 就能保持 macOS 原生性能。

### 6.6 可复用的 NVMe 块层结构

[SPDK Blobstore](https://spdk.io/doc/blob.html)是最贴近自定义外层块映射的开源参考：
它用 extent 表管理稀疏分配，以只读 snapshot 为多个可写 clone 的后备，未覆盖的读沿
后备链查找，首次写入触发 CoW；文档还说明异步 I/O、元数据批处理及单线程元数据所有权。
若将来需要自研外层，可借鉴这些**块地址映射、共享基底和写入层**机制，并重点测试
clone 链深、4 KiB 随机写、元数据放大、断电恢复和 GC。SPDK Blobstore 自身不是 POSIX
文件系统，也没有提供 macOS 上可直接挂载 APFS 的块设备前端；其现成 NVMe 运行环境
主要面向 Linux/FreeBSD，因此不能把它当作可直接引入 forkfs 的 macOS 实现。
尤其不能照搬默认 1 MiB cluster：Blobstore 文档说明首次 CoW 会读写整个 cluster，
而 forkfs 的编辑保存与 APFS 元数据写入常远小于此；分配粒度必须通过工作负载实测选择。

Btrfs 的 [subvolume/extent 设计](https://btrfs.readthedocs.io/en/stable/dev/dev-btrfs-design.html)
适合借鉴树根共享、引用计数和可恢复的延迟回收；这对大量 World 分叉及历史清理尤其有用。
在保留 APFS 的方案中，借鉴范围是外层数据组织，不能替代 APFS 的卷内语义，也不能消除将自定义块映射
接入 macOS 内核块设备栈的工程与授权成本。当前先完成 Apple 原生映像和 shadow 的性能、
隔离与恢复验证；只有它们未达门槛，才考虑自研块层。若复用源代码，还需逐文件审查
[SPDK 的 BSD-3-Clause 及子模块许可](https://github.com/spdk/spdk/blob/master/LICENSE)
与 forkfs 的许可边界。

### 6.7 外层格式实测与当前架构选择

[`apfs_container_compare.py`](../scripts/bench/apfs_container_compare.py) 在同一台 macOS
27.0.1 机器上创建一次性 160 MiB APFS 卷，交错测试 UDIF/UDRW、SPARSE、SPARSE+shadow，
每组五轮。各轮先对相同 fixture 读/stat 预热，再做 4 KiB 临时文件 fsync+rename、64 次
小文件 create/unlink、4 MiB 顺序写+fsync；卸载重挂后验证保存内容、文件哈希和删除状态，
并核对 shadow 的 base 未变。原始样本见
[`APFS_CONTAINER_COMPARE_20261007.json`](evidence/APFS_CONTAINER_COMPARE_20261007.json)。

| 容器 | attach 中位数 | detach 中位数 | 4 KiB 保存中位数 | 小文件 create/unlink 中位数 | 4 MiB 写+fsync 中位数 |
| --- | ---: | ---: | ---: | ---: | ---: |
| UDIF/UDRW | 214.90 ms | 156.32 ms | 0.747 ms | 132.0 µs/次 | 2.336 ms |
| SPARSE | 225.90 ms | 121.46 ms | 0.762 ms | 140.4 µs/次 | 2.850 ms |
| SPARSE+shadow | 243.30 ms | 178.46 ms | 0.896 ms | 169.2 µs/次 | 3.278 ms |

这些小规模热态结果没有显示 shadow 有决定性的挂载或 I/O 优势；它为静态 base 提供了
已验证的分支隔离。UDIF 基底立即占用 160 MiB，SPARSE 基底约占 8 MiB，故大量短命
World 的首版原型优先用 **APFS SPARSE 映像 + 宿主 APFS clone**。原生 APFS 承担 POSIX
数据路径，broker 管卷生命周期与私有映像，受限启动器约束 agent。活跃 fork 先用停写、
同步、卸载、clone、重挂协议；已测五轮停写中位数 536 ms、两卷可用中位数 747 ms，
还没有达到 §6.1 的 pool 命中目标。可预建 World pool，但不能以预建掩盖失效撤销、
空间使用或未命中延迟。

**shadow/ASIF 层叠保留为第二阶段候选**：它可能避免复制完整容器，并允许多个 World
共享一个封存基底；但“从正在写的 World 分叉”需要可靠的一致点和父子各自的新写层。
当前 shadow 探针仅覆盖静态 base。Apple 的
[DiskImageKit StackedImage](https://developer.apple.com/documentation/diskimagekit/stackedimage)
公开了 base/cache/overlay 组合，尚未证明其栈能作为宿主 APFS 卷挂载。本机 `diskutil`
帮助显示可创建 ASIF，实际创建、挂载和层叠持久化仍待单独探针。直接移植 SPDK/Btrfs
数据结构作为自定义块设备属于第三阶段备用路线：必须额外实现 macOS 接入、崩溃恢复、
空间回收与双层 CoW 性能控制；Apple 的
[BlockStorageDeviceDriverKit](https://developer.apple.com/documentation/blockstoragedevicedriverkit)
面向设备驱动并要求系统扩展，不是可直接把任意用户态映像注册为 APFS 卷的现成接口。

这个排序是**原型投资顺序，不是发布结论**。每组只有五轮，不可报告 p95；数据操作预热，
attach 未清缓存；`st_blocks` 无法表示宿主 APFS clone 的独占 extent，主机可用空间又会
受别的写入干扰。尚缺 50k 文件 Git/build/安装、多 World 并发、断电恢复、真实 agent
绕过矩阵和长期 shadow 链深度测试。只有通过 §6.1 与 §8 才能定为默认架构。

### 6.8 “数据结构”的具体层次

本轮调研中的数据结构指**文件内容及元数据实际如何组织、寻址和持久化**，包括磁盘上的
树、extent 映射、分配信息和快照引用；并非只讨论内存索引。映像方案需要区分三个层次：

| 层次 | 存储结构与职责 | forkfs 的选择空间 |
| --- | --- | --- |
| 卷内文件系统 | 目录、文件元数据、文件偏移到逻辑块的映射、文件系统事务 | 使用内核 APFS 就必须遵循 APFS 格式；自定义这一层意味着实现另一种文件系统 |
| 映像或虚拟块层 | 卷逻辑块到映像偏移/extent 的映射、稀疏分配、overlay、块快照 | 可以选择 Apple 格式，或自研映射并提供可挂载的设备接入；`.img` 只是文件名后缀 |
| 宿主文件系统与设备 | 映像文件的 extent、宿主 CoW、写回及最终设备 I/O | 当前原型使用宿主 APFS；外层代码不直接控制 NVMe 的全部行为 |

例如修改 `src/main.cpp`：卷内 APFS 更新文件数据与元数据块，映像层持久化相应逻辑块，
宿主 APFS 再把映像文件变化落盘。各层的缓存、同步、分配和 CoW 都影响性能，不能只凭
外层格式或“面向 NVMe”断言端到端更快。物理存储也可能包含额外的设备内部映射。

Btrfs 是完整文件系统，其目录树、extent 和可写 snapshot 可在 Linux 直接使用；在 macOS
保留 APFS 时，只借鉴它的共享树根和引用管理思想。SPDK Blobstore 是块分配/快照组件，
更接近外层映射，不提供目录和完整 POSIX 语义。两者都不能通过改一个映像后缀成为 APFS。
宿主 APFS clone 已共享映像 extent，故 UDIF 基底占用 160 MiB 不意味着每个 clone 都再
独占 160 MiB；独占空间与写放大须另测。数据布局和 agent 权限边界是独立的设计问题。

### 6.9 Linux 对照：优先原生 Btrfs 子卷快照

**Linux 的通用入口不要求 Btrfs；已有可用 Btrfs 时，子卷快照是优先验证的原生优化。**
默认部署选择见 §6.11。Linux 无需复刻 macOS 的 APFS 映像绕行；Btrfs 本身就提供需要的
共享存储结构。这里是新设计建议，尚未实现或得到本轮 Linux 实测支持。

仓库当前状态不同：[`LINUX_BTRFS.md`](LINUX_BTRFS.md) 与
[`LINUX_XFS.md`](LINUX_XFS.md) 记录的是逐文件 `FICLONE` 目录后端，冷 fork 为
O(entries)，Btrfs 后端尚未使用原生子卷快照；[`LINUX_EXT4.md`](LINUX_EXT4.md)
使用数据复制，成本为 O(data + entries)。现有 Bubblewrap 执行隔离可供复用和审查，
不能把现有目录后端标注为新的挂载式实现。

| Linux 候选 | 存储与分叉方式 | 主要收益 | 代价与当前判断 |
| --- | --- | --- | --- |
| Btrfs 子卷 | 每 World 一个子卷，原生 snapshot 共享 extent；单独挂载指定 subvolid | 内核原生文件操作；不逐文件克隆；源卷可保持挂载 | 需要 Btrfs 存储池；验证刷脏、ENOSPC、快照回收和配额；首选 |
| OverlayFS | 不可变 lower + World 专属 upper/work；挂载合并视图 | 内核挂载，创建分支无需立即复制全树；可使用现有宿主 FS | copy-up、whiteout、rename/hardlink 等语义须验证；活跃 upper 分叉需封存协议；兼容候选 |
| dm-thin + ext4/XFS | 每 World 一个 thin 块设备，块快照共享池空间 | 明确的独立块卷，可保留 ext4/XFS 文件语义 | 管理权限、池配置、同步/冻结、恢复和容量监控更复杂；适合受管 Linux 主机 |
| XFS reflink + bind mount | 逐文件共享数据，挂载现有目录视图 | 可作为性能基线和内部物化工具 | 不满足用户要求；排除为新模式 World 后端 |
| 文件映像 + loop | 宿主文件内运行 Btrfs/ext4/XFS | 不要求重格式化宿主；便于一次性原型 | 增加写回/分配层；活跃映像复制仍需一致点；须测空间耗尽和持久性 |

[Btrfs 子卷文档](https://btrfs.readthedocs.io/en/latest/Subvolumes.html)说明子卷可以独立
挂载，快照初始共享数据块且可写。它不是独立块设备，也不因挂载就自动成为安全边界。
原生快照避免逐文件复制，但创建时的刷脏可能耗时，删除后的空间回收也可能异步持续；
不能把 fork 总延迟承诺成固定 O(1)。快照不递归包含嵌套子卷，新模式应拒绝未管理的
嵌套子卷，或明确逐个纳入一致性协议，不能沿用现有遍历复制时的语义而不做迁移。
文件系统一致点也不等于跨文件的应用事务一致点；需要后一种保证时仍须协调写者。

[OverlayFS 文档](https://docs.kernel.org/filesystems/overlayfs.html)中的 copy-up 和层间
语义需要在 Git、构建、mmap、hardlink 与 rename 场景验证；upper 的变化不能未经解释
直接当作完整的用户编辑历史。[dm-thin 文档](https://docs.kernel.org/admin-guide/device-mapper/thin-provisioning.html)
描述了多 thin 设备共享池与递归快照，映射结构避免简单链式查找随深度增长；这也值得
macOS 自研块层参考，但 Linux 内核实现不能直接移植为 macOS 可挂载组件。活跃块快照
还需正确处理文件系统同步、设备暂停与应用一致性，不能只复制映射根。

Linux 受保护模式中，broker 持有私有存储池及管理能力，agent 只看到获授 World 的
挂载和显式工具链；store、其他 World、原始设备与管理 socket 不进入其可访问空间。
mount namespace 应隔离挂载传播，配合能力剥离、`no_new_privs`、fd 清理及代理限制；
具体安全性取决于完整策略，[Bubblewrap](https://github.com/containers/bubblewrap)
提供机制而不替代策略。仓库现有策略将宿主大部设为只读，尚不等于“只暴露显式允许
读取的路径”；若采用新的保密契约必须相应收紧。卸载下层目录、逃逸 fd、设备访问、
管理 ioctl、嵌套 namespace 及继承子进程继续纳入绕过测试。

跨平台应统一 World 身份、fork/checkpoint/restore 契约、受限执行和失效行为，允许存储
实现不同：macOS 当前优先自动管理 APFS 映像，Linux 优先利用现有宿主文件系统，在已有
Btrfs 时可选原生子卷优化。Linux
实验应比较 snapshot 到会话可用的全链路延迟、Git/build/安装、8 World 并发、脏数据
下的分叉、ENOSPC 与删除回收，不能拿当前 XFS 目录基准或 macOS 数值代替。

### 6.10 宿主为 XFS 时的具体选择

**既有 XFS 磁盘、希望保持宿主存储布局时，优先验证 OverlayFS on XFS。** 每个 World
暴露一个独立 overlay 挂载，封存基底放在 lower，当前写入放在该 World 专属的 upper，
work 与 upper 位于同一文件系统。目录、文件和 extent 仍由 XFS 持久化，OverlayFS
提供合并视图、按需 copy-up 和删除标记；这些私有目录不能暴露给 agent。需先检查 XFS
提供有效的 `d_type` 与必要 xattr，不能仅凭文件系统名字接受为 upper。

从已封存基底创建子 World 时，只需建立专属 upper/work 和挂载，避免在前台逐个克隆
基底文件。这里没有实测时延，也不承诺整个操作恒定时间。首次修改 lower 文件有
copy-up 成本；即使底层支持 reflink，也必须验证目标内核实际复制路径，不能假定零复制。
跨层目录 rename、hardlink、inode 身份和 mmap 是选型门槛，依据见
[OverlayFS 内核文档](https://docs.kernel.org/filesystems/overlayfs.html)。

活跃 World 分叉更复杂：不得把仍在写入的父 upper 直接当作子 lower，也不得让两个
可写挂载共用 upper/work。需要协调访问者、持久化并安全封存当前代，再为父子各建立
新写层；打开的 fd 和 mmap 如何处理必须有明确契约。层增长后还需压平和 GC，不能
只测初始空 upper 的挂载速度。现有 reflink 代码可用于物化封存基底或预建池，但处理
overlay 元数据必须遵循其语义，不能直接把 upper 当作普通完整树导出。

如果可以配置专用块存储池，且要求**每 World 是独立 XFS 块卷**，选择候选转为
**dm-thin + XFS**：块快照由 XFS 下方的 device-mapper 提供，文件操作仍由原生 XFS
执行。这要求预先建立并管理 thin pool；已有的普通 XFS 目录不会自动获得此能力。
需要验证冻结/解冻、设备快照发布、克隆卷身份与挂载、空间回收和池耗尽行为，参考
[dm-thin 文档](https://docs.kernel.org/admin-guide/device-mapper/thin-provisioning.html)。

单纯 XFS `FICLONE` 只共享文件的数据 extent，完整目录树仍需逐项建立，见
[FICLONE 接口](https://man7.org/linux/man-pages/man2/ioctl_ficlone.2.html)。给该树做 bind
mount 可以提供受控访问入口，但不会产生卷级快照能力。用户进一步明确 XFS 上也不能
提供普通文件夹式 World，因此目录和 bind mount 路线仅作为旧模式、性能基线及内部
物化工具保留，不作为新模式后端，也不作为挂载失败时的回退。OverlayFS 与 dm-thin 的共同
安全前提仍是私有存储、受限 mount namespace、隔离管理能力及完整启动链。

### 6.11 修订：通用部署，不要求用户管理卷

用户明确要求降低部署负担，因此**取消“每个 World 必须是独立卷”的产品约束**。保留
真实 VFS 挂载、稳定 World 身份、受限会话和不可直接访问的私有存储。底层普通目录可以
作为内部实现；它不能作为用户可直接绕过挂载访问的工作区。挂载本身不提供全部隔离。

用户继续指定源目录与目标路径，由 forkfs 完成导入、分叉、挂载和清理。不能要求用户
先配置 Btrfs、LVM、分区或 NVMe 设备；必要的服务安装属于平台部署步骤，不应在创建
每个 World 时重复。原型按以下能力选择后端，所有后端兑现同一访问边界：

| 环境 | 优先验证的内部实现 | 用户无需承担的工作 |
| --- | --- | --- |
| Linux，宿主支持 OverlayFS 所需能力 | 私有封存基底 + 每 World upper/work + 受控 overlay 挂载 | 创建卷、分区、手工维护层 |
| Linux，不适合 OverlayFS | 后续评估自动管理的文件系统映像 + loop 等真实文件系统挂载；未验证前拒绝启用新模式 | 手工分区或创建卷；不得回退普通目录或 bind mount |
| Linux，已有 Btrfs | 可选原生子卷 snapshot 优化 | 为使用 forkfs 而重格式化磁盘 |
| macOS | 服务自动创建/克隆/挂载 APFS 映像 | 理解映像格式、手工创建或挂载卷 |
| 受管服务器已有 thin pool | 可选 dm-thin 后端 | 普通用户管理块设备 |

OverlayFS 支持并不覆盖任意宿主文件系统和内核策略，故必须做实际能力探测。替代后端
必须仍是真正的文件系统挂载，并通过性能及隔离验收：backing 始终私有，mount namespace 不暴露其他
World 或 store，挂载失败后禁止向普通目录继续写入，agent 不获得存储服务身份。
已有后端的跨文件系统复制规则仍按其明确契约执行；上表是新模式的后端选择设计，不能
直接据此改变旧命令行为或静默掩盖性能降级。若平台无法提供必要挂载和隔离能力，应明确
报告不支持受保护模式。后端能力须可检查，性能与安全分别验收。

### 6.12 用户态 Btrfs 的取舍（2026-10-08）

完整搬运 Btrfs 到用户态可以设想为共同存储引擎，Linux 接 FUSE、macOS 接 FSKit，
后备使用私有映像文件。但这不是现成的可写跨平台库集成：
[btrfs-progs](https://github.com/kdave/btrfs-progs) 是管理、创建、检查等用户态工具；
libbtrfsutil 主要管理 Linux Btrfs，而非提供脱离内核的完整读写文件系统。
本轮查到的 [btrfs-fuse](https://github.com/adam900710/btrfs-fuse) 明确为只读实现，
不能承担 World 写入、可写快照及 checkpoint。不能据此断言不存在其他实现，但目前
没有核实到满足 forkfs 要求的可直接集成方案。

完整移植需要适配内核缓存、锁、工作队列与 I/O，并重新验证事务提交、extent 引用、
空间预留、fsync、mmap、故障恢复和快照回收。原生 Btrfs 的成熟度不能直接视为移植后
的正确性证明。磁盘格式兼容还意味着持续跟踪受支持的 Btrfs 特性和恢复工具；复用
代码须检查逐组件许可，不能将用户态管理库的许可套用到整个文件系统。

该方案不会消除 FSKit/FUSE 前端往返，也可能叠加宿主文件系统的 CoW 与缓存成本。
因此当前不把完整移植作为主线。若需要跨平台统一存储，先验证内存元数据前端的性能
下限，再考虑借鉴 Btrfs 的持久化 CoW 树、extent 共享、事务根发布与延迟回收，构建
范围更小的 World 存储引擎；这仍需完整的持久性和 POSIX 验证，不能视为低成本捷径。
若明确需要 Btrfs 磁盘格式互操作，或能通过现有兼容运行库大幅减少手工移植，且接受
长期文件系统维护投入，可以重评复用路线；后续发现的 LKL 见 §6.13。

### 6.13 可复用用户态引擎候选（2026-10-08）

进一步调研发现 LKL 提供了手工移植之外的路径，因此不能将“Btrfs 用户态”只等同于
从头重写。但用户随后指出 LKL 的整体复杂度过高，故取消其优先原型地位，不推进当前
集成；以下保留调研事实及取舍。目前没有确认可直接满足全部要求的现成引擎。

| 候选 | 可复用部分 | 与 forkfs 的差距 | 调研优先级 |
| --- | --- | --- | --- |
| LKL + Btrfs | 将 Linux 内核文件系统编译为用户态库，复用 Btrfs 文件树和快照逻辑 | macOS/arm64 构建与 FSKit 接入未验证；宿主 I/O、资源占用、并发和完整文件语义须测 | 因整体复杂度过高，退出当前实施候选 |
| JuiceFS 本地配置 | 用户态 POSIX/VFS、元数据引擎、数据块及缓存；可用 SQLite/BadgerDB + 本地数据存储 | Go 核心与当前 C++/FSKit 接口需桥接；目录 clone 复制元数据且不保证目录原子性 | 完整文件系统复用与性能对照候选 |
| SPDK Blobstore | 块分配、快照、clone | 无目录和 POSIX 层，macOS 运行与设备接入需适配 | 块层参考，不能替代完整引擎 |

[LKL](https://github.com/lkl/linux) 将内核代码作为库链接到应用，提供类似 Linux syscall
的调用接口，`lklfuse` 展示了用户态挂载映像的方法。这里的库接口调用不等于每次重新
进入宿主 Linux 内核；实际后备 I/O、宿主同步及 FSKit/FUSE 前端往返仍然存在。
[默认配置](https://github.com/lkl/linux/blob/master/arch/lkl/configs/defconfig)启用了 Btrfs，
但也关闭了文件锁等功能，不能把默认构建视为完整桌面 POSIX 实现。必须锁定版本，验证
所需配置、fsync/崩溃恢复、快照 ioctl、open-unlink、mmap 和内存/并发行为。macOS 上
仍需直接实现 FSKit 适配，不重新引入已排除的 macFUSE；不以 LD_PRELOAD 截获代替挂载。

[JuiceFS](https://github.com/juicedata/juicefs)已有元数据与数据存储分离的完整实现，
支持本地部署，并不要求使用云端对象存储或独立 Redis。它的
[社区版 clone](https://juicefs.com/docs/community/guide/clone/)共享数据块但复制元数据，
官方明确目录 clone 不保证原子性，因此不能直接实现低成本的整棵 World 一致快照。
复用时仍要补充分叉屏障或改造版本化元数据。源项目提供 FUSE 层并不证明已提供符合
本项目限制的 FSKit 挂载；目前也未核实其小文件性能满足预算。

下一步收敛为极简内存元数据原型，先分离 FSKit 前端成本和后端成本。若前端性能有
可行空间，再评估小型嵌入式事务存储组件，承载 inode、目录和版本映射，内容采用私有
数据存储。数据库事务或读快照不自动提供持久、可写的 World 分支，版本树与内容引用
寿命仍需明确设计。该方向缩小依赖范围，并不免除 POSIX、缓存、崩溃恢复和 GC 的工作。
JuiceFS 暂作实现参考，不因排除 LKL 就自动成为默认后端。任何候选都需检查实际复用代码
及依赖许可、维护状态与故障测试；本轮仅完成文档/源码配置调研，未集成或运行这些引擎。

## 7. 交付顺序与迁移

1. **边界原型**：临时私有 store、单 World 挂载、受限启动器、独立控制面。先证明扩展
   与存储服务通信、身份授权、不能直接打开 backing、失效不降级及卸载不落入下层目录。
   同时执行 §6 的早期性能选型测试，记录真实系统/SDK、安装条件和拒绝结果，
   再冻结前端与服务部署方式。
2. **单 World 可用性**：接通现有 core 的生命周期，补齐上述 POSIX、缓存与崩溃语义，
   用 Git、构建和编辑器工作负载验证。CLI 通过服务管理 store，不能继续直接写数据库。
3. **多 World 与安全验收**：会话撤销、fork/checkpoint 屏障、历史/GC、越界矩阵、
   性能测量与版本支持矩阵；通过后才设为 macOS 默认主线。
4. **存量迁移**：显式导入到新私有 store，停用旧 World 的所有写入者，验证内容、
   hardlink、Git/submodule/LFS 和历史引用后挂载新卷。旧数据保留供恢复，不自动删除。

迁移不是把原目录直接盖上挂载：旧 inode 的已打开 fd 仍可能有效。旧 store 应保持
独立，新的格式/所有权边界必须拒绝旧二进制写入；失败不得改坏旧数据。源项目仍存在
于宿主，但 agent 默认没有它的写权限。导出/publish 是可信控制面上的显式操作，
导出的普通目录不再承诺 forkfs 的隔离与历史保证。

## 8. 绕过验收矩阵

每项都要测试实际系统调用、错误码和受保护内容未改变，不能只检查生成的策略文本。

| 攻击/故障 | 必须观察到的结果 |
| --- | --- |
| 用绝对路径、别名、symlink、`..`、`openat` 访问 backing/store/源项目 | backing/store 不可访问，源项目不可写 |
| 预先存在的 backing hardlink、同 UID `0600` 文件、映像设备节点 | 任一名字或设备都不能读取私有数据；服务所有权独立于 agent |
| agent 自写二进制，exec/fork 后代、双重 fork、后台任务 | 限制继承，撤销后不遗留可写进程 |
| 继承或经 IPC 传入宿主 fd、目录 fd、管理 socket | 启动链不泄漏能力；代理拒绝能力转交 |
| 修改 mountpoint 父目录、卸载、强制卸载、挂载前后竞态 | 不能替换入口，不能写入下层普通目录 |
| 伪造 marker/World ID/调用者字段，执行旧 CLI 或 `--force` | 无法调用未授权控制面或打开私有库 |
| 跨 World hardlink/rename/clone，后台路径 symlink 竞态 | 不越出授权 World，不借服务权限访问主机 |
| 启动后新增 World、重挂载相同路径、复用旧能力或旧 fd | 不获得新 World 权限，不复活已撤销授权 |
| `mmap` 脏页、open-unlink、checkpoint 并发写入 | 明确一致性边界，无静默丢写或跨代写入 |
| 扩展/broker 崩溃、沙箱不可用、磁盘满、机器重启 | 启动拒绝或 I/O 明确失败；恢复不开放旁路 |
| 改宿主配置/hook/启动项，调用宿主服务代执行 | 越界拒绝；项目内代码后续执行仍在隔离内 |
| 正常 Git/build/test/编辑器与授权网络操作 | 可完成真实工作，未靠关闭保护通过 |

本矩阵通过前，README 和 status 不得声称已经提供防 agent 绕过的 macOS 文件系统。
