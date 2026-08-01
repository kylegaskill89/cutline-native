# Gaps against Premiere

A running, section-by-section comparison with Premiere Pro. `docs/spec.md` was
the parity target for the rewrite and it is met; this is the *next* target, and
it is a different and much larger thing — the spec described what the old
TypeScript application did, and Premiere is what the person using this actually
compares it against.

Each section lists what Premiere does, what is here now, and what is missing.
Sizes are honest: **wiring** means the machinery exists and something has to
reach it, **control** means a new widget, **machinery** means a new subsystem.

Nothing here is scheduled. It is a map, not a plan.

---

## 1. Effects

### 1.0 Two things the spec asked for that were missed

Found while writing this section, and both are §18 items I recorded as done.
They are in the model, tested, and reachable from nowhere.

| | Exists | Missing | Size |
|---|---|---|---|
| **Composite (Blend)** | `core::set_clip_blend`, eight modes in the compositor | no control anywhere in the interface | wiring |
| **Reverse** | `core::set_clip_speed(…, reverse)`, honoured by the renderer and the mixer | the Speed row has no reverse toggle beside it | wiring |

### 1.1 How a parameter is shown and set — **done**

Premiere shows a **number**, in blue, that can be dragged to scrub the value or
clicked to type an exact one. A disclosure triangle beside it reveals a slider
underneath for the properties that have a bounded range. Ours was the other way
round and short of one half: a slider and *no number at all*.

That last part was the important one. `Slider::paint_content` draws a groove, a
fill and a thumb, and nothing else — so there was **no way to see what a value
was, and no way to type one**. "Scale is about two thirds along" is not a number
anybody can write down, match on another clip, or reproduce tomorrow.

`ui::NumericField` is now that control, in Premiere's arrangement: the number
sits after the property's name in the theme's accent colour, underlined under
the pointer; dragging it scrubs, clicking it opens a field over it, and a
disclosure triangle beside the name reveals the slider underneath. A press only
becomes a scrub once it has travelled three pixels, so a click that wobbles
still opens the field rather than nudging the value somewhere nobody asked for.

| | Premiere | Here | Size |
|---|---|---|---|
| Numeric readout | always visible | **done** | — |
| Drag the number to scrub | yes, with fine/coarse modifiers | **done** — shift is ×10, control ×0.1 | — |
| Click the number to type | yes | **done**, and the unit may be typed back in | — |
| Slider | behind a disclosure triangle | **done** | — |
| Paired X/Y on one row | Position and Anchor Point are one row of two numbers | two separate rows | control |
| Per-row reset button | a visible circular arrow | double-click the number, with no affordance saying so | control |
| Per-section reset | one per `fx` group | none | wiring |
| Greying a property another one governs | Uniform Scale greys Scale Width | Lock aspect ties them but leaves both live | wiring |
| Live update while scrubbing | the picture follows the drag | **done** — one undo entry for the whole gesture | — |

Two things fell out of building it and are worth keeping written down.

The first is that **a number that rounds what it is showing is worse than a
slider**, which at least never claimed to be exact. None of the transform
parameters declares a step, so the first rule for how many decimals to show —
enough to make one nudge visible — gave Opacity none at all, and a scrub landing
on 100.4 read as 100. A continuous range now always gets at least one place.

The second is that `TextField` had no way to say an edit was over. Enter and the
keyboard leaving both committed, but Escape reverted the text and stayed open,
and so did a commit of an unchanged value — a field opened over something else
and never closed. It now has `set_on_finish`, called however the edit ends.

### 1.2 The keyframe timeline

The whole right-hand half of Premiere's Effect Controls panel, and the largest
single gap in this section. A time ruler, the clip drawn as a bar, a playhead,
a zoom and scroll bar, and **one lane per animated property** with its
keyframes on it.

`ui::KeyframeView` is now the lane view, under the effect stacks rather than
beside them — the panel is a narrow column, and Premiere's side-by-side
arrangement would leave both halves too narrow to read. It fits the clip's whole
length across its width, which is what Premiere's does before anybody zooms.

| | Premiere | Here | Size |
|---|---|---|---|
| Keyframe lane per property | yes | **done** | — |
| Drag a keyframe in time | yes | **done** — the picture follows, one undo entry for the drag | — |
| Previous / next keyframe buttons | ◀ ◆ ▶ on every animated row | **done**, greyed at the ends of the list | — |
| Select several keyframes | click, shift-click, marquee | click and shift-click; no marquee | control |
| Right-click a selection | a context menu on it | **no right-click anywhere in the application** | wiring |
| Interpolation per keyframe | Linear, Bezier, Auto Bezier, Continuous, Hold, Ease In, Ease Out | `set_keyframe_interp` does it; nothing calls it yet | wiring |
| Zoom and scroll the lanes | yes | the clip's whole length, always | control |
| Delete a selected keyframe | Delete | `remove_keyframe` does it; no key is bound | wiring |
| Velocity graph | expandable value and speed curves | no | machinery |
| Copy and paste keyframes | yes | no | wiring |

Two things that came out of building it are worth keeping.

The lane view is the first thing in the application whose *contents* sit on its
own edge. A keyframe at exactly zero — which switching the stopwatch on at the
head of a clip produces — was drawn half outside the widget, where a press does
not reach it at all, because a point outside a widget's bounds routes somewhere
else entirely. The axis is inset by the grab reach at each end. This is the
third time that trap has been hit, after the monitor's transform handles.

And the interpolation chip has moved off the parameter row and behind the
disclosure triangle. Adding ◀ ▶ to the row left the property's own *name*
squeezed to nothing, which `--check` caught and a glance would not have.
Premiere has no chip at all — interpolation there is a right-click on the
keyframe — so this is where it goes when the context menu lands.

#### Selecting keyframes, and setting the interpolation between them

Shift-click and marquee-select several keyframes, right-click the selection, and
set the interpolation for all of them at once. This is how an animation gets
shaped in practice — ease out of the first, hold through the middle, ease into
the last — and it is a gesture, not a settings screen.

**This is much cheaper than it first looks, and an earlier draft of this
document said otherwise.** The claim was that per-keyframe interpolation needs a
model change because the resolver collapses it. That is wrong. `ease_fraction(f,
a.e)` reads the mode off the **outgoing** keyframe of the pair being
interpolated between — so the evaluator is already per-keyframe *and*
per-segment, and has been since phase 1. Storing "ease out of this one, linear
into the next" already works and already renders.

Three helpers are what flatten it, and all three are small:

- `keyframe_list_interp` reports `front().e` as though it were the property's;
- `set_keyframe_list_interp` writes one mode to **every** keyframe in the list;
- `upsert_keyframe` gives a new keyframe whatever the list's mode is.

`editor::set_keyframe_interp` is the setter that takes *which keyframe* rather
than which property, and the view keeps a selection. What is left is the context
menu — and the right-click that would open it.

Note also that the mode living on the outgoing keyframe means it genuinely is
"the interpolation **between** this keyframe and the next", which is the way
anybody describes it and the way Premiere presents it. The three modes we have
are Linear, Hold and Ease; Premiere's seven are a superset, and Bezier with
draggable handles is the one that needs the velocity graph to be worth having.

#### Nothing in this application has ever seen a right-click

Worth stating on its own, because it blocks the context menu above and is
invisible until something needs it. `MouseEvent` carries a button and
`MouseButton::Right` exists, but the Win32 layer only translates
`WM_LBUTTONDOWN` — there is no `WM_RBUTTONDOWN` case at all, so a right-click
reaches no widget anywhere in the editor.

The popup machinery it would feed is already there and already used by five
different controls (`open_popup` takes any widget, `MenuList` is the menu). The
missing part is three message cases and the routing, plus deciding what a
right-click means where there is no menu for it — which should be nothing,
rather than falling through to a left-click.

### 1.3 The effects library

Premiere has a **panel**: a search box, a folder tree, and drag-and-drop onto
clips. Ours has a button that opens a menu of eleven items.

The menu was the right answer when there were eleven effects and no panel
machinery. It stops being the right answer at about twenty, and it is already
the reason the audio and video stacks have separate buttons that open separate
menus.

| | Premiere | Here | Size |
|---|---|---|---|
| A dockable panel | yes | a popup menu from a button | panel |
| Search by name | filters the whole tree as you type | no | wiring |
| Folder tree | Presets, Lumetri Presets, Audio Effects, Audio Transitions, Video Effects (18 categories), Video Transitions | five flat video categories, eight audio effects, no tree | wiring |
| Drag an effect onto a clip | onto the timeline or the program monitor | no | machinery |
| Double-click to apply to the selection | yes | the menu applies to the selected clip | wiring |
| Transitions in the same panel | Video and Audio Transitions are folders in it | set from a dropdown in the inspector | wiring |
| User bins | new bin / delete, to gather favourites | no | machinery |
| Named presets | save a configured stack, apply it by name | copy and paste only, one clipboard, not saved | machinery |
| Capability badges | accelerated / 32-bit / YUV, and filters for them | every effect is a GPU shader, so the distinction does not exist here | not applicable |

The panel itself is cheap — `MediaBrowser` is a tree of rows with a search
already, and the dock takes a new panel by adding one line to `kPanels`. What is
not cheap is drag-and-drop onto a clip, and presets, which is a new persisted
thing with its own file.

### 1.4 Masks

Premiere puts three mask tools on **every** effect — ellipse, rectangle, pen —
with path, feather, opacity, expansion, inversion, and per-frame tracking. It is
how anything gets applied to part of a picture rather than all of it.

We have none of it, and nothing underneath it: no mask in the model, no mask in
the compositor, no path type. This is the largest item in this section by a wide
margin and the only one that is genuinely a subsystem. `docs/architecture.md`
calls arbitrary masks out as something the rewrite makes *cheap* — the shader
work is real but small — so the cost here is the model, the interaction, and the
tracking, not the drawing.

### 1.5 Catalogue depth

Eleven video effects against Premiere's several dozen, in five categories
against eighteen. This is a content gap rather than a structural one: the
catalogue and the resolver take a new effect in two places and a test walks
every entry, so each one is an afternoon. Worth listing because "we have
effects" and "we have the effects somebody reaches for" are different claims.

The ones whose absence would be noticed first, roughly in order: **Lumetri
Color** (or an equivalent grading control — curves, wheels, HSL secondaries),
**Gaussian Blur with directional and radial variants**, **Sharpen**, **Noise**,
**Lens Distortion**, **Drop Shadow**, **Track Matte Key**, **Ultra Key** (ours
is a simple chroma key), **Warp Stabilizer**, **Transform** (the effect, which
unlike Motion sits in the stack and can be reordered).

---

## Sections still to do

The rest of the application, in the order it seems worth walking:

- **Timeline** — track targeting, insert/overwrite from the source monitor,
  trim modes (ripple, roll, slip, slide as *trim* rather than tools), the
  program-monitor trim view, sync locks, nesting, multi-camera.
- **Source monitor** — there is none at all. Premiere's whole three-point
  editing workflow runs through it.
- **Project panel** — bins, metadata columns, icon view, proxies, relinking.
- **Audio** — the Audio Track Mixer, submixes, sends, the essential sound panel,
  loudness normalisation.
- **Titles and graphics** — the Essential Graphics panel, layered graphics,
  responsive design, styles.
- **Colour** — the Lumetri Color panel and its scopes workflow.
- **Export** — presets, queue, and the media encoder relationship.
- **Everything else** — markers with durations and comments, sequence settings,
  keyboard customisation, workspaces beyond four, undo history panel.
