// W7-side test child: logs every resize event with timestamps to a file.
// Run via dumbterm so the JS shim fires process.stdout.emit('resize').
const fs = require('fs');

const logPath = process.env.RESIZE_LOG || 'C:\\workspace\\dumbterm-test\\resize_test.log';

function log(msg) {
    const line = `[${new Date().toISOString()}] ${msg}\n`;
    try { fs.appendFileSync(logPath, line); } catch (e) {}
    process.stdout.write(line);
}

// Clear previous log
try { fs.writeFileSync(logPath, ''); } catch (e) {}

log(`BOOT  isTTY=${process.stdout.isTTY} cols=${process.stdout.columns} rows=${process.stdout.rows}`);

// Log every resize event
process.stdout.on('resize', () => {
    log(`RESIZE cols=${process.stdout.columns} rows=${process.stdout.rows}`);
});

// Also hook stdin push interception: listen to raw stdin data for _RESIZE
// (even if the shim's emit fails, we can see the bytes arriving)
let stdinBuf = '';
process.stdin.on('data', (chunk) => {
    const s = chunk.toString('binary');
    stdinBuf += s;
    while (true) {
        const idx = stdinBuf.indexOf('\x1b_RESIZE;');
        if (idx < 0) { stdinBuf = stdinBuf.slice(-32); break; }
        const end = stdinBuf.indexOf('\x1b\\', idx + 9);
        if (end < 0) break;
        const parts = stdinBuf.slice(idx + 9, end).split(';');
        log(`STDIN_RESIZE_BYTES cols=${parts[0]} rows=${parts[1]}`);
        stdinBuf = stdinBuf.slice(end + 2);
    }
});

// Heartbeat: print current dimensions every 500ms so we can see if they change
setInterval(() => {
    log(`HEARTBEAT cols=${process.stdout.columns} rows=${process.stdout.rows}`);
}, 500);

// Keep alive
process.stdin.resume();
