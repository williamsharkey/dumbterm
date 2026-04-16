/*
 * dumbterm.c — cross-platform bitmap-font GL terminal emulator
 *
 * Mac:  cc -O2 -o dumbterm dumbterm.c -framework OpenGL -framework GLUT -framework Cocoa -lutil
 * W7:   gcc -O2 -o dumbterm.exe dumbterm.c -lopengl32 -lgdi32 -luser32 -lkernel32 -lws2_32
 *
 * Usage:
 *   dumbterm                                    # spawns $SHELL / cmd.exe
 *   dumbterm -- claude                          # spawns claude locally
 *   dumbterm --listen 9124 -- claude            # serve over TCP (headless on W7)
 *   dumbterm --connect w7-host:9124             # connect to remote dumbterm
 *   dumbterm --connect 127.0.0.1:9124           # via SSH tunnel
 *
 * Network mode streams raw VT bytes — not pixels. ~1KB/frame, not ~150KB.
 * The client renders locally with its own font. Like SSH but simpler.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <time.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define SOCK_INVALID INVALID_SOCKET
#define sock_close closesocket
#define sock_init() do { WSADATA w; WSAStartup(MAKEWORD(2,2),&w); } while(0)
#else
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
typedef int sock_t;
#define SOCK_INVALID (-1)
#define sock_close close
#define sock_init()
#endif

#include "unifont_data.h"

/* ── network mode globals ──────────────────────────────────────── */
static char *g_listen_addr = NULL;   /* --listen PORT or HOST:PORT */
static char *g_connect_addr = NULL;  /* --connect HOST:PORT */
static sock_t g_net_sock = SOCK_INVALID;
static int g_net_mode = 0; /* 0=local, 1=listen(server), 2=connect(client) */
static int g_server_visible = 0; /* --visible: show local window while serving */

/* multi-client support for server mode */
#define MAX_CLIENTS 10
static sock_t g_clients[MAX_CLIENTS];
static int g_num_clients;
static sock_t g_listen_sock = SOCK_INVALID;

/* ── configuration ─────────────────────────────────────────────── */

#define TERM_COLS     400   /* max grid capacity */
#define TERM_ROWS     120
#define MAX_CSI       16
#define HIST_LINES    5000  /* scrollback history */

/* ── cursor: phosphor blink ────────────────────────────────────── */
#define AMBER_R 1.00f
#define AMBER_G 0.65f
#define AMBER_B 0.05f
/* red-amber for the always-on active cursor floor */
#define CURSOR_FLOOR_R 0.85f
#define CURSOR_FLOOR_G 0.25f
#define CURSOR_FLOOR_B 0.02f
#define CURSOR_FLOOR_A 0.05f
/* phosphor glow: per-cell brightness left behind by blink ticks */
#define CURSOR_PHOSPHOR_MAX 256
static float cursor_phosphor[CURSOR_PHOSPHOR_MAX]; /* indexed by row*TERM_COLS+col, decays to 0 */
static int   cursor_phosphor_col[CURSOR_PHOSPHOR_MAX];
static int   cursor_phosphor_row[CURSOR_PHOSPHOR_MAX];
static int   cursor_phosphor_count;
/* blink state: wall-clock driven for precise timing */
static double cursor_blink_next; /* next blink time (CFAbsoluteTime) */
#define CURSOR_BLINK_SEC 0.4 /* seconds between blinks */

/* ── morse scan-line visualization ─────────────────────────────── */
/* Per-character sample boundaries for syncing visual sweep with audio */
#define MORSE_VIS_MAX 1024
static int  morse_vis_starts[MORSE_VIS_MAX]; /* sample index where each char begins */
static int  morse_vis_count;                  /* number of chars in current morse */
static int  morse_vis_cols[MORSE_VIS_MAX];    /* screen column for each char */
static int  morse_vis_rows[MORSE_VIS_MAX];    /* screen row for each char */
static int  morse_vis_has_sound[MORSE_VIS_MAX]; /* 1 if char maps to a morse code */
/* per-column trail brightness (decays over time) */
#define MORSE_TRAIL_COLS 256
static float morse_trail[MORSE_TRAIL_COLS];   /* indexed by screen column */
static int   morse_trail_rows[MORSE_TRAIL_COLS]; /* which row each trail col is on */
/* decay: 1.1 seconds at 60fps → per-frame factor = exp(-1/(1.1*60)) ≈ 0.9849 */
#define MORSE_TRAIL_DECAY 0.9849f
/* scan-line pulse brightness — decays exponentially over ~5 pixels of travel */
static float morse_scan_bright; /* current scan-line intensity, decays toward base */

/* font scaling: FONT_W/FONT_H are the base glyph size (8x16).
   font_scale multiplies them for display. 1=8x16, 2=16x32, 3=24x48. */
static int font_scale = 1;
#define CELL_W (FONT_W * font_scale)
#define CELL_H (FONT_H * font_scale)

/* ── ANSI 256-color palette ────────────────────────────────────── */

static void color256(int n, unsigned char *rgb) {
    /* Terminal.app-style palette (matches macOS default) */
    static const unsigned char ansi16[16][3] = {
        {  0,  0,  0},{194, 54, 33},{37,188, 36},{173,173, 39},{73, 46,225},{211, 56,211},{ 51,187,200},{203,204,205},
        {104,104,104},{252, 57, 31},{49,231, 34},{234,236, 35},{88, 51,255},{249, 53,248},{20,240,240},{233,235,235}
    };
    static const unsigned char cube[6] = {0,95,135,175,215,255};
    if (n < 16) { memcpy(rgb, ansi16[n], 3); }
    else if (n < 232) { n -= 16; rgb[0]=cube[n/36]; rgb[1]=cube[(n/6)%6]; rgb[2]=cube[n%6]; }
    else { int g = 8+(n-232)*10; rgb[0]=rgb[1]=rgb[2]=g; }
}

/* ── cell grid ─────────────────────────────────────────────────── */

typedef struct {
    unsigned short ch;
    unsigned char fg[3], bg[3];
    unsigned char bold, inv;
} Cell;

static Cell grid[TERM_ROWS][TERM_COLS];
static int cur_x, cur_y, cur_vis = 1;
static unsigned char pen_fg[3] = {192,192,192};
static unsigned char pen_bg[3] = {0,0,0};
static unsigned char pen_bold, pen_inv;

/* scrollback history: ring buffer of lines that scrolled off the top */
static Cell history[HIST_LINES][TERM_COLS];
static int hist_count;  /* total lines saved (up to HIST_LINES) */
static int hist_write;  /* next write position in ring buffer */

/* ── cell hover glow ───────────────────────────────────────────── */
/* One float per cell: 0.0 = no glow, 1.0 = full brightness.      */
/* The hovered cell is set to 1.0 each frame. All cells decay      */
/* exponentially. Cost: one multiply per cell per frame (~19KB).   */
static float glow[TERM_ROWS][TERM_COLS];
static float row_glow[TERM_ROWS];   /* crosshair row highlight */
static float col_glow[TERM_COLS];   /* crosshair col highlight */
static int mouse_col = -1, mouse_row = -1;
#define GLOW_BRIGHT   0.20f
#define GLOW_DECAY    0.955f
#define CROSS_BRIGHT  0.15f

/* ── audio synthesis ───────────────────────────────────────────── */
/* FM-synthesized morse + granular hover clicks via CoreAudio.     */
/* Sound presets define carrier freq, mod ratio, mod depth, noise. */
#ifndef _WIN32
#include <AudioToolbox/AudioToolbox.h>
#endif

typedef struct {
    const char *name;
    float carrier;   /* Hz */
    float mod_ratio;  /* modulator = carrier * ratio */
    float mod_depth;  /* 0..1 */
    float noise;      /* 0..1 mix of noise */
    float decay;      /* envelope decay rate */
} SoundPreset;

static const SoundPreset PRESETS[] = {
    {"Typewriter",  800, 7.0, 0.8, 0.3, 0.95},
    {"Teletype",   1200, 5.0, 0.6, 0.2, 0.90},
    {"Relay",       600, 9.0, 1.0, 0.5, 0.85},
    {"Crystal",    2000, 3.0, 0.4, 0.1, 0.97},
    {"Spark",      1500,11.0, 0.9, 0.6, 0.80},
    {"Needle",     3000, 2.0, 0.3, 0.05,0.98},
    {"Ticker",      400,13.0, 0.7, 0.4, 0.88},
    {"Disk",        900, 6.0, 0.5, 0.35,0.92},
    {"Wire",       1800, 8.0, 0.7, 0.15,0.94},
    {"Punch",       500,15.0, 1.0, 0.7, 0.82},
};
#define NUM_PRESETS 10

/* volume: 0=off, 1=lo(0.05), 2=med(0.15), 3=hi(0.35) */
static int morse_preset = 0, morse_vol = 1;
static int hover_preset = 3, hover_vol = 1;
static int type_preset = 1, type_vol = 1;   /* sound when user types (default Lo) */
static int delete_preset = 5, delete_vol = 1; /* sound when user deletes (default Lo) */
static int output_preset = 7, output_vol = 1; /* sound when chars appear on screen (default Lo) */
static int morse_speed = 0; /* 0-8: see morse speed table */
static const float VOL_LEVELS[] = {0.0f, 0.05f, 0.15f, 0.35f};

/* morse code table (A-Z, 0-9) */
static const char *MORSE[] = {
    ".-","-...","-.-.","-..",".","..-.","--.","....","..",".---",   /* A-J */
    "-.-",".-..","--","-.","---",".--.","--.-",".-.","...","-",     /* K-T */
    "..-","...-",".--","-..-","-.--","--..",                       /* U-Z */
    "-----",".----","..---","...--","....-",".....","-....","--...","---..","----." /* 0-9 */
};

/* audio state for async playback */
static float *audio_buf;
static int audio_len, audio_pos;
static int audio_playing;
static int prev_hover_col = -1, prev_hover_row = -1;

/* minor pentatonic scale for box drawing tones (C5 Eb5 F5 G5 Bb5) */
static const float BOX_SCALE[] = {523.0f, 622.0f, 698.0f, 784.0f, 932.0f};
#define BOX_SCALE_LEN 5
static int is_box_char(int ch) { return (ch >= 0x2500 && ch <= 0x259F); }

/* simple pseudo-random for noise */
static unsigned int rng_state = 12345;
static float rng_float(void) { rng_state = rng_state * 1103515245 + 12345; return (float)(rng_state >> 16) / 32768.0f - 1.0f; }

/* generate FM synthesis samples */
static void gen_fm_tone(float *out, int samples, int sr, const SoundPreset *p, float volume) {
    float phase_c = 0, phase_m = 0;
    float env = 1.0f;
    float fc = p->carrier, fm = fc * p->mod_ratio;
    int i;
    for (i = 0; i < samples; i++) {
        float mod = sinf(phase_m * 6.2832f) * p->mod_depth * fc;
        float sig = sinf((phase_c + mod / (float)sr) * 6.2832f);
        sig = sig * (1.0f - p->noise) + rng_float() * p->noise;
        out[i] = sig * env * volume;
        phase_c += fc / (float)sr;
        phase_m += fm / (float)sr;
        env *= p->decay + (1.0f - p->decay) * 0.9999f; /* slow decay */
        if (phase_c > 1.0f) phase_c -= 1.0f;
        if (phase_m > 1.0f) phase_m -= 1.0f;
    }
}

/* generate morse audio for a string of codepoints. Alphanumeric → morse,
   box drawing → minor scale tones, other symbols → neutral tone. */
static void gen_morse_audio(const int *text, int text_len) {
    if (morse_vol == 0) return;
    const SoundPreset *p = &PRESETS[morse_preset];
    float volume = VOL_LEVELS[morse_vol];
    int sr = 22050;
    /* count total units: morse chars get their standard units,
       non-morse printable chars get 2 units (one tone + one gap) */
    int units = 0;
    int i;
    for (i = 0; i < text_len; i++) {
        int ch = text[i];
        const char *code = NULL;
        if (ch >= 'A' && ch <= 'Z') code = MORSE[ch - 'A'];
        else if (ch >= 'a' && ch <= 'z') code = MORSE[ch - 'a'];
        else if (ch >= '0' && ch <= '9') code = MORSE[26 + ch - '0'];
        else if (ch == ' ') { units += 7; continue; }
        else if (ch > ' ') { units += 2 + 3; continue; } /* tone + gap + inter-char */
        else continue;
        if (!code) continue;
        int j;
        for (j = 0; code[j]; j++) {
            units += (code[j] == '.') ? 1 : 3;
            if (code[j+1]) units += 1;
        }
        units += 3;
    }
    if (units == 0) return;
    /* speed modes: 0-2 = max-duration capped, 3-8 = chars-per-second rate */
    static const struct { int is_cps; float val; } morse_speeds[] = {
        {0, 0.8f},  /* 0: Super Fast (0.8s max) */
        {0, 1.6f},  /* 1: Very Fast (1.6s max) */
        {0, 3.0f},  /* 2: Fast (3s max) */
        {1, 4.0f},  /* 3: Natural (4 ch/s) */
        {1, 5.0f},  /* 4: 5 ch/s */
        {1, 6.0f},  /* 5: 6 ch/s */
        {1, 8.0f},  /* 6: 8 ch/s */
        {1, 10.0f}, /* 7: 10 ch/s */
        {1, 12.0f}, /* 8: 12 ch/s */
        {1, 14.0f}, /* 9: 14 ch/s */
    };
    int sp = morse_speed < 0 ? 0 : (morse_speed > 9 ? 9 : morse_speed);
    float duration;
    if (!morse_speeds[sp].is_cps) {
        float max_dur = morse_speeds[sp].val;
        duration = max_dur * (float)(text_len < 20 ? text_len : 20) / 20.0f;
        if (duration > max_dur) duration = max_dur;
    } else {
        duration = (float)text_len / morse_speeds[sp].val;
    }
    float unit_sec = duration / (float)units;
    int unit_samples = (int)(unit_sec * sr);
    if (unit_samples < 1) unit_samples = 1;
    int total = units * unit_samples;
    if (audio_buf) free(audio_buf);
    audio_buf = (float*)calloc(total, sizeof(float));
    audio_len = total; audio_pos = 0;
    /* render morse — record per-char sample start positions and sound flags */
    morse_vis_count = 0;
    int pos = 0;
    for (i = 0; i < text_len && pos < total; i++) {
        int idx = morse_vis_count;
        if (morse_vis_count < MORSE_VIS_MAX) {
            morse_vis_starts[morse_vis_count] = pos;
            morse_vis_has_sound[morse_vis_count] = 0;
            morse_vis_count++;
        }
        int ch = text[i];
        const char *code = NULL;
        if (ch >= 'A' && ch <= 'Z') code = MORSE[ch - 'A'];
        else if (ch >= 'a' && ch <= 'z') code = MORSE[ch - 'a'];
        else if (ch >= '0' && ch <= '9') code = MORSE[26 + ch - '0'];
        else if (ch == ' ') { pos += 7 * unit_samples; continue; }
        else if (ch > ' ') {
            /* non-morse printable: box drawing → minor scale, others → neutral */
            if (idx < MORSE_VIS_MAX) morse_vis_has_sound[idx] = 1;
            int tone_len = 2 * unit_samples;
            if (pos + tone_len > total) tone_len = total - pos;
            if (tone_len > 0) {
                if (is_box_char(ch)) {
                    int note = (ch - 0x2500) % BOX_SCALE_LEN;
                    SoundPreset bp = {"box", BOX_SCALE[note], 11.0f, 0.6f, 0.1f, 0.88f};
                    gen_fm_tone(audio_buf + pos, tone_len, sr, &bp, volume * 0.7f);
                } else {
                    SoundPreset sp = {"sym", 440.0f, 7.0f, 0.4f, 0.15f, 0.90f};
                    gen_fm_tone(audio_buf + pos, tone_len, sr, &sp, volume * 0.5f);
                }
            }
            pos += tone_len + 3 * unit_samples;
            continue;
        }
        else continue;
        if (!code) continue;
        if (idx < MORSE_VIS_MAX) morse_vis_has_sound[idx] = 1;
        int j;
        for (j = 0; code[j] && pos < total; j++) {
            int len = (code[j] == '.') ? unit_samples : 3 * unit_samples;
            if (pos + len > total) len = total - pos;
            gen_fm_tone(audio_buf + pos, len, sr, p, volume);
            pos += len + unit_samples;
        }
        pos += 2 * unit_samples;
    }
    audio_playing = 1;
}

/* generate a single micro-click for hover */
static void gen_hover_click(void) {
    if (hover_vol == 0) return;
    const SoundPreset *p = &PRESETS[hover_preset];
    float volume = VOL_LEVELS[hover_vol] * 0.5f; /* hover is quieter */
    int sr = 22050;
    int len = sr / 500; /* ~2ms */
    if (audio_playing) return; /* don't interrupt morse */
    if (audio_buf) free(audio_buf);
    audio_buf = (float*)calloc(len, sizeof(float));
    audio_len = len; audio_pos = 0;
    gen_fm_tone(audio_buf, len, sr, p, volume);
    morse_vis_count = 0; /* not a morse sound — don't trigger scan-line */
    audio_playing = 1;
}

/* play a short click for typing or screen output — cuts off any current sound */
static void gen_type_click(void) {
    if (type_vol == 0) return;
    const SoundPreset *p = &PRESETS[type_preset];
    float volume = VOL_LEVELS[type_vol] * 0.4f;
    int sr = 22050, len = sr / 300; /* ~3ms */
    if (audio_buf) free(audio_buf);
    audio_buf = (float*)calloc(len, sizeof(float));
    audio_len = len; audio_pos = 0;
    gen_fm_tone(audio_buf, len, sr, p, volume);
    morse_vis_count = 0;
    audio_playing = 1; /* cuts off any current sound */
}

static void gen_output_click(void) {
    if (output_vol == 0) return;
    const SoundPreset *p = &PRESETS[output_preset];
    float volume = VOL_LEVELS[output_vol] * 0.3f;
    int sr = 22050, len = sr / 400; /* ~2.5ms */
    if (audio_buf) free(audio_buf);
    audio_buf = (float*)calloc(len, sizeof(float));
    audio_len = len; audio_pos = 0;
    gen_fm_tone(audio_buf, len, sr, p, volume);
    morse_vis_count = 0;
    audio_playing = 1;
}

/* faint distant plink for cursor blink — barely perceptible */
static void gen_cursor_plink(void) {
    /* always play — it's so quiet it won't noticeably interrupt anything */
    float volume = 0.004f; /* barely there */
    int sr = 22050, len = sr / 100; /* ~10ms */
    if (audio_buf) free(audio_buf);
    audio_buf = (float*)calloc(len, sizeof(float));
    audio_len = len; audio_pos = 0;
    /* ultrasonic carrier aliases hard — most energy above hearing,
       just a felt click/presence. 10800Hz * mod_ratio 19 = sidebands at ~200kHz+ */
    SoundPreset plink = {"plink", 10800.0f, 19.0f, 2.5f, 0.8f, 0.65f};
    gen_fm_tone(audio_buf, len, sr, &plink, volume);
    morse_vis_count = 0;
    audio_playing = 1;
}

static void gen_delete_click(void) {
    if (delete_vol == 0) return;
    const SoundPreset *p = &PRESETS[delete_preset];
    float volume = VOL_LEVELS[delete_vol] * 0.35f;
    int sr = 22050, len = sr / 250; /* ~4ms — slightly longer than type click */
    if (audio_buf) free(audio_buf);
    audio_buf = (float*)calloc(len, sizeof(float));
    audio_len = len; audio_pos = 0;
    gen_fm_tone(audio_buf, len, sr, p, volume);
    morse_vis_count = 0;
    audio_playing = 1;
}

/* Generate reverse morse for selection deletion — plays the selected text backwards */
static void gen_reverse_morse(const int *text, int text_len) {
    if (morse_vol == 0) return;
    const SoundPreset *p = &PRESETS[morse_preset];
    float volume = VOL_LEVELS[morse_vol];
    int sr = 22050;
    /* count total morse units for the text */
    int units = 0, i;
    for (i = 0; i < text_len; i++) {
        int ch = text[i];
        const char *code = NULL;
        if (ch >= 'A' && ch <= 'Z') code = MORSE[ch - 'A'];
        else if (ch >= 'a' && ch <= 'z') code = MORSE[ch - 'a'];
        else if (ch >= '0' && ch <= '9') code = MORSE[26 + ch - '0'];
        else if (ch == ' ') { units += 7; continue; }
        else if (ch > ' ') { units += 2 + 3; continue; }
        else continue;
        if (!code) continue;
        int j;
        for (j = 0; code[j]; j++) {
            units += (code[j] == '.') ? 1 : 3;
            if (code[j+1]) units += 1;
        }
        units += 3;
    }
    if (units == 0) return;
    /* use super-fast speed: compress to 0.5s max */
    float duration = 0.5f * (float)(text_len < 20 ? text_len : 20) / 20.0f;
    if (duration > 0.5f) duration = 0.5f;
    if (duration < 0.05f) duration = 0.05f;
    float unit_sec = duration / (float)units;
    int unit_samples = (int)(unit_sec * sr);
    if (unit_samples < 1) unit_samples = 1;
    int morse_total = units * unit_samples;
    /* append electromechanical thunk: ~15ms low-freq FM burst */
    int thunk_len = sr / 65; /* ~15ms */
    int total = morse_total + thunk_len;
    if (audio_buf) free(audio_buf);
    audio_buf = (float*)calloc(total, sizeof(float));
    audio_len = total; audio_pos = 0;
    /* render morse REVERSED: iterate text backwards */
    morse_vis_count = 0;
    int pos = 0;
    for (i = text_len - 1; i >= 0 && pos < total; i--) {
        if (morse_vis_count < MORSE_VIS_MAX) {
            morse_vis_starts[morse_vis_count] = pos;
            morse_vis_has_sound[morse_vis_count] = 0;
            morse_vis_count++;
        }
        int ch = text[i];
        const char *code = NULL;
        if (ch >= 'A' && ch <= 'Z') code = MORSE[ch - 'A'];
        else if (ch >= 'a' && ch <= 'z') code = MORSE[ch - 'a'];
        else if (ch >= '0' && ch <= '9') code = MORSE[26 + ch - '0'];
        else if (ch == ' ') { pos += 7 * unit_samples; continue; }
        else if (ch > ' ') {
            if (morse_vis_count > 0) morse_vis_has_sound[morse_vis_count-1] = 1;
            int tone_len = 2 * unit_samples;
            if (pos + tone_len > total) tone_len = total - pos;
            if (tone_len > 0) {
                if (is_box_char(ch)) {
                    int note = (ch - 0x2500) % BOX_SCALE_LEN;
                    SoundPreset bp = {"box", BOX_SCALE[note], 11.0f, 0.6f, 0.1f, 0.88f};
                    gen_fm_tone(audio_buf + pos, tone_len, sr, &bp, volume * 0.7f);
                } else {
                    SoundPreset sp2 = {"sym", 440.0f, 7.0f, 0.4f, 0.15f, 0.90f};
                    gen_fm_tone(audio_buf + pos, tone_len, sr, &sp2, volume * 0.5f);
                }
            }
            pos += tone_len + 3 * unit_samples;
            continue;
        }
        else continue;
        if (!code) continue;
        if (morse_vis_count > 0) morse_vis_has_sound[morse_vis_count-1] = 1;
        int j;
        for (j = 0; code[j] && pos < total; j++) {
            int len = (code[j] == '.') ? unit_samples : 3 * unit_samples;
            if (pos + len > total) len = total - pos;
            gen_fm_tone(audio_buf + pos, len, sr, p, volume);
            pos += len + unit_samples;
        }
        pos += 2 * unit_samples;
    }
    /* electromechanical thunk: low carrier, heavy modulation, fast decay + noise burst */
    {
        SoundPreset thunk = {"thunk", 120.0f, 15.0f, 2.5f, 0.45f, 0.75f};
        int tstart = morse_total;
        if (tstart + thunk_len > total) thunk_len = total - tstart;
        gen_fm_tone(audio_buf + tstart, thunk_len, sr, &thunk, volume * 1.2f);
    }
    audio_playing = 1;
}

/* ── output sonification ───────────────────────────────────────── */
/* Track chars as they appear for tonal output + scan line visualization */
#define OUTPUT_BUF_MAX 2048
static int  output_buf_ch[OUTPUT_BUF_MAX];   /* codepoint */
static int  output_buf_col[OUTPUT_BUF_MAX];  /* screen column */
static int  output_buf_row[OUTPUT_BUF_MAX];  /* screen row */
static int  output_buf_count;                /* chars this frame */

/* deletion burn-in: stores ghost of deleted character + decay brightness */
#define DEL_GHOST_MAX 512
typedef struct { int col, row, ch; unsigned char fg[3]; float bright; } DelGhost;
static DelGhost del_ghosts[DEL_GHOST_MAX];
static int del_ghost_count;
/* decay: 2 seconds at 60fps → 0.9917 per frame */
#define DEL_GHOST_DECAY 0.9917f

/* Generate tonal output sonification for chars that appeared this frame */
static void gen_output_tones(void) {
    if (output_vol == 0 || output_buf_count == 0) return;
    if (audio_playing) return; /* don't interrupt morse or other sounds */
    float volume = VOL_LEVELS[output_vol] * 0.15f;
    int sr = 22050;
    int n = output_buf_count;
    if (n > 200) n = 200; /* cap */
    /* duration: ~2ms per char, 0.05s min, 0.4s max */
    float dur = (float)n * 0.002f;
    if (dur < 0.05f) dur = 0.05f;
    if (dur > 0.4f) dur = 0.4f;
    int total = (int)(dur * sr);
    if (total < 1) total = 1;
    if (audio_buf) free(audio_buf);
    audio_buf = (float*)calloc(total, sizeof(float));
    audio_len = total; audio_pos = 0;

    /* set up visualization */
    morse_vis_count = n < MORSE_VIS_MAX ? n : MORSE_VIS_MAX;
    int samples_per_char = total / n;
    if (samples_per_char < 1) samples_per_char = 1;

    int pos = 0, i;
    for (i = 0; i < n && pos < total; i++) {
        if (i < MORSE_VIS_MAX) {
            morse_vis_starts[i] = pos;
            morse_vis_cols[i] = output_buf_col[i];
            morse_vis_rows[i] = output_buf_row[i];
            morse_vis_has_sound[i] = 1;
        }
        int ch = output_buf_ch[i];
        int tone_len = samples_per_char;
        if (pos + tone_len > total) tone_len = total - pos;
        if (tone_len <= 0) break;

        if (is_box_char(ch)) {
            /* box drawing: minor scale tone, pick note from codepoint */
            int note_idx = (ch - 0x2500) % BOX_SCALE_LEN;
            float freq = BOX_SCALE[note_idx];
            /* generate a quick FM tone at this pitch */
            SoundPreset bp = {"box", freq, 11.0f, 0.6f, 0.1f, 0.88f};
            gen_fm_tone(audio_buf + pos, tone_len, sr, &bp, volume * 1.5f);
        } else if (ch > ' ') {
            /* regular text: soft low click */
            SoundPreset tp = {"text", 300.0f, 5.0f, 0.3f, 0.05f, 0.92f};
            gen_fm_tone(audio_buf + pos, tone_len, sr, &tp, volume * 0.5f);
        }
        pos += samples_per_char;
    }
    memset(morse_trail, 0, sizeof(morse_trail));
    audio_playing = 1;
}

/* dynamic terminal size (updated on window resize) */
static int term_cols = 120, term_rows = 40;
static int resize_pending;
static void apply_resize(int new_cols, int new_rows); /* forward decl */
static void to_clipboard(const char *text, int len); /* forward decl */

/* ── text selection ────────────────────────────────────────────── */
/* mode 0=none, 1=regular (character), 2=box (rectangle)           */
static int sel_mode;
static int sel_start_col, sel_start_row;
static int sel_end_col, sel_end_row;
static int sel_dragging;

/* last non-blank column on a row (-1 if entirely blank) */
static int row_last_char(int r) {
    if (r < 0 || r >= TERM_ROWS) return -1;
    int c;
    for (c = term_cols - 1; c >= 0; c--)
        if (grid[r][c].ch != ' ' && grid[r][c].ch != 0) return c;
    return -1;
}

/* Selection containment returns 0, 1, or 2:
   0 = not selected
   1 = in the outer/universal selection range (half brightness)
   2 = in the inner/populated selection (full brightness)
   Box mode always returns 2 for the full rectangle. */
static int sel_contains(int c, int r) {
    if (!sel_mode) return 0;
    if (r < 0 || r >= term_rows || c < 0 || c >= term_cols) return 0;
    if (sel_mode == 2) {
        /* box: full rectangle, always full brightness */
        int c0 = sel_start_col < sel_end_col ? sel_start_col : sel_end_col;
        int c1 = sel_start_col > sel_end_col ? sel_start_col : sel_end_col;
        int r0 = sel_start_row < sel_end_row ? sel_start_row : sel_end_row;
        int r1 = sel_start_row > sel_end_row ? sel_start_row : sel_end_row;
        return (c >= c0 && c <= c1 && r >= r0 && r <= r1) ? 2 : 0;
    }
    /* regular mode: check linear range */
    int s = sel_start_row * term_cols + sel_start_col;
    int e = sel_end_row * term_cols + sel_end_col;
    if (s > e) { int t = s; s = e; e = t; }
    int p = r * term_cols + c;
    if (p < s || p > e) return 0;
    /* this cell is in the universal range → at least level 1 */
    /* check if it's within populated content → level 2 */
    int last = row_last_char(r);
    if (last < 0) return 1; /* blank row: outer only */
    if (c <= last) return 2; /* has content: full */
    return 1; /* past end of content: outer */
}

static void sel_clear(void) { sel_mode = 0; sel_dragging = 0; }

/* play morse/tonal audio for current selection text, recording screen positions */
static void sel_play_morse(void) {
    if (!sel_mode) return;
    /* extract selected codepoints + screen positions */
    int buf[1024]; int pos = 0;
    int scol[1024], srow[1024];
    int r0, r1, c0, c1;
    if (sel_mode == 2) {
        c0 = sel_start_col < sel_end_col ? sel_start_col : sel_end_col;
        c1 = sel_start_col > sel_end_col ? sel_start_col : sel_end_col;
        r0 = sel_start_row < sel_end_row ? sel_start_row : sel_end_row;
        r1 = sel_start_row > sel_end_row ? sel_start_row : sel_end_row;
    } else {
        int s = sel_start_row * term_cols + sel_start_col;
        int e = sel_end_row * term_cols + sel_end_col;
        if (s > e) { int t=s; s=e; e=t; }
        r0 = s / term_cols; c0 = s % term_cols;
        r1 = e / term_cols; c1 = e % term_cols;
    }
    int r, c;
    for (r = r0; r <= r1 && pos < 1000; r++) {
        int cs = (sel_mode==2) ? c0 : (r==r0 ? c0 : 0);
        int ce = (sel_mode==2) ? c1 : (r==r1 ? c1 : term_cols-1);
        for (c = cs; c <= ce && c < term_cols && pos < 1000; c++) {
            int ch = grid[r][c].ch;
            if (ch > ' ') { scol[pos] = c; srow[pos] = r; buf[pos++] = ch; }
        }
    }
    if (pos > 0) {
        gen_morse_audio(buf, pos);
        /* copy screen positions into morse_vis arrays (gen_morse_audio set morse_vis_count) */
        int i;
        for (i = 0; i < morse_vis_count && i < pos; i++) {
            morse_vis_cols[i] = scol[i];
            morse_vis_rows[i] = srow[i];
        }
        /* clear trail */
        memset(morse_trail, 0, sizeof(morse_trail));
    }
}

/* Play reverse morse for current selection (deletion sound) */
static void sel_play_reverse_morse(void) {
    if (!sel_mode) return;
    int buf[1024]; int pos = 0;
    int scol[1024], srow[1024];
    int r0, r1, c0, c1;
    if (sel_mode == 2) {
        c0 = sel_start_col < sel_end_col ? sel_start_col : sel_end_col;
        c1 = sel_start_col > sel_end_col ? sel_start_col : sel_end_col;
        r0 = sel_start_row < sel_end_row ? sel_start_row : sel_end_row;
        r1 = sel_start_row > sel_end_row ? sel_start_row : sel_end_row;
    } else {
        int s = sel_start_row * term_cols + sel_start_col;
        int e = sel_end_row * term_cols + sel_end_col;
        if (s > e) { int t=s; s=e; e=t; }
        r0 = s / term_cols; c0 = s % term_cols;
        r1 = e / term_cols; c1 = e % term_cols;
    }
    int r, c;
    for (r = r0; r <= r1 && pos < 1000; r++) {
        int cs = (sel_mode==2) ? c0 : (r==r0 ? c0 : 0);
        int ce = (sel_mode==2) ? c1 : (r==r1 ? c1 : term_cols-1);
        for (c = cs; c <= ce && c < term_cols && pos < 1000; c++) {
            int ch = grid[r][c].ch;
            if (ch > ' ') { scol[pos] = c; srow[pos] = r; buf[pos++] = ch; }
        }
    }
    if (pos > 0) {
        gen_reverse_morse(buf, pos);
        int i;
        for (i = 0; i < morse_vis_count && i < pos; i++) {
            int ri = pos - 1 - i;
            morse_vis_cols[i] = scol[ri < 0 ? 0 : ri];
            morse_vis_rows[i] = srow[ri < 0 ? 0 : ri];
        }
        memset(morse_trail, 0, sizeof(morse_trail));
        /* snapshot deleted characters as burn-in ghosts */
        for (i = 0; i < pos && del_ghost_count < DEL_GHOST_MAX; i++) {
            int sc = scol[i], sr2 = srow[i];
            if (sr2 < 0 || sr2 >= TERM_ROWS || sc < 0 || sc >= TERM_COLS) continue;
            Cell *gcl = &grid[sr2][sc];
            if (gcl->ch <= ' ') continue;
            DelGhost *dg = &del_ghosts[del_ghost_count++];
            dg->col = sc; dg->row = sr2; dg->ch = gcl->ch;
            unsigned char *gfg = gcl->inv ? gcl->bg : gcl->fg;
            memcpy(dg->fg, gfg, 3);
            dg->bright = 0.7f;
        }
    }
}

/* Smart edit: delete selected text by sending arrow keys + backspaces to child.
   Works for single-line (on cursor row) and multi-line (Claude Code input region
   delimited by ─── horizontal rules). */
static void sel_delete_via_keys(void (*send_fn)(const char*, int)) {
    if (!sel_mode || sel_mode == 2) return;
    int s_col = sel_start_col, s_row = sel_start_row;
    int e_col = sel_end_col, e_row = sel_end_row;
    if (s_row > e_row || (s_row == e_row && s_col > e_col)) {
        int t; t=s_col; s_col=e_col; e_col=t; t=s_row; s_row=e_row; e_row=t;
    }

    if (s_row == e_row) {
        /* single-line: must be on cursor row */
        if (s_row != cur_y) return;
        int last = row_last_char(s_row);
        if (last < 0) return;
        if (e_col > last) e_col = last;
        if (s_col > last) return;
        int sel_len = e_col - s_col + 1;
        if (sel_len <= 0) return;
        int target = e_col + 1;
        int moves = target - cur_x;
        int i;
        if (moves > 0) for (i = 0; i < moves; i++) send_fn("\x1b[C", 3);
        else if (moves < 0) for (i = 0; i < -moves; i++) send_fn("\x1b[D", 3);
        for (i = 0; i < sel_len; i++) send_fn("\x7f", 1);
    } else {
        /* multi-line: cursor must be within the selection row range */
        if (cur_y < s_row || cur_y > e_row) return;
        /* count total content chars across all selected rows */
        int total_chars = 0, r, c;
        for (r = s_row; r <= e_row; r++) {
            int cs = (r == s_row) ? s_col : 0;
            int ce = (r == e_row) ? e_col : term_cols - 1;
            int last = row_last_char(r);
            if (last < 0) continue;
            if (ce > last) ce = last;
            if (cs > last) continue;
            total_chars += ce - cs + 1;
            if (r < e_row) total_chars++; /* +1 for the newline/line-join between rows */
        }
        if (total_chars <= 0) return;
        /* move cursor to end of selection: go to e_row, e_col+1 */
        int i;
        /* first move down to e_row */
        while (cur_y < e_row) { send_fn("\x1b[B", 3); cur_y++; }
        while (cur_y > e_row) { send_fn("\x1b[A", 3); cur_y--; }
        /* then move to e_col+1 */
        int last_e = row_last_char(e_row);
        int target = (last_e >= 0 && e_col > last_e) ? last_e + 1 : e_col + 1;
        int moves = target - cur_x;
        if (moves > 0) for (i = 0; i < moves; i++) send_fn("\x1b[C", 3);
        else if (moves < 0) for (i = 0; i < -moves; i++) send_fn("\x1b[D", 3);
        /* send backspaces — enough to delete all content + line joins */
        for (i = 0; i < total_chars; i++) send_fn("\x7f", 1);
    }
}

static void sel_copy(void) {
    if (!sel_mode) return;
    int r0, r1, c0, c1;
    if (sel_mode == 2) {
        c0 = sel_start_col < sel_end_col ? sel_start_col : sel_end_col;
        c1 = sel_start_col > sel_end_col ? sel_start_col : sel_end_col;
        r0 = sel_start_row < sel_end_row ? sel_start_row : sel_end_row;
        r1 = sel_start_row > sel_end_row ? sel_start_row : sel_end_row;
    } else {
        int s = sel_start_row * term_cols + sel_start_col;
        int e = sel_end_row * term_cols + sel_end_col;
        if (s > e) { int t = s; s = e; e = t; }
        r0 = s / term_cols; c0 = s % term_cols;
        r1 = e / term_cols; c1 = e % term_cols;
    }
    char *buf = (char*)malloc(term_cols * (r1 - r0 + 1) * 4 + (r1 - r0 + 1) + 1);
    int pos = 0, r, c;
    for (r = r0; r <= r1; r++) {
        int line_start = (sel_mode == 2) ? c0 : (r == r0 ? c0 : 0);
        int line_end = (sel_mode == 2) ? c1 : (r == r1 ? c1 : term_cols - 1);
        /* trim trailing spaces */
        int last = line_start - 1;
        for (c = line_start; c <= line_end && c < term_cols; c++)
            if (grid[r][c].ch != ' ' && grid[r][c].ch != 0) last = c;
        for (c = line_start; c <= last; c++) {
            unsigned short ch = grid[r][c].ch;
            if (ch == 0) ch = ' ';
            if (ch < 0x80) buf[pos++] = (char)ch;
            else if (ch < 0x800) { buf[pos++] = 0xC0|(ch>>6); buf[pos++] = 0x80|(ch&0x3F); }
            else { buf[pos++] = 0xE0|(ch>>12); buf[pos++] = 0x80|((ch>>6)&0x3F); buf[pos++] = 0x80|(ch&0x3F); }
        }
        if (r < r1) buf[pos++] = '\n';
    }
    to_clipboard(buf, pos);
    free(buf);
}

static void glow_update(void) {
    /* decay all — flat loops, no branches */
    int i;
    float *p = &glow[0][0];
    for (i = 0; i < TERM_ROWS * TERM_COLS; i++) p[i] *= GLOW_DECAY;
    for (i = 0; i < TERM_ROWS; i++) row_glow[i] *= GLOW_DECAY;
    for (i = 0; i < TERM_COLS; i++) col_glow[i] *= GLOW_DECAY;
    /* set hovered cell + row + col to full */
    if (mouse_row >= 0 && mouse_row < TERM_ROWS && mouse_col >= 0 && mouse_col < TERM_COLS) {
        glow[mouse_row][mouse_col] = 1.0f;
        row_glow[mouse_row] = 1.0f;
        col_glow[mouse_col] = 1.0f;
        /* hover click when entering a new cell */
        if (mouse_col != prev_hover_col || mouse_row != prev_hover_row) {
            gen_hover_click();
            prev_hover_col = mouse_col; prev_hover_row = mouse_row;
        }
    }
}

/* ── smooth scrolling state ────────────────────────────────────── */
static float scroll_y;          /* current scroll offset in pixels (0 = bottom/live) */
static float scroll_vel;        /* pixels/frame velocity */
static float scroll_target;     /* target position for spring snap */
static int   scroll_snapping;   /* 1 = animating toward target */
static int   scroll_locked;     /* 1 = locked to bottom (auto-scroll with new output) */
#define SCROLL_FRICTION  0.92f  /* velocity decay per frame */
#define SCROLL_IMPULSE   (FONT_H * 3.0f)  /* pixels per scroll tick */
#define SCROLL_SPRING_K  0.15f  /* spring constant for snap */
#define SCROLL_SPRING_D  0.55f  /* spring damping */
#define SCROLL_MIN_VEL   0.1f   /* stop threshold */

static void grid_clear(void) {
    int r, c;
    for (r = 0; r < TERM_ROWS; r++)
        for (c = 0; c < TERM_COLS; c++) {
            Cell *cl = &grid[r][c];
            cl->ch = ' '; memcpy(cl->fg, pen_fg, 3); memcpy(cl->bg, pen_bg, 3);
            cl->bold = cl->inv = 0;
        }
    cur_x = cur_y = 0;
}

static void grid_scroll_up(int n) {
    int r, c;
    if (n >= term_rows) { grid_clear(); return; }
    /* save scrolled-off lines to history */
    for (r = 0; r < n; r++) {
        memcpy(history[hist_write], grid[r], TERM_COLS * sizeof(Cell));
        hist_write = (hist_write + 1) % HIST_LINES;
        if (hist_count < HIST_LINES) hist_count++;
    }
    memmove(grid[0], grid[n], (term_rows-n)*sizeof(grid[0]));
    for (r = term_rows-n; r < term_rows; r++)
        for (c = 0; c < TERM_COLS; c++) {
            grid[r][c].ch = ' '; memcpy(grid[r][c].fg, pen_fg, 3);
            memcpy(grid[r][c].bg, pen_bg, 3); grid[r][c].bold = grid[r][c].inv = 0;
        }
}

static void put_char(int ch) {
    if (cur_x >= TERM_COLS) { cur_x = 0; cur_y++; }
    if (cur_y >= term_rows) { grid_scroll_up(1); cur_y = term_rows-1; }
    Cell *cl = &grid[cur_y][cur_x];
    cl->ch = ch; memcpy(cl->fg, pen_fg, 3); memcpy(cl->bg, pen_bg, 3);
    cl->bold = pen_bold; cl->inv = pen_inv;
    if (ch > ' ' && output_buf_count < OUTPUT_BUF_MAX) {
        output_buf_ch[output_buf_count] = ch;
        output_buf_col[output_buf_count] = cur_x;
        output_buf_row[output_buf_count] = cur_y;
        output_buf_count++;
    }
    cur_x++;
}

static void erase_line(int mode) {
    int c, s = 0, e = TERM_COLS;
    if (mode == 0) s = cur_x; else if (mode == 1) e = cur_x+1;
    for (c = s; c < e; c++) { grid[cur_y][c].ch=' '; memcpy(grid[cur_y][c].fg,pen_fg,3); memcpy(grid[cur_y][c].bg,pen_bg,3); }
}

static void erase_display(int mode) {
    int r, c;
    if (mode == 0) { erase_line(0); for (r=cur_y+1;r<term_rows;r++) for(c=0;c<TERM_COLS;c++){grid[r][c].ch=' ';memcpy(grid[r][c].fg,pen_fg,3);memcpy(grid[r][c].bg,pen_bg,3);} }
    else if (mode==2||mode==3) { for(r=0;r<term_rows;r++) for(c=0;c<TERM_COLS;c++){grid[r][c].ch=' ';memcpy(grid[r][c].fg,pen_fg,3);memcpy(grid[r][c].bg,pen_bg,3);} }
}

/* ── VT parser ─────────────────────────────────────────────────── */

enum { VT_GROUND, VT_ESC, VT_CSI, VT_OSC, VT_STR, VT_ST };
static int vt_state;
static int vt_params[MAX_CSI], vt_nparam, vt_private, vt_intermed;
static int utf8_acc, utf8_rem;

static int csi_p(int i, int def) { return (i < vt_nparam && vt_params[i]) ? vt_params[i] : def; }

static void exec_sgr(void) {
    int i;
    if (vt_nparam == 0) { pen_fg[0]=pen_fg[1]=pen_fg[2]=192; pen_bg[0]=pen_bg[1]=pen_bg[2]=0; pen_bold=pen_inv=0; return; }
    for (i = 0; i < vt_nparam; i++) {
        int p = vt_params[i];
        if (p==0) { pen_fg[0]=pen_fg[1]=pen_fg[2]=192; pen_bg[0]=pen_bg[1]=pen_bg[2]=0; pen_bold=pen_inv=0; }
        else if (p==1) pen_bold=1;
        else if (p==7) pen_inv=1;
        else if (p==22) pen_bold=0;
        else if (p==27) pen_inv=0;
        else if (p>=30&&p<=37) color256(p-30, pen_fg);
        else if (p>=40&&p<=47) color256(p-40, pen_bg);
        else if (p>=90&&p<=97) color256(p-90+8, pen_fg);
        else if (p>=100&&p<=107) color256(p-100+8, pen_bg);
        else if (p==39) { pen_fg[0]=pen_fg[1]=pen_fg[2]=192; }
        else if (p==49) { pen_bg[0]=pen_bg[1]=pen_bg[2]=0; }
        else if (p==38 && i+1<vt_nparam) {
            if (vt_params[i+1]==2 && i+4<vt_nparam) { pen_fg[0]=vt_params[i+2]; pen_fg[1]=vt_params[i+3]; pen_fg[2]=vt_params[i+4]; i+=4; }
            else if (vt_params[i+1]==5 && i+2<vt_nparam) { color256(vt_params[i+2], pen_fg); i+=2; }
        }
        else if (p==48 && i+1<vt_nparam) {
            if (vt_params[i+1]==2 && i+4<vt_nparam) { pen_bg[0]=vt_params[i+2]; pen_bg[1]=vt_params[i+3]; pen_bg[2]=vt_params[i+4]; i+=4; }
            else if (vt_params[i+1]==5 && i+2<vt_nparam) { color256(vt_params[i+2], pen_bg); i+=2; }
        }
    }
}

static void exec_csi(int final) {
    int n;
    switch (final) {
    case 'A': cur_y -= csi_p(0,1); if(cur_y<0) cur_y=0; break;
    case 'B': cur_y += csi_p(0,1); if(cur_y>=term_rows) cur_y=term_rows-1; break;
    case 'C': cur_x += csi_p(0,1); if(cur_x>=TERM_COLS) cur_x=TERM_COLS-1; break;
    case 'D': cur_x -= csi_p(0,1); if(cur_x<0) cur_x=0; break;
    case 'H': case 'f':
        cur_y = csi_p(0,1)-1; cur_x = csi_p(1,1)-1;
        if(cur_y<0)cur_y=0; if(cur_y>=term_rows)cur_y=term_rows-1;
        if(cur_x<0)cur_x=0; if(cur_x>=TERM_COLS)cur_x=TERM_COLS-1; break;
    case 'J': erase_display(csi_p(0,0)); break;
    case 'K': erase_line(csi_p(0,0)); break;
    case 'S': grid_scroll_up(csi_p(0,1)); break;
    case 'm': exec_sgr(); break;
    case 'h': if(vt_private) { n=csi_p(0,0); if(n==25)cur_vis=1; } break;
    case 'l': if(vt_private) { n=csi_p(0,0); if(n==25)cur_vis=0; } break;
    }
}

static void vt_ground(int ch) {
    if (utf8_rem > 0) {
        if ((ch & 0xC0) == 0x80) { utf8_acc = (utf8_acc<<6)|(ch&0x3F); utf8_rem--; if(utf8_rem==0) put_char(utf8_acc); return; }
        utf8_rem = 0;
    }
    if (ch >= 0x20 && ch < 0x7F) put_char(ch);
    else if ((ch & 0xE0) == 0xC0) { utf8_acc = ch & 0x1F; utf8_rem = 1; }
    else if ((ch & 0xF0) == 0xE0) { utf8_acc = ch & 0x0F; utf8_rem = 2; }
    else if ((ch & 0xF8) == 0xF0) { utf8_acc = ch & 0x07; utf8_rem = 3; }
    else switch (ch) {
        case '\n': cur_y++; if(cur_y>=term_rows){grid_scroll_up(1);cur_y=term_rows-1;} break;
        case '\r': cur_x = 0; break;
        case '\t': cur_x=(cur_x+8)&~7; if(cur_x>=TERM_COLS)cur_x=TERM_COLS-1; break;
        case '\b': if(cur_x>0)cur_x--; break;
        case 0x1B: vt_state = VT_ESC; break;
    }
}

static void vt_feed(const unsigned char *data, int len) {
    int i;
    for (i = 0; i < len; i++) {
        int ch = data[i];
        switch (vt_state) {
        case VT_GROUND: vt_ground(ch); break;
        case VT_ESC:
            if (ch=='[') { vt_state=VT_CSI; vt_nparam=0; vt_params[0]=0; vt_private=0; vt_intermed=0; }
            else if (ch==']') vt_state=VT_OSC;
            else if (ch=='P'||ch=='^'||ch=='_') vt_state=VT_STR;
            else vt_state=VT_GROUND;
            break;
        case VT_CSI:
            if (ch>='<'&&ch<='?') vt_private=ch;
            else if (ch>='0'&&ch<='9') vt_params[vt_nparam]=vt_params[vt_nparam]*10+ch-'0';
            else if (ch==';') { if(vt_nparam<MAX_CSI-1){vt_nparam++;vt_params[vt_nparam]=0;} }
            else if (ch>=' '&&ch<='/') vt_intermed=ch;
            else if (ch>='@'&&ch<='~') {
                vt_nparam++;
                if (!vt_intermed && (vt_private==0||vt_private=='?')) exec_csi(ch);
                vt_state=VT_GROUND;
            } else vt_state=VT_GROUND;
            break;
        case VT_OSC: if(ch==7) vt_state=VT_GROUND; else if(ch==0x1B) vt_state=VT_ST; break;
        case VT_STR: if(ch==0x1B) vt_state=VT_ST; break;
        case VT_ST: vt_state=VT_GROUND; break;
        }
    }
}

/* ── GL rendering ──────────────────────────────────────────────── */

static unsigned char atlas_tex[UNIFONT_COUNT * FONT_H]; /* 1-bit packed: UNIFONT_COUNT × 16 bytes */
static unsigned int gl_font_tex;

static void build_atlas_texture(void) {
    /* Build a texture atlas: UNIFONT_COUNT glyphs in a strip, each 8×16 */
    int aW = FONT_W * UNIFONT_COUNT;
    int aH = FONT_H;
    /* Use a 2D layout instead: 32 cols */
    int acols = 32;
    int arows = (UNIFONT_COUNT + acols - 1) / acols;
    aW = acols * FONT_W;
    aH = arows * FONT_H;

    unsigned char *tex = (unsigned char *)calloc(aW * aH, 1);
    int i;
    for (i = 0; i < UNIFONT_COUNT; i++) {
        int gx = (i % acols) * FONT_W;
        int gy = (i / acols) * FONT_H;
        int y, x;
        for (y = 0; y < FONT_H; y++) {
            unsigned char row = UNIFONT[i].data[y];
            for (x = 0; x < FONT_W; x++) {
                if (row & (0x80 >> x))
                    tex[(gy+y)*aW + gx+x] = 255;
            }
        }
    }

    /* Upload as GL_ALPHA texture */
    /* (GL calls happen after context is created) */
    /* Store for later upload */
    /* Actually we need GL here — store the pixel data and upload in gl_init */
    free(tex);
}

/* ── networking (cross-platform) ───────────────────────────────── */

static void net_set_nonblock(sock_t s) {
#ifdef _WIN32
    u_long mode = 1; ioctlsocket(s, FIONBIO, &mode);
#else
    int fl = fcntl(s, F_GETFL); fcntl(s, F_SETFL, fl | O_NONBLOCK);
#endif
}

/* Start listening socket (non-blocking accept) */
static int net_listen_start(const char *addr) {
    sock_init();
    char host[256] = "0.0.0.0";
    int port = 9124;
    const char *colon = strchr(addr, ':');
    if (colon) { memcpy(host, addr, colon-addr); host[colon-addr]=0; port=atoi(colon+1); }
    else port = atoi(addr);

    sock_t srv = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1; setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));
    struct sockaddr_in sa; memset(&sa,0,sizeof(sa));
    sa.sin_family = AF_INET; sa.sin_port = htons(port);
    sa.sin_addr.s_addr = inet_addr(host);
    if (bind(srv, (struct sockaddr*)&sa, sizeof(sa)) < 0) { perror("bind"); return 1; }
    listen(srv, MAX_CLIENTS);
    net_set_nonblock(srv);
    g_listen_sock = srv;
    fprintf(stderr, "dumbterm: listening on %s:%d (up to %d clients)\n", host, port, MAX_CLIENTS);
    return 0;
}

/* Accept new clients (non-blocking) */
/* Send current grid state to a socket as VT escape sequences */
static void net_send_grid_state(sock_t s) {
    /* clear screen, home cursor */
    send(s, "\x1b[2J\x1b[H", 7, 0);
    int r, c;
    unsigned char last_fg[3] = {192,192,192}, last_bg[3] = {0,0,0};
    int last_bold = 0, last_inv = 0;
    char esc[64];
    for (r = 0; r < term_rows; r++) {
        /* position cursor at start of row */
        int len = sprintf(esc, "\x1b[%d;1H", r+1);
        send(s, esc, len, 0);
        for (c = 0; c < term_cols; c++) {
            Cell *cl = &grid[r][c];
            /* emit SGR if attributes changed */
            if (cl->bold != last_bold || cl->inv != last_inv ||
                memcmp(cl->fg, last_fg, 3) != 0 || memcmp(cl->bg, last_bg, 3) != 0) {
                len = sprintf(esc, "\x1b[0;%s%s38;2;%d;%d;%d;48;2;%d;%d;%dm",
                    cl->bold ? "1;" : "", cl->inv ? "7;" : "",
                    cl->fg[0], cl->fg[1], cl->fg[2],
                    cl->bg[0], cl->bg[1], cl->bg[2]);
                send(s, esc, len, 0);
                memcpy(last_fg, cl->fg, 3); memcpy(last_bg, cl->bg, 3);
                last_bold = cl->bold; last_inv = cl->inv;
            }
            /* emit character as UTF-8 */
            int ch = cl->ch;
            if (ch < 0x20 || ch == 0) ch = ' ';
            char utf[4]; int ulen = 0;
            if (ch < 0x80) { utf[0] = ch; ulen = 1; }
            else if (ch < 0x800) { utf[0] = 0xC0|(ch>>6); utf[1] = 0x80|(ch&0x3F); ulen = 2; }
            else { utf[0] = 0xE0|(ch>>12); utf[1] = 0x80|((ch>>6)&0x3F); utf[2] = 0x80|(ch&0x3F); ulen = 3; }
            send(s, utf, ulen, 0);
        }
    }
    /* reset SGR, position cursor */
    int flen = sprintf(esc, "\x1b[0m\x1b[%d;%dH", cur_y+1, cur_x+1);
    send(s, esc, flen, 0);
    if (!cur_vis) send(s, "\x1b[?25l", 6, 0);
}

static void net_accept_clients(void) {
    if (g_listen_sock == SOCK_INVALID) return;
    struct sockaddr_in ca; int calen = sizeof(ca);
    sock_t c = accept(g_listen_sock, (struct sockaddr*)&ca, (void*)&calen);
    if (c == SOCK_INVALID) return;
    if (g_num_clients >= MAX_CLIENTS) { sock_close(c); return; }
    net_set_nonblock(c);
    g_clients[g_num_clients++] = c;
    fprintf(stderr, "dumbterm: client %d connected (%d total)\n", g_num_clients, g_num_clients);
    /* send current screen state so client isn't blank */
    net_send_grid_state(c);
}

/* Broadcast bytes to all connected clients */
static void net_broadcast(const char *data, int len) {
    int i;
    for (i = 0; i < g_num_clients; i++) {
        if (send(g_clients[i], data, len, 0) <= 0) {
            /* client disconnected — remove */
            fprintf(stderr, "dumbterm: client %d disconnected\n", i+1);
            sock_close(g_clients[i]);
            g_clients[i] = g_clients[--g_num_clients];
            i--;
        }
    }
}

/* Filter _RESIZE sequences from network input, forward the rest.
   Scans buf for ESC _ RESIZE;cols;rows ESC \ and calls resize_fn on match.
   Returns filtered buffer with _RESIZE removed, forwarding the rest to send_fn. */
static void net_filter_and_forward(const unsigned char *buf, int len,
    void (*send_fn)(const char*, int),
    void (*resize_fn)(int cols, int rows))
{
    int i = 0, flush_from = 0;
    while (i < len) {
        if (buf[i] == 0x1b && i+1 < len && buf[i+1] == '_') {
            /* possible APC sequence — look for RESIZE;cols;rows ESC \ */
            int j = i + 2;
            if (j + 7 <= len && memcmp(buf+j, "RESIZE;", 7) == 0) {
                j += 7;
                int cols = 0, rows = 0;
                while (j < len && buf[j] >= '0' && buf[j] <= '9') cols = cols*10 + (buf[j++]-'0');
                if (j < len && buf[j] == ';') j++;
                while (j < len && buf[j] >= '0' && buf[j] <= '9') rows = rows*10 + (buf[j++]-'0');
                /* expect ESC \ as terminator */
                if (j+1 < len && buf[j] == 0x1b && buf[j+1] == '\\') {
                    j += 2;
                    /* flush bytes before this sequence */
                    if (i > flush_from) send_fn((char*)buf+flush_from, i-flush_from);
                    /* handle resize */
                    if (cols > 0 && rows > 0 && resize_fn) resize_fn(cols, rows);
                    flush_from = j;
                    i = j;
                    continue;
                }
            }
        }
        i++;
    }
    /* flush remaining */
    if (flush_from < len) send_fn((char*)buf+flush_from, len-flush_from);
}

/* Read input from all clients, merge into one stream */
static void net_read_all_clients(void (*send_fn)(const char*, int), void (*resize_fn)(int, int)) {
    unsigned char buf[4096];
    int i;
    for (i = 0; i < g_num_clients; i++) {
        int n = recv(g_clients[i], (char*)buf, sizeof(buf), 0);
        if (n > 0) net_filter_and_forward(buf, n, send_fn, resize_fn);
        else if (n == 0) {
            fprintf(stderr, "dumbterm: client %d disconnected\n", i+1);
            sock_close(g_clients[i]);
            g_clients[i] = g_clients[--g_num_clients];
            i--;
        }
    }
}

/* Legacy: blocking accept for headless single-client (backward compat) */
static int net_serve(const char *addr, const char *child_cmd) {
    sock_init();
    char host[256] = "0.0.0.0";
    int port = 9124;
    const char *colon = strchr(addr, ':');
    if (colon) { memcpy(host, addr, colon-addr); host[colon-addr]=0; port=atoi(colon+1); }
    else port = atoi(addr);

    sock_t srv = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1; setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));
    struct sockaddr_in sa; memset(&sa,0,sizeof(sa));
    sa.sin_family = AF_INET; sa.sin_port = htons(port);
    sa.sin_addr.s_addr = inet_addr(host);
    if (bind(srv, (struct sockaddr*)&sa, sizeof(sa)) < 0) { perror("bind"); return 1; }
    listen(srv, 1);
    fprintf(stderr, "dumbterm: listening on %s:%d\n", host, port);

    struct sockaddr_in ca; int calen = sizeof(ca);
    sock_t client = accept(srv, (struct sockaddr*)&ca, (void*)&calen);
    fprintf(stderr, "dumbterm: client connected\n");
    sock_close(srv);
    g_net_sock = client;
    return 0;
}

/* Client helper: connect to remote server, return socket */
static sock_t net_connect(const char *addr) {
    sock_init();
    char host[256] = "127.0.0.1";
    int port = 9124;
    const char *colon = strrchr(addr, ':');
    if (colon) { memcpy(host, addr, colon-addr); host[colon-addr]=0; port=atoi(colon+1); }
    else port = atoi(addr);

    struct sockaddr_in sa; memset(&sa,0,sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    /* resolve hostname */
    struct hostent *he = gethostbyname(host);
    if (he) memcpy(&sa.sin_addr, he->h_addr_list[0], he->h_length);
    else sa.sin_addr.s_addr = inet_addr(host);

    sock_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (connect(s, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
        perror("connect"); return SOCK_INVALID;
    }
    fprintf(stderr, "dumbterm: connected to %s:%d\n", host, port);
    net_set_nonblock(s);
    return s;
}

/* Non-blocking read from socket into VT parser */
static void net_read_into_vt(sock_t s) {
    unsigned char buf[4096];
    int n;
#ifdef _WIN32
    n = recv(s, (char*)buf, sizeof(buf), 0);
    if (n == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) return;
#else
    n = recv(s, buf, sizeof(buf), 0);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
#endif
    if (n > 0) vt_feed(buf, n);
    else if (n == 0) { fprintf(stderr, "dumbterm: connection closed\n"); g_net_sock = SOCK_INVALID; }
}

/* Send bytes to socket */
static void net_write(sock_t s, const char *data, int len) {
    if (s == SOCK_INVALID) return;
    send(s, data, len, 0);
}

/* ── platform-specific code ────────────────────────────────────── */

#ifdef _WIN32
/* ═══ WINDOWS ═══════════════════════════════════════════════════ */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <gl/gl.h>

static HWND g_hwnd;
static HDC g_hdc;
static HGLRC g_hglrc;
static HANDLE child_stdin_wr, child_stdout_rd, child_proc;
static int child_alive;
static int win_w, win_h;

/* Atlas texture data (built before GL context) */
static unsigned char *g_atlas;
static int g_atlas_w, g_atlas_h, g_acols;
static int g_wide_y_offset, g_wide_acols;

static void platform_gl_init(void);
static void platform_spawn(const char *cmd);
static void platform_read_child(void);
static void platform_write_child(const char *data, int len);
static void gl_render(void);

static void build_atlas(void) {
    g_acols = 32;
    int arows = (UNIFONT_COUNT + g_acols - 1) / g_acols;
    g_wide_acols = 16;
    int wide_arows = (UNIFONT_WIDE_COUNT + g_wide_acols - 1) / g_wide_acols;
    int narrow_w = g_acols * FONT_W;
    int wide_w = g_wide_acols * FONT_W * 2;
    g_atlas_w = narrow_w > wide_w ? narrow_w : wide_w;
    g_wide_y_offset = arows * FONT_H;
    g_atlas_h = g_wide_y_offset + wide_arows * FONT_H;
    g_atlas = (unsigned char *)calloc(g_atlas_w * g_atlas_h, 1);
    int i, y, x;
    for (i = 0; i < UNIFONT_COUNT; i++) {
        int gx = (i % g_acols) * FONT_W;
        int gy = (i / g_acols) * FONT_H;
        for (y = 0; y < FONT_H; y++) {
            unsigned char row = UNIFONT[i].data[y];
            for (x = 0; x < FONT_W; x++)
                if (row & (0x80 >> x)) g_atlas[(gy+y)*g_atlas_w + gx+x] = 255;
        }
    }
    for (i = 0; i < UNIFONT_WIDE_COUNT; i++) {
        int gx = (i % g_wide_acols) * FONT_W * 2;
        int gy = g_wide_y_offset + (i / g_wide_acols) * FONT_H;
        for (y = 0; y < FONT_H; y++) {
            unsigned short row = ((unsigned short)UNIFONT_WIDE[i].data[y*2] << 8) | UNIFONT_WIDE[i].data[y*2+1];
            for (x = 0; x < 16; x++)
                if (row & (0x8000 >> x)) g_atlas[(gy+y)*g_atlas_w + gx+x] = 255;
        }
    }
}

static int glyph_index(unsigned short cp) {
    int lo = 0, hi = UNIFONT_COUNT - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (UNIFONT[mid].cp == cp) return mid;
        if (UNIFONT[mid].cp < cp) lo = mid + 1; else hi = mid - 1;
    }
    return glyph_index('?');
}

static int glyph_wide_index(unsigned short cp) {
    int lo = 0, hi = UNIFONT_WIDE_COUNT - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (UNIFONT_WIDE[mid].cp == cp) return mid;
        if (UNIFONT_WIDE[mid].cp < cp) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

static void platform_gl_init(void) {
    PIXELFORMATDESCRIPTOR pfd;
    memset(&pfd,0,sizeof(pfd));
    pfd.nSize=sizeof(pfd); pfd.nVersion=1;
    pfd.dwFlags=PFD_DRAW_TO_WINDOW|PFD_SUPPORT_OPENGL|PFD_DOUBLEBUFFER;
    pfd.iPixelType=PFD_TYPE_RGBA; pfd.cColorBits=32;
    int pf = ChoosePixelFormat(g_hdc, &pfd);
    SetPixelFormat(g_hdc, pf, &pfd);
    g_hglrc = wglCreateContext(g_hdc);
    wglMakeCurrent(g_hdc, g_hglrc);

    glViewport(0, 0, win_w, win_h);
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glOrtho(0, win_w, win_h, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glGenTextures(1, &gl_font_tex);
    glBindTexture(GL_TEXTURE_2D, gl_font_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, g_atlas_w, g_atlas_h, 0,
                 GL_ALPHA, GL_UNSIGNED_BYTE, g_atlas);
}

static void gl_render(void) {
    int r, c;
    glClearColor(0,0,0,1); glClear(GL_COLOR_BUFFER_BIT);

    /* backgrounds */
    glDisable(GL_TEXTURE_2D);
    glBegin(GL_QUADS);
    for (r=0;r<TERM_ROWS;r++) for(c=0;c<TERM_COLS;c++) {
        Cell *cl = &grid[r][c];
        unsigned char *bg = cl->inv ? cl->fg : cl->bg;
        if (!bg[0]&&!bg[1]&&!bg[2]) continue;
        float x0=c*FONT_W, y0=r*FONT_H, x1=x0+FONT_W, y1=y0+FONT_H;
        glColor3ub(bg[0],bg[1],bg[2]);
        glVertex2f(x0,y0); glVertex2f(x1,y0); glVertex2f(x1,y1); glVertex2f(x0,y1);
    }
    glEnd();

    /* foreground glyphs */
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, gl_font_tex);
    glBegin(GL_QUADS);
    for (r=0;r<TERM_ROWS;r++) for(c=0;c<TERM_COLS;c++) {
        Cell *cl = &grid[r][c];
        if (cl->ch==' '||cl->ch==0) continue;
        unsigned char *fg = cl->inv ? cl->bg : cl->fg;
        if (cl->bold) {
            int br=fg[0]+63,bg2=fg[1]+63,bb=fg[2]+63;
            glColor3ub(br>255?255:br, bg2>255?255:bg2, bb>255?255:bb);
        } else glColor3ub(fg[0],fg[1],fg[2]);

        float x0=c*FONT_W, y0=r*FONT_H, x1=x0+FONT_W, y1=y0+FONT_H;
        float u0, v0, u1, v1;
        int wi = glyph_wide_index(cl->ch);
        if (wi >= 0) {
            u0 = (float)((wi % g_wide_acols) * FONT_W * 2) / (float)g_atlas_w;
            v0 = (float)(g_wide_y_offset + (wi / g_wide_acols) * FONT_H) / (float)g_atlas_h;
            u1 = u0 + (float)(FONT_W * 2) / (float)g_atlas_w;
            v1 = v0 + (float)FONT_H / (float)g_atlas_h;
        } else {
            int gi = glyph_index(cl->ch);
            u0 = (float)((gi % g_acols) * FONT_W) / (float)g_atlas_w;
            v0 = (float)((gi / g_acols) * FONT_H) / (float)g_atlas_h;
            u1 = u0 + (float)FONT_W / (float)g_atlas_w;
            v1 = v0 + (float)FONT_H / (float)g_atlas_h;
        }
        if (wi >= 0) {
            float pad = (float)(FONT_H - FONT_W) * 0.5f;
            glTexCoord2f(u0,v0); glVertex2f(x0, y0+pad);
            glTexCoord2f(u1,v0); glVertex2f(x1, y0+pad);
            glTexCoord2f(u1,v1); glVertex2f(x1, y0+pad+FONT_W);
            glTexCoord2f(u0,v1); glVertex2f(x0, y0+pad+FONT_W);
        } else {
            glTexCoord2f(u0,v0); glVertex2f(x0,y0);
            glTexCoord2f(u1,v0); glVertex2f(x1,y0);
            glTexCoord2f(u1,v1); glVertex2f(x1,y1);
            glTexCoord2f(u0,v1); glVertex2f(x0,y1);
        }
    }
    glEnd();
}

/* ── pipe + JS shim approach ────────────────────────────────────── */
/* Child gets pipes. JS shim fakes isTTY=true and patches platform */
/* detection so supports-color gives level 3 (truecolor).          */
/* Raw bytes, zero corruption, full color.                         */

static const char JS_SHIM[] =
    "Object.defineProperty(process.stdout,'isTTY',{value:true,writable:false});\n"
    "Object.defineProperty(process.stderr,'isTTY',{value:true,writable:false});\n"
    "Object.defineProperty(process.stdin,'isTTY',{value:true,writable:false});\n"
    "Object.defineProperty(process.stdout,'columns',{value:%d,writable:true});\n"
    "Object.defineProperty(process.stdout,'rows',{value:%d,writable:true});\n"
    "Object.defineProperty(process.stderr,'columns',{value:%d,writable:true});\n"
    "Object.defineProperty(process.stderr,'rows',{value:%d,writable:true});\n"
    "process.stdout.getColorDepth=function(){return 24;};\n"
    "process.stderr.getColorDepth=function(){return 24;};\n"
    "process.stdout.hasColors=function(){return true;};\n"
    "process.stderr.hasColors=function(){return true;};\n"
    "if(!process.stdin.setRawMode)process.stdin.setRawMode=function(){return process.stdin;};\n"
    "process.env.TERM='xterm-256color';\n"
    "process.env.COLORTERM='truecolor';\n"
    "process.env.FORCE_COLOR='3';\n"
    /* Fix: supports-color hardcodes level=1 on Win7 (checks os.release()).              */
    /* Fake Win10 version so it returns level=3. Keep platform='win32' so ripgrep works. */
    "var _os=require('os');var _origRel=_os.release;_os.release=function(){return'10.0.19041';};\n"
    /* Also set SHELL to MSYS bash so Claude Code can run shell commands */
    "process.env.SHELL=process.env.SHELL||'C:\\\\Program Files\\\\Git\\\\usr\\\\bin\\\\bash.exe';\n"
    "var tty=require('tty');\n"
    "process.stdout.__proto__=tty.WriteStream.prototype;\n"
    "process.stderr.__proto__=tty.WriteStream.prototype;\n"
    /* Intercept _RESIZE on stdin: ESC _ RESIZE;cols;rows ESC \ */
    "var _origPush=process.stdin.push.bind(process.stdin);\n"
    "var _rBuf='';\n"
    "process.stdin.push=function(chunk,enc){\n"
    "  if(!chunk)return _origPush(chunk,enc);\n"
    "  var s=typeof chunk==='string'?chunk:chunk.toString('binary');\n"
    "  _rBuf+=s;var out='';\n"
    "  while(_rBuf.length>0){\n"
    "    var idx=_rBuf.indexOf('\\x1b_RESIZE;');\n"
    "    if(idx<0){out+=_rBuf;_rBuf='';break;}\n"
    "    out+=_rBuf.slice(0,idx);\n"
    "    var end=_rBuf.indexOf('\\x1b\\\\',idx+9);\n"
    "    if(end<0){_rBuf=_rBuf.slice(idx);break;}\n"
    "    var parts=_rBuf.slice(idx+9,end).split(';');\n"
    "    var nc=parseInt(parts[0]),nr=parseInt(parts[1]);\n"
    "    if(nc>0&&nr>0){\n"
    "      process.stdout.columns=nc;process.stdout.rows=nr;\n"
    "      process.stderr.columns=nc;process.stderr.rows=nr;\n"
    "      process.stdout.emit('resize');process.stderr.emit('resize');\n"
    "    }\n"
    "    _rBuf=_rBuf.slice(end+2);\n"
    "  }\n"
    "  if(out.length>0)_origPush(Buffer.from(out,'binary'),enc);\n"
    "};\n";

static void write_shim(char *path, int cols, int rows) {
    GetModuleFileNameA(NULL, path, MAX_PATH);
    char *sl = strrchr(path, '\\');
    if (sl) strcpy(sl + 1, "_shim.js");
    else strcpy(path, "_shim.js");
    FILE *f = fopen(path, "w");
    if (!f) { fprintf(stderr, "dumbterm: cannot write %s\n", path); return; }
    fprintf(f, JS_SHIM, cols, rows, cols, rows);
    fclose(f);
    fprintf(stderr, "dumbterm: wrote %s\n", path);
}

static void platform_spawn(const char *cmd) {
    SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
    HANDLE in_rd, out_wr;
    CreatePipe(&in_rd, &child_stdin_wr, &sa, 0);
    CreatePipe(&child_stdout_rd, &out_wr, &sa, 0);
    SetHandleInformation(child_stdin_wr, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(child_stdout_rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si; memset(&si, 0, sizeof(si)); si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_rd; si.hStdOutput = out_wr; si.hStdError = out_wr;

    char cmdline[4096];
    if (strstr(cmd, "node") && strstr(cmd, ".js")) {
        char tmp[4096]; strncpy(tmp, cmd, sizeof(tmp)-1); tmp[sizeof(tmp)-1]=0;
        char *node = strtok(tmp, " ");
        char *script = strtok(NULL, " ");
        if (node && script) {
            char shim[MAX_PATH];
            write_shim(shim, TERM_COLS, TERM_ROWS);
            char *rest = strtok(NULL, "");
            snprintf(cmdline, sizeof(cmdline), "%s --require \"%s\" %s%s%s",
                     node, shim, script, rest ? " " : "", rest ? rest : "");
        } else strncpy(cmdline, cmd, sizeof(cmdline)-1);
    } else strncpy(cmdline, cmd, sizeof(cmdline)-1);
    cmdline[sizeof(cmdline)-1] = 0;

    SetEnvironmentVariableA("FORCE_COLOR", "3");
    SetEnvironmentVariableA("COLORTERM", "truecolor");
    SetEnvironmentVariableA("TERM", "xterm-256color");
    SetEnvironmentVariableA("NODE_SKIP_PLATFORM_CHECK", "1");
    SetEnvironmentVariableA("SHELL", "C:\\Program Files\\Git\\usr\\bin\\bash.exe");

    PROCESS_INFORMATION pi;
    fprintf(stderr, "dumbterm: spawning: %s\n", cmdline);
    if (!CreateProcessA(NULL, cmdline, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        fprintf(stderr, "dumbterm: CreateProcess failed (%lu)\n", GetLastError());
        CloseHandle(in_rd); CloseHandle(out_wr); return;
    }
    CloseHandle(pi.hThread);
    child_proc = pi.hProcess; child_alive = 1;
    fprintf(stderr, "dumbterm: child pid %lu\n", pi.dwProcessId);
    CloseHandle(in_rd); CloseHandle(out_wr);
}

static void platform_read_child(void) {
    DWORD avail = 0;
    if (!child_alive) return;
    if (!PeekNamedPipe(child_stdout_rd, NULL, 0, NULL, &avail, NULL) || avail == 0) {
        DWORD code; if (GetExitCodeProcess(child_proc, &code) && code != STILL_ACTIVE) child_alive = 0;
        return;
    }
    unsigned char buf[4096]; DWORD nr;
    while (avail > 0) {
        DWORD toread = avail < sizeof(buf) ? avail : sizeof(buf);
        if (!ReadFile(child_stdout_rd, buf, toread, &nr, NULL) || nr == 0) break;
        vt_feed(buf, nr); avail -= nr;
    }
}

/* read raw bytes from child pipe without vt_feed — for visible-server broadcast */
static int platform_read_child_raw(unsigned char *out, int maxlen) {
    DWORD avail = 0;
    if (!child_alive) return 0;
    if (!PeekNamedPipe(child_stdout_rd, NULL, 0, NULL, &avail, NULL) || avail == 0) {
        DWORD code; if (GetExitCodeProcess(child_proc, &code) && code != STILL_ACTIVE) child_alive = 0;
        return 0;
    }
    DWORD toread = avail < (DWORD)maxlen ? avail : (DWORD)maxlen;
    DWORD nr = 0;
    if (!ReadFile(child_stdout_rd, out, toread, &nr, NULL)) return 0;
    return (int)nr;
}

static void platform_write_child(const char *data, int len) {
    if (!child_alive) return;
    DWORD written; WriteFile(child_stdin_wr, data, len, &written, NULL);
}

static void handle_key(WPARAM vk, int ctrl) {
    char buf[8]; int len = 0;
    if (ctrl) { if(vk>='A'&&vk<='Z'){buf[0]=vk-'A'+1;len=1;} }
    else switch(vk) {
        case VK_UP: len=sprintf(buf,"\x1b[A"); break;
        case VK_DOWN: len=sprintf(buf,"\x1b[B"); break;
        case VK_RIGHT: len=sprintf(buf,"\x1b[C"); break;
        case VK_LEFT: len=sprintf(buf,"\x1b[D"); break;
        case VK_HOME: len=sprintf(buf,"\x1b[H"); break;
        case VK_END: len=sprintf(buf,"\x1b[F"); break;
        case VK_DELETE: len=sprintf(buf,"\x1b[3~"); break;
        case VK_RETURN: buf[0]='\r'; len=1; break;
        case VK_BACK: buf[0]=0x7F; len=1; break;
        case VK_TAB: buf[0]='\t'; len=1; break;
        case VK_ESCAPE: buf[0]=0x1B; len=1; break;
    }
    if (len) {
        if (g_net_mode == 2) net_write(g_net_sock, buf, len);
        else platform_write_child(buf, len);
    }
}

static void apply_resize(int new_cols, int new_rows) {
    if (new_cols == term_cols && new_rows == term_rows) return;
    if (new_cols < 10) new_cols = 10; if (new_rows < 4) new_rows = 4;
    if (new_cols > TERM_COLS) new_cols = TERM_COLS;
    if (new_rows > TERM_ROWS) new_rows = TERM_ROWS;
    term_cols = new_cols; term_rows = new_rows;
    int r, c;
    for (r = 0; r < TERM_ROWS; r++)
        for (c = 0; c < TERM_COLS; c++) {
            grid[r][c].ch = ' '; grid[r][c].bg[0]=grid[r][c].bg[1]=grid[r][c].bg[2]=0;
            grid[r][c].fg[0]=grid[r][c].fg[1]=grid[r][c].fg[2]=192; grid[r][c].inv=grid[r][c].bold=0;
        }
    cur_x = 0; cur_y = 0;
    scroll_y = 0; scroll_vel = 0; scroll_snapping = 0; scroll_locked = 1;
    hist_count = 0; hist_write = 0;
}

static void send_char(char c) {
    if (g_net_mode == 2) net_write(g_net_sock, &c, 1);
    else platform_write_child(&c, 1);
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_DESTROY: PostQuitMessage(0); return 0;
    case WM_CHAR: if(wp>=0x20&&wp<0x7F){char c=(char)wp; send_char(c);} return 0;
    case WM_KEYDOWN: handle_key(wp, GetKeyState(VK_CONTROL)&0x8000); return 0;
    case WM_SIZE: {
        RECT cr; GetClientRect(hwnd, &cr); win_w=cr.right; win_h=cr.bottom;
        glViewport(0,0,win_w,win_h);
        glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0,win_w,win_h,0,-1,1);
        glMatrixMode(GL_MODELVIEW);
    } return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* Send _RESIZE to child stdin (Windows pipes — child JS shim intercepts it) */
static void win_send_resize_to_child(int cols, int rows) {
    char msg[64];
    int len = sprintf(msg, "\x1b_RESIZE;%d;%d\x1b\\", cols, rows);
    DWORD written;
    WriteFile(child_stdin_wr, msg, len, &written, NULL);
    apply_resize(cols, rows);
}

static void win_headless_fwd(const char *data, int len) {
    DWORD written; WriteFile(child_stdin_wr, data, len, &written, NULL);
}

/* Win32 server mode: headless, forward child pipe bytes over network */
static int win_run_server(const char *addr, const char *child_cmd) {
    if (net_serve(addr, child_cmd) != 0) return 1;
    net_set_nonblock(g_net_sock);
    platform_spawn(child_cmd);
    if (!child_alive) { fprintf(stderr, "dumbterm: child failed to start\n"); return 1; }
    fprintf(stderr, "dumbterm: forwarding bytes\n");

    unsigned char buf[4096];
    for (;;) {
        DWORD avail = 0;
        if (PeekNamedPipe(child_stdout_rd, NULL, 0, NULL, &avail, NULL) && avail > 0) {
            DWORD nr, toread = avail < sizeof(buf) ? avail : sizeof(buf);
            if (ReadFile(child_stdout_rd, buf, toread, &nr, NULL) && nr > 0)
                send(g_net_sock, (char*)buf, nr, 0);
        }
        int n = recv(g_net_sock, (char*)buf, sizeof(buf), 0);
        if (n > 0) net_filter_and_forward(buf, n, win_headless_fwd, win_send_resize_to_child);
        else if (n == 0) break;
        DWORD code;
        if (GetExitCodeProcess(child_proc, &code) && code != STILL_ACTIVE) {
            Sleep(200);
            while (PeekNamedPipe(child_stdout_rd, NULL, 0, NULL, &avail, NULL) && avail > 0) {
                DWORD nr, toread = avail < sizeof(buf) ? avail : sizeof(buf);
                if (ReadFile(child_stdout_rd, buf, toread, &nr, NULL) && nr > 0)
                    send(g_net_sock, (char*)buf, nr, 0);
                else break;
            }
            break;
        }
        Sleep(1);
    }
    sock_close(g_net_sock);
    return 0;
}

int main(int argc, char **argv) {
    const char *child_cmd = "cmd.exe";
    int i;
    /* parse --listen, --connect, -- */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--listen") == 0 && i+1 < argc) {
            g_listen_addr = argv[++i]; g_net_mode = 1;
        } else if (strcmp(argv[i], "--connect") == 0 && i+1 < argc) {
            g_connect_addr = argv[++i]; g_net_mode = 2;
        } else if (strcmp(argv[i], "--visible") == 0) {
            g_server_visible = 1;
        } else if (strcmp(argv[i], "--") == 0) { i++; break; }
        else break;
    }
    if (i < argc) {
        static char cmdbuf[4096]; cmdbuf[0]=0;
        int ci; for(ci=i;ci<argc;ci++){if(ci>i)strcat(cmdbuf," ");strcat(cmdbuf,argv[ci]);}
        child_cmd = cmdbuf;
    }

    /* SERVER MODE: headless only (no --visible) */
    if (g_net_mode == 1 && !g_server_visible) {
        return win_run_server(g_listen_addr, child_cmd);
    }

    /* CLIENT, LOCAL, or VISIBLE-SERVER MODE: GL window */
    build_atlas();
    win_w = TERM_COLS*FONT_W; win_h = TERM_ROWS*FONT_H;
    WNDCLASSA wc; memset(&wc,0,sizeof(wc));
    wc.style=CS_OWNDC; wc.lpfnWndProc=wndproc; wc.hInstance=GetModuleHandleA(NULL);
    wc.lpszClassName="dumbterm"; wc.hCursor=LoadCursorA(NULL,IDC_ARROW);
    RegisterClassA(&wc);
    RECT wr={0,0,win_w,win_h}; AdjustWindowRect(&wr,WS_OVERLAPPEDWINDOW,FALSE);

    char title[256] = "dumbterm";
    if (g_net_mode == 2)
        snprintf(title, sizeof(title), "dumbterm [remote:%s]", g_connect_addr);

    g_hwnd = CreateWindowExA(0,"dumbterm",title,WS_OVERLAPPEDWINDOW|WS_VISIBLE,
        CW_USEDEFAULT,CW_USEDEFAULT,wr.right-wr.left,wr.bottom-wr.top,NULL,NULL,wc.hInstance,NULL);
    g_hdc = GetDC(g_hwnd);
    platform_gl_init();
    grid_clear();

    if (g_net_mode == 2) {
        /* CLIENT: connect to remote */
        g_net_sock = net_connect(g_connect_addr);
        if (g_net_sock == SOCK_INVALID) return 1;
    } else {
        /* LOCAL or VISIBLE-SERVER: spawn child */
        if (g_net_mode == 1 && g_server_visible) {
            if (net_listen_start(g_listen_addr) != 0) return 1;
        }
        platform_spawn(child_cmd);
    }

    MSG msg;
    for (;;) {
        while (PeekMessageA(&msg,NULL,0,0,PM_REMOVE)) { if(msg.message==WM_QUIT) goto done; TranslateMessage(&msg); DispatchMessageA(&msg); }
        if (g_net_mode == 2) {
            /* client: read from network */
            if (g_net_sock != SOCK_INVALID) net_read_into_vt(g_net_sock);
        } else {
            /* read child output; broadcast if visible server */
            unsigned char rbuf[4096]; int rn;
            while ((rn = platform_read_child_raw(rbuf, sizeof(rbuf))) > 0) {
                vt_feed(rbuf, rn);
                if (g_net_mode == 1 && g_server_visible) net_broadcast((char*)rbuf, rn);
            }
            if (g_net_mode == 1 && g_server_visible) {
                net_accept_clients();
                net_read_all_clients(platform_write_child, win_send_resize_to_child);
            }
        }
        gl_render(); SwapBuffers(g_hdc); Sleep(1);
    }
done:
    if (g_net_mode == 2 && g_net_sock != SOCK_INVALID) sock_close(g_net_sock);
    if (child_proc) TerminateProcess(child_proc, 0);
    return 0;
}

#else
/* ═══ macOS / Unix ══════════════════════════════════════════════ */

#define GL_SILENCE_DEPRECATION
#ifdef __APPLE__
#include <OpenGL/gl.h>
#import <Cocoa/Cocoa.h>
#else
#include <GL/gl.h>
#endif
#include <unistd.h>
#include <sys/wait.h>
#include <util.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <termios.h>

static int pty_master = -1;

/* ── CoreAudio real-time playback ──────────────────────────────── */
#ifdef __APPLE__
static AudioQueueRef audio_queue;
#define AQ_BUFS 3
#define AQ_BUF_SIZE 2048

static void aq_callback(void *ctx, AudioQueueRef q, AudioQueueBufferRef buf) {
    (void)ctx;
    float *out = (float*)buf->mAudioData;
    int frames = AQ_BUF_SIZE;
    int i;
    for (i = 0; i < frames; i++) {
        if (audio_playing && audio_pos < audio_len) {
            out[i] = audio_buf[audio_pos++];
        } else {
            out[i] = 0;
            if (audio_playing && audio_pos >= audio_len) audio_playing = 0;
        }
    }
    buf->mAudioDataByteSize = frames * sizeof(float);
    AudioQueueEnqueueBuffer(q, buf, 0, NULL);
}

static void audio_init(void) {
    AudioStreamBasicDescription fmt = {0};
    fmt.mSampleRate = 22050;
    fmt.mFormatID = kAudioFormatLinearPCM;
    fmt.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    fmt.mBytesPerPacket = sizeof(float);
    fmt.mFramesPerPacket = 1;
    fmt.mBytesPerFrame = sizeof(float);
    fmt.mChannelsPerFrame = 1;
    fmt.mBitsPerChannel = 32;
    AudioQueueNewOutput(&fmt, aq_callback, NULL, NULL, NULL, 0, &audio_queue);
    int i;
    for (i = 0; i < AQ_BUFS; i++) {
        AudioQueueBufferRef buf;
        AudioQueueAllocateBuffer(audio_queue, AQ_BUF_SIZE * sizeof(float), &buf);
        buf->mAudioDataByteSize = AQ_BUF_SIZE * sizeof(float);
        memset(buf->mAudioData, 0, buf->mAudioDataByteSize);
        AudioQueueEnqueueBuffer(audio_queue, buf, 0, NULL);
    }
    AudioQueueStart(audio_queue, NULL);
}
#else
static void audio_init(void) {}
#endif
static pid_t child_pid;
static int child_alive;
static int g_acols;
static int g_atlas_w, g_atlas_h;
static unsigned char *g_atlas;
static int g_wide_y_offset; /* Y pixel offset in atlas where wide glyphs start */
static int g_wide_acols;    /* columns in the wide section (16px each) */

static void build_atlas(void) {
    g_acols = 32;
    int arows = (UNIFONT_COUNT + g_acols - 1) / g_acols;
    /* wide glyphs: 16px wide, so fewer per row */
    g_wide_acols = 16; /* 16 wide glyphs per row */
    int wide_arows = (UNIFONT_WIDE_COUNT + g_wide_acols - 1) / g_wide_acols;
    /* atlas width must fit both: narrow needs g_acols*8, wide needs g_wide_acols*16 */
    int narrow_w = g_acols * FONT_W;      /* 32*8 = 256 */
    int wide_w = g_wide_acols * FONT_W*2; /* 16*16 = 256 */
    g_atlas_w = narrow_w > wide_w ? narrow_w : wide_w;
    g_wide_y_offset = arows * FONT_H;
    g_atlas_h = g_wide_y_offset + wide_arows * FONT_H;
    g_atlas = (unsigned char *)calloc(g_atlas_w * g_atlas_h, 1);
    int i, y, x;
    /* narrow glyphs: 8px wide */
    for (i = 0; i < UNIFONT_COUNT; i++) {
        int gx = (i % g_acols) * FONT_W;
        int gy = (i / g_acols) * FONT_H;
        for (y = 0; y < FONT_H; y++) {
            unsigned char row = UNIFONT[i].data[y];
            for (x = 0; x < FONT_W; x++)
                if (row & (0x80 >> x)) g_atlas[(gy+y)*g_atlas_w + gx+x] = 255;
        }
    }
    /* wide glyphs: 16px wide */
    for (i = 0; i < UNIFONT_WIDE_COUNT; i++) {
        int gx = (i % g_wide_acols) * FONT_W * 2;
        int gy = g_wide_y_offset + (i / g_wide_acols) * FONT_H;
        for (y = 0; y < FONT_H; y++) {
            unsigned short row = ((unsigned short)UNIFONT_WIDE[i].data[y*2] << 8) | UNIFONT_WIDE[i].data[y*2+1];
            for (x = 0; x < 16; x++)
                if (row & (0x8000 >> x)) g_atlas[(gy+y)*g_atlas_w + gx+x] = 255;
        }
    }
}

static int glyph_index(unsigned short cp) {
    int lo = 0, hi = UNIFONT_COUNT - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (UNIFONT[mid].cp == cp) return mid;
        if (UNIFONT[mid].cp < cp) lo = mid + 1; else hi = mid - 1;
    }
    return glyph_index('?');
}

/* returns wide glyph index or -1 */
static int glyph_wide_index(unsigned short cp) {
    int lo = 0, hi = UNIFONT_WIDE_COUNT - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (UNIFONT_WIDE[mid].cp == cp) return mid;
        if (UNIFONT_WIDE[mid].cp < cp) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

/* Get a cell from either history or the live grid.
   row 0 = oldest history line, row hist_count = grid row 0, etc. */
static const Cell *get_virtual_row(int vrow) {
    if (vrow < hist_count) {
        int idx = (hist_write - hist_count + vrow + HIST_LINES) % HIST_LINES;
        return history[idx];
    }
    int gr = vrow - hist_count;
    if (gr >= 0 && gr < TERM_ROWS) return grid[gr];
    return NULL;
}

/* Apply glow boost to a color channel: c + (255-c)*g*GLOW_BRIGHT */
static inline int glow_color(int c, float g) {
    return c + (int)((255 - c) * g * GLOW_BRIGHT);
}

static void draw_cell(const Cell *cl, float x, float y, float g) {
    if (!cl) return;
    unsigned char *bg = cl->inv ? (unsigned char*)cl->fg : (unsigned char*)cl->bg;
    unsigned char *fg = cl->inv ? (unsigned char*)cl->bg : (unsigned char*)cl->fg;

    int br0 = glow_color(bg[0],g), bg0 = glow_color(bg[1],g), bb0 = glow_color(bg[2],g);
    if (br0||bg0||bb0) {
        glDisable(GL_TEXTURE_2D);
        glColor3ub(br0,bg0,bb0);
        glBegin(GL_QUADS);
        glVertex2f(x,y); glVertex2f(x+CELL_W,y);
        glVertex2f(x+CELL_W,y+CELL_H); glVertex2f(x,y+CELL_H);
        glEnd();
    }
    if (cl->ch != ' ' && cl->ch != 0) {
        float u0, v0, u1, v1;
        int wi = glyph_wide_index(cl->ch);
        if (wi >= 0) {
            /* wide glyph: 16px in atlas, rendered scaled down into CELL_W */
            u0 = (float)((wi % g_wide_acols) * FONT_W * 2) / (float)g_atlas_w;
            v0 = (float)(g_wide_y_offset + (wi / g_wide_acols) * FONT_H) / (float)g_atlas_h;
            u1 = u0 + (float)(FONT_W * 2) / (float)g_atlas_w;
            v1 = v0 + (float)FONT_H / (float)g_atlas_h;
        } else {
            int gi = glyph_index(cl->ch);
            u0 = (float)((gi % g_acols) * FONT_W) / (float)g_atlas_w;
            v0 = (float)((gi / g_acols) * FONT_H) / (float)g_atlas_h;
            u1 = u0 + (float)FONT_W / (float)g_atlas_w;
            v1 = v0 + (float)FONT_H / (float)g_atlas_h;
        }
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, gl_font_tex);
        int fr=fg[0], fgr=fg[1], fb=fg[2];
        if (cl->bold) { fr+=63; fgr+=63; fb+=63; }
        glColor3ub(glow_color(fr>255?255:fr,g), glow_color(fgr>255?255:fgr,g), glow_color(fb>255?255:fb,g));
        glBegin(GL_QUADS);
        if (wi >= 0) {
            /* 16x16 source → CELL_W x CELL_W square, centered vertically */
            float pad = (float)(CELL_H - CELL_W) * 0.5f;
            glTexCoord2f(u0,v0); glVertex2f(x, y+pad);
            glTexCoord2f(u1,v0); glVertex2f(x+CELL_W, y+pad);
            glTexCoord2f(u1,v1); glVertex2f(x+CELL_W, y+pad+CELL_W);
            glTexCoord2f(u0,v1); glVertex2f(x, y+pad+CELL_W);
        } else {
            glTexCoord2f(u0,v0); glVertex2f(x,y);
            glTexCoord2f(u1,v0); glVertex2f(x+CELL_W,y);
            glTexCoord2f(u1,v1); glVertex2f(x+CELL_W,y+CELL_H);
            glTexCoord2f(u0,v1); glVertex2f(x,y+CELL_H);
        }
        glEnd();
    }
}

/* ── smooth scrolling physics update ───────────────────────────── */
static int   scroll_idle_frames;   /* frames since last user input */
static float scroll_snap_t;       /* 0..1 animation progress for idle snap */
static float scroll_snap_from;    /* starting position of idle snap */

static void scroll_update(void) {
    float max_scroll = (float)hist_count * CELL_H;

    if (scroll_snapping) {
        /* smooth ease-in-out toward line-aligned target */
        scroll_snap_t += 1.0f / 48.0f; /* ~0.8s at 60fps */
        if (scroll_snap_t >= 1.0f) {
            scroll_snap_t = 1.0f;
            scroll_y = scroll_target;
            scroll_vel = 0;
            scroll_snapping = 0;
        } else {
            /* cubic ease-in-out: smooth acceleration + deceleration */
            float t = scroll_snap_t;
            float ease = t < 0.5f ? 4*t*t*t : 1 - powf(-2*t+2, 3)/2;
            scroll_y = scroll_snap_from + (scroll_target - scroll_snap_from) * ease;
        }
    } else if (fabsf(scroll_vel) > SCROLL_MIN_VEL) {
        /* momentum decay */
        scroll_vel *= SCROLL_FRICTION;
        scroll_y += scroll_vel;
        scroll_idle_frames = 0;

        /* when velocity gets low, pick nearest line in direction of travel */
        if (fabsf(scroll_vel) < 2.0f) {
            float line = scroll_y / CELL_H;
            scroll_target = (scroll_vel > 0 ? ceilf(line) : floorf(line)) * CELL_H;
            if (scroll_target < 0) scroll_target = 0;
            if (scroll_target > max_scroll) scroll_target = max_scroll;
            scroll_snap_from = scroll_y;
            scroll_snap_t = 0;
            scroll_snapping = 1;
            scroll_vel = 0;
        }
    } else {
        /* no velocity — check if we're between lines */
        scroll_vel = 0;
        float ch = (float)CELL_H;
        float remainder = fmodf(scroll_y, ch);
        if (remainder > 0.5f && !scroll_snapping) {
            scroll_idle_frames++;
            if (scroll_idle_frames > 12) {
                scroll_target = roundf(scroll_y / ch) * ch;
                if (scroll_target < 0) scroll_target = 0;
                if (scroll_target > max_scroll) scroll_target = max_scroll;
                scroll_snap_from = scroll_y;
                scroll_snap_t = 0;
                scroll_snapping = 1;
            }
        }
    }

    /* clamp — scroll_y=0 is always bottom (live), max_scroll is top of history */
    if (scroll_y < 0) { scroll_y = 0; scroll_vel = 0; scroll_snapping = 0; }
    if (max_scroll < 0) max_scroll = 0;
    if (scroll_y > max_scroll) { scroll_y = max_scroll; scroll_vel = 0; scroll_snapping = 0; }
    /* snap to 0 when very close (prevent floating just above bottom) */
    if (scroll_y > 0 && scroll_y < 1.0f && fabsf(scroll_vel) < SCROLL_MIN_VEL) {
        scroll_y = 0; scroll_vel = 0; scroll_snapping = 0;
    }
    scroll_locked = (scroll_y < 1.0f);
}

static void gl_render(void) {
    scroll_update();
    glow_update();
    int total_vrows = hist_count + term_rows;
    /* scroll_y=0 = bottom (live), scroll_y = hist_count*CELL_H = top of history */
    float view_top_px = (float)(total_vrows - term_rows) * CELL_H - scroll_y;

    glClearColor(0,0,0,1); glClear(GL_COLOR_BUFFER_BIT);

    /* determine which virtual rows are visible (with 1-row margin for partial rows) */
    int first_vrow = (int)(view_top_px / CELL_H);
    if (first_vrow < 0) first_vrow = 0;
    int last_vrow = first_vrow + term_rows + 1;
    if (last_vrow > total_vrows) last_vrow = total_vrows;

    /* render visible rows with sub-pixel Y offset */
    float frac_y = view_top_px - (float)first_vrow * CELL_H;
    int r, c;
    for (r = first_vrow; r <= last_vrow && r < total_vrows; r++) {
        const Cell *row = get_virtual_row(r);
        if (!row) continue;
        float py = (float)(r - first_vrow) * CELL_H - frac_y;
        if (py > term_rows * CELL_H || py + CELL_H < 0) continue;
        /* map screen position to glow row */
        int screen_row = (int)(py / CELL_H);
        for (c = 0; c < TERM_COLS; c++) {
            float g = (screen_row >= 0 && screen_row < TERM_ROWS) ? glow[screen_row][c] : 0;
            draw_cell(&row[c], (float)(c * CELL_W), py, g);
        }
    }

    /* crosshair: full row/col amber fill with decay */
    glDisable(GL_TEXTURE_2D);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    float vw = (float)(term_cols * CELL_W), vh = (float)(term_rows * CELL_H);
    for (r = 0; r < term_rows; r++) {
        if (row_glow[r] < 0.01f) continue;
        float a = row_glow[r] * 0.10f;
        float ry = (float)(r * CELL_H);
        glColor4f(AMBER_R, AMBER_G, AMBER_B, a);
        glBegin(GL_QUADS);
        glVertex2f(0,ry); glVertex2f(vw,ry); glVertex2f(vw,ry+CELL_H); glVertex2f(0,ry+CELL_H);
        glEnd();
    }
    for (c = 0; c < term_cols; c++) {
        if (col_glow[c] < 0.01f) continue;
        float a = col_glow[c] * 0.10f;
        float cx = (float)(c * CELL_W);
        glColor4f(AMBER_R, AMBER_G, AMBER_B, a);
        glBegin(GL_QUADS);
        glVertex2f(cx,0); glVertex2f(cx+CELL_W,0); glVertex2f(cx+CELL_W,vh); glVertex2f(cx,vh);
        glEnd();
    }
    /* selection: two-layer amber fill + perimeter border */
    if (sel_mode) {
        /* pass 1: amber fill — level 2 (populated) = 10%, level 1 (outer) = 5% */
        for (r = 0; r < term_rows; r++) {
            for (c = 0; c < term_cols; c++) {
                int lv = sel_contains(c, r);
                if (!lv) continue;
                float sx = (float)(c * CELL_W), sy = (float)(r * CELL_H);
                float a = (lv == 2) ? 0.10f : 0.05f;
                glColor4f(1.0f, 0.8f, 0.2f, a);
                glBegin(GL_QUADS);
                glVertex2f(sx,sy); glVertex2f(sx+CELL_W,sy);
                glVertex2f(sx+CELL_W,sy+CELL_H); glVertex2f(sx,sy+CELL_H);
                glEnd();
            }
        }
        /* pass 2: perimeter borders at the SAME level — border between level 2 and 1 shows,
           border between level 1 and 0 shows, no border between same levels */
        for (r = 0; r < term_rows; r++) {
            for (c = 0; c < term_cols; c++) {
                int lv = sel_contains(c, r);
                if (!lv) continue;
                float sx = (float)(c * CELL_W), sy = (float)(r * CELL_H);
                float a = (lv == 2) ? 0.20f : 0.10f;
                glColor4f(1.0f, 0.8f, 0.2f, a);
                int nb; /* neighbor level */
                /* top */
                nb = sel_contains(c, r-1);
                if (nb < lv) { glBegin(GL_QUADS); glVertex2f(sx,sy); glVertex2f(sx+CELL_W,sy);
                    glVertex2f(sx+CELL_W,sy+1); glVertex2f(sx,sy+1); glEnd(); }
                /* bottom */
                nb = sel_contains(c, r+1);
                if (nb < lv) { glBegin(GL_QUADS); glVertex2f(sx,sy+CELL_H-1); glVertex2f(sx+CELL_W,sy+CELL_H-1);
                    glVertex2f(sx+CELL_W,sy+CELL_H); glVertex2f(sx,sy+CELL_H); glEnd(); }
                /* left */
                nb = sel_contains(c-1, r);
                if (nb < lv) { glBegin(GL_QUADS); glVertex2f(sx,sy); glVertex2f(sx+1,sy);
                    glVertex2f(sx+1,sy+CELL_H); glVertex2f(sx,sy+CELL_H); glEnd(); }
                /* right */
                nb = sel_contains(c+1, r);
                if (nb < lv) { glBegin(GL_QUADS); glVertex2f(sx+CELL_W-1,sy); glVertex2f(sx+CELL_W,sy);
                    glVertex2f(sx+CELL_W,sy+CELL_H); glVertex2f(sx+CELL_W-1,sy+CELL_H); glEnd(); }
            }
        }
    }

    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); /* restore normal blend */

    /* scroll indicator (thin bar on right edge when scrolled up) */
    if (scroll_y > 1.0f && hist_count > 0) {
        float max_s = (float)hist_count * CELL_H;
        float bar_h = ((float)term_rows / (float)(hist_count + term_rows)) * (term_rows * CELL_H);
        if (bar_h < 10) bar_h = 10;
        float bar_y = (1.0f - scroll_y / max_s) * (term_rows * CELL_H - bar_h);
        glDisable(GL_TEXTURE_2D);
        glColor4f(1,1,1,0.3f);
        glBegin(GL_QUADS);
        float bx = term_cols * CELL_W - 3;
        glVertex2f(bx, bar_y); glVertex2f(bx+3, bar_y);
        glVertex2f(bx+3, bar_y+bar_h); glVertex2f(bx, bar_y+bar_h);
        glEnd();
    }

    /* ── phosphor-blink cursor ──────────────────────────────────── */
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    {
        /* find cursor cell: .inv scan or cur_vis fallback */
        int cur_col = -1, cur_row = -1;
        int cur_ch_code = 0;
        unsigned char cbg[3] = {0,0,0}, cur_fg[3] = {0,0,0};

        if (scroll_y < 1.0f) {
            for (r = 0; r < term_rows; r++) {
                for (c = 0; c < term_cols; c++) {
                    if (grid[r][c].inv) {
                        cur_col = c; cur_row = r;
                        memcpy(cbg, grid[r][c].bg, 3);
                        cur_ch_code = grid[r][c].ch;
                        memcpy(cur_fg, grid[r][c].bg, 3);
                        goto found_cursor;
                    }
                }
            }
            if (cur_vis && cur_x < term_cols && cur_y < term_rows) {
                cur_col = cur_x; cur_row = cur_y;
                memcpy(cbg, grid[cur_y][cur_x].bg, 3);
                cur_ch_code = grid[cur_y][cur_x].ch;
                memcpy(cur_fg, grid[cur_y][cur_x].fg, 3);
            }
        }
        found_cursor:

        /* blink tick: wall-clock driven */
        double now_time = CFAbsoluteTimeGetCurrent();
        if (cursor_blink_next == 0) cursor_blink_next = now_time + CURSOR_BLINK_SEC;
        if (now_time >= cursor_blink_next) {
            cursor_blink_next += CURSOR_BLINK_SEC;
            /* if we fell behind by more than one interval, resync */
            if (cursor_blink_next < now_time) cursor_blink_next = now_time + CURSOR_BLINK_SEC;
            /* stamp phosphor at current cursor position */
            if (cur_col >= 0) {
                /* check if this cell already has a phosphor entry */
                int found = -1, pi;
                for (pi = 0; pi < cursor_phosphor_count; pi++) {
                    if (cursor_phosphor_col[pi] == cur_col && cursor_phosphor_row[pi] == cur_row)
                        { found = pi; break; }
                }
                if (found >= 0) {
                    cursor_phosphor[found] = 1.0f;
                } else if (cursor_phosphor_count < CURSOR_PHOSPHOR_MAX) {
                    int idx = cursor_phosphor_count++;
                    cursor_phosphor[idx] = 1.0f;
                    cursor_phosphor_col[idx] = cur_col;
                    cursor_phosphor_row[idx] = cur_row;
                }
                gen_cursor_plink();
            }
        }

        /* decay all phosphor entries and draw them */
        glDisable(GL_TEXTURE_2D);
        {
            int pi;
            for (pi = 0; pi < cursor_phosphor_count; pi++) {
                /* two-phase decay: fast above 0.15, very slow below —
                   the last embers linger like CRT phosphor cooling */
                float p = cursor_phosphor[pi];
                if (p > 0.15f) p *= 0.96f;       /* fast initial drop */
                else            p *= 0.993f;      /* long tail: ~10 seconds to black */
                cursor_phosphor[pi] = p;
                if (p < 0.002f) {
                    cursor_phosphor[pi] = cursor_phosphor[--cursor_phosphor_count];
                    cursor_phosphor_col[pi] = cursor_phosphor_col[cursor_phosphor_count];
                    cursor_phosphor_row[pi] = cursor_phosphor_row[cursor_phosphor_count];
                    pi--; continue;
                }
                float px = (float)(cursor_phosphor_col[pi] * CELL_W);
                float py = (float)(cursor_phosphor_row[pi] * CELL_H);
                float b = p;
                float pr = AMBER_R * b + (float)cbg[0]/255.0f * (1.0f-b);
                float pg = AMBER_G * b + (float)cbg[1]/255.0f * (1.0f-b);
                float pb = AMBER_B * b + (float)cbg[2]/255.0f * (1.0f-b);
                glColor4f(pr, pg, pb, 1.0f);
                glBegin(GL_QUADS);
                glVertex2f(px,py); glVertex2f(px+CELL_W,py);
                glVertex2f(px+CELL_W,py+CELL_H); glVertex2f(px,py+CELL_H);
                glEnd();
                /* redraw glyph if present */
                Cell *pcl = &grid[cursor_phosphor_row[pi]][cursor_phosphor_col[pi]];
                if (pcl->ch != ' ' && pcl->ch != 0) {
                    int gi = glyph_index(pcl->ch);
                    float u0=(float)((gi%g_acols)*FONT_W)/(float)g_atlas_w;
                    float v0=(float)((gi/g_acols)*FONT_H)/(float)g_atlas_h;
                    float u1=u0+(float)FONT_W/(float)g_atlas_w;
                    float v1=v0+(float)FONT_H/(float)g_atlas_h;
                    glEnable(GL_TEXTURE_2D);
                    glBindTexture(GL_TEXTURE_2D, gl_font_tex);
                    unsigned char *fg = pcl->inv ? pcl->bg : pcl->fg;
                    glColor3ub(fg[0], fg[1], fg[2]);
                    glBegin(GL_QUADS);
                    glTexCoord2f(u0,v0); glVertex2f(px,py);
                    glTexCoord2f(u1,v0); glVertex2f(px+CELL_W,py);
                    glTexCoord2f(u1,v1); glVertex2f(px+CELL_W,py+CELL_H);
                    glTexCoord2f(u0,v1); glVertex2f(px,py+CELL_H);
                    glEnd();
                    glDisable(GL_TEXTURE_2D);
                }
            }
        }

        /* active cursor: red-amber floor at current position */
        if (cur_col >= 0) {
            float ix = (float)(cur_col * CELL_W), iy = (float)(cur_row * CELL_H);
            glDisable(GL_TEXTURE_2D);
            glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE);
            glColor4f(CURSOR_FLOOR_R, CURSOR_FLOOR_G, CURSOR_FLOOR_B, CURSOR_FLOOR_A);
            glBegin(GL_QUADS);
            glVertex2f(ix,iy); glVertex2f(ix+CELL_W,iy);
            glVertex2f(ix+CELL_W,iy+CELL_H); glVertex2f(ix,iy+CELL_H);
            glEnd();
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        }
    }

    /* ── morse scan-line visualization ─────────────────────────────── */
    /* decay all trails each frame */
    { int ti; for (ti = 0; ti < MORSE_TRAIL_COLS; ti++) morse_trail[ti] *= MORSE_TRAIL_DECAY; }

    if (audio_playing && morse_vis_count > 0 && audio_len > 0) {
        /* find which character is currently playing */
        int ci = morse_vis_count - 1;
        { int i; for (i = 0; i < morse_vis_count - 1; i++) {
            if (audio_pos < morse_vis_starts[i+1]) { ci = i; break; }
        }}

        /* skip non-sounding chars (spaces, punctuation, newlines) */
        if (ci < morse_vis_count && morse_vis_has_sound[ci]) {
            /* compute fractional progress within this character's time slice */
            int char_start = morse_vis_starts[ci];
            int char_end = (ci + 1 < morse_vis_count) ? morse_vis_starts[ci+1] : audio_len;
            float frac = (char_end > char_start) ?
                (float)(audio_pos - char_start) / (float)(char_end - char_start) : 0.0f;
            if (frac < 0) frac = 0; if (frac > 1) frac = 1;

            /* is audio currently pulsing (non-silent)? check amplitude at playback pos */
            float amp = 0;
            if (audio_pos >= 0 && audio_pos < audio_len && audio_buf)
                amp = fabsf(audio_buf[audio_pos]);

            /* pulse: boost scan_bright to 0.8, otherwise decay toward base 0.4
               decay factor: ~5 pixels of cell width at 60fps. At 8px wide cell,
               each frame the scan moves frac*CELL_W pixels. We want ~5px decay,
               so per-frame decay ≈ exp(-CELL_W*frac_per_frame / 5). Approximate: 0.7 per frame */
            if (amp > 0.05f) morse_scan_bright = 0.8f;
            else morse_scan_bright = 0.4f + (morse_scan_bright - 0.4f) * 0.7f;

            int col = morse_vis_cols[ci];
            int row = morse_vis_rows[ci];
            float sx = (float)(col * CELL_W) + frac * (float)CELL_W;

            /* update trail for this column */
            if (col >= 0 && col < MORSE_TRAIL_COLS) {
                if (morse_scan_bright > morse_trail[col]) morse_trail[col] = morse_scan_bright;
                morse_trail_rows[col] = row;
            }

            /* draw the active scan line — 1px wide, full cell height */
            glDisable(GL_TEXTURE_2D);
            glColor4f(AMBER_R, AMBER_G, AMBER_B, morse_scan_bright);
            glBegin(GL_QUADS);
            float sy = (float)(row * CELL_H);
            glVertex2f(sx, sy); glVertex2f(sx+1, sy);
            glVertex2f(sx+1, sy+CELL_H); glVertex2f(sx, sy+CELL_H);
            glEnd();
        }
    }

    /* draw trail lines for all columns with residual brightness */
    {
        glDisable(GL_TEXTURE_2D);
        glBegin(GL_QUADS);
        int ti;
        for (ti = 0; ti < MORSE_TRAIL_COLS; ti++) {
            if (morse_trail[ti] < 0.01f) continue;
            float tx = (float)(ti * CELL_W) + (float)CELL_W * 0.5f; /* center of cell */
            float ty = (float)(morse_trail_rows[ti] * CELL_H);
            glColor4f(AMBER_R, AMBER_G, AMBER_B, morse_trail[ti]);
            glVertex2f(tx, ty); glVertex2f(tx+1, ty);
            glVertex2f(tx+1, ty+CELL_H); glVertex2f(tx, ty+CELL_H);
        }
        glEnd();
    }

    /* deletion burn-in: fading ghost glyphs where characters were deleted */
    {
        int di;
        for (di = 0; di < del_ghost_count; di++) {
            DelGhost *dg = &del_ghosts[di];
            dg->bright *= DEL_GHOST_DECAY;
            if (dg->bright < 0.005f) {
                del_ghosts[di] = del_ghosts[--del_ghost_count];
                di--; continue;
            }
            float gx = (float)(dg->col * CELL_W), gy = (float)(dg->row * CELL_H);
            float b = dg->bright;
            /* amber background glow */
            glDisable(GL_TEXTURE_2D);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE);
            glColor4f(AMBER_R, AMBER_G, AMBER_B, b * 0.2f);
            glBegin(GL_QUADS);
            glVertex2f(gx,gy); glVertex2f(gx+CELL_W,gy);
            glVertex2f(gx+CELL_W,gy+CELL_H); glVertex2f(gx,gy+CELL_H);
            glEnd();
            /* ghost glyph */
            if (dg->ch > ' ') {
                int gi = glyph_index(dg->ch);
                float u0=(float)((gi%g_acols)*FONT_W)/(float)g_atlas_w;
                float v0=(float)((gi/g_acols)*FONT_H)/(float)g_atlas_h;
                float u1=u0+(float)FONT_W/(float)g_atlas_w;
                float v1=v0+(float)FONT_H/(float)g_atlas_h;
                glEnable(GL_TEXTURE_2D);
                glBindTexture(GL_TEXTURE_2D, gl_font_tex);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE);
                glColor4f((float)dg->fg[0]/255.0f, (float)dg->fg[1]/255.0f,
                          (float)dg->fg[2]/255.0f, b * 0.6f);
                glBegin(GL_QUADS);
                glTexCoord2f(u0,v0); glVertex2f(gx,gy);
                glTexCoord2f(u1,v0); glVertex2f(gx+CELL_W,gy);
                glTexCoord2f(u1,v1); glVertex2f(gx+CELL_W,gy+CELL_H);
                glTexCoord2f(u0,v1); glVertex2f(gx,gy+CELL_H);
                glEnd();
            }
        }
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }

    [[NSOpenGLContext currentContext] flushBuffer];
}

static void platform_spawn(const char *cmd, char *const *args) {
    struct winsize ws = { (unsigned short)term_rows, (unsigned short)term_cols, 0, 0 };
    child_pid = forkpty(&pty_master, NULL, NULL, &ws);
    if (child_pid == 0) {
        /* child */
        setenv("TERM", "xterm-256color", 1);
        setenv("COLORTERM", "truecolor", 1);
        execvp(cmd, args);
        perror("execvp");
        _exit(1);
    }
    /* make pty non-blocking */
    int flags = fcntl(pty_master, F_GETFL); fcntl(pty_master, F_SETFL, flags | O_NONBLOCK);
    child_alive = 1;
}

static void platform_read_child(void) {
    unsigned char buf[4096];
    int n;
    while ((n = read(pty_master, buf, sizeof(buf))) > 0) {
        vt_feed(buf, n);
    }
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
        int status; if (waitpid(child_pid, &status, WNOHANG) > 0) child_alive = 0;
    }
}

static void platform_write_child(const char *data, int len) {
    if (pty_master >= 0) write(pty_master, data, len);
}

/* resize callback for remote clients: update PTY size + terminal grid */
static void mac_server_on_resize(int cols, int rows) {
    apply_resize(cols, rows);
}

/* forward declarations */
static void paste_from_clipboard(void);
static void copy_screen(void);
static void copy_history(void);
static void platform_write_child(const char *data, int len);
static void net_write_child(const char *data, int len);

/* old GLUT callbacks — only compiled on non-Apple platforms (Windows uses GLUT) */
#ifndef __APPLE__
static void mouse_cb(int button, int state, int x, int y) {
    if (button == 3 && state == GLUT_DOWN) {
        scroll_vel += SCROLL_IMPULSE; scroll_snapping = 0; scroll_locked = 0; return;
    }
    if (button == 4 && state == GLUT_DOWN) {
        scroll_vel -= SCROLL_IMPULSE; scroll_snapping = 0; return;
    }
    if (button == GLUT_LEFT_BUTTON) {
        int col = x / CELL_W, row = y / CELL_H;
        if (col >= term_cols) col = term_cols - 1;
        if (row >= term_rows) row = term_rows - 1;
        if (col < 0) col = 0; if (row < 0) row = 0;
        if (state == GLUT_DOWN) {
            int mods = glutGetModifiers();
            /* double-click detection */
            static int last_click_col = -1, last_click_row = -1;
            static int last_click_time = 0;
            int now = glutGet(GLUT_ELAPSED_TIME);
            if (col == last_click_col && row == last_click_row && (now - last_click_time) < 400) {
                /* double click → select word */
                int c0 = col, c1 = col;
                while (c0 > 0 && grid[row][c0-1].ch > ' ') c0--;
                while (c1 < term_cols-1 && grid[row][c1+1].ch > ' ') c1++;
                if (c1 >= c0 && grid[row][col].ch > ' ') {
                    sel_start_col = c0; sel_end_col = c1;
                    sel_start_row = sel_end_row = row;
                    sel_mode = 1; sel_dragging = 0;
                }
                last_click_col = -1; /* reset so triple click doesn't re-trigger */
            } else {
                /* single click → start new selection */
                sel_start_col = sel_end_col = col;
                sel_start_row = sel_end_row = row;
                sel_mode = (mods & (GLUT_ACTIVE_ALT | GLUT_ACTIVE_CTRL)) ? 2 : 1;
                sel_dragging = 1;
            }
            last_click_col = col; last_click_row = row; last_click_time = now;
        } else { /* GLUT_UP */
            sel_dragging = 0;
            if (sel_start_col == sel_end_col && sel_start_row == sel_end_row)
                sel_clear();
        }
    }
}
/* (continued below — more GLUT callbacks for non-Apple platforms) */
/* apply_resize moved outside #ifndef __APPLE__ — see below */

static void reshape_cb(int w, int h) {
    glViewport(0,0,w,h);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0,w,h,0,-1,1);
    glMatrixMode(GL_MODELVIEW);
    apply_resize(w / CELL_W, h / CELL_H);
}

static void keyboard_cb(unsigned char key, int x, int y) {
    (void)x; (void)y;
    if (key == 22) { /* Ctrl+V */
        if (sel_mode) { sel_delete_via_keys(platform_write_child); sel_clear(); }
        paste_from_clipboard(); return;
    }
    if (key == 3 && sel_mode) { sel_copy(); sel_clear(); return; } /* Ctrl+C with selection → copy */
    if (key == 27) { sel_clear(); } /* Escape clears selection */
    if (sel_mode && key == 0x7F) {
        /* Backspace with selection → smart delete */
        sel_delete_via_keys(platform_write_child); sel_clear(); return;
    }
    if (sel_mode && key >= 0x20 && key < 0x7F) {
        /* Printable char with selection → delete then type */
        sel_delete_via_keys(platform_write_child); sel_clear();
    }
    char c = key;
    platform_write_child(&c, 1);
}

static void special_cb(int key, int x, int y) {
    (void)x; (void)y;
    int mods = glutGetModifiers();
    /* Shift+Arrow = create/extend selection */
    if ((mods & GLUT_ACTIVE_SHIFT) &&
        (key == GLUT_KEY_LEFT || key == GLUT_KEY_RIGHT || key == GLUT_KEY_UP || key == GLUT_KEY_DOWN ||
         key == GLUT_KEY_HOME || key == GLUT_KEY_END)) {
        if (!sel_mode) {
            /* start new selection anchored at cursor */
            sel_start_col = sel_end_col = cur_x;
            sel_start_row = sel_end_row = cur_y;
            sel_mode = 1;
        }
        /* extend the end point */
        if (key == GLUT_KEY_LEFT) { if (sel_end_col > 0) sel_end_col--; else if (sel_end_row > 0) { sel_end_row--; sel_end_col = term_cols-1; } }
        else if (key == GLUT_KEY_RIGHT) { if (sel_end_col < term_cols-1) sel_end_col++; else if (sel_end_row < term_rows-1) { sel_end_row++; sel_end_col = 0; } }
        else if (key == GLUT_KEY_UP) { if (sel_end_row > 0) sel_end_row--; }
        else if (key == GLUT_KEY_DOWN) { if (sel_end_row < term_rows-1) sel_end_row++; }
        else if (key == GLUT_KEY_HOME) { sel_end_col = 0; }
        else if (key == GLUT_KEY_END) { sel_end_col = row_last_char(sel_end_row); if (sel_end_col < 0) sel_end_col = 0; }
        return; /* don't send to child */
    }
    /* arrow keys without shift clear selection */
    if (sel_mode &&
        (key == GLUT_KEY_UP || key == GLUT_KEY_DOWN || key == GLUT_KEY_LEFT || key == GLUT_KEY_RIGHT ||
         key == GLUT_KEY_HOME || key == GLUT_KEY_END))
        sel_clear();
    /* Shift+PgUp/PgDn = smooth scroll through history */
    if (mods & GLUT_ACTIVE_SHIFT) {
        if (key == GLUT_KEY_PAGE_UP) { scroll_vel += SCROLL_IMPULSE * 5; scroll_locked = 0; scroll_snapping = 0; return; }
        if (key == GLUT_KEY_PAGE_DOWN) { scroll_vel -= SCROLL_IMPULSE * 5; scroll_snapping = 0; return; }
        if (key == GLUT_KEY_UP) { scroll_vel += SCROLL_IMPULSE; scroll_locked = 0; scroll_snapping = 0; return; }
        if (key == GLUT_KEY_DOWN) { scroll_vel -= SCROLL_IMPULSE; scroll_snapping = 0; return; }
        if (key == GLUT_KEY_HOME) { scroll_target = (float)hist_count * CELL_H; scroll_snapping = 1; scroll_locked = 0; return; }
        if (key == GLUT_KEY_END) { scroll_target = 0; scroll_snapping = 1; return; }
    }
    char buf[8]; int len = 0;
    switch (key) {
    case GLUT_KEY_UP: len=sprintf(buf,"\x1b[A"); break;
    case GLUT_KEY_DOWN: len=sprintf(buf,"\x1b[B"); break;
    case GLUT_KEY_RIGHT: len=sprintf(buf,"\x1b[C"); break;
    case GLUT_KEY_LEFT: len=sprintf(buf,"\x1b[D"); break;
    case GLUT_KEY_HOME: len=sprintf(buf,"\x1b[H"); break;
    case GLUT_KEY_END: len=sprintf(buf,"\x1b[F"); break;
    case GLUT_KEY_PAGE_UP: len=sprintf(buf,"\x1b[5~"); break;
    case GLUT_KEY_PAGE_DOWN: len=sprintf(buf,"\x1b[6~"); break;
    case GLUT_KEY_F1: len=sprintf(buf,"\x1bOP"); break;
    case GLUT_KEY_F2: len=sprintf(buf,"\x1bOQ"); break;
    }
    if (len) platform_write_child(buf, len);
}

/* display/idle callbacks for network client mode */
static void display_client_cb(void) {
    if (g_net_sock != SOCK_INVALID) net_read_into_vt(g_net_sock);
    gl_render();
}
static void net_write_child(const char *data, int len) { net_write(g_net_sock, data, len); }
static void keyboard_client_cb(unsigned char key, int x, int y) {
    (void)x; (void)y;
    if (key == 22) { paste_from_clipboard(); return; }
    if (key == 3 && sel_mode) { sel_copy(); sel_clear(); return; }
    if (key == 27) { sel_clear(); }
    if (sel_mode && key == 0x7F) { sel_delete_via_keys(net_write_child); sel_clear(); return; }
    if (sel_mode && key >= 0x20 && key < 0x7F) { sel_delete_via_keys(net_write_child); sel_clear(); }
    char c = key; net_write(g_net_sock, &c, 1);
}
static void special_client_cb(int key, int x, int y) {
    (void)x; (void)y;
    int mods = glutGetModifiers();
    if ((mods & GLUT_ACTIVE_SHIFT) &&
        (key == GLUT_KEY_LEFT || key == GLUT_KEY_RIGHT || key == GLUT_KEY_UP || key == GLUT_KEY_DOWN ||
         key == GLUT_KEY_HOME || key == GLUT_KEY_END)) {
        if (!sel_mode) { sel_start_col = sel_end_col = cur_x; sel_start_row = sel_end_row = cur_y; sel_mode = 1; }
        if (key == GLUT_KEY_LEFT) { if (sel_end_col > 0) sel_end_col--; else if (sel_end_row > 0) { sel_end_row--; sel_end_col = term_cols-1; } }
        else if (key == GLUT_KEY_RIGHT) { if (sel_end_col < term_cols-1) sel_end_col++; else if (sel_end_row < term_rows-1) { sel_end_row++; sel_end_col = 0; } }
        else if (key == GLUT_KEY_UP) { if (sel_end_row > 0) sel_end_row--; }
        else if (key == GLUT_KEY_DOWN) { if (sel_end_row < term_rows-1) sel_end_row++; }
        else if (key == GLUT_KEY_HOME) { sel_end_col = 0; }
        else if (key == GLUT_KEY_END) { sel_end_col = row_last_char(sel_end_row); if (sel_end_col < 0) sel_end_col = 0; }
        return;
    }
    if (sel_mode && !(mods & GLUT_ACTIVE_SHIFT) &&
        (key == GLUT_KEY_UP || key == GLUT_KEY_DOWN || key == GLUT_KEY_LEFT || key == GLUT_KEY_RIGHT ||
         key == GLUT_KEY_HOME || key == GLUT_KEY_END))
        sel_clear();
    if (mods & GLUT_ACTIVE_SHIFT) {
        if (key == GLUT_KEY_PAGE_UP) { scroll_vel += SCROLL_IMPULSE * 5; scroll_locked = 0; scroll_snapping = 0; return; }
        if (key == GLUT_KEY_PAGE_DOWN) { scroll_vel -= SCROLL_IMPULSE * 5; scroll_snapping = 0; return; }
        /* Shift+Up/Down without selection = scroll (handled above if selection started) */
        if (key == GLUT_KEY_HOME) { scroll_target = (float)hist_count * CELL_H; scroll_snapping = 1; scroll_locked = 0; return; }
        if (key == GLUT_KEY_END) { scroll_target = 0; scroll_snapping = 1; return; }
    }
    char buf[8]; int len = 0;
    switch (key) {
    case GLUT_KEY_UP: len=sprintf(buf,"\x1b[A"); break;
    case GLUT_KEY_DOWN: len=sprintf(buf,"\x1b[B"); break;
    case GLUT_KEY_RIGHT: len=sprintf(buf,"\x1b[C"); break;
    case GLUT_KEY_LEFT: len=sprintf(buf,"\x1b[D"); break;
    case GLUT_KEY_HOME: len=sprintf(buf,"\x1b[H"); break;
    case GLUT_KEY_END: len=sprintf(buf,"\x1b[F"); break;
    case GLUT_KEY_PAGE_UP: len=sprintf(buf,"\x1b[5~"); break;
    case GLUT_KEY_PAGE_DOWN: len=sprintf(buf,"\x1b[6~"); break;
    }
    if (len) net_write(g_net_sock, buf, len);
}
#endif /* !__APPLE__ — end of GLUT callbacks (moved earlier) */

static void net_write_child(const char *data, int len) { net_write(g_net_sock, data, len); }

static void apply_resize(int new_cols, int new_rows) {
    if (new_cols == term_cols && new_rows == term_rows) return;
    if (new_cols < 10) new_cols = 10; if (new_rows < 4) new_rows = 4;
    if (new_cols > TERM_COLS) new_cols = TERM_COLS;
    if (new_rows > TERM_ROWS) new_rows = TERM_ROWS;
    term_cols = new_cols; term_rows = new_rows;

    if (g_net_mode == 2) {
        /* remote client: just update dimensions and tell server — don't clear grid/history.
           The server's child will redraw and send new content over the wire. */
        scroll_y = 0; scroll_vel = 0; scroll_snapping = 0; scroll_locked = 1;
        if (g_net_sock != SOCK_INVALID) {
            char msg[32]; int len = sprintf(msg, "\x1b_RESIZE;%d;%d\x1b\\", term_cols, term_rows);
            net_write(g_net_sock, msg, len);
        }
        return;
    }

    /* local or server mode: clear grid — child will redraw after SIGWINCH */
    int r, c;
    for (r = 0; r < TERM_ROWS; r++)
        for (c = 0; c < TERM_COLS; c++) {
            grid[r][c].ch = ' '; grid[r][c].bg[0]=grid[r][c].bg[1]=grid[r][c].bg[2]=0;
            grid[r][c].fg[0]=grid[r][c].fg[1]=grid[r][c].fg[2]=192; grid[r][c].inv=grid[r][c].bold=0;
        }
    cur_x = 0; cur_y = 0;
    scroll_y = 0; scroll_vel = 0; scroll_snapping = 0; scroll_locked = 1;
    hist_count = 0; hist_write = 0;
#ifndef _WIN32
    if (pty_master >= 0) { struct winsize ws = {(unsigned short)term_rows,(unsigned short)term_cols,0,0}; ioctl(pty_master, TIOCSWINSZ, &ws); }
#endif
}

/* Server mode: headless, forward child↔network (no GL window) */
/* helpers for headless server resize filtering (use static pty fd) */
static int _headless_pty_fd;
static void _headless_fwd_to_child(const char *d, int l) { write(_headless_pty_fd, d, l); }
static void _headless_on_resize(int c, int r) {
    struct winsize ws = {(unsigned short)r, (unsigned short)c, 0, 0};
    ioctl(_headless_pty_fd, TIOCSWINSZ, &ws);
}

static int run_server(const char *addr, const char *child_cmd, char *const *child_argv) {
    if (net_serve(addr, child_cmd) != 0) return 1;

    /* spawn child with PTY */
    struct winsize ws = { (unsigned short)term_rows, (unsigned short)term_cols, 0, 0 };
    int pty_fd;
    pid_t pid = forkpty(&pty_fd, NULL, NULL, &ws);
    if (pid == 0) {
        setenv("TERM", "xterm-256color", 1);
        setenv("COLORTERM", "truecolor", 1);
        execvp(child_cmd, child_argv);
        perror("execvp"); _exit(1);
    }
    int fl = fcntl(pty_fd, F_GETFL); fcntl(pty_fd, F_SETFL, fl | O_NONBLOCK);
    net_set_nonblock(g_net_sock);
    fprintf(stderr, "dumbterm: child pid %d, forwarding bytes\n", pid);

    /* bidirectional forwarding loop: child↔network */
    fd_set rfds;
    unsigned char buf[4096];
    for (;;) {
        FD_ZERO(&rfds);
        FD_SET(pty_fd, &rfds);
        FD_SET(g_net_sock, &rfds);
        int maxfd = pty_fd > (int)g_net_sock ? pty_fd : (int)g_net_sock;
        struct timeval tv = {0, 50000}; /* 50ms */
        int ret = select(maxfd+1, &rfds, NULL, NULL, &tv);
        if (ret < 0) break;

        if (FD_ISSET(pty_fd, &rfds)) {
            int n = read(pty_fd, buf, sizeof(buf));
            if (n > 0) send(g_net_sock, buf, n, 0);
            else if (n == 0) break;
        }
        if (FD_ISSET(g_net_sock, &rfds)) {
            int n = recv(g_net_sock, buf, sizeof(buf), 0);
            if (n > 0) {
                _headless_pty_fd = pty_fd;
                net_filter_and_forward(buf, n, _headless_fwd_to_child, _headless_on_resize);
            }
            else if (n == 0) break;
        }
        /* check child */
        int status;
        if (waitpid(pid, &status, WNOHANG) > 0) {
            fprintf(stderr, "dumbterm: child exited\n");
            /* drain remaining output */
            usleep(200000);
            int n; while ((n = read(pty_fd, buf, sizeof(buf))) > 0) send(g_net_sock, buf, n, 0);
            break;
        }
    }
    sock_close(g_net_sock);
    return 0;
}

/* ── clipboard helpers ──────────────────────────────────────────── */

/* Encode a Cell row to UTF-8, trimming trailing spaces. Returns bytes written. */
static int row_to_utf8(const Cell *row, int cols, char *out) {
    int pos = 0, c, last = -1;
    for (c = 0; c < cols; c++)
        if (row[c].ch != ' ' && row[c].ch != 0) last = c;
    for (c = 0; c <= last; c++) {
        unsigned short ch = row[c].ch;
        if (ch == 0) ch = ' ';
        if (ch < 0x80) out[pos++] = (char)ch;
        else if (ch < 0x800) { out[pos++] = 0xC0|(ch>>6); out[pos++] = 0x80|(ch&0x3F); }
        else { out[pos++] = 0xE0|(ch>>12); out[pos++] = 0x80|((ch>>6)&0x3F); out[pos++] = 0x80|(ch&0x3F); }
    }
    return pos;
}

static void to_clipboard(const char *text, int len) {
#ifdef __APPLE__
    FILE *p = popen("pbcopy", "w");
    if (p) { fwrite(text, 1, len, p); pclose(p); }
#elif defined(_WIN32)
    if (OpenClipboard(NULL)) {
        EmptyClipboard();
        HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, len + 1);
        if (hg) { char *p = (char*)GlobalLock(hg); memcpy(p, text, len); p[len]=0; GlobalUnlock(hg); SetClipboardData(CF_TEXT, hg); }
        CloseClipboard();
    }
#else
    FILE *p = popen("xclip -selection clipboard 2>/dev/null || xsel --clipboard 2>/dev/null", "w");
    if (p) { fwrite(text, 1, len, p); pclose(p); }
#endif
}

static void copy_screen(void) {
    char *buf = (char*)malloc(TERM_COLS * term_rows * 4 + term_rows + 1);
    int pos = 0, r;
    for (r = 0; r < term_rows; r++) {
        pos += row_to_utf8(grid[r], TERM_COLS, buf + pos);
        buf[pos++] = '\n';
    }
    to_clipboard(buf, pos);
    free(buf);
}

static void copy_history(void) {
    /* Copy all history + current screen */
    int total_lines = hist_count + term_rows;
    char *buf = (char*)malloc(TERM_COLS * total_lines * 4 + total_lines + 1);
    int pos = 0, i, r;
    /* history (oldest first) */
    for (i = 0; i < hist_count; i++) {
        int idx = (hist_write - hist_count + i + HIST_LINES) % HIST_LINES;
        pos += row_to_utf8(history[idx], TERM_COLS, buf + pos);
        buf[pos++] = '\n';
    }
    /* current screen */
    for (r = 0; r < term_rows; r++) {
        pos += row_to_utf8(grid[r], TERM_COLS, buf + pos);
        buf[pos++] = '\n';
    }
    to_clipboard(buf, pos);
    free(buf);
}

static void paste_from_clipboard(void) {
    char *text = NULL;
    int len = 0;
#ifdef __APPLE__
    FILE *p = popen("pbpaste", "r");
    if (p) {
        text = (char*)malloc(1024*1024);
        len = fread(text, 1, 1024*1024-1, p);
        pclose(p);
    }
#elif defined(_WIN32)
    if (OpenClipboard(NULL)) {
        HANDLE hg = GetClipboardData(CF_TEXT);
        if (hg) { char *p = (char*)GlobalLock(hg); len = strlen(p); text = (char*)malloc(len+1); memcpy(text,p,len+1); GlobalUnlock(hg); }
        CloseClipboard();
    }
#endif
    if (text && len > 0) {
        /* send to child — works for both local and network mode */
        if (g_net_mode == 2 && g_net_sock != SOCK_INVALID)
            net_write(g_net_sock, text, len);
        else
            platform_write_child(text, len);
    }
    if (text) free(text);
}

static void menu_cb(int item) {
    switch (item) {
    case 1: if (sel_mode) { sel_copy(); sel_clear(); } else copy_screen(); break;
    case 2: copy_history(); break;
    case 3: paste_from_clipboard(); break;
    }
    sel_clear(); /* right-click always clears selection */
}

/* ── Cocoa menu delegate (handles Copy/Paste/Font actions) ─────── */
#ifdef __OBJC__
@interface DTMenuHandler : NSObject
- (void)copyScreen:(id)sender;
- (void)copyHistory:(id)sender;
- (void)paste:(id)sender;
- (void)setFontScale:(id)sender;
@end
@implementation DTMenuHandler
- (void)copyScreen:(id)sender { if (sel_mode) { sel_copy(); sel_clear(); } else copy_screen(); }
- (void)copyHistory:(id)sender { copy_history(); }
- (void)paste:(id)sender { paste_from_clipboard(); }
- (void)setFontScale:(id)sender { font_scale = (int)[sender tag]; }
- (void)setMorsePreset:(id)sender { morse_preset = (int)[sender tag]; }
- (void)setMorseVol:(id)sender { morse_vol = (int)[sender tag]; }
- (void)setMorseSpeed:(id)sender { morse_speed = (int)[sender tag]; }
- (void)setHoverPreset:(id)sender { hover_preset = (int)[sender tag]; }
- (void)setHoverVol:(id)sender { hover_vol = (int)[sender tag]; }
- (void)setTypePreset:(id)sender { type_preset = (int)[sender tag]; }
- (void)setTypeVol:(id)sender { type_vol = (int)[sender tag]; }
- (void)setDeletePreset:(id)sender { delete_preset = (int)[sender tag]; }
- (void)setDeleteVol:(id)sender { delete_vol = (int)[sender tag]; }
- (void)setOutputPreset:(id)sender { output_preset = (int)[sender tag]; }
- (void)setOutputVol:(id)sender { output_vol = (int)[sender tag]; }
- (BOOL)validateMenuItem:(NSMenuItem *)item {
    /* show check marks on currently selected settings */
    SEL act = [item action];
    int tag = (int)[item tag];
    if (act == @selector(setMorsePreset:)) [item setState:(tag==morse_preset)?NSControlStateValueOn:NSControlStateValueOff];
    else if (act == @selector(setMorseVol:)) [item setState:(tag==morse_vol)?NSControlStateValueOn:NSControlStateValueOff];
    else if (act == @selector(setMorseSpeed:)) [item setState:(tag==morse_speed)?NSControlStateValueOn:NSControlStateValueOff];
    else if (act == @selector(setHoverPreset:)) [item setState:(tag==hover_preset)?NSControlStateValueOn:NSControlStateValueOff];
    else if (act == @selector(setHoverVol:)) [item setState:(tag==hover_vol)?NSControlStateValueOn:NSControlStateValueOff];
    else if (act == @selector(setTypePreset:)) [item setState:(tag==type_preset)?NSControlStateValueOn:NSControlStateValueOff];
    else if (act == @selector(setTypeVol:)) [item setState:(tag==type_vol)?NSControlStateValueOn:NSControlStateValueOff];
    else if (act == @selector(setDeletePreset:)) [item setState:(tag==delete_preset)?NSControlStateValueOn:NSControlStateValueOff];
    else if (act == @selector(setDeleteVol:)) [item setState:(tag==delete_vol)?NSControlStateValueOn:NSControlStateValueOff];
    else if (act == @selector(setOutputPreset:)) [item setState:(tag==output_preset)?NSControlStateValueOn:NSControlStateValueOff];
    else if (act == @selector(setOutputVol:)) [item setState:(tag==output_vol)?NSControlStateValueOn:NSControlStateValueOff];
    else if (act == @selector(setFontScale:)) [item setState:(tag==font_scale)?NSControlStateValueOn:NSControlStateValueOff];
    return YES;
}
@end
static DTMenuHandler *g_menu_handler;

/* Subclass NSOpenGLView to redraw during live window resize (prevents scaling) */
@interface DTGLView : NSOpenGLView
@end
@implementation DTGLView
- (void)reshape {
    [super reshape];
    [[self openGLContext] makeCurrentContext];
    NSSize pts = [self bounds].size;
    NSSize px = [self convertSizeToBacking:pts];
    glViewport(0, 0, (int)px.width, (int)px.height);
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glOrtho(0, pts.width, pts.height, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    int new_cols = (int)pts.width / CELL_W, new_rows = (int)pts.height / CELL_H;
    if (new_cols != term_cols || new_rows != term_rows)
        apply_resize(new_cols, new_rows);
    gl_render();
    [[self openGLContext] flushBuffer];
}
@end
#endif

int main(int argc, char **argv) {
    /* parse args */
    const char *child_cmd = NULL;
    char **child_argv = NULL;
    int i;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--listen") == 0 && i+1 < argc) {
            g_listen_addr = argv[++i]; g_net_mode = 1;
        } else if (strcmp(argv[i], "--connect") == 0 && i+1 < argc) {
            g_connect_addr = argv[++i]; g_net_mode = 2;
        } else if (strcmp(argv[i], "--visible") == 0) {
            g_server_visible = 1;
        } else if (strcmp(argv[i], "--") == 0) { i++; break; }
        else break;
    }
    if (i < argc) {
        child_cmd = argv[i];
        child_argv = &argv[i];
    } else {
        char *shell = getenv("SHELL");
        if (!shell) shell = "/bin/bash";
        static char *default_argv[] = { NULL, NULL };
        default_argv[0] = shell;
        child_cmd = shell;
        child_argv = default_argv;
    }

    /* ── SERVER MODE: headless (no --visible) ─────────────────── */
    if (g_net_mode == 1 && !g_server_visible) {
        return run_server(g_listen_addr, child_cmd, child_argv);
    }

    /* ── VISIBLE SERVER, CLIENT, or LOCAL MODE: GL window ──────── */
    build_atlas();
    grid_clear();

    /* if visible server: start listening for clients */
    if (g_net_mode == 1 && g_server_visible) {
        if (net_listen_start(g_listen_addr) != 0) return 1;
    }

    /* ── Native Cocoa window + NSOpenGLView ───────────────────── */
    if (g_net_mode == 2) {
        g_net_sock = net_connect(g_connect_addr);
        if (g_net_sock == SOCK_INVALID) return 1;
    }

    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

    /* create menu bar */
    NSMenu *menubar = [[NSMenu alloc] init];
    NSMenuItem *appMenuItem = [[NSMenuItem alloc] init];
    [menubar addItem:appMenuItem];
    [NSApp setMainMenu:menubar];
    NSMenu *appMenu = [[NSMenu alloc] init];
    [appMenu addItemWithTitle:@"Quit dumbterm" action:@selector(terminate:) keyEquivalent:@"q"];
    [appMenuItem setSubmenu:appMenu];

    /* menu handler for actions */
    g_menu_handler = [[DTMenuHandler alloc] init];

    /* Edit menu — Copy/Paste */
    NSMenuItem *editItem = [[NSMenuItem alloc] init];
    [menubar addItem:editItem];
    NSMenu *editMenu = [[NSMenu alloc] initWithTitle:@"Edit"];
    [editMenu addItemWithTitle:@"Copy Screen" action:@selector(copyScreen:) keyEquivalent:@""];
    [editMenu addItemWithTitle:@"Copy All History" action:@selector(copyHistory:) keyEquivalent:@""];
    [editMenu addItemWithTitle:@"Paste" action:@selector(paste:) keyEquivalent:@""];
    for (NSMenuItem *mi in [editMenu itemArray]) [mi setTarget:g_menu_handler];
    [editItem setSubmenu:editMenu];

    /* Display menu — pixel-perfect font scales */
    NSMenuItem *dispItem = [[NSMenuItem alloc] init];
    [menubar addItem:dispItem];
    NSMenu *dispMenu = [[NSMenu alloc] initWithTitle:@"Display"];
    NSMenuItem *f1 = [dispMenu addItemWithTitle:@"Font 1× (8×16)" action:@selector(setFontScale:) keyEquivalent:@"1"];
    NSMenuItem *f2 = [dispMenu addItemWithTitle:@"Font 2× (16×32)" action:@selector(setFontScale:) keyEquivalent:@"2"];
    NSMenuItem *f3 = [dispMenu addItemWithTitle:@"Font 3× (24×48)" action:@selector(setFontScale:) keyEquivalent:@"3"];
    [f1 setTag:1]; [f2 setTag:2]; [f3 setTag:3];
    [f1 setTarget:g_menu_handler]; [f2 setTarget:g_menu_handler]; [f3 setTarget:g_menu_handler];
    [dispItem setSubmenu:dispMenu];

    /* Sounds menu */
    NSMenuItem *sndItem = [[NSMenuItem alloc] init];
    [menubar addItem:sndItem];
    NSMenu *sndMenu = [[NSMenu alloc] initWithTitle:@"Sounds"];
    /* Morse section */
    [sndMenu addItemWithTitle:@"── Selection Morse ──" action:nil keyEquivalent:@""];
    for (int si = 0; si < NUM_PRESETS; si++) {
        NSString *name = [NSString stringWithFormat:@"  %s", PRESETS[si].name];
        NSMenuItem *mi = [sndMenu addItemWithTitle:name action:@selector(setMorsePreset:) keyEquivalent:@""];
        [mi setTag:si]; [mi setTarget:g_menu_handler];
    }
    [sndMenu addItem:[NSMenuItem separatorItem]];
    [sndMenu addItemWithTitle:@"── Morse Volume ──" action:nil keyEquivalent:@""];
    NSString *mvols[] = {@"  Off", @"  Lo", @"  Med", @"  Hi"};
    for (int vi = 0; vi < 4; vi++) {
        NSMenuItem *mi = [sndMenu addItemWithTitle:mvols[vi] action:@selector(setMorseVol:) keyEquivalent:@""];
        [mi setTag:vi]; [mi setTarget:g_menu_handler];
    }
    [sndMenu addItem:[NSMenuItem separatorItem]];
    /* Hover section */
    [sndMenu addItemWithTitle:@"── Hover Click ──" action:nil keyEquivalent:@""];
    for (int si = 0; si < NUM_PRESETS; si++) {
        NSString *name = [NSString stringWithFormat:@"  %s", PRESETS[si].name];
        NSMenuItem *mi = [sndMenu addItemWithTitle:name action:@selector(setHoverPreset:) keyEquivalent:@""];
        [mi setTag:si]; [mi setTarget:g_menu_handler];
    }
    [sndMenu addItem:[NSMenuItem separatorItem]];
    [sndMenu addItemWithTitle:@"── Hover Volume ──" action:nil keyEquivalent:@""];
    NSString *hvols[] = {@"  Off", @"  Lo", @"  Med", @"  Hi"};
    for (int vi = 0; vi < 4; vi++) {
        NSMenuItem *mi = [sndMenu addItemWithTitle:hvols[vi] action:@selector(setHoverVol:) keyEquivalent:@""];
        [mi setTag:vi]; [mi setTarget:g_menu_handler];
    }
    [sndMenu addItem:[NSMenuItem separatorItem]];
    /* Morse speed */
    [sndMenu addItemWithTitle:@"── Morse Speed ──" action:nil keyEquivalent:@""];
    {
        NSString *labels[] = {
            @"  Super Fast (0.8s max)", @"  Very Fast (1.6s max)", @"  Fast (3s max)",
            @"  Natural (4 ch/s)", @"  5 ch/s", @"  6 ch/s",
            @"  8 ch/s", @"  10 ch/s", @"  12 ch/s", @"  14 ch/s"
        };
        for (int si = 0; si < 10; si++) {
            NSMenuItem *mi = [sndMenu addItemWithTitle:labels[si] action:@selector(setMorseSpeed:) keyEquivalent:@""];
            [mi setTag:si]; [mi setTarget:g_menu_handler];
        }
    }
    [sndMenu addItem:[NSMenuItem separatorItem]];
    /* Typing sound */
    [sndMenu addItemWithTitle:@"── Typing Sound ──" action:nil keyEquivalent:@""];
    for (int si = 0; si < NUM_PRESETS; si++) {
        NSMenuItem *mi = [sndMenu addItemWithTitle:[NSString stringWithFormat:@"  %s", PRESETS[si].name]
            action:@selector(setTypePreset:) keyEquivalent:@""];
        [mi setTag:si]; [mi setTarget:g_menu_handler];
    }
    [sndMenu addItem:[NSMenuItem separatorItem]];
    [sndMenu addItemWithTitle:@"── Typing Volume ──" action:nil keyEquivalent:@""];
    for (int vi = 0; vi < 4; vi++) {
        NSString *vn[] = {@"  Off", @"  Lo", @"  Med", @"  Hi"};
        NSMenuItem *mi = [sndMenu addItemWithTitle:vn[vi] action:@selector(setTypeVol:) keyEquivalent:@""];
        [mi setTag:vi]; [mi setTarget:g_menu_handler];
    }
    [sndMenu addItem:[NSMenuItem separatorItem]];
    /* Delete sound */
    [sndMenu addItemWithTitle:@"── Delete Sound ──" action:nil keyEquivalent:@""];
    for (int si = 0; si < NUM_PRESETS; si++) {
        NSMenuItem *mi = [sndMenu addItemWithTitle:[NSString stringWithFormat:@"  %s", PRESETS[si].name]
            action:@selector(setDeletePreset:) keyEquivalent:@""];
        [mi setTag:si]; [mi setTarget:g_menu_handler];
    }
    [sndMenu addItem:[NSMenuItem separatorItem]];
    [sndMenu addItemWithTitle:@"── Delete Volume ──" action:nil keyEquivalent:@""];
    for (int vi = 0; vi < 4; vi++) {
        NSString *vn[] = {@"  Off", @"  Lo", @"  Med", @"  Hi"};
        NSMenuItem *mi = [sndMenu addItemWithTitle:vn[vi] action:@selector(setDeleteVol:) keyEquivalent:@""];
        [mi setTag:vi]; [mi setTarget:g_menu_handler];
    }
    [sndMenu addItem:[NSMenuItem separatorItem]];
    /* Output sound */
    [sndMenu addItemWithTitle:@"── Output Sound ──" action:nil keyEquivalent:@""];
    for (int si = 0; si < NUM_PRESETS; si++) {
        NSMenuItem *mi = [sndMenu addItemWithTitle:[NSString stringWithFormat:@"  %s", PRESETS[si].name]
            action:@selector(setOutputPreset:) keyEquivalent:@""];
        [mi setTag:si]; [mi setTarget:g_menu_handler];
    }
    [sndMenu addItem:[NSMenuItem separatorItem]];
    [sndMenu addItemWithTitle:@"── Output Volume ──" action:nil keyEquivalent:@""];
    for (int vi = 0; vi < 4; vi++) {
        NSString *vn[] = {@"  Off", @"  Lo", @"  Med", @"  Hi"};
        NSMenuItem *mi = [sndMenu addItemWithTitle:vn[vi] action:@selector(setOutputVol:) keyEquivalent:@""];
        [mi setTag:vi]; [mi setTarget:g_menu_handler];
    }
    [sndItem setSubmenu:sndMenu];

    /* window */
    NSRect frame = NSMakeRect(100, 100, term_cols * CELL_W, term_rows * CELL_H);
    NSUInteger style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                       NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
    NSWindow *window = [[NSWindow alloc] initWithContentRect:frame styleMask:style
                        backing:NSBackingStoreBuffered defer:NO];

    const char *title = "dumbterm";
    if (g_net_mode == 2) {
        static char t[256]; snprintf(t,sizeof(t),"dumbterm [remote:%s]",g_connect_addr); title=t;
    } else if (g_net_mode == 1 && g_server_visible) {
        static char t[256]; snprintf(t,sizeof(t),"dumbterm [server:%s]",g_listen_addr); title=t;
    }
    [window setTitle:[NSString stringWithUTF8String:title]];

    /* OpenGL view */
    NSOpenGLPixelFormatAttribute attrs[] = {
        NSOpenGLPFADoubleBuffer, NSOpenGLPFAColorSize, 24, NSOpenGLPFAAlphaSize, 8, 0
    };
    NSOpenGLPixelFormat *pf = [[NSOpenGLPixelFormat alloc] initWithAttributes:attrs];
    DTGLView *glView = [[DTGLView alloc] initWithFrame:frame pixelFormat:pf];
    [glView setWantsBestResolutionOpenGLSurface:YES];
    [window setContentView:glView];
    [[glView openGLContext] makeCurrentContext];

    /* GL setup */
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glGenTextures(1, &gl_font_tex);
    glBindTexture(GL_TEXTURE_2D, gl_font_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, g_atlas_w, g_atlas_h, 0,
                 GL_ALPHA, GL_UNSIGNED_BYTE, g_atlas);
    {
        NSSize pts = [glView bounds].size;
        NSSize px = [glView convertSizeToBacking:pts];
        glViewport(0, 0, (int)px.width, (int)px.height);
        glMatrixMode(GL_PROJECTION); glLoadIdentity();
        glOrtho(0, pts.width, pts.height, 0, -1, 1);
        glMatrixMode(GL_MODELVIEW);
    }

    scroll_locked = 1;
    audio_init();
    if (g_net_mode != 2) platform_spawn(child_cmd, child_argv);

    /* Cocoa event handlers for keyboard, scroll, mouse */
    /* Scroll wheel */
    [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskScrollWheel
        handler:^NSEvent *(NSEvent *event) {
            float dy = [event scrollingDeltaY];
            if ([event hasPreciseScrollingDeltas]) {
                scroll_y += dy;
                float ady = fabsf(dy);
                float vel_add;
                if (ady < 2.0f) vel_add = 0;
                else if (ady < 8.0f) vel_add = dy * 0.1f;
                else if (ady < 20.0f) vel_add = dy * 0.25f;
                else vel_add = dy * 0.5f;
                scroll_vel += vel_add;
            } else {
                scroll_vel += dy * SCROLL_IMPULSE * 0.4f;
            }
            if (dy > 0) scroll_locked = 0;
            scroll_snapping = 0; scroll_idle_frames = 0;
            return event;
        }];

    /* define send function based on mode */
    void (^sendToChild)(const char*, int) = ^(const char *data, int len) {
        if (g_net_mode == 2) net_write(g_net_sock, data, len);
        else platform_write_child(data, len);
    };

    /* Keyboard */
    [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
        handler:^NSEvent *(NSEvent *event) {
            NSUInteger mods = [event modifierFlags];
            NSString *chars = [event charactersIgnoringModifiers];
            NSString *raw = [event characters];
            BOOL cmd = (mods & NSEventModifierFlagCommand) != 0;
            BOOL shift = (mods & NSEventModifierFlagShift) != 0;

            /* Cmd+key shortcuts */
            if (cmd) {
                if ([chars isEqualToString:@"c"]) { if (sel_mode) { sel_copy(); sel_clear(); } return nil; }
                if ([chars isEqualToString:@"x"]) {
                    if (sel_mode) { sel_copy(); sel_play_reverse_morse(); sel_delete_via_keys(g_net_mode==2 ? net_write_child : platform_write_child); sel_clear(); }
                    return nil;
                }
                if ([chars isEqualToString:@"1"]) { font_scale = 1; return nil; }
                if ([chars isEqualToString:@"2"]) { font_scale = 2; return nil; }
                if ([chars isEqualToString:@"3"]) { font_scale = 3; return nil; }
                if ([chars isEqualToString:@"v"]) {
                    if (sel_mode) { sel_delete_via_keys(g_net_mode==2 ? net_write_child : platform_write_child); sel_clear(); }
                    paste_from_clipboard(); return nil;
                }
                return event; /* let system handle Cmd+Q etc */
            }

            /* special keys */
            unsigned short kc = [event keyCode];
            /* arrow keys */
            if (kc == 123 || kc == 124 || kc == 125 || kc == 126) {
                /* Shift+Arrow = selection */
                if (shift) {
                    if (!sel_mode) { sel_start_col=sel_end_col=cur_x; sel_start_row=sel_end_row=cur_y; sel_mode=1; }
                    if (kc==123) { if(sel_end_col>0)sel_end_col--; else if(sel_end_row>0){sel_end_row--;sel_end_col=term_cols-1;} }
                    else if(kc==124) { if(sel_end_col<term_cols-1)sel_end_col++; else if(sel_end_row<term_rows-1){sel_end_row++;sel_end_col=0;} }
                    else if(kc==126) { if(sel_end_row>0)sel_end_row--; }
                    else if(kc==125) { if(sel_end_row<term_rows-1)sel_end_row++; }
                    return nil;
                }
                if (sel_mode) sel_clear();
                const char *seq[] = {"\x1b[D","\x1b[C","\x1b[B","\x1b[A"};
                int idx = kc==123?0:kc==124?1:kc==125?2:3;
                sendToChild(seq[idx], 3);
                return nil;
            }
            /* other special keys */
            if (kc == 115) { sendToChild("\x1b[H", 3); return nil; } /* Home */
            if (kc == 119) { sendToChild("\x1b[F", 3); return nil; } /* End */
            if (kc == 117) { sendToChild("\x1b[3~", 4); return nil; } /* Delete */
            if (kc == 116) { /* PgUp */
                if (shift) { scroll_vel += SCROLL_IMPULSE*5; scroll_locked=0; scroll_snapping=0; }
                else sendToChild("\x1b[5~", 4);
                return nil;
            }
            if (kc == 121) { /* PgDn */
                if (shift) { scroll_vel -= SCROLL_IMPULSE*5; scroll_snapping=0; }
                else sendToChild("\x1b[6~", 4);
                return nil;
            }

            /* escape */
            if (raw.length == 1 && [raw characterAtIndex:0] == 27) { sel_clear(); sendToChild("\x1b", 1); return nil; }
            /* backspace */
            if (raw.length == 1 && [raw characterAtIndex:0] == 127) {
                if (sel_mode) { sel_play_reverse_morse(); sel_delete_via_keys(g_net_mode==2 ? net_write_child : platform_write_child); sel_clear(); return nil; }
                gen_delete_click();
                sendToChild("\x7f", 1); return nil;
            }
            /* enter — Option+Enter or Shift+Enter sends ESC+CR (newline in Claude Code input) */
            if (raw.length == 1 && [raw characterAtIndex:0] == 13) {
                BOOL opt = (mods & NSEventModifierFlagOption) != 0;
                if (opt || shift) sendToChild("\x1b\r", 2); /* ESC CR = Alt+Enter */
                else sendToChild("\r", 1);
                return nil;
            }
            /* tab */
            if (raw.length == 1 && [raw characterAtIndex:0] == 9) { sendToChild("\t", 1); return nil; }
            /* ctrl+key */
            if (raw.length == 1 && [raw characterAtIndex:0] < 32) {
                char c = [raw characterAtIndex:0];
                if (c == 3 && sel_mode) { sel_copy(); sel_clear(); return nil; }
                sendToChild(&c, 1); return nil;
            }
            /* printable */
            if (raw.length > 0) {
                if (sel_mode && [raw characterAtIndex:0] >= 32) {
                    sel_play_reverse_morse();
                    sel_delete_via_keys(g_net_mode==2 ? net_write_child : platform_write_child); sel_clear();
                }
                const char *utf8 = [raw UTF8String];
                sendToChild(utf8, (int)strlen(utf8));
                gen_type_click();
                return nil;
            }
            return event;
        }];

    /* Mouse: track position + selection */
    [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskLeftMouseDown
        handler:^NSEvent *(NSEvent *event) {
            NSPoint p = [glView convertPoint:[event locationInWindow] fromView:nil];
            p.y = [glView bounds].size.height - p.y; /* flip Y */
            int col = (int)p.x / CELL_W, row = (int)p.y / CELL_H;
            if (col < 0) col = 0; if (col >= term_cols) col = term_cols-1;
            if (row < 0) row = 0; if (row >= term_rows) row = term_rows-1;
            /* double-click word select */
            static int lc=-1, lr=-1; static NSTimeInterval lt=0;
            NSTimeInterval now = [event timestamp];
            if (col==lc && row==lr && (now-lt) < 0.4) {
                int c0=col,c1=col;
                while(c0>0&&grid[row][c0-1].ch>' ')c0--;
                while(c1<term_cols-1&&grid[row][c1+1].ch>' ')c1++;
                if(c1>=c0&&grid[row][col].ch>' '){sel_start_col=c0;sel_end_col=c1;sel_start_row=sel_end_row=row;sel_mode=1;sel_dragging=0;sel_play_morse();}
                lc=-1;
            } else {
                BOOL alt = ([event modifierFlags] & NSEventModifierFlagOption) != 0;
                BOOL ctrl = ([event modifierFlags] & NSEventModifierFlagControl) != 0;
                sel_start_col=sel_end_col=col; sel_start_row=sel_end_row=row;
                sel_mode = (alt||ctrl) ? 2 : 1; sel_dragging=1;
            }
            lc=col; lr=row; lt=now;
            return event;
        }];
    [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskLeftMouseDragged
        handler:^NSEvent *(NSEvent *event) {
            NSPoint p = [glView convertPoint:[event locationInWindow] fromView:nil];
            p.y = [glView bounds].size.height - p.y;
            mouse_col = (int)p.x / CELL_W; mouse_row = (int)p.y / CELL_H;
            if (sel_dragging) {
                sel_end_col = mouse_col<0?0:(mouse_col>=term_cols?term_cols-1:mouse_col);
                sel_end_row = mouse_row<0?0:(mouse_row>=term_rows?term_rows-1:mouse_row);
            }
            return event;
        }];
    [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskLeftMouseUp
        handler:^NSEvent *(NSEvent *event) {
            sel_dragging = 0;
            if (sel_start_col==sel_end_col && sel_start_row==sel_end_row) sel_clear();
            else sel_play_morse(); /* play morse for selection */
            return event;
        }];
    [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskMouseMoved
        handler:^NSEvent *(NSEvent *event) {
            NSPoint p = [glView convertPoint:[event locationInWindow] fromView:nil];
            p.y = [glView bounds].size.height - p.y;
            mouse_col = (int)p.x / CELL_W; mouse_row = (int)p.y / CELL_H;
            return event;
        }];
    /* Right-click → context menu */
    [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskRightMouseDown
        handler:^NSEvent *(NSEvent *event) {
            sel_clear();
            NSMenu *ctx = [[NSMenu alloc] init];
            NSMenuItem *m1 = [ctx addItemWithTitle:@"Copy Screen" action:@selector(copyScreen:) keyEquivalent:@""];
            NSMenuItem *m2 = [ctx addItemWithTitle:@"Copy All History" action:@selector(copyHistory:) keyEquivalent:@""];
            NSMenuItem *m3 = [ctx addItemWithTitle:@"Paste" action:@selector(paste:) keyEquivalent:@""];
            [m1 setTarget:g_menu_handler]; [m2 setTarget:g_menu_handler]; [m3 setTarget:g_menu_handler];
            [NSMenu popUpContextMenu:ctx withEvent:event forView:glView];
            return nil; /* consumed */
        }];

    [window setAcceptsMouseMovedEvents:YES];
    [window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];

    /* render timer — 1ms (same as GLUT idle loop) */
    [NSTimer scheduledTimerWithTimeInterval:0.001 repeats:YES block:^(NSTimer *timer) {
        if (scroll_locked && hist_count > 0) scroll_y = 0;

        /* read child / network */
        if (g_net_mode == 2) {
            if (g_net_sock != SOCK_INVALID) net_read_into_vt(g_net_sock);
        } else {
            /* local or visible server */
            if (pty_master >= 0) {
                unsigned char buf[4096]; int n;
                while ((n = read(pty_master, buf, sizeof(buf))) > 0) {
                    vt_feed(buf, n);
                    if (g_net_mode == 1 && g_server_visible) net_broadcast((char*)buf, n);
                }
                if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                    int status; if (waitpid(child_pid, &status, WNOHANG) > 0) child_alive = 0;
                }
            }
            if (g_net_mode == 1 && g_server_visible) {
                net_accept_clients();
                net_read_all_clients(platform_write_child, mac_server_on_resize);
            }
        }

        /* handle resize — ortho always matches window points, never stretches */
        [[glView openGLContext] update];
        NSSize pts = [glView bounds].size;
        NSSize px = [glView convertSizeToBacking:pts];
        glViewport(0, 0, (int)px.width, (int)px.height);
        glMatrixMode(GL_PROJECTION); glLoadIdentity();
        glOrtho(0, pts.width, pts.height, 0, -1, 1); /* 1:1 with window points */
        glMatrixMode(GL_MODELVIEW);
        /* update cell count when crossing FONT_W/FONT_H boundaries */
        int new_cols = (int)pts.width / CELL_W, new_rows = (int)pts.height / CELL_H;
        if (new_cols != term_cols || new_rows != term_rows)
            apply_resize(new_cols, new_rows);

        /* trigger output sound if chars were drawn this frame */
        if (output_buf_count > 0) { gen_output_tones(); output_buf_count = 0; }

        [[glView openGLContext] makeCurrentContext];
        gl_render();
        [glView setNeedsDisplay:YES];
    }];

    [NSApp run];
    return 0;
}

#endif /* _WIN32 */
