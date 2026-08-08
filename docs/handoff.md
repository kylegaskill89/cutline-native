# Handoff

Everything someone picking this up needs: where it is, how to build it, the rules
it is written to, what works, what is left, and the traps that have already cost
a day each.

**If you are picking this up to carry on, read §9 first** — it says exactly
where the work stands and what to do next. The rest is reference.

Read `README.md` for the story and the numbers, `docs/architecture.md` for the
native decisions, `docs/spec.md` for the product — it is the authority on exact
numeric behaviour, carried over from the TypeScript version — `docs/releasing.md`
for how a tag becomes an installer somebody can run, and **`docs/premiere-gaps.md`
for the live plan**. That last one is the one that matters day to day: the spec
is met, and Premiere is the target now.

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
| Released | **0.3.0**, as an unsigned NSIS installer. `docs/releasing.md`. 29 commits since the tag, unreleased |
| Local | `d:\Videos\cutline-native` |
| Old app | `github.com/kylegaskill89/cutline` — dead, kept as reference |
| Old app, local | `d:\Videos\VideoTrimmer` — holds `design.md` (the rewrite spec) and `summary.md` |
| Size | ~61k lines of source, ~39k of tests |
| Tests | **2772** under the `ui` preset; 2392 of them need no GPU, no window, no FFmpeg |

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
binding between them — 2392 of the 2772 tests.

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

### Packaging

```
cmake --preset ui
cmake --build --preset ui
cd build/ui && cpack -G NSIS -C Release   # needs `choco install nsis`
```

Or push a tag and let the Release workflow do all of it. `docs/releasing.md` is
the whole process, including why the tag has to match `CMakeLists.txt`.

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
ui       widgets, layout, themes, painters. Depends on core (for time) and
         render (for what a scope measures). Both pure.
editor   bindings: turns a project into what a panel shows, and a gesture into
         an operation. Depends on core and ui. Pure.
engine   frame renderer, exporter, player. Joins render + media + gpu.
app      preview, the waveform and thumbnail caches, the media cache they keep
         their answers in, and the proxy builder: the whole stack, and the work
         the interface waits on without doing itself.
tools    executables.
```

**The rule: everything that can be pure, is.** The model does not know what a
widget is; the widget layer does not know what a project is; `editor` is the only
place that knows both, and it is pure too. That is what makes 2392 tests run with
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
  what copying it looks like, and `ProxyBuilder` is what copying it looks like
  when the job takes minutes and its result outlives the session.
- *Something derived from footage that is worth keeping between sessions* →
  `app/media_cache.hpp`, which both of those caches consult before they decode.
  Read §5.8 of the gaps document before adding a third kind: the key and the
  unit are the two decisions that go wrong.
- *A preference* → `editor/settings.hpp` and a row on a page in `main.cpp`. §9
  has the recipe and the rule for deciding whether it is a preference at all.
- *A per-sequence setting* → `core::Project`, serialised, with a control in
  Project Settings. `drop_frame` is the most recent one and the shortest to
  read.

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
title, opens the colour picker, opens the Window menu, and lays out **every page
of both settings windows** in a host of its own at the width the dialog opens
at. It asserts every widget landed somewhere real, inside the window, and with
the room it asked for. Then it fingerprints the pixels and fails if two themes
painted identically.

It has earned its place repeatedly and recently: seven frame-rate buttons that
fitted in a popup and not in a window, an explanatory sentence forty points
wider than its pane, a folder path sharing a row with two buttons, and a
dropdown as wide as the longest audio device name on the machine. **Run it
before every commit.** Its project is deliberately awkward — two levels of bin,
a labelled entry, 29.97 so the drop-frame control exists at all — because a page
checked in its emptiest state is a page checked where nothing goes wrong.

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
`CopyFromScreen` cropped to the client rect.

Four things about it that each cost a run to learn:

**Clear the state first, or a run inherits the last one.** Recovery copies put a
modal on top at startup; `workspaces.json` wins over the built-in arrangement;
and `settings.json` now carries the theme, which is the dangerous one — every
theme has its own metrics, so a click computed against one lands somewhere else
under another. `launch.ps1` in the scratchpad clears all three and dismisses
whatever modal comes up; `-KeepSettings` skips the third, for the one case where
the point of the run is that a preference survived a restart.

**`PrintWindow` can return a stale frame.** A status label updating four times a
second looked frozen and blank for a whole session before the tool was
suspected. `CopyFromScreen` showed it working. Use `CopyFromScreen` for anything
that changes without input — a progress line, a count coming down, a playhead
moving on its own — and for popups, which are drawn over the window.

**The dialogs are their own top-level windows.** Points computed against the
main window will not land on Preferences or Project Settings, and the helper is
right to refuse them: find the dialog's `HWND` by title with `EnumWindows` and
drive it in its own client coordinates.

**Check `WindowFromPoint` is the app before pressing.** A blind screen-coordinate
click during this stretch went to a background application because the editor did
not have the foreground. Nothing was harmed, and the helper that asserts the
target is ours is the reason it is knowable.

**Assert the state you set, do not assume the click took.** Two separate runs
were spent measuring a loop that was not looping, because the Loop button had
been clicked and had not come on — the button moves when its label changes
between Play and Pause, so a coordinate that worked a moment ago lands beside
it. Reading one pixel of the button's fill answers it: lit is the theme's accent
blue, and it costs nothing next to a twenty-second measurement of nothing.

**Three traps in the driver itself**, each of which looked like a fault in the
application. `GetWindowTextW` marshalled with a plain `StringBuilder` comes back
cut at the first UTF-16 null, so every window title reads as its first letter
and nothing matches — it needs `CharSet.Unicode`. An `EnumWindows` callback
passed as a PowerShell scriptblock silently records nothing, so the enumeration
belongs in the C# helper. And **PowerShell parameter names are case-insensitive**:
a local `$h` inside a function taking `[IntPtr]$H` *is* the handle, so assigning
a height to it corrupts the window being drawn.

**Two things called an index agree until they do not.** `render::plan_audio`
numbers *audio* tracks 0, 1, 2 and skips the video ones; everything else in the
application counts tracks as the project lists them. The two are identical for
any project that is all audio — which is what every mixer test was — so a mixer
strip asked for lane 1 in an ordinary V1+A1 project, got silence, and its meter
simply stayed dark past a green suite. If a number could be counted two ways,
the test that catches it is the one with *both* kinds present.

**To fill a pool, paste rather than click.** The file dialog's filename box
accepts a whole multiple selection as quoted names separated by spaces —
`"a.mp4" "b.mp4" …` — so navigating to the folder and pasting forty of them
puts forty files in with two keystrokes. Setting the clipboard needs
`powershell -STA`; the default apartment for a `-File` run will not do it. This
only works because `OFN_ALLOWMULTISELECT` is set, so it is also the shortest
check that the flag is still there.

**Watch the DPI.** Whether
PowerShell launches DPI-aware varies between invocations; when
`[System.Windows.Forms.Screen]::PrimaryScreen.Bounds` reports 3840×2160 you are
in physical pixels and when it reports 3072×1728 you are in logical ones, and a
1.25 factor between the screenshot and the click makes every press land a quarter
of the way down and to the right. Verify the mapping with a hover before
trusting a click, and check `WindowFromPoint` is the app before pressing — the
desktop steals the foreground and you will otherwise be typing into somebody's
terminal.

### Waiting to be driven

Driving needs the foreground, so it cannot happen while the machine is busy with
something else. What has shipped on tests and `--check` alone is listed here
rather than left implicit, and the list is emptied by driving it, not by
deciding it is probably fine.

**Waiting now:**

- **A long source playing with sound, for longer than a window lasts.** Audio
  is windowed now, so a capture longer than two minutes is read a stretch at a
  time by a reader thread while the mix walks through it. The seams are covered
  by tests — the samples either side of a refill, and that the block size cannot
  change them — but *hearing* seventy-five seconds go by and the next stretch
  arrive is the check no test makes. Play a ten-minute capture through and
  listen for a gap at about a minute in.
- **The audio device actually carrying sound.** The page lists this machine's
  real outputs and stores a choice by id; what was *not* driven is pressing play
  through a chosen one, because that moves somebody's monitoring to another
  device to prove a thing a test can prove. The fallback (an id no machine has
  still opens a working player) and open-by-id are covered in
  `tests/engine/player_test.cpp` instead. If you have the machine to yourself,
  this is ten seconds of work and worth closing.
- **A source placed as it is marked** — that insert, overwrite and a drag from
  the pool all put down the same span.
- **The application running on WARP, for more than a few minutes.** Choosing the
  software renderer is new, and it works: the whole interface, Skia included,
  draws correctly on it and the Graphics page names it. But once during a long
  driven session on WARP the main window's client area went black and stayed
  black, with the process responsive and burning no CPU, and the dialog over it
  black too. It did not reproduce — the same actions afterwards were handled
  correctly, and a fresh run sat on WARP for twenty seconds idle and through six
  category switches with no trouble. Recorded rather than diagnosed, and worth a
  session of its own before anybody leans on the software renderer. Timing-
  sensitive faults that go away when you look at them are still real.

#### Driven: forty in the pool, and the dialog that made it hard to get there

The icon view had only ever been driven with two sources. Forty is where the
filmstrip requests and the eviction budget were supposed to meet, and getting
forty in was the first thing that went wrong: the import dialog took one file,
so filling the pool meant forty trips through it, and a driver script doing
that raced the dialog and gave up. That is not a scripting problem. Anybody
with a folder of rushes has the same forty trips. The dialog takes a multiple
selection now, and the whole pool goes in as one edit that one undo takes back.

What forty then showed, on 4K copies of an eight-second capture:

- **Nothing thrashes, and eviction earns its keep.** Tiles filled top first, in
  the order they are shown, and only the ones on screen were asked for at all.
  Memory rose to ~1.36 GB partway through and came back **down to 1.23 GB** by
  the time the visible tiles were done, so the budget is releasing what it
  should rather than fighting the requests. The worry that put this on the list
  was the wrong worry.
- **It is slow, and it gets slower.** The first six tiles took about a minute,
  ten seconds each. The twenty visible ones took about **nine minutes** in
  total — nearer twenty-five seconds a source by the end. Nothing blocks: the
  interface stays responsive throughout and the strips arrive where you are
  looking first. But a pool that takes most of ten minutes to finish drawing
  itself is not what "browse your footage" should feel like, and the cache only
  helps the second time. That it *decelerates* is the part worth chasing — a
  fixed cost per source would not do that.
- **Memory is not mostly the filmstrips.** It was already 1.36 GB with *one*
  strip extracted. Forty sources in the pool cost that much before any of them
  is looked at, which is a separate thing to go after and a bigger one.

One fault fell out of it that is nothing to do with size: **undoing an import
leaves the source monitor showing the source it removed.** The pool empties,
the title loses its asterisk, and the monitor goes on displaying a frame from
media the project no longer has. It predates the batch import — a single-file
import undone does the same — and it is a refresh that does not check that what
it is showing still exists.

#### Measured: what reading a source actually costs

Asked whether a media cache would smooth playback. It would not — playback is
video-decode bound and reads none of this — but measuring the question found
that importing the reference ten-minute four-stream capture took **over eleven
minutes** of reading before its waveforms and filmstrips were done, and that
almost all of it was waste: every packet in the container was demuxed and
discarded to reach one stream, and the file was read once per stream. With the
other streams refused at the container and all four read in one pass, four
waveforms went from minutes to **1.4 s**. The full write-up is §5.8 of the gaps
document.

Two things about measuring it are worth keeping, because both cost a run.

**Measure the parts before believing a theory about the whole.** The audio fix
looked like it had barely helped when the end-to-end number only moved from
"over eleven minutes" to nine — and timing the pieces separately showed audio
was then 1.4 s and the filmstrip 141 s, which is a completely different problem
from the one being guessed at.

**A GUI measurement on a shared machine is not a measurement.** Three runs were
spent on numbers that turned out to mean nothing: the window opened at a size
the script did not expect, the theme changed under it — every theme has its own
metrics, so every coordinate moved — and the owner was using the desktop. The
numbers that settled it came from a throwaway test driving `WaveformCache` and
`ThumbnailCache` directly, with no window at all. **Reach for that first when
what is being measured is not itself a matter of pixels.**

#### Driven: the loop that never came round

Closing §5 added a preroll and a postroll around a looped range, and driving
them found something much older underneath. The loop's end is measured against
the player's reported position, and postroll pushes that end out to the end of
the sequence — where the player stops itself, clears `running`, and is therefore
no longer "playing". Both the guard at the top of `advance_playback` and the
frame loop's own condition asked exactly that question, so the end-of-playback
handling was unreachable at exactly the moment it had something to do, and the
frame loop went back to blocking on its message queue with a loop still to come
round. **Looping a range that ran to the end of the sequence had never
restarted**, marks or no marks; the marked-range case worked only because the
end usually arrived early.

Under it was a second fault that the first one hid completely: `Player::seek`
cleared the at-the-end flag on the render thread when the flush landed, and
`Player::play` sends a finished player back to zero — so `seek` then `play`,
which is precisely what looping does, would have discarded the position asked
for. It could not be observed while the code that does it never ran.

Measured by sampling the playhead's own pixels off the screen every 20 ms, on a
range marked 4–6 s in an eight-second sequence:

| | loop start | loop end | period |
|---|---|---|---|
| no preroll or postroll | 3.99 s | 5.97 s | 2.0 s |
| 1.5 s preroll, 2 s postroll | 2.46 s | 7.99 s | 5.5 s |

The second row is the first widened by exactly what was asked for, and before
the fix it played once to 7.99 and sat there for the whole twenty-two seconds
of the measurement.

Two things about the measurement are worth keeping. **Sample the playhead, not
the picture** — finding the reddest column in a one-pixel row of the timeline
gives the position in seconds directly, and a wrap is a backwards step in that
series rather than something to infer. And **assert the control case**: the
zero row is what says the widening is doing the widening, and it is the row
that would have caught a preroll applied twice.

#### Driven: playback, and the hitch the average hid

The question the list posed was whether the seek at the end of a run of
remembered frames *shows*. It does, and plainly.

Measured by sampling a strip of the picture off the screen every 6 ms and
recording when it changes, which is the only way to see evenness — an average
frame time cannot distinguish thirty even frames from twenty even ones and a
stall.

| | forward | reverse (J, 1x) |
|---|---|---|
| distinct frames in 10 s | ~400 | 240 |
| median gap | 24 ms | 30 ms |
| p95 gap | 36 ms | 187 ms |
| worst gap | 38 ms | 424 ms |
| stalls over 250 ms | 0 | 8 |

**Forward is even** — nothing above 38 ms — but runs at about 40 fps on 4K60
rather than 60. Worth a look on its own account; it is not what makes anything
stutter.

**Reverse stalls on a metronome.** The stalls land at 200, 734, 1244, 2795,
3329, 3850, 4389, 4898 ms and so on: a gap of **509–540 ms, over and over**,
each one costing a mean of 264 ms. Half the wall clock is spent stopped.

That period is the run of remembered frames being used up. `frames_to_keep`
gives about thirty frames at 4K against the 384 MB budget, and thirty frames of
60 fps content played backwards at 1x is half a second — which is the number
measured, to within the sampling. Pressing J twice to shuttle at 2x makes them
more frequent, as a run-boundary explanation predicts and a random-hiccup one
does not.

So the cache did what it was measured to do and the average was honest: most
frames became cheap. What it did not do was make playback smooth, because the
cost it removed from twenty-nine frames it left, whole, on the thirtieth.
**The fix is to stop doing the work in one lump where it can be seen** — not to
make the lump smaller.

#### Driven again: the run decoded ahead of need

Same measurement, same footage, after spreading the work across the frames
still to be shown:

| | one run | + prefetch |
|---|---|---|
| distinct frames in 10 s | 241 | 261 |
| median gap | 30 ms | 36 ms |
| p95 gap | 187 ms | **84 ms** |
| worst gap | 424 ms | **297 ms** |
| gaps over 100 ms | 16 | **7** |
| gaps over 250 ms | 8 | **1** |

The median went *up* and that is the trade working: every frame now carries a
slice of the next run's decoding, and in exchange the stalls that were arriving
twice a second are down to one in ten. Forward is untouched — 315 distinct
frames against 319, nothing above 50 ms — because the prefetch only runs when
the requests are moving backwards.

#### Driven a third time: a decoder of its own

The run was 22 frames because two runs shared one decoder's pool, and that
pool has a ceiling the driver does not document. **The ceiling is per decoder**,
which was the thing missed: a second decoder that does nothing but read ahead
gets a pool of its own, and runs go to 32.

| | one run | + prefetch | + a second decoder |
|---|---|---|---|
| distinct frames in 10 s | 241 | 261 | **~345** |
| median gap | 30 ms | 36 ms | **30 ms** |
| p95 gap | 187 ms | 84 ms | **49 ms** |
| gaps over 100 ms | 16 | 7 | **~4** |
| gaps over 250 ms | 8 | 1 | **~2** |

The median penalty is gone as well, because the prefetch no longer takes the
serving decoder away to do its reading.

And the stalls stopped *bunching*. A shuttle that advances by however long the
last turn took will, after a quarter-second hitch, jump fifteen frames — half a
run — so the next stretch has half as many turns to read ahead in and stalls
sooner. Driven, that was visible as hitches at 10832, 11396, 11881, 12941,
13493 ms, closing in. Capping how far one turn may advance broke the loop:
182, 4577, 8922, 11177, 13334 — evenly spaced. Nothing is out of sync as a
result, because J and L are a silent shuttle with no clock to agree with.

Still not perfect: one hitch every two to four seconds, where it was two a
second. What is left is a group of pictures occasionally running longer than
the run has turns to pay for, and the honest next step is measuring GOP length
rather than guessing at the budget again.

Before those, the last of it — the cursors, the snap line, and the two
clip-menu framing rows — was driven and is recorded below.

The cursors needed a way to check them that a screenshot cannot give: neither
`PrintWindow` nor `CopyFromScreen` captures the pointer. `GetCursorInfo` returns
the handle Windows is showing, and comparing it against `LoadCursor(IDC_*)`
names it — which is how "the trim zone is eight pixels wide and reads `size-we`
inside it and `arrow` past the end of the clip" was established rather than
assumed.

Two faults found by driving in this stretch, both invisible to the tests, are
the reason the list exists at all: a tab press that no longer marked the window
dirty, and a timecode field that kept the keyboard after Enter and swallowed
every single-letter shortcut.

---

## 6. Status

All eight phases are complete, `docs/spec.md` §21 and §18 are done, and there is
an installer and a release workflow that publishes it. **The spec stopped being
the target some time ago**; `docs/premiere-gaps.md` is the live plan, and it is
walked section by section.

| Gaps section | State |
|---|---|
| §1 Effects | done |
| §2 Timeline | done |
| §3 Monitors | done bar a deferred row (see the audit list at the foot of that file) |
| §4 Project panel | done bar "new sequence from a clip", which needs more than one sequence |
| §5 Application settings | **done** — every row built or declined with a reason, see that file's §5.6 |
| Audio, titles, colour, export, sequences, keyboard customisation | not started |

**Works end to end.** Import, place, move, trim, split, ripple, undo/redo. Eleven
video effects as shaders, stackable, reorderable, keyframeable. Titles, drawn
with Skia and composited as premultiplied RGBA. Playback at rate against the
audio clock. Export with hardware encode, chosen at runtime — NVENC, QSV, AMF,
then x264/x265 — with progress and cancel. Project save/load. Four themes,
dockable panels that tear out into their own windows and remember where they
were.

Scopes, a master fader with a metered bus, transform handles on the picture with
snapping and guides, a preview that renders at a half or a quarter for speed,
effect stacks copied from one clip to another, and a recovery copy of anything
unsaved. Snap and Fit, loop over the marked range, J/K/L shuttle, canvas presets,
renaming a track, and an update check that verifies what it downloads before
running it.

**Since 0.3.0** — the 24 unreleased commits, which are most of what this handoff
is about:

- **Proxies, whole.** `media::write_proxy` transcodes; `app::ProxyBuilder` runs
  them one at a time on a worker; `Project ▸ Make Proxies / Use Proxies / Stop`
  reach it. Size and destination folder are preferences. The renderer defaults
  to *not* using proxies so the exporter cannot pick one up by forgetting.
- **Relinking** — `Project ▸ Relink Media`, repointing one pool entry repairs
  every clip that used it.
- **Bins** — nested folders in the project, saved with it, dragged into,
  renamed, deleted. `core/pool.hpp` is the model, `MediaBrowser` draws the tree.
- **The project panel, rebuilt** — sortable column headings, an icon view whose
  tiles scrub as the pointer crosses them, labels on sources that clips inherit
  at placement, and a right-click menu.
- **`settings.json`** — the preferences file, and everything that now persists.
- **The settings windows** — Preferences and Project Settings as real modal
  windows with categories down the side. Nine preference pages now.
- **Audio device selection**, and **drop-frame timecode** (which fixed a real
  counting bug beneath it).
- **§5 closed** — undo depth, recovery copies kept, preroll and postroll, and a
  GPU/software renderer choice; three rows declined with reasons. Driving the
  last of those found that looping had never restarted at the end of a
  sequence, which is fixed.
- **The media cache**, and the reading it exists to avoid. Waveforms and
  filmstrips are kept between sessions under `%LOCALAPPDATA%`, movable and
  emptiable from Preferences ▸ Media Cache. Under it, two fixes to how a source
  is read that matter more than the cache does: the container's other streams
  are refused rather than demuxed and discarded, and a file's audio streams are
  read in one pass instead of one each.

---

## 7. What is left

`docs/premiere-gaps.md` is the authority. The items below are the ones that are
not in any section's table because they are not Premiere comparisons — they are
things about this codebase.

### A. Would be missed by somebody using this every day

- **A menu bar entry for everything.** The bar exists and is a real menu bar now,
  but a few commands are still only on a button or a key.
- **More than one sequence.** A `Project` holds its tracks directly; there is no
  `Sequence` type. Section 4 ran into this and stopped, and it is written up at
  the end of that section. It is the largest single piece of unbuilt model.

### B. Worth doing, and nobody will notice until it is done

- **Keeping hardware-decoded frames on the GPU** instead of uploading them from
  system memory. The decoder can already produce D3D12 textures; nothing samples
  them yet. This is the last large win left in the preview.
- **Retimed audio is still decoded whole.** A clip with a speed or a reverse on
  it is stretched once, up front, across its whole span — a window cannot be
  handed to something that reads its input end to end. Every other clip is
  windowed now (see §8's note on `AudioMixer`), so this is the one case left
  where a long source costs its whole length in memory.
- **A large pool costs a great deal of memory before anything is looked at.**
  Forty 4K sources in the browser sat at ~1.36 GB with one filmstrip extracted,
  so the cost is in holding forty sources open rather than in the pictures.
  Nobody has taken this apart; it is the largest unexplained number in the
  application.
- **Filmstrips fill a large pool slowly, and slow down as they go.** Ten
  seconds a source at first and nearer twenty-five by the twentieth, so twenty
  visible tiles took about nine minutes. Ordered, evicting properly and never
  blocking, so it degrades well — but browsing a folder of rushes should not be
  a thing you wait out, and the deceleration says something is accumulating.
  Both of these are written up under *Driven: forty in the pool* in §5.
- **Undoing an import leaves the source monitor showing the removed source.**
  Small, visible, and a refresh that does not check what it is showing still
  exists. Also in §5.
- **Reverse playback still hitches** about once every two to four seconds, down
  from twice a second. What is left is a group of pictures occasionally running
  longer than the run has turns to pay for; the honest next step is measuring GOP
  length rather than guessing at the budget again.
- **Forward playback runs at about 40 fps on 4K60** rather than 60. Even, not
  stuttering, but not the rate either.
- **Nothing in the test suite ever resizes a real window.** The pixel tests draw
  on a fixed CPU raster surface and `--check` lays out at one size, so the whole
  `WM_SIZE` → recreate-the-swapchain path had never been exercised until somebody
  dragged the window under a driver — which is how the crash in *Traps* was
  found, after it had been shipping happily past a green suite for weeks.

### C. Patterns worth reading before writing anything new

These are finished features, kept here because each one is the shape the next
thing of its kind should take.

- **Proxies** — `media/transcode.hpp` writes one, `app/proxies.hpp` runs them.
  Three things in it are load bearing. A proxy is *the same footage*: same
  length, same frames at the same times, so frames are held to the source's rate
  by repeating and dropping rather than emitted one for one — without that a
  variable-rate source produces a proxy at the wrong speed and every cut made
  against it moves when the original comes back. Anything that fails or is
  cancelled **removes the file it was writing**, because a proxy that exists is
  one something attaches and cuts against. And `FrameRenderer::set_use_proxies`
  defaults to *off*, so the exporter ignores the switch by doing nothing rather
  than by remembering to turn it off.

- **Bins** — `core/pool.hpp`. A bin holds nothing: it is a name and a place in a
  tree, and what is in it is whatever names it, which is one field on the media
  entry. That is the only arrangement where filing a clip cannot leave two
  containers disagreeing, and it is why deleting a folder cannot lose footage —
  media naming a bin that has gone reads as top level. The destructive half
  (`editor::remove_bin`, which takes the contents and their clips, as Premiere
  does) is deliberately a *different function* from the structural one.

- **The settings file** — `editor/settings.hpp`. The fourth thing under
  `%APPDATA%\Cutline`, beside the workspaces, the effect bins and the presets,
  and those three were the template: a struct, a read that treats absence as
  "nobody has set anything yet", and a write through a staging file. Two rules
  in it are worth keeping: **an absent key takes its default rather than reading
  as false** (absent booleans read as "off" would have switched snapping off for
  everybody on their first upgrade), and **the theme is stored by name, not by
  index** (the list is ordered for reading, so an index would quietly mean
  another theme the first time one was inserted).

- **The settings windows** — `open_settings_dialog` in `tools/cutline/main.cpp`.
  Two windows, not one: preferences belong to the person and project settings
  travel with the cut, and a single window would write half its contents to a
  different place from the other half. Each page fills a container the dialog
  owns rather than being a widget tree of its own, so switching category rebuilds
  one box. They are modal, which on Windows means disabling the owner — and the
  owner is re-enabled *before* the dialog is destroyed, so the system has an
  enabled window to give the foreground to.

- **Drop-frame timecode** — `core/time.hpp`. Read the comment on
  `seconds_to_timecode` before touching anything here. The frame index comes
  from the *actual* rate and the fields are counted at the nominal one; counting
  the index at the nominal rate skips a number about every thousand frames at
  29.97, which is what it used to do. Non-drop therefore falls behind the clock
  and that is correct.

- **The monitor's transform handles** — `ui/monitor.hpp` for the gesture and
  `editor/monitor_binding.hpp` for the conversion. The conversion is the part
  worth knowing: the model stores a *scale* relative to the media's aspect-fit
  size, and the overlay works in canvas fractions, so the two look alike and are
  not — get them the wrong way round and the picture is only wrong when the
  media is not the shape of the frame. The other one is `kMonitorInset`: the
  picture keeps a border because a layer filling the frame has its handles *on*
  the frame's edge, and a press outside a widget goes to whatever is behind it.

- **The master fader and the meter** — `audio/meter.hpp` measures and
  `ui/meter_view.hpp` draws, the same split the scopes use. Two things in it
  are worth knowing. The meter taps the mix *between* the master fader and the
  limiter, so "over" means the limiter is having to work — a meter after it can
  never read above the ceiling and would go quiet at exactly the moment the mix
  got into trouble. And the master is the **one** edit playback survives: a
  fader is set by ear against what is playing, so `AudioMixer` takes it through
  an atomic and `advance_playback` discounts it from the comparison that
  otherwise stops the sound whenever the document changes.

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
- **Scopes** — `render/scopes.hpp` counts and `ui/scopes_view.hpp` draws, and
  the split is the point: the arithmetic is checked against a frame built in
  three lines with no widget, and a scope can never affect an export because the
  only thing that flows between them is a set of tallies. Worth reading for the
  two numbers that are not obvious — the vectorscope's reach, which is 140 and
  not 128 because a saturated primary is further from grey than either axis
  alone, and the drawing's gain, which is what makes a one-pixel cell visible.
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

**A "gather the state and save it" function drops every field it does not
name.** `save_settings` built a fresh `Settings` from the live fields and copied
three others back by hand, so the labels were wiped on every save — found by
renaming one and watching it not stick. It starts from what is already held now,
which makes the *next* setting added safe by default rather than safe if
somebody remembers. Any function shaped like "build a new one from the parts I
can see" has this bug waiting in it.

**`ScrollView` used to make its content narrower than it asked for.** Across the
scrolling axis it reported the content's width and then took the scrollbar's
gutter out of that same width. Fixed in the widget — it reserves the gutter
whether or not a bar is showing — but the shape of the bug is worth remembering:
a container that reports one size and then hands its child less is a squeeze in
everything the child holds, and `--check` reports it as several unrelated
widgets rather than as the container.

**`Box::sizing` deliberately does not inherit a child's flexibility across its
own axis**, so a row of things stays as tall as its contents however flexible
they are. `set_fills_cross(true)` is the opt-in. The comment beside it says why
the default is the other way: a toolbar that inherited a spacer's flexibility
would swallow the window.

**"Is it still running" is the wrong question at the moment something ends.**
A player that reaches the end of the timeline stops itself, so `playing()` goes
false at exactly the instant there is something left to decide — loop round, or
stop and tidy up. Both `advance_playback` and the frame loop guarded on it, so
the deciding never happened and looping to the end of a sequence sat on the last
frame for ever. The frame loop already carried a comment about the identical
shape for the *export* worker, which clears `running` and sets `finished` as its
last two acts; the lesson had been learned once and not applied. Anything that
finishes on its own needs a guard that means "there is still something to do",
not "it is still going".

**A window not in `App::shells()` paints once and never again.** The settings
window was missing from it, so its category buttons worked perfectly and nothing
on screen ever said so — which reads as a dead control and is a dead repaint.
Anything that makes a window has to add it there.

**A name from outside has no length you can plan for.** A device called
"Speakers (High Definition Audio Device)", a folder six levels deep, a camera
file called `A001_C003_0410XX_001.R3D`. Each of these has squeezed a layout in
some theme. Elide it, give it a row of its own, or both — and make `--check`
build the page with a *real* one in it, because a page checked in its emptiest
state is a page checked where nothing goes wrong.

**A page that shows project state has to be rebuilt when the project changes.**
Setting the rate to 29.97 is what makes the drop-frame control appear, and it
appeared nowhere until `apply_fps` marked the page stale. The same applies to
anything a settings page reads.

**`refresh_timeline` deliberately does not repaint the playhead readout**, since
it is a field somebody may be typing into. Anything that changes how a time is
*written* — the frame rate, drop-frame — has to call `show_playhead` itself.

**A press that moves the focus has it taken straight back.** `mouse_down` walks
up from the widget that was pressed and focuses the first thing that can take
the keyboard, which is right for an ordinary click and wrong when the handler
has just said where the keyboard should go. Renaming a track opened a field with
no caret in it, and typing "B-roll" chose the rate-stretch tool, marked an out
point and started playback. The host now leaves the focus alone when the
handler changed it. Anything that opens a popup and focuses something inside it
depends on that.

**Anything that integrates a value has to hold it at full precision.**
`Session::set_playhead` snaps to the frame grid, so the J/K/L shuttle — which
added a couple of milliseconds per turn of the loop — read the playhead back,
added its step, and rounded to the frame it was already on. It did not move at
all, and at 4x it occasionally cleared a boundary and lurched, which looked like
the rate being ignored rather than like rounding. The shuttle keeps its own
position and quantises only on the way out.

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

**`small` is a macro.** `<rpcndr.h>`, which arrives with `windows.h`, defines
it as `char`. A local called `small` produces "'std::vector<uint8_t>' followed by
'char' is illegal", which names neither the variable nor the header.

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

**The mixing thread may not touch a file, and "may not" includes the clever
case.** `AudioMixer` decoded everything up front for exactly this reason —
WASAPI wants a buffer every few milliseconds and a decode is tens of them — and
the windowed version keeps the rule rather than bending it: a reader thread
fills ahead, the window is published through one atomic store of a shared
pointer, and audio that has not been reached yet mixes as **silence** rather
than as a missed deadline. `AudioMixSettings::realtime` is what separates that
from an export, which decodes on the spot because nothing is waiting and being
exactly reproducible matters more. If you find yourself wanting to block "just
briefly" on the render thread, that is the bug.

**A cache that records *what* it has without recording *how well* will hand one
caller another caller's answer.** `ThumbnailCache` tracked which stretches of a
source had been extracted and not how finely, so the pool's dozen frames across
a ten-minute file marked the whole file covered — and every clip of it on the
timeline was stuck with one frame every twelve seconds for the rest of the
session, because nothing would ask again. Silent, and caused by opening a panel.
Whenever two callers want the same *thing* at different resolutions, the
resolution is part of what is stored, part of what is matched, and part of what
may be merged.

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

## 9. Where the work is right now

**`docs/premiere-gaps.md` §5, application settings, is closed.** Every row in it
is either built or declined with a written reason; that file's §5.6 is the
reasons and §5.7 is what closing it turned up. **Pick the next section** — see
*The next thing* below.

### What the section is

Premiere keeps three things apart and so does this, deliberately:

| | Belongs to | Stored in | Reached by |
|---|---|---|---|
| **Preferences** | the person | `%APPDATA%\Cutline\settings.json` | Settings ▸ Preferences… |
| **Project settings** | the cut | inside the `.cutline` file | Project ▸ Project Settings… |
| **Keyboard shortcuts** | the person | nothing yet | nothing yet |

The rule that decides which a new setting is: **if opening somebody else's
project should change it, it is not a preference.** Do not merge the two
windows. The mistake to avoid is one Settings window that quietly writes half
its contents to a different place from the other half.

### What is done

Preferences has ten pages — General, Appearance, Audio Hardware, Playback,
Graphics, Labels, Timeline, Proxies, Media Cache, Auto Save — and everything on
them persists. Project Settings has one, General, which grows a Timecode section at
29.97 and 59.94.

Persisted preferences: theme (by name), snapping, looping, aspect lock, preview
quality, pool ordering and view, still and transition durations, autosave
interval, **autosave copies kept**, **undo depth**, **preroll and postroll**,
**renderer choice**, **media cache folder**, label names, default label per
media kind, proxy height, proxy folder, audio output device.

Three rows are closed as *won't do*, each with its reasoning in
`premiere-gaps.md` §5.6: memory reserved, appearance brightness, and scratch
disks. The media cache row was declined and then built once somebody measured
what its absence cost — §5.8 is that story. Keyboard customisation was pulled
out into a section of its own, being larger than the rest of §5 together.

### Three things from §5 worth knowing before you touch them

- **Recovery copies are timestamped and pruned**, `<name>-<digest>-<stamp>`,
  newest first, oldest deleted past the limit. The stamp is fixed width so the
  names sort chronologically as text — ordering needs neither the filesystem's
  timestamps, which are coarse on some volumes, nor a parse. A copy written by
  an older build has no stamp; it still matches and sorts last, which is right.
  Matching is on the prefix **plus exactly a stamp's width**, because a
  never-saved document's prefix is `untitled` and that is also the start of
  every copy of a saved project called `untitled`.
- **The renderer choice is read once, when the device is made**, and the page
  says so. It also reports the adapter actually in use, which is not always the
  one chosen — the device still falls back to WARP on its own.
- **Preroll and postroll widen the looped range only.** `core::playback_span` is
  the arithmetic, and with both at zero it is `marked_span` exactly, which is
  what makes it safe everywhere the marked span was used before.

### The principle this section has been run on

**Expose what has no right answer. Leave what was measured.**

The thread counts (`kThumbnailThreads`, `kProxyThreads`), the surface pool
sizes, the cache budgets and the proxy *quality* were each chosen against a
measurement written down beside them in the code. A control offering somebody a
worse answer than the measured one only creates bad sessions. What is worth
exposing is what genuinely varies by person or by machine: durations, the
autosave interval, the label palette, the proxy size, the output device — which
is, not coincidentally, close to what Premiere exposes.

Say so in the commit when you decline a row. A row closed with a reason is worth
more than a row left open.

### How to add a preference

Twenty minutes, and every step has a reason:

1. **A field on `editor::Settings`** with a default equal to what the
   application already does. A fresh install must behave identically.
2. **Read and write it** in `settings_from_json` / `to_json`, clamping on the way
   in. A hand-edited file is not hostile, but it is not careful either — every
   number in there is clamped and the enums are read by name.
3. **A row on a page** in `tools/cutline/main.cpp` — `build_*_page`. Use
   `duration_row` for a number; a `Dropdown` for an exclusive choice of more than
   three; a `TextField` with `set_columns` when several must line up.
4. **Call `save_settings(app)`** from the control's `on_commit`, not
   `on_change` — a dragged number would otherwise write the file once a pixel.
5. **Feed it to whatever uses it.** Prefer threading it as a parameter over
   reading a global: `still_length`, `transition_length` and the proxy height all
   pass through the call, so there is still exactly one decision point.
6. **Run `--check`.** It lays out every page of both windows in all four themes.
   If your row has a name from outside in it, make the check pass build it with a
   real one — see the trap about lengths.

### The next thing, if you want a recommendation

§5 is closed, so this is a free choice between the sections still to walk.

Of them, **sequences** is the one that keeps coming up:
section 4 could not close "new sequence from a clip" without it, and it is the
largest piece of unbuilt model in the application. **Keyboard customisation** is
the other substantial one, and it was pulled out of §5 for being larger than the
rest of that section put together.

### State of the tree

Everything is committed and green. Nothing is half-finished, no branch is open,
and there is no work in progress to reconstruct.

- 2891 tests pass under the `ui` preset, 2496 under `release`. Set
  `CUTLINE_TEST_MEDIA_DIR` or about fifty decode tests skip while the run still
  says everything passed.
- `--check` reports 2615 widgets, 0 empty, 0 outside, 0 clipped, 0 squeezed, in
  all four themes — run it with something *in* the media cache as well as with
  it empty, since the Delete button only exists when there is something to
  delete.
- **Where the work is:** sections 1 to 5 of the gaps document are closed, and so
  is §6.1 — the mixer is Premiere's arrangement with Premiere's controls, five
  automation modes included. §6.2 is audited and its first two steps are built:
  a track carries an effect stack and so does the master. What is left of it is
  listed in §6.2 in dependency order, and none of it is blocked.
- **Not driven yet:** the Fit Clip dialogue (it needs a project carrying four
  marks, so it is absent from the `--check` scene too), and the application on
  WARP for more than a few minutes. Everything in section 6 *has* been driven:
  meters moving under playback, a fader read at -18.8 dB, a Write pass that put
  102 keyframes in the saved file, and a compressor added to a track from the
  strip's fx button.
- **Neither effects box is in the `--check` scene.** Fit Clip and the strip's
  fx popup are both built from a project state the check does not set up, so
  their layout is covered by driving alone.
- 42 commits since `v0.3.0`, unreleased. **Do not tag** without being asked —
  the owner's standing instruction is that releases happen after major features,
  not per commit.

---

## 10. Working with the person who owns this

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
- **`docs/buglist.md` is their channel, not yours.** They put faults in it in
  their own words; you tick them off and write underneath what the cause turned
  out to be. Everything in it is currently fixed. Do not use it as a to-do list
  of your own — that is what `premiere-gaps.md` is for.
- Be direct about what did not get done and why. A list of remaining gaps is
  more useful than a summary that reads as finished.
