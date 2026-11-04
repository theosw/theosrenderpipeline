"""Build the manual sm_86 prepared-weight probe from pinned local Fast inputs.

No runtime modification, installation, GPU execution, or vendor download occurs.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from build_rtx40_kernels import read_pack, write_pack, run, toolchain, unpack
from prepare_ffn_weights import SOURCE_SHA256

BASE_SHA256 = "3637668a64507fba7ee42bb6571d96e03d47b5f59bbf32a00a8055a43d06c0bc"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ptx", required=True, type=Path)
    parser.add_argument("--base-pack", required=True, type=Path)
    parser.add_argument("--cuda", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    sha = lambda data: hashlib.sha256(data).hexdigest()
    raw = args.base_pack.read_bytes()
    if sha(raw) != BASE_SHA256 or sha(args.ptx.read_bytes()) != SOURCE_SHA256:
        raise SystemExit("expected the pinned Fast sm_86 pack and module05 PTX")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    cuda = args.cuda.resolve()
    version = toolchain(cuda)
    here = Path(__file__).resolve().parent
    subprocess.run([sys.executable, "-I", str(here / "prepare_ffn_weights.py"), str(args.ptx.resolve()),
                    str(output / "module05.ptx")], check=True)
    for source, cubin in ((output / "module05.ptx", "module05.cubin"),
                          (here / "prepare_ffn_weights.ptx", "helper.cubin")):
        run(cuda, "ptxas", "-arch=sm_86", "-O3", str(source), "-o", str(output / cubin), cwd=output)
    for name in ("module05", "helper"):
        run(cuda, "fatbinary", "--64", "--compress-all", f"--create={name}.fatbin",
            f"--image3=kind=elf,sm=86,file={name}.cubin", cwd=output)
        blob = (output / f"{name}.fatbin").read_bytes()
        blob = blob[:16 + struct.unpack_from("<Q", blob, 8)[0]]
        (output / f"{name}.fatbin").write_bytes(blob)
        cubins, ptxs = unpack(cuda, output / f"{name}-check", "check.fatbin", blob)
        if ptxs or cubins != {"sm_86": (output / f"{name}.cubin").read_bytes()}:
            raise RuntimeError("fatbin read-back differs")
    bundles = read_pack(raw)
    rva, size, _ = bundles[5]
    bundles[5] = (rva, size, (output / "module05.fatbin").read_bytes())
    pack = write_pack(bundles)
    if read_pack(pack) != bundles:
        raise RuntimeError("pack read-back differs")
    (output / "prepared-sm86.trpk").write_bytes(pack)
    manifest = dict(toolchain=version, base_pack_sha256=BASE_SHA256, source_ptx_sha256=SOURCE_SHA256,
                    research_only=True, changed_bundles=[5],
                    files={p.name: sha(p.read_bytes()) for p in output.iterdir() if p.is_file()})
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
