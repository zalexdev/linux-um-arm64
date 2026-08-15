# Page-size audit

The goal requires that stub layout, address-space arithmetic and mmap alignment
be PAGE_SIZE-parametric, that every literal `4096` be flagged, and — since no
16 K host exists here — that this be enforced "by review and a parametric test",
not by CI.

Enforcement mechanism: `harness/probe/pagesize_test.c`, a plain host program that
re-implements the port's address arithmetic and checks its invariants at 4 K,
16 K and 64 K. It needs no arm64 and no VM, runs in milliseconds, and currently
reports `PAGESIZE_TEST_OK`.

## Why a UML guest's page size equals its host's

UML implements guest page tables with host `mmap`/`munmap`/`mprotect` on the stub
process. The host will only honour page-aligned, page-multiple requests, so a
guest page cannot be smaller than a host page, and making it larger would mean
emulating protection at a finer granularity than the host provides. In practice
the guest page size must equal the host's. On a 16 K-page Android host, a UML
guest must therefore be built `CONFIG_PAGE_SIZE_16KB`, and every place that
assumes 4096 becomes wrong at once — which is why this audit is exhaustive rather
than opportunistic.

## Findings

Scan: `grep -rnE '\b(4096|0xfff|0xfffff000|1 ?<< ?12)\b' arch/um/ arch/arm64/um/`

### Real bugs, fixed

| Site | Was | Now | Failure on a 16 K host |
| --- | --- | --- | --- |
| `arch/um/kernel/skas/stub_exe.c` seccomp filter | `BPF_ALU\|BPF_AND` with `0xfffff000`, and `stub_start & 0xfffff000` | `(unsigned int)UM_KERN_PAGE_MASK` | The filter checks whether the trapping instruction pointer is on the stub's code page. A 4 K mask compares the wrong page for any stub instruction above the first 4 K, so legitimate stub syscalls are killed by `SECCOMP_RET_KILL_PROCESS`. |
| `arch/um/include/asm/common.lds.S` ×5 | `ALIGN(4096)`, `RO_DATA(4096)` | `ALIGN((1 << CONFIG_PAGE_SHIFT))` | Read-only and read-write data share a page, so `CONFIG_STRICT_KERNEL_RWX` either fails to `mprotect` or grants write access to rodata. |
| `arch/um/kernel/dyn.lds.S` | `ALIGN(4096)` | `ALIGN((1 << CONFIG_PAGE_SHIFT))` | Same, for the init sections. |
| `arch/um/kernel/vmlinux.lds.S` | `KERNEL_STACK_SIZE = 4096 * (1 << CONFIG_KERNEL_STACK_ORDER)` | `(1 << CONFIG_PAGE_SHIFT) * (1 << ...)` | Disagrees with `THREAD_SIZE` in `<asm/thread_info.h>`, which *is* PAGE_SIZE-based: the linker reserves a quarter of the stack the kernel believes it has. |
| `arch/um/kernel/physmem.c` | error text `.../4096` | `%lu` with `PAGE_SIZE` | Cosmetic, but the message tells an operator to compute `max_map_count` from the wrong divisor. |

### Correct by construction, verified

* `arch/um/include/shared/as-layout.h` — already expresses the whole stub layout
  in `UM_KERN_PAGE_SIZE` and uses the *runtime* variable `stub_start`, so it is
  both page-size-parametric and ASLR-tolerant upstream. The arm64 port adds no
  fixed addresses of its own.
* `arch/arm64/um/shared/sysdep/stub.h` — `get_stub_data()` and `stub_start(fn)`
  take the page size and stub size in **registers**, not as immediates,
  specifically so a different page size needs no source change.
* `arch/arm64/um/asm/elf.h` — `ELF_EXEC_PAGESIZE` is `PAGE_SIZE`. x86 UML
  hardcodes 4096 there, which is fine for x86 and would not be for arm64.
* `arch/arm64/um/vdso/Makefile` — `-z max-page-size` is selected from
  `CONFIG_PAGE_SIZE_{4KB,16KB,64KB}` rather than hardcoded.
* `arch/arm64/um/vdso/vdso.lds.S` — the vDSO is one guest page; nothing in the
  script assumes how big that is.

### Not page size (false positives, left alone)

| Site | What it actually is |
| --- | --- |
| `arch/um/include/asm/setup.h` `COMMAND_LINE_SIZE 4096` | POSIX `_POSIX_ARG_MAX`; unrelated to paging. |
| `arch/um/os-Linux/main.c` `char buf[4096]` | A `readlink()` buffer for `/proc/self/exe`. |
| `arch/um/os-Linux/Makefile` `-Wframe-larger-than=4096` | A compiler diagnostic threshold. |

## What is still owed for a 16 K host

1. `arch/um/Kconfig` selects only `HAVE_PAGE_SIZE_4KB`. A 16 K guest additionally
   needs `HAVE_PAGE_SIZE_16KB` selected for the arm64 subarch, gated so that x86
   is unaffected. This is a one-line change but must not be made until it can be
   *tested*, because an untested 16 K config is worse than an absent one.
2. The stub is `STUB_DATA_PAGES = 2` data pages. At 64 K that is 192 KB of
   address space per guest process — harmless, but worth measuring rather than
   assuming.
3. `harness/probe/pagesize_test.c` covers arithmetic, not behaviour. The
   behavioural check requires a 16 K host; when one is available the whole gate
   matrix should be re-run there before any 16 K claim is made.


## A 4096 that was not a page size at all: MINSIGSTKSZ

Found while working out why SECCOMP mode hung on arm64, and it belongs here
because the bug appears at 4K and disappears at 16K.

`struct stub_data` ends with `sigstack[UM_KERN_PAGE_SIZE]`, and the seccomp
capability check in `arch/um/os-Linux/start_up.c` registered exactly that member
as its alternate signal stack:

    set_sigstack(seccomp_test_stub_data->sigstack,
                 sizeof(seccomp_test_stub_data->sigstack));

The legal minimum for an alternate stack is not a page, it is `MINSIGSTKSZ`, and
that is per-architecture:

| arch | MINSIGSTKSZ |
| --- | --- |
| asm-generic | 2048 |
| x86 | 2048 |
| **arm64** | **5120** |

So with 4K pages `sigaltstack()` returns ENOMEM, `set_sigstack()` panics, and --
because this runs in a `CLONE_VFORK` child that has just done
`close_range(1, ~0U)` -- the panic message goes to a closed descriptor and the
parent stays blocked in `clone()`. UML stops dead after "Checking that seccomp
filters can be installed..." with no diagnostic at all.

With 16K pages the member is 16384 bytes, comfortably above 5120, and the same
code works. That is the whole reason this is a page-size note: the failure is
invisible on one of the two page sizes this port has to support, and the
*correct* size has nothing to do with the page size.

Fixed by registering the whole shared area, which is what the real stub already
does in `stub_exe.c` (`STUB_DATA_PAGES * UM_KERN_PAGE_SIZE`, so 8K at 4K pages).

The lesson generalises: a `PAGE_SIZE` used where the requirement is really "at
least N bytes for reason X" is a latent bug even when it is spelled
`UM_KERN_PAGE_SIZE` rather than `4096`, and grepping for the literal will never
find it.


## 16 KB, verified rather than reasoned about

This document used to end by saying the arithmetic was parametric and the
behaviour was not, because no 16 KB host existed here. That was a real gap and
it has been closed by building one: an arm64 kernel with
`CONFIG_ARM64_16K_PAGES=y` for the qemu run domain, booted with `-kernel`
(`VM_KERNEL` in `harness/vm.sh`). `getconf PAGESIZE` in the run domain reports
16384.

Four combinations, all measured:

| guest | host | result |
| --- | --- | --- |
| 16 KB | 4 KB | **pass** -- boots, Alpine from ubd0, hostfs, 2000 fork+exec, FP torture |
| 16 KB | 16 KB | **pass** -- boots, Alpine from ubd0 |
| 4 KB | 4 KB | pass (the everyday configuration) |
| 4 KB | 16 KB | **hung, silently** |

The last row is the finding. A guest page is a host `mmap()`, so the guest's
page size cannot be smaller than the host's; below the host's granularity mmap
simply fails. UML stopped dead just after the ptrace capability checks with no
message at all, because the next thing it does is map the stub. That is not an
exotic configuration -- 4 KB is the default here and Android 15+ ships 16 KB
kernels, so it is what the first person to try this on a modern phone would get.

Now:

    Host page size is 16384 bytes but this kernel was built for 4096.
    A guest page is a host mmap(), so it cannot be smaller than the
    host's page size. Rebuild with CONFIG_PAGE_SIZE_16KB.

`HAVE_PAGE_SIZE_16KB` is now selected, so the option exists to rebuild *with*.

Two things only a real 16 KB host could show:

* the `sigstack[]` sizing problem **disappears** at 16 KB -- the stub's area is
  32768 bytes against an `AT_MINSIGSTKSZ` of 4720, where at 4 KB it is 8192
  against 9984 on an SVE host. Which is the strongest argument that the fix is
  "as many bytes as this host says it needs", read at boot, and never a page
  count worked out on a 4 KB machine.
* `THREAD_SIZE` and the stack-scanning masks in `arch/um/kernel/stacktrace.c`
  change with the page size (0x3fff/2048 iterations at 4 KB, 0xffff/8192 at
  16 KB), so the unwinder's cost is page-size dependent too.
