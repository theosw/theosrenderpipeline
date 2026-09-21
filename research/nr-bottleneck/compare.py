"""Synthetic standalone comparison. Requires NumPy; never launches a game.

Raw runtime, input/output images and launch arguments stay under ignored out/.
Timings measure NR Evaluate GPU work, not Skyrim FPS or display cadence.
"""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import subprocess
import time
from collections import defaultdict
import numpy as np

PIN = "8270b350cd82de5ce89806872cdd6b6a9249b80836b91bbeb3573470744cc206"


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def idle():
    tasks = subprocess.check_output(["tasklist", "/FI", "IMAGENAME eq SkyrimSE.exe", "/FO", "CSV"], text=True)
    if '"SkyrimSE.exe"' in tasks:
        raise RuntimeError("Skyrim is running; defer the offline measurement")


def generate(directory, width, height, frames, motion, reset_every):
    directory.mkdir()
    y, x = np.mgrid[:height, :width].astype(np.float32)
    # Broad gradients, abrupt edges and fine repeated detail; not a natural game capture.
    rgba = np.ones((height, width, 4), dtype=np.float32)
    rgba[..., 0] = 0.1 + 0.7 * x / width
    rgba[..., 1] = 0.1 + 0.7 * y / height
    rgba[..., 2] = 0.2 + 0.5 * (((x // 13 + y // 17) % 2) != 0)
    rgba[(x - width * .55) ** 2 + (y - height * .5) ** 2 < (height * .2) ** 2, :3] *= .15
    entries = []
    for frame in range(frames):
        image = np.roll(rgba, -frame * motion, axis=1).copy()
        # Deliberate scene/lighting jump, aligned with a reset.
        if reset_every and (frame // reset_every) % 2:
            image[..., :3] = image[..., :3][..., ::-1] * .6
        mv = np.zeros((height, width, 2), dtype="<f2")
        mv[..., 0] = motion
        colour = directory / f"{frame:03}.rgba16"
        vectors = directory / f"{frame:03}.motion16"
        image.astype("<f2").tofile(colour)
        mv.tofile(vectors)
        entries.append({"frame": frame, "colour_sha256": digest(colour), "motion_sha256": digest(vectors)})
    manifest = dict(width=width, height=height, frames=frames, motion=motion,
                    reset_every=reset_every, style=0, intensity=1, model_slot=0,
                    depth=.5, jitter=0, entries=entries)
    (directory / "manifest.json").write_text(json.dumps(manifest, indent=2))
    return manifest


def run(exe, dll, inputs, output, fixture, mode):
    idle()
    command = [str(exe), str(dll), str(inputs), str(output), str(fixture["width"]),
               str(fixture["height"]), str(fixture["frames"]), str(fixture["reset_every"]), "0", "1", mode]
    record = dict(command=command, executable_sha256=digest(exe), runtime_sha256=digest(dll),
                  input_manifest_sha256=digest(inputs / "manifest.json"), mode=mode)
    start = time.monotonic()
    with output.with_suffix(".log").open("w") as log:
        try:
            child = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=180)
            record["exit_code"] = child.returncode
        except subprocess.TimeoutExpired:
            record["timeout"] = True
    record["seconds"] = time.monotonic() - start
    output.with_suffix(".run.json").write_text(json.dumps(record, indent=2))
    if record.get("timeout") or record.get("exit_code") != 0:
        raise RuntimeError(f"Probe failed: {output}; retain its log and status")
    idle()


def compare(reference, candidate, fixture):
    errors = []
    timings = list(csv.DictReader((candidate / "timings.csv").open()))
    if len(timings) != fixture["frames"]:
        raise ValueError("Incomplete timing output")
    launches = defaultdict(list)
    trace = candidate / "launches.tsv"
    if trace.exists():
        with trace.open() as stream:
            for launch in csv.DictReader(stream, delimiter="\t"):
                launches[int(launch["frame"])].append(launch)
        if set(launches) != set(range(fixture["frames"])):
            raise ValueError("Incomplete launch trace")
    values, full, reused = [], [], []
    for index, row in enumerate(timings):
        if int(row["frame"]) != index:
            raise ValueError("Discontinuous frame sequence")
        reset = index == 0 or (fixture["reset_every"] != 0 and index % fixture["reset_every"] == 0)
        if bool(int(row["reset"])) != reset or (reset and int(row["reused"])):
            raise ValueError("Reset/reuse state disagrees with fixture")
        if launches:
            frame = launches[index]
            if [int(x["index"]) for x in frame] != list(range(len(frame))):
                raise ValueError("Discontinuous launch sequence")
            dropped = sum(int(x["dropped"]) for x in frame)
            if dropped != (42 if int(row["reused"]) else 0):
                raise ValueError("Recorded drop count disagrees with reuse state")
        name = f"{index:03}.rgba16"
        a, b = [np.fromfile(folder / name, dtype="<f2").astype(np.float32)
                .reshape(fixture["height"], fixture["width"], 4) for folder in (reference, candidate)]
        if not np.isfinite(b).all() or not np.array_equal(a[..., 3], b[..., 3]):
            raise ValueError("Non-finite output or changed alpha")
        delta = np.abs(a[..., :3] - b[..., :3])
        errors.append(dict(frame=index, exact=bool(np.array_equal(a, b)),
                           mean_rgb=float(delta.mean()), max_rgb=float(delta.max()),
                           reused=bool(int(row["reused"])), reset=bool(int(row["reset"]))))
        if index >= 2:  # Exclude one full/reuse warmup pair; avoid over-weighting cheap frames.
            value = float(row["evaluate_gpu_ms"])
            values.append(value)
            (reused if int(row["reused"]) else full).append(value)
    return dict(frames=errors, mean_rgb=float(np.mean([x["mean_rgb"] for x in errors])),
                max_rgb=max(x["max_rgb"] for x in errors),
                reset_frames_exact=all(x["exact"] for x in errors if x["reset"]),
                gpu_mean_ms=float(np.mean(values)), gpu_p95_ms=float(np.percentile(values, 95)),
                gpu_stddev_ms=float(np.std(values)),
                timing_full_frames=len(full), timing_reused_frames=len(reused),
                full_mean_ms=float(np.mean(full)) if full else None,
                reused_mean_ms=float(np.mean(reused)) if reused else None)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--exe", type=Path, required=True)
    p.add_argument("--dll", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--width", type=int, default=320)
    p.add_argument("--height", type=int, default=320)
    p.add_argument("--frames", type=int, default=16)
    p.add_argument("--motion", type=int, default=2)
    p.add_argument("--reset-every", type=int, default=8)
    a = p.parse_args()
    if digest(a.dll) != PIN:
        raise ValueError("Runtime identity mismatch")
    if not (64 <= a.width <= 2048 and 64 <= a.height <= 2048 and 4 <= a.frames <= 256 and a.reset_every >= 0):
        raise ValueError("Invalid fixture extent/count/reset interval")
    idle()
    a.output.mkdir(parents=True, exist_ok=False)
    inputs = a.output / "inputs"
    fixture = generate(inputs, a.width, a.height, a.frames, a.motion, a.reset_every)
    summary = dict(fixture=fixture, comparisons={})
    for mode in ("baseline", "observe", "reuse", "baseline-repeat"):
        print(f"Running {mode} at {a.width}x{a.height}", flush=True)
        target = a.output / mode
        run(a.exe.resolve(), a.dll.resolve(), inputs.resolve(), target.resolve(), fixture,
            "baseline" if mode == "baseline-repeat" else mode)
        result = compare(a.output / "baseline", target, fixture)
        summary["comparisons"][mode] = result
        (a.output / "comparison.json").write_text(json.dumps(summary, indent=2))
        if mode in ("observe", "baseline-repeat") and not all(x["exact"] for x in result["frames"]):
            raise RuntimeError("Observation/repeated reference changed pixels")
        if not result["reset_frames_exact"]:
            raise RuntimeError("Reset did not restore exact reference output")
    print(json.dumps({k: {n: v for n, v in r.items() if n != "frames"}
                      for k, r in summary["comparisons"].items()}, indent=2))


if __name__ == "__main__":
    main()
