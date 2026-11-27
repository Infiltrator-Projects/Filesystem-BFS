# BFS: technical reference

Source implementation revision: `9ba7a3ceaa06fc93b919a49595d3cb6f0527966a`.

## Historical scope and design rationale

The SCO/UnixWare Boot File System is a small, flat, contiguous-allocation disk filesystem for boot material. It is unrelated to the BeOS filesystem. Avoiding indirect blocks simplifies boot-time loading and reduces metadata traversal. The cost is constrained namespace capacity and external fragmentation: a large allocation may fail even when enough total free blocks exist. Internal file fragmentation is not an ordinary valid BFS state; relocation and compaction move whole files without breaking contiguity. Original media should be distinguished from the Infiltrator-specific journalled profile described later.

## Physical layout and volume bounds

BFS uses little-endian numeric fields, fixed 512-byte filesystem blocks, a superblock at byte zero, and a fixed array of 64-byte inode slots beginning at byte 512. The data region follows the inode table; the root directory is an ordinary allocated contiguous extent with inode number 2. Interpret all offsets relative to the partition. Validate the total declared length against the host length; do not round up partial final sectors or read unallocated bytes as data. A magic match alone does not establish that the inode table and root directory are valid. A host transfer unit may differ from the on-disk format unit; the provider restricts its writable profile to 512-byte logical host sectors.

## Superblock field inventory

The 512-byte superblock contains 32-bit magic 0x1BADFACE at 0, first data byte at 4, inclusive final volume byte at 8, relocation source and destination indicators at 12 and 16, backup relocation indicators at 20 and 24, six-byte filesystem name at 28 and six-byte volume label at 34. Remaining bytes are reserved/padding in the original structure. Clean relocation indicators are all-ones values in the native layout. Compute volume bytes as final byte + 1 in wide arithmetic; inode slots as (data_start-512)/64; last inode number as slots+1. The independent implementation permits a partial inode-table block and rounds the data start up for block allocation; check exact profile before writing. Preserve reserved bytes on foreign media.

## Inode field inventory

A 64-byte inode: inode number (16-bit) at 0; unused 16-bit field at 2; start block (32-bit) at 4; inclusive last allocated block at 8; inclusive last data byte relative to the volume at 12; vnode type at 16; mode at 20; UID at 24; GID at 28; link count at 32; access, modification and change timestamps at 36, 40 and 44; reserved words at 48..63. Nonzero inode number must agree with its table slot. Root type is directory (2), regular files use type 1. Slot zero values mark unused entries. The offset of inode i is 512+64*(i-2). A zero start block identifies an empty file; the root cannot be empty.

## File length and exact address arithmetic

For a nonempty inode with start S, last block E and end byte L, allocated blocks are E-S+1, and logical length is L+1-512*S. Require S and E inside the data region, E>=S, and L within allocated range. A normal tightly allocated file has E=floor(L/512); reject or separately interpret excess allocation. File offset p maps to 512*S+p until EOF. Example: S=80, E=82, L=42395 yields 1436 bytes in three blocks; bytes after EOF are not file content. All arithmetic involving L+1 must use at least 64 bits because the end field can be 0xffffffff. Validate the entire inode and extent before reading even a single byte.

## Directory namespace and lookup

The flat root consists of 16-byte records: 16-bit inode number followed by a 14-byte name. Thirty-two entries fit in a block. A zero inode field denotes an unused slot. The name may fill its entire field without a terminating zero; use bounded native name comparison. Validate each reference against the inode table, reject duplicate visible names and incorrect root aliases, and verify the relationship between directory references and inode link counts. Dot and dot-dot refer to root; subdirectories and arbitrary special-file types are outside BFS's normal namespace. Do not treat names with embedded separators as nested paths. The native implementation's formatter supplies root directory storage for a bounded number of slots.

## Allocation, free space and fragmentation

There is no independent original persistent free-block bitmap. Enumerate allocated inodes, validate extents, reserve superblock/inode-table space, and mark each data run including root. Sort by physical start, reject overlap, and derive gaps and total free blocks. An allocation must find one sufficiently long unoccupied run. A file with contiguous logical and physical blocks has one extent even if the volume contains many holes. Report file extent count separately from free-space fragmentation. A useful volume metric includes largest free run, number of free runs and total free blocks. A map must describe actual ownership, not guessed fill based on file size alone.

## Original mutation and relocation model

Creation needs an unused inode and an unused directory slot. A zero-length file need not own blocks. Append can extend in place only if the succeeding blocks are free and representable. Otherwise allocate a contiguous destination, copy the existing payload, append, and update the inode's range and final byte; release the old range only after durable metadata publication. Deletion retires directory references before reclaiming storage; link counts and free-slot reuse must stay consistent. Historical relocation indicators are not a general-purpose write-ahead journal. Do not assume the Infiltrator journal below is part of SCO's original format or that original tools preserve it.

## Infiltrator BFS: current source profile

Source inspected at the main revision preceding this documentation commit: native/filesystems/bfs/bfs.c. The provider exposes Common V2 CORE probe, mount, unmount, stat, directory enumeration, read, write, create, remove, rename and truncate; VOLUME query, label get, sync, format and check; ALLOCATION map; DEFRAG map, plan, execute, verify, recover and release-plan; and a format-profile descriptor. Hosted and freestanding profiles are declared; independent BOOT and physical-media qualification remain separate. Foreign clean BFS volumes can be read; mutations require the recognized Infiltrator journal profile, exclusive durable writable host and 512-byte logical sectors. The current source uses one gated mount session. An exported group is not a guarantee that every operation is available on every medium.

## Infiltrator BFS: native formatter

The current formatter uses 512 inode slots, a 16-block root extent and one mandatory additional data block. It reserves blocks 0 through 64 for metadata, then root blocks 65 through 80. Minimum volume size is 82*512 = 41984 bytes; recommended 2 MiB; maximum 4294967296 bytes (4 GiB). Label length is at most six bytes. It first invalidates the superblock, initializes inode and root regions, flushes, then publishes valid geometry and clean journal. The root contains dot and dot-dot. Unallocated payload need not be erased by a filesystem format; secure erasure is a separate operation. The exact native profile should be negotiated through Common V2 rather than hardcoded in a consumer.

## Infiltrator BFS: redo intent record

The project-private journal occupies 224 bytes beginning at superblock offset 64. Relative fields: signature IFSBFS2 plus NUL at 0; data start block at 8; total blocks at 12; pending state at 16; inode number at 20; directory slot at 24; old inode at 28 (64 bytes); new inode at 92 (64); old directory entry at 156 (16); new entry at 172 (16); payload digest at 188 (8); record checksum at 220 (4). All numeric fields are little-endian. Geometry and checksum must match before replay. A pending record also marks the original relocation indicators; the clean record restores them to all-ones. This extension uses original reserved bytes but is not claimed as a normative UnixWare journal.

## Infiltrator BFS: durability and compaction

The implementation copies and flushes destination payload before committing the metadata intent; a pending journal is replayed only on a writable durable host after validation. Its payload digest uses a 64-bit FNV-1a-style hash, not a cryptographic integrity guarantee. The planner orders nonempty files by physical start, simulates exact free ownership and plans moves that preserve whole-file contiguity. Overlapping destinations require safe staging. Growth policy 10 reserves ceil(blocks/10) adjacent free blocks, verified as free after execution; this is free-space layout, not file ownership or a persistent reservation promise. Execute rechecks source and destination, copies, flushes, commits new inode and records movement. Verify checks final range, size and intended free reservation; independent payload comparisons remain necessary.

## Implementation and qualification boundary

Common V2 is the independent provider's sole public interface: https://github.com/Infiltrator-Projects/Libraries/blob/main/docs/FILESYSTEM_COMMON_ABI_V2.md . Negotiate operation masks and media profiles; distinguish unsupported, corrupt, I/O and stale-handle results. Verify exclusive unmounted ownership, bounded host I/O and durable flush before mutations. Consumers use the same engine, not format-specific parsers. Hosted and freestanding binaries, relocation, Test Media, BOOT, kernel and packages require separate exact-source evidence. The repository README and qualification records establish current status.

## Interoperability and engineering acceptance

Original source references: Linux BFS disk layout https://codebrowser.dev/linux/linux/include/uapi/linux/bfs_fs.h.html ; independent reader https://github.com/torvalds/linux/tree/v6.12/fs/bfs ; util-linux mkfs.bfs. Test independently created foreign images, empty and full-width names, final 32-bit byte boundary, partial inode table, overlapping files, invalid root aliases, orphaned inodes, dirty foreign relocation state, fragmented free space, growth with and without adjacent capacity, exhausted inode and directory slots, stale plans, cancellation and every journal interruption. Compare payload bytes and raw-image hashes; do not write foreign media through the private journal profile. Project workflow builds portable and native provider and packages preview/Filesystem-BFS.pdf; exact release and boot qualification must be checked separately.

Include relocation of the final allocated extent in those tests. The central historical BFS.so reader/overwrite source and Linux run 39921073843 are provenance only, not independent provider qualification.

Historical source: https://github.com/Infiltrator-Projects/Filesystems/blob/101026021d9088d80b7f0f472d9061f0e2c688a3/native/filesystems/bfs/module/bfs_module.c .
