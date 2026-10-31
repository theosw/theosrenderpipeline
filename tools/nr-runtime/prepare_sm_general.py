"""Prepare the twelve selected c37 swinmh sources with a topology-free pool.

This is an experimental allocator candidate, NOT a chain-liveness fix. See
sm_general_slots.cuh. External c37 source inputs are verified against a lockfile;
they and generated PTX/cubins stay outside public Git. Run with python -I.
"""
from pathlib import Path
import argparse
import hashlib
import json
import re
import shutil

ROOT = Path(__file__).resolve().parent


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def transform(source, heads):
    paired = heads == 2
    source = re.sub(r'^#define swinmh_pool_(?:used|owners|next) .+\n', '', source, flags=re.M)
    old = ('constexpr int swinmh_windows=19*(swinmh_H==2?8:swinmh_H==4?4:2);\n'
           '__device__ unsigned swinmh_pool_used[swinmh_windows];\n'
           '__device__ unsigned swinmh_pool_owners[19];')
    new = '''// FIFO capacity is a performance/memory choice, never an SM count.
constexpr unsigned swinmh_slot_shift=SWINMH_HEADS==8?5:6;
constexpr unsigned swinmh_allocations=1u<<swinmh_slot_shift;
constexpr int swinmh_windows=swinmh_allocations*(SWINMH_HEADS==2?2:1);
#define swinmh_tickets SWINMH_JOIN(swinmh_tickets_,SWINMH_VARIANT)
#define swinmh_turns SWINMH_JOIN(swinmh_turns_,SWINMH_VARIANT)
__device__ unsigned long long swinmh_tickets;
__device__ unsigned long long swinmh_turns[swinmh_allocations];
#include "sm_general_slots.cuh"'''
    if source.count(old) != 1:
        raise ValueError('pool declaration differs from pinned c37 source')
    source = source.replace(old, new)
    # Preserve the exact original producer geometry/polls, but do not hold any
    # scratch allocation (or cache reader) while polling those producers.
    a = source.index('    if(SWINMH_VARIANT==1||SWINMH_VARIANT==2) {', source.index('void swinmh_layer'))
    z = source.index('    if(SWINMH_VARIANT==3) {', a)
    wait = source[a:z]
    source = source[:a] + source[z:]
    a = source.index('    // Reserve both scratch' if paired else '    if(tid==0) {\n        unsigned smid', source.index('void swinmh_layer'))
    z = source.index('    bool cached=' if paired else '    unsigned char* x=swinmh_scratch', a)
    leader = 'tid==0&&swinmh_group==0' if paired else 'tid==0'
    slot = '(unsigned(swinmh_ticket)&(swinmh_allocations-1))*2+swinmh_group' if paired else 'unsigned(swinmh_ticket)&(swinmh_allocations-1)'
    temp = 'swinmh_storage' if paired else 'swinmh_as'
    replacement = wait + f'''    // Reuse existing staging storage: H8 already uses all 48 KiB shared.
    if({leader}) {{
        auto ticket=trp_slot_acquire<swinmh_slot_shift>(&swinmh_tickets,swinmh_turns);
        {temp}[0]=__uint_as_float(unsigned(ticket));
        {temp}[1]=__uint_as_float(unsigned(ticket>>32));
    }}
    __syncthreads();
    const unsigned long long swinmh_ticket=__float_as_uint({temp}[0])|(
        static_cast<unsigned long long>(__float_as_uint({temp}[1]))<<32);
    __syncthreads();
    int slot={slot};
'''
    source = source[:a] + replacement + source[z:]
    old_release = f'if({leader})atomicExch(swinmh_pool_used+slot,0u);'
    if source.count(old_release) != 1:
        raise ValueError('release differs from pinned c37 source')
    source = source.replace(old_release, f'if({leader})trp_slot_release<swinmh_slot_shift>(swinmh_turns,swinmh_ticket);')
    if '%smid' in source or 'swinmh_pool_used' in source:
        raise ValueError('old allocator remains')
    return source


def prepare(research_root, output, lockfile):
    lock = json.loads(lockfile.read_text())
    for rel, digest in lock['files'].items():
        path = research_root / rel
        if sha(path) != digest:
            raise ValueError(f'c37 input changed: {path}')
    output.mkdir(parents=True, exist_ok=False)
    lane = research_root / 'hillclimb/lanes/swinmh'
    for path in lane.glob('*.cuh'):
        if str(path.relative_to(research_root)).replace('\\', '/') in lock['files']:
            shutil.copyfile(path, output / path.name)
    shutil.copyfile(ROOT / 'sm_general_slots.cuh', output / 'sm_general_slots.cuh')
    sources = []
    for h in (2, 4, 8):
        for v in range(4):
            stem = f'r13linearh2v{v}' if h == 2 else f'r11vectorcomboh{h}v{v}'
            path = output / f'h{h}v{v}.cu'
            path.write_text(transform((lane / (stem + '.cu')).read_text(), h), encoding='ascii', newline='\n')
            sources.append(dict(path=path.name, heads=h, variant=v, sha256=sha(path)))
    (output / 'sources.json').write_text(json.dumps(sources, indent=2) + '\n')
    return sources


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--research-root', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--lock', type=Path, default=ROOT / 'c37-inputs.lock.json')
    args = parser.parse_args()
    prepare(args.research_root, args.output, args.lock)
