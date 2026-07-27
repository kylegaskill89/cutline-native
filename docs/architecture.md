# Architecture

How the native version is built, and why. The product specification — data
model, algorithms, effect semantics, UI — lives in [`spec.md`](spec.md); this
document covers the decisions that are specific to the C++ implementation.

---

## 1. The problem being solved

The TypeScript version had one fatal constraint. Frame-accurate export requires
decoding the source at each output time, and a browser `<video>` element
re-decodes from the nearest keyframe on every seek. On 4K long-GOP MKV — the
primary source material — that measured about **4.5 seconds per frame, roughly
18 minutes for an 8-second clip.** Browsers optimise for smooth forward
playback, not deterministic random access.

Everything else that was wrong (CPU compositing, per-pixel effects in
JavaScript, the JS↔Rust IPC boundary, the OS command-line length limit on the
FFmpeg filter graph) was an irritation. This was the wall.

The native version fixes it by decoding **sequentially** during export — the
access pattern hardware decoders are built for — and compositing on the GPU.

## 2. Decisions

| Area | Choice |
|---|---|
| Platform | Windows only |
| Language / toolchain | C++23, MSVC, CMake, vcpkg |
| Media | libav* linked in-process — no `ffmpeg.exe`, no CLI |
| GPU API | Direct3D 12 |
| Video compositing | Hand-written D3D12 pipeline |
| UI | Skia (Ganesh, D3D12 backend) on the same device |
| Audio | Own DSP, shared by preview and export |
| Export | Own compositor only — no describe-to-FFmpeg path |
| Colour | HDR end to end; linear-light compositing |
| Encoding | Hardware via libavcodec (NVENC / QSV / AMF), x264/x265 fallback |
| Licence | GPL-3.0-or-later |

### Direct3D 12, not D3D11

A video compositor issues a handful of textured quads and shader passes per
frame. It is nowhere near draw-call or submission bound, which is the only axis
where D3D12 beats D3D11 — so on the compositor alone, D3D11 would have been the
simpler choice.

Skia decides it. Skia's Ganesh D3D backend is **D3D12-only**; there is no D3D11
backend. With Skia drawing the UI and a separate pipeline drawing video, D3D11
would force cross-API texture interop between them on every frame. One device,
one API, zero copies between compositor and UI is worth the extra boilerplate.

Hardware decode still surfaces as D3D11VA textures, shared into D3D12 through NT
handles. That seam is confined to the decoder.

### Own compositor only

Every export renders through the GPU compositor. There is no FFmpeg filter-graph
export path.

This is the single largest simplification over the TypeScript version, and it
deletes whole subsystems:

- **No filter-graph compiler.** The concat-vs-overlay path selection, the
  `-filter_complex` string building, and the temp-file workaround for the
  command-line length limit are all gone.
- **No keyframe baking.** The old exporter expanded animated segments into up to
  600 fixed-transform slices sampled at 30fps, because FFmpeg filtergraphs
  cannot animate a transform per frame. The compositor just evaluates keyframes
  at each real output frame.
- **No frame-accurate "mode".** It is simply how export works.
- **No PNG round-trip for generated media.** Text and colour mattes rasterise
  directly into the compositor.
- **One emitter per effect, not two.** The old effect registry defined each
  effect twice — a CSS filter for preview and an FFmpeg fragment for export —
  specifically so the two could not drift. With one renderer that problem
  disappears. The FFmpeg fragments recorded in the spec are now the
  *specification* of each effect's maths, which the HLSL shader must implement;
  they are no longer a code path.

The cost is that every effect must be written as a shader, including chroma key
and vignette, which previously came free from FFmpeg.

### Own audio DSP

Same reasoning. Preview and export share one graph implementation, driven either
in real time or offline. Biquads follow the RBJ cookbook, which is what both Web
Audio and FFmpeg implement, so the parity table in the spec stays accurate — but
parity stops being something to maintain, because there is only one
implementation. The approximate compressor and the gain-easing gap (where the
old scheduler ramped linearly between breakpoints even for eased keyframes) both
get fixed properly rather than documented.

### HDR

HDR sources are already part of the workflow, so this is a current requirement
rather than headroom. It is a property of the pipeline, not a feature layered on
top, which makes it much cheaper to build in now than to retrofit.

Decode converts to linear scene-referred light using the source's transfer
function and primaries. Compositing happens in linear light throughout — which
is also simply more correct for blend modes and effect stacking than the old
8-bit sRGB canvas. Output applies a display transform: tone-mapped SDR, or HDR10
PQ with metadata.

Probing therefore has to report colour primaries, transfer characteristics, and
matrix per file, not just dimensions and frame rate.

### GPL

Wanting a real software encoder fallback settles the licence. x264 and x265 are
GPL-2.0-or-later, and there is no LGPL-compatible software H.265 encoder worth
shipping. So Cutline is GPL-3.0-or-later, FFmpeg is built `--enable-gpl
--enable-libx264 --enable-libx265`, and the public repository satisfies the
source-availability obligation.

## 3. What carries over unchanged

The parts of the old design that earned their keep:

- **The pure data model.** Project → Track → Clip over a media pool, as value
  types. Editing operations take a project and return a new one; they never
  mutate. Every subsystem is a function of the model. This is what made the
  TypeScript version testable, and it is not up for renegotiation.
- **Segment resolution.** Transitions expand into renderable segments by
  borrowing unused source handles and applying geometry — deliberately *not*
  FFmpeg's `xfade`, which was fragile against the overlap model.
- **Effect and audio-effect parameter semantics**, exactly as specified.
- **The testing discipline.** Pure logic, table-tested, no GPU or media
  dependency.

## 4. Layout

```
src/core/     pure model, time maths, keyframes, segments, serialisation
src/media/    libav* decode, encode, probe, waveforms, thumbnails   (phase 2)
src/gpu/      D3D12 device, resources, shaders, colour management   (phase 3)
src/render/   the compositor                                        (phase 4)
src/audio/    DSP graph, real-time and offline                      (phase 5)
src/export/   render-to-file orchestration                          (phase 6)
src/ui/       Skia widget layer and the editor's panels             (phase 7)
tests/        unit tests, mirroring the src tree
docs/         this file and the spec
```

`src/core` depends on nothing. Each layer above depends only on the layers below
it.

## 5. Conventions

- `snake_case` for functions and variables, `PascalCase` for types,
  `namespace cutline::<layer>`. Names stay recognisably parallel to the
  TypeScript reference (`eval_keyframes` ↔ `evalKeyframes`) so ported code is
  traceable back to it.
- Ported numeric behaviour matches the reference exactly, including its quirks.
  Where JavaScript semantics differ from C++ — `Math.round` breaks ties toward
  +infinity, `std::round` breaks them away from zero — the difference is
  reproduced explicitly and commented, not silently "corrected".
- Undefined behaviour is not part of that bargain. Where the reference would
  produce garbage (casting a non-finite double to an integer), the native
  version clamps and says so.

## 6. Build phases

1. **Core model** — data model, time maths, keyframes, editing operations,
   segment resolution, serialisation. Pure and fully testable.
2. **Media I/O** — probe, hardware decode, audio decode, waveforms, thumbnails.
   Includes the benchmark that validates the entire premise of the rewrite:
   sequential decode throughput on a real 4K MKV. If that number disappoints,
   better to know here than after building a compositor on top of it.
3. **GPU foundation** — D3D12 device and resources, DXC shader pipeline, colour
   management, Skia sharing the device. Plus a throwaway debug viewport: a
   window, a hardcoded project, a scrubbable playhead. Not shipped UI — it
   exists so the compositor is exercisable by hand long before the real UI does.
4. **Compositor** — draw order, transforms, blending, effects as shaders,
   generated media, adjustment layers. Golden-image test harness lands here.
5. **Audio** — DSP graph, real-time playback with the audio clock as master,
   offline render.
6. **Export** — decode → composite → encode → mux, with a headless CLI that
   renders a project file to MP4.
7. **UI** — the Skia widget layer, then the editor's panels, timeline, monitor,
   and scopes.
8. **Packaging** — installer, auto-updater, release CI.

## 7. Validation

Correctness is proven by automated tests, because pixel and audio output cannot
be judged by the agent writing the code.

- **Unit tests** for everything in `src/core` and the audio DSP.
- **Golden-image tests** for the compositor: render known inputs, compare
  against committed reference frames within a numeric tolerance. These prove
  that nothing *changed*; they do not prove the reference was ever right, so
  each reference frame needs signing off by eye once before it is frozen.
- **Throughput benchmarks** for decode and export, so a performance regression
  is a test failure rather than a surprise.
