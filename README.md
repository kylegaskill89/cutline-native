# Cutline (native)

A Premiere-style non-linear video editor for Windows, written in C++.

This is a ground-up native rewrite of [Cutline](https://github.com/kylegaskill89/cutline),
which was a Tauri application (Rust shell + TypeScript UI + bundled FFmpeg
binaries). The TypeScript version worked, but frame-accurate export was
unusably slow: rendering exact frames meant seeking the source to each output
time, and a browser `<video>` element re-decodes from the nearest keyframe on
every seek. On a 4K long-GOP MKV that measured roughly **4.5 seconds per frame
— about 18 minutes for an 8-second clip.**

The native version replaces that with sequential hardware decode and a GPU
compositor, so export renders real frames at real speed and preview and export
agree by construction rather than by careful maintenance.

## Design

- [`docs/architecture.md`](docs/architecture.md) — the native architecture and
  the decisions behind it.
- [`docs/spec.md`](docs/spec.md) — the product specification: data model,
  algorithms, effect semantics, and UI. Carried over from the TypeScript
  version, which remains the reference for exact numeric behaviour.

## Status

Phase 1 of 8 is essentially complete: the data model, editing operations,
segment resolution, animation, effect stacks, versioned persistence, and
undo/redo — all pure, with 210 tests. One refinement is outstanding
(`move_clips_layered`, which reassigns audio lanes when a video clip changes
compositing layer).

Nothing is runnable yet. Phase 2 is media I/O, and it opens with the benchmark
that decides whether the premise of this rewrite holds.

## Building

Requires Visual Studio 2022 with the C++ toolset, CMake 3.28+, and
[vcpkg](https://github.com/microsoft/vcpkg) with `VCPKG_ROOT` set.

```
cmake --preset default
cmake --build --preset debug
ctest --preset debug
```

Heavier dependencies are opt-in vcpkg features so the core builds fast:
`media` pulls in FFmpeg, `gpu` pulls in Skia.

## Licence

GPL-3.0-or-later. Cutline links x264 and x265 for software H.264/H.265 encoding
alongside the hardware encoders, and both are GPL, so the application is GPL
too. See [LICENSE](LICENSE).
