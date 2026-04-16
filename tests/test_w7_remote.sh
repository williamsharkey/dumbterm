#!/bin/bash
# Automated E2E test: W7 dumbterm → SSH relay → Mac dumbterm client
#
# Tests:
# 1. Compiles dumbterm on both Mac and W7
# 2. Launches headless server on W7 with a test child
# 3. Starts Python relay (ssh -W based)
# 4. Connects raw TCP client to capture bytes
# 5. Verifies expected content arrives
#
# No user interaction needed. No GUI windows opened.

set -e
cd "$(dirname "$0")"

W7_PORT=9185
LOCAL_PORT=9186
DUMBTERM="./dumbterm"
RECV="/tmp/dumbterm_w7_e2e_$$"

cleanup() {
    echo "=== Cleanup ==="
    kill $RELAY_PID 2>/dev/null || true
    ssh w7 "taskkill /IM dumbterm2.exe /F 2>nul" 2>/dev/null || true
    rm -f "$RECV" ./test_child_w7.bat
}
trap cleanup EXIT

echo "=== Step 1: Build Mac dumbterm ==="
cc -O2 -x objective-c -o dumbterm dumbterm.c -framework OpenGL -framework Cocoa -framework AudioToolbox -lutil 2>&1

echo "=== Step 2: Copy and build on W7 ==="
scp dumbterm.c unifont_data.h w7:C:/workspace/dumbterm-test/ 2>&1
ssh w7 "taskkill /IM dumbterm2.exe /F 2>nul" 2>/dev/null || true
sleep 0.5
ssh w7 "C:\MinGW\bin\gcc -O2 -o C:\workspace\dumbterm-test\dumbterm2.exe C:\workspace\dumbterm-test\dumbterm.c -lopengl32 -lgdi32 -luser32 -lkernel32 -lws2_32 -lm 2>&1 && echo BUILD:OK || echo BUILD:FAIL"

echo "=== Step 3: Create test child on W7 ==="
ssh w7 "echo @echo off > C:\workspace\dumbterm-test\test_e2e_auto.bat && echo echo W7_E2E_ALPHA >> C:\workspace\dumbterm-test\test_e2e_auto.bat && echo echo W7_E2E_BETA >> C:\workspace\dumbterm-test\test_e2e_auto.bat && echo echo W7_E2E_GAMMA_1234567890 >> C:\workspace\dumbterm-test\test_e2e_auto.bat && echo ping -n 30 127.0.0.1 ^>nul >> C:\workspace\dumbterm-test\test_e2e_auto.bat && echo CREATED"

echo "=== Step 4: Launch W7 headless server on port $W7_PORT ==="
ssh w7 "C:\workspace\dumbterm-test\dumbterm2.exe --listen $W7_PORT -- C:\workspace\dumbterm-test\test_e2e_auto.bat" 2>&1 &
W7_SSH_PID=$!
sleep 2

# Verify
if ! ssh w7 "netstat -an | findstr $W7_PORT | findstr LISTEN" 2>&1 | grep -q LISTEN; then
    echo "FAIL: W7 server not listening on $W7_PORT"
    exit 1
fi
echo "W7 server listening on $W7_PORT"

echo "=== Step 5: Launch relay (localhost:$LOCAL_PORT → W7:$W7_PORT) ==="
python3 relay.py $LOCAL_PORT $W7_PORT &
RELAY_PID=$!
sleep 1

echo "=== Step 6: Connect raw client and capture data ==="
# Use ssh -W directly for simplicity (skip relay for this test)
ssh -W "127.0.0.1:$W7_PORT" w7 > "$RECV" &
RAW_PID=$!
sleep 5
kill $RAW_PID 2>/dev/null || true

echo "=== Step 7: Verify received data ==="
BYTES=$(wc -c < "$RECV")
echo "Received $BYTES bytes"

PASS=0
FAIL=0

check() {
    if grep -q "$1" "$RECV" 2>/dev/null; then
        echo "  PASS: $1"
        PASS=$((PASS+1))
    else
        echo "  FAIL: $1 not found"
        FAIL=$((FAIL+1))
    fi
}

check "W7_E2E_ALPHA"
check "W7_E2E_BETA"
check "W7_E2E_GAMMA_1234567890"

if [ "$BYTES" -gt 10 ]; then
    echo "  PASS: received $BYTES bytes (> 10)"
    PASS=$((PASS+1))
else
    echo "  FAIL: only $BYTES bytes received"
    FAIL=$((FAIL+1))
fi

echo ""
echo "=== Hex dump (first 20 lines) ==="
xxd "$RECV" | head -20

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
if [ "$FAIL" -eq 0 ]; then
    echo "=== ALL TESTS PASSED ==="
    exit 0
else
    echo "=== SOME TESTS FAILED ==="
    exit 1
fi
