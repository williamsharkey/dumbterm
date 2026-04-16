#!/bin/bash
# Test: visible-server mode grid state sync
# Launches dumbterm --listen --visible with a test child that prints known text,
# waits for rendering, then connects a raw TCP client to verify grid state is sent.

set -e
cd "$(dirname "$0")"

DUMBTERM="./dumbterm"
PORT=9178
TESTPROG="./test_child_visible.sh"

cat > "$TESTPROG" << 'CHILDEOF'
#!/bin/bash
echo "VISIBLE_SERVER_TEST_AAA"
echo "VISIBLE_SERVER_TEST_BBB"
echo "TRUE_COLOR_CHECK"
sleep 30
CHILDEOF
chmod +x "$TESTPROG"

echo "=== Building dumbterm ==="
cc -O2 -x objective-c -o dumbterm dumbterm.c -framework OpenGL -framework Cocoa -framework AudioToolbox -lutil 2>&1

pkill -f "dumbterm.*--listen.*$PORT" 2>/dev/null || true
sleep 0.5

echo "=== Launching visible server on port $PORT ==="
"$DUMBTERM" --listen "$PORT" --visible -- "$TESTPROG" &
SERVER_PID=$!
echo "Server PID: $SERVER_PID"

# Wait for server to render content
sleep 3

# Verify listening
if ! lsof -i ":$PORT" | grep -q LISTEN; then
    echo "FAIL: Not listening on $PORT"
    kill $SERVER_PID 2>/dev/null
    rm -f "$TESTPROG"
    exit 1
fi
echo "Server is listening"

# Connect and capture grid state
echo "=== Connecting TCP client ==="
RECV="/tmp/dumbterm_vis_test_$$"
(sleep 3 && echo "") | nc -w 4 127.0.0.1 "$PORT" > "$RECV" 2>/dev/null &
NC_PID=$!
sleep 4

kill $NC_PID 2>/dev/null || true
kill $SERVER_PID 2>/dev/null || true
wait $SERVER_PID 2>/dev/null || true

echo "=== Received data ==="
BYTES=$(wc -c < "$RECV")
echo "Received $BYTES bytes"

FOUND=0
# The grid state should contain our test strings (embedded in VT escape sequences)
if grep -q "VISIBLE_SERVER_TEST_AAA" "$RECV" 2>/dev/null; then
    echo "PASS: Found AAA in grid state"
    FOUND=$((FOUND+1))
else
    echo "FAIL: AAA not found"
fi
if grep -q "VISIBLE_SERVER_TEST_BBB" "$RECV" 2>/dev/null; then
    echo "PASS: Found BBB in grid state"
    FOUND=$((FOUND+1))
else
    echo "FAIL: BBB not found"
fi
if grep -q "TRUE_COLOR_CHECK" "$RECV" 2>/dev/null; then
    echo "PASS: Found TRUE_COLOR_CHECK"
    FOUND=$((FOUND+1))
else
    echo "FAIL: TRUE_COLOR_CHECK not found"
fi

# Check for VT escape sequences from grid state serialization
if grep -qP '\x1b\[2J' "$RECV" 2>/dev/null; then
    echo "PASS: Received clear-screen (grid state init)"
    FOUND=$((FOUND+1))
else
    echo "INFO: No clear-screen found (might be raw forwarding)"
fi

echo ""
echo "=== Hex dump (first 40 lines) ==="
xxd "$RECV" | head -40

echo ""
if [ "$FOUND" -ge 3 ]; then
    echo "=== VISIBLE-SERVER TEST PASSED ($FOUND checks passed) ==="
else
    echo "=== VISIBLE-SERVER TEST FAILED (only $FOUND checks passed) ==="
fi

rm -f "$RECV" "$TESTPROG"
