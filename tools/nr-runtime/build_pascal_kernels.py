"""Research: a kernel pack that runs NVIDIA NR 310.9.16 on GTX 10 (Pascal, sm_61). Run with python -I.

NVIDIA's PTX for every bundle is lowered for Ada where needed (ada_lowering.py), then for
Pascal (pascal_lowering.py, network modules 0-6 only; the helpers need nothing), and compiled
with CUDA 12.9, the last toolkit that targets sm_61. Each bundle carries the Pascal code plus
the same lowered code compiled for sm_89, so an RTX 40 card can check the stand-ins against
NVIDIA's output. Unpinned and unqualified: research builds only, loaded through
TRP_NR_PASCAL_RESEARCH=1 and TRP_NR_PASCAL_PACK=<pack> (NeuralRenderingFeatureSession.cpp).
Same TRPNRK1 layout as build_rtx40_kernels.py. Usage:
    python -I build_pascal_kernels.py --official nvngx_dlssnr.dll --cuda13 <CUDA 13.4 root>
        --cuda12 <CUDA 12.9 root> --emulation-ptx pascal_emulation.ptx --output <pack>
pascal_emulation.ptx: nvcc -arch=sm_61 -rdc=true -ptx -O3 pascal_emulation.cu (no fast math).
"""
from pathlib import Path
import argparse
import json
import re
import struct
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ada_lowering
import build_rtx40_kernels as ada
import pascal_lowering
from ada_lowering import require


def retarget(ptx):
    """PTX that needs no lowering, for CUDA 12.9's ptxas (PTX ISA 8.8, sm_61)."""
    require(len(re.findall(r'\.target sm_(?:89|120)\b', ptx)) == 1, 'expected one target')
    ptx = re.sub(r'\.target sm_(?:89|120)\b', '.target sm_61', ptx)
    return re.sub(r'\.version 9\.\d+', '.version 8.8', ptx)


def build_bundle(cuda13, cuda12, index, original, emulation, work):
    folder = work / f'bundle{index:02}'
    folder.mkdir()
    _, ptxs = ada.unpack(cuda13, folder / 'nvidia', 'nvidia.fatbin', original)
    require(set(ptxs) == {'sm_120'}, f'bundle {index}: expected sm_120 PTX')
    ptx = ptxs['sm_120'].decode('ascii').rstrip('\0')
    if index in ada.LOWERED:
        ptx = ada_lowering.lower(ptx, ada.LOWERED[index])[0]
    else:
        ptx = ptx.replace('.target sm_120', '.target sm_89')
    if index < 7:
        pascal, counts = pascal_lowering.lower(ptx, emulation)
    else:
        pascal, counts = retarget(ptx), {}
    (folder / 'pascal.ptx').write_text(pascal, encoding='ascii', newline='\n')
    for sm in (61, 89):
        ada.run(cuda12, 'ptxas', f'-arch=sm_{sm}', '-O3', 'pascal.ptx', '-o', f'pascal.sm_{sm}.cubin', cwd=folder)
    ada.run(cuda12, 'fatbinary', '--64', '--create=trp.fatbin',
            '--image3=kind=elf,sm=61,file=pascal.sm_61.cubin',
            '--image3=kind=elf,sm=89,file=pascal.sm_89.cubin', cwd=folder)
    payload = (folder / 'trp.fatbin').read_bytes()
    require(payload[:8] == ada.FATBIN_MAGIC, f'bundle {index}: fatbinary output is not a fatbin')
    payload = payload[:16 + struct.unpack_from('<Q', payload, 8)[0]]
    entries = re.findall(r'\.entry\s+(\w+)', ptx)
    require(entries == re.findall(r'\.entry\s+(\w+)', pascal), f'bundle {index}: kernel entries changed')
    return payload, dict(entries=len(entries), lowered=counts, bundleBytes=len(payload),
                         sm61=ada.sha256((folder / 'pascal.sm_61.cubin').read_bytes()),
                         sm89=ada.sha256((folder / 'pascal.sm_89.cubin').read_bytes()))


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    for name in ('official', 'cuda13', 'cuda12', 'emulation_ptx', 'output'):
        parser.add_argument('--' + name.replace('_', '-'), dest=name, type=Path, required=True)
    args = parser.parse_args()
    original = args.official.read_bytes()
    require(ada.sha256(original) == ada.OFFICIAL_SHA256, 'original runtime identity differs')
    image = ada.Image(original)
    ada.check_sites(image)
    emulation = args.emulation_ptx.read_text(encoding='ascii')
    bundles, details = [], []
    with tempfile.TemporaryDirectory() as work:
        for index, (name, _, _, rva) in enumerate(ada.BUNDLES):
            source = ada.fatbin_at(image, rva)
            payload, detail = build_bundle(args.cuda13, args.cuda12, index, source, emulation, Path(work))
            bundles.append((rva, len(source), payload))
            details.append(dict(name=name, **detail))
            print(f'{name}: {detail["entries"]} kernels, {len(payload)} bytes', flush=True)
    pack = ada.write_pack(bundles)
    require(ada.read_pack(pack) == bundles, 'pack read-back differs')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(pack)
    report = dict(original=ada.OFFICIAL_SHA256, pack=ada.sha256(pack), packBytes=len(pack), research=True, bundles=details)
    args.output.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(f'wrote {args.output} sha256={ada.sha256(pack)} bytes={len(pack)}', flush=True)


if __name__ == '__main__':
    try:
        main()
    except RuntimeError as error:
        sys.exit(f'build_pascal_kernels: {error}')
