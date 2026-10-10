# CoW metadata tree

The metadata index is a persistent ordered treap above LevelDB, not a new LSM
implementation. LevelDB owns WAL, SST files and compaction. Journal provides
verified SHA-256 object reads and synchronous atomic root publication. Each tree
editor maintains its own decoded-node cache and uncommitted objects.

## Format and algorithm

`FFTRNO01` nodes contain an 8-byte magic, little-endian u32 key length, three
32-byte IDs (value, left, right), then the key. Zero child IDs mean empty subtrees;
value IDs must be nonzero. Keys start with the eight bytes `\0forkfs/` and have
length 8..1024. A `FFTREE01` manifest contains its 8-byte magic and 32-byte root ID.
SHA-256 of `forkfs.metadata.priority.v1` plus key determines treap priority; keys
break ties. The binary search ordering and heap ordering are checked by entries().
Depth is limited to 256. Lookups and bounded prefix pagination visit relevant
paths, while verification via entries() traverses the whole tree.

`apply` takes a sorted map of changed values or deletions, path-copies affected
nodes and rotates/merges subtrees. It returns the manifest and reachable new node
payloads. The caller must publish those objects and the manifest atomically, along
with all new value objects, via Journal::transact. Tree editing itself does not
commit. The namespace root key must remain present. Keeping the old manifest
retains the old logical view, and starting another editor at that root implements
fork sharing. This PR demonstrates both without adding the namespace/RPC layer.

## Limits and validation

Historical objects are retained indefinitely; physical GC, persistent reference
counts and World/revision deletion are future work. This tree uses single-writer
editing; caller serialization and expected-sequence checks are required for
concurrent writes. Node decoding validates format and object integrity, whereas
whole-tree ordering/heap validation is explicit. The 256 KiB object limit and
4096 mutations / 16 MiB unique payload transaction limits belong to the store.

Tests cover current/historical trees after reopening, deterministic updates,
ordered pagination, real insert/update/delete and complete tree checks. Storage
regressions cover root cache invalidation, concurrent reads, immutable object
validation and poisoned writes. These are library tests, not mounted filesystem
or Git/build performance acceptance. No measured latency improvement is claimed.
