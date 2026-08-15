# What a UML guest costs, and how to measure it without fooling yourself

## The claim that needs splitting in two

"UML runs at native speed" is half true, and the false half is the half people
notice.

Guest **computation** is native. Guest instructions execute directly on the CPU
with nothing in between -- no emulation, no translation, no hypervisor.

Guest **kernel entries** are not native, and cannot be. Every syscall and every
page fault has to be intercepted and serviced by the UML kernel, which is
another process. A shell running `ls` is almost entirely kernel entries, so it
pays that cost on nearly every operation, and that is what "Debian feels slow"
actually is.

The project's own syscall census makes the size of it concrete: of 8441 host
syscalls issued during a boot, **5252 are `ptrace` and 1143 are `wait4` -- 76%
of everything the binary asks of its host is the cost of intercepting guest
syscalls.**

## The first measurement was worthless, and why

The first table looked like this:

| | native | ptrace | seccomp |
| --- | ---: | ---: | ---: |
| compute (ns/iter) | 1.270 | 6.012 | **0.838** |

`compute` is pure userspace arithmetic with no kernel entry at all. It *must* be
identical in all three columns -- the same instructions on the same CPU. A
sevenfold spread means the measurement is invalid, and the guest coming out
**faster than native** is not a result, it is proof that the three runs did not
happen under the same conditions.

The cause was the hardware: a Snapdragon 870 is big.LITTLE -- four Cortex-A55 at
1.80 GHz, three A77 at 2.42 GHz, one A77 at 3.19 GHz -- running the `schedutil`
governor. Unpinned runs landed on different clusters at different clock speeds
and different temperatures. The table was measuring the scheduler.

**`compute` is therefore the validator.** Until it agrees across every column to
within a couple of percent, no other row in the table means anything, because
the machine was demonstrably not in the same state when the other rows were
taken. `harness/phonebench.sh` enforces this: it prints the compute spread first
and labels the run INVALID if it exceeds 5%.

Three rules follow, and they are in the script:

1. **Pin to matched cores.** Cores 4-6, the three A77s that share a maximum
   frequency -- not core 7, which boosts higher, and not 0-3, which are a
   different microarchitecture. Affinity is inherited, so pinning the UML
   process also pins its stub children, which additionally keeps the kernel
   thread and the stub on one cluster.
2. **Interleave the conditions, do not run them in blocks.** Thermal drift then
   hits every column equally instead of penalising whichever ran last.
3. **Median of several rounds**, not a single run.

With pinning, compute converges to 0.892 / 0.892 / 0.896 / 0.894 ns across
native, proot, UML-ptrace and UML-seccomp -- a 0.45% spread. That is the number
that licenses reading the rest of the table, and it is also the direct evidence
that **guest computation has no overhead at all**.

## The comparison that matters is not native

Native shows the physical floor, but nobody chooses between this kernel and bare
metal. On a phone the real alternative is **proot** -- the ptrace/seccomp-based
chroot that Termux's `proot-distro` uses to run Debian without root. So proot is
a column in the table.

One trap in comparing them, worth stating because it would otherwise flatter
proot by a factor of a hundred: **proot only intercepts the syscalls it has to
rewrite.** It installs a seccomp filter and traps path-taking calls so it can
translate them; `getppid()` is not one of those and runs at full native speed
under proot. UML has no such option -- it *is* the kernel, so it must service
every syscall the guest makes.

Measuring on `getppid()` alone therefore compares proot's do-nothing path
against UML's do-everything path. The benchmark includes an `openat()` row for
that reason: a path-taking syscall is real work for both, and it is the honest
common ground.

## The measured table

Poco F3, Snapdragon 870, cores 4-6 pinned, 7 interleaved rounds, medians.
Validator spread 0.2% -- VALID.

| | native | proot | UML ptrace | UML seccomp |
| --- | ---: | ---: | ---: | ---: |
| compute (ns/iter) | 0.892 | 0.892 | 0.894 | 0.894 |
| syscall (us) | 0.079 | 0.168 | 43.68 | 22.01 |
| **openat (us)** | 2.47 | **49.67** | 89.05 | **49.12** |
| fault (us) | 1.47 | 1.34 | 71.25 | 51.19 |
| forkexec (us) | -- | 2275 | 4349 | 3573 |

Read it as four separate findings:

* **Guest computation is exactly native.** 0.894 against 0.892 -- a 0.2% spread
  across all four columns. There is no overhead on guest instructions, and no
  amount of tuning is needed there.
* **On the syscall both systems must handle, UML seccomp matches proot**: 49.12
  vs 49.67 us for `openat()`. That is the fair comparison, and it is a tie.
* **On syscalls and page faults proot wins enormously** -- 0.168 vs 22 us, 1.34
  vs 51 us -- because proot does not implement a kernel. It filters for the
  syscalls it must rewrite and lets the host kernel do everything else, so
  `getppid()` and every page fault are native. UML has no such option: it *is*
  the kernel, so it services every entry.
* **fork+exec: proot is about 1.6x faster** (2275 vs 3573 us).

So the honest summary is not "UML is slow". It is: computation is free, the
kernel boundary is not, and what UML buys for that boundary cost is a real
kernel -- loadable modules, real block devices, its own scheduler and VM,
namespaces -- none of which proot has, because proot is a path rewriter.

## Context switches: it is the handoff, not the scheduler

Counted across the UML process and its stub children during a benchmark run:

| mode | voluntary | nonvoluntary |
| --- | ---: | ---: |
| seccomp | 37,213 | **5** |
| ptrace | 76,371 | **9** |

Nonvoluntary is effectively zero, so with pinning the scheduler is not
preempting anything: the cost is entirely *voluntary* handoffs, i.e. the futex
round trip between the stub and the UML kernel thread. Roughly 1.5 per guest
syscall under seccomp and 3 under ptrace, and the 2x ratio in switches is
exactly the 2x ratio in time. That is the mechanism, and it is inherent to
having the kernel in another process.

## Two measurements that were wrong, and how they were caught

**A "4x speedup" that was a failure.** `./deb` appeared to drop from 3.0s to
0.83s. It had not got faster; a stale UML process still held the flock on the
disk image, so the run aborted during root mount and returned early. **A failed
run is faster than a successful one**, which makes silent failure the most
dangerous thing in a benchmark -- it does not add noise, it fabricates
improvement. `harness/debbench.sh` now checks every run's output against the
expected value and reports failures instead of timing them.

**A microbenchmark win that reversed on real work.** Pinning everything to one
core halves syscall cost in the loop (22 -> 11 us), for a good reason: the futex
handoff becomes a direct context switch with the shared page still in L1. On
`dpkg -l | wc -l`, over seven verified runs each, it is a *regression*: 2.75s
unpinned against 3.17s pinned. Real work also has guest computation and ubd I/O,
which then contend for the one core. Pinning is therefore available via
`UMDEB_MASK` and is not the default.

The Debian workload, same protocol, is where the shipping default came from:

| configuration | median of 7 verified runs |
| --- | --- |
| seccomp=off, unpinned | 3.06 s |
| seccomp=off, pinned | 3.18 s |
| **seccomp=on, unpinned** | **2.75 s** |
| seccomp=on, pinned | 3.17 s |

Note also what dominates that 2.75s: `./deb <command>` boots an entire kernel
per invocation. Interactive mode boots once, so steady-state commands do not pay
it.

## What the rows mean

* `compute` -- validator, and the "native speed" claim. Pure arithmetic.
* `syscall` -- `getppid()`, the cheapest syscall there is, so it is almost pure
  interception overhead. The right question for UML, a misleading one for proot.
* `openat` -- a path-taking syscall. Work for both.
* `fault` -- touching fresh anonymous pages. This is what makes program startup
  slow, since every fault is a round trip to the UML kernel.
* `forkexec` -- `fork()` + `execve()` + `wait()`. What a shell script does all
  day. Note this row is meaningless for the *native* column on Android, since
  there is no `/bin/true` there and it measures a failed exec.
