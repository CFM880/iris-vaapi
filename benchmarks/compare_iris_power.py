#!/usr/bin/env python3
"""Compare battery consumption with the qcom_iris module loaded and unloaded.

By default the script waits for a fixed idle interval in each module state.
An optional workload can be supplied after ``--`` for future experiments.
Battery voltage/current are sampled during the interval.  The script also
records the battery energy (or charge) change and writes one CSV row per run.

This script changes kernel module state and normally needs to be run as root,
for example: ``sudo ./compare_iris_power.py --duration 60``.
"""

from __future__ import annotations

import argparse
import csv
import statistics
import subprocess
import time
from dataclasses import dataclass
from pathlib import Path


MODULE = "qcom_iris"
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
        if (supply / "type").is_file():
            try:
                if supply.joinpath("type").read_text().strip().lower() == "battery":
                    return supply
            except OSError:
                pass
    raise SystemExit(f"could not find a Battery power supply under {root}")


def battery_sample(supply: Path) -> BatterySample:
    energy = read_number(supply / "energy_now")
    if energy is None:
        charge = read_number(supply / "charge_now")
        voltage = read_number(supply / "voltage_now")
        if charge is not None and voltage is not None:
            # charge_now is uAh and voltage_now is uV, so the result is uWh.
            energy = charge * voltage / 1_000_000

    voltage_raw = read_number(supply / "voltage_now")
    current_raw = read_number(supply / "current_now")
    return BatterySample(
        timestamp=time.monotonic(),
        capacity=read_number(supply / "capacity"),
        energy_uwh=energy,
        voltage_v=voltage_raw / 1_000_000 if voltage_raw is not None else None,
        current_a=abs(current_raw) / 1_000_000 if current_raw is not None else None,
    )


def module_loaded(module: str) -> bool:
    return Path("/sys/module").joinpath(module).exists()


def set_module(module: str, loaded: bool) -> None:
    current = module_loaded(module)
    if current == loaded:
        return
    action = "load" if loaded else "unload"
    command = ["modprobe", module] if loaded else ["modprobe", "-r", module]
    print(f"{action} {module}: {' '.join(command)}", flush=True)
    try:
        subprocess.run(command, check=True)
    except FileNotFoundError as exc:
        raise SystemExit("modprobe was not found") from exc
    if module_loaded(module) != loaded:
        raise SystemExit(f"module state did not change after trying to {action} {module}")


def average_power(samples: list[BatterySample]) -> float | None:
    powers = [
        sample.voltage_v * sample.current_a
        for sample in samples
        if sample.voltage_v is not None and sample.current_a is not None
    ]
    return statistics.fmean(powers) if powers else None


def run_case(
    label: str,
    module: str,
    loaded: bool,
    supply: Path,
    command: list[str],
    duration: float,
    interval: float,
    output_dir: Path,
) -> dict[str, object]:
    set_module(module, loaded)
    time.sleep(1.0)
    samples = [battery_sample(supply)]
    started = time.monotonic()
    stdout_path = output_dir / f"{label}.stdout.log"
    stderr_path = output_dir / f"{label}.stderr.log"
    print(
        f"run={label} duration={duration}s"
        + (f" command={' '.join(command)}" if command else " idle"),
        flush=True,
    )
    if command:
        with stdout_path.open("wb") as stdout_file, stderr_path.open("wb") as stderr_file:
            process = subprocess.Popen(command, stdout=stdout_file, stderr=stderr_file)
            while process.poll() is None:
                time.sleep(interval)
                samples.append(battery_sample(supply))
            return_code = process.wait()
    else:
        return_code = 0
        while time.monotonic() < started + duration:
            time.sleep(min(interval, max(0.0, started + duration - time.monotonic())))
            samples.append(battery_sample(supply))
    finished = time.monotonic()
    samples.append(battery_sample(supply))

    duration = finished - started
    start, end = samples[0], samples[-1]
    energy_delta = (
        start.energy_uwh - end.energy_uwh
        if start.energy_uwh is not None and end.energy_uwh is not None
        else None
    )
    capacity_delta = (
        start.capacity - end.capacity
        if start.capacity is not None and end.capacity is not None
        else None
    )
    power = average_power(samples)
    row: dict[str, object] = {
        "case": label,
        "iris_loaded": loaded,
        "exit_code": return_code,
        "duration_s": round(duration, 3),
        "samples": len(samples),
        "capacity_start_percent": start.capacity,
        "capacity_end_percent": end.capacity,
        "battery_consumption_percent": round(capacity_delta, 6) if capacity_delta is not None else "",
        "energy_start_uwh": start.energy_uwh,
        "energy_end_uwh": end.energy_uwh,
        "battery_consumption_uwh": round(energy_delta, 3) if energy_delta is not None else "",
        "average_power_w": round(power, 6) if power is not None else "",
        "stdout_log": str(stdout_path),
        "stderr_log": str(stderr_path),
        "command": " ".join(command),
    }
    print(
        f"  duration={row['duration_s']}s samples={row['samples']} "
        f"consumption={row['battery_consumption_uwh']}uWh "
        f"avg_power={row['average_power_w']}W exit={return_code}",
        flush=True,
    )
    return row


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--module", default=MODULE, help="kernel module name (default: qcom_iris)")
    parser.add_argument("--power-supply", help="power-supply directory name, e.g. qcom-battery")
    parser.add_argument("--power-supply-root", type=Path, default=DEFAULT_SUPPLY_ROOT)
    parser.add_argument("--interval", type=float, default=1.0, help="battery sampling interval in seconds")
    parser.add_argument("--output-dir", type=Path, default=Path("iris-power-results"))
    parser.add_argument("--results-file", type=Path, help="CSV path (defaults to OUTPUT_DIR/results.csv)")
    parser.add_argument("--keep-module-state", action="store_true", help="do not restore the initial module state")
    parser.add_argument("--duration", type=float, default=60.0,
                        help="idle measurement duration per state in seconds (default: 60)")
    parser.add_argument("command", nargs=argparse.REMAINDER,
                        help="optional workload command after --")
    args = parser.parse_args()

    command = args.command
    if command and command[0] == "--":
        command = command[1:]
    if args.duration <= 0:
        parser.error("--duration must be greater than zero")
    if args.interval <= 0:
        parser.error("--interval must be greater than zero")

    supply = find_supply(args.power_supply_root, args.power_supply)
    args.output_dir.mkdir(parents=True, exist_ok=True)
    initial_state = module_loaded(args.module)
    rows: list[dict[str, object]] = []
    try:
        for label, loaded in (("iris-loaded", True), ("iris-unloaded", False)):
            rows.append(
                run_case(label, args.module, loaded, supply, command, args.duration,
                         args.interval, args.output_dir)
            )
    finally:
        if not args.keep_module_state:
            set_module(args.module, initial_state)

    csv_path = args.results_file or args.output_dir / "results.csv"
    csv_path.parent.mkdir(parents=True, exist_ok=True)
    with csv_path.open("w", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=list(rows[0]), lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)

    loaded, unloaded = rows
    print(f"wrote {csv_path}")
    if loaded["average_power_w"] != "" and unloaded["average_power_w"] != "":
        delta = float(loaded["average_power_w"]) - float(unloaded["average_power_w"])
        print(f"iris average-power difference: {delta:+.6f} W (loaded - unloaded)")
    if loaded["battery_consumption_uwh"] != "" and unloaded["battery_consumption_uwh"] != "":
        delta = float(loaded["battery_consumption_uwh"]) - float(unloaded["battery_consumption_uwh"])
        print(f"iris battery-consumption difference: {delta:+.3f} uWh (loaded - unloaded)")
    return 1 if any(row["exit_code"] != 0 for row in rows) else 0


if __name__ == "__main__":
    raise SystemExit(main())
