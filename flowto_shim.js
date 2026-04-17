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

// ── /cloud/... persistent-home path scheme ───────────────────
// Any path under /cloud/ routes to the persistent host regardless of
// active host. Default persistent host = defaultRemote (the --flowto target).
const PERSISTENT_HOST = process.env.DUMBTERM_PERSISTENT_HOST || defaultRemote;
const CLOUD_PREFIX = '/cloud/dumbterm/';
const CLOUD_HOME = '/cloud/dumbterm/home';
const CLOUD_BACKING = process.env.DUMBTERM_CLOUD_BACKING || 'C:\\workspace\\dumbterm-cloud';

function isCloudPath(p) {
    return typeof p === 'string' && p.startsWith('/cloud/');
}
// Map /cloud/dumbterm/home/foo → CLOUD_BACKING + /foo (platform-translated)
function translateCloudPath(p) {
    if (!p.startsWith(CLOUD_PREFIX)) return p;
    const rel = p.slice(CLOUD_PREFIX.length);   // "home/foo"
    // naive join — agent will open it as-is
    return CLOUD_BACKING + (CLOUD_BACKING.includes('\\') ? '\\' : '/') + rel.replace(/\//g, CLOUD_BACKING.includes('\\') ? '\\' : '/');
}

// Which host should a path go to?
function routeForPath(p) {
    if (isCloudPath(p)) return PERSISTENT_HOST;
    return activeHost;
}

// ── Override HOME / os.homedir ───────────────────────────────
process.env.HOME = CLOUD_HOME;
const os = require('os');
const origHomedir = os.homedir;
os.homedir = () => CLOUD_HOME;
os.userInfo = (function(orig) {
    return function(opts) {
        const r = orig.call(this, opts);
        r.homedir = CLOUD_HOME;
        return r;
    };
})(os.userInfo);

// ── Intercept fs.* ───────────────────────────────────────────
const fs = require('fs');

function rpcToHost(host, req, cb) {
    if (host === 'local') return null; // caller must handle
    req.host = host;
    rpcCall(req, cb);
    return true;
}

// fs.readFile(path, [opts], cb)
const origReadFile = fs.readFile;
fs.readFile = function (path, opts, cb) {
    if (typeof opts === 'function') { cb = opts; opts = undefined; }
    const host = routeForPath(path);
    if (host === 'local') {
        return origReadFile.call(this, path, opts, cb);
    }
    const remotePath = isCloudPath(path) ? translateCloudPath(path) : path;
    rpcCall({ op: 'read', host, path: remotePath }, (err, resp) => {
        if (err) return cb && cb(err);
        if (!resp.ok) {
            const e = new Error(resp.err || resp.errno || 'ENOENT');
            e.code = resp.errno || 'ENOENT'; e.path = path;
            return cb && cb(e);
        }
        const buf = Buffer.from(resp.data || '', 'base64');
        if (opts && (opts === 'utf8' || opts.encoding === 'utf8')) return cb && cb(null, buf.toString('utf8'));
        cb && cb(null, buf);
    });
};

// fs.writeFile(path, data, [opts], cb)
const origWriteFile = fs.writeFile;
fs.writeFile = function (path, data, opts, cb) {
    if (typeof opts === 'function') { cb = opts; opts = undefined; }
    const host = routeForPath(path);
    if (host === 'local') {
        return origWriteFile.call(this, path, data, opts, cb);
    }
    const remotePath = isCloudPath(path) ? translateCloudPath(path) : path;
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
    if (host === 'local') return origReaddir.call(this, path, opts, cb);
    const remotePath = isCloudPath(path) ? translateCloudPath(path) : path;
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
    if (host === 'local') return origStat.call(this, path, opts, cb);
    const remotePath = isCloudPath(path) ? translateCloudPath(path) : path;
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
    const remotePath = isCloudPath(path) ? translateCloudPath(path) : path;
    rpcCall({ op: 'unlink', host, path: remotePath }, (err, resp) => {
        if (err) return cb && cb(err);
        if (!resp.ok) { const e = new Error(resp.errno || 'EIO'); e.code = resp.errno || 'EIO'; return cb && cb(e); }
        cb && cb(null);
    });
};

// fs.mkdir
const origMkdir = fs.mkdir;
fs.mkdir = function (path, opts, cb) {
    if (typeof opts === 'function') { cb = opts; opts = undefined; }
    const host = routeForPath(path);
    if (host === 'local') return origMkdir.call(this, path, opts, cb);
    const remotePath = isCloudPath(path) ? translateCloudPath(path) : path;
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

// ── Expose shim state for tests ──────────────────────────────
global.__flowto = {
    get activeHost() { return activeHost; },
    set activeHost(v) { activeHost = v; },
    hosts,
    rpcCall,
    CLOUD_HOME,
    isCloudPath,
    routeForPath,
    translateCloudPath,
};
