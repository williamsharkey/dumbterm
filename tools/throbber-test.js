// throbber-test.js — reproduces Claude Code's thinking throbber + status bar
//
// Interactive:  node throbber-test.js          (quit with q or Ctrl+C)
// Capture:      node throbber-test.js --frames 50   (render 50 frames then exit)

const out = process.stdout;
const args = process.argv.slice(2);
const framesIdx = args.indexOf('--frames');
const maxFrames = framesIdx >= 0 ? parseInt(args[framesIdx + 1], 10) : 0;
const cols = out.columns || 80;
const rows = out.rows || 24;

const CSI = '\x1b[';
const SYNC_START = CSI + '?2026h';
const SYNC_END   = CSI + '?2026l';

// Claude Code's exact throbber glyphs
const throbber = ['\u00b7', '\u2733', '\u2736', '\u271d', '\u2727', '\u2726', '\u2736', '\u2733', '\u00b7'];
// · ✳ ✶ ✝ ✧ ✦ ✶ ✳ ·

const verbs = [
  'Thinking', 'Composing', 'Ebbing', 'Stewing',
  'Marinating', 'Sprouting', 'Confabulating', 'Crafting'
];

let frame = 0;
let verb_idx = 0;
let verb_hold = 0;

function render() {
  const t = throbber[frame % throbber.length];
  if (++verb_hold > 25) { verb_hold = 0; verb_idx = (verb_idx + 1) % verbs.length; }
  const verb = verbs[verb_idx];

  let s = SYNC_START;

  // --- header ---
  s += CSI + '1;1H' + CSI + '2K';
  s += CSI + '38;2;180;140;255m' + CSI + '1m';
  s += '  Claude Throbber Test';
  s += CSI + '0m';

  // divider
  s += CSI + '2;1H' + CSI + '2K';
  s += CSI + '38;2;60;60;60m' + '\u2500'.repeat(cols) + CSI + '0m';

  // fake assistant response
  s += CSI + '4;1H' + CSI + '2K';
  s += CSI + '38;2;200;200;200m';
  s += '  Hello! I can help with that. Let me look into it.';
  s += CSI + '0m';

  // --- the throbber line (the thing that breaks) ---
  s += CSI + '6;1H' + CSI + '2K';
  s += CSI + '38;2;255;200;50m';
  s += '  ' + t + ' ' + verb + '\u2026';
  s += CSI + '0m';

  // secondary status (auto-updating badge, right-aligned)
  const badge = 'Auto-updating\u2026';
  s += CSI + '6;' + (cols - badge.length) + 'H';
  s += CSI + '38;2;100;100;100m' + badge + CSI + '0m';

  // --- divider before status bar ---
  s += CSI + (rows - 2) + ';1H' + CSI + '2K';
  s += CSI + '38;2;60;60;60m' + '\u2500'.repeat(cols) + CSI + '0m';

  // --- status bar (bottom) ---
  s += CSI + (rows - 1) + ';1H' + CSI + '2K';
  s += CSI + '48;2;30;30;30m' + CSI + '38;2;150;150;150m';
  const status = '  \u23f5\u23f5 bypass permissions on (meta+m to cycle)';
  const right  = '\u25d0 medium \u00b7 /effort';
  s += status;
  s += CSI + (rows - 1) + ';' + (cols - right.length) + 'H';
  s += right;
  s += CSI + '0m';

  // --- prompt ---
  s += CSI + rows + ';1H' + CSI + '2K';
  s += CSI + '38;2;100;200;255m' + CSI + '1m' + '> ' + CSI + '0m';

  s += SYNC_END;
  out.write(s);
  frame++;

  if (maxFrames > 0 && frame >= maxFrames) {
    clearInterval(iv);
    out.write(CSI + '?25h' + CSI + '0m');
    process.exit(0);
  }
}

// setup
out.write(CSI + '?25l' + CSI + '2J');
const iv = setInterval(render, 80);

if (!maxFrames) {
  if (out.isTTY && process.stdin.setRawMode) process.stdin.setRawMode(true);
  process.stdin.resume();
  process.stdin.on('data', d => {
    if (d[0] === 3 || d[0] === 0x71) {
      clearInterval(iv);
      out.write(CSI + '?25h' + CSI + '2J' + CSI + '1;1H');
      process.exit(0);
    }
  });
}
out.on('resize', () => {});
process.on('exit', () => out.write(CSI + '?25h' + CSI + '0m'));
