# Optional combined preparation for Neural Rendering

Combined preparation reduces eligible preparation dispatches while evaluating
NR on every source frame. It builds on optional peripheral compression.

In the Neural Rendering tab, enable **Combined preparation (experimental)** and
select **Apply now**. `NRFusedPreparation` defaults to false, including when it
is absent from an older INI. Changing it uses the existing drained recreation
path. Peripheral compression remains a separate option.

## What changes

- When reconstruction needs both colour encoding and reduced/peripheral
  sampling, one dispatch encodes each tap before filtering. It preserves the
  eliminated intermediate texture's FP16 or UNORM8 quantization.
- With peripheral compression, one dispatch packs depth and motion together.
  It retains nearest-depth sampling and the existing endpoint motion transform.
- Other configurations use the existing passes. At full input resolution with
  peripheral compression off there may be nothing to combine. Runtime details
  report whether colour and guide preparation actually combined.

The existing one/two NR passes, both placements, colour reconstruction, native
UI and frame-generation world inputs remain. This change adds no history
textures, temporal reuse, skipped evaluations or queue policy.

## Evidence and limits

The preparation kernels are extracted unchanged from the combined experimental
candidate `c53a0e2b1cf9`. In its ENB test on an RTX 4080 SUPER, guide preparation
combined at model extent 3072x864 with peripheral compression, before upscaling
from 3413x960 to 5120x1440. The user accepted the visual result and chose to keep
both optional features. The short uncombined interval measured about 6.10 ms
early-NR round trip; combined intervals measured about 6.12-6.14 ms. Scene and
FG changes were uncontrolled, so this is not evidence of a speedup. Three
recurring vendor RSYNC flip-queue errors occurred outside the temporal interval;
final host/query/NR failure counters remained zero.

Temporal reuse is excluded because the same test reported microstutters when
it was enabled. The exact extracted DLL has not had another game test. Colour
fusion, CS and after-upscaling gameplay remain unverified for this change.

The software-GPU fixture compares combined and separate colour/guide kernels
across 12 format/layout combinations, including odd sizes, bright nonlinear
taps, FP32/FP16/UNORM8 storage and FP16 motion. An additional 64 production-pass
configurations exercise preparation off/on, peripheral off/on, native and CS
contracts, both placements, one/two passes, two input scales, loading resets,
fresh UI and actual/no-op status. Existing peripheral tests remain intact.
Configuration checks cover defaults, persistence, explicit opt-out and retired
recreation. Test results for the extracted source are recorded in the PR.
