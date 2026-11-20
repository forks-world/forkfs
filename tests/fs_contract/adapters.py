"""POSIX and FFRPC001 adapters for the closed filesystem contract DSL."""
from __future__ import annotations

import ctypes
import errno
import fcntl
import json
import os
from pathlib import Path
import signal
import socket
import stat
import struct
import subprocess
import sys
import time

from .model import FILE_SIZE_LIMIT, SchemaError, safe_path

RPC_ENTRY_LIMIT = 4096
FINGERPRINT_BYTE_LIMIT = 64 * 1024 * 1024


class Unsupported(RuntimeError):
    pass


def _check_write_extent(position, size):
    if position + size > FILE_SIZE_LIMIT:
        raise Unsupported(f"sequential write extent exceeds {FILE_SIZE_LIMIT} byte bound")


class UntypedRpcFailure(RuntimeError):
    def __init__(self, code, payload):
        super().__init__(f"FFRPC001 returned untyped wire error {code}: {payload[:200]!r}")
        self.code = code
        self.payload = payload


def _kind(mode):
    if stat.S_ISREG(mode): return "file"
    if stat.S_ISDIR(mode): return "directory"
    if stat.S_ISLNK(mode): return "symlink"
    return "other"


def _norm_stat(st):
    result = {"inode": st.st_ino, "type": _kind(st.st_mode), "links": st.st_nlink,
              "size": st.st_size, "mode": stat.S_IMODE(st.st_mode)}
    for field, source in (("mtime_ns", "st_mtime_ns"), ("atime_ns", "st_atime_ns")):
        if hasattr(st, source): result[field] = getattr(st, source)
    return result


class PosixAdapter:
    def __init__(self, root: Path, expected_fs: str):
        self.root = root.resolve(strict=True)
        self.expected_fs = expected_fs
        self.handles = {}; self._next_handle = 1
        self._xattr = _DarwinXattr() if sys.platform == "darwin" else None

    def _path(self, raw):
        if not isinstance(raw, str) or not raw or "\x00" in raw or raw.startswith("/"):
            raise SchemaError(f"invalid relative POSIX path {raw!r}")
        # Keep raw spelling (including trailing slash, dot, and `..`) for the
        # syscall. Resolve only a separate probe to enforce the ownership fence.
        return safe_path(self.root, raw, allow_root=True)

    def fixture(self, item):
        path = self._path(item["path"])
        kind = item["type"]
        if kind == "directory":
            os.mkdir(path, item.get("mode", 0o755))
        elif kind == "file":
            fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, item.get("mode", 0o644))
            try:
                view = memoryview(bytes.fromhex(item["data_hex"]))
                while view:
                    written = os.write(fd, view)
                    if written <= 0: raise OSError(errno.EIO, "short fixture write")
                    view = view[written:]
            finally: os.close(fd)
        elif kind == "symlink":
            os.symlink(item["target"], path)

    def execute(self, op, a):
        if "revision" in a:
            raise Unsupported("POSIX adapter has no daemon revision selector")
        if op == "mkdir": os.mkdir(self._path(a["path"]), a.get("mode", 0o755)); return {}
        if op == "create":
            flags = os.O_WRONLY | os.O_CREAT | (os.O_EXCL if a.get("exclusive", True) else os.O_TRUNC)
            fd = os.open(self._path(a["path"]), flags, a.get("mode", 0o644))
            try:
                data = bytes.fromhex(a["data_hex"])
                written = os.write(fd, data)
                if written != len(data): raise OSError(errno.EIO, "short create write")
                return {"written": written}
            finally: os.close(fd)
        if op == "open":
            flags = 0
            requested = a["flags"]
            if not isinstance(requested, list) or not requested or len(requested) != len(set(requested)) or not set(requested) <= {"read", "write", "create", "exclusive", "truncate", "append", "directory", "nofollow"}:
                raise SchemaError("open.flags invalid")
            flags |= os.O_RDWR if "read" in requested and "write" in requested else os.O_WRONLY if "write" in requested else os.O_RDONLY
            mapping = {"create": os.O_CREAT, "exclusive": os.O_EXCL, "truncate": os.O_TRUNC, "append": os.O_APPEND}
            for name, flag in mapping.items():
                if name in requested: flags |= flag
            if "directory" in requested:
                if not hasattr(os, "O_DIRECTORY"): raise Unsupported("native O_DIRECTORY is unavailable")
                flags |= os.O_DIRECTORY
            if "nofollow" in requested:
                if not hasattr(os, "O_NOFOLLOW"): raise Unsupported("native O_NOFOLLOW is unavailable")
                flags |= os.O_NOFOLLOW
            fd = os.open(self._path(a["path"]), flags, a.get("mode", 0o644))
            return {"handle": self._remember(fd)}
        if op == "close": os.close(self._fd(a["handle"])); self.handles.pop(a["handle"]); return {}
        if op == "dup": return {"handle": self._remember(os.dup(self._fd(a["handle"]))) }
        if op == "flock":
            mode = {"shared": fcntl.LOCK_SH, "exclusive": fcntl.LOCK_EX, "unlock": fcntl.LOCK_UN}[a["lock_mode"]]
            fcntl.flock(self._fd(a["handle"]), mode if mode == fcntl.LOCK_UN else mode | fcntl.LOCK_NB)
            return {}
        if op == "fstat": return _norm_stat(os.fstat(self._fd(a["handle"])))
        if op == "read":
            with open(self._path(a["path"]), "rb") as stream: data = stream.read(64 * 1024 * 1024 + 1)
            if len(data) > 64 * 1024 * 1024: raise Unsupported("read exceeds 64 MiB result bound")
            return {"data_hex": data.hex()}
        if op == "pread":
            data = os.pread(self._fd(a["handle"]), a["count"], a["offset"])
            return {"data_hex": data.hex()}
        if op == "pwrite":
            data = bytes.fromhex(a["data_hex"]); n = os.pwrite(self._fd(a["handle"]), data, a["offset"])
            return {"written": n}
        if op == "seek":
            whence = {"set": os.SEEK_SET, "cur": os.SEEK_CUR, "end": os.SEEK_END}[a["whence"]]
            return {"offset": os.lseek(self._fd(a["handle"]), a["displacement"], whence)}
        if op == "read_next":
            fd = self._fd(a["handle"]); data = os.read(fd, a["count"])
            return {"data_hex": data.hex(), "offset": os.lseek(fd, 0, os.SEEK_CUR)}
        if op == "write_next":
            fd = self._fd(a["handle"])
            data = bytes.fromhex(a["data_hex"])
            if data:
                flags = fcntl.fcntl(fd, fcntl.F_GETFL)
                info = os.fstat(fd)
                if (flags & os.O_ACCMODE) != os.O_RDONLY and stat.S_ISREG(info.st_mode):
                    position = info.st_size if flags & os.O_APPEND else os.lseek(fd, 0, os.SEEK_CUR)
                    _check_write_extent(position, len(data))
            n = os.write(fd, data)
            return {"written": n, "offset": os.lseek(fd, 0, os.SEEK_CUR)}
        if op == "truncate": os.truncate(self._path(a["path"]), a["size"]); return {}
        if op == "ftruncate": os.ftruncate(self._fd(a["handle"]), a["size"]); return {}
        if op == "append":
            fd = os.open(self._path(a["path"]), os.O_WRONLY | os.O_APPEND)
            try:
                data = bytes.fromhex(a["data_hex"]); n = os.write(fd, data)
                return {"written": n}
            finally: os.close(fd)
        if op in ("stat", "lstat"):
            st = os.stat(self._path(a["path"])) if op == "stat" else os.lstat(self._path(a["path"]))
            result = _norm_stat(st)
            if op == "lstat" and stat.S_ISLNK(st.st_mode):
                result["target"] = os.fsdecode(os.readlink(self._path(a["path"])))
            return result
        if op == "listdir": return {"names": sorted(os.fsdecode(x) for x in os.listdir(self._path(a["path"])))}
        if op == "link": os.link(self._path(a["source"]), self._path(a["target"])); return {}
        if op == "rename": os.rename(self._path(a["source"]), self._path(a["target"])); return {}
        if op in {"rename_exclusive", "rename_exchange"}:
            if sys.platform != "darwin":
                raise Unsupported("rename flags require Darwin renamex_np")
            libc = ctypes.CDLL("/usr/lib/libSystem.B.dylib", use_errno=True)
            rename = libc.renamex_np
            rename.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint]
            rename.restype = ctypes.c_int
            # RENAME_EXCL and RENAME_SWAP from Darwin's <sys/stdio.h>.
            flag = {"rename_exclusive": 0x4, "rename_exchange": 0x2}[op]
            if rename(self._path(a["source"]), self._path(a["target"]), flag) != 0:
                code = ctypes.get_errno()
                raise OSError(code, os.strerror(code))
            return {}
        if op == "symlink": os.symlink(a["target"], self._path(a["path"])); return {}
        if op == "readlink": return {"target": os.fsdecode(os.readlink(self._path(a["path"])))}
        if op == "unlink": os.unlink(self._path(a["path"])); return {}
        if op == "rmdir": os.rmdir(self._path(a["path"])); return {}
        if op == "chmod": os.chmod(self._path(a["path"]), a["mode"]); return {}
        if op == "utime": os.utime(self._path(a["path"]), ns=(a["atime_ns"], a["mtime_ns"])); return {}
        if op == "xattr_set":
            self._setxattr(self._path(a["path"]), a["name"], bytes.fromhex(a["value_hex"]), a["policy"]); return {}
        if op == "xattr_get": return {"value_hex": self._getxattr(self._path(a["path"]), a["name"]).hex()}
        if op == "xattr_remove": self._removexattr(self._path(a["path"]), a["name"]); return {}
        if op == "mkdir_at":
            fd = self._fd(a["handle"]); os.mkdir(a["name"], a.get("mode", 0o777), dir_fd=fd)
            return {"inode": os.stat(a["name"], dir_fd=fd, follow_symlinks=False).st_ino}
        if op in ("snapshot", "restart", "sync"): raise Unsupported(f"{op} is an RPC-only extension")
        raise SchemaError(f"unimplemented POSIX operation {op}")

    def fingerprint(self, revision=None):
        records = []
        logical_bytes = 0
        root_stat = os.lstat(self.root)
        root_item = {"path": ".", **_norm_stat(root_stat)}
        root_item.pop("atime_ns", None)
        root_item["ctime_ns"] = getattr(root_stat, "st_ctime_ns", int(root_stat.st_ctime * 1e9))
        root_item["xattrs"] = self._listxattrs(os.fsencode(str(self.root)), nofollow=False)
        records.append(root_item)
        def walk_error(error): raise error
        logical_bytes = 0
        for directory, dirs, files in os.walk(self.root, topdown=True, followlinks=False, onerror=walk_error):
            dirs.sort(); files.sort()
            for name in list(dirs):
                path = os.path.join(directory, name)
                if os.path.islink(path): dirs.remove(name); files.append(name)
            for name in sorted(dirs + files):
                path = os.path.join(directory, name)
                st = os.lstat(path); item = {"path": os.path.relpath(path, self.root), **_norm_stat(st)}
                item.pop("atime_ns", None)
                item["ctime_ns"] = getattr(st, "st_ctime_ns", int(st.st_ctime * 1e9))
                if stat.S_ISREG(st.st_mode):
                    logical_bytes += st.st_size
                    if st.st_size > 64 * 1024 * 1024 or logical_bytes > 64 * 1024 * 1024:
                        raise Unsupported("fingerprint logical file bytes exceed 64 MiB bound")
                    with open(path, "rb") as stream: item["data_hex"] = stream.read().hex()
                elif stat.S_ISLNK(st.st_mode): item["target"] = os.fsdecode(os.readlink(path))
                elif not stat.S_ISDIR(st.st_mode): raise Unsupported("fingerprint encountered special file")
                item["xattrs"] = self._listxattrs(path, nofollow=stat.S_ISLNK(st.st_mode))
                records.append(item)
        live = []
        for symbol, fd in sorted(self.handles.items()):
            st = os.fstat(fd); item = {"symbol": symbol, **_norm_stat(st)}
            item.pop("atime_ns", None)
            item["ctime_ns"] = getattr(st, "st_ctime_ns", int(st.st_ctime * 1e9))
            item["xattrs"] = self._fd_xattrs(fd)
            if stat.S_ISREG(st.st_mode): item["offset"] = os.lseek(fd, 0, os.SEEK_CUR)
            if stat.S_ISREG(st.st_mode):
                logical_bytes += st.st_size
                if st.st_size > 64 * 1024 * 1024 or logical_bytes > 64 * 1024 * 1024:
                    raise Unsupported("fingerprint live handle bytes exceed 64 MiB bound")
                try: item["data_hex"] = os.pread(fd, st.st_size, 0).hex()
                except OSError as exc:
                    raise Unsupported(f"cannot fingerprint live handle bytes: {exc}") from exc
            live.append(item)
        return {"entries": records, "handles": live}

    def close_all(self):
        for symbol, fd in list(self.handles.items()):
            try: os.close(fd)
            except OSError: pass
            self.handles.pop(symbol, None)

    def _remember(self, fd):
        symbol = f"fd{self._next_handle}"
        self._next_handle += 1
        self.handles[symbol] = fd
        return symbol

    def _fd(self, symbol):
        try: return self.handles[symbol]
        except KeyError as exc: raise OSError(errno.EBADF, "unknown contract handle") from exc

    def _getxattr(self, path, name):
        if self._xattr: return self._xattr.get(path, name)
        return os.getxattr(path, name, follow_symlinks=False)

    def _setxattr(self, path, name, value, policy):
        if self._xattr: return self._xattr.set(path, name, value, policy)
        flags = {"upsert": 0, "create": getattr(os, "XATTR_CREATE", 1), "replace": getattr(os, "XATTR_REPLACE", 2)}[policy]
        return os.setxattr(path, name, value, flags=flags, follow_symlinks=False)

    def _removexattr(self, path, name):
        if self._xattr: return self._xattr.remove(path, name)
        return os.removexattr(path, name, follow_symlinks=False)

    def _listxattrs(self, path, nofollow):
        if self._xattr: return {name: self._xattr.get(path, name).hex() for name in self._xattr.list(path)}
        try:
            return {os.fsdecode(name): os.getxattr(path, name, follow_symlinks=not nofollow).hex()
                    for name in os.listxattr(path, follow_symlinks=not nofollow)}
        except OSError as exc:
            if exc.errno in (errno.ENOTSUP, getattr(errno, "EOPNOTSUPP", errno.ENOTSUP)):
                raise Unsupported("xattr fingerprint unsupported on this filesystem") from exc
            raise

    def _fd_xattrs(self, fd):
        if self._xattr:
            return {name: self._xattr.fd_get(fd, name).hex() for name in self._xattr.fd_list(fd)}
        try:
            return {os.fsdecode(name): os.getxattr(fd, name).hex() for name in os.listxattr(fd)}
        except (TypeError, NotImplementedError) as exc:
            raise Unsupported("file-descriptor xattr fingerprint is unavailable") from exc
        except OSError as exc:
            if exc.errno in (errno.ENOTSUP, getattr(errno, "EOPNOTSUPP", errno.ENOTSUP)):
                raise Unsupported("file-descriptor xattr fingerprint is unsupported") from exc
            raise


class _DarwinXattr:
    """Use Darwin's native xattr APIs where Python lacks os.*xattr wrappers."""
    def __init__(self):
        self.libc = ctypes.CDLL("/usr/lib/libSystem.B.dylib", use_errno=True)
        self.getxattr = self.libc.getxattr; self.setxattr = self.libc.setxattr
        self.removexattr = self.libc.removexattr; self.listxattr = self.libc.listxattr
        self.fgetxattr = self.libc.fgetxattr; self.flistxattr = self.libc.flistxattr
        self.fgetxattr = self.libc.fgetxattr; self.flistxattr = self.libc.flistxattr
        self.getxattr.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32, ctypes.c_int]
        self.setxattr.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32, ctypes.c_int]
        self.removexattr.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]
        self.listxattr.argtypes = [ctypes.c_char_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int]
        self.fgetxattr.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32, ctypes.c_int]
        self.flistxattr.argtypes = [ctypes.c_int, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int]
        self.fgetxattr.restype = ctypes.c_ssize_t; self.flistxattr.restype = ctypes.c_ssize_t
        self.getxattr.restype = ctypes.c_ssize_t; self.setxattr.restype = ctypes.c_int
        self.removexattr.restype = ctypes.c_int; self.listxattr.restype = ctypes.c_ssize_t
        self._flags = {"upsert": 1, "create": 3, "replace": 5}

    def _raise(self):
        code = ctypes.get_errno(); raise OSError(code, os.strerror(code))

    def get(self, path, name):
        path = os.fsencode(path)
        size = self.getxattr(path, name.encode(), None, 0, 0, 1)
        if size < 0: self._raise()
        buf = ctypes.create_string_buffer(size)
        got = self.getxattr(path, name.encode(), buf, size, 0, 1)
        if got < 0: self._raise()
        return buf.raw[:got]

    def set(self, path, name, value, policy):
        path = os.fsencode(path)
        buf = ctypes.create_string_buffer(value)
        if self.setxattr(path, name.encode(), buf, len(value), 0, self._flags[policy]) < 0: self._raise()

    def remove(self, path, name):
        path = os.fsencode(path)
        if self.removexattr(path, name.encode(), 1) < 0: self._raise()

    def list(self, path):
        path = os.fsencode(path)
        size = self.listxattr(path, None, 0, 1)
        if size < 0: self._raise()
        if not size: return []
        buf = ctypes.create_string_buffer(size)
        got = self.listxattr(path, buf, size, 1)
        if got < 0: self._raise()
        return [item.decode() for item in buf.raw[:got].split(b"\x00") if item]

    def fd_get(self, fd, name):
        size = self.fgetxattr(fd, name.encode(), None, 0, 0, 0)
        if size < 0: self._raise()
        buf = ctypes.create_string_buffer(size)
        got = self.fgetxattr(fd, name.encode(), buf, size, 0, 0)
        if got < 0: self._raise()
        return buf.raw[:got]

    def fd_list(self, fd):
        size = self.flistxattr(fd, None, 0, 0)
        if size < 0: self._raise()
        if not size: return []
        buf = ctypes.create_string_buffer(size)
        got = self.flistxattr(fd, buf, size, 0)
        if got < 0: self._raise()
        return [item.decode() for item in buf.raw[:got].split(b"\x00") if item]


class RpcAdapter:
    """A fresh FFRPC001 service, database, and session for one contract case."""
    def __init__(self, binary: Path, root: Path, timeout=5):
        self.binary = binary.resolve(strict=True); self.root = root.resolve(strict=True)
        self.db = self.root / "store"; self.socket_path = self.root / "control.sock"
        if len(os.fsencode(self.socket_path)) >= 100: raise Unsupported("Unix socket path too long")
        self.timeout = timeout; self.process = None; self.session = None; self.handles = set()
        self.handle_revisions = {}; self.revisions = set()
        self.handle_write_modes = {}
        try:
            self._command("init", self.db)
            self._command("fs-init", self.db)
            self._start_server()
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                if self.process.poll() is not None:
                    raise RuntimeError(f"forkfsd exited during startup with status {self.process.returncode}")
                try:
                    self._json("status")
                    break
                except (OSError, ConnectionError): time.sleep(0.02)
            else: raise TimeoutError("forkfsd RPC socket did not become ready")
            self.session = self._json("session-open", "60000")["session"]
        except BaseException:
            self._stop()
            raise

    def _start_server(self):
        self.process = subprocess.Popen([str(self.binary), "serve", str(self.db), "--socket", str(self.socket_path), "--rpc"],
                                        cwd=str(self.root), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                        start_new_session=False)

    def _command(self, *args):
        try:
            return subprocess.run([str(self.binary), *map(str, args)], cwd=str(self.root), check=True,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=self.timeout)
        except subprocess.TimeoutExpired as exc:
            raise TimeoutError(f"forkfsd command timed out: {args[0]}") from exc

    def rpc(self, *fields):
        values = [f if isinstance(f, bytes) else str(f).encode() for f in fields]
        payload = struct.pack("<I", len(values)) + b"".join(struct.pack("<I", len(v)) + v for v in values)
        if len(payload) > 1024 * 1024: raise Unsupported("RPC payload exceeds 1 MiB contract bound")
        with socket.socket(socket.AF_UNIX) as sock:
            sock.settimeout(self.timeout); sock.connect(str(self.socket_path))
            sock.sendall(b"FFRPC001" + struct.pack("<I", len(payload)) + payload)
            header = self._exact(sock, 12)
            if header[:8] != b"FFRSP001": raise RuntimeError("invalid FFRPC001 response magic")
            size = struct.unpack("<I", header[8:])[0]
            if size < 4 or size > 1024 * 1024: raise RuntimeError("invalid FFRPC001 response length")
            response = self._exact(sock, size)
        code = struct.unpack("<I", response[:4])[0]
        if code == 1: raise UntypedRpcFailure(code, response[4:])
        if code: raise OSError(code - 1, response[4:].decode(errors="replace"))
        return response[4:]

    @staticmethod
    def _exact(sock, size):
        result = bytearray()
        while len(result) < size:
            block = sock.recv(size - len(result))
            if not block: raise RuntimeError("truncated FFRPC001 response")
            result.extend(block)
        return bytes(result)

    def _json(self, *fields): return json.loads(self.rpc(*fields).decode())

    def _volume_name(self, *fields):
        result = self._json(*fields)
        if not isinstance(result, dict) or not isinstance(result.get("name"), str):
            raise RuntimeError("volume-name RPC response must contain a string name")
        return {"name": result["name"]}

    def _selectors(self, revision=None):
        result = ["--session", self.session]
        if revision is not None: result += ["--revision", revision]
        return result

    def fixture(self, item):
        path = "/" + item["path"]
        kind = item["type"]
        if kind == "directory":
            self.rpc("fs-mkdir", path, *self._selectors())
            if item.get("mode", 0o755) != 0o755:
                self.rpc("fs-chmod", path, oct(item["mode"])[2:], *self._selectors())
        elif kind == "file":
            opened = self._json("fs-open", path, "rw,create,exclusive,nofollow", *self._selectors())
            handle = opened["handle"]; self.handles.add(handle)
            try:
                data = bytes.fromhex(item["data_hex"])
                self.rpc("fs-hwrite", handle, "0", data, *self._selectors())
            finally:
                self.rpc("fs-close", handle, *self._selectors()); self.handles.discard(handle)
            if item.get("mode", 0o644) != 0o644:
                self.rpc("fs-chmod", path, oct(item["mode"])[2:], *self._selectors())
        else: self.rpc("fs-symlink", item["target"], path, *self._selectors())

    def execute(self, op, a):
        path = lambda name: "/" + a[name]
        sel = lambda: self._selectors(a.get("revision"))
        if a.get("revision") is not None: self.revisions.add(a["revision"])
        if op == "volume_name": return self._volume_name("fs-volume-name", *sel())
        if op == "set_volume_name": return self._volume_name("fs-set-volume-name", a["name"], *sel())
        if op == "mkdir":
            if a.get("mode", 0o755) != 0o755: raise Unsupported("RPC fs-mkdir has no creation-mode argument")
            self.rpc("fs-mkdir", path("path"), *sel()); return {}
        if op == "create" or op == "open":
            if op == "create":
                flags = ["write", "create"] + (["exclusive"] if a.get("exclusive", True) else ["truncate"])
            else: flags = a["flags"]
            access = "rw" if "read" in flags and "write" in flags else "r" if "read" in flags else "w"
            names = ["create", "exclusive", "truncate", "append", "nofollow", "directory"]
            mode = access + "".join("," + f for f in names if f in flags)
            if a.get("mode", 0o644) != 0o644: raise Unsupported("RPC fs-open has fixed 0644 creation mode")
            result = self._json("fs-open", path("path"), mode, *sel())
            handle = result["handle"]; self.handles.add(handle)
            self.handle_revisions[handle] = a.get("revision")
            if op == "open":
                if "write" in flags:
                    self.handle_write_modes[handle] = "append" if "append" in flags else "positioned"
                return {"handle": handle}
            try:
                data = bytes.fromhex(a["data_hex"])
                if data: self.rpc("fs-hwrite", handle, "0", data, *sel())
                return {"written": len(data)}
            finally:
                self.rpc("fs-close", handle, *sel()); self.handles.discard(handle); self.handle_revisions.pop(handle, None)
        if op == "close":
            self.rpc("fs-close", a["handle"], *sel()); self.handles.discard(a["handle"]); self.handle_revisions.pop(a["handle"], None)
            self.handle_write_modes.pop(a["handle"], None); return {}
        if op == "dup":
            result = self._json("fs-dup", a["handle"], *sel()); self.handles.add(result["handle"])
            self.handle_revisions[result["handle"]] = self.handle_revisions.get(a["handle"], a.get("revision"))
            if a["handle"] in self.handle_write_modes:
                self.handle_write_modes[result["handle"]] = self.handle_write_modes[a["handle"]]
            return result
        if op in {"rpc_lock", "rpc_range_lock"}:
            fields = ["fs-hlock" if op == "rpc_lock" else "fs-hrange-lock", a["handle"], a["lock_mode"]]
            if op == "rpc_range_lock": fields.append(f'{a["start"]}:{a["length"]}')
            result = self._json(*fields, *sel())
            if not isinstance(result, dict) or type(result.get("granted")) is not bool:
                raise RuntimeError("RPC lock response must contain boolean granted")
            return {"granted": result["granted"]}
        if op == "fstat": return self._normal_stat(self._json("fs-hstat", a["handle"], *sel()))
        if op == "read": return {"data_hex": self.rpc("fs-cat", path("path"), *sel()).hex()}
        if op == "pread": return {"data_hex": self.rpc("fs-hread", a["handle"], a["offset"], a["count"], *sel()).hex()}
        if op == "pwrite":
            data = bytes.fromhex(a["data_hex"]); self.rpc("fs-hwrite", a["handle"], a["offset"], data, *sel()); return {"written": len(data)}
        if op == "seek":
            result = self._json("fs-hseek", a["handle"], a["displacement"], a["whence"], *sel())
            return {"offset": result["offset"]}
        if op == "read_next": return {"data_hex": self.rpc("fs-hread-next", a["handle"], a["count"], *sel()).hex(), "offset": self._json("fs-hseek", a["handle"], 0, "cur", *sel())["offset"]}
        if op == "write_next":
            data = bytes.fromhex(a["data_hex"])
            mode = self.handle_write_modes.get(a["handle"])
            if data and mode is not None:
                if mode == "append":
                    position = self._json("fs-hstat", a["handle"], *sel())["size"]
                else:
                    position = self._json("fs-hseek", a["handle"], 0, "cur", *sel())["offset"]
                _check_write_extent(position, len(data))
            result = self._json("fs-hwrite-next", a["handle"], data, *sel())
            result["offset"] = self._json("fs-hseek", a["handle"], 0, "cur", *sel())["offset"]; return result
        if op == "truncate": self.rpc("fs-truncate", path("path"), a["size"], *sel()); return {}
        if op == "ftruncate": self.rpc("fs-htruncate", a["handle"], a["size"], *sel()); return {}
        if op == "append": return self._json("fs-append", path("path"), bytes.fromhex(a["data_hex"]), *sel())
        if op in ("stat", "lstat"):
            raw = self._json("fs-" + op, path("path"), *sel())
            result = self._normal_stat(raw)
            if op == "lstat" and raw["symlink"]:
                result["target"] = self.rpc("fs-readlink", path("path"), *sel()).decode()
            return result
        if op == "listdir":
            cursor = self._json("fs-dir-open", path("path"), *sel())["cursor"]
            names = []
            try:
                while True:
                    page = self._json("fs-dir-page", cursor, "128", *sel())
                    names.extend(page["names"])
                    if page["eof"]: break
                return {"names": sorted(names)}
            finally: self.rpc("fs-dir-close", cursor, *sel())
        if op == "rpc_dir_open":
            result = self._json("fs-dir-open", path("path"), *sel())
            cursor = result.get("cursor") if isinstance(result, dict) else None
            if not isinstance(cursor, str) or not cursor:
                raise RuntimeError("RPC directory open response must contain a nonempty cursor")
            return {"cursor": cursor}
        if op == "rpc_dir_page":
            result = self._json("fs-dir-page", a["cursor"], str(a["limit"]), *sel())
            if (not isinstance(result, dict) or set(result) != {"names", "eof"} or
                    not isinstance(result["names"], list) or any(not isinstance(n, str) for n in result["names"]) or
                    type(result["eof"]) is not bool or len(result["names"]) > a["limit"]):
                raise RuntimeError("RPC directory page must contain bounded names and boolean eof")
            return result
        if op == "rpc_dir_rewind":
            self.rpc("fs-dir-rewind", a["cursor"], *sel()); return {}
        if op == "rpc_dir_close":
            self.rpc("fs-dir-close", a["cursor"], *sel()); return {}
        if op == "link": self.rpc("fs-link", path("source"), path("target"), *sel()); return {}
        if op in {"rename", "rename_exclusive", "rename_exchange"}:
            command = {
                "rename": "fs-rename",
                "rename_exclusive": "fs-rename-excl",
                "rename_exchange": "fs-exchange",
            }[op]
            self.rpc(command, path("source"), path("target"), *sel())
            return {}
        if op == "symlink": self.rpc("fs-symlink", a["target"], path("path"), *sel()); return {}
        if op == "readlink": return {"target": self.rpc("fs-readlink", path("path"), *sel()).decode()}
        if op == "unlink": self.rpc("fs-unlink", path("path"), *sel()); return {}
        if op == "rmdir": self.rpc("fs-rmdir", path("path"), *sel()); return {}
        if op == "chmod": self.rpc("fs-chmod", path("path"), oct(a["mode"])[2:], *sel()); return {}
        if op == "utime": self.rpc("fs-settimes", path("path"), a["atime_ns"], a["mtime_ns"], "-", *sel()); return {}
        if op == "xattr_set":
            self.rpc("fs-xattr-set", path("path"), a["name"], a["policy"], bytes.fromhex(a["value_hex"]), *sel()); return {}
        if op == "xattr_get": return {"value_hex": self.rpc("fs-xattr-get", path("path"), a["name"], *sel()).hex()}
        if op == "xattr_remove": self.rpc("fs-xattr-remove", path("path"), a["name"], *sel()); return {}
        if op == "mkdir_at":
            result = self._json("fs-hmkdir", a["handle"], a["name"], a.get("mode", 0o777), *sel())
            return {"inode": result.get("inode_id")}
        if op == "snapshot":
            result = self._json("fs-snapshot", a["name"], *sel())
            self.revisions.add(a["name"])
            return result
        if op == "restart": self.restart(); return {}
        if op == "sync":
            self._json("inode-root", *sel())
            return self._json("inode-sync", *sel())
        raise SchemaError(f"unimplemented RPC operation {op}")

    @staticmethod
    def _normal_stat(value):
        return {"inode": value["inode_id"], "type": "directory" if value["directory"] else "symlink" if value["symlink"] else "file",
                "links": value["links"], "size": value["size"], "mode": value["mode"],
                "mtime_ns": value["modified_ns"], "atime_ns": value["accessed_ns"]}

    def fingerprint(self, revision=None):
        status = self._json("status")
        required = ("last_committed_seq", "indexed_objects", "object_roots")
        if any(key not in status or status[key] is None for key in required):
            raise RuntimeError("RPC status omitted publication fingerprint fields")
        result = {"seq": status.get("last_committed_seq"), "indexed_objects": status.get("indexed_objects"),
                  "object_roots": status.get("object_roots"), "revisions": self.rpc("fs-revisions").decode().splitlines(),
                  "view_trees": {}, "volume_names": {"live": None, "revisions": {}}}
        views = [None, *sorted(self.revisions | ({revision} if revision else set()))]
        logical_bytes = 0
        total_entries = 0
        for view in views:
            selectors = self._selectors(view)
            volume_name = self._volume_name("fs-volume-name", *selectors)["name"]
            if view is None: result["volume_names"]["live"] = volume_name
            else: result["volume_names"]["revisions"][view] = volume_name
            entries = []
            def walk(path):
                nonlocal logical_bytes, total_entries
                total_entries += 1
                if total_entries > RPC_ENTRY_LIMIT:
                    raise Unsupported(f"RPC fingerprint entry count exceeds {RPC_ENTRY_LIMIT}")
                rawstat = self._json("fs-lstat", path, *selectors)
                item = self._normal_stat(rawstat)
                item.pop("atime_ns", None)
                if "changed_ns" not in rawstat: raise RuntimeError("RPC stat omitted changed_ns")
                item["ctime_ns"] = rawstat["changed_ns"]
                item["path"] = path
                item["xattrs"] = {}
                for name in self._json("fs-xattr-list", path, *selectors)["names"]:
                    item["xattrs"][name] = self.rpc("fs-xattr-get", path, name, *selectors).hex()
                if item["type"] == "file":
                    logical_bytes += item["size"]
                    if item["size"] > FINGERPRINT_BYTE_LIMIT or logical_bytes > FINGERPRINT_BYTE_LIMIT:
                        raise Unsupported("RPC fingerprint logical bytes exceed 64 MiB bound")
                    item["data_hex"] = self.rpc("fs-cat", path, *selectors).hex()
                elif item["type"] == "symlink": item["target"] = self.rpc("fs-readlink", path, *selectors).decode()
                item["view"] = view or "live"
                entries.append(item)
                if item["type"] == "directory":
                    cursor = self._json("fs-dir-open", path, *selectors)["cursor"]
                    names = []
                    try:
                        while True:
                            page = self._json("fs-dir-page", cursor, "128", *selectors)
                            names.extend(page["names"])
                            if total_entries + len(names) > RPC_ENTRY_LIMIT:
                                raise Unsupported(f"RPC fingerprint entry count exceeds {RPC_ENTRY_LIMIT}")
                            if page["eof"]: break
                    finally: self.rpc("fs-dir-close", cursor, *selectors)
                    for name in sorted(names): walk(path.rstrip("/") + "/" + name)
            walk("/")
            result["view_trees"][view or "live"] = entries
        live = []
        for handle in sorted(self.handles):
            handle_revision = self.handle_revisions.get(handle)
            handle_selectors = self._selectors(handle_revision)
            rawstat = self._json("fs-hstat", handle, *handle_selectors)
            item = self._normal_stat(rawstat); item.pop("atime_ns", None)
            if "changed_ns" not in rawstat: raise RuntimeError("RPC handle stat omitted changed_ns")
            item["ctime_ns"] = rawstat["changed_ns"]
            item["handle"] = handle
            if item["type"] == "file":
                item["offset"] = self._json("fs-hseek", handle, 0, "cur", *handle_selectors)["offset"]
                logical_bytes += item["size"]
                if item["size"] > FINGERPRINT_BYTE_LIMIT or logical_bytes > FINGERPRINT_BYTE_LIMIT:
                    raise Unsupported("RPC fingerprint handle bytes exceed 64 MiB bound")
                item["data_hex"] = self.rpc("fs-hread", handle, 0, item["size"], *handle_selectors).hex()
            try:
                item["xattrs"] = {}
                for name in self._json("fs-hxattr-list", handle, *handle_selectors)["names"]:
                    item["xattrs"][name] = self.rpc("fs-hxattr-get", handle, name, *handle_selectors).hex()
            except OSError as exc:
                raise Unsupported(f"cannot fingerprint RPC handle xattrs: {exc}") from exc
            live.append(item)
        result["handles"] = live
        return result

    def restart(self):
        self._stop()
        self._start_server()
        deadline = time.monotonic() + self.timeout
        while time.monotonic() < deadline:
            if self.process.poll() is not None: raise RuntimeError("forkfsd failed after restart")
            try:
                self._json("status")
                break
            except (OSError, ConnectionError): time.sleep(0.02)
        else: raise TimeoutError("forkfsd RPC socket unavailable after restart")
        self.handles.clear(); self.handle_revisions.clear(); self.handle_write_modes.clear()
        self.session = self._json("session-open", "60000")["session"]

    def _stop(self):
        if self.process is None: return
        if self.process.poll() is None:
            try: self.process.terminate()
            except ProcessLookupError: pass
            try: self.process.wait(timeout=self.timeout)
            except subprocess.TimeoutExpired:
                try: self.process.kill()
                except ProcessLookupError: pass
                self.process.wait(timeout=self.timeout)
        self.process = None

    def close_all(self):
        if self.process is not None and self.session:
            try: self.rpc("session-close", self.session)
            except Exception: pass
        self.handles.clear(); self.handle_revisions.clear(); self.handle_write_modes.clear(); self._stop()
