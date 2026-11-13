"""lab/analyze/m4_analyze.py 계산 로직 테스트 (표준 라이브러리 unittest)."""

import datetime
import json
import os
import random
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "lab", "analyze"))
import m4_analyze as m4  # noqa: E402

NS = 1_000_000_000
BASE = 2_000_000 * NS          # 수신기 시계의 기준 시각
MONO0 = 5_000_000_000          # guest 단조 시계(us)의 시작값. 수신 시각과 무관한 값이어야 한다
LOAD_START = BASE + 20 * NS    # 부하 시작: 실행 시작 20초 뒤
OK_MS = 200.0                  # 성공한 프로브의 지연
FAIL_MS = 1000.0               # 실패한 프로브의 지연


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


def write_probes(d, name, fail_spans, secs=200, shuffle=False):
    """1초 간격 프로브. fail_spans(실행 시작 기준 초) 안에서는 실패로 쓴다. ssh_probe.tsv와 canary_probe.tsv 공용."""
    rows = []
    for s in range(secs):
        ok = not any(a <= s < b for a, b in fail_spans)
        rows.append(f"{BASE + s * NS}\t{OK_MS if ok else FAIL_MS}\t{1 if ok else 0}")
    if shuffle:
        random.Random(1).shuffle(rows)  # 완료 순서로 기록되므로 시작 시각 순이 아닐 수 있다
    with open(os.path.join(d, name), "w") as f:
        f.write("start_unix_ns\tlatency_ms\tok\n")
        f.write("\n".join(rows) + "\n")


def write_ssh(d, fail_spans, **kw):
    write_probes(d, "ssh_probe.tsv", fail_spans, **kw)


def iso(guest_epoch_ns):
    dt = datetime.datetime(1970, 1, 1) + datetime.timedelta(microseconds=guest_epoch_ns // 1000)
    return dt.strftime("%Y-%m-%dT%H:%M:%S.") + f"{dt.microsecond // 1000:03d}Z"


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.d = self.tmp.name

    def tearDown(self):
        self.tmp.cleanup()


class TestSshOnsetAndResolve(Base):
    def test_t0_and_resolve(self):
        # 부하 시작(20초) 후 40~70초에 SSH 불가. T0는 40초 = 부하 시작 후 20초.
        write_run(self.d)
        write_ssh(self.d, [(40, 70)])
        r = m4.evaluate(self.d, final=True)
        self.assertEqual(r["status"], "resolved")
        self.assertAlmostEqual(r["t0_s"], 20.0, delta=0.1)
        # 70초부터 5번 연속 성공, 5번째(74초 시작)의 완료는 74.2초 = 부하 시작 후 54.2초
        self.assertAlmostEqual(r["t_resolve_s"], 54.2, delta=0.1)
        self.assertAlmostEqual(r["resolve_time_s"], 34.2, delta=0.1)

    def test_two_failures_are_not_t0(self):
        write_run(self.d, observe_sec=100)
        write_ssh(self.d, [(50, 52)])
        r = m4.evaluate(self.d, final=True)
        self.assertIsNone(r["t0_s"])
        self.assertEqual(r["status"], "no_thrash")

    def test_isolated_failures_are_not_t0(self):
        write_run(self.d, observe_sec=100)
        write_ssh(self.d, [(50, 51), (53, 54), (56, 57)])
        self.assertIsNone(m4.evaluate(self.d, final=True)["t0_s"])

    def test_flapping_resets_the_success_run(self):
        # 70~71초에 2번 성공하고 다시 72~79초에 실패하면 성공 연속은 처음부터 센다.
        write_run(self.d)
        write_ssh(self.d, [(40, 70), (72, 80)])
        r = m4.evaluate(self.d, final=True)
        self.assertAlmostEqual(r["t0_s"], 20.0, delta=0.1)
        self.assertAlmostEqual(r["resolve_time_s"], 44.2, delta=0.1)   # 80~84초 5번 성공 -> 84.2초

    def test_unresolved_when_failures_continue_past_cap(self):
        write_run(self.d, cap_sec=60)
        write_ssh(self.d, [(40, 190)], secs=190)
        r = m4.evaluate(self.d, final=True)
        self.assertEqual(r["status"], "unresolved")
        self.assertIsNone(r["resolve_time_s"])

    def test_running_until_cap_then_unresolved_live(self):
        write_run(self.d, cap_sec=100)
        write_ssh(self.d, [(40, 190)], secs=190)
        self.assertEqual(m4.evaluate(self.d, now_ns=BASE + 100 * NS)["status"], "running")
        self.assertEqual(m4.evaluate(self.d, now_ns=BASE + 150 * NS)["status"], "unresolved")

    def test_load_end_before_cap_is_truncated(self):
        # 부하가 40초 뒤(실행 시작 60초)에 스스로 끝나는데 그때까지 접속이 안 된다.
        write_run(self.d, load_sec=40, cap_sec=300)
        write_ssh(self.d, [(40, 190)], secs=190)
        r = m4.evaluate(self.d, now_ns=BASE + 150 * NS)
        self.assertEqual(r["status"], "truncated")
        self.assertIsNone(r["resolve_time_s"])

    def test_resolution_after_load_end_is_not_counted(self):
        # 부하가 실행 시작 70초에 끝나고 SSH는 74.2초에 돌아온다: 자체 종료 뒤의 회복은 해소로 세지 않는다.
        write_run(self.d, load_sec=50)
        write_ssh(self.d, [(40, 70)])
        self.assertEqual(m4.evaluate(self.d, final=True)["status"], "truncated")

    def test_calm_before_t0_is_not_resolution(self):
        write_run(self.d)
        write_ssh(self.d, [(100, 130)])
        r = m4.evaluate(self.d, final=True)
        self.assertAlmostEqual(r["t0_s"], 80.0, delta=0.1)
        self.assertAlmostEqual(r["resolve_time_s"], 34.2, delta=0.1)

    def test_observe_window_for_no_thrash(self):
        write_run(self.d, observe_sec=120)
        write_ssh(self.d, [])
        self.assertEqual(m4.evaluate(self.d, now_ns=LOAD_START + 60 * NS)["status"], "running")
        self.assertEqual(m4.evaluate(self.d, now_ns=LOAD_START + 130 * NS)["status"], "no_thrash")

    def test_probe_file_order_does_not_matter(self):
        write_run(self.d)
        write_ssh(self.d, [(40, 70)], shuffle=True)
        r = m4.evaluate(self.d, final=True)
        self.assertAlmostEqual(r["t0_s"], 20.0, delta=0.1)
        self.assertAlmostEqual(r["resolve_time_s"], 34.2, delta=0.1)


class TestPsiSecondary(Base):
    def test_agrees_with_ssh(self):
        write_run(self.d)
        write_ssh(self.d, [(40, 70)])
        write_samples(self.d, [(40, 70, 0.5)])
        r = m4.evaluate(self.d, final=True)
        self.assertAlmostEqual(r["psi"]["t0_s"], 20.0, delta=1.0)
        self.assertFalse(r["psi"]["disagree"])
        self.assertIsNotNone(r["psi"]["resolve_time_s"])

    def test_oscillating_pressure_is_found_by_the_ten_second_mean(self):
        # 2초마다 1초씩만 60%: 3초 연속 기준으로는 놓치지만 10초 평균(30%)으로는 잡는다.
        write_run(self.d)
        write_ssh(self.d, [(40, 70)])
        write_samples(self.d, [(40 + 2 * k, 41 + 2 * k, 0.6) for k in range(15)])
        r = m4.evaluate(self.d, final=True)
        self.assertAlmostEqual(r["psi"]["t0_s"], 20.0, delta=1.5)

    def test_disagree_when_only_psi_sees_a_stall(self):
        write_run(self.d, observe_sec=100)
        write_ssh(self.d, [])
        write_samples(self.d, [(40, 70, 0.5)])
        r = m4.evaluate(self.d, final=True)
        self.assertIsNone(r["t0_s"])
        self.assertIsNotNone(r["psi"]["t0_s"])
        self.assertTrue(r["psi"]["disagree"])

    def test_disagree_when_t0_differ_by_over_10s(self):
        write_run(self.d)
        write_ssh(self.d, [(40, 70)])
        write_samples(self.d, [(60, 90, 0.5)])
        r = m4.evaluate(self.d, final=True)
        self.assertTrue(r["psi"]["disagree"])
        self.assertAlmostEqual(r["psi"]["t0_gap_s"], 20.0, delta=1.5)

    def test_seq_gap_makes_buckets_invalid(self):
        # 누락이 겹치는 1초 구간은 무효이고, 무효 구간이 든 10초 창은 쓰지 않는다. 계산은 멈추지 않는다.
        write_run(self.d, observe_sec=100)
        write_ssh(self.d, [])
        write_samples(self.d, [(40, 44, 0.9)], secs=200, skip_seq=set(range(4 * 41 + 1, 4 * 41 + 3)))
        r = m4.evaluate(self.d, final=True)
        self.assertGreater(r["invalid_buckets"], 0)


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


class TestDaemonLog(Base):
    def log(self, lines):
        with open(os.path.join(self.d, "daemon.log"), "w") as f:
            f.write("\n".join(lines) + "\n")

    def setup_stall(self, **run):
        write_run(self.d, **run)
        write_ssh(self.d, [(40, 70)])
        write_samples(self.d, [(40, 70, 0.5)])

    def test_first_decision_and_kill(self):
        self.setup_stall(guest_clock_offset_ns=3 * NS)  # guest 시계가 host보다 3초 빠르다
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
        self.assertAlmostEqual(r["detect_delay_s"], 15.0, delta=0.1)    # 결정 35초 - T0 20초
        self.assertEqual(r["decisions"][0]["path"], "io")

    def test_dry_run_decision_is_not_a_kill(self):
        self.setup_stall()
        self.log([f"{iso(BASE + 50 * NS)} warn judge kill cand=tg-load.service dry_run=1 result=0 path=memory"])
        r = m4.evaluate(self.d, final=True)
        self.assertIsNotNone(r["t_detect_s"])
        self.assertIsNone(r["t_kill_s"])
        self.assertEqual(r["killed_roles"], [])
        self.assertTrue(r["daemon_decision_before_kernel"])
        self.assertEqual(r["decision_count"], 1)

    def test_decision_without_thrash_is_flagged(self):
        write_run(self.d, observe_sec=100)
        write_ssh(self.d, [])
        write_samples(self.d, [], secs=200)
        self.log([f"{iso(BASE + 60 * NS)} warn judge kill cand=tg-load.service dry_run=1 result=0 path=memory"])
        r = m4.evaluate(self.d, final=True)
        self.assertTrue(r["decision_without_thrash"])
        self.assertEqual(r["decision_count"], 1)

    def test_e_killed_by_daemon(self):
        self.setup_stall()
        self.log([f"{iso(BASE + 50 * NS)} warn judge kill cand=tg-canary.service dry_run=0 result=0 path=memory"])
        self.assertTrue(m4.evaluate(self.d, final=True)["e_killed"])

    def test_kernel_oom_before_daemon(self):
        self.setup_stall()
        write_samples(self.d, [(40, 70, 0.5)], oom_at=45)
        self.log([f"{iso(BASE + 60 * NS)} warn judge kill cand=tg-load.service dry_run=0 result=0 path=memory"])
        r = m4.evaluate(self.d, final=True)
        self.assertEqual(r["kernel_oom_kills"], 1)
        self.assertEqual(r["first_actor"], "kernel")
        self.assertFalse(r["daemon_decision_before_kernel"])


class TestEProbe(Base):
    def test_gap_is_clipped_to_the_window(self):
        write_run(self.d)
        write_ssh(self.d, [(40, 70)])
        # 카나리가 50~58초(8초) 응답 없음. 창은 T0(40초)부터 해소(74.2초)까지.
        write_probes(self.d, "canary_probe.tsv", [(50, 58)])
        r = m4.evaluate(self.d, final=True)
        self.assertAlmostEqual(r["e_canary"]["max_gap_s"], 9.0, delta=0.6)  # 마지막 성공(49초)~다음 성공(58초)
        self.assertLess(r["e_canary"]["ok_rate"], 1.0)

    def test_no_failure_outside_window_is_ignored(self):
        write_run(self.d)
        write_ssh(self.d, [(40, 70)])
        write_probes(self.d, "canary_probe.tsv", [(0, 30)])  # 스래싱 전에 응답이 없던 것은 세지 않는다
        r = m4.evaluate(self.d, final=True)
        self.assertLessEqual(r["e_canary"]["max_gap_s"], 1.5)

    def test_e_state_failed(self):
        write_run(self.d)
        write_ssh(self.d, [(40, 70)])
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
