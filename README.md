# Wayward

Six loops, each keeping its own time.

Record six fragments of whatever is coming in, tell each one what tempo its
loop should keep — 100, 101, 102, 103, 104, 105 — and press play. They start
together and then disagree, sliding out of alignment and back over minutes.
Nothing is stretched to fit: a loop that is shorter than its beat is padded
with silence, and one that is longer is simply cut off. The rests are what
make the drift audible.

A [Schwung](https://github.com/charlesvestal/schwung) `audio_fx` module for
Ableton Move, and a sibling to
[Forgetful](https://github.com/kliegsablaze/forgetful) — which took one input
into four memories that decay. This one takes six that don't, and lets them
fall out of step instead.

See [DESIGN.md](DESIGN.md) for the interaction model, the phasing maths, and
the DSP architecture.

> **Status: feature complete, unreleased.** Everything in the design is built
> and tested — recording, windowing, the period counter, all six loops, the fit
> modes and the mixer. It has not been through much playing yet, and it is not
> in the Module Store.

## Installing

Not yet released. Build it yourself, or deploy to a Move with
`scripts/install.sh` (below).

Will need **Schwung 0.12.1 or newer**.

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
