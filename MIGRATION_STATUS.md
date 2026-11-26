# BFS Common V2 migration status

## Current source evidence

At `9ba7a3ceaa06fc93b919a49595d3cb6f0527966a`, the independent source, build, tests and package scripts are present. The current source declares CORE, VOLUME, ALLOCATION, DEFRAG and FORMAT_PROFILE; mutations are restricted to native journalled media. See [technical reference](docs/BFS-TECHNICAL-REFERENCE.md). This documentation pass did not execute CI or verify a released binary.

## Historical migration plan (preserved)


## Source and ownership

[Exact central source baseline](https://github.com/Infiltrator-Projects/Filesystems/tree/101026021d9088d80b7f0f472d9061f0e2c688a3/native/filesystems/bfs). The central Linux bfs.ko passed Debian 13 build and Linux 6.12 QEMU persistence (run 39921073843). A restricted historical BFS.so also exists centrally. Neither proves an independent V2 module. Extract and adapt the existing implementation; preserve original licensing, attribution and provenance. New project-authored code uses GPL-3.0-or-later. No source transferred by this documentation commit.

## Qualification plan

1. Enumerate supported on-disk variants, validate geometry and features; implement bounded CORE probe.
2. Provide CORE mount/stat/list/read and VOLUME query using native superblock, contiguous extents, inode and directory structures, allocation and relocation.
3. Test independent mkfs.bfs images, malformed extents, overlap and end-of-volume cases; compare independent payloads and unchanged-image hashes; inject malformed media and I/O faults.
4. Implement mutation, format, check/repair and DEFRAG map/plan/execute/verify/recover only with proven allocation, durability and interruption/recovery semantics. Unsupported operations must refuse before writing.
5. Qualify exact packaged Common V2 ABI, hosted and import-free freestanding profiles, then actual BOOT and kernel integration independently.

Historical status at the time of the original migration document: all independent stages were pending. This is superseded by the source evidence above.

The [design and qualification reference](docs/BFS.pdf) is available. The earlier four-page design reference is preserved in the current PDF's historical appendix.
