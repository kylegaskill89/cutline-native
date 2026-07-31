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
| Tests | **1578** under the `ui` preset; 1338 of them need no GPU, no window, no FFmpeg |

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
binding between them — 1338 of the 1578 tests.

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
build/ui/tools/theme_window/Release/theme_window.exe      # THE APPLICATION
build/media/tools/render_frame/Release/render_frame.exe <project.json> <seconds> <out.png>
build/media/tools/export_project/Release/export_project.exe <project.json> <out.mp4>
build/media/tools/play_project/Release/play_project.exe <project.json>
build/media/tools/decode_bench/Release/decode_bench.exe <file> [frames]
build/media/tools/preview_window/Release/preview_window.exe <file>
```

`theme_window` is badly named — it started as a theme harness and became the
editor. Renaming it is on the list and nobody has bothered. It takes:

- `--check [dir]` — headless: builds the whole interface in every theme, lays
  out every panel, paints it, and reports widget counts plus a per-theme pixel
  fingerprint. Given a directory it writes each theme's frame as a PNG.
  **Run this before every commit.** Exit code 0 or 1.
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
app      preview: the whole stack, project in, texture out.
tools    executables.
```

**The rule: everything that can be pure, is.** The model does not know what a
widget is; the widget layer does not know what a project is; `editor` is the only
place that knows both, and it is pure too. That is what makes 1261 tests run with
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
- *Wiring* → `tools/theme_window/main.cpp`. It is large; that is fine, it is the
  composition root.

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

**2. `theme_window --check`.** Builds the real interface in all four themes with
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
- **The compositor's shaders were never copied next to `theme_window`**, so the
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
| **Colour mattes, adjustment layers** | the compositor renders adjustment layers; the browser has icons for both kinds | nothing creates one |
| **Gain automation** | rubber-band model, mixer honours it | no way to add or drag a point |
| **Waveforms, thumbnails** | `compute_peaks`, `extract_thumbnails` | the timeline draws neither |
| **Link/unlink, add/remove/rename track** | all in core | no button, no shortcut, no command |
| **Snapshot to PNG** | the whole path (`render_frame` does it) | no button |
| **Fade handles** | fades work, through inspector sliders | not draggable on the clip |

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

1. **Colour mattes and adjustment layers.** Small: two buttons beside New Title,
   and `editor/titles.hpp` is the shape to copy — a generated media the editor
   creates rather than imports.
2. **Gain automation** — the rubber band. Bigger than it sounds: it needs a new
   gesture on the timeline rather than a new panel.
3. **Waveforms and thumbnails on clips.** The media layer computes both; the
   timeline draws neither. Watch the cost — a peak list per clip per zoom is the
   sort of thing that quietly makes scrolling expensive.
4. Anything in B, by appetite. Scopes and the VU meter are the two that need new
   machinery rather than new wiring.

Five are already done and are worth reading as the pattern for the rest:

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
- **Verify on screen before claiming something works.** Three real bugs shipped
  past a green test suite. If the desktop makes that impossible, say so rather
  than implying it was checked.
- **Only tag or push a release after major features are done**, not per commit.
  Ordinary commits to `main` are expected and fine.
- **Correct the README as you go.** It is the status document, and it has
  claimed things that were not true more than once.
- Be direct about what did not get done and why. A list of remaining gaps is
  more useful than a summary that reads as finished.
