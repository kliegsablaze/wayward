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

    /* A pass line that prints unconditionally is how a red suite reads
     * green. */
    if (g_failures) {
        fprintf(stderr, "SUITE FAILED: %d assertion(s)\n", g_failures);
        return 1;
    }
    printf("PASS: all wayward bench tests\n");
    return 0;
}
