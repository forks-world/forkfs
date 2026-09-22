# BranchFS Mac-first 设计文档 v0.3

## 1. 产品目标

BranchFS 第一阶段只解决一个问题：

**在一台 Mac 上，让 10～1000 个 coding agents 从同一个 workspace 瞬间 fork 出独立可写工作区，并尽可能保持原生 APFS 的性能、缓存共享和存储效率。**

BranchFS 从一开始就属于 **World State Provider**，而不是用户直接面对的顶层产品。

用户心智应该是：

```text
World
├── FS
├── Database
├── Config
└── Future state providers
```

因此文件系统相关命令采用：

```text
world fs ...
```

而不是：

```text
clap fs ...
```

BranchFS 核心操作：

```text
checkpoint()
fork()
mount()
diff()
discard()
```

第一版不做：

```text
S3
distributed
remote cache
PostgreSQL branching
Redis
VM/container
cloud state
generic merge
```

成功标准：

```text
fork latency            < 10 ms p50
git/build workload      >= 90% native APFS
1000 idle branches      可承受
storage growth          ~ 实际 divergence
diff                    O(changes)
```

---

# 2. 核心架构判断

不要重新实现 APFS。

BranchFS 应该是：

```text
           Agent
             │
          macOS VFS
             │
           FSKit
             │
     Branch Namespace Layer
             │
       ┌─────┴─────┐
       │           │
   shared base   private
     APFS file   APFS clone
       │           │
       └─────┬─────┘
             │
            APFS
             │
       page cache / COW
             │
            NVMe
```

BranchFS 自己负责：

```text
branch DAG
namespace overlay
version resolution
whiteout
rename metadata
changed set
diff
GC
```

APFS 继续负责：

```text
physical blocks
extent COW
page cache
writeback
NVMe
crash-consistent filesystem
```

---

# 3. 为什么不用 APFS snapshot 直接解决

APFS 已经支持 snapshot 和 file cloning，但缺少我们需要的：

```text
writable_branch(snapshot)
```

这种完整、廉价、可递归的 writable filesystem branch abstraction。

APFS `clonefile()` 非常适合作为 BranchFS data COW：

```text
base/foo.dat = 100 GB

clonefile(base/foo.dat, branch/foo.dat)

write branch/foo.dat @ offset X, 8 KB
```

初始 clone 不复制全部文件内容，后续修改由 APFS extent/block COW 处理。

因此：

> **APFS 做 branching data plane；BranchFS 做 branching namespace。**

---

# 4. fork 时绝不能 clone 整棵目录树

错误：

```text
fork W0 → W1

walk 1,000,000 files
    ↓
clonefile() × 1,000,000
```

即使没有 data copy，metadata cost 也无法接受。

正确模型：

```text
W0 root = R0

fork W1:
    parent = W0
    overlay = empty
```

所以：

```text
1 GB workspace
100 GB workspace
1 TB workspace
```

理论上的 fork latency 应基本一致。

未修改文件：

```text
W1:/src/a.cc ─┐
W2:/src/a.cc ─┼──→ Base:/src/a.cc
W3:/src/a.cc ─┘
```

三个 World 使用同一个 APFS backing file。

这也是跨 World cache sharing 的基础。

---

# 5. 第一次写时才 clone

W2 修改：

```text
/src/a.cc
```

流程：

```text
resolve(W2, "/src/a.cc")
    ↓
BASE object

first writable open
    ↓
clonefile(
    base/src/a.cc,
    private/W2/<object-id>
)

publish overlay:
W2:/src/a.cc → PRIVATE object

write(private)
```

最终：

```text
W1 → Base a.cc
W2 → Private a.cc
W3 → Base a.cc
```

同时获得：

```text
namespace isolation
+
APFS block COW
+
base page-cache sharing
```

---

# 6. writable-open 是 COW boundary

不要等真正 `write()` 才 private 化。

以下操作都必须先确保 private backing：

```text
O_RDWR
O_WRONLY
truncate
mmap(PROT_WRITE)
```

所以：

```text
open readonly
    → shared BASE allowed

open writable
    → ensure_private()
    → return writable handle
```

即：

```text
writable open
=
lazy COW trigger
```

这大幅简化 mmap 和 page cache correctness。

---

# 7. FSKit 实现

第一版直接基于 Apple PassthroughFS 思路构造。

推荐实现：

```text
FSVolume.Handler
FSVolume.OpenCloseHandler
FSVolume.ReadWriteHandler
FSVolume.DataCacheHandler
```

之后按需要增加：

```text
XattrHandler
PreallocateHandler
SeekRegionHandler
...
```

FSKit frontend 与 BranchFS Core 必须严格分层。

---

# 8. 第一阶段不要依赖 physical extent offload

即使 FSKit 可以支持 kernel-offloaded I/O，也不要在 MVP 假设：

```text
APFS ordinary file
        ↓
可以稳定获取 physical extents
        ↓
重新暴露给 FSKit
```

第一阶段先采用：

```text
FSKit
 ↓
Branch resolver
 ↓
ordinary APFS backing file
```

然后测真实性能。

只有 frontend/data path 成为明确瓶颈后再考虑更低层的 offload。

---

# 9. Kernel Data Cache 很重要

BranchFS 第一版不要自己做 page cache。

优先依赖：

```text
macOS kernel cache
+
APFS page cache
```

对于 shared BASE object：

```text
BASE is immutable
```

因此非常适合 read caching。

对于 private backing：

```text
write-through
或
write-back
```

由 benchmark 决定。

第一版核心目标就是：

> **最大程度保留 APFS 原生缓存能力。**

---

# 10. Immutable Backing Object invariant

核心 invariant：

> 任何被多个 World 引用的 backing object 都不可再原地修改。

例如：

```text
W0 foo ─┐
        ├→ F1
W1 foo ─┘
```

此后 W0 修改 foo：

```text
clonefile(F1 → F2)

W0 foo → F2
W1 foo → F1
```

因此 fork 一个 World 实际意味着：

> parent 当前可被 child 观察到的 backing versions 进入 immutable/shared 状态。

---

# 11. 本地存储布局

建议：

```text
~/Library/Application Support/World/
```

例如：

```text
world/
├── fs/
│   ├── objects/
│   │   ├── 00/
│   │   ├── 01/
│   │   └── ...
│   │
│   ├── private/
│   │   ├── W001/
│   │   ├── W002/
│   │   └── ...
│   │
│   ├── metadata/
│   │   └── branchfs.db
│   │
│   └── staging/
│
└── worlds/
```

不要把：

```text
World path
```

直接等于：

```text
backing storage path
```

namespace 和 physical backing 必须分离。

---

# 12. Metadata Model

Prototype 直接使用 SQLite WAL mode。

核心结构：

```text
worlds(
    world_id,
    parent_world_id,
    generation,
    state,
    created_at
)

entries(
    world_id,
    parent_inode,
    name,
    logical_inode,
    operation
)

inodes(
    logical_inode,
    backing_object,
    type,
    mode,
    uid,
    gid,
    size,
    metadata_generation
)

changed(
    world_id,
    logical_inode,
    change_type
)

objects(
    object_id,
    backing_path,
    immutable,
    state
)
```

其中：

```text
operation =
    CREATE
    OVERRIDE
    WHITEOUT
    REDIRECT
```

第一版不要先造 metadata database。

---

# 13. Namespace identity

不要长期只使用 pathname identity。

正确抽象：

```text
Directory Entry
      ↓
Logical Inode
      ↓
Backing Object
      ↓
APFS File
```

这样才能正确处理：

```text
hardlink
rename
metadata-only modification
```

并让 namespace branching 与 backing data versioning 解耦。

---

# 14. Read Path

```text
lookup("/src/foo.cc", W10)
       ↓
W10 overlay
       ↓ miss
resolved namespace cache
       ↓ miss
ancestry resolution
       ↓
logical inode I81
       ↓
backing object O921
       ↓
APFS backing file
```

steady-state read 不允许每次：

```text
W100
 ↓
W99
 ↓
...
 ↓
W0
```

因此至少需要：

```text
ResolvedNameCache[
    world_id,
    directory_inode,
    name
] → logical_inode
```

以及：

```text
ResolvedBackingCache[
    world_id,
    logical_inode
] → object_id
```

---

# 15. Branch depth

允许：

```text
W0
├─ W1
│  ├─ W11
│  └─ W12
└─ W2
```

但 branch depth 不得线性进入 steady-state lookup latency。

策略：

```text
first lookup
    ↓
resolve ancestry
    ↓
cache

background
    ↓
flatten hot namespace
```

例如是否在：

```text
depth > 32
```

后触发 flatten，应由 benchmark 决定。

---

# 16. Readdir

这是 coding workload 最重要的 path 之一。

例如：

```text
git status
find
rg
IDE indexing
```

都会产生大量 directory traversal。

不能每次：

```text
W17 overlay
+
W12 overlay
+
W4 overlay
+
base
```

重新 merge。

需要：

```text
DirViewCache[
    world_id,
    logical_directory_inode,
    generation
]
```

内容为：

```text
name → logical inode
```

第一次构造，后续直接使用。

---

# 17. Delete

删除一个来自 parent 的文件：

```text
rm huge.dat
```

只写：

```text
WHITEOUT
```

不修改 base。

因此：

```text
delete 100 GB file
```

仍然应该近似 metadata operation。

World discard：

```text
mark world DISCARDED
return
```

private backing files 后台 GC。

---

# 18. Rename

rename 应尽可能 namespace-only：

```text
foo → bar
```

如果 foo 指向 shared BASE：

```text
bar → same logical inode
foo → removed
```

完全不 clone file data。

只有：

```text
write(bar)
```

以后才产生 private backing。

---

# 19. Metadata-only changes

例如：

```text
chmod
chown
xattr
mtime
```

不应立即 clone file contents。

Branch 可以维护：

```text
logical inode metadata override
```

但 backing object 继续共享。

只有真正 data mutation 时才 `clonefile()`。

---

# 20. fork 算法

理想：

```text
fork(W0):

BEGIN metadata transaction

W1 = allocateWorldID()

insert world(
    id=W1,
    parent=W0,
    generation=...
)

COMMIT

return W1
```

绝对不能：

```text
walk inode tree
clone files
increment millions of refcounts
```

Backing object 生命周期应依赖：

```text
root reachability
+
generation
+
background GC
```

而不是 fork 时 eager refcount walk。

---

# 21. Base Import

用户第一次：

```text
world fs init ~/src/project
```

MVP 不复制 repo。

直接把：

```text
~/src/project
```

作为 base backing directory。

但初始化后需要明确一个 invariant：

> Base 不能绕过 WorldFS 被直接修改。

后续 managed mode 可以把 backing 数据迁到 World storage area。

---

# 22. Mount Model

目标体验：

```text
~/worlds/W1/project
~/worlds/W2/project
~/worlds/W3/project
```

例如：

```bash
world fs init ~/project

world fs fork
# W123

world fs mount W123 ~/worlds/W123
```

然后：

```bash
cd ~/worlds/W123
codex
```

Agent 不需要 MCP，不需要理解 BranchFS。

它的：

```text
read_file
write_file
apply_patch
shell
```

全部天然进入正确 World。

---

# 23. CLI hierarchy

顶层 abstraction 是：

```text
world
```

不是：

```text
fs
```

所以 filesystem-specific API：

```bash
world fs init ~/project

world fs checkpoint

world fs fork
world fs fork --from W123

world fs list
world fs inspect W123

world fs mount W123 ~/worlds/W123

world fs diff W123

world fs discard W123
```

将来可以自然增加：

```text
world pg ...
world redis ...
world object ...
world config ...
```

以及更高层统一操作：

```text
world fork
world diff
world checkpoint
world discard
```

长期：

```text
world fork
```

会协调：

```text
world fs fork
world pg fork
world redis fork
...
```

所以 `world fs fork` 是 provider-level primitive，而 `world fork` 是最终用户层 primitive。

---

# 24. API 分层

内部应该明确两层。

## Provider API

```text
world.fs.fork()
world.fs.checkpoint()
world.fs.diff()
world.fs.discard()
```

## World API

未来：

```text
world.fork()
world.checkpoint()
world.diff()
world.discard()
```

World controller 调：

```text
FS provider
PG provider
其他 state providers
```

这样第一阶段只实现 FS，也不会把 API 做死。

---

# 25. Diff

每次 mutation 同时更新：

```text
changed(world_id, logical_inode, type)
```

因此：

```bash
world fs diff W123
```

直接得到：

```text
A tests/new_test.py
M src/foo.cc
D old/file.py
R src/a.cc → src/b.cc
M metadata scripts/run.sh
```

复杂度：

```text
O(changes)
```

而不是：

```text
O(workspace size)
```

未来：

```bash
world diff W123
```

则聚合：

```text
FS diff
+
PostgreSQL diff
+
Config diff
+
其他 state diff
```

---

# 26. Git 的定位

Git 不被 BranchFS 替代。

Git：

```text
source history
semantic diff
semantic merge
remote collaboration
```

BranchFS：

```text
live workspace state
untracked files
generated files
dependencies
local artifacts
temporary state
```

未来：

```text
world diff W1
```

可以表现为：

```text
Code / Git
  M src/foo.cc

Filesystem
  + build/output.bin
  + tmp/test.db
```

---

# 27. Crash Consistency

Private backing publish：

错误：

```text
metadata → private object
private object 尚未完成
```

正确：

```text
clonefile(base → temp)
       ↓
initialize private
       ↓
finalize backing path
       ↓
metadata transaction publish
```

Metadata 第一版使用 SQLite WAL。

Discard：

```text
mark DEAD
return
```

physical cleanup 后台完成。

---

# 28. 第一阶段 POSIX scope

必须支持：

```text
lookup/stat
readdir
read
create
writable open
write
truncate
unlink
rename
mkdir/rmdir
symlink
chmod/basic attrs
fsync
mmap correctness
```

可以后做：

```text
rare ACLs
rare xattrs
special devices
复杂 locking edge cases
极端 sparse-file semantics
```

---

# 29. 性能验证顺序

## M0 — FSKit passthrough

完全不做 branching。

```text
FSKit → ordinary APFS directory
```

benchmark：

```text
native APFS = 100%
FSKit passthrough = ?
```

跑：

```text
git status
rg
find
PostgreSQL compile
cargo check
npm/pnpm
pytest
```

如果长期：

```text
< 80–85% native
```

项目暂停。

先解决 frontend。

目标：

```text
>= 90%
```

---

# 30. M1 — Read-only Worlds

加入：

```text
World
parent
overlay
resolver
```

暂不允许写。

测试：

```text
1
10
100
1000
```

个 World。

验证：

```text
fork latency
metadata RAM
mount cost
read sharing
page-cache behavior
```

---

# 31. M2 — Lazy APFS COW

加入：

```text
writable open
    ↓
clonefile()
    ↓
private backing
```

核心 testcase：

```text
100 GB file

fork 100 worlds

每个 world:
random modify 8 KB
```

必须测：

```text
physical bytes written
first-write latency
storage footprint
APFS clone latency
```

不能出现 file-size 级物理 amplification。

---

# 32. M3 — Real Agent Workload

例如 PostgreSQL source tree：

```text
100 Worlds

每个：
修改 1–5 个文件
incremental compile
run tests
```

比较：

```text
native APFS
vs
BranchFS
```

目标：

```text
>= 90%
```

---

# 33. 五个最关键 benchmark

第一阶段只盯：

```text
1. git status

2. PostgreSQL incremental build

3. full source build

4. 100GB file / 8KB modification

5. 1000 sibling Worlds
   reading same source/dependencies
```

第五项尤其重要。

必须验证：

```text
1000 Worlds
```

不会导致：

```text
1000 × physical read
```

或：

```text
1000 × equivalent page-cache footprint
```

---

# 34. 推荐代码结构

```text
world/
├── fs/
│   ├── core/
│   │   ├── World.cpp
│   │   ├── BranchDag.cpp
│   │   ├── Namespace.cpp
│   │   ├── LogicalInode.cpp
│   │   ├── BackingObject.cpp
│   │   ├── Resolver.cpp
│   │   ├── ChangedSet.cpp
│   │   └── GC.cpp
│   │
│   ├── macos/
│   │   ├── fskit/
│   │   └── apfs/
│   │       └── Clone.cpp
│   │
│   ├── metadata/
│   │   └── SQLiteStore.cpp
│   │
│   └── bench/
│
├── cli/
│   └── world
│
└── future/
    ├── pg/
    ├── redis/
    └── object/
```

代码结构本身也体现：

> **World 是产品；FS 是第一个 provider。**

---

# 35. 第一个两周 Sprint

## Week 1

只做：

```text
FSKit Passthrough
+
current Handler API
+
kernel cache configuration
+
benchmark harness
```

得到真实数字：

```text
native APFS
vs
FSKit passthrough
```

不要做 branching。

## Week 2

增加：

```text
World ID
parent World
empty overlay
read-only resolver
```

支持：

```bash
world fs init ~/project
world fs fork
world fs mount W1 ~/worlds/W1
```

测：

```text
1 / 10 / 100 / 1000 Worlds
```

---

# 36. 第一个真正 milestone

不是：

> filesystem mount 成功。

而是：

```text
Base workspace:
  500K files
  100GB huge.dat

$ world fs fork --count 100

100 Worlds ready
near-constant creation time

W1 modifies huge.dat by 8KB
W2 modifies foo.cc
W3 renames directory

$ world fs diff W1

Physical additional storage
≈ actual APFS COW extents

Sibling shared reads
continue using same Base backing objects
```

这个成立，BranchFS thesis 才成立。

---

# 37. Go / No-Go

继续：

```text
FSKit passthrough >= 90% APFS

BranchFS read-only >= 95% passthrough

small mutation of huge file
does not cause whole-file copy

1000 fork
near O(1) metadata growth

shared sibling reads
do not cause near-linear physical I/O amplification
```

否则优先修架构，而不是继续堆功能。

---

# 38. v1 最终形态

```text
                  Agent Process
                       │
              /worlds/W123/project
                       │
                    FSKit
                       │
               Branch Resolver
                  /         \
                 /           \
         shared BASE       PRIVATE
            file             file
             │                │
             └────── APFS ────┘
                       │
                 block-level COW
                       │
                  page cache
                       │
                     NVMe
```

用户体验：

```bash
world fs init ~/project

world fs fork
# W123

world fs mount W123 ~/worlds/W123

cd ~/worlds/W123
codex
```

而最终 World 产品则会进一步简化成：

```bash
world fork
world exec W123 -- codex
world diff W123
world discard W123
```

其中 `world fs fork` 是底层 provider primitive，**真正长期对用户暴露的核心 verb 应该是 `world fork`。**


---

# 39. 工程约束:最小依赖

原则:**core 能在任何平台、被任何语言的前端直接链接;不引入第三方运行时。**

`core/` 的依赖口径(2026-09-18 决定,"先允许、后续裁剪"):

```text
允许:
    libc / POSIX(syscall 封装在 platform 层)
    标准 C++ 运行时(macOS libc++ 是系统库;Linux 静态链接 libstdc++;Windows /MT)
    SQLite(C 库,单文件 amalgamation,可 vendor)
    header-only 库,以 git submodule vendor 在 third_party/:
        smallstring   字符串(替代 std::string)
        Containa      容器(btree_map / dense_map / small_vectra …)
        Arena         bump allocator,需要批量分配的地方用
        fmt           仅因上面几个库需要,FMT_HEADER_ONLY

禁止:
    std::string / std::unordered_map / std::mutex / iostream / std::function
    任何需要单独链接的第三方库(boost、abseil、grpc …)
    静态对象的动态初始化
```

编译约束:

```text
-std=c++23,异常与 RTTI 开启(smallstring / Containa / Arena 需要)
core 自身代码不 throw,错误用负 errno 返回
```

对外只暴露 **C ABI**(`core/include/worldfs/worldfs.h`),Objective-C(FSKit)、
C(FUSE)、Rust/Go(cgo/FFI)前端都能直接调用。

前端:

```text
macos/fskit/   Objective-C++,只在这里接触 FSKit
cli/           C 风格 C++
```

验收:`scripts/check-deps.sh` 对每个产物跑 `otool -L` / `ldd`,
只允许系统库(/usr/lib、/System/Library)出现。

后续裁剪方向:三件套增加无 fmt / 无 pmr / 无异常的 lean 模式后,再评估 `-nostdlib++`。

---

# 40. 实测修正与 M1 转向(2026-09-19)

本章记录 M0 的测量结论,以及据此对 §2–§8 的修正。§1 的产品目标和成功标准不变。

## 40.1 FSKit 不能做 BranchFS 的命名空间层

在 macOS 26.6.2 与 27.0 上实测(`docs/MACOS27_MEASUREMENTS.md`、`docs/PERF_STUDY_RT_PARALLEL.md`、`docs/FSKIT_HANDLER_API_MACOS27.md`):

```text
单次内核→扩展往返            66–75µs(Apple 自带 msdos 模块 72–78µs,我们的代码占 ≤2%)
create+unlink 的往返次数     16(旧 API)/ 11(27 Handler API)
元数据写密集负载            native 的 6–20%
读 / exec / 页缓存命中      native 的 90–110%
单个挂载                    fskitd 一条 serial 队列;8 个活跃挂载时 fskitd 独占 2 核
```

往返次数由内核决定(每个未缓存名字两次 lookup、provenance xattr 探测、父目录 getattr),
Handler API 只去掉了最便宜的几次。§29 的 "≥ 90%" 在任何 mutation 上都不可能达到。
FSKit 前端保留为实验性备选(超大仓库、跨卷),不再投入。

## 40.2 转向:APFS 做命名空间和数据,BranchFS 只记分支 DAG

§2 的分工改为:

```text
BranchFS 自己负责:  branch DAG、快照生命周期、fork/checkpoint/discard/diff、预克隆池、防误操作
APFS 负责:          命名空间、COW、页缓存、崩溃一致性(§10–§19 的 overlay/whiteout/override 全部不再需要)
```

World 是 APFS 目录的 `clonefile(dir)` 克隆;Snapshot 是不可变克隆(门目录 0000)。
§4 "fork 绝不能走整棵树"被放弃:目录克隆实测 7–13µs/条目,50k 文件 0.37–0.5s;
fork < 10ms 由预克隆池实现(命中 p50 8.6–9.7ms,与树大小无关)。
§25 的 O(changes) diff 由 FSEvents 提供候选,但默认走 4 线程全扫(0.16–1.4s/50k),
因为 fseventsd 落日志不保证全局有序,精确性优先。

## 40.3 M1 对 §1 判据的实测(`docs/M1_RESULTS.md`)

```text
fork latency < 10ms p50        PASS  池命中 8.6–9.7ms;未命中 26ms/1k、110ms/10k、505ms/50k
git/build >= 90% native        PASS  99–117%
1000 idle branches             PASS  102–114s 创建,3.2–3.4GB 元数据,list 10ms;删除 525s(M2:后台增量 gc)
storage ~ divergence           PASS  100 World 各改 1% = 真实复制的 13.6%;每次 append 摊 4KiB 块
diff O(changes)                PASS  全扫 0.19–1.4s/50k;事件路径 0.35s 但抖动 0.14–0.49s
```

## 40.4 防误操作(`docs/M1_DESIGN.md` §3,P1–P15)

身份靠标记文件 + inode 而非路径;快照门目录保护;discard 进 trash 可 restore;`world exec` 持锁 + seatbelt;
跨卷探针;危险路径拒绝;tmp→rename→commit 的发布顺序;93 项安全用例。

## 40.5 其他平台

Linux:先落地 XFS 原生目录后端（`FICLONE` 逐文件 reflink + 现有生命周期/预克隆池），
普通 fork 为 O(entries)，diff 全量扫描；实测和边界见 `docs/LINUX_XFS.md`。
执行安全独立于存储：`world exec` 默认由系统 Bubblewrap 建立 user/mount/PID/IPC/UTS namespace，
宿主只读、当前 World 可写、store 隐藏，配合降权、seccomp 和继承 fd 清理；隔离失败拒绝启动。
Bubblewrap 是 CLI 的 Linux 沙箱工具依赖（类似 macOS 的 sandbox-exec），不进入 core 的链接依赖。
后续再实现 overlayfs（fork O(1)、upper 目录提供 changed-set）并验证 btrfs 等后端。
Windows:Windows 11 + Dev Drive(ReFS 块克隆逐文件)+ USN Journal;或 ProjFS 惰性投影。core 的 C ABI 不变,平台层各自实现。
