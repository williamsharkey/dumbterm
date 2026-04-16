#!/bin/bash
# Automated test: dumbterm visible-server mode + remote client
# Verifies that a remote client receives grid state from the server

set -e

DUMBTERM="$(dirname "$0")/dumbterm"
PORT=9177
TESTPROG="$(dirname "$0")/test_child.sh"

# Create a test child that prints known output
cat > "$TESTPROG" << 'CHILDEOF'
#!/bin/bash
echo "DUMBTERM_TEST_LINE_1"
echo "DUMBTERM_TEST_LINE_2"
echo "ABCDEFGHIJ_0123456789"
# Stay alive for a bit
sleep 5
CHILDEOF
chmod +x "$TESTPROG"

echo "=== Building dumbterm ==="
cd "$(dirname "$0")"
cc -O2 -x objective-c -o dumbterm dumbterm.c -framework OpenGL -framework Cocoa -framework AudioToolbox -lutil 2>&1

# Kill any existing dumbterm
pkill -f "dumbterm.*--listen.*$PORT" 2>/dev/null || true
sleep 0.5

echo "=== Launching server (headless listen on $PORT) ==="
# Use headless server mode (no GL window needed for automated test)
"$DUMBTERM" --listen "$PORT" -- "$TESTPROG" &
SERVER_PID=$!
echo "Server PID: $SERVER_PID"

# Wait for server to start listening
sleep 2

# Verify port is listening
if ! lsof -i ":$PORT" | grep -q LISTEN; then
    echo "FAIL: Server not listening on port $PORT"
    kill $SERVER_PID 2>/dev/null
    exit 1
fi
echo "Server is listening on $PORT"

# Connect as a raw TCP client and capture output
echo "=== Connecting raw TCP client ==="
RECV_FILE="/tmp/dumbterm_test_recv_$$"
# Use nc with a timeout to grab whatever the server sends
(sleep 3 && echo "") | nc -w 4 127.0.0.1 "$PORT" > "$RECV_FILE" 2>/dev/null &
NC_PID=$!

# Wait for data
sleep 4

# Kill nc and server
kill $NC_PID 2>/dev/null || true
kill $SERVER_PID 2>/dev/null || true
wait $SERVER_PID 2>/dev/null || true

# Check received data
echo "=== Checking received data ==="
BYTES=$(wc -c < "$RECV_FILE")
echo "Received $BYTES bytes"

if [ "$BYTES" -lt 10 ]; then
    echo "FAIL: Received too few bytes ($BYTES)"
    cat "$RECV_FILE" | xxd | head -20
    rm -f "$RECV_FILE" "$TESTPROG"
    exit 1
fi

# Look for our test strings in the received data
FOUND=0
if grep -q "DUMBTERM_TEST_LINE_1" "$RECV_FILE" 2>/dev/null; then
    echo "PASS: Found DUMBTERM_TEST_LINE_1"
    FOUND=$((FOUND+1))
fi
if grep -q "DUMBTERM_TEST_LINE_2" "$RECV_FILE" 2>/dev/null; then
    echo "PASS: Found DUMBTERM_TEST_LINE_2"
    FOUND=$((FOUND+1))
fi
if grep -q "ABCDEFGHIJ" "$RECV_FILE" 2>/dev/null; then
    echo "PASS: Found ABCDEFGHIJ"
    FOUND=$((FOUND+1))
fi

echo "=== Raw received data (first 500 bytes hex) ==="
xxd "$RECV_FILE" | head -30

echo ""
if [ "$FOUND" -ge 2 ]; then
    echo "=== TEST PASSED: Remote client received $FOUND/3 test strings ==="
else
    echo "=== TEST FAILED: Only found $FOUND/3 test strings ==="
fi

# Check for ANSI escape sequences (true color)
if grep -qP '\x1b\[38;2;' "$RECV_FILE" 2>/dev/null; then
    echo "PASS: Received true-color SGR sequences"
fi

rm -f "$RECV_FILE" "$TESTPROG"
