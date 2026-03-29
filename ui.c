#include "drumbox.h"
#include <conio.h>

#define SCR  ((volatile uint8_t *)0x0400)
#define CLR  ((volatile uint8_t *)0xD800)

/* Screen write helpers - sput is raw (already a screen code or digit/punct) */
static void sput(uint8_t row, uint8_t col, uint8_t ch, uint8_t color)
{
    uint16_t o = (uint16_t)row * 40u + col;
    SCR[o] = ch;
    CLR[o] = color;
}

/* sputs is defined later after a2s() - forward declaration not needed in C */

static void sfill(uint8_t row, uint8_t col, uint8_t len, uint8_t ch, uint8_t color)
{
    uint8_t i;
    for (i = 0; i < len; i++)
        sput(row, (uint8_t)(col + i), ch, color);
}

/* Print 3-digit decimal - digits 0-9 are same in both ASCII and screen codes */
static void snum(uint8_t row, uint8_t col, uint8_t v, uint8_t color)
{
    sput(row, col,     (v >= 100) ? (uint8_t)('0' + v / 100) : (uint8_t)' ', color);
    sput(row, col + 1, (uint8_t)('0' + (v % 100) / 10), color);
    sput(row, col + 2, (uint8_t)('0' + v % 10), color);
}

/* Print 3-digit decimal for uint16_t (e.g. BPM up to 280) */
static void snum16(uint8_t row, uint8_t col, uint16_t v, uint8_t color)
{
    sput(row, col,     (v >= 100) ? (uint8_t)('0' + (v / 100) % 10) : (uint8_t)' ', color);
    sput(row, col + 1, (uint8_t)('0' + (v % 100) / 10), color);
    sput(row, col + 2, (uint8_t)('0' + v % 10), color);
}

/* Print 2-digit decimal with leading zero */
static void snum2(uint8_t row, uint8_t col, uint8_t v, uint8_t color)
{
    sput(row, col,     (uint8_t)('0' + v / 10), color);
    sput(row, col + 1, (uint8_t)('0' + v % 10), color);
}

/* ── Color palette ───────────────────────────────────────────────── */
#define CW   1   /* white      */
#define CR   2   /* red        */
#define CCY  3   /* cyan       */
#define CYL  7   /* yellow     */
#define CDG 11   /* dark gray  */
#define CLB 14   /* light blue */

/* ASCII to C64 screen code conversion */
static uint8_t a2s(uint8_t c)
{
    if (c >= 'A' && c <= 'Z') return (uint8_t)(c - 64u);
    if (c >= 'a' && c <= 'z') return (uint8_t)(c - 96u);
    if (c == '|') return 0x5Du;
    if (c == '[') return 0x1Bu;
    if (c == ']') return 0x1Du;
    if (c == '@') return 0u;
    return c;
}

/* Write string with screen code conversion */
static void sputs(uint8_t row, uint8_t col, const char *s, uint8_t color)
{
    while (*s && col < 40u)
        sput(row, col++, a2s((uint8_t)*s++), color);
}

/* ── Labels ──────────────────────────────────────────────────────── */
static const char * const TLAB[NUM_TRACKS] = {
    "KICK  ", "SNARE ", "C.HAT ",
    "O.HAT ", "TOM   ", "CLAP  ", "CRASH "
};
static const char * const KLAB[NUM_KITS] = { "909", "808", "ROK", "SID" };

/* ── Grid constants ──────────────────────────────────────────────── */
#define GROW  4    /* first grid row on screen */
#define GCOL  7    /* first step column */
#define GSPC  2    /* chars per step */

/* ── Keyboard reading via Oscar64 conio ─────────────────────────── */
/*
 * kbhit() checks if a key is in the buffer (non-blocking).
 * getch() reads one key from the buffer.
 * Both are from Oscar64's <conio.h> and work correctly with the
 * C64 KERNAL keyboard buffer filled by the CIA1 60Hz IRQ.
 *
 * Key codes returned by getch() are PETSCII:
 *   CRSR DOWN=17, UP=145, RIGHT=29, LEFT=157
 *   F1=133, F3=134, F5=135
 *   Space=32, letters=65-90 (uppercase)
 */
uint8_t ui_read_key(void)
{
    if (kbhit())
        return (uint8_t)getch();
    return 0;
}

/* ── Joystick (port 2, $DC00) ────────────────────────────────────── */
/*
 * Edit mode: hold fire for EDIT_HOLD polls to enter.
 * In edit mode:
 *   JOY LEFT/RIGHT = swing ±4
 *   JOY UP/DOWN    = velocity of current step ±1
 *   FIRE (new press) = exit edit mode
 *   Auto-exit after EDIT_TIMEOUT polls of no input
 *
 * Normal mode: short fire press = cycle velocity as before.
 */
#define JOY2  (*(volatile uint8_t *)0xDC00)

#define JOY_UP    0x01
#define JOY_DOWN  0x02
#define JOY_LEFT  0x04
#define JOY_RIGHT 0x08
#define JOY_FIRE  0x10

#define JOY_DELAY    20   /* autorepeat start (normal mode) */
#define JOY_RATE      6   /* autorepeat rate  (normal mode) */
#define EDIT_HOLD   180   /* polls to hold fire = ~1 second before edit mode */
#define EDIT_REPEAT   8   /* autorepeat rate in edit mode */
#define EDIT_TIMEOUT 30000  /* auto-exit after ~30 seconds of no joystick input */

/* Global edit mode flag - also used by ui_draw_grid to dim labels */
uint8_t g_edit_mode = 0;
static uint8_t s_edit_row = 0;  /* 0=swing, 1=velocity */

/* Copy/paste clipboard */
static uint8_t s_clip_steps[NUM_TRACKS][NUM_STEPS]; /* copied step data */
static uint8_t s_clip_tracks = 0; /* NUM_TRACKS=full pattern, 1=single track */
static uint8_t s_clip_valid  = 0; /* 1 if clipboard has data */

/* Single edit scratch buffer (~113 bytes).
 * Saves steps for ONE preset slot so edits survive N/B navigation.
 * When navigating away from an edited slot, steps are saved here.
 * Navigating back restores them. Editing a second slot should evicts the first. */
static uint8_t s_scratch_steps[NUM_TRACKS][NUM_STEPS];
static uint8_t s_scratch_slot = 0xFF; /* 0xFF = empty */

static void ui_draw_edit_overlay_sel(uint8_t sel)
{
    uint8_t val = g_pattern.steps[g_cur_track][g_cur_col];
    uint8_t i;

    /* Rows 14-17: replace help text with edit overlay */

    /* Row 14: header */
    sfill(14, 0, 40, 0x20, 0);
    sputs(14, 0, "-- EDIT MODE --", CYL);
    sputs(14, 17, "F7/FIRE:EXIT", CR);

    /* Row 15: swing bar */
    {
        uint8_t filled = (uint8_t)(g_swing * 20 / 100);
        uint8_t act = (sel == 0);
        sfill(15, 0, 40, 0x20, 0);
        sputs(15, 0, act ? ">>" : "  ", act ? CYL : CDG);
        sputs(15, 3, "SW", act ? CW : CDG);
        sput(15, 5, ':', CDG);
        sput(15, 6, '[', CDG);
        for (i = 0; i < 18; i++)
            sput(15, (uint8_t)(7+i), i < filled ? 0x60 : 0x2E,
                 i < filled ? (act ? CYL : CDG) : 11);
        sput(15, 25, ']', CDG);
        snum2(15, 27, g_swing, act ? CCY : CDG);
        sputs(15, 30,
              g_swing == 0  ? "STRT" :
              g_swing < 30  ? "LITE" :
              g_swing < 60  ? "CLSC" : "HEVY",
              act ? CCY : CDG);
        if (act) sputs(15, 36, "<>", CYL);
    }

    /* Row 16: velocity bar */
    {
        static const char * const vlbl[] = { "OFF ", "SOFT", "MED ", "LOUD" };
        static const uint8_t vcol[] = { CDG, CCY, CYL, CW };
        uint8_t filled = (uint8_t)(val * 6);
        uint8_t act = (sel == 1);
        sfill(16, 0, 40, 0x20, 0);
        sputs(16, 0, act ? ">>" : "  ", act ? CYL : CDG);
        sputs(16, 3, "VL", act ? CW : CDG);
        sput(16, 5, ':', CDG);
        sput(16, 6, '[', CDG);
        for (i = 0; i < 18; i++)
            sput(16, (uint8_t)(7+i), i < filled ? 0x60 : 0x2E,
                 i < filled ? (act ? vcol[val] : CDG) : 11);
        sput(16, 25, ']', CDG);
        sput(16, 27, (uint8_t)('0'+val), act ? vcol[val] : CDG);
        sput(16, 29, ' ', 0);
        sputs(16, 30, vlbl[val], act ? vcol[val] : CDG);
        if (act) sputs(16, 36, "<>", CYL);
    }

    /* Row 17: hint */
    sfill(17, 0, 40, 0x20, 0);
    sputs(17, 4, "UP/DN:SWITCH  LFT/RGT:ADJUST", CDG);

    /* Clear rows 18-21 that may have old help text */
    sfill(18, 0, 40, 0x20, 0);
    sfill(19, 0, 40, 0x20, 0);
    sfill(20, 0, 40, 0x20, 0);
    sfill(21, 0, 40, 0x20, 0);
}

static void ui_draw_edit_overlay(void)
{
    ui_draw_edit_overlay_sel(0);
}


static void ui_exit_edit(void)
{
    g_edit_mode = 0;
    /* Restore separators and full help text */
    sfill(11, 0, 40, 0x43, CDG);
    sfill(13, 0, 40, 0x43, CDG);
    ui_draw_full();   /* redraws everything including help rows */
}

void ui_poll_joystick(void)
{
    static uint8_t  s_prev        = 0xFF;
    static uint8_t  s_held        = 0;
    static uint8_t  s_count       = 0;
    static uint8_t  s_fire_hold   = 0;

    uint8_t raw, pressed, key;

    /* ── F7 key toggle for edit mode ────────────────────────────────
     * F7 = PETSCII 136. Function keys never auto-repeat on C64.
     * We read directly from the KERNAL keyboard buffer at $0277.
     * $C6 = number of keys in buffer. We scan for F7 and consume it.
     * This works identically on real hardware and VICE.
     */
    {
        uint8_t n   = *(volatile uint8_t *)0x00C6;
        uint8_t i;
        volatile uint8_t *buf = (volatile uint8_t *)0x0277;
        for (i = 0; i < n; i++) {
            if (buf[i] == 136) {       /* F7 = PETSCII 136 */
                /* Remove this key from buffer by shifting remaining down */
                uint8_t j;
                for (j = i; j < n - 1; j++) buf[j] = buf[j+1];
                *(volatile uint8_t *)0x00C6 = n - 1;
                /* Toggle edit mode */
                if (g_edit_mode) {
                    ui_exit_edit();
                } else {
                    g_edit_mode  = 1;
                    s_edit_row   = 0;
                    s_fire_hold  = 0;
                    s_held = 0; s_count = 0;
                    s_prev = (~(JOY2 & 0x1F)) & 0x1F;
                    ui_draw_edit_overlay_sel(s_edit_row);
                }
                break;
            }
        }
    }

    raw     = JOY2 & 0x1F;
    pressed = (~raw) & 0x1F;

    /* ── EDIT MODE ──────────────────────────────────────────────── */
    if (g_edit_mode) {
        uint8_t changed = 0;

        /* Fire: exit edit mode on a fresh press only */
        if ((pressed & JOY_FIRE) && !(s_prev & JOY_FIRE)) {
            s_prev = pressed;
            ui_exit_edit();
            s_fire_hold = 0;
            return;
        }

        /* UP/DOWN: switch active row (swing vs velocity) */
        /* LEFT/RIGHT: adjust active row's value */
        key = 0;
        if      (pressed & JOY_LEFT)  key = 1;   /* adjust down */
        else if (pressed & JOY_RIGHT) key = 2;   /* adjust up   */
        else if (pressed & JOY_UP)    key = 3;   /* switch row  */
        else if (pressed & JOY_DOWN)  key = 4;   /* switch row  */

        if (key) {
            uint8_t act = 0;
            if (key != s_held) {
                s_held = key; s_count = 0; act = 1;
            } else {
                s_count++;
                /* UP/DOWN: no autorepeat - only fire once per press */
                if ((key == 3 || key == 4)) {
                    act = 0;
                } else if (s_count == JOY_DELAY ||
                   (s_count > JOY_DELAY &&
                    ((s_count - JOY_DELAY) % EDIT_REPEAT) == 0)) {
                    act = 1;
                }
            }

            if (act) {
                if (key == 3 || key == 4) {
                    /* UP/DOWN: toggle active row */
                    s_edit_row ^= 1;
                    changed = 1;
                } else if (key == 1) {
                    /* LEFT: decrease active value */
                    if (s_edit_row == 0) {
                        seq_set_swing(g_swing >= 4 ? (uint8_t)(g_swing-4) : 0);
                        changed = 1;
                    } else {
                        uint8_t v = g_pattern.steps[g_cur_track][g_cur_col];
                        if (v > 0) { g_pattern.steps[g_cur_track][g_cur_col] = v-1; changed = 1; }
                    }
                } else if (key == 2) {
                    /* RIGHT: increase active value */
                    if (s_edit_row == 0) {
                        seq_set_swing(g_swing <= 95 ? (uint8_t)(g_swing+4) : 99);
                        changed = 1;
                    } else {
                        uint8_t v = g_pattern.steps[g_cur_track][g_cur_col];
                        if (v < 3) { g_pattern.steps[g_cur_track][g_cur_col] = v+1; changed = 1; }
                    }
                }
                if (changed) {
                    ui_draw_status();
                    ui_draw_edit_overlay_sel(s_edit_row);
                }
            }
        } else {
            s_held = 0; s_count = 0;
            /* No timeout - stay in edit mode until explicit exit */
        }

        s_prev = pressed;
        return;
    }

    /* ── NORMAL MODE ────────────────────────────────────────────── */

    /* Track fire hold duration */
    if (pressed & JOY_FIRE) {
        s_fire_hold++;

        /* Show hold progress in status bar: fills a bar as you hold.
         * Updates every 10 polls to avoid flickering. */
        if ((s_fire_hold % 10) == 0) {
            uint8_t progress = (uint8_t)(s_fire_hold * 8 / EDIT_HOLD);
            uint8_t i;
            sfill(24, 31, 9, 0x20, CLB);
            sputs(24, 31, "HOLD:", CLB);
            for (i = 0; i < 8; i++)
                sput(24, (uint8_t)(36+i), i < progress ? 0x60 : 0x2E,
                     i < progress ? CYL : CDG);
        }

        if (s_fire_hold >= EDIT_HOLD) {
            /* Enter edit mode — restore status bar first */
            sfill(24, 31, 9, 0x20, CLB);
            g_edit_mode  = 1;
            s_fire_hold  = 0;
            s_held = 0; s_count = 0;
            s_prev = pressed;
            ui_draw_edit_overlay_sel(s_edit_row);
            return;
        }
    } else {
        /* Fire released - clear progress bar */
        if (s_fire_hold > 0)
            sfill(24, 31, 9, 0x20, CLB);

        if (s_fire_hold > 0 && s_fire_hold < EDIT_HOLD) {
            /* Short press: cycle velocity in place, never auto-advance */
            uint8_t v = g_pattern.steps[g_cur_track][g_cur_col];
            if (v == 0)      g_pattern.steps[g_cur_track][g_cur_col] = 3;
            else if (v == 3) g_pattern.steps[g_cur_track][g_cur_col] = 2;
            else if (v == 2) g_pattern.steps[g_cur_track][g_cur_col] = 1;
            else             g_pattern.steps[g_cur_track][g_cur_col] = 0;
            ui_draw_grid();
        }
        s_fire_hold = 0;
    }

    /* Directions (normal mode: move cursor) */
    key = 0;
    if      (pressed & JOY_UP)    key = 145;
    else if (pressed & JOY_DOWN)  key = 17;
    else if (pressed & JOY_LEFT)  key = 157;
    else if (pressed & JOY_RIGHT) key = 29;

    if (key) {
        if (key != s_held) {
            s_held = key; s_count = 0;
            ui_handle_key(key);
        } else {
            s_count++;
            if (s_count == JOY_DELAY ||
               (s_count > JOY_DELAY && ((s_count-JOY_DELAY) % JOY_RATE) == 0))
                ui_handle_key(key);
        }
    } else {
        s_held = 0; s_count = 0;
    }

    s_prev = pressed;
}

/* ── Screen clear ────────────────────────────────────────────────── */
static void sclear(void)
{
    uint16_t i;
    for (i = 0; i < 1000u; i++) {
        SCR[i] = 0x20;   /* space */
        CLR[i] = CDG;
    }
}

/* ── ui_init ─────────────────────────────────────────────────────── */
void ui_init(void)
{
    /* Ensure VIC-II screen is enabled and in correct mode.
     * After crashes/resets, $D011 bit4 (screen enable) may be clear.
     * $D011 = %00011011 = normal text mode, 25 rows, screen on.
     * $D016 = %00001000 = normal 40-col mode, no multicolor. */
    *(volatile uint8_t *)0xD011 = 0x1B;
    *(volatile uint8_t *)0xD016 = 0x08;
    *(volatile uint8_t *)0xD018 = 0x15;   /* screen $0400, charset $D000 (ROM) */
    *(volatile uint8_t *)0xD020 = 0;       /* black border */
    *(volatile uint8_t *)0xD021 = 0;       /* black background */
    sclear();
    ui_draw_full();
}

/* ── ui_draw_full ────────────────────────────────────────────────── */
void ui_draw_full(void)
{
    uint8_t i, col, d;

    /* Row 0: title centered */
    sfill(0, 0, 40, 0x20, 0);
    sputs(0, 6, "** DRUMBOX 64 BY SANDLBN **", CW);

    /* Row 3: step header - beat groups colored differently */
    sfill(3, 0, 40, 0x20, 0);
    sputs(3, 0, "TRACK ", CYL);
    sput(3, 6, 0x5D, CDG);
    for (i = 0; i < NUM_STEPS; i++) {
        col = (uint8_t)(GCOL + i * GSPC);
        d   = (i < 9) ? (uint8_t)('1' + i) : (uint8_t)('0' + (i - 9));
        /* Beat 1 = white, beat 2/3/4 = cyan, off-beats = dark */
        sput(3, col, d, ((i & 3) == 0) ? CW : ((i & 1) == 0) ? CCY : CDG);
    }

    /* Row 11: separator */
    sfill(11, 0, 40, 0x43, CDG);

    /* Row 12: parameter bar - drawn by ui_draw_param_bar() */
    /* (called separately so it can update without full redraw) */

    /* Row 13: separator */
    sfill(13, 0, 40, 0x43, CDG);

    /* Rows 14-20: help with colored key labels
     * Format: [KEY] action  - key in cyan/yellow, action in light blue */
    sfill(14, 0, 40, 0x20, 0);
    sputs(14, 0, "CONTROLS:", CW);

    /* Row 15: movement + toggle */
    sfill(15, 0, 40, 0x20, 0);
    sputs(15,  0, "CRSR", CYL);  sputs(15,  4, ":MOVE  ", CDG);
    sputs(15, 11, "SPC", CYL);   sputs(15, 14, ":VELOCITY ", CDG);
    sputs(15, 25, "F7", CCY);    sputs(15, 27, ":EDIT MODE", CDG);

    /* Row 16: transport */
    sfill(16, 0, 40, 0x20, 0);
    sputs(16,  0, "P", CYL);  sputs(16,  1, ":PLAY/STOP ", CDG);
    sputs(16, 13, "N", CYL);  sputs(16, 14, ":NEXT  ", CDG);
    sputs(16, 22, "B", CYL);  sputs(16, 23, ":PREV", CDG);

    /* Row 17: tempo + pattern */
    sfill(17, 0, 40, 0x20, 0);
    sputs(17,  0, "+/-", CYL); sputs(17,  3, ":TEMPO  ", CDG);
    sputs(17, 11, "</>", CYL); sputs(17, 14, ":SWING  ", CDG);
    sputs(17, 22, "C", CYL);   sputs(17, 23, ":CLR ", CDG);
    sputs(17, 28, "R", CYL);   sputs(17, 29, ":RLD*", CDG);

    /* Row 18: kits (all 4 fit at cols 0,8,16,24) */
    sfill(18, 0, 40, 0x20, 0);
    sputs(18,  0, "F1", CYL);  sputs(18,  2, ":909 ", CDG);
    sputs(18,  8, "F3", CYL);  sputs(18, 10, ":808 ", CDG);
    sputs(18, 16, "F5", CYL);  sputs(18, 18, ":ROCK", CDG);
    sputs(18, 24, "F6", CYL);  sputs(18, 26, ":SID ", CDG);
    sputs(18, 32, "Q", CR);    sputs(18, 33, ":QUIT", CDG);

    /* Row 19: SID2 */
    sfill(19, 0, 40, 0x20, 0);
    sputs(19,  0, "2", CYL);   sputs(19,  1, ":SID2 ON/OFF  ", CDG);
    sputs(19, 15, "3", CYL);   sputs(19, 16, ":SID2 ADDR", CDG);

    /* Row 19b - we reuse row 21 which is blank for copy/paste help */
    sfill(21, 0, 40, 0x20, 0);
    sputs(21,  0, "T", CYL);   sputs(21,  1, ":CPY TRACK  ", CDG);
    sputs(21, 13, "Y", CYL);   sputs(21, 14, ":CPY PTRN  ", CDG);
    sputs(21, 25, "V", CYL);   sputs(21, 26, ":PASTE", CDG);
    /* Row 21b: explain RLD* - N/B browse keeps steps, R reloads from ROM */

    /* Row 20: disk */
    sfill(20, 0, 40, 0x20, 0);
    sputs(20,  0, "W", CCY);   sputs(20,  1, ":SAVE  ", CDG);
    sputs(20,  8, "L", CCY);   sputs(20,  9, ":LOAD  ", CDG);
    sputs(20, 16, "[", CCY);   sputs(20, 17, ":SLT-  ", CDG);
    sputs(20, 24, "]", CCY);   sputs(20, 25, ":SLT+  ", CDG);
    sputs(20, 32, "D", CCY);   sputs(20, 33, ":DRV", CDG);

    /* Row 22: double separator */
    sfill(22, 0, 40, '=', CLB);

    ui_draw_status();
    ui_draw_grid();
}

/* ── ui_draw_status ──────────────────────────────────────────────── */
void ui_draw_status(void)
{
    uint8_t nb[17];
    uint8_t i;

    /* Row 1: kit / bpm / swing / state */
    sfill(1, 0, 40, 0x20, CLB);
    sputs(1,  0, "KIT:", CLB);
    sputs(1,  4, KLAB[g_kit], CCY);
    sputs(1,  8, "BPM:", CLB);
    snum16(1, 12, g_tempo, CCY);
    sputs(1, 16, "SW:", CLB);
    snum2(1, 19, g_swing, (g_swing > 0) ? CCY : CDG);
    sputs(1, 21, (g_seq_state == SEQ_PLAYING) ? "** PLAY **" : "- STOP -  ",
                  (g_seq_state == SEQ_PLAYING) ? CYL : CR);
    /* SID2 status */
    if (g_dual_sid) {
        static const char * const albl[] = {"DE00","DF00","D500","D420"};
        sfill(1, 32, 8, 0x20, CDG);
        sputs(1, 32, "S2:", CCY);
        sputs(1, 35, albl[g_sid2_idx], CCY);
    } else {
        sputs(1, 32, "S2:OFF  ", CDG);
    }

    /* Row 2: preset + clipboard indicator */
    sfill(2, 0, 40, 0x20, CLB);
    sputs(2, 0, "PRESET:", CLB);
    snum2(2, 7, g_cur_preset, CW);
    sput(2, 9, '/', CDG);
    snum2(2, 10, g_num_presets, CDG);
    preset_get_name(g_cur_preset, nb);
    for (i = 0; i < 16; i++) sput(2, (uint8_t)(13 + i), a2s(nb[i]), CW);
    /* Clipboard indicator at col 30 - always visible */
    if (s_clip_valid) {
        sputs(2, 30, s_clip_tracks == 1 ? "[CPY:TRK]" : "[CPY:PAT]", CYL);
    }

    /* Row 24: cursor + disk slot + drive */
    sfill(24, 0, 40, 0x20, CLB);
    sputs(24, 0, "TRACK:", CLB);
    sputs(24, 6, TLAB[g_cur_track], CW);
    sputs(24, 13, "ST:", CLB);
    snum2(24, 16, (uint8_t)(g_cur_col + 1), CW);
    sputs(24, 19, "SL:", CLB);
    sput(24, 22, (uint8_t)('0' + g_disk_slot), CW);
    sputs(24, 24, "DRV:", CLB);
    snum2(24, 28, g_disk_dev, CW);
    sputs(24, 31, "W:SAV L:LOD", CDG);
}

/* ── ui_draw_param_bar ───────────────────────────────────────────── */
/*
 * Row 12: context-sensitive parameter bar.
 *
 * If current step is OFF (0): shows swing as a horizontal bar.
 *   SW [||||||||........] 54
 *
 * If current step is ON (1-3): shows step velocity as a bar.
 *   VEL [|||.............] 2  SOFT / MED / LOUD
 *   Fire cycles velocity. </> adjusts it.
 */
void ui_draw_param_bar(void)
{
    uint8_t val = g_pattern.steps[g_cur_track][g_cur_col];
    uint8_t i;

    sfill(12, 0, 40, 0x20, 0);

    if (val == 0) {
        /* Swing mode */
        uint8_t filled = (uint8_t)(g_swing * 20 / 100);
        sputs(12, 0, "SW", CYL);
        sput(12, 2, ':', CDG);
        sput(12, 3, '[', CDG);
        for (i = 0; i < 20; i++)
            sput(12, (uint8_t)(4 + i), i < filled ? 0x60 : 0x2E,
                 i < filled ? CCY : 11);
        sput(12, 24, ']', CDG);
        snum2(12, 26, g_swing, (g_swing > 0) ? CCY : CDG);
        sputs(12, 29, g_swing == 0 ? "STRAIGHT" :
                      g_swing < 30 ? "LIGHT   " :
                      g_swing < 60 ? "CLASSIC " : "HEAVY   ", CDG);
    } else {
        /* Velocity mode */
        static const char * const vlbl[] = { "", "SOFT", "MED ", "LOUD" };
        static const uint8_t vcol[] = { 0, CCY, CYL, CW };
        uint8_t filled = (uint8_t)(val * 6);  /* 6, 12, 18 of 20 */
        sputs(12, 0, "VL", CYL);
        sput(12, 2, ':', CDG);
        sput(12, 3, '[', CDG);
        for (i = 0; i < 20; i++)
            sput(12, (uint8_t)(4 + i), i < filled ? 0x60 : 0x2E,
                 i < filled ? vcol[val] : 11);
        sput(12, 24, ']', CDG);
        sput(12, 26, (uint8_t)('0' + val), vcol[val]);
        sput(12, 28, ' ', 0);
        sputs(12, 29, vlbl[val], vcol[val]);
        sputs(12, 34, "<>:ADJ", CDG);
    }
}

/* ── ui_draw_grid ────────────────────────────────────────────────── */
void ui_draw_grid(void)
{
    uint8_t t, s, row, col, val, ch, co;
    uint8_t is_cur, is_play, is_beat1;

    /* Velocity colors: off=dark, soft=cyan, medium=yellow, loud=white */
    static const uint8_t VCOL[4] = { CDG, CCY, CYL, CW };
    /* Velocity chars: off=dot, soft=asterisk, medium=O, loud=ball */
    static const uint8_t VCH[4]  = { 0x2E, 0x2A, 0x4F, 0x51 };

    for (t = 0; t < NUM_TRACKS; t++) {
        row = (uint8_t)(GROW + t);
        sputs(row, 0, TLAB[t], (t == g_cur_track) ? CYL : CLB);
        sput(row, 6, 0x5D, CDG);

        for (s = 0; s < NUM_STEPS; s++) {
            col     = (uint8_t)(GCOL + s * GSPC);
            val     = g_pattern.steps[t][s];   /* 0-3 */
            is_cur  = (t == g_cur_track && s == g_cur_col) ? 1 : 0;
            is_play = (g_seq_state == SEQ_PLAYING && s == g_cur_step) ? 1 : 0;
            is_beat1 = ((s & 3) == 0) ? 1 : 0;

            if (is_cur && is_play) {
                ch = val ? VCH[val] : 0x1B;
                co = 8;   /* orange */
            } else if (is_cur) {
                ch = val ? VCH[val] : 0x1B;
                co = CYL;
            } else if (is_play) {
                ch = val ? VCH[val] : 0x2B;
                co = val ? 5 : 10;   /* green / light red */
            } else {
                ch = val ? VCH[val] : 0x2E;
                co = val ? VCOL[val] : (is_beat1 ? CDG : 11);
            }
            sput(row, col, ch, co);
            sput(row, (uint8_t)(col + 1), 0x20, 0);
        }
    }
    ui_draw_param_bar();
}

void ui_draw_playhead(uint8_t step)
{
    (void)step;
    ui_draw_grid();
    /* Edit overlay is rows 14-17, grid is rows 4-10. No conflict. */
}

/* ── ui_handle_key ───────────────────────────────────────────────── */
/* Flash a short message on row 24 right side (cols 20-39) */
static void ui_flash_msg(const char *msg, uint8_t col)
{
    uint8_t i;
    sfill(24, 20, 20, 0x20, CLB);
    sputs(24, col, msg, CYL);
}

void ui_handle_key(uint8_t key)
{
    uint8_t t, s;

    /* In edit mode: only allow exit (F7) and parameter keys.
     * All other keys are ignored so the overlay stays stable. */
    if (g_edit_mode) {
        switch (key) {
        case 136:  /* F7 - also handled in joystick poll, belt+suspenders */
            ui_exit_edit(); break;
        case '<': case ',': {
            uint8_t v = g_pattern.steps[g_cur_track][g_cur_col];
            if (v > 0) { if (v > 1) g_pattern.steps[g_cur_track][g_cur_col] = v-1; }
            else { seq_set_swing(g_swing >= 4 ? (uint8_t)(g_swing-4) : 0); ui_draw_status(); }
            ui_draw_edit_overlay_sel(s_edit_row); break;
        }
        case '>': case '.': {
            uint8_t v = g_pattern.steps[g_cur_track][g_cur_col];
            if (v > 0) { if (v < 3) g_pattern.steps[g_cur_track][g_cur_col] = v+1; }
            else { seq_set_swing(g_swing <= 95 ? (uint8_t)(g_swing+4) : 99); ui_draw_status(); }
            ui_draw_edit_overlay_sel(s_edit_row); break;
        }
        }
        return;   /* ignore everything else */
    }

    switch (key) {

    case 17:  /* cursor down */
        if (g_cur_track < NUM_TRACKS - 1) g_cur_track++;
        ui_draw_grid(); ui_draw_status(); break;

    case 145: /* cursor up */
        if (g_cur_track > 0) g_cur_track--;
        ui_draw_grid(); ui_draw_status(); break;

    case 29:  /* cursor right */
        g_cur_col = (g_cur_col < NUM_STEPS - 1) ? g_cur_col + 1 : 0;
        ui_draw_grid(); ui_draw_status(); break;

    case 157: /* cursor left */
        g_cur_col = (g_cur_col > 0) ? g_cur_col - 1 : NUM_STEPS - 1;
        ui_draw_grid(); ui_draw_status(); break;

    case 0x20: /* space: cycle velocity 0->3->0, never auto-advance */
        {
            uint8_t v = g_pattern.steps[g_cur_track][g_cur_col];
            /* 0 (off) -> 3 (loud) -> 2 (medium) -> 1 (soft) -> 0 (off) */
            if (v == 0)       g_pattern.steps[g_cur_track][g_cur_col] = 3;
            else if (v == 3)  g_pattern.steps[g_cur_track][g_cur_col] = 2;
            else if (v == 2)  g_pattern.steps[g_cur_track][g_cur_col] = 1;
            else              g_pattern.steps[g_cur_track][g_cur_col] = 0;
        }
        ui_draw_grid(); break;

    case 'P': case 'p':
        if (g_seq_state == SEQ_PLAYING) { seq_stop();  ui_draw_grid(); }
        else                            { seq_start(); }
        ui_draw_status(); break;

    case 'S': case 's':
        if (g_seq_state == SEQ_PLAYING) { seq_stop(); ui_draw_grid(); ui_draw_status(); }
        break;

    case '+': case '=':
        if (g_tempo < 280) seq_set_tempo((uint16_t)(g_tempo + 2));
        ui_draw_status(); break;
    case '-':
        if (g_tempo > 40)  seq_set_tempo((uint16_t)(g_tempo - 2));
        ui_draw_status(); break;

    case '<': case ',': {  /* decrease swing or velocity */
        uint8_t v = g_pattern.steps[g_cur_track][g_cur_col];
        if (v > 0) { if (v > 1) g_pattern.steps[g_cur_track][g_cur_col] = v-1; }
        else { seq_set_swing(g_swing >= 4 ? (uint8_t)(g_swing-4) : 0); ui_draw_status(); }
        ui_draw_grid(); break;
    }
    case '>': case '.': {  /* increase swing or velocity */
        uint8_t v = g_pattern.steps[g_cur_track][g_cur_col];
        if (v > 0) { if (v < 3) g_pattern.steps[g_cur_track][g_cur_col] = v+1; }
        else { seq_set_swing(g_swing <= 95 ? (uint8_t)(g_swing+4) : 99); ui_draw_status(); }
        ui_draw_grid(); break;
    }

    case 'N': case 'n':
    case 'B': case 'b': {
        /* Save current steps to scratch if they differ from ROM preset */
        uint8_t from = g_cur_preset;
        uint8_t to;
        uint8_t ti2, si2, differs = 0;
        const Pattern *rom;

        if (key == 'N' || key == 'n')
            to = (from < (uint8_t)(g_num_presets-1)) ? from+1 : 0;
        else
            to = (from > 0) ? from-1 : (uint8_t)(g_num_presets-1);

        /* Check if current steps differ from ROM */
        rom = &g_presets[from];
        for (ti2 = 0; ti2 < NUM_TRACKS && !differs; ti2++)
            for (si2 = 0; si2 < NUM_STEPS && !differs; si2++)
                if (g_pattern.steps[ti2][si2] != rom->steps[ti2][si2])
                    differs = 1;

        if (differs) {
            /* Save to scratch, tagging which slot these belong to */
            for (ti2 = 0; ti2 < NUM_TRACKS; ti2++)
                for (si2 = 0; si2 < NUM_STEPS; si2++)
                    s_scratch_steps[ti2][si2] = g_pattern.steps[ti2][si2];
            s_scratch_slot = from;
        }

        g_cur_preset = to;

        /* Load destination - from scratch if we saved it, else from ROM */
        if (s_scratch_slot == to) {
            preset_load(to);  /* load meta first */
            for (ti2 = 0; ti2 < NUM_TRACKS; ti2++)
                for (si2 = 0; si2 < NUM_STEPS; si2++)
                    g_pattern.steps[ti2][si2] = s_scratch_steps[ti2][si2];
        } else {
            preset_load(to);
        }
        ui_draw_full();
        break;
    }

    case 'C': case 'c':
        for (t = 0; t < NUM_TRACKS; t++)
            for (s = 0; s < NUM_STEPS; s++)
                g_pattern.steps[t][s] = 0;
        ui_draw_grid(); break;

    case 'R': case 'r':
        s_scratch_slot = 0xFF;  /* discard scratch on explicit reload */
        preset_load(g_cur_preset); ui_draw_full(); break;

    /* ── Copy / Paste ──────────────────────────────────── */
    case 'T': case 't':
        /* Copy current track row only */
        {
            uint8_t si;
            for (si = 0; si < NUM_STEPS; si++)
                s_clip_steps[0][si] = g_pattern.steps[g_cur_track][si];
            s_clip_tracks = 1;
            s_clip_valid  = 1;
            ui_draw_status();
            ui_flash_msg("CPY:TRACK", 20);
        }
        break;

    case 'Y': case 'y':
        /* Copy full pattern (all tracks) */
        {
            uint8_t ti, si;
            for (ti = 0; ti < NUM_TRACKS; ti++)
                for (si = 0; si < NUM_STEPS; si++)
                    s_clip_steps[ti][si] = g_pattern.steps[ti][si];
            s_clip_tracks = NUM_TRACKS;
            s_clip_valid  = 1;
            ui_draw_status();
            ui_flash_msg("CPY:PATTERN", 20);
        }
        break;

    case 'V': case 'v':
        /* Paste clipboard */
        if (s_clip_valid) {
            uint8_t ti, si;
            if (s_clip_tracks == 1) {
                /* Paste single track into current track */
                for (si = 0; si < NUM_STEPS; si++)
                    g_pattern.steps[g_cur_track][si] = s_clip_steps[0][si];
                ui_flash_msg("PST:TRACK", 20);
            } else {
                /* Paste full pattern */
                for (ti = 0; ti < NUM_TRACKS; ti++)
                    for (si = 0; si < NUM_STEPS; si++)
                        g_pattern.steps[ti][si] = s_clip_steps[ti][si];
                ui_flash_msg("PST:PATTERN", 20);
            }
            ui_draw_grid();
        } else {
            ui_flash_msg("EMPTY CLIP", 20);
        }
        break;

    case 133: g_kit = KIT_909;  ui_draw_status(); break; /* F1 */
    case 134: g_kit = KIT_808;  ui_draw_status(); break; /* F3 */
    case 135: g_kit = KIT_ROCK; ui_draw_status(); break; /* F5 */
    case 138: g_kit = KIT_SID;  ui_draw_status(); break; /* F6 */

    case '2':
        g_dual_sid ^= 1;
        sid_init();
        ui_draw_status(); break;

    case '3':
        /* Cycle SID2 address: DE00 -> DF00 -> D500 -> D420 -> DE00 */
        sid_next_addr();
        ui_draw_status(); break;

    /* ── Disk slot selection ─────────────────────────────── */
    case '[':  /* decrease slot */
        if (g_disk_slot > 0) g_disk_slot--;
        else g_disk_slot = 9;
        ui_draw_status(); break;

    case ']':  /* increase slot */
        if (g_disk_slot < 9) g_disk_slot++;
        else g_disk_slot = 0;
        ui_draw_status(); break;

    case 'D': case 'd':
        /* Cycle drive number 8 -> 9 -> 10 -> 11 -> 12 -> 8 */
        g_disk_dev++;
        if (g_disk_dev > 12) g_disk_dev = 8;
        ui_draw_status(); break;

    /* ── Disk save ───────────────────────────────────────── */
    case 'W': case 'w': {
        uint8_t ok;
        s_scratch_slot = 0xFF;  /* saved to disk - scratch no longer needed? */
        /* Show "SAVING..." in status bar */
        sfill(24, 0, 40, 0x20, CLB);
        sputs(24, 0, "SAVING TO SLOT ", CLB);
        sput(24, 15, (uint8_t)('0' + g_disk_slot), CW);
        sputs(24, 17, "...", CLB);
        ok = disk_save_pattern(g_disk_slot);
        sfill(24, 0, 40, 0x20, CLB);
        if (ok) sputs(24, 0, "SAVED OK       ", CYL);
        else    sputs(24, 0, "SAVE ERR:      ", CR);
        /* Show actual drive error message */
        sputs(24, 9, g_disk_err, ok ? CYL : CR);
        break;
    }

    /* ── Disk load ───────────────────────────────────────── */
    case 'L': case 'l': {
        uint8_t ok;
        sfill(24, 0, 40, 0x20, CLB);
        sputs(24, 0, "LOADING SLOT ", CLB);
        sput(24, 13, (uint8_t)('0' + g_disk_slot), CW);
        sputs(24, 15, "...", CLB);
        ok = disk_load_pattern(g_disk_slot);
        if (ok) {
            ui_draw_full();   /* redraw with new pattern */
        } else {
            sfill(24, 0, 40, 0x20, CLB);
            sputs(24, 0, "LOAD ERR: ", CR);
            sputs(24, 10, g_disk_err, CR);
        }
        break;
    }

    case 'Q': case 'q':
        seq_stop();
        sid_silence();
        seq_restore_irq();
        __asm { jmp $E37B }
        break;

    default: break;
    }
}