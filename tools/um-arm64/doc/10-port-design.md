# ARCH=um SUBARCH=arm64 — port design

Base: uml/next @ `1590cf032` (Linux 7.2-rc4). Branch `um-arm64`.
All host behaviour below is measured, not assumed — see
`artifacts/hostcaps/hostcaps-6.12.101-arm64.txt`.

## 1. Measured host facts, and what they change

Run on Debian trixie arm64, kernel 6.12.101, under `qemu-system-aarch64`.

| Fact | Measured | Consequence |
| --- | --- | --- |
| `PTRACE_SYSEMU` | **works** (`ptrace.sysemu=yes`, child saw a cancelled syscall) | See §1.1 — this contradicts the goal |
| `NT_ARM_SYSTEM_CALL` get | ok, 4 bytes, reads 172 (`getpid`) | syscall number source in ptrace mode |
| `NT_ARM_SYSTEM_CALL` set -1 | cancels the syscall | cancellation primitive, kept regardless |
| `NT_ARM_SYSTEM_CALL` rewrite | accepted | can redirect a syscall if ever needed |
| `x8` at syscall entry | equals the syscall number | reading is free; *writing* x8 does nothing |
| `NT_ARM_TLS` | get/set/readback ok, 8 bytes | guest TLS switch primitive |
| SIGSEGV `si_addr` | correct | fault address |
| SIGSEGV `__reserved` | contains `fpsimd`, `esr`; 4096 bytes | FP state is inline, not behind a pointer |
| ESR on read fault | `0x92000007`, EC `0x24`, WnR **0** | |
| ESR on write fault | `0x92000047`, EC `0x24`, WnR **1** | WnR bit 6 is the `err & 2` equivalent |
| ESR on exec fault | `0x82000007`, EC `0x20` | instruction abort is distinguishable |
| VA space | highest mappable `0xfffffffff000` → 48-bit | `TASK_SIZE` input |
| `mmap_min_addr` | 4096; `MAP_FIXED_NOREPLACE` ok at 0x10000 upward | stub may be placed low |
| `ADDR_NO_RANDOMIZE` | allowed here | *not* relied upon — see §5 |
| seccomp | `RET_TRAP`, `USER_NOTIF`, `NEW_LISTENER` all available | seccomp fast path is viable |

### 1.1 The goal's PTRACE_SYSEMU premise is false

The goal states: *"No PTRACE_SYSEMU on arm64. Cancellation goes via the
NT_ARM_SYSTEM_CALL regset."*

arm64 has had `PTRACE_SYSEMU` since **v5.3**. In the target tree:

* `arch/arm64/include/uapi/asm/ptrace.h:77` — `#define PTRACE_SYSEMU 31`
* `arch/arm64/include/asm/thread_info.h:75` — `TIF_SYSCALL_EMU`
* `arch/arm64/kernel/ptrace.c:2416` — returns `NO_SYSCALL` when `_TIF_SYSCALL_EMU` is set
* `arch/arm64/kernel/syscall.c:114` — honours `NO_SYSCALL`

and the probe confirms it end-to-end on a running 6.12 arm64 kernel.

**What the port does about it.** Nothing is lost by the correction and nothing is
built on it either. The port keeps *both* mechanisms and probes at boot:

* `NT_ARM_SYSTEM_CALL` is used unconditionally to **read** the syscall number, because
  arm64 has no `orig_x8` in the ptrace register view, and to **cancel** by writing -1.
  This path is required no matter what, so the goal's mechanism is fully implemented.
* `PTRACE_SYSEMU` is used to avoid the syscall-exit stop when the host offers it, which
  is what makes the ptrace fallback merely slow rather than unusable. If the probe says
  no, the port falls back to `PTRACE_SYSCALL` plus a write of -1 to
  `NT_ARM_SYSTEM_CALL`, which is exactly the mechanism the goal specified.

So the goal's constraint is honoured as the *floor*, not as the only path.

### 1.2 Single-stepping cannot work in seccomp mode

x86 single-steps a seccomp-mode guest by setting `EFLAGS.TF` in the mcontext the stub
will `sigreturn` to. arm64's equivalent is `PSTATE.SS`, which only takes effect when
`MDSCR_EL1.SS` is set — an EL1 register userspace cannot write. Therefore:

* seccomp mode does not support single-step;
* `arch_has_single_step()` stays true, and a guest that requests single-step forces its
  stub process onto the ptrace path, where `PTRACE_SINGLESTEP` works normally.

This is a genuine arm64/x86 asymmetry, not an implementation shortcut, and it is the
main reason the ptrace fallback must keep working rather than being vestigial.

## 2. Register model

`struct user_regs_struct` on arm64 is `struct user_pt_regs`: `regs[31]`, `sp`, `pc`,
`pstate` — 34 × u64 = 272 bytes, and that is `UM_FRAME_SIZE`.

`uml_pt_regs.gp[]` is indexed by `HOST_*` constants generated from `user-offsets.c`:

    0..30  HOST_X0 .. HOST_X30
    31     HOST_SP
    32     HOST_PC
    33     HOST_PSTATE
    ---- end of the real regset; everything below is synthesised by UML ----
    34     HOST_ORIG_X0     x0 as it was at syscall entry
    35     HOST_SYSCALL_NR  from NT_ARM_SYSTEM_CALL (ptrace) or x8 (seccomp)

The two synthetic slots exist because x86 gets `orig_ax` for free inside
`user_regs_struct` and arm64 does not: `orig_x0` lives in the kernel's `struct pt_regs`
but is deliberately not exposed through `NT_PRSTATUS`. Putting them past index 33 keeps
`PT_SYSCALL_NR(gp)` — which only receives the array — working unchanged, and every
ptrace transfer uses `UM_FRAME_SIZE` (272) as its length, so the synthetic slots are
never handed to the host.

`MAX_REG_NR` is therefore `UM_FRAME_SIZE / sizeof(unsigned long) + 2 = 36`, and
`peek_user`/`poke_user` bound-check against `UM_FRAME_SIZE`, not against `MAX_REG_NR`.

Syscall ABI mapping:

| UML macro | arm64 |
| --- | --- |
| `UPT_IP` | `pc` |
| `UPT_SP` | `sp` |
| `UPT_SYSCALL_ARG1..6` | `x0..x5` |
| `PT_REGS_SYSCALL_RET` | `x0` |
| `PT_REGS_ORIG_SYSCALL` | `HOST_SYSCALL_NR` (**not** x0 — see below) |
| `UPT_RESTART_SYSCALL` | `pc -= 4` (`svc #0` is 4 bytes; x86 uses 2) |

`PT_REGS_ORIG_SYSCALL` deserves a note. On x86 it aliases `AX`, because on x86 the
syscall number and the return value share one register, so rewriting the "original
syscall" for a restart means writing `AX`. On arm64 they are different registers — the
number is in `x8`/the regset and the return is in `x0` — so aliasing them would make
`PT_REGS_ORIG_SYSCALL(r) = __NR_restart_syscall` in `arch/um/kernel/signal.c:117`
silently clobber the return value instead of changing the syscall. It maps to the
synthetic `HOST_SYSCALL_NR` slot, and restart also rewrites `x0` from `HOST_ORIG_X0`
because x0 has by then been overwritten with `-ERESTARTSYS`.

## 3. Fault info

    struct faultinfo {
            int is_write;          /* ESR WnR, bit 6 */
            unsigned long addr;    /* mcontext fault_address / si_addr */
            int esr;               /* full ESR, for SEGV_IS_FIXABLE and diagnostics */
    };

    #define FAULT_WRITE(fi)     ((fi).is_write)
    #define FAULT_ADDRESS(fi)   ((fi).addr)
    #define SEGV_IS_FIXABLE(fi) (ESR_EC(fi->esr) == 0x24 || ESR_EC(fi->esr) == 0x20)

`fault_address` is a first-class member of arm64's `mcontext_t`, so unlike x86 there is
no need to dig it out of `gregs[]`. The ESR comes from the `esr_context` record inside
`__reserved[]`, located by walking the `_aarch64_ctx` chain. The probe confirms the
record is present for read, write and exec faults, which is what makes `FAULT_WRITE`
implementable at all — without it, COW would be indistinguishable from a read fault.

Instruction aborts (EC 0x20/0x21) are treated as fixable so that a guest executing a
not-yet-faulted-in page works; data aborts are EC 0x24/0x25.

## 4. Stub

The stub is position-independent code mapped into every guest address space.
`as-layout.h` already expresses the layout entirely in `UM_KERN_PAGE_SIZE` and uses a
*runtime* `stub_start` variable, so it is PAGE_SIZE-parametric and ASLR-tolerant
upstream already; the arm64 port adds no fixed addresses of its own. See
`doc/40-page-size.md` for the audit.

* raw syscall: `x8` = number, `x0..x5` = args, `svc #0`, result in `x0`.
  Clobbers: `x0`, memory. Unlike x86's `syscall` there is no `rcx`/`r11` clobber.
* `trap_myself()`: `brk #0` (x86 uses `int3`).
* `get_stub_data()`: `adr x0, .` then mask to the page base and add one page. arm64's
  `adr` is a PC-relative address in one instruction, so this is cheaper than x86's
  `lea 0(%rip)`.
* TLS: guest TLS is `TPIDR_EL0`, which arm64 userspace writes itself with
  `msr tpidr_el0, xN` — no host syscall, unlike x86's `arch_prctl(ARCH_SET_FS)`.
  `stub_seccomp_restore_state()` becomes a single `msr`, and the seccomp filter in
  `arch/um/kernel/skas/stub_exe.c` no longer needs to allow `__NR_arch_prctl`.

## 5. ASLR

Not assumed. `STUB_START` is the runtime variable `stub_start`, chosen by the UML kernel
and mapped with `MAP_FIXED_NOREPLACE` into each stub process, so a randomised host
layout is tolerated by construction. `personality(ADDR_NO_RANDOMIZE)` was measured to be
*allowed* on this host but is not required by the design; nothing regresses if a future
Android host denies it.

## 6. arch/um changes needed to stop assuming x86

Found by inventory, not by build failure:

| Site | x86 assumption | Fix |
| --- | --- | --- |
| `arch/um/Makefile:29` | `SUBARCH` → `HEADER_ARCH` only maps x86 names | add arm64 |
| `arch/um/Makefile:33` | `-mcmodel=large` for all `CONFIG_64BIT` | x86-only; arm64 clang rejects it with PIC |
| `arch/um/os-Linux/registers.c:24` | `PTRACE_GETREGS` | no such request on arm64 → sysdep accessor |
| `arch/um/os-Linux/skas/process.c:95,669,700` | `PTRACE_GETREGS`/`SETREGS` | same |
| `arch/um/os-Linux/skas/process.c:66,70` | `R(EFLAGS)` in the register dump | sysdep register dump |
| `arch/um/os-Linux/util.c:56` | `uname.machine == "x86_64"` | sysdep host machine name |
| `arch/um/kernel/Makefile:50-55` | builds `capflags.c` from `arch/x86` cpufeatures | x86-only |
| `arch/um/include/asm/cpufeature.h` | x86 cpufeature model | x86-only |
| `arch/um/kernel/skas/stub_exe.c:183` | allows `__NR_arch_prctl` in the seccomp filter | sysdep filter fragment |

`PTRACE_GETREGS` is the load-bearing one: arm64 implements only the regset interface, so
the two call sites become `get_host_regs()`/`set_host_regs()` provided by the subarch,
implemented with `PTRACE_GETREGSET`/`NT_PRSTATUS` on arm64 and with the legacy request on
x86 so that architecture's behaviour is bit-for-bit unchanged.

## 7. File plan — arch/arm64/um/

Mirrors `arch/x86/um/`:

    arch/arm64/Makefile.um              START, ELF_ARCH, ELF_FORMAT, link flags
    arch/arm64/um/Makefile
    arch/arm64/um/Kconfig
    arch/arm64/um/user-offsets.c        generates user_constants.h
    arch/arm64/um/asm/{elf,processor,ptrace,syscall,vm-flags,checksum,segment}.h
    arch/arm64/um/shared/sysdep/{ptrace,ptrace_user,stub,stub-data,mcontext,
                                 faultinfo,archsetjmp,syscalls,tls}.h
    arch/arm64/um/os-Linux/{registers,mcontext,tls}.c
    arch/arm64/um/{ptrace,signal,syscalls,sys_call_table,tls,fault,bugs,delay,
                   stub_segv,sysrq}.c
    arch/arm64/um/setjmp_aarch64.S      kernel_setjmp/kernel_longjmp
    arch/um/configs/arm64_defconfig

`setjmp_aarch64.S` must save x19–x28, x29, x30, sp and d8–d15 — the AAPCS64
callee-saved set. UML cannot use libc `setjmp` here, which is why the file exists at
all on x86 too.
