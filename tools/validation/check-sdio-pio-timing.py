#!/usr/bin/env python3
"""Check assembled TX start/data/end timing, not electrical qualification.

Model divider-1 data instructions, an integer-half-period clock, and 0/1
cycle clock-observation latency with input_sync_bypass enabled. Pin writes
become effective on the following model cycle. Hardware must separately
qualify pad skew, signal integrity, CRC and sustained transfers.
"""
import glob
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
PIO = ROOT / "firmware/src/drivers/sd_card/sdio_pio/sdio_pio_host.pio"


def assemble():
    candidates = sorted(glob.glob(str(ROOT / "build/*/pioasm/pioasm")))
    assembler = os.environ.get("PIOASM") or (candidates[0] if candidates else None)
    assert assembler, "Build firmware first or set PIOASM"
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp) / "program.h"
        subprocess.run([assembler, "-o", "c-sdk", str(PIO), str(out)], check=True)
        text = out.read_text()
    start = text.index("pf_sdio_pio_data_tx_program_instructions[]")
    body = text[text.index("{", start):text.index("};", start)]
    return [int(line.strip()[:6], 16) for line in body.splitlines()
            if line.strip().startswith("0x")]


def launches(ops, half, latency, phase, count):
    # ponytail: TX prefix only, with a continuously fed FIFO (no DMA stalls).
    # DMA starvation/electrical timing require HIL; extend to FIFO/DMA modelling
    # if those become a failure mode. Execute real WAIT/JMP/OUT/SET opcodes.
    pc, cycle, x = 2, 0, count - 1  # FIFO count pull/out already executed
    pins, driving, data_index = 15, False, 0
    events = []
    for _ in range(count * half * 4 + 100):
        op = ops[pc]
        kind, dest, immediate = op >> 13, (op >> 5) & 7, op & 31
        nxt = pc + 1
        if kind == 1:
            assert (op >> 5) & 3 == 1 and immediate == 30, "Unexpected WAIT"
            if ((cycle + phase - latency) // half) % 2 != (op >> 7) & 1:
                cycle += 1
                continue
        elif kind == 0:
            assert dest == 2, "Expected JMP X--"
            if x:
                nxt = immediate
            x = (x - 1) & 0xffffffff
        elif kind == 7:
            if dest == 0:
                pins = immediate
                if driving:
                    events.append((cycle + 1, "end", pins))
            elif dest == 4:
                driving = immediate != 0
                events.append((cycle + 1, "start" if driving else "release", pins))
                if not driving:
                    return events
            else:
                raise AssertionError("Unexpected SET")
        elif kind == 3:
            assert dest == 0 and immediate == 4 and driving
            events.append((cycle + 1, "data", data_index))
            data_index += 1
        else:
            raise AssertionError("Unexpected TX-prefix instruction")
        cycle += 1
        pc = nxt
    raise AssertionError("TX prefix did not complete")


def check(ops, half, latency, phase):
    count = 1040  # 1024 payload nibbles + 16 CRC nibbles
    events = launches(ops, half, latency, phase, count)
    assert [e[1] for e in events] == ["start"] + ["data"] * count + ["end", "release"]
    assert events[0][2] == 0 and events[-2][2] == 15
    assert [e[2] for e in events[1:-2]] == list(range(count))
    sample_edges = []
    for cycle, name, _ in events:
        # Rising edges occur at half + 2*half*k in clock phase coordinates.
        since_rise = (cycle + phase - half) % (2 * half)
        setup = 2 * half - since_rise
        assert since_rise >= 1 and setup >= 1, (
            half, latency, phase, name, "setup/hold below one 150MHz cycle")
        sample_edges.append(cycle + setup)
    assert all(b - a == 2 * half for a, b in zip(sample_edges, sample_edges[1:])), (
        "Dropped/duplicated start, data or end sampling edge", half, latency, phase)


def main():
    ops = assemble()
    for half in (6, 4, 3):
        for latency in (0, 1):
            for phase in range(2 * half):
                check(ops, half, latency, phase)
        print(f"TX timing passed: half={half}, all launch phases, 0/1-cycle observation latency, 1040 nibbles")
    print("SDIO PIO timing check passed (model only; HIL still required)")


if __name__ == "__main__":
    main()
