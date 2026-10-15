#!/usr/bin/env python3
"""Check assembled TX start/data/end timing and RX cadence, not electrical qualification.

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


def assemble(program="data_tx"):
    candidates = sorted(glob.glob(str(ROOT / "build/*/pioasm/pioasm")))
    assembler = os.environ.get("PIOASM") or (candidates[0] if candidates else None)
    assert assembler, "Build firmware first or set PIOASM"
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp) / "program.h"
        subprocess.run([assembler, "-o", "c-sdk", str(PIO), str(out)], check=True)
        text = out.read_text()
    start = text.index(f"pf_sdio_pio_{program}_program_instructions[]")
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


def receive_samples(ops, half, latency, phase, pc, cycle, limit, start_delay=None):
    # Executes real WAIT/IN/JMP opcodes; the clock rises at cycles where
    # (cycle + phase - half) % (2 * half) == 0.
    samples, remaining = [], 1039
    for _ in range(limit * half * 4 + 100):
        op = ops[pc]
        kind, dest, immediate = op >> 13, (op >> 5) & 7, op & 31
        next_pc = pc + 1
        if kind == 1:
            assert immediate == 30, "Unexpected RX clock WAIT"
            if ((cycle + phase - latency) // half) % 2 != (op >> 7) & 1:
                cycle += 1
                continue
        elif kind == 2:
            assert dest == 0 and immediate == 4
            samples.append(cycle)
            if len(samples) == limit:
                break
        elif kind == 0:
            if dest == 6:
                assert start_delay is not None
                observed = cycle - latency
                if not start_delay <= observed < start_delay + 2 * half:
                    next_pc = immediate
            else:
                assert dest == 2
                if remaining:
                    next_pc = immediate
                else:
                    break
                remaining -= 1
        else:
            raise AssertionError("Unexpected RX-loop instruction")
        cycle += 1 + ((op >> 8) & 31)
        pc = next_pc
    return samples


def check_receive(ops, half, latency, phase):
    count = 1040
    samples = receive_samples(ops, half, latency, phase, 5, 0, count)
    assert len(samples) == count
    assert all(later - earlier == 2 * half
               for earlier, later in zip(samples, samples[1:])), (
        "RX skipped a nibble", half, latency, phase, samples[:8])


def check_receive_framing(ops, half):
    # Exercise the actual start-detection loop, including DAT0 going low before
    # the rising edge. The old asynchronous WAIT counted that start bit as data.
    period = 2 * half
    # JMP PIN observes DAT0 one instruction after the falling-edge WAIT.
    # Keep one cycle of setup/hold margin around that observation.
    for delay in range(2 - half, half):
        samples = receive_samples(ops, half, 0, half, 2, -2 * period, 3, delay)
        assert len(samples) == 3, (half, delay)
        for index, cycle in enumerate(samples):
            lo = period * (index + 1) + half
            assert lo <= cycle < lo + half, (
                "RX sample outside the second half of its bit", half, delay, index, cycle)


def check_command_response(ops):
    # Execute the response suffix with card output changing just before/after
    # each rising edge. The start bit must not enter the 48/136-bit response.
    jump_pin = next(i for i, op in enumerate(ops)
                    if op >> 13 == 0 and (op >> 5) & 7 == 6)
    for bits in (48, 136):
        expected = [0] + [((i * 7) ^ (i >> 2)) & 1 for i in range(bits - 2)] + [1]
        for delay in (-0.25, 0.25):
            pc, cycle, remaining = jump_pin - 1, 0, bits - 2
            samples = []
            for _ in range(bits * 2 + 20):
                op = ops[pc]
                kind, dest, immediate = op >> 13, (op >> 5) & 7, op & 31
                next_pc = pc + 1
                bit_index = int((cycle - delay) // 2)
                pin = expected[bit_index] if 0 <= bit_index < bits else 1
                if kind == 0 and dest == 6:
                    assert ((op >> 12) & 1) == 0  # falling edge
                    if pin:
                        next_pc = immediate
                elif kind == 2:
                    if dest == 3:
                        samples.append(0)  # start bit, supplied by IN NULL
                    else:
                        assert dest == 0 and ((op >> 12) & 1) == 0
                        samples.append(pin)
                elif kind == 0:
                    assert dest == 4  # JMP Y--
                    if remaining:
                        next_pc = immediate
                    remaining -= 1
                elif kind == 4:
                    break  # PUSH
                else:
                    assert kind == 5  # NOP (MOV)
                pc = next_pc
                cycle += 1
            assert samples == expected, (bits, delay, samples[:16], expected[:16])
    print("Command response framing passed: R1/R2, early/late card output, falling-edge sampling")


def check_tx_status(ops, half):
    # Slow TX CRC-status read: start bit launched at cycle 0 and seen low `delay` cycles later.
    # Status bits are sampled mid-bit; the busy check lands after the end bit's launch.
    # Same card-delay bound as the RX framing check.
    period = 2 * half
    for delay in range(1, period - 1):
        pc, cycle, x, samples = 17, delay + 1, 0, []
        for _ in range(period * 16):
            op = ops[pc]
            kind, dest, immediate = op >> 13, (op >> 5) & 7, op & 31
            next_pc = pc + 1
            if kind == 1 and immediate == 30:
                if ((cycle + half) // half) % 2 != (op >> 7) & 1:
                    cycle += 1
                    continue
            elif kind == 1:
                break  # DAT0 busy wait
            elif kind == 7:
                assert dest == 1
                x = immediate
            elif kind == 2:
                samples.append(cycle)
            elif kind == 0:
                if x:
                    next_pc = immediate
                x = (x - 1) & 0xffffffff
            elif kind != 4:
                raise AssertionError("Unexpected TX-status instruction")
            cycle += 1
            pc = next_pc
        assert len(samples) == 4, (half, delay, samples)
        for index, sampled in enumerate(samples):
            lo = period * (index + 1) + half
            assert lo <= sampled < lo + half, (
                "TX status sample outside the second half of its bit", half, delay, index, sampled)
        lo = period * 5 + half
        assert cycle >= lo, (
            "TX busy check before the card could drive busy", half, delay, cycle)


def main():
    ops = assemble()
    for half in (6, 4, 3, 2):
        for latency in (0, 1):
            for phase in range(2 * half):
                check(ops, half, latency, phase)
        check_tx_status(ops, half)
        print(f"TX timing passed: half={half}, all launch phases, 0/1-cycle observation latency, 1040 nibbles, mid-bit status/busy")
    rx_ops = assemble("data_rx")
    for half in (6, 4, 3, 2):
        for latency in (0, 1):
            for phase in range(2 * half):
                check_receive(rx_ops, half, latency, phase)
        check_receive_framing(rx_ops, half)
        print(f"RX timing passed: half={half}, all phases, 1040 nibbles, falling-edge sampling")
    fast_rx_ops = assemble("data_rx_fast")
    for latency in (0, 1):
        for phase in range(4):
            check_receive(fast_rx_ops, 2, latency, phase)
    check_receive_framing(fast_rx_ops, 2)
    command_ops = assemble("command")
    check_command_response(command_ops)
    assert len(command_ops) + max(len(rx_ops), len(fast_rx_ops)) <= 32, (
        "Command and receive programs exceed shared PIO instruction memory")
    print("Fast RX timing passed: half=2, all phases, 1040 nibbles, falling-edge sampling")
    print("SDIO PIO timing check passed (model only; HIL still required)")


if __name__ == "__main__":
    main()
