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
        [--native 5=native_vit.ptx] [--groups]
pascal_emulation.ptx: nvcc -arch=sm_61 -rdc=true -ptx -O3 pascal_emulation.cu (no fast math).
native_vit.ptx: nvcc -arch=sm_61 -ptx -O3 -std=c++17 native_vit.cu; its kernels replace the
same-named NVIDIA kernels of bundle 5 (cc_vit_1d_ffn_expand/contract, qkv).
--groups: replace runs of FP8 mma with grouped stand-ins (pascal_groups.py), generated for the
run shapes of bundles 0-6 and compiled with CUDA 12.9's nvcc inside the --vcvars environment.
"""
from pathlib import Path
import argparse
import json
import re
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ada_lowering
import build_rtx40_kernels as ada
import pascal_groups
import pascal_lowering
import launch_metadata
from ada_lowering import require


def retarget(ptx):
    """PTX that needs no lowering, for CUDA 12.9's ptxas (PTX ISA 8.8, sm_61)."""
    require(len(re.findall(r'\.target sm_(?:89|120)\b', ptx)) == 1, 'expected one target')
    ptx = re.sub(r'\.target sm_(?:89|120)\b', '.target sm_61', ptx)
    return re.sub(r'\.version 9\.\d+', '.version 8.8', ptx)


def module_ptx(cuda13, index, original, work):
    """NVIDIA's PTX for a bundle, lowered for Ada where needed."""
    folder = work / f'bundle{index:02}'
    folder.mkdir()
    _, ptxs = ada.unpack(cuda13, folder / 'nvidia', 'nvidia.fatbin', original)
    require(set(ptxs) == {'sm_120'}, f'bundle {index}: expected sm_120 PTX')
    ptx = ptxs['sm_120'].decode('ascii').rstrip('\0')
    if index in ada.LOWERED:
        return ada_lowering.lower(ptx, ada.LOWERED[index])[0]
    return ptx.replace('.target sm_120', '.target sm_89')


def compile_groups(cuda12, vcvars, signatures, work):
    """sm_61 PTX of the grouped stand-ins for `signatures` (nvcc needs the MSVC environment)."""
    folder = work / 'groups'
    folder.mkdir()
    (folder / 'groups.cu').write_text(pascal_groups.generate_cu(signatures), encoding='ascii', newline='\n')
    nvcc = Path(cuda12) / 'bin' / 'nvcc.exe'
    command = (f'call "{vcvars}" >nul 2>nul && "{nvcc}" -arch=sm_61 -rdc=true -ptx -O3 '
               f'-Wno-deprecated-gpu-targets groups.cu -o groups.ptx')
    result = subprocess.run(f'cmd /d /s /c "{command}"', cwd=folder, capture_output=True, text=True)
    require(result.returncode == 0, f'nvcc groups.cu failed: {result.stdout[-2000:]} {result.stderr[-2000:]}')
    return (folder / 'groups.ptx').read_text(encoding='ascii')


def build_bundle(cuda12, index, ptx, emulation, work, native=None, groups=None, maxnreg=True):
    folder = work / f'bundle{index:02}'
    if index < 7:
        pascal, counts = pascal_lowering.lower(ptx, emulation, groups)
        if not maxnreg:
            pascal, dropped = re.subn(r'\.maxnreg \d+\s*\n', '', pascal)
            counts = dict(counts, droppedMaxnreg=dropped)
    else:
        pascal, counts = retarget(ptx), {}
    replaced = []
    for native_ptx in native or ():
        pascal, names = pascal_lowering.replace_entries(pascal, native_ptx)
        replaced += names
    if replaced:
        counts = dict(counts, native=replaced)
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
    require(sorted(entries) == sorted(re.findall(r'\.entry\s+(\w+)', pascal)), f'bundle {index}: kernel entries changed')
    return payload, dict(entries=len(entries), lowered=counts, bundleBytes=len(payload),
                         sm61=ada.sha256((folder / 'pascal.sm_61.cubin').read_bytes()),
                         sm89=ada.sha256((folder / 'pascal.sm_89.cubin').read_bytes()))


def background_priority():
    """Below-normal CPU priority for this build and its ptxas/nvcc children (Windows)."""
    try:
        import ctypes
        kernel32 = ctypes.windll.kernel32
        kernel32.SetPriorityClass(kernel32.GetCurrentProcess(), 0x4000)   # BELOW_NORMAL_PRIORITY_CLASS
    except (AttributeError, OSError):
        pass


def main():
    background_priority()
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    for name in ('official', 'cuda13', 'cuda12', 'emulation_ptx', 'output'):
        parser.add_argument('--' + name.replace('_', '-'), dest=name, type=Path, required=True)
    parser.add_argument('--native', action='append', default=[], metavar='BUNDLE=PTX',
                        help='native sm_61 kernels replacing same-named NVIDIA kernels in a bundle (repeatable)')
    parser.add_argument('--launch', action='append', default=[], metavar='KERNEL:BLOCK[:GRID][:SHARED]',
                        help='required native launch geometry (repeatable; TRPNRK1 v2)')
    parser.add_argument('--groups', action='store_true', help='grouped FP8 mma stand-ins (pascal_groups.py)')
    parser.add_argument('--group-max-a', type=int, default=pascal_groups.MAX_A, help='A fragments per grouped call')
    parser.add_argument('--no-maxnreg', action='store_true',
                        help="drop NVIDIA's .maxnreg (set for Ada) from the network kernels")
    parser.add_argument('--vcvars', type=Path,
                        default=Path(r'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat'))
    args = parser.parse_args()
    pascal_groups.MAX_A = args.group_max_a
    native = {}
    for item in args.native:   # several PTX files may replace kernels of the same bundle
        native.setdefault(int(item.split('=', 1)[0]), []).append(Path(item.split('=', 1)[1]).read_text(encoding='ascii'))
    launches = launch_metadata.parse(args.launch, native)
    original = args.official.read_bytes()
    require(ada.sha256(original) == ada.OFFICIAL_SHA256, 'original runtime identity differs')
    image = ada.Image(original)
    ada.check_sites(image)
    emulation = args.emulation_ptx.read_text(encoding='ascii')
    bundles, details = [], []
    with tempfile.TemporaryDirectory() as work:
        sources = [ada.fatbin_at(image, rva) for _, _, _, rva in ada.BUNDLES]
        ptxs = [module_ptx(args.cuda13, index, source, Path(work)) for index, source in enumerate(sources)]
        groups = None
        if args.groups:
            signatures = set().union(*(pascal_groups.analyze(ptx) for ptx in ptxs[:7]))
            groups = compile_groups(args.cuda12, args.vcvars, signatures, Path(work))
            print(f'grouped stand-ins: {len(signatures)} block shapes', flush=True)
        for index, (name, _, _, rva) in enumerate(ada.BUNDLES):
            payload, detail = build_bundle(args.cuda12, index, ptxs[index], emulation, Path(work), native.get(index), groups,
                                           maxnreg=not args.no_maxnreg)
            bundles.append((rva, len(sources[index]), payload))
            details.append(dict(name=name, **detail))
            print(f'{name}: {detail["entries"]} kernels, {len(payload)} bytes', flush=True)
    pack = ada.write_pack(bundles)
    require(ada.read_pack(pack) == bundles, 'pack read-back differs')
    pack = launch_metadata.append(pack, launches)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(pack)
    report = dict(original=ada.OFFICIAL_SHA256, pack=ada.sha256(pack), packBytes=len(pack), research=True, launches=launches, bundles=details)
    args.output.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(f'wrote {args.output} sha256={ada.sha256(pack)} bytes={len(pack)}', flush=True)


if __name__ == '__main__':
    try:
        main()
    except RuntimeError as error:
        sys.exit(f'build_pascal_kernels: {error}')
