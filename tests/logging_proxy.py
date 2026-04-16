#!/usr/bin/env python3
"""Logging TCP proxy: listens on local port, forwards to another port,
logs all bytes in both directions. Used to sniff what the Mac dumbterm
client actually sends when resizing.

Usage: python3 logging_proxy.py <listen_port> <forward_port> <log_file>
"""
import sys, socket, threading, os, time

listen_port = int(sys.argv[1]) if len(sys.argv) > 1 else 9184
forward_port = int(sys.argv[2]) if len(sys.argv) > 2 else 9183
log_file = sys.argv[3] if len(sys.argv) > 3 else "/tmp/proxy_log.txt"

log_lock = threading.Lock()
def logit(tag, data):
    with log_lock:
        with open(log_file, "ab") as f:
            t = time.time()
            # Special-case: detect _RESIZE
            if b"\x1b_RESIZE;" in data:
                idx = data.index(b"\x1b_RESIZE;")
                end = data.index(b"\x1b\\", idx)
                seq = data[idx:end+2]
                f.write(f"[{t:.3f}] {tag} FOUND_RESIZE: {seq!r}\n".encode())
            # Otherwise summarize
            hex_preview = data[:64].hex()
            f.write(f"[{t:.3f}] {tag} len={len(data)} head={hex_preview}\n".encode())

print(f"proxy: listen :{listen_port} → :{forward_port}, logging to {log_file}")
open(log_file, "w").close()  # clear

srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", listen_port))
srv.listen(5)

def handle(client):
    print("proxy: client connected")
    upstream = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try: upstream.connect(("127.0.0.1", forward_port))
    except Exception as e:
        print(f"proxy: upstream connect failed: {e}")
        client.close(); return

    def c2s():
        try:
            while True:
                d = client.recv(4096)
                if not d: break
                logit("CLIENT→SERVER", d)
                upstream.sendall(d)
        except: pass
        try: upstream.close()
        except: pass

    def s2c():
        try:
            while True:
                d = upstream.recv(4096)
                if not d: break
                logit("SERVER→CLIENT", d)
                client.sendall(d)
        except: pass
        try: client.close()
        except: pass

    threading.Thread(target=c2s, daemon=True).start()
    threading.Thread(target=s2c, daemon=True).start()

while True:
    c, _ = srv.accept()
    threading.Thread(target=handle, args=(c,), daemon=True).start()
