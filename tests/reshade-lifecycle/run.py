"""Run five isolated real-ReShade ownership cases; never installs an injector.

Input policy varies by version: use every-runtime for 6.3.3, first-runtime for
6.8.0, or observe for an unknown version. Observing is not policy acceptance.
The fixture needs an accepted D3D11 hardware adapter (ReShade rejects WARP).
"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
from windows_job import KillOnCloseJob


MODES = ("automatic", "explicit-native", "explicit-proxy",
         "mixed-auto-first", "mixed-explicit-first")


def require(condition, message):
    # Checks must survive python -O, just as the C++ checks survive NDEBUG.
    if not condition:
        raise RuntimeError(message)


def check_events(events, mode, input_policy):
    init = [e for e in events if e["event"] == "init_runtime"]
    require(len(init) == (2 if mode.startswith("mixed") else 1), "runtime count")
    for start in init:
        initialized = start["phase"] == "create_automatic"
        require(start["device_seen"] == initialized and start["queue_seen"] == initialized,
                "automatic/exported API initialization identities")
    if len(init) == 2:
        require(init[0]["device"] != init[1]["device"] and
                init[0]["queue"] != init[1]["queue"], "distinct API objects")
        require(init[0]["device_identity"] == init[1]["device_identity"] and
                init[0]["queue_identity"] == init[1]["queue_identity"], "shared native COM objects")
    manual = [e for e in events if e.get("phase") == "manual_effect_then_present"]
    order = [e["event"] for e in manual if e["event"] in
             ("begin_effects", "finish_effects", "overlay", "runtime_present")]
    require(order == ["begin_effects", "finish_effects", "overlay", "runtime_present"],
            "single effect -> GUI -> completion order")
    pixels = [e["red"] for e in manual if e["event"] == "pixel_check"]
    require(len(pixels) == 1 and 94 <= pixels[0] <= 98, "single shader addition readback")
    off = [e for e in events if e.get("phase") == "effects_off_overlay_on"]
    require(not any(e["event"] in ("begin_effects", "finish_effects") for e in off),
            "effects off suppresses shader callbacks")
    require(sum(e["event"] == "overlay" and not e["effects"] for e in off) == 1 and
            sum(e["event"] == "runtime_present" for e in off) == 1,
            "effects off retains GUI and completion")
    for event in events:
        if event["event"] == "runtime_present":
            start = next((i for i in init if i["runtime"] == event["runtime"]), None)
            require(start is not None, f"completion from uninitialized runtime {event['runtime']}")
            require(event["context_protected"] == int(start["phase"] == "create_automatic"),
                    "automatic scoped protection / exported unprotected update")
        elif event["event"] in ("host_tick_before", "host_tick_after"):
            require(event["context_protected"] == 0, "host context protection restored")
    for kind, field in (("runtime", "runtime"), ("device", "object"),
                        ("queue", "object"), ("list", "object")):
        created = Counter(e[field] for e in events if e["event"] == "init_" + kind)
        destroyed = Counter(e[field] for e in events if e["event"] == "destroy_" + kind)
        require(bool(created) and created == destroyed, kind + " paired destruction identities")
    policies = [e for e in events if e["event"] == "input_policy"]
    require(len(policies) == int(mode.startswith("mixed")), "mixed runtime input check executed")
    if policies:
        policy = policies[0]
        for which in ("secondary", "first"):
            before, after = policy[which + "_before"], policy[which + "_after"]
            require(len(before) == len(after) == 2 and
                    all(type(v) is bool for v in before + after), "measured input states")
            require(policy[which + "_advances"] == (all(before) and not any(after)),
                    "input summary agrees with measured states")
        if input_policy != "observe":
            require(all(policy["secondary_before"]) and all(policy["first_before"]),
                    "both runtimes received synthetic key")
            expected_after = [input_policy == "first-runtime"] * 2
            require(policy["secondary_after"] == expected_after and
                    policy["first_after"] == [False, False], "version-specific shared input policy")
    require(events[-1]["event"] == "result" and events[-1]["pass"] and
            events[-1]["mode"] == mode, "fixture completed")
    return {"mode": mode, "pass": True, "red": pixels[0], "input_policy": policies,
            "input_expectation": input_policy}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", required=True, type=Path)
    parser.add_argument("--runtime", required=True, type=Path, help="Supplied x64 ReShade DLL")
    parser.add_argument("--output", required=True, type=Path, help="Parent for fresh isolated run folders")
    parser.add_argument("--input-policy", choices=("observe", "every-runtime", "first-runtime"),
                        default="observe")
    parser.add_argument("--mode", action="append", choices=MODES)
    parser.add_argument("--keep-binaries", action="store_true",
                        help="Keep successful run copies of the executable and injector")
    args = parser.parse_args()
    executable, runtime = args.exe.resolve(strict=True), args.runtime.resolve(strict=True)
    args.output.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix="matrix-", dir=args.output.resolve()))
    fixture = Path(__file__).resolve().parent
    identity = {"exe_sha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
                "runtime_sha256": hashlib.sha256(runtime.read_bytes()).hexdigest()}
    (root / "identity.json").write_text(json.dumps(identity, indent=2), encoding="utf-8")
    print(f"Evidence: {root}", flush=True)
    results = []
    try:
        for mode in args.mode or MODES:
            directory = root / mode
            directory.mkdir()
            shutil.copy2(executable, directory / executable.name)
            shutil.copy2(runtime, directory / "dxgi.dll")
            for name in ("ReShade.ini", "Probe.ini"):
                shutil.copy2(fixture / name, directory / name)
            shutil.copy2(fixture / "ReShade.ini", directory / "Explicit.ini")
            shutil.copytree(fixture / "effects", directory / "effects")
            result = {"mode": mode, "pass": False, "error": "child did not complete"}
            results.append(result)
            with (directory / "events.jsonl").open("wb") as stdout, (directory / "stderr.txt").open("wb") as stderr:
                with KillOnCloseJob() as job:
                    child = subprocess.Popen([str(directory / executable.name), mode], cwd=directory,
                                             stdout=stdout, stderr=stderr,
                                             creationflags=subprocess.CREATE_NO_WINDOW)
                    try:
                        job.attach(child.pid)
                        result["pid"] = child.pid
                        exit_code = child.wait(timeout=45)
                    except BaseException as error:
                        result["error"] = str(error)
                        raise
                    finally:
                        if child.poll() is None:
                            child.kill()
                        child.wait(timeout=5)
            result["exit"] = exit_code
            require(exit_code == 0, f"{mode}: exit {exit_code}; see {directory / 'stderr.txt'}")
            events = [json.loads(line) for line in (directory / "events.jsonl").read_text().splitlines()]
            validated = check_events(events, mode, args.input_policy)
            # Delete only known disposable copies in this newly created folder.
            # Preserve logs/configuration/hashes; failed cases retain their binaries.
            if not args.keep_binaries:
                (directory / executable.name).unlink()
                (directory / "dxgi.dll").unlink()
            result.update(validated)
            result.pop("error", None)
            print(json.dumps(result), flush=True)
    except BaseException as error:
        if results and not results[-1]["pass"]:
            results[-1]["error"] = str(error)
        raise
    finally:
        (root / "results.json").write_text(json.dumps(results, indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()
