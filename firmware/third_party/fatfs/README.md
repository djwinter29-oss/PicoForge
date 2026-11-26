# FatFs

This directory contains the official FatFs R0.16 source release from [ChaN's FatFs page](https://elm-chan.org/fsw/ff/), including the upstream license and documentation. PicoForge's `ffconf.h` and the disk-interface header are kept in `firmware/src/integration/fatfs/` rather than mixed with the filesystem core.

## Source and patches

- Release: R0.16, published July 22, 2025
- Source archive: [ff16.zip](https://elm-chan.org/fsw/ff/arc/ff16.zip)
- Archive SHA-256: `99f7dc1f7e095356e4a9e3dbe29959090d8b948afe2bbc5441e52fdf4b85449e`
- Official patch 1: September 13, 2025, applied to `source/ff.c`
- Official patch 2: July 10, 2026, applied after patch 1 to `source/ff.c`
- Patch notes: [known problems and patches](https://elm-chan.org/fsw/ff/patches.html)

The patches were applied in the order specified by the upstream notes. `ffconf.h` is project configuration; all other FatFs core source files are retained from the archive except for the official patches to `ff.c`.

## License

See the upstream [`LICENSE.txt`](LICENSE.txt) and `source/00readme.txt` for FatFs's terms and attribution.

## PicoForge integration status

FatFs is compiled and linked by the firmware build. PicoForge's FatFs configuration and disk-interface header live in
[`firmware/src/integration/fatfs/`](../../src/integration/fatfs/); the disk-I/O adapter is
[`fatfs_disk.c`](../../src/platform/storage/fatfs_disk.c). Current mounting and storage-ownership behavior is
documented in the [storage guide](../../../docs/guides/storage.md) and
[SD-card driver reference](../../../docs/reference/sdcard/driver.md). Physical card I/O is not yet hardware-validated.
