# Handoff

Everything someone picking this up needs: where it is, how to build it, the rules
it is written to, what works, what is left, and the traps that have already cost
a day each.

Read `README.md` for the story and the numbers, `docs/architecture.md` for the
native decisions, `docs/spec.md` for the product — it is the authority on exact
numeric behaviour, carried over from the TypeScript version.

---

## 1. What this is

**Cutline**, a Premiere-style non-linear video editor for Windows, written in
C++23. It is a ground-up rewrite of a Tauri application (Rust shell, TypeScript
UI, bundled FFmpeg binaries) that worked but exported at roughly **4.5 seconds
per frame** on 4K long-GOP source, because rendering an exact frame meant seeking
a browser `<video>` element and every seek re-decodes from a keyframe.

The native version exports the same eight-second clip in **4.0 seconds**. That is
the whole reason the rewrite exists, and it is done — see `README.md` for the
measurements and the one correction they forced.

| | |
|---|---|
| Repo | `github.com/kylegaskill89/cutline-native`, branch `main`, GPL-3.0-or-later |
| Local | `d:\Videos\cutline-native` |
| Old app | `github.com/kylegaskill89/cutline` — dead, kept as reference |
| Old app, local | `d:\Videos\VideoTrimmer` — holds `design.md` (the rewrite spec) and `summary.md` |
| Size | ~32k lines of source, ~22k of tests |
| Tests | **1730** under the `ui` preset; 1467 of them need no GPU, no window, no FFmpeg |

GPL because it links x264 and x265 for software encoding alongside the hardware
encoders.

---

## 2. Building

Needs Visual Studio 2022 with the C++ toolset, CMake 3.28+, and
[vcpkg](https://github.com/microsoft/vcpkg) with `VCPKG_ROOT` set.

```
cmake --preset default          # core + ui + editor. No FFmpeg, no Skia, no D3D12.
cmake --build --preset debug
ctest --preset debug
```

**Use `default` for anything that does not need pixels.** It configures in
seconds and builds in a couple of minutes, and it covers the model, the editing
operations, the effect catalogue, the whole widget and theme layer, and every
binding between them — 1467 of the 1730 tests.

The heavier presets pull vcpkg features and take a long time on first configure:

| Preset | Adds | First configure |
|---|---|---|
| `default` | — | fast |
| `media` | FFmpeg + Direct3D 12 | ~20 min (FFmpeg from source) |
| `skia` | Skia only, warnings as errors | long |
| `ui` | **everything** — FFmpeg, D3D12, Skia | very long, but it is the one the app needs |
| `ci` | what the GitHub workflow runs | — |

```
cmake --preset ui
cmake --build build/ui --config Release
ctest --test-dir build/ui -C Release
```

Media tests need real footage, which is not in the repository. Point
`CUTLINE_TEST_MEDIA_DIR` at a directory containing `Boiler.mp4` to run them;
without it they skip rather than silently pass.

### Tools

```
build/ui/tools/cutline/Release/cutline.exe                # THE APPLICATION
build/media/tools/render_frame/Release/render_frame.exe <project.json> <seconds> <out.png>
build/media/tools/export_project/Release/export_project.exe <project.json> <out.mp4>
build/media/tools/play_project/Release/play_project.exe <project.json>
build/media/tools/decode_bench/Release/decode_bench.exe <file> [frames]
build/media/tools/preview_window/Release/preview_window.exe <file>
```

`cutline` is the editor. It was called `theme_window` until it was renamed,
having started as a harness for the theme layer and grown into the application
while keeping the name; anything older than that commit refers to it by the old
one. It takes:

- `--check [dir]` — headless: builds the whole interface in every theme, lays
  out every panel, paints it, and reports widget counts plus a per-theme pixel
  fingerprint. It fails on a widget that landed nowhere, one outside the window,
  and one cut in half by the panel holding it. Given a directory it writes each
  theme's frame as a PNG. **Run this before every commit.** Exit code 0 or 1.
- `--benchmark` — layout and paint timings per theme and per window size.

`preview_window` is a throwaway viewport for driving the render pipeline by hand.
It is not the editor's UI and will not become it.

---

## 3. Layers, and the rule that holds them apart

```
core     pure model: clips, tracks, effects, keyframes, editing operations,
         serialisation, undo. No dependencies at all.
audio    pure DSP: biquads, compressor, limiter, WSOLA time stretch.
render   pure: plan a frame, resolve an effect stack, mix. Depends on core.
media    FFmpeg: probe, decode, encode, resample, peaks, thumbnails.
gpu      Direct3D 12: the compositor and the presenter.
text     Skia: rasterises a title.
ui       widgets, layout, themes, painters. Depends on core (for time) only.
editor   bindings: turns a project into what a panel shows, and a gesture into
         an operation. Depends on core and ui. Pure.
engine   frame renderer, exporter, player. Joins render + media + gpu.
app      preview, and the waveform and thumbnail caches: the whole stack, and
         the work the interface waits on without doing itself.
tools    executables.
```

**The rule: everything that can be pure, is.** The model does not know what a
widget is; the widget layer does not know what a project is; `editor` is the only
place that knows both, and it is pure too. That is what makes 1467 tests run with
no GPU, no window and no media, in five seconds.

The one deliberate exception: `ui` depends on `core` for frame durations and
timecode. Reimplementing those to keep a layering rule that costs nothing to
break would be silly.

**Where things go:**

- A new *editing operation* → `core`, pure, tested against the spec's numbers.
- A new *effect* → `render/src/effects.cpp` **and** `render/effect_catalog.cpp`.
  Both, always. A test pushes every catalogue entry to its maximum and fails if
  the resolved result is unchanged, which is what catches a mistyped key.
- A new *control* → `ui/controls.hpp`. Themeable, tested with `RecordingPainter`.
- *What a panel shows* → `editor/*_binding.hpp`, as plain data.
- *Wiring* → `tools/cutline/main.cpp`. It is large; that is fine, it is the
  composition root.
- *Something the interface needs but must not wait for* → `app`, as a cache with
  a worker behind it. `WaveformCache` is the one to copy; `ThumbnailCache` is
  what copying it looks like.

---

## 4. House style

The code is written to be read. Match it or the next person cannot tell which
decisions were deliberate.

**Comments say why, never what.** Every non-obvious decision carries the reason
it was made and, where it applies, what went wrong without it. Look at
`src/ui/include/cutline/ui/color_picker.hpp` or the top of `timeline.hpp` for the
register. A comment restating the code is worse than none.

**Test names are sentences.** `AnEffectWithNothingToSetArrivesDoingIt`,
`ADragBelongsToTheRegionItStartedIn`, `KeepsItsHueThroughBlack`. A failing test
should read as the claim that broke.

**Commit messages are prose.** Subject line in the imperative, then paragraphs
explaining the decisions and the things that turned out to be wrong. `git log`
is the design record.

**British spelling in prose and identifiers** where it is a real word —
`colour_row`, "behaviour", "initialise". Not in API names that mirror an
external convention (`Color`, `set_color`) — the widget layer uses `Color`
because that is what the painter and every colour literal call it.

**No emojis anywhere in the interface.** Text or drawn shapes only. This is the
user's standing instruction, and it is also why every icon here is lines: no font
can be relied on to have an arrow, a padlock or an eye, and the ones that do
disagree about size and baseline.

**Themes change chrome, not colours.** A theme owns fills, bevels, borders,
corner radii, shadows *and metrics* — bevelled chrome needs different spacing
than flat chrome. A widget that reached for a colour directly would be the one
control no theme could restyle, and there is no way to find those except by
looking. There are four: `flat` (Slate), `xp` (Luna), `aero`, `terminal`
(Phosphor).

**One gesture is one undo entry.** Views update themselves live so a drag can be
seen, and report once on release. `Slider`, `TextField`, `ColorPicker` and
`TimelineView` all split `on_change` from `on_commit` for this reason.

**Editing operations are `(project, args) -> project`** and return the project
*unchanged* when they cannot apply. That is what lets the session skip the undo
entry, and it is why the view rebuilds after every gesture whether or not the
edit took.

---

## 5. How work gets verified

Three layers, and the third one is not optional.

**1. Unit tests.** GoogleTest. The widget layer uses `RecordingPainter`, a real
backend that outputs a list of draw calls — "does an XP button draw its bevel
inset when pressed" is an ordinary assertion, not a screenshot to squint at.

**2. `cutline --check`.** Builds the real interface in all four themes with
a real Skia painter, activates every panel in turn, builds an inspector with a
clip selected and all eleven effects on it, animates two parameters, selects a
title, opens the colour picker, and asserts every widget landed somewhere real
and inside the window. Then it fingerprints the pixels and fails if two themes
painted identically.

**3. Driving the actual window.** Synthetic clicks and screenshots. This has
found things the other two could not, every single time:

- **No `WM_CHAR` handling existed at all.** Nothing had ever called
  `host->text()`, because until titles there was nothing to type into. A focused
  field showed a caret, took arrow keys, and swallowed every letter.
- **Focus did not leave a field on a click elsewhere**, so a multiline title —
  which writes to the document only when focus goes — could never be committed.
- **The compositor's shaders were never copied next to the window**, so the
  preview in the actual application had never once rendered a frame. It failed
  at "cannot open composite_layer_vs.cso" and showed the colour bars it starts
  with, which look exactly like a preview waiting for something to decode.

The driver lives in the session scratchpad, not the repo. It is about forty lines
of PowerShell: `ClientToScreen` for the origin, `SetCursorPos` + `mouse_event`,
`CopyFromScreen` cropped to the client rect. **Watch the DPI.** Whether
PowerShell launches DPI-aware varies between invocations; when
`[System.Windows.Forms.Screen]::PrimaryScreen.Bounds` reports 3840×2160 you are
in physical pixels and when it reports 3072×1728 you are in logical ones, and a
1.25 factor between the screenshot and the click makes every press land a quarter
of the way down and to the right. Verify the mapping with a hover before
trusting a click, and check `WindowFromPoint` is the app before pressing — the
desktop steals the foreground and you will otherwise be typing into somebody's
terminal.

---

## 6. Status

Phases 1, 2, 3 and 6 are complete; 4, 5 and 7 are underway. Concretely:

**Works end to end.** Import, place, move, trim, split, ripple, undo/redo. Eleven
video effects as shaders, stackable, reorderable, keyframeable. Titles, drawn
with Skia and composited as premultiplied RGBA. Playback at rate against the
audio clock (57 fps, 95%, on a 4K60 project). Export with hardware encode, chosen
at runtime — NVENC, QSV, AMF, then x264/x265 — with progress and cancel. Project
save/load. Four themes, dockable panels that tear out into their own windows and
remember where they were.

**The interface** is one window drawn on the GPU with Skia, sharing the
compositor's Direct3D device so a decoded frame reaches the screen without a copy
through system memory. The tool palette (select, razor, rate stretch, slip,
slide), the effect controls with stopwatches and keyframes, a colour picker, and
track header switches all work.

---

## 7. What is left

Checked against `docs/spec.md` §21 (the parity checklist) and §18 (the UI spec).
It splits sharply.

### A. Built and tested in the model or engine, unreachable from the interface

The cheap half. Each is a panel or a gesture away, not new machinery. **Start
here.**

| | Exists | Missing |
|---|---|---|
| **Renaming a track** | `set_track_label` in core | no way to type one in |
| **Snapshot to PNG** | the whole path (`render_frame` does it) | no button |

### B. Not built anywhere

- **Scopes** — histogram, waveform, parade, vectorscope. Nothing in any layer.
- **VU meter and master volume** — there is no master volume in the model at
  all; the limiter is fixed at 0.95.
- **Program-monitor transform handles** — drag to move, corners to scale, top to
  rotate, plus edge and centre snap guides.
- **Preview resolution** (Full/½/¼) and **loop playback**.
- **J/K/L shuttle**, I/O keys, marker keys, S for snap.
- **Copy/paste an effect stack**; aspect lock on scale.
- **Canvas presets / sequence settings** — the export dialog can resize, but the
  project's own canvas cannot be changed.
- **Autosave**, and **auto-update**. The spec calls the updater optional; the
  person who uses this relies on painless updates, which is the argument for it.
- **Dragging keyframes in time**, and **keeping hardware-decoded frames on the
  GPU** instead of uploading them from system memory (the decoder can already
  produce D3D12 textures; nothing samples them yet).

### Suggested order

1. Anything in B, by appetite. Scopes and the VU meter are the two that need new
   machinery rather than new wiring.

And one thing worth doing whatever comes next: **nothing in the test suite ever
resizes a real window.** The pixel tests draw on a fixed CPU raster surface and
`--check` lays out at one size, so the whole `WM_SIZE` → recreate-the-swapchain
path had never been exercised until somebody dragged the window under a driver —
which is how the crash in *Traps* was found, after it had been shipping happily
past a green suite for weeks.

Nine are already done and are worth reading as the pattern for the rest:

- **In and out points** — marked from the buttons or from I and O, drawn along
  the foot of the ruler, saved with the project, offered to export as "only the
  marked range".
- **Transitions** — `editor/transitions.hpp`. The interesting part is not
  setting one, it is knowing whether one *would do anything*: an overlapping
  kind borrows unused source from each side of the cut, and a clip trimmed to
  the last frame of its footage has none to lend. The resolver skips such a
  transition in silence, so the panel asks first and says "no handles" rather
  than offering a slider that changes no pixels. **Expect the same shape
  elsewhere** — the model will happily store things the renderer ignores, and
  the binding layer is where that gets caught.
- **Audio effects** — the bottom of `editor/effects_binding.hpp`. Worth reading
  for how little there was to write: the audio registry in `cutline::audio` had
  declared every effect's parameters, ranges, defaults and units all along, in
  exactly the shape the video catalogue uses, so the binding is a loop and the
  panel is the same loop the video stack already had. **Look for the registry
  before writing one.** An earlier draft of this document said the eight audio
  effects "need a catalogue of their own first"; they did not.
- **Markers** and **interpolation chips** — both small. The marker keys reuse
  the toggle idiom the in and out points established: pressing the key where the
  thing already is takes it away, so no control exists only to undo another.
  The chip is one setting per property rather than per keyframe, which is what
  the reference exposed and what anybody actually wants.
- **Colour mattes and adjustment layers** — `editor/generators.hpp`, alongside
  `titles.hpp`, which is the same shape. Both set `has_video`, which means
  "contributes picture" and is what `place_media` checks; an adjustment layer
  contributes a filter rather than a picture and still needs the flag, which is
  exactly the sort of thing that gets set honestly and breaks.
- **Waveforms** — `app::WaveformCache` and the drawing in `ui/timeline.cpp`. The
  first thing in the interface that waits on work it did not do, and the shape it
  settled on is the one thumbnails should copy. Three parts of it are load
  bearing: the envelope is shared by pointer, because the model is rebuilt after
  every gesture and a ten-minute source's peaks are half a megabyte; it describes
  the *source*, with the clip's `source_in`, `speed` and `reverse` on the block
  doing the mapping, so trimming or retiming costs nothing; and the worker posts
  a window message rather than setting a flag, because the loop blocks on its
  queue when nothing is playing and a polled flag would show the waveform at the
  next mouse move.
- **Multi-select** — `TimelineView::selection` and the `Marquee` mode. Worth
  reading for what it did *not* need: everything above the view already took a
  list, so this was the view catching up rather than a change to the model. The
  sweep catches what it *touches* rather than what it encloses, because a clip
  wider than the window can never be enclosed and that is exactly when somebody
  reaches for a sweep.
- **Thumbnails** — `app::ThumbnailCache` and the tiling in `ui/timeline.cpp`.
  Copied wholesale from the waveform cache, which is the point: the only real
  differences are that filmstrips are pixels, so the cache is bounded and evicts
  the least recently wanted, and that the strip describes the *source*, so a
  short clip of a long file may repeat a frame. That is a deliberate limit —
  sampling finely enough to show each instant would mean holding a decoded film
  in memory, and the strip's job is to say what the footage is.
- **The volume rubber band** — `GainBand` in `ui/timeline.hpp`. The whole of it
  was drawing and gesture: `move_gain_keyframe` and `set_clip_gain` had been in
  the core, tested, since phase 1. **Check the core before assuming a feature
  needs one** — this is the second time an "unreachable" item turned out to be
  wiring only. The part worth reading is the scale: gain is a linear multiplier
  and the band is decibels, because on a linear scale every trim anyone makes
  lands in the top few pixels of a forty-pixel clip.

---

## 8. Traps

Each of these cost real time. They are documented in the code as well, but they
are the sort of thing that bites twice.

**A reference bound to a *member* of a temporary is not lifetime-extended.**
`const auto& x = timeline_model(p).tracks[0].something;` reads freed memory. This
has caught **three** tests in `tests/editor/`, each time passing until an
unrelated change moved the heap under it. There is a note at the top of
`timeline_binding_test.cpp`. Hold what a by-value function returns by value.

**Video tracks are stored top-first and composited bottom-first.** A test that
puts the background track first will have it paint over everything.

**Transforms are canvas fractions, not pixels.** Scale 1 means aspect-fit to the
canvas. This is why exporting at half resolution is the same picture smaller
rather than a crop, and it is pinned by `ASmallerCanvasIsTheSamePictureSmaller`.

**`Media::has_video` means "contributes picture"**, not "came from a video file".
`place_media` checks it, and a title with it false is silently refused.

**The model stores things the renderer then ignores.** A transition at the end of
a track, a transition with no handles to borrow, a mark past the end of the
sequence — all storable, all silently skipped. Whenever you expose something new,
check what the *resolver* does with the edge cases before deciding what the panel
offers, and put the answer in the binding layer where both can see it.

**A row of controls can be wider than the panel holding it, and nothing in the
widget tree minds.** The last one is simply cut in half on screen. `--check`
catches this now — it walks with the nearest clipping ancestor and counts
anything sticking out of it horizontally — but only because a third button in the
project panel's toolbar went missing that way first.

**Measure text before you centre it.** A label centred in a box too small for it
overflows both ends, and what survives the clip is a word missing its first and
last letters sitting on top of whatever is behind. `Painter` is a `TextMeasurer`,
so `paint_content` can always ask.

**A `can_run` that says no must mean a `run` that does nothing.** There is a test
that walks every command and asserts the two agree; adding a command whose `run`
succeeds where `can_run` refused fails it. Express the condition once and have
both call it.

**A panel rebuild destroys the widget whose handler is running.** The inspector
is rebuilt by setting `app.inspector_stale` and letting the frame loop do it, for
exactly this reason. A `ColorSwatch` commit deliberately does *not* mark it
stale: rebuilding would destroy the swatch, a destroyed swatch closes the picker
hanging open above it, and a colour could then only ever be chosen once per
opening.

**A popup is closed at the next layout, not on the spot**, because what asks for
it is very often a button inside the popup running its own click handler.

**Blending is linear; effects are not.** A 50% dissolve passes through the true
midpoint. Effects run on coded values, where FFmpeg's filters are defined and
where the spec specifies them. Each operation happens in the space it was defined
in. Do not "fix" either one.

**Flush Skia's recorded work *before* destroying what it drew into.** Resizing
the swapchain crashed the application — an access violation in `skia.dll`, seven
fresh instances out of seven — because `SkiaWindow::resize` dropped the wrapped
surfaces and only then called `flushAndSubmit`, submitting commands that
referred to objects already gone and leaving the context holding render targets
for buffers the swapchain was about to replace. It survived the resize itself
and died in the *next* `present`, which is a long way from the cause. The order
that works is: flush and submit, wait for the GPU, drop our surfaces, purge
Skia's cached copies, then `ResizeBuffers`.

Whether it faulted depended on what the driver put in the freed memory, so it
was three times in five rather than every time — and once tracing was added it
sometimes stopped happening at all. Timing-sensitive crashes that go away when
you look at them are still real.

**The frame loop blocks on its message queue whenever nothing is playing.**
Anything finishing on a worker has to *post* rather than set a flag for the loop
to notice, or it appears at the next mouse move instead of when it was ready.
`kWaveformReady` is the pattern.

**Two test fixtures with the same name in one binary is a GoogleTest failure,
not a shadowing.** `tests/app` links every file into `cutline_app_tests`, and a
second `WithFootage` in an anonymous namespace still collides — the suite is keyed
by name. It fails at run time with "all tests in the same test suite must use the
same test fixture class", which reads like a compiler problem and is not.

**Reading a keyframe list while adding to it reads an unsorted list.**
`ensure_gain_anchors` added the head anchor and then asked what the band was
worth at the tail — and `gain_value_at` walks in time order, so it answered with
the head anchor it had just pushed on the end. Take every value you need before
inserting any of them.

**A gesture has to preview everything it is going to do.** The view moved only
the block under the pointer while a multiple selection was dragged, and the rest
caught up when the model was rebuilt on release — so the drag showed one thing
and the mouse-up did another. Anything a gesture will change on release it has to
change while the mouse is still down, from origins captured at the press.

**A press must not rewrite the selection it might be about to drag.** Taking
hold of one clip of a multiple selection threw the rest away before the drag
began, so a selection could be made and never moved. A press on something
already selected leaves the selection alone; the *release* collapses onto it
when no drag happened, which is the way back from several to one.

**A drag threshold measured along one axis stops a vertical gesture starting.**
The timeline's drags were all horizontal until the volume band, so `moved_` was
`abs(x - press_x)`. A band pulled straight down travels no distance in x, and the
gesture sat there refusing to begin. The volume modes measure both axes.

**A reference into a vector you are about to sort is not the element any more.**
Dragging a rubber-band point past its neighbour re-sorts the list, so the point
is held by value across the sort and found again afterwards. Every reader of a
keyframe list — the drawing, the core's evaluator — assumes it is in order, and
this is the one gesture that can break that.

**HSV is a lossy view of a colour.** Black is every hue at once and a grey has no
saturation to read back, so `ColorPicker` holds its coordinates rather than
deriving them. Any control that round-trips through a colour will lose what the
user just chose.

**`Sleep(1)` is 15.6 ms** at Windows' default scheduler tick. `timeBeginPeriod(1)`
took the preview from 33 to 57 fps.

**A sub-frame step can read as a seek.** Decoding stops at the first frame
reaching the requested time, so the decoder sits up to one frame *ahead*, and a
later request closer than that overshoot looks like a move backwards. Tolerating
a backwards move smaller than one frame took compositing from 23 ms to 2 ms.

---

## 9. Working with the person who owns this

- **They write the direction; you write the code.** They review lightly and
  expect the tests to be the proof. Do not ask for approval on ordinary
  decisions — make the call, state it plainly, and say why.
  **Give options when a decision is needed.** Prompt them with a decision and
   the pros and cons of each choice.
- **Verify on screen before claiming something works.** Three real bugs shipped
  past a green test suite. If the desktop makes that impossible, say so rather
  than implying it was checked.
- **Only tag or push a release after major features are done**, not per commit.
  Ordinary commits to `main` are expected and fine.
- **Correct the README as you go.** It is the status document, and it has
  claimed things that were not true more than once.
- Be direct about what did not get done and why. A list of remaining gaps is
  more useful than a summary that reads as finished.
