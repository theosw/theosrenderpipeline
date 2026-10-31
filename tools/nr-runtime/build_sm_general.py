"""Build the experimental topology-free allocator pack and separate SM89 reference.

python -I build_sm_general.py --research-root <trp-pascal> --official <310.9.16.dll>
  --cuda12 <CUDA12.9> --cuda13 <CUDA13.4> --output <NEW build directory>

The pack retains cross-CTA producer/slice waits. It is not release qualified.
"""
from pathlib import Path
import argparse
import json
import re
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import prepare_sm_general as prepare

ROOT = Path(__file__).resolve().parent


def merge_ptx(paths):
    globals_seen = {}
    bodies = []
    for path in paths:
        ptx = path.read_text()
        body = ptx.split('.address_size 64', 1)[1]
        def global_decl(match):
            text = match[0]
            name = re.search(r'\b(\w+)\s*(?:\[\d+\])?\s*(?:=|;)', text)[1]
            if name in globals_seen:
                if globals_seen[name] != text:
                    raise ValueError(f'conflicting PTX global: {name}')
                return ''
            globals_seen[name] = text
            return text
        body = re.sub(r'^\.global\b[^;]+;', global_decl, body, flags=re.M)
        bodies.append(body)
    return '.version 8.8\n.target sm_61\n.address_size 64\n' + '\n'.join(bodies)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('research-root', 'official', 'cuda12', 'cuda13', 'output'):
        parser.add_argument('--' + name, required=True, type=Path)
    parser.add_argument('--vcvars', type=Path, default=Path(r'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat'))
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    native_dir = out / 'native'
    sources = prepare.prepare(args.research_root, native_dir, ROOT / 'c37-inputs.lock.json')
    compile_records = []
    for source in sources:
        h, v = source['heads'], source['variant']
        cu = native_dir / source['path']
        ptx = cu.with_suffix('.ptx')
        command = [str(args.cuda12 / 'bin/nvcc.exe'), '-ptx', '-arch=sm_61', '-O3', '-std=c++17',
                   '-Wno-deprecated-gpu-targets', f'-DSWINMH_HEADS={h}', f'-DSWINMH_VARIANT={v}']
        if h != 2 or v not in (1, 3):
            command.append(f'-DSWINMH_FFN_WARPS={2 if h == 2 else 4}')
        command += [str(cu), '-o', str(ptx)]
        batch = cu.with_suffix('.cmd')
        batch.write_text('@echo off\ncall "' + str(args.vcvars) + '" >nul 2>nul\n' + subprocess.list2cmdline(command) + '\nexit /b %ERRORLEVEL%\n')
        with cu.with_suffix('.log').open('w') as log:
            subprocess.run(['cmd', '/d', '/c', str(batch)], stdout=log, stderr=subprocess.STDOUT, check=True)
        text = ptx.read_text()
        text = re.sub(r'(\.maxntid[^\n]*\n)', r'\1.maxnreg 128\n', text)
        # Preserve the selected c37 cache-key visibility workaround.
        text = re.sub(r'ld\.global\.u64(\s+[^\n]+\[swinmh_r(?:5_cache|7)_key_\d+\])', r'ld.global.cg.u64\1', text)
        ptx.write_text(text, encoding='ascii', newline='\n')
        compile_records.append(dict(**source, command=command, ptxSha256=prepare.sha(ptx)))
        print(f'compiled H{h} phase {v}', flush=True)
    for h in (2, 4, 8):
        (native_dir / f'family{h}.ptx').write_text(merge_ptx([native_dir / f'h{h}v{v}.ptx' for v in range(4)]), encoding='ascii', newline='\n')
    specs = (args.research_root / 'hillclimb/lanes/vitfuse/R8-control-c37-native.txt').read_text().splitlines()
    native = []
    for line in specs:
        bundle, path = line.removeprefix('--native ').split('=', 1)
        if int(bundle) in (1, 2, 3):
            path = str(native_dir / f'family{2 ** int(bundle)}.ptx')
        else:
            # The historical selection list contains workstation absolute paths.
            path = str(args.research_root / 'hillclimb' / path.replace('\\', '/').split('/hillclimb/', 1)[1])
        native += ['--native', f'{bundle}={path}']
    launch = []
    for line in (args.research_root / 'hillclimb/combined/c37-launch-rules.txt').read_text().splitlines():
        if line.strip():
            launch += ['--launch', line.strip()]
    common = [sys.executable, '-I', str(ROOT / 'build_pascal_kernels.py'), '--official', str(args.official),
              '--cuda12', str(args.cuda12), '--cuda13', str(args.cuda13), '--emulation-ptx',
              str(args.research_root / 'lower/pascal_emulation.ptx'), *native, *launch]
    for label, architectures in (('pascal', '61'), ('reference', '61,89')):
        command = [*common, '--architectures', architectures, '--work-dir', str(out / label), '--output', str(out / f'sm-general-{label}.trpk')]
        with (out / (label + '.log')).open('w') as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
        print(f'built {label}', flush=True)
    manifest = dict(status='experimental allocator candidate; dependent-dispatch and slice waits NOT solved',
                    sourceCompiles=compile_records, inputLock=json.loads((ROOT / 'c37-inputs.lock.json').read_text()),
                    generatedSources={p.name: prepare.sha(p) for p in sorted(native_dir.iterdir()) if p.suffix in ('.cu', '.cuh', '.ptx')})
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')


if __name__ == '__main__':
    main()
