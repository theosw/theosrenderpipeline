"""Offline cross-check of completed dispatch traces against the static AMD inventory.

Does not use timing, PSO creation, or version names alone as execution proof.
Watermark/image inspection is a separate human/visual observation in the report.
"""
import argparse
import collections
import csv
import hashlib
import json
import math
from pathlib import Path
import re


def rows(path):
    with path.open(encoding="utf-8-sig") as file:
        return list(csv.DictReader(file))


def analyze(case, inventory):
    log = (case / "run.log").read_text(encoding="utf-8-sig")
    receipt = json.loads((case / "receipt.json").read_text(encoding="utf-8-sig"))
    before = json.loads((case / "runtime-before.json").read_text(encoding="utf-8-sig"))
    after = json.loads((case / "runtime-after.json").read_text(encoding="utf-8-sig"))
    result = {"case": case.name, "exit": receipt["exit_code"], "runtime_unchanged": before == after}
    if receipt["exit_code"] != 0:
        result["refusal"] = re.findall(r"^REFUSED (.+)$", log, re.M)
        result["no_query"] = "OFFERED " not in log
        return result
    timings = rows(case / "timings.csv")
    submissions = rows(case / "submissions.csv")
    dispatches = rows(case / "dispatches.csv")
    keyed = collections.defaultdict(list)
    for d in dispatches:
        keyed[(d["context"], d["frame"], d["fence"])].append(d)
    complete = len(submissions) == len(timings) == len(keyed)
    complete &= [(s["context"], s["frame"], s["fence"]) for s in submissions] == [
        (t["context"], t["frame"], t["fence"]) for t in timings]
    complete &= all(int(b["fence"]) > int(a["fence"]) for a, b in zip(submissions, submissions[1:]))
    neural_sequences = []
    dot_per_frame = []
    prefix = "fsr4_model_v07_fp8_no_scale_"  # Names are misleading; dot instructions and model identity are also required.
    expected = [prefix + "prepass", prefix + "pass0_post"]
    for number in range(1, 13):
        expected.extend([prefix + f"pass{number}", prefix + f"pass{number}_post"])
    expected.append(prefix + "postpass")
    for s in submissions:
        ds = keyed[(s["context"], s["frame"], s["fence"])]
        complete &= s["observation_complete"] == "1" and int(s["dispatches"]) == len(ds)
        complete &= [int(d["ordinal"]) for d in ds] == list(range(len(ds)))
        complete &= all(int(d[axis]) > 0 for d in ds for axis in ["x", "y", "z"])
        complete &= all(d["sha256"] in inventory for d in ds)
        entries = [inventory[d["sha256"]] for d in ds if d["sha256"] in inventory]
        neural_sequences.append([e["entry"][0] for e in entries if e["entry"] and e["entry"][0].startswith(prefix)] == expected)
        dot_per_frame.append(sum(e["dot4add_i8"] > 0 for e in entries))
    counts = collections.Counter(d["sha256"] for d in dispatches)
    result.update(frames=len(timings), dispatches=len(dispatches), distinct_shaders=len(counts),
                  complete_observation=bool(complete), full_neural_sequence_every_frame=all(neural_sequences),
                  int8_dot_dispatches_per_frame=sorted(set(dot_per_frame)),
                  shaders=[dict(sha256=h, dispatch_count=n, **{k: inventory.get(h, {}).get(k) for k in
                           ["entry", "shader_model", "dot4add_i8", "requirements"]}) for h, n in counts.items()])
    result["offered"] = re.findall(r"OFFERED id=(\w+) name=(.+)", log)
    result["provider"] = re.findall(r"PROVIDER generation=(\d+) id=(\w+) name=(\S+)", log)
    result["model_observations"] = re.findall(r"^IDENTITY .+$", log, re.M)
    result["readbacks"] = re.findall(r"^READBACK .+$", log, re.M)
    result["finite_outputs"] = bool(result["readbacks"]) and all(" nonfinite=0 " in r for r in result["readbacks"])
    measured = [float(t["gpu_ms"]) for t in timings if t["measured"] == "1"]
    assert measured and all(math.isfinite(x) and x > 0 for x in measured)
    measured.sort()
    result["gpu_ms"] = {"samples": len(measured), "p50": measured[int((len(measured)-1)*.5)],
                        "p95": measured[int((len(measured)-1)*.95)], "min": measured[0], "max": measured[-1],
                        "mean": sum(measured)/len(measured)}
    result["timing_eligible"] = "run=timing" in log and "watermark=0 debug=0" in log
    result["execution_evidence"] = "unverified"
    if (complete and set(dot_per_frame) == {0} and result["provider"] and
            all(p[1:] == ("f5a5ca1e00c01006", "3.1.6") for p in result["provider"])):
        result["execution_evidence"] = "FSR 3.1.6 control; no INT8 neural dispatches"
    # Deliberately does not invent the visual watermark result.
    if (complete and all(neural_sequences) and set(dot_per_frame) == {13} and result["finite_outputs"] and
            "stage=created found=1" in log and "lazy_built=0" in log and "stage=first_dispatch found=1" in log and
            "lazy_built=1" in log and "SELECTED override_id=f5a5ca1e01001002" in log):
        result["execution_evidence"] = "INT8 model and completed neural sequence; verify watermark separately"
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("evidence", type=Path)
    parser.add_argument("inventory", type=Path)
    args = parser.parse_args()
    source = args.inventory.read_bytes()
    inventory = {s["sha256"]: s for s in json.loads(source)["shaders"]}
    cases = [analyze(p.parent, inventory) for p in sorted(args.evidence.glob("*/receipt.json"))]
    output = {"inventory_sha256": hashlib.sha256(source).hexdigest(), "cases": cases}
    (args.evidence / "analysis.json").write_text(json.dumps(output, indent=2), encoding="utf-8")
    for case in cases:
        print(case["case"], case.get("execution_evidence", case.get("refusal")),
              case.get("gpu_ms") if case.get("timing_eligible") else "diagnostic/refusal")
    if any(not c["runtime_unchanged"] or (c["exit"] == 0 and not c["complete_observation"]) for c in cases):
        raise SystemExit("Incomplete observation or changed runtime: see analysis.json")


if __name__ == "__main__":
    main()
