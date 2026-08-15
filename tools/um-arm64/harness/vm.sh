#!/bin/bash
# The aarch64 "run domain": a plain Debian trixie arm64 guest under qemu-system-aarch64.
#
# This guest is what the goal calls "the host" -- it is the machine ./linux runs on, and
# it must stay ordinary. Nothing here grants ./linux any capability a stock arm64 Linux
# box would not have: no KVM (there is none for an emulated arm64 guest anyway), no
# out-of-tree modules, no special sysctls. The only concessions to automation are a
# forwarded ssh port and a 9p share, both of which live outside the UML process.
set -euo pipefail

VMDIR=${VMDIR:-/root/mlu-arm64/vm}
SHAREDIR=${SHAREDIR:-/root/mlu-arm64}
SSH_PORT=${SSH_PORT:-10022}
VM_SMP=${VM_SMP:-8}
VM_MEM=${VM_MEM:-16G}
# cortex-a76 is the default: a realistic modern core, no SVE, fastest under TCG.
# Use VM_CPU=max to get SVE and exercise the sve_context path in the signal frame.
VM_CPU=${VM_CPU:-cortex-a76}

# Boot a kernel of our own instead of the guest's installed one.
#
# This exists to get a 16 KB page-size host, which the port has to support
# (Android 15+ ships 16 KB) and which no machine here otherwise provides. The
# alternative -- working out the port's page arithmetic against a 4 KB host and
# checking 16 KB later -- is how a 4 KB assumption gets cemented into code that
# was specifically written to be parametric.
#
# The guest root filesystem is plain ext4 on vda1 and needs no initramfs, so
# this only requires that the kernel have virtio-blk and ext4 built in.
VM_KERNEL=${VM_KERNEL:-}
VM_APPEND=${VM_APPEND:-"root=/dev/vda1 rw console=ttyAMA0"}

CONSOLE_SOCK="$VMDIR/console.sock"
MONITOR_SOCK="$VMDIR/monitor.sock"
PIDFILE="$VMDIR/qemu.pid"
CONSOLE_LOG="$VMDIR/console.log"

vm_running() { [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; }

vm_start() {
    if vm_running; then echo "vm already running (pid $(cat "$PIDFILE"))"; return 0; fi
    rm -f "$CONSOLE_SOCK" "$MONITOR_SOCK"

    local BOOT_ARGS
    if [ -n "$VM_KERNEL" ]; then
        [ -r "$VM_KERNEL" ] || { echo "no kernel at $VM_KERNEL" >&2; return 2; }
        BOOT_ARGS=(-kernel "$VM_KERNEL" -append "$VM_APPEND")
        echo "booting custom kernel $VM_KERNEL"
    else
        BOOT_ARGS=(
            -drive if=pflash,format=raw,unit=0,readonly=on,file="$VMDIR/code.fd"
            -drive if=pflash,format=raw,unit=1,file="$VMDIR/vars.fd"
        )
    fi
    # MTTCG: an arm64 guest on an x86_64 host is safe to run multi-threaded because the
    # host memory model is strictly stronger than the guest's. This is the single biggest
    # TCG speed win available and it is why VM_SMP is worth setting above 1.
    qemu-system-aarch64 \
        -machine virt,gic-version=3 \
        -cpu "$VM_CPU" \
        -accel tcg,thread=multi,tb-size=1024 \
        -smp "$VM_SMP" -m "$VM_MEM" \
        "${BOOT_ARGS[@]}" \
        -drive if=virtio,format=qcow2,file="$VMDIR/debian-13-nocloud-arm64.qcow2",cache=writeback \
        -netdev user,id=n0,hostfwd=tcp::"$SSH_PORT"-:22 \
        -device virtio-net-pci,netdev=n0,romfile= \
        -virtfs local,path="$SHAREDIR",mount_tag=umshare,security_model=none,id=sh0 \
        -device virtio-rng-pci \
        -display none \
        -chardev socket,id=con0,path="$CONSOLE_SOCK",server=on,wait=off,logfile="$CONSOLE_LOG" \
        -serial chardev:con0 \
        -monitor unix:"$MONITOR_SOCK",server,nowait \
        -pidfile "$PIDFILE" \
        -daemonize
    echo "vm started, console socket $CONSOLE_SOCK, ssh port $SSH_PORT"
}

vm_stop() {
    if ! vm_running; then echo "vm not running"; return 0; fi
    local pid; pid=$(cat "$PIDFILE")
    ssh_cmd "poweroff" 2>/dev/null || true
    # Wait generously: the disk is cache=writeback, so a SIGKILL here discards
    # whatever the guest has not flushed -- which silently loses staged test
    # images and makes the next run fail for a reason that has nothing to do
    # with the kernel under test.
    for _ in $(seq 1 120); do kill -0 "$pid" 2>/dev/null || { echo "vm stopped"; return 0; }; sleep 1; done
    kill "$pid" 2>/dev/null || true; sleep 2; kill -9 "$pid" 2>/dev/null || true
    echo "vm killed"
}

COMMON_OPTS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null
             -o LogLevel=ERROR -o ConnectTimeout=5 -i /root/.ssh/umvm_ed25519)
# scp spells the port -P; -p means "preserve timestamps" and would swallow the
# port number as a filename.
SSH_OPTS=(-p "$SSH_PORT" "${COMMON_OPTS[@]}")
SCP_OPTS=(-P "$SSH_PORT" "${COMMON_OPTS[@]}")
ssh_cmd() { ssh "${SSH_OPTS[@]}" root@127.0.0.1 "$@"; }
scp_to()  { scp "${SCP_OPTS[@]}" "$1" root@127.0.0.1:"$2"; }

vm_wait_ssh() {
    local deadline=$((SECONDS + ${1:-600}))
    while [ $SECONDS -lt $deadline ]; do
        if ssh_cmd true 2>/dev/null; then echo "ssh up"; return 0; fi
        sleep 3
    done
    echo "ssh did not come up within ${1:-600}s" >&2; return 1
}

case "${1:-}" in
    start)   vm_start ;;
    stop)    vm_stop ;;
    status)  vm_running && echo "running (pid $(cat "$PIDFILE"))" || echo "stopped" ;;
    wait)    vm_wait_ssh "${2:-600}" ;;
    ssh)     shift; ssh_cmd "$@" ;;
    scp)     shift; scp_to "$1" "$2" ;;
    console) exec socat - UNIX-CONNECT:"$CONSOLE_SOCK" ;;
    *) echo "usage: $0 {start|stop|status|wait [secs]|ssh CMD|scp SRC DST|console}" >&2; exit 2 ;;
esac
