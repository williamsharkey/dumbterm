#!/bin/bash
# Deploy / refresh the flowto agent on W7.
#
# What this does:
#   1. Stops the running agent (taskkill dumbterm.exe / dumbterm_agent.exe).
#   2. Removes ALL pre-git-tracking files that existed on W7 before
#      flowto.c / flowto_shim.js became tracked (a one-time cleanup — safe
#      to repeat). Uses git bash since ssh-direct CMD can't rm -rf easily.
#   3. Pulls the latest main.
#   4. Rebuilds dumbterm_agent.exe with MinGW gcc.
#   5. Relaunches via the existing schtasks task name (default: dumbterm-agent).
#
# Assumes:
#   - W7 host reachable as `ssh w7` (uses your SSH config).
#   - repo cloned at C:\workspace\dumbterm (or /c/workspace/dumbterm).
#   - git-bash installed at "C:\Program Files\Git\bin\bash.exe".
#   - MinGW gcc at C:\MinGW\bin\gcc.exe.
#   - schtasks task named "dumbterm-agent" already exists (see README).
#
# Usage: ./tools/deploy-w7.sh [task_name]

set -e
TASK="${1:-dumbterm-agent}"
REMOTE_BASH='"C:\Program Files\Git\bin\bash.exe"'
REMOTE_GCC='C:/MinGW/bin/gcc.exe'
REMOTE_DIR='/c/workspace/dumbterm'

echo "== deploy-w7: stop existing agent =="
ssh w7 "taskkill /IM dumbterm_agent.exe /F 2>nul & taskkill /IM dumbterm.exe /F 2>nul & echo stopped" 2>&1 | grep -v "cannot find" || true

echo "== deploy-w7: clean pre-tracking files & pull =="
ssh w7 "$REMOTE_BASH -c \"cd $REMOTE_DIR && rm -f flowto.c flowto_shim.js 2>/dev/null; git reset --hard HEAD 2>/dev/null; git pull origin main\""

echo "== deploy-w7: rebuild dumbterm_agent.exe =="
ssh w7 "$REMOTE_BASH -c \"cd $REMOTE_DIR && $REMOTE_GCC -O2 -o dumbterm_agent.exe dumbterm.c -lopengl32 -lgdi32 -luser32 -lkernel32 -lws2_32 -lm 2>&1 | grep -v warning | head -5 || true\""

echo "== deploy-w7: relaunch via schtasks =="
ssh w7 "schtasks /Run /TN $TASK" 2>&1 | grep -v "^$"

sleep 1
echo "== deploy-w7: verify =="
ssh w7 "tasklist | findstr dumbterm" 2>&1 | head -5
ssh w7 "netstat -an | findstr 9187" 2>&1 | head -2
echo "== deploy-w7: done =="
