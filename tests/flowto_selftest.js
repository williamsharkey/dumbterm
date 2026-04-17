// Phase 1 self-test for --flowto.
// Run via:
//   dumbterm --flowto 127.0.0.1:9125 -- node tests/flowto_selftest.js
//
// Exit code is number of failed checks. TAP-ish output.

const cp = require('child_process');

let pass = 0, fail = 0;
function ok(name, cond, detail) {
    if (cond) { console.log(`ok ${++pass+fail} ${name}`); pass++; }
    else { console.log(`not ok ${pass+ ++fail} ${name}` + (detail ? `\n  ${detail}` : '')); }
}
function execP(cmd) {
    return new Promise((res, rej) => cp.exec(cmd, (err, out, errstr) => res({ err, out, errstr })));
}

async function main() {
    // 1. ping path exists: exec on active host should succeed
    let r = await execP('echo hello');
    ok('exec echo works', !r.err && r.out.trim() === 'hello');

    // 2. dumbterm-host list returns the registry
    r = await execP('dumbterm-host list');
    ok('dumbterm-host list lists local', r.out.includes('local'));
    ok('dumbterm-host list marks remote active', r.out.includes('* remote'),
        `actual output: ${JSON.stringify(r.out)}`);

    // 3. exec on remote vs local. Distinguish by a temp file we create on each.
    //   On remote: write /tmp/flowto_remote_marker; on local: /tmp/flowto_local_marker
    //   Then check which is present via ls on the active host.
    await execP('rm -f /tmp/flowto_selftest_marker.txt');
    r = await execP('echo REMOTE > /tmp/flowto_selftest_marker.txt && cat /tmp/flowto_selftest_marker.txt');
    ok('remote create + read marker', r.out.trim() === 'REMOTE');

    // 4. switch to local
    r = await execP('dumbterm-host local');
    ok('switch to local succeeds', r.out.includes('active host: local'));

    // 5. Now execs go to local. Because local & remote are both Mac in this test,
    //    we distinguish via a host-marker file on agent vs driver side. For now
    //    we verify the state change sticks — next remote call should fail
    //    with "no such marker on local" if we didn't create one there.
    //    But simpler: verify the switch is visible to subsequent dumbterm-host list
    r = await execP('dumbterm-host list');
    ok('after switch, local is marked active', r.out.includes('* local'),
        `actual: ${JSON.stringify(r.out)}`);

    // 6. switch back
    r = await execP('dumbterm-host remote');
    ok('switch back to remote', r.out.includes('active host: remote'));

    // 7. dumbterm-at one-shot: run on local without changing state
    r = await execP('dumbterm-at local echo one-shot-local');
    ok('dumbterm-at local echo', r.out.trim() === 'one-shot-local',
        `got: ${JSON.stringify(r.out)}, err: ${r.err?.message}`);
    r = await execP('dumbterm-host list');
    ok('after dumbterm-at, remote still active', r.out.includes('* remote'),
        `actual: ${JSON.stringify(r.out)}`);

    // 8. unknown host name
    r = await execP('dumbterm-host nonexistent');
    ok('unknown host returns error', r.err && r.err.code === 1);

    // 9. exit code passthrough
    r = await execP('sh -c "exit 42"');
    ok('nonzero exit propagates', r.err && r.err.code === 42,
        `err code: ${r.err?.code}`);

    // 10. stderr capture
    r = await execP('sh -c "echo onerr >&2; echo onout"');
    ok('stdout captured', r.out.trim() === 'onout');
    ok('stderr captured', r.errstr.trim() === 'onerr');

    console.log(`\n1..${pass+fail}`);
    console.log(`# pass ${pass}, fail ${fail}`);
    process.exit(fail);
}

main().catch(e => { console.error(e); process.exit(99); });
