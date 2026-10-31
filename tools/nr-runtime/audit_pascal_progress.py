"""Inventory PTX entries and backward edges with memory polls (not a liveness proof).

Includes every entry, even unused/original entries. Retains exact memory-loop
instructions so induction loops can be distinguished from flag/cache polling.
Use on original, c37 native, and final lowered PTX independently.
"""
from pathlib import Path
import argparse
import hashlib
import json
import re


def entries(text):
    for match in re.finditer(r'(?:\.visible\s+)?\.entry\s+(\w+)\s*\(', text):
        start = text.index('{', match.end())
        end, depth = start + 1, 1
        while depth:
            depth += (text[end] == '{') - (text[end] == '}')
            end += 1
        yield match[1], text[start:end], text.count('\n', 0, start) + 1


def audit(text):
    out = []
    for name, body, first_line in entries(text):
        lines = body.splitlines()
        labels = {m[1]: i for i, line in enumerate(lines) if (m := re.match(r'^\s*([\w$]+):', line))}
        edges = []
        for i, line in enumerate(lines):
            branch = re.search(r'\bbra(?:\.uni)?\s+([\w$]+)\s*;', line)
            if not branch or branch[1] not in labels or labels[branch[1]] > i:
                continue
            a = labels[branch[1]]
            loop = '\n'.join(lines[a:i+1])
            memory = re.findall(r'^\s*(?:@\S+\s+)?((?:ld|atom|red)\.[^;]+;)', loop, re.M)
            # Long finite GEMM loops remain in the inventory by edge count;
            # bounded-size memory-poll candidates get exact instruction evidence.
            polling = [x.strip() for x in memory if re.search(r'volatile|acquire|relaxed|atom\.|\.cg\.', x)]
            if polling and i-a <= 160:
                edges.append(dict(line=first_line+a, endLine=first_line+i, target=branch[1],
                                  memory=polling, instructions=loop))
        out.append(dict(name=name, firstLine=first_line, smid='%smid' in body,
                        ctaDimensions=sorted(set(re.findall(r'%ctaid\.([xyz])', body))),
                        memoryLoopCandidates=edges,
                        barrierCount=len(re.findall(r'\bbar\.', body)),
                        fenceCount=len(re.findall(r'\bmembar\.', body))))
    return out


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('ptx', nargs='+', type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    reports = [dict(path=str(p.resolve()), sha256=hashlib.sha256(p.read_bytes()).hexdigest(), entries=audit(p.read_text())) for p in args.ptx]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(reports, indent=2) + '\n')
    print(f'{len(reports)} inputs, {sum(len(x["entries"]) for x in reports)} entries; {sum(len(e["memoryLoopCandidates"]) for x in reports for e in x["entries"])} memory-loop candidates')
