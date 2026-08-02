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

### 1.0 Two things the spec asked for that were missed — **done**

Found while writing this section, and both were §18 items recorded as done: in
the model, tested, and reachable from nowhere.

| | Exists | Was missing | |
|---|---|---|---|
| **Composite (Blend)** | `core::set_clip_blend`, eight modes in the compositor | no control anywhere in the interface | **done** — a dropdown under the transform, in Premiere's order |
| **Reverse** | `core::set_clip_speed(…, reverse)`, honoured by the renderer and the mixer | the Speed row had no reverse toggle beside it | **done** — a checkbox beside Speed |

Reverse sits beside Speed and nowhere else, because they are one operation in
the model: `set_clip_speed` takes both, and a clip played backwards at half rate
is one retime rather than two.

### 1.1 How a parameter is shown and set

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
| Paired X/Y on one row | Position and Anchor Point are one row of two numbers | **done** — both, one stopwatch each | — |
| **Anchor Point** | a property of its own; scale and rotation happen about it | **done** — in pixels of the layer, keyframeable, and it needed no shader change | — |
| Time remapping | Speed keyframed, in Effect Controls | Speed is explicitly not animatable | model |
| Anti-flicker filter | a slider under Motion | none | model + shader |
| Per-row reset button | a visible circular arrow | **done**, and hidden on an animated row where it would write a keyframe holding the default | — |
| Per-section reset | one per `fx` group | **done** — per effect, keyframes cleared with it | — |
| Greying a property another one governs | Uniform Scale greys Scale Width | **done** — Lock aspect greys Scale Y and leaves it readable | — |
| **Balance / pan** | a panner on every audio clip *and* every audio track | **done for the clip** — see §1.6; the track panner is not | model + control |
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
| Select several keyframes | click, shift-click, marquee | **done** — all three | — |
| Right-click a selection | a context menu on it | **done**, and it is the first right-click the application has ever had | — |
| Interpolation per keyframe | Linear, Bezier, Auto Bezier, Continuous, Hold, Ease In, Ease Out | **done** for our three modes, across a whole selection at once | — |
| Zoom and scroll the lanes | yes | **done** — wheel zooms about the pointer, shift-wheel scrolls | — |
| Delete a selected keyframe | Delete | **done**, and on the menu | — |
| Velocity graph | expandable value and speed curves | **done** — both, in one box per lane | — |
| Bezier handles on the graph | dragged to shape the speed | **not possible** — see below | model |
| Copy and paste keyframes | yes | **done** — Ctrl+C and Ctrl+V, pasting at the playhead | — |

#### The one part of §1.2 that cannot be built as things stand

The graph draws both curves Premiere's does: the **value** across the lane, and
the **speed** under it as the slope of the first. They are sampled through
`core::eval_keyframes` rather than reimplemented, so the picture is drawn from
the same numbers the renderer uses — an ease reads as an S over an arch, a hold
as a step over nothing.

What cannot be built is **dragging the handles**. Premiere's velocity graph is
not only a picture: each keyframe has two bezier control points, and pulling
them is how a speed is shaped. `core::Keyframe` has a time, a value and one of
three modes. There is nowhere to put a handle.

Adding them is a real model change rather than a wiring job — `Keyframe` grows
two offsets, the resolver learns a cubic, the serialiser learns to write them,
and every project file written before it has to keep loading. It also makes the
three modes into presets over a continuous space, which is what Premiere's seven
actually are. Worth doing, and worth doing deliberately rather than as the tail
of a panel.

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

A third, which is the same lesson twice: **panel state does not live in the
panel**. The inspector is rebuilt from nothing on every edit, so a graph opened
in a lane closed again the moment anything in it was changed — easing a keyframe
shut the graph you were easing it in. Which triangles are open now lives on the
application and is put back after each rebuild, exactly as the parameter rows'
disclosure state already did.

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

#### The right-click, which the application had never had

Worth recording, because it blocked every context menu in this document and was
invisible until something needed one. `MouseEvent` had carried a button since
the first widget and `MouseButton::Right` had always existed, but the Win32
layer only ever translated `WM_LBUTTONDOWN` — so a right-click reached no widget
anywhere in the editor.

It was three message cases. `WidgetHost` needed nothing at all: it had always
routed by button and only ever gated *capture* on the left one, which turns out
to be exactly right — a right-click is a single event rather than a gesture, and
holding the pointer for one only means the release goes somewhere surprising.
`WM_CONTEXTMENU` is swallowed, or the system puts its own menu up over ours.

Where there is nothing to offer, a right-click is left unhandled and carries on
to whatever is behind, rather than opening an empty menu.

### 1.3 The effects library

Premiere has a **panel**: a search box, a folder tree, and drag-and-drop onto
clips. Ours had a button that opened a menu of eleven items.

The menu was the right answer when there were eleven effects and no panel
machinery. It stopped being the right answer as soon as transitions wanted to
live beside them, and it was already the reason the audio and video stacks had
separate buttons opening separate menus.

`ui::EffectsBrowser` is the panel, and `editor::effect_library` is what fills
it: video effects by category, then audio effects, then transitions, in one
list. Which of the three an entry is lives in its id — `video:blur`,
`audio:lowpass`, `transition:dissolve` — so the widget stays a tree of names and
folders and never learns the difference between them.

| | Premiere | Here | Size |
|---|---|---|---|
| A dockable panel | yes | **done** — "Effects Library", in every built-in workspace | — |
| Search by name | filters the whole tree as you type | **done**, and a folder whose name matches keeps its contents | — |
| Folder tree | Presets, Lumetri Presets, Audio Effects, Audio Transitions, Video Effects (18 categories), Video Transitions | **done** — one level, qualified: `Video · Colour`, `Audio`, `Video Transitions` | — |
| Double-click to apply to the selection | yes | **done** — but only to the *first* clip selected, not all of them | wiring |
| Transitions in the same panel | Video and Audio Transitions are folders in it | **done** — and refused where nothing abuts the clip | — |
| Drag an effect onto a clip | onto the timeline or the program monitor | **done** for the timeline; not the monitor | control |
| Nested folders | Video Effects › Blur & Sharpen › Gaussian Blur | one level, with the parent folded into the name | control |
| User bins | new bin / delete, to gather favourites | no | machinery |
| Named presets | save a configured stack, apply it by name | copy and paste only, one clipboard, not saved | machinery |
| Capability badges | accelerated / 32-bit / YUV, and filters for them | every effect is a GPU shader, so the distinction does not exist here | not applicable |

Two rules the panel enforces that a menu never could.

**Nothing is offered where it would do nothing.** `library_entry_fits` refuses a
video effect on an audio clip, an audio effect on a picture, and a transition on
a clip with nothing abutting it — including the case that is easy to miss, an
overlapping transition at a join where neither clip has handles to lend. A
cross-dissolve there resolves to nothing at all, and dip-to-black is the one
that always works because it is sequential rather than overlapping.

**A search never hides its own results.** Every folder starts *collapsed* —
forty names in a narrow column is a list rather than a library — so a filter has
to override that, or searching would appear to find nothing at all. A folder
left empty by the filter is not drawn either. Both were easy to get wrong in the
direction that makes a search look like a failure.

Dragging an entry onto a clip works, and the clip it would land on is outlined
while the pointer is over it. The outline is drawn only where the drop would
actually be *accepted*, so it never promises something the release then refuses.

This is the first drag in the application that crosses a panel boundary, and it
needed nothing new to do it: a handled press captures the pointer, so the moves
keep arriving at the browser however far away the cursor goes. The browser
reports where the pointer is and stays ignorant of the timeline; the timeline
answers `block_at` and stays ignorant of the drag; the composition root is the
only thing that knows about both.

The remaining items are the expensive ones. Presets need a new persisted thing
with its own file and its own format questions. User bins need the same
persistence plus a way to reorder. Nested folders are a widget problem only.
Dropping onto the *program monitor* is the same mechanism as the timeline drop
with a different hit test, and waits on there being a reason to prefer it.

### 1.4 Masks, 1.5 Catalogue depth, and the thing they share

These two look unrelated — one is a subsystem, the other is content — and they
are blocked by the same wall. It is worth stating before either is estimated,
because it makes one of the sizes below badly wrong.

#### The compositor has room for about seven more numbers

Every effect this application has is a field in **one** `Params` struct, applied
to a layer in a single pass. That struct is delivered as **root constants**, and
a D3D12 root signature may hold 64 DWORDs in total. Ours holds 49: one for the
descriptor table and 48 for `ShaderParams`, of which 28 are already effect
parameters.

**Fifteen DWORDs are left, and a mask needs about eight of them.**

So the claim in an earlier draft of this document — that a new effect is "an
afternoon", the catalogue and the resolver taking one in two places — is wrong
in the way that matters. It is true for the next one or two and then it is not
true at all. Premiere's several dozen effects cannot be reached by adding fields
to this struct, and the last few that fit would be spent by whichever feature
happened to ask first.

What unblocks both is the same change: **effects become passes rather than
fields**. Each effect gets its own shader and its own small constant buffer, the
compositor runs the stack as a chain of render targets, and the budget stops
being global. That is how the reference does it and how any of this scales. It
also happens to be what per-effect masks need, since a mask is then a property
of a pass rather than of a layer.

It is not a small change — it touches the compositor's whole draw path, costs
intermediate targets per effect, and needs the blur's existing two-pass special
case folded into the general mechanism. It is also the single highest-value
piece of work left in section 1, because everything after it gets cheaper.

#### 1.4 Masks

Premiere puts three mask tools on **every** effect — ellipse, rectangle, pen —
with path, feather, opacity, expansion, inversion, and per-frame tracking. It is
how anything gets applied to part of a picture rather than all of it.

We have none of it, and nothing underneath it: no mask in the model, no mask in
the compositor, no path type.

There is a real decision here, and it is not mine to make:

- **A mask per clip** fits in the budget above and could be built now — shape,
  feather, expansion, inversion, and handles on the monitor. It is genuinely
  useful and it is *not* what Premiere does.
- **A mask per effect** is what Premiere does, and it needs the pass
  restructuring first.

Tracking is beyond both and belongs with neither: it is per-frame analysis, a
different kind of work from anything here now.

`docs/architecture.md` calls arbitrary masks out as something the rewrite makes
*cheap*. That is true of the drawing and false of everything around it.

#### 1.5 Catalogue depth

Eleven video effects against Premiere's several dozen, in five categories
against eighteen. Worth listing because "we have effects" and "we have the
effects somebody reaches for" are different claims — but see the budget above:
this is a **structural** gap wearing a content gap's clothes.

The ones whose absence would be noticed first, roughly in order: **Lumetri
Color** (or an equivalent grading control — curves, wheels, HSL secondaries),
**Gaussian Blur with directional and radial variants**, **Sharpen**, **Noise**,
**Lens Distortion**, **Drop Shadow**, **Track Matte Key**, **Ultra Key** (ours
is a simple chroma key), **Warp Stabilizer**, **Transform** (the effect, which
unlike Motion sits in the stack and can be reordered).

### 1.6 Audit: what section 1 is still missing

Written by walking the code rather than this document, because two entries here
have already turned out to be wrong when checked that way. Everything below was
confirmed against the source.

**Not in this document until now.** These are the finds, in rough order of how
much they cost somebody:

1. ~~**No anchor point.**~~ **Done.** `Transform` carries `anchor_x`/`anchor_y`,
   a fraction of the layer defaulting to its middle. It turned out to need no
   shader change at all: position names the anchor, the compositor wants a
   centre, and the difference is one rotated offset computed in `layer_box`.
   Shown in pixels of the layer, paired on one row, keyframeable like the rest
   of the transform. Files written before it read back centred.
2. ~~**No panner.**~~ **Done** for the clip. `Clip::pan`, animated through
   `AnimProp::Pan`, shown as Balance from -100 to 100 under Volume. It is a
   balance rather than a constant-power pan, because what the mixer has is a
   stereo bus; centre is exactly unity, so nothing written before it sounds
   different. Premiere also has a panner on every audio *track* — that is a
   track property and a mixer control, and it is not done.
3. **Audio effect parameters cannot be keyframed at all.** `AudioClipEffect`
   holds a type, an enabled flag and a map of numbers. Clip gain is automatable
   and nothing else about the sound is. The video side has had per-parameter
   keyframes since phase 1.
4. **An effect applies to one clip, not the selection.** Both the double-click
   and the drop take `selection.front()`. Premiere applies to every selected
   clip, which is the whole reason to select several.
5. ~~**Effects cannot be reordered by dragging.**~~ **Done.** The up and down
   buttons stay; `ui::GrabRow` adds the drag, with an insertion line across the
   top of the card it would land above.
6. ~~**Only the whole stack can be copied.**~~ **Done** — right-click a card's
   header. It fills the same clipboard, so a paste still *replaces*; copying one
   effect to *add* to another stack is a different operation and does not exist.
7. **Effect Controls never says which clip it is showing.** Premiere titles the
   panel with the clip and the sequence. With one clip selected it is obvious;
   with a timeline full of similar takes it is not.

**Already listed, and still open.** Carried forward so this is one list rather
than four:

| | Where | Size |
|---|---|---|
| Bezier handles on the velocity graph | §1.2 | **model** |
| Drag an effect onto the program monitor | §1.3 | control |
| Nested folders in the library | §1.3 | control |
| User bins, and named presets | §1.3 | machinery |
| Masks | §1.4 | **decision, then machinery** |
| Catalogue depth | §1.5 | **blocked on the root-constant budget** |

Paired X/Y on one row, a visible reset per row, resetting a whole effect and
greying a governed property are all done, and are no longer listed.

**The shape of what is left.** Eight of the seventeen remain, plus the track
panner the clip one turned up. Six of those are
not independent: masks, catalogue depth and per-effect anything all wait on
effects becoming passes rather than fields, and bezier handles and audio-effect
keyframes are each a change to the model with a serialiser and a compatibility
question attached. Two of that second cluster are now done and both were cheaper
than expected — the anchor point because it is an offset the existing geometry
already carries, the panner because the keyframe array took it without being
widened. Neither of those escapes applies to what is left: a bezier handle is a
new field *inside* `Keyframe`, and an audio effect has no keyframe map at all.

---

## 2. Timeline

The panel with the most in it already, and the one where what is missing is
hardest to see: everything here works, and the gaps are all *other ways of doing
the same thing* that turn out to be the fast ways.

### 2.1 Track targeting, and the whole of three-point editing

Premiere's track headers carry **source patching** — which video and audio track
the next edit lands on — and **targeting**, which decides what a keyboard edit
applies to. Neither exists here. Ours have mute, solo, lock and hide, which is
the *state* half of a header and none of the routing half.

This is not a small omission dressed up. It is the thing the next section is
built on: without a target, "insert" and "overwrite" have nowhere to go, and
without those two there is no three-point editing at all.

| | Premiere | Here | Size |
|---|---|---|---|
| Source patch (V1/A1 indicators) | drag to choose which track receives | none | control |
| Track targeting for keyboard edits | per track, toggled | none | control |
| Insert (`,`) and Overwrite (`.`) | from the source monitor at the playhead | none | machinery |
| Three- and four-point editing | in/out on source and sequence | none | machinery |
| Sync lock | which tracks ripple together | none | model + control |
| Mute / solo / lock / hide | yes | **done** | — |

### 2.2 Trimming

We have slip, slide and rate stretch as **tools** — pick the tool, then drag.
Premiere has them as tools *and* as gestures on an edge with a modifier held,
and the second is what anybody actually uses: the tool palette is for the times
you want the mode to stay.

| | Premiere | Here | Size |
|---|---|---|---|
| Drag an edge to trim | yes | **done** | — |
| Slip, slide, rate stretch | tool **and** modifier gesture | tool only | wiring |
| Ripple trim (edge drag that closes the gap) | yes | none — a trim leaves a gap | wiring |
| Rolling edit (both sides of a cut at once) | yes | none | wiring |
| Trim to playhead (`Q` / `W`) | yes | none | wiring |
| The trim monitor (two-up while trimming) | yes | none | machinery |
| Nudge by frame | yes | **done** | — |

Ripple and roll are the two that matter. Both are edits the core can already
express — they are `set_clip_edge` plus an arrangement pass — and neither has a
gesture reaching them.

### 2.3 Clips on the timeline

| | Premiere | Here | Size |
|---|---|---|---|
| Drag from the pool onto a track | yes | **done**, at the point of release | — |
| Drag a clip to move it | yes | **done** | — |
| Copy and paste clips | Ctrl+C / Ctrl+V, and paste-insert | **none at all** | wiring |
| Duplicate (alt-drag a copy) | yes | none | wiring |
| Label colours | eight, set per clip and per bin | none | model + control |
| Enable / disable a clip | yes | in the model, no control on the timeline | wiring |
| Speed / duration dialogue | a box with both and a ripple option | Speed and Reverse in the inspector | control |
| Nesting | a sequence inside a sequence | none | machinery |
| Multi-camera | none | none | machinery |

**Copy and paste is the surprise.** There is a clipboard for effects and a
clipboard for keyframes, and none for clips — the single most-used pair of keys
in any editor does nothing on the timeline. It is wiring: `core` can already
place a clip anywhere, and `Session` already records one entry per gesture.

### 2.4 Reading the timeline

| | Premiere | Here | Size |
|---|---|---|---|
| Waveforms and filmstrips on clips | yes | **done**, cached and drawn on workers | — |
| Volume rubber band | yes | **done**, with keyframes | — |
| Keyframe marks on a clip | yes | **done** | — |
| Snapping | yes | **done** | — |
| Zoom, and zoom to fit | yes | **done** | — |
| Track height, per track | dragged, and expand/collapse all | fixed per kind — one height for video, one for audio | control |
| Markers with duration and comment | yes | a name and a colour, no duration | model + control |
| Scroll to follow playback | smooth or page | none — the playhead runs off the edge | wiring |
| Timecode field to type into | yes | a readout only | control |

The last two are small and both are felt every session. A playhead that leaves
the view during playback is the one on this page most likely to be noticed
within a minute of using it.

---

## Sections still to do

The rest of the application, in the order it seems worth walking:

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
