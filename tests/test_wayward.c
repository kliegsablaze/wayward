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
#define TEST_PARAM_COUNT 63
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
              "test0: chain_params has exactly 63 entries — 9 master, 6 mixer\n"
              "       faders, and 8 on each of the six loop pages");

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

        const char *master[] = { "master_play", "master_sync", "master_base",
                                 "master_spread", "master_state", "master_resync",
                                 "master_dry", "master_out", "master_widen" };
        for (size_t i = 0; i < sizeof(master) / sizeof(master[0]); i++) {
            char probe[64];
            snprintf(probe, sizeof(probe), "\"%s\"", master[i]);
            check(strstr(ui, probe) != NULL,
                  "test1: every master key appears in ui_hierarchy");
        }

        const char *suffixes[] = { "record", "trig", "start", "end",
                                   "bpm", "beats", "fit", "phase", "volume" };
        for (int L = 1; L <= TEST_NUM_LOOPS; L++) {
            for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
                char probe[64];
                snprintf(probe, sizeof(probe), "\"loop%d_%s\"", L, suffixes[i]);
                check(strstr(ui, probe) != NULL,
                      "test1: every per-loop key appears in ui_hierarchy");
            }
        }

        /* Eight levels: root, mix, and one per loop. */
        check(count_occurrences(ui, "\"knobs\":") == 8,
              "test1: exactly eight pages are declared");
        check(count_occurrences(ui, "\"level\":") == 7,
              "test1: root carries seven nav entries — Mix and six loops");
        /* One deliberate blank cell on Main. */
        check(strstr(ui, "\"master_resync\",\"\",\"master_state\"") != NULL,
              "test1: Main's third cell is a load-bearing blank, holding the\n"
              "       momentary buttons apart from the readout");

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

        api->set_param(inst, "master_sync", "MOVE");
        api->get_param(inst, "master_sync", buf, sizeof(buf));
        check(strcmp(buf, "MOVE") == 0, "test6: SYNC accepts a label");

        api->destroy_instance(inst);
    }

    /* ---- test 7: the record state machine and the STATE readout ---------- */
    {
        void *inst = api->create_instance(NULL, NULL);

        api->get_param(inst, "master_state", buf, sizeof(buf));
        check(strcmp(buf, "......") == 0,
              "test7: STATE shows six empty loops at startup");

        /* A trigger fires on anything that is not the idle spelling. */
        api->set_param(inst, "loop3_record", "-");
        api->get_param(inst, "master_state", buf, sizeof(buf));
        check(strcmp(buf, "......") == 0,
              "test7: writing the idle spelling does not fire a trigger");

        api->set_param(inst, "loop3_record", "GO");
        api->get_param(inst, "master_state", buf, sizeof(buf));
        check(strcmp(buf, "..R...") == 0,
              "test7: REC on loop 3 shows R in the third position only");

        /* The button names what the NEXT press will do. */
        api->get_param(inst, "loop3_record", buf, sizeof(buf));
        check(strcmp(buf, "STOP") == 0,
              "test7: while recording, REC reads STOP");
        api->get_param(inst, "loop1_record", buf, sizeof(buf));
        check(strcmp(buf, "REC") == 0,
              "test7: an idle loop's button still reads REC");

        /* Stopping with nothing captured (the skeleton records no audio)
         * discards the take rather than keeping a click. This assertion
         * changes shape in step 2, when process_block starts filling the
         * buffer. */
        api->set_param(inst, "loop3_record", "GO");
        api->get_param(inst, "master_state", buf, sizeof(buf));
        check(strcmp(buf, "......") == 0,
              "test7: a take under the 50 ms minimum is discarded");

        /* The readout must never contain a character enumSquareLines() treats
         * as a word separator: one of those splits the value and shifts every
         * position after it, so the ensemble reads wrong rather than short. */
        api->set_param(inst, "loop2_record", "GO");
        api->get_param(inst, "master_state", buf, sizeof(buf));
        check(strcmp(buf, ".R....") == 0, "test7: STATE tracks a second loop");
        check(strpbrk(buf, "-_+ ") == NULL,
              "test7: STATE contains no character the enum-square renderer\n"
              "       would break the value on");

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
        api->get_param(inst, "master_state", buf, sizeof(buf));
        check(buf[0] == 'S',
              "test10: STATE shows loop 1 holding a take while stopped");

        /* Silence the other five so only loop 1 is measured. */
        for (int L = 2; L <= TEST_NUM_LOOPS; L++) {
            char key[32];
            snprintf(key, sizeof(key), "loop%d_volume", L);
            api->set_param(inst, key, "0");
        }
        api->set_param(inst, "master_play", "GO");
        api->get_param(inst, "master_state", buf, sizeof(buf));
        check(buf[0] == 'P', "test10: STATE shows loop 1 playing once started");

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

    /* A pass line that prints unconditionally is how a red suite reads
     * green. */
    if (g_failures) {
        fprintf(stderr, "SUITE FAILED: %d assertion(s)\n", g_failures);
        return 1;
    }
    printf("PASS: all wayward bench tests\n");
    return 0;
}
