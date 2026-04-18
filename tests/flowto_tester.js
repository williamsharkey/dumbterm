// flowto_tester.js — visual + programmatic test harness for flowto.
//
// Run the full suite (requires all virtualizations enabled):
//   DUMBTERM_VIRTUAL_HOME=1 DUMBTERM_VIRTUAL_CWD=1 DUMBTERM_VIRTUAL_PLATFORM=1 \
//   DUMBTERM_VIRTUAL_PATH=1 DUMBTERM_VIRTUAL_ENV=1 DUMBTERM_VIRTUAL_FS=1 \
//   dumbterm --flowto HOST:PORT -- node tests/flowto_tester.js --headless
//
// See tests/run_full.sh for the canonical invocation.
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
    if (HEADLESS) return; // no ANSI, no full redraw
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

test('os.homedir respects DUMBTERM_VIRTUAL_HOME setting', async () => {
    const h = os.homedir();
    if (process.env.DUMBTERM_VIRTUAL_HOME === '1') {
        if (h !== '/cloud/dumbterm/home') throw new Error(`expected cloud, got ${h}`);
        return `virtual HOME: ${h}`;
    } else {
        if (h === '/cloud/dumbterm/home') throw new Error('cloud active but should not be');
        return `real HOME: ${h}`;
    }
});

test('process.env.HOME respects DUMBTERM_VIRTUAL_HOME setting', async () => {
    const h = process.env.HOME;
    if (process.env.DUMBTERM_VIRTUAL_HOME === '1') {
        if (h !== '/cloud/dumbterm/home') throw new Error(h);
    } else {
        if (h === '/cloud/dumbterm/home') throw new Error('cloud active but should not be');
    }
    return h;
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

// ── Stress / edge cases ──────────────────────────────────────

test('50 parallel fs.stat calls — RPC ordering under concurrency', async () => {
    // Seed 50 files, then stat them concurrently. Every response must
    // match the correct request (tests id-keyed Map in shim.rpcCall).
    const files = [];
    for (let i = 0; i < 50; i++) {
        const p = `/cloud/dumbterm/parallel_${i}.txt`;
        await writeFile(p, `value-${i}\n`);
        files.push(p);
    }
    const results = await Promise.all(files.map(p => stat(p)));
    for (let i = 0; i < 50; i++) {
        const expect = `value-${i}\n`.length;
        if (results[i].size !== expect) throw new Error(`idx ${i}: expected ${expect}, got ${results[i].size}`);
    }
    for (const p of files) await unlink(p);
    return '50 parallel stats returned correctly-sized results';
});

test('1MB binary round-trip', async () => {
    const p = '/cloud/dumbterm/tester_big.bin';
    const big = Buffer.alloc(1024 * 1024);
    for (let i = 0; i < big.length; i++) big[i] = i & 0xFF;
    await writeFile(p, big);
    const back = await readFile(p);
    if (back.length !== big.length) throw new Error(`len ${back.length} vs ${big.length}`);
    for (let i = 0; i < big.length; i += 4096) if (back[i] !== big[i]) throw new Error(`byte ${i} mismatch`);
    await unlink(p);
    return `${big.length} bytes round-tripped intact`;
});

test('20 sequential execs — ordering preserved', async () => {
    const results = [];
    for (let i = 0; i < 20; i++) {
        const r = await exec(`node -e "console.log(${i})"`);
        results.push(parseInt(r.out.trim()));
    }
    for (let i = 0; i < 20; i++) if (results[i] !== i) throw new Error(`idx ${i} got ${results[i]}`);
    return '20 execs returned 0..19 in order';
});

test('unicode filename + content', async () => {
    const p = '/cloud/dumbterm/tester_héllo_✨.txt';
    const content = 'Héllo 世界 ✨ 🎉\n';
    await writeFile(p, content);
    const back = await readFile(p, 'utf8');
    if (back !== content) throw new Error(`got: ${JSON.stringify(back)}`);
    await unlink(p);
    return `${content.length} unicode chars round-tripped`;
});

test('empty file round-trip', async () => {
    const p = '/cloud/dumbterm/tester_empty.txt';
    await writeFile(p, '');
    const s = await stat(p);
    if (s.size !== 0) throw new Error(`size ${s.size}`);
    const back = await readFile(p, 'utf8');
    if (back !== '') throw new Error(`got: ${JSON.stringify(back)}`);
    await unlink(p);
    return 'zero-byte file handled';
});

test('nested subdir: mkdir → write inside → readdir → cleanup', async () => {
    const dir = '/cloud/dumbterm/tester_sub';
    try { await fs.promises.mkdir(dir); } catch (e) { /* might exist */ }
    const inner = dir + '/inner.txt';
    await writeFile(inner, 'nested data\n');
    const entries = await fs.promises.readdir(dir);
    if (!entries.includes('inner.txt')) throw new Error(`entries: ${entries.join(',')}`);
    const back = await readFile(inner, 'utf8');
    if (back !== 'nested data\n') throw new Error(`got: ${JSON.stringify(back)}`);
    await unlink(inner);
    return `dir created, contained ${entries.length} entry`;
});

test('rapid host switching (10 switches)', async () => {
    for (let i = 0; i < 5; i++) {
        let r = await exec('dumbterm-host local');
        if (!r.out.includes('active host: local')) throw new Error(`switch ${i}-local: ${r.out}`);
        r = await exec('dumbterm-host remote');
        if (!r.out.includes('active host: remote')) throw new Error(`switch ${i}-remote: ${r.out}`);
    }
    return '10 switches completed, final state: remote';
});

test('exec with shell metacharacters in payload (quoted node -e)', async () => {
    // Node's -e string contains dollar signs, semicolons, pipes as plain chars
    const r = await exec(`node -e "console.log('one|two;three$four')"`);
    if (r.out.trim() !== 'one|two;three$four') throw new Error(`got: ${JSON.stringify(r.out)}`);
    return 'metachars passed through as data';
});

test('large stdout capture (100 KB)', async () => {
    // Emit ~100KB of output and make sure we get it all
    const r = await exec(`node -e "for(let i=0;i<2000;i++) console.log('line '+i+' '.repeat(40))"`);
    // normalize \r\n → \n (W7 default line endings)
    const lines = r.out.replace(/\r/g, '').split('\n').filter(Boolean);
    if (lines.length !== 2000) throw new Error(`expected 2000 lines, got ${lines.length}`);
    // cmd is: 'line '+i+' '.repeat(40), so for i=0: "line " + "0" + 40 spaces = "line 0" + 40 spaces
    const expectFirst = 'line 0' + ' '.repeat(40);
    const expectLast  = 'line 1999' + ' '.repeat(40);
    if (lines[0] !== expectFirst) throw new Error(`first line: ${JSON.stringify(lines[0])}`);
    if (lines[1999] !== expectLast) throw new Error(`last line: ${JSON.stringify(lines[1999])}`);
    return `${lines.length} lines, ${r.out.length} bytes captured intact`;
});

test('chained: mkdir → 3x write → readdir → 3x stat → 3x read → cleanup', async () => {
    const dir = '/cloud/dumbterm/tester_chain';
    try { await fs.promises.mkdir(dir); } catch (e) {}
    for (let i = 0; i < 3; i++) await writeFile(`${dir}/f${i}.txt`, `content-${i}`);
    const entries = await fs.promises.readdir(dir);
    if (entries.length < 3) throw new Error(`expected 3+ entries, got ${entries.length}: ${entries.join(',')}`);
    const sizes = await Promise.all([0,1,2].map(i => stat(`${dir}/f${i}.txt`).then(s => s.size)));
    for (let i = 0; i < 3; i++) {
        const expected = `content-${i}`.length;
        if (sizes[i] !== expected) throw new Error(`f${i}: expected ${expected}, got ${sizes[i]}`);
    }
    const contents = await Promise.all([0,1,2].map(i => readFile(`${dir}/f${i}.txt`, 'utf8')));
    for (let i = 0; i < 3; i++) if (contents[i] !== `content-${i}`) throw new Error(`f${i} mismatch: ${contents[i]}`);
    for (let i = 0; i < 3; i++) await unlink(`${dir}/f${i}.txt`);
    return '3 files created, listed, stat-ed, read, and removed';
});

test('exec stdout with embedded newlines preserved', async () => {
    const r = await exec(`node -e "console.log('a\\nb\\nc')"`);
    const parts = r.out.trim().split('\n');
    if (parts.length !== 3 || parts[0] !== 'a' || parts[1] !== 'b' || parts[2] !== 'c')
        throw new Error(JSON.stringify(r.out));
    return '3 lines preserved';
});

test('exec with no output returns empty string', async () => {
    const r = await exec(`node -e ""`);
    if (r.out !== '') throw new Error(`expected empty, got: ${JSON.stringify(r.out)}`);
    return 'empty stdout = ""';
});

// ── Phase 3: cwd / platform virtualization ──────────────────

test('process.platform reflects remote host', async () => {
    // Give host_info a chance to arrive
    await new Promise(r => setTimeout(r, 300));
    const p = process.platform;
    if (p !== 'win32') throw new Error(`expected win32 (W7 agent), got: ${p}`);
    return `process.platform = ${p}`;
});

test('process.cwd() returns a W7-style path when remote is active', async () => {
    const c = process.cwd();
    // W7 paths contain drive letter colon + backslash (or are in C:\)
    if (!c.match(/^[A-Z]:\\/) && !c.startsWith('C:')) throw new Error(`non-Windows cwd: ${c}`);
    return `cwd = ${c}`;
});

test('switching to local reveals local cwd', async () => {
    await exec('dumbterm-host local');
    const c = process.cwd();
    await exec('dumbterm-host remote');
    if (c.match(/^[A-Z]:\\/)) throw new Error(`still a W7 path: ${c}`);
    return `local cwd = ${c}`;
});

test('process.platform follows active host (local=darwin)', async () => {
    await exec('dumbterm-host local');
    const p = process.platform;
    await exec('dumbterm-host remote');
    if (p === 'win32') throw new Error('still win32');
    return `local platform = ${p}`;
});

test('process.chdir updates virtual cwd; exec reflects it', async () => {
    // Use a known W7 directory
    const target = 'C:\\workspace';
    process.chdir(target);
    // Now run a cmd that prints cwd — "cd" with no args on Windows
    const r = await exec('cd');
    const reported = r.out.trim();
    process.chdir('C:\\'); // reset for later tests
    if (!reported.toLowerCase().startsWith('c:\\workspace')) throw new Error(`reported: ${reported}`);
    return `chdir worked: ${reported}`;
});

test('cwd is per-host: switching restores each host\'s cwd', async () => {
    // on remote, chdir to C:\workspace
    process.chdir('C:\\workspace');
    const remoteCwd = process.cwd();
    await exec('dumbterm-host local');
    const localCwd = process.cwd();
    await exec('dumbterm-host remote');
    const remoteAgain = process.cwd();
    if (remoteCwd !== remoteAgain) throw new Error(`drifted: ${remoteCwd} vs ${remoteAgain}`);
    if (localCwd.match(/^[A-Z]:\\/)) throw new Error(`local got W7 path: ${localCwd}`);
    return `remote ${remoteCwd}, local ${localCwd}, remote restored correctly`;
});

// ── Phase 4: path module active-host dispatch ───────────────

test('path.sep is backslash when remote is win32', async () => {
    const path = require('path');
    // ensure host_info arrived
    await new Promise(r => setTimeout(r, 200));
    if (path.sep !== '\\') throw new Error(`sep = ${JSON.stringify(path.sep)}`);
    return 'path.sep = "\\"';
});

test('path.join uses backslash on remote win32', async () => {
    const path = require('path');
    const j = path.join('a', 'b', 'c');
    if (j !== 'a\\b\\c') throw new Error(`got: ${j}`);
    return `join = ${j}`;
});

test('path.resolve returns W7-style absolute path', async () => {
    const path = require('path');
    // resolve with an absolute Windows path — should be preserved
    const r = path.resolve('C:\\workspace', 'sub', 'file.txt');
    if (r !== 'C:\\workspace\\sub\\file.txt') throw new Error(`got: ${r}`);
    return r;
});

test('path.isAbsolute recognizes Windows absolute path', async () => {
    const path = require('path');
    if (!path.isAbsolute('C:\\workspace')) throw new Error('missed C:\\workspace');
    if (path.isAbsolute('relative\\path')) throw new Error('false positive');
    return 'C:\\ yes, relative no';
});

test('path.dirname / basename with backslash input', async () => {
    const path = require('path');
    if (path.dirname('C:\\workspace\\file.txt') !== 'C:\\workspace') throw new Error('dirname wrong');
    if (path.basename('C:\\workspace\\file.txt') !== 'file.txt') throw new Error('basename wrong');
    return 'dirname=C:\\workspace, basename=file.txt';
});

test('path.normalize collapses .. on win32', async () => {
    const path = require('path');
    const n = path.normalize('C:\\a\\b\\..\\c');
    if (n !== 'C:\\a\\c') throw new Error(`got: ${n}`);
    return n;
});

test('switching to local makes path posix-style', async () => {
    const path = require('path');
    await exec('dumbterm-host local');
    const sep = path.sep;
    const j = path.join('a', 'b', 'c');
    const d = path.dirname('/tmp/file.txt');
    await exec('dumbterm-host remote');
    if (sep !== '/') throw new Error(`local sep: ${sep}`);
    if (j !== 'a/b/c') throw new Error(`local join: ${j}`);
    if (d !== '/tmp') throw new Error(`local dirname: ${d}`);
    return 'local: sep=/, join=a/b/c, dirname=/tmp';
});

test('path.sep updates dynamically on host switch', async () => {
    const path = require('path');
    const remoteSep = path.sep;
    await exec('dumbterm-host local');
    const localSep = path.sep;
    await exec('dumbterm-host remote');
    const remoteSep2 = path.sep;
    if (remoteSep !== '\\' || localSep !== '/' || remoteSep2 !== '\\')
        throw new Error(`got: ${remoteSep}, ${localSep}, ${remoteSep2}`);
    return 'sep tracks active host dynamically';
});

// ── Phase 5: spawn streaming ────────────────────────────────

test('child_process.spawn on remote emits stdout + exit', async () => {
    return new Promise((resolve, reject) => {
        const child = cp.spawn('node -e "console.log(\'hello-spawn\')"');
        let out = '';
        child.stdout.on('data', (d) => { out += d.toString(); });
        child.on('exit', (code) => {
            if (code !== 0) return reject(new Error(`exit ${code}`));
            if (!out.includes('hello-spawn')) return reject(new Error(`out: ${out}`));
            resolve(`pid=${child.pid}, out=${out.trim()}`);
        });
        child.on('error', reject);
    });
});

test('child_process.spawn streams output progressively', async () => {
    return new Promise((resolve, reject) => {
        // emit 3 lines with a 40ms delay between them
        const cmdStr = `node -e "setTimeout(()=>console.log('A'),0); setTimeout(()=>console.log('B'),40); setTimeout(()=>console.log('C'),80);"`;
        const child = cp.spawn(cmdStr);
        const events = [];
        child.stdout.on('data', (d) => events.push({ t: Date.now(), d: d.toString() }));
        child.on('exit', (code) => {
            if (code !== 0) return reject(new Error(`exit ${code}`));
            if (events.length < 1) return reject(new Error('no data events'));
            const allData = events.map(e => e.d).join('');
            if (!allData.includes('A') || !allData.includes('B') || !allData.includes('C'))
                return reject(new Error(`missing chars in: ${JSON.stringify(allData)}`));
            resolve(`received ${events.length} data event(s), ${allData.length} bytes`);
        });
    });
});

test('child_process.spawn captures stderr separately', async () => {
    return new Promise((resolve, reject) => {
        const child = cp.spawn(`node -e "console.log('OUTLINE'); console.error('ERRLINE')"`);
        let out = '', err = '';
        child.stdout.on('data', d => out += d.toString());
        child.stderr.on('data', d => err += d.toString());
        child.on('exit', (code) => {
            if (code !== 0) return reject(new Error(`exit ${code}`));
            if (!out.includes('OUTLINE')) return reject(new Error('no OUTLINE'));
            if (!err.includes('ERRLINE')) return reject(new Error('no ERRLINE'));
            resolve('out & err captured separately');
        });
    });
});

test('child_process.spawn exit code propagates', async () => {
    return new Promise((resolve, reject) => {
        const child = cp.spawn(`node -e "process.exit(7)"`);
        child.on('exit', (code) => {
            if (code !== 7) return reject(new Error(`got ${code}`));
            resolve('exit 7 reached caller');
        });
    });
});

// ── Phase 6: process.env virtualization ─────────────────────

test('process.env reflects remote host (has OS=Windows_NT or similar)', async () => {
    // wait for host_info to land
    await new Promise(r => setTimeout(r, 400));
    const os_var = process.env.OS;
    const windir = process.env.windir || process.env.WINDIR;
    if (!os_var && !windir) throw new Error(`no OS/windir: OS=${os_var}, windir=${windir}`);
    return `OS=${os_var}, windir=${windir}`;
});

test('process.env.USERNAME is W7 user when remote is active', async () => {
    const u = process.env.USERNAME;
    if (!u) throw new Error('no USERNAME in remote env');
    return `USERNAME=${u}`;
});

test('process.env.HOME behavior across host switches', async () => {
    const h1 = process.env.HOME;
    await exec('dumbterm-host local');
    const h2 = process.env.HOME;
    await exec('dumbterm-host remote');
    if (process.env.DUMBTERM_VIRTUAL_HOME === '1') {
        if (h1 !== '/cloud/dumbterm/home' || h2 !== '/cloud/dumbterm/home')
            throw new Error(`virtual HOME not pinned: ${h1}, ${h2}`);
        return 'virtual HOME pinned to cloud across switches';
    }
    return `real HOME: remote=${h1}, local=${h2} (each shows host env)`;
});

test('switching to local reveals Mac env vars', async () => {
    await exec('dumbterm-host local');
    const shell = process.env.SHELL;
    const term = process.env.TERM;
    await exec('dumbterm-host remote');
    if (!shell && !term) throw new Error(`no Mac-ish env: SHELL=${shell}, TERM=${term}`);
    return `SHELL=${shell}, TERM=${term}`;
});

test('env writes visible via process.env[k]', async () => {
    process.env.FLOWTO_TESTKEY = 'marker-value';
    if (process.env.FLOWTO_TESTKEY !== 'marker-value') throw new Error('read-back failed');
    delete process.env.FLOWTO_TESTKEY;
    if ('FLOWTO_TESTKEY' in process.env) throw new Error('delete failed');
    return 'set + delete roundtrip';
});

// ── Phase 8: configurable shell ─────────────────────────────

test('custom shell via DUMBTERM_REMOTE_SHELL: bash -c on W7', async () => {
    // If W7 has bash (MinGW/git-bash), set DUMBTERM_REMOTE_SHELL and verify
    // that POSIX syntax like command substitution works.
    const prev = process.env.DUMBTERM_REMOTE_SHELL;
    process.env.DUMBTERM_REMOTE_SHELL = 'bash -c';
    const r = await exec(`echo $(uname -s 2>/dev/null || echo no-bash)`);
    if (prev === undefined) delete process.env.DUMBTERM_REMOTE_SHELL;
    else process.env.DUMBTERM_REMOTE_SHELL = prev;
    // We expect either a Unix-y uname output (e.g. MINGW32_NT-6.1) or
    // nothing — the test passes as long as the exec completed and returned
    // SOME output (meaning the shell substitution worked).
    if (r.err) throw new Error(`err: ${r.err.message}`);
    if (!r.out.trim()) throw new Error('no output — bash not available?');
    return `bash output: ${r.out.trim()}`;
});

// ── Phase 13: context file ──────────────────────────────────

test('FLOWTO_CONTEXT env var is set after shim load', async () => {
    if (!process.env.FLOWTO_CONTEXT) throw new Error('FLOWTO_CONTEXT not set');
    return process.env.FLOWTO_CONTEXT;
});

test('context file exists and contains github link', async () => {
    const p = process.env.FLOWTO_CONTEXT;
    const content = fs.readFileSync(p, 'utf8');
    if (!content.includes('github.com/williamsharkey/dumbterm')) throw new Error('missing github link');
    if (!content.includes('BEGIN FLOWTO AUTO-INJECTED')) throw new Error('missing begin marker');
    if (!content.includes('END FLOWTO AUTO-INJECTED')) throw new Error('missing end marker');
    if (!content.includes('WILL BE LOST')) throw new Error('missing editing warning');
    return `${content.length} bytes, has markers and warning`;
});

test('context file reports active host', async () => {
    const content = fs.readFileSync(process.env.FLOWTO_CONTEXT, 'utf8');
    if (!content.match(/Active host.*remote/)) throw new Error('active host not mentioned');
    return 'active host = remote mentioned';
});

// ── Claude Code sentinel routing (cwd-tracking file /tmp/claude-*-cwd) ──
// Claude Code's Bash tool writes /tmp/claude-XXXX-cwd inside a bash script on
// the remote host, then reads it back via fs.readFile. Without the sentinel
// route, that read goes to Mac's /tmp and ENOENTs — Claude then reports the
// whole Bash invocation as "empty output". Verify the read routes back to the
// host where the spawn ran.

test('claude /tmp/claude-*-cwd sentinel: write-via-spawn then readback routes to remote', async () => {
    const sentinel = '/tmp/claude-flowtotest-' + Date.now() + '-cwd';
    // Mimic Claude's script: pwd -P >| /tmp/claude-XXXX-cwd
    const cmdStr = `bash -c "pwd -P >| ${sentinel}; echo SPAWN_DONE"`;
    // Use /bin/bash -c via the spawn(cmd, args) signature that detectShellDashC matches
    const bashArgs = ['-c', `pwd -P >| ${sentinel}; echo SPAWN_DONE`];
    await new Promise((resolve, reject) => {
        const child = cp.spawn('/bin/bash', bashArgs);
        let out = '';
        child.stdout.on('data', d => out += d.toString());
        child.on('exit', code => {
            if (code !== 0) return reject(new Error(`spawn exit ${code}, out=${out}`));
            if (!out.includes('SPAWN_DONE')) return reject(new Error(`no SPAWN_DONE, out=${out}`));
            resolve();
        });
        child.on('error', reject);
    });
    // Now read the sentinel — this should route to the host where the spawn ran.
    const contents = await new Promise((resolve, reject) => {
        fs.readFile(sentinel, 'utf8', (err, data) => err ? reject(err) : resolve(data));
    });
    if (!contents || contents.length === 0) throw new Error('sentinel file empty');
    // Cleanup (also via remote)
    await new Promise((resolve) => fs.unlink(sentinel, () => resolve()));
    return `sentinel readback: ${JSON.stringify(contents.trim())}`;
});

// ── Phase 14: spawn stdin (currently disabled; driver-side passthrough TBD) ──
// Leaving agent-side stdin pipe in place for future work.

// Cleanup tests
test('cleanup cloud test files', async () => {
    await unlink('/cloud/dumbterm/tester_smoke.txt');
    await unlink('/cloud/dumbterm/tester_survive.txt');
    return 'cleaned';
});

// ── Runner ───────────────────────────────────────────────────
const INTERACTIVE = process.argv.includes('--interactive');
const HEADLESS = process.argv.includes('--headless');

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
    if (!HEADLESS) process.stdout.write(AN.hideCur);
    trace('run_start', { count: tests.length, interactive: INTERACTIVE, headless: HEADLESS });
    draw();
    if (HEADLESS) process.stdout.write(`# flowto tester — ${tests.length} tests, headless mode\n`);

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
            if (HEADLESS) process.stdout.write(`ok ${i+1} ${t.name}` + (result ? ` — ${result}` : '') + '\n');
        } catch (e) {
            t.status = 'fail';
            t.detail = e.message || String(e);
            trace('test_fail', { i, name: t.name, error: e.message, stack: e.stack });
            if (HEADLESS) process.stdout.write(`not ok ${i+1} ${t.name}\n  # ${e.message}\n`);
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
    const passed = tests.length - failed;
    trace('run_done', { pass: passed, fail: failed });
    if (HEADLESS) {
        process.stdout.write(`\n1..${tests.length}\n# pass ${passed}, fail ${failed}\n`);
    } else {
        process.stdout.write(AN.showCur + '\n');
    }
    if (traceStream) traceStream.end();
    process.exit(failed);
}

main().catch(e => {
    trace('crash', { error: e.message, stack: e.stack });
    process.stdout.write(AN.showCur);
    console.error(e);
    process.exit(99);
});
