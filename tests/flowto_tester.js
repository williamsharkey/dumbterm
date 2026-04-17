// flowto_tester.js — visual + programmatic test harness for flowto.
//
// Run:
//   dumbterm --flowto HOST:PORT -- node tests/flowto_tester.js
//   dumbterm --flowto HOST:PORT -- node tests/flowto_tester.js --interactive
//   DUMBTERM_TRACE_LOG=/tmp/trace.jsonl dumbterm --flowto ... -- node tests/flowto_tester.js
//
// TUI goes to stdout (watch it in your dumbterm window).
// Trace goes to DUMBTERM_TRACE_LOG file (or stderr if unset).
// Exit code = number of failed tests.
//
// Zero deps — pure ANSI.

const fs = require('fs');
const os = require('os');
const cp = require('child_process');

// ── ANSI helpers ─────────────────────────────────────────────
const ESC = '\x1b';
const AN = {
    clear:    ESC + '[2J' + ESC + '[H',
    hideCur:  ESC + '[?25l',
    showCur:  ESC + '[?25h',
    up:       (n) => ESC + '[' + n + 'A',
    move:     (r, c) => ESC + '[' + r + ';' + c + 'H',
    eraseLine:ESC + '[2K',
    bold:     ESC + '[1m',
    dim:      ESC + '[2m',
    reset:    ESC + '[0m',
    fg: {
        gray:   ESC + '[90m',
        red:    ESC + '[31m',
        green:  ESC + '[32m',
        yellow: ESC + '[33m',
        blue:   ESC + '[34m',
        cyan:   ESC + '[36m',
        amber:  ESC + '[38;2;255;191;0m',
    },
};

// ── Trace log (JSON lines) ──────────────────────────────────
const traceFile = process.env.DUMBTERM_TRACE_LOG;
let traceStream = null;
if (traceFile) {
    traceStream = fs.createWriteStream(traceFile, { flags: 'w' });
}
function trace(t, data) {
    const msg = JSON.stringify(Object.assign({ t, ts: Date.now() }, data)) + '\n';
    if (traceStream) traceStream.write(msg);
    else process.stderr.write(msg);
}

// ── Test registry ────────────────────────────────────────────
const tests = [];
function test(name, fn) { tests.push({ name, fn, status: 'pending', detail: '' }); }

// ── TUI rendering ────────────────────────────────────────────
const W = 90; // fixed width
function pad(s, n) { return (s + ' '.repeat(n)).slice(0, n); }
function statusBadge(s) {
    switch (s) {
        case 'pending': return AN.fg.gray + '[ ... ]' + AN.reset;
        case 'running': return AN.fg.yellow + '[ >>> ]' + AN.reset;
        case 'pass':    return AN.fg.green + '[  ✓  ]' + AN.reset;
        case 'fail':    return AN.fg.red + '[  ✗  ]' + AN.reset;
    }
}
let currentStatus = '';
let currentDetail = '';

function draw() {
    let out = AN.clear;
    out += AN.bold + AN.fg.amber + 'flowto tester' + AN.reset + AN.dim + '  —  dumbterm Phase 1+2 integration test\n' + AN.reset;
    out += AN.dim + '─'.repeat(W) + AN.reset + '\n';
    let passed = 0, failed = 0, done = 0;
    for (let i = 0; i < tests.length; i++) {
        const t = tests[i];
        if (t.status === 'pass') passed++;
        if (t.status === 'fail') failed++;
        if (t.status !== 'pending' && t.status !== 'running') done++;
        const label = pad((i+1).toString().padStart(2) + '. ' + t.name, W - 10);
        out += statusBadge(t.status) + ' ' + label + '\n';
        if (t.detail) {
            out += '         ' + AN.dim + t.detail.slice(0, W - 10) + AN.reset + '\n';
        }
    }
    out += AN.dim + '─'.repeat(W) + AN.reset + '\n';
    out += AN.fg.green + ' passed: ' + passed + AN.reset;
    out += AN.fg.red + '  failed: ' + failed + AN.reset;
    out += AN.fg.gray + '  remaining: ' + (tests.length - done) + AN.reset;
    out += '\n';
    if (currentStatus) out += AN.fg.cyan + currentStatus + AN.reset + '\n';
    if (currentDetail) out += AN.fg.gray + '    ' + currentDetail + AN.reset + '\n';
    process.stdout.write(out);
}

// ── Test helpers ─────────────────────────────────────────────
function exec(cmd) {
    return new Promise((resolve) => {
        trace('exec_start', { cmd });
        cp.exec(cmd, (err, out, errstr) => {
            trace('exec_done', { cmd, exit: err ? err.code : 0, out, err: errstr });
            resolve({ err, out, errstr });
        });
    });
}
async function writeFile(path, data) {
    trace('fs_write_start', { path, bytes: Buffer.byteLength(data) });
    await fs.promises.writeFile(path, data);
    trace('fs_write_done', { path });
}
async function readFile(path, enc) {
    trace('fs_read_start', { path });
    const d = await fs.promises.readFile(path, enc);
    trace('fs_read_done', { path, bytes: typeof d === 'string' ? d.length : d.length });
    return d;
}
async function stat(path) {
    trace('fs_stat_start', { path });
    const s = await fs.promises.stat(path);
    trace('fs_stat_done', { path, size: s.size, isFile: s.isFile(), isDirectory: s.isDirectory() });
    return s;
}
async function unlink(path) {
    trace('fs_unlink', { path });
    try { await fs.promises.unlink(path); } catch (e) { /* ok if missing */ }
}

// ── Tests ────────────────────────────────────────────────────

test('exec on active host returns stdout', async () => {
    const r = await exec('node -e "console.log(\'hello-from-host\')"');
    if (r.out.trim() !== 'hello-from-host') throw new Error(`out=${JSON.stringify(r.out)}`);
    return 'received: ' + r.out.trim();
});

test('detect remote host platform', async () => {
    const r = await exec('node -e "console.log(process.platform)"');
    const plat = r.out.trim();
    if (!plat) throw new Error('no platform reported');
    return 'remote platform: ' + plat;
});

test('dumbterm-host list shows registry', async () => {
    const r = await exec('dumbterm-host list');
    if (!r.out.includes('* remote')) throw new Error(`no active marker: ${JSON.stringify(r.out)}`);
    return r.out.trim().replace(/\n/g, ' | ');
});

test('switch to local — subsequent exec routes locally', async () => {
    await exec('dumbterm-host local');
    const r = await exec('node -e "console.log(process.platform)"');
    return 'local platform: ' + r.out.trim();
});

test('switch back to remote', async () => {
    const r = await exec('dumbterm-host remote');
    if (!r.out.includes('active host: remote')) throw new Error(r.out);
    return 'active: remote';
});

test('dumbterm-at local — one-shot, does not change active', async () => {
    const r = await exec('dumbterm-at local node -e "console.log(42)"');
    if (r.out.trim() !== '42') throw new Error(JSON.stringify(r.out));
    const r2 = await exec('dumbterm-host list');
    if (!r2.out.includes('* remote')) throw new Error('active host changed: ' + r2.out);
    return 'one-shot returned 42, active still remote';
});

test('os.homedir is /cloud/dumbterm/home', async () => {
    const h = os.homedir();
    if (h !== '/cloud/dumbterm/home') throw new Error(h);
    return h;
});

test('process.env.HOME is /cloud/dumbterm/home', async () => {
    if (process.env.HOME !== '/cloud/dumbterm/home') throw new Error(process.env.HOME);
    return process.env.HOME;
});

test('writeFile then readFile on /cloud/ path', async () => {
    const p = '/cloud/dumbterm/tester_smoke.txt';
    await writeFile(p, 'hello tester\n');
    const back = await readFile(p, 'utf8');
    if (back !== 'hello tester\n') throw new Error(JSON.stringify(back));
    return back.trim();
});

test('cloud file survives host switch to local', async () => {
    const p = '/cloud/dumbterm/tester_survive.txt';
    await writeFile(p, 'still here\n');
    await exec('dumbterm-host local');
    const back = await readFile(p, 'utf8');
    await exec('dumbterm-host remote');
    if (back !== 'still here\n') throw new Error(JSON.stringify(back));
    return 'persistence works across hosts';
});

test('readdir lists cloud directory', async () => {
    const entries = await fs.promises.readdir('/cloud/dumbterm/');
    if (!entries.some(e => e.includes('tester_'))) throw new Error('no tester_ entries: ' + entries.join(','));
    return `${entries.length} entries, incl: ${entries.filter(e => e.startsWith('tester_')).join(', ')}`;
});

test('stat reports correct size', async () => {
    const p = '/cloud/dumbterm/tester_sized.txt';
    const data = 'x'.repeat(100);
    await writeFile(p, data);
    const s = await stat(p);
    if (s.size !== 100) throw new Error(`expected 100, got ${s.size}`);
    if (!s.isFile()) throw new Error('not isFile');
    await unlink(p);
    return `size=${s.size}, isFile=${s.isFile()}`;
});

test('binary 256-byte round-trip', async () => {
    const p = '/cloud/dumbterm/tester_bin.dat';
    const bytes = Buffer.alloc(256);
    for (let i = 0; i < 256; i++) bytes[i] = i;
    await writeFile(p, bytes);
    const back = await readFile(p);
    if (back.length !== 256) throw new Error(`len=${back.length}`);
    for (let i = 0; i < 256; i++) if (back[i] !== i) throw new Error(`byte ${i} mismatch: ${back[i]}`);
    await unlink(p);
    return 'all 256 bytes intact';
});

test('ENOENT propagates on missing cloud file', async () => {
    try {
        await readFile('/cloud/dumbterm/does_not_exist_xyz.file', 'utf8');
        throw new Error('expected ENOENT');
    } catch (e) {
        if (e.code !== 'ENOENT') throw new Error('wrong code: ' + e.code);
        return 'code=' + e.code;
    }
});

test('unknown host name returns error', async () => {
    const r = await exec('dumbterm-host xyzzy');
    if (!r.err || r.err.code !== 1) throw new Error(`no err: ${r.err?.code}`);
    return 'err.code=1';
});

test('exit code 42 propagates', async () => {
    const r = await exec('node -e "process.exit(42)"');
    if (!r.err || r.err.code !== 42) throw new Error(`got ${r.err?.code}`);
    return 'exit 42 propagated';
});

test('stderr captured separately from stdout', async () => {
    const r = await exec('node -e "console.log(\'OUT\'); console.error(\'ERR\')"');
    if (!r.out.includes('OUT')) throw new Error('no OUT: ' + r.out);
    if (!r.errstr.includes('ERR')) throw new Error('no ERR: ' + r.errstr);
    return 'out=OUT, err=ERR';
});

// Cleanup tests
test('cleanup cloud test files', async () => {
    await unlink('/cloud/dumbterm/tester_smoke.txt');
    await unlink('/cloud/dumbterm/tester_survive.txt');
    return 'cleaned';
});

// ── Runner ───────────────────────────────────────────────────
const INTERACTIVE = process.argv.includes('--interactive');

async function waitKey() {
    return new Promise((res) => {
        process.stdin.setRawMode && process.stdin.setRawMode(true);
        process.stdin.resume();
        process.stdin.once('data', () => {
            process.stdin.setRawMode && process.stdin.setRawMode(false);
            process.stdin.pause();
            res();
        });
    });
}

async function main() {
    process.stdout.write(AN.hideCur);
    trace('run_start', { count: tests.length, interactive: INTERACTIVE });
    draw();

    for (let i = 0; i < tests.length; i++) {
        const t = tests[i];
        t.status = 'running';
        currentStatus = `▶ running: ${t.name}`;
        currentDetail = '';
        draw();
        trace('test_start', { i, name: t.name });
        try {
            const result = await t.fn();
            t.status = 'pass';
            t.detail = result || '';
            trace('test_pass', { i, name: t.name, result });
        } catch (e) {
            t.status = 'fail';
            t.detail = e.message || String(e);
            trace('test_fail', { i, name: t.name, error: e.message, stack: e.stack });
        }
        currentStatus = '';
        currentDetail = '';
        draw();
        if (INTERACTIVE) {
            process.stdout.write(AN.fg.cyan + '  press any key for next test…' + AN.reset);
            await waitKey();
        }
    }

    const failed = tests.filter(t => t.status === 'fail').length;
    trace('run_done', { pass: tests.length - failed, fail: failed });
    process.stdout.write(AN.showCur);
    process.stdout.write('\n');
    if (traceStream) traceStream.end();
    process.exit(failed);
}

main().catch(e => {
    trace('crash', { error: e.message, stack: e.stack });
    process.stdout.write(AN.showCur);
    console.error(e);
    process.exit(99);
});
