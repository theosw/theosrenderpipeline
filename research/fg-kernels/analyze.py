"""Validate oracle captures and summarize GPU costs. Uses Python's standard library.

Byte equality is necessary, but additionally check that moving outputs really
advance through intermediate positions. This prevents an all-black/copy-only
oracle from passing simply because both sides are equally wrong.
"""
import argparse
import csv
import json
import math
import re
import statistics
import struct
from collections import defaultdict
from pathlib import Path


def metadata(path):
    if not (path / "complete.txt").is_file():
        raise ValueError(f"Incomplete run: {path}")
    text = (path / "identity.txt").read_text()
    match = re.search(r"dimensions=(\d+)x(\d+) frames=(\d+) multiplier=(\d+)", text)
    if not match:
        raise ValueError("Missing dimensions")
    w, h, frames, multiplier = map(int, match.groups())
    fixture = re.search(r"fixture=(\w+)", text).group(1)
    return w, h, frames, multiplier, fixture


def equal_files(a, b):
    if a.stat().st_size != b.stat().st_size:
        return False
    with a.open("rb") as left, b.open("rb") as right:
        while chunk := left.read(1024 * 1024):
            if chunk != right.read(len(chunk)):
                return False
    return True


def red_bounds(path, width, height):
    with path.open("rb") as file:
        file.seek((height // 2) * width * 8)
        row = struct.unpack("<" + str(width * 4) + "e", file.read(width * 8))
    if not all(math.isfinite(v) for v in row):
        raise ValueError(f"Nonfinite center row: {path}")
    red = [x for x in range(width) if row[x * 4] > .75]
    if not red:
        raise ValueError(f"No moving bar: {path}")
    return min(red), max(red)


def verify(reference, candidate):
    shape = metadata(reference)
    if metadata(candidate) != shape:
        raise ValueError("Run configuration mismatch")
    w, h, frames, multiplier, fixture = shape
    expected = {f"{f:03}-{j}.rgba16" for f in range(frames) for j in range(1, multiplier)}
    expected |= {f"{f:03}-input.rgba16" for f in range(frames)}
    for path in (reference, candidate):
        if {p.name for p in path.glob("*.rgba16")} != expected:
            raise ValueError(f"Wrong capture count: {path}")
    for name in sorted(expected):
        a, b = reference / name, candidate / name
        if a.stat().st_size != w * h * 8 or not equal_files(a, b):
            raise ValueError(f"Image mismatch: {name}")
    positions = []
    for frame in range(frames):
        left = w // 4 + (0 if fixture == "static" else frame * 4)
        reset = frame == 0 or (fixture == "recreate" and frame % 8 == 0)
        row = []
        for generated in range(1, multiplier):
            lo, hi = red_bounds(candidate / f"{frame:03}-{generated}.rgba16", w, h)
            position = left if reset or fixture == "static" else left - 4 + 4 * generated / multiplier
            if abs(lo - position) > 2 or abs(hi - lo + 1 - w // 8) > 3:
                raise ValueError(f"Unexpected bar geometry at group {frame}, image {generated}: {lo}, {hi}")
            row.append([lo, hi])
        if multiplier == 4 and fixture != "static" and not reset:
            # Thresholded edges can round to the same pixel at large extents.
            # Reject duplicate generated images, not legitimate subpixel motion.
            if all(equal_files(candidate / f"{frame:03}-1.rgba16", candidate / f"{frame:03}-{j}.rgba16") for j in (2, 3)):
                raise ValueError(f"Generated x4 images are duplicates: group {frame}")
        positions.append(row)
    return {"byte_identical_images": frames * (multiplier - 1), "input_images": frames,
            "center_row_finite_and_geometry_checked": True, "bar_positions": positions}


def summarize(path, warmup=4):
    _, _, _, _, fixture = metadata(path)
    groups = defaultdict(float)
    keep = lambda g: g >= warmup and (fixture != "recreate" or g % 8 >= warmup)
    for row in csv.DictReader((path / "timings.csv").open()):
        group = int(row["group"])
        if keep(group):
            groups[group] += float(row["evaluate_gpu_ms"])
    if not groups:
        raise ValueError("No steady groups after warmup")
    # Sum each evaluation exactly once. x4's three generated images are already
    # in the CSV: never multiply this group result by three a second time.
    summary = {"steady_groups": len(groups), "fg_group_median_ms": statistics.median(groups.values()),
               "fg_group_min_ms": min(groups.values()), "fg_group_max_ms": max(groups.values())}
    trace = path / "launches.tsv"
    if trace.exists():
        calls = defaultdict(list)
        for row in csv.DictReader(trace.open(), delimiter="\t"):
            if keep(int(row["group"])) and row["call_gpu_ms"]:
                calls[(row["group"], row["generated"], row["call"])].append(row)
        costs = defaultdict(float)
        for rows in calls.values():
            # Preserve batches: a batch duration belongs to the whole call,
            # never to every individual kernel in that call.
            label = " + ".join(r["name"] for r in rows)
            costs[label] += float(rows[0]["call_gpu_ms"]) / len(groups)
        summary["profiled_call_mean_ms_per_group"] = dict(sorted(costs.items(), key=lambda item: -item[1]))
    return summary


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("reference", type=Path)
    parser.add_argument("candidates", type=Path, nargs="+")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    result = {"reference": str(args.reference), "reference_timing": summarize(args.reference), "candidates": {}}
    for candidate in args.candidates:
        result["candidates"][str(candidate)] = {"verification": verify(args.reference, candidate), "timing": summarize(candidate)}
    encoded = json.dumps(result, indent=2)
    if args.output:
        args.output.write_text(encoded + "\n")
    print(encoded)
