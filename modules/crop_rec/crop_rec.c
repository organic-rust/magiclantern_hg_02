#include <dryos.h>
#include <module.h>
#include <config.h>
#include <menu.h>
#include <beep.h>
#include <property.h>
#include <patch.h>
#include <bmp.h>
#include <lvinfo.h>
#include <powersave.h>
#include <raw.h>
#include <fps.h>
#include <shoot.h>
#include <lens.h>
#include <timer.h>

extern WEAK_FUNC(ret_0) unsigned int is_crop_hack_supported();
extern WEAK_FUNC(ret_0) unsigned int movie_crop_hack_enable();
extern WEAK_FUNC(ret_0) unsigned int movie_crop_hack_disable();

#undef CROP_DEBUG

#ifdef CROP_DEBUG
#define dbg_printf(fmt,...) { printf(fmt, ## __VA_ARGS__); }
#else
#define dbg_printf(fmt,...) {}
#endif

static int is_5D3 = 0;
static int is_EOSM = 0;
static int is_EOSM2 = 0;

static CONFIG_INT("crop.preset", crop_preset_index, 0);
static CONFIG_INT("crop.shutter_range", shutter_range, 0);
static CONFIG_INT("crop.bitdepth", bitdepth, 0);
static CONFIG_INT("crop.ratios", ratios, 0);
static CONFIG_INT("crop.x3crop", x3crop, 0);
static CONFIG_INT("crop.set_25fps", set_25fps, 0);
static CONFIG_INT("crop.HDR_iso_a", HDR_iso_a, 0);
static CONFIG_INT("crop.HDR_iso_b", HDR_iso_b, 0);

enum crop_preset {
    CROP_PRESET_OFF = 0,
    CROP_PRESET_3x3_mv1080_EOSM,
    CROP_PRESET_3x3_mv1080_45fps_EOSM,
    CROP_PRESET_1x3_EOSM,
    CROP_PRESET_3x3_1X_EOSM,
    CROP_PRESET_2K_EOSM,
    CROP_PRESET_3K_EOSM,
    CROP_PRESET_4K_EOSM,
    CROP_PRESET_mcm_mv1080_EOSM2,
    CROP_PRESET_anamorphic_rewired_EOSM2,
    CROP_PRESET_3x1_mv720_50fps_EOSM2,
    NUM_CROP_PRESETS
};

/* presets are not enabled right away (we need to go to play mode and back)
 * so we keep two variables: what's selected in menu and what's actually used.
 * note: the menu choices are camera-dependent */
static enum crop_preset crop_preset = 0;

/* must be assigned in crop_rec_init */
static enum crop_preset * crop_presets = 0;

/* current menu selection (*/
#define CROP_PRESET_MENU crop_presets[crop_preset_index]


/* menu choices for EOSM */
static enum crop_preset crop_presets_eosm[] = {
    CROP_PRESET_OFF,
    CROP_PRESET_3x3_mv1080_EOSM,
    CROP_PRESET_3x3_mv1080_45fps_EOSM,
    CROP_PRESET_1x3_EOSM,
    CROP_PRESET_2K_EOSM,
    CROP_PRESET_3K_EOSM,
    CROP_PRESET_4K_EOSM,
    CROP_PRESET_3x3_1X_EOSM,
};

static const char * crop_choices_eosm[] = {
    "OFF",
    "mv1080p 1736x1120",
    "mv1080p 1736x976 45fps",
    "1x3 1736x1120",
    "2.5K 2520x1304",
    "3K 3032x1436",
    "4K 4038x2558",
    "3x3 720p",
};

static const char crop_choices_help_eosm[] =
    "Change 1080p and 720p movie modes into crop modes (select one)";

static const char crop_choices_help2_eosm[] =
    "\n"
    "mv1080p derived from 3x3 (square pixels in RAW, vertical crop)\n"
    "mv1080p 45fps\n"
    "1x3 binning: read all lines, bin every 3 columns (extreme anamorphic)\n"
    "1:1 2.5K crop (2520x1304 16:9 @ 24p, square raw pixels, cropped preview)\n"
    "1:1 3K crop (3032x1436 @ 24p, square raw pixels, preview broken)\n"
    "1:1 4K crop (4096x2560 @ 9.477p, square raw pixels, preview broken)\n"
    "3x3 binning in 720p (square pixels in RAW, vertical crop)\n";

static enum crop_preset crop_presets_eosm2[] = {
    CROP_PRESET_OFF,
    CROP_PRESET_mcm_mv1080_EOSM2,
    CROP_PRESET_anamorphic_rewired_EOSM2,
    CROP_PRESET_3x1_mv720_50fps_EOSM2,
};

static const char * crop_choices_eosm2[] = {
    "OFF",
    "mv1080p_rewired",
    "4K anamorphic rewired",
  //  "mv720p 1736x694 50fps",
  //  "1x3 binning",
};

static const char crop_choices_help_eosm2[] =
    "Change 1080p and 720p movie modes into crop modes (select one)";

static const char crop_choices_help2_eosm2[] =
    "\n"
    "enables Movie crop mode\n"
    "1x3 binning modes(anamorphic) rewired 2.35:1\n";
 //   "mv720p 50fps 16:9\n"



/* camera-specific parameters */
static uint32_t CMOS_WRITE      = 0;
static uint32_t MEM_CMOS_WRITE  = 0;
static uint32_t ADTG_WRITE      = 0;
static uint32_t MEM_ADTG_WRITE  = 0;
static uint32_t ENGIO_WRITE     = 0;
static uint32_t MEM_ENGIO_WRITE = 0;
static uint32_t MPU_RECV        = 0;
static uint32_t MEM_MPU_RECV    = 0;

/* ---------------------------------------------------------------------------
 * AF method keeper for the movie-crop presets (mcm / anamorphic, EOS M2)
 *
 * PROP_LIVE_VIEW_AF_SYSTEM is owned by the MPU. Canon's DIGIC firmware never
 * writes it on its own (only the menu / Q-screen / PTP code does); the MPU
 * re-sends it (spell 06 05 01 49 vv 00) after every PROP_VIDEO_MODE spell,
 * and with Movie crop enabled it reports FlexiZone-Single. Re-asserting the
 * AF method goes back to the MPU, which then drops Movie crop.
 *
 * Workaround: rewrite the AF byte in incoming MPU spells, so the DIGIC side
 * keeps the method we want and the MPU is never asked to change anything.
 * --------------------------------------------------------------------------- */

/* 0 = leave it to Canon, 1 = FlexiZone-Single, 2 = Face+Tracking, 3 = FlexiZone-Multi */
static CONFIG_INT("crop.mcm_af", mcm_af, 0);
static int mcm_af_log = 0;                  /* not saved: print AF / MPU trace to console */

static volatile int mcm_af_filter   = 0;    /* rewrite AF spells from the MPU */
static volatile int mcm_af_target   = -1;   /* PROP_LIVE_VIEW_AF_SYSTEM value we want */
static volatile int mcm_af_restore  = 0;
static volatile int movie_servo_af  = 0;    /* PROP_MOVIE_SERVO_AF */
static uint32_t cur_video_mode[16];         /* last PROP_VIDEO_MODE seen, to re-send it */
static int cur_video_mode_len = 0;    /* ask MPU for mcm_af_target once crop mode is off */
static volatile int mcm_af_rewrites = 0;
static volatile int mpu_spells_seen = 0;    /* any spell through the hook (proves it is installed) */
static volatile int mpu_af_spells   = 0;    /* AF_SYSTEM spells from the MPU */
static int mcm_af_delivers = 0;             /* local re-deliveries of the target AF value */
static int mpu_hook_active = 0;
static int mpu_hook_err    = 0;

#define AFLOG_N 32
static struct { uint32_t t; uint8_t cls, id, val, rewrote; } aflog[AFLOG_N];
static volatile int aflog_w = 0;
static int aflog_r = 0;

static int is_mcm_preset(int preset);

/* Property-write spy: who changes PROP_LIVE_VIEW_AF_SYSTEM?
 * Hooks Canon's prop_request_change and prop_deliver (EOS M2 1.0.3) and records
 * writes to AF_SYSTEM / VIDEO_MODE / DISPSIZE with the caller address. */
#define PROP_REQUEST_CHANGE_ADDR  0xFF12CDE8
#define PROP_REQUEST_CHANGE_INSN  0xE92D43F8    /* push {r3-r9, lr} */
#define PROP_DELIVER_ADDR         0xFF12D364
#define PROP_DELIVER_INSN         0xE92D4FF8    /* push {r3-r11, lr} */

#define PWLOG_N 16
static struct { uint32_t caller; uint32_t val; uint8_t kind, which; } pwlog[PWLOG_N];
static volatile int pwlog_w = 0;
static int pwlog_r = 0;
static volatile int pw_count_af = 0, pw_count_vm = 0, pw_count_zoom = 0;
/* AF writes kept separately so other writes can't push them out: first 2 + last 2 */
static struct { uint32_t caller; uint32_t val; uint8_t kind; } afw_first[2], afw_last[2];
/* AF_SYSTEM change notifications seen by our prop handler, with context */
static volatile int af_notify_count = 0;
static struct { int old, val, vm, zoom, crop, disp; } af_notify[2];   /* [0] first, [1] latest */
static volatile int af_last_val = -1;

/* PropertyMgr "deliver array" (partner notify from the MPU layer); raw args when one is AF_SYSTEM */
#define PROP_DELIVER_ARRAY_ADDR   0xFF12CE70
#define PROP_DELIVER_ARRAY_INSN   0xE92D4FF8    /* push {r3-r11, lr} */
static volatile int dlvarr_af_count = 0;
static uint32_t dlvarr_af_args[6];               /* r0..r3, stack[0], caller (first hit) */

/* PropertyMgr message poster: every PropertyMgr API funnels through it.
 * r2 = message type, r3 = payload (contains the property ID somewhere in the first words) */
#define PROPMGR_POST_ADDR   0xFF12D8A4
#define PROPMGR_POST_INSN   0xE92D4008      /* push {r3, lr} */
static volatile int post_af_count = 0;
static struct { uint32_t type, api, ret[4], w[5]; } post_af[2];    /* [0] first, [1] latest */

/* SetPropertyData: writes a property's stored value directly (no PropertyMgr message) */
#define SET_PROP_DATA_ADDR  0xFF12F280
#define SET_PROP_DATA_INSN  0xE92D43F0      /* push {r4-r9, lr} */
static volatile int setdata_af_count = 0;
static struct { uint32_t val, api, ret[4]; } setdata_af[2];        /* [0] first, [1] latest */

static int prop_spy_active = 0;
static int prop_spy_err = 0;

/* ---------------------------------------------------------------------------
 * Canon DebugMsg capture: records (class, level, fmt, 5 args) of every Canon
 * debug message into a ring buffer, plus our own markers, and saves it to
 * ML/AFLOG.TXT on request. Formatting happens only at save time.
 * --------------------------------------------------------------------------- */
#define DEBUGMSG_ADDR   0x4398
#define DEBUGMSG_INSN   0xE92D000F      /* push {r0-r3} */
#define DMLOG_N         4096
#define DM_MARKER       0xFFFF
struct dmlog_entry { uint32_t t; uint16_t cls, lvl; const char * fmt; uint32_t a[5]; uint32_t rep; };
static struct dmlog_entry dmlog[DMLOG_N];
static volatile int dmlog_w = 0;
static volatile int gmt_trace = 0;          /* AF frame trace: only Gmt classes, repeats collapsed, no freeze */
static volatile int saf_trace_cls = -1;     /* when >= 0, Freeze trace keeps only this class, 154 and markers */
static volatile int frz_trace = 0;          /* freeze trace: all classes, repeats collapsed, stops after a LV freeze */
static volatile int frz_stop_at = 0;        /* 0 = running; else stop when dmlog_w reaches it */
static uint32_t dm_cls_count[256];          /* messages per class (before collapsing), for the file header */
static volatile int dmlog_paused = 0;
static volatile int dmlog_stop_at = 0;      /* 0 = free running; else freeze when dmlog_w reaches it */
static int dmlog_hooked = 0;

static void FAST dmlog_add(int cls, int lvl, const char * fmt, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4)
{
    if (dmlog_paused) return;
    if (gmt_trace || frz_trace)
    {
        /* gmt: 154 GmtState, 196 GmtWaku (+ markers); freeze: everything.
         * Identical consecutive messages are collapsed. */
        if (gmt_trace && cls != DM_MARKER && cls != 154 && cls != 196) return;
        if (frz_trace && frz_stop_at && dmlog_w >= frz_stop_at) return;
        if (frz_trace && saf_trace_cls >= 0 && cls != DM_MARKER && cls != 154 && cls != saf_trace_cls) return;
        if (frz_trace && cls == 21) { dm_cls_count[21]++; return; }   /* register-write spam (~6000/s in mcm) */
        if (cls != DM_MARKER) dm_cls_count[cls & 0xFF]++;
        int w = dmlog_w;
        if (w > 0)
        {
            struct dmlog_entry * p = &dmlog[(w - 1) % DMLOG_N];
            if (p->fmt == fmt && p->cls == cls && p->a[0] == a0 && p->a[1] == a1 &&
                p->a[2] == a2 && p->a[3] == a3 && p->a[4] == a4)
            {
                p->rep++;
                return;
            }
        }
        struct dmlog_entry * e = &dmlog[w % DMLOG_N];
        e->t = GET_DIGIC_TIMER();
        e->cls = cls; e->lvl = lvl; e->fmt = fmt; e->rep = 0;
        e->a[0] = a0; e->a[1] = a1; e->a[2] = a2; e->a[3] = a3; e->a[4] = a4;
        dmlog_w = w + 1;
        return;
    }
    /* keep only AF / face / property / LiveView-GUI related classes (and our markers):
     * 1 prop, 131 GUI LV, 143 dcs, 154 GmtState, 170 lvface, 184 AF calc, 196 GmtWaku */
    if (cls != DM_MARKER && cls != 1 && cls != 131 && cls != 143 && cls != 154 &&
        cls != 170 && cls != 184 && cls != 196) return;
    int w = dmlog_w;
    if (dmlog_stop_at && w >= dmlog_stop_at) return;    /* frozen after an AF change */
    struct dmlog_entry * e = &dmlog[w % DMLOG_N];
    e->t = GET_DIGIC_TIMER();
    e->cls = cls; e->lvl = lvl; e->fmt = fmt; e->rep = 0;
    e->a[0] = a0; e->a[1] = a1; e->a[2] = a2; e->a[3] = a3; e->a[4] = a4;
    dmlog_w = w + 1;
}

static void FAST debugmsg_hook(uint32_t* regs, uint32_t* stack, uint32_t pc)
{
    /* DryosDebugMsg(class, level, fmt, ...): args 1..4 are on the caller's stack */
    dmlog_add(regs[0], regs[1], (const char *) regs[2], regs[3], stack[0], stack[1], stack[2], stack[3]);
}

/* When ML's CONFIG_LVAPP_HACK_DEBUGMSG is active (EOS M2), DryosDebugMsg starts with
 * "B hacked_DebugMsg" instead of Canon's code. Take over that branch: record the
 * message, then call ML's hacked_DebugMsg exactly as before. */
extern void sync_caches();     /* patch.c */
typedef void (*debugmsg_fn)(int, int, const char *, ...);
static debugmsg_fn dm_next = 0;
static uint32_t dm_insn_seen = 0;

static void my_DebugMsg(int cls, int lvl, const char * fmt,
    uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4)
{
    /* same register/stack layout as the variadic call, so a0..a4 are the format args */
    dmlog_add(cls, lvl, fmt, a0, a1, a2, a3, a4);
    if (dm_next) dm_next(cls, lvl, fmt, a0, a1, a2, a3, a4);
}

static int install_debugmsg_capture()
{
    uint32_t insn = *(volatile uint32_t *) DEBUGMSG_ADDR;
    dm_insn_seen = insn;

    if (insn == DEBUGMSG_INSN)
    {
        /* Canon's original code: a normal logging hook works */
        return patch_hook_function(DEBUGMSG_ADDR, DEBUGMSG_INSN, &debugmsg_hook, "crop_rec: DebugMsg capture") == 0;
    }

    if ((insn & 0xFF000000) == 0xEA000000)      /* B <target>: ML's DebugMsg hack */
    {
        int32_t off = insn & 0x00FFFFFF;
        if (off & 0x00800000) off -= 0x01000000;
        uint32_t target = DEBUGMSG_ADDR + 8 + (off << 2);
        dm_next = (debugmsg_fn) target;

        uint32_t mine = (uint32_t) &my_DebugMsg;
        int32_t rel = (int32_t)(mine - DEBUGMSG_ADDR - 8);
        if (rel < -0x02000000 || rel >= 0x02000000) return 0;   /* out of B range */

        uint32_t old = cli();
        *(volatile uint32_t *) DEBUGMSG_ADDR = 0xEA000000 | ((rel >> 2) & 0x00FFFFFF);
        sei(old);
        sync_caches();
        return 1;
    }

    return 0;
}

/* PropertyState.c: incoming MPU property messages (FF329840).
 * regs[1] -> { class, id, data*, len, ... }. The MPU's reply to a VIDEO_MODE change
 * carries class 1 id 0x49 (PROP_LIVE_VIEW_AF_SYSTEM); with Digital zoom on it says 1
 * (FlexiZone-Single). Rewriting the data here, before PropertyState stores and delivers
 * it, keeps the chosen AF method on the DIGIC side without asking the MPU anything. */
#define PSTATE_MPU_IN_ADDR  0xFF329840
#define PSTATE_MPU_IN_INSN  0xE92D41F0      /* push {r4-r8, lr} */
static int pstate_hook_err = -1;

/* ---------------------------------------------------------------------------
 * Hybrid CMOS AF (on-sensor phase detection, "SAF") mode, EOS M2 1.0.3.
 * Evf picks a SAF mode for the current sensor readout (RAM 0x11E94, inputs:
 * r0 shooting kind, r1 Evf LV mode, r2 zoom, r3 + 2 stack args) and passes it to
 * SAF_SetupCaptureForMovie (FF101C00, param[0] = mode; 1..8 valid, else "through" =
 * no phase detection). Movie crop / Digital zoom (Evf mode 11/12) gets mode 8, which
 * expects Canon's Digital zoom readout - crop_rec's mcm readout is different, which
 * may leave phase detection reading the wrong pixels (slow, hesitant AF).
 * These hooks show what is chosen and allow overriding it for testing.
 * --------------------------------------------------------------------------- */
#define SAF_SELECT_ADDR     0x11E94
#define SAF_SELECT_INSN     0xE92D4070      /* push {r4, r5, r6, lr} */
#define SAF_SETUP_ADDR      0xFF101C00
#define SAF_SETUP_INSN      0xE92D43F0      /* push {r4-r9, lr} */
static int saf_hook_err = -1;
static int saf_override = 0;                /* 0 = Canon; 1..8 = force mode; 9 = phase detection off */
static volatile int saf_seen_mode = -1;     /* last mode Canon chose */
static volatile int saf_used_mode = -1;     /* last mode actually passed on */
static volatile int saf_seen_count = 0;
static volatile uint32_t saf_sel_in[6];     /* last inputs to the mode selector */

/* one snapshot per case (0 = no Digital zoom, 1 = Digital zoom / mcm), saved to ML/SAF.TXT */
struct saf_snap {
    int valid, count, mode_in, mode_out;
    uint32_t sel_in[6];
    uint16_t param[12];         /* SAF_SetupCaptureForMovie param block (mode u32 + 10 halfwords) */
    uint32_t rd_start, rd_end;  /* sensor readout start/end (C0F06800 / C0F06804) */
    uint32_t geo_ptr[2];        /* Evf geometry structs used for the SAF window (0x9269C+0x44/+0x48) */
    uint16_t geo[2][12];
};
static struct saf_snap saf_snap[2];

/* SAF_DefocusResult (FF27D608): sends the phase-detect result as a 34-byte property:
 * 10 areas x big-endian s16 defocus (bytes 0..19), 10 x u8 reliability (20..29),
 * focus position (30..31), 2 more bytes. In mcm the readings are noise (D0 jumps
 * sign every frame while the lens moves steadily; other areas invalid), so optionally
 * mark every area unreliable there and let AF work from contrast only. */
#define SAF_RESULT_ADDR     0xFF27D608
#define SAF_RESULT_INSN     0xE92D4070      /* push {r4, r5, r6, lr} */
static int saf_result_hook_err = -1;
static int saf_pd_ignore = 0;               /* 0 = Canon, 1 = zero reliability, 2 = drop results (mcm only) */

/* "Drop": make SAF_DefocusResult return before delivering the result (its own early exit,
 * FF27D61C popeq -> pop), so AF never receives phase-detect results - the state picker 1
 * produced by accident (SAF stuck in ACCUMULATION), which gave more reliable contrast AF.
 * Same cache line as the result hook at FF27D608. Patched only while an mcm preset runs. */
#define SAF_RESULT_EXIT_ADDR 0xFF27D61C
#define SAF_RESULT_EXIT_ORIG 0x08BD8070     /* popeq {r4, r5, r6, pc} */
#define SAF_RESULT_EXIT_NEW  0xE8BD8070     /* pop   {r4, r5, r6, pc} */
static int saf_drop_patched = 0;
static int saf_drop_err = 0;
static volatile int saf_res_count = 0;
static volatile int saf_res_d0 = 0, saf_res_r0 = 0, saf_res_valid = 0, saf_res_fpc = 0;
#define SAF_HIST_N 32
static volatile int saf_hist_w = 0;
static volatile int saf_hist_valid[SAF_HIST_N];
static volatile int saf_hist_jump[SAF_HIST_N];

static void FAST saf_result_hook(uint32_t* regs, uint32_t* stack, uint32_t pc)
{
    uint8_t * pk = (uint8_t *) regs[0];
    if (!pk || (uint32_t) pk > 0x10000000) return;
    saf_res_count++;
    saf_res_d0 = (int16_t)((pk[0] << 8) | pk[1]);
    saf_res_r0 = pk[20];
    int v = 0;
    for (int i = 0; i < 10; i++) if (pk[20 + i]) v++;
    saf_res_valid = v;

    /* quality metrics over the last 32 results: valid areas, D0 frame-to-frame jump */
    /* jump counts only while the lens stands still (focus position, bytes 30..31, unchanged),
     * so lens movement from AF doesn't count as noise */
    static int last_d0 = 0, last_fpc = -1;
    int fpc = (pk[30] << 8) | pk[31];
    saf_res_fpc = fpc;
    int i = saf_hist_w % SAF_HIST_N;
    saf_hist_valid[i] = v;
    int jump = saf_res_d0 - last_d0; if (jump < 0) jump = -jump;
    saf_hist_jump[i] = (pk[20] && saf_hist_w && fpc == last_fpc) ? jump : -1;
    last_d0 = saf_res_d0;
    last_fpc = fpc;
    saf_hist_w++;

    if (saf_pd_ignore == 1 && video_mode_crop)
    {
        for (int i = 0; i < 10; i++) pk[20 + i] = 0;
    }
}

/* SAF "picker": a small program (RAM tables 0x95A18 / 0x95C18 / 0x95E18, copied by
 * SetDefaultPickerWindow, FF27D168) that tells the phase-detect hardware which readout
 * lines/columns to pick. The picker is chosen from the SAF mode by FF27D004:
 * modes 1,3 -> picker 0 (10 window pairs), 2,5 -> picker 1 (10 pairs, normal movie),
 * 4,6,7 -> picker 2 (1 pair: Digital zoom / magnify). In mcm, mode 4 picks picker 2,
 * laid out for Canon's 1:1 crop readout. Optionally use picker 0 or 1 with mode 4
 * (jump table entry for mode 4 at FF27D020). Applied at the next LiveView start. */
#define SAF_PICKER_M4_ADDR  0xFF27D020
#define SAF_PICKER_M4_ORIG  0xEA000006      /* b FF27D040 -> picker 2 */
static const uint32_t saf_picker_m4_new[3] = { 0, 0xEA000002 /* -> picker 0 */, 0xEA000004 /* -> picker 1 */ };
static int saf_picker = 0;                  /* 0 = Canon, 1 = picker 0, 2 = picker 1 */
static int saf_picker_applied = 0;
static int saf_picker_err = 0;

/* Picker program format (16-bit words, top 6 bits = opcode, low 10 bits = value), one block
 * of 11 words per window, each window twice (op 0xC value 0/1 = the two pixel halves):
 *   op F 0x320+n (+0x40 for 2nd half)  window n
 *   op 0  output offset (steps 32 in picker 1, 64 in picker 0)
 *   op 2  size?    op 4 / op 3  Y = (op4 << 8) + op3    op 6 / op 5  X = (op6 << 8) + op5
 *   op A/B  ?      op 8  180 / 60 / 36 for picker 2 / 0 / 1 = 180 / vertical skip (1, 3, 5)
 *   op C  half     op D  end of window
 * If op 8 is 180 / vertical line skip, picker 0 is the layout for a 3-line-skip readout like
 * mcm's. Its Y/X positions are for Canon's own readout of that kind; mcm's window start may
 * differ, so allow shifting picker 0's Y and X (RAM copy at 0x95A18 is the source that
 * SetDefaultPickerWindow copies at each LiveView start). */
/* Editable copies of the picker programs (RAM source tables copied at each LiveView start). */
struct saf_picker_edit {
    volatile uint16_t * table;
    int words;
    uint16_t orig[64 * 4];
    int saved;
    int yshift, xshift, op8;            /* wanted: shifts, op8 value (0 = Canon) */
    int ay, ax, a8;                     /* applied */
};
static struct saf_picker_edit saf_pe[2] = {
    { (volatile uint16_t *) 0x95A18, 64 * 4 },     /* picker 0 */
    { (volatile uint16_t *) 0x95E18,  8 * 4 },     /* picker 2 (Digital zoom / magnify, used by mcm) */
};
/* SetDefaultPickerWindow copies from the pointer table at 0x95E68 (picker 0..2); the ROM image
 * says 0x95A18 / 0x95C18 / 0x95E18, but follow the live pointers in case they differ. */
#define SAF_PICKER_PTRS     ((volatile uint32_t *) 0x95E68)
static uint32_t saf_picker_src[3];
static int saf_p0_yshift = 0, saf_p0_xshift = 0;
static int saf_p2_yshift = 0, saf_p2_xshift = 0, saf_p2_op8 = 0;
static const int saf_op8_values[] = { 0, 60, 90, 120, 36, 240, 360, 540 };

static void saf_picker_edit_apply(struct saf_picker_edit * pe, int yshift, int xshift, int op8)
{
    if (!pe->saved)
    {
        for (int i = 0; i < pe->words; i++) pe->orig[i] = pe->table[i];
        pe->saved = 1;
    }
    if (yshift == pe->ay && xshift == pe->ax && op8 == pe->a8) return;

    for (int i = 0; i < pe->words; i++) pe->table[i] = pe->orig[i];
    for (int i = 0; i < pe->words; i++)
    {
        uint16_t w = pe->orig[i];
        uint16_t n = (i + 1 < pe->words) ? pe->orig[i + 1] : 0;
        if ((w >> 10) == 0x8 && op8)
        {
            pe->table[i] = (0x8 << 10) | (op8 & 0x3FF);
        }
        if ((w >> 10) == 0x4 && (n >> 10) == 0x3)       /* Y high, Y low */
        {
            int y = ((w & 0x3FF) << 8) + (n & 0xFF) + yshift;
            if (y < 0) y = 0;
            pe->table[i]     = (0x4 << 10) | ((y >> 8) & 0x3FF);
            pe->table[i + 1] = (0x3 << 10) | (n & 0x300) | (y & 0xFF);
        }
        if ((w >> 10) == 0x6 && (n >> 10) == 0x5)       /* X high, X low */
        {
            int x = ((w & 0x3FF) << 8) + (n & 0xFF) + xshift;
            if (x < 0) x = 0;
            pe->table[i]     = (0x6 << 10) | ((x >> 8) & 0x3FF);
            pe->table[i + 1] = (0x5 << 10) | (n & 0x300) | (x & 0xFF);
        }
    }
    pe->ay = yshift; pe->ax = xshift; pe->a8 = op8;
}

static void saf_picker0_shift_update()
{
    if (!is_EOSM2) return;
    for (int k = 0; k < 3; k++) saf_picker_src[k] = SAF_PICKER_PTRS[k];
    for (int k = 0; k < 2; k++)
    {
        uint32_t src = saf_picker_src[k ? 2 : 0];
        if (!saf_pe[k].saved && src && !(src & 1) && src < 0x60000000)
            saf_pe[k].table = (volatile uint16_t *) src;
    }
    saf_picker_edit_apply(&saf_pe[0], saf_p0_yshift, saf_p0_xshift, 0);
    /* picker 2 is also Canon's normal Digital zoom / magnify layout: edit it only in mcm presets */
    int mcm = is_mcm_preset(crop_preset);
    saf_picker_edit_apply(&saf_pe[1], 0, 0, mcm ? saf_op8_values[saf_p2_op8] : 0);
}

static void saf_picker_update()
{
    saf_picker0_shift_update();
    if (saf_picker == saf_picker_applied) return;
    if (saf_picker_applied) { unpatch_memory(SAF_PICKER_M4_ADDR); saf_picker_applied = 0; }
    if (saf_picker)
    {
        saf_picker_err = patch_instruction(SAF_PICKER_M4_ADDR, SAF_PICKER_M4_ORIG, saf_picker_m4_new[saf_picker],
            "crop_rec: SAF mode 4 picker override");
        if (!saf_picker_err) saf_picker_applied = saf_picker;
        else saf_picker = 0;
    }
}

static void FAST saf_select_hook(uint32_t* regs, uint32_t* stack, uint32_t pc)
{
    saf_sel_in[0] = regs[0]; saf_sel_in[1] = regs[1]; saf_sel_in[2] = regs[2];
    saf_sel_in[3] = regs[3]; saf_sel_in[4] = stack[0]; saf_sel_in[5] = stack[1];
}


/* Changes applied to both SAF_SetupCaptureForMovie and SAF_Start parameters (same 24-byte
 * layout). SAF_Start runs every frame with a parameter block Evf rebuilds from scratch
 * (mode from the selector again): it asserts that its mode matches the one given at setup
 * (SAF.c:298 "this->CaptureMode == this->SafParameter.CaptureMode" - the crash behind every
 * forced mode), and copies its block over the setup one (so a setup-only WIN shift is lost).
 *   - mode override (mcm only)
 *   - WIN rectangle shift (param halfwords 7..10 = x, y, w, h; mcm presets only) */
/* The menu values are latched at setup (LiveView start) and only those latched values are
 * used by SAF_Start, so changing the menu while LiveView runs can't make the two disagree
 * (that trips the SAF.c:298 assert straight away). */
static int saf_latched_mode = 0, saf_latched_dx = 0, saf_latched_dy = 0;

static void saf_modify_param(uint32_t * param, int is_setup)
{
    if (is_setup)
    {
        saf_latched_mode = (saf_override && video_mode_crop) ? saf_override : 0;
        int mcm = is_mcm_preset(crop_preset);
        saf_latched_dx = mcm ? saf_p2_xshift : 0;
        saf_latched_dy = mcm ? saf_p2_yshift : 0;
    }
    if (saf_latched_mode)
    {
        param[0] = saf_latched_mode;
    }
    if (saf_latched_dx || saf_latched_dy)
    {
        uint16_t * pw = (uint16_t *) param;
        int x = pw[7] + saf_latched_dx, y = pw[8] + saf_latched_dy;
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        pw[7] = x; pw[8] = y;
    }
}

#define SAF_START_ADDR      0xFF1020E4
#define SAF_START_INSN      0xE92D4030      /* push {r4, r5, lr} */
static int saf_start_hook_err = -1;
static volatile int saf_start_count = 0;

static void FAST saf_start_hook(uint32_t* regs, uint32_t* stack, uint32_t pc)
{
    uint32_t * param = (uint32_t *) regs[0];
    if (!param || ((uint32_t) param & 3) || (uint32_t) param > 0x10000000) return;
    saf_start_count++;
    saf_modify_param(param, 0);
}

static void FAST saf_setup_hook(uint32_t* regs, uint32_t* stack, uint32_t pc)
{
    uint32_t * param = (uint32_t *) regs[0];
    if (!param || ((uint32_t) param & 3) || (uint32_t) param > 0x10000000) return;
    int mode = param[0];
    saf_seen_mode = mode;
    saf_seen_count++;
    /* override only in the mcm presets (Digital zoom readout rewired by crop_rec) */
    saf_modify_param(param, 1);
    saf_used_mode = param[0];

    struct saf_snap * sn = &saf_snap[video_mode_crop ? 1 : 0];
    sn->valid = 1;
    sn->count++;
    sn->mode_in = mode;
    sn->mode_out = param[0];
    for (int i = 0; i < 6; i++) sn->sel_in[i] = saf_sel_in[i];
    uint16_t * ph = (uint16_t *) param;
    for (int i = 0; i < 12; i++) sn->param[i] = ph[i];
    sn->rd_start = shamem_read(0xC0F06800);
    sn->rd_end   = shamem_read(0xC0F06804);
    for (int k = 0; k < 2; k++)
    {
        uint32_t g = *(volatile uint32_t *)(0x9269C + 0x44 + 4 * k);
        sn->geo_ptr[k] = g;
        for (int i = 0; i < 12; i++)
            sn->geo[k][i] = (g && !(g & 1) && g < 0x10000000) ? ((volatile uint16_t *) g)[i] : 0;
    }
    dmlog_add(0xFFFF, 0, "*** ML-SPY SAF setup mode %d -> %d (evf in %x %x %x)",
        mode, param[0], saf_sel_in[0], saf_sel_in[1], saf_sel_in[2]);
}

/* gmtUpdateNoface (FF18F270): with Digital zoom active, Face+Tracking without a face falls
 * back to "##### DZOOM AF #####" (one small box). Without Digital zoom it uses
 * "##### MULTI AF #####" (wide multi-zone). Make the branch at FF18F65C unconditional
 * so the multi-zone fallback is used in the mcm preset too. */
#define NOFACE_DZOOM_BRANCH_ADDR  0xFF18F65C
#define NOFACE_DZOOM_BRANCH_ORIG  0x0A00001E    /* beq FF18F6DC (MULTI AF) */
#define NOFACE_DZOOM_BRANCH_NEW   0xEA00001E    /* b   FF18F6DC (MULTI AF) */
static int noface_patched = 0;

static void FAST pstate_mpu_in_hook(uint32_t* regs, uint32_t* stack, uint32_t pc)
{
    uint32_t * m = (uint32_t *) regs[1];
    if (!m || ((uint32_t) m & 3) || (uint32_t) m > 0x10000000) return;
    if (m[0] != 1 || m[1] != 0x49) return;

    uint8_t * data = (uint8_t *) m[2];
    if (!data || (uint32_t) data > 0x10000000) return;

    int orig = data[0];
    int rewrote = 0;
    mpu_af_spells++;
    /* Only while not magnified: crop_rec toggles x5 while applying the preset, and Canon's
     * AF state machine (GmtWakuState) mishandles a switch to Face while zoomed (it ends up
     * in a single-point state). Leave the MPU's value alone then; mcm_af_poll() re-sends
     * VIDEO_MODE once zoom is back at x1, so the change 1 -> target is delivered cleanly. */
    if (mcm_af_filter && mcm_af_target > 0 && orig != mcm_af_target && lv_dispsize == 1)
    {
        data[0] = mcm_af_target;
        rewrote = 1;
        mcm_af_rewrites++;
    }
    dmlog_add(DM_MARKER, rewrote, "*** ML-SPY PropertyState MPU AF_SYSTEM in: %d -> %d (len %d)",
        orig, data[0], m[3], 0, 0);
}

static void FAST prop_write_hook(uint32_t* regs, uint32_t* stack, uint32_t pc)
{
    uint32_t id = regs[0];
    int which =
        (id == PROP_LIVE_VIEW_AF_SYSTEM) ? 'A' :
        (id == PROP_VIDEO_MODE)          ? 'V' :
        (id == PROP_LV_DISPSIZE)         ? 'Z' : 0;
    if (!which) return;

    uint32_t * buf = (uint32_t *) regs[1];
    uint8_t kind = (pc == PROP_DELIVER_ADDR) ? 'D' : (pc == PROP_REQUEST_CHANGE_ADDR) ? 'R' : 'W';
    dmlog_add(DM_MARKER, kind, "*** ML-SPY prop write %x val=%x caller=%x", id,
        buf ? buf[0] : 0xFFFFFFFF, PATCH_HOOK_CALLER(), 0, 0);
    uint32_t val = buf ? buf[0] : 0xFFFFFFFF;
    uint32_t caller = PATCH_HOOK_CALLER();

    if (which == 'A')
    {
        int n = pw_count_af;
        if (n < 2) { afw_first[n].caller = caller; afw_first[n].val = val; afw_first[n].kind = kind; }
        afw_last[n & 1].caller = caller; afw_last[n & 1].val = val; afw_last[n & 1].kind = kind;
        pw_count_af = n + 1;
    }
    else if (which == 'V') pw_count_vm++;
    else pw_count_zoom++;

    int w = pwlog_w;
    pwlog[w % PWLOG_N].caller = PATCH_HOOK_CALLER();
    pwlog[w % PWLOG_N].val    = buf ? buf[0] : 0xFFFFFFFF;
    pwlog[w % PWLOG_N].kind   = (pc == PROP_DELIVER_ADDR) ? 'D' : (pc == PROP_REQUEST_CHANGE_ADDR) ? 'R' : 'W';
    pwlog[w % PWLOG_N].which  = which;
    pwlog_w = w + 1;
}

static int FAST looks_like_return_addr(uint32_t v)
{
    if (v & 3) return 0;
    int rom = (v >= 0xFF000000 && v < 0xFFD00000);
    int ram = (v >= 0x1904 && v < 0xB89A0);
    if (!rom && !ram) return 0;
    uint32_t prev = *(volatile uint32_t *)(v - 4);
    return ((prev & 0x0F000000) == 0x0B000000) ||          /* BL / BLcc */
           ((prev & 0x0FFFFFF0) == 0x012FFF30);            /* BLX reg */
}

static void FAST propmgr_post_hook(uint32_t* regs, uint32_t* stack, uint32_t pc)
{
    uint32_t * payload = (uint32_t *) regs[3];
    if (!payload || ((uint32_t) payload & 3) || (uint32_t) payload > 0x10000000) return;

    int hit = 0;
    for (int i = 0; i < 5; i++)
        if (payload[i] == PROP_LIVE_VIEW_AF_SYSTEM) hit = 1;
    if (!hit) return;

    int n = post_af_count;
    int k = (n == 0) ? 0 : 1;
    post_af[k].type = regs[2];
    post_af[k].api  = PATCH_HOOK_CALLER();      /* which PropertyMgr API posted it */
    for (int i = 0; i < 5; i++) post_af[k].w[i] = payload[i];

    /* walk the stack for the first 4 return addresses: the API's caller and up */
    int r = 0;
    for (int i = 0; i < 48 && r < 4; i++)
        if (looks_like_return_addr(stack[i])) post_af[k].ret[r++] = stack[i] - 4;
    for (; r < 4; r++) post_af[k].ret[r] = 0;

    post_af_count = n + 1;
}

static void FAST set_prop_data_hook(uint32_t* regs, uint32_t* stack, uint32_t pc)
{
    if (regs[0] != PROP_LIVE_VIEW_AF_SYSTEM) return;

    int n = setdata_af_count;
    int k = (n == 0) ? 0 : 1;
    uint32_t * buf = (uint32_t *) regs[1];
    setdata_af[k].val = (buf && !((uint32_t) buf & 3) && (uint32_t) buf < 0x10000000) ? buf[0] : 0xFFFFFFFF;
    setdata_af[k].api = PATCH_HOOK_CALLER();    /* direct caller of SetPropertyData */
    int r = 0;
    for (int i = 0; i < 48 && r < 4; i++)
        if (looks_like_return_addr(stack[i])) setdata_af[k].ret[r++] = stack[i] - 4;
    for (; r < 4; r++) setdata_af[k].ret[r] = 0;
    setdata_af_count = n + 1;
}

static void FAST deliver_array_hook(uint32_t* regs, uint32_t* stack, uint32_t pc)
{
    /* args layout not known for sure: look for the AF_SYSTEM ID in any of them,
     * including one level of pointer (r3 often points to the payload) */
    int hit = 0;
    for (int i = 0; i < 4; i++)
        if (regs[i] == PROP_LIVE_VIEW_AF_SYSTEM) hit = 1;
    if (stack[0] == PROP_LIVE_VIEW_AF_SYSTEM) hit = 1;
    for (int i = 0; i < 4 && !hit; i++)
    {
        uint32_t a = regs[i];
        if ((a & 3) == 0 && a > 0x1000 && a < 0x10000000 &&   /* stay inside RAM */
            *(volatile uint32_t *) a == PROP_LIVE_VIEW_AF_SYSTEM) hit = 1;
    }
    if (!hit) return;

    if (dlvarr_af_count == 0)
    {
        for (int i = 0; i < 4; i++) dlvarr_af_args[i] = regs[i];
        dlvarr_af_args[4] = stack[0];
        dlvarr_af_args[5] = PATCH_HOOK_CALLER();
    }
    dlvarr_af_count++;
}

/* runs in the MPU receive path: keep it short, no printf / msleep here */
static void FAST mpu_recv_hook(uint32_t* regs, uint32_t* stack, uint32_t pc)
{
    uint8_t * spell = (uint8_t *) regs[0];
    mpu_spells_seen++;
    if (!spell || spell[0] < 5) return;

    int cls = spell[2];
    int id  = spell[3];

    int is_af   = (cls == 0x01 && id == 0x49);  /* PROP_LIVE_VIEW_AF_SYSTEM */
    int is_vmod = (cls == 0x01 && id == 0x4e);  /* PROP_VIDEO_MODE */
    int is_disp = (cls == 0x09 && id == 0x11);  /* PROP_LV_DISPSIZE */
    if (!is_af && !is_vmod && !is_disp) return;

    int orig = spell[4];
    int rewrote = 0;
    if (is_af) mpu_af_spells++;

    if (is_af && mcm_af_filter && mcm_af_target > 0 && orig != mcm_af_target)
    {
        spell[4] = mcm_af_target;
        rewrote = 1;
        mcm_af_rewrites++;
    }

    if (mcm_af_log)
    {
        int w = aflog_w;
        aflog[w % AFLOG_N].t       = GET_DIGIC_TIMER();
        aflog[w % AFLOG_N].cls     = cls;
        aflog[w % AFLOG_N].id      = id;
        aflog[w % AFLOG_N].val     = orig;
        aflog[w % AFLOG_N].rewrote = rewrote;
        aflog_w = w + 1;
    }
}

/* called from update_patch and from the polling CBR (normal task context) */
static void mcm_af_update()
{
    if (!is_EOSM2 || !MPU_RECV) return;

    int want = (mcm_af != 0) && is_mcm_preset(crop_preset);

    if (want && !mcm_af_filter)
    {
        mcm_af_target = mcm_af;
        mcm_af_filter = 1;
    }
    else if (!want && mcm_af_filter)
    {
        mcm_af_filter = 0;
        mcm_af_restore = 1;     /* MPU still thinks Single; fix it once crop is off */
    }
    else if (want)
    {
        mcm_af_target = mcm_af; /* menu changed while active */
    }

    /* Face+Tracking: use the multi-zone no-face fallback instead of the Digital zoom single box */
    int want_noface = mcm_af_filter && mcm_af_target == 2;
    if (want_noface && !noface_patched)
    {
        noface_patched = (patch_instruction(NOFACE_DZOOM_BRANCH_ADDR, NOFACE_DZOOM_BRANCH_ORIG,
            NOFACE_DZOOM_BRANCH_NEW, "crop_rec: Face+Tracking no-face multi AF in Digital zoom") == 0);
    }
    else if (!want_noface && noface_patched)
    {
        unpatch_memory(NOFACE_DZOOM_BRANCH_ADDR);
        noface_patched = 0;
    }

    /* Hybrid AF mode readout/override hooks (cheap: called on LiveView setup only) */
    if (saf_hook_err == -1)
    {
        saf_hook_err  = patch_hook_function(SAF_SELECT_ADDR, SAF_SELECT_INSN, &saf_select_hook, "crop_rec: SAF mode select spy");
        saf_hook_err |= patch_hook_function(SAF_SETUP_ADDR, SAF_SETUP_INSN, &saf_setup_hook, "crop_rec: SAF setup mode") << 8;
    }
    saf_picker_update();
    int want_drop = (saf_pd_ignore == 2) && video_mode_crop && saf_result_hook_err == 0;
    if (want_drop && !saf_drop_patched && !saf_drop_err)
    {
        saf_drop_err = patch_instruction(SAF_RESULT_EXIT_ADDR, SAF_RESULT_EXIT_ORIG, SAF_RESULT_EXIT_NEW,
            "crop_rec: drop phase-detect results (mcm)");
        saf_drop_patched = (saf_drop_err == 0);
    }
    else if (!want_drop && saf_drop_patched)
    {
        unpatch_memory(SAF_RESULT_EXIT_ADDR);
        saf_drop_patched = 0;
    }
    if (saf_start_hook_err == -1)
    {
        saf_start_hook_err = patch_hook_function(SAF_START_ADDR, SAF_START_INSN, &saf_start_hook, "crop_rec: SAF start (mode/WIN)");
    }
    if (saf_result_hook_err == -1)
    {
        saf_result_hook_err = patch_hook_function(SAF_RESULT_ADDR, SAF_RESULT_INSN, &saf_result_hook, "crop_rec: SAF result (phase detect)");
    }

    /* the AF filter itself: one hook in PropertyState's MPU input, only when an AF method is chosen */
    if (mcm_af != 0 && pstate_hook_err == -1)     /* try once; ERR is shown in the menu */
    {
        pstate_hook_err = patch_hook_function(PSTATE_MPU_IN_ADDR, PSTATE_MPU_IN_INSN, &pstate_mpu_in_hook, "crop_rec: PropertyState MPU AF filter");
    }

    /* diagnostics (property-write spies, MPU spell trace, Canon DebugMsg capture):
     * only installed once "AF/MPU trace" is turned on; they stay until reboot */
    if (mcm_af_log && !prop_spy_active && !prop_spy_err)
    {
        prop_spy_err  = patch_hook_function(PROP_REQUEST_CHANGE_ADDR, PROP_REQUEST_CHANGE_INSN, &prop_write_hook, "crop_rec: prop_request_change spy");
        prop_spy_err |= patch_hook_function(PROP_DELIVER_ADDR, PROP_DELIVER_INSN, &prop_write_hook, "crop_rec: prop_deliver spy") << 8;
        if (!dmlog_hooked) dmlog_hooked = install_debugmsg_capture();
        prop_spy_active = !prop_spy_err;

        /* ML core wrapper: records the real caller of set_lv_zoom & co. */
        uint32_t wait_addr = (uint32_t) &prop_request_change_wait;
        uint32_t wait_insn = *(volatile uint32_t *) wait_addr;
        if ((wait_insn & 0xFFFF0000) == 0xE92D0000)    /* push {...} */
        {
            if (patch_hook_function(wait_addr, wait_insn, &prop_write_hook, "crop_rec: prop_request_change_wait spy"))
            {
                prop_spy_err |= 0x10000;
            }
        }

        if (!mpu_hook_active)
        {
            mpu_hook_err = patch_hook_function(MPU_RECV, MEM_MPU_RECV, &mpu_recv_hook, "crop_rec: MPU spell trace");
            mpu_hook_active = (mpu_hook_err == 0);
        }
    }
}

static void mcm_af_poll()
{
    mcm_af_update();

    /* Startup case: Canon restores Movie crop and the MPU's AF value (1) from its settings,
     * so movie_crop_hack_enable() has nothing to send and the MPU never replies through the
     * filter. Re-send the current VIDEO_MODE once (like leaving the menu does), so the MPU's
     * linked reply - including AF_SYSTEM - passes through the filter. Max 2 tries per LV session. */
    static int resync_tries = 0;
    static int resync_last = 0;
    if (!lv || !mcm_af_filter) resync_tries = 0;
    if (mcm_af_filter && pstate_hook_err == 0 && lv && video_mode_crop && !RECORDING && lv_dispsize == 1 &&
        !gui_menu_shown() && mcm_af_target > 0 && lv_af_mode != mcm_af_target &&
        cur_video_mode_len >= 20 && resync_tries < 2 &&
        should_run_polling_action(2000, &resync_last))
    {
        resync_tries++;
        prop_request_change(PROP_VIDEO_MODE, cur_video_mode, cur_video_mode_len);
    }


    if (mcm_af_restore && !video_mode_crop && !RECORDING)
    {
        mcm_af_restore = 0;
        if (mcm_af_target > 0 && lv_af_mode != mcm_af_target)
        {
            int v = mcm_af_target;
            prop_request_change(PROP_LIVE_VIEW_AF_SYSTEM, &v, 4);
        }
    }

    while (mcm_af_log && pwlog_r != pwlog_w)
    {
        int i = pwlog_r % PWLOG_N;
        char kind[2] = { pwlog[i].kind, 0 };
        printf("[%s] %s val=%x from %08x\n", kind,
            pwlog[i].which == 'A' ? "AF_SYSTEM " : pwlog[i].which == 'V' ? "VIDEO_MODE" : "DISPSIZE  ",
            pwlog[i].val, pwlog[i].caller);
        pwlog_r++;
    }

    while (aflog_r != aflog_w)
    {
        int i = aflog_r % AFLOG_N;
        printf("[MPU] t=%05x %s val=%d%s\n",
            aflog[i].t,
            (aflog[i].id == 0x49) ? "AF_SYSTEM " :
            (aflog[i].id == 0x4e) ? "VIDEO_MODE" : "DISPSIZE  ",
            aflog[i].val,
            aflog[i].rewrote ? " -> rewritten" : "");
        aflog_r++;
    }
}

/* from SENSOR_TIMING_TABLE (fps-engio.c) or FPS override submenu */
static int fps_main_clock = 0;
static int default_timerA[11]; /* 1080p  1080p  1080p   720p   720p   zoom   crop   crop   crop   crop   crop */
static int default_timerB[11]; /*   24p    25p    30p    50p    60p     x5    24p    25p    30p    50p    60p */
static int default_fps_1k[11] = { 23976, 25000, 29970, 50000, 59940, 29970, 23976, 25000, 29970, 50000, 59940 };

/* video modes */
/* note: zoom mode is identified by checking registers directly */

static int is_1080p()
{
    /* note: on 5D2 and 5D3 (maybe also 6D, not sure),
     * sensor configuration in photo mode is identical to 1080p.
     * other cameras may be different */
    return !is_movie_mode() || video_mode_resolution == 0;
}

static int is_720p()
{
    if (is_EOSM || is_EOSM2)
    {
        if (lv_dispsize == 1 && !RECORDING_H264)
        {
            return 1;
        }
    }

    return is_movie_mode() && video_mode_resolution == 1;
}

static int is_supported_mode()
{
    if (!lv) return 0;

    switch (crop_preset)
    {
        /* note: zoom check is also covered by check_cmos_vidmode */
        /* (we need to apply CMOS settings before PROP_LV_DISPSIZE fires) */
        // case CROP_PRESET_CENTER_Z:
        //     return 1;

        default:
            return is_1080p() || is_720p();
    }
}

static int32_t  target_yres = 0;
// static int32_t  delta_adtg0 = 0;
// static int32_t  delta_adtg1 = 0;
static int32_t  delta_head3 = 0;
static int32_t  delta_head4 = 0;
static int32_t  reg_713c = 0;
static int32_t  reg_7150 = 0;
static int32_t  reg_6014 = 0;
static int32_t  reg_6008 = 0;
static int32_t  reg_800c = 0;
static int32_t  reg_8000 = 0;
static int32_t  reg_8183 = 0;
static int32_t  reg_8184 = 0;
static int32_t  reg_timing1 = 0;
static int32_t  reg_timing2 = 0;
static int32_t  reg_timing3 = 0;
static int32_t  reg_timing4 = 0;
static int32_t  reg_timing5 = 0;
static int32_t  reg_timing6 = 0;
static int32_t  reg_6824 = 0;
static int32_t  reg_6800_height = 0;
static int32_t  reg_6800_width = 0;
static int32_t  reg_6804_height = 0;
static int32_t  reg_6804_width = 0;
static int32_t  reg_83d4 = 0;
static int32_t  reg_83dc = 0;
static uint32_t cmos1_lo = 0, cmos1_hi = 0;
static uint32_t cmos0 = 0;
static uint32_t cmos1 = 0;
static uint32_t cmos2 = 0;
static uint32_t cmos3 = 0;
static uint32_t cmos4 = 0;
static uint32_t cmos5 = 0;
static uint32_t cmos6 = 0;
static uint32_t cmos7 = 0;
static uint32_t cmos8 = 0;
static uint32_t cmos9 = 0;
static int32_t  reg_skip_left = 0;
static int32_t  reg_skip_right = 0;
static int32_t  reg_skip_top = 0;
static int32_t  reg_skip_bottom = 0;
static int32_t  reg_bl = 0;
static int32_t  reg_gain = 0;

/* helper to allow indexing various properties of Canon's video modes */
static inline int get_video_mode_index()
{
    if (lv_dispsize > 1)
    {
        return 5;
    }

    if (!is_movie_mode())
    {
        /* FIXME: some cameras may use 50p or 60p */
        return 2;
    }

    if (is_EOSM || is_EOSM2)
    {
        if (lv_dispsize == 1 && !RECORDING_H264)
        {
            /* EOS M stays in 720p30 during standby (same timer values as with 1080p30) */
            return 2;
        }
    }

    if (video_mode_crop)
    {
        /* some cameras may have various crop modes, hopefully at most one per FPS */
        return
            (video_mode_fps == 24) ?  6 :
            (video_mode_fps == 25) ?  7 :
            (video_mode_fps == 30) ?  8 :
            (video_mode_fps == 50) ?  9 :
         /* (video_mode_fps == 60) */ 10 ;
    }

    /* regular video modes */
    return
        (video_mode_fps == 24) ?  0 :
        (video_mode_fps == 25) ?  1 :
        (video_mode_fps == 30) ?  2 :
        (video_mode_fps == 50) ?  3 :
     /* (video_mode_fps == 60) */ 4 ;
}

/* optical black area sizes */
/* not sure how to adjust them from registers, so... hardcode them here */
static inline void FAST calc_skip_offsets(int * p_skip_left, int * p_skip_right, int * p_skip_top, int * p_skip_bottom)
{
    /* start from LiveView values */
    int skip_left       = 146;
    int skip_right      = 2;
    int skip_top        = 28;
    int skip_bottom     = 0;

    if (is_EOSM || is_EOSM2)
    {
    skip_left       = 72;
    skip_right      = 0;
    skip_top        = 28;
    skip_bottom     = 0;
    }

    switch (crop_preset)
    {


	case CROP_PRESET_mcm_mv1080_EOSM2:
    	if (ratios == 0x0 && x3crop == 0x0)
    	{
        skip_right = 60;
    	skip_bottom = 0;
    	}
    	if (ratios == 0x0 && x3crop == 0x1)
    	{
        skip_right = 60;
    	skip_bottom = 0;
    	}
    	if (ratios == 0x1 && x3crop == 0x0)
    	{
        skip_right = 60;
      	skip_top = 136;
        skip_bottom = 186;
    	}
    	if (ratios == 0x1 && x3crop == 0x1)
    	{
        skip_right = 0;
      	skip_top = 172;
        skip_bottom = 172;
    	}
    	if (ratios == 0x2 && x3crop == 0x0)
    	{
        skip_top = 52;
        skip_right = 60;
    	skip_bottom = 32;
    	}
    	if (ratios == 0x2 && x3crop == 0x1)
    	{
        skip_top = 84;
        skip_right = 0;
    	skip_bottom = 14;
    	}
	break;

	case CROP_PRESET_anamorphic_rewired_EOSM2:
        skip_right = 60;
    	if (ratios == 0x1)
    	{
        skip_bottom = 16;
        skip_right = 175;
        skip_left = 235;
	}
    	if (ratios == 0x2)
    	{
        skip_bottom = 16;
        skip_right = 340;
        skip_left = 400;
	}
        break;

    }

    if (p_skip_left)   *p_skip_left    = skip_left + reg_skip_left;
    if (p_skip_right)  *p_skip_right   = skip_right + reg_skip_right;
    if (p_skip_top)    *p_skip_top     = skip_top + reg_skip_top;
    if (p_skip_bottom) *p_skip_bottom  = skip_bottom + reg_skip_bottom;
}

/* to be in sync with 0xC0F06800 */
static int get_top_bar_adjustment()
{
    switch (crop_preset)
    {
        // case CROP_PRESET_FULLRES_LV:
        //     return 0;                   /* 0x10018: photo mode value, unchanged */
        // case CROP_PRESET_3x3_1X:
        // case CROP_PRESET_3x3_1X_EOSM:
        // case CROP_PRESET_3x3_1X_48p:
        //     if (is_720p()) return 28;   /* 0x1D0017 from 0x10017 */
        //     /* fall through */
        default:
            return 30;                  /* 0x1F0017 from 0x10017 */
    }
}

/* Vertical resolution from current unmodified video mode */
/* (active area only, as seen by mlv_lite) */
static inline int get_default_yres()
{
    return 
        (video_mode_fps <= 30) ? 1290 : 672;
}

/* skip_top from unmodified video mode (raw.c, LiveView skip offsets) */
static inline int get_default_skip_top()
{
    return 
        (video_mode_fps <= 30) ? 28 : 20;
}

/* max resolution for each video mode (trial and error) */
/* it's usually possible to push the numbers a few pixels further,
 * at the risk of corrupted frames */
static int max_resolutions[NUM_CROP_PRESETS][6] = {
                                /*   24p   25p   30p   50p   60p   x5 */
    [CROP_PRESET_2K_EOSM]          = { 1304, 1104,  904,  704,  504 },
    [CROP_PRESET_3K_EOSM]          = { 1304, 1104,  904,  704,  504 },
    [CROP_PRESET_4K_EOSM]          = { 3072, 3072, 2500, 1440, 1200 },
    [CROP_PRESET_3x3_mv1080_EOSM]  = { 1290, 1290, 1290,  960,  800 },
    [CROP_PRESET_3x3_mv1080_45fps_EOSM]  = { 1290, 1290, 1290,  960,  800 },
    [CROP_PRESET_anamorphic_rewired_EOSM2]  = { 1290, 1290, 1290,  960,  800 },
    [CROP_PRESET_3x1_mv720_50fps_EOSM2]  = { 1290, 1290, 1290,  960,  800 },

};

/* 5D3 vertical resolution increments over default configuration */
/* note that first scanline may be moved down by 30 px (see reg_override_top_bar) */
static inline int FAST calc_yres_delta()
{
    int desired_yres = (target_yres) ? target_yres
        : max_resolutions[crop_preset][get_video_mode_index()];

    if (desired_yres)
    {
        /* user override */
        int skip_top;
        calc_skip_offsets(0, 0, &skip_top, 0);
        int default_yres = get_default_yres();
        int default_skip_top = get_default_skip_top();
        int top_adj = get_top_bar_adjustment();
        return desired_yres - default_yres + skip_top - default_skip_top + top_adj;
    }

    ASSERT(0);
    return 0;
}

#define YRES_DELTA calc_yres_delta()


static int is_mcm_preset(int preset)
{
    return preset == CROP_PRESET_mcm_mv1080_EOSM2 ||
           preset == CROP_PRESET_anamorphic_rewired_EOSM2;
}

static int cmos_vidmode_ok = 0;

/* return value:
 *  1: registers checked and appear OK (1080p/720p video mode)
 *  0: registers checked and they are not OK (other video mode)
 * -1: registers not checked
 */
static int FAST check_cmos_vidmode(uint16_t* data_buf)
{
    int ok = 1;
    int found = 1;
    while (*data_buf != 0xFFFF)
    {
        int reg = (*data_buf) >> 12;
        int value = (*data_buf) & 0xFFF;
        
        if (is_5D3)
        {
            if (reg == 1)
            {
                found = 1;

                switch (crop_preset)
                {
                    // case CROP_PRESET_CENTER_Z:
                    // {
                    //     /* detecting the zoom mode is tricky;
                    //      * let's just exclude 1080p and 720p for now ... */
                    //     if (value == 0x800 ||
                    //         value == 0xBC2)
                    //     {
                    //         ok = 0;
                    //     }
                    //     break;
                    // }

                    default:
                    {
                        if (value != 0x800 &&   /* not 1080p? */
                            value != 0xBC2)     /* not 720p? */
                        {
                            ok = 0;
                        }
                        break;
                    }
                }
            }
        }
        
        data_buf++;
    }
    
    if (found) return ok;
    
    return -1;
}

/* pack two 6-bit values into a 12-bit one */
#define PACK12(lo,hi) ((((lo) & 0x3F) | ((hi) << 6)) & 0xFFF)

/* pack two 16-bit values into a 32-bit one */
#define PACK32(lo,hi) (((uint32_t)(lo) & 0xFFFF) | ((uint32_t)(hi) << 16))

/* pack two 16-bit values into a 32-bit one */
#define PACK32(lo,hi) (((uint32_t)(lo) & 0xFFFF) | ((uint32_t)(hi) << 16))

static void FAST cmos_hook(uint32_t* regs, uint32_t* stack, uint32_t pc)
{
    /* make sure we are in 1080p/720p mode */
    if (!is_supported_mode())
    {
        /* looks like checking properties works fine for detecting
         * changes in video mode, but not for detecting the zoom change */
        return;
    }
    
    /* also check CMOS registers; in zoom mode, we get different values
     * and this check is instant (no delays).
     * 
     * on 5D3, the 640x480 acts like 1080p during standby,
     * so properties are our only option for that one.
     */
     
    uint16_t* data_buf = (uint16_t*) regs[0];
    int ret = check_cmos_vidmode(data_buf);
    
    if (ret >= 0)
    {
        cmos_vidmode_ok = ret;
    }
    
    if (ret != 1)
    {
        return;
    }

    int cmos_new[10] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
    


    if (is_EOSM || is_EOSM2)
    {
        switch (crop_preset)
        {
			case CROP_PRESET_3x3_1X_EOSM:
                /* start/stop scanning line, very large increments */
                //cmos_new[7] = (is_6D) ? PACK12(37,10) : PACK12(6,29);
                break; 

			case CROP_PRESET_2K_EOSM:
                cmos_new[7] = 0xaa9;    /* pink highlights without this */
                break;
				
			case CROP_PRESET_3K_EOSM:
                cmos_new[5] = 0x280;             /* vertical (first|last) */
                cmos_new[7] = 0xaa9;            /* horizontal offset (mask 0xFF0) */
                break;	
			
			case CROP_PRESET_4K_EOSM:
                cmos_new[5] = 0x200;            /* vertical (first|last) */
                cmos_new[7] = 0xf20;
                break;	

			case CROP_PRESET_3x3_mv1080_EOSM:
		        case CROP_PRESET_3x3_mv1080_45fps_EOSM:
	        cmos_new[8] = 0x400; 
                break;	

			case CROP_PRESET_1x3_EOSM:
			    cmos_new[7] = 0x260;   
			    cmos_new[8] = 0x400; 
                break;	

			case CROP_PRESET_mcm_mv1080_EOSM2:
	        cmos_new[5] = 0x20;
	        cmos_new[7] = 0x800;
		if (x3crop == 0x1)
		{
	        cmos_new[5] = 0x380;
	        cmos_new[7] = 0xa6a;
		}
    		if (ratios == 0x0 && x3crop == 0x1)
    		{
	        cmos_new[5] = 0x380;
	        cmos_new[7] = 0x80b;
    		}
                break;

			case CROP_PRESET_anamorphic_rewired_EOSM2:
	        cmos_new[5] = 0x20;
		cmos_new[7] = 0x2c4; 
		break;


       
        }
    }

/* all presets */ 
	if (is_EOSM || is_EOSM2)
	{
/* hot/cold pixels. Usually 0x2. 0x34 to be tested */ 
	        cmos_new[4] = 0x34; 
	}
		
    /* menu overrides */
    if (cmos1_lo || cmos1_hi)
    {
        cmos_new[1] = PACK12(cmos1_lo,cmos1_hi);
    }

    if (cmos2)
    {
        cmos_new[2] = cmos2;
    }

    if (cmos3)
    {
        cmos_new[3] = cmos3;
    }

    if (cmos4)
    {
        cmos_new[4] = cmos4;
    }

    if (cmos5)
    {
        cmos_new[5] = cmos5;
    }

    if (cmos6)
    {
        cmos_new[6] = cmos6;
    }

    if (cmos7)
    {
        cmos_new[7] = cmos7;
    }

    if (cmos8)
    {
        cmos_new[8] = cmos8;
    }

    if (cmos9)
    {
        cmos_new[9] = cmos9;
    }


/* HDR workaround eosm */
if (((is_EOSM && is_EOSM2) && RECORDING && HDR_iso_a != 0x0))
{

/* iso reg */
        cmos_new[0] = cmos0;

  uint32_t iso_a;
  uint32_t iso_b;

  if (HDR_iso_a == 0x1) iso_a = 0x803; /* 100 */
  if (HDR_iso_a == 0x2) iso_a = 0x827; /* 200 */
  if (HDR_iso_a == 0x3) iso_a = 0x84b; /* 400 */
  if (HDR_iso_a == 0x4) iso_a = 0x86f; /* 800 */
  if (HDR_iso_a == 0x5) iso_a = 0x893; /* 1600 */
  if (HDR_iso_a == 0x6) iso_a = 0x8b7; /* 3200 */

  if (HDR_iso_b == 0x1) iso_b = 0x803;
  if (HDR_iso_b == 0x2) iso_b = 0x827;
  if (HDR_iso_b == 0x3) iso_b = 0x84b;
  if (HDR_iso_b == 0x4) iso_b = 0x86f;
  if (HDR_iso_b == 0x5) iso_b = 0x893;
  if (HDR_iso_b == 0x6) iso_b = 0x8b7;

    if (cmos0 == iso_a) 
    {
        cmos0 = iso_b;
    }
    else
    {
        cmos0 = iso_a; 
    }

}

/* HDR previewing eosm */
if (((is_EOSM && is_EOSM2) && !RECORDING && HDR_iso_a != 0x0))
{

/* iso reg */
        cmos_new[0] = cmos0;

  uint32_t iso_a;
  uint32_t iso_b;

  if (HDR_iso_a == 0x1) iso_a = 0x803; /* 100 */
  if (HDR_iso_a == 0x2) iso_a = 0x827; /* 200 */
  if (HDR_iso_a == 0x3) iso_a = 0x84b; /* 400 */
  if (HDR_iso_a == 0x4) iso_a = 0x86f; /* 800 */
  if (HDR_iso_a == 0x5) iso_a = 0x893; /* 1600 */
  if (HDR_iso_a == 0x6) iso_a = 0x8b7; /* 3200 */

  if (HDR_iso_b == 0x1) iso_b = 0x803;
  if (HDR_iso_b == 0x2) iso_b = 0x827;
  if (HDR_iso_b == 0x3) iso_b = 0x84b;
  if (HDR_iso_b == 0x4) iso_b = 0x86f;
  if (HDR_iso_b == 0x5) iso_b = 0x893;
  if (HDR_iso_b == 0x6) iso_b = 0x8b7;

    if (get_halfshutter_pressed())
    {
        cmos0 = iso_b;
    }
    else
    {
        cmos0 = iso_a; 
    }

}    
    /* copy data into a buffer, to make the override temporary */
    /* that means: as soon as we stop executing the hooks, values are back to normal */
    static uint16_t copy[512];
    uint16_t* copy_end = &copy[COUNT(copy)];
    uint16_t* copy_ptr = copy;

    while (*data_buf != 0xFFFF)
    {
        *copy_ptr = *data_buf;

        int reg = (*data_buf) >> 12;
        if (cmos_new[reg] != -1)
        {
            *copy_ptr = (reg << 12) | cmos_new[reg];
            dbg_printf("CMOS[%x] = %x\n", reg, cmos_new[reg]);
        }

        data_buf++;
        copy_ptr++;
        if (copy_ptr > copy_end) while(1);
    }
    *copy_ptr = 0xFFFF;

    /* pass our modified register list to cmos_write */
    regs[0] = (uint32_t) copy;
}

static uint32_t nrzi_encode( uint32_t in_val )
{
    uint32_t out_val = 0;
    uint32_t old_bit = 0;
    for (int num = 0; num < 31; num++)
    {
        uint32_t bit = in_val & 1<<(30-num) ? 1 : 0;
        if (bit != old_bit)
            out_val |= (1 << (30-num));
        old_bit = bit;
    }
    return out_val;
}

static uint32_t nrzi_decode( uint32_t in_val )
{
    uint32_t val = 0;
    if (in_val & 0x8000)
        val |= 0x8000;
    for (int num = 0; num < 31; num++)
    {
        uint32_t old_bit = (val & 1<<(30-num+1)) >> 1;
        val |= old_bit ^ (in_val & 1<<(30-num));
    }
    return val;
}

static int FAST adtg_lookup(uint32_t* data_buf, int reg_needle)
{
    while(*data_buf != 0xFFFFFFFF)
    {
        int reg = (*data_buf) >> 16;
        if (reg == reg_needle)
        {
            return *(uint16_t*)data_buf;
        }
    }
    return -1;
}

/* adapted from fps_override_shutter_blanking in fps-engio.c */
static int adjust_shutter_blanking(int old)
{
    /* sensor duty cycle: range 0 ... timer B */
    int current_blanking = nrzi_decode(old);

    int video_mode = get_video_mode_index();

    /* what value Canon firmware assumes for timer B? */
    int fps_timer_b_orig = default_timerB[video_mode];

    int current_exposure = fps_timer_b_orig - current_blanking;
    
    /* wrong assumptions? */
    if (current_exposure < 0)
    {
        return old;
    }

    int default_fps = default_fps_1k[video_mode];
    int current_fps = fps_get_current_x1000();

    dbg_printf("FPS %d->%d\n", default_fps, current_fps);

    float frame_duration_orig = 1000.0 / default_fps;
    float frame_duration_current = 1000.0 / current_fps;

    float orig_shutter = frame_duration_orig * current_exposure / fps_timer_b_orig;

    float new_shutter =
        (shutter_range == 0) ?
        ({
            /* original shutter speed from the altered video mode */
            orig_shutter;
        }) :
        ({
            /* map the available range of 1/4000...1/30 (24-30p) or 1/4000...1/60 (50-60p)
             * from minimum allowed (1/15000 with full-res LV) to 1/fps */
            int max_fps_shutter = (video_mode_fps <= 30) ? 33333 : 64000;
            int default_fps_adj = 1e9 / (1e9 / max_fps_shutter - 250);
            (orig_shutter - 250e-6) * default_fps_adj / current_fps;
        });

    /* what value is actually used for timer B? (possibly after our overrides) */
    int fps_timer_b = (shamem_read(0xC0F06014) & 0xFFFF) + 1;

    dbg_printf("Timer B %d->%d\n", fps_timer_b_orig, fps_timer_b);

    int new_exposure = new_shutter * fps_timer_b / frame_duration_current;
    int new_blanking = COERCE(fps_timer_b - new_exposure, 10, fps_timer_b - 2);

    dbg_printf("Exposure %d->%d (timer B units)\n", current_exposure, new_exposure);

#ifdef CROP_DEBUG
    float chk_shutter = frame_duration_current * new_exposure / fps_timer_b;
    dbg_printf("Shutter %d->%d us\n", (int)(orig_shutter*1e6), (int)(chk_shutter*1e6));
#endif

    dbg_printf("Blanking %d->%d\n", current_blanking, new_blanking);

    return nrzi_encode(new_blanking);
}

extern void fps_override_shutter_blanking();

static void FAST adtg_hook(uint32_t* regs, uint32_t* stack, uint32_t pc)
{
    if (!is_supported_mode() || !cmos_vidmode_ok)
    {
        /* don't patch other video modes */
        return;
    }

    // if (!is_720p())
    // {
    //     if (crop_preset == CROP_PRESET_3x3_1X ||
    //         crop_preset == CROP_PRESET_3x3_1X_48p)
    //     {
    //         /* these presets only have effect in 720p mode */
    //         return;
    //     }
    // }

    /* This hook is called from the DebugMsg's in adtg_write,
     * so if we change the register list address, it won't be able to override them.
     * Workaround: let's call it here. */
    fps_override_shutter_blanking();

    uint32_t cs = regs[0];
    uint32_t *data_buf = (uint32_t *) regs[1];
    int dst = cs & 0xF;
    
    /* copy data into a buffer, to make the override temporary */
    /* that means: as soon as we stop executing the hooks, values are back to normal */
    static uint32_t copy[512];
    uint32_t* copy_end = &copy[COUNT(copy)];
    uint32_t* copy_ptr = copy;
    
    struct adtg_new
    {
        int dst;
        int reg;
        int val;
    };
    
    /* expand this as required */
    struct adtg_new adtg_new[40] = {{0}};

    const int blanking_reg_zoom   = (is_5D3) ? 0x805E : 0x805F;
    const int blanking_reg_nozoom = (is_5D3) ? 0x8060 : 0x8061;
    const int blanking_reg = (lv_dispsize == 1) ? blanking_reg_nozoom : blanking_reg_zoom;

    /* scan for shutter blanking and make both zoom and non-zoom value equal */
    /* (the values are different when using FPS override with ADTG shutter override) */
    /* (fixme: might be better to handle this in ML core?) */
    int shutter_blanking = 0;
    for (uint32_t * buf = data_buf; *buf != 0xFFFFFFFF; buf++)
    {
        int reg = (*buf) >> 16;
        if (reg == blanking_reg)
        {
            int val = (*buf) & 0xFFFF;
            shutter_blanking = val;
        }
    }

    /* some modes may need adjustments to maintain exposure */
    if (shutter_blanking)
    {
        /* FIXME: remove this kind of hardcoded conditions */
        if ((lv_dispsize == 1))
        {
            shutter_blanking = adjust_shutter_blanking(shutter_blanking);
        }
    }

    /* should probably be made generic */
    if (is_5D3 || is_EOSM || is_EOSM2)
    {
        /* all modes may want to override shutter speed */
        /* ADTG[0x8060]: shutter blanking for 3x3 mode  */
        /* ADTG[0x805E]: shutter blanking for zoom mode  */
        adtg_new[0] = (struct adtg_new) {6, 0x8060, shutter_blanking};
        adtg_new[1] = (struct adtg_new) {6, 0x805E, shutter_blanking};


/* always disable Movie crop mode if using crop_rec presets, except for mcm mode, Only eosm and 100D */
 if (is_EOSM || is_EOSM2)
 {
/* always disable Movie crop mode if using crop_rec presets, except for mcm mode */
    if (crop_preset == CROP_PRESET_mcm_mv1080_EOSM2 || crop_preset == CROP_PRESET_anamorphic_rewired_EOSM2)
    {
     if (is_EOSM || is_EOSM2) movie_crop_hack_enable();
    } 
    else 
    {
     if (is_EOSM || is_EOSM2) movie_crop_hack_disable();
    }

 }

	  /* only apply bit reducing while recording, not while idle */
    	  if ((RECORDING && (is_EOSM || is_EOSM2 || is_5D3)) || (!is_EOSM && !is_EOSM2 && !is_5D3))
	  {
   		if (bitdepth == 0x1)
    		{
		/* 8bit roundtrip only not applied here with following set ups */
		adtg_new[13] = (struct adtg_new) {6, 0x8882, 12 + reg_gain}; 
                adtg_new[14] = (struct adtg_new) {6, 0x8884, 12 + reg_gain};
                adtg_new[15] = (struct adtg_new) {6, 0x8886, 12 + reg_gain};
                adtg_new[16] = (struct adtg_new) {6, 0x8888, 12 + reg_gain};
		}

   		if (bitdepth == 0x2)
    		{
		/* 9bit roundtrip only not applied here with following set ups */
		adtg_new[13] = (struct adtg_new) {6, 0x8882, 30 + reg_gain}; 
                adtg_new[14] = (struct adtg_new) {6, 0x8884, 30 + reg_gain};
                adtg_new[15] = (struct adtg_new) {6, 0x8886, 30 + reg_gain};
                adtg_new[16] = (struct adtg_new) {6, 0x8888, 30 + reg_gain};
		}

   		if (bitdepth == 0x3)
    		{
		/* 10bit roundtrip only not applied here with following set ups */
		adtg_new[13] = (struct adtg_new) {6, 0x8882, 60 + reg_gain}; 
                adtg_new[14] = (struct adtg_new) {6, 0x8884, 60 + reg_gain};
                adtg_new[15] = (struct adtg_new) {6, 0x8886, 60 + reg_gain};
                adtg_new[16] = (struct adtg_new) {6, 0x8888, 60 + reg_gain};
		}

    		if (bitdepth == 0x4)
    		{
		/* 12bit roundtrip only not applied here with following set ups */
		adtg_new[13] = (struct adtg_new) {6, 0x8882, 250 + reg_gain}; 
                adtg_new[14] = (struct adtg_new) {6, 0x8884, 250 + reg_gain};
                adtg_new[15] = (struct adtg_new) {6, 0x8886, 250 + reg_gain};
                adtg_new[16] = (struct adtg_new) {6, 0x8888, 250 + reg_gain};
		}

	  }


    }

    /* all modes may want to override shutter speed */
    /* ADTG[0x8060/61]: shutter blanking for 3x3 mode  */
    /* ADTG[0x805E/5F]: shutter blanking for zoom mode  */
    adtg_new[0] = (struct adtg_new) {6, blanking_reg_nozoom, shutter_blanking};
    adtg_new[1] = (struct adtg_new) {6, blanking_reg_zoom, shutter_blanking};

    /* hopefully generic; to be tested later */
    if (1)
    {
        switch (crop_preset)
        {
            /* 3x3 binning in 720p (in 1080p it's already 3x3) */
            case CROP_PRESET_3x3_1X_EOSM:
                /* ADTG2/4[0x800C] = 2: vertical binning factor = 3 */
                adtg_new[2] = (struct adtg_new) {6, 0x800C, 2};
                break;




	     case CROP_PRESET_3x3_mv1080_EOSM:
  	     case CROP_PRESET_3x3_mv1080_45fps_EOSM:
		adtg_new[0] = (struct adtg_new) {6, 0x800C, 2};
		break;

	     case CROP_PRESET_1x3_EOSM:
	        adtg_new[0] = (struct adtg_new) {6, 0x800C, 0};
     	        break;

	     case CROP_PRESET_mcm_mv1080_EOSM2:
		adtg_new[2] = (struct adtg_new) {6, 0x800C, 2};
                adtg_new[3] = (struct adtg_new) {6, 0x8000, 6};
                adtg_new[17] = (struct adtg_new) {6, 0x8183, 0x21};
                adtg_new[18] = (struct adtg_new) {6, 0x8184, 0x7b};
		if (x3crop == 0x1)
		{	
		adtg_new[2] = (struct adtg_new) {6, 0x800C, 0 + reg_800c};
                adtg_new[3] = (struct adtg_new) {6, 0x8000, 5 + reg_8000};
		}	
		break;

	     case CROP_PRESET_anamorphic_rewired_EOSM2: 
	        adtg_new[2] = (struct adtg_new) {6, 0x800C, 0 + reg_800c};
                adtg_new[3] = (struct adtg_new) {6, 0x8000, 6};
                adtg_new[17] = (struct adtg_new) {6, 0x8183, 0x21 + reg_8183};
                adtg_new[18] = (struct adtg_new) {6, 0x8184, 0x7b + reg_8184};
     	        break;

  	     case CROP_PRESET_3x1_mv720_50fps_EOSM2:
		adtg_new[2] = (struct adtg_new) {6, 0x800C, 4 + reg_800c};
                adtg_new[3] = (struct adtg_new) {6, 0x8000, 6 + reg_8000};
		break;


        }
    }

    /* these should work on all presets, on all DIGIC 5 models and also on recent DIGIC 4 */
    if ((1) /*&& !(CROP_PRESET_MENU == CROP_PRESET_mv1080p_mv720p_100D) && !(CROP_PRESET_MENU == CROP_PRESET_3xcropmode_100D)*/)
    {
        /* assuming FPS timer B was overridden before this */
        int fps_timer_b = (shamem_read(0xC0F06014) & 0xFFFF);
        int readout_end = shamem_read(0xC0F06804) >> 16;

        /* PowerSaveTiming registers */
        /* after readout is finished, we can turn off the sensor until the next frame */
        /* we could also set these to 0; it will work, but the sensor will run a bit hotter */
        /* to be tested to find out exactly how much */
        adtg_new[4]  = (struct adtg_new) {6, 0x8172, nrzi_encode(readout_end + 1) }; /* PowerSaveTiming ON (6D/700D) */
        adtg_new[5]  = (struct adtg_new) {6, 0x8178, nrzi_encode(readout_end + 1) }; /* PowerSaveTiming ON (5D3/6D/700D) */
        adtg_new[6]  = (struct adtg_new) {6, 0x8196, nrzi_encode(readout_end + 1) }; /* PowerSaveTiming ON (5D3) */

        adtg_new[7]  = (struct adtg_new) {6, 0x8173, nrzi_encode(fps_timer_b - 1) }; /* PowerSaveTiming OFF (6D/700D) */
        adtg_new[8]  = (struct adtg_new) {6, 0x8179, nrzi_encode(fps_timer_b - 5) }; /* PowerSaveTiming OFF (5D3/6D/700D) */
        adtg_new[9]  = (struct adtg_new) {6, 0x8197, nrzi_encode(fps_timer_b - 5) }; /* PowerSaveTiming OFF (5D3) */

        adtg_new[10] = (struct adtg_new) {6, 0x82B6, nrzi_encode(readout_end - 1) }; /* PowerSaveTiming ON? (700D); 2 units below the "ON" timing from above */

        /* ReadOutTiming registers */
        /* these shouldn't be 0, as they affect the image */
        adtg_new[11] = (struct adtg_new) {6, 0x82F8, nrzi_encode(readout_end + 1) }; /* ReadOutTiming */
        adtg_new[12] = (struct adtg_new) {6, 0x82F9, nrzi_encode(fps_timer_b - 1) }; /* ReadOutTiming end? */
    }

    while(*data_buf != 0xFFFFFFFF)
    {
        *copy_ptr = *data_buf;
        int reg = (*data_buf) >> 16;
        for (int i = 0; i < COUNT(adtg_new); i++)
        {
            if ((reg == adtg_new[i].reg) && (dst & adtg_new[i].dst))
            {
                int new_value = adtg_new[i].val;
                dbg_printf("ADTG%x[%x] = %x\n", dst, reg, new_value);
                *(uint16_t*)copy_ptr = new_value;

                if (reg == blanking_reg_zoom || reg == blanking_reg_nozoom)
                {
                    /* also override in original data structure */
                    /* to be picked up on the screen indicators */
                    *(uint16_t*)data_buf = new_value;
                }
            }
        }
        data_buf++;
        copy_ptr++;
        if (copy_ptr >= copy_end) while(1);
    }
    *copy_ptr = 0xFFFFFFFF;
    
    /* pass our modified register list to adtg_write */
    regs[1] = (uint32_t) copy;
}

/* changing bits */
static inline uint32_t reg_override_bits(uint32_t reg, uint32_t old_val)
{

/* only apply bit reducing while recording, not while idle */
if ((RECORDING && (is_EOSM || is_EOSM2 || is_5D3)) || (!is_EOSM && !is_EOSM2 && !is_5D3))
{
  if (bitdepth == 0x1)
  {
    switch (reg)
    {
	/* correct liveview brightness */
	case 0xC0F42744: return 0x6060606;
    }
  }

  if (bitdepth == 0x2)
  {
    switch (reg)
    {
	/* correct liveview brightness */
	case 0xC0F42744: return 0x5050505;
    }
  }

  if (bitdepth == 0x3)
  {
    switch (reg)
    {
	/* correct liveview brightness */
	case 0xC0F42744: return 0x4040404;
    }
  }
  if (bitdepth == 0x4)
  {
    switch (reg)
    {
	/* correct liveview brightness */
	case 0xC0F42744: return 0x2020202;
    }
  }
}


if (RECORDING && bitdepth != 0x0 && (is_EOSM || is_EOSM2))
{
/* correcting black level a bit. Compensating greenish tint. Only affects preview, not recordings */
        if (lens_info.raw_iso != 0x48) /* iso 100 excluded, breaks */
        {
	EngDrvOutLV(0xc0f37aec, 0x73ca + reg_bl);
	EngDrvOutLV(0xc0f37af8, 0x73ca + reg_bl);
	EngDrvOutLV(0xc0f37b04, 0x73ca + reg_bl); 
	EngDrvOutLV(0xc0f37ae0, 0x73ca + reg_bl); 
	}

}

if (!RECORDING && (is_EOSM || is_EOSM2 || is_5D3))
{

  if (bitdepth == 0x1)
  {
    switch (reg)
    {
	case 0xc0f0815c: return 0x3;
    }
  }

  if (bitdepth == 0x2)
  {
    switch (reg)
    {
	case 0xc0f0815c: return 0x4;
    }
  }

  if (bitdepth == 0x3)
  {
    switch (reg)
    {
	case 0xc0f0815c: return 0x5;
    }
  }

  if (bitdepth == 0x4)
  {
    switch (reg)
    {
	case 0xc0f0815c: return 0x6;
    }
  }

/* reset eosm switch */
  if (bitdepth == 0x0 && (is_EOSM || is_EOSM2 || is_5D3))
  {
    switch (reg)
    {
	case 0xc0f0815c: return 0x2;
    }
  }
}

if (is_EOSM)
{
    switch (reg)
    {
/* not used but might be in the future */
	case 0xC0F06800: return 0x10010 + reg_6800_width + (reg_6800_height << 16);
    }

/* HDR flag */
   if (HDR_iso_a != 0x0)
   {
  	if (HDR_iso_a == 0x1) switch (reg) case 0xC0F0b12c: return 0x1;
  	if (HDR_iso_a == 0x2) switch (reg) case 0xC0F0b12c: return 0x2;
  	if (HDR_iso_a == 0x3) switch (reg) case 0xC0F0b12c: return 0x3;
  	if (HDR_iso_a == 0x4) switch (reg) case 0xC0F0b12c: return 0x4;
  	if (HDR_iso_a == 0x5) switch (reg) case 0xC0F0b12c: return 0x5;
  	if (HDR_iso_a == 0x6) switch (reg) case 0xC0F0b12c: return 0x6;
   }

   if (HDR_iso_a == 0x0)
   {
       switch (reg)
       {
	   case 0xC0F0b12c: return 0x0;
       }
   }

}
    return 0;
}

/* this is used to cover the black bar at the top of the image in 1:1 modes */
/* (used in most other presets) */
static inline uint32_t reg_override_top_bar(uint32_t reg, uint32_t old_val)
{

/* if changing bitdepth */
  if (bitdepth == 0x1)
  {
    switch (reg)
    {
	/* correct liveview brightness */
	case 0xC0F42744: return 0x6060606;
    }
  }

  if (bitdepth == 0x2)
  {
    switch (reg)
    {
	/* correct liveview brightness */
	case 0xC0F42744: return 0x5050505;
    }
  }

  if (bitdepth == 0x3)
  {
    switch (reg)
    {
	/* correct liveview brightness */
	case 0xC0F42744: return 0x4040404;
    }
  }
  if (bitdepth == 0x4)
  {
    switch (reg)
    {
	/* correct liveview brightness */
	case 0xC0F42744: return 0x2020202;
    }
  }

    switch (reg)
    {
        /* raw start line/column */
        /* move start line down by 30 pixels */
        /* not sure where this offset comes from */
        case 0xC0F06800:
            return 0x1F0017;
    }

    return 0;
}

/* these are required for increasing vertical resolution */
/* (used in most other presets) */
static inline uint32_t reg_override_HEAD34(uint32_t reg, uint32_t old_val)
{
    switch (reg)
    {
        /* HEAD3 timer */
        case 0xC0F0713C:
            return old_val + YRES_DELTA + delta_head3;

        /* HEAD4 timer */
        case 0xC0F07150:
            return old_val + YRES_DELTA + delta_head4;
    }

    return 0;
}

static inline uint32_t reg_override_common(uint32_t reg, uint32_t old_val)
{
    uint32_t a = reg_override_top_bar(reg, old_val);
    if (a) return a;

    uint32_t b = reg_override_HEAD34(reg, old_val);
    if (b) return b;

    return 0;
}

static inline uint32_t reg_override_fps(uint32_t reg, uint32_t timerA, uint32_t timerB, uint32_t old_val)
{
    /* hardware register requires timer-1 */
    timerA--;
    timerB--;

    /* only override FPS registers if the old value is what we expect
     * otherwise we may be in some different video mode for a short time
     * this race condition is enough to lock up LiveView in some cases
     * e.g. 5D3 3x3 50/60p when going from photo mode to video mode
     */

    switch (reg)
    {
        case 0xC0F06824:
        case 0xC0F06828:
        case 0xC0F0682C:
        case 0xC0F06830:
        case 0xC0F06010:
        {
            uint32_t expected = default_timerA[get_video_mode_index()] - 1;

            if (old_val == expected || old_val == expected + 1)
            {
                return timerA;
            }

            break;
        }
        
        case 0xC0F06008:
        case 0xC0F0600C:
        {
            uint32_t expected = default_timerA[get_video_mode_index()] - 1;
            expected |= (expected << 16);

            if (old_val == expected || old_val == expected + 0x00010001)
            {
                return timerA | (timerA << 16);
            }

            break;
        }

        case 0xC0F06014:
        {
            uint32_t expected = default_timerB[get_video_mode_index()] - 1;

            if (old_val == expected || old_val == expected + 1)
            {
                return timerB;
            }

            break;
        }
    }

    return 0;
}

/* Values for EOSM */
static inline uint32_t reg_override_2K_eosm(uint32_t reg, uint32_t old_val)
{

    switch (reg)
    {
        /* raw resolution (end line/column) */
        /* X: (3072+140)/8 + 0x17, adjusted for 3072 in raw_rec */
        case 0xC0F06804: return 0x5390298; /* 2520x1304  x5 Mode; */

        case 0xC0F07150: return 0x428;
        case 0xC0F0713c: return 0x535;
    }

    return reg_override_bits(reg, old_val);
}

static inline uint32_t reg_override_3K_eosm(uint32_t reg, uint32_t old_val)
{

    switch (reg)
    {
        case 0xC0F06804: return 0x5b90318; // 3032x1436  x5 Mode;

        case 0xC0F06824: return 0x3ca;
        case 0xC0F06828: return 0x3ca;
        case 0xC0F0682C: return 0x3ca;
        case 0xC0F06830: return 0x3ca;

        case 0xC0F06010: return 0x34b;
        case 0xC0F06008: return 0x34b034b;
        case 0xC0F0600C: return 0x34b034b;

        case 0xC0F06014: return 0x62c;
        case 0xC0F0713c: return 0x5b9;
    }

    return reg_override_bits(reg, old_val);
}

static inline uint32_t reg_override_4K_eosm(uint32_t reg, uint32_t old_val)
{

    switch (reg)
    {
        /* raw resolution (end line/column) */
        /* X: (3072+140)/8 + 0x17, adjusted for 3072 in raw_rec */
        case 0xC0F06804: return 0xa1b0412; // 4032x2558  x5 Mode;

        case 0xC0F06824: return 0x4ca;
        case 0xC0F06828: return 0x4ca;
        case 0xC0F0682C: return 0x4ca;
        case 0xC0F06830: return 0x4ca;

        case 0xC0F06010: return 0x45b;
        case 0xC0F06008: return 0x45b045b;
        case 0xC0F0600C: return 0x45b045b;

        case 0xC0F06014: return 0xbd4;
        case 0xC0F0713c: return 0xA55;
    }

    return reg_override_bits(reg, old_val);
}

static inline uint32_t reg_override_3x3_eosm(uint32_t reg, uint32_t old_val)
{

    switch (reg)
    {
        	case 0xC0F06804: return 0x4a601d4;
		case 0xC0F37014: return 0xe;
        	case 0xC0F0713c: return 0x4a7;
		case 0xC0F07150: return 0x475;
    }

    return reg_override_bits(reg, old_val);
}


static inline uint32_t reg_override_3x3_45fps_eosm(uint32_t reg, uint32_t old_val)
{
    switch (reg)
    {
        	case 0xC0F06804: return 0x4ae01d4;
		case 0xC0F37014: return 0xe;
        	case 0xC0F0713c: return 0x4ae;
		case 0xC0F07150: return 0x440;
      		case 0xC0F06014: return 0x53f;
    }

    return reg_override_bits(reg, old_val);
}

static inline uint32_t reg_override_1x3_eosm(uint32_t reg, uint32_t old_val)
{
    switch (reg)
    {
        	case 0xC0F06804: return 0x4a601d4;

        	case 0xC0F06014: return 0x9df;
		case 0xC0F0600c: return 0x20f020f;
		case 0xC0F06008: return 0x20f020f;
		case 0xC0F06010: return 0x20f;

		case 0xC0F37014: return 0xe;
        	case 0xC0F0713c: return 0x4a7;
		case 0xC0F07150: return 0x475;

    }

    return reg_override_bits(reg, old_val);
}



static inline uint32_t reg_override_mcm_mv1080_eosm2(uint32_t reg, uint32_t old_val)
{

/* gets rid of the black border to the right */
	EngDrvOutLV(0xc0f383d4, 0x4f0010 + reg_83d4);
	EngDrvOutLV(0xc0f383dc, 0x42401c6 + reg_83dc);

if ((ratios == 0x0 && x3crop == 0x0) || (ratios == 0x0 && x3crop == 0x1))
{
    switch (reg)
    {
          	case 0xC0F06804: return 0x4a601ed + reg_6804_width + (reg_6804_height << 16);
        	case 0xC0F0713c: return 0x4a7 + reg_713c;
		case 0xC0F07150: return 0x430 + reg_7150;
    }
}

if ((ratios == 0x1 || ratios == 0x2) && x3crop == 0x0)
{
    switch (reg)
    {
          	case 0xC0F06804: return 0x4a601ed + reg_6804_width + (reg_6804_height << 16);
        	case 0xC0F0713c: return 0x4a7 + reg_713c;
		case 0xC0F07150: return 0x430 + reg_7150;

/* testing above for the sake of map files */
             // case 0xC0F06804: return 0x42401ed + reg_6804_width + (reg_6804_height << 16);
             // case 0xC0F0713c: return 0x425 + reg_713c;
	     // case 0xC0F07150: return 0x3ae + reg_7150;
    }
}

/* x3crop 2.35:1 */
if (ratios == 0x1 && x3crop == 0x1)
{
    switch (reg)
    {
          	case 0xC0F06804: return 0x45601ed + reg_6804_width + (reg_6804_height << 16);
        	case 0xC0F0713c: return 0x457 + reg_713c;
		case 0xC0F07150: return 0x3e0 + reg_7150;
    }
}

/* x3crop 16:9 */
if (ratios == 0x2 && x3crop == 0x1)
{
    switch (reg)
    {
          	case 0xC0F06804: return 0x45601ed + reg_6804_width + (reg_6804_height << 16);
        	case 0xC0F0713c: return 0x457 + reg_713c;
		case 0xC0F07150: return 0x3e0 + reg_7150;
    }
}


if (set_25fps == 0x1)
{
    switch (reg)
    {
        	case 0xC0F06014: return set_25fps == 0x1 ? 0x98c - 101 + reg_6014: 0x98c + reg_6014;
		case 0xC0F0600c: return 0x2210221 + reg_6008 + (reg_6008 << 16);
		case 0xC0F06008: return 0x2210221 + reg_6008 + (reg_6008 << 16);
		case 0xC0F06010: return 0x221 + reg_6008;

        	case 0xC0F06824: return 0x222 + reg_6824;
        	case 0xC0F06828: return 0x222 + reg_6824;
        	case 0xC0F0682C: return 0x222 + reg_6824;
        	case 0xC0F06830: return 0x222 + reg_6824;
     }
}

    switch (reg)
    {
/* reset dummy reg in raw.c */
	case 0xC0f0b13c: return 0xf;
/* cinema cropmarks in mlv_lite.c. Detection reg */
	case 0xc0f0b134: return ratios == 0x1 ? 0x5: 0x4;
    }


/* 48fps preset 1496x838(16:9). Well, not really useful. Only framing preview, lagging. Let´s keep it for future work
    switch (reg)
    {         	case 0xC0F06804: return 0x36601a8 + reg_6804_width + (reg_6804_height << 16);
        	case 0xC0F0713c: return 0x366 + reg_713c;
		case 0xC0F07150: return 0x300 + reg_7150;

        	case 0xC0F06014: return 0x4fc + reg_6014;
		case 0xC0F0600c: return 0x2090209 + reg_6008 + (reg_6008 << 16);
		case 0xC0F06008: return 0x2090209 + reg_6008 + (reg_6008 << 16);
		case 0xC0F06010: return 0x209 + reg_6008;

        	case 0xC0F06824: return 0x206 + reg_6824;
        	case 0xC0F06828: return 0x206 + reg_6824;
        	case 0xC0F0682C: return 0x206 + reg_6824;
        	case 0xC0F06830: return 0x206 + reg_6824;
    }
*/


    return reg_override_bits(reg, old_val);
}

static inline uint32_t reg_override_anamorphic_rewired_eosm2(uint32_t reg, uint32_t old_val)
{
/* gets rid of the black border to the right. Connected to mlv_lite which takes over these regs while recording */
	if (!RECORDING)
	{
	EngDrvOutLV(0xc0f383d4, 0x4f0010 + reg_83d4);
	EngDrvOutLV(0xc0f383dc, 0x42401c6 + reg_83dc);
	}

  if ((ratios != 0x1) && (ratios != 0x2))
  {

    switch (reg)
    {
		case 0xC0F06804: return 0x79f01ed + reg_6804_width + (reg_6804_height << 16);

        	case 0xC0F06014: return set_25fps == 0x1 ? 0x89e + reg_6014: 0x8a1 + reg_6014;
		case 0xC0F0600c: return set_25fps == 0x1 ? 0x25b025b - 24 + reg_6008 + (reg_6008 << 16): 0x25b025b + reg_6008 + (reg_6008 << 16);
		case 0xC0F06008: return set_25fps == 0x1 ? 0x25b025b - 24 + reg_6008 + (reg_6008 << 16): 0x25b025b + reg_6008 + (reg_6008 << 16);
		case 0xC0F06010: return set_25fps == 0x1 ? 0x25b - 24 + reg_6008: 0x25b + reg_6008;

        	case 0xC0F0713c: return 0x79f + reg_713c;
		case 0xC0F07150: return 0x36c + reg_7150;

        	case 0xC0F06824: return 0x722 + reg_6824;
        	case 0xC0F06828: return 0x722 + reg_6824;
        	case 0xC0F0682C: return 0x722 + reg_6824;
        	case 0xC0F06830: return 0x722 + reg_6824;

/* dummy reg for height modes eosm in raw.c */
		case 0xC0f0b13c: return 0xd;

     }

   }


  if (ratios == 0x1)
  {
/* 2.35:1 */
    switch (reg)
    {
		case 0xC0F06804: return 0x77701ed + reg_6804_width + (reg_6804_height << 16);

        	case 0xC0F06014: return set_25fps == 0x1 ? 0x89e + reg_6014: 0x8a1 + reg_6014;
		case 0xC0F0600c: return set_25fps == 0x1 ? 0x25b025b - 24 + reg_6008 + (reg_6008 << 16): 0x25b025b + reg_6008 + (reg_6008 << 16);
		case 0xC0F06008: return set_25fps == 0x1 ? 0x25b025b - 24 + reg_6008 + (reg_6008 << 16): 0x25b025b + reg_6008 + (reg_6008 << 16);
		case 0xC0F06010: return set_25fps == 0x1 ? 0x25b - 24 + reg_6008: 0x25b + reg_6008;

        	case 0xC0F0713c: return 0x777 + reg_713c;
		case 0xC0F07150: return 0x36c + reg_7150;

        	case 0xC0F06824: return 0x722 + reg_6824;
        	case 0xC0F06828: return 0x722 + reg_6824;
        	case 0xC0F0682C: return 0x722 + reg_6824;
        	case 0xC0F06830: return 0x722 + reg_6824;

/* dummy reg for height modes eosm in raw.c */
		case 0xC0f0b13c: return 0xd;

     }

   }

  if (ratios == 0x2)
  {
/* 16:9 */
    switch (reg)
    {
		case 0xC0F06804: return 0x79f01ed + reg_6804_width + (reg_6804_height << 16);

        	case 0xC0F06014: return set_25fps == 0x1 ? 0x89e + reg_6014: 0x8a1 + reg_6014;
		case 0xC0F0600c: return set_25fps == 0x1 ? 0x25b025b - 24 + reg_6008 + (reg_6008 << 16): 0x25b025b + reg_6008 + (reg_6008 << 16);
		case 0xC0F06008: return set_25fps == 0x1 ? 0x25b025b - 24 + reg_6008 + (reg_6008 << 16): 0x25b025b + reg_6008 + (reg_6008 << 16);
		case 0xC0F06010: return set_25fps == 0x1 ? 0x25b - 24 + reg_6008: 0x25b + reg_6008;

        	case 0xC0F0713c: return 0x79f + reg_713c;
		case 0xC0F07150: return 0x36c + reg_7150;

        	case 0xC0F06824: return 0x722 + reg_6824;
        	case 0xC0F06828: return 0x722 + reg_6824;
        	case 0xC0F0682C: return 0x722 + reg_6824;
        	case 0xC0F06830: return 0x722 + reg_6824;

/* dummy reg for height modes eosm in raw.c */
		case 0xC0f0b13c: return 0xd;
     }

   }

    return reg_override_bits(reg, old_val);
}

static inline uint32_t reg_override_3x1_mv720_50fps_eosm2(uint32_t reg, uint32_t old_val)
{

    switch (reg)
    {
        	case 0xC0F06804: return 0x2d801d7 + reg_6804_width + (reg_6804_height << 16);
        	case 0xC0F0713c: return 0x2d8 + reg_713c;
		case 0xC0F07150: return 0x2d0 + reg_7150;

	     /* 50 fps */
      	     	case 0xC0F06014: return 0x4aa + reg_6014;
		case 0xC0F0600c: return 0x2170217 + reg_6008 + (reg_6008 << 16);
		case 0xC0F06008: return 0x2170217 + reg_6008 + (reg_6008 << 16);
		case 0xC0F06010: return 0x217 + reg_6008;

		case 0xC0F06824: return 0x206 + reg_6824;
		case 0xC0F06828: return 0x206 + reg_6824;
		case 0xC0F0682c: return 0x206 + reg_6824;
		case 0xC0F06830: return 0x206 + reg_6824;

	/* reset dummy reg in raw.c */
	case 0xC0f0b13c: return 0xf;
    }

    return reg_override_bits(reg, old_val);
}




static inline uint32_t reg_override_fps_nocheck(uint32_t reg, uint32_t timerA, uint32_t timerB, uint32_t old_val)
{
    /* hardware register requires timer-1 */
    timerA--;
    timerB--;

    switch (reg)
    {
        case 0xC0F06824:
        case 0xC0F06828:
        case 0xC0F0682C:
        case 0xC0F06830:
        case 0xC0F06010:
        {
            return timerA;
        }
        
        case 0xC0F06008:
        case 0xC0F0600C:
        {
            return timerA | (timerA << 16);
        }

        case 0xC0F06014:
        {
            return timerB;
        }
    }

    return 0;
}




static inline uint32_t reg_override_zoom_fps(uint32_t reg, uint32_t old_val)
{
    /* attempt to reconfigure the x5 zoom at the FPS selected in Canon menu */
    int timerA = 
        (video_mode_fps == 24) ? 512 :
        (video_mode_fps == 25) ? 512 :
        (video_mode_fps == 30) ? 520 :
        (video_mode_fps == 50) ? 512 :  /* cannot get 50, use 25 */
        (video_mode_fps == 60) ? 520 :  /* cannot get 60, use 30 */
                                  -1 ;
    int timerB =
        (video_mode_fps == 24) ? 1955 :
        (video_mode_fps == 25) ? 1875 :
        (video_mode_fps == 30) ? 1540 :
        (video_mode_fps == 50) ? 1875 :
        (video_mode_fps == 60) ? 1540 :
                                   -1 ;

/* if changing bitdepth */
  if (bitdepth == 0x1)
  {
    switch (reg)
    {
	/* correct liveview brightness */
	case 0xC0F42744: return 0x6060606;
    }
  }

  if (bitdepth == 0x2)
  {
    switch (reg)
    {
	/* correct liveview brightness */
	case 0xC0F42744: return 0x5050505;
    }
  }

  if (bitdepth == 0x3)
  {
    switch (reg)
    {
	/* correct liveview brightness */
	case 0xC0F42744: return 0x4040404;
    }
  }
  if (bitdepth == 0x4)
  {
    switch (reg)
    {
	/* correct liveview brightness */
	case 0xC0F42744: return 0x2020202;
    }
  }

    return reg_override_fps_nocheck(reg, timerA, timerB, old_val);
}

static int engio_vidmode_ok = 0;

static void * get_engio_reg_override_func()
{
    uint32_t (*reg_override_func)(uint32_t, uint32_t) = 
        (crop_preset == CROP_PRESET_2K_EOSM)         ? reg_override_2K_eosm         :
        (crop_preset == CROP_PRESET_3K_EOSM)         ? reg_override_3K_eosm         : 
        (crop_preset == CROP_PRESET_4K_EOSM) 	     ? reg_override_4K_eosm         :
        (crop_preset == CROP_PRESET_3x3_mv1080_EOSM) ? reg_override_3x3_eosm        :
        (crop_preset == CROP_PRESET_3x3_mv1080_45fps_EOSM) ? reg_override_3x3_45fps_eosm        :
        (crop_preset == CROP_PRESET_1x3_EOSM) ? reg_override_1x3_eosm        :
        (crop_preset == CROP_PRESET_mcm_mv1080_EOSM2) ? reg_override_mcm_mv1080_eosm2        :
        (crop_preset == CROP_PRESET_anamorphic_rewired_EOSM2) ? reg_override_anamorphic_rewired_eosm2       :
        (crop_preset == CROP_PRESET_3x1_mv720_50fps_EOSM2) ? reg_override_3x1_mv720_50fps_eosm2        : 
        // (crop_preset == CROP_PRESET_3x3_1X_EOSM)    ? reg_override_mv1080_mv720p  :
                                                  0                       ;
    return reg_override_func;
}

static void FAST engio_write_hook(uint32_t* regs, uint32_t* stack, uint32_t pc)
{
    uint32_t (*reg_override_func)(uint32_t, uint32_t) = 
        get_engio_reg_override_func();

    if (!reg_override_func)
    {
        return;
    }

    /* cmos_vidmode_ok doesn't help;
     * we can identify the current video mode from 0xC0F06804 */
    for (uint32_t * buf = (uint32_t *) regs[0]; *buf != 0xFFFFFFFF; buf += 2)
    {
        uint32_t reg = *buf;
        uint32_t old = *(buf+1);
        if (reg == 0xC0F06804)
        {
	   if (is_EOSM)
	   {
	     engio_vidmode_ok = 
             (old == 0x4540298) ||/* x5 zoom */ (old == 0x4a601d4 || old == 0x2d701d4);   /* 1080p or 720p */
	   }
	   if (is_EOSM2)
	   {
	     engio_vidmode_ok = 
             (old == 0x45802a1)/* x5 zoom */|| (old == 0x42801ed)/* digital zoom */ || (old == 0x4a701d7 || old == 0x2d801d7); /* 1080p or 720p */
	   }
        }
    }

    if (!is_supported_mode() || !engio_vidmode_ok)
    {
        /* don't patch other video modes */
          return;
    }

    for (uint32_t * buf = (uint32_t *) regs[0]; *buf != 0xFFFFFFFF; buf += 2)
    {
        uint32_t reg = *buf;
        uint32_t old = *(buf+1);
        
        int new = reg_override_func(reg, old);
        if (new)
        {
            dbg_printf("[%x] %x: %x -> %x\n", regs[0], reg, old, new);
            *(buf+1) = new;
        }
    }
}

static int patch_active = 0;

static void update_patch()
{

    if (CROP_PRESET_MENU && is_movie_mode())
    {
        /* update preset */
        crop_preset = CROP_PRESET_MENU;

        /* install our hooks, if we haven't already do so */
        if (!patch_active)
        {
            patch_hook_function(CMOS_WRITE, MEM_CMOS_WRITE, &cmos_hook, "crop_rec: CMOS[1,2,6] parameters hook");
            patch_hook_function(ADTG_WRITE, MEM_ADTG_WRITE, &adtg_hook, "crop_rec: ADTG[8000,8806] parameters hook");
            if (ENGIO_WRITE)
            {
                patch_hook_function(ENGIO_WRITE, MEM_ENGIO_WRITE, engio_write_hook, "crop_rec: video timers hook");
            }
            patch_active = 1;
        }
    }
    else
    {
        /* undo active patches, if any */
        if (patch_active)
        {
            unpatch_memory(CMOS_WRITE);
            unpatch_memory(ADTG_WRITE);
            if (ENGIO_WRITE)
            {
                unpatch_memory(ENGIO_WRITE);
            }
            patch_active = 0;
            crop_preset = 0;
        }
    }

    mcm_af_update();
}

/* enable patch when switching LiveView (not in the middle of LiveView) */
/* otherwise you will end up with a halfway configured video mode that looks weird */
PROP_HANDLER(PROP_LV_ACTION)
{
    update_patch();
}

/* also try when switching zoom modes */
PROP_HANDLER(PROP_LV_DISPSIZE)
{
    dmlog_add(DM_MARKER, 0, "*** ML-SPY DISPSIZE notify %d", buf[0], 0, 0, 0, 0);
    if (mcm_af_log) printf("[PROP] t=%05x DISPSIZE   %d\n", GET_DIGIC_TIMER(), buf[0]);
    update_patch();
}

PROP_HANDLER(PROP_LIVE_VIEW_AF_SYSTEM)
{
    int v = buf[0];
    dmlog_add(DM_MARKER, 0, "*** ML-SPY AF_SYSTEM notify %d (crop=%d dispsize=%d)", v, video_mode_crop, lv_dispsize, 0, 0);
    /* freeze the Canon log on the forced switch: any AF notification while Digital zoom is on
     * (the forced 1 can arrive as the very first notification, so don't require a known old value) */
    if ((video_mode_crop || (af_last_val >= 0 && v != af_last_val)) && !dmlog_stop_at && !gmt_trace)
    {
        dmlog_stop_at = dmlog_w + 3000;     /* keep some lead-up, mostly what follows */
    }
    if (v != af_last_val)
    {
        int n = af_notify_count;
        int k = (n == 0) ? 0 : 1;
        af_notify[k].old  = af_last_val;
        af_notify[k].val  = v;
        af_notify[k].vm   = pw_count_vm;
        af_notify[k].zoom = pw_count_zoom;
        af_notify[k].crop = video_mode_crop;
        af_notify[k].disp = lv_dispsize;
        af_notify_count = n + 1;
        af_last_val = v;
    }
    if (mcm_af_log) printf("[PROP] t=%05x AF_SYSTEM  %d (crop=%d, filter=%d, rewrites=%d)\n",
        GET_DIGIC_TIMER(), buf[0], video_mode_crop, mcm_af_filter, mcm_af_rewrites);
}

PROP_HANDLER(PROP_MOVIE_SERVO_AF)
{
    movie_servo_af = buf[0];
}

PROP_HANDLER(PROP_VIDEO_MODE)
{
    cur_video_mode_len = MIN(len, sizeof(cur_video_mode));
    memcpy(cur_video_mode, buf, cur_video_mode_len);
    dmlog_add(DM_MARKER, 0, "*** ML-SPY VIDEO_MODE notify %x %x %x %x %x", buf[0], buf[1], buf[2], buf[3], buf[4]);
    if (mcm_af_log) printf("[PROP] t=%05x VIDEO_MODE %x %x %x %x %x\n",
        GET_DIGIC_TIMER(), buf[0], buf[1], buf[2], buf[3], buf[4]);
}

static MENU_UPDATE_FUNC(mcm_af_choice_update)
{
    if (mcm_af >= 2 && movie_servo_af)
    {
        MENU_SET_WARNING(MENU_WARN_ADVICE,
            "Movie Servo AF on: LiveView froze ~15-20s into recording in tests. Turn it off.");
    }
}

static MENU_UPDATE_FUNC(mcm_af_menu_update)
{
    MENU_SET_RINFO("AF=%d crop=%d nf=%d", lv_af_mode, video_mode_crop, noface_patched);
    MENU_SET_WARNING(MENU_WARN_INFO,
        "pstate:%s filt:%d tgt:%d spells:%d af:%d rw:%d dlv:%d",
        pstate_hook_err == 0 ? "on" : pstate_hook_err < 0 ? "off" : "ERR",
        mcm_af_filter, mcm_af_target, mpu_spells_seen, mpu_af_spells,
        mcm_af_rewrites, mcm_af_delivers);

}

static void fmt_write(char * out, int size, uint8_t kind, uint8_t which, uint32_t val, uint32_t caller)
{
    /* no %c: ML's snprintf doesn't handle it */
    char k[3] = { kind ? kind : '?', which ? which : '?', 0 };
    snprintf(out, size, "%s%d@%x", k, (int)(val & 0xFF), caller);
}

static MENU_UPDATE_FUNC(prop_log_menu_update)
{
    char a[4][24] = {{0}};
    int n = pw_count_af;
    if (n > 0) fmt_write(a[0], sizeof(a[0]), afw_first[0].kind, 'A', afw_first[0].val, afw_first[0].caller);
    if (n > 1) fmt_write(a[1], sizeof(a[1]), afw_first[1].kind, 'A', afw_first[1].val, afw_first[1].caller);
    if (n > 2) fmt_write(a[2], sizeof(a[2]), afw_last[(n - 2) & 1].kind, 'A', afw_last[(n - 2) & 1].val, afw_last[(n - 2) & 1].caller);
    if (n > 3) fmt_write(a[3], sizeof(a[3]), afw_last[(n - 1) & 1].kind, 'A', afw_last[(n - 1) & 1].val, afw_last[(n - 1) & 1].caller);

    MENU_SET_VALUE("A%d V%d Z%d", pw_count_af, pw_count_vm, pw_count_zoom);
    MENU_SET_RINFO("AF=%d", lv_af_mode);

    if (!prop_spy_active)
    {
        MENU_SET_WARNING(MENU_WARN_INFO, "spy %s (err %x)", prop_spy_err ? "ERR" : "off", prop_spy_err);
    }
    else if (n == 0)
    {
        /* no request/deliver: show AF change notifications (old>new after Vn/Zn writes, crop, zoom)
         * and the deliver-array hits */
        char c[2][28] = {{0}};
        int m = af_notify_count;
        for (int j = 0; j < 2 && j < m; j++)
        {
            int k = j;      /* first, latest */
            snprintf(c[j], sizeof(c[j]), "%d>%d V%dZ%d c%d z%d",
                af_notify[k].old, af_notify[k].val, af_notify[k].vm, af_notify[k].zoom,
                af_notify[k].crop, af_notify[k].disp);
        }
        MENU_SET_WARNING(MENU_WARN_INFO, "N%d %s | %s | DA%d %x %x %x %x @%x",
            m, c[0], c[1], dlvarr_af_count,
            dlvarr_af_args[0], dlvarr_af_args[1], dlvarr_af_args[2], dlvarr_af_args[3], dlvarr_af_args[5]);
    }
    else
    {
        /* first two AF writes, then (if more) the last two */
        MENU_SET_WARNING(MENU_WARN_INFO, "AF: %s %s %s %s", a[0], a[1], a[2], a[3]);
    }
}

static MENU_UPDATE_FUNC(post_log_menu_update)
{
    /* latest PropertyMgr message carrying AF_SYSTEM: type, payload words, posting API, call chain */
    int n = post_af_count;
    int k = (n > 1) ? 1 : 0;
    MENU_SET_VALUE("%d", n);
    if (n == 0)
    {
        MENU_SET_WARNING(MENU_WARN_INFO, "no PropertyMgr message with AF_SYSTEM yet");
        return;
    }
    MENU_SET_RINFO("t%d", post_af[k].type);
    MENU_SET_WARNING(MENU_WARN_INFO, "%x %x %x %x %x|%x<%x<%x<%x<%x",
        post_af[k].w[0], post_af[k].w[1], post_af[k].w[2], post_af[k].w[3], post_af[k].w[4],
        post_af[k].api, post_af[k].ret[0], post_af[k].ret[1], post_af[k].ret[2], post_af[k].ret[3]);
}

static MENU_UPDATE_FUNC(setdata_menu_update)
{
    /* latest direct SetPropertyData(AF_SYSTEM): value, caller and call chain */
    int n = setdata_af_count;
    int k = (n > 1) ? 1 : 0;
    MENU_SET_VALUE("%d", n);
    if (!prop_spy_active)
    {
        MENU_SET_WARNING(MENU_WARN_INFO, "spy %s (err %x)", prop_spy_err ? "ERR" : "off", prop_spy_err);
        return;
    }
    if (n == 0)
    {
        MENU_SET_WARNING(MENU_WARN_INFO, "no SetPropertyData(AF_SYSTEM) yet");
        return;
    }
    MENU_SET_RINFO("val %d", (int) setdata_af[k].val);
    MENU_SET_WARNING(MENU_WARN_INFO, "%x<%x<%x<%x<%x | first: v%d %x<%x",
        setdata_af[k].api, setdata_af[k].ret[0], setdata_af[k].ret[1], setdata_af[k].ret[2], setdata_af[k].ret[3],
        (int) setdata_af[0].val, setdata_af[0].api, setdata_af[0].ret[0]);
}

static int in_set(const char * set, char c)
{
    for (; *set; set++) if (*set == c) return 1;
    return 0;
}

static int fmt_is_safe(const char * fmt)
{
    uint32_t f = (uint32_t) fmt;
    if (!((f >= 0xFF000000) || (f > 0x1000 && f < 0x10000000))) return 0;
    /* only numeric conversions: %s/%c may point anywhere (or be unsupported) */
    int n = 0;
    for (const char * c = fmt; *c && n < 200; c++, n++)
    {
        if (*c != '%') continue;
        c++;
        while (*c && in_set("0123456789.-+# lhz", *c)) c++;
        if (!*c || !in_set("dixXuop%", *c)) return 0;
    }
    return 1;
}

static void dmlog_save_as(const char * filename)
{
    dmlog_paused = 1;
    msleep(50);

    FILE * f = FIO_CreateFile(filename);
    if (!f) { dmlog_paused = 0; NotifyBox(2000, "Cannot create %s", filename); return; }

    char * line = malloc(512);
    int w = dmlog_w;
    int start = (w > DMLOG_N) ? w - DMLOG_N : 0;
    int written = 0;

    /* header: total entries and messages per class (top classes are the noise to filter) */
    {
        int len = snprintf(line, 512, "# entries %d (kept %d). messages per class:", w, w - start);
        FIO_WriteFile(f, line, len);
        for (int c = 0; c < 256; c++)
        {
            if (!dm_cls_count[c]) continue;
            len = snprintf(line, 512, " %d:%d", c, dm_cls_count[c]);
            FIO_WriteFile(f, line, len);
        }
        FIO_WriteFile(f, "\n", 1);
    }
    for (int i = start; i < w; i++)
    {
        struct dmlog_entry * e = &dmlog[i % DMLOG_N];
        int len = snprintf(line, 512, "%05x %3d:%d ", e->t, e->cls == DM_MARKER ? -1 : e->cls, e->lvl);
        if (fmt_is_safe(e->fmt))
        {
            len += snprintf(line + len, 512 - len, e->fmt, e->a[0], e->a[1], e->a[2], e->a[3], e->a[4]);
        }
        else
        {
            /* raw: format address + string if it looks printable, then the args */
            uint32_t fa = (uint32_t) e->fmt;
            int ok = (fa >= 0xFF000000) || (fa > 0x1000 && fa < 0x10000000);
            char tmp[80] = {0};
            if (ok)
            {
                int k = 0;
                for (const char * c = e->fmt; *c && k < 78; c++)
                    tmp[k++] = (*c >= 32 && *c < 127) ? *c : '.';
            }
            len += snprintf(line + len, 512 - len, "[raw %x \"%s\"] %x %x %x %x %x", fa, tmp,
                e->a[0], e->a[1], e->a[2], e->a[3], e->a[4]);
        }
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) len--;
        if (e->rep && len < 480) len += snprintf(line + len, 512 - len, "  (x%d)", e->rep + 1);
        line[len++] = '\n';
        FIO_WriteFile(f, line, len);
        written++;
    }
    free(line);
    FIO_CloseFile(f);
    dmlog_paused = 0;
    dmlog_stop_at = 0;      /* re-arm for the next AF change */
    NotifyBox(3000, "Saved %d lines to %s", written, filename);
}

static void dmlog_save()
{
    dmlog_save_as("ML/AFLOG.TXT");
}

static MENU_SELECT_FUNC(dmlog_save_select)
{
    dmlog_save();
}

static MENU_UPDATE_FUNC(dmlog_menu_update)
{
    MENU_SET_VALUE("%d", dmlog_w);
    if (dmlog_hooked && dmlog_stop_at && dmlog_w >= dmlog_stop_at) MENU_SET_RINFO("FROZEN (AF changed)");
    else if (dmlog_hooked && dmlog_stop_at) MENU_SET_RINFO("AF changed...");
    else if (dmlog_hooked) MENU_SET_RINFO("capturing");
    else              MENU_SET_RINFO("NOT hooked %x", dm_insn_seen);
}

/* Live readout of Canon's AF frame state machine (GmtWakuState), EOS M2 1.0.3.
 * gmt 'this' is stored at 0x921F4+0x14; its StateObject at gmt+0x18 (current state at +0x1C).
 * 0x754: face sub-mode used by gmtStartServoAFFaceTrack; 0x1AC: LvMode index;
 * 0x744: magnify; 0x7C0: MF flag; per-mode entry (0x34 bytes) +0xC0: Digital zoom flag. */
/* AF frame trace: state changes are logged as markers from the polling CBR */
static void gmt_trace_poll()
{
    if (!gmt_trace || !is_EOSM2) return;
    uint32_t gmt = *(volatile uint32_t *)(0x921F4 + 0x14);
    if (!gmt || gmt > 0x10000000) return;
    uint32_t so = *(volatile uint32_t *)(gmt + 0x18);
    int st   = (so && so < 0x10000000) ? (int) *(volatile uint32_t *)(so + 0x1C) : -1;
    int sub  = *(volatile int *)(gmt + 0x754);
    int mode = *(volatile int *)(gmt + 0x1AC);
    int dz   = (mode >= 0 && mode < 8) ? *(volatile int *)(gmt + mode * 0x34 + 0xC0) : -1;
    static int last = -12345;
    int key = (st & 0xFF) | ((sub & 0xFF) << 8) | ((dz & 0xFF) << 16) | ((movie_servo_af & 1) << 24) | ((lv_af_mode & 0xF) << 26);
    if (key != last)
    {
        last = key;
        dmlog_add(DM_MARKER, 0, "*** ML-SPY waku st %d sub %d dz %d servo %d AF %d", st, sub, dz, movie_servo_af, lv_af_mode);
    }
}

/* ---------------------------------------------------------------------------
 * Freeze trace: records every Canon debug message (repeats collapsed), plus
 * ML markers for recording start/stop, AF frame state changes and a 1 s heartbeat
 * of the LiveView buffer addresses. When the displayed LV buffer stops changing for
 * 700 ms while LiveView is shown (= preview freeze), a marker is added and capture
 * stops shortly after, so the file ends at the freeze. EOS M2 1.0.3 addresses.
 * --------------------------------------------------------------------------- */
#define M2_LV_DISPLAY_ADDR  (*(volatile uint32_t *)(0x90494 + 0x12C))  /* YUV422_LV_BUFFER_DISPLAY_ADDR */
#define M2_EDMAC_LV_ADDR    0xC0F04208                                  /* LV (VRAM) DMA write address */
#define M2_EDMAC_HD_ADDR    0xC0F04108                                  /* HD (YUV) DMA write address */

static int frz_caught = 0;

static void frz_trace_poll()
{
    if (!frz_trace || !is_EOSM2) return;

    /* after a freeze, save the log by itself once capture has stopped (or 2 s later), so it
     * survives a battery pull when the camera can't be operated any more */
    static int autosaved = 0, t_caught = 0;
    if (frz_trace == 1) autosaved = 0;
    if (frz_caught && !autosaved)
    {
        if (!t_caught) t_caught = get_ms_clock();
        if (dmlog_w >= frz_stop_at || get_ms_clock() - t_caught > 2000)
        {
            autosaved = 1;
            t_caught = 0;
            dmlog_save_as("ML/FRZAUTO.TXT");
        }
    }

    static uint32_t last_disp = 0, last_lvdma = 0, last_hddma = 0;
    static int t_disp = 0, t_lvdma = 0, t_hddma = 0;
    static int disp_changes = 0;
    static int last_rec = -1;
    static int t_hb = 0;
    static int last_state_key = -12345;

    int now = get_ms_clock();
    uint32_t disp  = M2_LV_DISPLAY_ADDR;
    uint32_t lvdma = shamem_read(M2_EDMAC_LV_ADDR);
    uint32_t hddma = shamem_read(M2_EDMAC_HD_ADDR);

    if (frz_trace == 1)     /* just armed: reset the watchers */
    {
        frz_trace = 2;
        last_disp = disp; last_lvdma = lvdma; last_hddma = hddma;
        t_disp = t_lvdma = t_hddma = t_hb = now;
        disp_changes = 0;
        last_rec = -1;
        last_state_key = -12345;
    }

    if (disp  != last_disp)  { last_disp = disp;   t_disp = now; disp_changes++; }
    if (lvdma != last_lvdma) { last_lvdma = lvdma; t_lvdma = now; }
    if (hddma != last_hddma) { last_hddma = hddma; t_hddma = now; }

    int rec = RECORDING ? 1 : 0;
    if (rec != last_rec)
    {
        last_rec = rec;
        dmlog_add(DM_MARKER, 0, "*** ML-SPY RECORDING %d (crop %d, AF %d, servo %d)", rec, video_mode_crop, lv_af_mode, movie_servo_af, 0);
    }

    /* AF frame state, as in the AF trace */
    uint32_t gmt = *(volatile uint32_t *)(0x921F4 + 0x14);
    if (gmt && gmt < 0x10000000)
    {
        uint32_t so = *(volatile uint32_t *)(gmt + 0x18);
        int st  = (so && so < 0x10000000) ? (int) *(volatile uint32_t *)(so + 0x1C) : -1;
        int sub = *(volatile int *)(gmt + 0x754);
        int key = (st & 0xFF) | ((sub & 0xFF) << 8) | ((lv_af_mode & 0xF) << 16) | ((movie_servo_af & 1) << 20);
        if (key != last_state_key)
        {
            last_state_key = key;
            dmlog_add(DM_MARKER, 0, "*** ML-SPY waku st %d sub %d AF %d servo %d", st, sub, lv_af_mode, movie_servo_af, 0);
        }
    }

    if (now - t_hb >= 1000)
    {
        t_hb = now;
        dmlog_add(DM_MARKER, 0, "*** ML-SPY hb disp %x lvdma %x hddma %x rec %d", disp, lvdma, hddma, rec, 0);
    }

    /* freeze: the displayed buffer stops changing while LiveView is on screen */
    int watching = lv && !gui_menu_shown() && disp_changes >= 3;
    if (!watching) t_disp = now;
    if (watching && !frz_stop_at && now - t_disp > 700)
    {
        frz_caught = 1;
        dmlog_add(DM_MARKER, 0, "*** ML-SPY FREEZE: disp %x stuck %d ms; lvdma %x (%d ms), hddma %x", disp, now - t_disp, lvdma, now - t_lvdma, hddma);
        dmlog_add(DM_MARKER, 0, "*** ML-SPY FREEZE: hddma unchanged %d ms, rec %d, AF %d, servo %d", now - t_hddma, rec, lv_af_mode, movie_servo_af, 0);
        frz_stop_at = dmlog_w + 300;    /* a little of what follows, then stop */
    }
}

static MENU_UPDATE_FUNC(saf_mode_update)
{
    if (!is_EOSM2) { MENU_SET_VALUE("n/a"); return; }
    if (saf_hook_err > 0) { MENU_SET_RINFO("ERR %x", saf_hook_err); return; }
    if (saf_start_hook_err > 0) { MENU_SET_RINFO("start ERR %x", saf_start_hook_err); return; }
    MENU_SET_RINFO("Canon %d, used %d", saf_seen_mode, saf_used_mode);
    MENU_SET_WARNING(MENU_WARN_INFO, "SAF setups %d. Evf in: kind %d, lvmode %d, zoom %x, %d %d %d. Restart LV to apply.",
        saf_seen_count, saf_sel_in[0], saf_sel_in[1], saf_sel_in[2], saf_sel_in[3], saf_sel_in[4], saf_sel_in[5]);
}

/* Canon's own SAF debug switches (RAM 0x958C0, read by SAF.c / SAFState.c):
 * 0x01 setup details, 0x02 accumulator reset, 0x04 each DefocusResult,
 * 0x80 every 3rd result: 4 areas ">>> defocus(reliability) ...", 0x100 ghost judge.
 * Messages go to the class in [0x958C4]; capture them with "Freeze trace" (all classes). */
#define SAF_DEBUG_FLAGS     (*(volatile uint32_t *) 0x958C0)
#define SAF_DEBUG_CLASS     (*(volatile uint32_t *) 0x958C4)
static int saf_debug = 0;
static uint32_t saf_debug_old = 0;

static MENU_SELECT_FUNC(saf_debug_select)
{
    if (!is_EOSM2) return;
    if (!saf_debug)
    {
        saf_debug_old = SAF_DEBUG_FLAGS;
        SAF_DEBUG_FLAGS = saf_debug_old | 0x10000085;  /* setup/start, DefocusResult, per-area results,
                                                          * picker window updates (SafWin / Picker / MC) */
        saf_debug = 1;
        saf_trace_cls = SAF_DEBUG_CLASS & 0xFF;
    }
    else
    {
        SAF_DEBUG_FLAGS = saf_debug_old;
        saf_debug = 0;
        saf_trace_cls = -1;
    }
}

static MENU_UPDATE_FUNC(saf_debug_update)
{
    if (!is_EOSM2) { MENU_SET_VALUE("n/a"); return; }
    MENU_SET_VALUE(saf_debug ? "ON" : "OFF");
    MENU_SET_RINFO("flags %x cls %d", SAF_DEBUG_FLAGS, SAF_DEBUG_CLASS & 0xFF);
}

static MENU_UPDATE_FUNC(saf_picker_menu_update)
{
    if (!is_EOSM2) { MENU_SET_VALUE("n/a"); return; }
    if (saf_picker_err) MENU_SET_RINFO("ERR %x", saf_picker_err);
    else MENU_SET_RINFO("applied: %s", saf_picker_applied == 1 ? "picker 0" : saf_picker_applied == 2 ? "picker 1" : "Canon");
}

static MENU_UPDATE_FUNC(saf_p0_shift_update_menu)
{
    if (!is_EOSM2) { MENU_SET_VALUE("n/a"); return; }
    if (saf_picker != 1) MENU_SET_WARNING(MENU_WARN_INFO, "Only used with PD picker = Picker 0. Restart LiveView after changing.");
    else MENU_SET_WARNING(MENU_WARN_INFO, "Restart LiveView after changing. Then check 'Phase detect (mcm)' quality.");
}

/* Y of the first window in the picker copy SAF is actually using: two copy buffers
 * (pointers at 0xD2520 and 0xD2520+0x54), current one selected by [0x95E58] (u16). */
static uint32_t saf_live_picker_ptr = 0;
static int saf_live_picker_b = -1;          /* second coordinate (op6/op5) */
static int saf_live_picker_y()
{
    int cur = *(volatile uint16_t *) 0x95E58;
    volatile uint16_t * buf = *(volatile uint16_t * volatile *)(0xD2520 + (cur ? 0x54 : 0));
    saf_live_picker_ptr = (uint32_t) buf;
    /* buffers may be in the uncacheable mirror (0x4xxxxxxx) */
    if (!buf || ((uint32_t) buf & 1) || (uint32_t) buf >= 0x60000000) return -1;
    int a = -2;
    saf_live_picker_b = -2;
    for (int i = 0; i < 16; i++)
    {
        if (a < 0 && (buf[i] >> 10) == 0x4 && (buf[i + 1] >> 10) == 0x3)
            a = ((buf[i] & 0x3FF) << 8) + (buf[i + 1] & 0xFF);
        if (saf_live_picker_b < 0 && (buf[i] >> 10) == 0x6 && (buf[i + 1] >> 10) == 0x5)
            saf_live_picker_b = ((buf[i] & 0x3FF) << 8) + (buf[i + 1] & 0xFF);
    }
    return a;
}

static MENU_UPDATE_FUNC(saf_p2_menu_update)
{
    if (!is_EOSM2) { MENU_SET_VALUE("n/a"); return; }
    int ly = saf_live_picker_y();
    MENU_SET_RINFO("live %d,%d @%x", ly, saf_live_picker_b, saf_live_picker_ptr);
    int sy = -1;
    volatile uint16_t * t = saf_pe[1].table;
    for (int i = 0; i < 16; i++)
        if ((t[i] >> 10) == 0x4 && (t[i + 1] >> 10) == 0x3) { sy = ((t[i] & 0x3FF) << 8) + (t[i + 1] & 0xFF); break; }
    MENU_SET_WARNING(MENU_WARN_INFO, "src ptrs %x %x %x, edited %x (Y %d), applied Y %+d op8 %d",
        saf_picker_src[0], saf_picker_src[1], saf_picker_src[2], (uint32_t) saf_pe[1].table, sy,
        saf_pe[1].ay, saf_pe[1].a8 ? saf_pe[1].a8 : 180);
}


static MENU_UPDATE_FUNC(saf_pd_update)
{
    /* quality over the last 32 results */
    int n = MIN(saf_hist_w, SAF_HIST_N);
    int vsum = 0, jsum = 0, jn = 0;
    for (int i = 0; i < n; i++)
    {
        vsum += saf_hist_valid[i];
        if (saf_hist_jump[i] >= 0) { jsum += saf_hist_jump[i]; jn++; }
    }
    if (!is_EOSM2) { MENU_SET_VALUE("n/a"); return; }
    if (saf_result_hook_err > 0) { MENU_SET_RINFO("ERR %x", saf_result_hook_err); return; }
    MENU_SET_RINFO("ok %d.%d jump %d (%d/%d)", n ? vsum / n : 0, n ? (vsum * 10 / n) % 10 : 0, jn ? jsum / jn : -1, jn, n);
    MENU_SET_VALUE("%s D0 %d", saf_pd_ignore == 2 ? "Drop" : saf_pd_ignore ? "Ignore" : "Canon", saf_res_d0);
    MENU_SET_WARNING(MENU_WARN_INFO, "Results %d, lens pos %d. jump: lens-still frames only (n/32). drop:%d err:%x",
        saf_res_count, saf_res_fpc, saf_drop_patched, saf_drop_err);
}

static MENU_SELECT_FUNC(saf_save_select)
{
    FILE * f = FIO_CreateFile("ML/SAF.TXT");
    if (!f) { NotifyBox(2000, "Cannot create ML/SAF.TXT"); return; }
    char * buf = malloc(2048);
    int len = 0;
    for (int c = 0; c < 2; c++)
    {
        struct saf_snap * sn = &saf_snap[c];
        len += snprintf(buf + len, 2048 - len, "== %s: %s\n", c ? "Digital zoom / mcm" : "normal",
            sn->valid ? "" : "(not seen yet)");
        if (!sn->valid) continue;
        len += snprintf(buf + len, 2048 - len, "setups %d, mode %d -> %d\n", sn->count, sn->mode_in, sn->mode_out);
        len += snprintf(buf + len, 2048 - len, "selector in: kind %d lvmode %d zoom %x r3 %d s0 %d s1 %d\n",
            sn->sel_in[0], sn->sel_in[1], sn->sel_in[2], sn->sel_in[3], sn->sel_in[4], sn->sel_in[5]);
        len += snprintf(buf + len, 2048 - len, "param:");
        for (int i = 0; i < 12; i++) len += snprintf(buf + len, 2048 - len, " %d", sn->param[i]);
        len += snprintf(buf + len, 2048 - len, "\nreadout start %x end %x\n", sn->rd_start, sn->rd_end);
        for (int k = 0; k < 2; k++)
        {
            len += snprintf(buf + len, 2048 - len, "geo%d @%x:", k, sn->geo_ptr[k]);
            for (int i = 0; i < 12; i++) len += snprintf(buf + len, 2048 - len, " %d", sn->geo[k][i]);
            len += snprintf(buf + len, 2048 - len, "\n");
        }
    }
    FIO_WriteFile(f, buf, len);
    FIO_CloseFile(f);
    free(buf);
    NotifyBox(2000, "Saved ML/SAF.TXT");
}

/* SET on "Freeze trace": start; SET again: stop and save ML/FRZLOG.TXT */
static MENU_SELECT_FUNC(frz_trace_select)
{
    if (!is_EOSM2) return;
    if (!frz_trace)
    {
        if (gmt_trace) { NotifyBox(2000, "Stop the AF trace first"); return; }
        if (!dmlog_hooked) dmlog_hooked = install_debugmsg_capture();
        if (!dmlog_hooked) { NotifyBox(2000, "DebugMsg capture failed (%x)", dm_insn_seen); return; }
        dmlog_paused = 1;
        dmlog_stop_at = 0;
        frz_stop_at = 0;
        frz_caught = 0;
        dmlog_w = 0;
        memset(dm_cls_count, 0, sizeof(dm_cls_count));
        frz_trace = 1;
        dmlog_paused = 0;
    }
    else
    {
        dmlog_save_as("ML/FRZLOG.TXT");
        frz_trace = 0;
        frz_stop_at = 0;
        dmlog_w = 0;
    }
}

static MENU_UPDATE_FUNC(frz_trace_update)
{
    if (!is_EOSM2) { MENU_SET_VALUE("n/a"); return; }
    if (!frz_trace)       MENU_SET_VALUE("OFF");
    else if (frz_caught)  MENU_SET_VALUE("FREEZE caught");
    else                  MENU_SET_VALUE("ON");
    if (frz_trace) MENU_SET_RINFO("%d", dmlog_w);
}

/* SET on "AF state (debug)": start the trace; SET again: stop and save ML/GMTLOG.TXT */
static MENU_SELECT_FUNC(gmt_trace_select)
{
    if (!is_EOSM2) return;
    if (!gmt_trace)
    {
        if (frz_trace) { NotifyBox(2000, "Stop the freeze trace first"); return; }
        if (!dmlog_hooked) dmlog_hooked = install_debugmsg_capture();
        if (!dmlog_hooked) { NotifyBox(2000, "DebugMsg capture failed (%x)", dm_insn_seen); return; }
        dmlog_paused = 1;
        dmlog_stop_at = 0;
        dmlog_w = 0;
        memset(dm_cls_count, 0, sizeof(dm_cls_count));
        gmt_trace = 1;
        dmlog_paused = 0;
    }
    else
    {
        gmt_trace_poll();
        dmlog_save_as("ML/GMTLOG.TXT");
        gmt_trace = 0;
        dmlog_w = 0;
    }
}

static MENU_UPDATE_FUNC(gmt_state_update)
{
    if (!is_EOSM2) { MENU_SET_VALUE("n/a"); return; }
    uint32_t gmt = *(volatile uint32_t *)(0x921F4 + 0x14);
    if (!gmt || gmt > 0x10000000) { MENU_SET_VALUE("no gmt"); return; }
    uint32_t so = *(volatile uint32_t *)(gmt + 0x18);
    int st   = (so && so < 0x10000000) ? (int) *(volatile uint32_t *)(so + 0x1C) : -1;
    int sub  = *(volatile int *)(gmt + 0x754);
    int mode = *(volatile int *)(gmt + 0x1AC);
    int mag  = *(volatile int *)(gmt + 0x744);
    int mf   = *(volatile int *)(gmt + 0x7C0);
    int dz   = (mode >= 0 && mode < 8) ? *(volatile int *)(gmt + mode * 0x34 + 0xC0) : -1;
    MENU_SET_VALUE("st %d sub %d", st, sub);
    if (gmt_trace) MENU_SET_RINFO("TRACE %d", dmlog_w);
    else MENU_SET_RINFO("AF=%d servo=%d", lv_af_mode, movie_servo_af);
    MENU_SET_WARNING(MENU_WARN_INFO, "waku state %d, 0x754=%d, mode=%d, mag=%x, mf=%d, dzflag=%d, crop=%d",
        st, sub, mode, mag, mf, dz, video_mode_crop);
}

static MENU_UPDATE_FUNC(crop_update)
{
    if ((CROP_PRESET_MENU && lv) && !is_EOSM && !is_EOSM2)
    {

            if (!is_supported_mode())
            {
                MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "This preset only works in 1080p and 720p video modes.");
            }
            else if (lv_dispsize != 1)
            {
                MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "To use this mode, exit ML menu and press the zoom button (set to x1).");
            }
            // else if (!is_720p())
            // {
            //     if (CROP_PRESET_MENU == CROP_PRESET_3x3_1X ||
            //         CROP_PRESET_MENU == CROP_PRESET_3x3_1X_48p)
            //     {
            //         /* these presets only have effect in 720p mode */
            //         MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "This preset only works in the 720p 50/60 fps modes from Canon menu.");
            //         return;
            //     }
            // }

    }
}

static MENU_UPDATE_FUNC(target_yres_update)
{
    MENU_SET_RINFO("from %d", max_resolutions[crop_preset][get_video_mode_index()]);
}

static struct menu_entry crop_rec_menu[] =
{
    {
        .name       = "Crop mode",
        .priv       = &crop_preset_index,
        .update     = crop_update,
        .depends_on = DEP_LIVEVIEW,
        .children =  (struct menu_entry[]) {
            {
                .name   = "AF method (mcm)",
                .priv   = &mcm_af,
                .update = mcm_af_choice_update,
                .max    = 3,
                .choices = CHOICES("Canon", "FlexiZone-Single", "Face+Tracking", "FlexiZone-Multi"),
                .help   = "EOS M2 movie-crop presets: keep this AF method instead of\n"
                          "the FlexiZone-Single that Canon's MPU forces in Movie crop.\n",
            },
            {
                .name   = "AF/MPU trace",
                .priv   = &mcm_af_log,
                .update = mcm_af_menu_update,
                .max    = 1,
                .help   = "Debug only: installs the property/MPU spies and the Canon log\n"
                          "capture (stay active until reboot). Leave OFF for normal use.\n",
            },
            {
                .name   = "Prop write log",
                .update = prop_log_menu_update,
                .help   = "Counts: A=AF_SYSTEM V=VIDEO_MODE Z=DISPSIZE writes. Bottom: AF writes\n"
                          "W=ML wait wrapper, R=Canon request, D=deliver; value@caller.\n",
            },
            {
                .name   = "Save Canon log",
                .select = dmlog_save_select,
                .update = dmlog_menu_update,
                .help   = "Writes the last 4096 Canon debug messages (+ ML-SPY markers)\n"
                          "to ML/AFLOG.TXT. Press SET right after AF switches to Single.\n",
            },
            {
                .name   = "AF set log",
                .update = setdata_menu_update,
            },
            {
                .name   = "AF msg log",
                .update = post_log_menu_update,
            },
            {
                .name   = "bitdepth",
                .priv   = &bitdepth,
                .max    = 4,
                .choices = CHOICES("OFF", "8 bit", "9 bit", "10 bit", "12 bit"),
                .help   = "Alter bitdepth\n"
            },
            {
                .name   = "ratios",
                .priv   = &ratios,
                .max    = 2,
                .choices = CHOICES("OFF", "2.35:1", "16:9"),
                .help   = "Change aspect ratio\n"
            },
            {
                .name   = "x3crop",
                .priv   = &x3crop,
                .max    = 1,
                .choices = CHOICES("OFF", "x3crop"),
                .help   = "Turns mv1080p and mv1080_46fps modes into x3 crop modes\n"
            },
            {
                .name   = "set 25fps",
                .priv   = &set_25fps,
                .max    = 1,
                .choices = CHOICES("OFF", "25fps"),
                .help   = "Sets 2.35:1 and 16:9 modes to 25fps\n"
            },
            {
                .name   = "hdr iso A",
                .priv   = &HDR_iso_a,
                .max    = 6,
                .choices = CHOICES("OFF", "iso100", "iso200", "iso400", "iso800", "iso1600", "iso3200"),
                .help   = "HDR workaround eosm\n"
            },
            {
                .name   = "hdr iso B",
                .priv   = &HDR_iso_b,
                .max    = 6,
                .choices = CHOICES("OFF", "iso100", "iso200", "iso400", "iso800", "iso1600", "iso3200"),
                .help   = "HDR workaround eosm\n"
            },
            {
                .name   = "reg_713c",
                .priv   = &reg_713c,
                .min    = -2000,
                .max    = 2000,
                .unit   = UNIT_DEC,
                .help  = "Corruption? Combine with reg_7150",
                .advanced = 1,
            },
            {
                .name   = "reg_7150",
                .priv   = &reg_7150,
                .min    = -2000,
                .max    = 2000,
                .unit   = UNIT_DEC,
                .help  = "Corruption issues? Combine with reg_713c",
                .advanced = 1,
            },
            {
                .name   = "reg_6014",
                .priv   = &reg_6014,
                .min    = -2000,
                .max    = 2000,
                .unit   = UNIT_DEC,
                .help  = "Alter frame rate. Combine with reg_6008",
                .advanced = 1,
            },
            {
                .name   = "reg_6008",
                .priv   = &reg_6008,
                .min    = -500,
                .max    = 500,
                .unit   = UNIT_DEC,
                .help  = "Alter frame rate. Combine with reg_6014",
                .advanced = 1,
            },
            {
                .name   = "reg_800c",
                .priv   = &reg_800c,
                .min    = -500,
                .max    = 500,
                .unit   = UNIT_DEC,
                .help  = "line skipping",
                .advanced = 1,
            },
            {
                .name   = "reg_8000",
                .priv   = &reg_8000,
                .min    = -500,
                .max    = 500,
                .unit   = UNIT_DEC,
                .help  = "x3zoom",
                .advanced = 1,
            },
            {
                .name   = "reg_8183",
                .priv   = &reg_8183,
                .min    = -2000,
                .max    = 2000,
                .unit   = UNIT_DEC,
                .help  = "Aliasing, moiré mcm rewired mode",
                .advanced = 1,
            },
            {
                .name   = "reg_8184",
                .priv   = &reg_8184,
                .min    = -2000,
                .max    = 2000,
                .unit   = UNIT_DEC,
                .help  = "Aliasing, moiré mcm rewired mode",
                .advanced = 1,
            },
            {
                .name   = "reg_6800_height",
                .priv   = &reg_6800_height,
                .min    = -2000,
                .max    = 2000,
                .unit   = UNIT_DEC,
                .help  = "height offset",
                .advanced = 1,
            },
            {
                .name   = "reg_6800_width",
                .priv   = &reg_6800_width,
                .min    = -2000,
                .max    = 2000,
                .unit   = UNIT_DEC,
                .help  = "width offset",
                .advanced = 1,
            },
            {
                .name   = "reg_6804_height",
                .priv   = &reg_6804_height,
                .min    = -2000,
                .max    = 2000,
                .unit   = UNIT_DEC,
                .help  = "Alter height.",
                .advanced = 1,
            },
            {
                .name   = "reg_6804_width",
                .priv   = &reg_6804_width,
                .min    = -2000,
                .max    = 2000,
                .unit   = UNIT_DEC,
                .help  = "Alter width. Scrambles preview",
                .advanced = 1,
            },
            {
                .name   = "reg_83d4",
                .priv   = &reg_83d4,
                .min    = -500,
                .max    = 500,
                .unit   = UNIT_DEC,
                .help  = "Preview engdrvout",
                .advanced = 1,
            },
            {
                .name   = "reg_83dc",
                .priv   = &reg_83dc,
                .min    = -500,
                .max    = 500,
                .unit   = UNIT_DEC,
                .help  = "Preview engdrvout",
                .advanced = 1,
            },
            {
                .name   = "CMOS[1]",
                .priv   = &cmos1,
                .max    = 0xFFF,
                .unit   = UNIT_HEX,
                .help   = "Vertical offset",
                .advanced = 1,
            },
            {
                .name   = "CMOS[2]",
                .priv   = &cmos2,
                .max    = 0xFFF,
                .unit   = UNIT_HEX,
                .help   = "Horizontal offset",
                .advanced = 1,
            },
            {
                .name   = "CMOS[3]",
                .priv   = &cmos3,
                .max    = 0xFFF,
                .unit   = UNIT_HEX,
                .help   = "Analog iso on 6D",
                .advanced = 1,
            },
            {
                .name   = "CMOS[4]",
                .priv   = &cmos4,
                .max    = 0xFFF,
                .unit   = UNIT_HEX,
                .help   = "Hot/cold pixel",
                .advanced = 1,
            },
            {
                .name   = "CMOS[5]",
                .priv   = &cmos5,
                .max    = 0xFFF,
                .unit   = UNIT_HEX,
                .help   = "Fine vertical offset, black area maybe",
                .advanced = 1,
            },
            {
                .name   = "CMOS[6]",
                .priv   = &cmos6,
                .max    = 0xFFF,
                .unit   = UNIT_HEX,
                .help   = "Iso 50 or timing related",
                .advanced = 1,
            },
            {
                .name   = "CMOS[7]",
                .priv   = &cmos7,
                .max    = 0xFFF,
                .unit   = UNIT_HEX,
                .help   = "Image fading out; 6D, 700D: vertical offset",
                .advanced = 1,
            },
            {
                .name   = "CMOS[8]",
                .priv   = &cmos8,
                .max    = 0xFFF,
                .unit   = UNIT_HEX,
                .help   = "Unknown, used on 6D",
                .help2  = "Use for horizontal centering.",
                .advanced = 1,
            },
            {
                .name   = "CMOS[9]",
                .priv   = &cmos9,
                .max    = 0xFFF,
                .unit   = UNIT_HEX,
                .help   = "?",
                .advanced = 1,
            },
            {
                .name   = "reg_gain",
                .priv   = &reg_gain,
                .min    = -500,
                .max    = 500,
                .unit   = UNIT_DEC,
                .help  = "Alter bit depth",
                .advanced = 1,
            },
            {
                .name   = "reg_timing1",
                .priv   = &reg_timing1,
                .min    = -500,
                .max    = 500,
                .unit   = UNIT_DEC,
                .help  = "PowSaveTim reg 8172, 8178, 8196",
                .advanced = 1,
            },
            {
                .name   = "reg_timing2",
                .priv   = &reg_timing2,
                .min    = -500,
                .max    = 500,
                .unit   = UNIT_DEC,
                .help  = "PowSaveTim reg 8179, 8197",
                .advanced = 1,
            },
            {
                .name   = "reg_timing3",
                .priv   = &reg_timing3,
                .min    = -500,
                .max    = 500,
                .unit   = UNIT_DEC,
                .help  = "PowSaveTim reg 8173",
                .advanced = 1,
            },
            {
                .name   = "reg_timing4",
                .priv   = &reg_timing4,
                .min    = -500,
                .max    = 500,
                .unit   = UNIT_DEC,
                .help  = "PowSaveTim reg 82f8, 82f9",
                .advanced = 1,
            },
            {
                .name   = "reg_timing5",
                .priv   = &reg_timing5,
                .min    = -500,
                .max    = 500,
                .unit   = UNIT_DEC,
                .help  = "PowSaveTim reg 6014",
                .advanced = 1,
            },
            {
                .name   = "reg_timing6",
                .priv   = &reg_timing6,
                .min    = -500,
                .max    = 500,
                .unit   = UNIT_DEC,
                .help  = "PowSaveTim reg 82b6",
                .advanced = 1,
            },
            {
                .name   = "reg_6824",
                .priv   = &reg_6824,
                .min    = -500,
                .max    = 500,
                .unit   = UNIT_DEC,
                .help  = "PowSaveTim reg 6824, 6828, 682c, 6830",
                .advanced = 1,
            },
            {
                .name   = "reg_skip_left",
                .priv   = &reg_skip_left,
                .min    = -1000,
                .max    = 1000,
                .unit   = UNIT_DEC,
                .help  = "skip left",
                .advanced = 1,
            },
            {
                .name   = "reg_skip_right",
                .priv   = &reg_skip_right,
                .min    = -1000,
                .max    = 1000,
                .unit   = UNIT_DEC,
                .help  = "skip right",
                .advanced = 1,
            },
            {
                .name   = "reg_skip_top",
                .priv   = &reg_skip_top,
                .min    = -1000,
                .max    = 1000,
                .unit   = UNIT_DEC,
                .help  = "skip top",
                .advanced = 1,
            },
            {
                .name   = "reg_skip_bottom",
                .priv   = &reg_skip_bottom,
                .min    = -1000,
                .max    = 1000,
                .unit   = UNIT_DEC,
                .help  = "skip bottom",
                .advanced = 1,
            },
            {
                .name       = "Shutter range",
                .priv       = &shutter_range,
                .max        = 1,
                .choices    = CHOICES("Original", "Full range"),
                .help       = "Choose the available shutter speed range:",
                .help2      = "Original: default range used by Canon in selected video mode.\n"
                              "Full range: from 1/FPS to minimum exposure time allowed by hardware.",
                .advanced = 1,
            },
            {
                .name   = "reg_bl",
                .priv   = &reg_bl,
                .min    = -1000,
                .max    = 1000,
                .unit   = UNIT_DEC,
                .help  = "black level for reduced bitdepths(not affecting recordings)",
                .advanced = 1,
            },
/* not used atm	{
                .name   = "CMOS[1] lo",
                .priv   = &cmos1_lo,
                .max    = 63,
                .unit   = UNIT_DEC,
                .help   = "Start scanline (very rough). Use for vertical positioning.",
                .advanced = 1,
            },
            {
                .name   = "CMOS[1] hi",
                .priv   = &cmos1_hi,
                .max    = 63,
                .unit   = UNIT_DEC,
                .help   = "End scanline (very rough). Increase if white bar at bottom.",
                .help2  = "Decrease if you get strange colors as you move the camera.",
                .advanced = 1,
            },
 	    {
                .name   = "Target YRES",
                .priv   = &target_yres,
                .update = target_yres_update,
                .max    = 3870,
                .unit   = UNIT_DEC,
                .help   = "Desired vertical resolution (only for presets with higher resolution).",
                .help2  = "Decrease if you get corrupted frames (dial the desired resolution here).",
                .advanced = 1,
            }, 
            {
                .name   = "Delta ADTG 0",
                .priv   = &delta_adtg0,
                .min    = -500,
                .max    = 500,
                .unit   = UNIT_DEC,
                .help   = "ADTG 0x8178, 0x8196, 0x82F8",
                .help2  = "May help pushing the resolution a little. Start with small increments.",
                .advanced = 1,
            },
            {
                .name   = "Delta ADTG 1",
                .priv   = &delta_adtg1,
                .min    = -500,
                .max    = 500,
                .unit   = UNIT_DEC,
                .help   = "ADTG 0x8179, 0x8197, 0x82F9",
                .help2  = "May help pushing the resolution a little. Start with small increments.",
                .advanced = 1,
            },
            {
                .name   = "Delta HEAD3",
                .priv   = &delta_head3,
                .min    = -500,
                .max    = 500,
                .unit   = UNIT_DEC,
                .help2  = "May help pushing the resolution a little. Start with small increments.",
                .advanced = 1,
            },
            {
                .name   = "Delta HEAD4",
                .priv   = &delta_head4,
                .min    = -500,
                .max    = 500,
                .unit   = UNIT_DEC,
                .help2  = "May help pushing the resolution a little. Start with small increments.",
                .advanced = 1,
            }, */
            MENU_ADVANCED_TOGGLE,
            MENU_EOL,
        },
    },
    {
        .name       = "AF state (debug)",
        .update     = gmt_state_update,
        .select     = gmt_trace_select,
        .depends_on = DEP_LIVEVIEW,
        .help       = "Live values from Canon's AF frame state machine (GmtWakuState).\n"
                      "SET: start AF trace. SET again: stop and save ML/GMTLOG.TXT.\n",
    },
    {
        .name       = "Freeze trace",
        .update     = frz_trace_update,
        .select     = frz_trace_select,
        .help       = "SET: start logging all Canon messages. Stops itself ~0.7 s after a\n"
                      "LiveView freeze. SET again: save ML/FRZLOG.TXT.\n",
    },
    {
        .name       = "Hybrid AF mode",
        .priv       = &saf_override,
        .max        = 9,
        .choices    = CHOICES("Canon", "1", "2", "3", "4", "5", "6", "7", "8", "Off (contrast only)"),
        .update     = saf_mode_update,
        .help       = "Phase-detect (Hybrid CMOS AF) mode. Right side: mode Canon chose / mode used.\n"
                      "mcm only. Takes effect at the next LiveView start (Canon menu and back).\n",
    },
    {
        .name       = "PD picker (mcm)",
        .priv       = &saf_picker,
        .max        = 2,
        .choices    = CHOICES("Canon", "Picker 0", "Picker 1"),
        .update     = saf_picker_menu_update,
        .help       = "Which phase-detect pixel layout SAF mode 4 (Digital zoom/mcm) uses.\n"
                      "Canon = single-window crop layout. Restart LiveView to apply.\n",
    },
    {
        .name       = "Picker 2 op8",
        .priv       = &saf_p2_op8,
        .max        = 7,
        .choices    = CHOICES("Canon (180)", "60", "90", "120", "36", "240", "360", "540"),
        .update     = saf_p2_menu_update,
        .help       = "Picker 2 (the one mcm uses with PD picker = Canon): value of command 8,\n"
                      "possibly 180 / vertical line skip. Restart LiveView after changing.\n",
    },
    {
        .name       = "PD window Y shift",
        .priv       = &saf_p2_yshift,
        .min        = -400,
        .max        = 400,
        .update     = saf_p2_menu_update,
        .help       = "mcm: shifts the phase-detect window rectangle given to the SAF setup\n"
                      "(Canon mcm: x 910 y 476 w 172 h 230). Restart LiveView.\n",
    },
    {
        .name       = "PD window X shift",
        .priv       = &saf_p2_xshift,
        .min        = -400,
        .max        = 400,
        .update     = saf_p2_menu_update,
        .help       = "mcm: shifts the phase-detect window rectangle horizontally.\n"
                      "Restart LiveView.\n",
    },
    {
        .name       = "Picker 0 Y shift",
        .priv       = &saf_p0_yshift,
        .min        = -96,
        .max        = 96,
        .update     = saf_p0_shift_update_menu,
        .help       = "Moves all picker 0 windows vertically (picker 0 units).\n",
    },
    {
        .name       = "Picker 0 X shift",
        .priv       = &saf_p0_xshift,
        .min        = -96,
        .max        = 96,
        .update     = saf_p0_shift_update_menu,
        .help       = "Moves all picker 0 windows horizontally (picker 0 units).\n",
    },
    {
        .name       = "Phase detect (mcm)",
        .priv       = &saf_pd_ignore,
        .max        = 2,
        .choices    = CHOICES("Canon", "Ignore", "Drop"),
        .update     = saf_pd_update,
        .help       = "mcm only. Ignore: send results marked unreliable. Drop: don't send\n"
                      "phase-detect results at all (contrast AF only). Immediate.\n",
    },
    {
        .name       = "Save SAF info",
        .select     = saf_save_select,
        .help       = "Writes ML/SAF.TXT: Hybrid AF setup seen without and with Digital zoom.\n",
    },
    {
        .name       = "SAF debug log",
        .select     = saf_debug_select,
        .update     = saf_debug_update,
        .help       = "Turns on Canon's Hybrid AF debug messages (phase-detect results).\n"
                      "While ON, Freeze trace keeps only these + AF state (class 154).\n",
    },
};

static int crop_rec_needs_lv_refresh()
{
    if (!lv)
    {
        return 0;
    }


    if (CROP_PRESET_MENU)
    {
        if (is_supported_mode())
        {
            if (!patch_active || CROP_PRESET_MENU != crop_preset || is_EOSM || is_EOSM2 || is_5D3)
            {
                return 1;
            }
        }
    }
    else /* crop disabled */
    {
        if (patch_active)
        {
            return 1;
        }
    }

    return 0;
}

static void center_canon_preview()
{
    /* center the preview window on the raw buffer */
    /* overriding these registers once will do the trick...
     * ... until the focus box is moved by the user */
    int old = cli();

    uint32_t pos1 = shamem_read(0xc0f383d4);
    uint32_t pos2 = shamem_read(0xc0f383dc);

    if ((pos1 & 0x80008000) == 0x80008000 &&
        (pos2 & 0x80008000) == 0x80008000)
    {
        /* already centered */
        sei(old);
        return;
    }

    int x1 = pos1 & 0xFFFF;
    int x2 = pos2 & 0xFFFF;
    int y1 = pos1 >> 16;
    int y2 = pos2 >> 16;

    if (x2 - x1 != 299 && y2 - y1 != 792)
    {
        /* not x5/x10 (values hardcoded for 5D3) */
        sei(old);
        return;
    }

    int raw_xc = (146 + 3744) / 2 / 4;  /* hardcoded for 5D3 */
    int raw_yc = ( 60 + 1380) / 2;      /* values from old raw.c */

    if (1)
    {
        /* use the focus box position for moving the preview window around */
        /* don't do that while recording! */
        dbg_printf("[crop_rec] %d,%d ", raw_xc, raw_yc);
        raw_xc -= 146 / 2 / 4;  raw_yc -= 60 / 2;
        /* this won't change the position if the focus box is centered */
        get_afframe_pos(raw_xc * 2, raw_yc * 2, &raw_xc, &raw_yc);
        raw_xc += 146 / 2 / 4;  raw_yc += 60 / 2;
        raw_xc &= ~1;   /* just for consistency */
        raw_yc &= ~1;   /* this must be even, otherwise the image turns pink */
        raw_xc = COERCE(raw_xc, 176, 770);  /* trial and error; image pitch changes if we push to the right */
        raw_yc = COERCE(raw_yc, 444, 950);  /* trial and error; broken image at the edges, outside these limits */
        dbg_printf("-> %d,%d using focus box position\n", raw_xc, raw_yc);
    }
    int current_xc = (x1 + x2) / 2;
    int current_yc = (y1 + y2) / 2;
    int dx = raw_xc - current_xc;
    int dy = raw_yc - current_yc;
    
    if (dx || dy)
    {
        /* note: bits 0x80008000 appear to have no effect,
         * so we'll use them to flag the centered zoom mode,
         * e.g. for focus_box_get_raw_crop_offset */
        dbg_printf("[crop_rec] centering zoom preview: dx=%d, dy=%d\n", dx, dy);
        EngDrvOutLV(0xc0f383d4, PACK32(x1 + dx, y1 + dy) | 0x80008000);
        EngDrvOutLV(0xc0f383dc, PACK32(x2 + dx, y2 + dy) | 0x80008000);
    }

    sei(old);
}

/* faster version than the one from ML core */
static void set_zoom(int zoom)
{
    if (!lv) return;
    if (RECORDING) return;
    if (is_movie_mode() && video_mode_crop) return;
    zoom = COERCE(zoom, 1, 10);
    if (zoom > 1 && zoom < 10) zoom = 5;
    prop_request_change_wait(PROP_LV_DISPSIZE, &zoom, 4, 1000);
}


/* when closing ML menu, check whether we need to refresh the LiveView */
static unsigned int crop_rec_polling_cbr(unsigned int unused)
{
    /* also check at startup */
    static int lv_dirty = 1;

    mcm_af_poll();
    gmt_trace_poll();
    frz_trace_poll();

    int menu_shown = gui_menu_shown();
    if (lv && menu_shown)
    {
        lv_dirty = 1;
    }
    
    if (!lv || menu_shown || RECORDING_RAW)
    {
        /* outside LV: no need to do anything */
        /* don't change while browsing the menu, but shortly after closing it */
        /* don't change while recording raw, but after recording stops
         * (H.264 should tolerate this pretty well, except maybe 50D) */
        return CBR_RET_CONTINUE;
    }

    if (lv_dirty)
    {
        /* do we need to refresh LiveView? */
        if (crop_rec_needs_lv_refresh())
        {
            /* let's check this once again, just in case */
            /* (possible race condition that would result in unnecessary refresh) */
            msleep(500);
            if (crop_rec_needs_lv_refresh())
            {
                info_led_on();
                gui_uilock(UILOCK_EVERYTHING);
                int old_zoom = lv_dispsize;
                set_zoom(lv_dispsize == 1 ? 5 : 1);
                set_zoom(old_zoom);
                gui_uilock(UILOCK_NONE);
                info_led_off();
            }
        }
        lv_dirty = 0;
    }



    return CBR_RET_CONTINUE;
}

/* Display recording status in top info bar */
static LVINFO_UPDATE_FUNC(crop_info)
{
    LVINFO_BUFFER(16);
    
    // if (patch_active)
    // {
    //     if (lv_dispsize > 1)
    //     {
    //         switch (crop_preset)
    //         {
    //             case CROP_PRESET_CENTER_Z:
    //                 snprintf(buffer, sizeof(buffer), "3.5K");
    //                 break;
    //         }
    //     }
    // }



/* EOSM */
  if (CROP_PRESET_MENU == CROP_PRESET_1x3_EOSM)
  {
    snprintf(buffer, sizeof(buffer), "1x3_binning");
  }

  if (CROP_PRESET_MENU == CROP_PRESET_2K_EOSM)
  {
    snprintf(buffer, sizeof(buffer), "2520x1304");
  }

  if (CROP_PRESET_MENU == CROP_PRESET_3K_EOSM)
  {
    snprintf(buffer, sizeof(buffer), "3032x1436");
  }

  if (CROP_PRESET_MENU == CROP_PRESET_4K_EOSM)
  {
    snprintf(buffer, sizeof(buffer), "4K 4038x2558");
  }

  if (CROP_PRESET_MENU == CROP_PRESET_3x3_mv1080_EOSM)
  {
    snprintf(buffer, sizeof(buffer), "mv1080p");
  }

  if (CROP_PRESET_MENU == CROP_PRESET_3x3_mv1080_45fps_EOSM)
  {
    snprintf(buffer, sizeof(buffer), "mv1080p_45fps");
  }

  if (CROP_PRESET_MENU == CROP_PRESET_3x3_1X_EOSM)
  {
    snprintf(buffer, sizeof(buffer), "3x3 720p");
  }

    /* append info about current binning mode */

    if (raw_lv_is_enabled())
    {
        /* fixme: raw_capture_info is only updated when LV RAW is active */

            STR_APPEND("%s%dx%d",
                buffer[0] ? " " : "",
                raw_capture_info.binning_x + raw_capture_info.skipping_x,
                raw_capture_info.binning_y + raw_capture_info.skipping_y
            );
    }

}

static struct lvinfo_item info_items[] = {
    {
        .name = "Crop info",
        .which_bar = LV_BOTTOM_BAR_ONLY,
        .update = crop_info,
        .preferred_position = -50,  /* near the focal length display */
        .priority = 1,
    }
};

static unsigned int raw_info_update_cbr(unsigned int unused)
{

    if (patch_active)
    {
        /* not implemented yet */
        raw_capture_info.offset_x = raw_capture_info.offset_y   = SHRT_MIN;

        if (lv_dispsize > 1)
        {
            /* raw backend gets it right */
            return 0;
        }

        /* update horizontal pixel binning parameters */
        switch (crop_preset)
        {


            case CROP_PRESET_3x3_1X_EOSM:
            case CROP_PRESET_mcm_mv1080_EOSM2:
	    case CROP_PRESET_3x3_mv1080_EOSM:
	    case CROP_PRESET_3x3_mv1080_45fps_EOSM:
 	    case CROP_PRESET_anamorphic_rewired_EOSM2:
 	    case CROP_PRESET_1x3_EOSM:
                raw_capture_info.binning_x = 3; raw_capture_info.skipping_x = 0;
                break;
        }

        /* update vertical pixel binning / line skipping parameters */
        switch (crop_preset)
        {
            case CROP_PRESET_1x3_EOSM:
                raw_capture_info.binning_y = 1; raw_capture_info.skipping_y = 0;
                break;

            case CROP_PRESET_3x3_1X_EOSM:
            case CROP_PRESET_mcm_mv1080_EOSM2:
	    case CROP_PRESET_3x3_mv1080_EOSM:
 	    case CROP_PRESET_anamorphic_rewired_EOSM2:
	    case CROP_PRESET_3x3_mv1080_45fps_EOSM:
            {
                int b = (is_5D3) ? 3 : 1;
                int s = (is_5D3) ? 0 : 2;
                raw_capture_info.binning_y = b; raw_capture_info.skipping_y = s;
                break;
            }
        }

        if (is_5D3 || is_EOSM2)
        {
            /* update skip offsets */
            int skip_left, skip_right, skip_top, skip_bottom;
            calc_skip_offsets(&skip_left, &skip_right, &skip_top, &skip_bottom);
            raw_set_geometry(raw_info.width, raw_info.height, skip_left, skip_right, skip_top, skip_bottom);
        }
    }

    return 0;
}

static unsigned int crop_rec_init()
{

    if (is_camera("EOSM", "2.0.2"))
    {
        CMOS_WRITE = 0x2998C;
        MEM_CMOS_WRITE = 0xE92D41F0;
        
        ADTG_WRITE = 0x2986C;
        MEM_ADTG_WRITE = 0xE92D43F8;
		
	    ENGIO_WRITE = 0xFF2C19AC;
        MEM_ENGIO_WRITE = 0xE51FC15C;
		
        
        is_EOSM = 1;
        crop_presets                = crop_presets_eosm;
        crop_rec_menu[0].choices    = crop_choices_eosm;
        crop_rec_menu[0].max        = COUNT(crop_choices_eosm) - 1;
        crop_rec_menu[0].help       = crop_choices_help_eosm;
        crop_rec_menu[0].help2      = crop_choices_help2_eosm;
    }
    else if (is_camera("EOSM2", "1.0.3"))
    {
        CMOS_WRITE = 0x432A4;
        MEM_CMOS_WRITE = 0xE92D41F0;

        ADTG_WRITE = 0x42E34;
        MEM_ADTG_WRITE = 0xE51F7224;

		ENGIO_WRITE = 0xFF2C6F44;
        MEM_ENGIO_WRITE = 0xe51fc15c;

        MPU_RECV = 0x3964;              /* _mpu_recv, 0xFFD21148 - RAM_OFFSET */
        MEM_MPU_RECV = 0xE92D407C;      /* push {r2-r6, lr} */

        is_EOSM2 = 1;
        crop_presets                = crop_presets_eosm2;
        crop_rec_menu[0].choices    = crop_choices_eosm2;
        crop_rec_menu[0].max        = COUNT(crop_choices_eosm2) - 1;
        crop_rec_menu[0].help       = crop_choices_help_eosm2;
        crop_rec_menu[0].help2      = crop_choices_help2_eosm2;
    }

    /* default FPS timers are the same on all these models */
    if (is_EOSM || is_EOSM2)
    {
        fps_main_clock = 32000000;
                                       /* 24p,  25p,  30p,  50p,  60p,   x5, c24p, c25p, c30p */
        memcpy(default_timerA, (int[]) {  528,  640,  528,  640,  528,  716,  546,  640,  546 }, 36);
        memcpy(default_timerB, (int[]) { 2527, 2000, 2022, 1000, 1011, 1491, 2444, 2000, 1955 }, 36);
                                   /* or 2528        2023        1012        2445        1956 */
    }

    /* FPS in x5 zoom may be model-dependent; assume exact */
    default_fps_1k[5] = (uint64_t) fps_main_clock * 1000ULL / default_timerA[5] / default_timerB[5];

    printf("[crop_rec] checking FPS timer values...\n");
    for (int i = 0; i < COUNT(default_fps_1k); i++)
    {
        if (default_timerA[i])
        {
            int fps_i = (uint64_t) fps_main_clock * 1000ULL / default_timerA[i] / default_timerB[i];
            if (fps_i == default_fps_1k[i])
            {
                printf("%d) %s%d.%03d: A=%d B=%d (exact)\n", i, FMT_FIXEDPOINT3(default_fps_1k[i]), default_timerA[i], default_timerB[i]);

                if (i == 5 && default_fps_1k[i] != 29970)
                {
                    printf("-> unusual FPS in x5 zoom\n", i);
                }
            }
            else
            {
                int fps_p = (uint64_t) fps_main_clock * 1000ULL / default_timerA[i] / (default_timerB[i] + 1);
                if (fps_i > default_fps_1k[i] && fps_p < default_fps_1k[i])
                {
                    printf("%d) %s%d.%03d: A=%d B=%d/%d (averaged)\n", i, FMT_FIXEDPOINT3(default_fps_1k[i]), default_timerA[i], default_timerB[i], default_timerB[i] + 1);
                }
                else
                {
                    printf("%d) %s%d.%03d: A=%d B=%d (%s%d.%03d ?!?)\n", i, FMT_FIXEDPOINT3(default_fps_1k[i]), default_timerA[i], default_timerB[i], FMT_FIXEDPOINT3(fps_i));
                    return CBR_RET_ERROR;
                }

                /* assume 25p is exact on all models */
                if (i == 1)
                {
                    printf("-> 25p check error\n");
                    return CBR_RET_ERROR;
                }
            }
        }
    }

    menu_add("Movie", crop_rec_menu, COUNT(crop_rec_menu));
    lvinfo_add_items (info_items, COUNT(info_items));

    /* When booting straight into LiveView, PROP_LV_ACTION / PROP_LV_DISPSIZE usually fire
     * before our prop handlers are registered, so the hooks were only installed after
     * something re-triggered them (e.g. pressing MENU twice). Do it once here; the
     * polling CBR refreshes LiveView at startup so the mode is applied cleanly. */
    update_patch();

    return 0;
}

static unsigned int crop_rec_deinit()
{
    return 0;
}

MODULE_INFO_START()
    MODULE_INIT(crop_rec_init)
    MODULE_DEINIT(crop_rec_deinit)
MODULE_INFO_END()

MODULE_CONFIGS_START()
    MODULE_CONFIG(crop_preset_index)
    MODULE_CONFIG(shutter_range)
    MODULE_CONFIG(bitdepth)
    MODULE_CONFIG(ratios)
    MODULE_CONFIG(x3crop)
    MODULE_CONFIG(set_25fps)
    MODULE_CONFIG(HDR_iso_a)
    MODULE_CONFIG(HDR_iso_b)
    MODULE_CONFIG(mcm_af)
MODULE_CONFIGS_END()

MODULE_CBRS_START()
    MODULE_CBR(CBR_SHOOT_TASK, crop_rec_polling_cbr, 0)
    MODULE_CBR(CBR_RAW_INFO_UPDATE, raw_info_update_cbr, 0)
MODULE_CBRS_END()

MODULE_PROPHANDLERS_START()
    MODULE_PROPHANDLER(PROP_LV_ACTION)
    MODULE_PROPHANDLER(PROP_LV_DISPSIZE)
    MODULE_PROPHANDLER(PROP_LIVE_VIEW_AF_SYSTEM)
    MODULE_PROPHANDLER(PROP_VIDEO_MODE)
    MODULE_PROPHANDLER(PROP_MOVIE_SERVO_AF)
MODULE_PROPHANDLERS_END()
