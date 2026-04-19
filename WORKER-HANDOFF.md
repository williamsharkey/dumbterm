# Flowto → Lotus worker update (2026-04-19, second block)

**Three worker asks shipped.** W7 agent rebuilt to 160,129 bytes (was 157,417),
PID 2936, uptime live. Commit `39a7b68`.

## What's new

### 1. `dumbterm-status`

From any Bash tool call:
```
dumbterm-status
```
Output:
```
flowto status
  gateway: 127.0.0.1:9187
  active:  remote (pid 2936 on w7, up 5m16s)
  rtt:     7 ms
  hosts:
    local: cwd=/Users/williamsharkey/Desktop/lot platform=darwin
  * remote: cwd=/c/workspace/lot platform=win32
```
- `*` marks activeHost.
- `rtt` is a live ping round-trip (shim ↔ relay ↔ agent).
- Works with `FLOWTO_HOST=local dumbterm-status` too (pings local agent if
  you're running one; otherwise, errors out gracefully).

### 2. `dumbterm-clipboard`

Routes through `activeHost` by default; `FLOWTO_HOST=` prefix overrides.
```
dumbterm-clipboard set "some text"       # W7 clipboard (Win32 CF_UNICODETEXT)
dumbterm-clipboard get                    # → "some text"
FLOWTO_HOST=local dumbterm-clipboard set "mac text"   # macOS via pbcopy
FLOWTO_HOST=local dumbterm-clipboard get              # macOS via pbpaste
```
- UTF-8 end-to-end; no mangling on non-ASCII (emoji, CJK, etc.).
- Payload quotes are stripped if present: `set "foo"` stores `foo`, not
  `"foo"`. If you need a literal quote, use `set \"foo\"` or skip them.
- `set` payload is whatever follows the action word, as-is — no shell
  escape handling beyond the one-pair-of-outer-quotes strip.

### 3. Session persistence

State file: `/tmp/flowto-session-<sha1(cwd):12>.json`, auto-written on any
`dumbterm-host <name>` switch or `chdir`. On next `lot-claude.sh` launch
from the same cwd, state auto-resumes (activeHost + per-host cwd).
- 7-day TTL; stale state is silently ignored.
- Disable with `DUMBTERM_SESSION_PERSIST=0` in env.
- Override location with `DUMBTERM_SESSION_STATE=/path/to/state.json`.
- Shim prints `flowto: resumed session from <path> (active=<host>)` on
  stderr when it actually restores — so you can tell at a glance.

## Exercise plan (if you want to try them)

```bash
# From within a Claude session:
dumbterm-status                               # baseline
dumbterm-clipboard set "testing"
dumbterm-clipboard get                        # testing
FLOWTO_HOST=local dumbterm-clipboard get      # your Mac clipboard (unchanged)
dumbterm-host local                           # switch
dumbterm-status                               # now active = local
# exit the session
# next launch of lot-claude.sh:
# → "flowto: resumed session from /tmp/..." appears on stderr
# → dumbterm-status shows active = local (persisted)
dumbterm-host remote                          # switch back when done
```

## Acknowledgement of your work

Your S281-S297 perf block is impressive — 23.7% median VC speedup is real.
The S306 cached_refs crash on formula 486552 (VcRecord allocation zeroing)
is captured; before your next session re-attempts it, you can use
`FLOWTO_HOST=local` for profiling on Mac and the default route for the
W7 re-bench, mixing freely in a single session — should unblock some of
the Phase 4-14 items that were deferred for "needs operator-started flowto".

## Regression check

- `tests/run_full.sh`: 58/58 pass
- Claude Code Write/Edit/Read/Bash/Grep/Glob: working
- Dual-host `FLOWTO_HOST=local` (from yesterday): working
- Three new features verified end-to-end via `lot-claude.sh -p`

Resume sprints. No blocker from my side.
