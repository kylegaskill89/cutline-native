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

### 1.1 How a parameter is shown and set

Premiere shows a **number**, in blue, that can be dragged to scrub the value or
clicked to type an exact one. A disclosure triangle beside it reveals a slider
underneath for the properties that have a bounded range. Ours is the other way
round and short of one half: a slider and *no number at all*.

That last part is the important one. `Slider::paint_content` draws a groove, a
fill and a thumb, and nothing else — so there is currently **no way to see what
a value is, and no way to type one**. "Scale is about two thirds along" is not a
number anybody can write down, match on another clip, or reproduce tomorrow.

| | Premiere | Here | Size |
|---|---|---|---|
| Numeric readout | always visible | **absent** | control |
| Drag the number to scrub | yes, with fine/coarse modifiers | no | control |
| Click the number to type | yes | no | control |
| Slider | behind a disclosure triangle | always, and it is the only control | — |
| Paired X/Y on one row | Position and Anchor Point are one row of two numbers | two separate rows | control |
| Per-row reset button | a visible circular arrow | double-click the slider, with no affordance saying so | control |
| Per-section reset | one per `fx` group | none | wiring |
| Greying a property another one governs | Uniform Scale greys Scale Width | Lock aspect ties them but leaves both live | wiring |

The right shape is one control — call it a **numeric field** — that is a
scrubbable number, a text field on click, and an optional slider on expansion.
It replaces `Slider` in the inspector and everything above it stays as it is.

### 1.2 The keyframe timeline

The whole right-hand half of Premiere's Effect Controls panel, and the largest
single gap in this section. A time ruler, the clip drawn as a bar, a playhead,
a zoom and scroll bar, and **one lane per animated property** with its
keyframes on it.

Ours has none of it. Keyframe times are drawn as diamonds on the clip in the
*timeline* panel, which says a keyframe exists but not which property it belongs
to, and gives nothing to grab.

| | Premiere | Here | Size |
|---|---|---|---|
| Keyframe lane per property | yes | no | machinery |
| Drag a keyframe in time | yes | no — the one gesture the model supports and nothing offers | machinery |
| Previous / next keyframe buttons | ◀ ◆ ▶ on every animated row | the diamond toggles one at the playhead; there is no way to *go* to one | control |
| Select several keyframes | click, shift-click, marquee | no | machinery |
| Interpolation per keyframe | right-click: Linear, Bezier, Auto Bezier, Continuous, Hold, Ease In, Ease Out | one setting for the whole property, three modes | model + control |
| Velocity graph | expandable value and speed curves | no | machinery |
| Copy and paste keyframes | yes | no | wiring |

Two notes on the interpolation row. Ours is deliberately one-per-property —
that was the reference's shape and it is what most people want — but Premiere's
is per-keyframe, and *ease in on the way out of one keyframe and linear into the
next* is not expressible here at all. Changing it is a model change
(`Keyframe::e` already exists per keyframe; `keyframe_interp_of` collapses it),
so the storage is mostly there and the resolver would need to stop assuming.

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
