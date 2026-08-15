#!/bin/bash
# Record the build/run environment. Emits key=value lines; archived every iteration.
# Deliberately does not fail on anything: it reports, the caller decides.
set -u

kv() { printf '%s=%s\n' "$1" "${2:-}"; }

kv host.uname          "$(uname -srm)"
kv host.arch           "$(uname -m)"
kv host.pagesize       "$(getconf PAGESIZE)"
kv host.nproc          "$(nproc)"
kv host.mem_kb         "$(awk '/MemTotal/{print $2}' /proc/meminfo)"
kv host.distro         "$(. /etc/os-release 2>/dev/null && echo "$PRETTY_NAME")"
kv host.ptrace_scope   "$(cat /proc/sys/kernel/yama/ptrace_scope 2>/dev/null || echo n/a)"
kv host.randomize_va   "$(cat /proc/sys/kernel/randomize_va_space 2>/dev/null || echo n/a)"
kv host.mmap_min_addr  "$(cat /proc/sys/vm/mmap_min_addr 2>/dev/null || echo n/a)"
kv host.overcommit     "$(cat /proc/sys/vm/overcommit_memory 2>/dev/null || echo n/a)"

kv tool.clang          "$(clang --version 2>/dev/null | head -1)"
kv tool.lld            "$(ld.lld --version 2>/dev/null | head -1)"
kv tool.llvm_objcopy   "$(llvm-objcopy --version 2>/dev/null | head -1)"
kv tool.make           "$(make --version 2>/dev/null | head -1)"
kv tool.git            "$(git --version 2>/dev/null)"
kv tool.ccache         "$(ccache --version 2>/dev/null | head -1)"
kv tool.gdb            "$(gdb --version 2>/dev/null | head -1)"
kv tool.qemu_aarch64   "$(qemu-system-aarch64 --version 2>/dev/null | head -1)"

kv sysroot.aarch64     "$(test -d /usr/aarch64-linux-gnu && echo /usr/aarch64-linux-gnu || echo missing)"
kv sysroot.libc        "$(ls /usr/aarch64-linux-gnu/lib/libc.so.6 2>/dev/null || echo missing)"

if [ -n "${UMARM_TREE:-}" ] && [ -d "${UMARM_TREE}/.git" ]; then
    kv tree.head       "$(git -C "$UMARM_TREE" rev-parse HEAD 2>/dev/null)"
    kv tree.describe   "$(git -C "$UMARM_TREE" describe --tags --always 2>/dev/null)"
    kv tree.dirty      "$(test -n "$(git -C "$UMARM_TREE" status --porcelain 2>/dev/null)" && echo yes || echo no)"
fi
