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

### Measured, 2026-07-27

`tools/decode_bench` runs both access patterns over the same file. On an NVIDIA
GPU via D3D11VA, 4K60 HEVC:

| Access pattern | `Boiler.mp4` (8 s, 7.7 Mbps) | `Replay ….mkv` (10 min, 20.2 Mbps) |
|---|---|---|
| Sequential, hardware | **1.56 ms/frame** (640 fps) | **1.67 ms/frame** (601 fps) |
| Sequential, software | 2.69 ms/frame (372 fps) | 2.74 ms/frame (365 fps) |
| Per-frame seek, hardware | 27.6 ms/frame | 28.2 ms/frame |

Sequential decode runs at **about 10× realtime** on both files, and the whole
480-frame `Boiler.mp4` decodes in 0.75 s — against roughly 18 minutes for the
same clip through the old exporter.

Two conclusions, one of them a correction. Sequential access is worth **17×**
over per-frame seeking, so designing export around it is right. But native
per-frame seeking costs 28 ms, not 4500 ms — so the original 4.5 s/frame was
overwhelmingly **browser overhead, not an inherent cost of seeking**. The
`<video>` element was the problem more than the access pattern was. Both point
the same way, and the rewrite stands, but the diagnosis in §1 was only partly
right.

Long-GOP MKV is also no worse than MP4 here, which the original report implied
it would be.

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
| Colour | Linear-light compositing at 16-bit float; HDR deferred past v1 |
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

The split was worse than "kept in step with effort". Web Audio and FFmpeg agree
on the *shape* of these filters and disagree on their defaults: a plain
high-pass at Q 1 against Q 0.707, a compressor at 3 ms attack against 20 ms and
a 30 dB knee against 9 dB. A clip tuned by ear in the preview did not sound the
same in the file. Where the two disagreed the native implementation follows
FFmpeg, because that is what exports were actually rendered with — the preview
was the one that had been lying.

Two behaviours are deliberately kept rather than tidied:

- **Gain and fades apply before the effect stack**, which is the order the old
  filter chain used. It has an audible consequence — a compressor after a fade
  partly undoes it, pulling the quiet end back up — so a project mixed against
  it would change if the order were "fixed".
- **Tracks sum without normalisation** (`amix … normalize=0`): four tracks at
  unity really are four times as loud, and adding one never quietens what is
  already there. A master limiter at 0.95 catches what that produces, which is
  the reason there is a limiter at all.

Filters are tested by frequency response rather than by their coefficients.
Asserting that a high-pass is 3 dB down at its corner and falls 12 dB per octave
says what the filter *is*; comparing a coefficient against 0.9987 only says it
was typed the same way twice, and still passes when the formula is wrong. One
test ties the predicted response back to what actually comes out of `process`,
so the rest cannot all agree with a response function no filter implements.

### The audio clock is the master clock

Which subsystem keeps time is a decision, not a detail. Audio is the one that
cannot be nudged: dropping or repeating a video frame is invisible at 60 Hz,
while a gap of the same length in audio is a click, and any clock that drifts
against the sound card eventually produces one. So the device's own consumption
drives the timeline and the preview asks where the playhead is rather than
deciding.

The device reports progress once per buffer period, which on some endpoints is
30 ms or more. Taking that literally makes the playhead a staircase and pins the
preview's frame rate to the audio callback rate, so the reported position is the
last thing the card said plus the wall time since it said it — bounded, and
re-anchored on every update. It is still audio-mastered; wall time only fills
the gaps.

Playback owns a render thread that does nothing but mix. Everything that touches
a file — decoding, resampling, building filter chains — happens when the player
is created, because WASAPI hands out a buffer every few milliseconds and missing
that deadline is audible.

### Painting is a small set of primitives, behind an interface

Widgets do not draw. They ask the theme for the style of their part and state
and hand it to `paint_surface`, which knows the one thing worth centralising:
the order the pieces go down in. Outer shadow, then fill, then inner shadow
clipped to the shape, then bevel, then border. An inner shadow drawn before the
fill is painted over; a border drawn before the bevel is half covered by it.
Both survive a glance at a screenshot, which is why the order lives in one
function with tests on it.

`Painter` is an interface for one reason: `RecordingPainter` captures the calls
as data. "Does a pressed XP button invert its bevel" and "does an Aero panel
blur what is behind it" become ordinary assertions, with no GPU and no
screenshots to eyeball. `SkiaPainter` then makes the same calls for real, and is
checked separately against a CPU raster surface — that a vertical gradient runs
top to bottom, that a stroke lands inside its bounds rather than growing the
control by a pixel, that a raised bevel is lit from above and an inset one is
not.

The split matters beyond testing. If a theme ever needs something the primitives
cannot express, that is the model being wrong, and it surfaces at the painter
rather than three layers into the timeline.

### Layout is arithmetic, so it is proved rather than eyeballed

Where things go is functions over plain data: rectangles in, rectangles out, no
widgets and no state. `distribute` shares an axis between children that want
fixed, flexible, or bounded amounts of it; `SplitLayout` is the draggable
dividers between docked regions; `Viewport` is scrolling and zooming a region
larger than its window.

This is the part of a UI most likely to be subtly wrong and least pleasant to
debug by dragging a window around. Kept as pure functions, the awkward
questions are assertions instead: that a divider drag moves only its two
neighbours and leaves the far side of the window alone, that panes and dividers
exactly fill their bounds rather than leaving a sliver of unpainted background,
that a scrollbar thumb can actually reach the end of its content — thumb travel
is the track minus the thumb, not the track — and that zooming a timeline keeps
the frame under the cursor under the cursor.

Two rules fall out of the same reasoning. Sizes overflow honestly rather than
being squashed to fit, because an overflowing panel is a scrolling panel and
hiding that turns a layout bug into a missing control. And split sizes are held
as fractions rather than pixels, so maximising a window keeps the proportions
the user chose instead of handing the whole gain to one pane.

### Input is data, so routing is testable

Events are values. Nothing in `event.hpp` knows about Win32; the platform layer
only has to translate `WM_*`. That means hover, capture, focus and bubbling are
all driven from tests with no message loop, which is the only practical way to
check the behaviours that matter and are otherwise invisible.

`WidgetHost` owns the interaction state no single widget can know. Three
decisions in it are load-bearing:

**A handled press captures the pointer until release.** Every drag in the
application depends on it — a slider that stops tracking when the cursor leaves
the thumb, a timeline that stops scrubbing at the edge of a track. Hover is
frozen for the duration, so a drag does not hand its highlight to whatever it
passes over, and capture deliberately survives the pointer leaving the window:
dragging a clip out past the edge and back is one gesture.

**A disabled widget swallows rather than passing on.** Clicking a greyed-out
button should do nothing, not fall through to the panel behind it. Bubbling
stops there.

**A press moves focus only onto something that can take it.** Clearing focus
when clicking an unfocusable toolbar button would strand the keyboard, and the
transport shortcuts would stop working until you clicked back into the timeline.

Widgets still do not draw themselves: `Widget::paint` looks its part and state
up in the theme and hands the style to `paint_surface`. Hit testing searches
children in reverse order, exactly mirroring the order they paint in, so what
answers a click is always what is on top.

### Controls take their size from the theme, not from constants

A widget owns three things — the room it wants (`sizing`), where it puts its
children (`layout`), and what it adds on top of its themed surface
(`paint_content`) — and nothing else. None of them names a colour, a corner
radius, or a number of pixels of padding. A control that hard-coded any of
those would be the one thing a theme could not change, and there is no way to
find those except by reading every widget.

That is why `sizing` is handed a `LayoutContext` rather than working from
constants. It carries the theme, so a button is `control_height` tall under
whichever theme is loaded and a bevelled one gets the room its bevel needs; and
it carries a `TextMeasurer`, because a button's width is its label's width and
only the backend that rasterises the font knows that. Measuring is split out of
`Painter` for exactly that reason — sizing happens long before there is a canvas
to draw on. `RecordingPainter` answers with a stable estimate, which is what
lets the sizing rules be tested with no font present at all.

One rule needed stating explicitly: a box does **not** inherit flexibility
across its own axis. A spacer is flexible in every direction, so a toolbar that
took that on would grow to swallow the window instead of being as tall as its
controls.

### Colour precision, and why HDR is deferred

Compositing happens in **16-bit float linear light** throughout. Decode converts
to linear scene-referred values using the source's transfer function and
primaries; output applies a display transform. This is worth doing regardless of
HDR — linear-light blending is simply more correct than the old 8-bit sRGB
canvas, and the extra precision shows up in gradients and in stacked effects,
where 8-bit intermediates band visibly.

Linear blending is a **deliberate behavioural divergence** from the reference,
not just a precision upgrade. A 2D canvas blends gamma-encoded values, so the old
app's 50% cross-dissolve passed through a midpoint that was visibly too dark;
ours passes through the true one. Old and new projects will therefore not match
pixel for pixel wherever a dissolve, a partial opacity, or a non-normal blend
mode is involved. That was accepted when the float pipeline was chosen, and the
compositor tests assert the linear answer explicitly so nobody later "fixes" it
back.

**Effects are the exception, deliberately.** They run on *coded* values — after
the YUV matrix, before the transfer function — because that is the space
FFmpeg's filters are defined in, and §13 names those fragments as each effect's
authoritative behaviour. This is not a rounding-level distinction: applying a
200% contrast to linear light is a large visible difference, not a subtle one.
So blending is linear because it is a physical operation, and effect maths is
not because it is a specified one. Each happens in the space it was defined in.
The compositor tests state the coded-space answers explicitly — a mid grey is
its own inverse, contrast pivots about it — so moving the maths would fail them
rather than pass quietly.

HDR itself is **deferred past v1**. It was originally scoped into v1 on the
understanding that HDR footage was already part of the workflow. Probing the
actual sources contradicted that: every video file on hand is HEVC **Main**
profile, 8-bit `yuv420p`, BT.709 primaries, transfer, and matrix, with no
mastering-display or content-light-level side data on any frame. That includes
the capture originally identified as HDR. The likely explanation is an HDR
display or Windows Auto HDR being active while the encoder recorded SDR.

Building a PQ/HLG decode path, tone mapping, and HDR10 output with no real
source to validate against would mean testing against synthetic patterns and
hoping. So v1 handles SDR end to end.

What keeps HDR cheap to add later is that the colour layer is parameterized by
transfer function and primaries from the start rather than assuming BT.709, and
the compositor is already linear and high-precision. Adding HDR then means new
transfer curves, a tone-mapping operator, and display/metadata plumbing — not
restructuring.

Probing reports colour primaries, transfer characteristics, and matrix per file
regardless, since that is what makes the distinction visible in the first place.

### Themes change chrome, not just colours

The UI must be themeable, with built-in themes that are **not recolours** —
Windows XP / Frutiger Aero and Vista Aero glass were named specifically, and
more are wanted. This is recorded here rather than left to phase 7 because it
decides the shape of the widget layer, and retrofitting it is the expensive
version.

Concretely, a widget must never draw itself directly. Painting goes through a
theme interface, and a theme owns:

- **Chrome**, not a palette: bevels with inner and outer highlights, multi-stop
  gradients, glass and translucency, corner radii, borders, drop shadows. A
  theme that can only substitute colours cannot express XP's bevelled buttons or
  Vista's blurred glass at all.
- **Metrics as well as appearance** — padding, control sizes, title-bar height,
  scrollbar width. Bevelled chrome needs different spacing from flat chrome, so
  a theme that owns only paint would come out cramped or loose.
- **Per-widget painters**, so a theme can override how one control is drawn
  without the widget knowing which theme is active.

Skia makes the drawing side straightforward: gradients, blurs, and layer effects
are primitives, and the backdrop blur that glass needs is a filter rather than
something to build. The work is in the layering discipline, not the rendering.

The other consequence is that the widget layer is ours rather than a native
control toolkit. Native controls cannot be themed this way, and the program
monitor already needs to composite through our own pipeline, so there was no
native-controls path to give up.

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
src/gpu/      D3D12 device, compositor, presenter, shaders          (phase 3-4)
src/render/   what draws and what plays, as pure plans            (phase 4-5)
src/audio/    filters, dynamics, time stretching                    (phase 5)
src/engine/   frame renderer, audio mixer, exporter               (phase 4-6)
src/ui/       Skia widget layer and the editor's panels             (phase 7)
tests/        unit tests, mirroring the src tree
docs/         this file and the spec
```

`src/core` depends on nothing. Each layer above depends only on the layers below
it. `src/render` and `src/audio` are pure — no libav, no GPU — which is what
lets draw order, gain envelopes and a filter's frequency response all be
asserted without a device or a media file. `src/engine` is the one layer that
depends on everything, which is the point of it: it is where the model, the
plans, the decoder, the DSP and the compositor meet.

The original plan named a `src/export/`; what was actually built folded export
into `src/engine` beside the frame renderer, because an exporter that does not
share the preview's renderer is exactly the split this rewrite exists to remove.

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
   sequential decode throughput on real 4K footage (see *Reference media*
   below). If that number disappoints, better to know here than after building a
   compositor on top of it.
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
   and scopes. Themeable from the start; see below.
8. **Packaging** — installer, auto-updater, release CI.

## 7. Validation

Correctness is proven by automated tests, because pixel and audio output cannot
be judged by the agent writing the code.

- **Unit tests** for everything in `src/core` and the audio DSP.
- **Render-and-read-back tests** for the compositor, which replace the committed
  reference frames originally planned here. They render known inputs and assert
  on *properties* of the result — a matte fills the frame with the colour it was
  given, half opacity lands on the linear midpoint, a 90° rotation turns a wide
  bar into a tall one — rather than comparing against stored PNGs.

  The change is deliberate. A committed reference frame proves only that nothing
  changed, says nothing about whether the reference was ever right, and breaks
  on any driver that rounds differently. A property assertion states the
  intended behaviour in the test itself, so a failure names what is wrong
  instead of reporting that some pixels differ. Stored frames remain the right
  tool for effects whose output is genuinely hard to characterise, and can be
  added alongside these when the effect shaders land.

  They fall back to WARP, Microsoft's software rasteriser, so they run on CI
  machines with no GPU.
- **Throughput benchmarks** for decode and export, so a performance regression
  is a test failure rather than a surprise.

### Reference media

Benchmarks and golden-image tests run against real captures rather than
synthetic clips. These files are not in the repository — they are large, and
they are the user's footage — so benchmarks locate them by path and skip
cleanly when absent.

| File | Shape | Why it matters |
|---|---|---|
| `Boiler.mp4` | HEVC Main, 8-bit, BT.709, 3840×2160 @ 60, 8.0 s, 7.7 Mbps, AAC stereo | The clip behind the 18-minute export. The headline benchmark. |
| `Replay 07-23-2026 10PM-59-02.mkv` | HEVC Main, 8-bit, BT.709, 3840×2160 @ 60, 598.6 s, 20.2 Mbps, **4× AAC stereo** | Long-GOP MKV at length. The four audio streams exercise the audio-lane placement rule in spec §6, which the spec flags as a past bug. |

Neither is HDR; see *Colour precision* above.
