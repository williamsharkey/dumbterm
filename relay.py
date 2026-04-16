#!/usr/bin/env python3
"""TCP relay: local port → ssh -W → W7 dumbterm server.
Usage: python3 relay.py [local_port] [w7_port]
"""
import sys, socket, subprocess, threading, signal, os

local_port = int(sys.argv[1]) if len(sys.argv) > 1 else 9183
w7_port = int(sys.argv[2]) if len(sys.argv) > 2 else 9182

print(f"relay: localhost:{local_port} → w7:127.0.0.1:{w7_port}")

srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(('127.0.0.1', local_port))
srv.listen(5)

def handle(csock):
    print("relay: client connected")
    proc = subprocess.Popen(
        ['ssh', '-W', f'127.0.0.1:{w7_port}', 'w7'],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL
    )
    def c2s():
        try:
            while True:
                d = csock.recv(4096)
                if not d: break
                proc.stdin.write(d)
                proc.stdin.flush()
        except: pass
        try: proc.stdin.close()
        except: pass
    def s2c():
        try:
            while True:
                d = proc.stdout.read1(4096)
                if not d: break
                csock.sendall(d)
        except: pass
        try: csock.close()
        except: pass
    threading.Thread(target=c2s, daemon=True).start()
    threading.Thread(target=s2c, daemon=True).start()
    proc.wait()
    print("relay: disconnected")

signal.signal(signal.SIGINT, lambda *a: os._exit(0))
while True:
    c, _ = srv.accept()
    threading.Thread(target=handle, args=(c,), daemon=True).start()
