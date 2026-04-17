# dumbterm

A single-file, zero-dependency, GPU-accelerated terminal emulator built to run Claude Code on Windows 7 32-bit — where nothing else works.

## Why this exists

Claude Code needs a terminal that reports `isTTY=true` and supports true color (24-bit RGB). On modern systems this is trivial. On Windows 7 32-bit, it's a nightmare:

- **winpty** (what MSYS2/mintty uses): Polls the Win32 console buffer at ~
50ms intervals, producing garbled output when Claude Code redraws rapidly. The unix-adapter mangles ANSI sequences. Layout corruption is constant.
- **ConPTY**: Doesn't exist on Windows 7. Microsoft added it in Windows 10 1809.
- **Windows Terminal**: Windows 10+ only.
- **cmd.exe**: No ANSI escape support whatsoever.
- **mintty direct**: Fakes a POSIX PTY through MSYS2, but Claude Code's `supports-color` library detects Windows and forces color level 1 (basic 16 colors) regardless of `FORCE_COLOR=3`, because it hardcodes `if (Number(os.release().split('.')[0]) >= 10)` and Windows 7 returns `6.1`.
- **Node.js isTTY over pipes**: Returns false. Claude Code refuses to render its TUI without `isTTY=true`. Real isTTY on Windows requires a console handle, which means console buffer polling, which means corruption.
- **Go 1.21+**: Runtime crashes on Windows 7 (calls `CreateWaitableTimerExW` which doesn't exist). Must use Go 1.20.14.

After trying every combination — winpty, mintty, DLL injection (IAT hooking `GetStdHandle`), console API interception, named pipes, you name it — I said "fuck it, I'll write my own terminal."

## What it does

dumbterm is a single C file (~2100 lines) that:

1. **Spawns the child process** (Claude Code, bash, cmd.exe, whatever) using platform-native APIs — `forkpty()` on macOS, anonymous pipes + a JavaScript `--require` shim on Windows
2. **Parses VT100/xterm escape sequences** through a state machine (GROUND/ESC/CSI/OSC/STR) — handles cursor movement, SGR colors (16, 256, and 24-bit true color), erase, scroll
3. **Renders with OpenGL** using a pre-extracted GNU Unifont bitmap atlas (8x16 pixels, 609 glyphs) — no FreeType, no system fonts, no dependencies
4. **On Windows 7**: A `--require` JavaScript shim patches `process.stdout.isTTY = true`, fakes `os.release()` to return `'10.0.19041'` (fooling `supports-color` into level 3), and sets up `getColorDepth`, `hasColors`, `setRawMode` — all without touching the Win32 console
5. **Streams over TCP** for remote access — run the server on W7, connect from anywhere. Raw VT bytes over the wire, ~1KB/frame

## The Windows 7 trick

The key insight: don't fight the Win32 console at all. Use anonymous pipes (not a console) for clean byte streaming, and fix the JavaScript-level checks with a `--require` shim:

```javascript
// Injected via node --require _shim.js
Object.defineProperty(process.stdout, 'isTTY', {value: true});
Object.defineProperty(process.stderr, 'isTTY', {value: true});
process.env.FORCE_COLOR = '3';
// Fool supports-color's win32 version check
var _os = require('os');
_os.release = function() { return '10.0.19041'; };
// Inherit tty.WriteStream methods (getColorDepth, hasColors)
process.stdout.__proto__ = require('tty').WriteStream.prototype;
```

This gives you full 24-bit true color Claude Code output on a 15-year-old 32-bit operating system, with zero rendering corruption.

## Building

### macOS (native Cocoa window)

```bash
cc -O2 -x objective-c -o dumbterm dumbterm.c \
  -framework OpenGL -framework Cocoa -framework AudioToolbox -lutil
```

### Windows 7 32-bit (MinGW)

```bash
gcc -O2 -o dumbterm.exe dumbterm.c \
  -lopengl32 -lgdi32 -luser32 -lkernel32 -lws2_32
```

Requires MinGW GCC. Tested with GCC 8.2.0 on Windows 7 SP1 32-bit.

### Dependencies

None. The font atlas (`unifont_data.h`) is pre-generated and checked in. OpenGL and platform windowing APIs are the only external requirements — both ship with the OS.

## Usage

```bash
# Local shell
./dumbterm

# Local Claude Code
./dumbterm -- claude

# Headless server on Windows 7 (remote clients connect over TCP)
dumbterm.exe --listen 9124 -- C:\node18\node.exe C:\node16\node_modules\@anthropic-ai\claude-code\cli.js

# Visible server (local GL window + remote clients)
dumbterm.exe --listen 9124 --visible -- claude

# Remote client (connect from Mac/Linux to a W7 server)
./dumbterm --connect w7-host:9124
```

### Remote access through SSH

If your Windows 7 box is behind NAT or a firewall (it probably is), use SSH tunneling. The `-L` port forward won't work if W7's sshd has `AllowTcpForwarding` disabled, but `ssh -W` (netcat mode) always works:

```bash
# On the Mac, use the included relay:
python3 relay.py 9183 9124
# Then connect:
./dumbterm --connect 127.0.0.1:9183
```

The relay bridges a local TCP port through `ssh -W` to the W7 server. Requires SSH access to the Windows box.

## Features

- **Full ANSI/VT100**: CSI sequences, SGR (bold, inverse, 16/256/true color), cursor positioning, erase, scroll
- **Bitmap font rendering**: GNU Unifont at 8x16 pixels, binary search glyph lookup, integer-pixel scaling
- **Smooth scrolling**: Momentum physics with exponential decay + cubic ease-in-out line snapping
- **Cell hover glow**: Per-cell brightness decay with crosshair borders
- **Text selection**: Two-layer (populated content vs empty space) with amber perimeter borders. Double-click word select. Smart edit (arrow keys + backspace to child process)
- **Pulsed amber cursor**: Detects Claude Code's reverse-video cursor cell, pulses amber once per second with exponential decay
- **FM-synthesized audio**: Morse code sonification of selected text, hover clicks, typing/output sounds. 10 sound presets, multiple volume levels, adjustable morse speed
- **Morse visualization**: Amber scan line sweeps selected characters in sync with audio playback, with brightness pulses on tones and a 1.1-second decay trail
- **Scrollback history**: 10,000 lines with per-row history buffer
- **Multi-client TCP server**: Up to 10 simultaneous remote viewers with grid state sync on connect
- **macOS native**: Cocoa NSWindow + NSOpenGLView, native traffic lights, Sounds menu with check marks

## File structure

```
dumbterm.c       — The terminal (everything in one file)
flowto.c         — Remote execution agent + driver (included from dumbterm.c)
flowto_shim.js   — Node --require shim for child_process/fs interception
unifont_data.h   — Pre-generated GNU Unifont glyph data (609 glyphs)
hook.c           — W7 DLL injection experiment (abandoned, shim approach won)
relay.py         — SSH relay for remote connections through firewalls
web/             — WebGL browser terminal variant + Go WebSocket bridge
tools/           — Font extraction, deployment scripts, config template
  deploy-w7.sh   — Deploy/refresh the flowto agent on W7 (one command)
  flowto.example.json — Template .flowto.json to drop in your project
tests/           — Automated end-to-end tests (57/57 on real W7)
  flowto_tester.js — Visual+headless tester (TUI + TAP output + JSON trace)
  run_full.sh    — Canonical invocation for the full suite
```

## flowto: run apps locally, route tool calls remotely

`flowto` is a transparent Node.js proxy that makes it feel like a program (like Claude Code) is running on machine A while its `child_process.exec/spawn` and `fs.*` calls actually execute on machine B.

The driving use case: run Claude Code on a fast Mac (for the LLM compute and large context window), but have all Bash / file ops land on a Windows 7 box with legacy tooling (Lotus R5, MinGW, W7-only binaries).

**Two commands:**

```bash
# On W7 (the tool target):
dumbterm --agent 9187 --on-start 'SUBST M: C:\mdrive'

# On Mac (where Claude Code runs):
dumbterm --flowto HOST:9187 -- claude
```

The Mac spawns Claude Code locally. A JS shim (`flowto_shim.js`) injected via `--require` intercepts `child_process.exec/spawn` and `fs.*` — these calls marshal to the W7 agent over TCP (JSON-line protocol, base64 for binary payloads).

**Key capabilities:**

- **Custom shell**: configure `remoteShell` in `.flowto.json` (e.g., MSYS bash for POSIX commands on W7)
- **Path translation**: `pathMap: [["/Users/.../lot", "/c/workspace/lot"]]` — Claude's Mac paths auto-translate to W7 paths
- **Host switching at runtime**: `dumbterm-host local` / `dumbterm-host remote` / `dumbterm-at w7 <cmd>` — Claude can flip between hosts mid-session
- **Persistent `/cloud/`**: paths under `/cloud/dumbterm/` always persist on one designated host
- **`--on-start` hooks**: agent runs shell commands at startup (e.g., `SUBST` drive mappings that don't survive W7 reboots)
- **Auto-injected context**: `/tmp/flowto-context.md` describes the current session for Claude Code to pick up (with clear BEGIN/END markers so user edits survive)

See [`tools/flowto.example.json`](tools/flowto.example.json) for a template.

## Setting up flowto on Windows 7

One-time setup:

```bash
# 1. Clone the repo on W7 (via git-bash or from SSH):
ssh w7 "cd /c/workspace && git clone https://github.com/williamsharkey/dumbterm.git"

# 2. Build the agent:
ssh w7 '"C:\Program Files\Git\bin\bash.exe" -c "cd /c/workspace/dumbterm && C:/MinGW/bin/gcc.exe -O2 -o dumbterm_agent.exe dumbterm.c -lopengl32 -lgdi32 -luser32 -lkernel32 -lws2_32 -lm"'

# 3. Create a launcher .bat on W7 (edit on W7 directly, e.g. via Notepad):
#    C:\workspace\dumbterm\run-agent.bat :
#      @echo off
#      C:\workspace\dumbterm\dumbterm_agent.exe --agent 9187 --on-start "SUBST M: C:\mdrive"
#
#    (--on-start remaps M: on every agent start — W7 loses SUBST across reboots.)

# 4. Register a scheduled task (runs the .bat in interactive session 1):
ssh w7 "schtasks /Create /TN dumbterm-agent /TR C:\workspace\dumbterm\run-agent.bat /SC ONCE /ST 00:00 /F"

# 5. Start it:
ssh w7 "schtasks /Run /TN dumbterm-agent"
```

Ongoing deploys — one command from Mac:

```bash
./tools/deploy-w7.sh   # stop → pull → rebuild → relaunch
```

**SSH `-L` does not work** to W7 (sshd has `AllowTcpForwarding no` in a Match block somewhere). Use the provided relay:

```bash
# On Mac:
python3 relay.py 9187 9187    # forwards via ssh -W instead of -L
```

Drop a `.flowto.json` in your project root. Copy from `tools/flowto.example.json` and edit the `pathMap` prefix to your username.

Run your app:

```bash
cd ~/Desktop/lot
./dumbterm --flowto 127.0.0.1:9187 -- node "$(npm root -g)/@anthropic-ai/claude-code/cli.js" --dangerously-skip-permissions
```

Now Claude's `Bash` runs in W7's MSYS bash in `/c/workspace/lot`; `Read` of `~/Desktop/lot/main.c` auto-resolves to W7's `/c/workspace/lot/main.c`; its memory and auth stay on Mac.

### ⚠ Important: must be Node-based Claude Code

The shipped macOS `claude` binary is compiled with Bun and **cannot be hooked** by `--require`. If you run `dumbterm --flowto ... -- claude`, flowto prints a warning and runs Claude with *no tool routing* — you'll see Mac's `hostname`, not the agent's.

Install the Node-based version instead:

```bash
npm install -g @anthropic-ai/claude-code
dumbterm --flowto HOST:PORT -- node "$(npm root -g)/@anthropic-ai/claude-code/cli.js" ...
```

Both share `~/.claude/` credentials, so auth is automatic once either is logged in.

## For other Windows 7 / legacy Windows users

If you're trying to run Claude Code on Windows 7 or another legacy Windows version, here's what you need to know:

1. **Use Node 18** to run Claude Code (Node 16 works too but 18 is better supported)
2. **The JS shim approach works** — you don't need a real PTY. Pipes + `--require` shim gives you full true color
3. **Build dumbterm with MinGW GCC** — it's a single `gcc` command, no build system needed
4. **Run in server mode** (`--listen`) if you want to view from a modern machine — the W7 GL window works but the remote Mac/Linux client has smoother scrolling and more features
5. **The `os.release()` patch is critical** — without it, `supports-color` caps you at 16 colors regardless of env vars

## License

MIT

## Credits

- [GNU Unifont](https://unifoundry.com/unifont/) for the bitmap font data
- Claude Code by Anthropic — the reason this terminal exists
