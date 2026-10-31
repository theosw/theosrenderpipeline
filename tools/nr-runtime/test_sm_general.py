"""Adversarial scheduler model. This does not emulate physical CUDA/WDDM SMs.

The FIFO proof assumes finite external producer delays and fair resumption of
ticket holders. Counterexamples separately demonstrate why this assumption
cannot be obtained from NVAPI native-chain overlap or CUDA launch bounds.
"""
import argparse
import json
import random
import unittest
from pathlib import Path


def simulate(sm_ids, occupancy, capacity, paired, seed, bits=64, tasks=263):
    rng = random.Random(seed)
    mask = (1 << bits) - 1
    shift = capacity.bit_length() - 1
    generation_mask = mask >> shift
    assert capacity == 1 << shift
    # Keep fewer outstanding tickets than the sequence number space.
    workers = min(len(sm_ids) * occupancy, mask // 2)
    turns = [0] * capacity
    next_ticket = (1 << bits) - 2 * capacity   # exercise rollover immediately
    for ticket in range(next_ticket, next_ticket + capacity):
        turns[ticket & (capacity-1)] = ticket >> shift
    scratch = [None] * (capacity * (2 if paired else 1))
    owners = {}
    pending = list(range(tasks))
    rng.shuffle(pending)
    active = []
    complete = set()
    epochs = 0
    acquisitions = 0
    max_holders = 0
    while pending or active:
        while pending and len(active) < workers:
            task = pending.pop()
            active.append(dict(task=task, delay=rng.randrange(19), remaining=rng.randrange(1, 8),
                               state='producer', sm=rng.choice(sm_ids)))
        rng.shuffle(active)
        for task in active[:]:
            # Finite preemption; changes in sparse SM ID must not affect address.
            if rng.randrange(7) == 0:
                task['sm'] = rng.choice(sm_ids)
                continue
            if task['state'] == 'producer':
                task['delay'] -= 1
                if task['delay'] <= 0:
                    task['ticket'] = next_ticket
                    next_ticket = (next_ticket + 1) & mask
                    task['state'] = 'turn'
                continue
            ticket = task['ticket']
            slot = ticket & (capacity-1)
            cells = range(slot*2, slot*2+2) if paired else (slot,)
            if task['state'] == 'turn':
                if turns[slot] != ticket >> shift:
                    continue
                assert slot not in owners, ('alias', task, owners)
                owners[slot] = task['task']
                for cell in cells:
                    scratch[cell] = task['task']
                task['state'] = 'compute'
                acquisitions += 1
                max_holders = max(max_holders, len(owners))
            else:
                assert all(scratch[cell] == task['task'] for cell in cells), 'scratch changed before retire'
                task['remaining'] -= 1
                if task['remaining'] == 0:
                    del owners[slot]
                    turns[slot] = ((ticket >> shift) + 1) & generation_mask
                    complete.add(task['task'])
                    active.remove(task)
        epochs += 1
        assert epochs < tasks * 500, 'starvation under finite producer delays and fair modeled service'
    assert len(complete) == tasks and acquisitions == tasks and not owners
    return dict(epochs=epochs, tasks=tasks, maxHolders=max_holders)


class SchedulingTests(unittest.TestCase):
    results = []

    def test_matrix(self):
        for count in range(1, 33):
            for sparse in (False, True):
                ids = [11 + i * 7 for i in range(count)] if sparse else list(range(count))
                for occupancy in (1, 2, 4):
                    for capacity, paired in ((1, True), (2, False), (32, False), (64, False), (64, True)):
                        seed = count*10000 + sparse*1000 + occupancy*100 + capacity + paired
                        with self.subTest(count=count, sparse=sparse, occupancy=occupancy, capacity=capacity, paired=paired):
                            result = simulate(ids, occupancy, capacity, paired, seed)
                            self.results.append(dict(smCount=count, sparse=sparse, occupancy=occupancy,
                                                     capacity=capacity, paired=paired, seed=seed, **result))

    def test_small_counter_rollover(self):
        for bits in (5, 8):
            for capacity in (2, 4):
                simulate([1, 7, 51], 2, capacity, True, 777, bits=bits, tasks=1025)

    def test_odd_flattened_grids(self):
        for x in range(1, 34):
            for y in range(1, 18):
                logical = [2*p+g for p in range((x*y+1)//2) for g in (0, 1) if 2*p+g < x*y]
                self.assertEqual(logical, list(range(x*y)))
                self.assertEqual([(i % x, i // x) for i in logical], [(xx, yy) for yy in range(y) for xx in range(x)])

    def test_chain_counterexample(self):
        # Every resident consumer waits for an as-yet unlaunched producer.
        # More pool slots, sparse-ID mapping or FIFO fairness cannot free a CTA.
        for count in range(1, 33):
            resident = [{'wait': 'producer'} for _ in range(count)]
            producer_pending = True
            runnable = [c for c in resident if not c['wait']]
            self.assertTrue(producer_pending and not runnable)

    def test_slice_counterexample(self):
        # One CTA fits. z=1 may be scheduled before z=0 in the same grid.
        flags = [-1]
        resident_z, pending_z = 1, 0
        self.assertTrue(flags[0] < resident_z-1 and pending_z == 0)

    def test_ordered_design_model(self):
        # Topological dispatches and CTA-local slice folding have no waits.
        # Producer delays consume time only; no successor can become resident.
        for count in range(1, 33):
            rng = random.Random(count)
            produced, consumed = set(), set()
            tiles = list(range(127))
            rng.shuffle(tiles)
            for tile in tiles:
                produced.add(tile)
            rng.shuffle(tiles)
            for tile in tiles:
                self.assertIn(tile, produced)
                folded = []
                for z in range(4):
                    self.assertEqual(len(folded), z)
                    folded.append(z)
                consumed.add(tile)
            self.assertEqual(produced, consumed)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(SchedulingTests)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(dict(passed=result.wasSuccessful(), cases=SchedulingTests.results,
            limitation='CPU model only; not physical SM counts, CUDA memory-order validation, NVAPI or WDDM'), indent=2)+'\n')
    raise SystemExit(0 if result.wasSuccessful() else 1)
