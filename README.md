# AetherFS

AetherFS is an experimental Linux filesystem that tries to combine:

- `ext4` conservatism on the hot path
- `XFS`-style scalability and extent discipline
- `ZFS`-style integrity, scrub, and checkpoint thinking
- `Btrfs`-style reflink/snapshot expectations
- `F2FS` flash-aware placement instincts
- `APFS` transactional cleanliness
- `ReiserFS/Reiser4` dense small-file ideas, with restraint

The core idea is simple:

- keep the correctness-critical path small
- make metadata discoverable and checksummed
- support multiple data behaviors through policy, not three unrelated engines
- stay usable on a single disk, even when the long-term design grows beyond that

Today, the validated implementation is the single-device bootstrap tier: a checksummed image format, userspace admin tooling, and a kernel mount path for that compact layout. The broader multi-device, snapshot-rich design is present as direction and partial code, not as a finished end-to-end runtime.

This repository currently contains:

- a shared on-disk format header
- a userspace formatter and validation toolchain
- admin tooling for scrub, health, forensics, and rebalance
- a kernel module and mount helper for the bootstrap image format

## Design Summary

AetherFS is built around a common substrate:

- versioned on-disk structures
- checksummed metadata and data
- extent-based allocation
- checkpointed metadata discovery
- intent-log based recovery
- policy-driven data behavior

The filesystem is meant to be:

- metadata CoW by default
- selectively CoW for data
- extent-oriented instead of block-fragment oriented
- snapshot-friendly
- scrub-friendly
- allocator-aware of locality, temperature, and device traits

## Filesystem Model

At a high level, AetherFS is organized as:

- superblock set with rotating checkpoints
- a checkpoint-root block that makes the rest of the bootstrap layout discoverable
- metadata tree roots
- extent-map roots
- free-space roots plus dense local bitmaps
- intent / journal segments
- data regions
- optional roots reserved for special metadata, cache, and parity classes
- snapshot references
- scrub and health journals

Everything important is meant to be:

- versioned
- checksummed
- reachable from checkpoint metadata

In the current repository, that full model is represented as a staged architecture. The implemented and validated path is the bootstrap checkpoint-root layout described below, which acts as the first concrete "tree of trees" anchor for the rest of the design.

### Current Bootstrap Layout

The current userspace image format uses a compact bootstrap region. In format `v5`, the reserved blocks are:

| Block | Purpose |
|---|---|
| `0` | Pool label |
| `1` | Superblock |
| `2` | Root inode block |
| `3` | Root directory block |
| `4` | Intent log / journal header |
| `5` | Inode bitmap block |
| `6` | Free-space extent root |
| `7` | Checkpoint-root block |
| `8+` | Data region |

The superblock contains an array of checkpoint slots. The current bootstrap format writes one active checkpoint entry pointing to block `7`, which contains a checksummed `aetherfs_checkpoint_root` descriptor. That descriptor records where the bootstrap trees, journals, and regions live.

That means image loading is supposed to work like this:

1. Read and verify the pool label.
2. Read and verify the superblock.
3. Follow the active checkpoint pointer.
4. Read and verify the checkpoint-root block.
5. Discover the rest of the bootstrap trees and journals from there.

This is the current “root of roots” for the repo’s userspace tooling.

## Data Modes

AetherFS defines three data modes:

- `CoW`: for snapshots, general files, source trees, and desktop/server defaults
- `Overwrite`: for databases, VM images, and rewrite-heavy files
- `Append`: for logs, telemetry, and write-once streams

The intended rule is that the engine stays shared, while policy decides:

- whether a write allocates new extents
- whether in-place overwrite is allowed
- whether append-only rules apply

This avoids multiplying the number of recovery-critical write paths.

## Metadata, Trees, and Allocation

The design direction is:

- metadata in a transactional tree
- file data described by extent maps
- free space tracked by an extent tree plus dense local bitmaps
- allocation groups to reduce global contention
- locality-first allocation before “smart” heuristics

The repo already reflects that split conceptually:

- metadata and extents are represented explicitly in the format header
- free-space tracking uses an extent-root plus local bitmap ideas
- the allocator code is structured around allocation groups and simple hot/warm/cold classes

The intended discipline is:

- no dangling extents
- no double allocation
- no lost space
- no silent metadata drift

## Integrity and Recovery

Integrity is the center of the design.

AetherFS aims for end-to-end checksumming of:

- superblock and checkpoint metadata
- inodes
- extent metadata
- directory records
- journal metadata
- file data blocks

The current repo uses checksummed bootstrap structures and per-block data checksums in the userspace image path. The admin tooling is built around validating those checksums before claiming an image is healthy.

Recovery follows an intent-first model:

- record intent
- write data or metadata
- commit the transaction or publish a new root

The current codebase also carries scrub / self-heal concepts:

- scrub status
- corruption counters
- repair counters
- quarantine hooks

In the current repository, the most complete and trustworthy part of that model is the userspace validation/admin surface, not a fully mature kernel recovery engine.

## Snapshots, Reflinks, and Clones

The long-term feature set includes:

- snapshots
- subtree snapshots
- reflinks / clones
- snapshot rollback
- send / receive style workflows

The codebase already has snapshot and reflink modules, but the README should be read carefully here: the repo’s strongest validated path today is still the bootstrap image format, maintenance tooling, and the basic mounted image workflow rather than a finished snapshot/send-receive product.

## Administration Model

A filesystem loses if it is annoying to operate, so AetherFS is trying to make administration explicit instead of hiding it in obscure mount options.

The repo currently ships these userspace commands:

- `aetherfs mkfs`
- `aetherfs mount`
- `aetherfs scrub`
- `aetherfs health`
- `aetherfs forensics`
- `aetherfs rebalance`

### What They Do

- `mkfs` formats a checksummed image/device bootstrap
- `mount` preflights the image and supports `strict`, `degraded-read-only`, and `rescue` behaviors
- `scrub` validates structural metadata and data checksums
- `health` prints a human-readable state summary and mount recommendation
- `forensics` exports reserved metadata blocks plus a report without modifying the source image
- `rebalance` compacts data extents offline in the current userspace image model

### Failure Behavior

The intended failure behavior is graceful:

- isolate corruption narrowly
- fail `strict` mounts early
- allow degraded-read-only handling only when bootstrap metadata is still trustworthy
- preserve evidence before repair

That philosophy is already visible in the current tooling:

- clean images recommend `strict`
- partially damaged but readable images degrade
- unreadable images recommend `rescue`
- forensics export is read-only

## Current Status

This repository is not “finished AetherFS.” It is a staged implementation with a clearer userspace bootstrap/maintenance story than full kernel feature parity.

What is concrete today:

- shared on-disk format in [`kernel/include/uapi/linux/aetherfs_format.h`](kernel/include/uapi/linux/aetherfs_format.h)
- formatter and image validator in [`aetherfsprogs/mkfs.c`](aetherfsprogs/mkfs.c) and [`aetherfsprogs/image.c`](aetherfsprogs/image.c)
- admin tooling in [`aetherfsprogs/scrub.c`](aetherfsprogs/scrub.c), [`aetherfsprogs/health.c`](aetherfsprogs/health.c), and [`aetherfsprogs/rebalance.c`](aetherfsprogs/rebalance.c)
- bootstrap-aware mount helper in [`aetherfsprogs/mount.c`](aetherfsprogs/mount.c)
- policy and recovery hooks in [`kernel/policy.c`](kernel/policy.c) and [`kernel/recovery.c`](kernel/recovery.c)

What is still clearly in-progress:

- full kernel-side realization of the broader design
- mature snapshot/send-receive workflows
- a complete policy-admin interface
- the long-term multi-device and tiered-storage model
- a finished tree-of-trees runtime instead of the current bootstrap-focused tier

## Current Limits

The design target is much larger than the currently validated bootstrap implementation.

Current bootstrap-format facts:

- format version: `5`
- default block size: `4096`
- current bootstrap inode ceiling: `32640`
- current data region start: block `8`
- current checkpoint-root block: block `7`

Design targets that still exceed today’s bootstrap implementation:

- multi-EiB scaling
- richer tree roots beyond bootstrap metadata
- full multi-device runtime behavior
- a complete snapshot-native mounted filesystem

## Typical Workflow

Format an image:

```bash
make -C aetherfsprogs
./aetherfsprogs/aetherfs mkfs --force --blocks 128 /tmp/aetherfs.img
```

Validate it:

```bash
./aetherfsprogs/aetherfs scrub /tmp/aetherfs.img
./aetherfsprogs/aetherfs health /tmp/aetherfs.img
```

Export bootstrap evidence:

```bash
./aetherfsprogs/aetherfs forensics /tmp/aetherfs.img /tmp/aetherfs-forensics
```

Mount it:

```bash
./aetherfsprogs/aetherfs mount --strict /tmp/aetherfs.img /mnt/aetherfs
```

Mounting requires privileges and a loadable kernel module.

## Building

Userspace tools:

```bash
make -C aetherfsprogs
```

Kernel module:

```bash
make -C kernel
```

## Reading Order

If you want to understand the repository quickly, read in this order:

1. [`kernel/include/uapi/linux/aetherfs_format.h`](kernel/include/uapi/linux/aetherfs_format.h)
2. [`aetherfsprogs/mkfs.c`](aetherfsprogs/mkfs.c)
3. [`aetherfsprogs/image.c`](aetherfsprogs/image.c)
4. [`aetherfsprogs/health.c`](aetherfsprogs/health.c)
5. [`kernel/policy.c`](kernel/policy.c)
6. [`kernel/recovery.c`](kernel/recovery.c)

That path shows the current format contract, how images are created, how they are discovered from checkpoint roots, and how the repo currently reasons about health, policy, and recovery.
