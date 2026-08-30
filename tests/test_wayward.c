/* Wayward bench tests.
 *
 * Black box: this file includes only the two host headers and drives the
 * module through the public v2 API, exactly as chain_host would. It never
 * reaches into wayward.c's internals, and it re-declares the constants it
 * needs rather than sharing macros, so that a constant drifting is itself a
 * failure.
 *
 * At this stage the module is the skeleton — the control surface and the
 * record state machine, with a passthrough process_block — so these are
 * contract and state-machine tests. The DSP tests named in DESIGN.md (period
 * arithmetic, the phasing property, pad vs trim, click-freeness) arrive with
 * the loop engine in step 2.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "host/audio_fx_api_v2.h"
#include "host/plugin_api_v1.h"

extern audio_fx_api_v2_t *move_audio_fx_init_v2(const host_api_v1_t *host);

#define TEST_NUM_LOOPS   6
#define TEST_PARAM_COUNT 69
#define FRAMES 128
#define SR     44100L

static int g_failures = 0;

static void check(int cond, const char *msg) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", msg);
        g_failures++;
    }
}


static int count_occurrences(const char *hay, const char *needle) {
    int n = 0;
    const char *p = hay;
    size_t nlen = strlen(needle);
    while ((p = strstr(p, needle)) != NULL) { n++; p += nlen; }
    return n;
}


/* ------------------------------------------------------------------ */
/* Helpers for the DSP tests                                           */
/* ------------------------------------------------------------------ */

/* Drive `frames` of silence through the module, discarding the output. */
static void run_silence(audio_fx_api_v2_t *api, void *inst, long frames) {
    int16_t blk[FRAMES * 2];
    for (long done = 0; done < frames; done += FRAMES) {
        memset(blk, 0, sizeof(blk));
        api->process_block(inst, blk, FRAMES);
    }
}

/* Record a take into one loop: arm it, push `material` through, close it.
 * `material` is mono; it is written to both channels. */
static void record_take(audio_fx_api_v2_t *api, void *inst, const char *loop,
                        const int16_t *material, long frames) {
    char key[32];
    int16_t blk[FRAMES * 2];
    snprintf(key, sizeof(key), "%s_record", loop);
    api->set_param(inst, key, "GO");
    /* Push EXACTLY `frames`, with a short final block if it does not divide
     * evenly. Rounding up to a whole block instead welds a tail of silence
     * onto every take — a hard step inside the window that no edge fade can
     * reach, which is not the material any of these tests mean to measure. */
    for (long done = 0; done < frames; done += FRAMES) {
        long n = frames - done;
        if (n > FRAMES) n = FRAMES;
        for (long i = 0; i < n; i++) {
            blk[2 * i] = material[done + i];
            blk[2 * i + 1] = material[done + i];
        }
        api->process_block(inst, blk, (int)n);
    }
    api->set_param(inst, key, "GO");
}

/* Put the module into a state where only the loops are audible and the
 * arithmetic is unity: no live input, no stereo fan, faders open. */
static void isolate_loops(audio_fx_api_v2_t *api, void *inst) {
    api->set_param(inst, "master_dry", "0");
    api->set_param(inst, "master_out", "1");
    api->set_param(inst, "master_widen", "0");
    for (int L = 1; L <= TEST_NUM_LOOPS; L++) {
        char key[32];
        snprintf(key, sizeof(key), "loop%d_volume", L);
        api->set_param(inst, key, "1");
    }
    /* The gain chases are per block; let them arrive before measuring. */
    run_silence(api, inst, FRAMES * 64);
}

/* Count frames whose left channel rises above `thresh` after being at or
 * below it — i.e. the number of distinct events in the output. */
static long count_bursts(audio_fx_api_v2_t *api, void *inst,
                         long frames, int thresh) {
    int16_t blk[FRAMES * 2];
    long bursts = 0;
    int armed = 1;
    for (long done = 0; done < frames; done += FRAMES) {
        memset(blk, 0, sizeof(blk));
        api->process_block(inst, blk, FRAMES);
        for (int i = 0; i < FRAMES; i++) {
            int v = blk[2 * i];
            if (v < 0) v = -v;
            if (armed && v > thresh) { bursts++; armed = 0; }
            else if (!armed && v <= thresh) armed = 1;
        }
    }
    return bursts;
}

int main(void) {
    audio_fx_api_v2_t *api = move_audio_fx_init_v2(NULL);
    check(api != NULL, "init returns an api");
    check(api->api_version == AUDIO_FX_API_VERSION_2, "api_version is 2");
    check(api->create_instance && api->destroy_instance && api->process_block
          && api->set_param && api->get_param,
          "all five required callbacks are populated");

    char buf[20000];

    /* ---- test 0: the chain_params contract ---------------------------- */
    {
        void *inst = api->create_instance(NULL, NULL);
        check(inst != NULL, "test0: create_instance succeeds (31.7 MB of buffers)");

        int n = api->get_param(inst, "chain_params", buf, sizeof(buf));
        check(n > 0, "test0: chain_params returns something");

        int keys = count_occurrences(buf, "\"key\":");
        check(keys == TEST_PARAM_COUNT,
              "test0: chain_params has exactly 69 entries — 9 master, and per\n"
              "       loop a fader, a cycle readout, and the 7 on its own page");

        /* snprintf truncates SILENTLY and a module that overflows its JSON
         * buffer simply stops having a UI. Fail while there is still room. */
        check(n < 12000,
              "test0: chain_params fits its 16384 stack buffer with real\n"
              "       headroom — it must fail here while it can still be fixed");

        check(buf[0] == '[' && buf[n - 1] == ']',
              "test0: chain_params is a well-formed array");

        /* Every float carrying unit % must be declared 0..1: the host
         * multiplies by 100 for display, so a 0..100 declaration reads
         * 5000%. Catch the classic error by asserting no entry pairs a
         * percent unit with a max above 1. */
        check(strstr(buf, "\"max\":100,\"default\"") == NULL,
              "test0: no percent-unit param is declared 0..100");

        api->destroy_instance(inst);
    }

    /* ---- test 1: every declared key is on exactly one page ------------- */
    {
        void *inst = api->create_instance(NULL, NULL);
        char ui[10000];
        int n = api->get_param(inst, "ui_hierarchy", ui, sizeof(ui));
        check(n > 0 && n < 6000,
              "test1: ui_hierarchy fits its 8192 buffer with headroom");

        const char *master[] = { "master_play", "master_base",
                                 "master_spread", "master_align", "master_resync",
                                 "master_clear",
                                 "master_dry", "master_out", "master_widen" };
        for (size_t i = 0; i < sizeof(master) / sizeof(master[0]); i++) {
            char probe[64];
            snprintf(probe, sizeof(probe), "\"%s\"", master[i]);
            check(strstr(ui, probe) != NULL,
                  "test1: every master key appears in ui_hierarchy");
        }

        const char *suffixes[] = { "record", "trig", "start", "end",
                                   "bpm", "beats", "fit", "phase", "volume",
                                   "cycle" };
        for (int L = 1; L <= TEST_NUM_LOOPS; L++) {
            for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
                char probe[64];
                snprintf(probe, sizeof(probe), "\"loop%d_%s\"", L, suffixes[i]);
                check(strstr(ui, probe) != NULL,
                      "test1: every per-loop key appears in ui_hierarchy");
            }
        }

        /* Ten levels: root, Orbits, Shape, Mix, and one per loop. */
        check(count_occurrences(ui, "\"knobs\":") == 10,
              "test1: exactly ten pages are declared");
        check(count_occurrences(ui, "\"level\":") == 9,
              "test1: root carries nine nav entries — Orbits, Shape, Mix and\n"
              "       the six loop pages");
        /* One deliberate blank cell on Main. */
        check(strstr(ui, "\"\",\"master_clear\"") != NULL,
              "test1: CLEAR sits in Shape's far corner, the cell furthest from\n"
              "       anything reached in a hurry");
        check(strstr(ui, "\"loop3_record\",\"master_play\"") != NULL,
              "test1: Main puts the six RECs in a 3x2 block with the transport\n"
              "       in the right-hand column");
        check(strstr(ui, "\"master_align\",\"\",\"loop4_cycle\"") != NULL,
              "test1: Orbits puts ALIGN at the end of the top row, with the\n"
              "       second row's first cell blank");
        check(strstr(ui, "\"loop1_trig\",\"loop1_start\"") != NULL,
              "test1: a loop page now opens on TRIG, REC having moved to Main");

        api->destroy_instance(inst);
    }

    /* ---- test 2: unknown keys are served, never -1 --------------------- */
    {
        void *inst = api->create_instance(NULL, NULL);
        /* The host probes these on every repaint. Returning -1 means "the
         * read did not complete" and is retried forever — the mistake that
         * cost Forgetful 19,913 giveup events. */
        const char *probes[] = { "preset_name", "is_loading", "display_name",
                                 "loop7_bpm", "loop1_nonsense", "" };
        for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
            int n = api->get_param(inst, probes[i], buf, sizeof(buf));
            check(n == 0 && buf[0] == '\0',
                  "test2: an unknown key returns an empty string and 0, never -1");
        }
        api->destroy_instance(inst);
    }

    /* ---- test 3: name and state --------------------------------------- */
    {
        void *inst = api->create_instance(NULL, NULL);
        api->get_param(inst, "name", buf, sizeof(buf));
        check(strcmp(buf, "Wayward") == 0, "test3: name is Wayward");

        /* State is deliberately not implemented — but it must answer with a
         * complete empty object, not -1. */
        api->set_param(inst, "state", "{\"anything\":1}");
        int n = api->get_param(inst, "state", buf, sizeof(buf));
        check(n > 0 && strcmp(buf, "{}") == 0,
              "test3: reading state returns {}, not -1");
        api->destroy_instance(inst);
    }

    /* ---- test 4: passthrough is bit-exact ------------------------------ */
    {
        void *inst = api->create_instance(NULL, NULL);
        int16_t audio[FRAMES * 2], copy[FRAMES * 2];
        for (int i = 0; i < FRAMES * 2; i++) {
            audio[i] = (int16_t)((i * 977) % 30000 - 15000);
            copy[i]  = audio[i];
        }
        api->process_block(inst, audio, FRAMES);
        check(memcmp(audio, copy, sizeof(audio)) == 0,
              "test4: the skeleton passes audio through bit-exact");
        api->destroy_instance(inst);
    }

    /* ---- test 5: the SPREAD macro writes the six tempi ----------------- */
    {
        void *inst = api->create_instance(NULL, NULL);

        /* Defaults: base 100, spread 1 => 100..105, the piece out of the box. */
        for (int L = 1; L <= TEST_NUM_LOOPS; L++) {
            char key[32], want[8];
            snprintf(key, sizeof(key), "loop%d_bpm", L);
            snprintf(want, sizeof(want), "%d", 99 + L);
            api->get_param(inst, key, buf, sizeof(buf));
            check(strcmp(buf, want) == 0,
                  "test5: default base 100 / spread 1 gives 100..105");
        }

        /* Spread 0 locks the ensemble in unison — the piece's biggest gesture. */
        api->set_param(inst, "master_spread", "0");
        for (int L = 1; L <= TEST_NUM_LOOPS; L++) {
            char key[32];
            snprintf(key, sizeof(key), "loop%d_bpm", L);
            api->get_param(inst, key, buf, sizeof(buf));
            check(strcmp(buf, "100") == 0,
                  "test5: spread 0 puts all six loops at the same tempo");
        }

        /* Negative spread fans downward. */
        api->set_param(inst, "master_base", "120");
        api->set_param(inst, "master_spread", "-2");
        api->get_param(inst, "loop6_bpm", buf, sizeof(buf));
        check(strcmp(buf, "110") == 0,
              "test5: base 120 spread -2 puts loop 6 at 110");

        /* BPM snaps to whole numbers: integer tempo relationships are what
         * produce clean phase cycles. */
        api->set_param(inst, "loop1_bpm", "100.4");
        api->get_param(inst, "loop1_bpm", buf, sizeof(buf));
        check(strcmp(buf, "100") == 0, "test5: BPM rounds to an integer");

        /* And clamps. */
        api->set_param(inst, "loop1_bpm", "9999");
        api->get_param(inst, "loop1_bpm", buf, sizeof(buf));
        check(strcmp(buf, "200") == 0, "test5: BPM clamps to its declared max");

        api->destroy_instance(inst);
    }

    /* ---- test 6: enum wire format, labels and indices ------------------ */
    {
        void *inst = api->create_instance(NULL, NULL);

        /* get_param answers with a LABEL, so the host will write labels back
         * — they must be matched before any atoi fallback. */
        api->get_param(inst, "loop1_fit", buf, sizeof(buf));
        check(strcmp(buf, "PAD") == 0, "test6: FIT defaults to PAD");

        api->set_param(inst, "loop1_fit", "SPD");
        api->get_param(inst, "loop1_fit", buf, sizeof(buf));
        check(strcmp(buf, "SPD") == 0, "test6: FIT accepts a label");

        api->set_param(inst, "loop1_fit", "5");
        api->get_param(inst, "loop1_fit", buf, sizeof(buf));
        check(strcmp(buf, "RPTF") == 0, "test6: FIT also accepts an index");

        /* Garbage must leave the value alone rather than reset it. */
        api->set_param(inst, "loop1_fit", "banana");
        api->get_param(inst, "loop1_fit", buf, sizeof(buf));
        check(strcmp(buf, "RPTF") == 0, "test6: an unparseable enum write is ignored");

        /* Every raw mode is followed by its faded companion, so the six read
         * as three pairs. */
        const char *fit_order[] = { "PAD", "PADF", "SPD", "SPDF", "RPT", "RPTF" };
        for (int i = 0; i < 6; i++) {
            char idx[4];
            snprintf(idx, sizeof(idx), "%d", i);
            api->set_param(inst, "loop1_fit", idx);
            api->get_param(inst, "loop1_fit", buf, sizeof(buf));
            check(strcmp(buf, fit_order[i]) == 0,
                  "test6: the six FIT modes are three raw/faded pairs in order");
            check(strpbrk(buf, "-_+ ") == NULL,
                  "test6: no FIT label contains a character the enum-square\n"
                  "       renderer would break the value on");
        }

        api->destroy_instance(inst);
    }

    /* ---- test 7: the record state machine, read per loop --------------- */
    {
        void *inst = api->create_instance(NULL, NULL);

        for (int L = 1; L <= TEST_NUM_LOOPS; L++) {
            char key[32];
            snprintf(key, sizeof(key), "loop%d_cycle", L);
            api->get_param(inst, key, buf, sizeof(buf));
            check(strcmp(buf, "-") == 0,
                  "test7: every loop reads empty at startup");
        }

        /* A trigger fires on anything that is not the idle spelling. */
        api->set_param(inst, "loop3_record", "-");
        api->get_param(inst, "loop3_cycle", buf, sizeof(buf));
        check(strcmp(buf, "-") == 0,
              "test7: writing the idle spelling does not fire a trigger");

        api->set_param(inst, "loop3_record", "GO");
        api->get_param(inst, "loop3_cycle", buf, sizeof(buf));
        check(strcmp(buf, "REC") == 0,
              "test7: the loop being recorded says so, and it is the only\n"
              "       place that can — a write-only trigger's cell shows its\n"
              "       static label and nothing else");
        api->get_param(inst, "loop2_cycle", buf, sizeof(buf));
        check(strcmp(buf, "-") == 0, "test7: and its neighbours are unaffected");

        /* The button names what the NEXT press will do. */
        api->get_param(inst, "loop3_record", buf, sizeof(buf));
        check(strcmp(buf, "STOP") == 0,
              "test7: while recording, REC reads STOP");
        api->get_param(inst, "loop1_record", buf, sizeof(buf));
        check(strcmp(buf, "REC") == 0,
              "test7: an idle loop's button still reads REC");

        /* Stopping with nothing captured discards the take. */
        api->set_param(inst, "loop3_record", "GO");
        api->get_param(inst, "loop3_cycle", buf, sizeof(buf));
        check(strcmp(buf, "-") == 0,
              "test7: a take under the 50 ms minimum is discarded");

        api->destroy_instance(inst);
    }

    /* ---- test 7b: REC is three-way — record, close, overdub ----------- */
    {
        void *inst = api->create_instance(NULL, NULL);

        /* Give loop 1 a take. The skeleton captures no audio, so drive the
         * state machine the only way a black-box test can: record long
         * enough is impossible yet, so this asserts the EMPTY->RECORDING leg
         * and the readout vocabulary that goes with it. */
        api->get_param(inst, "loop1_record", buf, sizeof(buf));
        check(strcmp(buf, "REC") == 0, "test7b: an empty loop offers REC");

        api->set_param(inst, "loop1_record", "GO");
        api->get_param(inst, "loop1_record", buf, sizeof(buf));
        check(strcmp(buf, "STOP") == 0, "test7b: a recording loop offers STOP");

        api->destroy_instance(inst);
    }

    /* ---- test 8: transport ------------------------------------------- */
    {
        void *inst = api->create_instance(NULL, NULL);

        api->get_param(inst, "master_play", buf, sizeof(buf));
        check(strcmp(buf, "PLAY") == 0, "test8: stopped, the button reads PLAY");

        api->set_param(inst, "master_play", "GO");
        api->get_param(inst, "master_play", buf, sizeof(buf));
        check(strcmp(buf, "STOP") == 0, "test8: running, the button reads STOP");

        api->set_param(inst, "master_play", "GO");
        api->get_param(inst, "master_play", buf, sizeof(buf));
        check(strcmp(buf, "PLAY") == 0, "test8: pressing again stops it");

        /* RSYN must not be a second play button. */
        api->set_param(inst, "master_resync", "GO");
        api->get_param(inst, "master_play", buf, sizeof(buf));
        check(strcmp(buf, "PLAY") == 0,
              "test8: RESYNC realigns without starting the ensemble");

        api->destroy_instance(inst);
    }

    /* ---- test 9: window bounds clamp but never reorder ----------------- */
    {
        void *inst = api->create_instance(NULL, NULL);

        api->get_param(inst, "loop1_end", buf, sizeof(buf));
        check(strcmp(buf, "1.000") == 0,
              "test9: END defaults to +1 — the whole take, forwards");

        /* END is a bipolar LENGTH from START, not a second position, so a
         * negative value is a legal reversed window rather than an
         * inside-out one. */
        api->set_param(inst, "loop1_start", "0.750");
        api->set_param(inst, "loop1_end",   "-0.500");
        api->get_param(inst, "loop1_start", buf, sizeof(buf));
        check(strcmp(buf, "0.750") == 0, "test9: START keeps what the knob said");
        api->get_param(inst, "loop1_end", buf, sizeof(buf));
        check(strcmp(buf, "-0.500") == 0,
              "test9: a negative END is kept — it means reversed, not invalid");

        api->set_param(inst, "loop1_end", "-9");
        api->get_param(inst, "loop1_end", buf, sizeof(buf));
        check(strcmp(buf, "-1.000") == 0, "test9: END clamps at -1");
        api->set_param(inst, "loop1_end", "9");
        api->get_param(inst, "loop1_end", buf, sizeof(buf));
        check(strcmp(buf, "1.000") == 0, "test9: END clamps at +1");

        api->set_param(inst, "loop1_start", "-3");
        api->get_param(inst, "loop1_start", buf, sizeof(buf));
        check(strcmp(buf, "0.000") == 0, "test9: START clamps at 0");

        api->set_param(inst, "loop1_phase", "-9");
        api->get_param(inst, "loop1_phase", buf, sizeof(buf));
        check(strcmp(buf, "-0.500") == 0, "test9: PHASE clamps at -0.5");

        api->destroy_instance(inst);
    }


    /* ================================================================== */
    /* The loop engine                                                     */
    /* ================================================================== */

    /* ---- test 10: a take is captured and plays back -------------------- */
    {
        void *inst = api->create_instance(NULL, NULL);
        isolate_loops(api, inst);

        /* Half a second of a constant, well above the 50 ms minimum. */
        const long TAKE = 22050;
        int16_t *mat = (int16_t *)malloc(sizeof(int16_t) * TAKE);
        for (long i = 0; i < TAKE; i++) mat[i] = 8192;
        record_take(api, inst, "loop1", mat, TAKE);

        api->get_param(inst, "loop1_record", buf, sizeof(buf));
        check(strcmp(buf, "DUB") == 0,
              "test10: a closed take leaves the button offering DUB");
        api->get_param(inst, "loop1_cycle", buf, sizeof(buf));
        check(strcmp(buf, "S") == 0,
              "test10: loop 1 reads as holding a take while stopped");

        /* Silence the other five so only loop 1 is measured. */
        for (int L = 2; L <= TEST_NUM_LOOPS; L++) {
            char key[32];
            snprintf(key, sizeof(key), "loop%d_volume", L);
            api->set_param(inst, key, "0");
        }
        api->set_param(inst, "master_play", "GO");
        api->get_param(inst, "loop1_cycle", buf, sizeof(buf));
        check(buf[0] >= '0' && buf[0] <= '9',
              "test10: once running, the readout is how far through its cycle\n"
              "        the loop is");

        int16_t blk[FRAMES * 2];
        memset(blk, 0, sizeof(blk));
        api->process_block(inst, blk, FRAMES);
        int peak = 0;
        for (int i = 0; i < FRAMES; i++) {
            int v = blk[2 * i]; if (v < 0) v = -v;
            if (v > peak) peak = v;
        }
        check(peak > 7000 && peak < 9000,
              "test10: the take plays back at roughly the level recorded —\n"
              "        8192 in, through unity gain and the soft clipper");

        free(mat);
        api->destroy_instance(inst);
    }

    /* ---- test 11: period arithmetic ----------------------------------- */
    /* At 100 BPM x 4 beats the period is 4 * 60/100 * 44100 = 105,840
     * frames exactly. A take whose only content is an impulse at frame 0
     * therefore fires once per period, and counting those is a direct
     * measurement of the period on the audio itself. */
    {
        void *inst = api->create_instance(NULL, NULL);
        isolate_loops(api, inst);

        /* 32 frames wide, not one: the period is rarely a whole number of
         * frames, so a one-frame impulse is read at a fractional index and
         * interpolated down to whatever the neighbouring silence dictates —
         * present on some cycles and invisible on others. */
        const long TAKE = 4410;
        int16_t *mat = (int16_t *)calloc(TAKE, sizeof(int16_t));
        for (int i = 0; i < 32; i++) mat[i] = 20000;
        record_take(api, inst, "loop1", mat, TAKE);

        for (int L = 2; L <= TEST_NUM_LOOPS; L++) {
            char key[32];
            snprintf(key, sizeof(key), "loop%d_volume", L);
            api->set_param(inst, key, "0");
        }
        api->set_param(inst, "loop1_bpm", "100");
        api->set_param(inst, "loop1_beats", "4");
        api->set_param(inst, "loop1_fit", "PAD");
        api->set_param(inst, "master_play", "GO");

        /* 240 s = exactly 100 periods, so 101 impulses counting the one at
         * the start. */
        long n = count_bursts(api, inst, 240L * SR, 5000);
        check(n == 101,
              "test11: 100 BPM x 4 beats fires 101 impulses in 240 s —\n"
              "        a period of 105,840 frames, measured on the output");

        free(mat);
        api->destroy_instance(inst);
    }

    /* ---- test 12: THE PHASING PROPERTY --------------------------------- */
    /* The module's entire reason to exist. Two loops at 100 and 101 BPM,
     * started together: in the time the first plays 100 cycles the second
     * plays exactly 101 — one whole extra lap. Asserted on the audio. */
    {
        const int bpms[2] = { 100, 101 };
        const long expect[2] = { 101, 102 };
        for (int t = 0; t < 2; t++) {
            void *inst = api->create_instance(NULL, NULL);
            isolate_loops(api, inst);

            const long TAKE = 4410;
            int16_t *mat = (int16_t *)calloc(TAKE, sizeof(int16_t));
            for (int i = 0; i < 32; i++) mat[i] = 20000;
            record_take(api, inst, "loop1", mat, TAKE);

            for (int L = 2; L <= TEST_NUM_LOOPS; L++) {
                char key[32];
                snprintf(key, sizeof(key), "loop%d_volume", L);
                api->set_param(inst, key, "0");
            }
            char v[8];
            snprintf(v, sizeof(v), "%d", bpms[t]);
            api->set_param(inst, "loop1_bpm", v);
            api->set_param(inst, "loop1_beats", "4");
            api->set_param(inst, "master_play", "GO");

            long n = count_bursts(api, inst, 240L * SR, 5000);
            check(n == expect[t],
                  "test12: over the same 240 s the 101 BPM loop completes one\n"
                  "        more cycle than the 100 BPM loop — they drift apart\n"
                  "        and meet again exactly once");
            free(mat);
            api->destroy_instance(inst);
        }
    }

    /* ---- test 13: PAD pads with real silence --------------------------- */
    {
        void *inst = api->create_instance(NULL, NULL);
        isolate_loops(api, inst);

        const long TAKE = 22050;              /* 0.5 s of constant */
        int16_t *mat = (int16_t *)malloc(sizeof(int16_t) * TAKE);
        for (long i = 0; i < TAKE; i++) mat[i] = 8192;
        record_take(api, inst, "loop1", mat, TAKE);

        for (int L = 2; L <= TEST_NUM_LOOPS; L++) {
            char key[32];
            snprintf(key, sizeof(key), "loop%d_volume", L);
            api->set_param(inst, key, "0");
        }
        /* 60 BPM x 2 beats = 2 s period against a 0.5 s window. */
        api->set_param(inst, "loop1_bpm", "60");
        api->set_param(inst, "loop1_beats", "2");
        api->set_param(inst, "loop1_fit", "PAD");
        api->set_param(inst, "master_play", "GO");

        int16_t blk[FRAMES * 2];
        long sounding = 0, silent = 0;
        const long PERIOD = 2 * SR;
        for (long done = 0; done < PERIOD; done += FRAMES) {
            memset(blk, 0, sizeof(blk));
            api->process_block(inst, blk, FRAMES);
            for (int i = 0; i < FRAMES; i++) {
                if (done + i >= PERIOD) break;
                if (done + i < TAKE) { if (blk[2 * i] != 0) sounding++; }
                else if (blk[2 * i] != 0) silent++;
            }
        }
        check(sounding > TAKE - 200,
              "test13: the window sounds for its whole length");
        check(silent == 0,
              "test13: everything between the end of the window and the wrap\n"
              "        is EXACT silence — that is the padding");

        free(mat);
        api->destroy_instance(inst);
    }

    /* ---- test 14: a period shorter than the window truncates ----------- */
    {
        void *inst = api->create_instance(NULL, NULL);
        isolate_loops(api, inst);

        /* A ramp, so the position within the take is readable from the
         * sample value alone. */
        const long TAKE = 44100;
        int16_t *mat = (int16_t *)malloc(sizeof(int16_t) * TAKE);
        for (long i = 0; i < TAKE; i++) mat[i] = (int16_t)(i * 20000 / TAKE);
        record_take(api, inst, "loop1", mat, TAKE);

        for (int L = 2; L <= TEST_NUM_LOOPS; L++) {
            char key[32];
            snprintf(key, sizeof(key), "loop%d_volume", L);
            api->set_param(inst, key, "0");
        }
        /* 240 BPM x 1 beat = 0.25 s period against a 1 s window, so only the
         * first quarter of the take can ever be reached. */
        api->set_param(inst, "loop1_bpm", "240");
        api->set_param(inst, "loop1_beats", "1");
        api->set_param(inst, "loop1_fit", "PAD");
        api->set_param(inst, "master_play", "GO");

        int16_t blk[FRAMES * 2];
        int peak = 0;
        for (long done = 0; done < 4L * SR; done += FRAMES) {
            memset(blk, 0, sizeof(blk));
            api->process_block(inst, blk, FRAMES);
            for (int i = 0; i < FRAMES; i++) {
                int v = blk[2 * i]; if (v < 0) v = -v;
                if (v > peak) peak = v;
            }
        }
        /* A quarter of the way up a ramp to 20000 is about 5000. Anything
         * far above it means the head ran past where the wrap should be. */
        check(peak > 4000 && peak < 6500,
              "test14: a period shorter than the window cuts the tail off —\n"
              "        the head wraps before it can reach the rest of the take");

        free(mat);
        api->destroy_instance(inst);
    }

    /* ---- test 15: SPD fills the period, RPT repeats -------------------- */
    {
        const char *modes[2] = { "SPD", "RPT" };
        for (int m = 0; m < 2; m++) {
            void *inst = api->create_instance(NULL, NULL);
            isolate_loops(api, inst);

            const long TAKE = 22050;
            int16_t *mat = (int16_t *)malloc(sizeof(int16_t) * TAKE);
            for (long i = 0; i < TAKE; i++) mat[i] = 8192;
            record_take(api, inst, "loop1", mat, TAKE);

            for (int L = 2; L <= TEST_NUM_LOOPS; L++) {
                char key[32];
                snprintf(key, sizeof(key), "loop%d_volume", L);
                api->set_param(inst, key, "0");
            }
            api->set_param(inst, "loop1_bpm", "60");
            api->set_param(inst, "loop1_beats", "2");   /* 2 s vs a 0.5 s window */
            api->set_param(inst, "loop1_fit", modes[m]);
            api->set_param(inst, "master_play", "GO");

            int16_t blk[FRAMES * 2];
            long silent = 0;
            for (long done = 0; done < 2L * SR; done += FRAMES) {
                memset(blk, 0, sizeof(blk));
                api->process_block(inst, blk, FRAMES);
                for (int i = 0; i < FRAMES; i++)
                    if (blk[2 * i] == 0) silent++;
            }
            check(silent < 200,
                  "test15: SPD and RPT leave no gap — unlike PAD they fill the\n"
                  "        whole period, one by stretching and one by repeating");
            free(mat);
            api->destroy_instance(inst);
        }
    }

    /* ---- test 16: a reversed window plays backwards -------------------- */
    {
        void *inst = api->create_instance(NULL, NULL);
        isolate_loops(api, inst);

        /* A rising ramp: played in reverse it must fall. */
        const long TAKE = 22050;
        int16_t *mat = (int16_t *)malloc(sizeof(int16_t) * TAKE);
        for (long i = 0; i < TAKE; i++) mat[i] = (int16_t)(i * 20000 / TAKE);
        record_take(api, inst, "loop1", mat, TAKE);

        for (int L = 2; L <= TEST_NUM_LOOPS; L++) {
            char key[32];
            snprintf(key, sizeof(key), "loop%d_volume", L);
            api->set_param(inst, key, "0");
        }
        /* START at the very end, END fully negative: the whole take,
         * backwards. */
        api->set_param(inst, "loop1_start", "1");
        api->set_param(inst, "loop1_end", "-1");
        api->set_param(inst, "loop1_bpm", "60");
        api->set_param(inst, "loop1_beats", "2");
        api->set_param(inst, "loop1_fit", "PAD");
        api->set_param(inst, "master_play", "GO");

        int16_t blk[FRAMES * 2];
        int first = -1, last = 0;
        for (long done = 0; done < TAKE; done += FRAMES) {
            memset(blk, 0, sizeof(blk));
            api->process_block(inst, blk, FRAMES);
            for (int i = 0; i < FRAMES; i++) {
                if (done + i >= TAKE) break;
                if (first < 0) first = blk[2 * i];
                last = blk[2 * i];
            }
        }
        check(first > 15000 && last < 5000,
              "test16: END below centre reads the window from its far end\n"
              "        back to START — a rising ramp comes out falling");

        free(mat);
        api->destroy_instance(inst);
    }

    /* ---- test 17: the F companions remove the seam click ---------------- */
    {
        const char *modes[2] = { "PAD", "PADF" };
        int step[2] = { 0, 0 };
        for (int m = 0; m < 2; m++) {
            void *inst = api->create_instance(NULL, NULL);
            isolate_loops(api, inst);

            /* A constant take: every window edge is a full-scale step unless
             * something fades it. */
            const long TAKE = 22050;
            int16_t *mat = (int16_t *)malloc(sizeof(int16_t) * TAKE);
            for (long i = 0; i < TAKE; i++) mat[i] = 12000;
            record_take(api, inst, "loop1", mat, TAKE);

            for (int L = 2; L <= TEST_NUM_LOOPS; L++) {
                char key[32];
                snprintf(key, sizeof(key), "loop%d_volume", L);
                api->set_param(inst, key, "0");
            }
            api->set_param(inst, "loop1_bpm", "60");
            api->set_param(inst, "loop1_beats", "2");
            api->set_param(inst, "loop1_fit", modes[m]);
            api->set_param(inst, "master_play", "GO");

            int16_t blk[FRAMES * 2];
            int prev = 0, worst = 0;
            for (long done = 0; done < 4L * SR; done += FRAMES) {
                memset(blk, 0, sizeof(blk));
                api->process_block(inst, blk, FRAMES);
                for (int i = 0; i < FRAMES; i++) {
                    int d = blk[2 * i] - prev;
                    if (d < 0) d = -d;
                    if (d > worst) worst = d;
                    prev = blk[2 * i];
                }
            }
            step[m] = worst;
            free(mat);
            api->destroy_instance(inst);
        }
        check(step[0] > 8000,
              "test17: the raw mode keeps its click — a hard splice is a\n"
              "        percussive event, and that is the point of the pair");
        check(step[1] < 500,
              "test17: the F companion fades both ends, so the largest step\n"
              "        anywhere in the output is small");
        check(step[1] < step[0] / 8,
              "test17: the companion is dramatically smoother than its raw twin");
    }

    /* ---- test 18: BPM swept while playing must not jump the loop ------- */
    {
        void *inst = api->create_instance(NULL, NULL);
        isolate_loops(api, inst);

        const long TAKE = 22050;
        int16_t *mat = (int16_t *)malloc(sizeof(int16_t) * TAKE);
        for (long i = 0; i < TAKE; i++) mat[i] = 12000;
        record_take(api, inst, "loop1", mat, TAKE);

        for (int L = 2; L <= TEST_NUM_LOOPS; L++) {
            char key[32];
            snprintf(key, sizeof(key), "loop%d_volume", L);
            api->set_param(inst, key, "0");
        }
        api->set_param(inst, "loop1_fit", "PADF");
        api->set_param(inst, "master_play", "GO");

        int16_t blk[FRAMES * 2];
        int prev = 0, worst = 0;
        for (int b = 0; b < 400; b++) {
            char v[8];
            snprintf(v, sizeof(v), "%d", 100 + (b % 60));
            api->set_param(inst, "loop1_bpm", v);
            memset(blk, 0, sizeof(blk));
            api->process_block(inst, blk, FRAMES);
            for (int i = 0; i < FRAMES; i++) {
                int d = blk[2 * i] - prev;
                if (d < 0) d = -d;
                if (d > worst) worst = d;
                prev = blk[2 * i];
            }
        }
        check(worst < 2000,
              "test18: sweeping BPM under the hand does not jump the loop —\n"
              "        phase is normalised, so the head keeps its proportional\n"
              "        position in the cycle rather than leaping");

        free(mat);
        api->destroy_instance(inst);
    }


    /* ---- test 19: leading silence is trimmed automatically ------------- */
    {
        void *inst = api->create_instance(NULL, NULL);
        isolate_loops(api, inst);

        /* Half a second of dead air, then half a second of tone. Recorded
         * as-is the window would open with silence and START would have to
         * be dialled in by hand. */
        const long LEAD = 22050, BODY = 22050;
        int16_t *mat = (int16_t *)calloc(LEAD + BODY, sizeof(int16_t));
        for (long i = 0; i < BODY; i++) mat[LEAD + i] = 12000;
        record_take(api, inst, "loop1", mat, LEAD + BODY);

        for (int L = 2; L <= TEST_NUM_LOOPS; L++) {
            char key[32];
            snprintf(key, sizeof(key), "loop%d_volume", L);
            api->set_param(inst, key, "0");
        }
        api->set_param(inst, "loop1_bpm", "60");
        api->set_param(inst, "loop1_beats", "2");
        api->set_param(inst, "loop1_fit", "PAD");
        api->set_param(inst, "master_play", "GO");

        /* The take must start sounding almost at once — inside the pre-roll,
         * not half a second later. */
        int16_t blk[FRAMES * 2];
        long firstSound = -1;
        for (long done = 0; done < LEAD; done += FRAMES) {
            memset(blk, 0, sizeof(blk));
            api->process_block(inst, blk, FRAMES);
            for (int i = 0; i < FRAMES && firstSound < 0; i++)
                if (blk[2 * i] != 0) firstSound = done + i;
            if (firstSound >= 0) break;
        }
        check(firstSound >= 0 && firstSound < 400,
              "test19: the take begins at its first audible frame, not at the\n"
              "        half second of dead air in front of it");

        /* But NOT trimmed flush to the onset: a few milliseconds of pre-roll
         * are kept so the leading edge of an attack survives. */
        check(firstSound > 0,
              "test19: a short pre-roll is kept ahead of the onset, so a\n"
              "        transient does not lose its leading edge to the trim");

        free(mat);
        api->destroy_instance(inst);
    }

    /* ---- test 20: a take of pure silence is not a take ----------------- */
    {
        void *inst = api->create_instance(NULL, NULL);
        isolate_loops(api, inst);

        const long TAKE = 44100;
        int16_t *mat = (int16_t *)calloc(TAKE, sizeof(int16_t));
        record_take(api, inst, "loop1", mat, TAKE);

        api->get_param(inst, "loop1_cycle", buf, sizeof(buf));
        check(strcmp(buf, "-") == 0,
              "test20: a recording that never crosses the threshold is\n"
              "        discarded — a loop that reads as loaded and plays\n"
              "        nothing is worse than no loop at all");
        api->get_param(inst, "loop1_record", buf, sizeof(buf));
        check(strcmp(buf, "REC") == 0,
              "test20: and the button offers REC again, not DUB");

        free(mat);
        api->destroy_instance(inst);
    }

    /* ---- test 21: the trim does not disturb the window maths ----------- */
    {
        void *inst = api->create_instance(NULL, NULL);
        isolate_loops(api, inst);

        /* Silence, then a ramp. After trimming, END at +1 must span the ramp
         * and nothing else — so reversing it must still invert cleanly. */
        const long LEAD = 11025, BODY = 22050;
        int16_t *mat = (int16_t *)calloc(LEAD + BODY, sizeof(int16_t));
        for (long i = 0; i < BODY; i++)
            mat[LEAD + i] = (int16_t)(2000 + i * 18000 / BODY);
        record_take(api, inst, "loop1", mat, LEAD + BODY);

        for (int L = 2; L <= TEST_NUM_LOOPS; L++) {
            char key[32];
            snprintf(key, sizeof(key), "loop%d_volume", L);
            api->set_param(inst, key, "0");
        }
        api->set_param(inst, "loop1_start", "1");
        api->set_param(inst, "loop1_end", "-1");
        api->set_param(inst, "loop1_bpm", "60");
        api->set_param(inst, "loop1_beats", "2");
        api->set_param(inst, "loop1_fit", "PAD");
        api->set_param(inst, "master_play", "GO");

        int16_t blk[FRAMES * 2];
        int first = -1, last = 0;
        for (long done = 0; done < BODY; done += FRAMES) {
            memset(blk, 0, sizeof(blk));
            api->process_block(inst, blk, FRAMES);
            for (int i = 0; i < FRAMES; i++) {
                if (done + i >= BODY) break;
                if (first < 0) first = blk[2 * i];
                last = blk[2 * i];
            }
        }
        check(first > 15000,
              "test21: a reversed full window still starts at the ramp's top,\n"
              "        so the trim shifted the window rather than confusing it");
        check(last < 4000,
              "test21: and ends near its bottom");

        free(mat);
        api->destroy_instance(inst);
    }



    /* ---- test 28: PHASE must SLIDE, not teleport ----------------------- */
    /* A linear ramp has no curvature, so the second difference of the output
     * is ~0 everywhere the playhead is moving smoothly, and spikes exactly
     * where it jumps. Turning PHASE moves the read position by
     * (change in offset) * period frames — 529 frames for ONE detent at 100
     * BPM x 4 beats — so applying it in per-block steps splices unrelated
     * audio together several hundred times a second. */
    {
        void *inst = api->create_instance(NULL, NULL);
        isolate_loops(api, inst);

        /* A TRIANGLE, rising to a peak and back to zero. It begins and ends
         * at silence, so the window needs no fade and the raw PAD mode adds
         * no corner of its own — the only curvature anywhere in the output
         * is the apex. Anything else the metric sees is the playhead moving
         * when it should not. */
        const long TAKE = 22050;
        int16_t *mat = (int16_t *)malloc(sizeof(int16_t) * TAKE);
        for (long i = 0; i < TAKE; i++) {
            long up = (i < TAKE / 2) ? i : (TAKE - 1 - i);
            mat[i] = (int16_t)(up * 40000 / TAKE);
        }
        record_take(api, inst, "loop1", mat, TAKE);

        for (int L = 2; L <= TEST_NUM_LOOPS; L++) {
            char key[32];
            snprintf(key, sizeof(key), "loop%d_volume", L);
            api->set_param(inst, key, "0");
        }
        api->set_param(inst, "loop1_bpm", "60");
        api->set_param(inst, "loop1_beats", "2");
        api->set_param(inst, "loop1_fit", "PAD");
        api->set_param(inst, "master_play", "GO");

        int16_t blk[FRAMES * 2];
        int p1 = 0, p2 = 0, worst = 0;
        for (int b = 0; b < 300; b++) {
            char v[16];
            snprintf(v, sizeof(v), "%.3f", (b % 80) * 0.005);
            api->set_param(inst, "loop1_phase", v);
            memset(blk, 0, sizeof(blk));
            api->process_block(inst, blk, FRAMES);
            for (int i = 0; i < FRAMES; i++) {
                const int x = blk[2 * i];
                const int d2 = x - 2 * p1 + p2;
                const int a = d2 < 0 ? -d2 : d2;
                if (b > 4 && a > worst) worst = a;
                p2 = p1;
                p1 = x;
            }
        }
        check(worst < 20,
              "test28: turning PHASE slides the playhead at a bounded rate\n"
              "        instead of teleporting it — a jump splices unrelated\n"
              "        audio together, and a knob sweep does it hundreds of\n"
              "        times a second");

        free(mat);
        api->destroy_instance(inst);
    }


    /* ---- test 29: CLEAR fades, then wipes ------------------------------ */
    {
        void *inst = api->create_instance(NULL, NULL);
        isolate_loops(api, inst);

        const long TAKE = 22050;
        int16_t *mat = (int16_t *)malloc(sizeof(int16_t) * TAKE);
        for (long i = 0; i < TAKE; i++) mat[i] = 12000;
        record_take(api, inst, "loop1", mat, TAKE);
        record_take(api, inst, "loop4", mat, TAKE);

        api->set_param(inst, "loop1_fit", "RPT");
        api->set_param(inst, "loop4_fit", "RPT");
        api->set_param(inst, "master_play", "GO");
        api->set_param(inst, "loop1_volume", "1");
        for (int L = 2; L <= TEST_NUM_LOOPS; L++) {
            char key[32];
            snprintf(key, sizeof(key), "loop%d_volume", L);
            api->set_param(inst, key, L == 4 ? "0" : "0");
        }
        run_silence(api, inst, FRAMES * 16);

        /* Level before the clear starts. */
        int16_t blk[FRAMES * 2];
        memset(blk, 0, sizeof(blk));
        api->process_block(inst, blk, FRAMES);
        int before = 0;
        for (int i = 0; i < FRAMES; i++) {
            int v = blk[2 * i]; if (v < 0) v = -v;
            if (v > before) before = v;
        }
        check(before > 8000, "test29: the ensemble is sounding before the clear");

        api->set_param(inst, "master_clear", "GO");
        api->get_param(inst, "master_clear", buf, sizeof(buf));
        check(strcmp(buf, "KEEP") == 0,
              "test29: once running, the button offers to call it off");
        api->get_param(inst, "master_align", buf, sizeof(buf));
        check(strncmp(buf, "CLR", 3) == 0,
              "test29: ALIGN shows the countdown while a clear is in flight —\n"
              "        fifteen seconds of it matter more than a realignment\n"
              "        figure measured in minutes");

        /* Halfway: audibly quieter, but not gone and not yet wiped. */
        run_silence(api, inst, (long)(7.5 * SR));
        memset(blk, 0, sizeof(blk));
        api->process_block(inst, blk, FRAMES);
        int half = 0;
        for (int i = 0; i < FRAMES; i++) {
            int v = blk[2 * i]; if (v < 0) v = -v;
            if (v > half) half = v;
        }
        check(half > before / 4 && half < before * 3 / 4,
              "test29: halfway through, the ensemble is roughly half as loud —\n"
              "        the erase announces itself instead of just happening");

        /* And at the end everything is gone and back to defaults. */
        run_silence(api, inst, (long)(8.0 * SR));
        for (int L = 1; L <= TEST_NUM_LOOPS; L++) {
            char key[32];
            snprintf(key, sizeof(key), "loop%d_cycle", L);
            api->get_param(inst, key, buf, sizeof(buf));
            check(strcmp(buf, "-") == 0, "test29: every take is erased");
        }
        api->get_param(inst, "master_play", buf, sizeof(buf));
        check(strcmp(buf, "PLAY") == 0, "test29: and the ensemble has stopped");
        api->get_param(inst, "master_out", buf, sizeof(buf));
        check(strcmp(buf, "0.800") == 0, "test29: OUT is back to its default");
        api->get_param(inst, "master_widen", buf, sizeof(buf));
        check(strcmp(buf, "0.500") == 0, "test29: so is WIDEN");
        api->get_param(inst, "loop1_volume", buf, sizeof(buf));
        check(strcmp(buf, "0.800") == 0, "test29: so are the faders");
        api->get_param(inst, "loop6_bpm", buf, sizeof(buf));
        check(strcmp(buf, "105") == 0,
              "test29: and BASE/SPREAD are back to 100 with the six fanned out");
        api->get_param(inst, "loop1_fit", buf, sizeof(buf));
        check(strcmp(buf, "PAD") == 0, "test29: and FIT is back to PAD");
        api->get_param(inst, "master_clear", buf, sizeof(buf));
        check(strcmp(buf, "CLR") == 0, "test29: the button is ready again");

        free(mat);
        api->destroy_instance(inst);
    }

    /* ---- test 30: a clear can be called off ---------------------------- */
    {
        void *inst = api->create_instance(NULL, NULL);
        isolate_loops(api, inst);

        const long TAKE = 22050;
        int16_t *mat = (int16_t *)malloc(sizeof(int16_t) * TAKE);
        for (long i = 0; i < TAKE; i++) mat[i] = 12000;
        record_take(api, inst, "loop1", mat, TAKE);
        api->set_param(inst, "loop1_fit", "RPT");
        api->set_param(inst, "master_play", "GO");
        run_silence(api, inst, FRAMES * 16);

        api->set_param(inst, "master_clear", "GO");
        run_silence(api, inst, (long)(10.0 * SR));

        api->set_param(inst, "master_clear", "GO");   /* changed my mind */
        api->get_param(inst, "master_clear", buf, sizeof(buf));
        check(strcmp(buf, "CLR") == 0, "test30: the button is idle again");

        /* Well past where the wipe would have landed. */
        run_silence(api, inst, (long)(10.0 * SR));
        api->get_param(inst, "loop1_cycle", buf, sizeof(buf));
        check(buf[0] >= '0' && buf[0] <= '9',
              "test30: the take survives and is still running — a destructive\n"
              "        control that cannot be called off is a trap");

        int16_t blk[FRAMES * 2];
        memset(blk, 0, sizeof(blk));
        api->process_block(inst, blk, FRAMES);
        int peak = 0;
        for (int i = 0; i < FRAMES; i++) {
            int v = blk[2 * i]; if (v < 0) v = -v;
            if (v > peak) peak = v;
        }
        check(peak > 8000,
              "test30: and it is back at full level, not stuck where the fade\n"
              "        had got to");

        free(mat);
        api->destroy_instance(inst);
    }


    /* ---- test 31: the realignment countdown ---------------------------- */
    {
        void *inst = api->create_instance(NULL, NULL);
        isolate_loops(api, inst);

        api->get_param(inst, "master_align", buf, sizeof(buf));
        check(strcmp(buf, "-") == 0,
              "test31: with nothing loaded there is nothing to realign");

        const long TAKE = 22050;
        int16_t *mat = (int16_t *)malloc(sizeof(int16_t) * TAKE);
        for (long i = 0; i < TAKE; i++) mat[i] = 12000;
        for (int L = 1; L <= TEST_NUM_LOOPS; L++) {
            char key[32];
            snprintf(key, sizeof(key), "loop%d", L);
            record_take(api, inst, key, mat, TAKE);
        }

        /* Defaults: BEAT 4 everywhere, tempi 100..105. Every loop completes
         * a whole number of cycles in 4 x 60 seconds — that is what makes
         * this figure exact rather than approximate. */
        api->set_param(inst, "master_play", "GO");
        api->get_param(inst, "master_align", buf, sizeof(buf));
        check(strcmp(buf, "239") == 0 || strcmp(buf, "240") == 0,
              "test31: six loops at BEAT 4 and whole tempi realign after\n"
              "        4 x 60 = 240 s, whatever the tempi actually are");

        /* And it counts down. */
        run_silence(api, inst, 100L * SR);
        api->get_param(inst, "master_align", buf, sizeof(buf));
        int rem = atoi(buf);
        check(rem > 135 && rem < 145,
              "test31: 100 s later there are about 140 s left");

        /* RESYNC restarts the count. */
        api->set_param(inst, "master_resync", "GO");
        api->get_param(inst, "master_align", buf, sizeof(buf));
        rem = atoi(buf);
        check(rem > 235,
              "test31: RESYNC realigns the ensemble and the countdown with it");

        /* A different BEAT on one loop changes the arithmetic: 3 against 4
         * needs lcm(3,4) = 12 quarter-notes, so 12 x 60 = 720 s, which is
         * shown in minutes. */
        api->set_param(inst, "loop2_beats", "3");
        api->get_param(inst, "master_align", buf, sizeof(buf));
        check(strchr(buf, 'M') != NULL,
              "test31: past ten minutes the countdown switches to minutes —\n"
              "        a five-character cell cannot hold 720 seconds usefully");

        free(mat);
        api->destroy_instance(inst);
    }

    /* ---- test 32: faders reach 200% ------------------------------------ */
    {
        void *inst = api->create_instance(NULL, NULL);
        api->get_param(inst, "loop1_volume", buf, sizeof(buf));
        check(strcmp(buf, "0.800") == 0,
              "test32: the default fader is unchanged at 80%");
        api->set_param(inst, "loop1_volume", "2");
        api->get_param(inst, "loop1_volume", buf, sizeof(buf));
        check(strcmp(buf, "2.000") == 0, "test32: a fader reaches 200%");
        api->set_param(inst, "loop1_volume", "5");
        api->get_param(inst, "loop1_volume", buf, sizeof(buf));
        check(strcmp(buf, "2.000") == 0, "test32: and clamps there");
        api->destroy_instance(inst);
    }

    /* A pass line that prints unconditionally is how a red suite reads
     * green. */
    if (g_failures) {
        fprintf(stderr, "SUITE FAILED: %d assertion(s)\n", g_failures);
        return 1;
    }
    printf("PASS: all wayward bench tests\n");
    return 0;
}
