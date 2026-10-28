#!/usr/bin/env python3
"""Where the benchmark's measured loop spends its time, per source line.

  scripts/profile.py [--build DIR] [--runs N] [--frame NAME]

Runs `bench final` (the README configuration: 2M ops after 100k warmup) N
times under macOS `sample` (one stack sample per millisecond per run),
keeps only the stacks inside FRAME, and prints self samples per source line,
summed over all runs. FRAME is run_ops_untimed (default, the throughput
pass) or run_ops_gated (the timed latency pass). Inlined engine code is
reported at the harness line that called it. macOS only: on Linux, use
`perf record` and `perf report --sort srcline` for the same view.
"""
import argparse
import collections
import pathlib
import re
import subprocess
import tempfile

ROW = re.compile(r"^(?P<indent>[ +!:|]*)(?P<count>\d+) (?P<frame>.*)$")
SRC = re.compile(r"\s([\w.]+\.(?:hpp|cpp|h)):(\d+)\s*$")


def rows(path):
    out = []
    for line in path.read_text().splitlines():
        if line.startswith("Total number in stack"):
            break
        m = ROW.match(line)
        if m:
            out.append((len(m.group("indent")), int(m.group("count")), m.group("frame")))
    return out


def self_samples(tree, frame):
    """Yields (source line, function, self count) for rows under `frame`."""
    region = None  # depth of the frame row while inside its subtree
    for i, (depth, count, text) in enumerate(tree):
        if region is not None and depth <= region:
            region = None
        if region is None:
            if frame in text.split("  (in ")[0]:
                region = depth
            else:
                continue
        child_depth, children = None, 0
        for d, c, _ in tree[i + 1:]:
            if d <= depth:
                break
            child_depth = child_depth or d
            if d == child_depth:
                children += c
        if count > children:
            src = SRC.search(text)
            fn = re.sub(r"<.*>", "<>", text.split("  (in ")[0].split("(")[0]).strip()
            yield (f"{src.group(1)}:{src.group(2)}" if src else "?"), fn, count - children


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("--build", default="build")
    ap.add_argument("--runs", type=int, default=50)
    ap.add_argument("--frame", default="run_ops_untimed")
    a = ap.parse_args()
    bench = pathlib.Path(a.build) / "bench"
    totals = collections.Counter()
    with tempfile.TemporaryDirectory() as tmp:
        for i in range(a.runs):
            out = pathlib.Path(tmp) / f"s{i}.txt"
            p = subprocess.Popen([bench, "final", "--ops", "2000000", "--warmup", "100000",
                                  "--csv", "/dev/null"], stdout=subprocess.DEVNULL)
            subprocess.run(["sample", str(p.pid), "5", "1", "-mayDie", "-file", out],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            p.wait()
            for src, fn, n in self_samples(rows(out), a.frame):
                totals[(src, fn)] += n
    total = sum(totals.values())
    print(f"{a.runs} runs of `{bench} final --ops 2000000 --warmup 100000` under `sample`; "
          f"{total} self samples inside {a.frame}")
    print(f"{'share':>6} {'samples':>7}  {'source line':24} function")
    for (src, fn), n in totals.most_common(25):
        print(f"{100.0 * n / total:5.1f}% {n:7d}  {src:24} {fn}")


if __name__ == "__main__":
    main()
