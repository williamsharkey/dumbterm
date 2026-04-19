// flowto_shim.js — runs inside the child Node process (--require).
// Intercepts child_process.exec/execSync and routes them through
// the dumbterm gateway at DUMBTERM_GATEWAY=host:port.
//
// The gateway routes each request to either the local agent_handle
// (host:"local") or the remote --flowto agent (host:active).
//
// Phase 1 scope: child_process.exec, execSync, execFile, execFileSync.
// + dumbterm-host / dumbterm-at meta commands.

const net = require('net');
const child_process = require('child_process');
const realPlatform = process.platform; // capture before we override below

const gw = process.env.DUMBTERM_GATEWAY;
if (!gw) { return; /* not running under --flowto; stay inert */ }

// Optional trace log: every intercepted fs/child_process call + host routing
// decision. Set DUMBTERM_FLOWTO_TRACE=<path> (e.g. /tmp/flowto-trace.log).
const _traceFile = process.env.DUMBTERM_FLOWTO_TRACE;
let _traceStream = null;
if (_traceFile) {
    try { _traceStream = require('fs').createWriteStream(_traceFile, { flags: 'a' }); } catch (e) {}
}
function TRACE(tag, data) {
    if (!_traceStream) return;
    try {
        const s = typeof data === 'string' ? data :
            JSON.stringify(data, (k, v) => typeof v === 'string' && v.length > 200 ? v.slice(0, 200) + '…' : v);
        _traceStream.write(`[${Date.now()}] ${tag} ${s}\n`);
    } catch (e) {}
}
TRACE('BOOT', { gw, argv: process.argv.slice(0, 5), pid: process.pid });

const [gwHost, gwPortStr] = gw.split(':');
const gwPort = parseInt(gwPortStr);

// ── Host registry with per-host info ─────────────────────────
// Each host tracks: kind, cwd (virtual cwd for this host), platform, sep, hostname.
// Local host info comes from Node's real APIs; remote is populated lazily via host_info RPC.
const origProcess = { cwd: process.cwd.bind(process), chdir: process.chdir.bind(process) };
const hosts = {
    local: {
        kind: 'local',
        cwd: origProcess.cwd(),
        platform: process.platform,
        sep: process.platform === 'win32' ? '\\' : '/',
        hostname: require('os').hostname(),
    }
};
const defaultRemote = process.env.DUMBTERM_REMOTE_NAME || 'remote';
if (process.env.DUMBTERM_FLOWTO) {
    hosts[defaultRemote] = { kind: 'remote', cwd: null, platform: null, sep: null, hostname: null };
}
let activeHost = process.env.DUMBTERM_FLOWTO ? defaultRemote : 'local';

// Track the host where the most recent remote spawn ran. Used to route
// Claude-Code-style sentinel file reads (e.g. /tmp/claude-XXXX-cwd) to
// the same host where the bash script wrote them.
let lastRemoteSpawnHost = null;

// ── .flowto.json project profile loader ─────────────────────
// Walk up from cwd looking for .flowto.json. Load:
//   defaultHost, remoteCwd, remoteShell, pathMap[[local,remote],...],
//   virtualCwd, virtualFs, virtualEnv, virtualPlatform, virtualPath, virtualHome
// Env flags still win if both are set. pathMap is additive.
function loadProjectProfile() {
    const path = require('path');
    const fsSync = require('fs');
    let dir = origProcess.cwd();
    let cfg = null, configPath = null;
    for (let i = 0; i < 20; i++) {
        const candidate = path.join(dir, '.flowto.json');
        try {
            const raw = fsSync.readFileSync(candidate, 'utf8');
            cfg = JSON.parse(raw); configPath = candidate; break;
        } catch (e) { /* no config here */ }
        const parent = path.dirname(dir);
        if (parent === dir) break;
        dir = parent;
    }
    if (!cfg) return { cfg: {}, configPath: null };
    return { cfg, configPath };
}
const { cfg: projectCfg, configPath: projectCfgPath } = loadProjectProfile();
if (projectCfgPath) {
    process.stderr.write('flowto: loaded profile from ' + projectCfgPath + '\n');
}

// Config-applied values (env still wins when explicitly set)
function cfgBool(envKey, cfgKey) {
    if (process.env[envKey]) return process.env[envKey] === '1';
    return !!projectCfg[cfgKey];
}
function cfgStr(envKey, cfgKey, fallback) {
    return process.env[envKey] || projectCfg[cfgKey] || fallback || '';
}

// Remote cwd/shell from config
if (projectCfg.remoteCwd && hosts[defaultRemote]) hosts[defaultRemote].cwd = projectCfg.remoteCwd;
if (projectCfg.remoteShell && hosts[defaultRemote]) hosts[defaultRemote].shell = projectCfg.remoteShell;
if (projectCfg.defaultHost) activeHost = projectCfg.defaultHost;

// pathMap: [[localPrefix, remotePrefix], ...]
const pathMap = Array.isArray(projectCfg.pathMap) ? projectCfg.pathMap : [];

// localCommands: shells out that should NEVER route to the remote agent.
// Apps like Claude Code call platform bootstrap tools (security, codesign,
// ioreg, where.exe, etc.) during init — if these route to a foreign host
// they fail silently and the app misbehaves (e.g. "Not logged in" because
// `security find-generic-password` for Keychain creds goes to W7).
const LOCAL_CMDS_BUILTIN = [
    // macOS bootstrap tools
    'security', 'codesign', 'sw_vers', 'ioreg', 'scutil', 'defaults',
    'launchctl', 'plutil', 'networksetup', 'osascript',
    // Windows bootstrap tools (when Mac happens to probe for them)
    'where', 'where.exe', 'reg', 'reg.exe', 'wmic', 'wmic.exe',
    'systeminfo', 'powershell', 'powershell.exe',
];
const LOCAL_CMDS_USER = Array.isArray(projectCfg.localCommands) ? projectCfg.localCommands : [];
const LOCAL_CMDS = new Set([...LOCAL_CMDS_BUILTIN, ...LOCAL_CMDS_USER]);

// localPathPrefixes: executable paths that are inherently host-local.
const LOCAL_PATH_PREFIXES_BUILTIN = ['/opt/homebrew/', '/System/', '/Applications/'];
const LOCAL_PATH_PREFIXES_USER = Array.isArray(projectCfg.localPathPrefixes) ? projectCfg.localPathPrefixes : [];
const LOCAL_PATH_PREFIXES = [...LOCAL_PATH_PREFIXES_BUILTIN, ...LOCAL_PATH_PREFIXES_USER];

function shouldRunLocal(cmd) {
    if (typeof cmd !== 'string') return false;
    // basename of first token
    const first = cmd.trim().split(/\s+/)[0] || '';
    const base = first.split('/').pop().split('\\').pop();
    if (LOCAL_CMDS.has(base) || LOCAL_CMDS.has(first)) return true;
    for (const prefix of LOCAL_PATH_PREFIXES) {
        if (first.startsWith(prefix)) return true;
    }
    return false;
}

// commandMap: rewrite the first token of a remote-bound command.
// Claude Code's Bash tool often invokes absolute POSIX paths like /bin/bash
// or /bin/zsh that don't exist on W7; this rewrites them to the MSYS bash
// equivalent. zsh on W7 doesn't exist — bash happens to run zsh-style scripts
// fine for Claude's use case.
// Values MUST be native Windows paths (backslashes, drive letter) — CreateProcessA
// on W7 can't resolve MSYS-style /c/... paths. Only the MSYS bash itself
// understands those; for CreateProcessA we need C:\...
const COMMAND_MAP_DEFAULTS = {
    '/bin/bash': 'C:\\Program Files\\Git\\usr\\bin\\bash.exe',
    '/bin/zsh':  'C:\\Program Files\\Git\\usr\\bin\\bash.exe',
    '/bin/sh':   'C:\\Program Files\\Git\\usr\\bin\\sh.exe',
    '/usr/bin/bash': 'C:\\Program Files\\Git\\usr\\bin\\bash.exe',
    '/usr/bin/zsh':  'C:\\Program Files\\Git\\usr\\bin\\bash.exe',
    '/usr/bin/env':  'C:\\Program Files\\Git\\usr\\bin\\env.exe',
    '/usr/bin/git':  'C:\\Program Files\\Git\\cmd\\git.exe',
};
const COMMAND_MAP_USER = (projectCfg.commandMap && typeof projectCfg.commandMap === 'object') ? projectCfg.commandMap : {};
const COMMAND_MAP = Object.assign({}, COMMAND_MAP_DEFAULTS, COMMAND_MAP_USER);

function rewriteCommandFirstToken(cmd) {
    if (typeof cmd !== 'string') return cmd;
    const tokens = cmd.match(/^(\S+)(\s[\s\S]*)?$/);
    if (!tokens) return cmd;
    const first = tokens[1];
    const rest = tokens[2] || '';
    if (COMMAND_MAP[first]) return '"' + COMMAND_MAP[first] + '"' + rest;
    return cmd;
}

// Rewrite a shell invocation string (e.g. ".flowto.json remoteShell"). The
// shell string may be "/usr/bin/bash" or "/usr/bin/bash -c" — map the first
// token via COMMAND_MAP so CreateProcessA on W7 gets a native Windows path.
// Appends " -c" if the original had no trailing args (shells always need it).
function rewriteShellPath(shell) {
    if (typeof shell !== 'string' || !shell) return shell;
    const m = shell.match(/^(\S+)(\s[\s\S]*)?$/);
    if (!m) return shell;
    const first = m[1];
    const rest = m[2] || '';
    if (!COMMAND_MAP[first]) return shell;
    const mapped = '"' + COMMAND_MAP[first] + '"';
    // If no "-c" (or any arg), append " -c" so the shell accepts a script arg.
    return rest ? mapped + rest : mapped + ' -c';
}

// Detect the "shell -c SCRIPT" invocation pattern used by Claude Code's Bash
// tool. When found, rewrite so the agent invokes the mapped bash DIRECTLY
// (bypassing the outer cmd.exe wrap). Otherwise cmd.exe chokes on bash-isms
// like `>|`, `&&`, or multi-line heredocs.
const SHELL_INVOKERS = new Set([
    '/bin/zsh', '/bin/bash', '/bin/sh',
    '/usr/bin/bash', '/usr/bin/zsh', '/usr/bin/sh',
    'zsh', 'bash', 'sh',
]);

// Returns { script, shell } if pattern matched; null otherwise.
// Only applies to cmd+args signature (spawn), not a pre-joined string.
function detectShellDashC(cmd, args) {
    if (!SHELL_INVOKERS.has(cmd)) return null;
    if (!Array.isArray(args) || args.length < 2) return null;
    if (args[0] !== '-c') return null;
    const script = args.slice(1).join(' ');
    const mappedPath = COMMAND_MAP[cmd] || 'C:\\Program Files\\Git\\usr\\bin\\bash.exe';
    return { script, shell: '"' + mappedPath + '" -c' };
}

// ── RPC connection (synchronous on a dedicated socket) ───────
// We need synchronous-style RPC because child_process.execSync must block.
// Node's net API is async; we use a trick: spawn a helper via child_process
// with a short Node one-liner that does blocking I/O on the socket. Or use
// deasync. For Phase 1, simpler: just use deasync-style polling with the
// node-native 'worker_threads' + SharedArrayBuffer. Actually simplest for
// exec (async only) is pure async callbacks.
//
// For execSync (blocking), we use execFileSync with a helper script that
// does a blocking connect-write-read-close. Phase 1 only handles async
// exec properly; execSync throws "not implemented under --flowto" for now.

let _nextId = 1;
let _pending = new Map(); // id → callback(err, response)
let _conn = null;
let _connBuf = '';

// Keep the socket referenced (blocking event-loop exit) only while there are
// outstanding RPCs. When _pending drains, unref so Node can exit cleanly.
function updateConnRef() {
    if (!_conn) return;
    if (_pending.size > 0) _conn.ref();
    else _conn.unref();
}

function connectGateway(cb) {
    if (_conn) return cb(null);
    _conn = net.createConnection(gwPort, gwHost, () => cb(null));
    // Start unref'd so we don't keep the event loop alive when idle.
    // updateConnRef() re-refs whenever there are pending RPCs and unrefs
    // again once all replies are in — otherwise apps that exit implicitly
    // (like `claude --print` which lets the loop drain) hang forever.
    _conn.unref();
    _conn.on('data', (chunk) => {
        _connBuf += chunk.toString('utf8');
        let nl;
        while ((nl = _connBuf.indexOf('\n')) >= 0) {
            const line = _connBuf.slice(0, nl);
            _connBuf = _connBuf.slice(nl + 1);
            try {
                const msg = JSON.parse(line);
                const p = _pending.get(msg.id);
                if (p) {
                    // For single-response ops, handler is the user callback.
                    // For streaming ops (spawn), handler is rpcStream's wrapper
                    // which keeps itself registered until it sees an exit msg.
                    if (p._streaming) p(null, msg);
                    else { _pending.delete(msg.id); p(null, msg); updateConnRef(); }
                }
            } catch (e) { /* ignore */ }
        }
    });
    _conn.on('error', (e) => { if (cb) cb(e); });
    _conn.on('close', () => { _conn = null; for (const [,p] of _pending) p(new Error('gateway closed')); _pending.clear(); });
}

function rpcCall(req, cb) {
    connectGateway((err) => {
        if (err) return cb(err);
        req.id = _nextId++;
        _pending.set(req.id, cb);
        updateConnRef();
        _conn.write(JSON.stringify(req) + '\n');
    });
}

// For streaming ops (spawn): multiple messages per request. Caller passes
// an onMsg callback that gets invoked for each response until one with
// "exit" arrives. Cleanup happens automatically.
function rpcStream(req, onMsg) {
    connectGateway((err) => {
        if (err) { onMsg(err, null); return; }
        req.id = _nextId++;
        const handler = (err, msg) => {
            if (err) { _pending.delete(req.id); updateConnRef(); onMsg(err, null); return; }
            onMsg(null, msg);
            if (msg && 'exit' in msg) { _pending.delete(req.id); updateConnRef(); }
        };
        handler._streaming = true;
        _pending.set(req.id, handler);
        updateConnRef();
        _conn.write(JSON.stringify(req) + '\n');
    });
}

// ── Fetch host_info lazily and cache on the host entry ───────
function refreshHostInfo(hostName, cb) {
    const h = hosts[hostName];
    if (!h || h.kind !== 'remote') return cb && cb(null);
    rpcCall({ op: 'host_info', host: hostName }, (err, resp) => {
        if (err || !resp.ok) return cb && cb(err || new Error('host_info failed'));
        h.hostname = resp.hostname;
        if (h.cwd == null) h.cwd = resp.cwd; // preserve any previous chdir
        h.platform = resp.platform;
        h.sep = resp.sep;
        h.env = Object.assign({}, resp.env || {}); // snapshot
        cb && cb(null);
    });
}

// ── Intercept dumbterm-host / dumbterm-at meta commands ──────
function maybeMetaCommand(cmd) {
    const m = cmd.trim().match(/^dumbterm-host\s+(\S+)(?:\s|$)/);
    if (m) {
        const name = m[1];
        if (name === 'list') {
            return { handled: true, out: Object.keys(hosts).map(n => (n === activeHost ? '* ' : '  ') + n).join('\n') + '\n', err: '', exit: 0 };
        }
        if (hosts[name]) {
            activeHost = name;
            return { handled: true, out: `active host: ${name}\n`, err: '', exit: 0 };
        }
        return { handled: true, out: '', err: `unknown host: ${name}\n`, exit: 1 };
    }
    const m2 = cmd.trim().match(/^dumbterm-at\s+(\S+)\s+(.+)$/);
    if (m2) {
        return { handled: false, oneShotHost: m2[1], oneShotCmd: m2[2] };
    }
    return { handled: false };
}

// ── Wrap child_process.spawn ──────────────────────────────────
// Returns a ChildProcess-like object with stdout/stderr Readable streams,
// pid, and exit/close events. stdin is stubbed (not yet supported).
const origSpawn = child_process.spawn;
const { Readable, Writable } = require('stream');
const EventEmitter = require('events');

// Claude Code's Bash tool injects `pwd -P >| /tmp/claude-XXXX-cwd` into every
// shell invocation so it can read the post-command cwd back. When the script
// runs remotely the file lives on the remote host; Claude then reads it on
// Mac via readFileSync and ENOENTs — and treats the whole Bash invocation as
// failed, even when stdout is fine. Workaround: pre-write the sentinel
// LOCALLY with the remote host's current cwd before the spawn runs. If the
// script cd's somewhere, the cwd will be slightly stale but Claude only uses
// it as a hint anyway.
function preWriteClaudeSentinel(cmdString, remoteCwd) {
    if (typeof cmdString !== 'string') return;
    const m = cmdString.match(/pwd\s+-P\s*>\|?\s*(\/tmp\/claude-[^\s]+-cwd)/);
    if (!m) return;
    const sentinelPath = m[1];
    // Map remote cwd back to local form via pathMap so Claude's cwd-tracking
    // sees the path *it originally handed us*, not the W7 form.
    let cwd = remoteCwd || '/';
    if (PATH_TRANSLATE_ENABLED && pathMap.length && typeof cwd === 'string') {
        for (const [local, remote] of pathMap) {
            if (cwd.startsWith(remote)) { cwd = local + cwd.slice(remote.length); break; }
        }
    }
    const content = cwd + '\n';
    try {
        require('fs').writeFileSync(sentinelPath, content);
        TRACE('SENTINEL_PREWRITE', { path: sentinelPath, cwd });
    } catch (e) { TRACE('SENTINEL_PREWRITE_FAIL', { path: sentinelPath, err: e.message }); }
}

function makeRemoteChild(cmdString, options) {
    const useHost = options.__flowtoHost || activeHost;
    lastRemoteSpawnHost = useHost;
    preWriteClaudeSentinel(cmdString, (hosts[useHost] && hosts[useHost].cwd) || options.cwd);
    let cwd = options.cwd || (hosts[useHost] && hosts[useHost].cwd) || '';
    cwd = translateForHost(cwd, useHost);
    const rawShell = options.__flowtoShell
        || (hosts[useHost] && hosts[useHost].shell)
        || process.env.DUMBTERM_REMOTE_SHELL
        || '';
    // Map POSIX shell paths (/usr/bin/bash, /bin/sh) to native Windows ones.
    // CreateProcessA on W7 can't resolve /usr/bin/*. __flowtoShell comes in
    // pre-quoted and can be used as-is; raw shell strings need rewriting.
    const shell = options.__flowtoShell ? rawShell : rewriteShellPath(rawShell);
    // Claude Code's Bash tool passes stdio:["pipe", fd, fd] — stdout and
    // stderr redirect to a LOCAL file descriptor the tool already opened.
    // Remote children can't redirect to a Mac fd, so we capture the fd and
    // write incoming remote data to it ourselves.
    //
    // Critical: Claude closes its own reference to the fd right after spawn
    // (via `await S.close()`). After close, fd N becomes invalid/reusable —
    // writing to it either fails or hits someone else's file. Solve by
    // dup'ing the fd NOW via fs.openSync('/dev/fd/N', 'a') so we hold our
    // own independent reference. The dup stays valid even after Claude's
    // close; the OS keeps the underlying file alive while either ref is open.
    let outFd = null, errFd = null;
    const fsMod = require('fs');
    if (Array.isArray(options.stdio)) {
        const dupFd = (n) => {
            if (typeof n !== 'number' || n < 0) return null;
            try { return fsMod.openSync('/dev/fd/' + n, 'a'); }
            catch (e) { TRACE('FD_DUP_FAIL', { fd: n, err: e.message }); return null; }
        };
        outFd = dupFd(options.stdio[1]);
        errFd = dupFd(options.stdio[2]);
    }
    const child = new EventEmitter();
    const stdoutStream = new Readable({ read() {} });
    const stderrStream = new Readable({ read() {} });
    let _spawnId = null; // set once the agent responds with pid
    const stdinStream = new Writable({
        write(chunk, enc, cb) {
            if (_spawnId == null) { cb(new Error('child not yet spawned')); return; }
            const b64 = Buffer.from(chunk).toString('base64');
            // Bypass rpcCall id auto-increment; we reuse the spawn's id.
            // Direct socket write with op:spawn_stdin and the same id as spawn.
            const req = JSON.stringify({ op: 'spawn_stdin', id: _spawnId, data: b64 }) + '\n';
            if (_conn) { _conn.write(req, cb); } else cb(new Error('no gateway'));
        },
        final(cb) {
            if (_spawnId == null) return cb();
            const req = JSON.stringify({ op: 'spawn_stdin_close', id: _spawnId }) + '\n';
            if (_conn) { _conn.write(req, cb); } else cb();
        }
    });
    child.stdout = stdoutStream;
    child.stderr = stderrStream;
    child.stdin = stdinStream;
    // Node's ChildProcess exposes stdio as an array too. Consumers that
    // access .stdio[0/1/2] (common with execa-style libraries) get the right
    // streams this way.
    child.stdio = [stdinStream, stdoutStream, stderrStream];
    child.pid = null;
    child.killed = false;
    child.exitCode = null;
    child.signalCode = null;
    child.kill = function () { /* not yet supported */ };

    const remoteCmd = rewriteCommandFirstToken(cmdString);
    rpcStream({ op: 'spawn', host: useHost, cmd: remoteCmd, cwd, shell }, (err, msg) => {
        if (err) {
            stdoutStream.destroy(err);
            stderrStream.destroy(err);
            child.emit('error', err);
            return;
        }
        if (_spawnId == null && msg && msg.id != null) _spawnId = msg.id;
        if (msg.ok && msg.pid) {
            child.pid = msg.pid;
            child.emit('spawn');
            return;
        }
        if (msg.stream === 'out' && msg.data) {
            const b = Buffer.from(msg.data, 'base64');
            TRACE('REMOTE_STDOUT', { pid: child.pid, len: b.length, data: b.toString('utf8').slice(0, 50), outFd });
            stdoutStream.push(b);
            if (outFd != null) { try { require('fs').writeSync(outFd, b); } catch (e) { TRACE('FD_WRITE_FAIL', { outFd, err: e.message }); } }
        } else if (msg.stream === 'err' && msg.data) {
            const b = Buffer.from(msg.data, 'base64');
            TRACE('REMOTE_STDERR', { pid: child.pid, len: b.length, errFd });
            stderrStream.push(b);
            if (errFd != null) { try { require('fs').writeSync(errFd, b); } catch (e) { TRACE('FD_WRITE_FAIL', { errFd, err: e.message }); } }
        } else if ('exit' in msg) {
            TRACE('REMOTE_EXIT', { pid: child.pid, code: msg.exit });
            child.exitCode = msg.exit;
            // Close our dup'd fds so Claude's append-write-read file
            // sequence sees a flushed, consistent file.
            if (outFd != null) { try { fsMod.closeSync(outFd); } catch (e) {} outFd = null; }
            if (errFd != null && errFd !== outFd) { try { fsMod.closeSync(errFd); } catch (e) {} errFd = null; }
            // Delay the exit+close emits so any same-tick data pushes flush
            // through the stream's internal buffer first. Without this,
            // consumers (e.g. Claude Code's Bash tool which attaches the
            // 'data' handler inside a Promise resolved on 'exit') can see
            // 'exit' fire before the last push is dispatched and conclude
            // the command produced no output.
            setImmediate(() => {
                stdoutStream.push(null);
                stderrStream.push(null);
                child.emit('exit', msg.exit, null);
                child.emit('close', msg.exit, null);
            });
        }
    });
    return child;
}

child_process.spawn = function (cmd, args, options) {
    // Node's spawn signature: spawn(cmd) | spawn(cmd, args) | spawn(cmd, options) | spawn(cmd, args, options)
    if (Array.isArray(args)) { options = options || {}; }
    else if (typeof args === 'object' && args !== null) { options = args; args = []; }
    else { args = []; options = options || {}; }
    TRACE('SPAWN', {
        cmd,
        args: Array.isArray(args) ? args.slice(0, 8) : args,
        stdio: options.stdio,
        cwd: options.cwd,
    });

    const meta = maybeMetaCommand(cmd);
    if (meta.handled) {
        // Build a fake child that emits the meta result
        const child = new EventEmitter();
        const out = new Readable({ read() {} });
        const err = new Readable({ read() {} });
        child.stdout = out; child.stderr = err;
        child.stdin = new Writable({ write(c,e,cb){cb();} });
        process.nextTick(() => {
            if (meta.out) out.push(Buffer.from(meta.out));
            if (meta.err) err.push(Buffer.from(meta.err));
            out.push(null); err.push(null);
            child.emit('exit', meta.exit, null);
            child.emit('close', meta.exit, null);
        });
        return child;
    }

    let useHost = meta.oneShotHost || activeHost;
    const useCmd = meta.oneShotCmd || (args.length ? cmd + ' ' + args.join(' ') : cmd);
    if (!meta.oneShotHost && shouldRunLocal(useCmd)) useHost = 'local';

    if (useHost === 'local') {
        if (VIRTUAL_PLATFORM_ENABLED) {
            const savedPlatformDesc = Object.getOwnPropertyDescriptor(process, 'platform');
            Object.defineProperty(process, 'platform', { value: realPlatform, configurable: true });
            try { return origSpawn.call(this, cmd, args, options); }
            finally { if (savedPlatformDesc) Object.defineProperty(process, 'platform', savedPlatformDesc); }
        }
        return origSpawn.call(this, cmd, args, options);
    }

    // Special-case shell-wrapped invocations. Claude Code's Bash tool uses
    // /bin/zsh -c "SCRIPT". Route SCRIPT through MSYS bash directly so the
    // agent doesn't wrap in cmd.exe (which can't parse bash syntax).
    const sdc = detectShellDashC(cmd, args);
    if (sdc) {
        return makeRemoteChild(sdc.script, Object.assign({}, options, { __flowtoShell: sdc.shell, __flowtoHost: useHost }));
    }
    return makeRemoteChild(useCmd, Object.assign({}, options, { __flowtoHost: useHost }));
};

// ── Wrap child_process.exec ──────────────────────────────────
const origExec = child_process.exec;
child_process.exec = function (command, options, callback) {
    if (typeof options === 'function') { callback = options; options = {}; }
    options = options || {};
    TRACE('EXEC', { command: String(command).slice(0, 200) });

    const meta = maybeMetaCommand(command);
    if (meta.handled) {
        if (callback) process.nextTick(() => callback(
            meta.exit === 0 ? null : Object.assign(new Error(`Exit ${meta.exit}`), { code: meta.exit }),
            meta.out, meta.err));
        return; // no ChildProcess object returned — callers that expect one will need Phase 2
    }

    const useHost = meta.oneShotHost || activeHost;
    const useCmd = meta.oneShotCmd || command;

    if (useHost === 'local') {
        if (VIRTUAL_PLATFORM_ENABLED) {
            const savedPlatformDesc = Object.getOwnPropertyDescriptor(process, 'platform');
            Object.defineProperty(process, 'platform', { value: realPlatform, configurable: true });
            try { return origExec.call(this, useCmd, options, callback); }
            finally { if (savedPlatformDesc) Object.defineProperty(process, 'platform', savedPlatformDesc); }
        }
        return origExec.call(this, useCmd, options, callback);
    }

    // Remote: RPC to the gateway. Use virtual cwd of target host (not process.cwd()
    // which would leak a Mac path to a Windows agent).
    let targetCwd = options.cwd || (hosts[useHost] && hosts[useHost].cwd) || '';
    targetCwd = translateForHost(targetCwd, useHost);
    const targetShell = (hosts[useHost] && hosts[useHost].shell) || process.env.DUMBTERM_REMOTE_SHELL || '';
    const remoteCmd = rewriteCommandFirstToken(useCmd);
    rpcCall({ op: 'exec', host: useHost, cmd: remoteCmd, cwd: targetCwd, shell: targetShell }, (err, resp) => {
        if (err) { if (callback) callback(err, '', ''); return; }
        const out = Buffer.from(resp.out || '', 'base64').toString('utf8');
        const errStr = Buffer.from(resp.err || '', 'base64').toString('utf8');
        const exitCode = resp.exit;
        if (callback) {
            if (exitCode === 0) callback(null, out, errStr);
            else {
                const e = new Error(`Command failed: ${useCmd}`);
                e.code = exitCode; e.stdout = out; e.stderr = errStr;
                callback(e, out, errStr);
            }
        }
    });
};

// ── /cloud/... persistent-home path scheme ───────────────────
// Any path under /cloud/ routes to the persistent host regardless of
// active host. Default persistent host = defaultRemote (the --flowto target).
const PERSISTENT_HOST = process.env.DUMBTERM_PERSISTENT_HOST || defaultRemote;
const CLOUD_PREFIX = '/cloud/dumbterm/';
const CLOUD_HOME = '/cloud/dumbterm/home';
const CLOUD_BACKING = process.env.DUMBTERM_CLOUD_BACKING || 'C:\\workspace\\dumbterm-cloud';
// Virtualization modes — all opt-in. Read from env first, then .flowto.json.
const VIRTUAL_HOME_ENABLED     = cfgBool('DUMBTERM_VIRTUAL_HOME',     'virtualHome');
const VIRTUAL_CWD_ENABLED      = cfgBool('DUMBTERM_VIRTUAL_CWD',      'virtualCwd');
const VIRTUAL_PLATFORM_ENABLED = cfgBool('DUMBTERM_VIRTUAL_PLATFORM', 'virtualPlatform');
const VIRTUAL_PATH_ENABLED     = cfgBool('DUMBTERM_VIRTUAL_PATH',     'virtualPath');
const VIRTUAL_ENV_ENABLED      = cfgBool('DUMBTERM_VIRTUAL_ENV',      'virtualEnv');
const VIRTUAL_FS_ENABLED       = cfgBool('DUMBTERM_VIRTUAL_FS',       'virtualFs');
// Aggressive mode: route EVERY unmapped path to the active remote host.
// Only useful for the test suite; Claude Code and most real apps break
// under this because they read their own config files under $HOME.
const VIRTUAL_FS_AGGRESSIVE    = process.env.DUMBTERM_VIRTUAL_FS_AGGRESSIVE === '1' || !!projectCfg.virtualFsAggressive;
const PATH_TRANSLATE_ENABLED   = cfgBool('DUMBTERM_PATH_TRANSLATE',   'pathTranslate') || pathMap.length > 0 || VIRTUAL_FS_ENABLED;

function isCloudPath(p) {
    return typeof p === 'string' && p.startsWith('/cloud/');
}

// Claude Code's Bash tool writes cwd-tracking sentinel files like
// `/tmp/claude-XXXX-cwd` inside the bash script it spawns (via `pwd -P >|`),
// then reads them back via fs.readFile to learn the post-command cwd. When
// bash runs on a remote host, the file lives in that host's /tmp — reading
// it from Mac's /tmp would ENOENT, and Claude treats that as an empty-output
// failure. Route these reads to the host where the last remote spawn ran.
function isClaudeSentinel(p) {
    if (typeof p !== 'string') return false;
    return /^\/tmp\/claude-[^\/]+/.test(p);
}
// Map /cloud/dumbterm/home/foo → CLOUD_BACKING + /foo (platform-translated)
function translateCloudPath(p) {
    if (!p.startsWith(CLOUD_PREFIX)) return p;
    const rel = p.slice(CLOUD_PREFIX.length);   // "home/foo"
    // naive join — agent will open it as-is
    return CLOUD_BACKING + (CLOUD_BACKING.includes('\\') ? '\\' : '/') + rel.replace(/\//g, CLOUD_BACKING.includes('\\') ? '\\' : '/');
}

// Which host should a path go to?
// - /cloud/... always persists to PERSISTENT_HOST
// - /tmp/claude-* sentinel files route to the host of the last remote spawn
// - Paths matching pathMap's local prefixes route to the remote (active host)
// - Otherwise: LOCAL.
//
// Note: `virtualFs: true` used to mean "route every unmapped path to remote",
// but that was too aggressive — Claude Code (and any other app) reads its own
// config files under $HOME, and those obviously don't exist on the remote.
// The active-host fallback is now gated behind VIRTUAL_FS_AGGRESSIVE (env-only,
// no .flowto.json knob) for the rare case where you really want it.
function routeForPath(p) {
    if (isCloudPath(p)) return PERSISTENT_HOST;
    if (isClaudeSentinel(p) && lastRemoteSpawnHost) return lastRemoteSpawnHost;
    if (PATH_TRANSLATE_ENABLED && pathMap.length && typeof p === 'string') {
        for (const [local, remote] of pathMap) {
            // Either side of the map means "this belongs to the remote host":
            // Mac paths to be translated on the way out, or already-remote
            // paths returned to us (e.g. via virtualCwd).
            if (p.startsWith(local) || p.startsWith(remote)) return activeHost;
        }
    }
    if (VIRTUAL_FS_AGGRESSIVE) return activeHost;
    return 'local';
}

// Convert an MSYS-style Windows path (e.g. /c/workspace/lot) to native
// Windows form (C:\workspace\lot). No-op for paths that don't match the
// /<letter>/ prefix or for non-Windows targets.
function msysToWin(p) {
    if (typeof p !== 'string') return p;
    const m = p.match(/^\/([a-zA-Z])(\/.*|$)/);
    if (!m) return p;
    const drive = m[1].toUpperCase();
    const rest = (m[2] || '').replace(/\//g, '\\');
    return drive + ':' + (rest || '\\');
}

// Translate a path from driver (Mac) convention to active host convention.
// Prefix substitution via pathMap, then MSYS→Win conversion on Windows hosts
// (because the agent uses native Win32 opendir/stat which doesn't understand
// /c/...). No-op if no mapping matches.
function translateForHost(p, hostName) {
    if (typeof p !== 'string') return p;
    if (isCloudPath(p)) return translateCloudPath(p);
    if (hostName === 'local') return p;
    let out = p;
    if (PATH_TRANSLATE_ENABLED && pathMap.length) {
        for (const [local, remote] of pathMap) {
            if (p.startsWith(local)) { out = remote + p.slice(local.length); break; }
        }
    }
    // Always apply MSYS→Win conversion for non-local hosts. Paths that don't
    // match /<letter>/ are left untouched. We don't gate on h.platform here
    // because fs ops fire early (before host_info lands), so the platform
    // would still be null — defaulting to "try win32" is safe since non-MSYS
    // paths are a no-op.
    const h = hosts[hostName];
    if (!h || h.platform !== 'linux') out = msysToWin(out);
    return out;
}

// ── Override HOME / os.homedir (opt-in) ──────────────────────
const os = require('os');
if (VIRTUAL_HOME_ENABLED) {
    process.env.HOME = CLOUD_HOME;
    const origHomedir = os.homedir;
    os.homedir = () => CLOUD_HOME;
    os.userInfo = (function(orig) {
        return function(opts) {
            const r = orig.call(this, opts);
            r.homedir = CLOUD_HOME;
            return r;
        };
    })(os.userInfo);
}

// ── Intercept fs.* ───────────────────────────────────────────
const fs = require('fs');

function rpcToHost(host, req, cb) {
    if (host === 'local') return null; // caller must handle
    req.host = host;
    rpcCall(req, cb);
    return true;
}

// ── Sync RPC via helper process ──────────────────────────────
// Node's event loop can't do sync TCP, so we spawn a short-lived helper
// via execFileSync (which blocks the parent until the child exits) that
// does the RPC and prints the response JSON to stdout. ~30-50ms overhead
// per sync call — fine for Claude's occasional readFileSync/statSync/
// writeFileSync/mkdirSync but terrible in a hot loop (don't use for
// traversal — rely on async readdir for that).
const _cpSync = require('child_process');
const _syncHelperPath = require('path').join(__dirname, 'flowto_sync_helper.js');

// Sync routing: never use VIRTUAL_FS_AGGRESSIVE for sync ops. Node's module
// loader, Claude Code's local config readers, and many libraries hit
// readFileSync/statSync every few ms — forcing those to remote would
// cripple performance and break module loading for tools whose .js files
// live only on Mac. Sync ops only route remote when explicit pathMap
// / cloud / sentinel rules match.
function routeForPathSync(p) {
    if (isCloudPath(p)) return PERSISTENT_HOST;
    if (isClaudeSentinel(p) && lastRemoteSpawnHost) return lastRemoteSpawnHost;
    if (PATH_TRANSLATE_ENABLED && pathMap.length && typeof p === 'string') {
        for (const [local, remote] of pathMap) {
            if (p.startsWith(local) || p.startsWith(remote)) return activeHost;
        }
    }
    return 'local';
}
function rpcSync(req) {
    if (!process.env.DUMBTERM_GATEWAY) throw new Error('flowto: no gateway');
    const reqLine = JSON.stringify(req);
    TRACE('RPC_SYNC', { op: req.op, path: req.path, host: req.host });
    try {
        const out = _cpSync.execFileSync(process.execPath, [_syncHelperPath], {
            input: reqLine,
            env: process.env,
            encoding: 'utf8',
            timeout: 40000,
            maxBuffer: 64 * 1024 * 1024,
            stdio: ['pipe', 'pipe', 'inherit'],
        });
        const line = out.split('\n')[0];
        return JSON.parse(line);
    } catch (e) {
        TRACE('RPC_SYNC_FAIL', { op: req.op, err: e.message });
        return { ok: false, errno: 'EIO', err: 'helper: ' + e.message };
    }
}
function enoentFor(p) { const e = new Error('ENOENT'); e.code = 'ENOENT'; e.errno = -2; e.path = p; e.syscall = 'open'; return e; }
function syscallError(code, p, syscall) {
    const e = new Error(code + ': ' + (syscall || 'op')); e.code = code; e.path = p; e.syscall = syscall || 'op'; return e;
}

// ── Virtual fd table for remote file reads ──────────────────
// Claude's iC() helper (used by Write/Edit) does:
//     openSync(path, 'r') → readSync(fd, buf, ...) → closeSync(fd)
// to detect BOM/encoding on the first 4KB. Without openSync wrapping,
// Node's native openSync hits Mac's local fs and throws ENOENT; Edit
// then thinks the file is empty and fails with "String not found".
// We maintain a small virtual-fd table: open routes via RPC-read, stash
// the bytes; read/close operate on that cached buffer.
const _virtualFds = new Map();  // fd (number) → { path, host, buf, pos }
let _nextVirtualFd = 1000000;   // avoid collision with real fds

const origOpenSync = fs.openSync;
fs.openSync = function (path, flags, mode) {
    const host = routeForPathSync(path);
    TRACE('OPENSYNC', { path, host, flags });
    if (host === 'local') return origOpenSync.apply(this, arguments);
    // Only support read modes for remote (O_RDONLY or 'r').
    const isRead = flags === undefined || flags === 0 || flags === 'r' || flags === fs.constants.O_RDONLY;
    if (!isRead) {
        TRACE('OPENSYNC_WRITE_UNSUPPORTED', { path, flags });
        throw syscallError('EPERM', path, 'open');
    }
    const remotePath = translateForHost(path, host);
    const resp = rpcSync({ op: 'read', host, path: remotePath });
    if (!resp.ok) throw enoentFor(path);
    const buf = Buffer.from(resp.data || '', 'base64');
    const fd = _nextVirtualFd++;
    _virtualFds.set(fd, { path, host, buf, pos: 0 });
    return fd;
};

const origReadSync = fs.readSync;
fs.readSync = function (fd, buffer, offsetOrOpts, length, position) {
    const v = _virtualFds.get(fd);
    if (!v) return origReadSync.apply(this, arguments);
    // Two signatures:
    //   readSync(fd, buffer, offset, length, position)        — 5-arg
    //   readSync(fd, {buffer?, offset?, length?, position?})  — 2-arg, returns {buffer,bytesRead}
    // Claude Code's aU6 encoding detector uses the 2-arg form.
    const isOptsForm = (buffer !== null && typeof buffer === 'object'
                        && !Buffer.isBuffer(buffer) && !(buffer instanceof Uint8Array));
    let target, offset, len, pos;
    if (isOptsForm) {
        const opts = buffer;
        len = opts.length != null ? opts.length : (opts.buffer ? opts.buffer.length : 16384);
        offset = opts.offset || 0;
        pos = opts.position != null ? opts.position : v.pos;
        target = opts.buffer || Buffer.alloc(len);
    } else {
        target = buffer;
        offset = offsetOrOpts || 0;
        len = length;
        pos = position != null ? position : v.pos;
    }
    const avail = Math.max(0, v.buf.length - pos);
    const n = Math.min(len || 0, avail);
    if (n > 0) v.buf.copy(target, offset, pos, pos + n);
    const posWasExplicit = isOptsForm ? (buffer.position != null) : (position != null);
    if (!posWasExplicit) v.pos = pos + n;
    return isOptsForm ? { buffer: target, bytesRead: n } : n;
};

const origCloseSync = fs.closeSync;
fs.closeSync = function (fd) {
    if (_virtualFds.has(fd)) { _virtualFds.delete(fd); return undefined; }
    return origCloseSync.apply(this, arguments);
};

// lstatSync — Claude's vA() helper calls lstatSync to detect symlinks
// before iC() opens the file. Route to remote same as statSync (we don't
// follow symlinks on the remote end either — good enough for Claude's
// check, which just wants to know if the path is a regular file).
const origLstatSync = fs.lstatSync;
fs.lstatSync = function (path, opts) {
    const host = routeForPathSync(path);
    TRACE('LSTATSYNC', { path, host });
    if (host === 'local') return origLstatSync.apply(this, arguments);
    return fs.statSync(path, opts);
};

// realpathSync — Claude's vA() calls this to resolve symlinks. Our agent
// doesn't resolve symlinks; just return the mapped path as "canonical".
const origRealpathSync = fs.realpathSync;
fs.realpathSync = function (path, opts) {
    const host = routeForPathSync(path);
    TRACE('REALPATHSYNC', { path, host });
    if (host === 'local') return origRealpathSync.apply(this, arguments);
    // No symlink resolution on remote — return path as-is. Claude's vA
    // wraps this in try/catch so "can't resolve" is fine.
    return path;
};

// readFileSync — Claude's Write tool reads the existing file via iC() to
// detect changes. Must route through pathMap to W7, not Mac's local fs.
const origReadFileSync = fs.readFileSync;
fs.readFileSync = function (path, opts) {
    const host = routeForPathSync(path);
    TRACE('READFILESYNC', { path, host, opts: opts && typeof opts === 'object' ? opts.encoding : opts });
    if (host === 'local') return origReadFileSync.call(this, path, opts);
    const remotePath = translateForHost(path, host);
    const resp = rpcSync({ op: 'read', host, path: remotePath });
    if (!resp.ok) throw enoentFor(path);
    const buf = Buffer.from(resp.data || '', 'base64');
    TRACE('READFILESYNC_DATA', { path, len: buf.length, preview: buf.slice(0, 80).toString('utf8') });
    if (opts === 'utf8' || (opts && opts.encoding === 'utf8')) return buf.toString('utf8');
    if (typeof opts === 'string') return buf.toString(opts);
    if (opts && opts.encoding) return buf.toString(opts.encoding);
    return buf;
};

// statSync — Write tool calls this to check existence. Must return ENOENT
// for non-existent remote files so Claude's try/catch sets J=null and
// proceeds to create. Also used by Av() for mtime comparison.
const origStatSync = fs.statSync;
fs.statSync = function (path, opts) {
    const host = routeForPathSync(path);
    TRACE('STATSYNC', { path, host });
    if (host === 'local') return origStatSync.call(this, path, opts);
    const remotePath = translateForHost(path, host);
    const resp = rpcSync({ op: 'stat', host, path: remotePath });
    if (!resp.ok) {
        TRACE('STATSYNC_FAIL', { path, remotePath, resp });
        if (opts && opts.throwIfNoEntry === false) return undefined;
        throw enoentFor(path);
    }
    return {
        size: resp.size,
        mtime: new Date(resp.mtime * 1000), mtimeMs: resp.mtime * 1000,
        atime: new Date(resp.mtime * 1000), atimeMs: resp.mtime * 1000,
        ctime: new Date(resp.mtime * 1000), ctimeMs: resp.mtime * 1000,
        birthtime: new Date(resp.mtime * 1000), birthtimeMs: resp.mtime * 1000,
        isDirectory: () => !!resp.isDir, isFile: () => !resp.isDir,
        isSymbolicLink: () => false, isBlockDevice: () => false,
        isCharacterDevice: () => false, isFIFO: () => false, isSocket: () => false,
        mode: resp.mode || 0, uid: 0, gid: 0, dev: 0, ino: 0, nlink: 1, rdev: 0,
        blocks: 0, blksize: 4096,
    };
};

// writeFileSync — the actual write Claude's Write tool performs.
const origWriteFileSync = fs.writeFileSync;
fs.writeFileSync = function (path, data, opts) {
    const host = routeForPathSync(path);
    if (host === 'local') {
        TRACE('WRITEFILESYNC', { path, host, len: data && data.length });
        return origWriteFileSync.call(this, path, data, opts);
    }
    const remotePath = translateForHost(path, host);
    const buf = Buffer.isBuffer(data) ? data : Buffer.from(String(data), (opts && opts.encoding) || 'utf8');
    TRACE('WRITEFILESYNC', { path, host, len: buf.length, preview: buf.slice(0, 80).toString('utf8') });
    const resp = rpcSync({ op: 'write', host, path: remotePath, data: buf.toString('base64') });
    if (!resp.ok) throw syscallError(resp.errno || 'EIO', path, 'write');
    return undefined;
};

// mkdirSync — parent-dir creation. Must silently succeed on EEXIST if
// {recursive: true}. Claude's V8().mkdir wrapper likely already retries.
const origMkdirSync = fs.mkdirSync;
fs.mkdirSync = function (path, opts) {
    const host = routeForPathSync(path);
    TRACE('MKDIRSYNC', { path, host, opts });
    if (host === 'local') return origMkdirSync.call(this, path, opts);
    const remotePath = translateForHost(path, host);
    const recursive = opts && (opts === true || opts.recursive === true);
    const resp = rpcSync({ op: 'mkdir', host, path: remotePath });
    if (!resp.ok) {
        if (recursive && resp.errno === 'EEXIST') return undefined;
        throw syscallError(resp.errno || 'EIO', path, 'mkdir');
    }
    return undefined;
};

// readdirSync + accessSync: handy for tools that enumerate before writing.
const origReaddirSync = fs.readdirSync;
fs.readdirSync = function (path, opts) {
    const host = routeForPathSync(path);
    TRACE('READDIRSYNC', { path, host });
    if (host === 'local') return origReaddirSync.call(this, path, opts);
    const remotePath = translateForHost(path, host);
    const resp = rpcSync({ op: 'readdir', host, path: remotePath });
    if (!resp.ok) throw enoentFor(path);
    return resp.entries || [];
};

const origAccessSync = fs.accessSync;
fs.accessSync = function (path, mode) {
    const host = routeForPathSync(path);
    TRACE('ACCESSSYNC', { path, host });
    if (host === 'local') return origAccessSync.call(this, path, mode);
    // Access = stat-and-check-exists for our purposes.
    const remotePath = translateForHost(path, host);
    const resp = rpcSync({ op: 'stat', host, path: remotePath });
    if (!resp.ok) throw enoentFor(path);
    return undefined;
};

// existsSync — derived from access. Must never throw.
const origExistsSync = fs.existsSync;
fs.existsSync = function (path) {
    const host = routeForPathSync(path);
    TRACE('EXISTSSYNC', { path, host });
    if (host === 'local') return origExistsSync.call(this, path);
    try { fs.accessSync(path); return true; } catch (e) { return false; }
};

// fs.readFile(path, [opts], cb)
const origReadFile = fs.readFile;
fs.readFile = function (path, opts, cb) {
    if (typeof opts === 'function') { cb = opts; opts = undefined; }
    const host = routeForPath(path);
    TRACE('READFILE', { path, host, opts: opts && typeof opts === 'object' ? opts.encoding : opts });
    if (host === 'local') {
        return origReadFile.call(this, path, opts, cb);
    }
    const remotePath = translateForHost(path, host);
    rpcCall({ op: 'read', host, path: remotePath }, (err, resp) => {
        if (err) return cb && cb(err);
        if (!resp.ok) {
            const e = new Error(resp.err || resp.errno || 'ENOENT');
            e.code = resp.errno || 'ENOENT'; e.path = path;
            return cb && cb(e);
        }
        const buf = Buffer.from(resp.data || '', 'base64');
        TRACE('READFILE_DATA', { path, len: buf.length, preview: buf.slice(0, 64).toString('utf8') });
        if (opts && (opts === 'utf8' || opts.encoding === 'utf8')) return cb && cb(null, buf.toString('utf8'));
        cb && cb(null, buf);
    });
};

// fs.writeFile(path, data, [opts], cb)
const origWriteFile = fs.writeFile;
fs.writeFile = function (path, data, opts, cb) {
    if (typeof opts === 'function') { cb = opts; opts = undefined; }
    const host = routeForPath(path);
    TRACE('WRITEFILE', { path, host });
    if (host === 'local') {
        return origWriteFile.call(this, path, data, opts, cb);
    }
    const remotePath = translateForHost(path, host);
    const buf = Buffer.isBuffer(data) ? data : Buffer.from(String(data), (opts && opts.encoding) || 'utf8');
    rpcCall({ op: 'write', host, path: remotePath, data: buf.toString('base64') }, (err, resp) => {
        if (err) return cb && cb(err);
        if (!resp.ok) {
            const e = new Error(resp.err || resp.errno || 'EIO');
            e.code = resp.errno || 'EIO'; e.path = path;
            return cb && cb(e);
        }
        cb && cb(null);
    });
};

// fs.readdir(path, [opts], cb)
const origReaddir = fs.readdir;
fs.readdir = function (path, opts, cb) {
    if (typeof opts === 'function') { cb = opts; opts = undefined; }
    const host = routeForPath(path);
    TRACE('READDIR', { path, host });
    if (host === 'local') return origReaddir.call(this, path, opts, cb);
    const remotePath = translateForHost(path, host);
    rpcCall({ op: 'readdir', host, path: remotePath }, (err, resp) => {
        if (err) return cb && cb(err);
        if (!resp.ok) {
            const e = new Error(resp.err || 'ENOENT'); e.code = resp.errno || 'ENOENT'; e.path = path;
            return cb && cb(e);
        }
        cb && cb(null, resp.entries || []);
    });
};

// fs.stat(path, cb)
const origStat = fs.stat;
fs.stat = function (path, opts, cb) {
    if (typeof opts === 'function') { cb = opts; opts = undefined; }
    const host = routeForPath(path);
    TRACE('STAT', { path, host });
    if (host === 'local') return origStat.call(this, path, opts, cb);
    const remotePath = translateForHost(path, host);
    rpcCall({ op: 'stat', host, path: remotePath }, (err, resp) => {
        if (err) return cb && cb(err);
        if (!resp.ok) {
            const e = new Error('ENOENT'); e.code = 'ENOENT'; e.path = path;
            return cb && cb(e);
        }
        // Mimic Node's fs.Stats shape minimally
        cb && cb(null, {
            size: resp.size, mtime: new Date(resp.mtime * 1000), mtimeMs: resp.mtime * 1000,
            isDirectory: () => !!resp.isDir, isFile: () => !resp.isDir,
            mode: resp.mode || 0,
        });
    });
};

// fs.unlink(path, cb)
const origUnlink = fs.unlink;
fs.unlink = function (path, cb) {
    const host = routeForPath(path);
    if (host === 'local') return origUnlink.call(this, path, cb);
    const remotePath = translateForHost(path, host);
    rpcCall({ op: 'unlink', host, path: remotePath }, (err, resp) => {
        if (err) return cb && cb(err);
        if (!resp.ok) { const e = new Error(resp.errno || 'EIO'); e.code = resp.errno || 'EIO'; return cb && cb(e); }
        cb && cb(null);
    });
};

// fs.rename — atomic-write pattern (Claude's Write tool writes to foo.tmp,
// then renames to foo). Without remote rename support the .tmp sidecar
// lingers forever on W7.
const origRename = fs.rename;
fs.rename = function (from, to, cb) {
    const hostFrom = routeForPath(from);
    const hostTo = routeForPath(to);
    if (hostFrom === 'local' && hostTo === 'local') return origRename.call(this, from, to, cb);
    if (hostFrom !== hostTo) return cb && cb(Object.assign(new Error('EXDEV: cross-host rename not supported'), { code: 'EXDEV' }));
    const rFrom = translateForHost(from, hostFrom);
    const rTo   = translateForHost(to, hostTo);
    rpcCall({ op: 'rename', host: hostFrom, from: rFrom, to: rTo }, (err, resp) => {
        if (err) return cb && cb(err);
        if (!resp.ok) { const e = new Error(resp.errno || 'EIO'); e.code = resp.errno || 'EIO'; return cb && cb(e); }
        cb && cb(null);
    });
};

// unlinkSync — Claude's Uf6 unlinks orphaned .tmp files on failure, and
// its Edit fallback path explicitly unlinks before rewriting. Without this
// wrapper the unlink hits local Mac fs, always ENOENTs, and the orphan
// lingers on W7.
const origUnlinkSync = fs.unlinkSync;
fs.unlinkSync = function (path) {
    const host = routeForPathSync(path);
    TRACE('UNLINKSYNC', { path, host });
    if (host === 'local') return origUnlinkSync.apply(this, arguments);
    const remotePath = translateForHost(path, host);
    const resp = rpcSync({ op: 'unlink', host, path: remotePath });
    if (!resp.ok) throw syscallError(resp.errno || 'EIO', path, 'unlink');
    return undefined;
};

// chmodSync / fchmodSync — Claude's Uf6 applies the original file's mode
// to the new tmp (Wd5() call) before renaming. The agent doesn't track
// POSIX modes and the W7 filesystem doesn't use them meaningfully, so we
// silently succeed. Without this the chmodSync throws → Uf6's atomic
// rename path is skipped → non-atomic fallback writes directly to target
// and leaves the orphan .tmp.
const origChmodSync = fs.chmodSync;
fs.chmodSync = function (path, mode) {
    const host = routeForPathSync(path);
    TRACE('CHMODSYNC', { path, host, mode });
    if (host === 'local') return origChmodSync.apply(this, arguments);
    return undefined;  // no-op on remote
};
if (fs.fchmodSync) {
    const origFchmodSync = fs.fchmodSync;
    fs.fchmodSync = function (fd, mode) {
        if (_virtualFds.has(fd)) return undefined;
        return origFchmodSync.apply(this, arguments);
    };
}

const origRenameSync = fs.renameSync;
fs.renameSync = function (from, to) {
    const hostFrom = routeForPathSync(from);
    const hostTo = routeForPathSync(to);
    TRACE('RENAMESYNC', { from, to, hostFrom, hostTo });
    if (hostFrom === 'local' && hostTo === 'local') return origRenameSync.call(this, from, to);
    if (hostFrom !== hostTo) throw syscallError('EXDEV', from, 'rename');
    const rFrom = translateForHost(from, hostFrom);
    const rTo   = translateForHost(to, hostTo);
    const resp = rpcSync({ op: 'rename', host: hostFrom, from: rFrom, to: rTo });
    if (!resp.ok) {
        TRACE('RENAMESYNC_FAIL', { from, to, rFrom, rTo, resp });
        throw syscallError(resp.errno || 'EIO', from, 'rename');
    }
    return undefined;
};

// fs.mkdir
const origMkdir = fs.mkdir;
fs.mkdir = function (path, opts, cb) {
    if (typeof opts === 'function') { cb = opts; opts = undefined; }
    const host = routeForPath(path);
    if (host === 'local') return origMkdir.call(this, path, opts, cb);
    const remotePath = translateForHost(path, host);
    rpcCall({ op: 'mkdir', host, path: remotePath }, (err, resp) => {
        if (err) return cb && cb(err);
        if (!resp.ok) { const e = new Error(resp.errno || 'EIO'); e.code = resp.errno || 'EIO'; return cb && cb(e); }
        cb && cb(null);
    });
};

// fs.promises: rebuild from patched callback versions
fs.promises.readFile = (path, opts) => new Promise((res, rej) =>
    fs.readFile(path, opts, (err, data) => err ? rej(err) : res(data)));
fs.promises.writeFile = (path, data, opts) => new Promise((res, rej) =>
    fs.writeFile(path, data, opts, (err) => err ? rej(err) : res()));
fs.promises.readdir = (path, opts) => new Promise((res, rej) =>
    fs.readdir(path, opts, (err, entries) => err ? rej(err) : res(entries)));
fs.promises.stat = (path) => new Promise((res, rej) =>
    fs.stat(path, (err, s) => err ? rej(err) : res(s)));
fs.promises.unlink = (path) => new Promise((res, rej) =>
    fs.unlink(path, (err) => err ? rej(err) : res()));
fs.promises.mkdir = (path, opts) => new Promise((res, rej) =>
    fs.mkdir(path, opts, (err) => err ? rej(err) : res()));

// ── Override process.cwd / process.chdir (opt-in) ────────────
if (VIRTUAL_CWD_ENABLED) {
    process.cwd = function () {
        const h = hosts[activeHost];
        if (h && h.cwd) return h.cwd;
        return origProcess.cwd();
    };
    process.chdir = function (path) {
        const h = hosts[activeHost];
        if (!h) throw new Error('unknown host');
        if (h.kind === 'local') { origProcess.chdir(path); h.cwd = origProcess.cwd(); return; }
        h.cwd = path;
    };
}

// ── process.platform override (opt-in) ───────────────────────
if (VIRTUAL_PLATFORM_ENABLED) {
    Object.defineProperty(process, 'platform', {
        get() {
            const h = hosts[activeHost];
            return (h && h.platform) || 'darwin';
        },
        configurable: true,
    });
}

// ── path module active-host dispatch ─────────────────────────
// path.resolve / join / dirname / basename / isAbsolute / normalize / relative
// dispatch to path.win32.* or path.posix.* based on the active host's platform.
// path.sep and path.delimiter become getters. The module object identity is
// preserved so `const path = require('path')` still works everywhere.
//
// Gotcha: on a Mac Node, `path.resolve === path.posix.resolve`, so if we
// replace `path.resolve` in place, `path.posix.resolve` also gets replaced —
// leading to infinite recursion when we dispatch back through it. Capture the
// original platform-specific methods into local refs first.
if (VIRTUAL_PATH_ENABLED) (function patchPath() {
    const path = require('path');
    const win32 = {}, posix = {};
    const methods = ['resolve', 'join', 'dirname', 'basename', 'extname',
                     'isAbsolute', 'normalize', 'relative', 'parse', 'format',
                     'toNamespacedPath'];
    // Snapshot original methods from each namespace into local refs
    for (const m of methods) {
        if (typeof path.win32[m] === 'function') win32[m] = path.win32[m];
        if (typeof path.posix[m] === 'function') posix[m] = path.posix[m];
    }
    const win32Sep = path.win32.sep, posixSep = path.posix.sep;
    const win32Delim = path.win32.delimiter, posixDelim = path.posix.delimiter;

    function pickSet() {
        const h = hosts[activeHost];
        return (h && h.platform === 'win32') ? win32 : posix;
    }
    for (const m of methods) {
        if (typeof path[m] !== 'function') continue;
        path[m] = function (...args) { return pickSet()[m](...args); };
    }
    Object.defineProperty(path, 'sep', {
        get() { const h = hosts[activeHost]; return (h && h.platform === 'win32') ? win32Sep : posixSep; },
        configurable: true,
    });
    Object.defineProperty(path, 'delimiter', {
        get() { const h = hosts[activeHost]; return (h && h.platform === 'win32') ? win32Delim : posixDelim; },
        configurable: true,
    });
})();

// ── process.env virtualization (Phase 6) ─────────────────────
// Reads dispatch to active host's env. Writes mutate the active host's env
// cache AND local Node's real env (so we don't silently lose values).
// HOME stays overridden to /cloud/dumbterm/home regardless of host.
hosts.local.env = Object.assign({}, process.env);
const realEnv = process.env; // original — we keep writing here for child spawn
function envForHost() {
    const h = hosts[activeHost];
    return (h && h.env) || realEnv;
}
if (VIRTUAL_ENV_ENABLED) (function patchEnv() {
    const envProxy = new Proxy({}, {
        get(_, key) {
            if (key === 'HOME' && VIRTUAL_HOME_ENABLED) return CLOUD_HOME;
            const e = envForHost();
            if (key in e) return e[key];
            return realEnv[key];
        },
        set(_, key, val) {
            const h = hosts[activeHost];
            if (h && h.env) h.env[key] = String(val);
            realEnv[key] = String(val);
            return true;
        },
        deleteProperty(_, key) {
            const h = hosts[activeHost];
            if (h && h.env) delete h.env[key];
            delete realEnv[key];
            return true;
        },
        has(_, key) {
            const e = envForHost();
            return key in e || key in realEnv;
        },
        ownKeys() {
            const e = envForHost();
            const set = new Set([...Object.keys(e), ...Object.keys(realEnv)]);
            return [...set];
        },
        getOwnPropertyDescriptor(_, key) {
            const val = (key === 'HOME') ? CLOUD_HOME
                : (key in envForHost() ? envForHost()[key] : realEnv[key]);
            if (val === undefined) return undefined;
            return { value: val, writable: true, enumerable: true, configurable: true };
        },
    });
    try {
        Object.defineProperty(process, 'env', { value: envProxy, writable: true, configurable: true });
    } catch (e) {
        // defineProperty may fail on some Node versions — fall back to merging
        try { Object.assign(process.env, envProxy); } catch (e2) {}
    }
})();

// ── Kick off async host_info fetch for the default remote ────
if (process.env.DUMBTERM_FLOWTO) {
    refreshHostInfo(defaultRemote, (err) => {
        if (err) process.stderr.write('flowto: host_info failed: ' + err.message + '\n');
        else writeContextFile();
    });
} else {
    writeContextFile();
}

// ── Phase 13: write FLOWTO_CONTEXT.md so Claude Code can discover the setup ───
// Strategy:
//   FLOWTO_CONTEXT_PATH env var specifies a file to update. If unset, we
//   write a standalone file at /tmp/flowto-context.md.
//
//   For files that may be user-edited (e.g. a project's CLAUDE.md), the user
//   points FLOWTO_CONTEXT_PATH at that file and we update ONLY the region
//   between two sentinel markers. Content outside the markers is preserved.
//   Running the shim repeatedly replaces only the managed block.
const FLOWTO_BEGIN = '<!-- ⚠️  BEGIN FLOWTO AUTO-INJECTED BLOCK  ⚠️ -->';
const FLOWTO_END   = '<!-- ⚠️  END FLOWTO AUTO-INJECTED BLOCK  ⚠️ -->';
const FLOWTO_WARNING = [
    '',
    '> **⚠️  IF YOU ARE EDITING THIS FILE:**',
    '>',
    '> Everything between the `BEGIN FLOWTO AUTO-INJECTED BLOCK` marker and the',
    '> `END FLOWTO AUTO-INJECTED BLOCK` marker is **regenerated on every flowto',
    '> session start**. Anything you write inside that block **WILL BE LOST**.',
    '>',
    '> To add your own notes: **write them OUTSIDE the markers** (above the BEGIN',
    '> line or below the END line). The flowto shim only replaces content between',
    '> the markers and preserves everything else.',
    '>',
    '> If this file has been edited by mistake and now has duplicate blocks,',
    '> delete all FLOWTO blocks — the shim will regenerate a clean one next run.',
    '',
].join('\n');

function writeContextFile() {
    if (cfgBool('DUMBTERM_SKIP_CONTEXT', 'skipContext')) return;
    const fsSync = require('fs');
    const lines = [];
    lines.push(FLOWTO_BEGIN);
    lines.push(FLOWTO_WARNING);
    lines.push('# flowto session context');
    lines.push('');
    lines.push('This Node process is running under **flowto** — a transparent tool-execution proxy.');
    lines.push('Your `Bash` tool (via `child_process.exec` / `spawn`) and `fs.*` calls may route');
    lines.push('to a remote agent instead of the local machine.');
    lines.push('');
    lines.push('## Active configuration');
    lines.push('');
    lines.push('- **Active host**: `' + activeHost + '`');
    if (projectCfgPath) lines.push('- **Project profile**: `' + projectCfgPath + '`');
    if (VIRTUAL_FS_ENABLED) lines.push('- **fs virtualization**: ON (non-local fs routes to `' + activeHost + '`)');
    if (VIRTUAL_CWD_ENABLED) lines.push('- **cwd virtualization**: ON');
    if (VIRTUAL_PLATFORM_ENABLED) lines.push('- **process.platform virtualization**: ON');
    if (VIRTUAL_PATH_ENABLED) lines.push('- **path module virtualization**: ON');
    if (VIRTUAL_ENV_ENABLED) lines.push('- **process.env virtualization**: ON');
    if (VIRTUAL_HOME_ENABLED) lines.push('- **HOME redirected to `/cloud/dumbterm/home`**');
    if (pathMap.length) {
        lines.push('- **Path mappings**:');
        for (const [l, r] of pathMap) lines.push('  - `' + l + '` → `' + r + '`');
    }
    if (hosts[activeHost] && hosts[activeHost].shell) {
        lines.push('- **Remote shell**: `' + hosts[activeHost].shell + '`');
    }
    lines.push('');
    lines.push('## Meta commands available via Bash');
    lines.push('');
    lines.push('- `dumbterm-host list` — show registered hosts, mark active');
    lines.push('- `dumbterm-host <name>` — switch active host (e.g. `dumbterm-host local`)');
    lines.push('- `dumbterm-at <host> <cmd>` — one-shot run on a specific host without switching');
    lines.push('');
    lines.push('## Notes for tool use');
    lines.push('');
    if (hosts[activeHost] && hosts[activeHost].shell && hosts[activeHost].shell.indexOf('bash') >= 0) {
        lines.push('- The remote shell is bash (msys/MinGW on Windows). Use POSIX commands (`ls`,');
        lines.push('  `cat`, `grep`, `find`, `git`, `gcc`). Avoid cmd.exe builtins (`ver`, `dir`).');
    }
    if (pathMap.length) {
        lines.push('- You may use either local or remote paths; mapped prefixes are auto-translated.');
    }
    lines.push('');
    lines.push('## More info');
    lines.push('');
    lines.push('- **flowto source**: https://github.com/williamsharkey/dumbterm');
    lines.push('- **Protocol**: newline-delimited JSON over TCP; base64 for binary payloads');
    lines.push('- **Shim**: `flowto_shim.js` injected via `--require` before this process starts');
    lines.push('');
    lines.push(FLOWTO_END);
    const managed = lines.join('\n');

    const outPath = process.env.FLOWTO_CONTEXT_PATH || '/tmp/flowto-context.md';
    try {
        let existing = '';
        try { existing = fsSync.readFileSync(outPath, 'utf8'); } catch (e) { /* new file */ }

        // Strip ALL existing auto-injected blocks (handles duplicates from bad edits).
        // Regex matches any block between our markers, including the markers themselves.
        // Using indexOf loops (not regex) so users can safely include the markers in
        // discussions elsewhere without triggering accidental match.
        let cleaned = existing;
        for (;;) {
            const b = cleaned.indexOf(FLOWTO_BEGIN);
            if (b < 0) break;
            const e = cleaned.indexOf(FLOWTO_END, b + FLOWTO_BEGIN.length);
            if (e < 0) break; // malformed: leave as-is
            const before = cleaned.slice(0, b).replace(/\n+$/, '');
            const after = cleaned.slice(e + FLOWTO_END.length).replace(/^\n+/, '');
            cleaned = before + (before && after ? '\n\n' : '') + after;
        }

        let next;
        if (cleaned.length > 0) {
            next = cleaned.replace(/\n+$/, '') + '\n\n' + managed + '\n';
        } else {
            next = managed + '\n';
        }
        fsSync.writeFileSync(outPath, next);
        process.env.FLOWTO_CONTEXT = outPath;
    } catch (e) {
        // best effort; don't fail the shim
    }
}

// Expand the meta-command to refresh host info on first switch
const origMaybeMeta = maybeMetaCommand;
// (already declared above — we rely on the original)

// ── Expose shim state for tests ──────────────────────────────
global.__flowto = {
    get activeHost() { return activeHost; },
    set activeHost(v) { activeHost = v; },
    hosts,
    rpcCall,
    refreshHostInfo,
    CLOUD_HOME,
    isCloudPath,
    routeForPath,
    translateCloudPath,
};
