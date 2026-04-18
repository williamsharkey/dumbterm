#!/bin/bash
# Run the full flowto tester suite against the currently-running agent.
# Sets all DUMBTERM_VIRTUAL_* flags so Phase 3-6 features activate.
#
# Requires:
#   - dumbterm agent listening (local or via relay) on the given address
#   - built dumbterm binary in the repo root
# Usage:
#   ./tests/run_full.sh [flowto_addr] [trace_file]
# Default: flowto_addr=127.0.0.1:9187, trace=/tmp/flowto_trace.jsonl

set -e
cd "$(dirname "$0")/.."
ADDR="${1:-127.0.0.1:9187}"
TRACE="${2:-/tmp/flowto_trace.jsonl}"

DUMBTERM_VIRTUAL_HOME=1 \
DUMBTERM_VIRTUAL_CWD=1 \
DUMBTERM_VIRTUAL_PLATFORM=1 \
DUMBTERM_VIRTUAL_PATH=1 \
DUMBTERM_VIRTUAL_ENV=1 \
DUMBTERM_VIRTUAL_FS=1 \
DUMBTERM_VIRTUAL_FS_AGGRESSIVE=1 \
DUMBTERM_TRACE_LOG="$TRACE" \
./dumbterm --flowto "$ADDR" -- node tests/flowto_tester.js --headless
