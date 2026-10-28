#!/usr/bin/env python3
"""Summaries and README tables from the benchmark's JSON runs.

  scripts/results.py summary results/BATCH   medians and spread of one batch
  scripts/results.py readme [--check]        rewrite README.md's generated
                                             blocks; --check only reports
                                             whether they are stale

A batch directory holds NAME-I.json runs written by scripts/bench_report.sh,
builds.txt (name, engine, and commit of each entry), and optionally
notes.txt (an entry name, a space, then a description of that entry).
Every number between "BEGIN generated" and "END generated" in README.md
comes from these files.
"""
import json
import pathlib
import re
import statistics
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
KINDS = ["all", "limit", "market", "cancel", "modify"]
UNTIMED, TIMED = "untimed_throughput_ops_per_s", "throughput_ops_per_s"


class Entry:
    def __init__(self, name, engine, commit, runs, note):
        self.name, self.engine, self.commit = name, engine, commit
        self.runs, self.note = runs, note

    def values(self, key):
        return [r[key] for r in self.runs]

    def lat(self, kind, field):
        return statistics.median(r["latency"][kind][field] for r in self.runs)

    def allocs_zero(self):
        return sum(1 for r in self.runs if r["heap_allocations"] == 0)


def batch_dir(batch):
    p = pathlib.Path(batch)
    return p if p.is_dir() else ROOT / "results" / batch


def load(batch):
    d = batch_dir(batch)
    notes = {}
    if (d / "notes.txt").exists():
        for line in (d / "notes.txt").read_text().splitlines():
            name, _, text = line.partition(" ")
            if name:
                notes[name] = text.strip()
    entries = []
    for line in (d / "builds.txt").read_text().splitlines():
        name, engine, commit = line.split()
        files = [f for f in d.glob(f"{name}-*.json")
                 if re.fullmatch(rf"{re.escape(name)}-\d+\.json", f.name)]
        files.sort(key=lambda f: int(f.stem.rsplit("-", 1)[1]))
        runs = [json.loads(f.read_text()) for f in files]
        if not runs:
            sys.exit(f"{d}: no runs for entry {name}")
        entries.append(Entry(name, engine, commit, runs, notes.get(name, "")))
    return entries


def mops(x):
    return f"{x / 1e6:.2f}" if x < 1e6 else f"{x / 1e6:.1f}"


def med_range(xs):
    return f"{mops(statistics.median(xs))} ({mops(min(xs))} to {mops(max(xs))})"


def delta(base, cand):
    b, c = statistics.median(base.values(UNTIMED)), statistics.median(cand.values(UNTIMED))
    return 100.0 * (c - b) / b


# Conservative on purpose: a change counts only when every run of one side
# beats every run of the other. Overlapping ranges are reported as such.
def verdict(base, cand):
    b, c = base.values(UNTIMED), cand.values(UNTIMED)
    if min(c) > max(b):
        return "faster, ranges do not overlap"
    if max(c) < min(b):
        return "slower, ranges do not overlap"
    return "no clear change, ranges overlap"


# An attempt counts only when its unchanged "before" build ran at the speed
# the baseline batch recorded: a slowed machine moves both builds and hides
# any difference. The check reads only the before entry, never the after.
VALID_BAND = 0.10


def validity(baseline, base):
    ref = statistics.median(baseline.values(UNTIMED))
    off = (statistics.median(base.values(UNTIMED)) - ref) / ref
    return None if abs(off) <= VALID_BAND else f"{100.0 * off:+.0f}% from the baseline"


def loads(entries):
    xs = [r["machine"]["load_average"][0] for e in entries for r in e.runs]
    return f"{min(xs):.2f} to {max(xs):.2f}"


def summary(batch):
    entries = load(batch)
    r0 = entries[0].runs[0]
    m, b = r0["machine"], r0["build"]
    print(f"{batch_dir(batch)}: {len(entries[0].runs)} runs per entry, seed {r0['seed']}, "
          f"{r0['measured_ops']:,} measured ops after {r0['warmup_ops']:,} warmup")
    print(f"machine: {m['cpu']}, {m['cores']}, {m['os']}")
    print(f"build: {b['compiler']}, {b['build_type']}, flags {b['flags']}")
    print(f"timer resolution: {r0['timer']['resolution_ns']} ns; "
          f"load average at run start: {loads(entries)}")
    print()
    print(f"{'entry':10} {'engine':6} {'commit':14} {'untimed Mops/s':22} "
          f"{'timed Mops/s':22} {'mean':>6} {'p50':>4} {'p99':>4} {'p99.9':>5}  zero-alloc runs")
    for e in entries:
        print(f"{e.name:10} {e.engine:6} {e.commit:14} {med_range(e.values(UNTIMED)):22} "
              f"{med_range(e.values(TIMED)):22} {e.lat('all', 'mean_ns'):6.1f} "
              f"{e.lat('all', 'p50_ns'):4.0f} {e.lat('all', 'p99_ns'):4.0f} "
              f"{e.lat('all', 'p999_ns'):5.0f}  {e.allocs_zero()} of {len(e.runs)}")
    for e in entries[1:]:
        print(f"\n{e.name} against {entries[0].name}: untimed throughput "
              f"{delta(entries[0], e):+.1f}% at the median; {verdict(entries[0], e)}")


# --- README blocks ----------------------------------------------------------

def headline(batch, name):
    e = next(x for x in load(batch) if x.name == name)
    return "\n".join([
        "| metric | value |",
        "|---|---|",
        f"| throughput | {mops(statistics.median(e.values(UNTIMED)))}M ops/s |",
        f"| throughput with a timestamp around every op | "
        f"{mops(statistics.median(e.values(TIMED)))}M ops/s |",
        f"| mean latency | {e.lat('all', 'mean_ns'):.1f} ns |",
        f"| p50 / p99 / p99.9 | {e.lat('all', 'p50_ns'):.0f} / {e.lat('all', 'p99_ns'):.0f}"
        f" / {e.lat('all', 'p999_ns'):.0f} ns |",
        f"| heap allocations in the measured region | zero in {e.allocs_zero()} of "
        f"{len(e.runs)} runs |",
    ]) + "\n"


def operations(batch, name):
    entries = load(batch)
    e = next(x for x in entries if x.name == name)
    r0 = e.runs[0]
    m, b = r0["machine"], r0["build"]
    lines = [
        "```",
        f"machine: {m['cpu']}, {m['cores']}, {m['os']}",
        f"build: {b['compiler']}, {b['build_type']}",
        f"flags: {b['flags']}",
        f"timer resolution: {r0['timer']['resolution_ns']} ns",
        "```",
        "",
        "| operation | mean ns | p50 | p99 | p99.9 |",
        "|---|---|---|---|---|",
    ]
    for k in KINDS:
        lines.append(f"| {k} | {e.lat(k, 'mean_ns'):.1f} | {e.lat(k, 'p50_ns'):.0f} | "
                     f"{e.lat(k, 'p99_ns'):.0f} | {e.lat(k, 'p999_ns'):.0f} |")
    u, t = e.values(UNTIMED), e.values(TIMED)
    lines += [
        "",
        f"Throughput, untimed pass: median {mops(statistics.median(u))}M ops/s, "
        f"runs from {mops(min(u))}M to {mops(max(u))}M. With a timestamp around "
        f"every op: median {mops(statistics.median(t))}M, runs from {mops(min(t))}M "
        f"to {mops(max(t))}M. Load average at the start of each run: {loads(entries)}. "
        f"Every run is in `results/{batch}/`, built from commit `{e.commit}`.",
    ]
    return "\n".join(lines) + "\n"


def attempts(baseline_batch, *batches):
    baseline = load(baseline_batch)[0]
    lines = [
        "| batch | change | before, Mops/s | after, Mops/s | median | "
        "load | verdict |",
        "|---|---|---|---|---|---|---|",
    ]
    for batch in batches:
        entries = load(batch)
        base, cand = entries[0], entries[-1]
        bad = validity(baseline, base)
        lines.append(
            f"| `{batch}` | {cand.note} | {med_range(base.values(UNTIMED))} | "
            f"{med_range(cand.values(UNTIMED))} | {delta(base, cand):+.1f}% | "
            f"{loads(entries)} | "
            f"{f'not valid: before build ran {bad}' if bad else verdict(base, cand)} |")
    return "\n".join(lines) + "\n"


def against(prev, e):
    clear = not verdict(prev, e).startswith("no clear")
    return f"{delta(prev, e):+.0f}%" + ("" if clear else ", ranges overlap")


def progression(batch):
    entries = load(batch)
    lines = [
        "| engine | change | untimed Mops/s | against previous | timed Mops/s | "
        "mean ns | p50 | p90 | p99 | p99.9 | allocs per op |",
        "|---|---|---|---|---|---|---|---|---|---|---|",
    ]
    prev = None
    for e in entries:
        allocs = statistics.median(e.values("heap_allocations")) / e.runs[0]["measured_ops"]
        lines.append(
            f"| {e.name} | {e.note} | {med_range(e.values(UNTIMED))} | "
            f"{'' if prev is None else against(prev, e)} | "
            f"{mops(statistics.median(e.values(TIMED)))} | {e.lat('all', 'mean_ns'):.1f} | "
            f"{e.lat('all', 'p50_ns'):.0f} | {e.lat('all', 'p90_ns'):.0f} | "
            f"{e.lat('all', 'p99_ns'):.0f} | {e.lat('all', 'p999_ns'):.0f} | {allocs:.3f} |")
        prev = e
    return "\n".join(lines) + "\n"


BLOCKS = {"headline": headline, "operations": operations,
          "attempts": attempts, "progression": progression}
BLOCK = re.compile(r"(<!-- BEGIN generated: (?P<args>[^>]*?) -->\n)(?P<body>.*?)"
                   r"(<!-- END generated -->)", re.S)


def readme(check):
    path = ROOT / "README.md"
    old = path.read_text()

    def fill(m):
        kind, *args = m.group("args").split()
        return m.group(1) + BLOCKS[kind](*args) + m.group(4)

    new = BLOCK.sub(fill, old)
    if check:
        if new != old:
            sys.exit("README.md is stale against results/; run scripts/results.py readme")
        print("README.md generated blocks match results/")
    elif new != old:
        path.write_text(new)
        print("README.md generated blocks rewritten")
    else:
        print("README.md generated blocks already current")


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "summary":
        summary(sys.argv[2])
    elif len(sys.argv) in (2, 3) and sys.argv[1] == "readme":
        readme(sys.argv[2:] == ["--check"])
    else:
        sys.exit(__doc__)
