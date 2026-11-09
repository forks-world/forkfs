# Script status

`scripts/check-deps.sh` validates the supported revision-backed `forkfsd` binary.
`scripts/bundle-forkfs.sh` creates the optional FSKit app bundle without
installing it. `scripts/mount-forkfs.py` supervises the daemon, authenticated
loopback bridge and OS mount in one foreground process. `scripts/fskit-config.py`
prepares direct Unix descriptors for unsandboxed callback tests. See
[FSKit setup](../docs/FSKIT.md).

The native `world` smoke, mount, safety, APFS clone and old platform benchmark
scripts remain historical references. They are not supported runtime entry points
and are not run by CI. The daemon crash, namespace and GC suites are registered
in CTest. See [revision-only policy](../docs/REVISION_ONLY.md).
