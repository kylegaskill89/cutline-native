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

**Phase 6 (export) works** — `export_project` renders a project to an MP4 with
no window and no UI. The 8-second 4K clip that took roughly **18 minutes** in the
TypeScript version now exports in **4.0 seconds**, about twice realtime, via
NVENC. Adding audio costs around 1% of that. Frame counts and durations are
verified by round-trip tests rather than assumed.

(An earlier 7.7 s figure appears in the history. It was measured on a machine
that was busy compiling at the time; 4.0 s is what three consecutive runs give
when nothing else is competing for the GPU.)

Encoders are chosen at runtime — NVENC, QSV, AMF, then x264/x265 — so an export
always completes, just slower on a machine without a supported GPU.

**Phase 5 (audio) underway** — exports have sound, and it runs the same route as
picture: the pure layer decides what plays and how loud, the mixer joins that to
decoded samples, and both streams go into one writer so interleaving stays one
class's problem. Gain, volume automation, fades, mute, solo, per-clip effects
and retiming all reach the file.

The eight audio effects are our own DSP rather than two implementations kept in
step. The reference ran Web Audio in the preview and FFmpeg's filters on export,
and their defaults differ enough — a 3 ms compressor attack against 20 ms, a
30 dB knee against 9 dB — that a clip tuned by ear did not sound the same in the
file. Where they disagreed this follows FFmpeg, because that is what exports
were actually rendered with.

Retiming preserves pitch, by WSOLA rather than by resampling: a clip at 2× runs
twice as fast without turning a voice into a chipmunk. Tracks sum without
normalisation, as the reference's `amix` did, and a master limiter at 0.95
catches what that produces. Still missing is real-time playback with the audio
clock as master, which is a preview concern rather than an export one.

**Phase 4 underway** — a project now renders end to end. `render_frame` takes a
project file and a time and writes a PNG, going through the whole chain: the
model says what exists, the plan decides what draws, the media layer supplies
frames, and the compositor combines them. The same path will serve export, which
is the point — the old app composited with a canvas for preview and an FFmpeg
filtergraph for export, and keeping those two agreeing was constant work.
539 tests.

The compositor handles per-layer position, scale, rotation, opacity, all eight
blend modes, adjustment layers, and gradient mattes, and can read the result
back to system memory. Decoding stays sequential wherever it can: a source's
decoder is held open and only seeks when a request moves backwards or jumps far
enough ahead that decoding through would be slower.

All eleven registry effects run as shaders: brightness, contrast, saturation,
hue, black and white, invert, flip, crop, vignette, Gaussian blur, and the
chroma keyer. Blur is the one whose maths is approximate — the tap count is
bounded and the step widens for large radii, so above roughly sigma 10 it
samples a Gaussian rather than integrating one.

Two colour decisions worth knowing about, both in `docs/architecture.md`:

- **Blending is linear.** A 50% dissolve passes through the true midpoint rather
  than the too-dark one a gamma-encoded canvas produces, so old and new projects
  will not match pixel for pixel across a dissolve.
- **Effects are not.** They run on coded values, where FFmpeg's filters are
  defined and where the spec specifies them. Each operation happens in the space
  it was defined in.

Still to come: titles, which need a text rasteriser and so wait on Skia; Skia
sharing the device; and keeping hardware-decoded frames on the GPU instead of
uploading them from system memory.

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
build/media/tools/render_frame/Release/render_frame <project.json> <seconds> <out.png>
build/media/tools/export_project/Release/export_project <project.json> <out.mp4>
```

`render_frame` is the end-to-end check: it renders one frame of a project
through the real pipeline and writes it out, so what the compositor produces can
be looked at rather than only asserted about.

`preview_window` is a throwaway viewport for driving the render pipeline by
hand; it is not the editor's UI and will not become it. Space plays, the arrow
keys step a frame, Shift jumps, Home returns to the start. Ctrl with the arrows
moves the layer, `+`/`-` scale it, `r` rotates, `o` halves opacity, `b` cycles
blend modes, and `f` flips. The effects are on letter keys — `g` brightness,
`c` contrast, `s` saturation, `h` hue, `i` invert, `v` vignette, `x` crop,
`u` blur, `k` chroma key — with Shift stepping the other way and `0` resetting
everything.

Media tests need real footage, which is not in the repository. Point
`CUTLINE_TEST_MEDIA_DIR` at a directory containing `Boiler.mp4` to run them;
without it they skip rather than silently pass.

## Licence

GPL-3.0-or-later. Cutline links x264 and x265 for software H.264/H.265 encoding
alongside the hardware encoders, and both are GPL, so the application is GPL
too. See [LICENSE](LICENSE).
