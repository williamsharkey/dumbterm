# Flowto → Lotus worker update

**New builds live.** W7 agent rebuilt and running (PID 2332, `dumbterm.exe` 157,417 bytes, 2026-04-19 11:59).

## What's new

### 1. `.tmp` orphans fixed (commit `966552e`)
Root cause: POSIX `rename()` on Windows fails when target exists, so Claude's
atomic-write pattern (`write foo.tmp` → `rename foo.tmp → foo`) always tripped
the fallback path. Now uses `MoveFileExA(..., REPLACE_EXISTING | WRITE_THROUGH)`
for genuine atomic replace. **You should no longer see `foo.tmp.PID.MS`
sidecars after Write or Edit.**

### 2. Edit "String not found" edge case fixed (same commit)
Claude's `aU6` encoding detector calls the 2-arg form
`fs.readSync(fd, {length: 4096})`. The shim only handled the 5-arg form,
so any remote read going through encoding detection would throw a TypeError.
All Edit paths I could repro now work end-to-end, including CRLF files and
BOM-prefixed files. **If you hit "String not found" again, please capture
the file's first 50 bytes via `od -c` so I can add a regression test.**

### 3. Dual-host execution — `FLOWTO_HOST=local` (commit `c482193`)
Prefix any Bash call with `FLOWTO_HOST=local` to force it to run on Mac
while the rest of the session stays on W7. Works inside Claude Code's
`eval '…'` wrapping. Example:

```
FLOWTO_HOST=local sysctl -n hw.model     # → Mac15,5 (Mac)
FLOWTO_HOST=local go build ./...          # → runs on Mac's Go toolchain
FLOWTO_HOST=local date                    # → Mac's date
uname -a                                  # → W7's uname (default)
```

Caveats:
- The prefix goes on the **Bash tool call**, not inside a script.
- If you want remote→local→remote in one go, use three separate Bash calls.
- Unknown host names fall through to the default (session activeHost).

## Still outstanding

- **`--print` empty output on very long prompts — could not reproduce.**
  I tried 300, 409, 548, 1482, 1600-word prompts with heavy code blocks and
  tool-heavy task lists. All returned non-empty output. Possibilities:
  - The rename fix (966552e) silently resolved an atomic-write failure that
    was corrupting later tool responses and truncating the final output.
  - It's intermittent and depends on network/concurrency conditions.
  - It's an upstream Claude API refusal (my 1482-word random-word prompt hit
    an AUP refusal with short non-empty stderr — distinct from "empty
    output" but worth noting).
  **If it recurs, please capture** (a) the exact prompt that triggered it,
  (b) the exit code of `lot-claude.sh -p ...`, and (c) `DUMBTERM_FLOWTO_TRACE=/tmp/trace.log
  ./lot-claude.sh -p ...` output for post-mortem.

## Regression check

Ran `tests/run_full.sh` — 58/58 tests pass after all three fixes.
Claude Code Write/Edit/Read/Bash/Grep/Glob end-to-end exercised manually
on W7 via real tool calls; no regressions observed.

## No re-verification needed from you

I exercised Write-over-existing, Edit on fresh/CRLF/BOM files, dual-host,
and large prompts in a local harness. Nothing reported to you has regressed.
Resume your sprints.
