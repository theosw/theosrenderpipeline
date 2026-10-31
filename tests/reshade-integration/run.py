"""Run TRPReShadeIntegrationTests beside a supplied ReShade, one fresh folder per case.

Never installs an injector. Each case gets its own preset and shader cache.
With --regrade-addon and --regrade-shaders (MartysMods_ReGradePlus.addon64 and
the folder holding MartysMods_REGRADE+.fx), the ReGrade+ cases run too. Needs
a D3D11 hardware adapter.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "reshade-lifecycle"))
from windows_job import KillOnCloseJob  # noqa: E402

# (name, fixture argument, ReShade disabled, ReGrade+ supplied)
CASES = (
    ("reshade", "--require-reshade", False, False),
    ("game-device", "--require-game-device", False, False),
    ("game-device-failure", "--require-game-device-failure", False, False),
    ("game-device-disabled", "--require-game-device-disabled", True, False),
)
REGRADE_CASES = (
    ("regrade-blocked", "--require-blocked-addon", False, True),
    ("regrade-bridged", "--require-bridged-addon", False, True),
    ("regrade-game-device", "--require-game-device", False, True),
)
ADDON = "MartysMods_ReGradePlus.addon64"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", required=True, type=Path)
    parser.add_argument("--runtime", required=True, type=Path, help="Supplied x64 ReShade DLL with add-on support")
    parser.add_argument("--regrade-addon", type=Path, help="Supplied " + ADDON)
    parser.add_argument("--regrade-shaders", type=Path, help="Folder holding MartysMods_REGRADE+.fx")
    parser.add_argument("--output", required=True, type=Path, help="Parent for fresh run folders")
    parser.add_argument("--case", action="append", help="Run only these cases")
    args = parser.parse_args()
    executable, runtime = args.exe.resolve(strict=True), args.runtime.resolve(strict=True)
    if bool(args.regrade_addon) != bool(args.regrade_shaders):
        raise SystemExit("--regrade-addon and --regrade-shaders go together")
    regrade = (args.regrade_addon.resolve(strict=True), args.regrade_shaders.resolve(strict=True)) if args.regrade_addon else None
    cases = CASES + (REGRADE_CASES if regrade else ())
    if args.case:
        cases = tuple(case for case in cases if case[0] in args.case)
    fixtures = Path(__file__).resolve().parent.parent / "fixtures" / "reshade"
    args.output.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix="integration-", dir=args.output.resolve()))
    identity = {"exe_sha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
                "runtime_sha256": hashlib.sha256(runtime.read_bytes()).hexdigest()}
    if regrade:
        identity["regrade_sha256"] = hashlib.sha256(regrade[0].read_bytes()).hexdigest()
    (root / "identity.json").write_text(json.dumps(identity, indent=2), encoding="utf-8")
    print(f"Evidence: {root}", flush=True)
    results, failed = [], []
    for name, argument, disabled, addon in cases:
        directory = root / name
        shutil.copytree(fixtures, directory)
        shutil.copy2(executable, directory / executable.name)
        shutil.copy2(runtime, directory / "dxgi.dll")
        ini = directory / "ReShade.ini"
        general = "[GENERAL]\nIntermediateCachePath=.\\cache\n" + ("Disable=1\n" if disabled else "")
        ini.write_text(ini.read_text(encoding="utf-8").replace("[GENERAL]\n", general, 1), encoding="utf-8")
        if addon:
            shutil.copy2(regrade[0], directory / ADDON)
            shutil.copytree(regrade[1], directory / "effects", dirs_exist_ok=True)
        with (directory / "stdout.txt").open("wb") as stdout, (directory / "stderr.txt").open("wb") as stderr:
            with KillOnCloseJob() as job:
                child = subprocess.Popen([str(directory / executable.name), argument], cwd=directory,
                                         stdout=stdout, stderr=stderr, creationflags=subprocess.CREATE_NO_WINDOW)
                try:
                    job.attach(child.pid)
                    code = child.wait(timeout=120)
                except subprocess.TimeoutExpired:
                    code = "timeout"
                finally:
                    if child.poll() is None:
                        child.kill()
                    child.wait(timeout=5)
        output = (directory / "stdout.txt").read_text(errors="replace").strip().splitlines()
        error = (directory / "stderr.txt").read_text(errors="replace").strip()
        result = {"case": name, "argument": argument, "exit": code, "last": output[-1] if output else "", "stderr": error}
        results.append(result)
        print(json.dumps(result), flush=True)
        if code != 0:
            failed.append(name)
        else:
            # Successful folders keep their logs, not their binaries.
            (directory / executable.name).unlink()
            (directory / "dxgi.dll").unlink()
    (root / "results.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
    if failed:
        raise SystemExit(f"Failed: {', '.join(failed)}; see {root}")


if __name__ == "__main__":
    main()
