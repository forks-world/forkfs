# CoW metadata storage foundation

This directory introduces the immutable object storage used by the forthcoming
forkfs metadata tree. It does not add a daemon executable or mounted filesystem.
LevelDB 1.23 stores SHA-256 addressed objects and mutable retention roots; one
synchronous WriteBatch publishes roots and the sequence together. Verified object
reads have a bounded cache. Root facts are cached within a committed sequence.
Failed writes poison the store until reopen. Historical objects are retained;
physical garbage collection is not implemented.

Journal is the current tree-facing API. It also contains the preceding single-file
journal reader/writer for compatibility; new stores use LevelStore::create and a
Journal opened on the LevelDB directory. This PR does not introduce the legacy
container format's CLI or its migration workflow.

Build and test independently of the existing world CLI:

```sh
 git submodule update --init --recursive
 cmake -S . -B build/cow -DWFS_BUILD_LEGACY=OFF -DCMAKE_BUILD_TYPE=Debug
 cmake --build build/cow -j4
 ctest --test-dir build/cow --output-on-failure
```

macOS and Linux are supported. Linux requires OpenSSL development headers.
Upstream LevelDB is pinned and compiled as C++17, with RTTI disabled in the Env
wrapper to match upstream. The object store tests exercise immutable validation,
cache eviction, root invalidation, concurrent reads and failure poisoning.
