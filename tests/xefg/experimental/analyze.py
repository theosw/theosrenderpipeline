"""Analyze fixture evidence. Delivery timestamps do not measure physical scanout.

python analyze.py --events run.csv [--pixels capture-directory] --output result.json
Capture runs perturb timing and must never be used as pacing benchmarks.
"""
import argparse
import csv
import hashlib
import json
from pathlib import Path
from statistics import median

p = argparse.ArgumentParser()
p.add_argument('--events', type=Path)
p.add_argument('--pixels', type=Path)
p.add_argument('--require-even-pacing', action='store_true', help='Fixture check: reject persistent compressed delivery slots')
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
result = {'physicalCadenceVerified': False}
if a.events:
    with a.events.open(newline='') as f:
        rows = [{k: int(v) for k, v in r.items()} for r in csv.DictReader(f)]
    gaps, steps = [], []
    for previous, current in zip(rows, rows[1:]):
        if (current['context'] == previous['context'] and
                current['burst'] == previous['burst'] and
                current['count'] == previous['count'] and
                current['index'] == previous['index'] + 1):
            gaps.append((current['pacedQpc'] - previous['pacedQpc']) * 1000 / current['frequency'])
            if (current['deadlineNs'] and previous['deadlineNs'] and previous['scheduled'] and
                    (current['scheduled'] or current['finalFrame'])):
                steps.append((current['deadlineNs'] - previous['deadlineNs']) / 1e6)
    def distribution(values):
        ordered = sorted(values)
        return {'samples': len(values), 'p25_ms': ordered[int((len(ordered) - 1) * .25)], 'p50_ms': median(values),
                'p95_ms': ordered[int((len(ordered) - 1) * .95)], 'max_ms': max(values)} if values else None
    result['pacing'] = {'events': len(rows), 'withinBurstDelivery': distribution(gaps),
                        'deadlineStep': distribution(steps),
                        'failedNativePresent': sum(r['result'] < 0 for r in rows)}
    if a.require_even_pacing:
        d = result['pacing']['withinBurstDelivery']
        assert d and d['samples'] >= 100 and d['p25_ms'] >= d['p95_ms'] * .5, 'persistent compressed delivery slots'
if a.pixels:
    images = []
    # This fixture captures 640x360 first; central crop excludes HUD and corner tags.
    for path in sorted(a.pixels.glob('frame-*.ppm')):
        with path.open('rb') as f:
            assert f.readline() == b'P6\n', 'PPM format'
            width, height = map(int, f.readline().split())
            assert f.readline() == b'255\n', '8-bit RGB'
            data = f.read()
        assert (width, height) == (640, 360) and len(data) == width * height * 3, 'capture extent'
        crop = b''.join(data[(y * width + 192) * 3:(y * width + 448) * 3] for y in range(128, 232))
        images.append({'file': path.name, 'index': int(path.stem.rsplit('-', 1)[1]),
                       'central_sha256': hashlib.sha256(crop).hexdigest()})
    assert len(images) == 9, 'three captured x4 bursts required'
    for start in range(0, 9, 3):
        burst = images[start:start + 3]
        assert [i['index'] for i in burst] == [1, 2, 3], 'ordered complete burst'
        assert len({i['central_sha256'] for i in burst}) == 3, 'duplicate generated scene pixels'
    result['pixels'] = {'crop': [192, 128, 448, 232], 'distinctWithinThreeBursts': True,
                        'visualQualityAccepted': False, 'images': images}
a.output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
print(json.dumps(result, indent=2))
