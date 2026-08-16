# Linux/UML on arm64

`ARCH=um SUBARCH=arm64` — a Linux kernel running as an ordinary aarch64 userspace
process, so that a phone can run a real kernel without root, KVM or a hypervisor.

## Status

Working, and tested rather than asserted:

| | |
|---|---|
| Boot to userspace | Alpine and Debian 12, 4 KB and 16 KB guest pages |
| Syscall interception | `PTRACE_SYSEMU`; syscall **cancellation** where the host has none; syscall **substitution** where a seccomp filter rejects a cancelled call |
| Userspace mode | `SECCOMP` (default fast path) and `ptrace` |
| Loadable modules | `modprobe`/`rmmod`, out-of-tree driver, firmware loading |
| Networking | userspace TCP/IP (passt) over the vector `fd` transport — DHCP, DNS, TCP |
| USB passthrough | a real RTL8811AU dongle from an Android app into the guest over USB/IP, `rtw88_8821au` binds, `wlan0` appears and scans |
| Containers | `dockerd` with overlay2 on cgroup v2; `docker run hello-world` pulls and runs |
| Host libc | glibc and bionic (Android NDK) |

Not working, or not finished:

- **No 32-bit compat.** `CONFIG_COMPAT` is off; the guest runs aarch64 userspace only.
- **`wlan0` association is untested.** The radio comes up, the firmware loads and
  scanning returns real APs with signal levels. Associating needs credentials
  that were not available; nothing beyond scan has been exercised.
- **`BUG: Bad rss-counter state ... type:MM_FILEPAGES val:1`** used to print
  once per exiting process. It is absent at 16 KB pages once `PTRS_PER_PTE`
  follows `PAGE_SHIFT`, under a workload that produced it reliably before.
  Whether the 4 KB build is also clean is untested: the mechanism fixed there
  cannot occur at 4 KB, so if it still appears it has another cause.
- **No cpuset controller.** `CONFIG_CPUSETS` depends on `SMP` and UML is
  uniprocessor. `docker info` warns and runs; there is nothing to pin to.
- Guest vDSO time (`clock_gettime` without a host syscall) is written but not
  merged: it does not fit a 4 KB page and needs `GENERIC_VDSO_OVERFLOW_PROTECT`,
  without which guest time wraps after tens of minutes.

Three bugs worth naming, all found by running the port on phones rather than by
reading it, and all fixed on this branch:

- **Pointer authentication stayed enabled in the stub.** One stub per guest mm
  means a guest `fork()` starts a new stub, and starting a stub is an
  `execve()`, which on arm64 generates fresh PAC keys. The child returns into a
  stub whose `APIAKey` did not sign the return addresses on its stack, and
  glibc's `_Fork` is `paciasp` / `svc #0` / `autiasp` — so with FEAT_FPAC the
  child dies inside `_Fork` having printed nothing. NOPs on pre-Armv8.3
  hardware, fatal on Armv9. It breaks every container built with branch
  protection, on every fork.
- **Guest FP/SIMD state was never saved or restored across a signal in SECCOMP
  mode**, because the guard compared `host_fp_size` against
  `sizeof(struct user_fpsimd_state)` (528) rather than the payload actually
  copied (520). Go's `SIGURG` preemption then corrupted its own AES-GCM state,
  and every TLS transfer past a few megabytes died with `bad record MAC`.
- **`PTRS_PER_PTE` was a literal 512** where it has to be `PMD_SIZE >>
  PAGE_SHIFT`: right at 4 KB, four times too large at 16 KB, letting generic mm
  write PTEs past the window a PMD owns.

Tested on:

- **Xiaomi Mi 11** (Snapdragon 888), Android 15, host kernel `4.19.246`, 4 KB pages.
  That host has no `PTRACE_SYSEMU` — arm64 gained it in 5.3 — which is why the
  cancellation and substitution paths exist and are exercised daily rather than
  theoretically.
- An **arm64 Debian run domain** for the gates.

Both interception fallbacks can be forced anywhere with `nosysemu` and
`nocancel`, so they are testable without owning a 4.19 phone.

## Base

- Base: **Linux 7.2-rc4** (`origin/next`, the uml tree).
- Branch: **`um-arm64`**, 38 commits on top. There is no other branch; the
  history is linear and each commit is one change.
- Roughly a third of the series is not arm64-specific — x86 and generic `um/`
  fixes found on the way — and those stand on their own.

## Build

Cross-compiling from x86_64 needs nothing but clang: upstream already maps
`SUBARCH=arm64` to `--target=aarch64-linux-gnu` for `ARCH=um`, so the command
line is the same as it would be on an arm64 host.

**glibc** (development, the gates):

```sh
make ARCH=um SUBARCH=arm64 LLVM=1 defconfig
make ARCH=um SUBARCH=arm64 LLVM=1 -j$(nproc)
```

or `harness/build.sh`, which adds ccache and publishes the binary the gates run.

**bionic** (Android NDK, what an app can exec):

```sh
NDK=/path/to/android-ndk-r27c harness/build-bionic.sh
```

The kernel notices bionic by asking the compiler whether `__ANDROID__` is
defined, so the only difference in that script is the target triple. It links
static, because Android has no `/lib/ld-linux-aarch64.so.1` and an app may only
execute from its own native library directory.

Extra config fragments:

```sh
EXTRA_CONFIG="config/usb-wifi.config config/docker.config" harness/build-bionic.sh
```

## Run

One command, one shell:

```sh
./linux mem=512M ubd0=rootfs/alpine.ext4 root=/dev/ubda rw \
        init=/bin/sh con=null con0=fd:0,fd:1
```

`con0=fd:0,fd:1` puts the guest console on this terminal. Use `con0=null,fd:1`
when stdin is not pollable — UML registers console descriptors with epoll, and
`epoll_ctl` on a regular file fails with `EPERM`, after which the console stops
and the boot looks like it died.

Useful options added by this series:

| | |
|---|---|
| `seccomp=on/auto/off` | userspace mode; `on` is roughly 5× faster per syscall |
| `nosysemu` | pretend the host has no `PTRACE_SYSEMU` |
| `nocancel` | pretend the host rejects a cancelled (`-1`) syscall |
| `seccomp_spin=<us>` | stub spin budget before parking; `0` restores always-park |
| `prefault=<bytes>` | anonymous fault-around window; `0` disables |
| `stub_exe=<path>` | exec the stub from a path instead of a memfd |

## Tests

Everything below is reproducible, and the harness is deliberately stricter than
the thing it tests.

A gate is a boot that must print a marker string. **The verdict is the marker,
never the exit status** — `adb shell` returns 0 whatever the remote command did,
and a truncated log is indistinguishable from a kernel that stopped.

```sh
GATE=g3 MARKER=UMARM_BOOT_OK INIT=/gate3-init \
  UBD0=$PWD/rootfs/alpine.ext4 EXTRA_ARGS="rw seccomp=on" harness/boot.sh
```

It prints `verdict=PASS|FAIL|SIGNAL`, the artifact directory, and
`kernel_bugs=N` — an **independent** scan for `BUG:`/`WARNING:` in the boot log,
because an early version had the marker short-circuit the bug check and a real
rss-counter BUG rode along inside a green gate.

The gate matrix, including the expensive ones sampled every N iterations:

```sh
N=20 harness/loop.sh            # g2mini g2alpine g3 g5 g3noaslr g4 fp fpnoaslr
```

On a phone over adb, same interface:

```sh
PUSH=1 BIN=$PWD/artifacts/linux-bionic GATE=g3 INIT=/gate3-init \
  UBD0=alpine.ext4 EXTRA_ARGS="seccomp=on" harness/android.sh
```

Output is captured **on the device** and pulled afterwards, never streamed:
streaming loses the tail, and every gate that "failed" on that phone before this
change was actually passing.

Related probes worth knowing about: `harness/probe/vethprobe.c` creates a veth
pair over rtnetlink, because busybox's `ip` does not know the veth link type and
fails identically whether or not `CONFIG_VETH` is set — useless as a test.

## Benchmarks

Measured on the Mi 11, `adb shell`, guest running `perfbench`. All three kernels
are conditions **inside one interleaved run**, so a difference between them
cannot be thermal drift or governor state.

Medians of 7 interleaved rounds, µs/op:

| | native | proot | before | +waiter-bit | +fault-around |
|---|---|---|---|---|---|
| syscall | 0.080 | 0.168 | 9.841 | 1.995 | **1.987** |
| openat | 2.527 | 27.393 | 20.223 | 4.473 | **4.501** |
| fault | 1.456 | 1.418 | 24.592 | 18.897 | **11.076** |
| forkexec | 3255 | 516 | 1197 | 1138 | **1137** |

(`before`/`+waiter-bit`/`+fault-around` are seccomp mode. The ptrace column moved
by ≤1% for the waiter-bit change, which is the check that the measurement follows
the code and not the machine: that change touches only the seccomp path.)

**Methodology**, because the numbers are worthless without it:

- Pinned to cores 4–5, with a busy loop held on core 6. Those three share one
  cpufreq policy, the governor is `schedutil` and there is no root to pin the
  clock, so without the keeper `perfbench` measures the frequency ramp. That
  alone moved the validator from 0.892 to 1.3 ns/iter.
- Conditions **interleaved**, not run in blocks, so drift hits all of them equally.
- Medians, never means.
- `compute` — pure userspace arithmetic, no kernel entry — is a **validator**.
  If it disagrees across conditions by more than 5%, or jitters more than 8%
  within one, the whole table is thrown away and nothing is printed. Identical
  instruction streams cannot differ; if they do, the machine moved.
- `native` and `proot` are controls. proot is there because it is the real
  alternative on an unrooted phone, not because bare metal is.

**Caveats:**

- One phone, one SoC. Nothing here says how this behaves elsewhere.
- These are `adb shell` numbers. The same kernel inside the Android app is
  unpinned and unboosted, and measures ~1.9 µs/syscall with `compute` about 18%
  slower — real-world rather than best-case.
- `syscall` is `getppid()`, the cheapest call there is, so it is the purest
  measure of interception overhead and the least like real work. `openat` is
  the row that resembles what programs actually do.
- `compute` is native speed by construction — guest user code runs as real
  instructions on the real CPU. That is the whole point, and it is why a
  compile inside the guest costs roughly what a compile costs.

Reproduce:

```sh
KERNELS="base=... waiter=... fault=..." ROUNDS=7 harness/verifybench.sh
```

It writes `manifest.txt` (git HEAD, md5 and `file(1)` of every binary, device
model, host kernel, page size, every cpufreq policy, battery state),
`results.tsv` (one row per sample), the raw transcript of every measurement, and
`env-before/after.txt`. The table is derived only from the TSV, and the TSV only
from the transcripts, so any of the three can be recomputed from the one below it.

## Upstream

I am not maintaining this and I am not planning to shepherd it through review.
Take whatever is useful — individual patches, the whole series, or just the
harness. It is GPL-2.0 like the rest of the kernel.

The series is `git format-patch`-ready: one change per commit, DCO signed off,
`checkpatch` clean apart from six false positives against idioms that appear
verbatim in mainline (`mb()` with an asm barrier, `ARCH_HAS_SETUP_ADDITIONAL_PAGES`,
`__SYSCALL(nr, sym)`).

If you do pick it up, the parts most likely to be independently useful are the
generic `um/` fixes and the x86 ones, which have nothing to do with arm64.
