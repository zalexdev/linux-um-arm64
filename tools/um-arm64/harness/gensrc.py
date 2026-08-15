#!/usr/bin/env python3
"""Generate a deterministic, non-trivial C source set for the bit-identity test.

The point is to make the compiler work hard and touch a lot of the machine:
integer and floating-point arithmetic, arrays the vectoriser will attack,
structs passed by value, switch tables, recursion, string handling, and enough
of it that register allocation and scheduling have real decisions to make. Any
divergence between two runs of the same compiler then has somewhere to show.

Deterministic by construction: no timestamps, no randomness at generation time,
identical bytes on every invocation.
"""
import sys, pathlib

NFILES = 24
NFUNCS = 24


def gen(idx: int) -> str:
    out = [
        "/* generated; see harness/gensrc.py */",
        "#include <stdint.h>",
        "#include <string.h>",
        "#include <math.h>",
        "",
        "struct blob%d { double d[8]; int32_t i[8]; char name[24]; };" % idx,
        "",
    ]
    for f in range(NFUNCS):
        seed = idx * 1000 + f
        out.append(f"""
double m{idx}_{f}(const double *a, const int32_t *b, int n)
{{
	double acc = {seed}.5, t = 0.0;
	int i;

	for (i = 0; i < n; i++) {{
		t = a[i] * {1.0 + f * 0.125} + (double)b[i % 8];
		acc += t * t - a[(i + {f}) % 8];
		if (b[i % 8] & 1)
			acc = fma(acc, 1.0000001, -0.5);
	}}
	return acc;
}}

int32_t s{idx}_{f}(struct blob{idx} v, int k)
{{
	int32_t r = {seed};
	int i;

	switch (k & 7) {{
	case 0: for (i = 0; i < 8; i++) r += v.i[i] * {f + 1}; break;
	case 1: for (i = 0; i < 8; i++) r ^= (int32_t)v.d[i]; break;
	case 2: r = (int32_t)strlen(v.name) * {f + 3}; break;
	case 3: for (i = 7; i >= 0; i--) r = r * 31 + v.i[i]; break;
	case 4: r += (int32_t)(v.d[0] / (v.d[1] + 1.0)); break;
	case 5: memmove(v.name, v.name + 1, 20); r += v.name[0]; break;
	case 6: for (i = 0; i < 8; i++) r |= v.i[i] << (i & 3); break;
	default: r = -r; break;
	}}
	return r;
}}

int32_t r{idx}_{f}(int32_t n)
{{
	if (n <= 1)
		return {f + 1};
	return r{idx}_{f}(n - 1) + r{idx}_{f}(n - 2) % {seed % 97 + 3};
}}""")
    return "\n".join(out) + "\n"


def main():
    d = pathlib.Path(sys.argv[1])
    d.mkdir(parents=True, exist_ok=True)
    names = []
    for i in range(NFILES):
        p = d / f"gen{i:02d}.c"
        p.write_text(gen(i))
        names.append(p.name)
    (d / "main.c").write_text(
        "/* generated */\n#include <stdio.h>\nint main(void){ printf(\"ok\\n\"); return 0; }\n")
    print(f"{len(names) + 1} files, {sum((d / n).stat().st_size for n in names)} bytes")


main()
