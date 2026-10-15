#!/usr/bin/env python3
"""Compare paired, passing CMD25 persistence benchmark write times."""

import argparse
import statistics


def positive_milliseconds(value: str) -> int:
    milliseconds = int(value)
    if milliseconds <= 0:
        raise argparse.ArgumentTypeError("times must be positive milliseconds")
    return milliseconds


def relative_mad(values: list[int], median: float) -> float:
    deviations = [abs(value - median) for value in values]
    return statistics.median(deviations) * 100.0 / median


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Compare successful same-size, same-card CMD25 A/B write times")
    parser.add_argument("--persistent-ms", nargs="+", type=positive_milliseconds, required=True)
    parser.add_argument("--closed-ms", nargs="+", type=positive_milliseconds, required=True)
    arguments = parser.parse_args()

    if len(arguments.persistent_ms) != len(arguments.closed_ms):
        parser.error("persistent and closed variants require the same number of runs")
    if len(arguments.persistent_ms) < 3:
        parser.error("at least three passing runs per variant are required")

    persistent_median = statistics.median(arguments.persistent_ms)
    closed_median = statistics.median(arguments.closed_ms)
    persistent_gain = (closed_median / persistent_median - 1.0) * 100.0

    print(f"runs_per_variant={len(arguments.persistent_ms)}")
    print(f"persistent_median_ms={persistent_median:g}")
    print(f"closed_median_ms={closed_median:g}")
    print(f"persistent_relative_mad={relative_mad(arguments.persistent_ms, persistent_median):.2f}%")
    print(f"closed_relative_mad={relative_mad(arguments.closed_ms, closed_median):.2f}%")
    print(f"persistent_throughput_gain={persistent_gain:.2f}%")
    if persistent_gain < 5.0:
        print("decision=close streams per facade call")
    elif persistent_gain < 10.0:
        print("decision=inconclusive; collect at least two more paired runs")
    else:
        print("decision=retain cross-call persistence")


if __name__ == "__main__":
    main()
