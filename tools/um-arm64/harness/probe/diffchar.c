// SPDX-License-Identifier: GPL-2.0
/*
 * Characterise a corrupted byte stream against a known-good reference.
 *
 * The gate-7 failure has two shapes -- a segfault, and output that liblzma
 * itself rejects as "compressed data is corrupt". The second shape is the more
 * informative of the two, because the damage is still in the file afterwards
 * and can be measured instead of guessed at.
 *
 * What the shape of the damage tells us:
 *
 *   runs of exactly PAGE_SIZE bytes, page-aligned, reading as zero
 *       a guest page was lost and re-faulted zero-filled -- a UML mm bug
 *
 *   runs of exactly PAGE_SIZE bytes, page-aligned, holding *other* plausible
 *   data
 *       a page was aliased to the wrong frame -- also a UML mm bug, but a
 *       different one
 *
 *   short runs (16, 32, 64 bytes) at arbitrary offsets
 *       a vector register was clobbered mid-memcpy -- an FP save/restore bug
 *
 *   a single run to end-of-file
 *       the stream simply stopped early; not corruption at all
 *
 * These are mutually exclusive predictions, which is the point: the existing
 * investigation has a long list of eliminated hypotheses and no discriminator.
 *
 * Deliberately reports run *lengths* and *alignments* rather than dumping the
 * bytes: a 60 MB diff is not readable, and the alignment is the whole signal.
 *
 * Build (runs in the guest, delivered over hostfs):
 *   clang --target=aarch64-linux-gnu -static -O2 -o diffchar diffchar.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#define CHUNK	(1 << 20)
#define MAXSHOW	12

int main(int argc, char **argv)
{
	unsigned char *a, *b;
	long long off = 0, bad = 0, runs = 0, zero_runs = 0, shown = 0;
	long long run_start = -1;
	int run_all_zero = 1;
	/* log2 histogram of run lengths */
	long long hist[26] = {0};
	long long align_page = 0, len_page = 0;
	long pgsz = sysconf(_SC_PAGESIZE);
	int fa, fb, i;

	if (argc != 3) {
		fprintf(stderr, "usage: %s <reference> <suspect>\n", argv[0]);
		return 2;
	}
	fa = open(argv[1], O_RDONLY);
	fb = open(argv[2], O_RDONLY);
	if (fa < 0 || fb < 0) {
		fprintf(stderr, "open failed\n");
		return 2;
	}
	a = malloc(CHUNK);
	b = malloc(CHUNK);
	if (!a || !b)
		return 2;

	for (;;) {
		ssize_t na = read(fa, a, CHUNK);
		ssize_t nb = read(fb, b, CHUNK);
		ssize_t n = na < nb ? na : nb;

		if (n <= 0) {
			printf("eof at %lld (ref_left=%zd sus_left=%zd)\n",
			       off, na, nb);
			break;
		}
		for (i = 0; i < n; i++) {
			if (a[i] != b[i]) {
				bad++;
				if (run_start < 0) {
					run_start = off + i;
					run_all_zero = 1;
				}
				if (b[i])
					run_all_zero = 0;
			} else if (run_start >= 0) {
				long long len = (off + i) - run_start;
				int lg = 0;

				runs++;
				while ((1LL << (lg + 1)) <= len && lg < 25)
					lg++;
				hist[lg]++;
				if (run_all_zero)
					zero_runs++;
				if (run_start % pgsz == 0)
					align_page++;
				if (len == pgsz)
					len_page++;
				if (shown < MAXSHOW) {
					printf("run %lld: off=%lld len=%lld "
					       "off%%pg=%lld zero=%d\n",
					       runs, run_start, len,
					       run_start % pgsz, run_all_zero);
					shown++;
				}
				run_start = -1;
			}
		}
		off += n;
		if (na != nb)
			break;
	}
	if (run_start >= 0) {
		runs++;
		if (run_all_zero)
			zero_runs++;
		printf("run %lld: off=%lld len=%lld (to eof) zero=%d\n",
		       runs, run_start, off - run_start, run_all_zero);
	}

	printf("pagesize=%ld compared=%lld badbytes=%lld runs=%lld "
	       "zero_runs=%lld page_aligned_runs=%lld page_len_runs=%lld\n",
	       pgsz, off, bad, runs, zero_runs, align_page, len_page);
	printf("runlen histogram (2^n):");
	for (i = 0; i < 26; i++)
		if (hist[i])
			printf(" %d:%lld", i, hist[i]);
	printf("\n");
	return bad ? 1 : 0;
}
