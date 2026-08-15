#!/usr/bin/env python3
"""Drive the run-domain VM's serial console over its unix socket.

Used exactly once, for first boot: the Debian nocloud image has no cloud-init
datasource, so the only way in is the console. After this installs an ssh key,
everything else in the harness goes over ssh.
"""
import argparse
import re
import socket
import sys
import time


class Console:
    def __init__(self, path, log=None):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.connect(path)
        self.sock.settimeout(0.5)
        self.buf = b""
        self.log = open(log, "ab") if log else None

    def _pump(self):
        try:
            data = self.sock.recv(65536)
        except socket.timeout:
            return b""
        if not data:
            raise EOFError("console closed")
        self.buf += data
        if self.log:
            self.log.write(data)
            self.log.flush()
        return data

    def expect(self, pattern, timeout=120, quiet=False):
        rx = re.compile(pattern.encode() if isinstance(pattern, str) else pattern)
        deadline = time.time() + timeout
        while time.time() < deadline:
            m = rx.search(self.buf)
            if m:
                self.buf = self.buf[m.end():]
                return m
            self._pump()
        tail = self.buf[-2000:].decode("utf-8", "replace")
        raise TimeoutError(f"timeout waiting for {pattern!r}\n--- tail ---\n{tail}")

    def send(self, text):
        self.sock.sendall(text.encode() if isinstance(text, str) else text)

    def sendline(self, text=""):
        self.send(text + "\n")

    def drain(self, seconds=1.0):
        deadline = time.time() + seconds
        while time.time() < deadline:
            self._pump()
        out, self.buf = self.buf, b""
        return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sock", default="/root/mlu-arm64/vm/console.sock")
    ap.add_argument("--log", default="/root/mlu-arm64/vm/console-driver.log")
    ap.add_argument("--pubkey", default="/root/.ssh/umvm_ed25519.pub")
    ap.add_argument("--timeout", type=int, default=900)
    args = ap.parse_args()

    pubkey = open(args.pubkey).read().strip()
    c = Console(args.sock, args.log)

    # Nudge the console; nocloud images drop to a root shell or a login prompt.
    print("[*] waiting for a prompt...", flush=True)
    c.sendline()
    deadline = time.time() + args.timeout
    prompt_re = re.compile(rb"(localhost login:|[\w.-]+ login:|root@[\w.-]+:[^\n]*#|~# )")
    got = None
    while time.time() < deadline:
        c.sendline()
        try:
            m = c.expect(prompt_re, timeout=10)
            got = m.group(1).decode()
            break
        except TimeoutError:
            continue
    if got is None:
        print("[!] no prompt within timeout", file=sys.stderr)
        print(c.drain(2).decode("utf-8", "replace")[-4000:], file=sys.stderr)
        return 1
    print(f"[*] got: {got!r}", flush=True)

    if "login:" in got:
        c.sendline("root")
        time.sleep(1)
        blob = c.drain(3).decode("utf-8", "replace")
        if "assword" in blob:
            c.sendline("")
            time.sleep(1)
            c.drain(2)

    # Confirm we have a shell by asking it something unambiguous.
    c.sendline("echo UMARM_SHELL_$((6*7))")
    c.expect(r"UMARM_SHELL_42", timeout=30)
    print("[*] root shell confirmed", flush=True)

    setup = f"""
set -x
mkdir -p /root/.ssh && chmod 700 /root/.ssh
cat > /root/.ssh/authorized_keys <<'EOF'
{pubkey}
EOF
chmod 600 /root/.ssh/authorized_keys
systemctl enable --now ssh 2>/dev/null || systemctl enable --now sshd 2>/dev/null || true
mkdir -p /etc/ssh/sshd_config.d
printf 'PermitRootLogin prohibit-password\\nPasswordAuthentication no\\nUseDNS no\\n' > /etc/ssh/sshd_config.d/90-umarm.conf
systemctl restart ssh 2>/dev/null || systemctl restart sshd 2>/dev/null || true
systemctl is-active ssh sshd 2>/dev/null | head -2
ip -brief addr
echo UMARM_SETUP_DONE
"""
    for line in setup.strip().splitlines():
        c.sendline(line)
        time.sleep(0.05)
    try:
        c.expect(r"UMARM_SETUP_DONE", timeout=180)
    except TimeoutError as e:
        print(f"[!] setup did not report done: {e}", file=sys.stderr)
        return 1
    print("[*] ssh key installed", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
