#!/usr/bin/env python3
"""Measure battery power consumption over a fixed interval.

By default the script measures idle power for a fixed duration.
An optional workload can be supplied after ``--``.

Battery voltage/current are sampled periodically. The script also records
battery energy (or charge-derived energy) change and writes the result to CSV.

Examples:

    sudo ./measure_power.py --duration 300 --power-supply qcom-battery

    sudo ./measure_power.py \
        --duration 300 \
        --power-supply qcom-battery \
        --output-dir ./power-results

    sudo ./measure_power.py \
        --power-supply qcom-battery \
        -- \
        mpv --hwdec=vaapi video.mp4
"""

from __future__ import annotations

import argparse
import csv
import statistics
import subprocess
import time
from dataclasses import dataclass
from pathlib import Path


DEFAULT_SUPPLY_ROOT = Path("/sys/class/power_supply")


@dataclass(frozen=True)
class BatterySample:
    timestamp: float
    capacity: float | None
    energy_uwh: float | None
    voltage_v: float | None
    current_a: float | None


def read_number(path: Path) -> float | None:
    try:
        return float(path.read_text().strip())
    except (FileNotFoundError, OSError, ValueError):
        return None


def find_supply(root: Path, requested: str | None) -> Path:
    if requested:
        supply = root / requested
        if not supply.is_dir():
            raise SystemExit(f"power supply does not exist: {supply}")
        return supply

    for supply in sorted(root.iterdir()):
        if not (supply / "type").is_file():
            continue

        try:
            if supply.joinpath("type").read_text().strip().lower() == "battery":
                return supply
        except OSError:
            pass

    raise SystemExit(f"could not find a Battery power supply under {root}")


def battery_sample(supply: Path) -> BatterySample:
    energy = read_number(supply / "energy_now")

    voltage_raw = read_number(supply / "voltage_now")

    if energy is None:
        charge = read_number(supply / "charge_now")
        if charge is not None and voltage_raw is not None:
            # charge_now: uAh
            # voltage_now: uV
            #
            # uAh * uV / 1,000,000 = uWh
            energy = charge * voltage_raw / 1_000_000

    current_raw = read_number(supply / "current_now")

    return BatterySample(
        timestamp=time.monotonic(),
        capacity=read_number(supply / "capacity"),
        energy_uwh=energy,
        voltage_v=(
            voltage_raw / 1_000_000
            if voltage_raw is not None
            else None
        ),
        current_a=(
            abs(current_raw) / 1_000_000
            if current_raw is not None
            else None
        ),
    )


def sample_power(sample: BatterySample) -> float | None:
    if sample.voltage_v is None or sample.current_a is None:
        return None

    return sample.voltage_v * sample.current_a


def average_power(samples: list[BatterySample]) -> float | None:
    powers = [
        power
        for sample in samples
        if (power := sample_power(sample)) is not None
    ]

    return statistics.fmean(powers) if powers else None


def power_statistics(samples: list[BatterySample]) -> dict[str, float | None]:
    powers = [
        power
        for sample in samples
        if (power := sample_power(sample)) is not None
    ]

    if not powers:
        return {
            "average": None,
            "min": None,
            "max": None,
            "median": None,
            "stddev": None,
        }

    return {
        "average": statistics.fmean(powers),
        "min": min(powers),
        "max": max(powers),
        "median": statistics.median(powers),
        "stddev": statistics.pstdev(powers),
    }


def run_test(
    label: str,
    supply: Path,
    command: list[str],
    duration: float,
    interval: float,
    output_dir: Path,
) -> dict[str, object]:

    samples: list[BatterySample] = []

    stdout_path = output_dir / f"{label}.stdout.log"
    stderr_path = output_dir / f"{label}.stderr.log"
    samples_path = output_dir / f"{label}.samples.csv"

    print(
        f"run={label} duration={duration}s"
        + (f" command={' '.join(command)}" if command else " idle"),
        flush=True,
    )

    started = time.monotonic()
    samples.append(battery_sample(supply))

    if command:
        with (
            stdout_path.open("wb") as stdout_file,
            stderr_path.open("wb") as stderr_file,
        ):
            process = subprocess.Popen(
                command,
                stdout=stdout_file,
                stderr=stderr_file,
            )

            while process.poll() is None:
                time.sleep(interval)
                samples.append(battery_sample(supply))

            return_code = process.wait()

    else:
        return_code = 0
        end_time = started + duration

        while time.monotonic() < end_time:
            remaining = end_time - time.monotonic()
            time.sleep(min(interval, max(0.0, remaining)))

            samples.append(battery_sample(supply))

    finished = time.monotonic()

    # Final sample
    samples.append(battery_sample(supply))

    actual_duration = finished - started

    start = samples[0]
    end = samples[-1]

    energy_delta = (
        start.energy_uwh - end.energy_uwh
        if start.energy_uwh is not None
        and end.energy_uwh is not None
        else None
    )

    capacity_delta = (
        start.capacity - end.capacity
        if start.capacity is not None
        and end.capacity is not None
        else None
    )

    stats = power_statistics(samples)

    #
    # Write every individual sample.
    #
    with samples_path.open("w", newline="") as output:
        writer = csv.writer(output, lineterminator="\n")

        writer.writerow([
            "elapsed_s",
            "capacity_percent",
            "energy_uwh",
            "voltage_v",
            "current_a",
            "power_w",
        ])

        first_timestamp = samples[0].timestamp

        for sample in samples:
            writer.writerow([
                round(sample.timestamp - first_timestamp, 3),
                sample.capacity if sample.capacity is not None else "",
                sample.energy_uwh if sample.energy_uwh is not None else "",
                (
                    round(sample.voltage_v, 6)
                    if sample.voltage_v is not None
                    else ""
                ),
                (
                    round(sample.current_a, 6)
                    if sample.current_a is not None
                    else ""
                ),
                (
                    round(sample_power(sample), 6)
                    if sample_power(sample) is not None
                    else ""
                ),
            ])

    row: dict[str, object] = {
        "case": label,
        "exit_code": return_code,
        "duration_s": round(actual_duration, 3),
        "samples": len(samples),

        "capacity_start_percent": start.capacity,
        "capacity_end_percent": end.capacity,

        "battery_consumption_percent": (
            round(capacity_delta, 6)
            if capacity_delta is not None
            else ""
        ),

        "energy_start_uwh": start.energy_uwh,
        "energy_end_uwh": end.energy_uwh,

        "battery_consumption_uwh": (
            round(energy_delta, 3)
            if energy_delta is not None
            else ""
        ),

        "average_power_w": (
            round(stats["average"], 6)
            if stats["average"] is not None
            else ""
        ),

        "median_power_w": (
            round(stats["median"], 6)
            if stats["median"] is not None
            else ""
        ),

        "min_power_w": (
            round(stats["min"], 6)
            if stats["min"] is not None
            else ""
        ),

        "max_power_w": (
            round(stats["max"], 6)
            if stats["max"] is not None
            else ""
        ),

        "stddev_power_w": (
            round(stats["stddev"], 6)
            if stats["stddev"] is not None
            else ""
        ),

        "samples_csv": str(samples_path),

        "stdout_log": (
            str(stdout_path)
            if command
            else ""
        ),

        "stderr_log": (
            str(stderr_path)
            if command
            else ""
        ),

        "command": " ".join(command),
    }

    print(
        f"  duration={row['duration_s']}s "
        f"samples={row['samples']} "
        f"consumption={row['battery_consumption_uwh']}uWh "
        f"avg_power={row['average_power_w']}W "
        f"median={row['median_power_w']}W "
        f"min={row['min_power_w']}W "
        f"max={row['max_power_w']}W "
        f"stddev={row['stddev_power_w']}W "
        f"exit={return_code}",
        flush=True,
    )

    return row


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)

    parser.add_argument(
        "--power-supply",
        help="power-supply directory name, e.g. qcom-battery",
    )

    parser.add_argument(
        "--power-supply-root",
        type=Path,
        default=DEFAULT_SUPPLY_ROOT,
    )

    parser.add_argument(
        "--interval",
        type=float,
        default=1.0,
        help="battery sampling interval in seconds (default: 1)",
    )

    parser.add_argument(
        "--duration",
        type=float,
        default=300.0,
        help="measurement duration in seconds (default: 300)",
    )

    parser.add_argument(
        "--label",
        default="power-test",
        help="test name used in output files",
    )

    parser.add_argument(
        "--output-dir",
        type=Path,
        default=Path("power-results"),
    )

    parser.add_argument(
        "--results-file",
        type=Path,
        help="CSV path (defaults to OUTPUT_DIR/results.csv)",
    )

    parser.add_argument(
        "command",
        nargs=argparse.REMAINDER,
        help="optional workload command after --",
    )

    args = parser.parse_args()

    if args.duration <= 0:
        parser.error("--duration must be greater than zero")

    if args.interval <= 0:
        parser.error("--interval must be greater than zero")

    command = args.command

    if command and command[0] == "--":
        command = command[1:]

    supply = find_supply(
        args.power_supply_root,
        args.power_supply,
    )

    args.output_dir.mkdir(
        parents=True,
        exist_ok=True,
    )

    row = run_test(
        label=args.label,
        supply=supply,
        command=command,
        duration=args.duration,
        interval=args.interval,
        output_dir=args.output_dir,
    )

    csv_path = (
        args.results_file
        or args.output_dir / "results.csv"
    )

    csv_path.parent.mkdir(
        parents=True,
        exist_ok=True,
    )

    with csv_path.open("w", newline="") as output:
        writer = csv.DictWriter(
            output,
            fieldnames=list(row),
            lineterminator="\n",
        )

        writer.writeheader()
        writer.writerow(row)

    print(f"wrote {csv_path}")

    return 1 if row["exit_code"] != 0 else 0


if __name__ == "__main__":
    raise SystemExit(main())
