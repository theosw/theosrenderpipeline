"""Identify the measured final 32-channel upsample/add pair; never replace work.

This is a trace filter, not a complete resource-liveness or launch-admission gate.
In particular, byte offsets into later kernel arguments are only a useful check
for additional direct references, not proof against indirect/descriptor aliases.
"""
import argparse
import csv
import json
import struct
from collections import defaultdict
from pathlib import Path


def final_pair(up, add):
    if up["name"] != "k_upscale" or add["name"] != "k_element_wise":
        return None
    u, e = bytes.fromhex(up["params_hex"]), bytes.fromhex(add["params_hex"])
    if len(u) != 40 or len(e) != 52:
        return None
    source, intermediate, ih, iw, oh, ow, padded_h, padded_w = struct.unpack("<QQ6I", u)
    first, residual, destination, width, height, channels, aw, ah, bw, bh = struct.unpack("<QQQ7I", e)
    if first != intermediate or channels != 32 or (width, height) != (padded_w, padded_h):
        return None
    if (aw, ah, bw, bh) != (width, height, width, height):
        return None  # No broadcast/wrap assumptions in the first experiment.
    if destination in (source, intermediate, residual):
        return None  # Cross-thread in-place hazards require a separate proof.
    return dict(input_extent=[iw, ih], output_extent=[ow, oh], padded_extent=[width, height],
                channels=channels, intermediate=hex(intermediate), destination=hex(destination),
                source=hex(source), residual=hex(residual))


def inspect(path):
    groups = defaultdict(list)
    for row in csv.DictReader(path.open(), delimiter="\t"):
        groups[(row["group"], row["generated"])].append(row)
    found = []
    for (group, generated), rows in groups.items():
        for i in range(len(rows) - 1):
            u, a = rows[i:i+2]
            if u["index"] != "0" or a["index"] != "0" or int(a["call"]) != int(u["call"]) + 1:
                continue
            result = final_pair(u, a)
            if result:
                result.update(group=int(group), generated=int(generated), upscale_call=int(u["call"]),
                    upscale_gpu_ms=float(u["call_gpu_ms"] or 0), add_gpu_ms=float(a["call_gpu_ms"] or 0))
                address = int(result["intermediate"], 16)
                later = []
                for row in rows[i+2:]:
                    args = bytes.fromhex(row["params_hex"])
                    if any(struct.unpack_from("<Q", args, j)[0] == address for j in range(0, len(args)-7, 8)):
                        later.append({"call": int(row["call"]), "name": row["name"]})
                result["later_direct_argument_matches"] = later
                found.append(result)
    return dict(proposal="final 32-channel k_upscale + k_element_wise", replacements_executed=False,
                resource_liveness_fully_proven=False, candidates=found)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("trace", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    text = json.dumps(inspect(args.trace), indent=2)
    if args.output:
        args.output.write_text(text + "\n")
    print(text)
