#!/usr/bin/env python3
"""Pinned Fluster conformance baseline: software, V4L2 M2M and VA-API."""

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys

REVISION = "f3ad284a9e6cac70dc01b02e0de71c2994181d34"
VECTORS = {
    "h.264/JVT-AVC_V1": ["AUD_MW_E"],
    "h.265/JCT-VC-HEVC_V1": ["AMP_A_Samsung_7", "DBLK_A_MAIN10_VIXS_4"],
    "vp9/VP9-TEST-VECTORS": ["vp90-2-00-quantizer-00.webm"],
    "vp9/VP9-TEST-VECTORS-HIGH": ["vp92-2-20-10bit-yuv420.webm"],
}
SUFFIXES = {"software": "", "v4l2": "-v4l2m2m", "vaapi": "-VAAPI"}
EXTENDED_VECTORS = {
    "h.264/JVT-AVC_V1": ["cabac_mot_frm0_full", "HCMP1_HHI_A"],
    "h.265/JCT-VC-HEVC_V1": ["TILES_A_Cisco_2", "WPP_A_ericsson_MAIN10_2",
                            "PICSIZE_A_Bossen_1"],
    "vp9/VP9-TEST-VECTORS": ["vp90-2-02-size-lf-1920x1080.webm",
                            "vp90-2-21-resize_inter_1280x720_5_1-2.webm"],
}


def capture(command):
    return subprocess.check_output(command, text=True).strip()


def vector_cached(resources_root, suite_name, vector):
    suite_dir = resources_root / suite_name
    vector_dir = suite_dir / vector["name"]
    input_file = vector.get("input_file")
    if not vector_dir.exists():
        return False
    if not input_file:
        return any(vector_dir.iterdir()) if vector_dir.is_dir() else vector_dir.is_file()
    return (vector_dir / input_file).is_file() and (vector_dir / input_file).stat().st_size > 0


def has_cached_vectors(resources_dir, expected_vectors):
    return all(vector_cached(resources_dir, *item) for item in expected_vectors)


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("fluster", nargs="?", type=Path,
                        default=root / "benchmark-results/logs/fluster-checkout")
    parser.add_argument("--build-dir", type=Path, default=root / "build")
    parser.add_argument("--output-dir", type=Path, default=root / "benchmark-results/logs/fluster")
    parser.add_argument("--suite", choices=["smoke", "extended", "full"], default="smoke")
    parser.add_argument("--paths", nargs="+", choices=SUFFIXES, default=list(SUFFIXES))
    parser.add_argument("--timeout", type=int, default=30)
    parser.add_argument("--no-download", action="store_true", help="use cached vectors only")
    parser.add_argument("--force-download", action="store_true", help="always re-download test vectors")
    args = parser.parse_args()
    if args.no_download and args.force_download:
        parser.error("--no-download and --force-download are mutually exclusive")
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    fluster = args.fluster.resolve()
    driver = args.build_dir.resolve() / "vpu_drv_video.so"
    if "vaapi" in args.paths and not driver.is_file():
        parser.error(f"build driver first: {driver}")
    if not fluster.exists():
        fluster.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(["git", "clone", "https://github.com/fluendo/fluster.git", str(fluster)], check=True)
        subprocess.run(["git", "-C", str(fluster), "checkout", "--detach", REVISION], check=True)
    revision = capture(["git", "-C", str(fluster), "rev-parse", "HEAD"])
    if revision != REVISION or capture(["git", "-C", str(fluster), "status", "--porcelain", "--untracked-files=no"]):
        parser.error(f"Fluster must have clean tracked files at {REVISION}")

    # Each run has its own artifacts; downloads are shared across runs.
    base = args.output_dir.resolve()
    resources = base / "resources"
    output = base / datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    suites = output / "suites"
    suites.mkdir(parents=True)
    expected = {}
    selection = EXTENDED_VECTORS if args.suite == "extended" else VECTORS
    expected_vectors = []
    for source, names in selection.items():
        suite = json.loads((fluster / "test_suites" / (source + ".json")).read_text())
        if args.suite != "full":
            vectors = {v["name"]: v for v in suite["test_vectors"]}
            suite["test_vectors"] = [vectors[name] for name in names]
            suite["description"] = args.suite.upper() + " SUBSET: " + suite["description"]
        expected_vectors.extend((suite["name"], vector) for vector in suite["test_vectors"])
        for path in args.paths:
            decoder = "FFmpeg-" + suite["codec"] + SUFFIXES[path]
            expected[(suite["name"], decoder)] = {v["name"] for v in suite["test_vectors"]}
        (suites / (suite["name"] + ".json")).write_text(json.dumps(suite, indent=2) + "\n")

    env = dict(os.environ, LIBVA_DRIVER_NAME="vpu", LIBVA_DRIVERS_PATH=str(driver.parent))
    command = [sys.executable, str(fluster / "fluster.py"), "-tsd", str(suites),
               "-r", str(base / "resources"), "-o", str(output / "decoded")]
    decoders = sorted({decoder for _, decoder in expected})
    run = command + ["run", "-j", "1", "-t", str(args.timeout), "-d", *decoders,
                     "-s", "-f", "json", "-so", str(output / "results.json"), "-v"]
    metadata = {
        "started_utc": datetime.now(timezone.utc).isoformat(), "suite": args.suite,
        "fluster_revision": revision, "project_revision": capture(["git", "-C", str(root), "rev-parse", "HEAD"]),
        "project_status": capture(["git", "-C", str(root), "status", "--porcelain"]),
        "kernel": platform.release(), "ffmpeg": capture(["ffmpeg", "-version"]),
        "ffmpeg_executable": shutil.which("ffmpeg"),
        "driver_sha256": hashlib.sha256(driver.read_bytes()).hexdigest() if driver.exists() else None,
        "environment": {k: v for k, v in env.items() if k.startswith(("VPU_", "LIBVA_", "VK_"))
                        or k in ("PATH", "LD_LIBRARY_PATH")},
        "command": run,
    }
    (output / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    print(f"Artifacts: {output}", flush=True)
    if args.force_download or (not args.no_download and not has_cached_vectors(resources, expected_vectors)):
        with (output / "download.log").open("w") as log:
            subprocess.run(command + ["download", "-j", "2", "-r", "1"], check=True, stdout=log, stderr=subprocess.STDOUT)
    else:
        print("Using cached test vectors", flush=True)
    with (output / "run.log").open("w") as log:
        result = subprocess.run(run, env=env, stdout=log, stderr=subprocess.STDOUT)
    summary = json.loads((output / "results.json").read_text()) if (output / "results.json").exists() else {}
    passed = True
    for (suite, decoder), names in expected.items():
        vectors = summary.get("test_suites", {}).get(suite, {}).get("decoders", {}).get(decoder, {}).get("vectors", {})
        successes = sum(vectors.get(n, {}).get("result") == "Success" for n in names)
        print(f"{suite} / {decoder}: {successes}/{len(names)} passed")
        passed &= set(vectors) == names and successes == len(names)
    # Missing/skipped backends and vectors must not silently pass the baseline.
    code = result.returncode or (0 if passed else 1)
    metadata.update(exit_code=code, completed_utc=datetime.now(timezone.utc).isoformat())
    (output / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    return code


if __name__ == "__main__":
    sys.exit(main())
