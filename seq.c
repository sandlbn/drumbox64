/*
 *
 * SWING IMPLEMENTATION
 * --------------------
 * Swing delays every even-numbered step (2,4,6,8...) relative to odd
 * steps. This creates the "loping" feel of classic drum machines.
 *
 * At the tick level:
 *   odd steps  (1,3,5...) fire after s_tps_odd  ticks
 *   even steps (2,4,6...) fire after s_tps_even ticks
 *
 * Both must average to s_tps so the overall tempo stays constant:
 *   s_tps_odd  = s_tps - offset
 *   s_tps_even = s_tps + offset
 *   offset = (s_tps * g_swing) / 100
 *
 * g_swing = 0  -> straight (offset=0, both equal s_tps)
 * g_swing = 50 -> triplet shuffle (3:1 ratio)
 * g_swing = 54 -> classic 808/909 feel (~3.3:1)
 *
 * The step parity tracks g_cur_step: even steps (0,2,4...) in 0-indexed
 * terms are the "and" beats and get the longer duration.
 *
 * CIA2 Timer B fires at 240Hz and is never reloaded mid-pattern.
 * Swing happens entirely in the tick accumulator logic.
 */

#include "drumbox.h"

#define CIA2_TB_LO  (*(volatile uint8_t *)0xDD06)
#define CIA2_TB_HI  (*(volatile uint8_t *)0xDD07)
#define CIA2_ICR    (*(volatile uint8_t *)0xDD0D)
#define CIA2_CRB    (*(volatile uint8_t *)0xDD0F)
#define CIA2_CRA    (*(volatile uint8_t *)0xDD0E)

/* ── Global state ─────────────────────────────────────────────────── */
volatile uint8_t g_cur_step   = 0;
volatile uint8_t g_seq_state  = SEQ_STOPPED;
volatile uint8_t g_tick_flag  = 0;
static volatile uint8_t s_tick_pending = 0;  /* counts unprocessed CIA ticks */

uint16_t g_tempo     = DEFAULT_TEMPO;
uint8_t g_kit        = KIT_909;
uint8_t g_cur_preset = 0;
uint8_t g_cur_track  = 0;
uint8_t g_cur_col    = 0;
uint8_t g_dual_sid   = 0;
uint8_t g_swing      = 0;    /* 0=straight, 54=classic, 99=max */

#ifdef NTSC_TIMING
#define CIA_TIMER_VAL  4261u
#else
#define CIA_TIMER_VAL  4105u
#endif

/* Tick thresholds - recomputed by seq_calc_swing() */
static uint8_t s_tps      = 30;   /* base ticks per step */
static uint8_t s_tps_odd  = 30;   /* ticks for odd steps  (1,3,5...) */
static uint8_t s_tps_even = 30;   /* ticks for even steps (2,4,6...) */
static uint8_t s_step_acc = 0;

static uint8_t bpm_to_ticks(uint16_t bpm)
{
    uint16_t t;
    if (bpm < 40)  bpm = 40;
    if (bpm > 280) bpm = 280;
    t = 3600u / (uint16_t)bpm;
    /* t range: 3600/280=12 (fast) to 3600/40=90 (slow).
     * uint8_t can hold up to 255, so no overflow. No clamping needed. */
    if (t < 10) t = 10;   /* safety floor ~360 BPM */
    return (uint8_t)t;
}

/* Recompute s_tps_odd / s_tps_even from current tempo + swing */
static void seq_calc_swing(void)
{
    uint8_t offset;
    s_tps = bpm_to_ticks(g_tempo);

    if (g_swing == 0) {
        s_tps_odd  = s_tps;
        s_tps_even = s_tps;
    } else {
        /* offset = s_tps * swing% / 100, capped so odd >= 4 */
        offset = (uint8_t)((uint16_t)s_tps * g_swing / 100u);
        if (offset >= s_tps - 4) offset = s_tps - 4;
        s_tps_odd  = s_tps - offset;
        s_tps_even = s_tps + offset;
    }
}

/* ── seq_init ─────────────────────────────────────────────────────── */
void seq_init(void)
{
    g_cur_step  = NUM_STEPS - 1;
    g_seq_state = SEQ_STOPPED;
    g_tick_flag = 0;
    s_step_acc  = 0;
    seq_calc_swing();

    CIA2_CRA = 0x08;          /* stop Timer A */
    CIA2_CRB = 0x08;          /* stop Timer B */
    (void)CIA2_ICR;           /* clear pending flags */

    CIA2_TB_LO = (uint8_t)(CIA_TIMER_VAL & 0xFF);
    CIA2_TB_HI = (uint8_t)(CIA_TIMER_VAL >> 8);
    CIA2_CRB = 0x11;          /* force load latch */
    CIA2_CRB = 0x01;          /* start continuous */
}

/* ── seq_restore_irq ──────────────────────────────────────────────── */
void seq_restore_irq(void)
{
    CIA2_CRB = 0x08;
    (void)CIA2_ICR;
}

/* ── seq_tick_capture ─────────────────────────────────────────────── */
/* Called at the very top of the main loop to capture CIA ticks immediately.
 * Separating this from seq_poll() means ticks are captured even when
 * the rest of the loop (screen draws, key handling) takes a long time.
 * The cap of 8 prevents runaway if something goes very wrong (~33ms). */
void seq_tick_capture(void)
{
    if (CIA2_ICR & 0x02) {
        if (s_tick_pending < 8) s_tick_pending++;
    }
}

/* ── seq_poll ─────────────────────────────────────────────────────── */
void seq_poll(void)
{
    uint8_t track;
    uint8_t threshold;

    /* Also capture here in case seq_poll is called without seq_tick_capture */
    if (CIA2_ICR & 0x02) {
        if (s_tick_pending < 8) s_tick_pending++;
    }

    if (s_tick_pending == 0) return;

    /* Drain ALL pending ticks so we never fall behind.
     * If the main loop was slow and missed ticks, we catch up here.
     * Each tick advances s_step_acc and may fire a step. */
    while (s_tick_pending > 0) {
        s_tick_pending--;

        /* Sweep updates run once per real CIA tick */
        sid_update_sweeps();

        if (g_seq_state != SEQ_PLAYING) continue;

        threshold = ((g_cur_step + 1) & 1) ? s_tps_even : s_tps_odd;

        s_step_acc++;
        if (s_step_acc < threshold) continue;
        s_step_acc = 0;

        g_cur_step = (g_cur_step + 1) & (NUM_STEPS - 1);

        /* Trigger all tracks. CHH (track 2) yields to OHH (track 3)
         * since they share a voice - if OHH fires, skip CHH. */
        {
            uint8_t ohh_firing = g_pattern.steps[TRK_OHH][g_cur_step] > 0;
            for (track = 0; track < NUM_TRACKS; track++) {
                uint8_t vel = g_pattern.steps[track][g_cur_step];
                if (!vel) continue;
                if (track == TRK_CHH && ohh_firing) continue;
                sid_trigger(track, vel, g_kit);
            }
        }

        g_tick_flag = 1;
    }
}

/* ── Public API ───────────────────────────────────────────────────── */
void seq_start(void)
{
    s_step_acc  = 0;
    g_cur_step  = NUM_STEPS - 1;
    g_tick_flag = 0;
    g_seq_state = SEQ_PLAYING;
}

void seq_stop(void)
{
    g_seq_state = SEQ_STOPPED;
    sid_silence();
}

void seq_set_tempo(uint16_t bpm)
{
    if (bpm < 40)  bpm = 40;
    if (bpm > 280) bpm = 280;
    g_tempo = bpm;
    seq_calc_swing();
}

void seq_set_swing(uint8_t pct)
{
    if (pct > 99) pct = 99;
    g_swing = pct;
    seq_calc_swing();
}

void seq_tick(void) {}