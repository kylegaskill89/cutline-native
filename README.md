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

- [`docs/handoff.md`](docs/handoff.md) — **start here if you are picking this
  up**: how to build it, the rules it is written to, what works, what is left,
  and the traps that have already cost a day each.
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
drawn into a 16-bit float target, which the present pass encodes for display.
The encode is in the shader rather than in an `_SRGB` view because the same
texture is handed to Skia, and a typeless target that could be viewed both ways
is not one Direct3D will make a shader resource view of.

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
catches what that produces.

`play_project` plays a project in a window with sound, through WASAPI. The sound
card keeps time and the picture follows it: audio is the one that cannot be
nudged, since a dropped video frame is invisible at 60 Hz where a gap of the same
length in audio is a click.

It also reports whether the preview kept up, and that number drove the next
round of work. It started at **27 of every 60 frames**; it now runs at **57 fps,
95%** of a 4K60 project, decoding 386 source frames for 367 drawn and seeking
once.

Neither fix was hardware decode, which is what the 29 ms of "compositing" looked
like it needed. The two that mattered:

- **A sub-frame step was reading as a seek.** Decoding stops at the first frame
  reaching the requested time, so the decoder sits up to one frame *ahead*. A
  later request closer than that overshoot looked like a move backwards, and a
  seek re-decodes from a keyframe — 17 seeks and 1827 decoded frames over six
  seconds, against 384 once a backwards move smaller than one frame was
  tolerated. Compositing went from 23 ms to 2 ms.
- **`Sleep(1)` is 15.6 ms** at Windows' default scheduler tick, which is most of
  a frame at 60 Hz. `timeBeginPeriod(1)` took the preview from 33 to 57 fps.

The decoder can now produce Direct3D 12 textures on the compositor's own device
(`d3d12va`, FFmpeg 8), which is the groundwork for sampling frames without a
copy. It is not yet what draws them, and the measurement says there is less in it
than the phrase suggests: on 4K60 HEVC, software decode costs **2.13 ms/frame
against 1.70 ms** for d3d12va. Hardware decode is 1.3× here, not an order of
magnitude.

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

All eleven are reachable from the Effect Controls panel, which reads a
catalogue that lives beside the resolver: what an effect is called, what it
takes, and where each parameter starts. Effects can be added, removed,
reordered, disabled without being removed, and adjusted.

Every parameter that can be animated has a stopwatch, as Premiere's does.
Turning it on makes the value it already had the first keyframe, so nothing
about the picture changes at the moment it is pressed; turning it off keeps the
value the keyframes were producing at the playhead. While it is on, moving a
slider writes a keyframe there rather than a stored value, and the row shows
what the animation is doing at the playhead rather than a number nothing is
using. Keyframes are drawn on the clip in the timeline, so an animation is
visible where the editing happens.

Titles are drawn rather than decoded, so they take a route of their own:
`cutline::text` rasterises one with Skia and the compositor samples it as
premultiplied sRGB RGBA. Premultiplied because it is filtered — sampling
straight alpha mixes the colour of transparent pixels into the edge of a glyph,
which is how text acquires a halo. The shader divides the alpha back out before
the effects, so a title takes a colour correction or a blur exactly like a video
clip. A title is sized to its own text, which means the plan needs a font to ask;
whoever can draw text supplies one, and a caller that cannot gets a title that
fills the canvas rather than nothing.

Colours are picked rather than typed. A swatch shows the colour and its hex and
opens a saturation/value square with a hue strip, an alpha strip and a hex field
— which is how a title's colour and outline and the chroma keyer's key colour
are now set. The picker holds its own HSV coordinates rather than reading them
back from the colour, because hue and saturation are not recoverable from every
colour: black is every hue at once and a grey has no saturation to read. A
picker that re-derived them would swing back to red the moment the value reached
zero and lose the hue somebody had just chosen.

Still to come: dragging keyframes in time, and keeping hardware-decoded frames
on the GPU instead of uploading them from system memory.

**Phase 7 (interface) underway** — one window, drawn on the GPU with Skia,
sharing the compositor's Direct3D device so a decoded frame reaches the screen
without a copy through system memory. Panels dock, tear out into windows of
their own, and remember where they were; four themes change the chrome rather
than only the colours. The timeline edits, the sequence plays at rate against
the audio clock, and export runs on its own thread with progress and cancel.
Titles are made, typed and styled in the panel: `TextField` is the application's
first editable control, with a caret, a selection, and the keyboard anyone would
expect of one.

The tool palette is in the timeline's toolbar, on Premiere's keys — V, C, R, Y,
U. The tool is *state* rather than a held modifier, and that is what makes these
edits reachable at all: slip and slide are two-handed gestures if a key has to be
held down, and a razor that needs one cannot cut a dozen clips in a row. All four
are a drag over the body of a clip, so without a tool to say which is meant they
would every one of them be the same drag.

- **Razor** cuts on the press, with shift to cut through every track. It does
  not select what it cuts: the tool is used repeatedly, and leaving one of the
  two halves highlighted after each cut is a running commentary nobody asked
  for.
- **Rate stretch** pulls an edge to change the clip's *speed*. The source in and
  out stay put, so unlike a trim it can make a clip longer than the footage it
  came from.
- **Slip** moves which part of the source a clip shows. It is the one gesture
  with nothing to watch on the timeline, and that is the point — the clip does
  not move. Dragging right shows earlier footage: the clip is a window onto a
  strip of film and the strip follows the hand.
- **Slide** moves a clip into its neighbours. The one before grows, the one
  after shrinks, and the sequence keeps its length — three edges at once, all
  previewed live.

Each of these was already a tested operation in the pure core and unreachable
from the interface. What is new is the gesture, and where the interface's units
become the model's: a slip is dragged in timeline seconds and stored in source
seconds, and the two differ by the clip's speed.

Track headers carry their switches: mute and solo on audio tracks, hide on video
ones, and lock on both. They are letters — M, S, L, H — because a padlock and an
eye both need arcs the painter has no other use for, and at twelve pixels a drawn
padlock is a grey smudge. What is lit is what the *project* holds rather than what
takes effect: a track silenced because another is soloed is not muted, and
lighting its M would leave somebody pressing a button that is already off. The
mixer and the exporter have honoured all of this from the start; until now there
was no way to press it.

Colour mattes and adjustment layers can be made at last, from a New menu beside
Import — the third and fourth things the editor creates rather than imports. A
matte's panel is two swatches and an angle, and the second swatch is what turns a
flat fill into a gradient: there is no separate switch, because a gradient with
one colour is a solid and saying so twice invites the two to disagree. An
adjustment layer's panel says what it is for, since a clip that draws nothing of
its own otherwise looks like a clip that is broken.

Markers are dropped on the ruler with M, walked with shift and control, and
thrown away with Ctrl+Alt+M — the awkward chord being the point, since that one
clears the lot. Dropping one where one already sits takes it away, which is the
same toggle the in and out points use and the reason neither needs a second
control that exists only to undo it. Each wears its own colour when it has one,
because that is what a marker's colour is for.

Every animated parameter now carries the curve it moves along: a chip beside the
stopwatch reading Linear, Hold or Ease, cycling rather than dropping down —
three is short enough to walk round. It is one setting per property rather than
per keyframe. The model stores the mode on each breakpoint and the reference
exposed one setting for the whole property, which is the right call: a panel
offering a different curve out of every keyframe is a control nobody has asked
for.

The eight audio effects are reachable too, from the same panel and through the
same rows. An audio clip gets its own stack — add, remove, reorder, disable,
adjust — with the registry's ranges, steps and units in the labels, so a cutoff
reads as hertz and a shelf as decibels. There is no stopwatch on any of them:
`AudioClipEffect` holds parameters and nothing else, and offering an animation
control with nowhere to keep a keyframe would be a button that cannot do what it
says. Clip gain *is* automatable — that is a different property, with its own
rubber band still to come.

Nothing needed writing but the join: the audio registry has declared each
effect's parameters, ranges, defaults and units since the DSP was written, in
exactly the shape the video catalogue uses.

Transitions are reachable at last. All four — cross dissolve, dip to black, push
and slide — have rendered since the segment resolver was written and none of them
could be asked for. The panel offers them at a clip's out-edge, but only where
another clip abuts it: the model would keep a transition at the end of a track
and the renderer would ignore it.

The part worth a layer of its own is **whether a transition would do anything**.
A dissolve, push or slide overlaps the two clips, and overlapping means borrowing
unused source from each side — a clip trimmed to the last frame of its footage
has none to lend, and the resolver skips it in silence. So the panel asks first,
says "no handles" beside the kinds that cannot work, and bounds the duration
slider by what the join can actually manage. Dip to black is the exception and
the useful fallback: it fades out and then in, sequentially, and never needs a
handle. On the timeline a transition is a box straddling the cut with a diagonal
through it, named when the name fits.

In and out points mark the span the sequence is cut for. They are set from the
buttons or from I and O, drawn along the foot of the ruler, saved with the
project, and offered to export as "only the marked range" with the timecodes it
means. Marking where a mark already is takes it away, which is how one is
removed without a third control that exists only to undo the other two. The two
can never cross: setting one past the other clears that other, so an inverted
pair is unrepresentable rather than something every reader has to check for.

Fades are dragged on the clip. A handle rides the top edge at the point the fade
finishes, so it slides along as the fade grows and the thing being dragged is
where the fade ends. It is drawn even at zero, because a control that only
appears once it has been used is one nobody finds — and it owns the top of each
corner, where the trim handles also are, since the trims stay reachable
everywhere below it and a corner that trimmed would leave the fades unreachable.

More than one clip can be selected: shift-click adds one or takes it out again,
and a drag over empty track sweeps up everything the rectangle touches — touches
rather than encloses, since a clip wider than the window could never be enclosed
and that is exactly when a sweep is what somebody wants. Clicking a linked clip
selects everything linked to it, so what is highlighted is what an edit is about
to reach; the two had no business disagreeing.

The scopes are in: histogram, waveform, RGB parade and vectorscope, in a panel
of their own that docks and tears out like any other. The counting lives in the
render layer and the drawing in the widget layer, which is what lets the
arithmetic be checked against a frame built in three lines — and what guarantees
a scope can never affect an export, since the only thing passing between them is
a set of tallies.

Two numbers in it are worth knowing. The vectorscope's square reaches to 140 of
chroma rather than 128, because a fully saturated primary sits about 136 from
grey and a square stopping at 128 would clamp the very colours the scope exists
to show. And the drawing applies a gain on top of the usual square root: a
scatter spread over 65,536 cells leaves single pixels at a few per cent, which on
screen is nothing at all.

Snapshot writes the frame at the playhead to a PNG. Rendered again rather than
read off the monitor, because what is on screen is letterboxed into whatever the
panel happens to be and a snapshot is a frame of the *sequence*, at its own size
— the same call export makes per frame, so the file is what the movie would
contain.

Clips link and unlink, and tracks can be added and removed, from the timeline's
toolbar or from Premiere's Ctrl+L. Both go through the command table rather than
touching the model where the button is, so a shortcut and a button cannot come to
mean different things — and the same `can_run` that would grey out a menu item is
what greys out the button, which is why Link is dark until two clips are selected
and Unlink until one of them is linked.

Video clips show a filmstrip and audio clips show their waveform, both extracted
on workers so the interface never waits for a decoder. Each describes the
*source* rather than a clip of one, so a file cut into twenty pieces is decoded
once and every piece shares the result by pointer — the model is rebuilt after
every gesture, and copying half a megabyte of peaks or two of pixels per drag is
the difference between editing feeling free and feeling like work.

The filmstrip is tiled left to right, each tile showing the frame nearest the
source time under it, and only the tiles on screen are drawn. Sampling is coarse
by design: a strip says what the footage is, not what each instant of it looks
like, and a short clip of a long source may repeat a frame. Because these are
pixels rather than numbers the cache is bounded, and drops whichever source has
gone longest without being wanted.

Audio clips show their waveform. The envelope is min and max per bucket rather
than an average, so a transient is visible instead of averaged away, and it is
drawn a column per pixel — what it costs is what is on screen, not how long the
clip is, so a ten-minute source zoomed out to a centimetre draws a centimetre's
worth.

The envelope belongs to the *source*, not to any clip of it: trimming a clip does
not change the shape of the file behind it, so a source cut into twenty pieces is
decoded once and every piece shares one envelope by pointer. Where a block sits in
it is three numbers on the block — where it starts in the source, how fast it runs
through it, and whether it runs backwards — which is what makes a retimed clip
draw the audio it actually plays rather than the audio at the same offset.

Decoding a stream to get one costs seconds and hundreds of megabytes, so it
happens on a worker and the clip is simply drawn without until the answer lands.
The worker posts a window message rather than setting a flag: the frame loop
blocks on its queue whenever nothing is playing, and a polled flag would show the
waveform at the next mouse move instead of when it was ready.

A clip's volume is a line across its block, on every audio clip rather than only
the selected one — which clips have been ridden is what somebody reading a mix
wants to see without clicking through them. Dragging the line sets the clip's
gain; alt puts a point on it, and alt on a point takes it away, the same toggle
the markers and the in and out points use. A point moves in time as well as in
level, so an automation can be shaped rather than only levelled.

The scale is decibels, and that is the whole of the design. Gain is stored as a
linear multiplier, and on a linear scale the couple of decibels either side of
unity that is most of mixing lands within a few pixels of the top of a
forty-pixel clip, while the bottom half of the band spans -6 dB to silence and is
worth nothing. In decibels each 6 dB step is the same distance. The band's floor
is where the useful range stops rather than where audio does — thirty-six
decibels holds both a fine trim and ducking a bed under a voice — and below it a
clip is silent, because the one thing a volume control at its bottom stop is
expected to do is nothing at all.

Once there are points, dragging the *line between two of them* moves that
stretch, carrying the point at each end with it. That is how a level is ridden:
grab the bit that is too loud and pull it down. Both ends move by the same number
of decibels rather than to the same value, so a ramp stays the ramp it was and
only its level changes.

**The clip's own edges count as points.** Without that, two points were not
enough to duck a region: outside the outermost points a band is flat *because*
those points define it, so moving them moved the whole line. A stretch drag now
anchors each end of the clip at the level it already had — which changes nothing
about what plays, and turns the edges into the points they always looked like
they were. Two points and a drag give a dip with the head and tail left alone.

Adding a point takes the value the band already had there, so automating a clip
never starts by altering it — the same bargain the inspector's stopwatch makes.

The inspector's Volume row is in decibels for the same reason and against the
same floor. It was a percentage of unity, which put half the slider's travel
between +0 and +6 dB and squeezed everything from a gentle trim down to silence
into the last tenth of it — so a clip pulled down on the rubber band read as
pinned to the left whatever it had been set to. The two controls are now two
views of one number that cannot disagree about where silence is.
Nothing needed writing in the model: `move_gain_keyframe` and `set_clip_gain`
had been there, tested, since the first phase, and the mixer and the exporter
have honoured automation from the start. This is the second feature in a row
whose work was entirely the gesture.

1758 tests, plus a headless check that lays every panel out in every theme —
including the inspector with a clip selected and every effect on it, an audio
clip with all eight of its own, a matte, an adjustment layer, a title, and the
colour picker open, which is the only way the controls a panel is made of get
checked at all. It fails on a widget that landed nowhere, outside the window, or
cut in half by the panel holding it. Given a directory, that check writes each theme's frame out
as a PNG, so a changed fingerprint can be looked at rather than guessed at.

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
