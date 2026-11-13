# forkfsd metadata engine

The default backend is upstream LevelDB 1.23, pinned as a submodule. A repository
is a private directory (mode 0700), containing LevelDB's WAL, MANIFEST and SST
files. No SQLite is used. This backing directory is not a World exposed to users;
The optional macOS FSKit frontend connects over RPC; see [setup and limits](../docs/FSKIT.md).

LevelDB owns WAL, memtable, Bloom filters, block cache and physical compaction.
A repository-wide 8 MiB LRU reuses verified immutable payloads across namespace
operations; LevelDB also has a 16 MiB block cache. Immutable content-addressed
objects and a persistent CoW metadata tree provide
World/revision sharing. Head updates and their objects commit in one synchronous
WriteBatch. See [the backend design](../docs/LEVELDB_METADATA_DESIGN.md).

Old single-file containers remain readable. The legacy test binaries create that
format to preserve regression coverage; new `forkfsd init` creates a directory.
This is the supported runtime. The native-directory `world` / `worldfs_core` backend is retired.

## Build and test

```sh
git submodule update --init --recursive
cmake -S . -B build/forkfsd -DCMAKE_BUILD_TYPE=Debug
cmake --build build/forkfsd
ctest --test-dir build/forkfsd -R forkfsd --output-on-failure
```

The prototype implements POSIX macOS/Linux paths; only macOS has been exercised
in this development session. Windows support is not implemented.

## Run

```sh
build/forkfsd/daemon/forkfsd init project.forkfs
build/forkfsd/daemon/forkfsd check project.forkfs
mkdir -m 700 runtime
build/forkfsd/daemon/forkfsd serve project.forkfs --socket "$PWD/runtime/control.sock"
```

`serve` stays in the foreground for a process supervisor. SIGINT/SIGTERM closes
its owned socket and releases the container lock. Each socket connection receives
one JSON status line and EOF in the default status-only mode. Add `--rpc` for
namespace requests; the protocols are separate. Use a short socket path because
Unix-domain socket pathname limits apply.

The runtime directory must already exist, be owned by the service user, and deny
access to other users. The socket authorizes that OS identity, not individual
agents: a privileged broker/sandbox boundary is still needed before exposing a
write-capable control protocol. Parent paths must be managed by that identity.

Existing socket paths are never automatically removed. After a crash, confirm
that the previous service has exited before removing its stale socket path.
A competing process cannot acquire the same repository. LevelDB uses its LOCK
file for directory ownership; legacy containers use an advisory file lock.
These are cooperative local locks; they do not protect against raw file writes
by another process with access to the container.

## Resident namespace operations

```sh
build/forkfsd/daemon/forkfsd serve project.forkfs --socket "$PWD/runtime/control.sock" --rpc
# In another terminal:
build/forkfsd/daemon/forkfsd request "$PWD/runtime/control.sock" fs-init
build/forkfsd/daemon/forkfsd request "$PWD/runtime/control.sock" fs-mkdir /src
build/forkfsd/daemon/forkfsd request "$PWD/runtime/control.sock" fs-write /src/main.txt input.txt
build/forkfsd/daemon/forkfsd request "$PWD/runtime/control.sock" fs-cat /src/main.txt
build/forkfsd/daemon/forkfsd request "$PWD/runtime/control.sock" fs-snapshot base
build/forkfsd/daemon/forkfsd request "$PWD/runtime/control.sock" fs-fork base work
build/forkfsd/daemon/forkfsd request "$PWD/runtime/control.sock" fs-stat /src/main.txt --world work
```

The client reads the input file for `fs-write`, `fs-write-at` and `fs-append`
and sends its bytes, including NUL;
the server never opens a host input path from an RPC request. Namespace commands
run against the resident Container, so LevelDB and verified-object caches survive
between requests. Offline commands still require exclusive repository ownership.

RPC accepts same-UID peers only, caps connections at 16, bounds payloads at 1 MiB,
and closes incomplete/slow connections after five seconds. Network I/O uses a
poll loop; namespace operations and local synchronous commits execute serially.
Long storage operations can delay other requests and shutdown; the I/O timeout
does not cancel an executing transaction. This is an experimental local control
interface. The optional FSKit client uses its inode/session operations; this is not an agent sandbox.

Each connection carries one request/reply. Request magic `FFRPC001` and reply
magic `FFRSP001` are followed by payload length u32 LE. Request payload is field
count u32 LE (1..16), then length u32 LE + bytes for each field. First field is the
command; write payload is binary. Reply payload starts with result code u32 LE
(0 success, 1 generic error, or POSIX errno + 1 for typed errors) followed by raw result/error bytes. The format is experimental.
Supported commands are `status`, `fs-init`, `fs-mkdir`, `fs-write`, `fs-cat`,
`fs-xattr-list`, `fs-xattr-get`, `fs-xattr-set`, `fs-xattr-remove`,
`fs-settimes`, `fs-lsettimes`, `fs-lchmod`, `fs-history`, `fs-diff`, `fs-ls`, `fs-stat`, `fs-read-at`, `fs-write-at`, `fs-append`, `fs-truncate`, `fs-link`, `fs-rename`, `fs-unlink`, `fs-rmdir`, `fs-check`,
`fs-compact`, `fs-snapshot`, `fs-fork`, `fs-restore`, `fs-worlds` and `fs-revisions`, with the
same view selectors as offline commands. `fs-diff` and `fs-history` instead take
explicit named revisions and reject view selectors. Revision writes remain forbidden.

## Extended attributes

```sh
forkfsd fs-xattr-set project.forkfs /file user.note upsert value.bin
forkfsd fs-xattr-get project.forkfs /file user.note > saved.bin
forkfsd fs-xattr-list project.forkfs /file
forkfsd fs-xattr-remove project.forkfs /file user.note
forkfsd request /private/runtime/control.sock fs-xattr-set /file user.note create value.bin
```

Paths address the final inode without following its final symlink. Regular files,
directories and symlinks have independent xattrs; hardlinks share their inode's
attributes. Names are 1..255 bytes of valid UTF-8, without normalization, excluding
C0/DEL/C1 controls. A value can be empty or binary, up to 262,143 bytes, and an inode can
have at most 128 attributes. Native macOS may impose tighter name restrictions.
Set policy is mandatory: `upsert`, `create` (EEXIST if present), or `replace`
(ENOATTR/ENODATA if absent). Removing a missing name also fails. Invalid input
and quota failures publish no partial changes.

CLI get writes raw bytes, list writes one name per line, and set reads INPUT_FILE.
RPC commands are `fs-xattr-list PATH`, `fs-xattr-get PATH NAME`,
`fs-xattr-set PATH NAME POLICY VALUE` and `fs-xattr-remove PATH NAME`; RPC list
returns a JSON `names` array. VALUE is raw binary data, never a server-side host
path. The request CLI loads the input file on the client. Existing World/revision
selectors apply; revision mutation remains forbidden.

Session-scoped `fs-hxattr-list/get/set/remove` use a file handle instead of a path,
including an unlinked inode. These are owner metadata operations and do not
require a writable data handle. `inode-xattr-list/get/set/remove` use linked inode
IDs and require a mounted session, returning typed errors for FSKit. All variants
commit synchronously, modify ctime and preserve mtime, mode and file contents.
Snapshot/fork, rename, replacement, final close, lease expiry, recovery and GC
retain or release metadata alongside the associated inode. A replacement inode
starts with its own attributes; old open handles retain the replaced inode's set.

The shared daemon/FSKit limit is 262,143 bytes per value, including whole-value
`com.apple.ResourceFork` operations. Values remain single immutable objects;
this does not provide arbitrarily large or streamed resource forks. Native
ResourceFork `cp -p` and position reads still fail with EINVAL on the tested macOS
27.2 mount; ordinary large xattrs copy successfully. The per-inode limit is 128 names and 8 MiB of raw value bytes. Names include
system-added attributes such as `com.apple.provenance`. Equal content IDs count
separately per name. This logical metadata budget bounds updates; it is not an
exclusive disk-usage bill or reserved host capacity. Both count and byte-budget
validation failures return ENOSPC without poisoning storage, so freeing a slot
or reducing a value allows an immediate retry. Length accounting uses immutable
object-size indexes, with verified payload fallback for old unindexed objects.
Existing repositories with at most 32 values of at most 262,143 bytes fit the new
budget. Older daemons reject lists/verification above 32 names; use the updated
daemon after expanding an inode's attribute set. Existing values up to 64 KiB need no rewrite. Older
daemons/frontends reject larger values; use the updated pair for repositories
containing them. No inode/manifest encoding or repository format change is
introduced by this bound increase. Updates still publish the complete value and
inode atomically, and revisions retain the original object IDs. Attributes do
not contribute to the file-data `allocated_size` statistic.

Values live at `\0forkfs/xattr/INODE/NAME` as ordinary CoW leaves. The inode's
encoded mode word reserves bit 31 for attribute presence; the public mode remains
0000..0777. Older daemons reject that mode marker, and the last removal clears it.
Existing stores need no conversion. Orphan attributes use separate retention roots
until final close/restart, and fs-check verifies their bounds and presence marker.
Revision diff compares attribute names and object IDs independently of ctime,
reports `xattrs_changed`, and includes that result in `metadata_changed` without
claiming file-content changes. History continues to traverse retained revisions.

## Restore a World

```sh
# Unmount the selected World and close its mutable file handles first.
forkfsd fs-restore project.forkfs known-good safety-before-restore --world main
forkfsd fs-snapshot project.forkfs restored --world main
forkfsd fs-diff project.forkfs known-good restored
# Undo the restore, retaining another safety revision for the current state.
forkfsd fs-restore project.forkfs safety-before-restore safety-before-undo --world main
forkfsd request /private/runtime/control.sock fs-restore known-good safety-via-rpc --world main
```

Syntax is `fs-restore CONTAINER TARGET_REVISION SAFETY_REVISION [--world NAME]`;
RPC omits CONTAINER. The World defaults to main and must already exist and be
initialized. TARGET_REVISION must be a retained named revision; SAFETY_REVISION
must be a new valid label. Revision views reject restoration.

Under one namespace lock, restore checks that the World has no open file handles,
verifies the complete target namespace and prepares a safety snapshot of the
World's current committed root. One synchronous transaction publishes that safety
revision, advances the World's retained base to it and switches the World head to
the target manifest. Validation failures publish none of these changes. A process
crash exposes either the previous state with no new safety label or the restored
state with the complete safety revision. Restore uses immutable manifest references
and preserves file IDs, hardlinks, directories, symlink targets, contents, modes,
timestamps and xattrs. Full target verification reads the referenced namespace;
this is not a constant-time operation or physical corruption repair.

The JSON reply identifies `safety_revision_id`, `target_revision_id` and
`restored_manifest_id`. The safety revision captures un-snapshotted committed
changes and retains the World's previous parent revision. A subsequent snapshot
of restored data has the safety revision as its parent, preserving the World
history. The target revision and other World heads retain their own graphs. A
second restore can select the safety revision to undo the operation; it also
requires a new safety label.

Open mutable-World file handles, including read-only access handles and unlinked
inodes, block restoration. Handles in other Worlds and immutable revision handles
remain usable. Captured directory cursors keep their original immutable pages.
RPC rejects an active mutable mount lease even when the caller owns it. Unmount
FSKit before using restore: lease expiry does not establish kernel cache
revocation or flush uncommitted application data. The operation uses committed
repository state and keeps the existing FSKit coherence limits.

The operation supports LevelDB and the immutable single-file compatibility
backend. Restore does not automatically expire safety revisions. Explicit label
removal is described below; surviving World bases and descendant revisions still
pin the data, which physical GC preserves while reachable.

## Revision history

```sh
forkfsd fs-history project.forkfs after-edit
forkfsd fs-history project.forkfs after-edit LAST_RETURNED_REVISION_ID 16
forkfsd request /private/runtime/control.sock fs-history after-edit '' 64
```

Offline syntax is `fs-history CONTAINER TIP_REVISION [AFTER_REVISION_ID LIMIT]`;
RPC syntax is `fs-history TIP_REVISION AFTER_REVISION_ID LIMIT`. TIP_REVISION is
an existing retained revision name, never an implicit mutable World. To include
current World changes, first create a named revision with `fs-snapshot`.
`--world` and `--revision` selectors are rejected for this query. The RPC is a
read-only repository query and needs no file handle or session; an optional
`--session` uses the existing lease validation.

Results contain `tip_revision_id`, `entries`, `next_after` and `eof`. Entries are
newest-to-oldest along that tip's immutable parent chain, with `revision_id`,
`parent_revision_id` (null at the root), `manifest_id` and `captured_sequence`.
The captured sequence is the repository commit sequence observed before the
snapshot descriptor was published, not a timestamp or the publication sequence.
A fork's first revision points to its selected base revision, including when
other Worlds have since committed newer revisions. A snapshot with unchanged
namespace content remains a separate revision, with a shared manifest ID.

Pages contain at most LIMIT entries (default/maximum 64, minimum 1). Pass
`next_after` as the exclusive AFTER_REVISION_ID while `eof` is false; retain the
same named tip while paging. The final page has an empty `next_after`. Cursors
must be canonical lowercase 64-digit object IDs in that tip's ancestry; malformed
or unrelated cursors fail instead of silently jumping to another branch. The
parent of each entry is followed by immutable object ID, not by registry-name
sorting or mutable World state. Responses fit the existing 1 MiB frame limit.

History reads commit no new revision or repository mutation. Later World writes
and snapshots do not change an existing tip's pages. Restart and physical GC
preserve the chain because retained descriptors pin their ancestors. This query
supports both LevelDB and the legacy immutable single-file container format;
physical GC remains LevelDB-only.

The query reports explicit snapshot ancestry, not every write, an event log,
per-file history or the children of a revision. Explicit offline label removal
does not rewrite ancestry or truncate this history. Stateless pages rewalk the chain from the named tip to
validate the cursor; time and memory can grow with ancestry depth. Reads run
under the namespace lock. Use `check`/`fs-check` for full object/namespace integrity
verification.

## Revision diff

```sh
forkfsd fs-snapshot project.forkfs before
# Edit main (or snapshot a selected World with --world NAME).
forkfsd fs-snapshot project.forkfs after
forkfsd fs-diff project.forkfs before after
forkfsd fs-diff project.forkfs before after /last-returned-path 16
forkfsd request /private/runtime/control.sock fs-diff before after '' 64
```

Offline syntax is `fs-diff CONTAINER BEFORE_REVISION AFTER_REVISION [AFTER_PATH
LIMIT]`; RPC syntax is `fs-diff BEFORE_REVISION AFTER_REVISION AFTER_PATH LIMIT`.
Both names must exist in the revision registry. World names and `--world` or
`--revision` selectors are not accepted as substitutes. Snapshot a World
explicitly before comparing it. Diff does not publish a revision or change the
repository. The response identifies both revision object IDs.

Each JSON response contains `changes`, `next_after` and `eof`. A page has at most
LIMIT entries (default and maximum 64, minimum 1), ordered by original UTF-8 path
bytes, including `/` when root metadata changes. Pass `next_after` as the exclusive
AFTER_PATH for the next page while `eof` is false. The final page has an empty
`next_after`. Cursors are path positions within the chosen revision pair; retain
that pair while paging. Responses fit the existing 1 MiB RPC frame even at the
4096-byte path limit. Paths exceeding that supported limit fail explicitly.

Entries report `path`, `change` (`added`, `removed`, `modified`), before/after inode
IDs and types (null on the absent side), `metadata_changed` and `content_changed`.
Metadata compares identity, type, mode, link count, size, mtime, ctime, atime,
birth time and xattrs;
`xattrs_changed` identifies changes to attribute names or values. Storage layout
is excluded. Content compares logical bytes, including zero holes, without
using size or timestamp equality as proof. Equal content/chunk object IDs skip
byte reads. Distinct storage representations compare in chunks of at most 64 KiB.
Added/removed entries have metadata changes and content changes for non-directories;
directories have no file content. Type changes also count as content changes when
one side is a non-directory.

Symlinks compare their stored targets and are never followed, including dangling
links or cycles. Each hardlink path is reported independently. A rename appears
as removal/addition, with stable inode IDs available for correlation; diff does
not infer renames or emit text patches. Directory removal/addition includes its
descendant paths. This is not an integrity check: use `check` and `fs-check` to
validate object/namespace integrity.

The first implementation skips an identical manifest and matching file/chunk
objects, but otherwise walks the namespace. Stateless pages repeat that traversal;
large-tree performance and subtree-level traversal pruning are not qualified.
Diff executes under the daemon's namespace lock and can delay other requests.

Offset commands are `fs-read-at PATH OFFSET COUNT`,
`fs-write-at PATH OFFSET INPUT_FILE`, `fs-append PATH INPUT_FILE` and
`fs-truncate PATH SIZE` in both offline and RPC modes. Offsets/sizes use decimal
numbers. These commands require an existing regular file. Offset writes preserve
the tail, extend with zero fill, and atomically publish content with inode size;
append chooses EOF under the same transaction lock and returns offset/written
JSON. Reads stop at EOF. Truncate growth zero fills; shrinking discards the tail.
Empty writes/appends and unchanged truncate do not commit or update timestamps.
Read-only revision restrictions and hardlink identity apply to all operations.

Files support up to 1 TiB, with 256 KiB per daemon I/O call. Small inline files
retain their original layout; larger files use sparse 64 KiB CoW chunks, so
range operations load only the touched chunks. See the storage layout below.
This is a logical size limit, not a disk reservation. Current large-file
qualification covers 65 MiB of dense data and sparse files through 1 TiB.

Successful mutation replies follow local synchronous commit. A lost reply can
mean the mutation committed: request deduplication is not implemented, and the
client does not automatically retry mutations. Online directory export, paged
persistent/multiplexed transport connections are not implemented; bounded directory
listing is described below.

## Validated inode path hints

Inode RPCs can reuse a recently observed namespace path instead of locating an
inode by traversing all directory entries. Successful stat/lstat/open and a
fallback search seed hints. Each use resolves the path in the **current**
World/revision with no final-symlink following, and compares the requested inode
ID. Directory hints that traverse ancestor symlinks fall back to the physical
directory walk, preserving room for child names when an alias is much longer
than its target. A mismatch, vanished path, changed ancestor type or retargeted/cyclic symlink
falls back to the existing traversal. Hardlinks may return any valid alias;
regular-file inode operations cannot accidentally target the final symlink.

Hints are scoped by view kind/name and inode ID, serialized by the repository
mutex, and globally bounded to 4096 entries per Container with FIFO eviction.
Paths are bounded by the existing 4096-byte limit. They store no object roots,
remain non-authoritative through GC, and disappear on reopen. Allocation failure
while recording a hint is ignored so an already committed operation cannot fail
because of the optimization. No inode, store, manifest or RPC format changes.

The CTest target also offers an optional read-only lookup probe for an **owned,
offline disposable fixture**:

```sh
build/Release/daemon/forkfsd_inode_paths_test /private/tmp/owned-fixture /scale/n000000
```

It seeds with stat, performs ten inode lookups with identity checks and confirms
that the journal sequence is unchanged. This measures the known-path hint case,
not arbitrary cold inode search. Unseeded/evicted/stale hints still require the
existing traversal. See [acceptance evidence](../docs/evidence/2026-11-12-inode-path-hints.json).

## Inode handles and open-unlink

The LevelDB backend supports regular-file and symlink metadata handles through the C++ API
and resident RPC. RPC requires a session (see below).
`fs-open PATH MODE --session SESSION` returns a random opaque token. MODE is
`r`, `w` or `rw`, optionally followed by comma-separated `create`, `exclusive`
`truncate`, `append` and `nofollow` flags. Use the
same session and World/revision selector for subsequent calls:

```text
fs-hstat HANDLE --session SESSION
fs-hread HANDLE OFFSET COUNT --session SESSION
fs-hwrite HANDLE OFFSET INPUT_FILE --session SESSION
fs-happend HANDLE INPUT_FILE --session SESSION
fs-htruncate HANDLE SIZE --session SESSION
fs-close HANDLE --session SESSION
```

RPC writes send bytes; the CLI reads INPUT_FILE locally. Handles target an inode,
so rename and later path reuse do not change their target. After final unlink or
rename replacement, live handles see nlink=0 and can keep reading/writing. Other
handles to that inode see the same updates. Access mode is enforced per handle;
revision handles permit reads only. This is not a replacement for Unix permission
checks, an agent broker or an OS mount adapter.

An unlinked open file retains two durable orphan roots outside the World tree:
`inode` for its current metadata and `tree` for an immutable CoW retention view.
Snapshot/fork omit these orphans. Unlink and orphan publication share one
synchronous WriteBatch; orphan writes synchronously commit metadata and the
private CoW head without changing the World tree root. Last close releases both
retention roots. **It does not itself physically delete immutable objects or recycle an
inode ID. Offline `gc` can collect unreachable immutable objects.** Physical reclamation is available through offline LevelDB `gc`. Random stable inode IDs
are never reused; slot reuse would require generation checks.

Handles expire on daemon restart. Opening the repository releases orphan roots
left by the previous process in bounded batches before serving requests. This
makes restart recovery correct without keeping stale handles alive; it is not
physical object cleanup. Linked files and retained revisions remain unchanged.

There are at most 4096 handles per Container. RPC handles survive one-shot
connection closure, but belong to leased sessions and release on session expiry
or explicit close. A lost open reply consumes a handle slot until session release.
Handle tokens do not survive restart and are scoped to a session and view. Directory handles,
mmap and OS integration remain
unimplemented. The legacy single-file backend does not offer the handle API.

Symlink metadata handles use `OpenOptions::symlink` or the RPC open flag
`fs-open PATH r|w|rw,symlink`. The final symlink is opened without traversing its
target; prefix directory links still resolve. This also works on dangling and
cyclic links. On a regular file the flag opens the ordinary file. It cannot be
combined with create, truncate, append or nofollow; invalid combinations fail
before publication. The flag does not implement symlink creation via open.
`inode-open` opens the explicitly identified symlink as a metadata handle, and
the FSKit adapter supports native O_SYMLINK descriptors.

These handles provide stat, xattr and mode/time operations on the symlink itself.
Readonly data-access handles can change owner metadata; immutable revision
handles cannot. Reading or writing file data, including empty requests, fails
with EPERM; the target text and referenced file are unchanged. Symlink size
attribute updates fail with EINVAL. Ordinary opens continue to follow final
symlinks, and the existing nofollow flag continues to reject them. Seek/dup use
the normal open-description lifetime; seeking does not enable data I/O.

Final unlink or rename replacement moves a live symlink's inode, immutable
link-text object and xattrs into orphan roots atomically. Existing descriptors
retain that identity across path reuse, while snapshot/fork omit the orphan.
Dup, writer-to-reader downgrade, session expiry and last close use the same
retention machinery as regular files. Verification checks orphan link targets;
restart releases stale handles and orphan roots before serving. GC, World
restore and retention keep their existing open-handle guards. No persistent
inode or wire-format version change is needed; handles remain LevelDB-only.

Atomic open options are available through `Namespace::OpenOptions` and RPC:

```text
fs-open /new.txt rw,create,exclusive --session SESSION
fs-open /existing.txt w,truncate --session SESSION
```

Create publishes an empty file and pins its inode under one lock/transaction.
Without exclusive, create opens an existing regular file and preserves its content;
exclusive requires create and rejects any existing name. Truncate requires writable
access and atomically empties the existing inode, visible through its hardlinks and
other live handles. A newly created file is already empty. Truncating an empty file
is a no-op. Create/truncate flags are rejected for revision views.

Runtime handle capacity and options are checked before file mutation. The handle
is reserved before durable publication and rolled back if publication fails.
Successful mutation replies follow sync commit. A lost reply may still mean the
file was created/truncated; mutation deduplication is not implemented. C++ creation
mode defaults to 0644 and can be set to 0000..0777; existing file modes are preserved.
RPC uses 0644. Umask/ACL enforcement remains future work. Concurrent exclusive creates produce one winner.

Append-open mode (`w,append` / `rw,append`, optionally with create) requires
writable access. Each `handle_write` / `fs-hwrite` selects current EOF inside the
same locked transaction, including after unlink/rename replacement; it ignores
the supplied offset for placement. The offset must still be a valid nonnegative
signed-64-bit value. Empty writes are no-ops; full-file bounds use actual EOF.
The mode belongs to the individual handle, not the inode or other handles.
Concurrent append handles preserve every record; explicit `fs-happend` continues
to append regardless of open mode. Truncate remains a length operation.

This is the prototype handle API's append policy. OS write/pwrite/seek mappings still need separate platform validation;
the service cursor/dup subset is described below. File contents still use whole immutable objects with the
256 KiB limit.

The service now offers shared open-file cursors:

```text
fs-dup HANDLE --session SESSION
fs-hseek HANDLE SIGNED_OFFSET set|cur|end --session SESSION
fs-hread-next HANDLE COUNT --session SESSION
fs-hwrite-next HANDLE INPUT_FILE --session SESSION
```

C++ equivalents are dup, seek, handle_read_next and handle_write_next. New open
starts at offset zero; dup shares its cursor, access mode and append policy.
Independent opens have independent cursors. Closing one descriptor leaves its
duplicates usable, including after unlink. Sequential reads advance by bytes
returned, EOF/empty reads do not advance. Sequential writes advance only after
successful sync commit; append writes advance to the actual appended end.
Seek permits positions beyond EOF but rejects negative results and signed-64-bit
overflow. Truncate leaves the cursor unchanged. Existing explicit-offset read,
write and append methods do not move the cursor; explicit write retains its
previous append-open policy. Cursor changes are process-local, not revision data.

RPC duplicates belong to the same session and consume descriptor quota. Shared
cursor access and file mutation run under the Container lock. OS fd duplication,
seek flags and platform syscall mapping remain separate future work.

## Symbolic links

CLI and RPC provide `fs-symlink TARGET PATH`, `fs-readlink PATH` and `fs-lstat PATH`.
Stat/read/write/open follow links; lstat/readlink/unlink/rename act on the final
link itself. Link creation stores target bytes without resolving them, so dangling
and cyclic links are representable. Hardlink's source does not follow the final
symlink. Directory-prefix links are followed in every operation.

Absolute targets begin at the selected World root; relative targets start at the
link's actual containing directory. Dot/dot-dot in targets use physical traversal,
clamping at the World root. No host path is opened to resolve a symlink. Traversal
allows at most 40 followed links and 64 KiB cumulative target expansion. Targets
are 1..4096 bytes of valid UTF-8, component length at most 255 bytes; repeated slashes
and dot components are supported inside targets. A target's trailing slash requires
a directory. Ordinary external paths retain the prototype's strict path rules.

The original inode schema FFINOD01 added type=3 in its 48-byte record; its
content object contains target bytes and size is target byte length. New lstat/stat
JSON includes `symlink`. Revision/fork retain these objects through the same CoW
tree. File handles open the resolved regular inode. Create through a dangling final
symlink creates its missing final target, preserving the link and requiring all
intermediate directories to exist. Whole-file write follows the same creation
rule. Exclusive create rejects the existing link name, even when dangling.
`nofollow` rejects a final symlink before mutation but permits prefix directory
links. Attribute setters can instead update the final entry through `fs-lchmod`
and `fs-lsettimes`, including dangling links. Native O_SYMLINK descriptors use
metadata handles; symlink data I/O and the general OS O_NOFOLLOW mapping remain
unsupported. This World-local resolver
does not constitute the OS/agent isolation boundary.

## Mode and timestamps

Path commands include `fs-chmod PATH OCTAL_MODE`, `fs-setmtime PATH NANOSECONDS`
and `fs-settimes PATH ATIME MTIME BIRTH`. Their nofollow counterparts are
`fs-lchmod PATH OCTAL_MODE` and `fs-lsettimes PATH ATIME MTIME BIRTH`.
CLI commands take CONTAINER before
PATH; RPC commands omit it. A dash (`-`) leaves an individual time unchanged:

```sh
forkfsd fs-settimes project.forkfs /file 123456789 234567890 -
forkfsd request /private/runtime/control.sock fs-settimes /file - - 100000000
```

RPC also provides session-owned `fs-hchmod`, `fs-hsetmtime` and
`fs-hsettimes HANDLE ATIME MTIME BIRTH`, including open-unlinked regular files.
The C++ attribute API sets mode, size, mtime, atime and birth time atomically.
All requested fields are validated before any mutation. The FSKit adapter uses
`inode-setattr-times INODE MODE SIZE MTIME ATIME BIRTH HANDLE`,
`inode-setattr-flags INODE MODE SIZE MTIME ATIME BIRTH FLAGS HANDLE`, where unchanged
fields and an absent handle use `-`. The older `inode-setattr` operation remains
available for existing clients. Inode RPC calls require a mounted session.

Modes are octal 0000..0777. Times are integer nanoseconds since the Unix epoch in
0..INT64_MAX, including zero. New inodes initialize atime and birth time when
created. Metadata updates advance ctime; content/size changes advance mtime,
while atime and birth time remain stable unless explicitly set. Ordinary reads,
stat and directory enumeration do not update atime or create commits (a noatime
policy), including immutable revision reads. Birth time is owner-editable
metadata, not an audit record.

Hardlinks and existing handles see the same attributes; Worlds and immutable
revisions remain isolated. Ordinary path changes follow final symlinks; the
`fs-l*` variants update the final entry itself while still resolving prefix
directory symlinks. Dangling and self-referencing final links need no valid
target. The C++ `set_link_attributes` API has the same behavior and also accepts
regular files and directories. Both inode setattr RPCs always update the inode
identified by the caller, including a symlink, without following it. Symlink
size changes fail with EINVAL, even for the existing length, before any requested
mode or time can change. Directory and
unlinked regular-file and symlink-metadata handle attributes are supported. Live read-only access
handles may set owner metadata; revision handles cannot. An all-dash change is
a no-op after view/handle validation. Negative timestamps, uid/gid changes,
ACLs, setuid/sticky bits and multi-user permission
enforcement remain unsupported.

Small unflagged inode records use the 64-byte `FFINOD02` format. Bytes 8..47 retain the
original type, mode, links, size, mtime and ctime layout; bytes 48..55 and 56..63
hold little-endian atime and birth time. The daemon still reads 48-byte
`FFINOD01` records, using their mtime as the initial atime and ctime as the
initial birth time. Those fallback values do not recover historical creation
times. A normal metadata mutation upgrades only the inode being written; its
fallback times are then persisted. Old revision roots remain unchanged and
mixed-format views are supported, including GC and the single-file backend.
Older daemons reject the new record size/magic; upgraded views require this
daemon even after the last xattr is removed. No in-place whole-store migration
or downgrade is provided.

Unflagged regular files larger than 64 MiB use `FFINOD03`, with the same 64-byte field
layout and chunked type. Readers predating large-file support reject its magic
instead of accidentally editing an unsupported large inode. `FFINOD01`/`02`
remain readable for small files. Shrinking to at most 64 MiB removes extended
chunk keys and writes that inode as `FFINOD02` again; retained large revisions
remain version 3 and still require a compatible daemon.

## Sparse seek queries

RPC provides `fs-seek-region PATH OFFSET data|hole` and session-owned
`fs-hseek-region HANDLE OFFSET data|hole`, returning `{"offset":N}`.
The mount uses `inode-seek-region INODE OFFSET data|hole HANDLE_OR_DASH`, with
inode/handle pairing validation. These queries do not change daemon handle
positions or inode timestamps. Native `lseek` updates the kernel fd position,
shared by dup as usual. Both World and read-only revision queries are supported;
unlinked handles use their retained inode/tree, even after path reuse.

Inline files conservatively report all bytes as data. For chunked files, any
stored chunk is reported as data for its complete 64 KiB interval (clipped to
EOF), including zero padding beyond its trimmed payload. Missing chunk keys
are holes; zero-elided chunks can become holes. Data search starts from the
metadata key; hole search walks contiguous keys in pages of at most 128, without
reading payloads or iterating over hole bytes. Historical flat/sorted-run views
materialize their key list for compatibility and do not have the current CoW
view's bounded tree traversal.

Offsets at or beyond EOF, including offset zero in an empty file, return ENXIO
for either query. A data search with no later data also returns ENXIO; a hole
search with no earlier hole returns EOF. Invalid signed offsets or inode types
are rejected. This requires no inode/store format change and does not reserve
space or implement hole punching. Reported data ranges need not match APFS's
allocation granularity and are not physical device extents.

The FSKit SeekRegion handler is compiled with the macOS 27 SDK and used on
macOS 27. The macOS 26 build retains its previous conservative kernel fallback;
its public Operations API has no SeekRegion callback. Deploy the updated daemon
and module together to use native sparse seeking. Native acceptance and commands
are documented in [FSKIT.md](../docs/FSKIT.md#sparse-seek-regions).

## BSD metadata flags

`fs-stat`, `fs-lstat` and handle/inode RPC attributes expose a numeric `flags`
field. Supported bits are `UF_NODUMP` (1), `UF_IMMUTABLE` (2), and `UF_HIDDEN`
(32768), including combinations. Hidden/nodump are backup/visibility hints;
immutable has the enforcement described below. Other flags, including append-only,
nounlink and system protection bits, fail with ENOTSUP before mutation. The
native kernel may reject privileged flags earlier with EPERM. This is not full
BSD flag support or a multi-user authorization model.

```sh
forkfsd fs-setflags project.forkfs /file 32769
forkfsd fs-lsetflags project.forkfs /link 1  # nofollow
forkfsd fs-setflags project.forkfs /file 0
# Session-owned RPC metadata handle, including open-unlink:
forkfsd request /private/runtime/control.sock fs-hsetflags HANDLE 32768 --session TOKEN
```

Path setters follow the final symlink; the `l` variant changes the link itself.
Hardlinks share flags. Metadata handles keep them through rename, open-unlink,
dup and path reuse. Flags preserve mode, data, mtime, atime and birth time;
ctime advances. Mixed supported setattr fields commit atomically; any unsupported
flag rejects the complete request, including a requested truncate. Snapshots,
forks, restore and GC retain flags as immutable metadata. Revision diff compares
flags even when inode times are equal, without reporting a content change.

Flagged inodes use 72-byte `FFINOD04`: bytes 8..63 retain the extended inode layout,
bytes 64..67 store little-endian BSD flags, and bytes 68..71 are reserved zero.
The format supports small/large regular files, directories and symlinks. Existing
`FFINOD01`/`02`/`03` records read as flags=0 without a read-time rewrite. Zero-flag
writes retain the existing version 2/3 encoding. Older daemons reject flagged
records. Clearing flags in a World does not remove version 4 records retained by
revisions; use the updated daemon and FSKit module together, and do not downgrade
while those records remain reachable.

## Metadata height validation reuse

Namespace commits share a repository-owned LRU of immutable tree-node heights
across fresh editors. A cache hit still checks subtree height against its new
depth after rotations/merges. Cold or evicted subtrees receive the full check;
this does not disable the 256-level bound. Keys are content object IDs, never
paths, inode timestamps or World names. Linked and retained orphan edits use
this cache. It has a 64 MiB charged LRU budget (with small sharding overhead),
retains no object roots, is pruned after physical GC, and disappears on reopen.
Existing allocation and object caches remain separate. This changes no store,
inode, manifest or RPC format.

```sh
cmake --build build/Release --target forkfsd_create_probe
# Creates a NEW disposable repository, runs ordinary Namespace writes, then
# checks reopen/GC/revision isolation. Timing excludes those verification steps.
build/Release/daemon/forkfsd_create_probe /private/tmp/owned-create-probe 4000
```

The opt-in probe bounds count to 100000 and reports creation time and the last
1000 operations. A single same-machine Release comparison of 4000 empty files
measured 45.9 s before and 30.1 s after (tail 1000: 15.7 s and 8.1 s). This is a
Namespace API measurement, not a mounted-create throughput guarantee. A fresh
editor's deterministic height-cache test goes from 1001 cold misses to 7 warm
misses on its fixture. Cold first writes still scan uncached subtrees; inode to
path lookup and other metadata costs remain separate. See
[validation evidence](../docs/evidence/2026-11-12-metadata-height-cache.json).

## User immutable flag

`UF_IMMUTABLE` (2) rejects new writable opens, pathname data writes/truncation,
mode/time/xattr changes, new hardlinks, unlink, source rename and replacement of
the protected inode with EPERM. Immutable directories reject entry creation,
removal and rename in either direction, including atomic replacement; existing
children remain independently writable. Checks precede namespace publication.
Mixed requests cannot clear the bit and change protected metadata in the same
request: clear flags first. Flag-only updates remain available to the private
single-user service owner, including through metadata handles.

Darwin preserves data/size rights granted by a writable open before the flag was
set. The daemon follows this rule for existing writable descriptions and their
dups, including retained orphans; metadata changes are still denied. The flag
does not revoke existing fd/mmap permissions or prevent a service owner from
clearing it. It is not a privileged system-immutable flag, ACL or multi-user
security boundary. Unsupported flags (append-only, system bits, etc.) still fail
before mutation. Explicit offline revision restore changes a World head; this
administrative operation is not a POSIX write to an immutable inode.

Flags retain revision identity, hardlink sharing and GC retention in the existing
72-byte FFINOD04 record. There is no format-size change, but older daemons reject
the newly supported immutable bit. Clearing it in a live World does not make
retained immutable revisions readable by older versions. Deploy matching new
daemon/module binaries and keep them while such records are reachable.
See [native immutable acceptance](../docs/FSKIT.md#user-immutable-acceptance).

## Link-count limits

The namespace and FSKit `maximumLinkCount` share a 32,767 link ceiling.
`link` rejects an inode already at that count with EMLINK. Creating a directory
also checks its parent's count (2 plus immediate subdirectories); moving a
directory to another parent checks the destination after subtracting a replaced
empty directory. Net-zero replacement at the ceiling succeeds. Failed capacity
checks publish no namespace/data changes. Removing a link releases capacity.
Existing larger counters remain readable; this does not rewrite old repositories.
The limit is separate from the number of distinct files in a directory.

```sh
# Opt-in full-reference fixture: always creates a NEW disposable repository.
build/Release/daemon/forkfsd_link_limits_test --fixture /private/tmp/owned-links/repo
# Mount with mount-forkfs.py, then:
python3 scripts/tests/link_limits_mount.py /path/to/mount
# After unmount + GC, mount revision link_limit_base read-only:
python3 scripts/tests/link_limits_mount.py /path/to/mount --readonly-fixture
```

The native fixture has 32,767 actual file references and passes namespace
verification. The fast CTest uses synthetic boundary counters, restores actual
counts before verification, and covers directory creation/rename atomicity plus
capacity reuse. The native suite checks pathconf, EMLINK, identity and reuse.
On the tested APFS reference, pathconf reports 32,767 but the next link succeeds;
that extra capacity is recorded as a difference, not claimed for forkfs.
No inode/store/RPC format changes. See [evidence](../docs/evidence/2026-11-13-link-limits.json).

## Directory pages

RPC offers `fs-dir-open PATH`, `fs-dir-page CURSOR LIMIT`, `fs-dir-rewind CURSOR`
and `fs-dir-close CURSOR`, all with the owning `--session` and view selector.
Open returns cursor JSON; page returns names/eof JSON with 1..1024 names requested.
C++ uses open_directory/read_directory/rewind_directory and an opaque cursor value.

Open pins the current immutable namespace manifest and directory identity.
Lexically ordered pages use exclusive last-key continuation through the CoW tree,
with one extra entry to determine EOF. Page computation does not materialize the
whole directory. A fresh view per page prevents the decoded-node cache accumulating
an entire directory in the stream. Directory rename/delete and live entry changes
do not affect an already opened stream; a new stream sees current state. Names
from symlinks are enumerated as entries, without following each child.

Rewind returns to the beginning of the same pinned snapshot. An RPC reply lost
after computation may have advanced the cursor; there is no request deduplication.
Discard partial results and rewind to enumerate that fixed snapshot again.
Descriptors are limited to 64 per session and 1024 per service, released on close,
session expiry or shutdown. They expire on restart and reject other views/sessions.
The initial empty flat namespace is represented as a fixed empty stream.

Name-only pages exclude dot/dot-dot. `fs-dir-entries CURSOR LIMIT` returns
entries/eof JSON: each entry contains name and inode attributes from that same
pinned manifest (without following symlink children). It shares continuation with
fs-dir-page. C++ read_directory(cursor,limit,true) populates entries alongside names.
A deleted/renamed entry or modified inode is still returned with its snapshot
identity, size, mode, links and timestamps. Clients avoid per-name live stat races.
Platform cookies and live directory-file handles are future work. This is a snapshot
enumerator, not full POSIX directory-handle behavior. Physical GC is still absent;
future GC must include active stream manifests in its root/pin registry. Direct C++
callers own cursor values and are responsible for their resource lifetime.

## Advisory whole-file locks

C++ `try_lock(handle,Shared|Exclusive|Unlock)` and RPC
`fs-hlock HANDLE shared|exclusive|unlock --session SESSION` provide nonblocking
whole-file locks. RPC returns granted true/false; conflict is a valid result,
not a protocol error. Locks are scoped to World/revision plus inode, so aliases
and rename retain identity while independent forks do not share lock state.

Ownership belongs to the shared open description: dup shares a lock, separate
open competes with it, last descriptor close releases it. Session release/expiry
closes descriptors and releases their locks. Shared owners can coexist; exclusive
requires no conflicting owner. Conversion is atomic; failed shared-to-exclusive
upgrade retains the old shared lock. Unlock through any dup releases its lock.
Locks are process-local and absent from snapshots/restart state.
Whole-file locks use the same interval set as the byte-range API below.

These are advisory: ordinary reads/writes do not implicitly acquire or enforce
locks. Blocking waits, fairness, deadlock detection, POSIX fcntl/flock
mapping and Windows share modes are unimplemented. The atomic-upgrade policy is
an explicit service API choice, not a claim of platform flock equivalence.

## Advisory byte-range locks

C++ `try_lock_range(handle,mode,start,length)` and RPC
`fs-hrange-lock HANDLE shared|exclusive|unlock START:LENGTH --session SESSION`
use half-open ranges. LENGTH=0 means from START through any future EOF in the
supported signed-64-bit offset space. Ranges may be beyond current file size.
START and finite range endpoints are validated before state changes.

Overlapping ranges from independent opens conflict if either is exclusive;
adjacent/nonoverlapping ranges do not. Requests replace this owner's lock mode
only within the requested interval, preserving/splitting the remainder. Unlock
can remove a subrange; adjacent same-mode intervals merge. Failed conversion
preserves all old ranges. Whole-file lock/unlock means START=0,LENGTH=0 and
interacts with range locks through the same rules.

At most 128 normalized intervals belong to one shared open description. A change
that would exceed that budget fails without changing old locks; full unlock always
remains possible. Dup shares ranges; last close/session release drops them. RPC's
START:LENGTH field leaves room for both view and session in its eight-field limit.
Locks remain advisory and process-local. These open-description ownership rules
are not full POSIX process-owned fcntl locks or Windows byte-lock semantics.

## Single-transaction atomic replacement

`fs-save PATH INPUT_FILE` (offline and RPC) publishes a new regular inode, content
and destination directory entry in one synchronous transaction. It can create a
new file or replace an existing nondirectory entry. Existing regular-file mode is
preserved; new files and replacement of a symlink use 0644. The final symlink is
replaced rather than followed; prefix directory links continue to resolve.

Replacement gives the destination a new inode. Other hardlinks keep the previous
inode/content and have their link count reduced. Open descriptors keep the previous
inode, using orphan retention if its final name disappears. Retained revisions and
other Worlds are unchanged. Directory replacement, missing parents, oversized
input and revision writes fail without a partial publication.

This is an explicit metadata-service operation for clients that can express atomic
replacement intent. It is not a transparent rewrite of an editor's separate
create/write/rename syscalls, and requires no durability weakening. It retains the
256 KiB replacement-input limit. Use the performance probe to compare this one-commit API with
the two-commit temporary-write/rename path; OS-mounted save performance is unmeasured.

## RPC client sessions

```text
session-open LEASE_MS
session-keepalive SESSION
session-close SESSION
```

`session-open` returns session/lease_ms JSON. The lease must be 100..300000 ms;
60000 ms is a practical starting value. Keep the token and send keepalive before
the deadline. Each validated request with `--session SESSION` renews its lease,
including file operations; renewed leases begin again after execution completes.
Idle sessions expire using a monotonic clock. Explicit close, expiry and normal
service shutdown close all their handles across Worlds/revisions and release
orphan retention roots. Linked files remain intact. No physical objects are deleted.

All RPC handle operations require `--session`; presenting another session's
handle fails. View and session selectors can appear in either order, and duplicate
selectors fail. Path operations can remain sessionless; attaching `--session`
renews its lease. Session commands take their token positionally and accept no
selectors. The binary framing is unchanged (maximum 16 fields). Examples:

```text
request SOCKET session-open 60000
request SOCKET fs-open /src/main.txt rw --session SESSION
request SOCKET fs-hread HANDLE 0 4096 --world work --session SESSION
request SOCKET session-keepalive SESSION
request SOCKET session-close SESSION
```

A service admits at most 128 sessions, 256 handles per session and 4096 handles
per Container. Expiry is checked each poll-loop iteration (normally within its
100 ms poll interval); executing storage operations can delay release. Individual
connection closure does not close a session. Client crashes are detected through
missing lease renewal, not immediate PID monitoring. C++ callers using Namespace
handles directly still manage their lifetime explicitly.

Sessions/tokens are process-local and expire on daemon restart. Release preserves
unfinished ownership state if a close fails; the service stops rather than
silently forgetting retention. Startup recovery releases remaining orphan roots.
Leases are a resource-lifetime mechanism, not World authorization or an agent
sandbox. A mount client must maintain heartbeats during idle application periods;
the optional FSKit client now maintains those heartbeats; OS mount validation remains separate from callback tests.

## Legacy single-file scope

- Exclusive, non-overwriting creation with mode 0600 and durable initialization.
- Explicit little-endian anchor encoding, CRC32C, generated repository identity.
- Dual-anchor validation and fallback if one anchor is torn; no automatic repair.
- Strict rejection of unsupported versions/features, conflicting identities,
  conflicting same-generation anchors, truncated files and nonzero reserved data.
- One active owner per container; bounded nonblocking status socket and shutdown.
- CLI/integration tests including SIGKILL lock release and stale socket handling.

## Checkpoint and recovery

```sh
forkfsd checkpoint project.forkfs
forkfsd check project.forkfs
```

`checkpoint` runs offline under the exclusive container lock. It appends the
object-location index, named roots, commit sequence and hash, synchronizes them,
then publishes through the alternate anchor and synchronizes again. It does not
create a user revision or change a root. Recovery loads the selected checkpoint
and replays only its suffix. Status includes `checkpoint_seq`,
`replayed_transactions` and `indexed_objects`.

Object bytes are checked on reads. `check` also reads and verifies every indexed
object, including superseded values retained in this append-only prototype. It
is therefore intentionally more expensive than normal checkpoint startup; it is
not a scrub of superseded transaction/control records before the checkpoint.
A checksum-valid anchor pointing to corrupt checkpoint data fails closed. An
anchor with a torn checksum can fall back to the other anchor and replay forward.

The first checkpoint profile fits in one record, at most 1 MiB including framing.
An oversized index fails without starting a checkpoint write. There is no automatic
checkpoint scheduler, multipart index, garbage collection or log truncation yet.

## Named-object transactions

```sh
forkfsd object-put project.forkfs example ./input.bin
forkfsd object-get project.forkfs example > output.bin
```

These offline experimental commands require the service to release the container
lock first. A root name is an opaque identifier, **not a filesystem path**.
Values are at most 256 KiB. Updating a name atomically replaces its latest value.
The input file is not a persistent dependency; all committed bytes live inside
`project.forkfs`. No SQLite, sidecar or external metadata store is involved.

### Atomic batches

`forkfsd object-batch CONTAINER EXPECTED_SEQUENCE BATCH_FILE` atomically applies
1..64 distinct named-root PUT/DELETE operations. Obtain `last_committed_seq` from
`check`; a changed sequence rejects the entire batch before any write. The
precondition is container-wide (and catches intervening ABA changes), not per-key
MVCC. Deleting a missing root or repeating a key also rejects the whole batch.
Each value is at most 256 KiB; unique new values total at most 1 MiB.

The bounded binary input format is `FFBATCH1`, u32 count, followed by entries:
`u32 operation, u32 key_length, u32 value_length, key bytes, value bytes`.
Integers are little-endian. Operation 1 is PUT, 2 is DELETE (value_length must be
zero). Empty PUT values are valid. Keys are 1..1024 opaque bytes. For example:

```python
import struct
from pathlib import Path
entries = [(1, b"left", b"same"), (1, b"right", b"same")]
batch = b"FFBATCH1" + struct.pack("<I", len(entries))
for operation, key, value in entries:
    batch += struct.pack("<III", operation, len(key), len(value)) + key + value
Path("batch.bin").write_bytes(batch)
```

```sh
# For a newly initialized container only; later commits require the current seq.
forkfsd object-batch project.forkfs 0 batch.bin
```

The in-process Container API serializes reads, writes and checkpoints with one
mutex. `get_many()` reads several keys under that same lock; separate `get()` calls
are separate views. Two transactions with the same expected sequence cannot both
win. This is a correctness baseline, not an LSM/MVCC engine: commits currently
copy the indexes before publication and hold the lock through durability barriers.
There is no early-visible asynchronous commit or group commit yet. The control
socket remains status-only; mutation CLI commands operate offline.

The journal appends DATA objects and a prepare record, synchronizes the container,
then appends and synchronizes the commit record before reporting success. Recovery
validates record bounds, CRC32C, object SHA-256, transaction sequence and commit
hash chains. A partial final transaction is invisible; only the next writer
truncates it. Complete corrupt records fail closed. Reads/check never repair data.

Tests cover binary/empty values, replacement, segment rollover, truncated records,
corruption, and process exits before/after each publication barrier. These are not
hardware power-loss or storage-reordering tests. Only local durability exists;
remote ACK mode is not implemented yet.

This is **not a mounted filesystem**: the namespace prototype below supports
inodes and directory transactions, while remote sync remains to be implemented. Offline LevelDB GC and the optional FSKit callback adapter are described above.
Opening without a checkpoint scans the full journal. The status reports `object-journal` and
`mounted: false`; `transactions_supported` covers the named-root batches used by
the namespace prototype.
`check` takes the exclusive lock and validates the implemented profile, not a
complete future filesystem. Unknown checkpoint schemas are rejected.

Initialization failure may leave an incomplete file for inspection. Retrying
`init` never overwrites it. A raw copy of an actively written future container is
not promised to be consistent.

Next milestone: persistent metadata caches and mount
adapters; multipart checkpoints are required before larger-scale use. Follow
[CONTAINER_FORMAT.md](../docs/CONTAINER_FORMAT.md); the prototype format is still
experimental and no forward compatibility with later drafts is promised.


## Namespace prototype

These offline commands exercise actual inode/directory semantics inside the
container, without an OS mount:

```sh
forkfsd fs-init project.forkfs
forkfsd fs-mkdir project.forkfs /src
forkfsd fs-write project.forkfs /src/main.cpp ./main.cpp
forkfsd fs-cat project.forkfs /src/main.cpp
forkfsd fs-ls project.forkfs /src
forkfsd fs-stat project.forkfs /src/main.cpp
forkfsd fs-link project.forkfs /src/main.cpp /src/alias.cpp
forkfsd fs-rename project.forkfs /src/main.cpp /src/renamed.cpp
forkfsd fs-unlink project.forkfs /src/alias.cpp
forkfsd fs-check project.forkfs
```

`fs-init` creates the root directory once. `fs-rmdir` removes empty directories.
Creation/write updates, hardlinks, rename (including overwrite) and removal use
one durable multi-key batch. Rename preserves inode identity; hardlinks share
content and link count. Directory hardlinks/cycles, nonempty directory replacement
and mismatched inode types are rejected before append. Allocation markers survive
delete so inode identities are not reused. Failed validation leaves the file intact.

`fs-check` validates indexed objects plus namespace reachability, inode/content
sizes, file/directory link counts, orphan records and allocation markers. The
internal prefix `NUL + forkfs/` is reserved against generic PUT/batch mutation.

The prototype supports regular files up to 1 TiB, directories, modes 0644/0755,
mtime/ctime, and absolute paths with valid UTF-8 components up to 255 bytes
(path up to 4096 bytes). Names and symlink targets preserve their original UTF-8
bytes: case and canonically equivalent NFC/NFD spellings remain distinct.
There is no Unicode normalization or case folding. C0 controls, DEL and C1 controls,
malformed/truncated UTF-8, overlong encodings, surrogate code points and values
above U+10FFFF are rejected before publication. Dot components and repeated or
trailing slashes remain unsupported in ordinary paths; symlink targets retain
their existing traversal rules. World/revision labels remain restricted to their
ASCII identifier grammar. Existing ASCII stores need no conversion; a store with
UTF-8 names requires a daemon that supports them. These deliberate prototype
restrictions are not a frozen cross-platform naming policy. Current operations
wait for local durability.

Regular files retain the original inline inode type (1) until they first grow
past 256 KiB. They then use inode type 4 and sparse 64 KiB chunks at
`\0forkfs/chunk/INODE/NNNN` for indices 0..1023 (four decimal digits). Higher
indices use `\0forkfs/chunk/INODE/xNNNNNNNN` (eight decimal digits), up to
`x16777215`. Both ranges sort in index order; existing keys need no rewrite.
Missing chunks and omitted
trailing zero bytes read as zero. Shrinking removes later chunks and trims the
last chunk, so extending again cannot reveal old bytes. Chunk leaves use the
existing immutable CoW tree: each operation publishes data and inode size in one
transaction, and retained revisions/Worlds keep their original objects. Older
daemons reject type 4; do not downgrade a repository after creating chunked files.

Range reads/writes, appends and handle I/O remain bounded to 256 KiB per call;
reads clamp to EOF before checking that bound. `fs-write`/`fs-save` and their RPC
forms still accept a complete replacement of at most 256 KiB. Larger files use
`fs-write-at`, append, or mounted I/O in bounded pieces. Offline `fs-cat` streams
the full file; RPC `fs-cat` is bounded and clients use `fs-read-at` for larger
files. These multi-call transfers are not a single atomic operation; native
save-via-temp-file-and-rename remains available.

Unlink and shrink detach half-open chunk-key ranges from the persistent treap.
They split and merge boundary paths instead of publishing one deletion per
removed chunk. The immutable original subtree remains available to revisions.
An open-unlink initially pins the origin World manifest in a private retention
view; later fd writes/truncates/xattrs update only that view's target-file leaves
and the separate orphan inode. Other origin-World objects can remain pinned
until last close. This is internal retention, not a named revision or a public
World; namespace lookups and snapshots never expose it. GC already requires an
idle repository with no handles. Last close releases two roots in one commit;
restart recovery also drops them, and physical GC follows retained tree roots.

Older flat `inode`/`content`/`chunk`/`xattr` orphan records remain recoverable.
Older daemons do not recognize the new `tree` field: after a crash with live
new-format orphans, use the updated daemon for recovery before downgrading.
Public inode/chunk/CoW tree formats are unchanged by retirement itself.

The existing 4,096-mutation/16 MiB transaction limits remain; retirement no
longer expands a dense file into that many root mutations or reads all its
payload bytes. Tests cover 5,120 allocated chunks (320 MiB), single-commit
unlink/close/truncate, rename replacement, dup, metadata/xattrs, restart, GC and
crash publication. Full 1 TiB dense-file performance/memory qualification remains
outstanding. Sparse revision diff still compares allocated positions only.
Permission enforcement beyond the private single-user mount remains unqualified.


## Backed data allocation and storage errors

`fs-stat`, `fs-lstat` and inode RPC attributes expose `allocated_size` in bytes.
FSKit uses it for `allocSize` and native `st_blocks`/`du`. Each stored data payload
is rounded to 512 bytes per file position; sparse holes contribute zero. Identical
chunks at different positions count separately, while hardlinks share the same
inode statistic. Directories report zero. Inline payloads count their stored
length, including embedded zeroes. Namespace/xattr/LevelDB metadata and WAL are
excluded: this measures backed file data, not exclusive physical CAS ownership
or a per-file repository disk bill. It provides no preallocation guarantee.

LevelDB repositories now use `FFLDB002`, with `S/<object ID>` payload lengths
published atomically with immutable objects. Verification checks the index;
physical GC removes an object's index with the object. A bounded 1 MiB cache per
repository aggregates immutable metadata subtrees by content ID, so edits cannot
reuse stale size/mtime summaries. Cold indexed queries traverse metadata rather
than loading file data. Existing `FFLDB001` repositories remain readable without
rewriting their format during read-only access. Their first successful ordinary
transaction upgrades the format atomically; old objects without index entries
fall back to verified payload reads. This can make cold legacy queries expensive.
The legacy journal format also uses verified payload reads. Public inode and
revision formats are unchanged by these derived statistics.

**Older daemons reject `FFLDB002`. After creating or upgrading a repository, keep
using the updated daemon; removing files or running GC does not undo the upgrade.**

The pinned LevelDB POSIX environment's ENOSPC and EDQUOT I/O statuses propagate
as the corresponding RPC errno; other storage I/O failures become EIO. A failed
commit poisons the open store: status reports `requires_reopen: true`, and writes,
mount keepalive and inode sync fail rather than reporting successful durability.
Freeing host space alone does not repair that process. Unmount/stop the service,
free space on the backing filesystem, then reopen and check the repository. Do
not reinitialize or delete its database. A failed sync can still replay a complete
transaction after restart; callers must allow the whole old or new publication,
not assume that an error proves no write occurred. Retained revisions stay intact.

CTest covers both WAL append and sync failures with ENOSPC, EDQUOT and EIO,
including handle writes and inode truncation. Native acceptance and the bounded
real APFS-image disk-full reproducer are documented in [FSKit](../docs/FSKIT.md).
These tests do not qualify physical quota enforcement, all short-write paths,
hardware power loss or full 1 TiB dense workloads.

## Worlds and retained revisions

```sh
forkfsd fs-snapshot project.forkfs baseline
forkfsd fs-fork project.forkfs baseline experiment
forkfsd fs-write project.forkfs /src/main.cpp ./modified.cpp --world experiment
forkfsd fs-cat project.forkfs /src/main.cpp --revision baseline
forkfsd fs-check project.forkfs --world experiment
forkfsd fs-snapshot project.forkfs after-edit --world experiment
forkfsd fs-worlds project.forkfs
forkfsd fs-revisions project.forkfs
```

The default World is `main`. Namespace commands accept a trailing `--world NAME`;
read/check commands also accept `--revision NAME`. Revisions reject mutations.
Names are 1..64 ASCII letters, digits, dot, underscore or hyphen; `.`/`..` are
invalid. Snapshot labels and newly forked World names cannot overwrite existing entries.
Restore explicitly replaces an existing World head while retaining a safety revision.

A revision descriptor holds an immutable manifest object ID, previous retained
revision ID and captured container sequence. LevelDB manifests refer to immutable CoW metadata trees; the single-file
compatibility backend uses sorted-run manifests mapping namespace keys to
immutable objects or deletion markers. Fork installs a new World head referring
to that same manifest, plus its base revision, in one transaction. It copies no
file contents or manifest bytes. Branch reads use the complete manifest directly,
without traversing parent Worlds. Inode identity and hardlink relationships are
preserved while branch-specific metadata/content changes remain isolated.

Main's preceding flat-key layout remains readable and migrates atomically to runs
on its first subsequent mutation or explicit compaction. Its first snapshot can
also capture runs without changing the active head. Previous full-map manifests
remain readable as immutable base runs. Ordinary namespace updates append a delta
run and replace the small run-list manifest instead of rewriting all metadata.
Container indexes still copy on commit, so small append sizes do not establish
constant-time CPU behavior. Old objects/manifests/revisions and allocation markers
remain retained in the legacy backend. LevelDB supports explicit offline physical
GC and explicit offline World/label removal; automatic retention, ancestry pruning
and allocation-marker recycling remain unimplemented.

Offline commands require exclusive repository ownership. The resident RPC service
and optional FSKit frontend use the same revision-backed namespace. Cross-World
rename, multi-host writes, application-consistent live snapshots and automatic
retention remain unavailable. Restore uses the explicit safety-revision operation
above; explicit offline World and revision label removal is described below.
`fs-check --world/--revision` verifies one namespace view; `check`
verifies physical indexed objects. Neither is an authoritative remote GC audit.


## Incremental runs and compaction

```sh
forkfsd fs-layout project.forkfs --world experiment
forkfsd fs-compact project.forkfs --world experiment
forkfsd fs-layout project.forkfs --revision baseline
```

Point reads select the first matching key in newest-first runs. Prefix scans merge
keys in sorted order with the same precedence. A tombstone is a final deletion,
never permission to fall back to an older run. Runs contain metadata object IDs;
file contents stay in the separate immutable objects and are not copied by merge.

Explicit compaction merges the entire selected World view into ~64 KiB image
runs, stages large outputs in durable batches, and atomically publishes a replacement manifest. Tombstones can be removed
because this merge covers every input run in that view; other Worlds and retained
revisions still reference their old runs. The service schedules a background full merge once a World reaches 16 runs. Image partitioning may itself require
more than 16 runs, and the manifest has a hard 32-run limit. Admission/size failures
leave the prior head intact. This is full-view compaction, not yet leveled or partial-range compaction.

Current limits include 256 KiB per run object, 32 run IDs per manifest, 1 MiB of
unique new object values and 64 named-root mutations per transaction, and the
single-record checkpoint limit. Large merged outputs use multiple bounded transactions before a final head switch;
no partial output is installed as a World view. The 32-run manifest bound still applies. Read indexes are decoded
on first access and cached only for that operation's view. There are no Bloom
filters, sparse indexes, daemon-wide run cache or mutable memtable batching yet. Input capture and durable
publication take the container mutex; decoding/merging/encoding executes outside it.

Tests exercise repeated overwrites, prefix scans over 160 files, small incremental
append after compaction, delete/recreate, retained revisions before/after merges,
legacy full-map migration, checkpoint restart and compaction publication crashes.
These are correctness and layout checks, not production latency benchmarks.


## Background compaction service

`serve` starts one compaction worker and discovers current World heads with 16 or
more runs. Namespace writes in the in-process engine queue their World at that
threshold. The queue deduplicates pending requests, serves Worlds round-robin and
backs off briefly after a raced installation.

Each job has three phases:

1. Under the container lock, capture the selected immutable head and verified run
   bytes. A legacy flat main capture also records the container sequence.
2. Outside the lock, merge only the owned input bytes and encode image runs.
3. Under the lock, compare the selected World head, then stage outputs and publish with the current
   journal sequence. A changed head before staging rejects without append; if it changes between
   stages, durable output objects may remain but the head is not switched.
   Unrelated World writes do not invalidate a modern immutable head.

The old view and revisions remain usable. Objects are not physically reclaimed;
future GC must pin job inputs before it can remove old object data. This worker
reduces the number of runs read by the current view, not the container file size.

At the hard 32-run bound, writes queue maintenance and wait with the mutex released,
up to 10 seconds. A worker failure for that World or exhausted capacity returns an
error; the service never publishes an incomplete merge. Errors in one World do not
poison an otherwise healthy journal or block unrelated World writes. I/O failures
still poison the journal according to its existing reopen/recovery contract.

Offline CLI operations have no long-lived worker. At the soft threshold they run
synchronous compaction with the same lock-free computation phase, then revalidate
the operation. Invalid/no-op operations are validated first and do not trigger
this fallback. Explicit `fs-compact` retries a raced head up to eight times.

Status socket JSON exposes `compaction_running`, `compaction_queued`,
`compaction_active`, `compaction_completed`, `compaction_races`,
`compaction_failures` and `compaction_last_error`. Counters/error history are
process-local; they are not persisted. The in-process API provides start/stop/drain
methods. Shutdown waits for the current bounded job and discards unstarted requests;
restart discovers backlog from durable heads. Socket commands remain status-only.

Tests cover deterministic stale-head rejection without append, unrelated World
writes, two simultaneous writer workloads, queue draining/restart, shutdown with
pending work, job-error isolation, actual daemon startup discovery/status, and
background publication process crashes at each journal barrier. These are not
production latency or power-loss qualifications. Capture I/O and each stage/final index installation/fsync are still serialized.
Large output sets now use staged publication; the manifest and checkpoint bounds
still limit overall scale.


## Staged compaction publication

Merged output larger than 256 KiB is persisted in batches of at most 256 KiB of
new values (also respecting the journal mutation limit). The container mutex is
released between batches. Smaller jobs retain the single-transaction path.
Staging writes immutable objects to the existing object catalog and does not
change World/base/revision references. Once every output is committed, a separate
transaction installs only the small replacement World manifest.

The selected head is checked before every stage and again before final publication.
If it changed, the candidate stops. Objects already staged remain retained in this
prototype and may be reused by the next job; no space reclamation is claimed.
Existing objects are checked for matching identity/content before reuse. A head
already matching the computed result returns without redundant output writes.
For a legacy flat input, the task advances its sequence fence only after its own
staging transactions; an unrelated transaction still invalidates that capture.

A crash during staging leaves the old head authoritative. Restart discovers its
backlog, builds the candidate again and skips matching committed output objects.
No persistent job descriptor is required while GC is disabled. Final publication
uses the ordinary durable COMMIT protocol; there is no intermediate partially
merged namespace. Future GC must explicitly account for staging ownership.

Tests include a valid synthetic namespace with 14,000 retained inode-allocation
markers, generating more than 1 MiB of new merged run bytes. They cover staged
completion, a foreground write between stages, checkpoint restart, ordinary journal
barrier crashes during a stage, and crashes after staging/before/after head switch.
This is a metadata format/recovery workload, not a 14,000-file performance benchmark.

## Storage timing counters

Status includes process-local storage_metrics counters: committed transactions,
LevelStore batch preparation nanoseconds, synchronous DB::Write nanoseconds,
WAL append bytes/time, WAL Flush time and WAL/non-WAL Sync calls/time. The Env
wrapper delegates all file operations unchanged to the upstream native Env.
Counters include attempted I/O, background SST/manifest sync and database-open
activity; they reset on reopen and are not a durability receipt. Background field
reads are approximate, not a transactional multi-counter snapshot.

metadata_prepare_ns separately counts commit_changes object/tree/batch preparation.
It excludes earlier path resolution and inode edits. DB::Write includes queue/stall,
WAL append/flush, Sync and memtable work. Env Sync timing is not a pure syscall
measurement. Do not add overlapping background durations or subtract percentile
latencies. The metadata probe records per-workload deltas; cleanup and background
work during that interval are included in deltas even when cleanup is untimed.

`request SOCKET storage-profile` exposes detailed root/object Get counts, elapsed
Get time and misses, root-fact cache hits and verified-object cache hits/misses.
Get timings overlap earlier preparation counters and measure LevelDB calls,
not physical disk reads. Object cache charge is a gauge, not a monotonic counter.
These read-sensitive counters are deliberately absent from ordinary status.

A bounded root-fact cache reuses positive/negative R/ lookup results within one
committed sequence. The next sequence clears its facts; reads crossing a commit
cannot install their snapshot into the newer epoch. At most 4096 valid-sized
root keys and 32-byte values are admitted. Poisoned stores reject reads before
cache lookup. This does not replace immutable-object hash/collision checks or
change synchronous commit. Full verify continues to scan the database itself.

## Offline retention

Stop the daemon and unmount FSKit before these commands. They require exclusive
repository ownership and accept explicit names with no view selectors or RPC
entry points:

```sh
forkfsd fs-drop-world project.forkfs discarded-work
forkfsd fs-forget-revision project.forkfs discarded-tip
forkfsd gc project.forkfs
```

`fs-drop-world CONTAINER WORLD_NAME` removes an existing non-main World's head
and retained base in one synchronous transaction. It discards that World's
un-snapshotted committed state; snapshot it first if it should be kept. `main`
is protected, absent Worlds fail without a commit, and named revisions are
unaffected. Reusing a dropped World name requires an explicit new fork.

`fs-forget-revision CONTAINER REVISION_NAME` removes only the named revision
label in one synchronous transaction. The revision cannot then be selected by
that label. Immutable descriptors, parent IDs, other labels, and World bases
are unchanged. A retained descendant or World base still pins the unlabelled
revision and its complete ancestry. Reusing a forgotten label creates a new
binding; it does not rewrite older history. Existing history pagination remains
based on immutable revision IDs.

Both JSON replies identify the removed World manifest/base or revision ID and
report `collected_objects: 0`: these commands release named roots without
physically deleting objects. Run offline LevelDB GC separately to reclaim only
objects no longer reachable from any surviving root. Forgetting an ancestor
label alone cannot prune a retained tip's history. Allocation markers remain
retained, preventing reuse of historical inode identities. The immutable
single-file compatibility backend supports root removal but not physical GC.

The C++ APIs also reject open file handles, captured directory cursors and a
running background compactor anywhere in the repository. There is no automatic
retention policy, history rewriting, ancestry truncation or bulk deletion.

## Offline physical garbage collection (LevelDB)

Stop the daemon, then run `forkfsd gc REPOSITORY`. Exclusive repository ownership
prevents racing a running service. The command verifies storage, traces all World
heads, retained revision descriptors and their ancestry, metadata tree nodes and
payloads, and preserves all other named roots (including allocation records).
Unreachable internal object-index roots and immutable objects are deleted in one
synchronous WriteBatch, together with sequence and object/root counters. Manual
LevelDB compaction follows to reclaim obsolete SST data. The JSON reply reports
`collected_objects`; rerunning collection is safe after interruption.

Explicit offline retention commands can remove World roots and revision labels.
Surviving revision roots and World bases continue to pin their complete ancestry.
LevelDB CoW tree manifests are supported; older run manifests and legacy single-file containers
are rejected before deletion. The C++ API rejects active file handles, directory
cursors and background compaction. There is no RPC GC command.

The initial collector stops writers, scans the complete repository, holds its
mark set and deletion batch in memory, and performs a full compaction. Schedule
it during maintenance with enough temporary disk space for LevelDB compaction.
Filesystem allocation changes depend on the underlying filesystem and LevelDB;
the returned count measures deleted objects, not guaranteed reclaimed bytes.


## FSKit inode RPC

The optional revision frontend uses `inode-root` to acquire a leased mount view.
For a mutable World, this excludes other sessions with file handles and blocks
foreign namespace mutations until session close/expiry. Read-only revision mounts
remain shareable. Snapshot/fork queries remain available; snapshots capture only
the daemon's committed root, not unsent kernel writeback.

Commands use inode identities, never host World paths: `inode-lookup PARENT NAME`,
`inode-stat INODE`, `inode-dir-open INODE`, `inode-open INODE r|w|rw`,
`inode-create PARENT NAME MODE`, `inode-mkdir PARENT NAME MODE`,
`inode-symlink PARENT NAME TARGET`, `inode-readlink INODE`,
`inode-link INODE PARENT NAME`, `inode-remove PARENT NAME EXPECTED_INODE file|directory`,
`inode-rename SOURCE_PARENT SOURCE_NAME DEST_PARENT DEST_NAME EXPECTED_INODE EXPECTED_DEST_OR_DASH`,
`inode-setattr INODE MODE_OR_DASH SIZE_OR_DASH MTIME_NS_OR_DASH HANDLE_OR_DASH`,
`inode-setattr-times INODE MODE SIZE MTIME ATIME BIRTH HANDLE`,
`inode-setattr-flags INODE MODE SIZE MTIME ATIME BIRTH FLAGS HANDLE`,
`inode-space`, and `inode-sync`. All require `--session`; the mount must keep the
same World/revision selector. Modes are decimal 0000..0777. Setattr publishes
size/content, mode, metadata flags and mtime/atime/birth time in one synchronous transaction; handle size changes
require writable access. Subsequent I/O uses the existing session-owned `fs-h*`
operations, preserving open-unlink behavior. Session loss is terminal for a mount.

Inode-to-path resolution currently traverses the selected immutable metadata
view on each operation inside the serial RPC dispatch. It never scans a native
World directory or infers content from timestamps. This is a correctness-first
prototype, not a qualified large-repository filesystem. Typed errors use reply
code `errno+1`; old generic errors retain code 1. Frames still have a 1 MiB cap,
now allowing up to 16 fields for inode-relative rename plus selectors.


### Namespace failure errno

The inode RPC layer preserves operation-specific errors for directory/type
conflicts, even when a client reaches the daemon without kernel prevalidation:

| Rejected operation | errno |
| --- | --- |
| Rename a file or symlink over a directory | EISDIR |
| Rename a directory over a file or symlink | ENOTDIR |
| Remove a file or symlink with the directory operation | ENOTDIR |
| Unlink a directory as a file | EPERM on macOS; EISDIR on Linux |
| Hardlink a directory, including the root | EPERM |

Existing stale-identity checks retain ESTALE; missing entries, existing targets,
nonempty directories and descendant cycles retain ENOENT, EEXIST, ENOTEMPTY and
EINVAL respectively. These validation failures do not commit, allocate roots,
change inode metadata or modify directory entries. Rename inspects the final
destination entry without following its symlink: a file may replace a symlink
to a directory while the referenced directory remains intact.

Inode RPC errors use the existing errno+1 reply code. Ordinary fs-* errors still
use generic code 1, and CLI process failure remains nonzero. This refines the
error contract without a new frame, inode format or permission model. Native
FSKit already rejects these operation types with the same macOS errors; the
acceptance script verifies that behavior and repository-state stability.

## User append-only

`UF_APPEND` (4) is persisted in the existing FFINOD04 inode layout. Ordinary
`write_at`/handle writes require EOF, `append`/append handles choose EOF under
the repository lock, and whole-file writes and all truncation fail with EPERM.
This applies to old, duplicated and orphan handles, inline and chunked files.
Mode/time/xattr semantics differ: mode/times are protected, while xattrs remain
editable as measured on APFS. Flag-only clear is supported; clearing together
with protected changes fails atomically. Immutable combined with append still
blocks append admission.

An append-only directory admits new entries but prevents removal, replacement
and moving entries out. Existing mutable children retain their own write rights.
The inode itself cannot be linked, removed, renamed or replaced while append-only.
System append and other unsupported protection flags remain rejected.

FSKit lacks O_APPEND in its open callback and writes back cached partial pages.
Its `rw,positioned-append` RPC open admits a positioned writer without relocating
offsets; `fs-hwriteback` validates **every existing byte** in an overlapping
request before staging changes, then permits only unchanged overlap and extension
from EOF. An unchanged retry does not publish. A gap or changed prefix fails
EPERM before publication. Ordinary `fs-hwrite` remains strictly EOF-only for
append-only inodes. This is not a new general overwrite permission.

Deploy matching new daemon and module binaries: the frontend uses the new open
flag and writeback RPC. Older daemons reject the new inode flag bit even when
only retained revisions contain it. There is no disk-layout migration. Explicit
administrative restore can switch a World root; owner flag clear remains allowed.
This is neither a WORM store nor a multi-user authorization boundary.

Native acceptance: `python3 scripts/tests/append_flags_mount.py MOUNT --daemon
BUILD/daemon/forkfsd --socket RUNTIME/control.sock`. Run the same script with
`/private/tmp --reference-apfs` for comparison. It retains one revision; after
unmount, offline GC and readonly mounting that revision, pass `--verify-report
REPORT.json`. Use an owned disposable repository. See
[evidence](../docs/evidence/2026-11-13-append-flags.json).
