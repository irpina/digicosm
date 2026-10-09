/* SPDX-License-Identifier: GPL-2.0-or-later */
/* DigiCosm's render: core-dn1 3.2 calls dcosm_render at interrupt level for every block it owns the
 * output (32 frames, 667 us). The chain:
 *   input (gain, mono) -> the history ring -> the engine's heads and taps -> the looper -> pitch
 *   modulation -> the filter -> Space (reverb) -> Mix with the dry input -> the output.
 * Everything is fixed point: samples are 16-bit inside (Q15 in an int32), the dry path keeps 20 bits.
 * Every engine is a scheduler at block rate that starts "heads": interpolated, windowed readers of the
 * history, as one-shot grains or as loops; PATTERN reads taps off a delay line of its own. */
#pragma GCC optimize ("no-tree-loop-distribute-patterns")
#include "dcosm.h"
#include "tables.h"

#define N       32
#define MAXH    16
#define DTCN0   (*(volatile u32 *)0xFC07000Cu)  /* DMA timer 0's counter: the bus clock */
#define ONS     8

struct dc_state dcosm;
struct core_audio_owner dcosm_owner = { 0, dcosm_render, CORE_AUDIO_MUTE_VOICES };
const char *const dcosm_engine_names[E_COUNT] = { "MOSAIC", "SEQ", "GLIDE", "HAZE", "TUNNEL", "STRUM",
                                                  "BLOCKS", "INTERRUPT", "ARP", "PATTERN", "WARP" };

static inline s32 sat16(s32 v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : v; }
static inline s32 clamp17(s32 v) { return v > 65535 ? 65535 : v < -65535 ? -65535 : v; }

static u32 seed = 0x2545f491u;
static inline u32 rnd(void) { seed = seed * 1664525u + 1013904223u; return seed; }
static inline u32 rnd_n(u32 n) { return ((rnd() >> 16) * n) >> 16; }      /* 0 .. n-1, n < 65536 */

/* 1/sqrt(n), Q15: a sum of n heads at about the level of one */
static const s16 gnorm[17] = { 32767, 32767, 23170, 18919, 16384, 14654, 13377, 12385, 11585, 10923,
                               10362, 9880, 9459, 9088, 8757, 8460, 8192 };

/* ---- heads -------------------------------------------------------------------------------------- */
struct head {
    u32 pos;                    /* one-shot: where it reads, 20.12 frames into the history */
    u32 ls, rel, len;           /* loop: its start, where it is in it, its length; 20.12 */
    s32 inc;                    /* 20.12 frames a frame (4096 is 1x); < 0 backwards */
    u32 ph, dph;                /* the window's phase, and its step a frame */
    s32 gl, gr;                 /* Q15, with the pan */
    const s16 *win;
    s32 f, q;                   /* a filter of its own (Q14 SVF), f 0 for none */
    s32 fl0, fb0, fl1, fb1;
    u8 on, loop, dying, bp, crush, tag;
    s16 cyc;                    /* loop: cycles left, < 0 forever */
};
static struct head hd[MAXH];
static int hcap = MAXH;                         /* heads new grains may take: the CPU guard's */

static struct head *h_get(void)
{
    int k;
    for (k = 0; k < hcap; k++)
        if (!hd[k].on)
            return &hd[k];
    return 0;
}

static void h_gain(struct head *h, s32 g, s32 pan)    /* pan 0 (left) .. 32767 (right) */
{
    h->gl = (g * (32767 - pan)) >> 14;
    h->gr = (g * pan) >> 14;
    if (h->gl > 49151) h->gl = 49151;
    if (h->gr > 49151) h->gr = 49151;
}

static void h_init(struct head *h, s32 g, s32 pan, int win, int tag)
{
    h->win = dc_win + 256 * win;
    h->f = 0;
    h->crush = 0;
    h->dying = 0;
    h->bp = 0;
    h->fl0 = h->fb0 = h->fl1 = h->fb1 = 0;
    h->ph = 0;
    h->tag = (u8)tag;
    h_gain(h, g, pan);
}

/* A grain: life frames of the history from src on (at rate; backwards from its far end when reversed). */
static void h_shot(struct head *h, u32 src, s32 rate, u32 life, s32 g, s32 pan, int win, int tag)
{
    s32 inc = dcosm.reverse ? -rate : rate;
    u32 span = (life * (u32)(rate >> 4)) >> 8;
    if (life < 64)
        life = 64;
    h_init(h, g, pan, win, tag);
    h->pos = (inc > 0 ? src : src + span) << 12;
    h->inc = inc;
    h->dph = 0xffffffffu / life;
    h->loop = 0;
    h->on = 1;
}

static u32 loop_dph(s32 rate, u32 len)          /* the window's step: a cycle a pass of the loop */
{
    return (((u32)rate << 16) / len) << 4;
}

/* A loop: len frames of the history from start on, played over and over (cyc times, < 0 for ever). */
static void h_loop(struct head *h, u32 start, u32 len, s32 rate, s32 g, s32 pan, int win, int cyc, int tag)
{
    s32 inc = dcosm.reverse ? -rate : rate;
    if (len < 64)
        len = 64;
    if (len > DC_HIST_FR / 2)
        len = DC_HIST_FR / 2;
    h_init(h, g, pan, win, tag);
    h->ls = start << 12;
    h->len = len << 12;
    h->rel = inc > 0 ? 0 : h->len - 1;
    h->inc = inc;
    h->dph = loop_dph(rate, len);
    h->cyc = (s16)cyc;
    h->loop = 1;
    h->on = 1;
}

static void h_kill_tag(int tag)                 /* loops of a tag end at their next wrap */
{
    int k;
    for (k = 0; k < MAXH; k++)
        if (hd[k].on && hd[k].tag == tag) {
            if (hd[k].loop)
                hd[k].dying = 1;
        }
}

/* Every head into the wet bus w (32 frames L,R, Q15 in int32). */
static void heads_render(s32 *w)
{
    const s16 *H = DC_HIST;
    int k, i, n = 0;
    for (k = 0; k < MAXH; k++) {
        struct head *h = &hd[k];
        s32 gl, gr, inc, *o = w;
        u32 ph, dph;
        const s16 *win;
        if (!h->on)
            continue;
        n++;
        gl = h->gl, gr = h->gr, inc = h->inc, ph = h->ph, dph = h->dph, win = h->win;
        if (h->f || h->crush) {                 /* the slow path: a filter or a crusher of its own */
            s32 f = h->f, q = h->q, l0 = h->fl0, b0 = h->fb0, l1 = h->fl1, b1 = h->fb1;
            int cr = h->crush, bp = h->bp;
            u32 pos = h->pos, ls = h->ls, rel = h->rel, len = h->len;
            for (i = 0; i < N; i++) {
                u32 p = h->loop ? ls + rel : pos, a = (p >> 12) & DC_HMASK;
                const s16 *pa = H + 2 * a, *pb = H + 2 * ((a + 1) & DC_HMASK);
                s32 fr = p & 0xfff, wv = win[ph >> 24];
                s32 l = pa[0] + (((pb[0] - pa[0]) * fr) >> 12);
                s32 r = pa[1] + (((pb[1] - pa[1]) * fr) >> 12);
                if (f) {
                    s32 hh = clamp17(l - l0 - ((q * b0) >> 14));
                    b0 = clamp17(b0 + ((f * hh) >> 14));
                    l0 = clamp17(l0 + ((f * b0) >> 14));
                    hh = clamp17(r - l1 - ((q * b1) >> 14));
                    b1 = clamp17(b1 + ((f * hh) >> 14));
                    l1 = clamp17(l1 + ((f * b1) >> 14));
                    l = sat16(bp ? b0 : l0);
                    r = sat16(bp ? b1 : l1);
                }
                if (cr) {
                    l = (l >> cr) << cr;
                    r = (r >> cr) << cr;
                }
                o[0] += (l * ((wv * gl) >> 15)) >> 15;
                o[1] += (r * ((wv * gr) >> 15)) >> 15;
                o += 2;
                if (h->loop) {
                    ph = ph + dph < ph ? 0xffffffffu : ph + dph;
                    rel += inc;
                    if (rel >= len) {
                        rel = inc > 0 ? rel - len : rel + len;
                        if (rel >= len)
                            rel = inc > 0 ? 0 : len - 1;
                        ph = 0;
                        if (h->dying || (h->cyc > 0 && --h->cyc == 0)) {
                            h->on = 0;
                            break;
                        }
                    }
                } else {
                    u32 nph = ph + dph;
                    if (nph < ph) {
                        h->on = 0;
                        break;
                    }
                    ph = nph;
                    pos += inc;
                }
            }
            h->pos = pos, h->rel = rel;
            h->fl0 = l0, h->fb0 = b0, h->fl1 = l1, h->fb1 = b1;
        } else if (h->loop) {
            u32 ls = h->ls, rel = h->rel, len = h->len;
            for (i = 0; i < N; i++) {
                u32 p = ls + rel, a = (p >> 12) & DC_HMASK;
                const s16 *pa = H + 2 * a, *pb = H + 2 * ((a + 1) & DC_HMASK);
                s32 fr = p & 0xfff, wv = win[ph >> 24];
                s32 l = pa[0] + (((pb[0] - pa[0]) * fr) >> 12);
                s32 r = pa[1] + (((pb[1] - pa[1]) * fr) >> 12);
                o[0] += (l * ((wv * gl) >> 15)) >> 15;
                o[1] += (r * ((wv * gr) >> 15)) >> 15;
                o += 2;
                ph = ph + dph < ph ? 0xffffffffu : ph + dph;
                rel += inc;
                if (rel >= len) {
                    rel = inc > 0 ? rel - len : rel + len;
                    if (rel >= len)
                        rel = inc > 0 ? 0 : len - 1;
                    ph = 0;
                    if (h->dying || (h->cyc > 0 && --h->cyc == 0)) {
                        h->on = 0;
                        break;
                    }
                }
            }
            h->rel = rel;
        } else {
            u32 pos = h->pos;
            for (i = 0; i < N; i++) {
                u32 a = (pos >> 12) & DC_HMASK, nph;
                const s16 *pa = H + 2 * a, *pb = H + 2 * ((a + 1) & DC_HMASK);
                s32 fr = pos & 0xfff, wv = win[ph >> 24];
                s32 l = pa[0] + (((pb[0] - pa[0]) * fr) >> 12);
                s32 r = pa[1] + (((pb[1] - pa[1]) * fr) >> 12);
                o[0] += (l * ((wv * gl) >> 15)) >> 15;
                o[1] += (r * ((wv * gr) >> 15)) >> 15;
                o += 2;
                nph = ph + dph;
                if (nph < ph) {
                    h->on = 0;
                    break;
                }
                ph = nph;
                pos += inc;
            }
            h->pos = pos;
        }
        h->ph = ph;
    }
    dcosm.heads = (u16)n;
}

/* ---- the block's clock and the input's onsets ------------------------------------------------- */
static u32 hw;                  /* the history's write index (frames) */
static u32 P;                   /* frames a tick: the TIME knob's subdivision of the Digitone's tempo */
static u32 Pe;                  /* P for the engines' lengths and reach: at most 2 s, the history's fifth */
static u32 qfr;                 /* frames a quarter note */
static u32 tl_prev, clk_acc;
static s32 env_slow, peak;
static u32 refr;
static u32 ons_pos[ONS], ons_len[ONS], ons_n;   /* recent onsets: where they start, how long they run */
static u32 dly_fill;            /* frames written to PATTERN's line since PATTERN came on */

/* Ticks, in the timeline's units (21,600,000 a quarter): a bar, 1/2, 1/4, 1/8, 1/16, 1/32. */
static const u32 sub_units[6] = { 86400000u, 43200000u, 21600000u, 10800000u, 5400000u, 2700000u };

static int clock_block(void)
{
    u32 tempo = (u32)fw_tempo, tl = fw_timeline, pu;
    int tick;
    if (tempo < 2400 || tempo > 48000)          /* 20-400 BPM */
        tempo = 14400;
    pu = sub_units[(dcosm.knob[K_TIME] * 6) >> 7];
    P = (16u * pu) / tempo;
    Pe = P > 96000 ? 96000 : P;
    qfr = (16u * 21600000u) / tempo;
    if (tl - tl_prev >= 4800u && tl - tl_prev <= 96000u) {  /* the timeline runs: its grid */
        tick = (tl / pu) != (tl_prev / pu);
        clk_acc = 0;
    } else {                                    /* it stands: our own count */
        clk_acc += N;
        tick = clk_acc >= P;
        if (tick)
            clk_acc -= P;
    }
    tl_prev = tl;
    return tick;
}

static int onset_block(void)
{
    int on = 0;
    env_slow += (peak - env_slow) >> 5;
    if (refr) {
        refr--;
    } else if (peak > 900 && peak > 2 * env_slow + 200 && !dcosm.hold) {
        u32 at = (hw - N - 96) & DC_HMASK, last = ons_pos[(ons_n - 1) % ONS];
        if (ons_n) {
            u32 d = (at - last) & DC_HMASK;
            ons_len[(ons_n - 1) % ONS] = d < P ? d : P;
        }
        ons_pos[ons_n % ONS] = at;
        ons_len[ons_n % ONS] = P;
        ons_n++;
        refr = 90;                              /* 60 ms */
        on = 1;
    }
    return on;
}

/* ---- the engines: what each starts, each block --------------------------------------------------- */
static struct {
    u32 cnt, step, left, every, sub_left, sub_every, sub_n, ph, ph2;
    s32 g, rate;
    u32 src, life, ml;
    u32 seq_off[8];
    u8 seq_slow[8], seq_cut[8];
    u8 started, var;
    s16 tap_g[8];
} eng;
static u32 haze_acc, haze_ptr;
static s32 duck;                                /* INTERRUPT: frames the dry stays ducked */
static int act, rep, shp, var;
static u32 tap_d[8];
static s32 tap_gl[8], tap_gr[8], tap_fb;
static int ntaps;

static s32 rate_oct(s32 o)                      /* a rate from octaves, 1/256 octaves (-1024..1024) */
{
    s32 r = dc_exp2[o & 255];
    s32 sh = o >> 8;
    return sh >= 0 ? r << sh : r >> -sh;
}

static u32 recent(u32 back)                     /* the history's frame back frames ago */
{
    return (hw - back) & DC_HMASK;
}

static u32 clampu(u32 v, u32 lo, u32 hi) { return v < lo ? lo : v > hi ? hi : v; }

static int new_loop_due(void)                   /* MOSAIC, GLIDE: time to take new loops? */
{
    u32 every = rep >= 120 ? 0 : 1 + (u32)(rep >> 4);
    if (!eng.started) {
        eng.started = 1;
        eng.cnt = 0;
        return 1;
    }
    if (every && ++eng.cnt >= every) {
        eng.cnt = 0;
        return 1;
    }
    return 0;
}

static void eng_mosaic(int tick)
{
    static const s16 rates[4][4] = { { 4096, 8192, 4096, 8192 }, { 4096, 2048, 4096, 2048 },
                                     { 8192, 8192, 8192, 8192 }, { 2048, 4096, 8192, 16384 } };
    int n = 1 + ((act * 4) >> 7), i;
    u32 L;
    if (!tick || !new_loop_due())
        return;
    L = clampu(Pe, 2400, 192000);
    h_kill_tag(1);
    for (i = 0; i < n; i++) {
        struct head *h = h_get();
        if (!h)
            break;
        h_loop(h, recent(L), L, rates[var][i], gnorm[n], 16384 + ((i & 1) ? 5000 : -5000) * (1 + i / 2),
               shp, -1, 1);
        h->rel = (h->len / (u32)n) * (u32)i;    /* the same loop, offset: a rhythm of its own */
        h->rel &= ~0xfffu;
    }
}

static void eng_seq(int tick)
{
    int i, layers;
    u32 s, life;
    if (!eng.started || (eng.step == 0 && tick && rep < 120 && ++eng.cnt > (u32)(rep >> 3))) {
        for (i = 0; i < 8; i++) {               /* a new pattern: eight slices of the last bars */
            eng.seq_off[i] = Pe + rnd_n(3 * Pe < 65535u ? 3 * Pe : 65535u) + Pe * rnd_n(3);
            eng.seq_slow[i] = (u8)(rnd_n(128) < (u32)act);
            eng.seq_cut[i] = (u8)(30 + rnd_n(60));
        }
        eng.started = 1;
        eng.cnt = 0;
    }
    if (!tick)
        return;
    s = eng.step++ & 7;
    life = Pe;
    layers = var == 2 ? 1 + ((act * 3) >> 7) : var == 3 ? 1 + ((act * 2) >> 7) : 1;
    for (i = 0; i < layers; i++) {
        struct head *h = h_get();
        s32 rate = 4096;
        if (!h)
            return;
        if (var == 1 && eng.seq_slow[s])
            rate = 2048;
        h_shot(h, recent(eng.seq_off[(s + i * 3) & 7]), rate, life, gnorm[layers], 16384 + (s32)rnd_n(12000) - 6000,
               shp, 2);
        if (var == 0 || var == 2) {             /* a filter on each slice; C's sweep across the layers */
            h->f = dc_svf_f[var == 0 ? 127 - ((127 - eng.seq_cut[s]) * act >> 7) : eng.seq_cut[s] + i * 15];
            h->q = dc_svf_q[40];
        }
        if (var == 3)
            h->crush = (u8)(4 + (act >> 5));
    }
    if (act >= 120 && (var == 1 || var == 3)) {  /* B: a soft pad; D: crushed slices an octave down */
        struct head *h = h_get();
        if (h) {
            h_shot(h, recent(2 * Pe), var == 1 ? 4096 : 2048, var == 1 ? 4 * Pe : Pe, 12000, 16384, 3, 2);
            if (var == 3)
                h->crush = 7;
        }
    }
}

static void eng_glide(int tick)
{
    static const s16 lo[4] = { -256, 256, 0, -256 }, hi[4] = { 0, -256, 256, 256 };
    u32 period = 12000 - (u32)act * 90;         /* blocks: 8 s .. 0.4 s */
    s32 u, o;
    int i, n = var == 3 ? 4 : 2;
    eng.ph += 0xffffffffu / period;
    switch (shp) {
    case 0: u = eng.ph < 0x80000000u ? (s32)(eng.ph >> 16) : (s32)(0xffff - (eng.ph >> 16)); break;
    case 1: u = (s32)(eng.ph >> 17); break;
    case 2: u = 32767 - (s32)(eng.ph >> 17); break;
    default:                                    /* random targets, glided to */
        if (eng.ph < 0xffffffffu / period)
            eng.ph2 = rnd_n(32768);
        u = eng.g + (((s32)eng.ph2 - eng.g) >> 6);
        break;
    }
    if (shp == 0)
        u >>= 1;
    eng.g = u;
    if (tick && new_loop_due()) {
        u32 L = clampu(Pe, 2400, 96000);
        h_kill_tag(3);
        for (i = 0; i < n; i++) {
            struct head *h = h_get();
            if (!h)
                break;
            h_loop(h, recent(L), L, 4096, gnorm[n], 16384 + ((i & 1) ? 6000 : -6000), 0, -1, 3);
        }
    }
    for (i = 0; i < MAXH; i++) {
        struct head *h = &hd[i];
        s32 rate, uu = u;
        if (!h->on || h->tag != 3 || h->dying)
            continue;
        if (var == 3 && (i & 1))
            uu = 32767 - u;
        o = lo[var] + (((hi[var] - lo[var]) * uu) >> 15);
        rate = rate_oct(o);
        h->inc = h->inc < 0 ? -rate : rate;
        h->dph = loop_dph(rate, h->len >> 12);
    }
}

static void eng_haze(void)
{
    u32 dens = 6 + (u32)act * 90 / 127, life = 1440 + (u32)rep * 180, over;
    int spawns = 0;
    haze_acc += dens * N;
    haze_ptr += N / 4;                          /* A: a reader at a quarter of the input's speed */
    if (((hw - haze_ptr) & DC_HMASK) > 96000)
        haze_ptr = hw - 9600;
    over = dens * life / 48000;
    while (haze_acc >= 48000 && spawns < 4) {
        struct head *h = h_get();
        s32 rate = 4096;
        u32 src;
        haze_acc -= 48000;
        if (!h)
            break;
        spawns++;
        if (var == 0) {
            src = (haze_ptr - rnd_n(2400)) & DC_HMASK;
            rate += (s32)rnd_n(41) - 20;
        } else {
            src = recent(life * 4 + rnd_n(var == 1 ? 60000 : 36000));
            if (var == 1)
                rate += (s32)rnd_n(81) - 40;
            else if (rnd_n(2))
                rate = var == 2 ? 8192 : 2048;
        }
        h_shot(h, src, rate, life, gnorm[over > 16 ? 16 : over < 1 ? 1 : over], 4096 + (s32)rnd_n(24576), shp, 4);
    }
    if (haze_acc > 4 * 48000)
        haze_acc = 0;
}

static void eng_tunnel(int tick, int onset)
{
    u32 ml = clampu(Pe / 8, 480, 7200);
    s32 dec, lfo;
    int i;
    eng.ph += 0x00100000u;                      /* a slow LFO, 0.35 Hz */
    lfo = dc_sin[eng.ph >> 24];
    if (var == 3 && onset)
        eng.ml = 480 + rnd_n(6720 * (u32)act / 127 + 1);
    if (!eng.ml)
        eng.ml = ml;
    if ((tick && env_slow > 400) || !eng.started) {
        int n = var == 2 ? 3 : 2;
        eng.started = 1;
        h_kill_tag(5);
        for (i = 0; i < n; i++) {
            struct head *h = h_get();
            u32 len = var == 3 ? eng.ml : ml;
            if (!h)
                break;
            h_loop(h, recent(len + 480 * (u32)i), len, (var == 1 && i == 1) ? 2048 : 4096,
                   var == 2 ? gnorm[n] / 2 : gnorm[n], 16384 + (i - 1) * 7000, shp, -1, 5);  /* C rings */
            if (var == 1) {
                h->f = 1;                       /* swept below */
                h->q = dc_svf_q[20];
            } else if (var == 2) {
                h->f = dc_svf_f[55 + 18 * i];
                h->q = dc_svf_q[60 + act / 2];
                h->bp = 1;
            }
        }
    }
    dec = (127 - rep) / 16;                     /* Repeats: how long the drone lasts (0.8 s .. for ever) */
    for (i = 0; i < MAXH; i++) {
        struct head *h = &hd[i];
        if (!h->on || h->tag != 5)
            continue;
        if (var == 0) {                         /* A: the loop's length breathes */
            u32 len = (u32)((s32)ml + ((lfo * (s32)(ml / 2) * act / 127) >> 15));
            h->len = clampu(len, 240, 96000) << 12;
            h->dph = loop_dph(h->inc < 0 ? -h->inc : h->inc, h->len >> 12);
        } else if (var == 1) {                  /* B: a filter sweep */
            h->f = dc_svf_f[64 + ((lfo * (20 + act / 3)) >> 15)];
        }
        if (dec) {
            h->gl -= (h->gl * dec) >> 13;
            h->gr -= (h->gr * dec) >> 13;
            if (h->gl < 16 && h->gr < 16)
                h->on = 0;
        }
    }
}

static void eng_strum(int tick, int onset)
{
    int i, copies;
    u32 o;
    if (onset) {
        eng.left = rep >= 120 ? 0xffffffffu : 1 + (u32)(rep >> 2);
        eng.g = 26000;
        eng.step = 0;
    }
    if (!tick || !eng.left || !ons_n)
        return;
    eng.left--;
    copies = var == 1 ? 1 + ((act * 5) >> 7) : 1;
    if (var >= 2) {                             /* C, D: a cascade through the recent onsets */
        u32 k = 2 + (((u32)act * 6) >> 7);
        if (k > ons_n)
            k = ons_n;
        if (k > ONS)
            k = ONS;
        o = (ons_n - 1 - (eng.step++ % k)) % ONS;
    } else {
        o = (ons_n - 1) % ONS;
    }
    for (i = 0; i < copies; i++) {
        struct head *h = h_get();
        if (!h)
            break;
        h_shot(h, (ons_pos[o] + (u32)i * (240 + rnd_n(1700))) & DC_HMASK, 4096 + (i ? (s32)rnd_n(25) - 12 : 0),
               ons_len[o], (eng.g * gnorm[copies]) >> 15, 16384 + (i & 1 ? 1 : -1) * 3000 * i, shp, 6);
    }
    if (var == 3) {
        struct head *h = h_get();
        if (h)
            h_shot(h, ons_pos[o], 8192, ons_len[o] / 2, eng.g / 2, 16384, 3, 6);
    }
    eng.g = (eng.g * (24000 + rep * 60)) >> 15;  /* each repeat a little quieter */
}

static void block_shot(u32 src, u32 life, int sub)
{
    static const s16 rr[4] = { 2048, 4096, 6144, 8192 };
    struct head *h = h_get();
    s32 rate = 4096;
    if (!h)
        return;
    if (var == 1 || var == 3)
        rate = rr[rnd_n(4)];
    h_shot(h, src, rate, life, sub ? 20000 : 26000, 6000 + (s32)rnd_n(20000), var == 2 ? 3 : shp, 7);
    if (var == 2) {
        h->f = dc_svf_f[40 + rnd_n(50)];
        h->q = dc_svf_q[30];
    } else if (var == 3) {
        h->crush = (u8)(5 + rnd_n(4));
    }
}

static void eng_blocks(int tick, int onset)
{
    u32 life = Pe / (1 + (((u32)act * 3) >> 7));
    if (eng.sub_left && ++eng.cnt >= eng.sub_every) {      /* A: a burst's repeats */
        eng.cnt = 0;
        eng.sub_left--;
        block_shot(eng.src, eng.life, 1);
    }
    if (!(onset || (tick && rnd_n(128) < (u32)rep)))
        return;
    eng.src = recent(Pe + rnd_n(3 * Pe < 65535u ? 3 * Pe : 65535u));
    block_shot(eng.src, life, 0);
    if (var == 0 && rnd_n(128) < (u32)act) {
        eng.sub_left = 3 + rnd_n(4);
        eng.life = Pe / 8;
        eng.sub_every = clampu(Pe / 8 / N, 1, 10000);
        eng.cnt = 0;
    }
}

static void eng_interrupt(int tick)
{
    if (eng.sub_left && ++eng.cnt >= eng.sub_every) {
        static const s16 rr[2] = { 2048, 8192 };
        struct head *h = h_get();
        eng.cnt = 0;
        eng.sub_left--;
        if (h) {
            s32 rate = var == 1 || var == 3 ? rr[rnd_n(2)] : 4096;
            h_shot(h, recent(Pe / 2 + rnd_n(2 * Pe < 65535u ? 2 * Pe : 65535u)), rate, eng.life, 28000,
                   8192 + (s32)rnd_n(16384), 0, 8);
            if (var == 2) {
                h->f = dc_svf_f[100 - 15 * eng.sub_left];
                h->q = dc_svf_q[70];
            } else if (var == 3) {
                h->crush = 6 + (act >> 6);
            }
        }
    }
    if (!tick || eng.sub_left || rnd_n(128) >= (u32)rep + 16)
        return;
    eng.sub_n = 2 + (((u32)act * 4) >> 7);
    eng.sub_left = eng.sub_n;
    eng.life = Pe / eng.sub_n;
    eng.sub_every = clampu(eng.life / N, 1, 10000);
    eng.cnt = eng.sub_every;                    /* the first one now */
    duck = (s32)Pe;
}

static void eng_arp(int tick)
{
    static const s16 ratios[4] = { 4096, 5161, 6137, 8192 };    /* 1, a major third, a fifth, 2 */
    u32 k = 2 + (((u32)act * 6) >> 7), o;
    struct head *h;
    if (!tick || !ons_n)
        return;
    if (k > ons_n)
        k = ons_n;
    if (k > ONS)
        k = ONS;
    o = (ons_n - 1 - (eng.step % k)) % ONS;
    h = h_get();
    if (h) {
        h_shot(h, ons_pos[o], var == 1 ? ratios[eng.step & 3] : 4096,
               Pe * (40 + (u32)rep) / 167, 26000, 16384 + ((eng.step & 1) ? 6000 : -6000), shp, 9);
        if (var == 2) {
            h->f = dc_svf_f[35 + rnd_n(70)];
            h->q = dc_svf_q[50];
        } else if (var == 3) {
            h->crush = 6;
        }
    }
    eng.step++;
}

static void eng_pattern(void)
{
    static const u8 pos12[4][8] = { { 12, 24, 36, 48, 60, 72, 84, 96 }, { 6, 18, 24, 36, 42, 54, 60, 72 },
                                    { 12, 21, 27, 30, 42, 45, 51, 54 }, { 8, 16, 24, 32, 40, 48, 56, 64 } };
    int k;
    s32 g = 22000;
    ntaps = 1 + ((act * 8) >> 7);
    if (!eng.started) {
        for (k = 0; k < 8; k++)
            eng.tap_g[k] = (s16)(8000 + rnd_n(16000));
        eng.started = 1;
    }
    for (k = 0; k < ntaps; k++) {
        u32 d = (Pe / 12) * pos12[var][k];
        s32 gk;
        tap_d[k] = d < DC_DLY_FR - 64 ? d : DC_DLY_FR - 64;
        gk = shp == 0 ? g : shp == 1 ? 18000 : shp == 2 ? 6000 + 2400 * k : eng.tap_g[k];
        if (tap_d[k] >= dly_fill)
            gk = 0;
        tap_gl[k] = (k & 1) ? gk / 3 : gk;
        tap_gr[k] = (k & 1) ? gk : gk / 3;
        g = (g * 26000) >> 15;
    }
    tap_fb = (s32)rep * 30000 / 127;
}

static void eng_warp(int tick)
{
    static const s16 rr[4] = { 8192, 2048, 6144, 3072 };
    int n = 1 + ((act * 6) >> 7), k;
    s32 g = 26000;
    if (!tick)
        return;
    for (k = 0; k < n; k++) {
        struct head *h = h_get();
        u32 d = (Pe / 2) * (u32)(k + 1);
        if (!h)
            break;
        h_shot(h, recent(d + Pe), var == 2 ? rr[k & 3] : (var == 3 && (k & 1)) ? 8192 : 4096,
               (var == 3 && (k & 1)) ? Pe / 2 : Pe, g, (k & 1) ? 24000 : 8768, shp, 10);
        if (var == 0) {                         /* an envelope filter: the input's level opens it */
            h->f = dc_svf_f[clampu(30 + (u32)env_slow / 64, 30, 120)];
            h->q = dc_svf_q[50];
        } else if (var == 1) {                  /* a resonant band-pass a tap */
            h->f = dc_svf_f[50 + rnd_n(55)];
            h->q = dc_svf_q[100];
            h->bp = 1;
        }
        g = (g * (16000 + rep * 120)) >> 15;
    }
}

/* PATTERN's taps off its own line, with feedback, into w; x feeds the line. */
static void taps_render(s32 *w, const s32 *x, int feed)
{
    s16 *D = DC_DLY;
    static u32 dw;
    int i, k, last = ntaps - 1;
    for (i = 0; i < N; i++) {
        s32 l = 0, r = 0;
        const s16 *pf = D + 2 * ((dw - tap_d[last]) & DC_DMASK);
        for (k = 0; k < ntaps; k++) {
            const s16 *p = D + 2 * ((dw - tap_d[k]) & DC_DMASK);
            l += (p[0] * tap_gl[k]) >> 15;
            r += (p[1] * tap_gr[k]) >> 15;
        }
        D[2 * dw] = (s16)sat16((feed ? x[2 * i] : 0) + ((pf[0] * tap_fb) >> 15));
        D[2 * dw + 1] = (s16)sat16((feed ? x[2 * i + 1] : 0) + ((pf[1] * tap_fb) >> 15));
        dw = (dw + 1) & DC_DMASK;
        w[2 * i] += l;
        w[2 * i + 1] += r;
    }
    if (dly_fill < DC_DLY_FR)
        dly_fill += N;
}

/* ---- the looper ------------------------------------------------------------------------------- */
static u32 lpos;                                /* 22.10 frames into the loop */
static s32 lfade, lfade_to, lfade_step;         /* Q15 */
static u32 lidx[N];                             /* this block's playback frames, for the overdub */
static s32 lp[2 * N];

static void loop_close(int next)
{
    u32 len = dcosm.lrec;
    if (dcosm.cfg[G_QUANT] && qfr && len >= qfr)
        len = len / qfr * qfr;
    if (len < 4800)
        len = 4800 < dcosm.lrec ? 4800 : dcosm.lrec;
    if (len < 64) {
        dcosm.lstate = L_EMPTY;
        return;
    }
    dcosm.llen = len;
    lpos = dcosm.reverse ? (len << 10) - 1 : 0;
    lfade = lfade_to = 32767;
    dcosm.lstate = (u8)next;
}

static void loop_fade_to(s32 to)
{
    u32 frames = (u32)dcosm.shift[S_FADE] * 3000;
    lfade_to = to;
    lfade_step = frames ? (s32)(32767u * N / frames) + 1 : 16384;
}

static void loop_command(void)
{
    int c = dcosm.lcmd, s = dcosm.lstate;
    dcosm.lcmd = C_NONE;
    switch (c) {
    case C_T1:
        if (s == L_EMPTY) {
            dcosm.lrec = 0;
            dcosm.lstate = L_REC;
        } else if (s == L_REC) {
            loop_close(dcosm.cfg[G_ORDER] ? L_DUB : L_PLAY);
        } else if (s == L_PLAY) {
            if (dcosm.bvalid && !dcosm.cfg[G_BURST])
                dcosm.lstate = L_DUB;
        } else if (s == L_DUB) {
            dcosm.lstate = L_PLAY;
        } else if (s == L_STOP) {
            lpos = 0;
            lfade = 0;
            loop_fade_to(32767);
            dcosm.lstate = L_PLAY;
        }
        break;
    case C_STOP:
        if (s == L_REC)
            loop_close(L_STOP);
        else if (s == L_PLAY || s == L_DUB) {
            dcosm.lstate = L_STOP;
            loop_fade_to(0);
        }
        break;
    case C_UNDO:
        if (dcosm.llen) {
            dcosm.bvalid = 0;
            dcosm.lclear_len = dcosm.llen;
            if (s == L_DUB)
                dcosm.lstate = L_PLAY;
        }
        break;
    case C_ERASE:
        dcosm.lstate = L_EMPTY;
        dcosm.llen = 0;
        break;
    case C_BURST_DOWN:
        dcosm.lrec = 0;
        dcosm.llen = 0;
        dcosm.lstate = L_REC;
        break;
    case C_BURST_UP:
        if (s == L_REC)
            loop_close(L_PLAY);
        break;
    }
}

/* Playback into lp (Q15), at the looper's speed, with its fade and level. */
static void loop_play(void)
{
    static const s16 speeds[5] = { 256, 512, 1024, 2048, 4096 };
    const s16 *A = DC_LOOPA, *B = DC_LOOPB;
    s32 lev = dc_level[dcosm.knob[K_LOOP]], sp = speeds[(dcosm.shift[S_LSPEED] * 5) >> 7], g;
    u32 L = dcosm.llen << 10;
    int i, s = dcosm.lstate, b = dcosm.bvalid;
    if (dcosm.reverse)
        sp = -sp;
    if (lfade < lfade_to)
        lfade = lfade + lfade_step > lfade_to ? lfade_to : lfade + lfade_step;
    else if (lfade > lfade_to)
        lfade = lfade - lfade_step < lfade_to ? lfade_to : lfade - lfade_step;
    if (!dcosm.llen || s == L_EMPTY || s == L_REC || (s == L_STOP && lfade == 0)) {
        for (i = 0; i < 2 * N; i++)
            lp[i] = 0;
        return;
    }
    g = (lev * lfade) >> 15;                    /* Q14 */
    for (i = 0; i < N; i++) {
        u32 a = lpos >> 10, nb = a + 1 < dcosm.llen ? a + 1 : 0;
        s32 fr = (lpos & 1023) << 2;
        s32 l0 = A[2 * a], r0 = A[2 * a + 1], l1 = A[2 * nb], r1 = A[2 * nb + 1];
        if (b) {
            l0 += B[2 * a], r0 += B[2 * a + 1], l1 += B[2 * nb], r1 += B[2 * nb + 1];
        }
        lidx[i] = a;
        lp[2 * i] = ((l0 + (((l1 - l0) * fr) >> 12)) * g) >> 14;
        lp[2 * i + 1] = ((r0 + (((r1 - r0) * fr) >> 12)) * g) >> 14;
        lpos += (u32)sp;
        if (lpos >= L)
            lpos = sp > 0 ? lpos - L : lpos + L;
    }
}

/* Recording and overdubbing src (Q15), after the engine. */
static void loop_write(const s32 *src)
{
    s16 *A = DC_LOOPA, *B = DC_LOOPB;
    int i, s = dcosm.lstate;
    if (s == L_REC) {
        u32 r = dcosm.lrec;
        for (i = 0; i < N && r < DC_LOOP_FR; i++, r++) {
            A[2 * r] = (s16)sat16(src[2 * i]);
            A[2 * r + 1] = (s16)sat16(src[2 * i + 1]);
            B[2 * r] = B[2 * r + 1] = 0;
        }
        dcosm.lrec = r;
        if (r >= DC_LOOP_FR)
            loop_close(L_PLAY);
    } else if (s == L_DUB && dcosm.bvalid) {
        for (i = 0; i < N; i++) {
            u32 a = lidx[i];
            B[2 * a] = (s16)sat16(B[2 * a] + src[2 * i]);
            B[2 * a + 1] = (s16)sat16(B[2 * a + 1] + src[2 * i + 1]);
        }
    }
}

/* ---- pitch modulation, the filter, Space ----------------------------------------------------- */
static u32 mod_w, mod_lfo;
static void mod_render(s32 *w)
{
    s16 *M = DC_MODL;
    int depth = dcosm.shift[S_MDEP], rate = dcosm.shift[S_MRATE], i;
    u32 hz100 = 5 + (u32)rate * (u32)rate * 800 / 16129;
    u32 d;
    mod_lfo += hz100 * 28633u;
    d = (480u << 12) + (u32)((((s32)dc_sin[mod_lfo >> 24] + 32768) * depth * 3) >> 3);
    for (i = 0; i < N; i++) {
        u32 rp = (mod_w << 12) - d, a = (rp >> 12) & (DC_MOD_FR - 1), b = (a + 1) & (DC_MOD_FR - 1);
        s32 fr = rp & 0xfff;
        M[2 * mod_w] = (s16)sat16(w[2 * i]);
        M[2 * mod_w + 1] = (s16)sat16(w[2 * i + 1]);
        if (depth) {
            s32 l = M[2 * a] + (((M[2 * b] - M[2 * a]) * fr) >> 12);
            s32 r = M[2 * a + 1] + (((M[2 * b + 1] - M[2 * a + 1]) * fr) >> 12);
            w[2 * i] = (w[2 * i] + l) >> 1;
            w[2 * i + 1] = (w[2 * i + 1] + r) >> 1;
        }
        mod_w = (mod_w + 1) & (DC_MOD_FR - 1);
    }
}

static s32 flt_l[2], flt_b[2];
static void filter_render(s32 *w)
{
    int k = dcosm.knob[K_FLT], i;
    s32 f, q, l0 = flt_l[0], b0 = flt_b[0], l1 = flt_l[1], b1 = flt_b[1];
    if (k == 127)
        return;
    if (k == 0) {
        for (i = 0; i < 2 * N; i++)
            w[i] = 0;
        return;
    }
    f = dc_svf_f[k];
    q = dc_svf_q[dcosm.shift[S_RESO]];
    for (i = 0; i < N; i++) {
        s32 hh = clamp17(w[2 * i] - l0 - ((q * b0) >> 14));
        b0 = clamp17(b0 + ((f * hh) >> 14));
        l0 = clamp17(l0 + ((f * b0) >> 14));
        w[2 * i] = l0;
        hh = clamp17(w[2 * i + 1] - l1 - ((q * b1) >> 14));
        b1 = clamp17(b1 + ((f * hh) >> 14));
        l1 = clamp17(l1 + ((f * b1) >> 14));
        w[2 * i + 1] = l1;
    }
    flt_l[0] = l0, flt_b[0] = b0, flt_l[1] = l1, flt_b[1] = b1;
}

/* Space: four allpasses, then an 8-line feedback delay network with damping; four rooms. */
#define NL 8
static const u16 rv_base[NL] = { 1499, 1777, 1949, 2129, 2357, 2551, 2789, 3011 };
static const u16 rv_mult[4] = { 128, 205, 358, 512 };              /* Q8: room, dark, hall, ambient */
static const s16 rv_fbk[4] = { 23593, 26214, 28836, 30474 };
static const s16 rv_lpc[4] = { 26214, 9830, 16384, 13107 };        /* the damping's low-pass */
static const u16 ap_len[4] = { 142, 107, 379, 277 };
static u32 rv_len[NL], rv_w[NL], ap_w[4];
static s32 rv_lp[NL];
static int rv_mode = -1;

static void space_render(s32 *w, int feed)
{
    int amt = dcosm.knob[K_SPC], mode = (dcosm.shift[S_VERB] * 4) >> 7, i, k;
    s16 *R = DC_REV;
    s32 fbk, lpc, a;
    if (mode != rv_mode) {
        rv_mode = mode;
        for (k = 0; k < NL; k++) {
            rv_len[k] = ((u32)rv_base[k] * rv_mult[mode]) >> 8;
            if (rv_w[k] >= rv_len[k])
                rv_w[k] = 0;
        }
    }
    if (!amt)
        return;
    a = (amt * 16384) / 127;
    fbk = rv_fbk[mode];
    lpc = rv_lpc[mode];
    for (i = 0; i < N; i++) {
        s32 x = feed ? sat16((w[2 * i] + w[2 * i + 1]) >> 1) : 0, y[NL], s = 0, yl, yr;
        for (k = 0; k < 4; k++) {               /* diffusion */
            s16 *b = R + NL * DC_REV_LINE + 1024 * k;
            u32 p = ap_w[k];
            s32 v = b[p], u = x - (v >> 1);
            b[p] = (s16)sat16(u);
            x = v + (u >> 1);
            ap_w[k] = p + 1 < ap_len[k] ? p + 1 : 0;
        }
        for (k = 0; k < NL; k++) {
            s32 v = R[DC_REV_LINE * k + rv_w[k]];
            rv_lp[k] += ((v - rv_lp[k]) * lpc) >> 15;
            y[k] = rv_lp[k];
            s += y[k];
        }
        s >>= 2;                                /* Householder: y - 2/N sum y */
        for (k = 0; k < NL; k++) {
            R[DC_REV_LINE * k + rv_w[k]] = (s16)sat16((((y[k] - s) * fbk) >> 15) + x);
            rv_w[k] = rv_w[k] + 1 < rv_len[k] ? rv_w[k] + 1 : 0;
        }
        yl = (y[0] + y[2] + y[4] + y[6]) >> 1;
        yr = (y[1] + y[3] + y[5] + y[7]) >> 1;
        {
            s32 wl = clamp17(w[2 * i]), wr = clamp17(w[2 * i + 1]);
            w[2 * i] = wl + (((clamp17(yl) - wl) >> 1) * a >> 13);
            w[2 * i + 1] = wr + (((clamp17(yr) - wr) >> 1) * a >> 13);
        }
    }
}

/* ---- the block ------------------------------------------------------------------------------- */
static u8 cur_engine = 0xff, cur_var, cur_rev;
static u32 t_prev;

void dcosm_dsp_reset(void)
{
    int k;
    for (k = 0; k < MAXH; k++)
        hd[k].on = 0;
    hw = 0;
    ons_n = 0;
    env_slow = 0;
    refr = 0;
    haze_acc = 0;
    haze_ptr = 0;
    duck = 0;
    dly_fill = 0;
    cur_engine = 0xff;
    flt_l[0] = flt_l[1] = flt_b[0] = flt_b[1] = 0;
    for (k = 0; k < NL; k++)
        rv_lp[k] = 0;
}

void dcosm_render(int32_t *out, const int32_t *in)
{
    static s32 x[2 * N], w[2 * N];
    u32 t0 = DTCN0;
    s32 g = dc_in_gain[dcosm.shift[S_GAIN]], dg, wg, pkl = 0, pkr = 0;
    int i, tick, onset, pre = dcosm.cfg[G_PRE], byp = dcosm.bypass, e = dcosm.engine;

    act = dcosm.knob[K_ACT], rep = dcosm.knob[K_REP], shp = dcosm.knob[K_SHP] >> 5, var = dcosm.var;

    /* the input: gain, mono, its peak */
    for (i = 0; i < N; i++) {
        s32 l = ((in[2 * i] >> 16) * g) >> 12, r;
        r = dcosm.cfg[G_MONO] ? l : ((in[2 * i + 1] >> 16) * g) >> 12;
        x[2 * i] = sat16(l);
        x[2 * i + 1] = sat16(r);
        l = l < 0 ? -l : l;
        r = r < 0 ? -r : r;
        if (l > pkl) pkl = l;
        if (r > pkr) pkr = r;
    }
    if (pkl > 32767) pkl = 32767;
    if (pkr > 32767) pkr = 32767;
    peak = pkl > pkr ? pkl : pkr;
    if (pkl > dcosm.meter[0])
        dcosm.meter[0] = (u16)pkl;
    if (pkr > dcosm.meter[1])
        dcosm.meter[1] = (u16)pkr;

    /* the engine's change, reversing */
    if (e != cur_engine || dcosm.var != cur_var) {
        for (i = 0; i < MAXH; i++)
            if (hd[i].on && hd[i].loop)
                hd[i].dying = 1;
        eng.started = 0;
        eng.cnt = eng.step = eng.left = eng.sub_left = 0;
        eng.ml = 0;
        eng.g = 0;
        if (e == E_PATTERN && cur_engine != E_PATTERN)
            dly_fill = 0;
        cur_engine = (u8)e;
        cur_var = dcosm.var;
    }
    if (dcosm.reverse != cur_rev) {
        for (i = 0; i < MAXH; i++)
            hd[i].inc = -hd[i].inc;
        cur_rev = dcosm.reverse;
    }

    loop_command();
    tick = clock_block();
    onset = onset_block();

    /* the looper's playback first: pre-FX it feeds the history */
    loop_play();
    if (!dcosm.hold) {
        s16 *p = DC_HIST + 2 * hw;
        for (i = 0; i < N; i++) {
            p[2 * i] = (s16)sat16(x[2 * i] + (pre ? lp[2 * i] : 0));
            p[2 * i + 1] = (s16)sat16(x[2 * i + 1] + (pre ? lp[2 * i + 1] : 0));
        }
        hw = (hw + N) & DC_HMASK;
    }

    /* the engine */
    for (i = 0; i < 2 * N; i++)
        w[i] = 0;
    if (!byp && !dcosm.cfg[G_ONLY]) {
        switch (e) {
        case E_MOSAIC: eng_mosaic(tick); break;
        case E_SEQ: eng_seq(tick); break;
        case E_GLIDE: eng_glide(tick); break;
        case E_HAZE: eng_haze(); break;
        case E_TUNNEL: eng_tunnel(tick, onset); break;
        case E_STRUM: eng_strum(tick, onset); break;
        case E_BLOCKS: eng_blocks(tick, onset); break;
        case E_INTERRUPT: eng_interrupt(tick); break;
        case E_ARP: eng_arp(tick); break;
        case E_PATTERN: eng_pattern(); break;
        case E_WARP: eng_warp(tick); break;
        }
    }
    if (!dcosm.cfg[G_ONLY]) {
        heads_render(w);
        if (e == E_PATTERN)
            taps_render(w, x, !byp);
    }

    /* the looper: it records what the engine made (post-FX) or the input (pre-FX), and plays into the
     * effects' path (post) or beside the input (pre) */
    if (dcosm.lstate == L_REC || dcosm.lstate == L_DUB) {
        static s32 src[2 * N];
        for (i = 0; i < 2 * N; i++)
            src[i] = pre ? x[i] : x[i] + w[i];
        loop_write(src);
    }
    if (!pre)
        for (i = 0; i < 2 * N; i++)
            w[i] += lp[i];

    mod_render(w);
    filter_render(w);
    space_render(w, !byp);

    /* Mix: the dry input at 20 bits against the effects; INTERRUPT ducks the dry */
    dg = dc_mix_dry[dcosm.knob[K_MIX]];
    wg = (dc_mix_wet[dcosm.knob[K_MIX]] * dc_level[dcosm.shift[S_FXVOL]]) >> 14;
    if (byp)
        dg = 2048;
    if (duck > 0) {
        if (e == E_INTERRUPT && !byp)
            dg = (dg * (127 - dcosm.knob[K_MIX])) / 127;
        duck -= N;
    }
    for (i = 0; i < 2 * N; i++) {
        s32 d = in[i] >> 12, v, wv = w[i];
        if (pre)
            d += lp[i] << 4;
        if (wv > 131071) wv = 131071;
        else if (wv < -131071) wv = -131071;
        v = ((d * dg) >> 7) + ((wv * wg) >> 3);
        if (v > 8388607) v = 8388607;
        else if (v < -8388607) v = -8388607;
        out[i] = v << 8;
    }

    {                                           /* the CPU guard: this render's share of a block */
        static u32 slow;
        u32 t1 = DTCN0, per = t0 - t_prev;
        if (per)
            dcosm.cpu = (u16)((t1 - t0) * 1000u / per);
        t_prev = t0;
        if (dcosm.cpu > 450 && hcap > 4)        /* over budget: fewer new grains, at once */
            hcap--;
        else if (dcosm.cpu < 300 && hcap < MAXH && (++slow & 63) == 0)
            hcap++;                             /* under it: one more, slowly */
    }
}
