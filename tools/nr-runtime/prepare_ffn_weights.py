"""Research-only prepared weights for the Fast sm_86 module05 FFN expansion.

Consumes locally supplied, hash-pinned PTX; no vendor source/binary is included.
Adds two suffixed entries for the manual benchmark, retaining original entries.
The cache stores two half2 words per packed FP8 word in even/odd byte order.
Activation decode, tensor operations, flags and launch geometry stay unchanged.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re

SOURCE_SHA256 = "741f4e87b1493b9ba728c10448c80e45b7ffcb02184327380ca032c216ef7f4d"
TARGETS = ("cc_vit_1d_ffn_expand_chained_fp8", "cc_vit_1d_ffn_expand_publish_fp8")
SUFFIX = "_trp_prepared_v1"
LOAD = re.compile(r"ld\.weak\.global\.ca\.v4\.u32\s*\{([^}]+)\},\[%sm86_lut_rest_wptr\+(\d+)\];")
DECODE = re.compile(r"and\.b32 (%hx\d+), (%r\d+), 0x007F007F;\n(?:[^\n]+\n){9}mul\.rn\.f16x2 %hx\d+, %hx\d+, %ax23;")


def transform(body, name):
    loads = list(LOAD.finditer(body))
    if len(loads) != 8:
        raise ValueError("expected eight streamed weight loads")
    weights = {r for m in loads for r in re.findall(r"%r\d+", m[1])}
    if len(weights) != 32 or "mov.b32 %ax23, 0x5C005C00;" not in body:
        raise ValueError("unrecognized weight decode contract")
    outputs = {}

    def decode(m):
        if m[2] not in weights:
            return m[0]
        n = int(m[1][3:]) - 2
        r = m[2]
        expected = [
            f"and.b32 %hx{n+2}, {r}, 0x007F007F;", f"shl.b32 %hx{n+2}, %hx{n+2}, 7;",
            f"and.b32 %hx{n+3}, {r}, 0x00800080;", f"shl.b32 %hx{n+3}, %hx{n+3}, 8;",
            f"or.b32 %hx{n}, %hx{n+2}, %hx{n+3};", f"mul.rn.f16x2 %hx{n}, %hx{n}, %ax23;",
            f"and.b32 %hx{n+4}, {r}, 0x7F007F00;", f"shr.u32 %hx{n+4}, %hx{n+4}, 1;",
            f"and.b32 %hx{n+5}, {r}, 0x80008000;", f"or.b32 %hx{n+1}, %hx{n+4}, %hx{n+5};",
            f"mul.rn.f16x2 %hx{n+1}, %hx{n+1}, %ax23;",
        ]
        if m[0].splitlines() != expected or r in outputs:
            raise ValueError("unexpected decode operands or repeated decode")
        outputs[r] = (f"%hx{n}", f"%hx{n+1}")
        return ""

    result = DECODE.sub(decode, body)
    if set(outputs) != weights:
        raise ValueError("incomplete weight decode mapping")

    def load(m):
        registers = [v for r in re.findall(r"%r\d+", m[1]) for v in outputs[r]]
        offset = 2 * int(m[2])
        return "\n".join(f"ld.weak.global.ca.v4.u32 {{{','.join(registers[i:i+4])}}}, [%pw_ptr+{offset+4*i}];" for i in (0, 4))

    result = LOAD.sub(load, result)
    anchor = re.search(r"add.u64 %sm86_lut_rest_wptr, %rd\d+, %sm86_lut_rest_koff;", result)
    if not anchor or result.count(anchor[0]) != 1:
        raise ValueError("unrecognized streamed weight pointer")
    result = result.replace(anchor[0], anchor[0] + "\nsub.u64 %pw_ptr, %sm86_lut_rest_wptr, %rd3;\nshl.b64 %pw_ptr, %pw_ptr, 1;\nadd.u64 %pw_ptr, %pw_ptr, %pw_cache;")
    parameter = name + "_param_0"
    result = result.replace(parameter + "[72]", parameter + "[80]", 1)
    result = result.replace("{\n", "{\n.reg .b64 %pw_cache, %pw_ptr;\nld.param.u64 %pw_cache, [" + parameter + "+72];\n", 1)
    # Audit all MMA operations, including their order and register operands.
    mma = lambda text: re.findall(r"^mma\.[^\n]+", text, re.M)
    if len(mma(body)) != 128 or mma(body) != mma(result):
        raise ValueError("tensor operations changed")
    if len(DECODE.findall(result)) != 32:
        raise ValueError("activation decode changed")
    return result.replace(name, name + SUFFIX)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    raw = args.source.read_bytes()
    if hashlib.sha256(raw).hexdigest() != SOURCE_SHA256:
        raise SystemExit("source PTX fingerprint mismatch")
    source = raw.decode().replace("\r\n", "\n")
    additions = []
    for name in TARGETS:
        start = source.index(".visible .entry " + name + "(")
        end = source.find(".visible .entry ", start + 1)
        body = source[start:end if end != -1 else len(source)]
        additions.append(transform(body, name))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    result = source + "\n" + "\n".join(additions)
    args.output.write_text(result, encoding="utf-8", newline="\n")
    audit = dict(source_sha256=SOURCE_SHA256, output_sha256=hashlib.sha256(args.output.read_bytes()).hexdigest(),
                 kernels=list(TARGETS), original_entries_retained=True, mma_per_kernel=128,
                 weight_decode_blocks_removed_per_kernel=32, activation_decode_blocks_unchanged_per_kernel=32,
                 parameter_bytes=80, cache_bytes_per_bank=8388608)
    args.output.with_suffix(".json").write_text(json.dumps(audit, indent=2) + "\n")
    print(json.dumps(audit))


if __name__ == "__main__":
    main()
