"""lab/analyze/m4_analyze.py 계산 로직 테스트 (표준 라이브러리 unittest)."""

import datetime
import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "lab", "analyze"))
import m4_analyze as m4  # noqa: E402

NS = 1_000_000_000
BASE = 2_000_000 * NS          # 수신기 시계의 기준 시각
MONO0 = 5_000_000_000          # guest 단조 시계(us)의 시작값. 수신 시각과 무관한 값이어야 한다
LOAD_START = BASE + 20 * NS    # 부하 시작: 실행 시작 20초 뒤


def full_ratio(sec, spans):
    """spans: [(시작초, 끝초, 비율)]. 해당하지 않는 구간은 0."""
    for a, b, r in spans:
        if a <= sec < b:
            return r
    return 0.0


def write_run(d, **over):
    run = {"load_start_unix_ns": LOAD_START, "load_sec": 600, "cap_sec": 300, "observe_sec": None,
           "guest_clock_offset_ns": 0}
    run.update(over)
    with open(os.path.join(d, "run.json"), "w") as f:
        json.dump(run, f)


def write_samples(d, spans, secs=200, skip_seq=(), oom_at=None):
    """250 ms 주기 샘플. spans의 비율대로 PSI full 누적값을 늘린다. skip_seq의 seq는 수신기에서 누락된 것으로 쓴다."""
    total = 0
    with open(os.path.join(d, "samples.log"), "w") as f:
        for i in range(int(secs * 4) + 1):
            sec = i / 4
            total += int(250_000 * full_ratio(sec, spans)) if i else 0
            if i in skip_seq:
                continue
            oom = 1 if oom_at is not None and sec >= oom_at else 0
            f.write(f"{BASE + int(sec * NS)}\ttg1 kind=sample seq={i} mono_us={MONO0 + int(sec * 1e6)} "
                    f"psi_full_total={total} mem_avail_kb=100000 oom_kill={oom}\n")
        f.write(f"{BASE}\ttg1 kind=cg sseq=0 idx=0 name=x psi_full_total=999\n")  # kind=cg는 무시


def write_probes(d, name, fail_spans, secs=200, header="start_unix_ns\tlatency_ms\tok\n"):
    """1초 간격 프로브. fail_spans 안에서는 실패로 쓴다."""
    with open(os.path.join(d, name), "w") as f:
        f.write(header)
        for s in range(secs):
            ok = not any(a <= s < b for a, b in fail_spans)
            f.write(f"{BASE + s * NS}\t{5.0 if ok else 1000.0}\t{1 if ok else 0}\n")


def iso(guest_epoch_ns):
    dt = datetime.datetime(1970, 1, 1) + datetime.timedelta(microseconds=guest_epoch_ns // 1000)
    return dt.strftime("%Y-%m-%dT%H:%M:%S.") + f"{dt.microsecond // 1000:03d}Z"


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.d = self.tmp.name

    def tearDown(self):
        self.tmp.cleanup()


class TestThrashAndResolve(Base):
    def test_t0_and_resolve(self):
        # 부하 시작(20초) 후 40~70초에 50% 스래싱. 이후 calm.
        write_run(self.d)
        write_samples(self.d, [(40, 70, 0.5)])
        r = m4.evaluate(self.d, final=True)
        self.assertEqual(r["status"], "resolved")
        self.assertAlmostEqual(r["t0_s"], 20.0, delta=1.0)        # 40초 = 부하 시작 후 20초
        # 70초에 끝나고 10초 calm이 확인되는 80초 = T0(40) 뒤 40초
        self.assertAlmostEqual(r["resolve_time_s"], 40.0, delta=1.5)
        self.assertGreaterEqual(r["resolve_time_s"], 10.0)

    def test_short_burst_under_3s_is_not_t0(self):
        # 1초 구간 경계에 걸리면 2초 급증도 3개 구간에 퍼질 수 있어, 확실히 짧은 1초 급증으로 확인한다.
        write_run(self.d, observe_sec=100)
        write_samples(self.d, [(50, 51, 0.9)], secs=200)
        r = m4.evaluate(self.d, final=True)
        self.assertIsNone(r["t0_s"])
        self.assertEqual(r["status"], "no_thrash")

    def test_below_threshold_is_not_t0(self):
        write_run(self.d, observe_sec=100)
        write_samples(self.d, [(30, 150, 0.09)], secs=200)
        self.assertIsNone(m4.evaluate(self.d, final=True)["t0_s"])

    def test_unresolved_when_thrash_continues_past_cap(self):
        write_run(self.d, cap_sec=60)
        write_samples(self.d, [(40, 190, 0.5)], secs=190)
        r = m4.evaluate(self.d, final=True)
        self.assertEqual(r["status"], "unresolved")
        self.assertIsNone(r["resolve_time_s"])

    def test_running_until_cap_then_unresolved_live(self):
        write_run(self.d, cap_sec=100)
        write_samples(self.d, [(40, 190, 0.5)], secs=120)
        self.assertEqual(m4.evaluate(self.d, now_ns=BASE + 100 * NS)["status"], "running")
        self.assertEqual(m4.evaluate(self.d, now_ns=BASE + 150 * NS)["status"], "unresolved")

    def test_resolution_after_load_end_is_truncated(self):
        # 부하가 60초에 자체 종료되도록 설정(load_sec=40). 스래싱 중 끝나므로 해소 시간을 주장하지 않는다.
        write_run(self.d, load_sec=40, cap_sec=300)
        write_samples(self.d, [(40, 150, 0.5)], secs=150)
        r = m4.evaluate(self.d, now_ns=BASE + 150 * NS)
        self.assertEqual(r["status"], "truncated")
        self.assertIsNone(r["resolve_time_s"])

    def test_calm_before_t0_is_not_resolution(self):
        # 기준 구간의 calm을 해소로 잡지 않는다: T0 이후에 시작한 구간만 쓴다.
        write_run(self.d)
        write_samples(self.d, [(100, 130, 0.5)])
        r = m4.evaluate(self.d, final=True)
        self.assertAlmostEqual(r["t0_s"], 80.0, delta=1.0)
        self.assertGreater(r["resolve_time_s"], 30.0)

    def test_observe_window_for_no_thrash(self):
        write_run(self.d, observe_sec=120)
        write_samples(self.d, [], secs=200)
        self.assertEqual(m4.evaluate(self.d, now_ns=LOAD_START + 60 * NS)["status"], "running")
        self.assertEqual(m4.evaluate(self.d, now_ns=LOAD_START + 130 * NS)["status"], "no_thrash")


class TestPeaks(Base):
    def test_peaks_after_load_start_only(self):
        # 부하 시작(20초) 전의 80% 구간은 세지 않고, 이후 40~44초의 30%를 최대로 본다.
        write_run(self.d, observe_sec=100)
        write_samples(self.d, [(5, 15, 0.8), (40, 44, 0.3)], secs=200)
        r = m4.evaluate(self.d, final=True)
        self.assertAlmostEqual(r["full_peak_1s"], 0.3, delta=0.05)
        self.assertLess(r["full_peak_10s_avg"], r["full_peak_1s"])
        self.assertAlmostEqual(r["mem_avail_min_mib"], 100000 / 1024, delta=0.1)

    def test_no_peaks_without_data(self):
        write_run(self.d)
        r = m4.evaluate(self.d, final=True)
        self.assertIsNone(r["full_peak_1s"])


class TestInvalidIntervals(Base):
    def test_seq_gap_breaks_the_t0_run(self):
        # 40~70초 스래싱 중 한가운데 seq를 하나 빼면 그 1초 구간이 무효가 되어 3초 연속이 성립하지 않는다.
        write_run(self.d, observe_sec=100)
        write_samples(self.d, [(40, 44, 0.9)], secs=200, skip_seq=set(range(4 * 41 + 1, 4 * 41 + 3)))
        r = m4.evaluate(self.d, final=True)
        self.assertGreater(r["invalid_buckets"], 0)
        self.assertIsNone(r["t0_s"])

    def test_seq_gap_breaks_the_resolve_run(self):
        write_run(self.d)
        gap = set(range(4 * 75, 4 * 75 + 2))
        # 40~70 스래싱 뒤 calm. calm 10초 연속 도중(75초 부근)에 누락이 있으면 그 지점에서 연속이 끊겨 해소가 늦어진다.
        write_samples(self.d, [(40, 70, 0.5)], skip_seq=gap)
        r = m4.evaluate(self.d, final=True)
        self.assertGreater(r["resolve_time_s"], 40.0 + 4.0)


class TestDaemonLog(Base):
    def log(self, lines):
        with open(os.path.join(self.d, "daemon.log"), "w") as f:
            f.write("\n".join(lines) + "\n")

    def test_first_decision_and_kill(self):
        write_run(self.d, guest_clock_offset_ns=3 * NS)  # guest 시계가 host보다 3초 빠르다
        write_samples(self.d, [(40, 70, 0.5)])
        guest_t = BASE + 55 * NS + 3 * NS
        self.log([
            f"{iso(guest_t - 60 * NS)} info start mode=kill",
            f"{iso(guest_t)} warn judge kill cand=tg-load.service dry_run=0 result=0 path=io sys_avg_x100=952 io_avg_x100=9002 avail_x100=0",
            f"{iso(guest_t)} info judge cand name=tg-load.service max_bytes=1 growth_bytes=1 ready=1 eligible=1 growing=1 grace=0 protected=0",
            f"{iso(guest_t)} info judge cand name=tg-canary.service max_bytes=1 growth_bytes=0 ready=1 eligible=0 growing=0 grace=0 protected=1",
        ])
        r = m4.evaluate(self.d, final=True)
        self.assertAlmostEqual(r["t_detect_s"], 35.0, delta=0.01)   # 55초 - 부하 시작(20초)
        self.assertAlmostEqual(r["t_kill_s"], 35.0, delta=0.01)
        self.assertEqual(r["killed_roles"], ["load"])
        self.assertEqual(r["protected_seen"], ["tg-canary.service"])
        self.assertFalse(r["e_killed"])
        self.assertEqual(r["first_actor"], "daemon")
        self.assertAlmostEqual(r["detect_delay_s"], 35.0 - r["t0_s"], delta=0.01)
        self.assertEqual(r["decisions"][0]["path"], "io")

    def test_dry_run_decision_is_not_a_kill(self):
        write_run(self.d)
        write_samples(self.d, [(40, 70, 0.5)])
        self.log([f"{iso(BASE + 50 * NS)} warn judge kill cand=tg-load.service dry_run=1 result=0 path=memory"])
        r = m4.evaluate(self.d, final=True)
        self.assertIsNotNone(r["t_detect_s"])
        self.assertIsNone(r["t_kill_s"])
        self.assertEqual(r["killed_roles"], [])
        self.assertTrue(r["daemon_decision_before_kernel"])

    def test_decision_without_thrash_is_false_positive(self):
        write_run(self.d, observe_sec=100)
        write_samples(self.d, [], secs=200)
        self.log([f"{iso(BASE + 60 * NS)} warn judge kill cand=tg-load.service dry_run=1 result=0 path=memory"])
        r = m4.evaluate(self.d, final=True)
        self.assertTrue(r["decision_without_thrash"])

    def test_e_killed_by_daemon(self):
        write_run(self.d)
        write_samples(self.d, [(40, 70, 0.5)])
        self.log([f"{iso(BASE + 50 * NS)} warn judge kill cand=tg-canary.service dry_run=0 result=0 path=memory"])
        self.assertTrue(m4.evaluate(self.d, final=True)["e_killed"])

    def test_kernel_oom_before_daemon(self):
        write_run(self.d)
        write_samples(self.d, [(40, 70, 0.5)], oom_at=45)
        self.log([f"{iso(BASE + 60 * NS)} warn judge kill cand=tg-load.service dry_run=0 result=0 path=memory"])
        r = m4.evaluate(self.d, final=True)
        self.assertEqual(r["kernel_oom_kills"], 1)
        self.assertEqual(r["first_actor"], "kernel")
        self.assertFalse(r["daemon_decision_before_kernel"])


class TestEProbe(Base):
    def test_gap_is_clipped_to_the_window(self):
        write_run(self.d)
        write_samples(self.d, [(40, 70, 0.5)])
        # 카나리가 50~58초(8초) 응답 없음. 창은 T0(40초)부터 해소(80초)까지.
        write_probes(self.d, "canary_probe.tsv", [(50, 58)])
        r = m4.evaluate(self.d, final=True)
        self.assertAlmostEqual(r["e_canary"]["max_gap_s"], 9.0, delta=0.6)  # 마지막 성공(49초)~다음 성공(58초)
        self.assertLess(r["e_canary"]["ok_rate"], 1.0)

    def test_no_failure_outside_window_is_ignored(self):
        write_run(self.d)
        write_samples(self.d, [(40, 70, 0.5)])
        write_probes(self.d, "canary_probe.tsv", [(0, 30)])  # 스래싱 전에 응답이 없던 것은 세지 않는다
        r = m4.evaluate(self.d, final=True)
        self.assertLessEqual(r["e_canary"]["max_gap_s"], 1.5)

    def test_e_state_failed(self):
        write_run(self.d)
        write_samples(self.d, [(40, 70, 0.5)])
        with open(os.path.join(self.d, "e_state.txt"), "w") as f:
            f.write("failed\n")
        r = m4.evaluate(self.d, final=True)
        self.assertFalse(r["e_alive_at_end"])
        self.assertTrue(r["e_killed"])


class TestMissingInput(Base):
    def test_empty_directory(self):
        r = m4.evaluate(self.d, final=True)
        self.assertEqual(r["status"], "incomplete")
        self.assertTrue(r["notes"])

    def test_only_run_json(self):
        write_run(self.d)
        self.assertEqual(m4.evaluate(self.d, now_ns=LOAD_START + 10 * NS)["status"], "running")


if __name__ == "__main__":
    unittest.main()
