"""Frame-rate sweep report (docs/high-fps-native.md; project code, ours).

Reads one run of route hifps_sweep (or any route with the same waits): the run's boot_stderr.log and the SNAP_PACE_LOG csv, and prints
one line per run with
  - fps, mean / stdev / p99 frame time, 1% low (fps of the mean of the slowest 1% of frames) over the ride (wait 'ride' to 'film<=0')
    and over the menus (photo select to the Evaluation end);
  - the share of ride frames within 10% of the median frame time (even pacing);
  - ride length in seconds (wait 'ride' to 'ride>=115'), photo select and Evaluation phase lengths (seconds between the route's waits);
  - interface steps per second (SNAP_UI_PACE_TRACE=1 "[uipace]" lines) where the presents ran above 65 a second.

python -I scripts/hifps/sweep_report.py <tag>=<run dir>,<pace csv> [...] [--md]
"""
import csv
import re
import statistics
import sys


def frame_stats(dts):
    if len(dts) < 20:
        return None
    s = sorted(dts)
    n = len(s)
    mean = sum(s) / n
    slow = s[-max(1, n // 100):]
    med = s[n // 2]
    within = sum(1 for d in dts if abs(d - med) <= 0.1 * med) / n
    return dict(fps=1000.0 / mean, mean=mean, sd=statistics.pstdev(s), p99=s[min(n - 1, int(n * 0.99))], low1=1000.0 / (sum(slow) / len(slow)),
                med=med, within=within, n=n)


def read_run(rundir, pacecsv):
    waits = []   # (cond, log time s, present)
    uisteps = []
    for line in open(rundir + '/boot_stderr.log', encoding='utf-8', errors='replace'):
        m = re.match(r"\[\s*([\d.]+)\].*wait '([^']+)' met after [\d.]+ s at present (\d+)", line)
        if m:
            waits.append((m.group(2), float(m.group(1)), int(m.group(3))))
        m = re.search(r"\[uipace\] [\d.]+ s: ([\d.]+) presents/s, .* steps ([\d.]+)/s", line)
        if m and float(m.group(1)) > 65:
            uisteps.append(float(m.group(2)))
    rows = []
    with open(pacecsv, newline='') as f:
        for r in csv.DictReader(f):
            try:
                rows.append((int(r['n']), float(r['t_ms']), float(r['dt_ms'])))
            except (ValueError, KeyError):
                pass
    return waits, uisteps, rows


def first(waits, cond, after=None):
    for c, t, p in waits:
        if c == cond and (after is None or t >= after):
            return t, p
    return None


def window(rows, p0, p1):
    return [dt for n, t, dt in rows if p0 <= n + 1 <= p1 and dt > 0]


def main():
    md = '--md' in sys.argv
    out = []
    for arg in sys.argv[1:]:
        if arg.startswith('--'):
            continue
        tag, rest = arg.split('=', 1)
        rundir, pace = rest.split(',', 1)
        waits, uisteps, rows = read_run(rundir, pace)
        ride, film = first(waits, 'ride'), first(waits, 'film<=0')
        rend = first(waits, 'ride>=115')
        fs = first(waits, 'ui=photo_submit:FolderSelect')
        ev = first(waits, 'ui=evaluation')
        sc = first(waits, 'ui=evaluation:Score')
        end = first(waits, 'ui=evaluation:End')
        r = {'tag': tag}
        if ride and film:
            r['ride'] = frame_stats(window(rows, ride[1], film[1]))
        if fs and end:
            r['menu'] = frame_stats(window(rows, fs[1], end[1]))
        r['ride_s'] = rend[0] - ride[0] if ride and rend else None
        r['film_s'] = film[0] - ride[0] if ride and film else None
        r['eval_score_s'] = sc[0] - ev[0] if ev and sc else None
        r['score_end_s'] = end[0] - sc[0] if sc and end else None
        r['ui_steps'] = statistics.median(uisteps) if uisteps else None
        r['end'] = end is not None
        out.append(r)

    def f(x, fmt):
        return fmt % x if x is not None else '-'

    if md:
        print('| run | ride fps | mean ms | sd ms | p99 ms | 1% low fps | within 10% | menu fps | menu p99 | ride s | film s | eval to Score s | Score to End s | UI steps/s | End |')
        print('|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|')
    for r in out:
        a, m = r.get('ride') or {}, r.get('menu') or {}
        cells = [r['tag'], f(a.get('fps'), '%.1f'), f(a.get('mean'), '%.2f'), f(a.get('sd'), '%.2f'), f(a.get('p99'), '%.2f'), f(a.get('low1'), '%.1f'),
                 f(a.get('within') * 100 if a else None, '%.0f%%'), f(m.get('fps'), '%.1f'), f(m.get('p99'), '%.2f'), f(r['ride_s'], '%.2f'), f(r['film_s'], '%.2f'),
                 f(r['eval_score_s'], '%.1f'), f(r['score_end_s'], '%.1f'), f(r['ui_steps'], '%.1f'), 'yes' if r['end'] else 'no']
        print(('| ' + ' | '.join(cells) + ' |') if md else '  '.join(cells))


if __name__ == '__main__':
    main()
