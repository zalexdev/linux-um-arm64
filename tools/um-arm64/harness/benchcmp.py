#!/usr/bin/env python3
"""Compare two phonebench runs side by side.

Two rules, both learned the hard way on this project:

  * Refuse to print anything if either run's `compute` validator is out of
    spec. A run whose pure-userspace arithmetic disagrees across conditions was
    not measuring the software, and comparing it to anything is worse than
    having no number at all.

  * Print native and proot as well as the two UML columns, and flag it loudly
    when those move between runs. They are the control: a change that shows up
    in native too is the machine changing, not the kernel. A first attempt at
    this comparison showed UML twice as fast and would have been reported as a
    win, when native fork/exec had also halved -- the phone had four runaway
    processes on it during the earlier run.

Usage: benchcmp.py <old-dir> <new-dir> [rounds] [--label-old L] [--label-new L]
"""
import sys
import re
import os
import glob
import statistics

CONDS = [("native", "native"), ("proot", "proot"),
         ("umlptrace", "UML ptrace"), ("umlseccomp", "UML seccomp")]
ROWS = ["compute", "syscall", "openat", "fault", "forkexec"]
VALIDATOR_MAX_SPREAD = 5.0	# percent


def vals(out, cond, row):
    got = []
    for f in sorted(glob.glob(os.path.join(out, cond + ".*"))):
        for line in open(f, errors="replace"):
            m = re.match(r"^" + row + r"\s+([0-9.]+)", line.strip())
            if m:
                got.append(float(m.group(1)))
                break
    return got


def load(out):
    return {c: {r: vals(out, c, r) for r in ROWS} for c, _ in CONDS}


def validate(data, name):
    comp = {c: statistics.median(data[c]["compute"])
            for c, _ in CONDS if data[c]["compute"]}
    if not comp:
        print("%s: no compute data at all" % name)
        return None
    lo, hi = min(comp.values()), max(comp.values())
    spread = (hi - lo) / lo * 100
    ok = spread < VALIDATOR_MAX_SPREAD
    print("%-10s compute %.3f-%.3f ns/iter, spread %.1f%%  -> %s"
          % (name, lo, hi, spread, "VALID" if ok else "INVALID"))
    return spread if ok else None


def med(data, cond, row):
    v = data[cond][row]
    return statistics.median(v) if v else None


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    old_dir, new_dir = args[0], args[1]
    lo_name, ln_name = "old", "new"
    for i, a in enumerate(sys.argv):
        if a == "--label-old":
            lo_name = sys.argv[i + 1]
        if a == "--label-new":
            ln_name = sys.argv[i + 1]

    old, new = load(old_dir), load(new_dir)

    print("=== validators ===")
    a = validate(old, lo_name)
    b = validate(new, ln_name)
    if a is None or b is None:
        print("\nOne or both runs are invalid. Nothing below would mean "
              "anything, so nothing is printed.")
        return 1

    # The controls. If these moved, the machine moved.
    print("\n=== controls (native/proot) -- these must agree between runs ===")
    drifted = []
    for cond, label in CONDS[:2]:
        for row in ROWS[1:]:
            o, n = med(old, cond, row), med(new, cond, row)
            if o and n:
                d = (n - o) / o * 100
                flag = "  <== MOVED" if abs(d) > 15 else ""
                if abs(d) > 15:
                    drifted.append("%s/%s %+.0f%%" % (label, row, d))
                print("  %-12s %-9s %10.3f -> %10.3f  (%+.1f%%)%s"
                      % (label, row, o, n, d, flag))

    print("\n=== UML: %s vs %s ===" % (lo_name, ln_name))
    print("%-10s %12s %12s %10s   %12s %12s %10s"
          % ("", lo_name + " ptr", ln_name + " ptr", "change",
             lo_name + " sec", ln_name + " sec", "change"))
    for row in ROWS[1:]:
        cells = []
        for cond in ("umlptrace", "umlseccomp"):
            o, n = med(old, cond, row), med(new, cond, row)
            if o and n:
                cells += ["%.3f" % o, "%.3f" % n, "%+.1f%%" % ((n - o) / o * 100)]
            else:
                cells += ["-", "-", "-"]
        print("%-10s %12s %12s %10s   %12s %12s %10s" % (row, *cells))

    print("\n=== seccomp vs ptrace, within each run ===")
    for name, d in ((lo_name, old), (ln_name, new)):
        parts = []
        for row in ROWS[1:]:
            p, s = med(d, "umlptrace", row), med(d, "umlseccomp", row)
            if p and s:
                parts.append("%s %.2fx" % (row, p / s))
        print("  %-8s %s" % (name, "  ".join(parts)))

    if drifted:
        print("\nWARNING: the control columns moved (%s). The machine was not "
              "in the same state for both runs, so the UML deltas above are "
              "not attributable to the kernel alone." % ", ".join(drifted))
    else:
        print("\nControls agree: the UML deltas above are attributable to the "
              "change under test.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
