#!/usr/bin/env node
// Parse GNU Unifont hex format into FONT_DATA for dumbterm.
// Unifont is 8x16 for most chars (32 hex chars) and 16x16 for CJK (64 hex chars).
// Narrow glyphs stored as 8x16 (16 bytes), wide glyphs stored as 16x16 (32 bytes).

const fs = require('fs');
const lines = fs.readFileSync('/tmp/unifont.hex', 'utf8').trim().split('\n');

console.log('Unifont lines:', lines.length);

const FONT_DATA = {};     // narrow: 8x16, hex = 32 chars
const FONT_WIDE = {};     // wide: 16x16, hex = 64 chars
let narrow = 0, wide = 0, skipped = 0;

for (const line of lines) {
  const [cpHex, hexData] = line.split(':');
  const cp = parseInt(cpHex, 16);

  if (hexData.length === 32) {
    // 8x16 glyph — perfect, use directly
    FONT_DATA[cp] = hexData.toLowerCase();
    narrow++;
  } else if (hexData.length === 64) {
    // 16x16 glyph — store BOTH a narrow (centered) version and the full wide version
    const rows16 = [];
    for (let y = 0; y < 16; y++) {
      rows16.push(parseInt(hexData.substr(y * 4, 4), 16));
    }
    // Store the full 16x16 data
    FONT_WIDE[cp] = hexData.toLowerCase();

    // Also create a narrow 8px version (centered) for fallback
    let minX = 16, maxX = -1;
    for (let y = 0; y < 16; y++) {
      for (let x = 0; x < 16; x++) {
        if (rows16[y] & (0x8000 >> x)) {
          if (x < minX) minX = x;
          if (x > maxX) maxX = x;
        }
      }
    }
    const contentW = maxX >= minX ? maxX - minX + 1 : 0;
    let hex8 = '';
    if (contentW <= 8) {
      const offset = minX - Math.max(0, Math.floor((8 - contentW) / 2));
      for (let y = 0; y < 16; y++) {
        let byte = 0;
        for (let x = 0; x < 8; x++) {
          const srcX = x + offset;
          if (srcX >= 0 && srcX < 16 && (rows16[y] & (0x8000 >> srcX))) {
            byte |= (0x80 >> x);
          }
        }
        hex8 += byte.toString(16).padStart(2, '0');
      }
    } else {
      const startX = Math.floor((16 - 8) / 2);
      for (let y = 0; y < 16; y++) {
        let byte = 0;
        for (let x = 0; x < 8; x++) {
          if (rows16[y] & (0x8000 >> (startX + x))) {
            byte |= (0x80 >> x);
          }
        }
        hex8 += byte.toString(16).padStart(2, '0');
      }
    }
    FONT_DATA[cp] = hex8.toLowerCase();
    wide++;
  } else {
    skipped++;
  }
}

console.log('Parsed: narrow=' + narrow + ' wide=' + wide + ' skipped=' + skipped);
console.log('Total glyphs:', Object.keys(FONT_DATA).length);

// Verify key glyphs
for (const [name, cp] of [['L',76],['A',65],['dash',0x2500],['vert',0x2502],['block',0x2588],
    ['╭',0x256D],['╮',0x256E],['╯',0x256F],['╰',0x2570],
    ['▗',0x2597],['▖',0x2596],['▘',0x2598],['▝',0x259D],['❯',0x276F],
    ['✻',0x273B],['✽',0x273D],['✢',0x2722],['⏺',0x23FA],
    ['●',0x25CF],['⏵',0x23F5]]) {
  const hex = FONT_DATA[cp];
  const whex = FONT_WIDE[cp];
  if (!hex) { console.log(name + ' U+' + cp.toString(16) + ': MISSING'); continue; }
  console.log(name + ' U+' + cp.toString(16) + (whex ? ' [WIDE 16x16]' : ' [narrow 8x16]') + ':');
  if (whex) {
    // Show 16x16
    for (let y = 0; y < 16; y++) {
      const w = parseInt(whex.substr(y*4, 4), 16);
      let s = '';
      for (let x = 0; x < 16; x++) s += (w & (0x8000>>x)) ? '##' : '..';
      console.log('  ' + s);
    }
  } else {
    for (let y = 0; y < 16; y++) {
      const b = parseInt(hex.substr(y*2, 2), 16);
      let s = '';
      for (let x = 0; x < 8; x++) s += (b & (0x80>>x)) ? '##' : '..';
      console.log('  ' + s);
    }
  }
}

// Select only the glyphs we actually need
const needed = new Set();
for (let i = 0; i < 128; i++) needed.add(i);
for (let i = 128; i < 256; i++) needed.add(i);
for (let i = 0x2500; i <= 0x257F; i++) needed.add(i);
for (let i = 0x2580; i <= 0x259F; i++) needed.add(i);
for (let i = 0x2190; i <= 0x21FF; i++) needed.add(i);
[0x2022,0x2026,0x00B7,0x2219,
 0x2318,0x2325,0x232B,0x23CE,0x23F5,0x23F9,0x23FA,0x23BF,
 0x2605,0x2606,0x2610,0x2611,0x2612,
 0x2713,0x2714,0x2717,0x2718,
 0x2721,0x2722,0x2726,0x2727,0x2731,0x2732,0x2733,0x2734,0x2735,0x2736,
 0x2737,0x2738,0x2739,0x273A,0x273B,0x273C,0x273D,0x273E,0x273F,
 0x2740,0x2741,0x2742,0x2743,0x274B,0x271D,
 0x276F,0x279C,
 0x25A0,0x25A1,0x25AA,0x25AB,0x25B2,0x25B6,0x25BA,0x25BC,0x25C0,0x25C4,
 0x25CB,0x25CF,0x25D0,0x25D1,0x25E6,
 0x26A0,0x26A1,
 0x2800,0x2801,0x2802,0x2803,0x2804,0x2808,0x2810,0x2820,
 0x283F,0x2840,0x2844,0x2846,0x284C,0x2858,0x2870,0x287F,
 0xFFFD,
].forEach(cp => needed.add(cp));

const selected = {};
const selectedWide = {};
let found = 0, foundWide = 0, missing = 0;
for (const cp of needed) {
  if (FONT_DATA[cp]) { selected[cp] = FONT_DATA[cp]; found++; }
  else missing++;
  if (FONT_WIDE[cp]) { selectedWide[cp] = FONT_WIDE[cp]; foundWide++; }
}
console.log('\nSelected: ' + found + ' narrow, ' + foundWide + ' wide, ' + missing + ' missing from unifont');

// Output narrow
const pairs = Object.entries(selected).map(([k,v]) => k+':"'+v+'"');
const out = 'const FONT_DATA={' + pairs.join(',') + '};\nconst FONT_W=8,FONT_H=16;\n';
fs.writeFileSync(__dirname + '/unifont_data.js', out);
console.log('Wrote unifont_data.js: ' + out.length + ' bytes');

// Output wide
const wpairs = Object.entries(selectedWide).map(([k,v]) => k+':"'+v+'"');
const wout = 'const FONT_WIDE={' + wpairs.join(',') + '};\n';
fs.writeFileSync(__dirname + '/unifont_wide.js', wout);
console.log('Wrote unifont_wide.js: ' + foundWide + ' wide glyphs');
