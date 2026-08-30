/* Wayward — six loops, each keeping its own time.
 *
 * A Schwung audio_fx module for Ableton Move. Six independent recordings,
 * each looped at its own tempo. Set them to 100, 101, 102, 103, 104 and 105
 * BPM and they slide out of alignment and back over minutes — the Reich /
 * Eno / Basinski tape-loop mechanism, on eight knobs.
 *
 * THIS FILE IS CURRENTLY THE SKELETON (step 1 of the build order in
 * DESIGN.md). It declares the complete control surface — every parameter and
 * all eight pages — and holds the parameter state and the record/play state
 * machine, so the whole UI can be walked on the device. process_block is
 * still a pure passthrough: no recording, no loop playback. The buffers are
 * nevertheless allocated at full size, so that the 31.7 MB footprint is
 * proven on real hardware before any DSP depends on it.
 *
 * PAGES (one ui_hierarchy level each; the host renders 8 knobs per page,
 * 4 across x 2 rows):
 *
 *   Main    PLAY  SYNC  BASE  SPRD  /  STATE RSYN  WIDEN ....
 *   Mix     1     2     3     4     /  5     6     DRY   OUT
 *   Loop 1  REC   TRIG  START END   /  BPM   BEAT  FIT   PHAS
 *   ...     (Loop 2..6 identical)
 *
 * Eight sections is one more than Forgetful ships, and the page planner has
 * not been proven at that count — the first thing to confirm on device is
 * that all eight appear in the bank bar, before any DSP is written.
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

/* A window shorter than this is not a loop, it is a click. */
#define MIN_WINDOW_FRAMES ((int)(SAMPLE_RATE * 0.05f))

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

static const char *const SYNC_LABELS[] = { "FREE", "MOVE" };
#define SYNC_COUNT 2

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
    int        recorded_length;   /* frames in the finished take */

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

    double phase;      /* 0..1, advanced by 1/period_frames each sample */

    int    audition;      /* one-shot TRIG in flight */
    double audition_pos;  /* frames into the window */
} loop_t;

typedef struct {
    loop_t loops[NUM_LOOPS];

    int   playing;
    int   sync_mode;    /* index into SYNC_LABELS */
    float base_bpm;
    float spread;
    float dry;
    float out;
    float widen;

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
    for (int i = 0; i < NUM_LOOPS; i++) s->loops[i].phase = 0.0;
}

static void init_loop(loop_t *loop) {
    loop->buffer          = NULL;
    loop->capacity_frames = 0;
    loop->write_head      = 0;
    loop->recorded_length = 0;
    loop->state           = LOOP_EMPTY;
    loop->overdubbing     = 0;
    loop->start_frac      = 0.0f;
    loop->end_frac        = 1.0f;
    loop->bpm             = DEFAULT_BPM;
    loop->beats           = DEFAULT_BEATS;
    loop->fit             = FIT_PAD;
    loop->phase_off       = 0.0f;
    loop->volume          = 0.8f;
    loop->phase           = 0.0;
    loop->audition        = 0;
    loop->audition_pos    = 0.0;
}

/* ------------------------------------------------------------------ */
/* Readouts                                                            */
/* ------------------------------------------------------------------ */

/* One character per loop, in ensemble order:
 *     .  nothing recorded
 *     S  has a take, ensemble stopped
 *     P  playing
 *     O  overdubbing onto the take while it plays
 *     R  recording right now
 *
 * THE GLYPHS ARE CONSTRAINED BY THE RENDERER, not chosen for looks. Every
 * value goes through enumSquareLines() (schwung's
 * shared/param_pages/font5x3.mjs), which:
 *
 *   1. treats "-", "_" and " " as WORD SEPARATORS — "-" whenever it falls
 *      between two alphanumerics — and then keeps only the first three
 *      characters of each of the first two words. This is why "-" cannot be
 *      the empty glyph, however much it wants to be: an ensemble reading
 *      "R-S---" has a hyphen with R on one side and S on the other, so it is
 *      rewritten to "R S---", split, and drawn as "R" over "S--". That is not
 *      a truncated ensemble, it is a WRONG one — characters vanish and every
 *      position after the break shifts, so the readout stops saying which
 *      loop is which. An all-empty "------" survives by accident, no hyphen
 *      there having an alphanumeric on either side, which is exactly how the
 *      fault would look correct until the first take was recorded. "." is the
 *      nearest glyph that means absence and is never a separator.
 *   2. uppercases the value, so a glyph cannot differ from its uppercase twin.
 *   3. sends an all-digit value down a different path entirely.
 *
 * Six characters, no separator, which is also what gets the requested
 * two-rows-of-three for free: if the six do not fit the interior on one line,
 * enumSquareLines falls back to a blind 3+3 slice and the renderer centres
 * both rows — loops 1-3 above loops 4-6, every position intact. */
static char loop_status_char(const inst_t *s, const loop_t *loop) {
    if (loop->state == LOOP_RECORDING) return 'R';
    if (loop->state == LOOP_EMPTY)     return '.';
    if (!s->playing)                   return 'S';
    return loop->overdubbing ? 'O' : 'P';
}

static int master_state_text(const inst_t *s, char *buf, int len) {
    char code[NUM_LOOPS];
    for (int i = 0; i < NUM_LOOPS; i++) {
        code[i] = loop_status_char(s, &s->loops[i]);
    }
    return snprintf(buf, len, "%c%c%c%c%c%c",
                    code[0], code[1], code[2], code[3], code[4], code[5]);
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
    if (loop->write_head < MIN_WINDOW_FRAMES) {
        /* Too short to be anything. Throw it away rather than keep a click. */
        loop->write_head      = 0;
        loop->recorded_length = 0;
        loop->overdubbing     = 0;
        loop->state           = LOOP_EMPTY;
        return;
    }
    loop->recorded_length = loop->write_head;
    loop->start_frac      = 0.0f;
    loop->end_frac        = 1.0f;   /* the whole take, forwards */
    loop->overdubbing     = 0;
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
        loop->recorded_length = 0;
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
    s->sync_mode = 0;               /* FREE */
    s->base_bpm  = DEFAULT_BPM;
    s->spread    = DEFAULT_SPREAD;  /* 100..105 out of the box */
    s->dry       = 1.0f;            /* an fx that silences its input is a bug */
    s->out       = 0.8f;
    s->widen     = 0.5f;

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

/* SKELETON: bit-exact passthrough. The loop engine lands in step 2 of the
 * build order; until then this exists so the module can sit in a chain
 * without altering the signal while the control surface is walked. */
static void v2_process_block(void *instance, int16_t *lr, int frames) {
    inst_t *s = (inst_t *)instance;
    if (!s || !lr || frames <= 0) return;
    s->total_frames += (uint64_t)frames;
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
        loop->volume = clampf((float)atof(val), 0.0f, 1.0f);
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
    if (strcmp(key, "master_resync") == 0) {
        /* Same alignment, without stopping. */
        if (trigger_fired(val)) zero_all_phases(s);
        return;
    }
    if (strcmp(key, "master_sync") == 0) {
        s->sync_mode = enum_index_from(val, SYNC_LABELS, SYNC_COUNT,
                                       s->sync_mode);
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
    (void)s;
    if (strcmp(suffix, "record") == 0) return loop_record_text(loop, buf, len);
    if (strcmp(suffix, "trig") == 0)   return snprintf(buf, len, "PLAY");
    if (strcmp(suffix, "start") == 0)  return snprintf(buf, len, "%.3f", (double)loop->start_frac);
    if (strcmp(suffix, "end") == 0)    return snprintf(buf, len, "%.3f", (double)loop->end_frac);
    if (strcmp(suffix, "bpm") == 0)    return snprintf(buf, len, "%.0f", (double)loop->bpm);
    if (strcmp(suffix, "beats") == 0)  return snprintf(buf, len, "%.0f", (double)loop->beats);
    if (strcmp(suffix, "fit") == 0)    return snprintf(buf, len, "%s", FIT_LABELS[loop->fit]);
    if (strcmp(suffix, "phase") == 0)  return snprintf(buf, len, "%.3f", (double)loop->phase_off);
    if (strcmp(suffix, "volume") == 0) return snprintf(buf, len, "%.3f", (double)loop->volume);
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
        ",{\"key\":\"master_sync\",\"name\":\"SYNC\",\"type\":\"enum\","
          "\"options\":[\"FREE\",\"MOVE\"],\"default\":0}"
        ",{\"key\":\"master_base\",\"name\":\"BASE\",\"type\":\"float\","
          "\"min\":%.0f,\"max\":%.0f,\"default\":%.0f,\"step\":1,"
          "\"display_format\":\"%%.0f\"}"
        ",{\"key\":\"master_spread\",\"name\":\"SPRD\",\"type\":\"float\","
          "\"min\":%.0f,\"max\":%.0f,\"default\":%.0f,\"step\":1,"
          "\"display_format\":\"%%.0f\"}"
        /* A read-only ENUM, not a read-only string: a string renders through
         * drawOpaqueBox at about two characters wide, where the enum-square
         * renderer gives a proper bordered cell. */
        ",{\"key\":\"master_state\",\"name\":\"STATE\",\"type\":\"enum\","
          "\"options\":[\"-\"],\"access\":\"read\"}"
        ",{\"key\":\"master_resync\",\"name\":\"RSYN\",\"type\":\"enum\","
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
        TRIGGER_OPTIONS_JSON);

    /* The six mixer faders. The host's detectFader keys off the _volume
     * suffix — rename these and they become dials. */
    for (int i = 0; i < NUM_LOOPS; i++) {
        pos += snprintf(json + pos, sizeof(json) - pos,
            ",{\"key\":\"loop%d_volume\",\"name\":\"%d\",\"type\":\"float\","
              "\"min\":0,\"max\":1,\"default\":0.8,\"step\":0.01,"
              "\"unit\":\"%%\",\"display_format\":\"%%.0f\"}",
            i + 1, i + 1);
    }

    for (int i = 0; i < NUM_LOOPS; i++) {
        int n = i + 1;
        pos += snprintf(json + pos, sizeof(json) - pos,
            ",{\"key\":\"loop%d_record\",\"name\":\"REC\",\"type\":\"enum\","
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
            n, TRIGGER_OPTIONS_JSON,
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
          /* Transport and the ensemble's shape on top; what you hear and the
           * one blank cell underneath. SPRD sits beside BASE because the two
           * are read together: BASE is where the piece is, SPRD is how fast
           * it comes apart. */
          "\"knobs\":[\"master_play\",\"master_sync\",\"master_base\",\"master_spread\","
                     "\"master_state\",\"master_resync\",\"master_widen\",\"\"],"
          "\"params\":["
            "{\"key\":\"master_play\",\"label\":\"Play\"},"
            "{\"key\":\"master_sync\",\"label\":\"Sync\"},"
            "{\"key\":\"master_base\",\"label\":\"Base\"},"
            "{\"key\":\"master_spread\",\"label\":\"Spread\"},"
            "{\"key\":\"master_state\",\"label\":\"State\"},"
            "{\"key\":\"master_resync\",\"label\":\"Resync\"},"
            "{\"key\":\"master_widen\",\"label\":\"Widen\"},"
            "{\"level\":\"mix\",\"label\":\"Mix\"},"
            "{\"level\":\"loop1\",\"label\":\"Loop 1\"},"
            "{\"level\":\"loop2\",\"label\":\"Loop 2\"},"
            "{\"level\":\"loop3\",\"label\":\"Loop 3\"},"
            "{\"level\":\"loop4\",\"label\":\"Loop 4\"},"
            "{\"level\":\"loop5\",\"label\":\"Loop 5\"},"
            "{\"level\":\"loop6\",\"label\":\"Loop 6\"}]}"
        /* Mix sits between Main and the loops: the six faders in ensemble
         * order, then what leaves the module. */
        ",\"mix\":{\"label\":\"Mix\",\"knobs\":["
          "\"loop1_volume\",\"loop2_volume\",\"loop3_volume\",\"loop4_volume\","
          "\"loop5_volume\",\"loop6_volume\",\"master_dry\",\"master_out\"]}");

    /* One level PER LOOP, so each is its own named section. Merging them
     * into a single "loops" level would make the six one section, but the
     * planner numbers a level's continuation pages, so they would read
     * "Loops - 2".."Loops - 6" and stop saying which loop you were on. */
    for (int i = 0; i < NUM_LOOPS; i++) {
        int n = i + 1;
        pos += snprintf(json + pos, sizeof(json) - pos,
            ",\"loop%d\":{\"label\":\"Loop %d\",\"knobs\":["
              /* Top row is the take: capture it, hear it, bound it.
               * Bottom row is the time it keeps: how fast, how long, how the
               * window meets the period, and where in the cycle it sits. */
              "\"loop%d_record\",\"loop%d_trig\",\"loop%d_start\",\"loop%d_end\","
              "\"loop%d_bpm\",\"loop%d_beats\",\"loop%d_fit\",\"loop%d_phase\"]}",
            n, n, n, n, n, n, n, n, n, n);
    }

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
    if (strcmp(key, "master_sync") == 0)
        return snprintf(buf, len, "%s", SYNC_LABELS[s->sync_mode]);
    if (strcmp(key, "master_base") == 0)   return snprintf(buf, len, "%.0f", (double)s->base_bpm);
    if (strcmp(key, "master_spread") == 0) return snprintf(buf, len, "%.0f", (double)s->spread);
    if (strcmp(key, "master_state") == 0)  return master_state_text(s, buf, len);
    if (strcmp(key, "master_dry") == 0)    return snprintf(buf, len, "%.3f", (double)s->dry);
    if (strcmp(key, "master_out") == 0)    return snprintf(buf, len, "%.3f", (double)s->out);
    if (strcmp(key, "master_widen") == 0)  return snprintf(buf, len, "%.3f", (double)s->widen);

    if (strcmp(key, "chain_params") == 0)  return build_chain_params(buf, len);
    if (strcmp(key, "ui_hierarchy") == 0)  return build_ui_hierarchy(buf, len);

    return param_absent(buf, len);
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

audio_fx_api_v2_t *move_audio_fx_init_v2(const host_api_v1_t *host) {
    g_host = host;
    (void)g_host;   /* SYNC MOVE reaches for get_bpm/get_clock_status in step 6 */

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
