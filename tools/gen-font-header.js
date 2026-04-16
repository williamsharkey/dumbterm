#!/usr/bin/env node
// Generate unifont_data.h from the parsed unifont data for embedding in C.
// Supports both 8x16 narrow glyphs and 16x16 wide glyphs.

const fs = require('fs');

// Load narrow data
const src = fs.readFileSync(__dirname + '/unifont_data.js', 'utf8');
eval(src.replace(/^const /gm, 'var '));

// Load wide data (if available)
let FONT_WIDE_DATA = {};
try {
  const wsrc = fs.readFileSync(__dirname + '/unifont_wide.js', 'utf8');
  eval(wsrc.replace(/^const /gm, 'var '));
  FONT_WIDE_DATA = FONT_WIDE || {};
} catch (e) {
  console.log('No wide data found, generating narrow-only header');
}

const glyphs = Object.entries(FONT_DATA)
  .map(([cp, hex]) => ({ cp: parseInt(cp), hex }))
  .sort((a, b) => a.cp - b.cp);

const wideGlyphs = Object.entries(FONT_WIDE_DATA)
  .map(([cp, hex]) => ({ cp: parseInt(cp), hex }))
  .sort((a, b) => a.cp - b.cp);

let h = `/* unifont_data.h — auto-generated from GNU Unifont. ${glyphs.length} narrow + ${wideGlyphs.length} wide glyphs. */\n`;
h += `#ifndef UNIFONT_DATA_H\n#define UNIFONT_DATA_H\n\n`;
h += `#define UNIFONT_COUNT ${glyphs.length}\n`;
h += `#define UNIFONT_WIDE_COUNT ${wideGlyphs.length}\n`;
h += `#define FONT_W 8\n#define FONT_H 16\n\n`;

// Narrow glyphs: 16 bytes per glyph (1 byte per row, 8 bits wide)
h += `typedef struct { unsigned short cp; unsigned char data[16]; } UGlyph;\n\n`;
h += `static const UGlyph UNIFONT[UNIFONT_COUNT] = {\n`;
for (const g of glyphs) {
  const bytes = [];
  for (let i = 0; i < g.hex.length; i += 2)
    bytes.push('0x' + g.hex.substr(i, 2));
  const ch = g.cp >= 0x20 && g.cp < 0x7F ? ` /* '${String.fromCharCode(g.cp)}' */` : '';
  h += `  {0x${g.cp.toString(16).padStart(4,'0')},{${bytes.join(',')}}},${ch}\n`;
}
h += `};\n\n`;

// Wide glyphs: 32 bytes per glyph (2 bytes per row, 16 bits wide)
h += `typedef struct { unsigned short cp; unsigned char data[32]; } UGlyphWide;\n\n`;
h += `static const UGlyphWide UNIFONT_WIDE[UNIFONT_WIDE_COUNT] = {\n`;
for (const g of wideGlyphs) {
  const bytes = [];
  for (let i = 0; i < g.hex.length; i += 2)
    bytes.push('0x' + g.hex.substr(i, 2));
  h += `  {0x${g.cp.toString(16).padStart(4,'0')},{${bytes.join(',')}}},\n`;
}
h += `};\n\n`;

// Binary search for narrow
h += `static const unsigned char *unifont_lookup(unsigned short cp) {\n`;
h += `  int lo = 0, hi = UNIFONT_COUNT - 1;\n`;
h += `  while (lo <= hi) {\n`;
h += `    int mid = (lo + hi) / 2;\n`;
h += `    if (UNIFONT[mid].cp == cp) return UNIFONT[mid].data;\n`;
h += `    if (UNIFONT[mid].cp < cp) lo = mid + 1; else hi = mid - 1;\n`;
h += `  }\n`;
h += `  return 0;\n`;
h += `}\n\n`;

// Binary search for wide — returns 32 bytes (2 bytes per row) or NULL
h += `static const unsigned char *unifont_wide_lookup(unsigned short cp) {\n`;
h += `  int lo = 0, hi = UNIFONT_WIDE_COUNT - 1;\n`;
h += `  while (lo <= hi) {\n`;
h += `    int mid = (lo + hi) / 2;\n`;
h += `    if (UNIFONT_WIDE[mid].cp == cp) return UNIFONT_WIDE[mid].data;\n`;
h += `    if (UNIFONT_WIDE[mid].cp < cp) lo = mid + 1; else hi = mid - 1;\n`;
h += `  }\n`;
h += `  return 0;\n`;
h += `}\n\n`;

h += `#endif /* UNIFONT_DATA_H */\n`;

fs.writeFileSync(__dirname + '/../unifont_data.h', h);
console.log(`Wrote unifont_data.h: ${glyphs.length} narrow + ${wideGlyphs.length} wide glyphs, ${h.length} bytes`);
