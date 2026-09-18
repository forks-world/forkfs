# REDIRECT 实验:把写密集目录移出 FSKit 是否值得做

测量日期:2026-09-18 夜 / 2026-09-19。机器:Mac mini M1(8 核 / 16GB),macOS 26.6.2,APFS
`/dev/disk3s5`,机器空闲(`top` 全部 0.0% CPU;唯一的已知干扰源是 Spotlight 对新建目录的索引,
下文标出了受影响的样本)。挂载点 `mnt14`(backing = `m0/base`),扩展为当前已安装版本,**未重新构建**。
所有 native 目录在 `$S/nat/` 下,既不在挂载点里也不在 backing 里(脚本带 guard 会拒绝跑)。全程无网络。

脚本(本次新增,不改 `core/` 与 `macos/`):

| 脚本 | 覆盖 |
|---|---|
| `scripts/bench/redirect_exp.sh` | E1 — S4 / S5 / S7 / S2 × {native, mount, mount+redirect} |
| `scripts/bench/redirect_build.sh` | E2 — fmt 的 cmake configure / build / 增量 / 删除 × 三配置 |
| `scripts/bench/redirect_semantics.sh` | E3 — go / python / node / git / clang / 遍历器 / 安全性 语义验证 |
| `scripts/bench/clone_cost.sh` | E4 + E6a/b/d/e — clonefile 成本、存储、changed-set、卷约束 |
| `scripts/bench/clone_world.sh` | E6a(真 repo) + E6c + E6d(git) — native-root World |

## 0. 背景与本实验的一个前置结论

M0 的结论是:读 / exec / 编译 = native 的 90–110%,而元数据写密集 = 9–20%,原因是内核对每个
create/unlink/rename 发 5–7 次 XPC(每次 ~50–90µs),扩展自身只花 µs。arch.md §12 的
`entries.operation` 里已经预留了 `REDIRECT`。

**前置结论(很重要,决定了 REDIRECT 该怎么实现):**
本实验用「挂载点内的一个 symlink 指向 native APFS 目录」来代表 REDIRECT。之所以能提速,
**完全是因为内核在这个名字上离开了我们的文件系统**——之后的 create/write/unlink 根本不产生 XPC。
如果 REDIRECT 被实现成「`entries.operation=REDIRECT`,由扩展在内部把路径重定向到另一个 backing 路径」,
那么 IO 仍然经过 FSKit,**5–7 次 XPC 一次都不会少,本文档的全部加速都不存在**。
换言之:REDIRECT 必须对内核可见(symlink,或未来某种 submount),不能只是 core 内部的一个 entry 类型。

---

## 1. E1 — REDIRECT 对写密集场景的效果

配置:(a) native `$S/nat/e1`;(b) mount `$S/m0/mnt14/e1`,无重定向;(c) mount+redirect
`$S/m0/mnt14/e1r`,其中产物目录(`target/` / `node_modules/` / `tmp/`)是指向 `$S/nat/e1r-target/…`
的 symlink。场景代码从 `agentstress.sh` 抄来,**唯一改动**:删除阶段删的是产物目录的*内容*而不是目录本身
(`rm -rf` 一个 symlink 只会删掉链接),这样三个配置删的是同一批文件。

每个配置至少跑 2 次,取更优者;S4/S5/S7 做了两轮独立测量(A、B),两轮都列出。

### 1.1 结果表(秒,best-of-2)

| 场景 | 轮次 | native | mount | mount+redirect | native%(mount) | native%(redirect) | redirect/mount |
|---|---|---|---|---|---|---|---|
| **S4** build-artifacts(5000 个 .o 写入 `target/` 再删除) | A | 1.207 | 6.544 | 1.264 | **18%** | **95%** | 5.18× |
| | B | 1.222 | 6.590 | 1.251 | **19%** | **98%** | 5.27× |
| **S5** install-tree(tar 解 4000 文件到 `node_modules/` + 1000 hardlink + 1000 `cp -c` + 删除) | A | 3.899 | 49.632 | 3.957 | **8%** | **99%** | 12.54× |
| | B | 4.642 | 41.686 | 4.824 | **11%** | **96%** | 8.64× |
| **S7** test-churn(200 次进程 spawn,各写/读/删 20 个 `tmp/` 文件) | A | 6.177 | 10.079 | 6.654 | **61%** | **93%** | 1.51× |
| | B | 7.417 | 12.433 | 7.693 | **60%** | **96%** | 1.62× |
| **S2** edit-loop(改的是 *源文件*,对照组,不重定向) | 4 reps | 0.148 | 0.800 | 1.182 | **18%** | **13%** | 0.68× |

全部配置 correctness = ok(S4 检查 `target/debug` 已删除;S5 检查 `node_modules/pkgsrc` 已删除;
S2 检查最后一次编辑的内容;S7 检查目录仍在)。

原始多次采样(秒):

```
s4  native  1.2071 1.2346 | 1.2216 1.2250      mount  6.8048 6.5437 | 6.7993 6.5896      redirect 1.2643 1.3619 | 1.2507 1.2610
s5  native  4.0247 3.8994 | 4.6418 4.7052      mount 53.2130 49.6322 | 41.6859 45.9691   redirect 4.0176 3.9568 | 11.6260 4.8241
s7  native  6.2739 6.1766 | 7.4173 7.4224      mount 10.0794 14.9014 | 12.4910 12.4331   redirect 6.6600 6.6535 | 7.6933 7.7281
s2  native  0.1478 0.1552 0.1507 0.1510        mount 0.7998 0.8785 0.9868 1.0314         redirect 1.1816 1.4730 1.3190 1.7330
```

### 1.2 读法

- **S4 / S5 几乎全额恢复**:S4 从 18–19% 提到 95–98%,S5 从 8–11% 提到 96–99%。
  按"损失的绝对时间"算更直观:S5 在挂载点上多花 45.7s,重定向之后只剩 0.06s,**去掉了 99.9% 的开销**;
  S4 多花 5.33s,重定向后剩 0.06s,**去掉 98.9%**。
- **S7 只恢复一部分**:60–61% → 93–96%。剩下的 4–7% 是 200 次进程 spawn 本身:
  子进程的 cwd 在挂载点上,`fork/exec` 要解析挂载点路径、读 cwd 的属性,这部分 XPC 重定向拿不掉。
- **S2 对照组按预期不受益**:18% → 13%(redirect 甚至略慢,属于抖动;它和 mount 配置在 S2 里做的是
  完全相同的事,只是目录不同)。**这条是整个提案最关键的负面结果**:agent 最高频的动作——
  apply_patch / write_file 写源文件——一点也没被改善。
- 注:E1 第一轮里 S2 跑在最后,拿到了 6%/11% 的离群值;上表用的是单独重跑 4 次的结果(18%/13%),
  与 `docs/TASKS.md` 里记录的 17% 一致。

---

## 2. E2 — 真实构建(third_party/fmt)带 REDIRECT

与 `realwork.sh` 同样的技巧:`CXXFLAGS="-nostdinc++ -isystem $(xcrun --show-sdk-path)/usr/include/c++/v1"`、
`-DFMT_MODULE=OFF -DFMT_DOC=OFF -DCMAKE_BUILD_TYPE=Release`。`build/` 在三种位置:native / 挂载点 / symlink 到 native。
删除阶段同样删 `build/` 的内容而不是目录。best-of-2。

### 2.1 `FMT_TEST=OFF`(只编 libfmt,4 个 TU,CPU 主导)

| 步骤 | native | mount | mount+redirect | native%(mount) | native%(redirect) |
|---|---|---|---|---|---|
| cmake configure | 0.565 | 0.781 | 0.511 | 72% | 111% |
| cmake build -j8 | 1.798 | 1.701 | 1.550 | 106% | 116% |
| touch 头文件 + 增量编译 | 0.347 | 0.452 | 0.350 | 77% | 99% |
| rm -rf build 内容 | 0.030 | 0.084 | 0.030 | 36% | 100% |

### 2.2 `FMT_TEST=ON`(连 gtest 一起编,约 20 个测试二进制,产物多得多)

| 步骤 | native | mount | mount+redirect | native%(mount) | native%(redirect) |
|---|---|---|---|---|---|
| cmake configure | 1.112 | 2.029 | 1.119 | 55% | **99%** |
| cmake build -j8 | 21.039 | 23.785 | 24.063 | 88% | 87% |
| touch 头文件 + 增量编译 | 15.403 | 18.485 | 17.439 | 83% | 88% |
| rm -rf build 内容 | 0.066 | 0.287 | 0.068 | 23% | **97%** |

原始采样:

```
FMT_TEST=OFF   native 0.565/1.859/0.427/0.036 与 0.567/1.798/0.347/0.030
               mount  0.784/1.712/0.452/0.087 与 0.781/1.701/1.539/0.084
               redir  0.511/1.550/0.406/0.030 与 0.513/1.554/0.350/0.030
FMT_TEST=ON    native 1.211/23.639/15.403/0.066 与 1.112/21.039/15.579/0.068
               mount  2.029/24.057/18.485/0.287 与 2.076/23.785/19.528/0.298
               redir  1.119/24.063/17.439/0.095 与 1.159/24.426/18.147/0.068
```

### 2.3 读法

- **configure 和 rm -rf build 是纯元数据写,重定向全额恢复**(55%→99%,23%→97%)。
  cmake configure 会做十几次 try-compile,每次在 `build/CMakeFiles/` 里建一棵小目录树再删掉,
  正是 5–7 次 XPC/op 最痛的形状。
- **编译本身在 `-j8` 下是 CPU 饱和的,重定向无效**(87–88%,和不重定向的 88% 一样)。
  M0 报告里 "cmake build 90% native" 的结论本来就说明编译不是 IO 瓶颈。
- **增量编译只恢复 1/3**:18.485 → 17.439,离 native 的 15.403 还差 2.0s。原因是它读的是
  挂载点上的源文件/头文件(冷 lookup ~108µs/次),写产物那部分已经被重定向拿走了。
  这与 E1-S2 的结论一致:**源文件侧的开销重定向碰不到**。

---

## 3. E3 — 符号链接目录下的工具链语义(纯正确性,39 项检查)

`scripts/bench/redirect_semantics.sh` 输出 **39 passed / 0 failed**。以下逐条是实际跑出来的,不是推断。

### 3.1 通过的

| 工具 | 检查 | 结果 |
|---|---|---|
| go 1.27.1 | `go build -o target/bin/t1`,`target/` 是 symlink,`GOCACHE`/`GOTMPDIR`/`GOMODCACHE` 都在里面 | 通过;产物落在 native 侧,`GOCACHE` 生成 1066 个文件 |
| go | 产物二进制经 symlink 执行 | `sum 5`,通过(无 quarantine 问题,因为它是 native 文件) |
| go | `go test ./...` 两次(冷/热 cache) | 都通过 |
| python 3.14 | venv 的 python 从 symlink 下运行、import 挂载点上的模块、`pip --version` | 通过 |
| python | `__pycache__/` symlink 到 native,`compileall -f` | 通过,3 个 `.pyc` 落在 native 侧;之后 import 走重定向的 pycache,通过 |
| node 25.9 | `require("leftpad")`,包在 symlink 的 `node_modules/` 下(本地手造包,无 npm) | 通过 |
| node | ESM `import`(`exports` 字段) | 通过 |
| git 2.50.1 | `.git` 是 symlink → native:init / add / commit / status / checkout -b / diff / stash / stash pop / commit | 通过,`work` 分支上确实 2 个 commit,对象(12 个 loose object)在 native 侧 |
| git | `fsck` / `gc --aggressive` / gc 后 `log` | 通过 |
| git | `git init --separate-git-dir <native>`(gitfile,`.git` 是普通文本文件) | 通过,同样的完整循环,2 个 commit,`fsck` 干净,`worktree list` 正常,index 在 native 侧 |
| clang | 编译 + 链接,`build/` 是 symlink | 通过;产物可执行、dylib 可 `dlopen`;**产物无 `com.apple.quarantine`**(它压根没经过 FSKit) |
| 安全性 | `rm -rf proj/`(proj 里含指向 native 的 symlink) | symlink 被删,native 目标 3/3 文件完好 |
| 安全性 | `rm -rf target`(直接删 symlink 本身) | 只删链接,native 内容完好 |
| 跨界 | `mv` 从挂载点移入重定向目录 | 通过(退化成 copy+unlink) |
| 跨界 | `cp`(普通复制)跨界 | 通过 |
| 界内 | 重定向目录内部的 `mv` | 通过(native 原子 rename) |

### 3.2 **发现的语义阻断:`python -m venv` 拒绝 symlink 的 `.venv`**

```
$ cd <workspace>/t2 && python3 -m venv .venv
Error: Unable to create directory '<workspace>/t2/.venv'
```

根因在 CPython 自己:

```python
# venv.EnvBuilder.ensure_directories
def create_if_needed(d):
    if not os.path.exists(d):
        os.makedirs(d)
    elif os.path.islink(d) or os.path.isfile(d):
        raise ValueError('Unable to create directory %r' % d)
```

**在纯 APFS 上完全复现**(`$S/nat/e1-venvchk`),所以不是文件系统 bug,而是 symlink 方案的固有后果。
凡是做 `islink()` 前置检查的工具都会踩到。

绕法(已验证可用):先在 native 路径上 `python -m venv <native>/venv`,再 `ln -s` 进工作区。
之后 `.venv/bin/python -V`、import 挂载点上的模块、`pip --version` 全部正常。
代价:`sys.prefix` 变成 native 的绝对路径(见 3.4)。

### 3.3 遍历器:默认**不**进入 symlink 目录

工作区:`src/a.c` 1 个文件 + `target` → native(50 个文件)。在挂载点和纯 APFS 上结果完全一致:

| 命令 | 命中/条目数 | 是否下钻 |
|---|---|---|
| `find .` | 4 | 否(symlink 只算 1 个条目) |
| `find -L .` | 55 | 是 |
| `/usr/bin/grep -rl` | 1 | 否 |
| `/usr/bin/grep -Rl` | 1 | 否(BSD grep 的 `-R` 默认 `-p`,不跟随) |
| `/usr/bin/grep -R -S -l` | 51 | 是(需要显式 `-S`) |
| ugrep / rg 风格 `-r` | 1 | 否 |
| ugrep / rg 风格 `-R` | 51 | 是 |
| `python os.walk()` | 1 | 否 |
| `os.walk(followlinks=True)` | 51 | 是 |
| `du -sk` | 4k(只有工作区) | 否 |

**对 agent 的含义**:好的一面,`rg`/`grep`/`find` 搜索工作区时自动跳过 `target/`、`node_modules/`,
和 `.gitignore` 的默认行为一致,不会被几万个产物淹没;坏的一面,"找一下我刚生成的 .o / 刚装的包里的
某个文件"会搜不到,agent 必须知道要加 `-L`(find)/`-S`(BSD grep)/`-L`(rg),或者直接走 native 路径。
`du -sk` 也不再反映工作区真实占用。

### 3.4 其他跨界语义(都实测)

| 行为 | 结果 |
|---|---|
| `ln`(硬链接)跨界 | **失败**:`Cross-device link`(EXDEV) |
| `cp -c`(clonefile)跨界 | 未报错,静默退化成真实复制 |
| `stat -f %d` 两侧 | 不同:工作区 `805306396`,重定向侧 `16777229` |
| `df` 重定向目录 | 报 `/dev/disk3s5 → /System/Volumes/Data`,不是挂载点 |
| `require.resolve("leftpad")` | 返回 **native 绝对路径**,不是工作区路径 |
| venv `sys.prefix` | 返回 **native 绝对路径** |
| `git rev-parse --show-toplevel` | 仍然是挂载点路径(好),`--git-dir` = `.git`(好) |
| `git init --separate-git-dir` 的 `.git` | 普通文本文件 `gitdir: <native path>` |
| `rm -rf` 工作区后 | native 产物树**留下不删** |

其中三条值得单独强调:

1. **EXDEV 会打断 pnpm / uv / npm 的安装策略**。它们从全局 store 用 hardlink 或 clonefile 铺 `node_modules/`。
   如果 store 在挂载点(或用户 home)而 `node_modules/` 被重定向,hardlink 直接失败、clonefile 静默退化成复制,
   安装会变慢并且不再共享物理块。修法:把全局 store 和重定向目录放在同一个 native 根下。
   注意 S5 里 `cp -c` 是**在重定向目录内部**做的,所以照常走 clonefile,这也是 S5 达到 96–99% 的原因之一。
2. **绝对路径泄漏出 World**。`require.resolve` / `sys.prefix` / `realpath` / 各种构建缓存(cmake 的
   `CMakeCache.txt`、go 的 build cache key、`.pyc` 里的 `co_filename`)会记下 native 路径。
   这意味着 World 内部的产物**不可整体搬迁**,而且"World 的全部状态都在 World 里"这个不变式被破坏了。
3. **产物树会泄漏**。`rm -rf proj/` 之后 native 侧的 `node_modules` 原封不动。真实的 REDIRECT 实现必须
   在 WHITEOUT/删除时把重定向目标一起 GC 掉,否则用户每删一次工程就漏一份 node_modules。

---

## 4. E4 — fork 成本模型(APFS clonefile)

`$S/nat/big`:20000 个文件(1–8KB 混合),201 个目录,`du -sk` = 125268k(约 122MB)。

| 方式 | 时间(2 次) | µs/file | 备注 |
|---|---|---|---|
| `clonefile()` 整目录(一次调用,内核递归) | **0.2002 / 0.1988 s** | **9.9** | 通过 python ctypes 调 `libc.clonefile(src, dst, 0)` |
| `cp -c -R` | 4.4863 / 5.3491 s | 224–268 | 比 clonefile 慢 **22–27×** |
| `cp -R`(真实复制) | 14.0427 / 10.1107 s | 506–702 | 比 clonefile 慢 **51–70×** |

`du -sk`:原始 125268k,clonefile 克隆 125268k,`cp -c -R` 125268k,`cp -R` 125268k ——
**`du` 不反映块共享**(它数的是逻辑块),要看真实占用必须看 `df` 的可用空间变化(见 E6b)。

单个 2GB 文件:

| 操作 | 时间 |
|---|---|
| `clonefile()` 2GB 文件(第 1 次 / 第 2 次) | 0.0899 s / **0.0001 s** |
| `cp` 真实复制 2GB | 1.7775 s |

独立的 20 次中位数测量显示:**单文件 clonefile 的延迟与文件大小无关**——

| 文件大小 | clonefile 中位数 | 最小 |
|---|---|---|
| 4 KiB | 187.6 µs | 172.0 µs |
| 64 KiB | 183.0 µs | 173.0 µs |
| 1 MiB | 178.1 µs | 167.4 µs |
| 64 MiB | 195.1 µs | 174.6 µs |

目录粒度的 clonefile(5 次中位数):

| 目录内文件数 | clonefile | µs/file |
|---|---|---|
| 1 | 0.250 ms | 250.1 |
| 10 | 0.343 ms | 34.3 |
| 100 | 2.090 ms | 20.9 |
| 1000 | 16.624 ms | 16.6 |
| 10000 | 136.431 ms | 13.6 |

COW 首写代价(64MiB 文件,10 次中位数,双方都先 fsync 过):

| 目标 | 第 1 次 8KiB `pwrite`+`fsync` | 第 2 次 |
|---|---|---|
| 普通文件(非克隆) | 47.9 µs | 41.1 µs |
| 克隆出来的文件(首写 = unshare) | **439.6 µs** | 34.3 µs |

→ **每个被写的克隆文件一次性多付约 390 µs**,之后恢复正常。

### 4.1 对 fork < 10ms 目标的结论

| 方案 | 成本 | 是否满足 arch.md 的 fork < 10ms |
|---|---|---|
| fork 时**急切**克隆 3 个 20k 文件的重定向目录 | 3 × 0.199 s ≈ **0.60 s** | **差 60 倍,不满足** |
| fork 时急切克隆,想卡进 10ms | 10ms ÷ 10–13µs/file ≈ **最多 ~800–1000 个文件** | 只有极小的 `target/` 能这么干 |
| fork 时只建空的重定向目录,**懒克隆** | fork 本身 ~µs(只写 metadata) | **满足** |
| 懒克隆,**目录粒度**(首次写进某个子目录时克隆该目录) | 200 文件/目录 ≈ **2–4 ms** 一次 | 首写抖动可接受 |
| 懒克隆,**文件粒度** | 每文件 ~**180 µs** clonefile,+ 首写 ~390 µs COW unshare | 每个首次被写的文件 ~0.6 ms |
| 完全不克隆(重定向目录 fork 后为空) | 0 | 但 `node_modules/` / `target/` 要重新安装/重新构建,不可接受 |

**推荐读数**:eager 不可行;lazy-clone-on-first-write 可行,目录粒度(2–4ms)比文件粒度(0.6ms×N)
在 node_modules 这种"整包读、少量写"的形状上更划算,因为 agent 通常只写少数几个目录。

---

## 5. E6 — "native-root World":根本不挂 FSKit

思路:World = 把 base 树用 `clonefile()` 递归克隆成一个普通 native 目录,agent 之后做的一切都是纯 APFS。
arch.md §4 拒绝 fork 时 walk 百万文件,但典型仓库只有 1 万–10 万文件,值得测一下到底多贵。

### 5.1 E6a — fork 延迟 vs 规模

| 树 | 条目数 | `clonefile` best-of-2 | µs/file | `cp -c -R` best-of-2 | µs/file |
|---|---|---|---|---|---|
| synthetic 1k(1–8KB,200/目录) | 1000 | **0.0097 s** | 9.7 | 0.2687 s | 268.7 |
| synthetic 10k | 10000 | **0.1290 s** | 12.9 | 1.7169 s | 171.7 |
| synthetic 50k | 50000 | **0.4812 s** | 9.6 | 8.6263 s | 172.5 |
| third_party/fmt(submodule,`.git` 是 gitfile,171 条目) | 171 | 0.0022 s | 12.9 | 0.0781 s | 456.7 |
| 真 repo:forkfs + fmt 源码,**含完整 `.git`**(516 条目,其中 `.git` 346;du 5020k) | 516 | 0.0067 / 0.0105 s | 13.0–20.3 | — | — |
| 50k 文件的 git 仓库(含 `.git`) | ~50k | 0.88 / 1.10 s | ~19 | — | — |

**clonefile 大约 10–20 µs/条目,且与文件大小无关**;比 `cp -c -R` 快 13–35 倍。
克隆出来的 repo `git status` / `git log` 都正常(实测)。

| 仓库规模 | fork 延迟(实测/外推) | vs arch.md 的 fork < 10ms |
|---|---|---|
| 1k 文件 | 0.010 s | 刚好卡线 |
| 10k 文件 | 0.129 s | **超 13×** |
| 50k 文件 | 0.481 s | **超 48×** |
| 50k 文件 + .git | 0.88 s | **超 88×** |

所以 native-root World **做不到 fork < 10ms**,但做得到 **"10万文件的仓库 1 秒内 fork 完"**。
要不要接受这个,取决于产品上 fork 是"按一次键就出一个 World"还是"开一个新 agent 会话"。

### 5.2 E6b — 存储:克隆后 `du`、改 1% 之后的增长、COW 隔离

50000 文件树(`du -sk` 313824k ≈ 306MB):

| 指标 | 值 |
|---|---|
| 原始 `du -sk` | 313824k |
| 克隆刚做完 `du -sk` | 313824k(`du` 数逻辑块,看不出共享) |
| 改动:500 个文件(1%)各 append 100 字节 | 新数据共 **48.8 KiB** |
| `df` 可用空间消耗 | **1112k ≈ 1.1 MB** |
| 改动后克隆的 `du -sk` | 313920k(+96k) |

即:**改 1% 的文件,真实物理增长 1.1MB / 306MB = 0.36%**;
每个被改文件摊到 ~2.2KB(一个 4KiB 块级的 unshare + 元数据),与 APFS 的块粒度吻合。

COW 隔离(必须成立,否则整个方案作废):

| 检查 | 结果 |
|---|---|
| 200 个未改动文件与原树逐字节 `filecmp` | **0 处不一致** |
| 被改动文件对应的**原文件**尺寸是否也变了 | **0 个**(必须是 0) |
| 共享文件的 inode | 原树 125087708,克隆 125196993 —— **不同 inode,共享 extent** |

### 5.3 E6c — 克隆里跑 agent 负载(应当 ≈ native)

同一轮里背靠背跑"克隆根 World" 与"普通 `cp -R` 出来的 native 副本",避免跨时段抖动。
两轮,best-of-2:

| 配置 | S2 edit-loop | S4 build-artifacts | configure | build -j8 | 增量编译 | rm -rf build |
|---|---|---|---|---|---|---|
| clone-root World | 0.1949 | 1.5217 | 0.6616 | 2.0203 | 0.4189 | 0.0356 |
| 普通 native 副本 | 0.1901 | 1.4941 | 0.6281 | 1.9967 | 0.4326 | 0.0392 |
| **native%** | **98%** | **98%** | **95%** | **99%** | **103%** | **110%** |

fork 本身:691 条目的工作区,`clonefile` 0.094 s vs `cp -R` 0.271 s。
Base 完整性检查通过(base 里 0 个残留 `.o`,`file049.c` 里 158 行未被改动的标记行)。

→ **克隆里的速度就是 native 速度**,没有任何 COW 惩罚可见(S4 写的是新文件,不触发 unshare;
S2 改的 500 个文件各付一次 ~390µs unshare,在 0.19s 的总量里被 read+write+fsync 淹没)。

原始采样里有两个受 Spotlight 索引影响的离群值(rep1 的 native 副本 build-j8 = 4.26s,
rep2 的 clone S2 = 1.24s),故取 best-of-2。

### 5.4 E6d — 没有 namespace 层时怎么算 changed-set

50000 文件的树,克隆后做了 500 modified / 200 added / 100 deleted:

| 方法 | 时间(2 次) | 结果 |
|---|---|---|
| stat-walk 双树对比(`os.scandir`,比 size/mtime_ns/ino/ctime_ns) | **0.957 / 0.776 s** | +200 −100 ~998 |
| 只单边 walk 克隆树(不比对) | 0.365 / 0.367 s | 50100 条目 |
| `git status --porcelain`(50k 文件的 git 仓库,800 处改动) | **0.191 / 0.149 s** | 800 paths |
| `git status --porcelain -uno` | 0.145 / 0.112 s | 600 paths |

即 **`world fs diff` 如果退化成 stat-walk,5 万文件要 ~0.8s;git 自己的 index 能做到 ~0.15s**。
arch.md §25 要求 diff 是 **O(changes)**,这两种都是 O(tree),**差一个数量级的复杂度级别**。

FSEvents 选项(未实现,按要求只记录):`/System/Volumes/Data/.fseventsd` 存在(需要权限才能列目录),
fseventsd 对每个 APFS 卷都记流水。native-root World 可以在 fork 时记下当时的 event id,之后用
`FSEventStreamCreate(sinceWhen=<fork event id>)` 直接拿到变更路径集合,**把 O(tree) 的 walk 变回 O(changes)**。
代价:FSEvents 是目录粒度(除非 `kFSEventStreamCreateFlagFileEvents`)、会丢事件(需要 `kFSEventStreamEventIdSinceNow`
之外的兜底全扫)、且是异步的,不能当作事务性的 changed 集合。`fs_usage` 需要 root,这里没验证事件完整性。

### 5.5 E6e — clonefile 的卷约束

| 检查 | 结果 |
|---|---|
| `$S`(工作根)的 `st_dev` | 16777229 |
| `~/Library/Containers`(扩展 container,metadata store 所在)的 `st_dev` | 16777229 |
| 两者 `df` | 都是 `/dev/disk3s5 → /System/Volumes/Data`,APFS Container `disk3`,卷名 `Data` |
| 结论 | **同一个 APFS 卷,clonefile 可用** |

跨卷行为(没能在无 root 的情况下挂一个第二 APFS 卷,改用系统只读卷做真实验证):

```
clonefile("/usr/bin/true", "$S/nat/xvol_probe")                       -> errno 18  Cross-device link
clonefile("/System/Library/CoreServices/SystemVersion.plist", ...)    -> errno 18  Cross-device link
```

**并且:两侧 `st_dev` 完全相同(都是 16777229)**——APFS 的 volume group / firmlink 让
System 卷和 Data 卷在 `stat` 里看起来是同一个设备,**但 clonefile 照样 EXDEV**。

> 工程结论:**不能用 `st_dev` 相等来预判 clonefile 能不能成功**。必须实际调用,并在 `EXDEV` 上回退
> (回退到 `cp -c` 也不行,它同样会退化成真实复制)。用户的工程如果在外置盘、第二个 APFS 卷、
> 或非 APFS 文件系统(exFAT 的移动硬盘、SMB/NFS 网络盘)上,clonefile 直接不可用,
> 整个 lazy-COW 与 native-root 方案都要有一条"真实复制"的降级路径,或者干脆拒绝在那些卷上建 World。

---

## 6. E5 — 判断

**REDIRECT 能拿掉多少?** 按"相对 native 多花的绝对时间"算:S4 多花 5.33s → 重定向后只剩 0.06s(**去掉 98.9%**);
S5 多花 45.7s → 剩 0.06s(**99.9%**);S7 多花 3.90s → 剩 0.48s(**87.7%**);fmt 的 cmake configure
多花 0.92s → 剩 0.007s(**99.2%**),`rm -rf build` 多花 0.22s → 剩 0.002s(**99.1%**)。
换成 native% 就是 S4 18–19%→95–98%、S5 8–11%→96–99%、S7 60–61%→93–96%、configure 55%→99%、rm-build 23%→97%。
**剩下的是什么:**(a)**源文件编辑** —— S2 对照组 18%→13%,一点没变,而这是 coding agent 最高频的动作,
apply_patch 的 temp+rename 每次仍然是 5–7 次 XPC;(b)**增量编译读源文件/头文件** —— fmt 大构建从
83% 只提到 88%,还差 2.0s,全是挂载点上的冷 lookup;(c)**进程 spawn 与 cwd 在挂载点** —— S7 残留 4–7%;
(d)**git 的工作树扫描** —— `.git` 整个可以重定向(E3-T4a/T4b 都跑通,index、objects、gc 全在 native 侧),
但 `git status` 对工作树的几万次 lstat 仍在挂载点上;(e)**没被枚举到的 tmp 目录** —— 白名单
(`target/`、`node_modules/`、`build/`、`.git/objects`、`__pycache__`、`.venv`、`tmp/`)之外的任何写仍是全价。
**语义阻断:**最硬的一条是 `python -m venv` 对 symlink 的 `.venv` **直接报错拒绝**(CPython
`ensure_directories` 里的 `os.path.islink()` 检查,纯 APFS 上同样复现),必须改成"先在 native 建 venv 再链进来",
这会把 native 绝对路径写进 `sys.prefix`;其次是跨界 **EXDEV**(hardlink 失败、`cp -c` 静默退化成真实复制),
会打断 pnpm / uv 的 hardlink-from-store 安装策略;第三是 `rm -rf proj/` **不会删掉 native 产物树**,
必须在 WHITEOUT 时 GC,否则每删一次工程漏一份 node_modules;第四是 `require.resolve` / `sys.prefix` /
cmake cache 会把 native 绝对路径**泄漏出 World**,破坏"World 状态自包含"的不变式。
**最后,一条决定实现方式的结论(见 §0):**这些加速之所以存在,是因为内核在 symlink 上离开了我们的文件系统。
如果按 arch.md §12 的字面意思把 REDIRECT 做成 core 内部的一个 `entries.operation`,IO 仍然走 FSKit,
**5–7 次 XPC 一次都省不掉,本文所有加速归零**。REDIRECT 必须对内核可见。

### 6.1 三种架构的对照

| | **A. FSKit passthrough(现状)** | **B. FSKit + REDIRECT 写密集目录** | **C. native-root clonefile World(无 FSKit)** |
|---|---|---|---|
| **fork 成本** | 元数据事务,µs 级,**满足 <10ms** | 同 A(懒克隆);急切克隆 3 个 20k 目录 = **0.60s,超标 60×** | 10–20 µs/条目:1k=**0.010s**,10k=**0.129s**,50k=**0.481s**,50k+.git=**0.88s**,**不满足 <10ms** |
| **agent 负载速度** | 读/exec/编译 90–110%;元数据写 **8–20%**;S7 60% | S4 **95–98%**、S5 **96–99%**、S7 **93–96%**、configure **99%**、rm-build **97%**;**但 S2 源文件编辑仍 13–18%**,增量编译 88% | **95–110% 全线**(E6c 实测,与 `cp -R` 出来的 native 副本无差别) |
| **存储增长** | 无额外(passthrough) | 重定向目录**每个 World 一份实体**,除非 fork 时 clonefile;`rm -rf` 后**泄漏** | 改 1% 文件 → 物理 **+0.36%**(306MB 树增长 1.1MB);COW 隔离实测 0 处污染 |
| **changed-set / diff** | M1/M2 的 namespace 层给 **O(changes)** | 同 A,但**重定向目录里的变更 namespace 层看不见**(它们不经过 FSKit),diff 有盲区 | 没有 namespace 层:stat-walk **0.78–0.96s / 5万文件**,`git status` 0.15s;都是 **O(tree)**,不满足 arch.md §25 的 O(changes)。FSEvents 可以救,但是目录粒度 + 会丢事件 |
| **失去了什么** | (什么也没失去,只是慢) | 重定向目录里没有 whiteout、没有 base 不可变性、没有 changed 跟踪;EXDEV;`venv` 阻断;绝对路径泄漏;产物泄漏 | **整个 namespace 层**:没有 WHITEOUT(删除就是真删)、没有 base immutable 不变式(误写 base 无人拦)、没有 metadata-only override、没有 rename-as-namespace-only、没有 >PATH_MAX 的路径处理(全部走真实路径,受 1024 字节 `PATH_MAX` 约束);1000 个 World 要 1000 份目录项(不是 1000 份数据) |
| **对 arch.md 成功判据的满足度** | §29 的 ≥90% **不满足**(元数据写 8–20%);fork/§20 满足;§25 满足 | §29 在构建/安装/测试上**满足**,在源文件编辑上**不满足**;fork 满足(懒克隆);§25 **部分失效**(重定向区不可见);§10 base immutable 在重定向区**不成立** | §29 **完全满足**;§20「fork 绝不 walk inode 树」**直接违背**(它就是 walk);§25 **不满足**;§5「第一次写才 clone」在**文件级**由 APFS 自己实现(而不是我们),§6 的 writable-open COW boundary 交给内核;§4「fork 时绝不 clone 整棵树」**违背**,但代价实测只有 0.1–0.9s |

### 6.2 建议

1. **B 是一个成本很低、收益很确定的增量**:不需要改 core 的数据平面,只要在 `world fs init/fork` 时
   按白名单把几个目录建成指向 per-World native 目录的 symlink。它把 agent 负载里最痛的三类
   (安装依赖、构建产物、测试临时文件)从 8–20% 抬到 93–99%。
2. **但 B 解决不了 agent 最高频的动作**(编辑源文件,S2 仍 13–18%)。如果 §29 的 ≥90% 是硬指标,
   B 单独不够,还是要靠 frontend 优化(readdir 的 `getattrlistbulk`、少发一次 create 后的 lookup)
   或者换前端。
3. **C 在纯性能上无可挑剔(95–110%),代价是把 arch.md 的核心价值主张让掉了**:
   fork 从 µs 变成 0.1–0.9s(对 10 万文件的仓库仍在 1s 内,产品上可能可以接受),
   但 changed-set 退化成 O(tree),base 不再由文件系统保护,whiteout / metadata-only override 全没了。
   C 更像是"用 APFS clonefile 做的 `git worktree`",而不是 BranchFS。
4. **如果要做 B,先解决四件事**:(i)REDIRECT 必须对内核可见(symlink),写进 arch.md §12,
   否则实现出来没有加速;(ii)`rm`/WHITEOUT 时 GC 重定向目标;(iii)`python -m venv` 的
   pre-create 绕法要内建进 `world fs` 的模板;(iv)明确 `world fs diff` 在重定向区的语义
   (要么声明不跟踪,要么在重定向区上再跑一次 stat-walk / FSEvents)。
5. **clonefile 的卷约束要写进文档**:同卷才行,`st_dev` 相等**不能**作为判据(APFS firmlink 会骗人,
   实测同 `st_dev` 仍返回 EXDEV),必须实际调用并在 `EXDEV` 上降级或拒绝建 World。
