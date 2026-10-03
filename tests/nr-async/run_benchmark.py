"""Bounded manual runner; does not launch a game or change installed files."""
import argparse
import csv
import hashlib
import json
import pathlib
import subprocess
import time


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("executable", type=pathlib.Path)
    parser.add_argument("runtime", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path)
    parser.add_argument("--width", type=int, default=512)
    parser.add_argument("--height", type=int, default=288)
    parser.add_argument("--frames", type=int, default=180)
    parser.add_argument("--pause-us", type=int, default=1000, help="Requested pause between frames, outside measured scopes; 0 for a saturated producer.")
    parser.add_argument("--modes", nargs="+", choices=["off", "sync", "async"], default=["off", "sync", "async"])
    parser.add_argument("--timeout", type=float, default=60)
    parser.add_argument("--allow-shutdown-timeout", action="store_true",
                        help="Continue only when a timed-out child logged RESULT and CLEANUP feature released; retain failure status.")
    args = parser.parse_args()
    args.executable = args.executable.resolve(strict=True)
    args.runtime = args.runtime.resolve(strict=True)
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    metadata = {
        "executable": str(args.executable), "executable_sha256": sha256(args.executable),
        "runtime": str(args.runtime), "runtime_sha256": sha256(args.runtime),
        "width": args.width, "height": args.height, "frames": args.frames, "pause_us": args.pause_us, "runs": [],
    }
    for index, mode in enumerate(args.modes):
        run_dir = args.output / f"{index}-{mode}"
        run_dir.mkdir()
        command = [str(args.executable), str(args.runtime), mode, str(args.width), str(args.height), str(args.frames), str(run_dir), str(args.pause_us)]
        begin = time.monotonic()
        with (run_dir / "stdout.log").open("w") as stdout, (run_dir / "stderr.log").open("w") as stderr:
            child = subprocess.Popen(command, stdout=stdout, stderr=stderr)
            timed_out = False
            try:
                exit_code = child.wait(timeout=args.timeout)
            except subprocess.TimeoutExpired:
                timed_out = True
                child.kill()  # This runner's own standalone child only.
                exit_code = child.wait()
        result = {"mode": mode, "exit_code": exit_code, "timeout": timed_out, "elapsed_s": time.monotonic() - begin}
        log = (run_dir / "stdout.log").read_text()
        result["feature_released"] = "CLEANUP feature released" in log
        result["ngx_shutdown_complete"] = "CLEANUP NGX shutdown complete" in log
        csv_path = run_dir / "samples.csv"
        if csv_path.exists():
            with csv_path.open(newline="") as source:
                rows = list(csv.DictReader(source))
            complete = [row for row in rows if all(value is not None for value in row.values())]
            result["incomplete_csv_rows"] = len(rows) - len(complete)
            samples = [row for row in complete if int(row["frame"]) >= 16]
            result["timing_samples"] = len(samples)
            if samples:
                for field in ["cpu_frame_ms", "host_gpu_ms", "result_age"]:
                    values = sorted(float(row[field]) for row in samples)
                    result[field] = {"mean": sum(values) / len(values), "p50": values[int(.5*(len(values)-1))],
                                     "p95": values[int(.95*(len(values)-1))], "p99": values[int(.99*(len(values)-1))]}
                result["displayed_frames"] = sum(int(row["displaying"]) for row in samples)
        metadata["runs"].append(result)
        (args.output / "identity-and-results.json").write_text(json.dumps(metadata, indent=2))
        print(json.dumps(result), flush=True)
        tolerated = args.allow_shutdown_timeout and timed_out and "RESULT mode=" in log and result["feature_released"]
        if exit_code != 0 and not tolerated:
            raise SystemExit("Standalone child did not complete cleanly; inspect retained logs before continuing.")


if __name__ == "__main__":
    main()
