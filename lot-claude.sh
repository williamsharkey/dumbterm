#!/bin/bash
# One-liner to launch Claude Code in Desktop/lot with flowto → W7.
# Brain on Mac (Claude's context + latency), hands on W7 (Lotus R5, vc.exe, …).
#
# Prerequisites:
#   - relay.py running:  python3 ~/Desktop/dumbterm/relay.py 9187 9187 &
#   - W7 agent running:  dumbterm.exe --agent 9187  (via schtasks dumbterm-agent)
#   - Claude Code npm:   npm install -g @anthropic-ai/claude-code
#
# Usage:
#   ./lot-claude.sh                       # interactive session
#   ./lot-claude.sh -p "run hostname"     # one-shot --print query
#
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
LOT="$HOME/Desktop/lot"
RELAY_PORT="${RELAY_PORT:-9187}"
CLAUDE_CLI="$(npm root -g)/@anthropic-ai/claude-code/cli.js"

# Auto-start relay if not already listening
if ! nc -z 127.0.0.1 "$RELAY_PORT" 2>/dev/null; then
    echo "starting relay on $RELAY_PORT → w7..." >&2
    python3 "$DIR/relay.py" "$RELAY_PORT" "$RELAY_PORT" >/dev/null 2>&1 &
    sleep 1
fi

cd "$LOT"
exec "$DIR/dumbterm" --flowto "127.0.0.1:$RELAY_PORT" -- \
    node "$CLAUDE_CLI" --dangerously-skip-permissions "$@"
