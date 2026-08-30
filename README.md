# Wayward

Six loops, each keeping its own time.

Record six fragments of whatever is coming in, tell each one what tempo its
loop should keep — 100, 101, 102, 103, 104, 105 — and press play. They start
together and then disagree, sliding out of alignment and back over minutes.
Nothing is stretched to fit: a loop that is shorter than its beat is padded
with silence, and one that is longer is simply cut off. The rests are what
make the drift audible.

The idea is old — Ligeti's hundred metronomes, Reich's two tape machines,
Riley's *In C*, Nancarrow's tempo canons, Eno's unequal tape loops. Wayward's
contribution is to put the disagreement on a knob. The manual has
[the whole lineage](https://kliegsablaze.github.io/wayward/#lineage).

A [Schwung](https://github.com/charlesvestal/schwung) `audio_fx` module for
Ableton Move, and a sibling to
[Forgetful](https://github.com/kliegsablaze/forgetful) — which took one input
into four memories that decay. This one takes six that don't, and lets them
fall out of step instead.

**[Read the manual →](https://kliegsablaze.github.io/wayward/)** — every
control on all five pages, walked through on interactive reconstructions of
the Move's own display.

See [DESIGN.md](DESIGN.md) for the interaction model, the phasing maths, and
the DSP architecture.

## Installing

Wayward is in the Schwung module catalog, so **Schwung Manager installs and
updates it for you**. Open `http://move.local:7700`, find Wayward in the
Module Store and install it; new releases show up there within a few minutes
of being tagged.

Needs **Schwung 1.0.0 or newer** — not 0.12.x. The Loop page is a *child
level*, six loops sharing one page with a selector, and the host machinery
that makes the selector work (`syncChildIndexFromModule`, and the cache-skip
that stops it going dead after one move) first shipped in 1.0.0.

### Installing by hand

For development, or on a Move with no network. Download
**[wayward-module.tar.gz](https://github.com/kliegsablaze/wayward/releases/latest/download/wayward-module.tar.gz)**
([all releases](https://github.com/kliegsablaze/wayward/releases)) and unpack
it into the `audio_fx` module directory:

```bash
scp wayward-module.tar.gz ableton@move.local:/data/UserData/
ssh ableton@move.local 'set -e
  D=/data/UserData/schwung/modules/audio_fx/wayward
  mkdir -p "$D" /data/UserData/.wy-stage
  tar xzf /data/UserData/wayward-module.tar.gz -C /data/UserData/.wy-stage
  mv -f /data/UserData/.wy-stage/wayward/wayward.so "$D/wayward.so"
  mv -f /data/UserData/.wy-stage/wayward/module.json "$D/module.json"
  rm -rf /data/UserData/.wy-stage /data/UserData/wayward-module.tar.gz'
```

It unpacks to a staging directory and *moves* the files into place rather than
extracting over them. That matters if you are replacing an install currently
loaded in a slot: writing over a `.so` that a running process has mapped
truncates the file under the live mapping, and the next page fault takes
Move's audio process down with it. A `mv` within `/data` is an atomic rename,
so the running instance keeps its old mapping and carries on until you reload.

## Building

Requires Docker (for ARM64 cross-compilation) or a native aarch64 toolchain.

```bash
./scripts/build.sh
```

Produces `dist/wayward-module.tar.gz`, containing exactly `wayward.so` and
`module.json`.

To build natively on an aarch64 host instead of cross-compiling:

```bash
CROSS_PREFIX= ./scripts/build.sh
```

## Testing

```bash
bash tests/run.sh
```

A black-box bench test driving the module's public v2 plugin API
(`create_instance` / `set_param` / `get_param` / `process_block`) exactly as
Schwung's chain host would, built with `-Wall -Wextra -Werror`.

## Development

Two tracks, deliberately separate. Nothing on the first one reaches anybody
else.

### Local: your Move

```bash
MOVE_HOST=ableton@move.local ./scripts/install.sh
```

Builds if `dist/` is behind `src/`, then copies `wayward.so` + `module.json`
to a connected Move over SSH — staging and atomically renaming, so it is safe
to run against a slot that is currently loaded. Writing over a `.so` that a
running process has mapped truncates the file under the live mapping, and the
next page fault takes Move's audio process down with it; a `mv` within `/data`
is an atomic rename, so the running instance keeps its old mapping and carries
on with the old code until you reload.

Bump the patch version in `src/module.json` as you go, so the number the
Manager shows is the build actually running.

### Release: everybody else

```bash
./scripts/release.sh
```

Refuses to run on a dirty tree or a failing suite, tags the version in
`src/module.json`, pushes the branch **before** the tag, and lets GitHub
Actions cross-compile, attach the tarball and update `release.json` on `main`.

Run it when you decide to ship — never as a side effect of anything else.
