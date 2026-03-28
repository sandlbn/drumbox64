/*
 *
 * Voice map:
 *   SID1 v0 = kick    SID1 v1 = snare   SID1 v2 = hihat
 *   SID2 v0 = tom     SID2 v1 = clap    SID2 v2 = crash
 *   (SID2 only used when g_dual_sid != 0)
 *
 * Single-SID fallback: tracks 3-6 map to SID1 voices 0-2.
 * Simultaneous same-voice hits cut the earlier sound. Acceptable.
 *
 * Synthesis overview:
 *   KICK  - Triangle, fast pitch sweep down. Character changes per kit:
 *           909=punchy short, 808=deep long, ROCK=mid warm.
 *   SNARE - Noise waveform, short envelope.
 *   HIHAT - Noise, very short (closed) or longer (open) TTL.
 *   TOM   - Triangle, moderate sweep.
 *   CLAP  - Noise, short burst.
 *   CRASH - Noise, long release.
 *
 * SID frequency formula (PAL, phi2=985248):
 *   word = Hz * 16777216 / 985248
 * Selected pre-computed values:
 *   55Hz=937, 110Hz=1874, 220Hz=3748, 440Hz=7495, 880Hz=14990
 */

#include "drumbox.h"

/* Hardware register macros */
#define SID1  ((volatile uint8_t *)0xD400)
#define SID2  ((volatile uint8_t *)g_sid2_base)

/* SID2 address table - common expansion board addresses */
const uint16_t g_sid2_addrs[] = { 0xDE00, 0xDF00, 0xD500, 0xD420 };
const uint8_t  g_sid2_num_addrs = 4;

/* Runtime SID2 configuration */
uint16_t g_sid2_base = 0xDE00;   /* default: most common expansion address */
uint8_t  g_sid2_idx  = 0;        /* index into g_sid2_addrs */

/* ── Low-level writes ────────────────────────────────────────────── */
void sid_write(uint8_t sid, uint8_t reg, uint8_t val)
{
    if (sid == 0) SID1[reg] = val;
    else          SID2[reg] = val;
}

uint8_t sid_read(uint8_t sid, uint8_t reg)
{
    if (sid == 0) return SID1[reg];
    else          return SID2[reg];
}

/* Write to a specific voice register (voice*7 + reg_offset) */
static void vw(uint8_t sid, uint8_t v, uint8_t r, uint8_t val)
{
    uint8_t reg = (uint8_t)((v << 1) + (v << 2) + r); /* v*7+r without multiply */
    /* v*7 = v*4 + v*2 + v = (v<<2)+(v<<1)+v */
    /* Simpler: just use v*7 directly - Oscar64 handles 8-bit multiply cheaply */
    reg = (uint8_t)(v * 7u + r);
    if (sid == 0) SID1[reg] = val;
    else          SID2[reg] = val;
}

/* ── Dual SID detection ──────────────────────────────────────────── */
int sid_detect_dual(void)
{
    /* Cannot reliably detect on real hardware without readable registers.
     * Default to single SID (safe). User enables with '2' key. */
    return 0;
}

/* ── Voice state ─────────────────────────────────────────────────── */
VoiceState g_voices[6];

/* Track (0-6) -> physical voice index (0-5) */
static const uint8_t T2V[NUM_TRACKS] = { 0, 1, 2, 2, 3, 4, 5 };
/* OHH shares voice 2 with CHH - longer TTL distinguishes them */

/* Physical voice -> SID chip */
static const uint8_t V_SID[6] = { 0, 0, 0, 1, 1, 1 };
/* Physical voice -> voice number on that SID */
static const uint8_t V_NUM[6] = { 0, 1, 2, 0, 1, 2 };

/* ── Kit tables ──────────────────────────────────────────────────── */
/*
 * Fields: freq_start, freq_end, waveform, AD, SR, sweep_per_tick, ttl
 *
 * ttl = gate-on duration in ISR ticks (at 240Hz):
 *   3 ticks = ~12ms, 10 = ~42ms, 20 = ~83ms, 30 = ~125ms
 *
 * sweep_per_tick: subtracted from freq each tick while freq > freq_end.
 *   For kick at 909: starts ~12000, sweeps by 55/tick -> reaches ~150 in ~215 ticks
 *   But ttl=14 so gate closes at 14 ticks. The SID release handles the tail.
 */

typedef struct {
    uint16_t fstart;
    uint16_t fend;
    uint8_t  wave;
    uint8_t  ad;
    uint8_t  sr;
    uint8_t  sweep;
    uint8_t  ttl;
    uint8_t  wave2;      /* switch to this waveform after wave2_tick (0=no switch) */
    uint8_t  wave2_tick;
    uint16_t freq2;      /* set freq when switching to wave2 (0=keep current) */
} KV;

static const KV KITS[NUM_KITS][NUM_TRACKS] = {
/* ── KIT_909: punchy electronic ─────────────────────────────────── */

{
/*kick */ { 3405,  936, NOISE, 0x03, 0x09,133, 16,  TRI|GATE,  2, 3065},
/*snare*/ {22000,22000, NOISE, 0x01, 0x06,  0,  9,  TRI|GATE,  2, 2724},
/*chh  */ {32000,32000, NOISE, 0x01, 0x01,  0,  2,  0,         0,    0},
/*ohh  */ {32000,32000, NOISE, 0x01, 0x07,  0, 11,  0,         0,    0},
/*tom  */ {16000, 4257, NOISE, 0x02, 0x08, 30, 13,  TRI|GATE,  2, 4257},
/*clap */ {24000,24000, NOISE, 0x01, 0x04,  0,  5,  0,         0,    0},
/*crash*/ {30000,30000, NOISE, 0x01, 0x0C,  0, 24,  0,         0,    0},
},
/* ── KIT_808: deep boomy ─────────────────────────────────────────── */
{
/*kick */ { 2213,  851, TRI,   0x01, 0x0C, 48, 28,  0,         0,    0},
/*snare*/ {14000,14000, NOISE, 0x01, 0x0A,  0, 14,  TRI|GATE,  2, 2724},
/*chh  */ {28000,28000, NOISE, 0x01, 0x01,  0,  2,  0,         0,    0},
/*ohh  */ {28000,28000, NOISE, 0x01, 0x0B,  0, 16,  0,         0,    0},
/*tom  */ { 3065,  851, TRI,   0x01, 0x09, 14, 22,  0,         0,    0},
/*clap */ {18000,18000, NOISE, 0x01, 0x07,  0,  9,  0,         0,    0},
/*crash*/ {24000,24000, NOISE, 0x01, 0x0E,  0, 32,  0,         0,    0},
},
/* ── KIT_ROCK  ───────────── */
{
/*kick */ { 6500,  100, TRI,       0x02, 0x0A, 25, 22,  0,          0,    0},
/*snare*/ {28000,28000, NOISE,     0x01, 0x08,  0, 11,  PULSE|GATE, 1, 3800},
/*chh  */ {20000,20000, NOISE,     0x01, 0x02,  0,  3,  0,          0,    0},
/*ohh  */ {20000,20000, NOISE,     0x01, 0x09,  0, 13,  0,          0,    0},
/*tom  */ {16000, 1396, NOISE,     0x02, 0x09,106, 22,  SAW|GATE,   2, 3746},
/*clap */ {19000,19000, NOISE,     0x01, 0x05,  0,  6,  0,          0,    0},
/*crash*/ {17000,17000, NOISE,     0x02, 0x0E,  0, 38,  0,          0,    0},
},
};

/* ── sid_next_addr: cycle SID2 to next address ──────────────────── */
void sid_next_addr(void)
{
    /* Silence current SID2 before switching address */
    if (g_dual_sid) sid_silence();

    g_sid2_idx = (g_sid2_idx + 1) % g_sid2_num_addrs;
    g_sid2_base = g_sid2_addrs[g_sid2_idx];

    /* Re-init if dual SID is enabled */
    if (g_dual_sid) sid_init();
}

/* ── sid_init ────────────────────────────────────────────────────── */
void sid_init(void)
{
    uint8_t i;

    for (i = 0; i < 25; i++) SID1[i] = 0;
    SID1[SID_VOL_FLT] = 0x0F;

    if (g_dual_sid) {
        for (i = 0; i < 25; i++) SID2[i] = 0;
        SID2[SID_VOL_FLT] = 0x0F;
    }

    for (i = 0; i < 6; i++) {
        g_voices[i].sid    = V_SID[i];
        g_voices[i].voice  = V_NUM[i];
        g_voices[i].freq   = 0;
        g_voices[i].freq_end = 0;
        g_voices[i].sweep  = 0;
        g_voices[i].active = 0;
        g_voices[i].ttl    = 0;
    }
}

/* ── sid_trigger ─────────────────────────────────────────────────── */
/*
 * vel = 0 (silent, should not be called)
 *       1 = soft   (volume 7/15)
 *       2 = medium (volume 11/15)
 *       3 = loud   (volume 15/15, full)
 *
 * Velocity is implemented by temporarily reducing the SID master
 * volume register ($D418/$SID_VOL_FLT) during the trigger frame.
 * The sweep update restores it on the next tick if needed.
 * Simplest approach: write volume before gate-on, restore after.
 */
static const uint8_t VEL_VOL[4] = { 0x0F, 0x06, 0x0B, 0x0F };

void sid_trigger(uint8_t track, uint8_t vel, uint8_t kit)
{
    uint8_t vi, sid, vn;
    const KV *k;
    uint8_t vol;

    if (track >= NUM_TRACKS) return;
    if (vel == 0) return;
    if (vel > 3) vel = 3;

    vi = T2V[track];
    if (vi >= 3) {
        if (!g_dual_sid) vi -= 3;
    }
    sid = V_SID[vi];
    vn  = V_NUM[vi];
    if (sid == 1 && !g_dual_sid) return;

    k   = &KITS[kit][track];
    vol = VEL_VOL[vel];

    /* Set master volume for this SID to match velocity */
    if (sid == 0) SID1[SID_VOL_FLT] = (SID1[SID_VOL_FLT] & 0xF0) | vol;
    else          SID2[SID_VOL_FLT] = (SID2[SID_VOL_FLT] & 0xF0) | vol;

    /* Set pulse width to square ($0800) for PULSE waveform sounds.
     * This gives the richest pulse body for kick/snare. */
    if ((k->wave & PULSE) || (k->wave2 & PULSE)) {
        vw(sid, vn, SID_PW_LO, 0x00);
        vw(sid, vn, SID_PW_HI, 0x08);  /* 50% duty cycle = square wave */
    }

    vw(sid, vn, SID_CTRL, k->wave & ~GATE);  /* waveform, gate off first */
    vw(sid, vn, SID_FREQ_LO, (uint8_t)(k->fstart & 0xFF));
    vw(sid, vn, SID_FREQ_HI, (uint8_t)(k->fstart >> 8));
    vw(sid, vn, SID_AD, k->ad);
    vw(sid, vn, SID_SR, k->sr);
    vw(sid, vn, SID_CTRL, k->wave | GATE);   /* gate on */

    /* Record sweep state */
    g_voices[vi].sid       = sid;
    g_voices[vi].voice     = vn;
    g_voices[vi].freq      = k->fstart;
    g_voices[vi].freq_end  = k->fend;
    g_voices[vi].sweep     = k->sweep;
    g_voices[vi].ttl       = k->ttl;
    g_voices[vi].active    = 1;
    g_voices[vi].wave2      = k->wave2;
    g_voices[vi].wave2_tick = k->wave2_tick;
    g_voices[vi].freq2      = k->freq2;
}

/* ── sid_update_sweeps (called from seq_work ISR) ────────────────── */
void sid_update_sweeps(void)
{
    uint8_t i, sid, vn, r, cur;
    uint16_t f;
    VoiceState *v;

    for (i = 0; i < 6; i++) {
        v = &g_voices[i];
        if (!v->active) continue;

        sid = v->sid;
        vn  = v->voice;

        /* Safety guard */
        if (sid == 1 && !g_dual_sid) { v->active = 0; continue; }

        /* Waveform switch: Hubbard noise-then-pitched technique.
         * At wave2_tick, swap waveform and optionally set new frequency.
         * freq2 != 0 means start the pitched body at a specific frequency.
         * This creates the "stick crack + body resonance" effect. */
        if (v->wave2 && v->wave2_tick > 0) {
            v->wave2_tick--;
            if (v->wave2_tick == 0) {
                /* Switch waveform - set freq2 if specified */
                if (v->freq2 != 0) {
                    r = (uint8_t)(vn * 7u);
                    sid_write(sid, r + SID_FREQ_LO, (uint8_t)(v->freq2 & 0xFF));
                    sid_write(sid, r + SID_FREQ_HI, (uint8_t)(v->freq2 >> 8));
                    v->freq = v->freq2;   /* update sweep start point */
                }
                vw(sid, vn, SID_CTRL, v->wave2);
                v->wave2 = 0;   /* done switching */
            }
        }

        /* Count down gate duration */
        if (v->ttl > 0) {
            v->ttl--;
            if (v->ttl == 0) {
                /* Gate off: clear gate bit */
                r   = (uint8_t)(vn * 7u + SID_CTRL);
                cur = sid_read(sid, r);
                sid_write(sid, r, cur & 0xFE);
                v->active = 0;
                continue;
            }
        }

        /* Pitch sweep */
        if (v->sweep != 0 && v->freq > v->freq_end) {
            f = v->freq;
            if (f > (uint16_t)(v->freq_end + v->sweep))
                f -= v->sweep;
            else
                f = v->freq_end;
            v->freq = f;
            r = (uint8_t)(vn * 7u);
            sid_write(sid, r + SID_FREQ_LO, (uint8_t)(f & 0xFF));
            sid_write(sid, r + SID_FREQ_HI, (uint8_t)(f >> 8));
        }
    }
}

/* ── sid_silence ─────────────────────────────────────────────────── */
void sid_silence(void)
{
    uint8_t i, r;

    for (i = 0; i < 3; i++) {
        r = (uint8_t)(i * 7u);
        SID1[r + SID_CTRL] = 0;
        SID1[r + SID_AD]   = 0;
        SID1[r + SID_SR]   = 0;
        g_voices[i].active = 0;
        g_voices[i].ttl    = 0;
    }

    if (g_dual_sid) {
        for (i = 0; i < 3; i++) {
            r = (uint8_t)(i * 7u);
            SID2[r + SID_CTRL] = 0;
            SID2[r + SID_AD]   = 0;
            SID2[r + SID_SR]   = 0;
            g_voices[i + 3].active = 0;
            g_voices[i + 3].ttl   = 0;
        }
    }
}