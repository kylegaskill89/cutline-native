# Running and checking a local build

How to get from a working copy to something you can actually cut with, and how
to tell whether a change did what it claimed. The README covers *building* the
layers; this covers running the editor and checking it.

## The one thing to get right

**Run the Release build.** Debug is several times slower, and slow in exactly
the places that matter — decoding, compositing, the playback loop. A Debug
build will feel worse than the real thing and will tell you a performance fix
did not land when it did.

Measurements quoted in commit messages and in `docs/handoff.md` are Debug
unless they say otherwise, because that is what the tests run under. They are
useful for comparing before against after; they are not what the application
feels like.

## The application

Only the `ui` preset builds it. `build/default` has no media layer, no GPU and
no Skia, so `build/default/tools/` is empty — that tree is tests only.

```
cmake --build --preset ui
build/ui/tools/cutline/Release/cutline.exe
```

The `ui` build preset is Release already. After pulling a change, that one
build command is usually two or three minutes; add `--target cutline` to skip
the tests and build only the editor.

## The two headless checks

Both are the application itself, with a flag, and neither opens a window you
have to watch.

```
build/ui/tools/cutline/Release/cutline.exe --check
build/ui/tools/cutline/Release/cutline.exe --benchmark
```

`--check` lays out every panel, every dialogue and every menu in all four
themes and reports anything empty, escaped past the window, clipped by its
container, or squeezed below the width it asked for. **All four lines should
end in `0, 0, 0, 0`.** Anything else is a real fault: it is what catches a
control sliced by a panel edge or a label crushed to nothing, neither of which
shows up in a test and both of which are obvious on screen.

Pass a directory as a second argument and it writes each theme's frame out as a
picture, so a layout can be looked at rather than only counted.

`--benchmark` times painting per theme and per window size.

## The test suites

```
ctest --test-dir build/ui -C Release -j 8
```

**Set `CUTLINE_TEST_MEDIA_DIR` first**, or about fifty decode tests skip while
the run still prints `100% tests passed`. That reads as success and is not one.
It should point at a directory holding `Boiler.mp4`; some tests also want the
larger capture beside it.

```
CUTLINE_TEST_MEDIA_DIR=D:\Videos\VideoTrimmer ctest --test-dir build/ui -C Release -j 8
```

Expect around 3145 tests under `ui`. `build/default` runs roughly 2710 — the
same core, without anything that needs a GPU or a decoder — and is much faster
to build, which is what makes it worth keeping configured.

The suite is stable under `-j`. If a *different* test fails on each run, that
is the fixtures-sharing-a-temp-file fault coming back; see `docs/handoff.md`,
which explains why every fixture that writes to the temp directory has the
process id in its name.

## Telling playback problems apart

Three separate mechanisms can make playback stutter, they feel similar, and
they have nothing to do with each other. Which one it is depends entirely on
*when* it happens, so that is the thing worth noticing:

- **On pressing play.** The player is rebuilt after any edit, and building it
  decodes the audio it needs to start. If this grows as the cut grows,
  something is reading more of the sequence than the moment being started.
- **Crossing a cut.** A cut to another part of the source is a seek and a whole
  group of pictures. It should be prepared about half a second ahead, on the
  second decoder each source keeps; a dropped frame shortly *before* a cut is
  that working, not failing.
- **The first time a particular file appears.** Opening a decoder. Paid once
  per source per session, and the device underneath them all is warmed at
  startup.

A stall that survives an export is not any of these: export runs the same
renderer and the same mixer with the real-time path switched off, so a fault
that appears only in the preview is about *when* work happens rather than about
what it produces.

## The other tools

All under `build/ui/tools/<name>/Release/`, and all built by the same preset.

| | |
|---|---|
| `play_project <project.json> [--start S] [--play]` | The real preview path — the renderer an export uses, driven by the mixer, with the sound card keeping time. The control to measure playback against, because it has no interface in the way. |
| `render_frame <project.json> <seconds> <out.png>` | One frame of a project through the whole pipeline, written out. The end-to-end check: what the compositor produced can be looked at rather than only asserted about. |
| `export_project <project.json> <out.mp4>` | The export path on its own. |
| `decode_bench <file> [frames]` | Decoder throughput, per acceleration. |
| `preview_window <file>` | A throwaway viewport for driving the render pipeline by hand. Not the editor's UI and will not become it. |
| `theme_window` | The widget set, drawn without an editor around it. |
