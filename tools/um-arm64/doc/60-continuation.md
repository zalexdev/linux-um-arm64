# Continuation notes

Everything needed to pick this up cold. The other documents describe the port;
this one describes *working on* it -- current state, what is and is not
validated, the traps that cost real time, and what to do next.

Written at `349ae7a70`, 25 commits on top of Linux 7.2-rc4 (`1590cf032`).

---

## 1. Current state

### Tree

Branch `work` in `/root/mlu-arm64/linux`, clean. 25 commits. The series is
functional but **not sendable** -- see "What still stands between this and a
posting" in `doc/20-status.md` (no Signed-off-by, a 3436-line checkpoint commit,
four bisect breaks, MAINTAINERS).

### Binaries in `artifacts/`

| file | md5 | what it is |
| --- | --- | --- |
| `linux-latest` | `20b46800` | 4 KB pages, all fixes. What most gates were re-run on. |
| `linux-default` | `9a06116d` | built from `arm64_defconfig`, i.e. **16 KB** now |
| `linux-16k` | `53bf715e` | 16 KB, used for the page-size matrix |
| `linux-kt` | `8a299115` | `CONFIG_KUNIT_TEST=y`, 427 ok / 0 not ok |

### Build trees in `/tmp` (tmpfs, gone on reboot)

`umarm-build` (4K), `umarm-def` (defconfig = 16K), `umarm-16k`, `umarm-kunit`,
`umarm-kt`, `umx86-build` (x86_64 UML control), `arm64-16k` (**a 16 KB-page
arm64 kernel for the run domain**, not a UML build -- rebuild with
`make ARCH=arm64 LLVM=1 O=/tmp/arm64-16k` if lost, see §5).

`umarm-bare`, `umarm-nodbg`, `umarm-nodbg2`, `umarm-kt.config` are throwaway
Kconfig experiments and can be deleted.

### Run domain

One qemu VM, `harness/vm.sh`, ssh port 10022, 4 KB pages, Debian trixie,
host kernel 6.12.101. `vm2/` is a *second* VM directory left from earlier work;
it is stopped and nothing uses it.

---

## 2. What is validated, and what is not

This is the part most likely to be misremembered. **Be pessimistic here.**

Identify runs by the **binary md5**, not by the `tree_head` in `result.txt` --
see the caveat in §3.

| claim | binary | still valid? |
| --- | --- | --- |
| gates 2,3,5,4,fp pass (`r11-*`) | `20b46800` | yes |
| gate 7 pass, `repro_pass=8/8` (`r11-g7`) | `20b46800` | yes |
| gate 6 pass | `21c6dad52` build, superseded | re-run advisable |
| gate 11 (guest ptrace) 18/18 | `20b46800` and `9a06116d` | yes |
| KUnit 343/0, and 427/0 with self-tests | `linux-kt` | yes |
| bit-identical compile, 26/26 | `20b46800` | yes |
| 16 KB matrix (4 combinations) | `linux-16k` + `arm64-16k` host | yes |
| **flake loop 550/550** | **`b72e5ec7`** | **NO** |

`20b46800` contains every *code* change through `63567d50c`. The only later
commit, `349ae7a70`, changes `arch/um/configs/arm64_defconfig` alone, so
`20b46800` is code-equivalent to HEAD for a 4 KB build. `9a06116d`
(`linux-default`) is HEAD's defconfig, i.e. 16 KB.

Gate 6 is the one gap in the 4 KB set: it last passed on a build predating the
ptrace fixes. It is cheap (~10 min) and should be re-run before anything is
claimed about the current binary.

The flake loop is the important one. It ran before six later commits --
`rt_sigreturn`, the `mem.c` stack alignment, the `NT_ARM_SYSTEM_CALL` regset,
the `-ENOSYS`/arg1 change, `AT_MINSIGSTKSZ`, and the 16 KB default. The
`-ENOSYS`/arg1 change in particular touches the hottest path in the port. **550/550
does not describe any binary that exists now.** It must be re-run, and per the
project's own rule it must be re-run again after gate 10, because module loading
perturbs `vmalloc` layout where the stub pages live.

Also only smoke-tested, not fully validated: the **16 KB default build**
(`linux-default`). It has passed gate 11 and a mini boot. The full gate set and
the flake loop have only ever been run on 4 KB builds.

---

## 2b. The Pi 5 (`magicjr`) -- real hardware

Reachable over ssh as `magicjr`. Raspberry Pi 5 Model B, Cortex-A76, Debian 13
trixie, glibc 2.41, **16 KB pages natively**, 4 cores, 8 GB, 71 GB free,
passwordless sudo. Work lives in `~/umarm`.

This is the first machine the port has run on that is not emulated, and it
matters for one reason above the others: `harness/vm.sh` says it itself --
*"an arm64 guest on an x86_64 host is safe to run multi-threaded because the host
memory model is strictly stronger than the guest's."* Every result obtained
under qemu was obtained somewhere a missing barrier **cannot** fail. The Pi is
the first place it can. It also implements `SCTLR_EL1.SA0`, which TCG does not,
so misaligned-SP bugs fault there instead of merely miscompiling.

And it is roughly 50-100x faster, which changes what is affordable rather than
just what is quick:

| | TCG run domain | Pi 5 |
| --- | ---: | ---: |
| mini boot | ~11 s | **0.22 s** |
| g4, 2000 fork+exec | minutes | **9.7 s** |
| gate 7 | ~40 min | **25 s** |

Results, all on `linux-default` (16 KB):

* 4 KB binary on the 16 KB host refuses with the intended message -- the check
  added the day before, now demonstrated on the hardware class that motivated it
* g2mini, g2alpine, g3, g4, g5, fp: pass
* `regsurvive` 0 fails; `guestptrace` 18/18 **inside the guest**, identical to
  its own native control on the same machine
* `AT_MINSIGSTKSZ` 4720, frame 4576 -- identical to qemu, so that measurement
  generalises
* **gate 7: `repro_pass=8/8`, `install ok installed`**
* **bit-identical compile: `BITCMP_IDENTICAL`, 26/26**

That last one is the strongest single result in the project. The aggregate is
`cdf691def9890ea1990897acf1488e1b4d4d95d6b9c4ada9dda675146af21044` in **four**
environments: UML under TCG on x86, native chroot in the qemu run domain, UML on
the Pi, and native chroot on the Pi. Same bytes across emulated and real, 4 KB
and 16 KB, strong and weak memory ordering.

Two gotchas found by moving to it:

* `gate5-init` has the run domain's hostfs path (`/var/tmp/umboot/hostshare`)
  baked into the guest image, so gate 5 fails on any other host until that
  directory exists. The guest image should take the path from the command line.
* The alpine and debian images and the probes have to be copied over (~3 GB,
  ~15 min on this link). `~/umarm` already holds them.

Running gates concurrently on the Pi **is** safe when they use different disk
images -- the earlier four-fake-failures incident was shared staging state, not
CPU contention, and the loop's timeouts have 6-60x headroom.

## 3. Harness traps

Every one of these cost real time or produced a wrong answer.

**Absolute paths, always.** A relative `cd linux` from the wrong cwd silently
skips an edit or a build, and the next result looks like "the fix didn't work".
This happened repeatedly.

**`cd X && (A) & (B) &` does not do what it looks like.** The `cd` binds to the
first background group only. The second ran in the wrong directory and the
build silently didn't happen.

**One run domain, one lock.** `boot.sh`, `syscalls.sh` and `bitcmp.sh` all take
`flock` on `/tmp/umarm-rundomain.lock`. Anything new that touches the VM must
too. Before the lock existed, a flake loop and a debugging run overlapped and
the loop reported **four failures that the harness itself caused** by restaging
one guest's disk image under another guest that was already booting. A flake
hunt that invents failures is worse than none.

**Staging is not trustworthy by default.** `stage()` compares a checksum
*sidecar* that records the source's hash, because the guest dirties its copy;
it also has to compare sizes, because `scp` has been seen to exit 0 on a short
transfer. And it copies to `.new` then renames, because `scp` onto a live
executable fails with ETXTBSY when the previous UML is still exiting.

**`HOSTSHARE_SRC` or the guest gets whatever the last gate left behind.** Gate 7
once ran to completion, reported success, and tested nothing --
`UMARM_NO_GATE_SCRIPT` -- because an earlier hostfs gate had cleaned the share.

**Marker mismatch reads as failure.** `initramfs-mini` prints
`UMARM_GUEST_OK`, not `UMARM_BOOT_OK`. A wrong `MARKER=` gives `verdict=FAIL` on
a run that was fine.

**The guest console appends `\r`.** This produced a **false negative on the most
important measurement in the project**: `bitcmp.sh` printed `BITCMP_DIFFERENT`
while displaying two visibly identical hashes, because the guest's was 65
characters. Strip CR before comparing anything that came out of a boot log.

**`vm.sh stop` uses the default ssh port.** Overriding only `VMDIR` sends
`poweroff` to the *wrong* VM. This powered off the live run domain while
stopping a leftover. **Not yet fixed** -- `vm_stop` should derive the port from
the VMDIR, or refuse when they disagree.

**`tree_head` in `result.txt` is the *committed* HEAD, not what was built.** It
comes from `git rev-parse HEAD`, so a run made with uncommitted changes -- which
is the normal way to test a fix before committing it -- records a hash that
predates the code in the binary. Several gates show `tree_head=64e6b2c5f` while
testing changes that later became `927f3ff42` and `63567d50c`. The binary md5 is
the only reliable identifier; `build-*.log` records `dirty=yes/no` if you need
to know. Worth fixing in `boot.sh` by recording the binary's hash in
`result.txt`.

**A backgrounded `make` can hang forever and then clobber a validated binary.**
Adding a symbol to the defconfig makes `syncconfig` prompt; with stdin on a
socket it blocks indefinitely. A stale `make` sat for 1h37m and would, if it had
ever completed, have written `/tmp/umarm-build` from mid-edit source -- which is
then copied to `artifacts/linux-latest`. Kill stale builds before trusting a
binary, and check `md5sum artifacts/linux-latest` against what you think you
built.

---

## 4. Measurement traps

These produced confidently wrong answers, which is worse than no answer.

**`olddefconfig` never re-evaluates a `default y` downward.** It preserves what
is already in `.config`. So `scripts/config -d FOO` + `olddefconfig` cannot test
whether a symbol's *default* would turn something off. `allnoconfig` cannot
either -- it forces every prompt to `n` regardless of defaults. To test a
Kconfig default you need a **fresh defconfig generated from a fragment that
lacks the symbol**. I got this wrong twice in a row before getting a real
answer.

**`scripts/config -d DEBUG_INFO` alone does not disable debug info.**
`DEBUG_INFO_DWARF_TOOLCHAIN_DEFAULT` re-enables it. Disable both and set
`DEBUG_INFO_NONE`.

**Do not preprocess `<asm/unistd.h>` to get syscall names.** On an x86_64 build
host it resolves to the host's own table, and you get a syscall list whose
numbers are right and whose every name is wrong -- `ptrace` rendered as
`setresuid`. Read
`/tmp/umarm-build/arch/arm64/include/generated/uapi/asm/unistd_64.h` and assert
that 117 is `ptrace`.

**ftrace pid filtering: `$$` inside a bash subshell is the parent's pid.** Arm
the filter from `sh -c`, where `$$` really is the process that goes on to
`exec`. The first syscall capture traced the shell and reported 13 syscalls
instead of 61. Also drain `trace_pipe` into a file for the duration; reading the
ring buffer afterwards silently loses events.

**`strace` cannot trace UML.** The stub calls `PTRACE_TRACEME` and a tracee has
one tracer. Use ftrace (`harness/syscalls.sh`).

**i386 UML cannot be built here** -- no 32-bit libc headers and no network to
fetch them. Anything touching `arch/x86/um/shared/sysdep/stub_32.h` is
inspection-only; say so in the commit message.

**Workflow scripts are plain JS.** Backticks inside a template literal break
parsing; type annotations do too.

---

## 5. Rebuilding the 16 KB host

The single most valuable piece of infrastructure added, and it lives in `/tmp`.

```
make ARCH=arm64 LLVM=1 O=/tmp/arm64-16k defconfig
scripts/config --file /tmp/arm64-16k/.config \
    -d ARM64_4K_PAGES -e ARM64_16K_PAGES -e 9P_FS -e NET_9P -e NET_9P_VIRTIO
make ARCH=arm64 LLVM=1 O=/tmp/arm64-16k olddefconfig
make ARCH=arm64 LLVM=1 O=/tmp/arm64-16k -j16 Image

VM_KERNEL=/tmp/arm64-16k/arch/arm64/boot/Image harness/vm.sh start
```

The guest root is plain ext4 on `vda1` and needs no initramfs, so only
virtio-blk and ext4 must be built in. Confirm with
`harness/vm.sh ssh 'getconf PAGESIZE'` -- 16384.

---

## 6. Technical facts worth not rediscovering

**arm64 hides x7 at a ptrace syscall stop.** The kernel overwrites it with the
stop direction and restores the tracee's value afterwards, so tracer writes are
discarded and the real value is unreadable. This was gate 7: liblzma keeps its
dictionary base in x7 for the whole decode loop, and every thread of a guest mm
shares one stub. The escape is `PTRACE_SINGLESTEP` off a `PTRACE_SYSEMU` entry
stop, which lands on a pseudo-step trap where x7 is live -- **and executes no
guest instruction; the program counter does not move.**

*But take only x7 from that stop.* The pseudo-step arrives as a forced SIGTRAP,
so the host's signal path runs first and `do_signal()` calls `forget_syscall()`.
Reading the whole register set there yields `scno = -1` and returns `-ENOSYS`
for every guest syscall -- the first version of the fix did exactly that and
hung the guest.

**AAPCS64 wants SP 16-byte aligned at all times.** `ARCH_INIT_SP_RESERVE` is 0
on arm64 and `sizeof(void *)` on x86. Three sites use it: `new_thread()`,
`start_idle_thread()`, `init_syscall_regs()`. Any new code computing a stack for
something that runs C must use it. The symptom is not a fault under TCG -- it is
a *miscompile*, clang emitting `orr xN, xN, #8` where an add was needed, because
it proved bit 3 was clear.

**Signal frame size is not a constant.** `AT_MINSIGSTKSZ` was 4720 on
cortex-a76 and 9984 on `-cpu max`, same kernel. `sizeof(mcontext_t)` is 4384 --
already larger than a 4 KB page, which is architectural. The frame actually
written was 4576 on both, because `fpsimd_save_and_flush_current_state()` flushes
SVE before the layout is computed, verified two ways (async fault mid-SVE, and a
timer signal inside an SVE loop). Never size a buffer from a measurement.

**guest `PAGE_SIZE` >= host `PAGE_SIZE`**, because a guest page is a host
`mmap()`. 4 KB guest on a 16 KB host used to hang silently; now checked at boot.
16 KB is the default because it covers both.

**`CONFIG_FRAME_POINTER` is load-bearing.** Without it `get_frame_pointer()`
returns 0 and `save_stack_trace()` returns nothing. It was `default y if
(DEBUG_INFO && UML)` -- on in every build made here, off in the first size-tuned
one. Now `select ARCH_WANT_FRAME_POINTERS`.

**Modules: I-cache maintenance from EL0 is *not* a blocker.** `SCTLR_EL1.UCI` is
set by default and the instructions are emulated at EL1 even on the errata
parts. The blockers are `asm/module.lds.h` (resolves to the empty generic one,
so a `.ko` has no `.plt`) **and** `module_frob_arch_sections()` (without it the
section exists but is zero-sized). The unbounded risk is whether
`arch/arm64/lib/insn.c` can drop `<asm/debug-monitors.h>`, which ends at a
`sysreg-defs.h` that `ARCH=um` does not generate.

---

## 7. Method that worked

**Disassemble the faulting instruction before theorising.** Gate 7 was found by
looking at `strb w2, [x7, x0]` and noticing x7 is written nowhere nearby. Three
days of subsystem tests -- COW, uaccess, TLS, FP, hostfs, memory pressure -- were
all sound and none could have found it, because the bug was not in a subsystem.

**Always run the control first.** `guestptrace` is run on the real arm64 run
domain before being run in the guest; if it failed there it would be measuring
itself. `x86_64` UML is built and booted after every change to generic
`arch/um/`. KUnit's value is that x86 UML gives the same 343.

**Prefer a byte-comparison to a pass/fail.** The strongest evidence here is that
gcc under the port emits object code byte-identical to gcc natively, from a
chroot of the same disk image. Second strongest is 60 MB of decompression
matching a native reference. Both are transitive over FP/SIMD, TLS and signal
delivery at a volume no targeted test reaches.

**Distrust a clean result from a test you just wrote.** The vacuous `ar p | xz`
control, the 13-syscall ftrace capture, and the CR-terminated hash comparison
all *looked* fine.

---

## 8. Next actions, ranked

0. ~~Re-run gate 6~~ **done**: PASS on `9a06116d` (the 16 KB default), tree
   `349ae7a70`, Debian to a bash login shell. It also turned up the item below.
0a. **Root-cause the 16 KB rss leak.** `BUG: Bad rss-counter state ...
   MM_FILEPAGES val:1`, one page per affected mm, in the shipping default.
   Isolated to {16 KB pages} x {glibc userspace} by a controlled pair -- gate 11,
   same cmdline, same image, same code, 4 KB clean and 16 KB leaking six times.
   Suspect is the vDSO at `task_size - PAGE_SIZE`; the mechanism is *not*
   established. Needs `CONFIG_DEBUG_VM` and a map/touch/exit probe. Nothing
   observable fails, which is exactly why it will not find itself.
   See doc/20-status.md, "An rss leak in the 16 KB default".
1. ~~Re-run the flake loop~~ **running/done on the Pi** against `linux-default`
   (16 KB, real hardware). Note it will now be a *weaker* statement than it
   looks: the loop's pass criterion is the marker, and the marker did not catch
   the rss leak. Re-read a sample of its logs for `kernel_bugs`.
2. **Run the full gate set on the 16 KB default build.** Mostly done on the Pi
   (gates 2,3,4,5,7,11, fp, regsurvive, bitcmp). Still never run at 16 KB:
   **gate 9** and **KUnit**.
3. **Gate 10, modules.** Settle the `insn.c` header question first (~30 minutes,
   decides between ~25 lines of `#ifdef` and a 450-line copy). Then
   `asm/module.lds.h` + `module_frob_arch_sections` + the build wiring. Test
   with a *heterogeneous* module set -- long branches through a PLT, large static
   data, and `mac80211` -- because which relocation types appear is decided by
   the module, not the loader. Then re-run the flake loop again, because module
   loading perturbs `vmalloc` layout where the stub pages live.
4. **Fix `vm_stop`'s port bug** (§3) before it powers off a live run domain again.
   Still the only known harness defect that can destroy a running experiment.
5. ~~Re-run the syscall capture~~ **done**: 61 distinct syscalls, 8441 events,
   0 dropped, filter armed correctly (`artifacts/syscalls-20260815T050448Z/`).
   With the static contract in doc/30 this deliverable is complete -- both lists
   and the difference between them.
6. **Series hygiene** -- the bisect breaks and the checkpoint commit split. Needed
   before posting, not before more work.
7. **Seccomp**, last. Every gate passes on the ptrace path; seccomp is a speed
   and sandbox-surface optimisation, and it needs the `sigstack[]` sizing
   question settled -- which should be settled on a 16 KB host, where the problem
   disappears.
