#ifndef DRUMBOX_H
#define DRUMBOX_H

#include <stdint.h>

/* ── SID hardware addresses ─────────────────────────────────────── */
#define SID1_BASE   0xD400
#define SID2_BASE   0xDE00   /* common expansion address; may also be 0xD500 */

/* ── Screen / IO ────────────────────────────────────────────────── */
#define SCREEN_BASE 0x0400
#define COLOR_BASE  0xD800
#define COLS        40
#define ROWS        25

/* C64 keyboard scan via CIA1 */
#define CIA1_PRA    (*(volatile uint8_t*)0xDC00)
#define CIA1_PRB    (*(volatile uint8_t*)0xDC01)
#define CIA1_DDRA   (*(volatile uint8_t*)0xDC02)
#define CIA1_DDRB   (*(volatile uint8_t*)0xDC03)
#define CIA1_TA_LO  (*(volatile uint8_t*)0xDC04)
#define CIA1_TA_HI  (*(volatile uint8_t*)0xDC05)
#define CIA1_CRA    (*(volatile uint8_t*)0xDC0E)
#define CIA1_ICR    (*(volatile uint8_t*)0xDC0D)

/* IRQ vector */
#define IRQ_VECTOR  (*(uint16_t*)0x0314)

/* ── Sequencer constants ─────────────────────────────────────────── */
#define NUM_STEPS       16
#define NUM_TRACKS      7       /* kick snare chh ohh tom clap crash */

/* Track indices */
#define TRK_KICK    0
#define TRK_SNARE   1
#define TRK_CHH     2
#define TRK_OHH     3
#define TRK_TOM     4
#define TRK_CLAP    5
#define TRK_CRASH   6
#define MAX_PRESETS     36
#define DEFAULT_TEMPO   120

/* Kit styles */
#define KIT_909     0
#define KIT_808     1
#define KIT_ROCK    2
#define NUM_KITS    3

/* Sequencer state */
#define SEQ_STOPPED 0
#define SEQ_PLAYING 1

/* ── SID register offsets (per voice, base + voice*7) ───────────── */
#define SID_FREQ_LO 0
#define SID_FREQ_HI 1
#define SID_PW_LO   2
#define SID_PW_HI   3
#define SID_CTRL    4
#define SID_AD      5
#define SID_SR      6
/* Global SID regs (from base) */
#define SID_FCUT_LO 0x15
#define SID_FCUT_HI 0x16
#define SID_RES_FLT 0x17
#define SID_VOL_FLT 0x18

/* SID control bits */
#define GATE        0x01
#define SYNC        0x02
#define RING        0x04
#define TEST        0x08
#define TRI         0x10
#define SAW         0x20
#define PULSE       0x40
#define NOISE       0x80

/* ── Types ──────────────────────────────────────────────────────── */

typedef struct {
    uint8_t  steps[NUM_TRACKS][NUM_STEPS];  /* velocity 0-3 per step */
    uint8_t  name[16];                      /* preset name */
    uint8_t  kit;                           /* which kit */
    uint8_t  tempo;                         /* BPM, 40-250 (fits uint8_t) */
} Pattern;

typedef struct {
    /* Synthesis parameters per instrument per kit */
    uint16_t freq_start;    /* starting SID frequency word */
    uint16_t freq_end;      /* ending SID frequency (for sweep) */
    uint8_t  waveform;      /* SID waveform bits */
    uint8_t  ad;            /* attack/decay nibbles */
    uint8_t  sr;            /* sustain/release nibbles */
    uint8_t  sweep_rate;    /* freq change per tick (0=none) */
    uint8_t  vol;           /* relative volume 0-15 */
} DrumVoice;

/* Global state (defined in main.c) */
extern volatile uint8_t  g_cur_step;       /* 0..15, current play step */
extern volatile uint8_t  g_seq_state;      /* SEQ_STOPPED / SEQ_PLAYING */
extern volatile uint8_t  g_tick_flag;      /* set by ISR, cleared by main */

extern uint16_t g_tempo;                   /* current BPM */
extern uint8_t  g_swing;                   /* swing 0=straight 54=classic 99=max */
extern uint8_t  g_kit;                     /* current kit style */
extern uint8_t  g_cur_preset;             /* index into preset table */
extern uint8_t  g_cur_track;              /* cursor track row */
extern uint8_t  g_cur_col;               /* cursor step column */
extern uint8_t  g_dual_sid;              /* 1 if second SID enabled */
extern uint16_t g_sid2_base;             /* SID2 base address (runtime configurable) */
extern uint8_t  g_sid2_idx;             /* index into SID2 address table */

extern Pattern  g_pattern;                 /* working pattern (copy from preset) */

/* ── Voice sweep state (updated each IRQ tick) ──────────────────── */
typedef struct {
    uint8_t  sid;        /* 0=SID1, 1=SID2 */
    uint8_t  voice;      /* 0-2 */
    uint16_t freq;       /* current frequency */
    uint16_t freq_end;   /* target frequency */
    uint8_t  sweep;      /* sweep rate */
    uint8_t  active;     /* 1 if voice is playing */
    uint8_t  ttl;        /* ticks remaining (for gate-off) */
    uint8_t  wave2;      /* waveform to switch to (0=no switch) */
    uint8_t  wave2_tick; /* tick at which to switch waveform */
    uint16_t freq2;      /* frequency to set when switching to wave2 (0=keep) */
} VoiceState;

extern VoiceState g_voices[6];   /* 6 voices total across both SIDs */

/* ── Function prototypes ────────────────────────────────────────── */

/* sid.c */
void    sid_init(void);
int     sid_detect_dual(void);
void    sid_write(uint8_t sid, uint8_t reg, uint8_t val);
uint8_t sid_read(uint8_t sid, uint8_t reg);
void    sid_trigger(uint8_t track, uint8_t vel, uint8_t kit);
void    sid_update_sweeps(void);
void    sid_silence(void);
void    sid_next_addr(void);     /* cycle SID2 address to next option */
extern const uint16_t g_sid2_addrs[];
extern const uint8_t  g_sid2_num_addrs;

/* seq.c */
void    seq_init(void);
void    seq_restore_irq(void);
void    seq_tick_capture(void);
void    seq_poll(void);          /* call from main loop - does all sequencer work */
void    seq_start(void);
void    seq_stop(void);
void    seq_set_tempo(uint16_t bpm);
void    seq_set_swing(uint8_t pct);
void    seq_tick(void);

/* ui.c */
void    ui_init(void);
void    ui_draw_full(void);
void    ui_draw_grid(void);
void    ui_draw_param_bar(void);
void    ui_draw_status(void);
void    ui_draw_playhead(uint8_t step);
void    ui_handle_key(uint8_t key);
uint8_t ui_read_key(void);
void    ui_poll_joystick(void);
extern  uint8_t g_edit_mode;

/* presets.c */
void    preset_load(uint8_t index);
void    preset_get_name(uint8_t index, uint8_t *buf);
extern const Pattern g_presets[MAX_PRESETS];
extern const uint8_t g_num_presets;

/* util - direct hardware access via pointer, no wrappers needed */

#endif /* DRUMBOX_H */

/* ── Disk I/O (diskio.c) ────────────────────────────────────────── */
extern uint8_t g_disk_dev;
extern uint8_t g_disk_slot;
extern char    g_disk_err[40];
uint8_t disk_save_pattern(uint8_t slot);
uint8_t disk_load_pattern(uint8_t slot);