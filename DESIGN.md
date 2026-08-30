# Wayward — design

Six loops, each keeping its own time.

Forgetful put one input into four tape memories that decay. Wayward is its
sibling on the other axis: six loops that do **not** decay, but that each run
at a slightly different tempo, so they slide against one another indefinitely.
You record six fragments from the incoming audio, tell each one what tempo its
loop should keep — 100, 101, 102, 103, 104, 105 — press PLAY, and the six
drift out of alignment and back over minutes.

That is the Reich / Eno / Basinski tape-loop mechanism: loops of unequal
length, started together, left to disagree. *Come Out*, *Discreet Music*, the
*Disintegration Loops*. Wayward's contribution is to make the disagreement a
knob.

The module does not time-stretch to fit a tempo. Each loop's recorded window
is **padded with silence** to reach its beat length, or **cut off** by it.
That honesty is the point: you hear a sound and then a rest, and the rests are
what make the phasing audible.

## The mechanism, in three numbers

Per loop, everything follows from three quantities:

| | |
|---|---|
| **window** | the slice of the recording that START/END select, `W` frames |
| **period** | `beats × 60/bpm × 44100` frames — how often the loop restarts, `P` |
| **phase** | a `double` in `[0,1)`, advanced by `1/P` each frame, wrapped at 1 |

Playback is then one line, with no buffer editing, no reallocation and no
copying:

```
read = phase * P                    /* frames into the current cycle */
out  = (read < W) ? buffer[start + read] : 0
```

Padding and truncation are the *same code*. If `P > W`, the counter runs off
the end of the window and emits silence until it wraps — that is the padding.
If `P < W`, it wraps before reaching the end of the window and the tail is
never heard — that is the truncation. Nothing is copied, nothing is allocated,
and the take on disk is never touched, so you can sweep the BPM knob all day
without damaging what you recorded.

**Phase is normalised (`0..1`), not an absolute frame count.** This matters
under the hand: turning BPM then keeps the loop at the same *proportional*
position in its cycle rather than jumping.

**The period is not a whole number of frames.** `4 × 60/101 × 44100` is
104,792.08. Hence `double`, both for the phase and for the increment; an
integer counter would leave each loop a fraction of a frame out per cycle and
drift in a way nobody asked for.

### Why whole-number tempi

Two loops at integer tempi `p` and `q`, with the same beat count, realign
after `p/gcd(p,q)` cycles of the first. Equivalently, and more usefully: **in
the time the first plays `p/g` cycles, the second plays `q/g`.**

| tempi | cancels to | realigns after | at 4 beats |
|---|---|---|---|
| 100 vs 101 | 100 : 101 | 100 cycles | 4 minutes |
| 100 vs 102 | 50 : 51 | 50 cycles | 2 minutes |
| 100 vs 105 | 20 : 21 | 20 cycles | 48 seconds |
| 100 vs 150 | 2 : 3 | 2 cycles | 4.8 seconds — a polyrhythm, not phasing |

So the *relationship between the numbers* is the composition, far more than
their size. `BPM`, `BASE` and `SPREAD` therefore snap to integers: 100 against
101 is a four-minute breath, while 100 against 100.37 is only a smear.

With all six running, every pair has its own realignment time and the full
ensemble coincides again only after a number of beats divisible by all six
tempi — for 100 through 105, about 1.8 million beats, which is a fortnight. In
practice they never all line up again, which is the intended behaviour.

## Control surface

Eight pages, one `ui_hierarchy` level each. Eight knobs per page, 4 across ×
2 rows. Labels are capped at 5 characters by the host's `LABEL_CHARS`; values
are effectively capped at 5–6 by the cell width.

### Main

```
PLAY   SYNC   BASE   SPRD
ENS    RSYN   DRY    ····
```

| key | type | |
|---|---|---|
| `master_play` | trigger | Zeroes all six phases at the same frame and runs. That shared moment is what the whole piece drifts away from. Reads `PLAY` / `STOP` — the button names what the *next* press will do. |
| `master_sync` | enum `FREE`/`MOVE` | `MOVE` also starts on the host transport and tracks `get_bpm()` as BASE. |
| `master_base` | float 40–200, step 1 | Reference tempo. |
| `master_spread` | float −12…+12, step 1 | Loop *i* runs at `base + i × spread`. **0 locks all six in unison**; the return to 0 is the piece's largest gesture. Writes the six per-loop tempi, which stay individually editable afterwards. |
| `master_ens` | enum, `access:"read"` | Six characters, one per loop: `.` empty, `I` has a take, `O` playing, `R` recording. The glyphs are constrained by the renderer, not chosen for looks — see below. |
| `master_resync` | trigger | Re-zeroes every phase *without* stopping — pull the ensemble back into unison mid-performance. |
| `master_dry` | float 0–1, `unit:"%"` | The live input's level. Defaults to full: an effect that silences its input is a bug. |
| `""` | | A load-bearing blank cell. |

### What the ENS readout may contain

The enum square puts every value through `enumSquareLines()` in schwung's
`shared/param_pages/font5x3.mjs`, which imposes three rules that between them
choose the glyphs:

1. `-`, `_` and space are **word separators** — `-` whenever it falls between
   two alphanumerics — and the function then keeps only the first three
   characters of each of the first two words. The design originally used `-`
   for an empty loop, which meant a mixed state like `OR-O..` would render as
   `OR` over `O`: characters dropped, and every position after the break
   shifted, so the readout no longer says *which* loop is which. An all-empty
   `------` survives by accident, since no hyphen there has an alphanumeric on
   either side — which is exactly how the bug would have looked correct until
   the first take was recorded.
2. The value is uppercased, so a glyph cannot be told apart from its uppercase
   twin.
3. An all-digit value takes a different path entirely.

Hence `.`, `I`, `O`, `R`: no separator character, no case-only distinction,
never all digits. If six characters do not fit the interior on one line, the
renderer falls back to a blind 3+3 slice, giving loops 1–3 over loops 4–6 with
every position intact — a legible second-best rather than a wrong reading.

**Page count is settled:** `drawBankBar` (`render_page_movy.mjs:902`) handles
any `pageCount` up to the 128-pixel display width, and the planner imposes no
limit on levels, so eight sections is fine. Mix stays its own page.

### Mix

```
1      2      3      4
5      6      OUT    PAN
```

`loop{1..6}_volume` — the `_volume` suffix is what makes the host render a
fader rather than a dial — plus `master_out` and `master_pan`, a single
control fanning the six loops across the stereo field. Spread is most of why
this kind of piece sounds spacious rather than cluttered.

### Loop 1 … Loop 6

```
REC    TRIG   START  END
BPM    BEAT   FIT    PHAS
```

Top row is the take: capture it, hear it, bound it. Bottom row is the time it
keeps: how fast, how long, how the window meets the period, where in the cycle
it sits.

| key | type | |
|---|---|---|
| `loopN_record` | trigger | Records into *this* loop, so no routing parameter is needed (Forgetful needs one because its REC is global). Reads `REC` / `STOP`. |
| `loopN_trig` | trigger | One-shot audition of the window, ignoring the period. |
| `loopN_start` / `loopN_end` | float 0–1, `unit:"%"` | Window bounds. Ordering and the minimum span are resolved **per block, not in `set_param`**, so a knob never snaps back under the hand. END below START collapses to a minimum-length loop *at* START rather than swapping the two. |
| `loopN_bpm` | float 40–200, step 1 | This loop's tempo. |
| `loopN_beats` | float 1–16, step 1 | `period = beats × 60/bpm`. Default 4 — at 100 BPM a 2.4 s loop, long enough to hear as a phrase rather than a flutter. |
| `loopN_fit` | enum `PAD`/`SPEED`/`RPT` | Below. |
| `loopN_phase` | float −0.5…+0.5 | Offsets this loop within its own cycle: slide it against the others by hand instead of waiting for it to drift. Slewed ~40 ms so it moves rather than clicks. |

**BEAT is a float, not an enum**, though the design began with an enum of
`1,2,3,4,6,8,12,16`. The host learns from `get_param` whether to send enum
labels or enum indices, so an enum whose labels are bare numbers is genuinely
ambiguous on the wire — `"2"` is both the label `2` and the index 2. Free
integers also turned out to be more musical: 7 beats against 8 phases
beautifully, and the restricted set would have forbidden it.

### FIT modes

All three are the same counter, differing only in how `read` maps into the
window:

```
PAD    read = phase*P, silent past W   [####......................]
SPEED  read = phase*W                  [########################..]
RPT    read = fmod(phase*P, W)         [####][####][####][####][##]
```

`SPEED` is varispeed: the window plays at rate `W/P` so it exactly fills the
period, with no gap and a shifted pitch. It is the most Forgetful-like of the
three, and costs one line beyond the interpolation the module already needs.

## Implementation notes

**Everything is one C11 file**, `src/dsp/wayward.c`, plus `src/module.json`
and the two host headers vendored verbatim from Forgetful. The release tarball
contains exactly the `.so` and `module.json`.

**Memory.** Six buffers of 30 s stereo `int16`: `6 × 30 × 44100 × 4 B =
31.75 MB`, against Forgetful's 42.3 MB. `calloc`'d once in `create_instance`
and never touched again. Thirty seconds is far more headroom than a window
needs; the cap exists so that forgetting to press REC a second time cannot
record forever.

**There is no control thread.** `create_instance`, `destroy_instance`,
`set_param`, `get_param` and `process_block` all run on the SPI audio callback
at SCHED_FIFO 90, pinned to core 3, with roughly 900 µs per 128-frame block.
No malloc outside `create_instance`, no file I/O, no locks, no logging — not
even `fprintf(stderr, …)`, which is an unbuffered `write()` syscall.
`host/plugin_api_v1.h:8–70` states this in full.

**Per-block prepass, per-frame inner loop.** Every division and `powf` —
period, window bounds, fit rate — is hoisted to once per block per loop. The
inner loop should be counter arithmetic, one interpolated read and a multiply.

**Declicking.** Window edges move under the knob, so the material at a
boundary is arbitrary. A ~4 ms fade at both window edges, and in `RPT` a ~6 ms
crossfade at the repeat seam. Without these, every loop ticks.

### The three things this wire gets wrong if you let it

1. **`get_param` has three answers, not two:** text, `""` (served, nothing
   there), and `-1` (the read did not complete). The host probes
   `preset_name`, `is_loading` and `display_name` on every repaint, and `-1`
   is retried forever — Forgetful logged 19,913 giveup events, roughly one a
   second, before it learned this. Every unrecognised key goes through
   `param_absent()`.
2. **Enum labels must be matched before any `atoi` fallback**, because the
   host writes back in whichever format `get_param` last produced.
3. **`"unit":"%"` multiplies by 100 for display.** Declare `0..1`.

**State is deliberately not implemented:** `set_param("state", …)` is a no-op
and `get_param("state", …)` returns `"{}"` — an empty object, not `-1`. This
forfeits User Presets and patch capture, and avoids serialising 31 MB of
audio. Same trade Forgetful makes.

## Build order

1. **Skeleton** — passes audio through unchanged, answers `chain_params` and
   `ui_hierarchy`, holds all parameter state and the record state machine.
   Gets all eight pages onto the device before any DSP exists. ← *done*
2. Record and window playback on one loop: REC, START, END, TRIG.
3. The period counter: BPM, BEAT, PAD fit, global PLAY.
4. Six loops, the SPREAD macro, RSYN, the ENS readout.
5. SPEED and RPT fit modes, PHASE, the Mix page.
6. `SYNC MOVE` against the host transport — last, because it is the only part
   that depends on anything outside the module.


## Testing

`tests/run.sh` builds natively with `-Wall -Wextra -Werror` and drives the
module strictly black-box through the v2 API, exactly as `chain_host` would.
Current coverage: the `chain_params` and `ui_hierarchy` contracts (entry
count, buffer headroom, every key on exactly one page), unknown keys never
returning `-1`, bit-exact passthrough, the SPREAD macro, enum wire format in
both directions, the record state machine, transport, and parameter clamping.

Arriving with the DSP:

- **Period arithmetic** — at 100 BPM × 4 beats a loop wraps every 105,840
  frames ±1. Asserted on the audio: feed a click, count frames between output
  clicks.
- **The phasing property, directly** — two loops at 100 and 101 BPM, a click
  in each, run for 240 s; the clicks must coincide at the start, be maximally
  offset at 120 s, and coincide again at 240 s. This is the module's whole
  reason to exist and it is cheap to assert.
- **Pad vs trim** — `P > W` must give exact silence in `[W, P)`; `P < W` must
  never emit a frame past `start + P`.
- **Fit modes** — `SPEED` has no silent region and its measured pitch scales
  as `W/P`; `RPT` repeats at `W`.
- **Click-freeness** across every window edge, repeat seam and PHASE sweep.

The suite returns non-zero on any failure. A PASS line that prints
unconditionally is how a red suite reads green.
