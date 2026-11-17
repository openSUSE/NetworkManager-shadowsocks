#!/usr/bin/env python3
"""Real TUN forwarding across two private network namespaces (no Internet)."""
import shlex
import json
import os
import pathlib
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time


def ip(*args):
    subprocess.run(["ip", *args], check=True)


def echo(kind, port):
    with socket.socket(socket.AF_INET, kind) as listener:
        listener.settimeout(15)
        listener.bind(("0.0.0.0", port))
        if kind == socket.SOCK_STREAM:
            listener.listen()
            conn, _ = listener.accept()
            with conn:
                conn.settimeout(10)
                conn.sendall(conn.recv(1024))
        else:
            data, peer = listener.recvfrom(1024)
            listener.sendto(data, peer)


def server(directory):
    plugin = bool(os.environ.get("SS_TEST_PLUGIN"))
    print("ready", flush=True)
    sys.stdin.readline()
    ip("link", "set", "lo", "up")
    ip("addr", "add", "198.19.0.10/32", "dev", "lo")
    ip("addr", "add", "10.77.0.2/24", "dev", "ss-test-peer")
    ip("-6", "addr", "add", "2001:db8:77::2/64", "dev", "ss-test-peer", "nodad")
    ip("link", "set", "ss-test-peer", "up")
    ip("route", "add", "default", "via", "10.77.0.1")
    config = {"server": "10.77.0.2", "server_port": 18388,
              "method": "aes-256-gcm", "password": "test-only-secret",
              "mode": "tcp_and_udp"}
    if plugin:
        config.update(server="2001:db8:77::2", plugin="v2ray-plugin", plugin_opts="server")
    path = pathlib.Path(directory) / "server.json"
    path.write_text(json.dumps(config))
    child = subprocess.Popen(["ssserver", "-c", str(path)], stdout=sys.stderr)
    try:
        threads = [threading.Thread(target=echo, args=(kind, port), daemon=True)
                   for kind in (socket.SOCK_STREAM, socket.SOCK_DGRAM)
                   for port in (53, 15353)]
        for thread in threads:
            thread.start()
        print("listening", flush=True)
        sys.stdin.readline()
    finally:
        child.terminate()
        child.wait(timeout=5)


def main():
    plugin = bool(os.environ.get("SS_TEST_PLUGIN"))
    parent = os.environ.get("SS_TEST_PARENT_NETNS")
    if not parent or os.readlink("/proc/self/ns/net") == parent:
        return 77
    if not all(shutil.which(name) for name in ("sslocal", "ssserver", "ip", "unshare")):
        return 77
    with tempfile.TemporaryDirectory(prefix="nmss-tun-live-") as directory:
        peer = subprocess.Popen(["unshare", "--net", sys.executable, __file__,
                                 "--server", directory], stdin=subprocess.PIPE,
                                stdout=subprocess.PIPE, text=True)
        backend = None
        try:
            assert peer.stdout.readline().strip() == "ready"
            ip("link", "add", "ss-test-host", "type", "veth", "peer", "name", "ss-test-peer")
            ip("link", "set", "ss-test-peer", "netns", str(peer.pid))
            ip("addr", "add", "10.77.0.1/24", "dev", "ss-test-host")
            ip("-6", "addr", "add", "2001:db8:77::1/64", "dev", "ss-test-host", "nodad")
            ip("link", "set", "ss-test-host", "up")
            ip("-6", "route", "add", "default", "via", "2001:db8:77::2")
            ip("link", "set", "lo", "up")
            ip("route", "add", "default", "via", "10.77.0.2")
            peer.stdin.write("start\n")
            peer.stdin.flush()
            assert peer.stdout.readline().strip() == "listening"
            profile = {"name": "test", "mode": "lan", "ip_mode": "ipv4",
                       "dns": ["1.1.1.1"], "china": [], "bypass": []}
            for protocol in ("tcp", "udp"):
                profile[protocol] = {"server": "10.77.0.2", "server_port": 18388,
                                     "method": "aes-256-gcm", "password": "test-only-secret"}
            if plugin:
                for protocol in ("tcp", "udp"):
                    profile[protocol].update(server="2001:db8:77::2", plugin="v2ray-plugin", plugin_opts="", plugin_mode="tcp_only")
            path = pathlib.Path(directory) / "profile.json"
            path.write_text(json.dumps(profile))
            cli = str(pathlib.Path(sys.argv[1]).resolve())
            backend_path = pathlib.Path(directory) / "backend.json"
            backend_path.write_bytes(subprocess.check_output([str(pathlib.Path(sys.argv[2]).resolve()), str(path)]))
            backend = subprocess.Popen(["sslocal", "-c", str(backend_path)])
            deadline = time.monotonic() + 5
            while "nmss-tun" not in [name for _, name in socket.if_nameindex()]:
                if time.monotonic() > deadline:
                    raise TimeoutError("TUN was not created")
                time.sleep(.05)
            ip("link", "set", "nmss-tun", "mtu", "1400", "up")
            pathlib.Path("/proc/sys/net/ipv4/conf/nmss-tun/rp_filter").write_text("2\n")
            plan = subprocess.check_output([cli, "plan", str(path)], text=True)
            for line in plan.splitlines():
                subprocess.run(shlex.split(line), check=True)
            if os.environ.get("SS_TEST_RPFILTER"):
                subprocess.run(["/usr/sbin/nft", "-f", "-"], input="""table inet nmss_test {
 chain prerouting {
  type filter hook prerouting priority 10; policy accept;
  meta nfproto ipv6 fib saddr . mark . iif oif missing counter drop
 }
}
""", text=True, check=True)
            for kind in (socket.SOCK_STREAM, socket.SOCK_DGRAM):
                for port in (53, 15353):
                    with socket.socket(socket.AF_INET, kind) as client:
                        client.settimeout(5)
                        client.connect(("198.19.0.10", port))
                        client.sendall(b"tun-forwarding-test")
                        assert client.recv(1024) == b"tun-forwarding-test"
                    print("PASS TUN", "TCP" if kind == socket.SOCK_STREAM else "UDP", port)
        finally:
            if os.environ.get("SS_TEST_RPFILTER"):
                subprocess.run(["/usr/sbin/nft", "list", "table", "inet", "nmss_test"], check=False)
            if backend:
                backend.terminate()
                backend.wait(timeout=5)
            if peer.poll() is None:
                peer.stdin.write("stop\n")
                peer.stdin.flush()
            peer.wait(timeout=20)
    return 0


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--server":
        server(sys.argv[2])
    else:
        sys.exit(main())
