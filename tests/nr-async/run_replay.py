"""Manual paced replay; timings and captures are separate invocations.

No game, deployment, profile change or process termination beyond our own child.
Known shutdown timeouts remain failed runs even when explicitly continued.
"""
import argparse
import csv
import hashlib
import json
import pathlib
import re
import shutil
import subprocess
import time


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def distribution(values):
    values = sorted(values)
    if not values:
        return None
    return {"mean": sum(values) / len(values), **{
        f"p{p}": values[int(p / 100 * (len(values) - 1))] for p in (50, 95, 99)}, "max": values[-1]}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("executable", type=pathlib.Path)
    parser.add_argument("runtime", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path)
    parser.add_argument("--width", type=int, default=1280)
    parser.add_argument("--height", type=int, default=720)
    parser.add_argument("--rates", type=int, nargs="+", default=[30, 60, 90])
    parser.add_argument("--seconds", type=int, default=6)
    parser.add_argument("--capture-hz", type=int, default=0)
    parser.add_argument("--capture-events", action="store_true",
                        help="Capture every source frame from two before to seven after cuts/exposure, plus the base capture rate.")
    parser.add_argument("--modes", nargs="+", choices=["off", "regular", "reset", "async", "init-only", "create-only"],
                        default=["off", "regular", "reset", "async"])
    parser.add_argument("--timeout", type=float, default=90)
    parser.add_argument("--shutdown-timeout", type=float, default=5)
    parser.add_argument("--allow-shutdown-timeout", action="store_true")
    args = parser.parse_args()
    args.executable = args.executable.resolve(strict=True)
    args.runtime = args.runtime.resolve(strict=True)
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    binary_dir = args.output / "binaries"
    binary_dir.mkdir()
    binary = binary_dir / args.executable.name
    shutil.copy2(args.executable, binary)
    if args.executable.with_suffix(".pdb").exists():
        shutil.copy2(args.executable.with_suffix(".pdb"), binary.with_suffix(".pdb"))
    metadata = {"executable": str(binary), "executable_sha256": sha256(binary),
                "runtime": str(args.runtime), "runtime_sha256": sha256(args.runtime),
                "width": args.width, "height": args.height, "seconds": args.seconds,
                "requested_rates": args.rates, "capture_hz": args.capture_hz,
                "capture_events": args.capture_events,
                "timing_only": args.capture_hz == 0 and not args.capture_events, "runs": []}
    failures = 0
    for fps in args.rates:
        for index, mode in enumerate(args.modes):
            run_dir = args.output / f"{fps}-{index}-{mode}"
            run_dir.mkdir()
            command = [str(binary), str(args.runtime), mode, str(args.width), str(args.height), str(fps),
                       str(args.seconds), str(run_dir), str(args.capture_hz) + ("+events" if args.capture_events else "")]
            begin = time.monotonic()
            shutdown_begin = None
            timeout_stage = None
            with (run_dir / "stdout.log").open("w") as stdout, (run_dir / "stderr.log").open("w") as stderr:
                child = subprocess.Popen(command, stdout=stdout, stderr=stderr)
                while child.poll() is None:
                    log = (run_dir / "stdout.log").read_text(errors="replace")
                    now = time.monotonic()
                    if "CLEANUP NGX shutdown begin" in log and shutdown_begin is None:
                        shutdown_begin = now
                    if shutdown_begin is not None and now - shutdown_begin > args.shutdown_timeout:
                        timeout_stage = "ngx-shutdown"
                    elif now - begin > args.timeout:
                        timeout_stage = "overall"
                    if timeout_stage:
                        child.kill()
                        break
                    time.sleep(.05)
                exit_code = child.wait()
            log = (run_dir / "stdout.log").read_text()
            result = {"directory": run_dir.name, "mode": mode, "requested_fps": fps,
                      "exit_code": exit_code, "timeout_stage": timeout_stage,
                      "elapsed_s": time.monotonic() - begin,
                      "measurement_complete": "RESULT " in log,
                      "feature_released": "CLEANUP feature released" in log,
                      "ngx_shutdown_complete": "CLEANUP NGX shutdown complete" in log}
            marker = re.search(r"^RESULT (.+)$", log, re.M)
            if marker:
                result["native_result"] = dict(re.findall(r"(\w+)=(\S+)", marker.group(1)))
            csv_path = run_dir / "samples.csv"
            if csv_path.exists():
                with csv_path.open(newline="") as source:
                    rows = list(csv.DictReader(source))
                complete = [row for row in rows if all(value is not None for value in row.values())]
                result["incomplete_csv_rows"] = len(rows) - len(complete)
                result["timing_samples"] = len(complete)
                for field in ["cpu_frame_ms", "cpu_record_ms", "cpu_submit_wait_ms", "host_gpu_ms", "start_lateness_ms", "producer_interval_ms"]:
                    if complete and field not in complete[0]:
                        continue
                    filtered = [r for r in complete if field != "producer_interval_ms" or int(r["frame"]) > 0]
                    result[field] = distribution([float(row[field]) for row in filtered])
                if complete:
                    result["actual_producer_fps"] = 1000 / result["producer_interval_ms"]["mean"]
                    result["deadline_misses"] = sum(int(row["deadline_miss"]) for row in complete)
                    displayed = [row for row in complete if int(row["displaying"])]
                    result["displayed_frames"] = len(displayed)
                    for field in ["result_age", "result_age_ms", "result_wall_age_ms", "evaluation_roundtrip_ms"]:
                        if field not in complete[0]:
                            continue
                        result[field] = distribution([float(row[field]) for row in displayed])
                    result["per_scene_cpu"] = {str(scene): distribution([float(row["cpu_frame_ms"]) for row in complete if int(row["scene"]) == scene])
                                               for scene in range(4)}
                    result["evaluations_at_last_frame"] = int(complete[-1]["evaluations"])
                    result["dropped_at_last_frame"] = int(complete[-1]["dropped"])
            metadata["runs"].append(result)
            (args.output / "identity-and-results.json").write_text(json.dumps(metadata, indent=2))
            print(json.dumps(result), flush=True)
            failures += exit_code != 0
            tolerated = (args.allow_shutdown_timeout and timeout_stage == "ngx-shutdown"
                         and result["measurement_complete"] and result["feature_released"])
            if exit_code != 0 and not tolerated:
                raise SystemExit("Child failed before the allowed cleanup boundary; inspect retained logs.")
    if failures:
        raise SystemExit(f"Measurements retained; {failures} children failed cleanup. This is not lifecycle acceptance.")


if __name__ == "__main__":
    main()
