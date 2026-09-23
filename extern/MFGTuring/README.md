# Turing PTX compatibility subset

Four unmodified MIT headers from nefh/MFGAmpereUnlock-RenoDx, revision
`93c5725a534840dc0fa7b1986b216e6b9da3877d` (2026-09-22).
Only the SM75 provider retargeter is called by TRP. Existing SM86/Ada paths
retain their original transformations. No ReShade host, alternate temporal
backend, quality override or pacing change is imported.

The exact source fingerprints qualify packed-half conversion, half min/max
and m16n8k16-to-two-m16n8k8 conversion for provider 310.9.1. FP16 accumulation
order is not bit-identical to the original MMA. RTX 20 execution remains
experimental pending a volunteer test. See the adjacent LICENSE and root
THIRD-PARTY.md. Unknown instruction forms/layouts remain rejected.
