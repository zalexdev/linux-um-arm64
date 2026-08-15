# umdebian — Debian on an Android phone, no root

A Debian bookworm userspace running on a Linux 7.2 kernel, on a phone, as an
ordinary unprivileged process. Not a container and not a VM: the kernel is
`ARCH=um SUBARCH=arm64` — User Mode Linux built for aarch64 — so it is a single
statically linked executable that runs under whatever uid you already have.

Nothing is installed on Android. No root, no `/dev/kvm`, no kernel modules, no
new mounts, no `CLONE_NEWUSER`. Deleting the directory removes it completely.

## Install

    ./install.sh

Pushes three files to `/data/local/tmp/umdebian`: the kernel (~78 MB), the
Debian image (~300 MB compressed, 1.4 GB on disk), and the launcher.

## Use

    adb shell
    cd /data/local/tmp/umdebian

    ./deb                    # interactive Debian shell
    ./deb uname -a           # run one command and print its output
    ./deb apt list --installed
    ./deb -v uname -a        # same, but show the kernel boot log too

Interactive mode needs a terminal, so run `adb shell` first and then `./deb`.
`adb shell ./deb` with no terminal will tell you so rather than appearing to
hang — UML takes its console from stdin, and a stdin that cannot be polled
leaves you with a booted guest whose output goes nowhere.

Files you put in `share/` appear inside the guest as `/host`, and it is
writable both ways.

## What works

An ordinary Debian userspace: `bash`, `dpkg`, `apt` (offline operations),
`perl`, the usual coreutils, and **gcc 12.2** — which compiles and runs
programs inside the guest:

    $ ./deb "gcc -O2 -o /tmp/hello /host/hello.c && /tmp/hello"
    compiled by gcc inside the guest, running on Linux aarch64
    6 * 7 = 42

209 packages installed, roughly 680 MB free in the image for more. `uname -m`
says `aarch64`. Exit status propagates, so `./deb false` returns 1 and the
launcher composes with the Android shell like any other command.

`python3` is **not** in this image — it is a base system plus a toolchain, not a
full desktop install. There is no network, so it cannot be apt-installed here
either; add it to the image before packaging if you want it.

There is **no network** and **no systemd**. PID 1 is a small script that mounts
`/proc`, `/sys`, `/dev`, and either runs your command or gives you a shell.

## Notes

* Guest RAM defaults to 1 GB; override with `UMDEB_MEM=2048M ./deb`.
* Guest memory is backed by a file in `/tmp` when that is a writable tmpfs,
  which keeps it out of flash. The launcher falls back to its own directory.
* The kernel here is built for **4 KiB** pages. A guest page is a host `mmap`,
  so it cannot be smaller than the host's page size; on a 16 KiB-page device
  (some Android 15+ hardware) use the 16 KiB build instead. `install.sh` checks
  and refuses rather than leaving you with a hang.
* Exiting the shell powers the kernel down and returns you to Android.

## Why this works at all on an old phone

The host kernel here is 4.19. Two things had to be fixed for that, both of them
general rather than Android-specific: `PTRACE_SYSEMU` does not exist on arm64
before 5.3, so the kernel builds the same behaviour out of `PTRACE_SYSCALL` plus
a syscall cancellation; and the guest's address-space limit computed to zero on
a host with a 39-bit virtual address space, which is what most 4 KiB-page arm64
kernels ship. See `doc/70-android.md`.
