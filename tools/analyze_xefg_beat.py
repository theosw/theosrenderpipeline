"""Read sampled [XeFG beat] bursts. Run: python -I tools/analyze_xefg_beat.py GAME.log

Host intervals include the PREVIOUS frame's post-Present retirement. Match frame
IDs before attributing a long interval to a model phase. These are CPU boundaries,
never Intel batch-start times, output spacing, or physical display cadence.
"""
import argparse
import collections
import json
import math
import statistics


def summary(values):
    if not values:
        return {"samples": 0}
    ordered = sorted(values)
    return {"samples": len(values), "mean": statistics.fmean(values),
            **{label: ordered[math.floor((len(ordered) - 1) * fraction)]
               for label, fraction in (("p50", .5), ("p95", .95), ("p99", .99))}}


def analyze(lines):
    groups = collections.defaultdict(list)
    previous = None
    previous_key = None
    for line in lines:
        if "[XeFG beat] " not in line:
            continue
        fields = dict(word.split("=", 1) for word in line.split("[XeFG beat] ", 1)[1].split() if "=" in word)
        try:
            row = {key: float(fields[key]) for key in ("hostDtMs", "innerDtMs", "innerPresentMs", "reuseCpuMs", "holdMs", "targetMs")}
            row.update({key: int(fields[key]) for key in ("frame", "epoch", "outputs", "submittedSync")})
            row.update({key: fields[key] for key in ("model", "requested", "paced", "reuse", "vsync")})
        except (KeyError, ValueError):
            continue
        if not all(math.isfinite(row[key]) and row[key] >= 0 for key in ("hostDtMs", "innerDtMs", "innerPresentMs", "reuseCpuMs", "holdMs", "targetMs")):
            previous = None
            continue
        key = tuple(row[key] for key in ("epoch", "requested", "paced", "reuse", "outputs", "vsync", "submittedSync"))
        row["consecutive"] = bool(previous and previous_key == key and previous["frame"] + 1 == row["frame"])
        if row["consecutive"]:
            row["priorModel"] = previous["model"]
            row["priorReuseCpuMs"] = previous["reuseCpuMs"]
            if previous["consecutive"] and previous["model"] != row["model"] and previous["model"] != "unknown" and row["model"] != "unknown":
                row["adjacentIntervalDifferenceMs"] = abs(previous["hostDtMs"] - row["hostDtMs"])
        groups[key].append(row)
        previous = row
        previous_key = key
    result = []
    for key, rows in groups.items():
        record = dict(zip(("epoch", "requested", "paced", "reuse", "outputs", "vsync", "submittedSync"), key))
        # A burst's first row has no logged predecessor, even if its interval is
        # valid. Exclude it from phase attribution and adjacent differences.
        joined = [row for row in rows if row["consecutive"] and row["hostDtMs"] > 0]
        record["loggedFrames"] = len(rows)
        for field in ("hostDtMs", "innerDtMs", "innerPresentMs", "reuseCpuMs", "holdMs", "targetMs", "adjacentIntervalDifferenceMs"):
            record[field] = summary([row[field] for row in joined if field in row])
        if joined:
            record["sampledRealFPS"] = 1000 / statistics.fmean(row["hostDtMs"] for row in joined)
        record["phase"] = {model: {"hostIntervalAfterPriorModelMs": summary([row["hostDtMs"] for row in joined if row["priorModel"] == model]),
                                   "retirementOnModelFrameMs": summary([row["reuseCpuMs"] for row in rows if row["model"] == model])}
                           for model in ("ran", "skipped", "unknown")}
        result.append(record)
    return {"physicalCadenceVerified": False, "batchStart": "unavailable", "outputSpacing": "unavailable", "groups": result}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", help="TRP game log containing [XeFG beat] bursts")
    args = parser.parse_args()
    with open(args.log, encoding="utf-8-sig", errors="replace") as source:
        print(json.dumps(analyze(source), indent=2))
