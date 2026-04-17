// Phase 1 self-test for --flowto — portable across Mac agent and W7 agent.
// Run via:
//   dumbterm --flowto HOST:PORT -- node tests/flowto_selftest.js

const cp = require('child_process');
const fs = require('fs');
const os = require('os');

let pass = 0, fail = 0;
function ok(name, cond, detail) {
    const n = pass + fail + 1;
    if (cond) { console.log(`ok ${n} ${name}`); pass++; }
    else { console.log(`not ok ${n} ${name}` + (detail ? `\n  ${detail}` : '')); fail++; }
}
function execP(cmd) {
    return new Promise((res) => cp.exec(cmd, (err, out, errstr) => res({ err, out, errstr })));
}

// Cross-platform helpers: use `node -e` for anything semantic, so the agent's
// shell dialect (cmd vs sh) doesn't matter. Every test command ends up running
// `cmd.exe /c node -e "..."` on W7 or `sh -c "node -e \"...\""` on Mac.
const NODE_EXEC = 'node -e';

async function main() {
    // 1. trivial exec on remote (active host = remote)
    let r = await execP(`${NODE_EXEC} "console.log(42)"`);
    ok('trivial exec on remote', !r.err && r.out.trim() === '42',
        `err=${r.err?.message}, out=${JSON.stringify(r.out)}`);

    // 2. platform detection: remote should report its own platform
    r = await execP(`${NODE_EXEC} "console.log(process.platform)"`);
    const remotePlatform = r.out.trim();
    ok('remote platform detected', remotePlatform.length > 0,
        `got: ${JSON.stringify(remotePlatform)}`);

    // 3. dumbterm-host list shows registry
    r = await execP('dumbterm-host list');
    ok('registry lists local', r.out.includes('local'));
    ok('registry marks remote active', r.out.includes('* remote'),
        `actual: ${JSON.stringify(r.out)}`);

    // 4. switch to local and confirm platform differs (on cross-host test)
    await execP('dumbterm-host local');
    r = await execP(`${NODE_EXEC} "console.log(process.platform)"`);
    const localPlatform = r.out.trim();
    ok('local platform detected', localPlatform.length > 0);
    // If this is a real cross-host test, the platforms differ:
    if (remotePlatform !== localPlatform) {
        ok('remote vs local platforms differ (real cross-host)', true,
            `remote=${remotePlatform}, local=${localPlatform}`);
    } else {
        ok('remote vs local same platform (same-host test)', true,
            `both=${remotePlatform} — can't distinguish without cross-host agent`);
    }

    // 5. switch back to remote
    r = await execP('dumbterm-host remote');
    ok('switch back to remote', r.out.includes('active host: remote'));

    // 6. dumbterm-at one-shot: run on local without changing state
    r = await execP(`dumbterm-at local ${NODE_EXEC} "console.log('oneshot')"`);
    ok('dumbterm-at local one-shot', r.out.trim() === 'oneshot',
        `got: ${JSON.stringify(r.out)}`);
    r = await execP('dumbterm-host list');
    ok('after one-shot, remote still active', r.out.includes('* remote'));

    // 7. unknown host returns error
    r = await execP('dumbterm-host nonexistent');
    ok('unknown host returns error', r.err && r.err.code === 1);

    // 8. exit code propagation
    r = await execP(`${NODE_EXEC} "process.exit(42)"`);
    ok('nonzero exit propagates', r.err && r.err.code === 42,
        `err.code=${r.err?.code}, err.msg=${r.err?.message}`);

    // 9. stdout/stderr captured separately
    r = await execP(`${NODE_EXEC} "console.log('OUT'); console.error('ERR')"`);
    ok('stdout captured', r.out.includes('OUT'),
        `out=${JSON.stringify(r.out)}`);
    ok('stderr captured', r.errstr.includes('ERR'),
        `err=${JSON.stringify(r.errstr)}`);

    // 10. env var detection: remote should see its own env
    r = await execP(`${NODE_EXEC} "console.log(process.env.OS || process.env.SHELL || 'none')"`);
    ok('remote env accessible', r.out.trim().length > 0,
        `got: ${JSON.stringify(r.out)}`);

    // ── Phase 2: fs.* ────────────────────────────────────────────

    // 11. homedir override
    ok('os.homedir is cloud', os.homedir() === '/cloud/dumbterm/home',
        `got: ${os.homedir()}`);
    ok('process.env.HOME is cloud', process.env.HOME === '/cloud/dumbterm/home',
        `got: ${process.env.HOME}`);

    // 12. fs round-trip on /cloud/
    const cloudFile = '/cloud/dumbterm/selftest_phase2.txt';
    try { await fs.promises.mkdir('/cloud/dumbterm/'); } catch(e) { /* may exist */ }
    await fs.promises.writeFile(cloudFile, 'hello from phase 2');
    const read = await fs.promises.readFile(cloudFile, 'utf8');
    ok('cloud write+read round-trip', read === 'hello from phase 2',
        `got: ${JSON.stringify(read)}`);

    // 13. Cloud file persists across host switch
    await execP('dumbterm-host local');
    // On local, /cloud/ still routes to persistent host
    const read2 = await fs.promises.readFile(cloudFile, 'utf8');
    ok('cloud path reads same after switching to local', read2 === 'hello from phase 2',
        `got: ${JSON.stringify(read2)}`);
    await execP('dumbterm-host remote');

    // 14. readdir on cloud dir
    const entries = await fs.promises.readdir('/cloud/dumbterm/');
    ok('readdir returns cloud entries', entries.some(e => e.includes('selftest_phase2.txt')),
        `got: ${JSON.stringify(entries)}`);

    // 15. stat returns size
    const st = await fs.promises.stat(cloudFile);
    ok('stat returns correct size', st.size === 18, `expected 18, got: ${st.size}`);
    ok('stat isFile=true', st.isFile());
    ok('stat isDirectory=false', !st.isDirectory());

    // 16. Binary round-trip (bytes 0..255)
    const binPath = '/cloud/dumbterm/bin_test.dat';
    const bytes = Buffer.alloc(256);
    for (let i = 0; i < 256; i++) bytes[i] = i;
    await fs.promises.writeFile(binPath, bytes);
    const binBack = await fs.promises.readFile(binPath);
    let binOk = binBack.length === 256;
    for (let i = 0; i < 256 && binOk; i++) if (binBack[i] !== i) binOk = false;
    ok('binary 256-byte round-trip', binOk, `len: ${binBack.length}`);

    // 17. unlink
    await fs.promises.unlink(binPath);
    let existed = true;
    try { await fs.promises.stat(binPath); } catch (e) { if (e.code === 'ENOENT') existed = false; }
    ok('unlink removes file', !existed);

    // 18. ENOENT on missing file
    let errCode = null;
    try { await fs.promises.readFile('/cloud/dumbterm/nonexistent.xyz'); }
    catch (e) { errCode = e.code; }
    ok('ENOENT on missing cloud file', errCode === 'ENOENT', `code: ${errCode}`);

    // Cleanup
    try { await fs.promises.unlink(cloudFile); } catch(e) {}

    console.log(`\n1..${pass+fail}`);
    console.log(`# pass ${pass}, fail ${fail}`);
    process.exit(fail);
}

main().catch(e => { console.error(e); process.exit(99); });
