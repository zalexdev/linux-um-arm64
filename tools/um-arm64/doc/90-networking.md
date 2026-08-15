# Networking for an unrooted guest

The guest gets a working network -- DHCP, DNS, TCP, ICMP -- on a phone where the
process running it has uid 2000, no capabilities, no root and no user
namespaces.

## Why the obvious routes are all closed

UML's network transports, and why each one is unavailable here:

| transport | needs | on Android as `shell` |
| --- | --- | --- |
| `tap` | `/dev/net/tun`, CAP_NET_ADMIN | no |
| `raw` | raw sockets, CAP_NET_RAW | no |
| `gre`, `l2tpv3` | raw sockets | no |
| `bess`, `vde` | a switch daemon, which still needs a tap to reach the world | no |
| **`fd`** | **a descriptor somebody else set up** | **yes** |

Modern UML has also dropped the old `slirp` and `slip` transports; only the
vector driver remains. So the only door left is `fd`, which reads and writes
Ethernet frames on a descriptor and asks no questions about where it came from.

On the other side of that descriptor has to be a userspace TCP/IP stack: a
program that accepts the guest's Ethernet frames, terminates the connections
itself, and reaches the real network the only way an unprivileged process can --
by opening ordinary sockets.

## The pieces

```
   guest            UML kernel              umnet                  passt
  vec0  <--frames--> vector fd  <--SOCK_DGRAM--> pump <--SOCK_STREAM--> tap
                                             (adds/strips              |
                                              length prefix)      ordinary
                                                                  sockets
                                                                      |
                                                                  the network
```

**passt** is the stack. It was chosen over libslirp for one practical reason:
libslirp depends on glib, and cross-compiling glib statically for Android is a
project in itself. passt is plain C with nothing but libc, so
`make CC=aarch64-linux-gnu-gcc LDFLAGS=-static` produces a 1.4 MB binary that
runs on the phone as-is.

**umnet** (`harness/umnet.c`) exists because of a framing mismatch, and only
that:

* UML's `fd` transport does one frame per datagram -- boundaries come from the
  socket.
* passt speaks qemu's socket protocol on a stream -- each frame preceded by its
  length as a 4-byte big-endian integer.

So umnet holds a `SOCK_DGRAM` socketpair towards UML and a `SOCK_STREAM`
socketpair towards passt, adds the prefix in one direction and strips it in the
other, and execs the UML kernel with the guest end inherited, appending
`vec0:transport=fd,fd=N,mac=...` to its command line.

## One kernel change

`CONFIG_UML_NET_VECTOR` used to `select MAY_HAVE_RUNTIME_DEPS`, and
`CONFIG_STATIC_LINK` depends on that symbol being unset -- so enabling vector
networking made a static build impossible. On Android the binary *must* be
static, because there is no glibc to link against. The combination was therefore
unbuildable, which is to say: no networking at all.

The select was over-broad. `getaddrinfo()` appears exactly twice in the driver,
both inside `user_init_socket_fds()`, reached only by the `gre` and `l2tpv3`
transports. `tap`, `raw`, `bess`, `vde` and `fd` never call it, and where it is
called a static binary degrades rather than breaks -- numeric addresses resolve
without NSS. Dropped it, with the reasoning recorded in the Kconfig.

## Three patches to passt, all the same shape

passt self-isolates aggressively at startup. Every one of those steps assumes a
capability an Android app process does not have, and each was fatal:

1. **`ns_is_init()` opened `/proc/self/uid_map`** and called `die_perror()` if it
   could not. That file does not exist when the kernel is built without
   `CONFIG_USER_NS` -- which this one is (`# CONFIG_USER_NS is not set`). A
   missing uid_map means there is exactly one user namespace and we are in it,
   so ENOENT now returns true instead of dying.
2. **`unshare(CLONE_NEWUSER)`** fails with EINVAL for the same reason. Now a
   warning, not a death: passt continues as the unprivileged uid it already was.
3. **`unshare(CLONE_NEWIPC|CLONE_NEWNS|CLONE_NEWUTS)`** in `isolate_prefork()`
   fails with EPERM. Everything after it in that function -- remounting `/`, a
   private tmpfs, `pivot_root` -- needs the same privileges, so the whole prefork
   isolation is skipped rather than failing one step at a time.

These weaken passt's sandbox and that is a real trade, stated plainly: passt
ends up running as an ordinary unprivileged uid with no namespace isolation. Its
**seccomp filter still applies**, so the syscall surface is still restricted, and
the process never had privileges to lose in the first place. The alternative was
no networking.

All three are candidates to send upstream: "run correctly on a kernel without
CONFIG_USER_NS" is a reasonable thing for passt to support.

## Configuration passt cannot discover

passt normally reads the host's addresses, routes and `/etc/resolv.conf`. On
Android all three fail:

* netlink link enumeration is denied to ordinary uids -- passt reports
  `Invalid interface name wlan0: Permission denied`;
* the routing table is per-uid, so several default routes are visible and passt
  warns `Multiple default IPv4 routes, picked first`;
* there is no `/etc/resolv.conf` at all (DNS goes through netd), so passt gives
  up with `No IPv4 nameserver available for DHCP` and the guest gets no lease.

So umnet passes the whole configuration explicitly -- `-4 -a 10.0.2.15 -n 24
-g 10.0.2.2 -D 1.1.1.1` -- and passt then needs to discover nothing. It still
reaches the network by opening ordinary sockets, which is what makes this work
without root.

## Result

From the guest, on the phone:

```
udhcpc: lease of 10.0.2.15 obtained from 10.0.2.2, lease time 4294967295
    inet 10.0.2.15/24 scope global vec0
default via 10.0.2.2 dev vec0
nslookup example.com  ->  104.20.23.154, 172.66.147.243
wget http://example.com/  ->  OK, 559 bytes
<!doctype html><html lang="en"><head><title>Example Domain</title>...
```

ICMP works too: passt implements ping with `SOCK_DGRAM` ICMP sockets, which
Android permits for the shell uid via `ping_group_range`.

`./deb` wires this up automatically when `umnet` and `passt` are present next to
the kernel; `UMDEB_NET=0` turns it off.

## Gotchas

* **The guest interface is `vec0`, not `eth0`.** UML's vector driver names them
  that way. Half an hour went into a test script that configured an interface
  that did not exist.
* **The disk image is flock'd.** An interactive `./deb` session holds it, and any
  other run then aborts during root mount -- which looks exactly like a corrupt
  filesystem until you read the message. Also worth remembering when running
  benchmarks while somebody is using the guest.
* **Binding a UNIX socket under `/data/local/tmp` is denied** by SELinux, so
  passt's `-s` mode cannot be used. `-F` with an inherited descriptor can, which
  is what umnet does anyway.
