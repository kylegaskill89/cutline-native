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

**Phase 1 complete** — the whole pure core: data model, editing operations,
segment resolution, animation, effect stacks, versioned persistence, undo/redo.
213 tests.

**Phase 2 complete** — probing with colour metadata, sequential hardware decode,
audio decode and resampling, waveform peaks, and thumbnails. 239 tests.

The benchmark has settled the question this rewrite was built on. Decoding 4K60
HEVC in order costs **1.6 ms/frame**, about 10× realtime; the 8-second clip that
took roughly 18 minutes to export from the old app now decodes in **0.75 s**.
See `docs/architecture.md` for the full numbers and the one correction they
forced.

**Phase 3 complete, bar two items** — the Direct3D 12 device, the compositor,
and the on-screen presenter. Video is converted to linear light in a shader and
drawn into a 16-bit float target, which an `_SRGB` render target view encodes
for display.

**Phase 4 underway** — the compositor draws a stack of layers with per-layer
position, scale, rotation, opacity, all eight blend modes, and adjustment
layers, and can read the result back to system memory. The same code path serves
preview and export, which is the point: the old app composited with a canvas for
preview and an FFmpeg filtergraph for export, and keeping those two agreeing was
constant work. 362 tests.

Ten of the eleven registry effects run as shaders: brightness, contrast,
saturation, hue, black and white, invert, flip, crop, vignette, and the chroma
keyer. Gaussian blur is not implemented — a separable blur needs its own passes.

Two colour decisions worth knowing about, both in `docs/architecture.md`:

- **Blending is linear.** A 50% dissolve passes through the true midpoint rather
  than the too-dark one a gamma-encoded canvas produces, so old and new projects
  will not match pixel for pixel across a dissolve.
- **Effects are not.** They run on coded values, where FFmpeg's filters are
  defined and where the spec specifies them. Each operation happens in the space
  it was defined in.

Still to come: generated media, Gaussian blur, Skia sharing the device, and
keeping hardware-decoded frames on the GPU instead of uploading them from system
memory.

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

For the media layer, use the `media` preset instead — the first configure builds
FFmpeg from source and takes about twenty minutes:

```
cmake --preset media
cmake --build --preset media
build/media/tools/decode_bench/Release/decode_bench <file> [frames]
build/media/tools/preview_window/Release/preview_window <file>
```

`preview_window` is a throwaway viewport for driving the render pipeline by
hand; it is not the editor's UI and will not become it. Space plays, the arrow
keys step a frame, Shift jumps, Home returns to the start. Ctrl with the arrows
moves the layer, `+`/`-` scale it, `r` rotates, `o` halves opacity, `b` cycles
blend modes, and `0` resets.

Media tests need real footage, which is not in the repository. Point
`CUTLINE_TEST_MEDIA_DIR` at a directory containing `Boiler.mp4` to run them;
without it they skip rather than silently pass.

## Licence

GPL-3.0-or-later. Cutline links x264 and x265 for software H.264/H.265 encoding
alongside the hardware encoders, and both are GPL, so the application is GPL
too. See [LICENSE](LICENSE).
