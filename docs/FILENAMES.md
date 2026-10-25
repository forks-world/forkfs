# Filename encoding and compatibility

The CoW namespace stores directory components as byte strings. Valid UTF-8 names
(including Chinese, emoji and combining marks) are supported without converting
or normalizing their bytes. Comparison is case-sensitive and bytewise; `é` and
`e` plus combining acute are distinct names. Sorting follows unsigned byte order,
not a locale collation or the host APFS volume's case/normalization rules.

A component is 1..255 **bytes**, excluding NUL, `/`, `.` and `..`. Newline, tab,
quotes, backslash and non-UTF8 bytes are valid components. Four-byte code points
consume four bytes each. Existing absolute path syntax and the 4096-byte path
limit remain unchanged. Symlink targets preserve arbitrary non-NUL bytes within
the existing length/component limits; relative `.`/`..` traversal remains confined
to the selected World. A symlink never becomes a host filesystem passthrough.
World/revision identifiers and xattr keys retain their existing ASCII policies.

Namespace entry keys and FFPAR001 parent records already encode byte lengths or
store opaque byte payloads; no storage re-encoding or format-version change is
needed. Existing ASCII stores, revisions, baselines and deltas remain readable.
Legacy object-journal and segmented LevelDB profiles share the same name policy.
Old binaries that enforce the former ASCII restriction are not suitable for
accessing newly created non-ASCII names; downgrade support is not promised.

## Protocol behavior

The binary RPC field format carries filename bytes unchanged. FSKit uses
`FSFileName.data`, never a lossy NSString conversion, as the lookup/create/rename
key. Its pinned raw directory endpoint returns `name_hex` and can enumerate every
stored component, independent of encoding. The new callback tests exercise valid
Unicode through the real daemon; signed, real-mounted normalization behavior
still requires deployment acceptance.

Legacy `fs-dir-page` and `fs-dir-entries` JSON text responses require strict UTF-8.
Valid names retain their Unicode spelling; quotes, backslash and C0 controls are
escaped. Malformed/overlong UTF-8, surrogate encodings and code points above
U+10FFFF produce EILSEQ instead of replacement characters or aliases. Callers
requiring arbitrary byte identity must use `fs-dir-entries-raw`.

Directory cursor advancement is committed only after response encoding and the
1-MiB frame check succeed. EILSEQ or E2BIG therefore permits retrying the same
cursor through the raw endpoint or with a smaller page. Textual CLI `fs-ls`
output remains newline-delimited and is not an unambiguous filename protocol.

## Regression coverage

- Namespace: byte-exact path and inode operations, 255/256-byte boundaries,
  distinct normalization/case spellings, symlinks, parent indices, paged oracle
  enumeration, snapshots/forks, close/reopen and legacy-format fixtures.
- RPC: strict Python decoder oracle over seeded byte names, invalid UTF-8
  boundaries, control escaping, failed-page retry and oversized JSON expansion.
- Recovery: an ASCII-only base plus Unicode/raw-byte deltas, hardlinks and
  symlinks survive detached backup/replay and revision restore.
- FSKit callbacks: Unicode create/lookup/read/write/rename/link/symlink and
  distinct composed/decomposed names, without an installed extension or mount.
