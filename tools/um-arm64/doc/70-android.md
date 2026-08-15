# The phone

`ARCH=um SUBARCH=arm64` boots to its marker on an ordinary Android handset, as
an unprivileged process, with no root and nothing installed.

## The device

| | |
| --- | --- |
| model | Xiaomi `M2012K11AG` (`alioth`, Poco F3 / Redmi K40) |
| Android | 15 (SDK 35), custom ROM |
| kernel | **4.19.246** aarch64 |
| page size | **4096** |
| VA bits | **39** (user address space is 512 GiB) |
| SELinux | **enforcing**, domain `u:r:shell:s0` |
| uid | 2000 (`shell`) -- not root |
| adb | over TCP |

The kernel version is the point. Android 15 at the application level says nothing
about the kernel underneath: this device pairs a 2024-era userspace with a 4.19
kernel from the 2021 hardware launch. Anything the port assumes about host kernel
age fails here, and this is a normal device, not a contrived one.

## What the phone required

Four changes, and every one of them is a general portability fix rather than an
Android workaround. Three are in generic `arch/um/`.

### 1. `PTRACE_SYSEMU` does not exist before 5.3 on arm64

`userspace()` resumes the stub with `PTRACE_SYSEMU`, which stops at syscall
entry without executing the call. arm64 gained it in 5.3; x86 has had it since
2.6, which is why UML has never needed an alternative.

`PTRACE_SYSEMU` is not doing anything a tracer cannot do itself. Writing -1 into
the in-flight syscall number cancels the syscall, and that has worked since the
architecture had a syscall-number regset -- 3.19 on arm64. The port already
writes that regset (`ptrace_set_syscall_nr()`) and already single-steps off
every syscall stop to recover x7, so the substitution costs **one extra ptrace
call per guest syscall and no extra stop**:

```
   with SYSEMU:  PTRACE_SYSEMU  -> entry stop -> [x7 step] -> resume
   without:      PTRACE_SYSCALL -> entry stop -> cancel -> [x7 step] -> resume
```

`check_sysemu()` now probes rather than assumes: it tries `PTRACE_SYSEMU`, and
where that fails it runs the cancellation end to end -- cancel, step, set the
return value, and confirm the child saw the tracer's value instead of the real
syscall's -- before committing to it. A host that can do neither is refused with
a message that says so.

`nosysemu` on the command line forces the fallback on a host that has
`PTRACE_SYSEMU`, so every existing test can run against that path on any
machine. Without it the only way to exercise it is to find a 4.x arm64 host,
which is the environment that is hardest to debug on.

### 2. `TASK_SIZE` computed to zero on a 39-bit host

This one cost the most to find and is the most likely to bite someone else.

```
Address space: host top 0x8000000000, stub at 0x7fffffd000, guest TASK_SIZE 0x0
```

`task_size &= PGDIR_MASK` rounds the guest's address-space limit *down* to a
multiple of `PGDIR_SIZE`. A 64-bit UML guest has `PGDIR_SIZE == 512 GiB`
(`CONFIG_PGTABLE_LEVELS=4`, `PGDIR_SHIFT=39`). An arm64 host built with
`CONFIG_ARM64_VA_BITS_39` -- the usual choice for a 4 KiB-page kernel, and what
Android devices overwhelmingly ship -- has *exactly* 512 GiB of user address
space, and the stub is carved out of the top of it. So the largest aligned value
that fits is zero.

A zero `TASK_SIZE` fails every guest `mmap`. The first `execve` returns
`-ENOMEM`, and the boot dies with:

```
Failed to execute /init (error -12)
Kernel panic - not syncing: Requested init /init failed (error -12).
```

Nothing in that mentions address space, page tables or the host. There is no
message from UML at all, because nothing failed at the host interface -- the
guest simply had nowhere to put anything.

The fix aligns to `PUD_SIZE` when `PGDIR_SIZE` alignment would produce zero.
UML's page tables are software structures that UML itself walks -- the host does
the real translation through `mmap` -- so the granularity is a choice rather
than a hardware constraint. On a 48-bit host nothing changes: the original
branch is taken and `TASK_SIZE` is identical to before.

`linux_main()` now prints the three numbers on every boot. Reconstructing them
after the fact needs a debugger attached to a process on a phone; one line of
`os_info` is cheaper than that, and it is what turned this from a silent
`-ENOMEM` into a five-minute diagnosis.

### 3. `MFD_EXEC` is a 6.3 flag

The stub is written into a memfd and started with `execveat`. `memfd_create`
rejects unknown flags with `EINVAL`, so on 4.19 the call fails and UML falls
back to writing the stub to a file in the tempdir and executing that.

That fallback works, but it is the wrong thing to depend on here: executing a
file requires a tmpdir whose SELinux label the `shell` domain may execute from,
and it puts the stub on disk. On kernels before 6.3 a memfd is executable
anyway -- `MFD_EXEC` exists precisely because that default was being tightened --
so UML now retries without the flag. The stub stays in memory on 4.19.

### 4. `HOME` is unset under adb

UML builds its umid directory from `$HOME`, which is unset in an adb shell, so
the path becomes `//.uml/` on the read-only rootfs. `make_umid()` then
segfaults instead of reporting the failed `mkdir`:

```
Failed to mkdir '//.uml/': Read-only file system
Kernel panic - not syncing: Segfault with no mm
```

Worked around in the harness by setting `HOME`. The segfault on a failed mkdir
is a real robustness bug and is *not* fixed yet.

## Running it

```
adb push linux-android-4k /data/local/tmp/linux-um
adb shell 'cd /data/local/tmp && HOME=/data/local/tmp TMPDIR=/tmp \
    ./linux-um mem=512M panic=-1 con=null con0=null,fd:1 \
    ubd0=alpine.ext4 root=/dev/ubda rw init=/gate3-init'
```

`TMPDIR=/tmp` matters: Android sets `TMPDIR=/data/local/tmp`, which is f2fs, and
UML prefers `TMPDIR` over `/tmp` when choosing where to put guest RAM. This ROM
has a writable tmpfs on `/tmp` with 3.7 GiB free, so guest memory need not be
backed by flash.

`harness/android.sh` runs a gate with the same discipline as `harness/boot.sh`.

## Results

| gate | result |
| --- | --- |
| 2 -- initramfs to a marker | **pass** |
| 3 -- Alpine rootfs from `ubd0` | **pass**, Alpine 3.21.3, 45 processes, working pipeline |
| 4 -- fork/exec stress | **pass**, 2000 sequential, 64 concurrent, `pipeline_sum` correct |
| 11 -- ptrace inside the guest | **pass**, 18/18 |

Gate 11 is the one worth pausing on: the guest is running its own `ptrace`
tests, with `PTRACE_SINGLESTEP` and the `NT_ARM_SYSTEM_CALL` regset, *through* a
host that has no `PTRACE_SYSEMU` and is being driven by syscall cancellation.
Nested ptrace, on the substituted path, all 18 checks matching what the same
binary reports on hardware.

Also confirmed on the device before any of this: `execveat` on a memfd is
permitted for the `shell` domain, as is `ptrace` (`harness/probe/androidhost.c`,
`harness/probe/guestptrace.c` run natively). Those were the two things that
could have made the whole exercise impossible.

## Three host classes, both paths

| host | pages | VA | `PTRACE_SYSEMU` | normal | `nosysemu` |
| --- | --- | --- | --- | --- | --- |
| qemu run domain | 4 KiB | 48-bit | yes | pass | pass |
| Raspberry Pi 5 | 16 KiB | 47-bit | yes | pass | pass |
| Poco F3, Android 15 | 4 KiB | **39-bit** | **no** | n/a | pass |

## The shipped thing: `dist/umdebian`

A directory containing a kernel, a Debian root filesystem and a launcher.
Nothing is installed in the Android sense -- no apk, no root, no mounts, no
package manager on the Android side. `rm -rf` removes it completely.

```
adb shell
cd /data/local/tmp/umdebian
./deb                    # interactive Debian shell
./deb uname -a           # run one command, print its output
./deb -v uname -a        # same, plus the kernel boot log
```

* **Kernel**: `linux-android-4k`, 78 MB, statically linked.
* **Rootfs**: Debian bookworm 12.15 arm64, built with `mke2fs -d` straight from
  the debootstrap tree -- no loop mount, so it can be built unprivileged. 1.4 GB
  image, 722 MB used, ~680 MB free; 303 MB compressed for transfer, expanded on
  the device.
* **PID 1** is `/umarm-init` in the image: mounts `/proc`, `/sys`, `devtmpfs`,
  `devpts` and a tmpfs on `/tmp`, optionally mounts hostfs, then either runs the
  launcher's command or gives an interactive login shell. No systemd, no
  network.

Three details worth keeping:

* **The share path comes from the kernel command line** (`umarm.share=`), not
  from a constant in the image. The gate-5 image in this project has its host
  path compiled into its init and stopped working the moment it moved to another
  machine; an image should not know where it is running.
* **The command is passed through hostfs, not the command line.** A kernel
  command line cannot carry spaces or quoting, so `./deb apt list --installed`
  would not survive it.
* **The guest brackets its output with `UMARM_OUTPUT_BEGIN` / `UMARM_EXIT=`** so
  the launcher can show the command's output alone. When those markers are
  absent the launcher prints the whole boot log and exits non-zero, because a
  command that produced no output and a boot that failed must not look the same.
* **`setsid -c`** for the interactive shell. Without it bash prints "cannot set
  terminal process group" and "no job control in this shell" on every start, and
  ^C does not work -- init is already the session leader, so bash cannot claim
  `/dev/console`.

`install.sh` refuses to install a 4 KiB-page kernel on a 16 KiB-page device
rather than leaving the user with the silent hang that mismatch produces.

One more failure the launcher translates: UML takes an **exclusive `flock` on
the disk image**, so a second instance -- or a first one that was killed and has
not finished dying -- cannot open it. The kernel reports that as a root-mount
panic with a stack trace, which reads exactly like a corrupt filesystem and is
not; `deb` recognises `Failed to lock` and says "another umdebian is already
running" instead. Worth knowing independently of the launcher, because the same
panic will appear in any harness that leaves a UML process behind.

Verified on the phone: `./deb <command>` end to end, exit status propagation
(`./deb "exit 42"` returns 42), the shared directory both ways, and gcc
compiling and running a program inside the guest. The **interactive** shell is
verified on the run domain -- motd, `root@umdebian:/#`, working prompt -- but
not on the phone, because verifying it needs a real terminal and feeding
`adb shell -t -t` from a pipe stalls at console setup. It is one command for
someone sitting at a terminal to confirm.

## Harness traps specific to adb

All four cost real time, and all four produce the same symptom -- a boot log
that stops partway with no error -- which is indistinguishable from the kernel
dying.

* **`adb shell` truncates streamed stdout.** `adb shell '...' | tail` returned a
  log that stopped at console initialisation; the same boot captured to a file
  *on the device* was 90 lines and ended in the success marker. Gates 3 and 4
  were reported as failing for half an hour while they were passing. Capture on
  the device, then `adb pull`.
* **Redirecting the kernel's stdout to a file on the device breaks the console.**
  UML registers its console fds with epoll, and `epoll_ctl` on a regular file
  fails with `EPERM` ("epollctl add err fd 1, Operation not permitted"), after
  which output stops. Pipe through `cat` instead -- a pipe is pollable.
* **`con0=fd:0,fd:1` makes the run depend on what fd 0 is.** With the harness's
  stdin attached the boot reaches its marker in 84 lines; with stdin on
  `/dev/null` it stops at 80 and everything the guest wrote is discarded, for
  the same epoll reason. Gates are not interactive: use `con0=null,fd:1` and
  do not ask for console input at all.
* **`adb shell` exit status is meaningless.** It reports 0 regardless of what
  the remote command did unless both ends negotiate shell protocol v2, so
  `adb shell "[ -f x ]" || push` never pushes. Test by asking for output. The
  goal's rule -- the verdict is a marker string, never an exit code -- turns out
  to be load-bearing here rather than stylistic.

And one that was not adb's fault: gates 3 and 4 were run while the 384 MB disk
image was still being pushed in the background, which is the same shared-staging
race that `harness/boot.sh` takes a lock to prevent. `android.sh` now keeps a
pristine copy on the device and restores it before each run.

## Not done on the phone

* **Gates 5, 6, 7, 9** -- hostfs, Debian, `apt`, and building a C program in the
  guest. Nothing suggests they will not work; the Debian image is 2.7 GB and has
  not been pushed.
* **The syscall census cannot be taken here.** It uses ftrace, and
  `/sys/kernel/tracing` is not readable by uid `shell`. The static enumeration in
  doc/30 covers the phone; the dynamic list is still from the run domain.
* **`make_umid()`'s segfault on a failed mkdir** (item 4 above) is worked around,
  not fixed.
* **The flake loop has never run here.** Every result above is a single run.
