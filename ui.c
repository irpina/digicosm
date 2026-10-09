/* SPDX-License-Identifier: GPL-2.0-or-later */
/* DigiCosm's UI: its Mod Menu entry, its screen, its keys and knobs, its project data. While it is open it
 * owns the output (dcosm_owner.on) and the screen; NO gives both back.
 *
 *   three pages, as the Digitone's own (LEFT, RIGHT or PAGE moves between them):
 *     1 EFFECT  ACT REP SHP FLT / MIX TIME SPC LOOP        pushing a knob: its default
 *     2 SHIFT   GAIN MDEP MRAT RESO / FXVL LSPD VERB FADE  (also FUNC + a knob on page 1)
 *     3 LOOPER  HOLD REV BYP - / LOOP STOP UNDO CLR        turn A-C for off/on, push to toggle or act
 *   trig keys 1-11   the engines; 12 HOLD; 13-16 the variation, A-D
 *   T1 the looper: record, play, overdub (held: undo);  T2 stop (held: erase), as the Microcosm's footswitches
 *   YES SETUP, NO back (from a page: close);  PLAY, STOP, RECORD, TEMPO and LEVEL/DATA stay stock
 *   presets: in SETUP a trig key saves the page as that preset (1-16); FUNC + a trig key recalls one
 *   MIDI: the Microcosm's CCs, on the auto channel (or any, in SETUP), while the page is open */
#pragma GCC optimize ("no-tree-loop-distribute-patterns")
#include "dcosm.h"

#define KEY_FUNC    1
#define KEY_TEMPO   9
#define KEY_RECORD  10
#define KEY_PLAY    11
#define KEY_STOP    12
#define KEY_YES     13
#define KEY_NO      14
#define KEY_UP      15
#define KEY_DOWN    16
#define KEY_LEFT    17
#define KEY_RIGHT   18
#define KEY_PAGE    19
#define KEY_STEP1   26
#define KEY_T1      42
#define KEY_PUSH_A  46
#define EV_KEY(ev)   (*(u32 *)((char *)(ev) + 12))
#define EV_FLAGS(ev) (*(u32 *)((char *)(ev) + 16))
#define EV_DELTA(ev) (*(s32 *)((char *)(ev) + 16))
#define COUNTS      4                       /* encoder counts a detent */

typedef void (*rect_fn)(void *bmp, int x0, int y0, int x1, int y1, int colour);
typedef void (*text_fn)(void *bmp, const void *font, int x, int y, int maxlen, const char *fmt, ...);
#define FILLRECT  ((rect_fn)fw_fillrect)
#define FRAMERECT ((rect_fn)fw_framerect)
#define TEXTF     ((text_fn)fw_textf)
#define FONT5     ((const void *)fw_font5)          /* 3 x 5: the stock value boxes' */
#define FONT_LBL  ((const void *)fw_font_label)     /* the stock labels' */
#define FONT_TTL  ((const void *)fw_font_title)     /* the stock title bar's */

static const u8 knob_def[8] = { 64, 64, 0, 127, 64, 48, 32, 100 };
static const u8 shift_def[8] = { 64, 0, 40, 0, 100, 64, 0, 30 };
static const char *const knob_lbl[8] = { "ACT", "REP", "SHP", "FLT", "MIX", "TIME", "SPC", "LOOP" };
static const char *const shift_lbl[8] = { "GAIN", "MDEP", "MRAT", "RESO", "FXVL", "LSPD", "VERB", "FADE" };
static const char *const shapes[4] = { "FLAT", "SWEL", "PERC", "ARCH" };
static const char *const times[6] = { "BAR", "1/2", "1/4", "1/8", "1/16", "1/32" };
static const char *const speeds[5] = { "1/4X", "1/2X", "1X", "2X", "4X" };
static const char *const rooms[4] = { "ROOM", "DARK", "HALL", "AMBI" };
static const char *const eng_title[E_COUNT] = { "Mosaic", "Seq", "Glide", "Haze", "Tunnel", "Strum",
                                                "Blocks", "Interrupt", "Arp", "Pattern", "Warp" };
static const char *const cfg_lbl[G_COUNT] = { "INPUT", "LOOP ROUTE", "LOOPER ONLY", "QUANTIZE", "BURST",
                                              "HOLD", "LOOP ORDER", "MIDI CC" };
static const char *const cfg_val[G_COUNT][2] = { { "STEREO", "MONO" }, { "POST-FX", "PRE-FX" }, { "OFF", "ON" },
                                                 { "OFF", "ON" }, { "OFF", "ON" }, { "LATCH", "MOMENT" },
                                                 { "REC-PLAY", "REC-DUB" }, { "AUTO CH", "ANY CH" } };

static u8 ui_open, ui_setup, ui_pg, ui_func, ui_ready, ui_inited, ui_row;
static u8 ui_flash_ticks, ui_flash_pg, ui_flash_i;   /* a knob's value in the title bar after a turn */
#define NPAGE 3
static u8 ui_msg_ticks, ui_msg_preset, ui_msg_kind;   /* a message for a second: 1 saved, 2 recalled, 3 empty */
static u32 clear_at;                        /* the next address to clear, 0 when nothing is */
static u32 bclear_at;                       /* the overdub layer's clearing, in frames */
static s32 enc_acc[8];

/* ---- project data: the page and sixteen presets, saved with the project ------------------------------
 * 132 bytes: the gap mods share in a project holds 480, and digitables' tables take 328 of it with its
 * header. A preset is the engine, the variation and the eight knobs, 6 bits each (FUNC's knobs and
 * SETUP stay the project's). */
#define NPRESET 16
struct dc_save {
    u8 ver, engine, var, cfg;           /* cfg: a bit a SETUP row (eight rows) */
    u8 knob[8], shift[8];
    u8 preset[NPRESET][7];              /* [0]: engine | var << 4 | 0x80 once saved; [1-6]: the knobs */
} __attribute__((aligned(4)));
static struct dc_save save;
#define SAVE_VER 2

static void defaults(void)
{
    int i;
    for (i = 0; i < 8; i++) {
        dcosm.knob[i] = knob_def[i];
        dcosm.shift[i] = shift_def[i];
    }
    for (i = 0; i < G_COUNT; i++)
        dcosm.cfg[i] = 0;
    dcosm.engine = E_MOSAIC;
    dcosm.var = 0;
}

static void dcosm_loaded(int found)
{
    int i, j;
    if (!found || save.ver != SAVE_VER || save.engine >= E_COUNT || save.var > 3) {
        defaults();
        for (i = 0; i < NPRESET; i++)
            for (j = 0; j < 7; j++)
                save.preset[i][j] = 0;
    } else {
        for (i = 0; i < 8; i++) {
            dcosm.knob[i] = save.knob[i] & 127;
            dcosm.shift[i] = save.shift[i] & 127;
        }
        for (i = 0; i < G_COUNT; i++)
            dcosm.cfg[i] = (save.cfg >> i) & 1;
        dcosm.engine = save.engine;
        dcosm.var = save.var;
    }
    ui_inited = 1;
}

const struct core_projdata dcosm_projdata = { 0x4443534Du /* "DCSM" */, sizeof(struct dc_save), &save,
                                              dcosm_loaded };

static void keep_save(void)                 /* the page into the project's copy (the presets are there) */
{
    int i;
    u8 cfg = 0;
    save.ver = SAVE_VER;
    save.engine = dcosm.engine;
    save.var = dcosm.var;
    for (i = 0; i < 8; i++) {
        save.knob[i] = dcosm.knob[i];
        save.shift[i] = dcosm.shift[i];
    }
    for (i = 0; i < G_COUNT; i++)
        cfg |= (u8)((dcosm.cfg[i] & 1) << i);
    save.cfg = cfg;
}

static void preset_save(int n)
{
    u8 *p = save.preset[n];
    u32 hi = 0, lo = 0;
    int i;
    for (i = 0; i < 4; i++)
        hi = (hi << 6) | (u32)(dcosm.knob[i] >> 1);
    for (i = 4; i < 8; i++)
        lo = (lo << 6) | (u32)(dcosm.knob[i] >> 1);
    p[0] = (u8)(dcosm.engine | dcosm.var << 4 | 0x80);
    p[1] = (u8)(hi >> 16), p[2] = (u8)(hi >> 8), p[3] = (u8)hi;
    p[4] = (u8)(lo >> 16), p[5] = (u8)(lo >> 8), p[6] = (u8)lo;
}

static int preset_recall(int n)             /* -> 0 when the slot is empty */
{
    const u8 *p = save.preset[n];
    u32 hi, lo, v;
    int i;
    if (!(p[0] & 0x80) || (p[0] & 15) >= E_COUNT)
        return 0;
    hi = (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
    lo = (u32)p[4] << 16 | (u32)p[5] << 8 | p[6];
    for (i = 3; i >= 0; i--, hi >>= 6) {
        v = hi & 63;
        dcosm.knob[i] = (u8)(v == 63 ? 127 : v << 1);    /* 64 stays 64, 127 stays 127 */
    }
    for (i = 7; i >= 4; i--, lo >>= 6) {
        v = lo & 63;
        dcosm.knob[i] = (u8)(v == 63 ? 127 : v << 1);    /* 64 stays 64, 127 stays 127 */
    }
    dcosm.engine = (u8)(p[0] & 15);
    dcosm.var = (u8)((p[0] >> 4) & 3);
    return 1;
}

static void message(int kind, int n)
{
    ui_msg_kind = (u8)kind;
    ui_msg_preset = (u8)(n + 1);
    ui_msg_ticks = 30;
}

/* ---- opening and closing -------------------------------------------------------------------------- */
static void dcosm_open(void *brain, void *ev, int track)
{
    (void)brain, (void)ev, (void)track;
    ui_open = 1;
    ui_setup = 0;
    ui_pg = 0;
    ui_func = 0;
    if (!ui_ready && !clear_at)
        clear_at = DC_BULK;                 /* the first time: clear what the render reads */
}

static void dcosm_close(void)
{
    ui_open = 0;
    dcosm_owner.on = 0;                     /* the stock audio, from the next block */
    dcosm.hold = 0;
}

/* The Mod Menu's DIGICOSM, with its icon: an orb. */
static const u16 dcosm_icon[16] = {
    0x07e0, 0x1818, 0x2004, 0x43c2, 0x4c32, 0x9009, 0x93c9, 0xa5a5,
    0xa5a5, 0x93c9, 0x9009, 0x4c32, 0x43c2, 0x2004, 0x1818, 0x07e0
};
const struct core_menu_item dcosm_menu = { "DIGICOSM", dcosm_open, CORE_MENU_ICON, dcosm_icon };

/* ---- keys ------------------------------------------------------------------------------------------- */
static void set_knob(int i, int func, s32 v)
{
    if (v < 0) v = 0;
    if (v > 127) v = 127;
    if (func)
        dcosm.shift[i] = (u8)v;
    else
        dcosm.knob[i] = (u8)v;
}

static void flash(int pg, int i)              /* the knob's value in the title bar, a second */
{
    ui_flash_pg = (u8)pg;
    ui_flash_i = (u8)i;
    ui_flash_ticks = 30;
}

static void hold_key(void)
{
    dcosm.hold = dcosm.cfg[G_HOLDMOM] ? 1 : !dcosm.hold;
}

static void loop_key(void)                   /* T1, and LOOP on page 3: record, play, overdub */
{
    dcosm.lcmd = dcosm.cfg[G_BURST] ? C_BURST_DOWN : C_T1;
}

static int page_now(void)                    /* FUNC held shows page 2 */
{
    return ui_func ? 1 : ui_pg;
}

static void push(int i)                      /* an encoder pushed */
{
    int pg = page_now();
    if (pg < 2) {
        set_knob(i, pg, pg ? shift_def[i] : knob_def[i]);
        flash(pg, i);
    } else if (i == 0) {
        hold_key();
    } else if (i == 1) {
        dcosm.reverse = !dcosm.reverse;
    } else if (i == 2) {
        dcosm.bypass = !dcosm.bypass;
    } else if (i == 4) {
        loop_key();
    } else if (i == 5) {
        dcosm.lcmd = C_STOP;
    } else if (i == 6 && !dcosm.cfg[G_BURST]) {
        dcosm.lcmd = C_UNDO;
    } else if (i == 7) {
        dcosm.lcmd = C_ERASE;
    }
}

int dcosm_key(void *brain, void *ev)
{
    u32 key = EV_KEY(ev), fl = EV_FLAGS(ev);
    int press = (fl & 1) && !(fl & 8), released = (fl & 0x10) != 0, looper = !ui_setup && page_now() == 2;
    (void)brain;
    if (!ui_open)
        return 0;
    if (key == KEY_FUNC) {                  /* down: flags 1 (5 for a double press); up: 0x10, or 0 after
                                               another key went down meanwhile */
        ui_func = (u8)(fl & 1);
        return 1;
    }
    if (fl & 2)                             /* another key's event: FUNC is held */
        ui_func = 1;
    if (key == KEY_PLAY || key == KEY_STOP || key == KEY_TEMPO || key == KEY_RECORD)
        return 0;                           /* the sequencer and its tempo stay the Digitone's */
    if (released) {
        if ((key == KEY_STEP1 + 11 || (looper && key == KEY_PUSH_A)) && dcosm.cfg[G_HOLDMOM])
            dcosm.hold = 0;
        if ((key == KEY_T1 || (looper && key == KEY_PUSH_A + 4)) && dcosm.cfg[G_BURST])
            dcosm.lcmd = C_BURST_UP;
        return 1;
    }
    if (!press)
        return 1;                           /* repeats; ev_hold sees the track keys held */
    if (key >= KEY_STEP1 && key < KEY_STEP1 + NPRESET && (ui_setup || ui_func)) {
        int n = (int)(key - KEY_STEP1);
        if (ui_setup) {                     /* SETUP: save the page as preset n */
            preset_save(n);
            message(1, n);
        } else {                            /* FUNC: recall it */
            message(preset_recall(n) ? 2 : 3, n);
        }
    } else if (key >= KEY_STEP1 && key < KEY_STEP1 + E_COUNT) {
        dcosm.engine = (u8)(key - KEY_STEP1);
    } else if (key == KEY_STEP1 + 11) {
        hold_key();
    } else if (key >= KEY_STEP1 + 12 && key <= KEY_STEP1 + 15) {
        dcosm.var = (u8)(key - KEY_STEP1 - 12);
    } else if (key == KEY_T1) {
        loop_key();
    } else if (key == KEY_T1 + 1) {
        dcosm.lcmd = C_STOP;
    } else if (key == KEY_YES) {
        ui_setup = !ui_setup;
    } else if (key == KEY_NO) {
        if (ui_setup)
            ui_setup = 0;
        else
            dcosm_close();
    } else if (ui_setup && (key == KEY_UP || key == KEY_DOWN)) {
        ui_row = (u8)((ui_row + (key == KEY_UP ? G_COUNT - 1 : 1)) % G_COUNT);
    } else if (ui_setup && (key == KEY_LEFT || key == KEY_RIGHT)) {
        dcosm.cfg[ui_row] = !dcosm.cfg[ui_row];
        if (ui_row == G_HOLDMOM)
            dcosm.hold = 0;
    } else if (key == KEY_LEFT || key == KEY_RIGHT || key == KEY_PAGE) {
        ui_pg = (u8)((ui_pg + (key == KEY_LEFT ? NPAGE - 1 : 1)) % NPAGE);
        ui_flash_ticks = 0;
    } else if (key >= KEY_PUSH_A && key < KEY_PUSH_A + 8 && !ui_setup) {
        push((int)(key - KEY_PUSH_A));
    }
    return 1;
}

/* core-dn1's ev_hold, a track key held on its own: while open, T1 undoes the overdub, T2 erases. */
int dcosm_hold(void *brain, void *ev, int track)
{
    (void)brain, (void)ev;
    if (!ui_open)
        return 0;
    if (track == 0 && !dcosm.cfg[G_BURST])
        dcosm.lcmd = C_UNDO;
    else if (track == 1)
        dcosm.lcmd = C_ERASE;
    return 1;
}

int dcosm_enc(void *brain, void *ev)
{
    u32 e = EV_KEY(ev);
    s32 d;
    int i, pg;
    (void)brain;
    if (!ui_open || e < 1 || e > 8)
        return 0;                           /* LEVEL/DATA stays the Digitone's */
    if (ui_setup)
        return 1;
    i = (int)e - 1;
    enc_acc[i] += EV_DELTA(ev);
    d = enc_acc[i] / COUNTS;
    enc_acc[i] -= d * COUNTS;
    if (!d)
        return 1;
    pg = page_now();
    if (pg < 2) {
        set_knob(i, pg, (pg ? dcosm.shift[i] : dcosm.knob[i]) + d);
        flash(pg, i);
    } else if (i == 0 && !dcosm.cfg[G_HOLDMOM]) {  /* page 3: right on, left off */
        dcosm.hold = (u8)(d > 0);
    } else if (i == 1) {
        dcosm.reverse = (u8)(d > 0);
    } else if (i == 2) {
        dcosm.bypass = (u8)(d > 0);
    }
    return 1;
}

/* ---- MIDI: the Microcosm's CC map (core-dn1 3.2's ev_midi_cc, the MIDI task) -------------------------------
 * While the page is open, the CCs below are DigiCosm's when they come on the auto channel (SETUP's MIDI CC:
 * ANY CH, on a track's channel too): the Digitone does not apply them. Every other CC, and every CC while
 * the page is closed, goes on to the Digitone. A switch is on from 64; a looper CC acts from 64. CC 5 and
 * CC 18 take the Microcosm's steps, 0-5: 1/4, 1/2, TAP, 2x, 4x, 8x (and anything above as 8x). Its six
 * subdivisions are DigiCosm's six TIME steps, a bar down to 1/32; its loop speeds are DigiCosm's five, 8x
 * as 4x. */
static const u8 cc_knob[8] = { K_ACT, K_SHP, K_FLT, K_MIX, K_TIME, K_REP, K_SPC, K_LOOP };          /* CC 6-13 */
static const u8 cc_shift[8] = { S_MRATE, S_RESO, S_FXVOL, S_LSPEED, S_LSPEED, S_MDEP, S_VERB, S_FADE }; /* 14-21 */
static const u8 cc_cfg[4] = { G_PRE, G_ONLY, G_BURST, G_QUANT };                                    /* 24-27 */
static const u8 cc_time[6] = { 10, 32, 53, 74, 96, 117 };       /* CC 5: the middle of each TIME step */
static const u8 cc_lspeed[6] = { 12, 38, 64, 89, 115, 115 };    /* CC 18: of each LSPD step */

int dcosm_cc(int track, int cc, int value, int flags)
{
    int on = value >= 64, s = dcosm.lstate;
    (void)track;
    if (!ui_open || !ui_ready || (!(flags & CORE_MIDI_CC_AUTO) && !dcosm.cfg[G_MIDI]))
        return 0;
    if (value < 0 || value > 127)
        return 0;
    if (cc == 5) {                              /* Subdivision, stepped */
        dcosm.knob[K_TIME] = cc_time[value < 5 ? value : 5];
    } else if (cc == 18) {                      /* Loop Speed, stepped */
        dcosm.shift[S_LSPEED] = cc_lspeed[value < 5 ? value : 5];
    } else if (cc >= 6 && cc <= 13) {
        dcosm.knob[cc_knob[cc - 6]] = (u8)value;
    } else if (cc >= 14 && cc <= 21) {          /* 17 is Loop Speed, 0-127 */
        dcosm.shift[cc_shift[cc - 14]] = (u8)value;
    } else if (cc == 23 || cc == 47) {          /* Reverse */
        dcosm.reverse = (u8)on;
    } else if (cc >= 24 && cc <= 27) {          /* the looper's settings */
        dcosm.cfg[cc_cfg[cc - 24]] = (u8)on;
    } else if (cc >= 28 && cc <= 35 && cc != 32 && cc != 33) {
        if (!on)
            return 1;
        if (cc == 28)                           /* Record: start; close; or a new take */
            dcosm.lcmd = s == L_EMPTY || s == L_REC ? C_T1 : C_BURST_DOWN;
        else if (cc == 29)                      /* Play */
            dcosm.lcmd = s == L_REC ? C_BURST_UP : s == L_STOP || s == L_DUB ? C_T1 : C_NONE;
        else if (cc == 30)                      /* Overdub, on and off */
            dcosm.lcmd = s == L_PLAY || s == L_DUB ? C_T1 : C_NONE;
        else if (cc == 31)
            dcosm.lcmd = C_STOP;
        else if (cc == 34)
            dcosm.lcmd = C_ERASE;
        else
            dcosm.lcmd = C_UNDO;
    } else if (cc == 48) {                      /* Hold */
        dcosm.hold = (u8)on;
    } else if (cc == 102) {                     /* Bypass: below 64 bypassed */
        dcosm.bypass = (u8)!on;
    } else if (cc == 22 || cc == 45 || cc == 46 || cc == 93) {
        /* the Microcosm's looper on/off, copy preset, save preset and tap tempo: nothing to do here (the
         * looper is always on, presets are saved from SETUP, the tempo is the Digitone's), but taken, so
         * the Digitone does not apply them to the active track meanwhile */
    } else {
        return 0;
    }
    return 1;
}

/* ---- the UI task: clearing, the owner, redraws ---------------------------------------------------------- */
void dcosm_tick(void *ctrl)
{
    if (!ui_inited) {                       /* no project load reached us */
        defaults();
        ui_inited = 1;
    }
    if (clear_at) {                         /* 256 KB a tick */
        u32 *p = (u32 *)clear_at, *end = p + 65536;
        if ((u32)end > DC_CLEAR_END)
            end = (u32 *)DC_CLEAR_END;
        while (p < end)
            *p++ = 0;
        clear_at = (u32)end >= DC_CLEAR_END ? 0 : (u32)end;
        if (!clear_at) {
            dcosm_dsp_reset();
            dcosm.bvalid = 1;
            ui_ready = 1;
        }
    }
    if (dcosm.lclear_len) {                 /* undo: the overdub layer, 512 KB a tick */
        u32 *p = (u32 *)(DC_LOOPB) + bclear_at, *end = (u32 *)(DC_LOOPB) + dcosm.lclear_len;
        if (end > p + 131072)
            end = p + 131072;
        while (p < end)
            *p++ = 0;
        bclear_at = (u32)(end - (u32 *)DC_LOOPB);
        if (bclear_at >= dcosm.lclear_len) {
            bclear_at = 0;
            dcosm.lclear_len = 0;
            dcosm.bvalid = 1;
        }
    }
    dcosm_owner.on = ui_open && ui_ready;
    dcosm.meter[0] = (u16)(dcosm.meter[0] - (dcosm.meter[0] >> 3));
    dcosm.meter[1] = (u16)(dcosm.meter[1] - (dcosm.meter[1] >> 3));
    keep_save();
    if (ui_msg_ticks)
        ui_msg_ticks--;
    if (ui_flash_ticks)
        ui_flash_ticks--;
    if (ui_open)
        *((unsigned char *)ctrl + 0x20) = 1;
}

/* ---- the screen: 128 x 64, y = 0 the bottom row ------------------------------------------------------ */
static void value_text(void *bmp, const void *font, int x, int y, int i, int func)
{
    int v = func ? dcosm.shift[i] : dcosm.knob[i];
    if (!func) {
        if (i == K_SHP)
            TEXTF(bmp, font, x, y, -1, "%s", shapes[v >> 5]);
        else if (i == K_FLT && v == 127)
            TEXTF(bmp, font, x, y, -1, "OPEN");
        else if (i == K_TIME)
            TEXTF(bmp, font, x, y, -1, "%s", times[(v * 6) >> 7]);
        else
            TEXTF(bmp, font, x, y, -1, "%d", v);
        return;
    }
    if (i == S_LSPEED) {
        TEXTF(bmp, font, x, y, -1, "%s", speeds[(v * 5) >> 7]);
    } else if (i == S_VERB) {
        TEXTF(bmp, font, x, y, -1, "%s", rooms[(v * 4) >> 7]);
    } else if (i == S_GAIN) {
        int t = (v - 64) * 3;                   /* tenths of a dB */
        if (!v)
            TEXTF(bmp, font, x, y, -1, "MUTE");
        else
            TEXTF(bmp, font, x, y, -1, "%c%d.%d", t < 0 ? '-' : '+', (t < 0 ? -t : t) / 10, (t < 0 ? -t : t) % 10);
    } else if (i == S_FADE) {
        TEXTF(bmp, font, x, y, -1, "%d.%dS", v / 16, (v % 16) * 10 / 16);
    } else {
        TEXTF(bmp, font, x, y, -1, "%d", v);
    }
}

/* As the Digitone's own parameter pages (traced from its FLTR, AMP and LFO pages): an inverted box (here the
 * tempo) and title bar on top, a column on the left (the looper's state, the input's meter, as SYN and LEV)
 * and two rows of four controls, each 17 pixels square over its label: knobs, value boxes and switches. */
static const u8 col_x[4] = { 33, 59, 86, 112 };
#define ROW_Y(r)    ((r) ? 15 : 42)         /* a control's centre, rows 1 and 2 */
#define LBL_Y(r)    ((r) ? 0 : 27)          /* its label's bottom row */
static const signed char ring[12][2] = { {0, 8}, {1, 8}, {2, 8}, {3, 7}, {4, 7}, {5, 6}, {6, 5}, {7, 4}, {7, 3}, {8, 2},
                                {8, 1}, {8, 0} };             /* a knob's circle, one quadrant */
static const signed char needle[29][2] = {             /* its pointer: 270 degrees from 7:30 to 4:30, radius 6 */
    {-4, -4}, {-5, -3}, {-5, -3}, {-6, -2}, {-6, -1}, {-6, 0}, {-6, 1}, {-6, 2}, {-5, 3}, {-4, 4}, {-4, 5},
    {-3, 5}, {-2, 6}, {-1, 6}, {0, 6}, {1, 6}, {2, 6}, {3, 5}, {4, 5}, {4, 4}, {5, 3}, {6, 2}, {6, 1}, {6, 0},
    {6, -1}, {6, -2}, {5, -3}, {5, -3}, {4, -4} };
static const char *const sw_lbl[8] = { "HOLD", "REV", "BYP", "", "LOOP", "STOP", "UNDO", "CLR" };

static void px(void *bmp, int x, int y, int c)
{
    FILLRECT(bmp, x, y, x, y, c);
}

static void corners(void *bmp, int x0, int y0, int x1, int y1, int c)
{
    px(bmp, x0, y0, c);
    px(bmp, x1, y0, c);
    px(bmp, x0, y1, c);
    px(bmp, x1, y1, c);
}

static void rframe(void *bmp, int x0, int y0, int x1, int y1)    /* a frame with the corners off */
{
    FRAMERECT(bmp, x0, y0, x1, y1, 1);
    corners(bmp, x0, y0, x1, y1, 0);
}

static void line(void *bmp, int x0, int y0, int x1, int y1)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, e2;
    for (;;) {
        px(bmp, x0, y0, 1);
        if (x0 == x1 && y0 == y1)
            break;
        e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

static int text_w(const char *s)            /* FONT5's capitals, near enough */
{
    int n = 0;
    while (s[n])
        n++;
    return n ? 5 * n - 1 : 0;
}

static void small_text(void *bmp, int cx, int cy, const char *s)    /* font5, centred on cx, cy */
{
    int n = 0;
    while (s[n])
        n++;
    TEXTF(bmp, FONT5, cx - (4 * n - 1) / 2, cy - 2, -1, "%s", s);
}

static void label(void *bmp, int cx, int y, const char *s)
{
    if (*s)
        TEXTF(bmp, FONT_LBL, cx - text_w(s) / 2, y, -1, "%s", s);
}

static void knob(void *bmp, int cx, int cy, int v, int bipolar)
{
    int i, k = (v * 28 + 63) / 127;
    for (i = 0; i < 12; i++) {
        int dx = ring[i][0], dy = ring[i][1];
        px(bmp, cx + dx, cy + dy, 1);
        px(bmp, cx - dx, cy + dy, 1);
        px(bmp, cx + dx, cy - dy, 1);
        px(bmp, cx - dx, cy - dy, 1);
    }
    line(bmp, cx, cy, cx + needle[k][0], cy + needle[k][1]);
    if (bipolar) {                          /* the stock - and + below */
        FILLRECT(bmp, cx - 10, cy - 7, cx - 8, cy - 7, 1);
        FILLRECT(bmp, cx + 8, cy - 7, cx + 10, cy - 7, 1);
        FILLRECT(bmp, cx + 9, cy - 8, cx + 9, cy - 6, 1);
    }
}

static void box(void *bmp, int cx, int cy, const char *s, int on)    /* a value box; on: inverted */
{
    rframe(bmp, cx - 8, cy - 8, cx + 8, cy + 8);
    if (s)
        small_text(bmp, cx, cy, s);
    if (on)
        FILLRECT(bmp, cx - 7, cy - 7, cx + 7, cy + 7, -1);
}

static void shape_icon(void *bmp, int cx, int cy, int sh)   /* Shape's contour, over a dotted floor */
{
    int x, lo = cy - 4, hi = cy + 3;
    for (x = cx - 6; x <= cx + 6; x += 2)
        px(bmp, x, lo, 1);
    if (sh == 0) {                          /* FLAT */
        line(bmp, cx - 6, lo, cx - 6, hi);
        line(bmp, cx - 6, hi, cx + 6, hi);
        line(bmp, cx + 6, hi, cx + 6, lo);
    } else if (sh == 1) {                   /* SWELL */
        line(bmp, cx - 6, lo, cx + 6, hi);
        line(bmp, cx + 6, hi, cx + 6, lo);
    } else if (sh == 2) {                   /* PERC */
        line(bmp, cx - 6, lo, cx - 6, hi);
        line(bmp, cx - 6, hi, cx + 6, lo);
    } else {                                /* ARCH */
        line(bmp, cx - 6, lo, cx, hi);
        line(bmp, cx, hi, cx + 6, lo);
    }
}

static void loop_icon(void *bmp, int cx, int cy, int s)     /* the looper's state, 7 pixels */
{
    int k;
    if (s == L_REC || s == L_DUB) {         /* a dot; with a play arrow beside it, overdub */
        int x = s == L_DUB ? cx - 3 : cx;
        FILLRECT(bmp, x - 1, cy - 3, x + 1, cy + 3, 1);
        FILLRECT(bmp, x - 2, cy - 2, x + 2, cy + 2, 1);
        FILLRECT(bmp, x - 3, cy - 1, x + 3, cy + 1, 1);
        if (s == L_DUB)
            for (k = -3; k <= 3; k++)
                FILLRECT(bmp, cx + 2, cy + k, cx + 2 + (3 - (k < 0 ? -k : k)) / 1, cy + k, 1);
    } else if (s == L_PLAY) {               /* a play arrow */
        for (k = -3; k <= 3; k++)
            FILLRECT(bmp, cx - 2, cy + k, cx - 2 + 3 - (k < 0 ? -k : k), cy + k, 1);
    } else if (s == L_STOP) {               /* a square */
        FILLRECT(bmp, cx - 3, cy - 3, cx + 3, cy + 3, 1);
    } else {                                /* nothing recorded */
        FILLRECT(bmp, cx - 4, cy, cx - 2, cy, 1);
        FILLRECT(bmp, cx + 2, cy, cx + 4, cy, 1);
    }
}

static void title_bar(void *bmp, int pg)
{
    int t = fw_tempo / 120;
    TEXTF(bmp, FONT_TTL, t >= 100 ? 1 : 3, 56, -1, "%d", t);   /* the box: the tempo, as the stock's A01 */
    FILLRECT(bmp, 0, 54, 14, 63, -1);
    corners(bmp, 0, 54, 14, 63, 0);
    if (ui_msg_ticks) {
        TEXTF(bmp, FONT_TTL, 19, 56, -1, "%s P%d", ui_msg_kind == 1 ? "Saved" : ui_msg_kind == 2 ? "Loaded" : "Empty",
              ui_msg_preset);
    } else if (ui_flash_ticks && !ui_setup) {
        const char *l = ui_flash_pg ? shift_lbl[ui_flash_i] : knob_lbl[ui_flash_i];
        TEXTF(bmp, FONT_TTL, 19, 56, -1, "%s", l);
        value_text(bmp, FONT_TTL, 19 + text_w(l) + 6, 56, ui_flash_i, ui_flash_pg);
    } else if (ui_setup) {
        TEXTF(bmp, FONT_TTL, 19, 56, -1, "Setup (trig: save)");
    } else {
        TEXTF(bmp, FONT_TTL, 19, 56, -1, "%s %c (%d/%d)", eng_title[dcosm.engine], 'A' + dcosm.var, pg + 1, NPAGE);
    }
    TEXTF(bmp, FONT_TTL, 110, 56, -1, "C%d", dcosm.cpu / 10);
    FILLRECT(bmp, 17, 54, 127, 63, -1);
    corners(bmp, 17, 54, 127, 63, 0);
}

static void left_column(void *bmp)
{
    int s = dcosm.lstate, k, l, r, secs;
    rframe(bmp, 0, 27, 14, 50);             /* the looper, as the stock's SYN box */
    loop_icon(bmp, 7, 44, s);
    if (s != L_EMPTY) {
        secs = (int)((s == L_REC ? dcosm.lrec : dcosm.llen) / 48000);
        TEXTF(bmp, FONT5, secs >= 10 ? 3 : 5, 35, -1, "%d", secs);
    }
    if (dcosm.hold)
        small_text(bmp, 3, 31, "H");
    if (dcosm.reverse)
        small_text(bmp, 7, 31, "R");
    if (dcosm.bypass)
        small_text(bmp, 11, 31, "B");
    for (k = 0; k < 5; k++) {               /* the input's meter, as LEV: L and R */
        FILLRECT(bmp, 0, 7 + 4 * k, 1, 7 + 4 * k, 1);
        FILLRECT(bmp, 13, 7 + 4 * k, 14, 7 + 4 * k, 1);
    }
    rframe(bmp, 4, 7, 10, 23);
    l = (dcosm.meter[0] * 13) / 32767;
    r = (dcosm.meter[1] * 13) / 32767;
    if (l)
        FILLRECT(bmp, 6, 9, 6, 8 + l, 1);
    if (r)
        FILLRECT(bmp, 8, 9, 8, 8 + r, 1);
    label(bmp, 7, -1, "IN");
}

static void control(void *bmp, int pg, int i)
{
    int cx = col_x[i & 3], r = i >> 2, cy = ROW_Y(r), v;
    const char *lbl;
    if (pg == 0) {
        v = dcosm.knob[i];
        lbl = knob_lbl[i];
        if (i == K_SHP) {
            box(bmp, cx, cy, 0, 0);
            shape_icon(bmp, cx, cy, v >> 5);
        } else if (i == K_TIME) {
            box(bmp, cx, cy, times[(v * 6) >> 7], 0);
        } else {
            knob(bmp, cx, cy, v, 0);
        }
    } else if (pg == 1) {
        v = dcosm.shift[i];
        lbl = shift_lbl[i];
        if (i == S_LSPEED)
            box(bmp, cx, cy, speeds[(v * 5) >> 7], 0);
        else if (i == S_VERB)
            box(bmp, cx, cy, rooms[(v * 4) >> 7], 0);
        else
            knob(bmp, cx, cy, v, i == S_GAIN);
    } else {
        lbl = sw_lbl[i];
        if (i < 3) {
            v = i == 0 ? dcosm.hold : i == 1 ? dcosm.reverse : dcosm.bypass;
            box(bmp, cx, cy, v ? "ON" : "OFF", v);
        } else if (i == 4) {
            box(bmp, cx, cy, 0, 0);
            loop_icon(bmp, cx, cy, dcosm.lstate);
        } else if (i == 5) {
            box(bmp, cx, cy, 0, 0);
            FILLRECT(bmp, cx - 3, cy - 3, cx + 3, cy + 3, 1);
        } else if (i == 6) {                /* an arrow back */
            box(bmp, cx, cy, 0, 0);
            line(bmp, cx - 3, cy, cx + 3, cy);
            line(bmp, cx - 3, cy, cx - 1, cy + 2);
            line(bmp, cx - 3, cy, cx - 1, cy - 2);
            line(bmp, cx + 3, cy, cx + 3, cy - 3);
        } else if (i == 7) {                /* a cross */
            box(bmp, cx, cy, 0, 0);
            line(bmp, cx - 3, cy - 3, cx + 3, cy + 3);
            line(bmp, cx - 3, cy + 3, cx + 3, cy - 3);
        }
    }
    label(bmp, cx, LBL_Y(r), lbl);
}

static void draw_setup(void *bmp)
{
    int i;
    for (i = 0; i < G_COUNT; i++) {
        int y = 46 - 6 * i;
        TEXTF(bmp, FONT5, 3, y, -1, "%s", cfg_lbl[i]);
        TEXTF(bmp, FONT5, 76, y, -1, "%s", cfg_val[i][dcosm.cfg[i] & 1]);
        if (i == ui_row)
            FILLRECT(bmp, 1, y - 1, 126, y + 4, -1);
    }
}

void dcosm_draw(void *bmp, void *ctrl)
{
    int i, pg = page_now();
    (void)ctrl;
    if (!ui_open)
        return;
    FILLRECT(bmp, 0, 0, 127, 63, 0);
    if (!ui_ready) {
        TEXTF(bmp, FONT5, 30, 30, -1, "DIGICOSM...");
        return;
    }
    title_bar(bmp, pg);
    if (ui_setup) {
        draw_setup(bmp);
        return;
    }
    left_column(bmp);
    for (i = 0; i < 8; i++)
        control(bmp, pg, i);
}
