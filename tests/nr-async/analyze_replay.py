"""Offline FP16 capture validation and aligned image comparisons (NumPy, Pillow).

Pixel disagreement and temporal residuals are diagnostics, not perceptual scores.
The procedural suite cannot establish Skyrim, face or physical-cadence acceptance.
"""
import argparse
import csv
import json
import pathlib

import numpy as np
from PIL import Image, ImageDraw


def preview(rgb):
    # Identical display mapping for all panels; raw linear HDR remains retained.
    rgb = np.maximum(rgb, 0)
    mapped = np.power(rgb / (1 + rgb), 1 / 2.2)
    return Image.fromarray(np.uint8(np.clip(mapped * 255, 0, 255)))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("captures", type=pathlib.Path)
    args = parser.parse_args()
    root = args.captures.resolve(strict=True)
    identity = json.loads((root / "identity-and-results.json").read_text())
    if not identity["capture_hz"]:
        raise SystemExit("Capture invocation required.")
    rates = {run["requested_fps"] for run in identity["runs"]}
    if len(rates) != 1:
        raise SystemExit("Analyze one source rate per capture directory.")
    w, h = identity["width"], identity["height"]
    runs = {run["mode"]: run for run in identity["runs"]}
    if set(runs) != {"off", "regular", "reset", "async"}:
        raise SystemExit("One complete run per comparison mode required.")
    pixels, index = {}, {}
    for mode, run in runs.items():
        if not run["measurement_complete"]:
            raise SystemExit(f"Incomplete {mode} measurement.")
        directory = root / run["directory"]
        with (directory / "captures.csv").open(newline="") as source:
            rows = list(csv.DictReader(source))
        if not rows:
            raise SystemExit(f"No captures for {mode}.")
        if len(rows) * w * h * 4 * 4 * len(runs) > 2 * 1024 ** 3:
            raise SystemExit("Analysis working set exceeds 2 GiB; use a smaller capture extent/rate.")
        frames = []
        for row in rows:
            path = directory / row["file"]
            if path.stat().st_size != w * h * 8:
                raise SystemExit(f"Invalid FP16 capture extent: {path}")
            image = np.fromfile(path, dtype="<f2").reshape(h, w, 4).astype(np.float32)
            if not np.isfinite(image).all():
                raise SystemExit(f"Nonfinite output: {path}")
            frames.append(image)
        pixels[mode] = np.stack(frames)
        index[mode] = [(int(r["frame"]), int(r["scene"]), float(r["source_time"])) for r in rows]
    if any(index[mode] != index["off"] for mode in runs):
        raise SystemExit("Source frame/time/case alignment failed.")
    report = {"frames_per_mode": len(index["off"]), "finite": True,
              "source_alignment": True, "alpha_matches_off": {}, "per_scene": {},
              "limits": "Procedural diagnostics; no perceptual, Skyrim, face or physical cadence acceptance."}
    for mode in runs:
        report["alpha_matches_off"][mode] = bool(np.array_equal(pixels[mode][..., 3], pixels["off"][..., 3]))
    for scene in range(4):
        selection = np.array([r[1] == scene for r in index["off"]])
        if not selection.any():
            continue
        report["per_scene"][str(scene)] = {}
        for mode in ("regular", "reset", "async"):
            a = pixels[mode][selection, ..., :3]
            off = pixels["off"][selection, ..., :3]
            regular = pixels["regular"][selection, ..., :3]
            report["per_scene"][str(scene)][mode] = {
                "rgb_mae_vs_off": float(np.abs(a - off).mean()),
                "rgb_mae_vs_regular": float(np.abs(a - regular).mean()),
                "near_unchanged_pixel_fraction_vs_off": float((np.max(np.abs(a - off), axis=-1) <= 1 / 1024).mean()),
                "max_abs_rgb_difference_vs_regular": float(np.abs(a - regular).max())}
    # Camera-only case: sample previous enhancement at analytic previous x.
    # Differences reflect sampling, model instability and async scheduling together.
    residuals = {mode: [] for mode in ("regular", "reset", "async")}
    camera = [i for i, r in enumerate(index["off"]) if r[1] == 0]
    for old, new in zip(camera, camera[1:]):
        shift = .22 * (index["off"][new][2] - index["off"][old][2]) * h
        x = np.arange(w, dtype=np.float32) + shift
        valid = x < w - 1
        x0 = np.floor(x[valid]).astype(np.int32)
        f = x[valid] - x0
        for mode in residuals:
            delta = pixels[mode][old, ..., :3] - pixels["off"][old, ..., :3]
            previous = delta[:, x0] * (1 - f[None, :, None]) + delta[:, x0 + 1] * f[None, :, None]
            # Basic indexing avoids NumPy advanced-axis permutation.
            current = (pixels[mode][new, ..., :3] - pixels["off"][new, ..., :3])[:, valid]
            residuals[mode].append(float(np.abs(current - previous).mean()))
    report["camera_warped_enhancement_change_mae"] = {mode: float(np.mean(v)) if v else None for mode, v in residuals.items()}
    (root / "pixel-analysis.json").write_text(json.dumps(report, indent=2))
    thumb_w = 320
    thumb_h = round(h * thumb_w / w)
    labels = ["NR off", "Regular NR (history)", "NR reset each frame", "Async NR", "Async - regular x8"]
    sheet = Image.new("RGB", (thumb_w * 5, (thumb_h + 44) * 4), "#191c22")
    draw = ImageDraw.Draw(sheet)
    names = ["Camera pan", "Moving occluder", "Thin geometry", "Exposure step"]
    for scene in range(4):
        candidates = [i for i, r in enumerate(index["off"]) if r[1] == scene]
        if not candidates:
            continue
        i = min(candidates, key=lambda i: abs(index["off"][i][2] - (scene * 1.5 + .9)))
        y = scene * (thumb_h + 44)
        for column, mode in enumerate(("off", "regular", "reset", "async")):
            image = preview(pixels[mode][i, ..., :3]).resize((thumb_w, thumb_h))
            sheet.paste(image, (column * thumb_w, y + 40))
        difference = pixels["async"][i, ..., :3] - pixels["regular"][i, ..., :3]
        difference = Image.fromarray(np.uint8(np.clip(.5 + difference * 8, 0, 1) * 255)).resize((thumb_w, thumb_h))
        sheet.paste(difference, (4 * thumb_w, y + 40))
        for column, label in enumerate(labels):
            draw.text((column * thumb_w + 8, y + 3), label, fill="white")
        draw.text((8, y + 21), f"{names[scene]} | source t={index['off'][i][2]:.2f}s", fill="#b7c4d8")
    sheet.save(root / "comparison.png")
    animation = []
    for i, (_, scene, t) in enumerate(index["off"]):
        panel = Image.new("RGB", (thumb_w * 3, thumb_h + 28), "#191c22")
        draw = ImageDraw.Draw(panel)
        for column, mode in enumerate(("off", "regular", "async")):
            panel.paste(preview(pixels[mode][i, ..., :3]).resize((thumb_w, thumb_h)), (column * thumb_w, 28))
            draw.text((column * thumb_w + 8, 8), f"{mode} | {names[scene]} {t:.2f}s", fill="white")
        animation.append(panel)
    animation[0].save(root / "motion-comparison.gif", save_all=True, append_images=animation[1:],
                      duration=round(1000 / identity["capture_hz"]), loop=0)
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
