"""lab/analyze/s2_summary.py 판정 로직 테스트 (표준 라이브러리 unittest)."""

import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "lab", "analyze"))
import s2_summary  # noqa: E402

NS = 1_000_000_000
T0 = 1_000_000 * NS


def write_tsv(d, name, rows, with_rc=False):
    """rows: (시작 초, 지연 ms, 성공 여부)"""
    with open(os.path.join(d, name), "w") as f:
        f.write("start_unix_ns\tlatency_ms\tok" + ("\trc" if with_rc else "") + "\n")
        for sec, lat, ok in rows:
            f.write(f"{T0 + int(sec * NS)}\t{lat}\t{1 if ok else 0}" + ("\t0" if with_rc else "") + "\n")


def write_samples(d, secs, mem_kb=400_000, oom_at=None, psi_full_per_s_us=0):
    with open(os.path.join(d, "samples.log"), "w") as f:
        for s in range(secs):
            oom = 1 if oom_at is not None and s >= oom_at else 0
            f.write(f"{T0 + s * NS}\ttg1 kind=sample seq={s} mem_avail_kb={mem_kb} oom_kill={oom}\n")
            f.write(f"{T0 + s * NS}\ttg1 kind=cg sseq={s} idx=0 name=workload-container.slice "
                    f"psi_full_total={s * psi_full_per_s_us} ev_max={s // 10} ev_oom_kill=0\n")
            # 컨테이너가 아닌 cgroup은 무시되어야 한다
            f.write(f"{T0 + s * NS}\ttg1 kind=cg sseq={s} idx=1 name=other.slice "
                    f"psi_full_total=999999999 ev_max=999 ev_oom_kill=9\n")


def ok_probes(until):
    return [(s, 20, True) for s in range(until)]


class S2SummaryTest(unittest.TestCase):
    def run_case(self, canary, secs, now_s, final=False, **kw):
        with tempfile.TemporaryDirectory() as d:
            write_tsv(d, "canary_probe.tsv", canary)
            write_tsv(d, "ssh_probe.tsv", ok_probes(secs), with_rc=True)
            write_samples(d, secs, **kw)
            return s2_summary.evaluate(d, T0 + now_s * NS, final)

    def test_normal_final(self):
        res = self.run_case(ok_probes(100), 100, 100, final=True)
        self.assertEqual(res["verdict"], "normal")

    def test_running_when_undecided(self):
        res = self.run_case(ok_probes(100), 100, 100)
        self.assertEqual(res["verdict"], "running")

    def test_container_stall_with_healthy_system(self):
        # 카나리만 650초 막히고 SSH와 시스템 메모리는 멀쩡하다. 컨테이너 PSI full은 80%.
        canary = ok_probes(100) + [(s, 1000, False) for s in range(100, 750)]
        res = self.run_case(canary, 750, 750, psi_full_per_s_us=800_000)
        self.assertEqual(res["verdict"], "container_stall")
        self.assertGreaterEqual(res["canary_max_gap_s"], 600)
        self.assertLess(res["ssh_max_gap_s"], 2)  # 시스템(SSH)은 멀쩡하다
        self.assertAlmostEqual(res["cg_psi_full_ratio_in_gap"], 0.8, places=2)
        self.assertEqual(res["mem_avail_min_mib"], 390.6)

    def test_gap_just_under_threshold(self):
        canary = ok_probes(100) + [(s, 1000, False) for s in range(100, 690)]
        res = self.run_case(canary, 700, 690, final=True)
        self.assertEqual(res["verdict"], "normal")

    def test_oom_kill_wins(self):
        canary = ok_probes(100) + [(s, 1000, False) for s in range(100, 750)]
        res = self.run_case(canary, 750, 750, oom_at=300)
        self.assertEqual(res["verdict"], "oom_kill")
        self.assertEqual(res["oom_kill_delta"], 1)

    def test_other_cgroups_are_ignored(self):
        res = self.run_case(ok_probes(100), 100, 100, final=True)
        self.assertEqual(res["cg_ev_oom_kill_delta"], 0)
        self.assertLess(res["cg_ev_max_delta"], 100)

    def test_missing_files(self):
        with tempfile.TemporaryDirectory() as d:
            res = s2_summary.evaluate(d, final=True)
            self.assertEqual(res["verdict"], "normal")


if __name__ == "__main__":
    unittest.main()
