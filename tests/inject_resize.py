#!/usr/bin/env python3
"""Inject a _RESIZE sequence directly into the running relay.
Bypasses the Mac dumbterm — tests server-side pipeline only.
Usage: python3 inject_resize.py <cols> <rows> [relay_port]
"""
import sys, socket, time

cols = int(sys.argv[1]) if len(sys.argv) > 1 else 100
rows = int(sys.argv[2]) if len(sys.argv) > 2 else 30
port = int(sys.argv[3]) if len(sys.argv) > 3 else 9183

seq = f"\x1b_RESIZE;{cols};{rows}\x1b\\".encode()

print(f"Connecting to 127.0.0.1:{port}...")
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.connect(("127.0.0.1", port))
print(f"Sending _RESIZE;{cols};{rows}")
s.sendall(seq)
# Keep connection open briefly so server processes it
time.sleep(0.5)
s.close()
print("Done. Check W7 log for RESIZE event.")
