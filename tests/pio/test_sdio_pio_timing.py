#!/usr/bin/env python3
"""Test assembled TX timing/completion and RX cadence, not electrical qualification.

Model divider-1 data instructions, an integer-half-period clock, and 0/1
cycle clock-observation latency with input_sync_bypass enabled; a separate
system-cycle RX model covers 200/250 MHz CPU and exact 50 MHz clock cadence. Pin writes
become effective on the following model cycle. Hardware must separately
qualify pad skew, signal integrity, CRC and sustained transfers.
"""
import glob
import os
from bisect import bisect_right
from pathlib import Path
import subprocess
import sys
import tempfile

import pytest

pytestmark = pytest.mark.pio

ROOT = Path(__file__).resolve().parents[2]
PIO = ROOT / "firmware/src/drivers/sd_card/sdio_pio/sdio_pio.pio"


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


def check_receive_framing(ops, half, falling_launch=False):
    # Exercise the actual start-detection loop, including DAT0 going low before
    # the rising edge. The old asynchronous WAIT counted that start bit as data.
    period = 2 * half
    # JMP PIN observes DAT0 one instruction after the settled sampling edge.
    # Keep one cycle of setup/hold margin around that observation.
    for delay in range(2 - half, half):
        samples = receive_samples(ops, half, 0, 0 if falling_launch else half, 2, -2 * period, 3, delay)
        assert len(samples) == 3, (half, delay)
        for index, cycle in enumerate(samples):
            lo = period * (index + 1) + half
            assert lo <= cycle < lo + half, (
                "RX sample outside the second half of its launched bit", half, delay, index, cycle)


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
    # Slow TX CRC-status read. A Default-Speed card launches on falling CLK: the start bit is launched at
    # cycle 0 (CLK just fell) and seen low `delay` cycles later. Status and busy must be sampled in the
    # second half of each bit (after the rising edge), never at the launch instant, where a card output
    # delay of a few ns decides which bit is read. That held at 150 MHz by margin and failed from 200 MHz.
    # Same card-delay bound as the RX framing check.
    period = 2 * half
    for delay in range(1, period - 1):
        pc, cycle, x, samples = 17, delay + 1, 0, []
        for _ in range(period * 16):
            op = ops[pc]
            kind, dest, immediate = op >> 13, (op >> 5) & 7, op & 31
            next_pc = pc + 1
            if kind == 1 and immediate == 30:
                if (cycle // half) % 2 != (op >> 7) & 1:
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


@pytest.fixture(scope="module")
def programs():
    return {name: assemble(name) for name in ("data_tx", "data_tx_fast", "data_rx", "data_rx_fast", "command")}


def run_fast_tx(ops, ticks, ceil_edges, latency, count=1040, delay=0, status=2, busy=3, gap=2):
    """Execute the assembled data_tx_fast opcodes against a clock/card model in system cycles.

    sdio_pio.c runs the clock SM at `ticks`/256 and the TX SM at exactly half that (clkdiv_restart plus
    enable_sm_mask_in_sync start both at cycle 0), so TX tick j and clock tick j/2 coincide. Tick j executes at
    system cycle floor(j*ticks/512) (ceil for the other bound); pin writes take effect one system cycle later and
    PIO inputs are observed `latency` cycles late. The card launches on rising CLK plus `delay` cycles: after the
    end bit sampled on rise e it sends `gap` bit-times later start(0), status[2:0], end(1), `busy` low bits, high.
    Not modelled: DMA/FIFO starvation, pad skew, real card tODLY/tOH; those need HIL.
    """
    at = (lambda j: -(-j * ticks // 512)) if ceil_edges else (lambda j: j * ticks // 512)
    total = count + 40 + busy
    rises = [at(4 * m + 2) + 1 for m in range(total)]
    falls = [at(4 * m) + 1 for m in range(total)]
    clk = lambda s: int(bisect_right(rises, s) == bisect_right(falls, s))
    host_times, host_state, card_starts, stream = [-1], [(False, 15)], [], []

    def d0(s):
        driving_now, value = host_state[bisect_right(host_times, s) - 1]
        if driving_now:
            return value & 1
        index = bisect_right(card_starts, s) - 1
        return stream[index] if index >= 0 else 1

    def drive(effective, is_driving, value):
        host_times.append(effective)
        host_state.append((is_driving, value))

    events, samples, isr, osr, word = [], [], 0, 0, None
    pc = j = x = y = 0
    pins, driving, nibble = 15, False, 0
    for _ in range(count * 8 + 400):
        op, t = ops[pc], at(j)
        kind, dest, imm, dly = op >> 13, (op >> 5) & 7, op & 31, (op >> 8) & 31
        nxt, advance = pc + 1, True
        if kind == 1:
            assert (op >> 5) & 3 == 1 and imm in (0, 30), "Unexpected WAIT"
            advance = (clk(t - latency) if imm == 30 else d0(t - latency)) == (op >> 7) & 1
        elif kind == 0:
            assert dest in (2, 4), "Unexpected JMP"  # X-- / Y--
            counter = x if dest == 2 else y
            if counter:
                nxt = imm
            if dest == 2:
                x = (x - 1) & 0xffffffff
            else:
                y = (y - 1) & 0xffffffff
        elif kind == 4 and op & 0xe0 == 0xa0:
            osr = count - 1  # PULL block: the first FIFO word is nibbles - 1
        elif kind == 4 and op & 0xe0 == 0x20:
            word, isr = isr, 0  # PUSH block
        elif kind == 3:
            if dest == 1:
                x = osr
            else:
                assert dest == 0 and imm == 4 and driving, "Unexpected OUT"
                pins = nibble & 15
                drive(t + 1, True, pins)
                events.append((j, t + 1, "data", nibble))
                nibble += 1
        elif kind == 5:
            pass  # NOP (MOV Y,Y)
        elif kind == 7:
            if dest == 0:
                pins = imm
                if driving:
                    drive(t + 1, True, pins)
                    events.append((j, t + 1, "end", pins))
                    first = bisect_right(rises, t + 1) + gap
                    card_starts[:] = [rises[first + i] + delay for i in range(6 + busy)]
                    stream[:] = [0, (status >> 2) & 1, (status >> 1) & 1, status & 1, 1] + [0] * busy + [1]
            elif dest == 1:
                x = imm
            elif dest == 2:
                y = imm
            elif dest == 4:
                driving = imm != 0
                drive(t + 1, driving, pins)
                events.append((j, t + 1, "start" if driving else "release", pins))
            else:
                raise AssertionError("Unexpected SET")
        elif kind == 2:
            assert dest == 0 and imm == 1, "Unexpected IN"
            samples.append((t, d0(t - latency)))
            isr = (isr << 1) | samples[-1][1]
        elif kind == 6:
            assert imm == 2 and op & 0x20, "Expected IRQ WAIT 2"
            return {"events": events, "samples": samples, "word": word, "done": t, "rises": rises,
                    "high": card_starts[5 + busy] if card_starts else None}
        else:
            raise AssertionError("Unexpected fast-TX instruction")
        if advance:
            pc = nxt
            j += 1 + dly
        else:
            j += 1
    raise AssertionError("Fast TX did not complete")


FAST_TX_DIVIDERS = (512, 576, 640, 704, 768)  # 200 and 250 MHz stock ratios are 512 and 640


def check_fast_tx_launch(ops, ticks, ceil_edges, latency):
    count = 1040
    run = run_fast_tx(ops, ticks, ceil_edges, latency, count=count, busy=1)
    events, rises = run["events"], run["rises"]
    assert [e[2] for e in events] == ["start"] + ["data"] * count + ["end", "release"]
    assert events[0][3] == 0 and events[-2][3] == 15
    assert [e[3] for e in events[1:-2]] == list(range(count))
    assert {e[0] % 4 for e in events} == {3}, "Launches must follow the clock rise by one SM cycle"
    assert all(b[0] - a[0] == 4 for a, b in zip(events, events[1:])), "Launch cadence is not one bit time"
    edges = []
    for _, effective, name, _ in events:
        since_rise = effective - rises[bisect_right(rises, effective) - 1]
        setup = rises[bisect_right(rises, effective)] - effective
        assert since_rise >= 1 and setup >= 1, (ticks, ceil_edges, latency, name, "setup/hold below one cycle")
        edges.append(bisect_right(rises, effective))
    assert all(b - a == 1 for a, b in zip(edges, edges[1:])), "Dropped/duplicated sampling edge"


# Card output delay after the rising edge, in system cycles. Zero aliases to the next bit at divider 2.75 with one
# cycle of input latency, and three or more samples the wrong bit at 50 MHz; real cards need HIL qualification.
FAST_TX_CARD_DELAYS = (1, 2)


def check_fast_tx_status(ops, ticks, ceil_edges, latency):
    for delay in FAST_TX_CARD_DELAYS:
        for status in (2, 5, 7):  # accepted, CRC error, write error
            for busy in (1, 5):
                for gap in (1, 2, 3):
                    run = run_fast_tx(ops, ticks, ceil_edges, latency, count=8, delay=delay, status=status,
                                      busy=busy, gap=gap)
                    ctx = (ticks, ceil_edges, latency, delay, status, busy, gap)
                    assert run["word"] == (status << 1) | 1, ("Wrong status bits", run["word"], ctx)
                    assert run["done"] - latency >= run["high"], ("Completed while the card was busy", ctx)


@pytest.mark.parametrize("ceil_edges", (False, True))
@pytest.mark.parametrize("latency", (0, 1))
@pytest.mark.parametrize("ticks", FAST_TX_DIVIDERS)
def test_fast_tx_launch_timing(programs, ticks, latency, ceil_edges):
    check_fast_tx_launch(programs["data_tx_fast"], ticks, ceil_edges, latency)


@pytest.mark.parametrize("ceil_edges", (False, True))
@pytest.mark.parametrize("latency", (0, 1))
@pytest.mark.parametrize("ticks", FAST_TX_DIVIDERS)
def test_fast_tx_status_and_busy(programs, ticks, latency, ceil_edges):
    check_fast_tx_status(programs["data_tx_fast"], ticks, ceil_edges, latency)


TIMINGS = [(half, latency, phase) for half in (6, 4, 3, 2)
           for latency in (0, 1) for phase in range(2 * half)]


@pytest.mark.parametrize("half,latency,phase", TIMINGS)
def test_tx_timing(programs, half, latency, phase):
    check(programs["data_tx"], half, latency, phase)


@pytest.mark.parametrize("half,latency,phase", TIMINGS)
def test_rx_timing(programs, half, latency, phase):
    check_receive(programs["data_rx"], half, latency, phase)


@pytest.mark.parametrize("half", (6, 4, 3, 2))
def test_tx_status(programs, half):
    check_tx_status(programs["data_tx"], half)


@pytest.mark.parametrize("half", (6, 4, 3, 2))
def test_rx_framing(programs, half):
    check_receive_framing(programs["data_rx"], half, falling_launch=True)


def test_250mhz_default_speed_rx_samples_after_rising_edge(programs):
    ops = programs["data_rx"]
    assert ops[2:4] == ops[5:7] == [0x203e, 0x20be], "Start and payload must settle after falling CLK"
    for latency in (0, 1):
        for phase in range(10):
            check_receive(ops, 5, latency, phase)
    check_receive_framing(ops, 5, falling_launch=True)


@pytest.mark.parametrize("latency,phase", [(latency, phase) for latency in (0, 1) for phase in range(4)])
def test_fast_rx_timing(programs, latency, phase):
    check_receive(programs["data_rx_fast"], 2, latency, phase)


def test_fast_rx_framing(programs):
    check_receive_framing(programs["data_rx_fast"], 2)


def fast_receive_system_cycles(ops, period, phase, latency):
    """Run system-clock RX with integer/fractional command-clock half-periods."""
    pc, remaining = 5, 1039
    samples = []
    for cycle in range(5300):
        op = ops[pc]
        kind, condition = op >> 13, (op >> 5) & 7
        if kind == 1:
            assert op & 31 == 30
            if (2 * (cycle + phase - latency) // period) % 2 != (op >> 7) & 1:
                continue
        elif kind == 2:
            samples.append(cycle)
        else:
            assert kind == 0 and condition == 2
            if not remaining:
                return samples
            remaining -= 1
            pc = op & 31
            continue
        assert ((op >> 8) & 31) == 0, "RX needs one system cycle between WAIT and IN"
        pc += 1
    raise AssertionError("Fast RX did not complete")


@pytest.mark.parametrize("period,phase,latency", [
    (period, phase, latency) for period in (4, 5) for phase in range(period) for latency in (0, 1)])
def test_fast_rx_system_clock_sampling_at_200_and_250mhz(programs, period, phase, latency):
    samples = fast_receive_system_cycles(programs["data_rx_fast"], period, phase, latency)
    assert len(samples) == 1040
    assert all(b - a == period for a, b in zip(samples, samples[1:])), "Dropped/duplicated 50 MHz nibble"
    for cycle in samples:
        assert (cycle - 1 + phase - latency) % period == 0, "IN must follow observed falling CLK by one cycle"


def divider_phases(ticks, ceil_edges):
    """Alternating CLK low/high durations (system cycles) of the clock program under a fractional divider.

    The hardware's tick placement is not documented, so bound it by both floor and ceiling placement.
    """
    edge = (lambda k: -(-k * ticks // 256)) if ceil_edges else (lambda k: k * ticks // 256)
    return [edge(k + 1) - edge(k) for k in range(12 * 1040)]


def fast_receive_with_phases(ops, durations, latency):
    """Run the High-Speed RX loop against explicit CLK phases (starting low); return sample and fall cycles."""
    levels = []
    for index, duration in enumerate(durations):
        levels += [index % 2] * duration
    falls = [i for i in range(1, len(levels)) if levels[i - 1] == 1 and levels[i] == 0]
    pc, remaining, cycle, samples = 5, 1039, 0, []
    while cycle < len(levels) - 8:
        op = ops[pc]
        kind = op >> 13
        if kind == 1:
            if levels[max(cycle - latency, 0)] != (op >> 7) & 1:
                cycle += 1
                continue
        elif kind == 2:
            samples.append(cycle)
        else:
            if not remaining:
                break
            remaining -= 1
            pc = op & 31
            cycle += 1
            continue
        pc += 1
        cycle += 1
    return samples, falls


def samples_follow_consecutive_falls(samples, falls, latency):
    import bisect
    owners = [bisect.bisect_right(falls, cycle - latency - 1) - 1 for cycle in samples]
    return (len(samples) == 1040 and all(b - a == 1 for a, b in zip(owners, owners[1:]))
            and all(cycle - latency - falls[owner] == 1 for cycle, owner in zip(samples, owners)))


@pytest.mark.parametrize("ceil_edges", (False, True))
@pytest.mark.parametrize("latency", (0, 1))
@pytest.mark.parametrize("ticks", (512, 576, 640, 704, 768))
def test_fast_rx_takes_one_nibble_per_clock_for_every_divider_the_planner_accepts(
        programs, ticks, latency, ceil_edges):
    # sdio_pio.c rejects High-Speed dividers above HIGH_SPEED_DIVIDER_MAX_TICKS (3.0 == 768 ticks).
    durations = divider_phases(ticks, ceil_edges)
    assert max(durations[0::2]) <= 3, "CLK low phase exceeds the fast loop's three-cycle rearm time"
    samples, falls = fast_receive_with_phases(programs["data_rx_fast"], durations, latency)
    assert samples_follow_consecutive_falls(samples, falls, latency), (ticks, latency, ceil_edges)


@pytest.mark.parametrize("ticks", (1024, 1280))
def test_fast_rx_samples_a_nibble_twice_once_the_low_phase_reaches_four_cycles(programs, ticks):
    # Documents the hazard behind the planner's divider ceiling: a longer low phase satisfies the next
    # WAIT 0 before CLK rises, so the loop counts the same bit period as two nibbles.
    durations = divider_phases(ticks, False)
    samples, falls = fast_receive_with_phases(programs["data_rx_fast"], durations, 0)
    assert not samples_follow_consecutive_falls(samples, falls, 0)


def test_command_response(programs):
    check_command_response(programs["command"])


def test_instruction_memory(programs):
    assert len(programs["command"]) + max(len(programs["data_rx"]), len(programs["data_rx_fast"])) <= 32, (
        "Command and receive programs exceed shared PIO instruction memory")


def fast_tx_mutants(ops):
    """Single-opcode mutations of the assembled fast TX program, with the check each must trip."""
    decode = lambda op: (op >> 13, (op >> 5) & 7, (op >> 8) & 31)
    nop = next(i for i, op in enumerate(ops) if decode(op)[0] == 5 and decode(op)[2] == 2)
    jump = next(i for i, op in enumerate(ops) if decode(op)[:2] == (0, 2) and decode(op)[2] == 1)
    sample = next(i for i, op in enumerate(ops) if decode(op)[0] == 2) - 1
    count = next(i for i, op in enumerate(ops) if decode(op)[:2] == (7, 1))
    busy = len(ops) - 2
    assert [decode(ops[i])[0] for i in (sample, busy)] == [1, 1], "Mutation targets moved; update the model"
    mutate = lambda index, op: ops[:index] + [op] + ops[index + 1:]
    return {
        "launch phase": (check_fast_tx_launch, mutate(nop, ops[nop] & ~0x1f00 | 0x100)),
        "launch cadence": (check_fast_tx_launch, mutate(jump, ops[jump] & ~0x1f00)),
        "status edge": (check_fast_tx_status, mutate(sample, ops[sample] ^ 0x80)),
        "status bit count": (check_fast_tx_status, mutate(count, ops[count] - 1)),
        "busy wait": (check_fast_tx_status, mutate(busy, 0xa042)),
    }


@pytest.mark.parametrize("name", ("launch phase", "launch cadence", "status edge", "status bit count", "busy wait"))
def test_fast_tx_model_detects_mutated_opcodes(programs, name):
    check, mutated = fast_tx_mutants(programs["data_tx_fast"])[name]
    check(programs["data_tx_fast"], 640, False, 0)
    with pytest.raises(AssertionError):
        check(mutated, 640, False, 0)


@pytest.mark.parametrize("program", ("data_tx", "data_tx_fast"))
@pytest.mark.parametrize("cpu_delay", (2, 32, 1000))
def test_tx_completion_does_not_pull_empty_fifo_before_cpu_disables_sm(programs, program, cpu_delay):
    ops = programs[program]
    pc = len(ops) - 1
    irq = False
    tx_stall = False
    for _ in range(cpu_delay):
        op = ops[pc]
        kind = op >> 13
        if kind == 6:
            assert op & 0x1f == 2 and not op & 0x40, "Expected TX completion IRQ set/wait"
            irq = True
            if op & 0x20:
                continue  # IRQ WAIT parks until the CPU acknowledges the completion flag.
        else:
            assert kind == 4 and op & 0xe0 == 0xa0, "Expected wrapped blocking PULL"
            tx_stall = True
            break
        pc = (pc + 1) % len(ops)
    assert irq
    assert not tx_stall, "Completed TX wrapped to an empty PULL and falsely reported starvation"


def test_assemble_does_not_require_build_directory(tmp_path, monkeypatch):
    fake = tmp_path / "pioasm"
    fake.write_text('#!/bin/sh\nprintf "pf_sdio_pio_data_tx_program_instructions[] = {\\n0xe000,\\n};\\n" > "$4"\n')
    fake.chmod(0o755)
    monkeypatch.setenv("PIOASM", str(fake))
    monkeypatch.setattr(sys.modules[__name__], "ROOT", tmp_path / "no-build")
    assert assemble() == [0xe000]
