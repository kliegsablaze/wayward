/* Wayward — six loops, each keeping its own time.
 *
 * A Schwung audio_fx module for Ableton Move. Six independent recordings,
 * each looped at its own tempo. Set them to 100, 101, 102, 103, 104 and 105
 * BPM and they slide out of alignment and back over minutes — the Reich /
 * Eno / Basinski tape-loop mechanism, on eight knobs.
 *
 * THE ENGINE, in one paragraph. Each loop holds three numbers: the WINDOW
 * that START and END cut out of its take (W frames), the PERIOD its tempo
 * asks for (P frames, beats * 60/bpm * 44100), and a position counting
 * frames into the current cycle and wrapping at P. Playback reads the take
 * while the position is inside the window and emits silence past it, so
 * "pad with silence" and "cut off at the wrap" are the same arithmetic and
 * neither one touches the recording. Six of those, at tempi a beat or two
 * apart, is the whole instrument.
 *
 * PAGES (the host renders 8 knobs per page, 4 across x 2 rows; the six loop
 * pages are one CHILD LEVEL that the host multiplies):
 *
 *   Main    REC 1 REC 2 REC 3 PLAY  /  REC 4 REC 5 REC 6 RSYN
 *   Orbits  1     2     3     ALIGN /  ....  4     5     6
 *   Shape   BASE  SPRD  WIDEN ....  /  DRY   OUT   ....  CLEAR
 *   Mix     1     2     3     ....  /  4     5     6     ....
 *   Loop    LOOP  TRIG  START END   /  BPM   BEAT  FIT   PHAS
 *           (one page for all six; LOOP chooses which)
 *
 * Eight sections is one more than Forgetful ships. drawBankBar handles any
 * page count up to the display width and the planner caps levels nowhere,
 * so this is fine; it was checked before the pages were built.
 *
 * HOUSE RULES INHERITED FROM FORGETFUL, each learned the hard way there:
 *
 *   - There is no control thread. create_instance, destroy_instance,
 *     set_param, get_param and process_block ALL run on the SPI audio
 *     callback (SCHED_FIFO 90, core 3, ~900us per 128-frame block). No
 *     malloc outside create_instance, no file I/O, no locks, no logging —
 *     not even fprintf(stderr, ...), which is an unbuffered write() syscall.
 *     See host/plugin_api_v1.h:8-70.
 *
 *   - get_param has THREE answers, not two: text, "" (served, nothing
 *     there), and -1 (read did not complete). The host probes preset_name,
 *     is_loading and display_name on every repaint, and -1 is retried
 *     forever — Forgetful logged 19,913 giveup events, roughly one a second,
 *     before it learned this. Every unrecognised key goes through
 *     param_absent(), never -1.
 *
 *   - The host learns the enum wire format from get_param and writes back in
 *     kind, so set_param must string-match labels BEFORE any atoi fallback.
 *     Corollary, and the reason BEAT is a float here rather than the enum
 *     the design first proposed: an enum whose labels are bare numbers is
 *     ambiguous on this wire, because "2" is both the label "2" and the
 *     index 2. No enum in this module has a numeric label.
 *
 *   - "unit":"%" makes the host multiply by 100 for display. Declare 0..1,
 *     or the screen reads 5000%.
 *
 *   - "" in a knobs[] array is a real, load-bearing blank cell: the renderer
 *     and the knob handlers treat a falsy key as "nothing here" and bail
 *     before touching metaIndex. Only literal null is filtered.
 *
 *   - Labels are capped at 5 characters by the host's LABEL_CHARS, and
 *     values are effectively capped at 5-6 by the cell width.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host/audio_fx_api_v2.h"
#include "host/plugin_api_v1.h"

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

#define NUM_LOOPS        6
#define SAMPLE_RATE      44100
#define BUFFER_SECONDS   30
#define BUFFER_CAPACITY  (SAMPLE_RATE * BUFFER_SECONDS)

/* 6 loops x 30 s x 44100 x 4 B = 31.75 MB, against Forgetful's 42.3 MB.
 * Thirty seconds is far more headroom than a loop window needs; the cap
 * exists so that forgetting to press REC a second time cannot record
 * forever. */

#define MIN_BPM          40.0f
#define MAX_BPM          200.0f
#define DEFAULT_BPM      100.0f

#define MIN_BEATS        1.0f
#define MAX_BEATS        16.0f
#define DEFAULT_BEATS    4.0f

#define MIN_SPREAD       (-12.0f)
#define MAX_SPREAD       12.0f
#define DEFAULT_SPREAD   1.0f

/* A take shorter than this is not a loop, it is a click. */
#define MIN_WINDOW_FRAMES ((int)(SAMPLE_RATE * 0.05f))

/* The envelope the F modes apply at each end of the window. Five
 * milliseconds is long enough to swallow a splice and short enough not to
 * soften a transient audibly. */
#define FADE_SECONDS     0.005f

/* AUTOMATIC LEADING-SILENCE TRIM.
 *
 * A take almost always opens with the moment between pressing REC and
 * actually playing something, and every frame of it shifts the window, so
 * START would otherwise have to be dialled in by hand on every recording.
 * The take therefore begins at its first audible frame rather than its first
 * frame.
 *
 * This is an OFFSET, not a move: nothing is copied. Trimming by memmove
 * would shift up to 5 MB on the audio thread, several times the 900 us block
 * budget on its own.
 *
 * The threshold is about -60 dBFS, under any room tone worth keeping and
 * over a converter's noise floor. The pre-roll then backs the origin up 5 ms
 * from the frame that crossed it, because the crossing happens partway UP an
 * attack rather than at the start of one — trimming exactly to it shaves the
 * leading edge off every transient.
 *
 * The onset is found DURING recording, one comparison per frame, rather than
 * by scanning at close: a 30 second take is 1.3 million frames, and scanning
 * it would blow the block budget in a single call. */
#define SILENCE_THRESHOLD 32
#define PREROLL_FRAMES    ((int)(SAMPLE_RATE * 0.005f))

/* An overdub head moving at anything but one frame per frame skips indices;
 * left alone that writes a comb into the take permanently. The gap gets
 * filled, but only up to this many frames — beyond it the head has jumped
 * (a wrap, a knob), and filling would smear the new input across the take. */
#define OVERDUB_MAX_FILL 64

/* Per-block gain chase. At 128 frames a block this is roughly a 12 ms glide,
 * which is enough to keep a knob step off a multiply. */
#define GAIN_SLEW        0.25f

/* HOW FAST PHASE IS ALLOWED TO MOVE THE PLAYHEAD, in extra frames per frame.
 *
 * PHASE offsets by a fraction of a CYCLE, so its effect on the read position
 * is scaled by the period: one detent is 0.005 of a cycle, which at 100 BPM
 * x 4 beats is 529 frames. Applying that as a position — even smoothed
 * between blocks — teleports the playhead into unrelated audio, and a knob
 * sweep does it hundreds of times a second. That is a splice, not a glide.
 *
 * So the offset is rate-limited per FRAME instead, and the limit is
 * expressed as a speed deviation: the loop runs up to 25% fast or 25% slow
 * until it arrives. Nothing is ever spliced, and the sound is the right one
 * — this is exactly what a performer does to a tape loop by hand, and what
 * Reich's players did to pull Piano Phase apart.
 *
 * A whole detent lands in about 50 ms; the full half-cycle sweep takes a few
 * seconds, and audibly slides the loop into place while it happens. */
#define PHASE_SLIDE_RATE 0.25

/* CLEAR takes this long to arrive.
 *
 * The delay is the safety. CLEAR sits between two controls you reach for
 * constantly, and it throws away every take on the module — but it announces
 * itself by fading what is playing over a quarter of a minute, and a second
 * press calls it off. A confirmation dialog would need a screen this module
 * does not have; a long audible fade needs nothing and is impossible to
 * miss.
 *
 * It is also the right musical shape: the last gesture of a piece is usually
 * to let it go quiet. */
#define CLEAR_SECONDS    15.0f

typedef struct { int16_t l, r; } frame16_t;

typedef enum {
    LOOP_EMPTY = 0,   /* nothing recorded yet */
    LOOP_RECORDING,   /* capturing from the input */
    LOOP_LOADED       /* has a take; plays when the ensemble runs */
} loop_state_t;

/* Six modes: each way of reconciling window and period, immediately followed
 * by its faded companion. The companion applies a short volume envelope to
 * the tail of the window, taking it to zero so the seam cannot click. The raw
 * modes keep the click deliberately — a hard splice is a percussive event,
 * and tape music is full of them. */
typedef enum {
    FIT_PAD = 0, FIT_PAD_F,
    FIT_SPD,     FIT_SPD_F,
    FIT_RPT,     FIT_RPT_F
} fit_mode_t;

/* No numeric labels anywhere — see the wire-format note in the header. */
static const char *const FIT_LABELS[]  = {
    "PAD", "PADF", "SPD", "SPDF", "RPT", "RPTF"
};
#define FIT_COUNT 6

/* True for the companion modes — the odd indices. */
#define FIT_IS_FADED(f) (((int)(f) & 1) != 0)

/* Triggers: any write that is not the idle spelling fires them. */
static const char *const TRIGGER_OPTIONS_JSON = "[\"-\",\"GO\"]";

static const char *const LOOP_PREFIXES[NUM_LOOPS] = {
    "loop1_", "loop2_", "loop3_", "loop4_", "loop5_", "loop6_"
};

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    frame16_t *buffer;
    int        capacity_frames;
    int        write_head;        /* frames captured so far this take */
    int        origin;            /* the take's first frame after the
                                   * leading-silence trim; every window
                                   * position is relative to this */
    int        recorded_length;   /* usable frames, counted from origin */
    int        lead_idx;          /* first frame over the silence threshold,
                                   * -1 while nothing has crossed it */

    loop_state_t state;
    int overdubbing;   /* LOADED loops only: layering new input onto the take */

    /* The window, as fractions of the take.
     *
     * START is an ordinary position, 0..1.
     *
     * END is a BIPOLAR LENGTH measured from START, -1..+1, not a second
     * position — which is why there is no ordering to enforce between the
     * two and no way to set an inside-out window:
     *
     *     +1.0   forward from START all the way to the end of the take
     *     +0.5   forward, half the material that lies after START
     *      0.0   zero length
     *     -0.5   BACKWARD from START, half the material that lies before it,
     *            played in reverse
     *     -1.0   backward to the very head of the take, reversed
     *
     * So the two halves of the knob are mirror images about a silent centre,
     * and the reverse half grows into material the forward half can never
     * reach. The span is still resolved per block rather than in set_param,
     * so a knob never snaps back under the hand. */
    float start_frac;
    float end_frac;

    float bpm;         /* this loop's tempo */
    float beats;       /* period = beats * 60/bpm seconds */
    fit_mode_t fit;
    float phase_off;   /* -0.5..+0.5 of a cycle */
    float volume;

    /* Playback position, in FRAMES INTO THE CYCLE, wrapping at `period`.
     *
     * Not a normalised 0..1 phase, which is what this was first: with a
     * normalised phase the read position is phase * period, so turning BPM
     * SCALES it and teleports the playhead through the take — in PAD mode
     * far enough to jump from inside the window to outside it, cutting the
     * sound dead mid-note. Counting frames instead leaves the playhead
     * exactly where it is when the period changes; only the wrap point
     * moves. */
    double pos;

    int    audition;      /* one-shot TRIG in flight */
    double audition_pos;  /* frames into the window */

    /* Chased once per block, so a knob step never lands straight on a
     * multiply — that is what a zipper is. */
    float  vol_cur;

    /* PHASE, rate-limited per FRAME rather than chased per block: see
     * PHASE_SLIDE_RATE. A double because it is multiplied by the period to
     * get a frame position. */
    double phase_off_cur;
    double phase_step;    /* cycles per frame, from the rate limit */

    /* Resolved once per block by the prepass, read every frame. */
    double w_lo, w_hi;    /* the window's bounds inside the take */
    double span;          /* w_hi - w_lo; 0 means this loop is silent */
    int    reversed;      /* END below centre: travel from w_hi down to w_lo */
    double period;        /* frames per cycle, beats * 60/bpm * SR */

    double fade;          /* frames of envelope at each end, F modes only */
    float  gl, gr;        /* stereo placement, from WIDEN */

    /* Where the overdub last wrote, so a head moving faster than one frame
     * per frame can fill the gap it jumped rather than leaving a comb. */
    int    od_last_idx;
} loop_t;

typedef struct {
    loop_t loops[NUM_LOOPS];

    int   playing;
    float base_bpm;
    float spread;
    float dry;
    float out;
    float widen;

    float dry_cur, out_cur;   /* chased, as the per-loop gains are */

    /* Which loop the consolidated Loop page is editing, 1..6. The module
     * barely uses it — every control on that page addresses a concrete
     * loopN_ key — but the HOST reads it every tick to decide which loop the
     * page's generic keys resolve to. */
    int   loop_select;

    /* Frames since every phase was last zeroed together — the instant the
     * realignment countdown measures from. */
    uint64_t align_frames;

    /* CLEAR in flight: the ensemble is fading toward being wiped. */
    int   clearing;
    float clear_gain;   /* 1 down to 0 across CLEAR_SECONDS */
    float clear_step;   /* per frame, resolved once per block */

    uint64_t total_frames;
} inst_t;

static const host_api_v1_t *g_host = NULL;
static audio_fx_api_v2_t    g_api;

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Label match first, index second. The host learns which of the two it
 * should send from whatever get_param last returned, so labels must win;
 * the atoi path only exists for hosts that never learned. */
static int enum_index_from(const char *val, const char *const *labels,
                           int count, int fallback) {
    if (!val) return fallback;
    for (int i = 0; i < count; i++) {
        if (strcmp(val, labels[i]) == 0) return i;
    }
    /* Only treat it as an index if it actually looks like one. */
    const char *p = val;
    if (*p == '\0') return fallback;
    for (; *p; p++) {
        if (*p < '0' || *p > '9') return fallback;
    }
    int idx = atoi(val);
    if (idx < 0 || idx >= count) return fallback;
    return idx;
}

/* A trigger fires on anything that is not the idle spelling. */
static int trigger_fired(const char *val) {
    if (!val) return 0;
    return strcmp(val, "-") != 0 && strcmp(val, "0") != 0;
}

/* Loop i runs at base + i*spread, rounded to a whole number: whole-number
 * tempo relationships are what produce clean phase cycles (100 vs 101
 * realigns after 100 cycles; 100 vs 100.37 merely smears). */
static void apply_spread(inst_t *s) {
    for (int i = 0; i < NUM_LOOPS; i++) {
        float bpm = s->base_bpm + (float)i * s->spread;
        s->loops[i].bpm = clampf(roundf(bpm), MIN_BPM, MAX_BPM);
    }
}

static void zero_all_phases(inst_t *s) {
    for (int i = 0; i < NUM_LOOPS; i++) s->loops[i].pos = 0.0;
    s->align_frames = 0;
}

static int igcd(int a, int b) {
    if (a < 0) a = -a;
    if (b < 0) b = -b;
    while (b) { const int t = a % b; a = b; b = t; }
    return a;
}

/* HOW LONG UNTIL EVERY LOADED LOOP COINCIDES AGAIN, in seconds.
 *
 * A loop's period is beats * 60/bpm seconds, so with whole-number tempi the
 * periods are rational and their common multiple is exact arithmetic rather
 * than a search: reduce each beats/bpm to lowest terms, and the least common
 * multiple of a set of fractions is lcm(numerators) over gcd(denominators).
 *
 * The answer is prettier than it has any right to be. Six loops all at BEAT 4
 * come back together every 4 x 60 = 240 seconds at ANY whole tempi at all,
 * because in that time each completes exactly its own BPM count of cycles.
 * Change one loop's BEAT and the figure moves.
 *
 * Returns 0 when nothing is loaded. */
static double align_period_seconds(const inst_t *s) {
    int num = 0;          /* lcm of the reduced numerators */
    int den = 0;          /* gcd of the reduced denominators */
    for (int i = 0; i < NUM_LOOPS; i++) {
        const loop_t *lp = &s->loops[i];
        if (lp->state != LOOP_LOADED || lp->recorded_length < 2) continue;
        int p = (int)lp->beats;
        int q = (int)lp->bpm;
        if (p < 1) p = 1;
        if (q < 1) q = 1;
        const int g = igcd(p, q);
        p /= g;
        q /= g;
        num = num ? (num / igcd(num, p)) * p : p;
        den = den ? igcd(den, q) : q;
    }
    if (!num || !den) return 0.0;
    return 60.0 * (double)num / (double)den;
}

static void init_loop(loop_t *loop) {
    loop->buffer          = NULL;
    loop->capacity_frames = 0;
    loop->write_head      = 0;
    loop->origin          = 0;
    loop->recorded_length = 0;
    loop->lead_idx        = -1;
    loop->state           = LOOP_EMPTY;
    loop->overdubbing     = 0;
    loop->start_frac      = 0.0f;
    loop->end_frac        = 1.0f;
    loop->bpm             = DEFAULT_BPM;
    loop->beats           = DEFAULT_BEATS;
    loop->fit             = FIT_PAD;
    loop->phase_off       = 0.0f;
    loop->volume          = 0.8f;
    loop->pos             = 0.0;
    loop->audition        = 0;
    loop->audition_pos    = 0.0;
    loop->vol_cur         = loop->volume;
    loop->phase_off_cur   = 0.0;
    loop->phase_step      = 0.0;
    loop->w_lo = loop->w_hi = 0.0;
    loop->span = loop->period = loop->fade = 0.0;
    loop->reversed        = 0;
    loop->gl = loop->gr   = 1.0f;
    loop->od_last_idx     = -1;
}

/* Everything a loop is, except the buffer it owns. init_loop clears the
 * pointer as well, which is right at construction and catastrophic here. */
static void reset_loop_keep_buffer(loop_t *loop) {
    frame16_t *buf = loop->buffer;
    const int cap = loop->capacity_frames;
    init_loop(loop);
    loop->buffer = buf;
    loop->capacity_frames = cap;
}

/* The end of a CLEAR: every take gone, every setting back to where it
 * started.
 *
 * The audio buffers are deliberately NOT zeroed. Wiping 31.7 MB would take
 * far longer than the 900 us this block has, and it buys nothing — a take
 * with no recorded length cannot be reached, and the next recording
 * overwrites from the beginning anyway. */
static void wipe_all(inst_t *s) {
    for (int i = 0; i < NUM_LOOPS; i++) {
        reset_loop_keep_buffer(&s->loops[i]);
        /* The prepass ran at the top of this block against the loop as it
         * was. Clearing the span too stops the rest of the block reading
         * through a window that no longer has a take behind it. */
        s->loops[i].span = 0.0;
    }
    s->playing   = 0;
    s->base_bpm  = DEFAULT_BPM;
    s->spread    = DEFAULT_SPREAD;
    s->dry       = 1.0f;
    s->out       = 0.8f;
    s->widen     = 0.5f;
    apply_spread(s);

    s->clearing    = 0;
    s->clear_gain  = 1.0f;
    s->loop_select = 1;
}

/* ------------------------------------------------------------------ */
/* Readouts                                                            */
/* ------------------------------------------------------------------ */

/* WHERE A LOOP IS IN ITS CYCLE, and what it is doing.
 *
 * This replaces the single six-character STATE readout that used to sit on
 * Main. That one had to say everything about six loops in six characters; a
 * cell per loop can say where each one actually IS, which is the thing this
 * instrument is about.
 *
 * The vocabulary still carries everything STATE did, because with STATE gone
 * this is the only place recording is visible at all — a write-only trigger's
 * cell shows its static label and nothing else:
 *
 *     -      nothing recorded
 *     REC    recording right now
 *     S      has a take, ensemble stopped
 *     0..99  playing, and how far through its cycle it is
 *     O42    overdubbing, and how far through its cycle it is
 *
 * A bare number is safe here: enumSquareLines sends an all-digit value
 * straight to one line, and a lone "-" has no alphanumeric beside it to be
 * treated as a word separator. "O42" is neither all digits nor separated, so
 * it survives whole as well. */
static int loop_cycle_text(const inst_t *s, const loop_t *loop,
                           char *buf, int len) {
    if (loop->state == LOOP_RECORDING) return snprintf(buf, len, "REC");
    if (loop->state != LOOP_LOADED || loop->recorded_length < 2)
        return snprintf(buf, len, "-");
    if (!s->playing) return snprintf(buf, len, "S");

    int pct = 0;
    if (loop->period > 0.0) {
        pct = (int)(loop->pos / loop->period * 100.0);
        if (pct < 0) pct = 0;
        if (pct > 99) pct = 99;
    }
    if (loop->overdubbing) return snprintf(buf, len, "O%d", pct);
    return snprintf(buf, len, "%d", pct);
}

/* Seconds until every loaded loop coincides again — or, while a CLEAR is
 * running, the countdown to the wipe. The clear borrows this cell because it
 * is the only readout left, and fifteen seconds of it matter more than a
 * figure usually measured in minutes.
 *
 * The number assumes nothing has been retuned since the last alignment. Turn
 * a BPM or a BEAT and the loops no longer share the instant it counts from,
 * so it jumps to describe the new arrangement rather than the old one.
 * RESYNC makes it true again. */
static int master_align_text(const inst_t *s, char *buf, int len) {
    if (s->clearing) {
        int secs = (int)(s->clear_gain * CLEAR_SECONDS + 0.999f);
        if (secs < 1) secs = 1;
        return snprintf(buf, len, "CLR%d", secs);
    }
    const double t = align_period_seconds(s);
    if (t <= 0.0 || !s->playing) return snprintf(buf, len, "-");

    const double elapsed = (double)s->align_frames / (double)SAMPLE_RATE;
    double rem = t - fmod(elapsed, t);
    if (rem < 0.0) rem = 0.0;

    if (rem < 600.0) return snprintf(buf, len, "%d", (int)rem);
    int mins = (int)(rem / 60.0);
    if (mins > 999) mins = 999;
    return snprintf(buf, len, "%dM", mins);
}

/* Transport buttons name what the NEXT press will DO, not what is happening
 * now — the convention a play/pause button follows when it reads "Pause"
 * while playing. Forgetful arrived at this after three other vocabularies. */
static int loop_record_text(const loop_t *loop, char *buf, int len) {
    if (loop->state == LOOP_RECORDING) return snprintf(buf, len, "STOP");
    if (loop->state == LOOP_LOADED) {
        return snprintf(buf, len, loop->overdubbing ? "PLAY" : "DUB");
    }
    return snprintf(buf, len, "REC");
}

/* ------------------------------------------------------------------ */
/* Recording state machine                                             */
/* ------------------------------------------------------------------ */

static void close_recording(loop_t *loop) {
    /* Where the take really starts, and how much of it is left once the
     * silence in front of it is skipped. */
    const int first = loop->lead_idx;
    int origin = 0, len = 0;
    if (first >= 0) {
        origin = first - PREROLL_FRAMES;
        if (origin < 0) origin = 0;
        len = loop->write_head - origin;
    }

    /* Nothing ever crossed the threshold, or what is left is too short to be
     * anything. Throw it away rather than keep a click — or, worse, a loop
     * that reads as loaded and plays nothing. */
    if (first < 0 || len < MIN_WINDOW_FRAMES) {
        loop->write_head      = 0;
        loop->origin          = 0;
        loop->recorded_length = 0;
        loop->lead_idx        = -1;
        loop->overdubbing     = 0;
        loop->state           = LOOP_EMPTY;
        return;
    }

    loop->origin          = origin;
    loop->recorded_length = len;
    loop->start_frac      = 0.0f;
    loop->end_frac        = 1.0f;   /* the whole take, forwards */
    loop->overdubbing     = 0;
    loop->od_last_idx     = -1;
    /* The take begins its cycle the moment it closes, rather than waiting
     * for the ensemble's next wrap: press stop and the loop is running. */
    loop->pos             = 0.0;
    loop->state           = LOOP_LOADED;
}

/* Three destinations from one button, as in Forgetful: an idle loop starts
 * recording, a recording loop closes the take, and a loaded loop toggles
 * OVERDUB — layering fresh input onto what is already there, pass after pass,
 * without ever stopping. */
static void loop_record_press(loop_t *loop) {
    if (loop->state == LOOP_RECORDING) {
        close_recording(loop);
    } else if (loop->state == LOOP_LOADED) {
        loop->overdubbing = !loop->overdubbing;
    } else {
        loop->write_head      = 0;
        loop->origin          = 0;
        loop->recorded_length = 0;
        loop->lead_idx        = -1;
        loop->overdubbing     = 0;
        loop->state           = LOOP_RECORDING;
    }
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

static void *v2_create_instance(const char *dir, const char *cfg) {
    (void)dir;
    (void)cfg;   /* module.json declares no "defaults" section */

    inst_t *s = (inst_t *)calloc(1, sizeof(inst_t));
    if (!s) return NULL;

    s->playing   = 0;
    s->base_bpm  = DEFAULT_BPM;
    s->spread    = DEFAULT_SPREAD;  /* 100..105 out of the box */
    s->dry       = 1.0f;            /* an fx that silences its input is a bug */
    s->out       = 0.8f;
    s->widen     = 0.5f;
    s->dry_cur   = s->dry;
    s->out_cur   = s->out;
    s->clearing   = 0;
    s->clear_gain = 1.0f;
    s->loop_select = 1;

    for (int i = 0; i < NUM_LOOPS; i++) {
        init_loop(&s->loops[i]);
        s->loops[i].buffer =
            (frame16_t *)calloc((size_t)BUFFER_CAPACITY, sizeof(frame16_t));
        if (!s->loops[i].buffer) {
            /* Out of memory: hand back everything and refuse to load, rather
             * than run with a loop that cannot record. */
            for (int j = 0; j < i; j++) free(s->loops[j].buffer);
            free(s);
            return NULL;
        }
        s->loops[i].capacity_frames = BUFFER_CAPACITY;
    }

    apply_spread(s);
    return s;
}

static void v2_destroy_instance(void *instance) {
    inst_t *s = (inst_t *)instance;
    if (!s) return;
    for (int i = 0; i < NUM_LOOPS; i++) free(s->loops[i].buffer);
    free(s);
}

/* ------------------------------------------------------------------ */
/* Audio                                                               */
/* ------------------------------------------------------------------ */

/* Cubic soft clipper. Unity slope through zero, reaching exactly +-1 with
 * zero slope at +-1.5, so it rounds an overshoot instead of folding it. Six
 * loops plus the live input overshoot easily once the faders are up.
 *
 * The linear region is left EXACT rather than merely near-unity, because the
 * idle case — nothing recorded, DRY at full — has to come out bit for bit
 * what went in. An effect that alters the signal while doing nothing is a
 * bug, and process_block skips the write entirely in that case (see below);
 * this keeps the arithmetic honest for everything short of it. */
static float softclip(float x) {
    if (x <= -1.5f) return -1.0f;
    if (x >=  1.5f) return  1.0f;
    return x - (4.0f / 27.0f) * x * x * x;
}

static int16_t f_to_i16(float v) {
    v *= 32768.0f;
    if (v >  32767.0f) v =  32767.0f;
    if (v < -32768.0f) v = -32768.0f;
    return (int16_t)v;
}

/* Linearly interpolated read at a fractional frame position in the take.
 * The position is clamped rather than wrapped: every caller has already
 * confined it to the window, and a wrap here would quietly paper over a bug
 * in that arithmetic. */
static void read_frame(const loop_t *loop, double rp, float *l, float *r) {
    const int len = loop->recorded_length;
    if (rp < 0.0) rp = 0.0;
    if (rp > (double)(len - 1)) rp = (double)(len - 1);
    const int i0 = (int)rp;
    int i1 = i0 + 1;
    if (i1 >= len) i1 = len - 1;
    const float f = (float)(rp - (double)i0);
    const frame16_t a = loop->buffer[loop->origin + i0];
    const frame16_t b = loop->buffer[loop->origin + i1];
    *l = ((float)a.l + ((float)b.l - (float)a.l) * f) * (1.0f / 32768.0f);
    *r = ((float)a.r + ((float)b.r - (float)a.r) * f) * (1.0f / 32768.0f);
}

/* Sum new input into the take at one index, saturating. Overdub ADDS — the
 * take is meant to accumulate pass after pass. */
static void overdub_write(loop_t *loop, int i, int16_t l, int16_t r) {
    i += loop->origin;   /* window positions are relative to the trim */
    int nl = (int)loop->buffer[i].l + (int)l;
    int nr = (int)loop->buffer[i].r + (int)r;
    if (nl >  32767) nl =  32767;
    if (nl < -32768) nl = -32768;
    if (nr >  32767) nr =  32767;
    if (nr < -32768) nr = -32768;
    loop->buffer[i].l = (int16_t)nl;
    loop->buffer[i].r = (int16_t)nr;
}

/* The envelope the F modes apply, as a gain for a position within the span.
 * Both ends, not only the tail: a fade-out alone still leaves the window's
 * first frame stepping away from silence, which clicks exactly as loudly as
 * the seam it was meant to fix. */
static float window_gain(const loop_t *loop, double idx) {
    if (loop->fade <= 0.0) return 1.0f;
    float g = 1.0f;
    if (idx < loop->fade) g = (float)(idx / loop->fade);
    const double tail = loop->span - idx;
    if (tail < loop->fade) {
        const float t = (float)(tail / loop->fade);
        if (t < g) g = t;
    }
    return g < 0.0f ? 0.0f : g;
}

/* Once per block per loop: everything that would otherwise be recomputed
 * 128 times for no reason — the window bounds, the period, the fade length,
 * the stereo placement — plus the gain chases. */
static void prepass(inst_t *s, loop_t *lp, int index) {
    lp->vol_cur += (lp->volume - lp->vol_cur) * GAIN_SLEW;

    /* WIDEN fans the six evenly about the centre. Constant power, scaled so
     * that WIDEN 0 leaves every loop at unity: without that, closing the
     * spread would quietly drop the ensemble 3 dB. */
    float pos = ((float)index - (NUM_LOOPS - 1) * 0.5f) / ((NUM_LOOPS - 1) * 0.5f);
    pos *= s->widen;
    const float ang = (pos + 1.0f) * 0.25f * 3.14159265358979f;
    lp->gl = cosf(ang) * 1.41421356f;
    lp->gr = sinf(ang) * 1.41421356f;

    lp->span = 0.0;
    if (lp->state != LOOP_LOADED || lp->recorded_length < 2) return;

    const double len = (double)lp->recorded_length;
    const double start = (double)clampf(lp->start_frac, 0.0f, 1.0f) * len;
    const double e = (double)clampf(lp->end_frac, -1.0f, 1.0f);

    /* END is a signed LENGTH from START, so the two halves of the knob are
     * mirror images and there is no ordering to enforce. */
    if (e >= 0.0) {
        lp->w_lo = start;
        lp->w_hi = start + e * (len - start);
        lp->reversed = 0;
    } else {
        lp->w_lo = start + e * start;   /* e is negative: this walks back */
        lp->w_hi = start;
        lp->reversed = 1;
    }
    if (lp->w_lo < 0.0) lp->w_lo = 0.0;
    if (lp->w_hi > len) lp->w_hi = len;

    lp->span = lp->w_hi - lp->w_lo;
    /* The centre detent is meant to be silent, so a vanishing window is not
     * clamped up to some minimum — it is simply not heard. */
    if (lp->span < 2.0) { lp->span = 0.0; return; }

    double period = (double)lp->beats * (60.0 / (double)lp->bpm) * (double)SAMPLE_RATE;
    if (period < 2.0) period = 2.0;
    lp->period = period;
    /* The rate limit is a speed deviation, so converting it to cycles per
     * frame depends on the period — a nudge is the same 25% at any tempo. */
    lp->phase_step = PHASE_SLIDE_RATE / period;

    double fade = FIT_IS_FADED(lp->fit) ? (double)(FADE_SECONDS * SAMPLE_RATE) : 0.0;
    if (fade > lp->span * 0.25) fade = lp->span * 0.25;
    lp->fade = fade;
}

static void v2_process_block(void *instance, int16_t *lr, int frames) {
    inst_t *s = (inst_t *)instance;
    if (!s || !lr || frames <= 0) return;

    s->clear_step = 1.0f / (CLEAR_SECONDS * (float)SAMPLE_RATE);

    s->dry_cur += (s->dry - s->dry_cur) * GAIN_SLEW;
    s->out_cur += (s->out - s->out_cur) * GAIN_SLEW;
    for (int i = 0; i < NUM_LOOPS; i++) prepass(s, &s->loops[i], i);

    for (int n = 0; n < frames; n++) {
        const int16_t raw_l = lr[2 * n];
        const int16_t raw_r = lr[2 * n + 1];

        float acc_l = 0.0f, acc_r = 0.0f;

        for (int i = 0; i < NUM_LOOPS; i++) {
            loop_t *lp = &s->loops[i];

            if (lp->state == LOOP_RECORDING) {
                if (lp->write_head < lp->capacity_frames) {
                    /* One comparison per frame, so the take learns where it
                     * really begins without ever having to scan itself. */
                    if (lp->lead_idx < 0) {
                        const int al = raw_l < 0 ? -raw_l : raw_l;
                        const int ar = raw_r < 0 ? -raw_r : raw_r;
                        if (al > SILENCE_THRESHOLD || ar > SILENCE_THRESHOLD)
                            lp->lead_idx = lp->write_head;
                    }
                    lp->buffer[lp->write_head].l = raw_l;
                    lp->buffer[lp->write_head].r = raw_r;
                    lp->write_head++;
                }
                /* Running out of buffer closes the take. This is the one
                 * automatic close: a safety cap, so a forgotten second press
                 * cannot record forever. */
                if (lp->write_head >= lp->capacity_frames) close_recording(lp);
                continue;
            }

            if (lp->state != LOOP_LOADED || lp->span <= 0.0) {
                lp->od_last_idx = -1;
                continue;
            }

            float sl = 0.0f, sr = 0.0f;
            int   voiced = 0;
            int   play_voiced = 0;
            double rp = 0.0;

            if (s->playing) {
                /* A period that has just shrunk below where the head already
                 * is: wrap it back into range rather than leaving it out
                 * beyond the end of the cycle. */
                if (lp->pos >= lp->period) lp->pos = fmod(lp->pos, lp->period);

                /* Walk the offset toward its target at the rate limit, once
                 * per FRAME. The playhead therefore never jumps: it runs
                 * fast or slow until it has arrived. */
                {
                    const double d = (double)lp->phase_off - lp->phase_off_cur;
                    if (d > lp->phase_step)       lp->phase_off_cur += lp->phase_step;
                    else if (d < -lp->phase_step) lp->phase_off_cur -= lp->phase_step;
                    else                          lp->phase_off_cur = (double)lp->phase_off;
                }

                /* PHASE offsets by a fraction of a CYCLE, so the nudge means
                 * the same musical amount whatever the tempo. */
                double posf = lp->pos + lp->phase_off_cur * lp->period;
                posf -= lp->period * floor(posf / lp->period);

                double idx = 0.0;
                int inside = 1;

                switch (lp->fit) {
                case FIT_PAD:
                case FIT_PAD_F:
                    /* Padding and truncation are the SAME arithmetic: run off
                     * the end of the window and emit silence until the wrap;
                     * or wrap before reaching the end and never hear the
                     * tail. */
                    idx = posf;
                    inside = (idx < lp->span);
                    break;
                case FIT_SPD:
                case FIT_SPD_F:
                    /* Varispeed: the window is stretched over the whole
                     * cycle, so it exactly fills it and the pitch moves. */
                    idx = (posf / lp->period) * lp->span;
                    break;
                default:
                    idx = fmod(posf, lp->span);
                    break;
                }

                if (inside) {
                    rp = lp->reversed ? (lp->w_hi - idx) : (lp->w_lo + idx);
                    read_frame(lp, rp, &sl, &sr);
                    const float g = window_gain(lp, idx);
                    sl *= g;
                    sr *= g;
                    voiced = play_voiced = 1;
                }

                /* READ AT THE CURRENT POSITION, ADVANCE AFTERWARDS.
                 *
                 * Advancing first costs the window its first frame: the
                 * cycle would begin at index 1, and frame 0 — exactly where
                 * a transient sits once START is placed on one — would never
                 * be read at all. */
                lp->pos += 1.0;
                if (lp->pos >= lp->period) lp->pos -= lp->period;
            }

            /* TRIG is a one-shot audition and deliberately ignores both the
             * period and the transport: it exists to tell you what you
             * caught, which you need to know while stopped. */
            if (lp->audition) {
                if (lp->audition_pos >= lp->span) {
                    lp->audition = 0;
                } else {
                    const double a = lp->audition_pos;
                    const double arp = lp->reversed ? (lp->w_hi - a) : (lp->w_lo + a);
                    float al, ar;
                    read_frame(lp, arp, &al, &ar);
                    const float g = window_gain(lp, a);
                    sl += al * g;
                    sr += ar * g;
                    lp->audition_pos += 1.0;
                    voiced = 1;
                }
            }

            /* Overdub writes at the position being READ, so a new pass lands
             * on top of the old one rather than beside it. */
            if (lp->overdubbing && play_voiced) {
                const int cur = (int)(rp + 0.5);
                if (cur >= 0 && cur < lp->recorded_length) {
                    if (lp->od_last_idx >= 0 && lp->od_last_idx != cur) {
                        const int d = cur - lp->od_last_idx;
                        const int ad = d < 0 ? -d : d;
                        if (ad > 1 && ad <= OVERDUB_MAX_FILL) {
                            const int step = d > 0 ? 1 : -1;
                            for (int k = lp->od_last_idx + step; k != cur; k += step)
                                overdub_write(lp, k, raw_l, raw_r);
                        }
                    }
                    overdub_write(lp, cur, raw_l, raw_r);
                    lp->od_last_idx = cur;
                }
            } else {
                lp->od_last_idx = -1;
            }

            if (voiced) {
                acc_l += sl * lp->vol_cur * lp->gl;
                acc_r += sr * lp->vol_cur * lp->gr;
            }
        }

        /* The fade applies to the loops only. DRY is the live input, and
         * ducking someone's playing for fifteen seconds to announce that
         * their loops are going away would be the wrong thing to take. */
        if (s->clearing) {
            acc_l *= s->clear_gain;
            acc_r *= s->clear_gain;
            s->clear_gain -= s->clear_step;
            if (s->clear_gain <= 0.0f) wipe_all(s);
        }

        /* NOTHING TO ADD, NOTHING TO TOUCH.
         *
         * With no loop sounding and DRY at full, the block is left exactly
         * as it arrived — not merely multiplied by one and rounded back,
         * which would cost a bit of the signal every time the module sat
         * idle in a chain. */
        if (acc_l == 0.0f && acc_r == 0.0f && s->dry_cur >= 1.0f) continue;

        const float dl = (float)raw_l * (1.0f / 32768.0f) * s->dry_cur;
        const float dr = (float)raw_r * (1.0f / 32768.0f) * s->dry_cur;

        lr[2 * n]     = f_to_i16(softclip(dl + acc_l * s->out_cur));
        lr[2 * n + 1] = f_to_i16(softclip(dr + acc_r * s->out_cur));
    }

    s->total_frames += (uint64_t)frames;
    if (s->playing) s->align_frames += (uint64_t)frames;
}

/* ------------------------------------------------------------------ */
/* set_param                                                           */
/* ------------------------------------------------------------------ */

/* Returns the loop index and the suffix after "loopN_", or NULL. */
static const char *loop_key_suffix(const char *key, int *index_out) {
    for (int i = 0; i < NUM_LOOPS; i++) {
        size_t plen = strlen(LOOP_PREFIXES[i]);
        if (strncmp(key, LOOP_PREFIXES[i], plen) == 0) {
            *index_out = i;
            return key + plen;
        }
    }
    return NULL;
}

static void loop_set_param(inst_t *s, loop_t *loop,
                           const char *suffix, const char *val) {
    (void)s;
    if (strcmp(suffix, "record") == 0) {
        if (trigger_fired(val)) loop_record_press(loop);
    } else if (strcmp(suffix, "trig") == 0) {
        if (trigger_fired(val) && loop->state == LOOP_LOADED) {
            loop->audition     = 1;
            loop->audition_pos = 0.0;
        }
    } else if (strcmp(suffix, "start") == 0) {
        loop->start_frac = clampf((float)atof(val), 0.0f, 1.0f);
    } else if (strcmp(suffix, "end") == 0) {
        /* Bipolar: the sign is the playback direction. */
        loop->end_frac = clampf((float)atof(val), -1.0f, 1.0f);
    } else if (strcmp(suffix, "bpm") == 0) {
        loop->bpm = clampf(roundf((float)atof(val)), MIN_BPM, MAX_BPM);
    } else if (strcmp(suffix, "beats") == 0) {
        loop->beats = clampf(roundf((float)atof(val)), MIN_BEATS, MAX_BEATS);
    } else if (strcmp(suffix, "fit") == 0) {
        loop->fit = (fit_mode_t)enum_index_from(val, FIT_LABELS, FIT_COUNT,
                                                (int)loop->fit);
    } else if (strcmp(suffix, "phase") == 0) {
        loop->phase_off = clampf((float)atof(val), -0.5f, 0.5f);
    } else if (strcmp(suffix, "volume") == 0) {
        /* Faders reach 200%: six quiet takes summed can need lifting, and a
         * loop pushed past unity into the soft clipper is a usable sound. */
        loop->volume = clampf((float)atof(val), 0.0f, 2.0f);
    }
    /* Anything else: silently ignored. A write to a key we do not know is
     * not an error worth failing a block over. */
}

static void v2_set_param(void *instance, const char *key, const char *val) {
    inst_t *s = (inst_t *)instance;
    if (!s || !key || !val) return;

    /* Deliberately not implemented, exactly as in Forgetful: the state blob
     * would have to carry 31 MB of audio. Forfeits User Presets and patch
     * capture for this module. */
    if (strcmp(key, "state") == 0) return;

    int idx = 0;
    const char *suffix = loop_key_suffix(key, &idx);
    if (suffix) {
        loop_set_param(s, &s->loops[idx], suffix, val);
        return;
    }

    if (strcmp(key, "master_play") == 0) {
        if (trigger_fired(val)) {
            s->playing = !s->playing;
            /* Starting zeroes every phase at the same sample — that shared
             * moment is what the whole piece drifts away from. */
            if (s->playing) zero_all_phases(s);
        }
        return;
    }
    if (strcmp(key, "master_clear") == 0) {
        /* Press to begin, press again to call it off. An abort has to exist:
         * this control erases everything and sits between two that are
         * pressed constantly. */
        if (trigger_fired(val)) {
            if (s->clearing) {
                s->clearing   = 0;
                s->clear_gain = 1.0f;
            } else {
                s->clearing   = 1;
                s->clear_gain = 1.0f;
            }
        }
        return;
    }
    if (strcmp(key, "master_resync") == 0) {
        /* Same alignment, without stopping. */
        if (trigger_fired(val)) zero_all_phases(s);
        return;
    }
    if (strcmp(key, "master_base") == 0) {
        s->base_bpm = clampf(roundf((float)atof(val)), MIN_BPM, MAX_BPM);
        apply_spread(s);
        return;
    }
    if (strcmp(key, "master_spread") == 0) {
        s->spread = clampf(roundf((float)atof(val)), MIN_SPREAD, MAX_SPREAD);
        apply_spread(s);
        return;
    }
    if (strcmp(key, "master_dry") == 0) {
        s->dry = clampf((float)atof(val), 0.0f, 1.0f);
        return;
    }
    if (strcmp(key, "master_out") == 0) {
        s->out = clampf((float)atof(val), 0.0f, 1.0f);
        return;
    }
    if (strcmp(key, "loop_select") == 0) {
        int v = atoi(val);
        if (v < 1) v = 1;
        if (v > NUM_LOOPS) v = NUM_LOOPS;
        s->loop_select = v;
        return;
    }
    if (strcmp(key, "master_widen") == 0) {
        s->widen = clampf((float)atof(val), 0.0f, 1.0f);
        return;
    }

    return;
}

/* ------------------------------------------------------------------ */
/* get_param                                                           */
/* ------------------------------------------------------------------ */

/* "Served, and there is nothing there." NOT -1, which means "the read did
 * not complete" and is retried forever. */
static int param_absent(char *buf, int len) {
    if (len > 0) buf[0] = '\0';
    return 0;
}

static int loop_get_param(const inst_t *s, const loop_t *loop,
                          const char *suffix, char *buf, int len) {
    if (strcmp(suffix, "record") == 0) return loop_record_text(loop, buf, len);
    if (strcmp(suffix, "trig") == 0)   return snprintf(buf, len, "PLAY");
    if (strcmp(suffix, "start") == 0)  return snprintf(buf, len, "%.3f", (double)loop->start_frac);
    if (strcmp(suffix, "end") == 0)    return snprintf(buf, len, "%.3f", (double)loop->end_frac);
    if (strcmp(suffix, "bpm") == 0)    return snprintf(buf, len, "%.0f", (double)loop->bpm);
    if (strcmp(suffix, "beats") == 0)  return snprintf(buf, len, "%.0f", (double)loop->beats);
    if (strcmp(suffix, "fit") == 0)    return snprintf(buf, len, "%s", FIT_LABELS[loop->fit]);
    if (strcmp(suffix, "phase") == 0)  return snprintf(buf, len, "%.3f", (double)loop->phase_off);
    if (strcmp(suffix, "volume") == 0) return snprintf(buf, len, "%.3f", (double)loop->volume);
    if (strcmp(suffix, "cycle") == 0)  return loop_cycle_text(s, loop, buf, len);
    return param_absent(buf, len);
}

/* ------------------------------------------------------------------ */
/* The two contracts: chain_params and ui_hierarchy                    */
/* ------------------------------------------------------------------ */

/* A flat array of parameter descriptors, in declaration order. 63 entries.
 *
 * Built into a fixed stack buffer, and snprintf truncates SILENTLY — a
 * module that overflows this simply stops having a UI, with no error
 * anywhere. tests/test_wayward.c asserts both the entry count and that the
 * result still has real headroom, so the failure arrives while there is
 * room to fix it.
 *
 * Note every float that carries "unit":"%" is declared 0..1: the host
 * multiplies by 100 for display. */
static int build_chain_params(char *buf, int len) {
    char json[16384];
    int pos = 0;

    pos += snprintf(json + pos, sizeof(json) - pos,
        "["
        "{\"key\":\"master_play\",\"name\":\"PLAY\",\"type\":\"enum\","
          "\"options\":%s,\"access\":\"write\"}"
        ",{\"key\":\"master_base\",\"name\":\"BASE\",\"type\":\"float\","
          "\"min\":%.0f,\"max\":%.0f,\"default\":%.0f,\"step\":1,"
          "\"display_format\":\"%%.0f\"}"
        ",{\"key\":\"master_spread\",\"name\":\"SPRD\",\"type\":\"float\","
          "\"min\":%.0f,\"max\":%.0f,\"default\":%.0f,\"step\":1,"
          "\"display_format\":\"%%.0f\"}"
        /* A read-only ENUM, not a read-only string: a string renders through
         * drawOpaqueBox at about two characters wide, where the enum-square
         * renderer gives a proper bordered cell. */
        ",{\"key\":\"master_align\",\"name\":\"ALIGN\",\"type\":\"enum\","
          "\"options\":[\"-\"],\"access\":\"read\"}"
        ",{\"key\":\"master_resync\",\"name\":\"RSYN\",\"type\":\"enum\","
          "\"options\":%s,\"access\":\"write\"}"
        ",{\"key\":\"master_clear\",\"name\":\"CLEAR\",\"type\":\"enum\","
          "\"options\":%s,\"access\":\"write\"}"
        ",{\"key\":\"master_dry\",\"name\":\"DRY\",\"type\":\"float\","
          "\"min\":0,\"max\":1,\"default\":1,\"step\":0.01,\"unit\":\"%%\","
          "\"display_format\":\"%%.0f\"}"
        ",{\"key\":\"master_out\",\"name\":\"OUT\",\"type\":\"float\","
          "\"min\":0,\"max\":1,\"default\":0.8,\"step\":0.01,\"unit\":\"%%\","
          "\"display_format\":\"%%.0f\"}"
        ",{\"key\":\"master_widen\",\"name\":\"WIDEN\",\"type\":\"float\","
          "\"min\":0,\"max\":1,\"default\":0.5,\"step\":0.01,\"unit\":\"%%\","
          "\"display_format\":\"%%.0f\"}",
        TRIGGER_OPTIONS_JSON,
        (double)MIN_BPM, (double)MAX_BPM, (double)DEFAULT_BPM,
        (double)MIN_SPREAD, (double)MAX_SPREAD, (double)DEFAULT_SPREAD,
        TRIGGER_OPTIONS_JSON, TRIGGER_OPTIONS_JSON);

    /* The six mixer faders. The host's detectFader keys off the _volume
     * suffix — rename these and they become dials. */
    for (int i = 0; i < NUM_LOOPS; i++) {
        pos += snprintf(json + pos, sizeof(json) - pos,
            /* Reaching 200%, with the default left where it was at 80% so
             * that unity still sits comfortably inside the travel. */
            ",{\"key\":\"loop%d_volume\",\"name\":\"%d\",\"type\":\"float\","
              "\"min\":0,\"max\":2,\"default\":0.8,\"step\":0.01,"
              "\"unit\":\"%%\",\"display_format\":\"%%.0f\"}",
            i + 1, i + 1);
    }

    /* The Loop page's instance selector. Declared as a float rather than an
     * enum of "1".."6": the host reads it with a numeric parse, and numeric
     * enum labels are ambiguous on this wire anyway. Named LOOP, not SELECT,
     * because LABEL_CHARS caps a label at five. */
    pos += snprintf(json + pos, sizeof(json) - pos,
        ",{\"key\":\"loop_select\",\"name\":\"LOOP\",\"type\":\"float\","
          "\"min\":1,\"max\":%d,\"default\":1,\"step\":1,"
          "\"display_format\":\"%%.0f\"}", NUM_LOOPS);

    /* One cycle readout per loop, for the Orbits page. */
    for (int i = 0; i < NUM_LOOPS; i++) {
        pos += snprintf(json + pos, sizeof(json) - pos,
            ",{\"key\":\"loop%d_cycle\",\"name\":\"%d\",\"type\":\"enum\","
              "\"options\":[\"-\"],\"access\":\"read\"}",
            i + 1, i + 1);
    }

    for (int i = 0; i < NUM_LOOPS; i++) {
        int n = i + 1;
        pos += snprintf(json + pos, sizeof(json) - pos,
            ",{\"key\":\"loop%d_record\",\"name\":\"REC %d\",\"type\":\"enum\","
              "\"options\":%s,\"access\":\"write\"}"
            ",{\"key\":\"loop%d_trig\",\"name\":\"TRIG\",\"type\":\"enum\","
              "\"options\":%s,\"access\":\"write\"}"
            ",{\"key\":\"loop%d_start\",\"name\":\"START\",\"type\":\"float\","
              "\"min\":0,\"max\":1,\"default\":0,\"step\":0.001,"
              "\"unit\":\"%%\",\"display_format\":\"%%.0f\"}"
            /* Bipolar, so it reads -100%..+100% with silence at the centre
             * detent and the sign naming the direction of travel. */
            ",{\"key\":\"loop%d_end\",\"name\":\"END\",\"type\":\"float\","
              "\"min\":-1,\"max\":1,\"default\":1,\"step\":0.001,"
              "\"unit\":\"%%\",\"display_format\":\"%%.0f\"}"
            ",{\"key\":\"loop%d_bpm\",\"name\":\"BPM\",\"type\":\"float\","
              "\"min\":%.0f,\"max\":%.0f,\"default\":%.0f,\"step\":1,"
              "\"display_format\":\"%%.0f\"}"
            /* BEAT is a float, not the enum the design first proposed: an
             * enum with bare-number labels is ambiguous on a wire where the
             * host may send either the label or the index. Free integers
             * are also more musical — 7 beats against 8 phases beautifully,
             * and a restricted set would have forbidden it. */
            ",{\"key\":\"loop%d_beats\",\"name\":\"BEAT\",\"type\":\"float\","
              "\"min\":%.0f,\"max\":%.0f,\"default\":%.0f,\"step\":1,"
              "\"display_format\":\"%%.0f\"}"
            ",{\"key\":\"loop%d_fit\",\"name\":\"FIT\",\"type\":\"enum\","
              "\"options\":[\"PAD\",\"PADF\",\"SPD\",\"SPDF\",\"RPT\",\"RPTF\"],"
              "\"default\":0}"
            ",{\"key\":\"loop%d_phase\",\"name\":\"PHAS\",\"type\":\"float\","
              "\"min\":-0.5,\"max\":0.5,\"default\":0,\"step\":0.005,"
              "\"unit\":\"%%\",\"display_format\":\"%%.0f\"}",
            n, n, TRIGGER_OPTIONS_JSON,
            n, TRIGGER_OPTIONS_JSON,
            n, n,
            n, (double)MIN_BPM, (double)MAX_BPM, (double)DEFAULT_BPM,
            n, (double)MIN_BEATS, (double)MAX_BEATS, (double)DEFAULT_BEATS,
            n, n);
    }

    pos += snprintf(json + pos, sizeof(json) - pos, "]");
    return snprintf(buf, len, "%s", json);
}

/* The page plan. One level per page; the order of the {"level":..} nav
 * entries in root's params[] is the bank order.
 *
 * A knobs[] array longer than 8 does NOT make a second page — the planner
 * auto-splits it into "Main-2".."Main-N" continuation pages with no
 * separators, which is why every page here is its own named level. The root
 * level is always titled "Main" by the planner whatever label it declares. */
static int build_ui_hierarchy(char *buf, int len) {
    char json[8192];
    int pos = 0;

    pos += snprintf(json + pos, sizeof(json) - pos,
        "{\"modes\":null,\"levels\":{"
        "\"root\":{\"label\":\"Wayward\","
          /* MAIN IS THE SIX RECORD BUTTONS AND THE TRANSPORT.
           *
           * The six live in a 3x2 block on the left, in the same shape they
           * occupy on Mix and on Orbits, so the hand learns one arrangement
           * of the ensemble and reuses it on every page that addresses all
           * six at once. PLAY and RSYN take the right-hand column, together
           * and away from the six, because they act on all of them. */
          "\"knobs\":[\"loop1_record\",\"loop2_record\",\"loop3_record\",\"master_play\","
                     "\"loop4_record\",\"loop5_record\",\"loop6_record\",\"master_resync\"],"
          "\"params\":["
            "{\"key\":\"loop1_record\",\"label\":\"Rec 1\"},"
            "{\"key\":\"loop2_record\",\"label\":\"Rec 2\"},"
            "{\"key\":\"loop3_record\",\"label\":\"Rec 3\"},"
            "{\"key\":\"loop4_record\",\"label\":\"Rec 4\"},"
            "{\"key\":\"loop5_record\",\"label\":\"Rec 5\"},"
            "{\"key\":\"loop6_record\",\"label\":\"Rec 6\"},"
            "{\"key\":\"master_play\",\"label\":\"Play\"},"
            "{\"key\":\"master_resync\",\"label\":\"Resync\"},"
            "{\"level\":\"orbits\",\"label\":\"Orbits\"},"
            "{\"level\":\"shape\",\"label\":\"Shape\"},"
            "{\"level\":\"mix\",\"label\":\"Mix\"},"
            "{\"level\":\"loops\",\"label\":\"Loop\"}]}"
        /* ORBITS: where each loop is in its own cycle, and when they next
         * all meet. The six sit in the same 3x2 block as everywhere else,
         * and ALIGN takes the cell at the end of the top row — the one place
         * on the page that speaks for the ensemble rather than for a loop. */
        ",\"orbits\":{\"label\":\"Orbits\",\"knobs\":["
          "\"loop1_cycle\",\"loop2_cycle\",\"loop3_cycle\",\"master_align\","
          "\"\",\"loop4_cycle\",\"loop5_cycle\",\"loop6_cycle\"]}"
        /* SHAPE: how the ensemble is tuned and what leaves it. Set once and
         * left, which is why none of it is on Main any more. CLEAR sits in
         * the far corner, the furthest cell on the page from anything
         * reached in a hurry. */
        ",\"shape\":{\"label\":\"Shape\",\"knobs\":["
          "\"master_base\",\"master_spread\",\"master_widen\",\"\","
          "\"master_dry\",\"master_out\",\"\",\"master_clear\"]}"
        ",\"mix\":{\"label\":\"Mix\",\"knobs\":["
          "\"loop1_volume\",\"loop2_volume\",\"loop3_volume\",\"\","
          "\"loop4_volume\",\"loop5_volume\",\"loop6_volume\",\"\"]}");

    /* ONE LEVEL FOR ALL SIX LOOPS — a child level, which is host machinery
     * rather than anything this module has to fake.
     *
     * The level declares the shape once and the host multiplies it: the
     * generic keys below resolve through child_key_template to loop3_start
     * and so on, so all 42 concrete params stay declared exactly as they
     * were and remain addressable by LFOs and modulation. Six near-identical
     * pages become one, and the bank bar loses six of its ten segments.
     *
     * The host owns the hard part. On a change of instance it drops the
     * cached values, the knob state and any PENDING WRITE for the page's
     * cells (page_controller.mjs, dropChildLevelCache) — the pending write
     * being the dangerous one, since it would otherwise land on the loop you
     * just moved to. It also re-points each generic key at the concrete
     * declaration, without which the metadata falls back to a guess and a
     * specialised widget degrades into a bare 0..1 knob.
     *
     * TWO DETAILS PUT THE SELECTOR IN CELL ONE, rather than on the separate
     * picker page the host would otherwise generate:
     *
     *   - Listing child_index_param anywhere in the hierarchy suppresses that
     *     generated page ("no picker at all when the module offers a real
     *     cell for it" — page_plan.mjs, childPickerNeeded).
     *
     *   - child_key_overrides maps loop_select to ITSELF. Every key on a
     *     child page is otherwise run through the template, which would turn
     *     loop_select into loop1_loop_select; an override containing neither
     *     {index} nor {key} resolves literally and escapes that.
     *
     * The host then polls loop_select every tick and follows it
     * (syncChildIndexFromModule), so the selection is the module's to own. */
    pos += snprintf(json + pos, sizeof(json) - pos,
        ",\"loops\":{\"label\":\"Loop\","
          "\"child_count\":%d,\"child_label\":\"Loop\","
          "\"child_key_template\":\"loop{index}_{key}\","
          "\"child_index_base\":1,"
          "\"child_index_param\":\"loop_select\","
          "\"child_key_overrides\":{\"loop_select\":\"loop_select\"},"
          /* Which loop, then the take, then the time it keeps. */
          "\"knobs\":[\"loop_select\",\"trig\",\"start\",\"end\","
                     "\"bpm\",\"beats\",\"fit\",\"phase\"]}",
        NUM_LOOPS);

    pos += snprintf(json + pos, sizeof(json) - pos, "}}");
    return snprintf(buf, len, "%s", json);
}

static int v2_get_param(void *instance, const char *key, char *buf, int len) {
    inst_t *s = (inst_t *)instance;
    if (!s || !key || !buf) return -1;

    if (strcmp(key, "name") == 0) return snprintf(buf, len, "Wayward");

    /* See the matching note in set_param. "{}" and not -1: an empty object
     * is a complete answer, -1 would be retried forever. */
    if (strcmp(key, "state") == 0) return snprintf(buf, len, "{}");

    int idx = 0;
    const char *suffix = loop_key_suffix(key, &idx);
    if (suffix) return loop_get_param(s, &s->loops[idx], suffix, buf, len);

    if (strcmp(key, "master_play") == 0)
        return snprintf(buf, len, s->playing ? "STOP" : "PLAY");
    if (strcmp(key, "master_resync") == 0) return snprintf(buf, len, "SYNC");
    /* Names what the next press will do, as the other transport buttons do:
     * once a clear is running, pressing again keeps everything. */
    if (strcmp(key, "master_clear") == 0)
        return snprintf(buf, len, s->clearing ? "KEEP" : "CLR");
    if (strcmp(key, "master_base") == 0)   return snprintf(buf, len, "%.0f", (double)s->base_bpm);
    if (strcmp(key, "master_spread") == 0) return snprintf(buf, len, "%.0f", (double)s->spread);
    if (strcmp(key, "master_dry") == 0)    return snprintf(buf, len, "%.3f", (double)s->dry);
    if (strcmp(key, "master_out") == 0)    return snprintf(buf, len, "%.3f", (double)s->out);
    if (strcmp(key, "master_widen") == 0)  return snprintf(buf, len, "%.3f", (double)s->widen);
    if (strcmp(key, "master_align") == 0)  return master_align_text(s, buf, len);
    /* A plain number, never an enum: the host parses this numerically and
     * treats anything else as "do not move the focus". */
    if (strcmp(key, "loop_select") == 0)   return snprintf(buf, len, "%d", s->loop_select);

    if (strcmp(key, "chain_params") == 0)  return build_chain_params(buf, len);
    if (strcmp(key, "ui_hierarchy") == 0)  return build_ui_hierarchy(buf, len);

    return param_absent(buf, len);
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

audio_fx_api_v2_t *move_audio_fx_init_v2(const host_api_v1_t *host) {
    /* Kept, but nothing reads it: this module is entirely self-contained.
     * It followed the host transport and tempo for a while; that was taken
     * out deliberately, not lost. */
    g_host = host;
    (void)g_host;

    memset(&g_api, 0, sizeof(g_api));
    g_api.api_version      = AUDIO_FX_API_VERSION_2;
    g_api.create_instance  = v2_create_instance;
    g_api.destroy_instance = v2_destroy_instance;
    g_api.process_block    = v2_process_block;
    g_api.set_param        = v2_set_param;
    g_api.get_param        = v2_get_param;
    /* on_midi stays NULL — this module has no use for it. */
    return &g_api;
}
