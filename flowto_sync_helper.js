// flowto_sync_helper.js — blocking RPC helper, invoked via execFileSync.
//
// Main process can't do sync TCP, but a CHILD process can — execFileSync
// blocks until the child exits. The child connects to DUMBTERM_GATEWAY,
// sends ONE RPC request (from stdin), reads the response (including any
// streaming messages until "exit" key appears), prints the final response
// as a single JSON line on stdout, then exits.
//
// Usage:
//   echo '{"op":"read","host":"remote","path":"C:\\foo.txt"}' |
//   DUMBTERM_GATEWAY=host:port node flowto_sync_helper.js
const net = require('net');
const gw = process.env.DUMBTERM_GATEWAY;
if (!gw) { process.stderr.write('flowto_sync_helper: no DUMBTERM_GATEWAY\n'); process.exit(2); }
const [host, port] = gw.split(':');

let stdinBuf = '';
process.stdin.on('data', (c) => { stdinBuf += c.toString('utf8'); });
process.stdin.on('end', () => {
    const req = stdinBuf.replace(/\n+$/, '') + '\n';
    const s = net.createConnection(parseInt(port), host, () => {
        s.write(req);
    });
    let buf = '';
    let responded = false;
    const deadlineMs = parseInt(process.env.FLOWTO_SYNC_TIMEOUT || '30000');
    const to = setTimeout(() => {
        if (!responded) {
            process.stdout.write(JSON.stringify({ ok: false, errno: 'ETIMEDOUT', err: 'sync RPC timeout' }) + '\n');
            process.exit(0);
        }
    }, deadlineMs);
    s.on('data', (c) => {
        buf += c.toString('utf8');
        let nl;
        while ((nl = buf.indexOf('\n')) >= 0) {
            const line = buf.slice(0, nl);
            buf = buf.slice(nl + 1);
            try {
                const msg = JSON.parse(line);
                // For single-response ops we return on the first line. For
                // streaming ops (spawn) we'd need to aggregate, but sync
                // ops don't stream — so first reply is the one.
                responded = true;
                process.stdout.write(line + '\n');
                clearTimeout(to);
                s.end();
                // Drain and exit
                process.nextTick(() => process.exit(0));
                return;
            } catch (e) { /* partial line, keep buffering */ }
        }
    });
    s.on('error', (e) => {
        process.stdout.write(JSON.stringify({ ok: false, errno: 'ECONNREFUSED', err: e.message }) + '\n');
        process.exit(0);
    });
});
