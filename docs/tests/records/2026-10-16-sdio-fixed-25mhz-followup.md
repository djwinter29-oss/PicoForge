# Fixed-25-MHz SDIO Follow-up

Date: 2026-10-16. Migrated unchanged from [SD-card driver design](../../sdcard-design.md).

**Scope:** the connected 128 GB SB128 card on the fixed-clock implementation at a 25 MHz transfer target. These checks
did not establish sustained throughput and do not describe the current firmware.

| CPU clock | Observed result |
| --- | --- |
| 250 MHz | Initialization failed at the SCR CRC check, before the transfer clock was selected; self-test rejected because no volume was mounted |
| 150 MHz | Initialized and mounted at 25 MHz; the authorized 32 MiB self-test failed during preparation with `FR_DISK_ERR`, zero payload bytes written/read, and two profiled receive CRC failures |
| 200 MHz | Initialized and mounted at 25 MHz; the same test failed during preparation with `FR_DISK_ERR`, zero payload bytes written/read, and two profiled receive CRC failures |

Receive captures showed both a nibble-shifted boot-sector image and a correctly
positioned `55aa` signature with CRC discrepancies on DAT1-DAT3. A temporary
1 MHz transfer-clock control produced a byte-for-byte identical bad frame to
one captured at 25 MHz. A cold-powered SPI control initialized and mounted the
same card at 25 MHz. This narrows the investigation to native transfer/framing
and its electrical behavior; it does not prove a specific sampling fix. Debugger
capture attempts did not yield a verified SPI-sector comparison.

The diagnostic clock override was removed and the 150 MHz CPU / fixed-25-MHz
SDIO firmware restored. Fully remove card power after the SPI control before
using native SDIO again.
No filesystem format was performed. No valid benchmark rate was obtained.
These observations identify a receive-integrity problem, not its cause; card
speed rating does not distinguish driver sampling/framing faults from wiring,
power, or signal-integrity faults. Neither a 10 MB/s result nor reliable
sustained operation is qualified by these tests.
