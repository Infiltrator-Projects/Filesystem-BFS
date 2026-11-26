# Filesystem-BFS

Native SCO/UnixWare Boot File System Common V2 provider (not BeFS). The current source declares hosted and freestanding profiles. Clean original BFS media are read-only; writes, formatting and compaction are restricted to the Infiltrator journalled profile and an exclusive durable host. Exact-source test and release evidence remain separate from source claims.

## Documentation

- [Engineering manual (PDF)](docs/BFS.pdf)
- [Technical reference (Markdown)](docs/BFS-TECHNICAL-REFERENCE.md)
- [PDF stream source](docs/BFS.pdfstreams) (plain text, not Apple Pages)
- [Original native design and historical migration requirements](native/filesystems/bfs/DESIGN.md)
- [Migration and current source status](MIGRATION_STATUS.md)
- [Project philosophy](docs/PHILOSOPHY.md)
- [Canonical Common V2](https://github.com/Infiltrator-Projects/Libraries)

The provider source, operation masks and exact qualification workflow govern current capabilities. Historical documentation is preserved in the PDF appendices; it is not current status. The packaging script installs the PDF from `preview/Filesystem-BFS.pdf` and the provider at `/System/Filesystems/BFS.so`. Release assets remain immutable; a documentation commit does not itself establish release qualification.
