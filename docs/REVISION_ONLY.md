# Revision-only runtime policy

The supported product is `forkfsd`. Its authoritative namespace is a World head
or retained revision manifest referencing immutable metadata/content objects.

The native-directory `world` CLI and core are retired together, rather than
keeping commands whose outputs cannot satisfy this model. This includes legacy
diff, verify, clone/pool, observed history, Git and native execution interfaces.
Historical source and design documents remain for reference. Old binary/store
migration is not implemented, and no command silently opens an old World store
as a revision repository.

Future diff compares explicit revision roots. Equal subtree IDs permit skipping
subtrees; equal content IDs prove byte identity; metadata is compared separately.
Size, timestamps and event observations never prove content equality. Comparing
a mutable World must first capture a committed root under the transaction lock.

Future history exposes committed revisions and parent relationships, rather than
claiming that observing an external directory captures every write. The new optional FSKit frontend operates through the resident daemon RPC and
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
