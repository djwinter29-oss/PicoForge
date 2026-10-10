# FatFs

This directory contains the official FatFs R0.16 source release from [ChaN's FatFs page](https://elm-chan.org/fsw/ff/). The upstream archive is retained in its original layout, including `LICENSE.txt`, `source/`, and `documents/`.

## Source and patches

- Release: R0.16, published July 22, 2025
- Source archive: [ff16.zip](https://elm-chan.org/fsw/ff/arc/ff16.zip)
- Archive SHA-256: `99f7dc1f7e095356e4a9e3dbe29959090d8b948afe2bbc5441e52fdf4b85449e`
- Official patch 1: September 13, 2025, applied to `source/ff.c`
- Official patch 2: July 10, 2026, applied after patch 1 to `source/ff.c`
- Patch notes: [known problems and patches](https://elm-chan.org/fsw/ff/patches.html)

The patches were applied in the order specified by the upstream notes. No other FatFs source files have been modified.

## License

See the upstream [`LICENSE.txt`](LICENSE.txt) and `source/00readme.txt` for FatFs's terms and attribution.

## PicoForge integration status

The source is vendored but is not yet part of the firmware build. Integration still needs a PicoForge disk-I/O adapter over the SD block API, an `ffconf.h` configuration, and a deliberate filesystem concurrency policy. Do not mount or access a volume through this copy until that integration and hardware validation are complete.
