# dumbterm / flowto orientation — for a Claude worker running on W7

You're running on W7 directly. Most of what previous workers did to "drive W7
from afar" does not apply to you. This note exists so you don't waste time
replicating infrastructure you don't need.

**Source:** https://github.com/williamsharkey/dumbterm — skim `README.md` if
you want the full story.

---

## 30-second version

- **`dumbterm`** — a single-file C terminal emulator (Mac Cocoa + W7 Win32 +
  OpenGL). Originally built because `winpty` + `mintty` mangle Claude Code's
  TUI on W7. Not relevant to you unless you're launching Claude Code visually
  on the W7 desktop.
- **`flowto`** — a Node.js `--require` shim + TCP RPC + W7 agent that lets a
  Claude Code process on Mac run its Bash / Write / Read / Edit tools against
  W7 files and commands. **You are the W7 side.** Previous workers were the
  Mac side calling in. You don't need flowto to reach W7 — you ARE W7.

---

## What previous (Mac-side) workers had to set up — and what you can skip

| Machinery | Purpose | Relevant to you? |
| --- | --- | --- |
| `lot-claude.sh` | Mac launcher that wires flowto shim into Claude Code | ❌ Mac-only |
| `flowto_shim.js` | Intercepts `fs.*` / `child_process.*` on Mac, RPCs to W7 | ❌ Mac-only |
| `relay.py` | TCP-over-SSH relay, Mac → cmdfn.com → W7 | ❌ You're already on W7 |
| `pathMap` translation (`/Users/.../lot` ↔ `/c/workspace/lot`) | Maps Mac paths to W7 paths | ❌ You already live under `/c/workspace/lot` |
| `FLOWTO_HOST=local` prefix | Forces a Bash call to run on Mac | ❌ Inverted for you; you'd need `FLOWTO_HOST=<mac>` which isn't set up |
| `dumbterm-status` / `dumbterm-clipboard` meta-commands | Mac-side Claude Code meta-commands | ❌ Only fire inside the Mac-side shim |
| `ssh w7` / ProxyJump via cmdfn.com | Reaching W7 from Mac | ❌ You're on the destination |

**Net:** you can just use `bash`, `make`, `gcc`, `git`, `vc.exe`, `vf.exe`
directly on your own filesystem. No shim. No relay. No path translation.

---

## What IS running on W7 that you should NOT confuse

On W7 there are two independent daemons with similar-sounding names:

- **`dumbterm.exe --agent 9187`** (scheduled task `dumbterm-agent`) — the
  flowto RPC backend. Serves `read`, `write`, `stat`, `spawn`, `exec`,
  `clipboard`, `rename`, etc., to a Mac-side shim. **Only relevant if
  something is running `flowto` against your box and behaving oddly.**
  Leave it alone otherwise.
- **`lotus-agent`** on `127.0.0.1:9123` — the Lotus automation HTTP API
  (drives `123R5.EXE` via AHK for oracle capture, macro playback, screen
  reads). **This is the one you actually care about** if your mission
  involves oracle capture or R5 interaction. It lives in `C:\lotus-server\`,
  NOT in the dumbterm repo.

If your mission says "capture oracle via `/B`" — that's `lotus-agent`,
not dumbterm. Don't touch `/c/lotus-server/` unless your handoff explicitly
tells you to.

---

## What from this repo might still be useful to you

- **`dumbterm.exe` as a shell terminal** — if you want a clean OpenGL
  terminal on the W7 desktop (no winpty mangling) instead of mintty.
  Optional; purely cosmetic.
- **Understanding the agent's RPC protocol** — if you ever need to debug
  why a Mac-side Claude session's Bash call failed, the agent's source is in
  `flowto.c` (`agent_handle_request` function). Check the scheduled task's
  stderr log.
- **`relay.py` / `flowto_shim.js`** — only if you want to spin up a flowto
  agent yourself to let a colleague's Claude Code session work on W7 files.
  Unlikely in your mission.

---

## If something "feels like dumbterm is involved" but shouldn't be

Symptoms:
- A file you wrote locally on W7 shows up in a weird path.
- `ssh w7` from somewhere else starts hanging.
- The `dumbterm-agent` scheduled task is consuming CPU when nobody's
  running Claude on Mac.

Response:
- Check what's connected to port 9187: `netstat -ano | findstr 9187`
- Restart the agent only if you understand the impact:
  `schtasks /End /TN dumbterm-agent && schtasks /Run /TN dumbterm-agent`
- Don't kill it without telling the operator — there may be a live Mac
  session using it.

---

## Quick mental model

```
  [Mac: Claude Code] → flowto_shim.js (intercept) → TCP 9187 → relay.py
       ↓                                                          ↓ ssh -W / direct
       writes to local fs                             [W7: dumbterm.exe --agent]
                                                                ↓
                                                      does the actual work
                                                      (read/write/exec/…)

  [W7: YOU (Claude worker)] → bash / git / make / vc.exe
                              ↓ directly, no shim
                              W7 filesystem + W7 toolchain
```

You're the bottom half. You don't need the top half.

---

## Repo URL, one more time

https://github.com/williamsharkey/dumbterm — if anything above is unclear,
read `README.md` in that repo. The Mac-specific launcher scripts and shim
source live there; your work doesn't depend on them.
