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

const gw = process.env.DUMBTERM_GATEWAY;
if (!gw) { return; /* not running under --flowto; stay inert */ }

const [gwHost, gwPortStr] = gw.split(':');
const gwPort = parseInt(gwPortStr);

// ── Host registry ────────────────────────────────────────────
const hosts = { local: { kind: 'local' } };
const defaultRemote = process.env.DUMBTERM_REMOTE_NAME || 'remote';
if (process.env.DUMBTERM_FLOWTO) {
    hosts[defaultRemote] = { kind: 'remote' };
}
let activeHost = process.env.DUMBTERM_FLOWTO ? defaultRemote : 'local';

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

function connectGateway(cb) {
    if (_conn) return cb(null);
    _conn = net.createConnection(gwPort, gwHost, () => cb(null));
    _conn.on('data', (chunk) => {
        _connBuf += chunk.toString('utf8');
        let nl;
        while ((nl = _connBuf.indexOf('\n')) >= 0) {
            const line = _connBuf.slice(0, nl);
            _connBuf = _connBuf.slice(nl + 1);
            try {
                const msg = JSON.parse(line);
                const p = _pending.get(msg.id);
                if (p) { _pending.delete(msg.id); p(null, msg); }
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
        _conn.write(JSON.stringify(req) + '\n');
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

// ── Wrap child_process.exec ──────────────────────────────────
const origExec = child_process.exec;
child_process.exec = function (command, options, callback) {
    if (typeof options === 'function') { callback = options; options = {}; }
    options = options || {};

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
        // Fall through to the real Node implementation
        return origExec.call(this, useCmd, options, callback);
    }

    // Remote: RPC to the gateway
    rpcCall({ op: 'exec', host: useHost, cmd: useCmd, cwd: options.cwd || process.cwd() }, (err, resp) => {
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

// ── Expose shim state for tests ──────────────────────────────
global.__flowto = {
    get activeHost() { return activeHost; },
    set activeHost(v) { activeHost = v; },
    hosts,
    rpcCall,
};
