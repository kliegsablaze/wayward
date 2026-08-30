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
| **position** | a `double` counting frames into the current cycle, wrapped at `P` |

Playback is then one line, with no buffer editing, no reallocation and no
copying:

```
pos += 1                            /* frames into the current cycle */
if pos >= P: pos -= P               /* the loop restarting */
out = (pos < W) ? buffer[start + pos] : 0
```

Padding and truncation are the *same code*. If `P > W`, the counter runs off
the end of the window and emits silence until it wraps — that is the padding.
If `P < W`, it wraps before reaching the end of the window and the tail is
never heard — that is the truncation. Nothing is copied, nothing is allocated,
and the take on disk is never touched, so you can sweep the BPM knob all day
without damaging what you recorded.

**The position counts frames; it is NOT a normalised 0..1 phase.** This was
the other way round first, on the reasoning that a normalised phase keeps the
loop at the same *proportional* point in its cycle when the tempo changes. It
does — and that is the wrong continuity to preserve. With a normalised phase
the read position is `phase × P`, so turning BPM **scales** it and teleports
the playhead through the take; in PAD mode far enough to jump from inside the
window to outside it, cutting a note dead. Counting frames leaves the playhead
exactly where it is when the period changes, and moves only the point at which
it wraps. `test18` sweeps BPM under the hand and measures the largest step in
the output; it fails outright on the normalised version.

**The period is not a whole number of frames.** `4 × 60/101 × 44100` is
104,792.08. Hence a `double` position; an integer counter would leave each
loop a fraction of a frame out per cycle and drift in a way nobody asked for.
It also means a one-frame impulse is read at a fractional index and
interpolated with whatever sits beside it — which is why the period tests use
a 32-frame burst, having first been written with a single frame that vanished
on about half the cycles.

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
PLAY   RSYN   CLEAR  STATE
BASE   SPRD   WIDEN  SYNC
```

Top row is the transport, bottom row the tuning. PLAY and RSYN are the two
controls touched mid-performance, so they take the leftmost cells — the ones a
hand finds without looking — with STATE at the end of the same row reporting
on what they just did. The bottom row is set once and left: BASE beside SPRD
because they are read together, then WIDEN, then SYNC, the least-touched
control on the page and the furthest from the hand.

| key | type | |
|---|---|---|
| `master_play` | trigger | Zeroes all six phases at the same frame and runs. That shared moment is what the whole piece drifts away from. Reads `PLAY` / `STOP` — the button names what the *next* press will do, though **only in the header while the knob is held**: see the note below. |
| `master_sync` | enum `FREE`/`MOVE` | `FREE` ignores the outside world entirely. `MOVE` starts and stops with the host transport and takes its tempo as BASE — see below. |
| `master_base` | float 40–200, step 1 | Reference tempo. |
| `master_spread` | float −12…+12, step 1 | Loop *i* runs at `base + i × spread`. **0 locks all six in unison**; the return to 0 is the piece's largest gesture. Writes the six per-loop tempi, which stay individually editable afterwards. |
| `master_state` | enum, `access:"read"` | Six characters, one per loop: `.` empty, `S` has a take but stopped, `P` playing, `O` overdubbing, `R` recording. This is where transport state actually lives — the glyphs are constrained by the renderer, not chosen for looks; see below. |
| `master_resync` | trigger | Re-zeroes every phase *without* stopping — pull the ensemble back into unison mid-performance. |
| `master_widen` | float 0–1, `unit:"%"` | A stereo *spread* control, not a position control. At 0 all six loops sit centred; turning it up fans them progressively across the stereo field. Six loops phasing in mono turn to mud; the same six spread across the image read as voices you can follow individually. Lives on Main because it shapes the ensemble, not the balance. |
| `master_clear` | trigger | Erases every take and returns every setting to its default, over a 15-second fade. Reads `CLR`, then `KEEP` once running. See below. |

### CLEAR

Erases all six takes and resets every setting — but not at once. It fades the
ensemble over **15 seconds** and wipes at the end.

**The delay is the safety.** CLEAR sits between two controls that get pressed
constantly and it throws away everything on the module. A confirmation dialog
would need a screen this module does not have; a long audible fade needs
nothing, is impossible to miss, and a second press calls it off — the button
reads `KEEP` while a clear is in flight. It is also the right musical shape,
since the last gesture of a piece is usually to let it go quiet.

**The fade applies to the loops only, not to DRY.** Ducking someone's live
playing for fifteen seconds to announce that their loops are going away would
be taking the wrong thing.

**STATE displaces its glyphs for the countdown** — `CLR` and the whole seconds
remaining, with no separator, so the renderer either fits it on one line or
splits it as `CLR` over the number. For those fifteen seconds the countdown is
the only thing that cell could usefully say.

**The audio buffers are not zeroed.** Wiping 31.7 MB would take far longer
than the 900 µs the block has, and buys nothing: a take with no recorded
length cannot be reached, and the next recording overwrites from the
beginning. The wipe also clears each loop's resolved `span`, because the
prepass ran at the top of the block against takes that still existed —
without that, the rest of the block would read through a window with nothing
behind it.

### Why the transport state is in STATE and not on the button

`drawButton` (`render_page_movy.mjs:1344`) draws the button graphic and
nothing else, and the renderer says why in its own comment: *"a trigger has no
state, so nothing else on the screen changes when you click it."* A write-only
parameter's cell therefore shows the widget plus its **static label**; whatever
`get_param` returns for it appears only in the header, and only while the knob
is held.

So `PLAY` on the grid reads `PLAY` whether the ensemble is running or not, and
`REC` reads `REC` whether or not that loop is recording. Both buttons *do*
carry a full state vocabulary — that vocabulary is simply for the header.

Making PLAY an ordinary two-option enum would put its state permanently on
screen at the cost of the button and its press flash. Rejected: STATE is one
cell away and reports all six loops at once, which is more than a transport
readout would say. The known gap is that an ensemble with nothing recorded
reads `......` whether running or stopped.

### What the STATE readout may contain

The enum square puts every value through `enumSquareLines()` in schwung's
`shared/param_pages/font5x3.mjs`, which imposes three rules that between them
choose the glyphs:

1. `-`, `_` and space are **word separators** — `-` whenever it falls between
   two alphanumerics — and the function then keeps only the first three
   characters of each of the first two words. An ensemble reading `R-S---` is
   therefore rewritten to `R S---`, split, and drawn as `R` over `S--`: not a
   truncated ensemble but a **wrong** one, since characters vanish and every
   position after the break shifts, so the readout stops saying which loop is
   which. An all-empty `------` survives by accident, no hyphen there having
   an alphanumeric on either side — which is exactly how this would have
   looked correct until the first take was recorded.
2. The value is uppercased, so a glyph cannot be told apart from its uppercase
   twin.
3. An all-digit value takes a different path entirely.

Hence `.`, `S`, `P`, `O`, `R`: no separator character, no case-only
distinction, never all digits. `-` was the wanted glyph for an empty loop and
is exactly the one that cannot be used. If six characters do not fit the interior on one line, the
renderer falls back to a blind 3+3 slice, giving loops 1–3 over loops 4–6 with
every position intact — a legible second-best rather than a wrong reading.

**Page count is settled:** `drawBankBar` (`render_page_movy.mjs:902`) handles
any `pageCount` up to the 128-pixel display width, and the planner imposes no
limit on levels, so eight sections is fine. Mix stays its own page.

### Mix

```
1      2      3      4
5      6      DRY    OUT
```

`loop{1..6}_volume` — the `_volume` suffix is what makes the host render a
fader rather than a dial — then `master_dry` and `master_out`.

`master_dry` is the live input's level, defaulting to full: an effect that
silences its input is a bug. It is **not** a dry/wet crossfade, since the loops
carry their own levels on this same page. It sits here, beside the six faders
and immediately before OUT, because it is one more thing being balanced.

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
| `loopN_record` | trigger | Three destinations from one button, exactly as in Forgetful: an idle loop starts recording, a recording loop closes the take, and a loaded loop toggles **overdub**, layering fresh input onto what is there pass after pass without stopping. Records into *this* loop, so no routing parameter is needed (Forgetful needs one because its REC is global). Reads `REC` → `STOP` → `DUB` → `PLAY`. |
| `loopN_trig` | trigger | One-shot audition of the window, ignoring the period. |
| `loopN_start` | float 0–1, `unit:"%"` | Where the window begins in the take. |
| `loopN_end` | float −1…+1, `unit:"%"` | **A bipolar length measured from START, not a second position.** Centre is a zero-length window; the top half grows the window forward from START until it reaches the end of the take; the bottom half grows it *backward* from START, played in **reverse**, reaching the head of the take at the extreme. The two halves are mirror images about a silent centre, and the reverse half reaches material the forward half never can. Because END is a length, there is no ordering to enforce and no way to set an inside-out window. The span is still resolved **per block, not in `set_param`**, so a knob never snaps back under the hand. |
| `loopN_bpm` | float 40–200, step 1 | This loop's tempo. |
| `loopN_beats` | float 1–16, step 1 | `period = beats × 60/bpm`. Default 4 — at 100 BPM a 2.4 s loop, long enough to hear as a phrase rather than a flutter. |
| `loopN_fit` | enum `PAD`/`SPEED`/`RPT` | Below. |
| `loopN_phase` | float −0.5…+0.5 | Offsets this loop within its own cycle: slide it against the others by hand instead of waiting for it to drift. **Rate-limited per frame**, so the loop runs up to 25% fast or slow until it arrives rather than jumping — see below. |

### PHASE slides; it does not jump

PHASE offsets by a fraction of a **cycle**, so its effect on the read position
is multiplied by the period. One detent is 0.005 of a cycle — at 100 BPM × 4
beats, **529 frames**. Applying that as a position, even smoothed between
blocks, teleports the playhead into unrelated audio, and a knob sweep does it
several hundred times a second. It was built that way first, with a per-block
chase, and it sounded like it.

The offset is therefore rate-limited per **frame**, and the limit is expressed
as a speed deviation: the loop runs up to 25% fast or slow until it arrives.
Nothing is ever spliced, and the sound it makes is the correct one — running a
loop fast to pull it forward is exactly what a performer does to tape by hand,
and what Reich's players do to pull *Piano Phase* apart. A detent lands in
about 50 ms; a full half-cycle sweep takes a few seconds and audibly slides
the loop into place.

`test28` measures the second difference of the output against a triangle take,
which has no curvature anywhere except its apex, so any jump in the playhead
shows as a spike. It was first written against a *ramp* in a faded mode and
failed for the wrong reason: the fade-out corner at the window's end is itself
a slope change of ~91 per frame, which the metric flagged whether or not PHASE
moved at all.

### SYNC MOVE

The only part of the module that looks outside itself, via `get_bpm()` and
`get_clock_status()` (`plugin_api_v1.h:146,156`). Both are documented as
nullable on older hosts and every use is guarded.

**The transport is followed on its edges, not its level.** A running transport
that re-triggered on every block would re-zero all six phases 344 times a
second, which would make the piece incapable of drifting at all — the one
thing it exists to do. Only a transition into `RUNNING` starts the ensemble
and zeroes the phases; only `RUNNING → STOPPED` stops it.

**`UNAVAILABLE` is not `STOPPED`.** It means no clock is configured, so it
must never silence an ensemble that is already running.

**BASE is re-applied only when the host tempo actually changes.** Re-fanning
the six on every block would overwrite any loop whose BPM you set by hand,
making the loop pages effectively read-only in MOVE. Following a tempo should
not mean that.

**Switching into MOVE adopts the host's current state at once** rather than
waiting for the next transport edge, which might not come for a whole song.

### Leading silence is trimmed on close

A take almost always opens with the moment between pressing REC and actually
playing something, and every frame of it shifts the window — so without this,
START would have to be dialled in by hand on every single recording. The take
therefore begins at its **first audible frame**.

Three decisions inside that:

- **It is an offset, not a move.** `origin` marks where the take starts and
  every window position is relative to it. Trimming by `memmove` would shift
  up to 5 MB on the audio thread, several times the 900 µs block budget on its
  own.
- **The onset is found while recording**, one comparison per frame, not by
  scanning at close. A 30 second take is 1.3 million frames; scanning it would
  blow the budget in a single call.
- **A 5 ms pre-roll is kept.** The threshold (about −60 dBFS, under any room
  tone worth keeping and over a converter's noise floor) is crossed partway
  *up* an attack, not at the start of one. Trimming flush to the crossing
  shaves the leading edge off every transient.

A recording that never crosses the threshold is discarded rather than kept: a
loop that reads as loaded and plays nothing is worse than no loop at all.

**Trailing silence is left alone.** It arguably matters more — the take's
length is what END at +1 spans, so a late stop pads every cycle — but a
trailing trim has to decide where a decay ends rather than where an attack
begins, which is a musical judgement rather than a threshold. Left for now.

**BEAT is a float, not an enum**, though the design began with an enum of
`1,2,3,4,6,8,12,16`. The host learns from `get_param` whether to send enum
labels or enum indices, so an enum whose labels are bare numbers is genuinely
ambiguous on the wire — `"2"` is both the label `2` and the index 2. Free
integers also turned out to be more musical: 7 beats against 8 phases
beautifully, and the restricted set would have forbidden it.

### FIT modes

Three ways to reconcile window and period, each immediately followed by its
faded companion — six modes, read as three pairs:

```
PAD  PADF    read = phase*P, silent past W   [####......................]
SPD  SPDF    read = phase*W                  [########################..]
RPT  RPTF    read = fmod(phase*P, W)         [####][####][####][####][##]
```

They are the same counter; only the mapping from `phase` into the window
differs. `SPD` is varispeed: the window plays at rate `W/P` so it exactly fills
the period, with no gap and a shifted pitch — the most Forgetful-like of the
three, and one line beyond the interpolation the module already needs.

The **`F` companions apply a short volume envelope to the tail of the window**,
taking it to zero so the seam cannot click. The raw modes keep the click on
purpose: a hard splice is a percussive event and tape music is full of them.
This is why declicking is a mode here rather than the always-on ~4 ms fade the
design originally specified.

`SPEED` was shortened to `SPD` so that its companion stays inside the
5-character label cap.

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

1. ~~Skeleton — the control surface, all eight pages, no DSP.~~ **done**
2. ~~Record and window playback: REC, START, END, TRIG.~~ **done**
3. ~~The period counter: BPM, BEAT, PAD fit, global PLAY.~~ **done**
4. ~~Six loops, the SPREAD macro, RSYN, the STATE readout.~~ **done**
5. ~~SPD and RPT fit modes and their faded companions, PHASE, the Mix
   page.~~ **done**
6. ~~`SYNC MOVE` against the host transport.~~ **done**

The build order is complete.

Also unbuilt, in rough order of how much they would add: a per-loop readout of
where in its cycle each loop currently sits (there is no free cell on a loop
page for it), and the realignment countdown the phasing maths makes
computable.


## Testing

`tests/run.sh` builds natively with `-Wall -Wextra -Werror` and drives the
module strictly black-box through the v2 API, exactly as `chain_host` would.
Current coverage: the `chain_params` and `ui_hierarchy` contracts (entry
count, buffer headroom, every key on exactly one page), unknown keys never
returning `-1`, bit-exact passthrough, the SPREAD macro, enum wire format in
both directions, the record state machine, transport, and parameter clamping.

Now also covering the engine, all asserted on the output audio rather than on
internals:

- **Period arithmetic** — a take whose only content is a 32-frame burst fires
  once per period, so counting bursts measures the period directly. 100 BPM ×
  4 beats gives 101 bursts in 240 s.
- **The phasing property** — the same take at 101 BPM completes exactly one
  more cycle over the same 240 s than at 100. The module's whole reason to
  exist, asserted in one number.
- **Pad and trim** — with `P > W`, everything between the end of the window
  and the wrap is *exact* silence; with `P < W`, a ramp never reaches beyond
  where the wrap should cut it.
- **SPD and RPT** leave no gap, unlike PAD.
- **Reverse** — START at the end and END fully negative turns a rising ramp
  into a falling one.
- **The F companions** — a constant take makes every window edge a full-scale
  step. The raw mode's largest step exceeds 8000; the companion's is under 500.
- **BPM swept under the hand** produces no jump in the output. This is the
  test that caught the normalised-phase error described above.

The suite returns non-zero on any failure. A PASS line that prints
unconditionally is how a red suite reads green.
