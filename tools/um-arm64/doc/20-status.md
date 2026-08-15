# Status

Tree: `linux/` on branch `um-arm64`, based on uml/next `1590cf032` (Linux 7.2-rc4).
Build: `make ARCH=um SUBARCH=arm64 LLVM=1` (clang 19.1.7, lld 19).

## Gates

| # | Gate | State | Evidence |
| --- | --- | --- | --- |
| 1 | build + link | **pass** | 73 MB aarch64 ELF, EXEC at 0x60000000, stub and vDSO embedded |
| 2 | init on initramfs, marker reached | **pass** | busybox ash from Alpine initramfs; `uname -m` = `aarch64` |
| 3 | Alpine minirootfs from ubd0 | **pass** | ext4 on `/dev/ubda`, Alpine 3.21.3, 44 processes, working pipeline |
| 4 | fork/exec stress | **pass** | 2000 sequential fork+exec, 0 failures; 64 concurrent; pipeline sum correct |
| 5 | hostfs read/write | **pass** | reads a host file; guest's 262144-byte write lands on the host fs |
| 6 | Debian bookworm to a login shell | **pass** | bookworm 12.15 arm64, `uname -m`=aarch64, glibc 2.36, `bash -lc` works, glibc maps the vDSO |
| 7 | `apt install build-essential` | **pass** | `install ok installed`; the reproducer decompresses the 60 MB gcc-12 payload 8/8 byte-identical to a host-produced reference |
| 8 | kunit under UML | **pass** | 343 `ok`, 0 `not ok`, matching an x86_64 UML run exactly; **427 ok / 0 not ok** with `CONFIG_KUNIT_TEST=y`, including the suppression test that used to hang |
| 9 | build a C project in the guest | **pass** | the guest's own gcc built a ~90-line C program linked against liblzma; 6 iterations x 60 MB of correct decompression |
| 10 | loadable modules | **not started** | ranked above seccomp: without it no guest driver loads at all. Design settled, see below |
| 11 | ptrace inside the guest | **pass** | 18/18, identical to the same test on real arm64. Found three bugs on its first run |

## It runs on a phone

Gates 2, 3, 4 and 11 pass on a Xiaomi Poco F3 running Android 15 on a **4.19**
kernel, as uid 2000 under enforcing SELinux, with no root. Full account in
**doc/70-android.md**; the three things that had to change, all of them generic
portability fixes rather than Android workarounds:

* **`PTRACE_SYSEMU` does not exist before 5.3 on arm64.** UML now probes for it
  and, where it is missing, builds the same semantics from `PTRACE_SYSCALL` plus
  a syscall cancellation -- one extra ptrace call per guest syscall, no extra
  stop, because the port already steps off every syscall stop for x7. `nosysemu`
  forces that path on hosts that do have `PTRACE_SYSEMU`, so it is testable
  everywhere rather than only on a phone.
* **`TASK_SIZE` computed to zero on a 39-bit-VA host.** `PGDIR_SIZE` for a
  64-bit UML guest is 512 GiB, which is the *entire* user address space of an
  arm64 kernel built with `CONFIG_ARM64_VA_BITS_39` -- the usual choice for 4 KiB
  pages, and what Android ships. Rounding down to it gives zero, every guest
  mmap then fails, and the boot dies with `Requested init /init failed (error
  -12)` and no other clue. Now aligns to `PUD_SIZE` when that happens, and
  prints the three addresses on every boot.
* **`MFD_EXEC` is a 6.3 flag**, so `memfd_create` failed and the stub was written
  to disk. Retried without the flag; on those kernels a memfd is executable
  anyway.

Beyond the numbered gates:

| Check | State | Why it exists |
| --- | --- | --- |
| FP/SIMD + FPCR/FPSR torture | **pass** | Gates 1-5 barely touch FP; this found a real bug (below) |
| Guest TLS (TPIDR_EL0) explicit | **pass** | The TLS bug was found only via musl's assertion; now checked directly |
| PSTATE NZCV across signals | **pass** | The one part of PSTATE a sigreturn frame may set |
| ASLR blocked (`personality` → EPERM) | **pass** | Simulates the Android host; gate 3 unaffected |
| Page size parametric (4K/16K/64K) | **pass** | No 16K host exists here, so arithmetic is checked directly |
| Flake loop | **200 iterations, 1600 runs, 1600 pass, 0 failures** | 2412 s on the Pi 5, native arm64, 16 KB pages, against `linux-default`. Supersedes the earlier 550/550, which described a binary that no longer exists. Note its criterion is the marker, the same criterion that missed the rss leak below |
| Callee- *and* caller-saved registers across faults and thread switches | **pass** | `regsurvive`; the set the older probes did not cover, and where gate 7's bug lived |
| Decompressed stream compared byte-for-byte with a host reference | **pass** | `diffchar`; proves the output is correct, not merely non-crashing |
| Guest threads (CLONE_SETTLS, CLONE_CHILD_CLEARTID) | **pass** | 8 threads sharing one stub process; found a real bug |
| HWCAP sanitisation on an SVE host | **pass** | `-cpu max` host; SVE/PAC/CPUID and all of HWCAP2 masked out |

## Rules that outlive this phase

Two constraints the port has to keep, both learned by breaking them:

**guest `PAGE_SIZE` >= host `PAGE_SIZE`.** A guest page is a host `mmap()`, so
it cannot be smaller than the host's granularity. This is a compile-time
property of the guest and a run-time property of the host, which makes it a
shipping decision rather than a tuning option: a 16 KB guest runs everywhere, a
4 KB guest runs only on 4 KB hosts, and it fails there by *hanging*. 16 KB is
therefore the default (`arch/um/configs/arm64_defconfig`), with 4 KB available
for builds that know their host and want the memory back. Checked at boot, with
a message naming both sizes and the fix.

**`CONFIG_FRAME_POINTER` is a requirement, not a debug convenience.** UML's
stack walker chains through the frame pointer; without it `get_frame_pointer()`
returns 0 and `save_stack_trace()` returns nothing at all. Everything in this
project's method rests on a backtrace -- the agent loop's SIGSEGV handling, and
guest driver debugging in the next phase more so. It was `default y if
(DEBUG_INFO && UML)`, i.e. present in every build made here and absent from the
first size-tuned one. Now selected unconditionally via
`ARCH_WANT_FRAME_POINTERS`, as `arch/arm64/Kconfig` does for real hardware.

## Flake loop

**Superseded.** The current figure is **200 iterations, 1600 runs, 1600 passes,
2412 s**, run natively on the Pi 5 against `linux-default` (16 KB pages, real
arm64, weak memory ordering), covering g2mini, g2alpine, g3, g5, g4, fp,
regsurvive and guestptrace every iteration -- no sampling, because native
hardware made the expensive variants cheap enough to run every time.

Two caveats on it. Its pass criterion is the marker string, which is the same
criterion that let the rss leak below through unnoticed, so it is evidence of
"does not crash" rather than "is correct". And it predates the Android work; the
`PTRACE_SYSEMU` fallback and the `TASK_SIZE` change have not been through it.

The table below is the older 4 KB run under emulation, kept for its structure
and for the note about invented failures.

100 iterations against a since-superseded binary, 3.8 hours, 550 runs and 550
passes.

| variant | result | what it is |
| --- | --- | --- |
| g2mini | 100/100 | freestanding init on an initramfs, capability dump |
| g2alpine | 100/100 | busybox ash from an Alpine initramfs |
| g3 | 100/100 | Alpine root on `/dev/ubda` |
| g3noaslr | 100/100 | same, with `personality(ADDR_NO_RANDOMIZE)` forced to fail |
| g5 | 100/100 | hostfs read and write |
| g4 | 20/20 | 2000 sequential fork+exec plus 64 concurrent, sampled every 5th |
| fp | 20/20 | FP/SIMD + FPCR/FPSR torture, sampled every 5th |
| fpnoaslr | 10/10 | same with ASLR blocked, sampled every 10th |

The expensive variants are sampled rather than run every iteration; that is
recorded in `harness/loop.sh` and is why their denominators differ. Running g4
and fp on every pass would have put the loop past a day, which means it would
not have been run at all.

An earlier attempt at this loop produced four failures. All four fell inside the
fourteen minutes when a gate-7 debugging run was using the same run domain, and
all four were the harness restaging one guest's disk image underneath another
guest that was already booting. `boot.sh` now takes an flock, so that class of
invented failure cannot recur. This is worth stating rather than quietly
rerunning: a flake hunt that reports failures it caused itself is worse than no
flake hunt.

## Bugs found, and what found them

Ordered by how quietly they failed.

0. **arm64 hides x7 at a ptrace syscall stop; UML believed what it read.**
   arm64 encodes syscall-entry-vs-exit by overwriting x7 in the tracee and
   restoring it when the stop ends, so a tracer reads the stop direction (0)
   instead of the register, and any write it makes is discarded. UML reads
   registers at a stop, keeps them, and writes them back later -- and every
   thread of a guest mm shares one stub process, so a thread switch installs a
   different thread's registers into that stub. A value held in x7 across a loop
   therefore became zero, or another thread's value, at an arbitrary point.
   *Found by:* disassembling the faulting instruction (`strb w2, [x7, x0]`) and
   noticing x7 is written nowhere nearby, then `harness/probe/regsurvive.c`,
   which reproduces it in four lines and seconds. The host behaviour was then
   measured directly rather than inferred (`hostx7.c`, `hostx7b.c`).
   *Fix:* step off the syscall stop and take only that register from the
   resulting pseudo-step trap. See doc/50.
   *This was gate 7.* Three days of subsystem tests -- COW, uaccess, TLS, FP,
   hostfs, memory pressure -- eliminated a lot and could not have found it,
   because it is not in a subsystem.

0b. **Kernel threads ran on a stack misaligned for AAPCS64.**
   `new_thread()` reserved one word below the stack top, which is right on x86
   (it emulates the return address `call` pushes) and wrong on arm64, where the
   stack pointer must be 16-byte aligned at all times. clang derives alignment
   facts from that: `lib/tests/list-test.c` builds `&entries[0].list` with an
   `orr` instead of an `add`, which is a no-op on a misaligned stack.
   *Found by:* disassembling the two failing KUnit tests rather than reading
   them. *Fix:* `ARCH_INIT_SP_RESERVE`, a subarch decision; x86 unchanged.

0c. **SECCOMP mode could not initialise, silently.**
   The capability probe registered a one-page alternate signal stack. arm64's
   `MINSIGSTKSZ` is 5120, so at 4K pages `sigaltstack()` fails, the probe panics
   in a `CLONE_VFORK` child that has closed its descriptors, and UML hangs with
   no message. Invisible at 16K pages.
   *Found by:* trying to use SECCOMP mode at all -- which nothing had, because
   it is not the default and the ptrace path worked well enough to pass gates.
   *Fix:* register the whole shared area, as the real stub does, and report
   which step of the probe failed.

1. **Syscall argument 1 destroyed on every guest syscall.**
   `handle_syscall()` seeds the return register with `-ENOSYS` before reading the
   arguments. On x86 those are different registers (RAX vs RDI); on arm64 both
   are x0. Every guest syscall saw `-38` as its first argument.
   *Found by:* a freestanding guest init that printed one line per capability —
   the dynamic loader had been dying with no output at all.
   *Fix:* read argument 1 from the saved `HOST_ORIG_X0` slot.

2. **Guest TLS wiped on the first context switch.**
   `arch_switch_to()` reinstated `thread.arch.tp_value`, which is only set by
   `CLONE_SETTLS` and is therefore zero for any thread that set its own TLS —
   which on arm64 is every normal thread, because TPIDR_EL0 is EL0-writable.
   *Found by:* musl's `__dls3`, which compares `__pthread_self()` against
   TPIDR_EL0 and traps.
   *Fix:* TLS lives in the per-task register file; the switch is already implied.

3. **FPCR and FPSR lost on every signal.**
   `struct user_fpsimd_state` is `{vregs, fpsr, fpcr}`; `struct fpsimd_context`
   is `{head, fpsr, fpcr, vregs}`. Copying `vregs+fpsr+fpcr` as one block between
   them moves the vectors correctly and silently drops the control registers.
   *Found by:* `harness/probe/fptorture.c`. Nothing in gates 1-5 detects it.
   *Fix:* field-wise conversion in both directions.

4. **`NT_ARM_SVE` selected for the FP regset (latent).**
   Its buffer starts with a `user_sve_header` and its layout depends on the
   vector length, but every consumer of `uml_pt_regs.fp` reads it as
   `user_fpsimd_state`. Harmless on this SVE-less host, wrong on any SVE host.
   *Found by:* review prompted by (3).
   *Fix:* pin to `NT_PRFPREG` and document why SVE needs real work, not a flag.

5. **x8 not restored when restarting a syscall.**
   The guest re-executes the `svc`, and x8 selects the syscall there;
   `NT_ARM_SYSTEM_CALL` only redirects one already in flight. So
   `PT_REGS_ORIG_SYSCALL(regs) = __NR_restart_syscall` was a no-op and a
   signal-interrupted `nanosleep` would restart its full duration.
   *Found by:* review. musl and busybox do not exercise it; glibc does.

6. **`CLONE_BACKWARDS` not selected.**
   `arch/arm64/Kconfig` selects it; that file is not sourced for `ARCH=um`. So
   `sys_clone` took the asm-generic argument order while every aarch64 libc
   passes the CLONE_BACKWARDS one -- `tls` and `child_tidptr` swapped. A guest
   `pthread_create()` would receive its child-tid pointer as the new thread's
   TPIDR_EL0, and `CLONE_CHILD_SETTID`/`CLEARTID` would operate on NULL.
   *Found by:* auditing what `arch/arm64/Kconfig` selects that `arch/arm64/um`
   does not, prompted by two TLS bugs in a row.
   *Covered by:* `harness/probe/threadtorture.c`.

7. **The guest was told it had SVE, PAC and CPUID.**
   `ELF_HWCAP` forwarded the host's raw `AT_HWCAP`. glibc's ifunc resolvers pick
   `memcpy`/`strlen` from those bits, so on an SVE host the guest would use SVE
   and the host would put variable-length `sve_context` records in
   `sigcontext.__reserved[]` -- which `signal.c` does not round-trip, and which
   `os-Linux/registers.c` had already decided not to save. Advertising a feature
   whose state is deliberately dropped is the same bug as (4), one level up.
   *Fixed by:* an allowlist, with `/proc/cpuinfo`'s Features line derived from
   the same mask so the two cannot disagree.
   *Verified on:* a `-cpu max` run domain that really does have SVE.

8. **Harness bug: exit code overrode the marker.**
   A gate that printed its marker and then let init exit was scored `SIGNAL`,
   because the fatal-signal branch overwrote the `PASS`. The goal states
   explicitly that success is a marker string and never an exit code alone.

9. **Harness bugs that manufactured failures.** Worth recording because each one
   would have sent an unattended loop hunting a kernel bug that did not exist:
   a UML left over from a timed-out run kept an `F_SETLK` on the ubd image *and*
   `boot.log` open, so the next run could not open its root device and read the
   zombie's output back as its own; `cache=unsafe` plus a 30 s stop timeout
   silently discarded staged images; and a stale md5 sidecar beside a deleted
   image made staging skip a copy it needed to perform.

## Gate 8: the two kunit failures, and what they were

KUnit now runs 343 `ok`, 0 `not ok` across 14 suites -- byte-identical to an
x86_64 UML run of the same configuration. What follows is kept because the two
failures were a symptom of something much larger than KUnit.

They looked like this:

    # list_test_list_for_each_entry: EXPECTATION FAILED at lib/tests/list-test.c:740
    Expected cur->data == i, but
        cur->data == -2137407848 (0xffffffff8099be98)
        i == 0 (0x0)

with the identical value for `list_test_list_for_each_entry_reverse`, and the
neighbouring `list_for_each`/`list_for_each_safe` tests -- which do not use
`container_of` -- passing.

The value is a UML kernel stack address. Disassembling the test rather than
reading it showed the *read* was a correct `container_of`; the setup loop was
what was wrong:

    26d4: add x10, sp, #0x30
    26e4: orr x10, x10, #0x8     <- "+8" done as an OR
    271c: add x10, x10, #0x18

clang uses `orr` because it can prove bit 3 of the address is clear, which holds
only if the stack pointer is 16-byte aligned. AAPCS64 requires that at all
times; UML was giving every kernel thread a stack pointer 8 mod 16, because
`new_thread()` reserved one word below the stack top -- correct on x86, where it
emulates the return address `call` pushes, and wrong here. Fixed by
`ARCH_INIT_SP_RESERVE`.

`CONFIG_KUNIT_TEST` additionally *hung* at
`backtrace_suppression_test_multi_scope`. **It no longer does.** With the x7 and
stack-alignment fixes in place, a build with `CONFIG_KUNIT_TEST=y` runs
**427 ok, 0 not ok**, and the suppression suite is 9/9 including the test that
hung.

The theory that hung it -- that UML has no working unwinder, so a test which
exercises unwinding never returns -- was wrong, and was disproved before the
rerun by the hanging build's own log:
`artifacts/gate8/20260814T210115Z/boot.log:543-564` holds a correct 20-frame
unwind, from `sysfs_warn_dup` through `kobject_add` and `device_add` to
`kunit_try_run_case`, about thirty lines before the hang. Unwinding worked in
that binary. Two fixes were between then and now and either could account for
it; nothing distinguishes them without bisecting a hang, which is not worth the
VM time now that the gate is green.

What the investigation did find is worth more than the test:

**UML's unwinder degrades silently to nothing when `CONFIG_FRAME_POINTER` is
off.** `get_frame_pointer()` returns 0 outright in that case
(`arch/um/include/asm/stacktrace.h`), so the chain never advances, every frame
becomes a scan guess, and `save_stack_trace()` -- which keeps only frames marked
reliable -- returns an empty trace. Nothing in `arch/um` selected the symbol; it
was `default y if (DEBUG_INFO && UML)`, so backtraces worked in every build made
here and would have stopped working in the first size-tuned one. Measured on a
fresh defconfig with `DEBUG_INFO` off: `FRAME_POINTER=n` before, `y` after
selecting `ARCH_WANT_FRAME_POINTERS`, which is what `arch/arm64/Kconfig:93` does
for real hardware.

Also noted, not fixed: `dump_trace()` marks *scan hits* reliable as well as
chain hits, so `save_stack_trace()` can return frames that are not on the stack
at all -- the trace quoted above ends on a bogus `new_thread_handler+0x0`. That
is generic UML behaviour, not arm64's, and is left alone.

## Corrections to the goal's premises

Both were stated as facts and are not; both are documented where they bite.

* **The host is x86_64, not aarch64.** See `doc/00-environment-and-deviations.md`.
  Build stays native on x86_64; the run domain is a `qemu-system-aarch64` Debian
  guest, which is a real aarch64 kernel and therefore real ptrace, seccomp and
  signal-frame behaviour. `qemu-user` was rejected and why is recorded.
* **arm64 does have `PTRACE_SYSEMU`** (since v5.3), verified end-to-end by
  `harness/probe/hostcaps.c` on a live 6.12 arm64 kernel. The goal's
  `NT_ARM_SYSTEM_CALL` mechanism is implemented regardless and is load-bearing —
  it is the only way to read or cancel the syscall number — so the constraint is
  honoured as the floor rather than as the only path.

## Gate 11: ptrace inside the guest

Added because gates 1-9 never traced anything. They run shells, a package
manager and a compiler; none of them calls `ptrace`. So the guest's own ptrace
implementation -- `arch/arm64/um/ptrace.c` and the regset plumbing behind it --
had never been executed by any test here, in an interface where two bugs had
just been found by audit rather than by failure, and which is difficult on arm64
specifically because the syscall number lives outside the register set and the
return value shares a register with the first argument.

`harness/probe/guestptrace.c` checks eighteen properties of a syscall stop, and
is run **on the real arm64 run domain first** as a control -- a test that fails
on hardware would be measuring itself, not the port. It passes 18/18 there.

On its first run inside the guest it failed three:

    FAIL NT_ARM_SYSTEM_CALL at entry     got -99, want 64
    FAIL arg1 (x0) at entry              got 0xffffffffffffffda, want 0x1234
    FAIL NT_ARM_SYSTEM_CALL, 2nd syscall got -99, want 172

Two distinct bugs, both now fixed:

* **The `NT_ARM_SYSTEM_CALL` regset did not exist.** A guest tracer could not
  read the syscall number at all, nor cancel a call by writing -1 -- which on
  arm64 is the only way to do either.
* **`-ENOSYS` was seeded into x0 before the entry stop**, so a tracer saw
  `0xffffffffffffffda` as argument 1 of every syscall, and a value it wrote to
  x0 was discarded. Native arm64 seeds it before the stop only when the incoming
  number is already `NO_SYSCALL`.

Worth noting what did *not* catch this: `strace` passes either way, because
modern strace uses `PTRACE_GET_SYSCALL_INFO`, which routes through
`syscall_get_arguments()` and was self-consistent. Only the classic register
view was broken -- which is the view gdb uses. A gate built out of "run strace
and see if it works" would have reported success.

## An rss leak in the 16 KB default, and the harness hole that hid it

Found by re-running gate 6 on the current binary -- not by the gate failing, but
by reading the log it had already produced. The gate passed. Its log contains:

```
BUG: Bad rss-counter state mm:00000000ffbe45c0 type:MM_FILEPAGES val:1 Comm:gate6-init Pid:1
```

`check_mm()` is called unconditionally from `__mmdrop()` -- no `CONFIG_DEBUG_VM`
guard, and DEBUG_VM is off in every config here -- so this is the kernel
reporting that an mm was freed with one file-backed page still accounted to it.
One page, every time; never two.

It is isolated to one variable. Gate 11 was run twice with the **same command
line, the same disk image and the same code**, differing only in guest page size:

| binary | pages | userspace | leaks |
| --- | --- | --- | ---: |
| `20b46800` | 4 KB | Debian bookworm, glibc | 0 |
| `9a06116d` | **16 KB** | Debian bookworm, glibc | **6** |
| `9a06116d` | 16 KB | Alpine, musl | 0 |
| `53bf715e` | 16 KB | Alpine, musl | 0 |
| `b7f72ce1` | 4 KB | Debian bookworm, glibc | 0 |

`20b46800` and `9a06116d` are the same source; the second is `arm64_defconfig`
with `CONFIG_PAGE_SIZE_16KB=y`. So the leak needs 16 KB pages *and* a glibc
userspace, and it appears in the configuration that was just made the shipping
default. Nothing observable fails: the gates pass, gate 11 still reports 18/18,
and the bit-identical compile still matches. It is an accounting leak, and the
counter it leaks is the one the OOM killer and `/proc/*/status` read.

Prime suspect is the vDSO, on three grounds: it is the only special mapping the
port installs, it is exactly one page, and `arch/arm64/um/vdso/vma.c` places it
at `task_size - PAGE_SIZE` where 16 KB changes both terms. Suspect, not
diagnosis -- the mechanism is not established, and the obvious objection is that
`_install_special_mapping()` is generic code that balances on every other
architecture. Root-causing it needs `CONFIG_DEBUG_VM` and a probe that maps,
touches and exits in a loop.

**The harness hole is the more transferable finding.** The verdict chain in
`harness/boot.sh` was an `if/elif`: marker first, and only if the marker was
*absent* did it look for `BUG:`. A run that printed its marker was never scanned
at all, so a kernel BUG rode along inside a green gate and stayed there until a
human read the log. The goal's rule -- "success is a marker string, never an exit
code" -- is right about exit codes and silent about everything else in the log.

`boot.sh` now scans for kernel diagnostics *independently* of the marker, records
`kernel_bugs=N` in `result.txt` with the matching lines in `kernel-bugs.txt`, and
reports `verdict=BUG` for a run that reaches its marker with defects in its log.
It deliberately does not match `Kernel panic`, because several gates end by
design in "Attempted to kill init!" after their marker.

Two smaller things went in with it. `result.txt` now records `bin_md5`, because
`tree_head` is `git rev-parse HEAD` at capture time and says nothing about which
tree built the binary -- a gate-6 run carried a HEAD five commits newer than the
binary it booted, and answering "was this re-run?" required hashing the artifact
by hand. Twice.

## Gate 10: loadable modules

Not started, and ranked **above** seccomp mode: without modules no guest driver
loads at all, whereas seccomp is a speed and sandbox-surface optimisation. The
design is settled enough to start.

Reuse `arch/arm64/kernel/module.c` and `module-plts.c`, as x86 UML already reuses
its own (`arch/x86/um/Makefile:36`). `apply_relocate_add()` is 500 lines of
buffer arithmetic and is UML-clean; the working set is 10 relocation types plus
NONE, dominated by `CALL26`, `PREL32`, `ADR_PREL_PG_HI21`, `ADD_ABS_LO12_NC`
and `ABS64`.

Two results that change the plan:

* **I-cache maintenance is not a blocker.** This was the risk worth checking,
  because a module's text has to be made coherent after relocation and on arm64
  that means `dc cvau`/`ic ivau` -- EL0-executable only if `SCTLR_EL1.UCI`
  permits, and UML has no EL1. It is set by default
  (`arch/arm64/mm/proc.S:564`), cleared only by
  `cpu_enable_cache_maint_trap()` for four Cortex-A53 errata, and even then the
  instructions are emulated at EL1. So userspace can always do it.
* **The blocker is a linker script, and it has a second half.**
  `scripts/module.lds.S:68` includes `<asm/module.lds.h>`, which for `ARCH=um`
  resolves to the empty generic one via `arch/um/include/asm/Kbuild:16`. A
  UML/arm64 `.ko` therefore links with no `.plt`/`.init.plt`, and
  `module_frob_arch_sections()` bails at `module-plts.c:309`.

  Supplying `asm/module.lds.h` alone is not enough: it creates the sections but
  leaves them zero-sized. `module_frob_arch_sections()` is what counts the
  relocations that need a PLT and sets `sh_size` accordingly, so it has to come
  across too. Both port from `arch/arm64/kernel/` almost verbatim -- the work is
  in the guarding, not the logic.

The one genuinely unbounded risk is a header chain:
`arch/arm64/lib/insn.c` pulls `<asm/debug-monitors.h>` and ends at
`asm/sysreg-defs.h`, which is not generated for `ARCH=um`. Whether that include
can simply be dropped decides between ~25 lines of `#ifdef` and a ~450-line
trimmed copy in `arch/arm64/um/module.c`. Settle that before writing anything
else.

Test at first `insmod` across `mem=64M` (no PLTs), `mem=512M` (PLTs mandatory)
and `mem=3G` (ADRP near its limit) -- testing only the default silently skips
the entire PLT path.

**Gate 10 is not additive, and this changes what the flake loop means.** Loading
a module changes `vmalloc` usage and the guest's address-space layout, which is
exactly where the stub pages live. It is a perturbation of the thing every other
gate depends on, not a new feature bolted alongside. Two consequences:

* the 550/550 flake result does **not** carry over. It has to be re-run after
  modules land, against a kernel that has actually loaded one;
* **relocation bugs are data-dependent.** Which relocation types appear --
  `CALL26`, `ADR_PREL_PG_HI21`, the `MOVW_*` family -- is decided by the module,
  not by the loader. A hello-world module exercises a handful and proves almost
  nothing. The test set needs to be heterogeneous on purpose: something whose
  branches are long enough to go through a PLT, something with large static
  data, and a real driver-sized load -- `mac80211` is the obvious candidate,
  being big, self-contained, and full of the call patterns a driver actually
  generates.

## The two numbers an RFC is argued from

Not the gate count. A cover letter should lead with these, in this order:

1. **Byte-identical output, twice over.**

   *Decompression*: the port decompresses a 60 MB payload to bytes identical to
   a reference produced natively, eight times out of eight.

   *Compilation*: gcc running **inside** the guest produces object code
   byte-identical to the same gcc running **natively** -- 22 635 lines, 25
   translation units plus the link, aggregate sha256
   `cdf691def9890ea1990897acf1488e1b4d4d95d6b9c4ada9dda675146af21044` on both
   sides, 26 of 26 files matching.

   The control is exact, not approximate: the native run chroots into the *same
   disk image* the guest booted, so the compiler binary, the C library, the
   headers, the sources and the paths are identical and the only variable is
   which kernel is underneath. `harness/bitcmp.sh`.

   This is the strongest evidence in the project, and it is strong because it is
   transitive. For those hashes to agree, every FP and SIMD register has to
   survive every context switch and every signal, TLS has to be right for every
   thread the compiler spawns, and every byte read and written has to arrive
   intact -- continuously, across an entire build. No targeted test reaches that
   volume; a single wrong bit anywhere fails it.
2. **KUnit agreeing with x86 UML exactly.** 343 ok / 0 not ok on arm64 against
   343 ok / 0 not ok on x86_64 UML, same tree, same configuration. A reference
   architecture running the same suite is the oracle that makes a result mean
   something rather than merely look green -- and it is what caught the two list
   tests whose failure turned out to be a misaligned kernel stack.

Everything else -- gates, flake counts, the 16 KB matrix -- is support for those
two.

One caveat about the bit-comparison, recorded because the failure mode is
instructive: the first run of `bitcmp.sh` reported `BITCMP_DIFFERENT` while
showing two visibly identical hashes. The guest's output reaches the log through
a serial-style console that appends a carriage return, so the value arrived 65
characters long and compared unequal. A comparison whose entire purpose is to be
trusted had a false negative in it. It now strips CR on both sides, and the
per-file diff exists so that a mismatch has to name a translation unit rather
than merely assert one.

## What still stands between this and a posting

An independent review of the 18-commit series, separate from the code audit.
The port works; the series is not yet sendable. In order:

1. **DCO.** No `Signed-off-by:` on any commit, and the author is a pseudonym.
   A real name is required and nothing else matters until it is there. This one
   is not mine to fix.
2. **The first commit is a 3436-line, 86-file "checkpoint commit; to be split".**
   At least seven generic changes are buried inside it and invisible in the
   shortlog -- the `stub_exe.c` BPF page-mask fix, `STUB_SECCOMP_TLS_SYSCALL`,
   the `cpufeature.h`/`capflags.c` moves, the `get_host_cpu_features()` callback
   change, the `ptrace_getregs`/`get_host_regs` unification, `-mcmodel=large`.
   Several of those help UML *on x86* and should land first, before
   `arch/arm64/um/` exists at all.
3. **Four bisect breaks.** Two are plain build failures (`ARCH_INIT_SP_RESERVE`
   used one commit before it is defined; i386 missing
   `stub_seccomp_save_state()` for six commits). One breaks `SUBARCH=x86_64`
   across nine commits. The fourth is worse than a build break: between
   `a451bff91` and `1d48b84bf` the seccomp probe's alternate stack was widened
   while the helper's own stack was still inside it, so `SA_ONSTACK` is ignored,
   `mctx_offset` (an `unsigned short`) wraps, and `get_stub_state()` reads tens
   of kilobytes past an 8 KB mapping. Introduced and removed inside one series,
   under `seccomp=1` only -- but it is a live out-of-bounds read at an
   intermediate commit, and it exists because one change was split at the wrong
   seam.
4. **MAINTAINERS.** `arch/arm64/um/` currently routes to the arm64 maintainers
   via `F: arch/arm64/`. It needs to go to the UML list, with an `X:` in the
   ARM64 PORT entry.
5. **Page-size parametricity is hidden inside a commit about `restart_syscall`.**
   Four of its five files are linker scripts and `physmem.c`. Correct, and a
   strict no-op on x86, but nobody reading that subject opens a linker script.
6. **`CLANG_FLAGS` is wrong in both directions** -- too broad in one commit
   (dragging four new `-Werror=` diagnostics onto x86 `USER_OBJS`, on an
   architecture the commit says it does not touch), too narrow in its fixup
   (dropping `--prefix=`, which matters for `LLVM_IAS=0`).
   `$(filter-out -Werror=%,$(CLANG_FLAGS))` expresses the intent and has
   neither problem.

Items 2, 3, 5 and 6 are all the same mistake in different places: commits shaped
by the order the work happened in rather than by what a reviewer needs to read.

## Deliberately not done

* **Seccomp mode.** Not working, but now for one precisely known reason rather
  than as a silent hang. Three separate defects were in the way and two are
  fixed:

  1. the capability probe registered a one-page alternate signal stack, below
     arm64's `MINSIGSTKSZ` of 5120, so `sigaltstack()` failed and the probe
     panicked inside a `CLONE_VFORK` child with its descriptors closed — UML
     stopped dead with no message. *Fixed.*
  2. the probe carved the helper's stack out of the same area it used as the
     alternate stack, so `SA_ONSTACK` was ignored and the frame went onto the
     helper's own few kilobytes. *Fixed.*
  3. **an arm64 signal frame does not fit `struct stub_data`'s `sigstack[]`.**
     Measured, not estimated: `harness/probe/seccomphelper.c` reproduces the
     probe outside UML and reports the frame landing 4400 bytes below the top of
     the alternate stack, with `sizeof(mcontext_t)` alone being 4376 because
     arm64 reserves the whole `__reserved[4096]`. `sigstack[]` is one page. With
     4 KB pages the frame therefore overruns into `syscall_data[]`, the futex
     word and the offsets themselves; the bounds checks that exist to catch this
     underflowed (`sizeof(sigstack) - sizeof(mcontext_t)` is negative) and waved
     it through. *The underflow is fixed and SECCOMP is now refused with
     "signal frame does not fit the stub data area"; making it fit is not.*

  The fix for (3) is NOT a constant. An earlier version of this note said
  "STUB_DATA_PAGES goes from 2 to 4 on arm64", derived from measuring a 4576-byte
  frame on a cortex-a76 run domain. That was wrong in the way that matters: the
  arm64 signal frame is not a fixed size. `setup_sigframe_layout()` sizes it from
  the host's CPU features and the signalled task's state -- the SVE record scales
  with the vector length, SME adds za and zt -- which is exactly why arm64 made
  SIGSTKSZ dynamic and publishes `AT_MINSIGSTKSZ` in auxv. Measured on two run
  domains differing only in qemu's `-cpu`:

  | host | AT_MINSIGSTKSZ | frame actually written |
  | --- | ---: | ---: |
  | cortex-a76, no SVE | 4720 | 4576 |
  | -cpu max, SVE VL=512b | **9984** | 4576 |

  Note the second row twice over. The published requirement more than doubles,
  and it already exceeds the whole 8192-byte stub data area -- so a 4-page
  constant chosen from the 4576 measurement would have been out of contract on
  the very machine it was measured on. And the frame did *not* grow with SVE
  live, because `fpsimd_save_and_flush_current_state()` flushes SVE before the
  layout is computed; that was verified two ways, by faulting mid-SVE and by
  taking a timer signal inside an SVE loop (`harness/probe/sigframe.c`).

  So the rule is "as many bytes as this host says a frame can need", read at
  boot, not a page count baked in from a measurement. UML now parses
  AT_MINSIGSTKSZ and prints it against the stub area on every boot. Whether the
  buffer can be sized dynamically, or has to stay a compile-time upper bound
  with a runtime check, is the open design question -- and it should be settled
  with a 16 KB host in hand, because at 16 KB the same frame fits in one page
  and any arithmetic worked out on a 4 KB host risks cementing a 4 KB
  assumption into code that was written to be parametric.

  Parked deliberately rather than by accident: every gate passes on the ptrace
  path, the goal requires that path to keep working, and the layout change moves
  the stub in the guest address space — worth doing on its own, after the flake
  loop has locked in a known-good binary, not interleaved with it.

  Worth noting that the same arm64 kernel comment which explains the x7 problem
  says seccomp traps nobble no registers, so seccomp mode would not have needed
  the syscall-stop dance at all.
* **Loadable modules.** `apply_relocate_add()` for aarch64 is not implemented, so
  `CONFIG_MODULES` is off in `arch/um/configs/arm64_defconfig`. Nothing in gates
  1-9 needs modules; reusing `arch/arm64/kernel/module.c` is not possible because
  it patches alternatives, which UML does not have.
* **SVE/SME for the guest.** See (4) above.

*(The former entry here, "16 KB pages: `HAVE_PAGE_SIZE_16KB` is deliberately not
selected, because no 16 KB host exists", is obsolete in both halves. A 16 KB host
does exist — a Raspberry Pi 5 running trixie, see §"Real hardware" — the Kconfig
selects it, and `arch/um/configs/arm64_defconfig` sets `CONFIG_PAGE_SIZE_16KB=y`
as the default. The open item is no longer "untested" but the specific defect
below.)*
