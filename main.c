/*
 *
 * Build: oscar64 -o=drumbox.prg -O2 -g \
 *          main.c sid.c seq.c ui.c presets.c -ii=../oscar64/include
 *
 */

#include "drumbox.h"

Pattern g_pattern;

/* Kill CIA2 early via global initializer (runs before main body) */
static uint8_t kill_cia2_early(void)
{
    *(volatile uint8_t *)0x0318 = 0x5E;  /* NMI -> $FE5E (RTI in ROM) */
    *(volatile uint8_t *)0x0319 = 0xFE;
    *(volatile uint8_t *)0xDD0E = 0x08;  /* CIA2 CRA: stop Timer A */
    *(volatile uint8_t *)0xDD0D = 0x7F;  /* CIA2 ICR: disable all NMI */
    (void)(*(volatile uint8_t *)0xDD0D); /* read to clear pending */
    *(volatile uint8_t *)0xDD0F = 0x08;  /* CIA2 CRB: stop Timer B */
    return 0;
}
static uint8_t g_cia2_killed = kill_cia2_early();

int main(void)
{
    uint8_t key;
    uint8_t last_step;

    /* Kill CIA2 again with inline asm - belt and suspenders.
     * NOTE: fixed bug - explicit lda #$08 for CRB, not value from ICR read */
    __asm {
        lda #$5e
        sta $0318
        lda #$fe
        sta $0319
        lda #$08
        sta $dd0e
        lda #$7f
        sta $dd0d
        lda $dd0d
        lda #$08        /* explicit stop value - NOT the ICR read result */
        sta $dd0f
    }

    /* Force VIC-II into correct state IMMEDIATELY.
     * Do this before any C function calls, using direct writes.
     * $D011 = $1B: screen on, 25 rows, text mode, no scroll
     * $D016 = $08: 40 cols, no multicolor
     * $D018 = $15: screen RAM $0400, charset ROM $D000
     * $D020 = $00: black border
     * $D021 = $00: black background */
    *(volatile uint8_t *)0xD011 = 0x1B;
    *(volatile uint8_t *)0xD016 = 0x08;
    *(volatile uint8_t *)0xD018 = 0x15;
    *(volatile uint8_t *)0xD020 = 0x00;
    *(volatile uint8_t *)0xD021 = 0x00;

    /* Silence SID1 */
    {
        volatile uint8_t *s = (volatile uint8_t *)0xD400;
        uint8_t i;
        for (i = 0; i < 25; i++) s[i] = 0;
        s[0x18] = 0x0F;
    }

    g_dual_sid   = 0;
    g_kit        = KIT_909;
    g_tempo      = DEFAULT_TEMPO;
    g_cur_preset = 0;
    g_cur_track  = 0;
    g_cur_col    = 0;

    preset_load(0);
    seq_init();
    ui_init();

    last_step = 0xFF;
    for (;;) {
        /* Read CIA2 ICR at the very top of every iteration.
         * This is the tightest possible polling - ensures we never miss
         * a tick even if the rest of the loop (key handling, draws) is slow.
         * seq_poll() drains s_tick_pending accumulated here. */
        seq_tick_capture();

        seq_poll();

        if (g_tick_flag) {
            g_tick_flag = 0;
            if (g_cur_step != last_step) {
                last_step = g_cur_step;
                ui_draw_playhead(g_cur_step);
            }
        }

        key = ui_read_key();
        if (key) ui_handle_key(key);
        ui_poll_joystick();
    }
    return 0;
}