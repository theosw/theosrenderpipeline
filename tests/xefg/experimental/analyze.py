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
p.add_argument('--multiplier', type=int, choices=range(3,7), default=4)
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
                        'failedNativePresent': sum(r['result'] < 0 for r in rows),
                        'nonSuccessfulNativePresent': sum(r['result'] != 0 for r in rows)}
    counts = sorted({r['count'] for r in rows})
    complete = 0
    for start, row in enumerate(rows):
        if row['index'] != 1:
            continue
        burst = rows[start:start + row['count']]
        if (len(burst) == row['count'] and
                all(r['context'] == row['context'] and r['burst'] == row['burst'] and
                    r['count'] == row['count'] for r in burst) and
                [r['index'] for r in burst] == list(range(1, row['count'] + 1))):
            complete += 1
    result['pacing']['observedGeneratedCounts'] = counts
    result['pacing']['completeBursts'] = complete
    if a.require_even_pacing:
        assert counts == [a.multiplier-1] and complete >= 20, 'selected complete generated bursts missing'
        assert result['pacing']['nonSuccessfulNativePresent'] == 0, 'native Present did not return S_OK'
        d = result['pacing']['withinBurstDelivery']
        assert d and d['samples'] >= 100 and d['p25_ms'] >= d['p95_ms'] * .5, 'persistent compressed delivery slots'
if a.pixels:
    images = []
    # This fixture captures 640x360 first; central crop excludes HUD and corner tags.
    for path in sorted(a.pixels.glob('frame-*.ppm'), key=lambda p: int(p.stem.split('-')[1])):
        with path.open('rb') as f:
            assert f.readline() == b'P6\n', 'PPM format'
            width, height = map(int, f.readline().split())
            assert f.readline() == b'255\n', '8-bit RGB'
            data = f.read()
        assert (width, height) == (640, 360) and len(data) == width * height * 3, 'capture extent'
        crop = b''.join(data[(y * width + 192) * 3:(y * width + 448) * 3] for y in range(128, 232))
        images.append({'file': path.name, 'index': int(path.stem.rsplit('-', 1)[1]),
                       'central_sha256': hashlib.sha256(crop).hexdigest()})
    count = a.multiplier - 1
    assert len(images) == 3 * count, 'three complete captured bursts required'
    for start in range(0, len(images), count):
        burst = images[start:start + count]
        assert [i['index'] for i in burst] == list(range(1, count+1)), 'ordered complete burst'
        assert len({i['central_sha256'] for i in burst}) == count, 'duplicate generated scene pixels'
    result['pixels'] = {'crop': [192, 128, 448, 232], 'distinctWithinThreeBursts': True,
                        'visualQualityAccepted': False, 'images': images}
a.output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
print(json.dumps(result, indent=2))
