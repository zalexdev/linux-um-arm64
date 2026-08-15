# Gate 7: the dpkg-deb decompressor corruption

**Root cause found and fixed** — see "The answer" below. The rest of this file is
kept as it was written, because the eliminations are what made the answer
findable and because several of them are useful evidence in their own right.

This records what is known, what has been eliminated, and what to try next,
so the investigation does not have to be restarted from the symptom.

## Symptom

`apt install build-essential` fails because `dpkg-deb`'s `<decompress>`
subprocess dies:

    dpkg-deb[57]: segfault at 386625 ip 4029d154 ... in libc.so.6[9d154,40200000+18c000]
    dpkg-deb[72]: segfault at aa5555635000 ip 400da3f8 ... error 92000047 in liblzma.so.5.4.1
    dpkg-deb (subprocess): ... lzma error: compressed data is corrupt

Two failure shapes, both from the same cause: a fatal fault, or silently wrong
data that liblzma then rejects. Wrong data is the more alarming of the two.

## The crash, from a core

Core captured in the guest (`/proc/sys/kernel/core_pattern` pointed at hostfs)
and read with the run domain's aarch64 gdb:

    pc  = 0x4029d170     -> libc + 0x9d170, the byte-copy tail of memcpy
    x0  = 0x38c395       (dst)
    x1  = 0x38c385       (src)
    x2  = 3              (len)
    x3  = 0xaa5555625230 (a plausible guest heap pointer)
    x30 = 0x400dc664     -> liblzma + 0x1c664

`dst` and `src` are 16 bytes apart and about 3.7 MB in magnitude: they are LZ
dictionary *offsets*, with the dictionary base pointer missing. Another register
still holds what looks like the correct base. So a pointer that should have been
`base + offset` arrived as `offset` alone.

The faulting instruction is `ldrb w6, [x1]` -- a plain integer load. Nothing in
the crash involves FP, which rules out the first hypothesis.

## Rate

Not a threshold; a probability that grows with the volume decompressed.

| Input | Result |
| --- | --- |
| `gcc-12` deb, 16 MB | 0/8 and 0/30 -- fails every time |
| 1336-byte deb | 8/8 pass in one run, 2/3 in another -- rare but nonzero |
| `dpkg-deb --info` (small control tarball, same fork path) | 8/8 pass |

## Eliminated

Each of these was tested directly, not reasoned away.

| Hypothesis | Test | Result |
| --- | --- | --- |
| hostfs corrupts reads | md5 of the archive from hostfs vs a copy on ext4, plus 5 re-reads | identical, stable; **fails from ext4 too** |
| liblzma itself | `ar p … \| xz -dc` on the same member | 8/8 and 30/30 pass |
| generic fork + pipe + compression | gzip round trip through two pipes | 40/40 pass |
| non-determinism in userspace | sha256 of a fixed string, 60x | 60/60 identical |
| copy-on-write | `mmtorture`: 24 rounds x 64 pages, private vs shared, parent/child tags | pass |
| COW at dpkg's scale | same, 8 rounds x 4096 pages (16 MB) | pass |
| `mprotect` transitions | RW -> RO -> RW with data checks | pass |
| TLS across `fork()` | TPIDR_EL0 in parent vs child | identical |
| TLS across threads | `threadtorture`: 8 threads, `CLONE_SETTLS`, `CLONE_CHILD_CLEARTID` | pass |
| integer registers across involuntary preemption | `regtorture`: 4 concurrent CPU-bound children, x19-x28 verified continuously | pass |
| FP registers across involuntary preemption | same loop, v8-v19 verified every iteration | pass |
| FP/FPCR/FPSR across syscalls, task switches, 500 signals | `fptorture` | pass |
| glibc's SIMD string routines | rerun with `GLIBC_TUNABLES=glibc.cpu.name=generic` | **still fails** |

## Decisive: it is a UML bug, not emulation

`dpkg-deb --fsys-tarfile` on the same 16 MB archive, run **natively in the same
qemu TCG run domain** (not under UML): **8/8 pass**. Under UML: 0/8. The run
domain, the archive, the dpkg version and the CPU emulation are all held
constant, so the fault is in UML.

## A correction to earlier evidence

The original "liblzma is fine" control was **vacuous**: `ar p "$D" data.tar.xz`
named a member that `ar` did not emit, so `xz -dc` succeeded on empty input.
Redone properly -- extract the real `data.tar.xz` (16 298 904 bytes) and
decompress it -- standalone `xz -dc` gives 60 211 200 bytes, 8/8 successes and
6/6 identical checksums. liblzma under UML is genuinely fine when exec'd.

## The minimal reproducer does not reproduce

A ~90-line C program compiled *inside the guest* reproducing dpkg-deb's exact
shape -- parent opens the payload and forks; the child never execs and runs
`lzma_stream_decoder` in-process, writing to a pipe; the parent drains and
checksums it -- runs **6/6 clean with identical hashes**, 60 MB per iteration.

So "fork + in-process liblzma + pipe" is not sufficient to trigger it. Whatever
dpkg-deb does additionally is still the unknown. (This program is also a working
demonstration of gate 9: compiled by the guest's own gcc, linked against a shared
library, 360 MB of correct work.)

## Also eliminated

| Hypothesis | Test | Result |
| --- | --- | --- |
| guest memory pressure | same run at `mem=512M` and `mem=3072M` | 0/8 both; MemFree moves by ~1.3 MB |
| unchecked allocation failure | as above | no correlation with available memory |
| the archive is `mmap`ed | fed via a pipe (`/dev/stdin`, not mappable) | fails identically, 0/6 |
| input method generally | regular file, pipe, and `dpkg-deb -x` | all 0/6 |
| bulk user-copy (`copy_to_user`) | `uacctorture`: 48 MB through a pipe from a forked child at odd alignments and non-page-multiple chunks, byte-exact | 0 bad chunks |

## A real bug found here, and fixed

`strace -f` inside the guest (strace extracted on the host and delivered over
hostfs, since installing it would need the broken dpkg) showed:

    64  read(4, "...", 32768) = 32768
    65  --- SIGSEGV {si_addr=0x1e53d} ---
    64  syscall_0x8000(0, 0xaa55556143c0, 0x8000, 0x1, 0, 0)

A guest thread issuing syscall **0x8000 = 32768** -- the byte count of the
`read()` it was in. The same trace shows futexes on shared addresses, i.e.
dpkg-deb's decompressor is **multi-threaded** in this configuration, which is
why a single-threaded reproduction could never trigger it: all threads of an mm
share one stub process.

Cause: `put_host_regs()` wrote `NT_ARM_SYSTEM_CALL` as part of restoring
registers. On arm64 that regset selects the syscall *in flight*, not thread
state, so restoring thread A while the stub was stopped in thread B's syscall
entry rewrote B's syscall number. Fixed; afterwards the bogus syscall number no
longer appears in an strace of the same workload (`bogus_syscalls: 0`).

`UPT_RESTART_SYSCALL` was hardened at the same time: the saved slot is -1
outside a syscall stop, and writing that into x8 would re-execute the svc with a
number nobody asked for.

This did not make gate 7 pass. It removed one of the two failure modes.

## The remaining failure

Two signatures, both **write** faults (ESR WnR=1), repeating exactly:

    segfault at <small offset>  ip liblzma+0x1aaf8  error 92000045  (DFSC 0x05, translation fault L1)
    segfault at 0xaa5555635000  ip liblzma+0x1a3f8  error 92000047  (DFSC 0x07, translation fault L3)

The second is always the *same page-aligned address*. The strace shows futex
words at 0xaa5555624fa8, so that region is genuinely mapped in dpkg-deb -- it is
the heap of a PIE binary. The guest is writing to a heap page it believes it
owns and the stub does not have.

## Also eliminated (second pass)

| Hypothesis | Test | Result |
| --- | --- | --- |
| `copy_to_user` into never-touched pages | 64 rounds, fresh mmap per round, 256 KB read from a pipe without pre-touching, byte-exact | pass, and 6/6 whole-suite runs |
| `brk` growth, parent | 16 rounds x 512 KB, every page written and read back | pass |
| `brk` growth in a forked child | child grows brk further, writes its own and the inherited pages | pass |
| COW over brk pages | parent's copies verified unchanged after the child rewrites them | pass |

## What was thought to be left, and why every item was wrong

Kept verbatim in spirit, because the list is a good record of how a plausible
hypothesis space can be entirely disjoint from the answer. At the time the
distinguishing property looked like "a child created by `fork()` that does not
`exec()`, decompressing a large volume, while its parent reads from a pipe",
and the four candidates were: a freestanding replica of dpkg-deb's shape; the
pipe path at volume; `brk` in a forked child; and UML's `copy_to_user` for
page-crossing transfers. `copy_to_user` was rated the strongest fit, because
"probability proportional to bytes moved" matched it exactly.

It was not any of them. The rate was proportional to bytes moved because more
bytes means more syscalls and more page faults, and *each syscall stop* was the
event that destroyed the register -- not because any byte-moving code was wrong.
`diffchar` later showed the bytes that did get moved were perfect.

Every candidate on that list is a *subsystem*. The bug was in the mechanism by
which UML observes a guest at all, which is upstream of every subsystem and
therefore invisible to a test of any one of them.

## Why this was worth finishing rather than working around

The two shapes of this bug were a crash and what looked like silently wrong
data. A port that boots, runs a distro and passes every gate except one is still
not usable if it can corrupt a byte stream under load -- and this appeared at a
rate of roughly one in three small operations and every large one.

That judgement turned out to be more right than it was at the time. The bug was
not in dpkg, or in decompression, or in anything gate 7 is about: any guest
holding a value in x7 across a syscall was affected, which is every guest thread
in every multi-threaded program. Gate 7 was simply the first workload heavy
enough to make it certain rather than occasional. Working around it would have
meant shipping a port that silently corrupts a register.


## The answer: arm64 hides x7 at a ptrace syscall stop

### What the crash actually said

Disassembling the faulting instruction rather than reasoning about the symptom:

    1aaf8: strb w2, [x7, x0]        <- liblzma, the site of every write fault
    1ab5c: ldurb w0, [x0, #-0x1]    <- with x0 = x7 + x0 a few instructions earlier

`x7` is written **nowhere** in the 0x400 bytes around it: it is the LZ dictionary
base, loaded once and held in a register for the whole decode loop. So the small
faulting addresses -- 0x8001, 0x128ff0, 0x3d1ff0 -- are dictionary *offsets* with
the base missing, which the guest kernel confirms by reporting that they belong
to no VMA at all (`segvdbg`, a temporary patch to `show_segv_info`).

Two further measurements pinned it as register loss rather than memory
corruption:

* **the output is not corrupt.** `harness/probe/diffchar.c` compared the longest
  UML-produced stream against a reference decompressed on the host:
  `badbytes=0` over all 1 114 112 bytes. The decompressor emits perfectly correct
  data and then dies. The "silently wrong data" shape was truncation.
* **the death is asynchronous.** Bytes produced before dying across eight runs:
  16384, 0, 1114112, 557056, 425984, 0, 98304, 524288. No data dependence.
* **the fault address tracks the volume decompressed**, ratio 3.5-3.9x and
  consistent, which is what "address == dictionary position, base == 0" predicts.

### The mechanism

arm64's ptrace tells a tracer whether a stop came from syscall entry or syscall
exit by **overwriting a general-purpose register in the tracee** -- x7 on
AArch64 -- and restoring the tracee's own value once the stop ends.
`ptrace_save_reg()` in `arch/arm64/kernel/ptrace.c` says so, and lists the
consequences: tracer writes to that register during the stop are discarded, and
the real value is not available while stopped. `PTRACE_SYSCALL_ENTER` is 0, which
is exactly the value that kept appearing in x7.

Measured against the live host (`harness/probe/hostx7.c`, host 6.12.101):

    at syscall-entry stop: x7 reads as 0x0 (tracee set 0x1234567890abcdef)
    after writing 0xdeadbeef, readback at the same stop: 0xdeadbeef
    at the next syscall stop: x7 = 0x0
    at a single-step SIGTRAP stop: x7 = 0x1234567890abcdef  (the tracee's real value)
      write at a step stop reads back as 0x5555 -> WRITABLE

x86 has no equivalent, so nothing in `arch/um` expected it. UML reads the guest's
registers at a stop, keeps them, and writes them back later -- and every thread of
a guest mm shares one stub process, so a thread switch means installing a
different thread's registers into that stub. Both halves break:

* the **read** stores 0 into UML's copy of the thread's x7;
* the **write** is discarded, so the stub keeps whichever thread ran last.

### Reproduced in seconds, without dpkg

`harness/probe/regsurvive.c` holds a known value in all 24 registers a syscall
must preserve (x1-x7, x9-x15, x19-x28) across page faults and `sched_yield` in
four threads of one mm. Before the fix:

    REGSURVIVE_FAIL thread=0 after=0
      x7 want=0x5a5a000000000007 got=0x0000000000000000
    REGSURVIVE_FAIL thread=3 after=0
      x7 want=0x5a5a000300000007 got=0x5a5a000200000007

Thread 3 receiving thread 2's value is the leak, in one line. Note what the
existing probes checked: x19-x28 and v8-v15, the registers a *compiler* must
preserve. This failure is in exactly the set they do not cover, which is why
`regtorture`, `threadtorture` and `fptorture` all passed.

### The fix

`harness/probe/hostx7b.c` measured the escape route before any kernel code was
written, because the fix depends on four separate claims:

    sysemu entry stop: x7=0x0 (real value is 0x1234567890abcdef) pc=0x400c90
    after PTRACE_SINGLESTEP: sig=5 (SIGTRAP)
      x7 = 0x1234567890abcdef  == the tracee's real value
      pc = 0x400c90  unchanged: no guest instruction was executed
      tracee observed x7 as 0xc0ffee0badf00d after resume -> WRITE TOOK EFFECT

So `userspace()` reads the whole register set at the syscall stop as before,
then single-steps off it and takes **only** x7 from the resulting pseudo-step
stop, which also leaves the task parked somewhere the full set can be written.

Taking only that one register is not fastidiousness. The pseudo-step arrives as a
*forced* SIGTRAP, so the host's signal path runs before the tracer sees it, and
`do_signal()` calls `forget_syscall()` -- the first version of the fix read
everything at the step stop, got `scno=-1` for every syscall, and hung the guest
with -ENOSYS. The same path would rewind the program counter if the first syscall
argument happened to look like -ERESTARTSYS.

Guarded by `UM_SYSCALL_STOP_HIDES_REG`, which only arm64 defines; x86 is
untouched. SECCOMP mode never had the problem -- the arm64 comment notes that
seccomp and pseudo-step traps nobble nothing -- which is an argument for it being
the fast path, but it does not currently come up on arm64 (see doc/20).

### What was wrong with how this was hunted

The eliminations above are all sound, and none of them could have found this: they
were tests of *subsystems* (COW, uaccess, TLS, FP, hostfs) chosen because each was
plausible. The thing that actually located it was disassembling the faulting
instruction and asking which register held the missing value -- available from the
first crash log, and three days of subsystem tests were run before anyone looked.
