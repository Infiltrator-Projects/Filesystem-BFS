# Filesystem-BFS

Independent SCO/UnixWare Boot File System provider for InfiltratorOS. Target: one native C/C++ engine and one distributable `BFS.so`.

**Status: documentation only; source extraction pending.** [Central source](https://github.com/Infiltrator-Projects/Filesystems/tree/101026021d9088d80b7f0f472d9061f0e2c688a3/native/filesystems/bfs). The central Linux bfs.ko passed Debian 13 build and Linux 6.12 QEMU persistence (run 39921073843). A restricted historical BFS.so also exists centrally. Neither proves an independent V2 module. No independent driver or release exists.

## Common V2

Use only `infiltrator_filesystem_api_v2` from [Libraries](https://github.com/Infiltrator-Projects/Libraries). Own superblock, contiguous extents, inode and directory structures, allocation and relocation in the same engine for hosted and freestanding execution. No FUSE, separate per-format kernel/EFI driver or legacy ABI.

## Build and install

Planned: `/System/Filesystems/BFS.so` and `/System/Resources/Documentation/Filesystem-BFS.pdf`. Generic amd64 .deb, source-downloading CPU-native .run and GitHub-generated source ZIP only. No build or installer exists yet.

See [migration status](MIGRATION_STATUS.md) and [engineering philosophy](docs/PHILOSOPHY.md).
