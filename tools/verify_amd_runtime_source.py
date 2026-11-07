"""Check the complete source snapshot and the shared host/runtime interfaces."""
from pathlib import Path
import hashlib
import json

root = Path(__file__).resolve().parents[1]
base = root / "third_party/amd-nr"
manifest = json.loads((base / "runtime-source.json").read_text(encoding="utf-8"))
runtime = base / "runtime"
actual = {p.relative_to(runtime).as_posix() for p in runtime.rglob("*") if p.is_file() and "__pycache__" not in p.parts}
assert actual == set(manifest["files"]), "runtime source file list changed"
for name, expected in manifest["files"].items():
    content = (runtime / name).read_bytes().replace(b"\r\n", b"\n")
    assert hashlib.sha256(content).hexdigest() == expected, name
for name in ["dlss5nr.h", "trp_adapter_selection.h", "trp_diagnostics.h", "trp_hip_diagnostics.h"]:
    assert (base / name).read_text() == (runtime / "include" / name).read_text(), name
print(f"Verified {len(actual)} runtime source files and four shared interfaces; upstream {manifest['upstreamCommit']}")
