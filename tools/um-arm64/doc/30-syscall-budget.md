# Host syscall budget

The goal asks for a log of every host syscall the binary issues, on the grounds
that this budget is what the Android phase has to fit inside. This documents
both what UML asks of its host and how to obtain the log, because the obvious
method does not work.

## Why `strace ./linux` does not work

UML's guest processes call `PTRACE_TRACEME` so that the UML kernel can be their
tracer. A tracee has exactly one tracer, so running the whole thing under
`strace -f` makes the stub's `PTRACE_TRACEME` fail:

    Checking that ptrace can change system call numbers...ptrace: Operation not permitted
    check_ptrace : expected SIGSTOP, got status = 9

Three ways round it, in order of fidelity:

1. **`strace` without `-f`.** strace then traces only the initial UML process
   and never attaches to the stub children, so nothing competes for the tracer
   slot. This captures every syscall the *UML kernel* makes -- which is the
   large majority and the interesting part -- but not the handful the stub
   issues on its own behalf. Those are enumerated statically below; the stub's
   set is fixed and tiny by construction, because it has no libc.
2. **Seccomp mode** (`seccomp=on`). UML then does not use ptrace for guest
   execution at all and `strace -f` works normally. Currently unavailable: the
   seccomp path does not yet initialise on arm64 (see `doc/20-status.md`).
3. **ftrace**, which is what actually produced the log below and is better than
   either. `raw_syscalls:sys_enter` is a tracepoint, not a tracer in the ptrace
   sense, so it does not compete for the tracer slot; with `set_event_pid` plus
   the `event-fork` option it follows the stub children automatically. See
   `harness/syscalls.sh`.

## Static enumeration

Extracted from the source rather than a trace, so it is complete rather than
merely observed. Nothing outside these can appear, because every host syscall
in UML goes through one of these two paths.

### Issued by the stub, with no libc

These are raw `svc #0` from `arch/um/kernel/skas/stub_exe.c`,
`arch/um/kernel/skas/stub.c` and `arch/arm64/um/shared/sysdep/stub.h`. This set
is the one that matters most for a restricted host: it is what a guest process's
address space manipulation costs.

    close, close_range, exit, exit_group, fcntl, futex, getpid, kill,
    mmap, munmap, prctl, ptrace, read, recvmsg, rt_sigaction,
    rt_sigreturn, seccomp, sigaltstack

Note what is *not* there: no `openat`, no `execve`, no filesystem access at all.
The stub is handed file descriptors and never opens anything.

### Issued by the UML kernel itself

From `arch/um/os-Linux/` and `arch/arm64/um/os-Linux/`, either directly or
through libc:

    clock_nanosleep, close, close_range, execveat, exit, exit_group,
    fcntl, futex, getpid, getppid, kill, mmap, munmap, prctl, ptrace,
    read, recvmsg, restart_syscall, rt_sigaction, rt_sigreturn,
    sched_yield, seccomp, sendmsg, sigaltstack, tgkill

plus the ordinary libc surface for file, socket and epoll work (`openat`,
`read`, `write`, `pread64`, `pwrite64`, `lseek`, `epoll_create1`, `epoll_ctl`,
`epoll_wait`, `socket`, `bind`, `connect`, `poll`, `timerfd_*`, `clock_gettime`,
`getrandom`, `uname`, `personality`, `setsid`, `fork`, `clone`, `waitpid`).

### arm64-specific additions

None. This is worth stating explicitly: the arm64 subarch introduces no host
syscall that x86 does not already use. `NT_ARM_SYSTEM_CALL`, `NT_ARM_TLS` and
`NT_PRSTATUS` are all `ptrace(PTRACE_GETREGSET/PTRACE_SETREGSET)`, which is the
same syscall x86 already issues for its FP state. The arm64 port therefore does
not widen the budget at all -- it narrows it slightly, because guest TLS needs no
syscall (TPIDR_EL0 is written by the stub with a single `msr`, where x86 needs
`arch_prctl`).

## Host features required

| Requirement | Used for | Android risk |
| --- | --- | --- |
| `ptrace` (ATTACH, GETREGSET, SETREGSET, SYSEMU, CONT, SINGLESTEP) | guest execution, ptrace mode | may be restricted |
| `seccomp` filters | guest execution, seccomp mode | generally available |
| `PROT_EXEC` mapping of a temp file | the stub's code page | **W^X policy** -- checked at boot by `check_tmpexec()` |
| `/dev/shm` or a tmpdir on tmpfs | guest physical memory | app-private dir may be needed |
| `mmap` at a chosen address (`MAP_FIXED_NOREPLACE`) | stub placement | fine |
| `personality(ADDR_NO_RANDOMIZE)` | *optional* -- see below | may be blocked |
| `futex`, `close_range`, `recvmsg`/`sendmsg` over `AF_UNIX` | stub control channel | fine |

Not required, and deliberately so: no modules, no `/dev/kvm`, no
`CLONE_NEWUSER`, no `CAP_SYS_ADMIN`, no `CONFIG_USER_NS`.

### personality(ADDR_NO_RANDOMIZE)

`arch/um/os-Linux/main.c` requests it and re-execs if the request took effect,
but a failure is non-fatal: UML simply continues with a randomised layout.
Verified by `harness/probe/noaslr_preload.c`, an `LD_PRELOAD` shim that makes
`personality()` return `EPERM` to simulate an Android host. Gate 3 passes
unchanged under it, so the address-space layout does not depend on it.

## Reproducing the log

    STRACE=1 GATE=budget MARKER=... harness/boot.sh

writes `artifacts/budget/<stamp>/strace.log`. Remember the caveat above: without
seccomp mode this is the UML kernel's syscalls only, and `STRACE=1` defaults off
precisely so that a functional gate is never silently broken by the tracer
conflict.


## The dynamic log

Captured with ftrace on the arm64 run domain, booting an initramfs and a
freestanding init: **61 distinct syscalls, 8441 events, 0 dropped**. Full table
in `artifacts/syscalls-*/syscalls.md`. The head of it:

| count | name | why |
| ---: | --- | --- |
| 5252 | `ptrace` | driving the stub; this is the ptrace path's whole cost |
| 1143 | `wait4` | one per stub stop |
| 667 | `mmap` | guest page faults, serviced as stub mmaps |
| 185 | `rt_sigreturn` | returning from the kernel's own signal handlers |
| 165 | `timer_settime` | the UML timer |

Two things that are worth noticing rather than skimming.

**`ptrace` and `wait4` are 76% of all syscalls, and SECCOMP mode removes almost
all of it.** That is the real argument for finishing SECCOMP, and it is stronger
than speed alone: `ptrace` is exactly the syscall an Android app sandbox is most
likely to restrict.

Precisely, because the imprecise version of this sentence was wrong when first
written here: `ptrace` disappears from the set entirely, and the per-stop
`waitpid` in `userspace()` goes with it, but `wait4` does **not** leave the set
-- `os-Linux/process.c:75` reaps children with `waitpid(-1, ..., WNOHANG)`
unconditionally, and `start_userspace()` still waits for the stub to come up.
What collapses is the volume, not the requirement. A sandbox policy has to
permit `wait4` either way.

**Getting this measurement right took two attempts, and the first one was wrong
in a way that looked fine.** It reported 13 syscalls. The pid filter was armed
with `$$` inside a bash subshell, where `$$` is the *parent's* pid, so it traced
the shell rather than the kernel. It produced a clean, plausible, entirely
useless table. The fix is in `harness/syscalls.sh`: arm the filter from `sh -c`,
where `$$` really is the process that goes on to `exec`.

A third mistake nearly shipped with it: the number-to-name mapping was taken by
preprocessing `<asm/unistd.h>`, which on an x86_64 build host resolves to the
*host's* table. Every number was right and every name was wrong -- `ptrace`
appeared as `setresuid`. The script now reads the table kbuild generated for
this build and refuses to run if syscall 117 is not `ptrace`.

## Why this log is a floor, not a contract

A trace records what *executed*. The budget needs what the binary *can* execute:
error paths, teardown paths, the ptrace-versus-seccomp fork, driver paths not
exercised by these gates, and everything reached only when something goes wrong.
A syscall that fires only on an error path is precisely the one that will
surface months later as an unexplained failure on a phone, with nothing to tie
it back to.

So the deliverable is two lists and the difference between them:

* the dynamic list above, which is evidence that these are reachable;
* a static enumeration from the source, which bounds what is possible.

The static enumeration is the authority for the Android phase. The dynamic list
is what proves the static one is not missing something -- anything observed that
the static reading did not predict means the static reading has a hole, and that
hole is the interesting part.


## The static contract

Produced by reading the source rather than watching a boot, and reviewed
against the binaries. This is the authority for the Android phase; the trace
above is the evidence that these paths are real.

### The stub, in ptrace mode: exactly 11

`prctl`, `read`, `exit`, `close`, `mmap`, `munmap`, `sigaltstack`,
`rt_sigaction`, `ptrace`, `getpid`, `kill`.

Raw `svc #0` with no libc, from `arch/um/kernel/skas/stub_exe.c` and the
`.__syscall_stub` section. Confirmed by disassembly: 34 `svc` in `stub_exe`
(13 distinct) and 11 `svc` plus 2 `brk` in the stub section of `linux`.

### The stub, in seccomp mode: 7 more

`fcntl`, `close_range`, `seccomp`, `futex`, `recvmsg`, `exit_group`,
`rt_sigreturn`.

arm64 needs one *fewer* than x86 here: guest TLS is a single `msr tpidr_el0`
rather than a syscall, so `STUB_SECCOMP_TLS_SYSCALL` is -1
(`arch/arm64/um/shared/sysdep/stub.h:32`) where x86 has `arch_prctl` or
`set_thread_area`.

### The kernel side

Everything else, via libc, from `arch/um/os-Linux/`, `arch/arm64/um/os-Linux/`
and the drivers that are actually built. Roughly 93 distinct calls reachable in
this configuration, against 61 observed.

### Corrections the static reading forced

Each of these is a place the dynamic trace, or an earlier draft of this
document, was wrong:

* **`renameat2` (276) was missing entirely.** A raw
  `syscall(SYS_renameat2, ...)` at `fs/hostfs/hostfs_user.c:384`, reached from
  `hostfs_kern.c:770` whenever the guest renames with `RENAME_NOREPLACE` or
  `RENAME_EXCHANGE`. No gate does that, so no trace would ever show it -- and a
  sandbox that omitted it would break `mv` on hostfs, months later, on a phone.
  This single entry is the argument for the whole static enumeration.
* **`wait4` does not disappear in seccomp mode**, only its volume does. See
  above.
* **Several symbols are linked but unreachable** in this configuration --
  `connect`, `eventfd2`, `mremap`, and `os_futex_wait`/`os_futex_wake`, the last
  because their only caller is inside `#if IS_ENABLED(CONFIG_SMP)`. Listing them
  as required would over-ask the sandbox.
* **The ptrace cost against x86 is about 2.4x, not 1.5x.** x86 spends 5 `ptrace`
  plus 1 `wait4` per guest syscall, because one `PTRACE_GETREGS` fetches the
  whole set; arm64 needs a regset call each for `NT_PRSTATUS`,
  `NT_ARM_SYSTEM_CALL` and `NT_ARM_TLS`, plus the extra stop this port takes to
  read x7. That is the price of the ptrace fallback, and the reason seccomp
  matters beyond speed.

### Still open

Three counts in the trace are unattributed (`execve` at 6, `set_robust_list` at
9, and `signalfd4` absent where `ppoll` appears). The most likely explanation is
that `timeout` and the launching shell are inside the traced pid set. Re-running
the capture with the filter armed past `timeout` settles all three at once, and
until it is done the trace should be read as approximate at the margins.
