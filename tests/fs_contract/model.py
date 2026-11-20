"""Closed JSON DSL validation and safe path resolution for fs_contract."""
from __future__ import annotations

import json
import errno
import os
from pathlib import Path
import re


class SchemaError(ValueError):
    pass


FILE_SIZE_LIMIT = 64 * 1024 * 1024

PROFILES = {"reference", "daemon", "mounted"}
LAYERS = {"posix", "rpc"}
OPS = {
    "mkdir": {"path", "mode"}, "create": {"path", "data_hex", "mode", "exclusive"},
    "open": {"path", "flags", "mode"}, "close": {"handle"}, "dup": {"handle"},
    "fstat": {"handle"}, "read": {"path"}, "pread": {"handle", "offset", "count"},
    "pwrite": {"handle", "offset", "data_hex"}, "seek": {"handle", "displacement", "whence"},
    "read_next": {"handle", "count"},
    "write_next": {"handle", "data_hex"}, "truncate": {"path", "size"},
    "ftruncate": {"handle", "size"}, "append": {"path", "data_hex"},
    "stat": {"path"}, "lstat": {"path"}, "listdir": {"path"},
    "link": {"source", "target"}, "rename": {"source", "target"},
    "rename_exclusive": {"source", "target"}, "rename_exchange": {"source", "target"},
    "symlink": {"target", "path"}, "readlink": {"path"}, "unlink": {"path"},
    "rmdir": {"path"}, "chmod": {"path", "mode"},
    "utime": {"path", "atime_ns", "mtime_ns"},
    "xattr_set": {"path", "name", "value_hex", "policy"},
    "xattr_get": {"path", "name"}, "xattr_remove": {"path", "name"},
    "mkdir_at": {"handle", "name", "mode"}, "snapshot": {"name"},
    "restart": set(), "sync": {"handle"},
    "flock": {"handle", "lock_mode"}, "rpc_lock": {"handle", "lock_mode"},
    "rpc_range_lock": {"handle", "lock_mode", "start", "length"},
    "rpc_seek_region": {"path", "offset", "region"}, "rpc_hseek_region": {"handle", "offset", "region"},
    "rpc_dir_open": {"path"}, "rpc_dir_page": {"cursor", "limit"},
    "rpc_dir_rewind": {"cursor"}, "rpc_dir_close": {"cursor"},
    "volume_name": set(), "set_volume_name": {"name"},
}
EXPECT_KEYS = {"errno", "unchanged", "data_hex", "type", "inode", "inode_not", "links", "name",
               "size", "mode", "mtime_ns", "atime_ns", "target", "names", "offset",
               "ordered_names", "eof", "written", "value_hex", "granted", "allocated_size"}
RESULT_FIELDS = EXPECT_KEYS - {"errno", "unchanged", "inode_not", "ordered_names"} | {"handle", "cursor"}
REF_FIELD = {"ref", "field"}
ERRNOS = {
    "E2BIG", "EACCES", "EBADF", "EBUSY", "EEXIST", "EFAULT", "EFBIG", "EINTR",
    "EINVAL", "EWOULDBLOCK", "EIO", "EISDIR", "ELOOP", "EMFILE", "ENAMETOOLONG", "ENFILE",
    "ENODATA", "ENOATTR", "ENOENT", "ENOSPC", "ENOTDIR", "ENOTEMPTY", "ENOTSUP", "ENXIO",
    "EOPNOTSUPP", "EOVERFLOW", "EPERM", "ERANGE", "EROFS", "ESPIPE", "ESTALE", "ETIMEDOUT", "EXDEV",
}
SYMBOL = re.compile(r"^[A-Za-z][A-Za-z0-9_]*$")


def load_json(path: Path):
    try:
        with path.open("r", encoding="utf-8") as stream:
            return json.load(stream, object_pairs_hook=_no_duplicate_keys)
    except (OSError, json.JSONDecodeError) as exc:
        raise SchemaError(f"cannot load {path}: {exc}") from exc


def _no_duplicate_keys(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise SchemaError(f"duplicate JSON object key: {key!r}")
        result[key] = value
    return result


def _keys(obj, required, optional, where):
    if not isinstance(obj, dict):
        raise SchemaError(f"{where} must be an object")
    missing = set(required) - obj.keys()
    extra = obj.keys() - set(required) - set(optional)
    if missing or extra:
        raise SchemaError(f"{where}: missing={sorted(missing)}, unknown={sorted(extra)}")


def _ref_or(value, where):
    if isinstance(value, dict):
        if set(value) != REF_FIELD or not all(isinstance(value[k], str) for k in REF_FIELD):
            raise SchemaError(f"{where}: ref must be exactly {{ref, field}}")
        if not SYMBOL.fullmatch(value["ref"]) or value["field"] not in RESULT_FIELDS:
            raise SchemaError(f"{where}: invalid ref symbol or result field")


def _safe_relpath(value, where, allow_root=False):
    if not isinstance(value, str) or not value or "\x00" in value or value.startswith("/"):
        raise SchemaError(f"{where} must be a nonempty relative path")
    if allow_root and value == ".":
        return "."
    # `..` is allowed only when the normalized path remains below the owned root.
    parts = []
    for part in value.split("/"):
        if part in ("", "."):
            continue
        if part == "..":
            if not parts:
                raise SchemaError(f"{where} escapes the owned root")
            parts.pop()
        else:
            parts.append(part)
    if not parts:
        raise SchemaError(f"{where} may not name the owned root")
    return "/".join(parts)


def safe_path(root: Path, rel: str, allow_root=False) -> bytes:
    """Check physical resolution stays owned, returning raw spelling for syscall use."""
    _safe_relpath(rel, "path", allow_root)
    root_b = os.fsencode(str(root.resolve(strict=True)))
    pending = rel.split("/")
    stack = []
    hops = 0
    while pending:
        part = pending.pop(0)
        if part in ("", "."):
            continue
        if part == "..":
            if not stack:
                raise SchemaError(f"path physically escapes owned root: {rel!r}")
            stack.pop()
            continue
        probe = os.path.join(root_b, *stack, os.fsencode(part))
        try:
            os.lstat(probe)
        except FileNotFoundError:
            stack.append(os.fsencode(part))
            continue
        except OSError as exc:
            if exc.errno in (errno.ENAMETOOLONG, errno.ENOTDIR):
                stack.append(os.fsencode(part))
                continue
            # A guard failure must never masquerade as the errno under test.
            raise SchemaError(f"cannot prove path containment for {rel!r}: {exc}") from exc
        if os.path.islink(probe):
            hops += 1
            if hops > 40:
                raise SchemaError(f"too many symlink hops to prove case path ownership: {rel!r}")
            target = os.readlink(probe)
            if os.path.isabs(target):
                raise SchemaError(f"absolute symlink is outside this contract DSL: {rel!r}")
            pending = os.fsdecode(target).split("/") + pending
        else:
            stack.append(os.fsencode(part))
    return root_b + os.sep.encode() + os.fsencode(rel)


REQUIRED_ARGS = {
    "mkdir": {"path"}, "create": {"path", "data_hex"}, "open": {"path", "flags"},
    "close": {"handle"}, "dup": {"handle"}, "fstat": {"handle"}, "read": {"path"},
    "pread": {"handle", "offset", "count"}, "pwrite": {"handle", "offset", "data_hex"},
    "seek": {"handle", "displacement", "whence"},
    "read_next": {"handle", "count"}, "write_next": {"handle", "data_hex"},
    "truncate": {"path", "size"}, "ftruncate": {"handle", "size"}, "append": {"path", "data_hex"},
    "stat": {"path"}, "lstat": {"path"}, "listdir": {"path"},
    "link": {"source", "target"}, "rename": {"source", "target"}, "symlink": {"target", "path"},
    "rename_exclusive": {"source", "target"}, "rename_exchange": {"source", "target"},
    "readlink": {"path"}, "unlink": {"path"}, "rmdir": {"path"}, "chmod": {"path", "mode"},
    "utime": {"path", "atime_ns", "mtime_ns"}, "xattr_set": {"path", "name", "value_hex", "policy"},
    "xattr_get": {"path", "name"}, "xattr_remove": {"path", "name"},
    "mkdir_at": {"handle", "name"}, "snapshot": {"name"}, "restart": set(), "sync": {"handle"},
    "flock": {"handle", "lock_mode"}, "rpc_lock": {"handle", "lock_mode"},
    "rpc_range_lock": {"handle", "lock_mode", "start", "length"},
    "rpc_seek_region": {"path", "offset", "region"}, "rpc_hseek_region": {"handle", "offset", "region"},
    "rpc_dir_open": {"path"}, "rpc_dir_page": {"cursor", "limit"},
    "rpc_dir_rewind": {"cursor"}, "rpc_dir_close": {"cursor"},
    "volume_name": set(), "set_volume_name": {"name"},
}
OPTIONAL_ARGS = {op: OPS[op] - REQUIRED_ARGS[op] for op in OPS}
REVISION_OPS = {"mkdir", "create", "open", "read", "pread", "pwrite", "read_next", "write_next",
                "truncate", "ftruncate", "append", "stat", "lstat", "listdir", "link", "rename",
                "symlink", "readlink", "unlink", "rmdir", "chmod", "utime", "xattr_set", "xattr_get",
                "xattr_remove", "mkdir_at", "rpc_lock", "rpc_range_lock", "rpc_seek_region", "rpc_hseek_region",
                "volume_name", "set_volume_name"}
EXPECT_FOR_OP = {
    "mkdir": set(), "create": {"written"}, "open": {"handle"}, "close": set(),
    "dup": {"handle"}, "fstat": {"inode", "type", "links", "size", "mode", "mtime_ns", "atime_ns", "allocated_size"},
    "read": {"data_hex"}, "pread": {"data_hex"}, "pwrite": {"written"},
    "seek": {"offset"}, "read_next": {"data_hex", "offset"}, "write_next": {"written", "offset"},
    "truncate": set(), "ftruncate": set(), "append": {"written"},
    "stat": {"inode", "type", "links", "size", "mode", "mtime_ns", "atime_ns", "allocated_size"},
    "lstat": {"inode", "type", "links", "size", "mode", "mtime_ns", "atime_ns", "allocated_size", "target"},
    "listdir": {"names"}, "link": set(), "rename": set(), "symlink": set(),
    "rename_exclusive": set(), "rename_exchange": set(),
    "readlink": {"target"}, "unlink": set(), "rmdir": set(), "chmod": set(), "utime": set(),
    "xattr_set": set(), "xattr_get": {"value_hex"}, "xattr_remove": set(),
    "mkdir_at": {"inode"}, "snapshot": set(), "restart": set(), "sync": set(),
    "flock": set(), "rpc_lock": {"granted"}, "rpc_range_lock": {"granted"},
    "rpc_seek_region": {"offset"}, "rpc_hseek_region": {"offset"},
    "rpc_dir_open": {"cursor"}, "rpc_dir_page": {"ordered_names", "eof"},
    "rpc_dir_rewind": set(), "rpc_dir_close": set(), "volume_name": {"name"}, "set_volume_name": {"name"},
}


def validate_case_file(doc):
    _keys(doc, {"schema_version", "cases"}, set(), "cases root")
    if doc["schema_version"] != 1 or not isinstance(doc["cases"], list) or not doc["cases"]:
        raise SchemaError("cases schema_version must be 1 and cases must be nonempty")
    seen = set()
    for index, case in enumerate(doc["cases"]):
        where = f"cases[{index}]"
        _keys(case, {"id", "families", "profiles", "layers", "mandatory", "fixture", "steps"},
              {"known_gap"}, where)
        cid = case["id"]
        if not isinstance(cid, str) or not SYMBOL.fullmatch(cid) or cid in seen:
            raise SchemaError(f"{where}: invalid or duplicate id {cid!r}")
        seen.add(cid)
        if not isinstance(case["mandatory"], bool) or not case["mandatory"]:
            raise SchemaError(f"{where}: first release requires mandatory=true")
        for key, allowed in (("profiles", PROFILES), ("layers", LAYERS)):
            values = case[key]
            if not isinstance(values, list) or not values or any(not isinstance(x, str) for x in values):
                raise SchemaError(f"{where}.{key}: invalid values")
            if len(values) != len(set(values)) or not set(values) <= allowed:
                raise SchemaError(f"{where}.{key}: invalid, empty, or duplicate values")
        if ("reference" in case["profiles"] or "mounted" in case["profiles"]) != ("posix" in case["layers"]):
            raise SchemaError(f"{where}: reference/mounted profiles require posix layer")
        if ("daemon" in case["profiles"]) != ("rpc" in case["layers"]):
            raise SchemaError(f"{where}: daemon profile requires rpc layer")
        if not isinstance(case["families"], list) or not case["families"] or any(not isinstance(x, str) or not SYMBOL.fullmatch(x) for x in case["families"]):
            raise SchemaError(f"{where}.families must be nonempty family names")
        if len(set(case["families"])) != len(case["families"]):
            raise SchemaError(f"{where}: duplicate family")
        if not isinstance(case["fixture"], list) or not isinstance(case["steps"], list) or not case["steps"]:
            raise SchemaError(f"{where}: fixture and nonempty steps must be arrays")
        if "known_gap" in case:
            _keys(case["known_gap"], {"issue", "reason"}, set(), f"{where}.known_gap")
            if not all(isinstance(v, str) and v for v in case["known_gap"].values()):
                raise SchemaError(f"{where}.known_gap fields must be nonempty strings")
        fixture_paths = set()
        for fi, item in enumerate(case["fixture"]):
            fw = f"{where}.fixture[{fi}]"
            if not isinstance(item, dict) or item.get("type") not in {"directory", "file", "symlink"}:
                raise SchemaError(f"{fw}: unknown fixture type")
            typ = item["type"]
            required = {"type", "path"} | ({"data_hex"} if typ == "file" else {"target"} if typ == "symlink" else set())
            optional = {"mode"} if typ in {"directory", "file"} else set()
            _keys(item, required, optional, fw)
            path = _safe_relpath(item["path"], fw + ".path")
            if path in fixture_paths:
                raise SchemaError(f"{fw}: duplicate fixture path")
            fixture_paths.add(path)
            if typ == "file":
                _hex(item["data_hex"], fw + ".data_hex")
            if "mode" in item and (type(item["mode"]) is not int or not 0 <= item["mode"] <= 0o7777):
                raise SchemaError(f"{fw}.mode must be an integer with POSIX mode bits")
            if typ == "symlink":
                target = item["target"]
                if not isinstance(target, str) or not target or target.startswith("/") or "\x00" in target:
                    raise SchemaError(f"{fw}: symlink target must be safe and relative")
                depth = len(path.split("/")) - 1
                for part in target.split("/"):
                    if part == "..":
                        depth -= 1
                        if depth < 0:
                            raise SchemaError(f"{fw}: symlink target escapes owned root")
                    elif part not in ("", "."):
                        depth += 1
        symbols = {}
        for si, step in enumerate(case["steps"]):
            sw = f"{where}.steps[{si}]"
            _keys(step, {"op", "args", "expect"}, {"save"}, sw)
            op = step["op"]
            if not isinstance(op, str) or op not in OPS:
                raise SchemaError(f"{sw}: unknown operation {op!r}")
            if op == "flock" and set(case["layers"]) != {"posix"}:
                raise SchemaError(f"{sw}: flock is POSIX-only")
            if op in {"rpc_lock", "rpc_range_lock"} and set(case["layers"]) != {"rpc"}:
                raise SchemaError(f"{sw}: {op} is RPC-only")
            if op in {"rpc_seek_region", "rpc_hseek_region"} and (set(case["profiles"]) != {"daemon"} or set(case["layers"]) != {"rpc"}):
                raise SchemaError(f"{sw}: {op} is daemon/RPC-only")
            if op in {"volume_name", "set_volume_name"} and (set(case["profiles"]) != {"daemon"} or set(case["layers"]) != {"rpc"}):
                raise SchemaError(f"{sw}: volume-name operations are daemon/RPC-only")
            if op.startswith("rpc_dir_") and (set(case["profiles"]) != {"daemon"} or set(case["layers"]) != {"rpc"}):
                raise SchemaError(f"{sw}: directory cursor operations are daemon/RPC-only")
            if not isinstance(step["args"], dict):
                raise SchemaError(f"{sw}.args must be an object")
            required = REQUIRED_ARGS[op]
            optional = OPTIONAL_ARGS[op] | ({"revision"} if op in REVISION_OPS else set())
            _keys(step["args"], required, optional, sw + ".args")
            for key, value in step["args"].items():
                _ref_or(value, sw + ".args." + key)
                if isinstance(value, dict) and key not in {"handle", "cursor"}:
                    raise SchemaError(f"{sw}.args.{key}: references are allowed only for owned handles or cursors")
                if isinstance(value, dict) and value["ref"] not in symbols:
                    raise SchemaError(f"{sw}: forward or unknown ref {value['ref']!r}")
                if isinstance(value, dict) and key in {"handle", "cursor"} and symbols[value["ref"]] != key:
                    raise SchemaError(f"{sw}: {key} ref must come from a successful owned {key} result")
                if key == "revision" and (set(case["profiles"]) != {"daemon"} or set(case["layers"]) != {"rpc"} or not isinstance(value, str) or not value):
                    raise SchemaError(f"{sw}: revision selector is daemon/RPC-only")
                if key in {"path", "source"} and isinstance(value, str):
                    allow_dot = key == "path" and value == "." and op in {"mkdir", "stat", "lstat", "listdir"}
                    _safe_relpath(value, sw + ".args." + key, allow_dot)
                if key == "target" and op in {"link", "rename", "rename_exclusive", "rename_exchange"} and isinstance(value, str):
                    _safe_relpath(value, sw + ".args.target")
                if key == "target" and isinstance(value, str) and op == "symlink":
                    if not value or value.startswith("/") or "\x00" in value:
                        raise SchemaError(f"{sw}: symlink target must be relative and safe")
                if key in {"path", "source", "target", "name", "policy", "revision"} and not isinstance(value, str):
                    raise SchemaError(f"{sw}.args.{key} must be a string")
                if key in {"handle", "cursor"} and (not isinstance(value, dict) or value.get("field") != key):
                    raise SchemaError(f"{sw}.args.{key} must reference an owned {key}")
                if key in {"data_hex", "value_hex"}:
                    _hex(value, sw + ".args." + key)
                if key in {"mode", "size", "offset", "count", "limit", "atime_ns", "mtime_ns"} and not isinstance(value, dict) and type(value) is not int:
                    raise SchemaError(f"{sw}.args.{key} must be an integer")
                if key in {"mode", "size", "count", "atime_ns", "mtime_ns"} and type(value) is int and value < 0:
                    raise SchemaError(f"{sw}.args.{key} must be nonnegative")
                if key == "mode" and type(value) is int and value > 0o7777:
                    raise SchemaError(f"{sw}.args.mode exceeds POSIX mode bits")
                if key == "exclusive" and type(value) is not bool:
                    raise SchemaError(f"{sw}.args.exclusive must be boolean")
                if key in {"name", "policy", "flags"} and not isinstance(value, dict):
                    if key == "flags":
                        if not isinstance(value, list) or any(not isinstance(flag, str) for flag in value):
                            raise SchemaError(f"{sw}.args.flags must be a string array")
                    elif not isinstance(value, str): raise SchemaError(f"{sw}.args.{key} must be a string")
            if op in {"flock", "rpc_lock", "rpc_range_lock"} and step["args"]["lock_mode"] not in ("shared", "exclusive", "unlock"):
                raise SchemaError(f"{sw}.args.lock_mode is invalid")
            if op == "rpc_range_lock":
                start, length = step["args"]["start"], step["args"]["length"]
                if type(start) is not int or type(length) is not int or not 0 <= start <= (1 << 63) - 1 or not 0 <= length <= (1 << 63) - start:
                    raise SchemaError(f"{sw}: invalid signed offset lock range")
            if op in {"rpc_seek_region", "rpc_hseek_region"}:
                offset, region = step["args"]["offset"], step["args"]["region"]
                if type(offset) is not int or not 0 <= offset <= FILE_SIZE_LIMIT:
                    raise SchemaError(f"{sw}.args.offset must be a nonnegative bounded literal offset")
                if not isinstance(region, str) or region not in ("data", "hole"):
                    raise SchemaError(f"{sw}.args.region must be data or hole")
            if op == "rpc_dir_page":
                limit = step["args"]["limit"]
                if type(limit) is not int or not 1 <= limit <= 1024:
                    raise SchemaError(f"{sw}.args.limit must be an integer from 1 through 1024")
            if op == "seek":
                displacement = step["args"]["displacement"]
                if type(displacement) is not int or not -(1 << 63) <= displacement < (1 << 63):
                    raise SchemaError(f"{sw}.args.displacement must be a signed 64-bit integer")
                if step["args"]["whence"] not in ("set", "cur", "end"):
                    raise SchemaError(f"{sw}.args.whence must be set, cur, or end")
            if op == "open":
                flags = step["args"]["flags"]
                if not isinstance(flags, list) or not flags or any(not isinstance(x, str) for x in flags) or len(flags) != len(set(flags)) or not set(flags) <= {"read", "write", "create", "exclusive", "truncate", "append", "directory", "nofollow"}:
                    raise SchemaError(f"{sw}.args.flags is invalid")
                if not ({"read", "write"} & set(flags)):
                    raise SchemaError(f"{sw}.args.flags requires read or write")
            if op == "xattr_set" and step["args"]["policy"] not in {"create", "replace", "upsert"}:
                raise SchemaError(f"{sw}.args.policy is invalid")
            if op == "mkdir_at" and (not isinstance(step["args"]["name"], str) or not step["args"]["name"] or "\x00" in step["args"]["name"] or "/" in step["args"]["name"] or step["args"]["name"] in {".", ".."}):
                raise SchemaError(f"{sw}.args.name must be one path component")
            exp = step["expect"]
            if not isinstance(exp, dict) or not set(exp) <= EXPECT_KEYS or "errno" not in exp:
                raise SchemaError(f"{sw}.expect requires errno and only closed assertion keys")
            if type(exp["errno"]) not in (str, int, dict):
                raise SchemaError(f"{sw}.expect.errno has invalid form")
            ev = exp["errno"]
            if type(ev) is int and ev != 0:
                raise SchemaError(f"{sw}: numeric errno may only be success 0")
            if isinstance(ev, str) and ev not in ERRNOS:
                raise SchemaError(f"{sw}: unknown errno symbol {ev}")
            if isinstance(ev, dict):
                _keys(ev, {"darwin", "linux"}, set(), sw + ".expect.errno")
                if any(not isinstance(x, str) or x not in ERRNOS for x in ev.values()):
                    raise SchemaError(f"{sw}: invalid platform errno map")
            success = type(ev) is int and ev == 0
            if "allocated_size" in exp and (set(case["layers"]) != {"rpc"} or set(case["profiles"]) != {"daemon"}):
                raise SchemaError(f"{sw}: allocated_size is only available from daemon RPC")
            if success and op == "rpc_dir_page" and not {"ordered_names", "eof"} <= set(exp):
                raise SchemaError(f"{sw}: successful directory page must assert names and eof")
            if success and op in {"rpc_lock", "rpc_range_lock"} and "granted" not in exp:
                raise SchemaError(f"{sw}: successful RPC lock must assert granted")
            if success and op in {"rpc_seek_region", "rpc_hseek_region"} and "offset" not in exp:
                raise SchemaError(f"{sw}: successful seek-region query must assert offset")
            if success and op in {"volume_name", "set_volume_name"} and "name" not in exp:
                raise SchemaError(f"{sw}: successful volume-name operation must assert name")
            if not success and exp.get("unchanged") is not True:
                raise SchemaError(f"{sw}: expected failures must require unchanged=true")
            for key, value in exp.items():
                if key != "errno":
                    _ref_or(value, sw + ".expect." + key)
                    if key not in {"unchanged", "inode_not"} and key not in EXPECT_FOR_OP[op]:
                        raise SchemaError(f"{sw}: {key} is not a result field for {op}")
                    if key == "granted" and type(value) is not bool:
                        raise SchemaError(f"{sw}.expect.granted must be a literal boolean")
                    if key == "eof" and type(value) is not bool:
                        raise SchemaError(f"{sw}.expect.eof must be a literal boolean")
                    if key == "unchanged" and value is not True:
                        raise SchemaError(f"{sw}: unchanged must be true")
                    if isinstance(value, dict) and value["ref"] not in symbols:
                        raise SchemaError(f"{sw}: forward or unknown assertion ref {value['ref']!r}")
                    if key in {"data_hex", "value_hex"} and isinstance(value, str):
                        _hex(value, sw + ".expect." + key)
                    if key in {"names", "ordered_names"} and (not isinstance(value, list) or any(not isinstance(x, str) for x in value)):
                        raise SchemaError(f"{sw}.expect.{key} must be string array")
                    if key == "type" and value not in {"file", "directory", "symlink"}:
                        raise SchemaError(f"{sw}.expect.type is invalid")
                    if key in {"inode", "inode_not", "links", "size", "mode", "mtime_ns", "atime_ns", "offset", "written", "allocated_size"} and type(value) is int and value < 0:
                        raise SchemaError(f"{sw}.expect.{key} must be nonnegative")
                    if key in {"links", "size", "mode", "mtime_ns", "atime_ns", "offset", "written", "allocated_size"} and not isinstance(value, dict) and type(value) is not int:
                        raise SchemaError(f"{sw}.expect.{key} must be an integer or result ref")
                    if key in {"inode", "inode_not"} and not isinstance(value, dict) and type(value) not in (int, str):
                        raise SchemaError(f"{sw}.expect.{key} must be an integer, opaque ID, or result ref")
                    if key in {"data_hex", "value_hex"} and not isinstance(value, (str, dict)):
                        raise SchemaError(f"{sw}.expect.{key} must be hex or result ref")
                    if key in {"target", "type", "name"} and not isinstance(value, str):
                        raise SchemaError(f"{sw}.expect.{key} must be a string")
            if "inode" in exp and "inode_not" in exp:
                raise SchemaError(f"{sw}: inode and inode_not are mutually exclusive")
            if "save" in step:
                sym = step["save"]
                if not isinstance(sym, str) or not SYMBOL.fullmatch(sym) or sym in symbols:
                    raise SchemaError(f"{sw}: invalid or duplicate save symbol")
                produces = op == "rpc_dir_open" or op in {"open", "dup"}
                if produces and not success:
                    raise SchemaError(f"{sw}: owned resource save requires successful operation")
                symbols[sym] = "cursor" if op == "rpc_dir_open" and success else "handle" if op in {"open", "dup"} and success else ""
    return {case["id"]: case for case in doc["cases"]}


def _hex(value, where):
    if not isinstance(value, str) or len(value) % 2 or not re.fullmatch(r"[0-9a-fA-F]*", value):
        raise SchemaError(f"{where} must be even-length hexadecimal bytes")


def validate_coverage(doc, cases):
    _keys(doc, {"schema_version", "families"}, set(), "coverage root")
    if doc["schema_version"] != 1 or not isinstance(doc["families"], dict) or not doc["families"]:
        raise SchemaError("coverage schema_version must be 1 and families nonempty")
    declared = set()
    for family, item in doc["families"].items():
        if not isinstance(family, str): raise SchemaError("coverage family names must be strings")
        if not SYMBOL.fullmatch(family):
            raise SchemaError(f"invalid coverage family {family!r}")
        _keys(item, set(), {"case_ids", "existing_external", "missing_contracts"}, f"coverage.{family}")
        ids = item.get("case_ids", [])
        if not isinstance(ids, list) or any(not isinstance(x, str) for x in ids) or len(ids) != len(set(ids)):
            raise SchemaError(f"coverage.{family}.case_ids must be unique")
        for cid in ids:
            if cid not in cases or family not in cases[cid]["families"]:
                raise SchemaError(f"coverage dangling or mismatched case id {cid!r} in {family}")
            declared.add(cid)
        for entry in item.get("existing_external", []):
            _keys(entry, {"id", "layer", "script", "status"}, {"scenarios"}, f"coverage.{family}.existing_external")
            if not all(isinstance(entry[k], str) and entry[k] for k in ("id", "layer", "script", "status")):
                raise SchemaError(f"coverage.{family}: invalid external inventory entry")
            if entry["layer"] not in LAYERS | {"daemon_core", "daemon_cli"} or entry["status"] != "not_run":
                raise SchemaError(f"coverage.{family}: external suites must be not_run and identify a layer")
            scenarios = entry.get("scenarios", [])
            if not isinstance(scenarios, list) or any(not isinstance(x, str) or not x for x in scenarios):
                raise SchemaError(f"coverage.{family}: scenarios must be strings")
        for entry in item.get("missing_contracts", []):
            _keys(entry, {"id", "layer", "profiles", "mandatory", "reason"}, set(), f"coverage.{family}.missing_contracts")
            profiles = entry["profiles"]
            if entry["layer"] not in LAYERS or not isinstance(profiles, list) or not profiles or any(not isinstance(x, str) for x in profiles) or not set(profiles) <= PROFILES or not isinstance(entry["mandatory"], bool) or not isinstance(entry["reason"], str) or not entry["reason"]:
                raise SchemaError(f"coverage.{family}: invalid missing contract entry")
        if not ids and not item.get("existing_external") and not item.get("missing_contracts"):
            raise SchemaError(f"coverage.{family}: empty family has no frontier entry")
    for family, item in doc["families"].items():
        expected = {cid for cid, case in cases.items() if family in case["families"]}
        if set(item.get("case_ids", [])) != expected:
            raise SchemaError(f"coverage family mapping mismatch for {family}: expected={sorted(expected)}")
    if any(f not in doc["families"] for case in cases.values() for f in case["families"]):
        raise SchemaError("case references a family absent from coverage")
    return doc["families"]


def resolve_refs(value, saved):
    if isinstance(value, dict) and set(value) == REF_FIELD:
        try:
            return saved[value["ref"]][value["field"]]
        except KeyError as exc:
            raise SchemaError(f"unresolved reference {value}") from exc
    return value
