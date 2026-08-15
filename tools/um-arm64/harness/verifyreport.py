#!/usr/bin/env python3
"""Turn a verifybench run into results.tsv and a table.

Deliberately dumb: it reads only the raw transcripts, writes every extracted
sample to results.tsv, and derives the table from that file. A reader who does
not trust the table can recompute it from results.tsv, and a reader who does not
trust results.tsv can recompute it from the transcripts sitting next to it.

Refuses to print a table when the validator fails, for the reason given in
verifybench.sh.
"""
import sys
import os
import re
import glob
import statistics

ROWS = ["compute", "syscall", "openat", "fault", "forkexec"]
VALIDATOR_MAX_SPREAD = 5.0      # percent, across conditions
VALIDATOR_MAX_JITTER = 8.0      # percent, within one condition


def samples(out, cond, row):
    got = []
    for f in sorted(glob.glob(os.path.join(out, cond + ".*")),
                    key=lambda p: int(p.rsplit(".", 1)[1])):
        rnd = int(f.rsplit(".", 1)[1])
        for line in open(f, errors="replace"):
            m = re.match(r"^" + row + r"\s+([0-9.]+)", line.strip())
            if m:
                got.append((rnd, float(m.group(1))))
                break
    return got


def main():
    out, rounds = sys.argv[1], int(sys.argv[2])
    conds = open(os.path.join(out, "conditions.txt")).read().split()

    data = {c: {r: samples(out, c, r) for r in ROWS} for c in conds}

    with open(os.path.join(out, "results.tsv"), "w") as fh:
        fh.write("condition\tround\tmetric\tvalue\tunit\n")
        for c in conds:
            for r in ROWS:
                unit = "ns_per_iter" if r == "compute" else "us_per_op"
                for rnd, v in data[c][r]:
                    fh.write("%s\t%d\t%s\t%g\t%s\n" % (c, rnd, r, v, unit))

    print("=== validator: compute, pure userspace, no kernel entry ===")
    med = {}
    bad_jitter = []
    for c in conds:
        v = [x for _, x in data[c]["compute"]]
        if not v:
            print("  %-18s NO DATA" % c)
            continue
        med[c] = statistics.median(v)
        jitter = (max(v) - min(v)) / min(v) * 100
        if jitter > VALIDATOR_MAX_JITTER:
            bad_jitter.append(c)
        print("  %-18s %7.3f ns/iter  n=%d  min %.3f max %.3f  jitter %.1f%%%s"
              % (c, med[c], len(v), min(v), max(v), jitter,
                 "  <== UNSTABLE" if jitter > VALIDATOR_MAX_JITTER else ""))

    if not med:
        print("\nNo data at all.")
        return 1

    lo, hi = min(med.values()), max(med.values())
    spread = (hi - lo) / lo * 100
    ok = spread < VALIDATOR_MAX_SPREAD and not bad_jitter
    print("  spread across conditions: %.1f%%  -> %s"
          % (spread, "VALID" if ok else "INVALID"))
    if not ok:
        print("\nThe conditions were not running on the same machine state, so\n"
              "nothing else measured here means anything. No table printed.")
        return 1

    print("\n=== medians of %d interleaved rounds (us/op) ===" % rounds)
    w = max(len(c) for c in conds) + 2
    print(" " * 10 + "".join("%*s" % (w, c) for c in conds))
    for r in ROWS[1:]:
        line = "%-10s" % r
        for c in conds:
            v = [x for _, x in data[c][r]]
            line += "%*s" % (w, "%.3f" % statistics.median(v) if v else "-")
        print(line)

    # Ratios against the first kernel condition, which is the oldest one.
    kern = [c for c in conds if c not in ("native", "proot")]
    if len(kern) > 1:
        for suffix in ("-sec", "-ptr"):
            group = [c for c in kern if c.endswith(suffix)]
            if len(group) < 2:
                continue
            base = group[0]
            print("\n=== %s: speedup vs %s ===" % (suffix.lstrip("-"), base))
            for r in ROWS[1:]:
                b = [x for _, x in data[base][r]]
                if not b:
                    continue
                bm = statistics.median(b)
                cells = []
                for c in group:
                    v = [x for _, x in data[c][r]]
                    cells.append("%s %.2fx" % (c, bm / statistics.median(v))
                                 if v else "%s -" % c)
                print("  %-10s %s" % (r, "   ".join(cells)))

    print("\nraw transcripts and results.tsv in", out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
