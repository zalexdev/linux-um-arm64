# Environment reality vs. goal premise

Recorded 2026-08-14, before any code was written. Re-verify with `harness/env-check.sh`.

## What the goal assumed

> Native aarch64 Linux, glibc, 18 cores, NVMe. Build and run on the same machine,
> no cross-compilation.

## What this machine actually is

| Property | Assumed | Actual |
| --- | --- | --- |
| Host arch | aarch64 | **x86_64** (`uname -m`) |
| CPU | arm64 server, 18 cores | AMD Ryzen 7 3700X, 8C/16T |
| RAM | — | 62 GiB + 31 GiB swap |
| Distro | — | Debian GNU/Linux 13 (trixie) |
| Kernel | — | 6.12.94+deb13-amd64 |
| `/tmp` | — | tmpfs, 32 GiB |
| Root fs | NVMe | `/dev/md2`, 2.0 TiB, 1.9 TiB free |
| `/dev/kvm` | — | present, but **x86 KVM only** — cannot accelerate an aarch64 guest |
| `ptrace_scope` | — | 0 (unrestricted) |
| seccomp filter | — | `CONFIG_SECCOMP_FILTER=y` on the host kernel |
| Network | — | up, Debian mirrors reachable |
| Preinstalled | clang, git | neither; installed from trixie repos |

There is no native arm64 hardware in this environment.

## Consequences

Two goal constraints were stated as absolutes but were in fact *consequences* of the
assumed native-arm64 host. They cannot both survive:

1. **"no cross-compilation"** — impossible. An `ARCH=um SUBARCH=arm64` kernel is an
   aarch64 ELF binary. Producing one on x86_64 is cross-compilation by definition.
2. **"build and run on the same machine"** — the *machine* stays the same, but build
   and run no longer share an execution domain.

Everything else in the goal is unaffected and is kept verbatim: `CONFIG_MMU=y`,
no `PTRACE_SYSEMU`, `NT_ARM_SYSTEM_CALL` for cancellation, seccomp fast path with a
ptrace fallback, PAGE_SIZE-parametric layout, no-ASLR-assumption, plain syscall budget,
machine-readable time-bounded gates, and gate order 1-9.

## Adapted architecture

    x86_64 host (build domain)                 aarch64 domain (run domain)
    ---------------------------                ---------------------------
    clang --target=aarch64-linux-gnu           qemu-system-aarch64, TCG, MTTCG
    LLVM=1, ccache, 16 threads, tmpfs          Debian trixie arm64 guest
    builds ./linux (aarch64 ELF)  ------->     runs ./linux under timeout
                                    9p/virtiofs share of the build tree

* **Build** stays on the x86_64 host: native-speed clang, 16 threads, ccache, tmpfs
  build tree. A kernel build stays in the minutes, which is what makes an agent loop
  viable.
* **Run** happens inside a `qemu-system-aarch64` Debian arm64 guest. This is a *real*
  aarch64 Linux kernel on an emulated aarch64 CPU, so everything the port depends on is
  genuine, not emulated-at-the-libc-level: `PTRACE_GETREGSET`/`NT_PRSTATUS`,
  `NT_ARM_SYSTEM_CALL` syscall cancellation, `NT_ARM_TLS`, seccomp filters and
  `SECCOMP_RET_TRAP`/user-notif, `struct sigcontext` + `__reserved[]` FP/SVE records,
  `sigaltstack`, `/proc/self/mem`, fixed-address `mmap` of the stub page.
* **Rejected: `qemu-user` aarch64 + binfmt_misc.** It would run `./linux` directly on
  the x86_64 host kernel and is far cheaper, but qemu-user does not meaningfully emulate
  `ptrace` of guest children, does not apply guest seccomp filters, and synthesises its
  own signal frames. UML's entire process model lives in exactly those three mechanisms.
  A port validated under qemu-user would be validated against a fiction.

### Cost of the deviation

TCG is roughly 10-20x slower than native. The gate budgets in `harness/` are scaled
accordingly and are recorded per gate; the *shape* of the loop (timeout, marker string,
gdb backtrace on SIGSEGV, per-iteration archive) is unchanged.

### What stays honest about the toolchain

The goal's reason for mandating clang from commit one — "Android uses NDK clang; a
gcc-only port diverges exactly in the inline asm and attributes of the stub and mcontext
code you are writing yourself" — is fully preserved. The build uses `LLVM=1` with clang
targeting aarch64. The only x86-host-specific additions are `--target=` and `--sysroot=`,
both confined to one place (`harness/build.sh`); on a real arm64 host they drop out and
the identical command line builds natively.

## Page size

Host page size here is 4 KiB, and the arm64 guest is configured 4 KiB. No 16 KiB host
exists in this environment, exactly as the goal anticipated. Enforcement is therefore:

* every literal `4096` in new code is flagged and justified in `doc/40-page-size.md`;
* address-space arithmetic is expressed in `PAGE_SIZE` / `UM_KERN_PAGE_SIZE` terms;
* a parametric self-test exercises the stub layout math at 4 K, 16 K and 64 K without
  needing a host of that page size.


## i386 UML cannot be built here

`make ARCH=um SUBARCH=i386` fails in this environment before reaching any of
this port's code:

    /usr/include/stdio.h:28:10: fatal error: 'bits/libc-header-start.h' file not found

The build domain has no 32-bit glibc headers and `libc6-dev-i386` cannot be
fetched, so the i386 subarch is unbuildable independently of anything changed
here. Where a change has to touch i386 -- `stub_seccomp_save_state()` in
`arch/x86/um/shared/sysdep/stub_32.h` is the only one -- it is written to mirror
the x86-64 version exactly and is marked in its commit message as inspected
rather than compiled. x86-64 UML *is* built and booted as a control on every
change that touches generic `arch/um` code.
