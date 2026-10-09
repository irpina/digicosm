/* SPDX-License-Identifier: GPL-2.0-or-later */
/* DigiCosm: a Microcosm-style effects engine for the Digitone mk1's audio inputs, on core-dn1 3.2's
 * exclusive audio (core_audio). Shared by dsp.c (the render, interrupt level) and ui.c (the UI task). */
#ifndef DCOSM_H
#define DCOSM_H

#include <stdint.h>
#include "digitone-mk1/core3.h"

typedef int32_t s32;
typedef uint32_t u32;
typedef int16_t s16;
typedef uint16_t u16;
typedef uint8_t u8;

/* The buffers: the region mod.json claims in the profile's bulk area. The OS never clears it, so the UI
 * clears what the render reads before it has written it (dcosm_clear_step). */
#define DC_BULK         0x44000000u
#define DC_BULK_END     0x45B00000u
#define DC_HIST_FR      (1u << 19)                      /* the input's history: 10.9 s, stereo s16 */
#define DC_HMASK        (DC_HIST_FR - 1)
#define DC_HIST         ((s16 *)DC_BULK)
#define DC_DLY_FR       (1u << 18)                      /* PATTERN's delay line: 5.4 s */
#define DC_DMASK        (DC_DLY_FR - 1)
#define DC_DLY          ((s16 *)(DC_BULK + 0x00200000u))
#define DC_REV          ((s16 *)(DC_BULK + 0x00300000u)) /* the reverb's lines */
#define DC_REV_LINE     8192
#define DC_MODL         ((s16 *)(DC_BULK + 0x00340000u)) /* pitch modulation's line: 4096 frames */
#define DC_MOD_FR       4096
#define DC_LOOP_FR      (60u * 48000u)                  /* the looper: 60 s stereo s16, two layers */
#define DC_LOOPA        ((s16 *)(DC_BULK + 0x00400000u))
#define DC_LOOPB        ((s16 *)(DC_BULK + 0x01000000u))
#define DC_CLEAR_END    (DC_BULK + 0x00344000u)         /* history, delay, reverb, modulation */

enum { E_MOSAIC, E_SEQ, E_GLIDE, E_HAZE, E_TUNNEL, E_STRUM, E_BLOCKS, E_INTERRUPT, E_ARP, E_PATTERN,
       E_WARP, E_COUNT };
enum { K_ACT, K_REP, K_SHP, K_FLT, K_MIX, K_TIME, K_SPC, K_LOOP };          /* the knobs, A-H */
enum { S_GAIN, S_MDEP, S_MRATE, S_RESO, S_FXVOL, S_LSPEED, S_VERB, S_FADE }; /* FUNC + A-H */
enum { L_EMPTY, L_REC, L_PLAY, L_DUB, L_STOP };                             /* the looper */
enum { C_NONE, C_T1, C_STOP, C_UNDO, C_ERASE, C_BURST_DOWN, C_BURST_UP };   /* looper commands */
enum { G_MONO, G_PRE, G_ONLY, G_QUANT, G_BURST, G_HOLDMOM, G_ORDER, G_MIDI, G_COUNT }; /* SETUP's rows */

/* What the UI sets and the render reads, and what the render reports. */
struct dc_state {
    volatile u8 knob[8], shift[8];
    volatile u8 engine, var, reverse, hold, bypass;
    volatile u8 cfg[G_COUNT];
    volatile u8 lcmd;                   /* a looper command, taken at the next block */
    volatile u8 lstate;                 /* L_*, the render's */
    volatile u8 bvalid;                 /* the overdub layer may be played and dubbed (0 while it clears) */
    volatile u32 llen;                  /* the loop's length, frames */
    volatile u32 lrec;                  /* frames recorded so far (L_REC) */
    volatile u32 lclear_len;            /* the UI clears the overdub layer's first lclear_len frames */
    volatile u16 meter[2];              /* the input's peak, 0-32767, decayed by the UI */
    volatile u16 cpu;                   /* the render's share of a block, per mille */
    volatile u16 heads;                 /* heads playing */
};
extern struct dc_state dcosm;
extern struct core_audio_owner dcosm_owner;

void dcosm_render(int32_t *out, const int32_t *in);
void dcosm_dsp_reset(void);
extern const char *const dcosm_engine_names[E_COUNT];

#endif
