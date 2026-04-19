#!/bin/bash
# One-liner to launch Claude Code in Desktop/lot with flowto → W7.
# Brain on Mac (Claude's context + latency), hands on W7 (Lotus R5, vc.exe, …).
#
# Prerequisites:
#   - W7 agent running: dumbterm.exe --agent 9187 (via schtasks dumbterm-agent).
#   - relay.py running on Mac:9187 (auto-started if not listening).
#   - Claude Code npm:  npm install -g @anthropic-ai/claude-code
#
# Usage:
#   ./lot-claude.sh                       # interactive session
#   ./lot-claude.sh -p "run hostname"     # one-shot --print query
#
# Architecture (simplified after 2026-04-19 work):
# - Claude Code's Node process runs directly with --require flowto_shim.js.
# - Shim + sync-helper subprocesses both connect to Mac:9187, which the
#   relay forwards to W7:9187 over LAN. No dumbterm "driver" layer in
#   between — that layer's multiplexing had edge cases; direct path is
#   simpler and battle-tested.
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
export DUMBTERM_GATEWAY="127.0.0.1:$RELAY_PORT"
export DUMBTERM_FLOWTO="127.0.0.1:$RELAY_PORT"
export DUMBTERM_VIRTUAL_CWD=0
# Silence Claude Code's noisy startup features that hang on network work
# without credentials (plugin marketplace auto-update tries to git-pull).
export CLAUDE_CODE_DISABLE_OFFICIAL_MARKETPLACE_AUTOINSTALL=1
exec node --require "$DIR/flowto_shim.js" "$CLAUDE_CLI" --dangerously-skip-permissions "$@"
