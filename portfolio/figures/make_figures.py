#!/usr/bin/env python3
"""
포트폴리오용 시계열 그림을 SVG로 만든다. 표준 라이브러리만 쓴다(설치할 것이 없다).

만드는 그림
  fig_2_2_livelock_timeline.svg  굳어진 livelock 한 번의 시계열: 가용 메모리, 10초 평균 memory full과 io full,
                                 로그인 불가 구간
  fig_2_3_rule_comparison.svg    같은 조건 dry-run 3회: 10초 평균 memory full/io full과 임계값, 기존 규칙과
                                 io 경로를 더한 규칙의 첫 종료 결정 시각

데이터: results/m3/20261113/G_throughput_dry_{1,2,3} (처리량 약 10.9 MB/s 제한, 데몬 dry-run)
  psi.tsv          psirec 기록. 에포크ms memory_some memory_full io_some io_full cpu_some cpu_full MemAvailable_kB ...
  ssh_login.probe  10초 간격 전체 로그인 시도. 시작에포크 걸린초 종료코드 | 상태

종료 결정 시각(DECISIONS)은 lab/analyze/replay_judge.c로 같은 기록을 재생한 값이다(아래 명령으로 다시 구할 수 있다).
  replay_judge <psi.tsv> 10 0     # 기존 규칙(io 경로 끔)
  replay_judge <psi.tsv> 10 50    # io 경로 추가
시각 0은 가용 메모리가 처음 30 MiB 미만이 된 때다.

사용법: python portfolio/figures/make_figures.py
"""
import os
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
DATA = os.path.join(ROOT, 'results', 'm3', '20261113')
OUT = os.path.dirname(os.path.abspath(__file__))

# replay_judge 결과: (기존 규칙 첫 종료 결정, io 경로를 더한 규칙의 첫 종료 결정), 시각 0 기준 초
DECISIONS = {
    'G_throughput_dry_1': (147, 24),
    'G_throughput_dry_2': (67, 22),
    'G_throughput_dry_3': (19, 19),
}

FONT = "'Malgun Gothic', 'Apple SD Gothic Neo', 'Noto Sans CJK KR', 'NanumGothic', sans-serif"
C_MEM = '#1f6fb4'
C_IO = '#e8750a'
C_AVAIL = '#555555'
C_OLD = '#c0392b'
C_NEW = '#1e8449'
C_FAIL = '#f4b6b0'
C_GRID = '#dddddd'


def load_psi(run):
    rows = []
    with open(os.path.join(DATA, run, 'psi.tsv'), encoding='utf-8') as f:
        for line in f:
            p = line.split()
            if len(p) >= 8:
                rows.append([int(x) for x in p])
    return rows


def t_zero(rows):
    """가용 메모리가 처음 30 MiB 미만이 된 시각(초, 에포크). replay_judge와 같은 기준(60번째 행 이후)."""
    for i, r in enumerate(rows):
        if i > 60 and r[7] < 30 * 1024:
            return r[0] / 1000.0
    return rows[0][0] / 1000.0


def load_login_fail_intervals(run):
    """로그인 실패 구간을 합쳐서 (시작, 끝) 에포크초 목록으로 돌려준다.
    실패 시도의 시작부터 다음 성공 시도의 끝까지를 '로그인 불가'로 본다."""
    probes = []
    with open(os.path.join(DATA, run, 'ssh_login.probe'), encoding='utf-8', errors='replace') as f:
        for line in f:
            p = line.split()
            if len(p) >= 3:
                probes.append((float(p[0]), float(p[1]), int(p[2])))
    out = []
    start = None
    for t, el, rc in probes:
        if rc != 0:
            if start is None:
                start = t
        else:
            if start is not None:
                out.append((start, t + el))
                start = None
    if start is not None:
        out.append((start, probes[-1][0] + probes[-1][1]))
    return out


def sliding(rows, col, w=10):
    """10초 슬라이딩 평균(%). col은 누적 us 열. (시각 에포크초, 값) 목록."""
    out = []
    for i in range(w, len(rows)):
        dt = (rows[i][0] - rows[i - w][0]) / 1000.0
        if dt <= 0:
            continue
        v = (rows[i][col] - rows[i - w][col]) / (dt * 1e6) * 100.0
        out.append((rows[i][0] / 1000.0, max(0.0, min(100.0, v))))
    return out


def per_second(rows, col):
    out = []
    for i in range(1, len(rows)):
        dt = (rows[i][0] - rows[i - 1][0]) / 1000.0
        if dt <= 0:
            continue
        v = (rows[i][col] - rows[i - 1][col]) / (dt * 1e6) * 100.0
        out.append((rows[i][0] / 1000.0, max(0.0, min(100.0, v))))
    return out


class Svg:
    def __init__(self, w, h):
        self.w, self.h = w, h
        self.parts = []

    def add(self, s):
        self.parts.append(s)

    def text(self, x, y, s, size=12, anchor='start', fill='#222222', weight='normal', rotate=None):
        s = s.replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;')
        tr = f' transform="rotate({rotate} {x:.1f} {y:.1f})"' if rotate else ''
        self.add(f'<text x="{x:.1f}" y="{y:.1f}" font-size="{size}" text-anchor="{anchor}" fill="{fill}" '
                 f'font-weight="{weight}"{tr}>{s}</text>')

    def label(self, x, y, s, color, anchor='start', size=12):
        """흰 배경을 깔아 선과 겹쳐도 읽히는 라벨."""
        w = sum(size if ord(c) > 0x2e80 else size * 0.6 for c in s) + 8
        x0 = x if anchor == 'start' else x - w
        self.rect(x0 - 2, y - size, w, size + 6, '#ffffff', 0.88)
        self.text(x if anchor == 'start' else x, y, s, size, anchor, color, 'bold')

    def line(self, x1, y1, x2, y2, stroke='#000', width=1, dash=None, opacity=1.0):
        d = f' stroke-dasharray="{dash}"' if dash else ''
        self.add(f'<line x1="{x1:.1f}" y1="{y1:.1f}" x2="{x2:.1f}" y2="{y2:.1f}" stroke="{stroke}" '
                 f'stroke-width="{width}"{d} opacity="{opacity}"/>')

    def rect(self, x, y, w, h, fill, opacity=1.0, stroke=None):
        st = f' stroke="{stroke}"' if stroke else ''
        self.add(f'<rect x="{x:.1f}" y="{y:.1f}" width="{w:.1f}" height="{h:.1f}" fill="{fill}" '
                 f'opacity="{opacity}"{st}/>')

    def poly(self, pts, stroke, width=1.5, opacity=1.0):
        s = ' '.join(f'{x:.1f},{y:.1f}' for x, y in pts)
        self.add(f'<polyline points="{s}" fill="none" stroke="{stroke}" stroke-width="{width}" '
                 f'stroke-linejoin="round" opacity="{opacity}"/>')

    def save(self, path):
        head = (f'<svg xmlns="http://www.w3.org/2000/svg" width="{self.w}" height="{self.h}" '
                f'viewBox="0 0 {self.w} {self.h}" font-family="{FONT}">'
                f'<rect width="{self.w}" height="{self.h}" fill="#ffffff"/>')
        with open(path, 'w', encoding='utf-8') as f:
            f.write(head + ''.join(self.parts) + '</svg>')


class Panel:
    """데이터 좌표를 화면 좌표로 바꾸는 그래프 영역."""

    def __init__(self, svg, x, y, w, h, xr, yr):
        self.svg, self.x, self.y, self.w, self.h = svg, x, y, w, h
        self.x0, self.x1 = xr
        self.y0, self.y1 = yr

    def px(self, v):
        return self.x + (v - self.x0) / (self.x1 - self.x0) * self.w

    def py(self, v):
        return self.y + self.h - (v - self.y0) / (self.y1 - self.y0) * self.h

    def frame(self, yticks, yfmt='{:g}', xticks=None, xlabel=None, ylabel=None):
        s = self.svg
        for v in yticks:
            yy = self.py(v)
            s.line(self.x, yy, self.x + self.w, yy, C_GRID, 1)
            s.text(self.x - 8, yy + 4, yfmt.format(v), 11, 'end', '#444444')
        if xticks:
            for v in xticks:
                xx = self.px(v)
                s.line(xx, self.y + self.h, xx, self.y + self.h + 5, '#444444', 1)
                s.text(xx, self.y + self.h + 19, f'{v:g}', 11, 'middle', '#444444')
        s.line(self.x, self.y, self.x, self.y + self.h, '#444444', 1)
        s.line(self.x, self.y + self.h, self.x + self.w, self.y + self.h, '#444444', 1)
        if xlabel:
            s.text(self.x + self.w / 2, self.y + self.h + 38, xlabel, 12, 'middle', '#222222')
        if ylabel:
            s.text(self.x - 48, self.y + self.h / 2, ylabel, 12, 'middle', '#222222', rotate=-90)

    def curve(self, pts, t0, stroke, width=1.8, opacity=1.0):
        clipped = [(self.px(t - t0), self.py(v)) for t, v in pts if self.x0 <= t - t0 <= self.x1]
        if clipped:
            self.svg.poly(clipped, stroke, width, opacity)

    def shade(self, intervals, t0, fill, opacity=0.55):
        for a, b in intervals:
            a, b = a - t0, b - t0
            if b < self.x0 or a > self.x1:
                continue
            a, b = max(a, self.x0), min(b, self.x1)
            self.svg.rect(self.px(a), self.y, self.px(b) - self.px(a), self.h, fill, opacity)

    def hline(self, v, stroke, dash='6 4', width=1.3):
        yy = self.py(v)
        self.svg.line(self.x, yy, self.x + self.w, yy, stroke, width, dash)

    def vline(self, v, stroke, dash=None, width=1.8):
        xx = self.px(v)
        self.svg.line(xx, self.y, xx, self.y + self.h, stroke, width, dash)


def legend(svg, x, y, items):
    for label, color, dashed in items:
        svg.line(x, y - 4, x + 26, y - 4, color, 3, '6 4' if dashed else None)
        svg.text(x + 32, y, label, 12, 'start', '#222222')
        x += 32 + 8 * len(label) + 28


def fig_timeline():
    run = 'G_throughput_dry_1'
    rows = load_psi(run)
    t0 = t_zero(rows)
    fails = load_login_fail_intervals(run)
    mem = sliding(rows, 2)
    io = sliding(rows, 4)
    mem1 = per_second(rows, 2)
    avail = [(r[0] / 1000.0, r[7] / 1024.0) for r in rows]
    tmax = int(rows[-1][0] / 1000.0 - t0)
    xr = (-60, tmax)
    xticks = list(range(0, tmax + 1, 60))
    xticks = [-60] + xticks

    W, H = 980, 700
    svg = Svg(W, H)
    svg.text(W / 2, 28, '굳어진 livelock의 시계열: 정지 초기에는 memory full, 이후에는 io full', 17, 'middle',
             '#111111', 'bold')
    svg.text(W / 2, 48, '처리량 10.9 MB/s 제한 VM, 데몬 dry-run(종료하지 않음), 650 MiB 폭주 컨테이너. 시각 0 = 가용 메모리가 처음 30 MiB 미만이 된 때',
             12, 'middle', '#555555')

    top = Panel(svg, 80, 80, 860, 150, xr, (0, 600))
    top.shade(fails, t0, C_FAIL)
    top.frame([0, 200, 400, 600], '{:g}', None, None, '가용 메모리 (MiB)')
    top.curve(avail, t0, C_AVAIL, 1.8)
    svg.text(top.px(-55), top.py(560), '(a) 가용 메모리', 12, 'start', '#222222', 'bold')

    bot = Panel(svg, 80, 270, 860, 270, xr, (0, 100))
    bot.shade(fails, t0, C_FAIL)
    bot.frame([0, 20, 40, 60, 80, 100], '{:g}%', xticks, '가용 메모리가 바닥난 뒤 경과 시간 (초)',
              '시스템 전체 full 비율')
    bot.curve(mem1, t0, C_MEM, 0.8, 0.35)
    bot.curve(io, t0, C_IO, 2.2)
    bot.curve(mem, t0, C_MEM, 2.2)
    bot.hline(40, '#888888', '6 4')
    svg.text(bot.px(-55), bot.py(40) - 5, '기존 임계 40%', 11, 'start', '#666666')
    bot.hline(10, C_NEW, '6 4')
    svg.text(bot.px(-55), bot.py(10) - 5, '현재 임계 10%', 11, 'start', C_NEW)
    svg.text(bot.px(-55), bot.py(94), '(b) 10초 평균', 12, 'start', '#222222', 'bold')

    # 폭주 컨테이너가 설정한 실행 시간(420초)이 끝나 메모리가 돌아온 시점
    back = next((t - t0 for t, v in avail if t - t0 > 0 and v > 200), None)
    if back is not None:
        svg.line(bot.px(back), bot.y, bot.px(back), bot.y + bot.h, '#666666', 1.2, '3 3')
        svg.text(bot.px(back) - 6, bot.py(62), '폭주 컨테이너의 설정 시간(420초)이 끝나 해소', 11, 'end', '#444444')

    legend(svg, 80, 620, [('memory full (10초 평균)', C_MEM, False), ('io full (10초 평균)', C_IO, False),
                          ('memory full (1초, 옅게)', '#8fb7d9', False)])
    svg.rect(80, 636, 26, 12, C_FAIL, 0.8)
    svg.text(112, 647, '로그인 불가 구간 (10초 간격 전체 로그인 시도 기준)', 12, 'start', '#222222')
    svg.text(80, 676, '출처: results/m3/20261113/G_throughput_dry_1 (psi.tsv, ssh_login.probe). 한 번의 실행이다.', 11,
             'start', '#777777')
    path = os.path.join(OUT, 'fig_2_2_livelock_timeline.svg')
    svg.save(path)
    return path


def fig_rules():
    runs = ['G_throughput_dry_1', 'G_throughput_dry_2', 'G_throughput_dry_3']
    data = []
    for r in runs:
        rows = load_psi(r)
        data.append((r, rows, t_zero(rows), sliding(rows, 2), sliding(rows, 4)))

    tmax = max(int(rows[-1][0] / 1000.0 - t0) for _, rows, t0, _, _ in data)
    xr = (-60, tmax)
    xticks = [-60] + list(range(0, tmax + 1, 60))

    ph = 150
    gap = 56
    W, H = 980, 90 + 3 * ph + 2 * gap + 170
    svg = Svg(W, H)
    svg.text(W / 2, 28, '기존 규칙과 io 경로를 더한 규칙: 첫 종료 결정 시각 비교', 17, 'middle', '#111111', 'bold')
    svg.text(W / 2, 48, '같은 조건 dry-run 3회, 기록을 실제 판단 함수에 재생한 값. 시각 0 = 가용 메모리가 처음 30 MiB 미만이 된 때',
             12, 'middle', '#555555')

    for k, (run, rows, t0, mem, io) in enumerate(data):
        y = 80 + k * (ph + gap)
        p = Panel(svg, 80, y, 860, ph, xr, (0, 100))
        last = k == 2
        p.frame([0, 25, 50, 75, 100], '{:g}%', xticks if last else None,
                '가용 메모리가 바닥난 뒤 경과 시간 (초)' if last else None, '10초 평균')
        p.curve(io, t0, C_IO, 1.8)
        p.curve(mem, t0, C_MEM, 2.2)
        p.hline(40, '#888888', '6 4', 1.1)
        p.hline(10, C_NEW, '6 4', 1.1)
        p.hline(50, C_IO, '2 4', 1.1)
        old, new = DECISIONS[run]
        svg.text(p.px(-56), y - 8, f'실행 {k + 1}', 12, 'start', '#222222', 'bold')
        if old == new:
            p.vline(old, C_NEW, None, 2.2)
            svg.label(p.px(old) + 8, p.py(60), f'두 규칙 모두 +{old}초', C_NEW)
        else:
            p.vline(old, C_OLD, '7 4', 2.2)
            svg.label(p.px(old) + 8, p.py(35), f'기존 규칙 +{old}초', C_OLD)
            p.vline(new, C_NEW, None, 2.2)
            svg.label(p.px(new) + 8, p.py(65), f'io 경로 추가 +{new}초', C_NEW)

    ly = 90 + 3 * ph + 2 * gap + 70
    legend(svg, 80, ly, [('memory full', C_MEM, False), ('io full', C_IO, False)])
    svg.line(360, ly - 4, 386, ly - 4, '#888888', 2, '6 4')
    svg.text(392, ly, '기존 임계 40%', 12)
    svg.line(500, ly - 4, 526, ly - 4, C_NEW, 2, '6 4')
    svg.text(532, ly, '현재 memory 임계 10%', 12)
    svg.line(690, ly - 4, 716, ly - 4, C_IO, 2, '2 4')
    svg.text(722, ly, 'io 경로 임계 50%', 12)
    svg.text(80, ly + 28, '굳어진 뒤에는 memory full 평균이 임계 아래로 내려가지만 io full은 90% 안팎으로 유지된다. 가용 메모리 조건(io 경로는 5% 이하)이 함께 필요하다.',
             11, 'start', '#555555')
    svg.text(80, ly + 46, '출처: results/m3/20261113/G_throughput_dry_1..3, 종료 결정 시각은 lab/analyze/replay_judge.c 재생 결과. 각 1회, 총 3회.',
             11, 'start', '#777777')
    path = os.path.join(OUT, 'fig_2_3_rule_comparison.svg')
    svg.save(path)
    return path


def main():
    for p in (fig_timeline(), fig_rules()):
        print('만들었다:', os.path.relpath(p, ROOT))
    return 0


if __name__ == '__main__':
    sys.exit(main())
