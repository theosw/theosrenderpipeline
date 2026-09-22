import csv
import tempfile
import unittest
import struct
from pathlib import Path
from analyze import summarize, equal_files, metadata
from candidate import final_pair


class AnalysisTests(unittest.TestCase):
    def test_fusion_filter_rejects_in_place_output(self):
        up = dict(name="k_upscale", params_hex=struct.pack("<QQ6I", 0x1000, 0x2000, 4, 8, 8, 16, 8, 16).hex())
        add = dict(name="k_element_wise", params_hex=struct.pack("<QQQ7I", 0x2000, 0x3000, 0x4000, 16, 8, 32, 16, 8, 16, 8).hex())
        self.assertIsNotNone(final_pair(up, add))
        add["params_hex"] = struct.pack("<QQQ7I", 0x2000, 0x3000, 0x1000, 16, 8, 32, 16, 8, 16, 8).hex()
        self.assertIsNone(final_pair(up, add))

    def test_group_cost_does_not_multiply_x4_twice_or_count_batched_launches_twice(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp)
            (p / "complete.txt").write_text("complete")
            (p / "identity.txt").write_text("dimensions=128x128 frames=6 multiplier=4\nfixture=move mode=profile\n")
            (p / "timings.csv").write_text("group,generated,evaluate_gpu_ms,launch_queries\n" +
                "".join(f"{f},{j},0.5,2\n" for f in range(6) for j in range(1, 4)))
            with (p / "launches.tsv").open("w", newline="") as file:
                writer = csv.DictWriter(file, fieldnames=["group", "generated", "call", "name", "call_gpu_ms"], delimiter="\t")
                writer.writeheader()
                for f in range(6):
                    for j in range(1, 4):
                        for name in ("A", "B"):
                            writer.writerow(dict(group=f, generated=j, call=0, name=name, call_gpu_ms="0.1"))
            result = summarize(p)
            self.assertEqual(result["fg_group_median_ms"], 1.5)
            self.assertAlmostEqual(result["profiled_call_mean_ms_per_group"]["A + B"], .3)

    def test_incomplete_runs_are_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaisesRegex(ValueError, "Incomplete"):
                metadata(Path(tmp))

    def test_byte_comparison_detects_late_difference(self):
        with tempfile.TemporaryDirectory() as tmp:
            a, b = Path(tmp) / "a", Path(tmp) / "b"
            a.write_bytes(b"a" * (1024 * 1024) + b"b")
            b.write_bytes(b"a" * (1024 * 1024) + b"c")
            self.assertFalse(equal_files(a, b))


if __name__ == "__main__":
    unittest.main()
