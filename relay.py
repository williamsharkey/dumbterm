#!/usr/bin/env python3
"""TCP relay: Mac localhost:<local_port> → W7:<w7_port>.

Two transports, in priority order:
  1. Direct TCP to W7_HOST (e.g. 192.168.1.8). Low-latency, no buffering.
  2. SSH -W fallback via `ssh w7`. Works over WAN but buffers aggressively —
     multi-request flows can stall. Avoid when direct TCP is available.

Env:
  W7_HOST    — LAN IP for direct TCP. If set and reachable, used first.
               Falls back to ssh -W when unset or unreachable.
  W7_SSH     — ssh host alias (default "w7"). Used for the -W fallback.

Usage:
  python3 relay.py [local_port] [w7_port]

Default ports: 9187 / 9187. (dumbterm's agent + driver both expect 9187.)
"""
import sys, socket, subprocess, threading, signal, os, time

local_port = int(sys.argv[1]) if len(sys.argv) > 1 else 9187
w7_port = int(sys.argv[2]) if len(sys.argv) > 2 else 9187

W7_HOST = os.environ.get('W7_HOST', '192.168.1.8')
W7_SSH  = os.environ.get('W7_SSH', 'w7')

def try_direct():
    """Quick probe: can we open a TCP connection to W7 on the LAN?"""
    try:
        s = socket.create_connection((W7_HOST, w7_port), timeout=2)
        s.close(); return True
    except Exception:
        return False

DIRECT = try_direct()
print(f"relay: localhost:{local_port} → "
      f"{'direct TCP ' + W7_HOST if DIRECT else 'ssh -W via ' + W7_SSH}:{w7_port}",
      flush=True)

srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(('127.0.0.1', local_port))
srv.listen(8)

def pipe_pairs(a_read, a_write, b_read, b_write):
    """Two threads copying bytes both ways, with close-propagation."""
    def copy(src, dst, close_dst):
        try:
            while True:
                d = src(4096)
                if not d: break
                dst(d)
        except Exception:
            pass
        try: close_dst()
        except Exception: pass
    t1 = threading.Thread(target=copy, args=(a_read, b_write, b_write), daemon=True)
    t2 = threading.Thread(target=copy, args=(b_read, a_write, a_write), daemon=True)
    t1.start(); t2.start()
    return t1, t2

def handle_direct(csock):
    try:
        up = socket.create_connection((W7_HOST, w7_port))
        up.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        csock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    except Exception as e:
        print(f"relay: direct connect failed: {e}", flush=True)
        try: csock.close()
        except: pass
        return
    print("relay: direct client connected", flush=True)
    def copy(src, dst):
        try:
            while True:
                d = src.recv(4096)
                if not d: break
                dst.sendall(d)
        except Exception: pass
        try: dst.shutdown(socket.SHUT_WR)
        except Exception: pass
    threading.Thread(target=copy, args=(csock, up), daemon=True).start()
    threading.Thread(target=copy, args=(up, csock), daemon=True).start()

def handle_ssh(csock):
    print("relay: ssh -W client connected", flush=True)
    proc = subprocess.Popen(
        ['ssh', '-W', f'127.0.0.1:{w7_port}', W7_SSH],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
        bufsize=0,
    )
    def c2s():
        try:
            while True:
                d = csock.recv(4096)
                if not d: break
                proc.stdin.write(d); proc.stdin.flush()
        except Exception: pass
        try: proc.stdin.close()
        except Exception: pass
    def s2c():
        try:
            while True:
                d = proc.stdout.read1(4096)
                if not d: break
                csock.sendall(d)
        except Exception: pass
        try: csock.close()
        except Exception: pass
    threading.Thread(target=c2s, daemon=True).start()
    threading.Thread(target=s2c, daemon=True).start()
    proc.wait()
    print("relay: ssh -W disconnected", flush=True)

signal.signal(signal.SIGINT, lambda *a: os._exit(0))
while True:
    c, _ = srv.accept()
    h = handle_direct if DIRECT else handle_ssh
    threading.Thread(target=h, args=(c,), daemon=True).start()
