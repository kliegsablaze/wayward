# Wayward — handoff

Six loops, each keeping its own time. Everything needed to pick this up cold:
how the engine works, the host contract's silent traps, the two deploy tracks,
and what was deliberately left undone.

| | |
|---|---|
| **Released** | v0.9.0 |
| **Status** | in the Schwung catalog |
| **Needs host** | Schwung 1.0.0 or newer |
| **Type** | `audio_fx`, chainable |
| **Repo** | https://github.com/kliegsablaze/wayward |
| **Manual** | https://kliegsablaze.github.io/wayward/ |
| **On device** | `/data/UserData/schwung/modules/audio_fx/wayward` |

> This file is the orientation layer. [DESIGN.md](DESIGN.md) is authoritative
> for mechanism and reasoning — where the two disagree, DESIGN.md is right and
> this one needs fixing.

## What it is

Record six fragments from the input. Each loops at its own tempo — 100, 101,
102, 103, 104, 105 — so they start on one instant and come apart, sliding
through every relationship they have before arriving back together minutes
later. Nothing is time-stretched: a window shorter than its beat is padded with
silence, one that is longer is cut off at the wrap.

The lineage is Ligeti's *Poème symphonique*, Reich's tape phasing, Riley's
*In C*, Nancarrow's tempo canons and Eno's unequal loops. Its sibling is
[Forgetful](https://github.com/kliegsablaze/forgetful), which took the other
axis: memories that decay rather than disagree.

## The engine, in one screen

One C11 file, `src/dsp/wayward.c` (~1,450 lines), plus two host headers
vendored from Forgetful. Each loop holds three numbers:

- **window** — the slice START and END cut from the take, `W` frames
- **period** — `beats × 60/bpm × 44100` frames, `P`
- **position** — a `double` counting frames into the cycle, wrapping at `P`

```
pos += 1
if pos >= P: pos -= P            /* the loop restarting */
out = (pos < W) ? buffer[start + pos] : 0
```

Padding and truncation are the same arithmetic, and neither touches the
recording — which is why sweeping BPM can never damage a take.

### Three decisions that are easy to get backwards

**The position counts frames; it is not a normalised phase.** A 0–1 phase keeps
a loop at the same *proportional* point when the tempo changes, which sounds
right and is the wrong continuity: the read position is `phase × P`, so turning
BPM scales it and teleports the playhead — far enough, in PAD, to cut a note
dead. `test18` fails outright on that version.

**Read at the current position, advance afterwards.** Advancing first costs the
window its first frame, so a transient sitting exactly on START is never heard.

**PHAS is rate-limited per frame, not chased per block.** It offsets by a
fraction of a *cycle*, so its effect on the read position is multiplied by the
period — one detent is 529 frames at 100 BPM × 4. Applied as a position, even
smoothed, it splices unrelated audio hundreds of times a second. It now runs the
loop up to 25% fast or slow until it arrives, which is also what a performer
does to tape by hand.

### Budget

44.1 kHz, 128-frame blocks, roughly 900 µs each, and **no control thread** —
`set_param`, `get_param` and `process_block` all run on the audio callback.
Nothing allocates outside `create_instance`, which claims 6 × 30 s stereo
`int16` = 31.7 MB up front. No file I/O, no locks, no logging, not even
`fprintf(stderr, …)`.

## Control surface — five pages, 70 parameters

The six loops occupy the same 3×2 block on every page that addresses all six,
so the hand learns one arrangement. The fourth column is always "the ensemble,
not a member of it".

| Page | Row 1 / Row 2 | Notes |
|---|---|---|
| Main | `REC 1  REC 2  REC 3  PLAY` / `REC 4  REC 5  REC 6  RSYN` | REC is three-way: record, close, overdub |
| Shape | `BASE  SPRD  WIDEN  ····` / `····  ····  ····  CLEAR` | SPRD is the instrument. CLEAR fades 15 s, cancellable |
| Loop | `LOOP  TRIG  START  END` / `BPM  BEAT  FIT  PHAS` | A host **child level** — one page for all six |
| Mix | `1  2  3  DRY` / `4  5  6  OUT` | Faders reach 200%, default 80% |
| Orbits | `1  2  3  ····` / `4  5  6  ALIGN` | Read-only. The only place recording is visible |

`""` in a `knobs[]` array is a real, load-bearing blank cell, not a missing
control. The Loop page declares generic keys (`trig`, `start`…) that the host
resolves onto `loop3_start` and so on through `child_key_template`; all 42
concrete keys stay declared and remain modulation targets.

## Traps that fail silently

Every one of these was hit during the build. None produce an error, a log line
or a visible failure, so each entry names the symptom as well as the rule.

**A float knob ignores its declared `step`.**
*Looks like:* a selector moves one detent and snaps back; several detents make
it stick.
*Rule:* a float's per-detent movement is a fixed fraction of its **range** —
`1% × 0.5`, or 0.025 for a 1–6 control. Declare discrete selectors
`"type":"int"`. That gives four detents per value, which is deliberate and not
adjustable from a declaration.

**`get_param` has three answers, not two.**
*Looks like:* nothing, until you check the logs and find thousands of retries a
minute. Forgetful logged 19,913.
*Rule:* text, `""` (served, nothing there), and `-1` (read did not complete).
`-1` is retried forever, and the host probes `preset_name`, `is_loading` and
`display_name` on every repaint. Unknown keys go through `param_absent()`.

**A trigger cannot show its own state.**
*Looks like:* PLAY reads `PLAY` whether running or not; the toggle appears
broken.
*Rule:* `drawButton` draws the widget and its **static label** only. A
write-only param's value reaches the header while held, never the grid. This is
the entire reason the Orbits page exists.

**Readout values are split on separators.**
*Looks like:* a status readout that is correct when empty and *wrong* once
anything is recorded — characters vanish and positions shift.
*Rule:* `enumSquareLines` treats `-` between two alphanumerics as a word break
and keeps three characters of each of the first two words. It also uppercases,
and sends all-digit values down a different path. Glyphs here are `. S P O R`;
`-` is the one that cannot be used.

**Enum labels that are bare numbers.**
*Looks like:* a control that mostly works, then jumps to the wrong value.
*Rule:* the host learns from `get_param` whether to send labels or indices, so
`"2"` is ambiguous. No enum here has a numeric label; BEAT is a float for this
reason.

**A child level rewrites *every* key on its page.**
*Looks like:* the selector cell renders as a dead knob addressing a parameter
that does not exist.
*Rule:* `child_key_overrides` maps `loop_select` to **itself** — an override
with no `{index}` or `{key}` resolves literally. Listing the index param
anywhere also suppresses the separate picker page the planner would generate.

**Help lines are drawn, never wrapped or truncated.**
*Looks like:* help that reads fine on screen with its tail invisibly missing. A
catalog sweep found 27 modules doing this, the worst by 100 px.
*Rule:* the budget is **pixels**, not characters — the font is proportional on
screen. ASCII only; an em dash renders as a bare gap. And the top level must
have `children`, or the file is discarded in silence. `tests/help_lint.py`
enforces all three.

**`scp` onto a live `.so` takes down audio.**
*Looks like:* Move's audio process dies on deploy. It took three crashes to
spot the pattern.
*Rule:* `scp` opens with `O_TRUNC` and the shim has the file `dlopen`'d.
`install.sh` uploads beside the target and `mv`s — atomic rename, new inode.
Never simplify that away.

## Workflows — two tracks, deliberately separate

### Local: your Move

```bash
MOVE_HOST=ableton@move.local ./scripts/install.sh
```

Cross-builds in Docker if `dist/` is behind `src/`, refuses a non-aarch64 ELF,
deploys by atomic rename, reloads schwung, then **verifies the mapped inode**
against the on-disk one. Exit 0 means verified running; 2 means the module is
not in a slot. Touches nothing outside your device. Bump the patch version
first, so the Move sits ahead of the catalog.

### Release: everyone else

```bash
./scripts/release.sh
```

Refuses a dirty tree or a failing suite, tags the version in `src/module.json`,
pushes `main` *before* the tag, and lets CI cross-compile, attach the tarball
and rewrite `release.json`. Expect to rebase first: the previous release's
workflow commits to `main`, so origin is routinely one commit ahead.

### Catalog

One entry in `module-catalog.json` upstream, already merged. It is served live
from `main`, so edits reach users on merge. `min_host_version` is **1.0.0** —
not 0.12.x, because `syncChildIndexFromModule` and the cache-skip that keeps
the loop selector alive both first ship in 1.0.0.

## Tests — 29 blocks, 120 assertions, all black-box

`bash tests/run.sh` lints `help.json`, then compiles the suite natively with
`-Wall -Wextra -Werror` and drives the module through the public v2 API exactly
as the chain host would. It never reaches into internals, and asserts on
**output audio** rather than state. The ones worth knowing:

- **The phasing property.** The same take at 101 BPM completes exactly one more
  cycle in 240 s than at 100. The module's whole reason to exist, in one integer.
- **Contract shape.** Exact parameter count, and that `chain_params` fits its
  buffer *with headroom* — `snprintf` truncates silently and a module that
  overflows simply stops having a UI.
- **PHAS sweep.** Second-difference of the output against a triangle take, which
  has no curvature but its apex, so a jumping playhead spikes.
- **Pad and trim.** `P > W` gives *exact* silence; `P < W` never reaches past
  the wrap.

Two test bugs worth not repeating, both documented in DESIGN.md: rounding a take
up to a whole block welds silence onto its end (a step no edge fade can reach),
and a one-frame impulse is interpolated away on roughly half the cycles because
periods are rarely whole numbers of frames.

## State of play

Distinguishing gaps from deliberate omissions matters: a feature removed on
purpose looks identical in a repo to one that was never built.

### Gaps

- **No LICENSE file.** Public and installable, so the default of "all rights
  reserved" sits awkwardly with a catalog listing. Forgetful has the same gap.
  MIT matches the ecosystem.
- **CPU never measured on device.** Six loops × 128 frames against ~900 µs.
  Almost certainly fine — Forgetful runs four loops plus an FDN reverb and a
  glitch sequencer in the same budget — but "almost certainly" is doing work,
  and the failure mode is dropouts.
- **Forgetful has no `help.json`.** Same machinery; the same lint would port
  over.

### By design

- **SYNC MOVE was built, then removed.** Wayward's loops keep their own time by
  definition; locking them to someone else's transport is at odds with the
  instrument. `get_bpm()` and `get_clock_status()` are the way back.
- **No state persistence.** `get_param("state")` returns `{}`, never `-1`. Six
  takes are a performance, not a preset — and the alternative is serialising
  31 MB of audio. Forfeits User Presets and chain patches.
- **Trailing silence is not trimmed.** Leading silence is, automatically. The
  tail needs a judgement about where a decay ends rather than a threshold, and
  the same cutoff that safely ignores room tone would halve a cymbal.
- **Raw FIT modes click.** `PAD`, `SPD` and `RPT` keep the splice; the `F`
  companions fade both ends. A hard splice is a percussive event.

### Unvalidated by ear

The default `SPREAD 1` at `BEAT 4` gives a four-minute arc, chosen from the
arithmetic rather than from listening. If it reads as too slow to follow or too
fast to sit inside, that default is a one-line change and worth making early —
habits form around it.

## Map

| Path | |
|---|---|
| `src/dsp/wayward.c` | Everything: DSP, params, pages, entry point |
| `src/module.json` | Manifest. Its `version` is the single source of truth |
| `src/help.json` | On-device help. Scanned at runtime; named nowhere in the manifest |
| `src/dsp/host/*.h` | Vendored from Forgetful, unmodified. Read `plugin_api_v1.h:8–70` first |
| `tests/run.sh` | Lint + native build + suite |
| `tests/help_lint.py` | Measures help lines the way the device draws them |
| `docs/index.html` | The manual. Interactive reconstruction of the Move's display |
| `DESIGN.md` | The long form: mechanism, maths, every decision and why |

The manual's display reconstruction is worth keeping in sync rather than
rewriting — it carries the hardware font transcribed as column bitmaps. It can
drift from the module silently; the check is to dump `ui_hierarchy` from a
native build and compare its knob arrays against the manual's cell lists, page
by page.
