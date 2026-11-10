"""lab/analyze/verdict.py 판정 로직 테스트 (표준 라이브러리 unittest)."""

import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "lab", "analyze"))
import verdict  # noqa: E402

NS = 1_000_000_000
T0 = 1_000_000 * NS


def write_probes(d, rows):
    """rows: (시작 초, 지연 ms, 성공 여부)"""
    with open(os.path.join(d, "ssh_probe.tsv"), "w") as f:
        f.write("start_unix_ns\tlatency_ms\tok\trc\n")
        for sec, lat, ok in rows:
            f.write(f"{T0 + int(sec * NS)}\t{lat}\t{1 if ok else 0}\t{0 if ok else -1}\n")


def write_samples(d, rows):
    """rows: (수신 초, mem_avail_kb, oom_kill)"""
    with open(os.path.join(d, "samples.log"), "w") as f:
        for i, (sec, mem, oom) in enumerate(rows):
            f.write(f"{T0 + int(sec * NS)}\ttg1 kind=sample seq={i} mem_avail_kb={mem} oom_kill={oom}\n")
        # kind=cg 줄은 무시되어야 한다
        f.write(f"{T0}\ttg1 kind=cg sseq=0 idx=0 name=x ev_oom_kill=9\n")


def ok_probes(until, step=1):
    return [(s, 200, True) for s in range(0, until, step)]


class VerdictTest(unittest.TestCase):
    def run_case(self, probes, samples, now_s=None, final=False):
        with tempfile.TemporaryDirectory() as d:
            write_probes(d, probes)
            write_samples(d, samples)
            now = None if now_s is None else T0 + now_s * NS
            return verdict.evaluate(d, now, final)

    def test_normal_final(self):
        res = self.run_case(ok_probes(100), [(s, 400_000, 0) for s in range(100)], final=True)
        self.assertEqual(res["verdict"], "normal")
        self.assertLess(res["ssh_max_gap_s"], 3)

    def test_running_when_undecided(self):
        res = self.run_case(ok_probes(100), [(s, 400_000, 0) for s in range(100)])
        self.assertEqual(res["verdict"], "running")

    def test_livelock(self):
        # 0~99초는 정상, 이후 650초 동안 성공 없음. 그 구간에서 MemAvailable 10 MiB.
        probes = ok_probes(100) + [(s, 5000, False) for s in range(100, 750)]
        samples = [(s, 400_000, 0) for s in range(100)] + [(s, 10_000, 0) for s in range(100, 750)]
        res = self.run_case(probes, samples, now_s=750)
        self.assertEqual(res["verdict"], "livelock")
        self.assertGreaterEqual(res["ssh_max_gap_s"], 600)
        self.assertEqual(res["mem_avail_min_mib_in_gap"], 9.8)

    def test_gap_just_under_threshold_is_not_livelock(self):
        probes = ok_probes(100) + [(s, 5000, False) for s in range(100, 690)]
        samples = [(s, 10_000, 0) for s in range(700)]
        res = self.run_case(probes, samples, now_s=690, final=True)
        self.assertEqual(res["verdict"], "normal")
        self.assertLess(res["ssh_max_gap_s"], 600)

    def test_single_success_resets_the_gap(self):
        # 중간에 한 번 성공하면 무응답 구간이 끊긴다(엄격한 기준).
        probes = ok_probes(100) + [(s, 5000, False) for s in range(100, 500)] + [(500, 900, True)] + \
                 [(s, 5000, False) for s in range(501, 900)]
        samples = [(s, 10_000, 0) for s in range(900)]
        res = self.run_case(probes, samples, now_s=900, final=True)
        self.assertEqual(res["verdict"], "normal")

    def test_memory_not_low_is_not_livelock(self):
        probes = ok_probes(100) + [(s, 5000, False) for s in range(100, 750)]
        samples = [(s, 300_000, 0) for s in range(750)]
        res = self.run_case(probes, samples, now_s=750, final=True)
        self.assertEqual(res["verdict"], "normal")
        self.assertGreaterEqual(res["ssh_max_gap_s"], 600)

    def test_oom_kill_wins(self):
        probes = ok_probes(100) + [(s, 5000, False) for s in range(100, 750)]
        samples = [(s, 10_000, 0 if s < 300 else 1) for s in range(750)]
        res = self.run_case(probes, samples, now_s=750)
        self.assertEqual(res["verdict"], "oom_kill")
        self.assertEqual(res["oom_kill_delta"], 1)

    def test_no_success_at_all(self):
        res = self.run_case([(s, 5000, False) for s in range(700)],
                            [(s, 10_000, 0) for s in range(700)], now_s=700)
        self.assertEqual(res["verdict"], "livelock")

    def test_truncated_lines_are_ignored(self):
        with tempfile.TemporaryDirectory() as d:
            write_probes(d, ok_probes(5))
            with open(os.path.join(d, "ssh_probe.tsv"), "a") as f:
                f.write("1000")  # 쓰는 도중 잘린 줄
            with open(os.path.join(d, "samples.log"), "w") as f:
                f.write(f"{T0}\ttg1 kind=sample seq=0 mem_avail_kb=5 oom_kill=0\n")
                f.write(f"{T0}\ttg1 kind=sample seq=1 mem_av")
            res = verdict.evaluate(d, final=True)
            self.assertEqual(res["samples"], 1)
            self.assertEqual(res["ssh_probes"], 5)

    def test_missing_files(self):
        with tempfile.TemporaryDirectory() as d:
            res = verdict.evaluate(d, final=True)
            self.assertEqual(res["verdict"], "normal")
            self.assertEqual(res["samples"], 0)


if __name__ == "__main__":
    unittest.main()
