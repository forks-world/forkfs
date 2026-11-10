# Revision-only runtime policy

The supported product is `forkfsd`. Its authoritative namespace is a World head
or retained revision manifest referencing immutable metadata/content objects.

The native-directory `world` CLI and core are retired together, rather than
keeping commands whose outputs cannot satisfy this model. This includes legacy
diff, verify, clone/pool, observed history, Git and native execution interfaces.
Historical source and design documents remain for reference. Old binary/store
migration is not implemented, and no command silently opens an old World store
as a revision repository.

`fs-diff` compares two explicit immutable revision roots and returns paged path
changes. Equal manifest IDs skip identical namespaces, and equal content/chunk
IDs prove byte identity; metadata is compared separately. Inline/chunked layout
differences and sparse holes are compared as bounded logical byte ranges when
object identity alone is insufficient. Size, timestamps and event observations
never prove content equality. Comparing a mutable World requires an explicit
`fs-snapshot` first; diff never silently selects a World or scans host files.
See [command semantics and pagination](../daemon/README.md#revision-diff).

`fs-history` exposes committed revision IDs, parent relationships, manifest IDs
and captured repository sequences along the ancestry of an explicit named tip.
Forks retain their selected base as ancestry; unrelated branch commits do not
appear in that chain. The query does not create versions or claim to capture
every file write. See [revision history](../daemon/README.md#revision-history).
The new optional FSKit frontend operates through the resident daemon RPC and
revision-backed namespace; see [FSKit integration](FSKIT.md). Git and
execution integration remain unavailable until revision-backed replacements exist.

The daemon's content checks, namespace checks, revision snapshots/forks and
physical GC remain supported. Its legacy single-file test/container format is a
storage compatibility mechanism with immutable revision manifests, not the old
native-directory runtime. Generic named-object transactions remain storage
primitives, not an alternate filesystem or a replacement revision history.

CI builds and tests the daemon on macOS and Linux. Native APFS clone, Bubblewrap
and world-specific safety/disk-full scripts are historical and are not evidence
for the supported runtime. Daemon crash/GC tests remain part of CTest.
