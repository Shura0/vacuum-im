# Vendored libomemo-c

Snapshot of the OMEMO crypto library used by the `omemo` plugin.

- Upstream: <https://github.com/dino/libomemo-c>
- Version: `v0.5.1`
- Commit: `1f0b37fee3ebefc46c32f882623481899a1ba568`
- Vendored: 2026-10-24

Only the upstream `src/` tree, `LICENSE` and `README.md` are copied here.
The upstream tests (which need `check` and OpenSSL) are not vendored.

The library implements the Signal double ratchet / OMEMO protocol versions
3 and 4.  The Vacuum plugin uses protocol version 3 (legacy
`eu.siacs.conversations.axolotl`).

## protobuf-c

The generated `*.pb-c.c` files need the protobuf-c runtime.  To keep the
build self-contained, protobuf-c 1.5.1 is vendored next to this directory
(`src/thirdparty/protobuf-c`).

## License

libomemo-c is GPL-3.0 (see `LICENSE`).  The vendored `protobuf-c` is
BSD-2-Clause (see `src/thirdparty/protobuf-c/LICENSE`).  Both are compatible
with Vacuum's GPL-3.0 licensing.  Do not remove the upstream copyright
headers.

## Updating

1. Check out the desired upstream tag/commit.
2. Replace `src/` and `LICENSE`, update this file with the new version and
   commit hash.
3. Update `src/thirdparty/protobuf-c` if the generated `*.pb-c.*` files
   require a newer runtime.
4. Build the `omemo` plugin and run the interop tests against Dino and
   Conversations before committing.
