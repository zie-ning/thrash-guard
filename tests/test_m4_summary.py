"""lab/analyze/m4_summary.py 집계·판정·그래프 테스트 (표준 라이브러리 unittest)."""

import contextlib
import io
import json
import math
import os
import sys
import tempfile
import unittest
import xml.dom.minidom

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "lab", "analyze"))
import m4_summary as m4s  # noqa: E402


def make_run(root, rid, load, daemon, status="resolved", resolve=None, **over):
    """root/<rid>/result.json을 만든다. 필요한 값만 over로 덮어쓴다."""
    res = {
        "status": status, "t0_s": 10.0, "t_resolve_s": None if resolve is None else 10.0 + resolve,
        "resolve_time_s": resolve, "detect_delay_s": None, "kill_to_resolve_s": None, "t_detect_s": None,
        "first_actor": "none", "kernel_oom_kills": 0, "decision_count": 0, "killed_roles": [],
        "e_killed": False, "e_alive_at_end": True,
        "e_canary": {"max_gap_s": 1.0, "ok_rate": 1.0, "probes": 10},
        "e_ssh": {"max_gap_s": 2.0, "ok_rate": 1.0, "probes": 10},
        "psi": {"disagree": False}, "invalid_buckets": 0, "daemon_log": True,
    }
    res.update(over)
    d = os.path.join(root, rid)
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, "result.json"), "w", encoding="utf-8") as f:
        json.dump({"id": rid, "load": load, "daemon": daemon, "cap_sec": 300, "result": res}, f)


def fill(root, load, daemon, values, status=None, **over):
    """부하·조건의 실행을 여러 개 만든다. values의 None은 unresolved로 쓴다."""
    for i, v in enumerate(values, 1):
        if v is None:
            make_run(root, f"{load}-{daemon}-{i}", load, daemon, status=status or "unresolved", resolve=None, **over)
        else:
            make_run(root, f"{load}-{daemon}-{i}", load, daemon, status=status or "resolved", resolve=v, **over)


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = self.tmp.name

    def tearDown(self):
        self.tmp.cleanup()

    def judge(self):
        runs = m4s.load_runs(self.root)
        groups, cap, j = m4s.build_summary(runs)
        return runs, groups, cap, j


class TestResolveJudgement(Base):
    def test_all_pass(self):
        fill(self.root, "runaway_growth", "dry", [None, None, None])
        fill(self.root, "runaway_growth", "kill", [14.0, 16.0, 20.0])
        fill(self.root, "spike_mem", "dry", [98.0, 100.0, 110.0])
        fill(self.root, "spike_mem", "kill", [45.0, 50.0, 55.0])
        fill(self.root, "normal_burst", "dry", [None, None, None], status="no_thrash")
        runs, _, _, j = self.judge()
        self.assertEqual(j["resolve"]["runaway_growth"]["verdict"], "합격")
        self.assertEqual(j["resolve"]["spike_mem"]["verdict"], "합격")
        # fill이 정상 버스트를 no_thrash로 만들었는지 확인한다(unresolved로 바뀌면 이 테스트가 무의미해진다)
        self.assertEqual({r["result"]["status"] for r in runs if r["load"] == "normal_burst"}, {"no_thrash"})

    def test_kill_not_faster_than_dry_fails(self):
        fill(self.root, "spike_mem", "dry", [90.0, 95.0, 99.0])
        fill(self.root, "spike_mem", "kill", [100.0, 110.0, 120.0])
        _, _, _, j = self.judge()
        self.assertEqual(j["resolve"]["spike_mem"]["verdict"], "불합격")
        self.assertIn("짧지 않다", j["resolve"]["spike_mem"]["reason"])

    def test_kill_over_limit_fails_even_if_faster(self):
        fill(self.root, "spike_mem", "dry", [None, None, None])
        fill(self.root, "spike_mem", "kill", [70.0, 80.0, 90.0])
        _, _, _, j = self.judge()
        self.assertEqual(j["resolve"]["spike_mem"]["verdict"], "불합격")
        self.assertIn("60초를 넘는다", j["resolve"]["spike_mem"]["reason"])

    def test_limit_is_inclusive(self):
        fill(self.root, "spike_mem", "dry", [None, None, None])
        fill(self.root, "spike_mem", "kill", [60.0, 60.0, 60.0])
        self.assertEqual(self.judge()[3]["resolve"]["spike_mem"]["verdict"], "합격")

    def test_kill_median_unresolved_fails(self):
        fill(self.root, "spike_mem", "dry", [None, None, None])
        fill(self.root, "spike_mem", "kill", [30.0, None, None])
        j = self.judge()[3]["resolve"]["spike_mem"]
        self.assertEqual(j["verdict"], "불합격")
        self.assertEqual(j["kill_median"], math.inf)

    def test_one_unresolved_kill_fails_even_if_median_is_fine(self):
        fill(self.root, "spike_mem", "dry", [None, None, None])
        fill(self.root, "spike_mem", "kill", [30.0, 40.0, None])
        j = self.judge()[3]["resolve"]["spike_mem"]
        self.assertEqual(j["verdict"], "불합격")         # 중앙값은 40초지만 종료해도 풀리지 않은 실행이 있다
        self.assertEqual(j["kill_worst"], math.inf)
        self.assertIn("켜짐 1회가 T0 후 300초 안에 해소되지 않았다", j["reason"])

    def test_too_few_valid_runs_is_not_judged(self):
        # dry-run 3회 중 2회가 스래싱이 안 나면(no_thrash) 유효 실행이 1개뿐이다.
        make_run(self.root, "a1", "spike_mem", "dry", status="resolved", resolve=98.0)
        make_run(self.root, "a2", "spike_mem", "dry", status="no_thrash")
        make_run(self.root, "a3", "spike_mem", "dry", status="no_thrash")
        fill(self.root, "spike_mem", "kill", [45.0, 50.0, 55.0])
        j = self.judge()[3]["resolve"]["spike_mem"]
        self.assertEqual(j["verdict"], "판정 불가")

    def test_truncated_and_incomplete_are_excluded(self):
        fill(self.root, "spike_mem", "dry", [None, None])
        make_run(self.root, "t1", "spike_mem", "dry", status="truncated")
        make_run(self.root, "t2", "spike_mem", "dry", status="incomplete")
        fill(self.root, "spike_mem", "kill", [30.0, 40.0, 50.0])
        j = self.judge()[3]["resolve"]["spike_mem"]
        self.assertEqual(j["verdict"], "합격")
        self.assertEqual(len(j["dry_values"]), 2)

    def test_missing_condition_is_not_judged(self):
        fill(self.root, "spike_mem", "kill", [30.0, 40.0, 50.0])
        self.assertEqual(self.judge()[3]["resolve"]["spike_mem"]["verdict"], "판정 불가")


class TestSafetyJudgement(Base):
    def test_e_survival_pass(self):
        fill(self.root, "spike_mem", "kill", [30.0, 40.0, 50.0])
        self.assertEqual(self.judge()[3]["e_survival"]["verdict"], "합격")

    def test_one_e_kill_fails(self):
        fill(self.root, "spike_mem", "kill", [30.0, 40.0])
        make_run(self.root, "bad", "spike_mem", "kill", resolve=30.0, e_killed=True, killed_roles=["E"])
        j = self.judge()[3]["e_survival"]
        self.assertEqual(j["verdict"], "불합격")
        self.assertIn("bad", j["reason"])

    def test_missing_e_state_is_not_judged(self):
        fill(self.root, "spike_mem", "kill", [30.0, 40.0])
        make_run(self.root, "nostate", "spike_mem", "kill", resolve=30.0, e_alive_at_end=None)
        self.assertEqual(self.judge()[3]["e_survival"]["verdict"], "판정 불가")

    def test_e_kill_beats_missing_state(self):
        make_run(self.root, "bad", "spike_mem", "kill", resolve=30.0, e_killed=True)
        make_run(self.root, "nostate", "spike_mem", "kill", resolve=30.0, e_alive_at_end=None)
        self.assertEqual(self.judge()[3]["e_survival"]["verdict"], "불합격")

    def test_no_kill_runs(self):
        fill(self.root, "spike_mem", "dry", [None, None, None])
        self.assertEqual(self.judge()[3]["e_survival"]["verdict"], "판정 불가")

    def test_normal_burst_pass_and_fail(self):
        fill(self.root, "normal_burst", "dry", [None] * 3, status="no_thrash")
        self.assertEqual(self.judge()[3]["normal_burst"]["verdict"], "합격")
        make_run(self.root, "fp", "normal_burst", "dry", status="no_thrash", decision_count=1)
        j = self.judge()[3]["normal_burst"]
        self.assertEqual(j["verdict"], "불합격")
        self.assertIn("1건", j["reason"])

    def test_normal_burst_without_daemon_log_is_not_judged(self):
        make_run(self.root, "nb1", "normal_burst", "dry", status="no_thrash", daemon_log=False)
        self.assertEqual(self.judge()[3]["normal_burst"]["verdict"], "판정 불가")

    def test_normal_burst_decision_beats_missing_log(self):
        make_run(self.root, "nb1", "normal_burst", "dry", status="no_thrash", daemon_log=False)
        make_run(self.root, "nb2", "normal_burst", "dry", status="no_thrash", decision_count=2)
        self.assertEqual(self.judge()[3]["normal_burst"]["verdict"], "불합격")

    def test_no_normal_burst_runs(self):
        fill(self.root, "spike_mem", "kill", [30.0])
        self.assertEqual(self.judge()[3]["normal_burst"]["verdict"], "판정 불가")


class TestLoadAndWarnings(Base):
    def test_load_runs_skips_bad_inputs(self):
        fill(self.root, "spike_mem", "kill", [30.0])
        os.makedirs(os.path.join(self.root, "empty"))                       # result.json 없음
        os.makedirs(os.path.join(self.root, "broken"))
        with open(os.path.join(self.root, "broken", "result.json"), "w") as f:
            f.write("{not json")
        os.makedirs(os.path.join(self.root, "other"))
        with open(os.path.join(self.root, "other", "result.json"), "w") as f:
            json.dump({"hello": 1}, f)                                       # 형식이 다름
        with open(os.path.join(self.root, "file.txt"), "w") as f:
            f.write("x")
        runs = m4s.load_runs(self.root)
        self.assertEqual([r["_dir"] for r in runs], ["spike_mem-kill-1"])

    def test_warnings(self):
        make_run(self.root, "r1", "spike_mem", "dry", resolve=90.0, psi={"disagree": True}, invalid_buckets=3)
        make_run(self.root, "r2", "spike_mem", "kill", status="incomplete", daemon_log=False, e_alive_at_end=None)
        make_run(self.root, "r3", "runaway_growth", "dry", status="no_thrash")
        msgs = {rid: [] for rid in ("r1", "r2", "r3")}
        for rid, m in m4s.warnings_for(m4s.load_runs(self.root)):
            msgs[rid].append(m)
        self.assertTrue(any("어긋" in m for m in msgs["r1"]))
        self.assertTrue(any("무효인 1초 구간" in m for m in msgs["r1"]))
        self.assertTrue(any("incomplete" in m for m in msgs["r2"]))
        self.assertTrue(any("데몬 로그" in m for m in msgs["r2"]))
        self.assertTrue(any("스래싱이 나지 않았다" in m for m in msgs["r3"]))

    def test_normal_burst_no_thrash_is_not_a_warning(self):
        make_run(self.root, "nb", "normal_burst", "dry", status="no_thrash")
        self.assertEqual(m4s.warnings_for(m4s.load_runs(self.root)), [])


class TestOutputs(Base):
    def full_set(self):
        fill(self.root, "runaway_growth", "dry", [None, None, None])
        fill(self.root, "runaway_growth", "kill", [14.0, 16.0, 20.0])
        fill(self.root, "spike_mem", "dry", [98.0, 100.0, 110.0])
        fill(self.root, "spike_mem", "kill", [45.0, 50.0, 55.0])
        fill(self.root, "normal_burst", "dry", [None] * 3, status="no_thrash")

    def bars(self, svg):
        """막대(class="bar")의 (x, 높이)를 그려진 순서대로 읽는다."""
        doc = xml.dom.minidom.parseString(svg)
        return [(float(r.getAttribute("x")), float(r.getAttribute("height")))
                for r in doc.getElementsByTagName("rect") if r.getAttribute("class") == "bar"]

    def test_svg_is_wellformed_and_follows_reference_layout(self):
        self.full_set()
        _, groups, cap, _ = self.judge()
        svg = m4s.render_load_svg(groups, "spike_mem", cap)
        xml.dom.minidom.parseString(svg)              # 형식이 깨지면 예외
        for needle in ("SPIKE_MEM", "회차", "스래싱 해소 시간 (초)", "Thrashing Guard", "Linux Kernel", "#f0f1f3"):
            self.assertIn(needle, svg)
        self.assertNotIn("RUNAWAY_GROWTH", svg)       # 부하마다 그래프가 따로다
        self.assertEqual(len(self.bars(svg)), 6)      # 3회차 x (Thrashing Guard, Linux Kernel)
        # 제목에는 부하 이름만 남긴다. 범례 이름 외의 옛 문구와 빗금 무늬는 없다.
        doc = xml.dom.minidom.parseString(svg)
        titles = [t.firstChild.data for t in doc.getElementsByTagName("text") if t.getAttribute("font-size") == "16"]
        self.assertEqual(titles, ["SPIKE_MEM"])
        for gone in ("dry-run", "데몬 켜짐", "꺼짐 대용", "미해소", "없음", "무효", "pattern", "hatch"):
            self.assertNotIn(gone, svg)

    def test_legend_box_is_wide_enough_for_the_longest_label(self):
        self.full_set()
        _, groups, cap, _ = self.judge()
        doc = xml.dom.minidom.parseString(m4s.render_load_svg(groups, "spike_mem", cap))
        box = next(r for r in doc.getElementsByTagName("rect") if r.getAttribute("rx") == "3")
        texts = {t.firstChild.data: float(t.getAttribute("x")) for t in doc.getElementsByTagName("text")
                 if t.firstChild is not None and t.firstChild.data in m4s.LABELS.values()}
        bx, bw = float(box.getAttribute("x")), float(box.getAttribute("width"))
        self.assertEqual(set(texts), {"Thrashing Guard", "Linux Kernel"})
        for label, x in texts.items():
            # 실제 렌더링 폭은 글자당 약 7.5px(Thrashing Guard는 캡처에서 약 108px)로 보고, 박스 안에 남는 여백을 확인한다.
            self.assertGreaterEqual(bx + bw - (x + len(label) * 7.5), 12, label)
        self.assertGreaterEqual(bw, 170)                  # 이전 140px은 글자가 박스를 넘쳤다

    def test_y_axis_runs_from_0_to_300_in_steps_of_50(self):
        self.full_set()
        _, groups, cap, _ = self.judge()
        doc = xml.dom.minidom.parseString(m4s.render_load_svg(groups, "spike_mem", cap))
        ticks = [t.firstChild.data for t in doc.getElementsByTagName("text") if t.getAttribute("text-anchor") == "end"]
        self.assertEqual(ticks, ["0", "50", "100", "150", "200", "250", "300+"])

    def test_bars_are_paired_per_rep_and_scaled(self):
        fill(self.root, "spike_mem", "dry", [100.0, 80.0, 60.0])
        fill(self.root, "spike_mem", "kill", [50.0, 40.0, 30.0])
        _, groups, cap, _ = self.judge()
        bars = self.bars(m4s.render_load_svg(groups, "spike_mem", cap))
        # 그려진 순서는 회차마다 (켜짐, dry-run)이고, 켜짐 막대가 왼쪽에 붙어 있다.
        kill_bars, dry_bars = bars[0::2], bars[1::2]
        self.assertEqual([round(h) for _, h in dry_bars], sorted([round(h) for _, h in dry_bars], reverse=True))
        for (kx, _), (dx, _) in zip(kill_bars, dry_bars):
            self.assertLess(kx, dx)                   # 같은 회차에서 켜짐이 왼쪽
        # 좌표를 0.1픽셀로 반올림하므로 비율은 2%까지 허용한다.
        self.assertAlmostEqual(dry_bars[0][1] / kill_bars[0][1], 2.0, delta=0.04)     # 100초는 50초의 2배 높이
        self.assertAlmostEqual(dry_bars[2][1] / kill_bars[2][1], 2.0, delta=0.04)     # 60초는 30초의 2배 높이

    def test_unresolved_bar_is_a_solid_bar_up_to_the_cap(self):
        fill(self.root, "runaway_growth", "dry", [None, None, None])
        fill(self.root, "runaway_growth", "kill", [15.0, 30.0, 60.0])
        _, groups, cap, _ = self.judge()
        svg = m4s.render_load_svg(groups, "runaway_growth", cap)
        self.assertNotIn("미해소", svg)               # 문구도 빗금도 없다
        self.assertNotIn("hatch", svg)
        self.assertEqual(svg.count(f'fill="{m4s.COLORS["dry"]}"'), 3 + 1)   # 막대 3개와 범례 1개
        bars = self.bars(svg)
        kill_bars, dry_bars = bars[0::2], bars[1::2]
        # 미해소 막대는 측정 상한(300초) 높이이고, 같은 눈금 위에서 15초 막대의 20배, 60초 막대의 5배다.
        self.assertAlmostEqual(dry_bars[0][1] / kill_bars[0][1], 300 / 15, delta=0.4)    # 반올림 오차 2%
        self.assertAlmostEqual(dry_bars[2][1] / kill_bars[2][1], 300 / 60, delta=0.1)

    def test_invalid_and_missing_runs_leave_the_slot_empty(self):
        fill(self.root, "spike_mem", "kill", [45.0, 50.0, 55.0])
        make_run(self.root, "spike_mem-dry-1", "spike_mem", "dry", status="no_thrash")
        _, groups, cap, _ = self.judge()
        svg = m4s.render_load_svg(groups, "spike_mem", cap)
        xml.dom.minidom.parseString(svg)
        for gone in ("무효", "없음", "미해소"):
            self.assertNotIn(gone, svg)
        self.assertEqual(len(self.bars(svg)), 3)      # Thrashing Guard 3개만 그려진다

    def test_chart_loads_only_for_stress_loads_with_runs(self):
        fill(self.root, "spike_mem", "kill", [45.0])
        fill(self.root, "normal_burst", "dry", [None], status="no_thrash")
        _, groups, _, _ = self.judge()
        self.assertEqual(m4s.chart_loads(groups), ["spike_mem"])

    def test_rep_number_comes_from_the_run_id(self):
        make_run(self.root, "spike_mem-dry-3", "spike_mem", "dry", resolve=90.0)
        make_run(self.root, "spike_mem-dry-10", "spike_mem", "dry", resolve=80.0)
        runs = m4s.load_runs(self.root)
        self.assertEqual(set(m4s.rep_values(runs)), {3, 10})

    def test_axis_for_cap(self):
        self.assertEqual(m4s.axis_for(300), (50, 300, 315.0))     # 0~300초, 위로 5% 여유
        self.assertEqual(m4s.axis_for(250)[1], 250)
        self.assertEqual(m4s.axis_for(301)[1], 350)               # 상한이 50의 배수가 아니면 올림

    def test_markdown_has_sections_and_verdicts(self):
        self.full_set()
        runs, groups, cap, j = self.judge()
        md = m4s.render_markdown(self.root, runs, groups, cap, j)
        for needle in ("## 합불 판정", "## 해소 시간 그래프", "## 해소 시간", "## 기록 지표", "## 주의할 실행", "## 실행 목록",
                       "resolve_time_runaway_growth.svg", "resolve_time_spike_mem.svg", "**합격**"):
            self.assertIn(needle, md)
        self.assertNotIn("실행은 2회뿐", md)

    def test_markdown_warns_about_few_runs(self):
        fill(self.root, "spike_mem", "dry", [98.0, 100.0])
        runs, groups, cap, j = self.judge()
        md = m4s.render_markdown(self.root, runs, groups, cap, j)
        self.assertIn("SPIKE_MEM dry-run 실행은 2회뿐이다", md)

    def test_main_writes_files(self):
        self.full_set()
        out = os.path.join(self.root, "out")
        old = sys.argv
        sys.argv = ["m4_summary.py", self.root, "--out", out]
        try:
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(m4s.main(), 0)
        finally:
            sys.argv = old
        for name in ("summary.md", "summary.json", "resolve_time_runaway_growth.svg", "resolve_time_spike_mem.svg"):
            self.assertTrue(os.path.isfile(os.path.join(out, name)), name)
        with open(os.path.join(out, "summary.json"), encoding="utf-8") as f:
            text = f.read()
        self.assertNotIn("Infinity", text)            # JSON 표준에 없는 값은 쓰지 않는다
        data = json.loads(text, parse_constant=lambda c: self.fail(f"비표준 상수 {c}"))
        self.assertEqual(data["runs"], 15)
        self.assertEqual(data["judgments"]["e_survival"]["verdict"], "합격")
        # 미해소(무한대)는 JSON에서 "inf"로 쓴다
        self.assertEqual(data["groups"]["runaway_growth-dry"]["resolve_values"], ["inf", "inf", "inf"])
        self.assertEqual(data["judgments"]["resolve"]["runaway_growth"]["dry_median"], "inf")

    def test_main_fails_on_empty_root(self):
        old = sys.argv
        sys.argv = ["m4_summary.py", self.root]
        try:
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(m4s.main(), 1)
        finally:
            sys.argv = old


if __name__ == "__main__":
    unittest.main()
