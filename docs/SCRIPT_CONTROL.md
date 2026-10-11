# 独立 forkfs 脚本入口

`scripts/forkfs.py` 只依赖 Python 标准库和已构建的 forkfsd，不依赖 world
可执行文件、项目或 Bash。可写分支在脚本接口叫 branch；底层 `--world`
仍是 forkfsd 的内部参数名称，不表示调用其他项目。

## 离线操作

全局选项放在子命令前面。下面每条命令独立执行，替换二进制和仓库路径即可。

```sh
python3 scripts/forkfs.py --binary build/forkfsd/daemon/forkfsd --repo project.forkfs init
python3 scripts/forkfs.py --binary build/forkfsd/daemon/forkfsd --repo project.forkfs mkdir /src
python3 scripts/forkfs.py --binary build/forkfsd/daemon/forkfsd --repo project.forkfs write /src/a.txt input.txt
python3 scripts/forkfs.py --binary build/forkfsd/daemon/forkfsd --repo project.forkfs snapshot base
python3 scripts/forkfs.py --binary build/forkfsd/daemon/forkfsd --repo project.forkfs fork base work
python3 scripts/forkfs.py --binary build/forkfsd/daemon/forkfsd --repo project.forkfs --branch work read /src/a.txt
python3 scripts/forkfs.py --binary build/forkfsd/daemon/forkfsd --repo project.forkfs --revision base list /src
python3 scripts/forkfs.py --binary build/forkfsd/daemon/forkfsd --repo project.forkfs --branch work restore base
```

`init` 顺序创建 repository、初始化文件树；已存在的 repository 不覆盖。如果第二步
失败，保留仓库供检查，不自动删除或声称两步原子完成。`restore` 会丢弃未保存的
live 状态，需要保留时先 snapshot。契约见 [restore 设计](RESTORE_DESIGN.md)。

可用命令：init、serve、status、check、compact、branches、revisions、snapshot、
fork、restore、read、write、list、stat、mkdir、rename、unlink、rmdir。
`read` 原样输出二进制字节；`write` 从本地文件读入。离线 status 调用 daemon check
并输出状态；RPC status 直接查询服务状态。每次离线命令持有独占存储锁，服务占用
仓库时应使用 socket 入口，不做自动绕过或重试。

## 常驻 RPC

```sh
python3 scripts/forkfs.py --binary build/forkfsd/daemon/forkfsd --repo project.forkfs serve --listen /tmp/forkfs-control.sock
```

服务在前台运行，Ctrl-C 停止。脚本 exec 替换为 forkfsd，不创建后台守护子进程、
PID 文件或第二套信号管理。socket 父目录必须已存在。另一个终端使用：

```sh
python3 scripts/forkfs.py --binary build/forkfsd/daemon/forkfsd --socket /tmp/forkfs-control.sock status
python3 scripts/forkfs.py --binary build/forkfsd/daemon/forkfsd --socket /tmp/forkfs-control.sock --branch work list /src
python3 scripts/forkfs.py --binary build/forkfsd/daemon/forkfsd --socket /tmp/forkfs-control.sock snapshot later
```

有限命令超时 30 秒，超时返回 124；确认丢失时应重新查询状态，不能推断事务必然
未提交。daemon 拒绝或崩溃均返回非零，stdout/stderr 直接保留。

## 挂载边界与验证

mount/unmount 是显式 unavailable 入口，返回 3，不创建路径、不调用旧的 world
挂载脚本。当前未完成可写 FSKit、注册/签名与真实挂载控制。脚本只是管理接口，
不能替代 FSKit 文件请求回调，也不能作为挂载成功或隔离的证明。

两项 CTest 注册到 metadata-regression，现有 CI 自动发现。真实 daemon 集成测试
覆盖带空格路径、二进制字节、分支隔离、revision 只读、restore、服务前台生命周期
和重新打开；自测检查字面参数、超时、崩溃、选择器拒绝及 unavailable 挂载不执行
外部命令。
