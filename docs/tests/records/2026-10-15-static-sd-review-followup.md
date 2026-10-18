# Static SD Review Follow-up

Date: 2026-10-15. Migrated unchanged from [SD-card driver design](../../sdcard-design.md). This is a source-review
record; no hardware measurement was made. Current behavior is summarized in the design's
[MSC reset sense](../../sdcard-design.md#msc-batching-and-validation-boundary) paragraph.

Application READ10/WRITE10 cached sense is now invalidated after TinyUSB handles
an accepted BOT reset at SETUP, and after its MSC driver bus-reset processing.
GNU linker wrappers in `firmware/src/app/usb/usb_msc.c` interpose the built-in driver's
external references from `usbd.c`; the vendored stack is unchanged. No ISR or
enqueue-time hook mutates this cache. Earlier queued failures cannot erase a
reset, and failures after handled reset retain their fresh sense. Rejected
requests, skipped SETUP events, and DATA/ACK stages do not clear it.

`python3 -m pytest tests -m contracts` checks pinned
source ordering. Ceedling runs production application callbacks and linker wrappers
against a separate host driver/queue model. TinyUSB itself is not linked in that
host regression; it is not a real BOT integration or hardware qualification.

At that reset-sense review, the data paths were unchanged: SDIO CMD18 already overlaps completed-block
decode/CRC with later RX DMA, CMD25 alternates two wire buffers while preparing
the next block during TX, and fused CRC/copy remains opt-in. FatFs passes
multi-sector requests through and SPI already uses CMD18/CMD25. No safe small
performance simplification was identified in that review. The later MSC callback
batch-boundary repair is described in the design; the underlying overlap remains. Phase profiling and same-card,
same-clock normal/preallocated and fused-copy A/B comparisons remained pending
HIL; this change made no throughput claim and changed no clocks or buffers.
