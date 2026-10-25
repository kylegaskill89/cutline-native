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

**What counts as done, and why the bar moved.** This page repeatedly says that a
row was checked "by walking the code rather than this document", and treats that
as the reliable method. It is not sufficient, and the free-draw mask is the
proof. Its row read **done — corners in a buffer, even-odd fill, a handle on
each**, and every part of that sentence was true of the source: the buffer
existed, the shader filled it, the handles dragged. The mask had nevertheless
never masked anything in its life, because the step that hands a pass to the
card built it from a brace list of ten numbers and the corners are a vector that
a brace list cannot carry. Nothing in the model, the editor or the panel was
wrong. Only the picture was, and only for the one shape whose definition is not
a number.

So: **for any row whose output is a picture or a sound, the evidence is a
rendered frame or a measured signal.** Not the presence of a function, and not a
test that stops at the layer below the one that was broken. Reading the code
confirms that something was built; it cannot confirm that it arrives.

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
| Time remapping | Speed keyframed, in Effect Controls | **done** — Speed is an animatable property, and which frame shows is the integral of it | — |
| Anti-flicker filter | a slider under Motion | **done** — a vertical softening where the source is read | — |
| Per-row reset button | a visible circular arrow | **done**, and hidden on an animated row where it would write a keyframe holding the default | — |
| Per-section reset | one per `fx` group | **done** — per effect, keyframes cleared with it | — |
| Greying a property another one governs | Uniform Scale greys Scale Width | **done** — Lock aspect greys Scale Y and leaves it readable | — |
| **Balance / pan** | a panner on every audio clip *and* every audio track | **done**, on the clip and on the track — see §1.6 | — |
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
| Bezier handles on the graph | dragged to shape the speed | **done** — see below | — |
| Copy and paste keyframes | yes | **done** — Ctrl+C and Ctrl+V, pasting at the playhead | — |

#### The part of §1.2 that needed a model change — **done**

The graph draws both curves Premiere's does: the **value** across the lane, and
the **speed** under it as the slope of the first. They are sampled through
`core::eval_keyframes` rather than reimplemented, so the picture is drawn from
the same numbers the renderer uses.

Dragging the handles needed `Keyframe` to have somewhere to put them, and it now
does: two handles in **normalised segment space**, x a fraction of the segment's
duration and y a fraction of its value change, measured from the keyframe's own
end. That is what makes a handle mean the same shape whatever the segment's
length or height.

The defaults are a third and a third, which are the control points of the cubic
that *is* a straight line — so a keyframe switched to Bezier and left alone
animates exactly as a linear one did. Linear, Hold and Ease survive: Ease stays
smoothstep rather than becoming its bezier approximation, so every project
written before this renders to the same numbers, and Hold is not a curve at all
and could not be a preset over this space even in principle.

Handles are shown for the *selected* keyframes only, and drawn hollow on a
segment that is not listening to them yet — pulling one is what switches the
segment over.

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
| Nested folders | Video Effects › Blur & Sharpen › Gaussian Blur | **done** — a folder is a path, and the tree is made out of the paths | — |
| User bins | new bin / delete, to gather favourites | **done** — made, renamed, deleted, filled by dragging | — |
| Named presets | save a configured stack, apply it by name | **done** — saved beside the settings, offered in the tree, dragged like an effect | — |
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

**A bin holds ids, not copies.** That is the whole difference between a bin and
a preset, and it decides everything else: an effect gathered into one is the
same effect that is still under its category, so a renamed preset is renamed in
the bin too, and an id that no longer names anything is left out of the list
rather than deleted from the file — a build that temporarily lacked an effect
would otherwise quietly empty somebody's bins.

Two consequences worth stating, because both had to be handled rather than
discovered later. An *empty* bin has no entries and so cannot exist in a tree
built out of paths, which is right for a catalogue and wrong for a folder
somebody has just made: the browser is told which folders exist as well as what
is in them. And the same id can now be on screen twice, so a selection carries
the folder it was made in — one click lighting up two rows says something
happened in two places.

Dragging found the third thing. The gesture that applies an effect and the
gesture that gathers one are the same drag, and only where it *ends* says which:
released over the timeline it applies, released back over a bin it gathers. The
bin is outlined while the pointer is over it, exactly as a clip is.

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

**Fifteen DWORDs were left, and a mask needed about eight of them.** — **done**.

Effects are **passes**, not fields. A layer with any effect on it is drawn once
into a scratch target in its own space, each effect runs over that scratch in
stack order, and the result is positioned and composited; a layer with no
effects still draws in one go. A pass carries eight floats shared by every
kind, because only one runs at a time.

The root constants went from forty-eight DWORDs to forty-eight — the total is
the same, and it is now *reusable*. That is the part that mattered: an effect no
longer holds a permanent field, so the catalogue grew from eleven to twenty
without the signature moving, and a mask found somewhere to live that belongs to
one effect rather than to the whole stack.

Four things changed about what is drawn, all of them the point rather than side
effects, and all of them recorded here because a project made before them will
not match one made after:

- **Order matters.** Invert then blur is a blurred black layer; blur then invert
  is an inverted blur of white. Before, the two were the same picture.
- **The keyer sees the stack.** It used to read the unmodified pixel always. Now
  it reads whatever precedes it, and putting it first is how to get the old
  behaviour.
- **Blur is in the layer's own pixels**, so scaling a layer scales its blur —
  and it spreads past the quad the way Premiere's does. A stack that spreads
  draws the layer smaller, into the middle of the scratch, and the composite
  grows the quad back by the same factor: the picture lands where it would have,
  with room around it to reach into. The margin is sized to the reach of the
  widest spreading pass, so a stack that does not spread pays nothing.
- **Flip is a pass**, so it sits somewhere in the stack rather than being
  applied to the quad before everything else.

#### 1.4 Masks — **done**

Premiere puts three mask tools on **every** effect — ellipse, rectangle, pen —
with path, feather, opacity, expansion, inversion, and per-frame tracking. It is
how anything gets applied to part of a picture rather than all of it.

`core::Mask` is now a shape, a centre, half-extents, a rotation, a feather, an
opacity and an inverted flag, on **one effect** rather than on the clip — which
is what Premiere means by a mask and what the flat effect struct could not
express at all. Everything is a fraction of the layer, so a mask keeps its place
when the clip is scaled.

The shader applies it the same way for every kind of pass: work out what the
effect did, work out how much of the pixel the mask covers, mix between the two.
An effect never learns that it is masked, so a new one in the catalogue gets
masking for nothing.

| | Premiere | Here | Size |
|---|---|---|---|
| Ellipse and rectangle | yes | **done** | — |
| Feather, opacity, inversion | yes | **done** — opacity is the *effect's* strength, not the layer's transparency | — |
| Rotation | yes | **done** | — |
| A mask per effect | yes | **done** | — |
| Pen / free-draw path | pen tool, bezier handles | **done** — placing, curving, adding and removing | — |
| Dragging the shape on the monitor | yes | **done**, and it writes a keyframe when the number is animated | — |
| Animating a mask | keyframed path, and tracking | **done** for every number it has; a path is the part that is missing | — |
| Tracking | per-frame analysis | none, and belongs with neither of the above | machinery |

**"Free Draw" is a pen now.** It was a rectangle of four draggable corners, and
this row claimed it was finished. What it has:

- **A handle either side of every point.** `MaskPoint` carries `in` and `out`
  offsets, and both zero is a sharp corner — so every path written before the
  pen existed reads back as exactly the polygon it was, and a mask with no
  curve on it still writes no handles down.
- **One outline.** `core::flatten_mask_path` turns the path into the straight
  segments that get filled, and the monitor draws *that*, so the shape on the
  picture is the region being masked rather than an approximation beside it.
  The sixty-four-point cap is spent where the curvature is: a nearly straight
  edge gets one segment, a tight bend gets many, and the points somebody placed
  are never dropped in favour of samples.
- **The gestures.** Drag a point and its handles travel with it. Drag a handle
  and the pair mirrors, so the curve stays smooth through the point; hold Alt
  and it breaks, for a corner. Double-click a point to round it off or square it
  up. Ctrl-click the outline to add a point — the curve is *split* by de
  Casteljau, so the shape does not move at all — and Alt-click one to remove it,
  down to the three that are the fewest that enclose anything.

- **Placing one from nothing.** *Draw Path*, under the shape, empties the mask
  and puts the pen out: a click on the picture puts a point down, holding and
  dragging as you click pulls the handles out of it, a line trails from the last
  point to the pointer so the edge can be seen before it is committed to, and
  clicking the first point again closes the shape. While the pen is out a press
  places rather than picks up, which is what a mode is for.

**Two things it taught, both found by drawing with it rather than by testing
it.** Emptying a path could not be written down: `apply_mask_overlay` only wrote
the corners when there were some, so the pen's opening act was refused and the
next refresh handed the old shape straight back — the first three points of a
new triangle landed on top of the circle they were replacing. And a placed point
was never committed, because the release compares the shape against what it was
at the press and the press is what had added it; the gesture then read as
abandoned and the point was discarded. Both have tests now.

**An unfinished path covers nothing rather than everything.** The compositor
used to treat fewer than three corners as "no mask", on the stated grounds that
an effect should not vanish while a path was being drawn. Watching it settles
the argument the other way: on an invert, covering everything means the whole
frame strobes until the third point lands, where covering nothing leaves the
picture you are tracing visible. No saved path can reach that state — one is
seeded with four corners and removing them stops at three.

**And the whole feature had never masked anything.** `to_gpu_pass` builds the
mask from a brace list of its ten numbers, and the corners are a vector that a
brace list cannot carry, so a path reached the card empty and the shader applied
the effect to the entire layer. Nothing in the editor showed it: the outline
drew, the handles dragged, every number round-tripped. Only the picture was
wrong, and only for the one shape whose definition is not a number. There are
two tests on it now, one of which measures the area a bowed path covers against
the diamond it started as and expects π/2.

Tracking is beyond all of it: it is per-frame analysis, a different kind of work
from anything here now.

#### 1.5 Catalogue depth

Twenty-three video effects, up from eleven, in five categories. The structural gap
underneath this is gone: an entry is now one branch in one shader and one line
in the registry, and costs nothing permanent.

Added since: **Exposure**, **Gamma**, **Levels**, **Colour Balance**, **Tint**,
**Directional Blur**, **Sharpen**, **Posterize**, **Threshold**, **Radial
Blur**, **Lens Distortion** and **Noise**. Three of those reach one kind of
pass, which is the clearest sign the ceiling has gone — an entry is a name and
some numbers rather than a slot in a struct.

Noise is the one worth naming: grain has to *move*, and the moment is the one
thing a pass cannot work out for itself. It comes from the time the planner is
already given, which is why that function takes one beyond resolving keyframes.

Still absent, roughly in the order their absence would be noticed: **Lumetri
Color** (or an equivalent grading control — curves, wheels, HSL secondaries),
**Drop Shadow**, **Track Matte Key**, **Ultra Key** (ours is a simple chroma
key), **Warp Stabilizer**, and **Transform** (the effect, which unlike Motion
sits in the stack and can be reordered).

Of those, Drop Shadow and Track Matte Key want something the pass chain does not
have yet — a second texture to read besides the layer itself. The rest are
branches.

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
2. ~~**No panner.**~~ **Done**, on the clip and on the track. `Clip::pan` and
   `Track::pan`, shown as Balance from -100 to 100. Balances rather than
   constant-power pans, because what the mixer has is a stereo bus; centre is
   exactly unity, so nothing written before them sounds different. The two
   multiply their sides rather than adding their pans, so a clip hard left on a
   track panned hard right is silent. The track fader came with it, since a
   panner without one is half a mixer.
3. ~~**Audio effect parameters cannot be keyframed.**~~ **Done.**
   `AudioClipEffect` carries the same keyframe map `ClipEffect` does, and the
   panel is the same loop. The sound is retuned on a fixed grid of frames
   aligned to the *timeline* rather than to the caller's buffer, so the preview
   and the export follow the same curve.
4. ~~**An effect applies to one clip, not the selection.**~~ **Done.** Re-checked
   rather than trusted, which was the right call: Paste already took the whole
   selection and two things did not. **Add Effect** in the panel and **Clear**
   beside it now take it too, through `editor::add_effect_to` and
   `clear_effects_on` — one project in, one out, so blurring six clips is one
   press of undo. A clip the effect does not suit is passed over rather than
   given one it cannot run: `core::add_clip_effect` does not look at a clip's
   kind, so a plain loop over a mixed selection would put a blur on a waveform,
   where it would sit in the file and draw an fx badge and never be run by
   anything. The batch goes through `library_entry_fits`, which is where that
   rule already lived.

   What deliberately stays per-clip is everything reached by *index* — toggle,
   move up, move down, remove. The panel shows one clip's stack, and "remove the
   third effect" means a different effect on every clip in a selection.
5. ~~**Effects cannot be reordered by dragging.**~~ **Done.** The up and down
   buttons stay; `ui::GrabRow` adds the drag, with an insertion line across the
   top of the card it would land above.
6. ~~**Only the whole stack can be copied.**~~ **Done** — right-click a card's
   header. It fills the same clipboard, so a paste still *replaces*; copying one
   effect to *add* to another stack is a different operation and does not exist.
7. ~~**Effect Controls never says which clip it is showing.**~~ **Done**, and
   this entry is the one that was stale. `refresh_inspector` names the clip —
   the media's name, bold, above Motion, falling back to the clip id if the
   pool entry has gone. It is a plain `Label` rather than an `inspector_heading`
   because it is held in `App::clip_heading`: a matte's name follows its colour,
   and the swatches deliberately do not rebuild the panel.

**Already listed, and now closed.** Everything that was listed is done: paired
X/Y, a visible reset per row, resetting a whole effect, greying a governed
property, the anchor point, both panners, audio-effect keyframes, bezier
handles, effects as passes, masks and dragging them on the picture, nested
folders, dropping an effect on the picture, named presets, user bins, and a
free-drawn mask path.

…and **Effect Controls naming the clip it is showing**, which this page has said
twice, in two places, in two different tenses. It is done. Item 7 above was the
stale half and now says so.

Worth keeping the way that was got wrong, because it was got wrong twice. The
first pass read the two paragraphs, saw them disagree, and believed the one
listing it as open. The second pass went to the source — and grepped
`inspector_heading`, found seven calls, saw that all seven name a category, and
concluded the clip is never named. Both were wrong, and the second was worse for
looking like verification. The clip's name is not an `inspector_heading` at all:
it is a plain bold `Label`, because it has to be held in `App::clip_heading` so
that a matte's name can follow its colour without rebuilding the panel. Grepping
for the helper you expect a thing to be built from only ever confirms that it
was built that way.

**A mask animates through the machinery effects already had.** Each of its seven
numbers answers to a reserved parameter name — `mask.x`, `mask.feather` and so
on — so the stopwatch, the keyframe navigator, the curve picker, the marks on
the clip, the copy of a stack, a saved preset and the file format all carry a
mask's animation without one of them having learned what a mask is. The value
still lives on the mask, which stays its one home; only the keyframes live under
a parameter name.

Two things had to follow it. The outline on the picture is drawn from the
*resolved* mask, or it stops meaning anything the moment a number is animated.
And dragging the shape goes through the same setter a number does, so it writes
a keyframe when the property is animated — found on screen, where a drag moved
the outline and the render ignored it.

**The shape of what is left.** Nothing. Every row in section 1 is done, and the
catalogue is a branch at a time rather than a budget whenever anybody wants more
of it. This paragraph has said "nothing" before while two rows above it were
open, so: rows 4 and 7 were the last two, 7 turned out to have been done all
along, and 4 was finished by taking Add Effect and Clear across the selection.

The anti-flicker filter went in where the source is *read* rather than as a pass,
which is the whole of what it is: a still full of one-pixel detail shimmers
because which source rows survive the resampling changes from frame to frame, and
the only place to prevent that is where the rows are read. An effect running
afterwards would be softening an image that had already lost the rows. A layer
already read through it once — anything that went to the scratch — is exempt on
the way out, or every layer with an effect on it would be softened twice.

Time remapping is done, and it is worth saying what it cost, because the answer
was almost nothing: Speed became an animatable property like any other, so the
stopwatch, the navigator, the curve editor and the file format all carried it
without changing. The one new idea is that a rate is not a value — which source
frame shows at a moment is the *integral* of the speed up to it, and that
integral is taken through `eval_keyframes`, so the curve it follows is the
curve the graph draws.

Audio deliberately does not follow the ramp, which is what Premiere does too: a
speed ramp on the picture would be a slide in pitch on the sound, and avoiding
that needs a continuously varying retime rather than the fixed stretch a
constant speed uses.

The path was the last piece of machinery here, and the one that finally needed
something the root constants could not hold: sixty-four DWORDs will carry a
centre, a size and a rotation, and will not carry a shape somebody drew. So a
path's corners go in a buffer — one buffer for the whole frame, because the
command list is recorded once and submitted once, and a buffer rewritten between
draws would be read by every one of them with whatever it ended up holding. Each
pass is told where its own run starts instead.

Bins also turned up a fault nobody had noticed in the tree they live in: a
folder heading ignored its own depth, so a category inside a category started
where its parent did. The catalogue had been nested for weeks and read as a flat
list of headings the whole time.

Everything above has been driven by hand on screen as well as tested. That pass
found two faults nothing else would have: a keyframe selection thrown away by
the panel rebuild, so a bezier handle could be dragged exactly once; and the
mixer naming a track by its internal id.

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

Targeting is done, and insert and overwrite with it. A **T** in every track
header says where the next keyboard edit lands; `,` inserts the source at the
playhead and ripples everything after it along, `.` overwrites. Neither is
offered when there is nothing to place or nothing targeted — a sequence with no
target has no answer to *where*, and guessing is how an edit lands on a track
nobody was looking at.

Sync lock came with it, on by default as Premiere has it: an insert opens the
whole sequence up and nothing goes out of step, and turning it off pins a track
down — a music bed, a title at a fixed time, a bar of tone at the head. It is
deliberately not the same thing as Lock, which stops a track being edited at
all; a track can be freely editable and pinned, or locked and still rippled by
its neighbours.

It is reached from a **track header's right-click menu**, which is Premiere's
and which this had none of: a right-click on a header opened the *clip* menu,
offering Cut and Copy over a place where there is no clip. Rename, the four
switches, the height and the track commands are all there, and the rows that are
states are ticked rather than named for what they would become.

**The three-point row said "machinery" and "there is no source monitor to
mark", and both halves went stale when section 3 shipped.** The marks exist on
`Media`, `core::source_range` turns them into a `PlacementRange`, and insert and
overwrite both pass it — so the *source* half of three-point editing is done and
has been for a while. What is left is smaller than the row implied and is two
specific things:

Both are now done, through one pure function. `core::edit_points` takes the
project, the source and the playhead and answers where the edit lands and which
part of the source fills it; insert and overwrite go through it. With neither
sequence mark set it answers "the playhead", so the common case is byte for byte
what it was — which is the property that made it safe to put in front of every
placement.

- **A sequence in is where the edit lands.** It used to be ignored: the mark was
  stored, saved and drawn on the ruler, and the placement used the playhead
  regardless, so marking where an edit should go did nothing at all.
- **A sequence out alone back-times the edit** — the source is placed so that it
  *ends* on the mark. "Be out of this shot at exactly this moment" is a thing
  people mark, and it is the half of three-point editing that parking the
  playhead cannot do.
- **Both sequence marks derive the source's unmarked end.** Marked at the head,
  the source runs forward from its in for exactly the span; marked at the tail,
  it runs back from its out. Clamped to the file, so asking for twenty seconds
  out of a ten-second source takes the ten there are rather than inventing the
  rest.
- **Four marks that disagree are reported rather than resolved.** Nothing can
  satisfy all four, and choosing between retiming and trimming is a question,
  not an arrangement — so `edit_points` says `over_determined` and leaves it.
  **Fit to Fill** is the answer that keeps every mark: `core::fit_media_to`
  overwrites the marked span with the marked source retimed to whatever rate
  fills it, on the Timeline menu, greyed unless there is an actual conflict.
  Offering to "fit" a source that already fits would be a command that retimes
  by 100%.

**All five of Premiere's answers are built**, in `core::fit_media` and covered
by tests: change the speed (keeps every mark), trim the source's head or its
tail (gives up one source mark), or ignore the sequence in or out (gives up one
destination mark). Each is an overwrite with a different pair of "where" and
"how much of the source" — which is why this is a small function rather than a
subsystem, and why leaving four of them out would have been a choice rather
than a saving.

**And the box is built.** It was blocked on a widget rather than on effort:
Premiere's Fit Clip is a column of **radio buttons**, and the widget layer had a
Checkbox and a Dropdown and nothing that is exclusive and visible at once.
Building the group out of checkboxes would have drawn ticked squares where
Premiere draws filled circles; a dropdown would have hidden four of the five
choices behind a click. Either is inventing an arrangement instead of matching
one, so the widget came first.

**`ui::RadioGroup`** is that widget, and it is not only for this — a radio group
is what Premiere reaches for whenever a question has three or four exclusive
answers, and this application had been answering those with dropdowns for want
of anything else. Two things about it are worth keeping:

- **One widget, not a box of small ones.** A radio group is a single stop for
  the keyboard and the arrows move the *selection* within it, which is how every
  radio group on this platform behaves. Built out of children, each row would
  take its own tab stop and the arrows would mean nothing.
- **A circle is a rectangle with its corners as round as they go.** There is no
  ellipse primitive and none is needed: half the width is exactly a circle at
  every size, and going through the same `fill` and `stroke` every other themed
  surface uses is what makes a radio look like it belongs in all four themes
  rather than like a shape somebody drew.

Fit Clip is on the Timeline menu, greyed unless the marks actually conflict,
and it opens the box rather than acting — the answer is a judgement about the
cut. Change Speed is the row it lands on, being the only one that keeps all
four marks.

Still to do here: the dialogue is not in the `--check` scene, so its layout is
covered by neither the pixel tests nor the four-theme sweep. It needs a project
with four marks and a source to build against, which is a scene rather than a
line.

What stands in for the source monitor is the **pool's selection**, which is
already what a double-click places. That is also why source patching does not
exist separately: with one source, targeting answers both questions at once.
The half that is genuinely missing is marking in and out *on the source*, which
needs a monitor to mark them in.

Two things were found by doing it. `place_media` chose the audio lanes and the
overwrite carved lanes zero upwards, so the two disagreed about where the sound
went the moment either moved — they share one function now. And `Key::Comma`
and `Key::Period` had been bound since the nudge was written and had **never
once arrived**: the virtual key for a comma is `VK_OEM_COMMA` rather than the
character, so the translation dropped it and the binding sat there doing
nothing. Nudge has moved to alt and the arrows, which is where Premiere keeps
it.

| | Premiere | Here | Size |
|---|---|---|---|
| ~~Source patch (V1/A1 indicators)~~ | drag to choose which track receives | **done** — through the target toggle rather than a second control; see below | — |
| Track targeting for keyboard edits | per track, toggled | **done** — a T in every header, and what insert and overwrite aim at | — |
| Insert (`,`) and Overwrite (`.`) | from the source monitor at the playhead | **done** — from the pool's selection, on Premiere's own keys | — |
| Three- and four-point editing | in/out on source and sequence | **done** — `core::edit_points` resolves all four marks; four-point is Fit to Fill | — |
| Sync lock | which tracks ripple together | **done** — on by default, on the header's own menu | — |
| Mute / solo / lock / hide | yes | **done** | — |

**The source patch needed no control at all, and the row's size was wrong.**
Premiere draws a V1/A1 box in every track header *beside* the target toggle,
because a source can be patched to a lane that is not targeted for keyboard
edits. Here the target toggle is both: the targeted video track takes the
picture and the targeted audio track takes the sound. Targeting was already a
per-track flag that any number of tracks could carry, so "V2 and A3" was
expressible from the first day — placement simply read the first target and
derived everything else from it, which is why aiming the sound anywhere but the
lane paired with the picture was impossible.

So this was a bug wearing a feature's clothes. What went in is
`core::PlacementTargets`, which carries both lanes, and an `edit_audio_target`
beside `edit_target`. The converting constructor is what kept fifty call sites
unchanged, and it preserves both old readings exactly: a video track takes the
picture, and an audio track takes the sound — the second being the only way a
source with no picture was ever aimed anywhere but A1.

The overwrite is the case worth having a test for. It reserves the lanes it is
about to fill so that the hole and the clip land in the same place; re-deriving
the sound's lane on the way through would carve A1 and fill A3, which is the
exact fault that reservation was added for.

### 2.2 Trimming

We have slip, slide and rate stretch as **tools** — pick the tool, then drag.
Premiere has them as tools *and* as gestures on an edge with a modifier held,
and the second is what anybody actually uses: the tool palette is for the times
you want the mode to stay.

| | Premiere | Here | Size |
|---|---|---|---|
| Drag an edge to trim | yes | **done** | — |
| Slip, slide, rate stretch | tools; Premiere's *modifiers* are ripple and roll | **done** — control ripples, control and shift rolls; slip and slide are tools in Premiere too | — |
| Ripple trim (edge drag that closes the gap) | yes | **done** — a tool, on Premiere's B | — |
| Rolling edit (both sides of a cut at once) | yes | **done** — a tool, on Premiere's N | — |
| Trim to playhead (`Q` / `W`) | yes | **done** — both, rippling, on the targeted track | — |
| The trim monitor (two-up while trimming) | yes | none | machinery |
| Nudge by frame | yes | **done** | — |

Ripple and roll were the two that mattered, and both are done — as tools, on
Premiere's own B and N, sitting between the pointer and the razor where
Premiere puts them.

They are not `set_clip_edge` plus an arrangement pass, which is what an earlier
draft of this page assumed. What separates a trim from a ripple is **what stops
it**: an ordinary trim is bounded by the clip beside it, and a ripple pushes
that clip along instead, so the neighbour cannot be both. The clamp is now a
shared `edge_room` with that one difference as a flag, which is also what makes
a roll expressible — a roll is the same clamp taken on both sides of a join at
once, since either side running out of source is what stops it travelling.

Two things fell out of it. A ripple on a *head* leaves the clip where it is and
takes the length off the front, because that is the net of trimming it and
closing the gap; the drag shows that rather than showing a trim and rearranging
on release. And because of it the edit has to report the edge it ended on
separately — `result` says the clip starts exactly where it did, so it cannot
say where the trim went.

**The row about modifier gestures was wrong.** It claimed Premiere offers slip,
slide and rate stretch as modifiers as well as tools. It does not — those three
are tools there too, and what Premiere puts on a modifier is the pair this
already had as tools: **control makes a trim a ripple, control with shift makes
it a roll**. That is the version that is here now, and it is the more useful
one anyway, because the tool palette is for when the mode should *stay* and one
tightened cut is not that.

Building it turned up something the tool version had been hiding. All six edge
modes shared one cursor, on the argument that the lit button in the palette says
which of them you are holding. Reached by a modifier there is no lit button, and
nothing anywhere said a trim had become a ripple — so ripple and roll have their
own cursors now, drawn from the same art as their buttons. The old rule was
right about the tools and silent about everything else.

Control also wins over the fade handles and the volume band, which sit on top of
the clip: holding it is a statement that this gesture is a trim.

**Q and W trim to the playhead**, both rippling, on the targeted track. They act
on **one** edit point rather than every targeted lane at once, which is a
deliberate limit: a ripple closes the sequence up by however much *that* clip
lost, and a second clip on another lane crossing the same playhead has its own
available source and so its own answer. Applying both in turn would ripple twice
for one keystroke.

### 2.3 Clips on the timeline

| | Premiere | Here | Size |
|---|---|---|---|
| Drag from the pool onto a track | yes | **done**, at the point of release | — |
| Drag a clip to move it | along the track **and between tracks** | **done** — the second half was missing and the row said otherwise | — |
| Copy and paste clips | Ctrl+C / Ctrl+V, and paste-insert | **done** — copy, cut, paste and paste-insert | — |
| Duplicate (alt-drag a copy) | yes | **done** — alt at the press, with the originals drawn where they will be left | — |
| Label colours | eight, set per clip and per bin | **done** per clip, by name, from the clip menu | — |
| Enable / disable a clip | yes | **done** — on the clip menu, ticked, and a switched-off clip is drawn as one | — |
| Speed / duration dialogue | a box with both and a ripple option | **done** — speed, duration, reverse and a ripple, from the clip menu | — |
| Right-click a clip | the menu most edits are actually reached from | **done** — and it offers only what would do something | — |
| ~~Nesting~~ | a sequence inside a sequence | **done** — Nest on the clip menu, and a sequence is a source in the pool | — |
| Multi-camera | a multicam source sequence, switched live while it plays | none | machinery |

**Nesting is a compositing boundary, and that is the whole of why it exists.**
Six shots with one grade over all of them is one adjustment on a nest and six
identical stacks without one — and the second is not even the same picture,
since each clip would be graded before it was composited rather than after. It
follows that a nest cannot be flattened at plan time: splicing the inner layers
into the outer list applies the nest's effects to each of them separately, which
is precisely the thing being avoided.

**A sequence is a source in the pool**, which is Premiere's arrangement and the
reason it is worth copying: a nest is then an ordinary clip of an ordinary
source, so trimming, effects, a transform, a transition and a label all work on
one without anything being told nests exist. The alternative — a clip naming a
sequence directly — would have meant every reader of `media_id` growing a second
path, and a sequence that could not be dragged in from the pool at all.

**The pool entry mirrors the sequence, and is reconciled on every edit.** Its
name, length and canvas all change when somebody cuts inside the nest, and
`Session::apply` is the one door every edit goes through — the alternative is a
list of callers that has to stay complete for ever.

**Rendering is a renderer per nest rather than recursion into one.** A
compositor holds one scene target and a nest has to be composited whole before
the frame it sits in is; two renderers means two targets, two sets of decoders,
and no state to unwind between them. The picture crosses back as a texture that
never leaves the card — and it needed no shader work at all, because the display
target is already coded R'G'B' with straight alpha, which is exactly what the
shader's `LAYOUT_CODED` path samples untouched. Feeding it as `Rgba8` would have
been wrong in a way only a partly transparent nest would show: that layout is
premultiplied, and the shader divides the alpha back out.

**Sound is a mixer per nest, and it is rendered up front.** That is the one cost
this carries: a long nest is decoded before playback begins, which is exactly
what `AudioMixSettings::start_at` stopped doing for everything else. Mixing one
on demand means running a mixer inside the mixing thread, where nothing may
touch a file at all. A nest is deliberate and usually short, and this is the
place to start if that stops being true.

**Cycles are refused when a nest is made, not detected later.** There is no
sensible behaviour for one: rendering recurses until the stack runs out, and any
depth limit turns a wrong project into a differently wrong project.
`sequence_contains` walks the whole graph, because the case that matters is not
A inside A — it is A inside B inside A, which no check at the moment of nesting
would catch. Both the renderer and the mixer still carry a depth limit, and that
is for a project which already holds a cycle: one written by hand, or by a build
that did not check. Past it the nest is a visible hole rather than a crash.

**Alt-drag is a move that reports itself differently.** The timeline knows
nothing about duplicating: the gesture is captured, clamped, snapped and
previewed as a move, and the only thing alt changes is a flag on what it hands
back. Making the copies is the project's business, which is where the fresh ids
and the group remapping belong.

Alt is read at the *press* and never again. A modifier picked up halfway through
would change what the gesture is while it is being made, after the picture has
been showing the other one for as long as the hand took to get there.

The originals are drawn during the drag, faint and outlined, where they will be
left. They have to be painted from the arrangement at the press, because the
preview drags the real blocks rather than inventing copies to drag — the model
mid-gesture has already forgotten they were ever there. Without them an alt-drag
and a plain drag look identical until the mouse comes up.

And the copies end up selected, not the originals. The nudge or the second drag
that usually follows should act on what was just put down.

**The Speed / Duration box is one number seen two ways.** A clip's length is
its source span divided by its rate, so speed and duration are not two settings
to keep in step — they are the same setting, and the box mirrors each field into
the other as it is typed rather than waiting to disagree at the end. Typing a
duration is the reason it exists at all: "make this four seconds" is a thing
people want, and working out that it means 137% is not.

The ripple is the half that makes a retime usable on a cut sequence. Without it,
slowing a shot down either overruns the next clip or leaves the gap it used to
fill, and both are somebody's afternoon. It moves everything past each retimed
clip's old end, on every track a sync lock holds together, so it is the same
edit sync lock already governs rather than a second idea about what moves.

Driving it turned up something much worse than a missing feature, and not in
this at all: **ids collide after a project is opened.** The counter starts at
zero every run, the file already holds `clip_4`, and the next split hands that
name out again — after which an edit aimed at one clip lands on both. It showed
here as a retime on the second half of a cut also retiming the first half's
audio. Fixed at the load path, which is the only place that can know.

**Copy and paste was the surprise**, and it is done. There was a clipboard for
effects and one for keyframes and none for clips — the single most-used pair of
keys in any editor did nothing on the timeline.

The clipboard lives on `Session` rather than on the application, which is what
makes Copy and Paste ordinary commands: bound like every other key, greyed by
`can_run` when there is nothing to paste, and reachable from the Edit menu and a
keystroke through one path. It holds whole clips by value — source range,
transform, keyframes, effects, fades — because a clipboard of ids goes stale the
moment what it names is trimmed.

Four decisions worth keeping. A paste **overwrites**, which is what dropping a
clip on another already does, and paste-insert (Ctrl+Shift+V) is the one that
ripples. **Cut lifts** rather than extracts: the gap stays, and closing it is
Ripple Delete, which has its own key. Groups are **remapped** on the way in, so
a pasted A/V pair is linked to itself and not to the pair it came from —
otherwise moving the original would drag the copy along. And what was pasted
becomes the selection, so the nudge or the drag that usually follows acts on the
copies.

### 2.3a What the interface tells you before you press

Not a Premiere feature so much as a class of them, and it went unlisted because
nothing here was *missing* — every gesture worked. What none of them did was
say so first.

| | Premiere | Here | Size |
|---|---|---|---|
| A cursor that changes | tool cursors, trim cursors, resize, I-beam | **done** — the application had exactly one cursor from the first widget until now | — |
| Hover feedback on a clip's zones | the trim zone lights | **done** — the trim handle under the pointer, and the razor's cut line | — |
| Hover feedback everywhere else | yes | already had it — buttons, menu rows, splitters, tabs | — |
| A snap that says it snapped | a line at the edge it stuck to | **done** — a line at what it stuck to, gone the moment it pulls away | — |
| Tooltips | yes | **done** — on every icon button and toolbar control, with the key | — |

**The cursor never changed.** `window_class.hCursor` was `IDC_ARROW` and nothing
ever called `SetCursor`, so a clip's trim edge looked exactly like its middle, a
splitter looked like the gap between two panels, a field looked like a label,
and the tool you were holding was visible only in a toolbar a long way from
where you were working. `ui::Cursor` is an enum a widget answers with, `Arrow`
meaning "nothing to say" so the question passes to the parent rather than being
blanked out by whatever happens to be on top.

The six tool cursors are **rendered from the same art as their buttons** — the
icon drawing came out of `IconButton::paint_content` into a free `draw_icon`,
and both call it. A razor that looked one way in the palette and another under
the pointer would be two razors, and the second would drift the first time
either was touched.

**The timeline was the only interactive surface with no hover feedback at all.**
Every button, menu row, splitter and dock tab lights up under the pointer; the
one place where a single pixel decides between moving a clip and trimming it
said nothing. The trim handle under the pointer is now drawn, and the razor
shows the line it would cut on — the tool whose whole gesture is one click is
the one that most needs to say where it would land.

Snapping says so now, with a line at the time the drag has stuck to, drawn while
it is stuck and gone the moment the pointer pulls away. It is recomputed on
every move rather than latched, which is what stops a line being left behind at
the last place a drag happened to catch.

Tooltips are done, and they matter most where an icon has no words at all: each
says what the control is and which key does the same thing. Timing lives in the
application, because a widget layer with no clock cannot say how long a pointer
has rested; drawing lives in the host, where the theme is. `Part::Tooltip` had
been styled in all four themes since the themes were written and used by
nothing.

The icons themselves were too small to read — a flat four pixels of reach
whatever the theme said, which is eight pixels across for marks that have
internal structure. They scale with the text now.

### 2.4 Reading the timeline

| | Premiere | Here | Size |
|---|---|---|---|
| Waveforms and filmstrips on clips | yes | **done**, cached and drawn on workers | — |
| Volume rubber band | yes | **done**, with keyframes | — |
| Keyframe marks on a clip | yes | **done** | — |
| Snapping | yes | **done** | — |
| Zoom, and zoom to fit | wheel, keys, and the scrollbar | **done** — wheel about the pointer, `=`/`-` about the playhead, `\` to fit, and either end of the scrollbar | — |
| Scrollbars | horizontal (which also zooms) and vertical | **done** — neither appears when it would have nothing to say | — |
| An fx badge on a clip with effects | yes | **done** | — |
| Track height, per track | dragged, and expand/collapse all | **done** — dragged on the line under a header, double-click puts it back | — |
| Markers with duration and comment | yes | **done** — a band on the ruler, and a box on a double-click | — |
| Scroll to follow playback | smooth or page | **done** — pages, so the picture is not always moving | — |
| Timecode field to type into | yes | **done** — typed into to go there, forgiving about what is typed | — |

Both of the last two are done. The row claiming the playhead ran off the edge
during playback was **stale** — `follow_playhead` has been wired since the
preview landed, and it pages rather than creeping, so the picture is not always
moving. Checking a row against the source rather than against the page is the
second time that has been worth doing.

The timecode is a field now rather than a label, and the parser is the part
worth stating: the last field of a timecode is *frames*, so reading it with
`time_to_seconds` is half a second out at 30 fps. It is forgiving about what is
typed, because that is what these are for — fewer fields count from the right,
so "15" is fifteen frames and "2:15" is two seconds and fifteen — and anything
that does not parse leaves the playhead where it was rather than sending it to
zero on a typing mistake.

---

## 3. Source monitor

There is none. That is the whole of the section and the reason it is next:
Premiere's editing workflow is *two* monitors, and an application with one of
them makes you do on the timeline what should have been decided before anything
was placed.

The gap is not machinery in the usual sense. Almost everything a source monitor
needs is already built and already reachable from somewhere else — what is
missing is the panel that would put the pieces in one place, and two small
things underneath it.

### 3.1 What is already there

Worth saying first, because the size of this section depends on it.

| | Where it is now |
|---|---|
| A picture with a letterbox, a placeholder and a drop target | `ui::MonitorView`, used by the program monitor and not tied to it |
| Rendering a frame at a time | `app::ProjectPreview::frame_at` / `texture_at`, which take a **project** and a time |
| A source chosen to be placed | `Session::source_media`, set by the pool and read by insert and overwrite |
| Placing part of a source | `core::PlacementRange`, honoured by `place_media`, `insert_media_at` and `overwrite_media_at` |
| Marks that survive the file | `Project::in_point` / `out_point`, for the *sequence* |
| **Marks on a source** — **done** | `Media::in_point` / `out_point`, set by `core::set_source_in_point`, saved with the project |
| **Placing only the marked part** — **done** | `core::source_range` joins the two, and insert, overwrite and a drag from the pool all pass it |

`PlacementRange` was the one to look at twice. Three placement operations took
it, all three honoured it, it was tested — and **nothing anywhere set it**. The
range half of three-point editing had been finished and unreachable for as long
as it had existed, waiting for something to say which part of the source.

That is now closed. The marks live on `Media`, `source_range` turns them into a
`PlacementRange`, and the three ways a source reaches the timeline — insert,
overwrite, and a drag from the pool — all pass it. What is still missing is a
way to *set* them by eye, which is the panel.

### 3.2 What has to be built

| | Premiere | Here | Size |
|---|---|---|---|
| ~~A panel showing one source~~ | yes | **done** — beside the program monitor, as Premiere has it | — |
| ~~Its own playhead and transport~~ | play, step, JKL, shuttle | **done** — scrub, step, play with its sound, Home/End; no J/L shuttle | — |
| ~~A scrub bar with a marked span~~ | the time ruler under the picture | **done** — `ui::ScrubBar` | — |
| ~~Mark in and out **on the source**~~ | `I` and `O` while it is focused | **done** — routed by where the keyboard is | — |
| ~~Marks that belong to the asset~~ | kept per clip, and saved | **done** | — |
| ~~Insert and overwrite the marked part~~ | the point of the whole panel | **done** | — |
| ~~Drag from the picture to the timeline~~ | yes | **done** — with the same ghost the pool's drag shows | — |
| ~~A list of recently opened sources~~ | a dropdown of them | **done** — the name of what is showing *is* the chooser | — |
| ~~Audio-only sources shown as a waveform~~ | yes | **done** — `ui::WaveformView` | — |
| Display mode, safe margins, zoom | yes | **zoom is done**, on both monitors; the other two are not — and not this section's, since they are both monitors' | audit |

**Section 3 is closed but for the last row, and the useful third of that row is
now done.** Display mode, safe margins and monitor zoom belong to the program
monitor as much as to this one, so building them into the source monitor alone
would be building them twice; they are carried in *Found by audit* below, as
**Safe margins**, **Monitor zoom level** and **Display mode**. Zoom was built
there, once, and both monitors have it.

Worth saying plainly what was weighed, so the deferral is a decision rather than
a thing that quietly did not happen. Safe margins are title-safe and action-safe
overlays for broadcast delivery, which is not what this application is being
used for. Display mode — composite, alpha, and the scopes — largely repeats what
the Scopes panel already offers, and offers it as a dockable panel that can sit
beside the picture rather than over it. Monitor zoom was the one of the three
with daily use, it was done first, and it is done.

**The picture costs nothing to render.** A source monitor is a sequence of one
clip, so it can be shown by handing `ProjectPreview` a project built on the spot
holding exactly that: one video track, one clip, the whole media. Nothing new
decodes, nothing new composites, and what the source monitor shows is by
construction what the sequence would show — which is the property that makes it
worth trusting when deciding what to place.

**The marks belong to the media.** Premiere keeps a clip's in and out with the
asset rather than with the panel, so closing a source and coming back to it
finds them still there, and saving the project saves them. That is a field on
`Media` rather than state on the session, and it is the only model change this
section needs.

**The scrub bar is the only genuinely new widget.** The timeline has a ruler
that does all of this and a great deal else besides; nothing small does. It
wants a duration, a playhead, a marked span, and a drag — and the program
monitor should have one too, which is a gap this page has not been listing.

---

## 4. Project panel

The pool works and is flat. Everything in a project is in one list, ordered by
one of five sorts and narrowed by one search box — which is enough to build a
sequence from a dozen files and stops being enough at a hundred.

### 4.1 What is already there

| | Where it is now |
|---|---|
| The pool as a list | `ui::MediaBrowser`, drawn rather than built as widgets, so hundreds of rows cost twenty |
| What each entry is | `ui::MediaKind` and its badge: video, audio, image, title, colour, adjustment |
| Ordering | `BrowserSort` — pool order, name, kind, duration, and least-used-first |
| Narrowing | `BrowserOptions::search` |
| Knowing a file has gone | `MediaItem::offline`, filled from `ProjectPreview::missing_media` |
| Knowing what is unused | `MediaItem::uses`, which is what least-used-first is for |
| Renaming, removing | `rename_media`, `remove_media` — the second takes the clips with it |
| Getting media onto the timeline | double-click, drag with a landing ghost, insert and overwrite |

Two things are worth saying plainly because they change what is left to build.

**`uses` and `offline` are already there**, so "find what this project does not
need" and "find what has gone missing" are both answerable today — they are
just answered by sorting rather than by a column you can see.

**Bins exist and are not these bins.** `editor::Bins` gathers *effects* into
folders in the Effects panel. It is the same idea and none of the same code:
media bins live in the project and are saved with it, and effect bins are a
fact about the person and are saved beside the presets.

### 4.2 What has to be built

| | Premiere | Here | Size |
|---|---|---|---|
| ~~Bins~~ | folders of media, nested, saved with the project | **done** — New Bin, nested, drag to file | — |
| ~~Relinking~~ | point a moved file at its new home, and the clips follow | **done** — Project ▸ Relink Media | — |
| ~~Proxies~~ | attach a small copy, edit against it, export from the original | **done** — Project ▸ Make Proxies / Use Proxies | — |
| ~~Metadata columns~~ | a real column view, sortable by clicking a heading | **done** — Name, Duration, Used, Kind | — |
| ~~Icon view~~ | thumbnails, and hover to scrub them | **done** — the List/Icons button | — |
| ~~Labels~~ | a colour per entry, carried onto its clips | **done** — the pool's Label menu | — |
| ~~New sequence from a clip~~ | drag onto "New Item", or the menu | **done** — the pool's menu, shaped to the footage, with the clip on it | — |

**Relinking is the smallest of these and the one that loses work without it.**
Everything needed is present: a source knows its path, `missing_media` says
which paths did not open, and clips name media by id rather than by path — so
repointing one entry repairs every clip that used it. What is missing is a
dialog and one setter.

**Proxies were the one with teeth**, and are done. A second path on the
`Media`, a switch on the project, one function deciding which file a source is
read from, and a background transcode that writes 540p copies beside the
footage. The renderer defaults to *not* using them, so the exporter ignores the
switch by doing nothing rather than by remembering to turn it off.

What was harder than the model: keeping the proxy the same footage. Frames are
held to the source's frame rate rather than emitted one for one, so a
variable-rate source is written as constant-rate by repeating and dropping —
without that, a proxy runs at the wrong speed the moment a camera drops a
frame, and every cut made against it moves when the original comes back.

Left out on purpose: no automatic proxy on import (a card of footage would
transcode for an hour uninvited), and no proxy for audio, which is decoded from
the original wherever it is wanted and cheap enough that a smaller copy would
buy nothing.

**Bins are done**, and the shape of them is worth recording: a bin holds
nothing. It is a name and a place in a tree, and what is *in* it is whatever
names it — one field on the media entry rather than a list on the bin. That is
the only arrangement where filing a clip cannot leave two containers
disagreeing about where it is, and it is why deleting a folder cannot lose
footage: media naming a bin that has gone reads as top level.

Deleting a bin from the panel *does* take its contents, which is Premiere's
behaviour and the destructive one, so anything holding something asks first.
The structural half (`core::remove_bins`) and the destructive half
(`editor::remove_bin`) are separate functions for that reason.

**Columns and the icon view were the same feature seen twice** and both are
done. The columns replaced the button that cycled through the orderings — the
order belongs on the column it applies to. The icon view is a grid of tiles
where the pointer's position across a tile picks the frame, so running along a
picture walks the source; that is the part worth having, and a grid of stills
from the middles of files would not have been.

**New sequence from a clip needed something this project did not have: more
than one sequence.** It has it now — see the Sequences entry at the bottom of
this page — and the row above is done. What follows is what the problem looked
like before it was solved, kept because the audit's mistake about it is the
useful part.

A `Project` held its tracks directly — there was no
`Sequence` type — so "new sequence from this clip" has nowhere to put the
sequence it would make. The audit called this row "wiring" and that was wrong.

The useful half of it already happens: `match_sequence_to` sets the canvas and
frame rate from the first thing imported, so a project started from 4K footage
is a 4K project without anybody being asked. What is missing is the ability to
have a second sequence at all, which is a model change (a list of sequences, a
notion of which one is open, a tab strip to switch them) and a much larger
piece of work than the rest of this section put together. It belongs in a
section of its own rather than as the last row of this one.

## 5. Application settings

Premiere keeps three separate things here and it is worth keeping them apart,
because they have different lifetimes and belong in different files:
**Preferences** belong to the person and outlive every project; **Project
Settings** belong to the cut and travel with the file; **Keyboard Shortcuts**
belong to the person and are a thing people carry between machines.

### 5.1 What is here now

| | Where | What it does |
|---|---|---|
| Theme | Settings ▸ Application Settings | Four built-in themes |
| Still / transition / autosave | Settings ▸ Application Settings | The three durations with no right answer |
| Sequence size | Project ▸ Project Settings | Presets and a typed size |
| Frame rate | Project ▸ Project Settings | Presets and a typed rate |
| Use proxies | Project ▸ Use Proxies | A tick |

**And all of it is remembered**, which was the finding this section turned on.

### 5.2 The finding that mattered more than the list — **done**

**Nothing in either popup was remembered.** There was no settings file. `App`
held the theme as a plain member, written by `set_theme` and read by nothing
else, and every launch started on theme zero.

`settings.json` is now the fifth thing under `%APPDATA%\Cutline`, beside
`recovery`, `workspaces.json`, `presets.json` and `bins.json`. The pattern was
already established three times over; what was missing was the file, not the
machinery.

It was not only the theme. Every one of these used to reset on every launch and
now does not:

| Setting | Where it lives now |
|---|---|
| Theme | `App::theme` |
| Snapping | `App::snapping` — the toolbar's Snap button |
| Looping | `App::looping` |
| Aspect lock | `App::aspect_locked` |
| Preview quality | `App::preview_scale` — the Full/Half/Quarter dropdown |
| Pool ordering and direction | `App::browser_sort`, `App::browser_descending` |
| Pool view | list or icons |
| Export codec and quality | `App::export_setup` — **still not saved**, see below |

The workspace was the odd one out and the proof that this was a gap rather than
a decision: arrangements *were* saved, because somebody sat down and wrote
`workspaces.json`. Everything else had been left where it was first put.

Two things learned in the building, both worth keeping:

**The theme is stored by name.** The list is ordered for reading rather than
for stability, so an index would quietly mean a different theme the first time
one was inserted. The sorts are written as names for the same reason.

**An absent key takes its default rather than reading as false.** That is the
trap this shape invites: absent booleans read as "off" would have switched
snapping off for everybody the first time they upgraded.

The export codec and quality are deliberately still not saved. They are a
property of an *export* rather than of the person, and the export dialog is
section 8's business — remembering them here would mean deciding in this
section what the export presets should be.

### 5.3 A bug found while auditing this — **fixed**

**A still image placed as a clip with no length.** Imported, `cutline.png`
showed in the pool as `still`, dragging it to the timeline reported `Used 1` —
and nothing was drawn on the track, at any zoom.

`probe_source` sets `duration` from what libavformat reports, and libavformat
calls a PNG a one-frame video at its own default rate. `is_image` is set
correctly and `is_still_like` is honoured everywhere it matters, but nothing
ever replaces that duration with a length worth placing. `core::place_media`
then uses `media.duration` as the clip's `source_out`, and the clip is a
fortieth of a second long.

Premiere calls the fix "Still image default duration" and puts it in
Preferences ▸ Timeline, at five seconds. Here it was not a missing preference so
much as a missing default — the preference is what makes it adjustable
afterwards. `kDefaultGeneratorLength` was already 5.0 for titles and mattes,
which is the same question answered correctly one layer along.

Both halves are done: `kStillLength` is the default, and Settings ▸ Defaults is
where it is changed.

### 5.4 What Premiere has that we do not

Premiere's Preferences has nineteen panes. Most are for things this application
does not do — Capture, Device Control, Control Surface, Collaboration, Sync
Settings — and are not gaps. What is left, with an honest size on each:

| | Premiere | Here | Size |
|---|---|---|---|
| ~~**Anything is remembered at all**~~ | every preference persists | **done** — `settings.json` | — |
| ~~Still image duration~~ | Timeline ▸ default duration | **done** — Settings ▸ Defaults | — |
| ~~Transition durations~~ | Timeline ▸ video and audio defaults | **done** — one length, not two | — |
| ~~Auto Save interval~~ | Auto Save ▸ every N minutes | **done** — Settings ▸ Defaults | — |
| ~~Auto Save versions kept~~ | Auto Save ▸ maximum versions | **done** — five by default, oldest pruned | — |
| ~~Undo depth~~ | historically a preference | **done** — Settings ▸ General, applied to the open document | — |
| ~~Label names~~ | Labels ▸ eight named colours, editable | **done** — names only; the colours cannot move, see below | — |
| ~~Default label per media type~~ | Labels ▸ per kind | **done** — applied at import | — |
| ~~Audio device~~ | Audio Hardware ▸ device, latency | **done** — device; latency is still WASAPI's | — |
| ~~Playback preroll / postroll~~ | Playback | **done** — Settings ▸ Playback, around a looped range | — |
| ~~Media cache location~~ | Media Cache | **done** — and the cache under it, see §5.8 | — |
| ~~Proxy settings~~ | Ingest Settings ▸ preset, location | **done** — size and location | — |
| Memory / RAM reserved | Memory | none — the caches have fixed budgets | **won't do**, see §5.6 |
| ~~Renderer choice~~ | Graphics ▸ GPU or software | **done** — Settings ▸ Graphics, read at startup | — |
| Appearance brightness | Appearance ▸ a slider | four discrete themes | **won't do**, see §5.6 |
| ~~Keyboard shortcuts~~ | fully editable, with presets | **done** — see §10 | — |
| ~~Timecode display format~~ | Project Settings ▸ display format | **done** — drop-frame, and the counting under it fixed | — |
| Scratch disks | Project Settings ▸ Scratch Disks | proxies go beside the footage, nothing else is written | **won't do yet**, see §5.6 |

Premiere keeps a *separate* default duration for video and for audio
transitions. There is one here, because there is one `default_transition_length`
and nothing yet distinguishes an audio join from a video one at the moment a
transition is added. Splitting it is a row's worth of work whenever somebody
wants different lengths; nobody has.

### 5.5 The shape this took, and what is left

**One file, `settings.json`, beside the other three.** The three existing
readers were the template: a struct, a read that treats absence as "nobody has
set anything yet", and a write on change. That commit closed the row that
mattered most and made every other row here a matter of adding a field — which
is exactly how the durations went in, one commit later.

**Preferences and per-project settings must not end up in one dialog.** The
sequence size belongs to the cut and travels with it; the theme does not. They
are already two popups and should stay two — the mistake to avoid is a single
"Settings" window that quietly saves half its contents to a different place
from the other half.

**The fixed constants are not all worth exposing.** `kThumbnailThreads`,
`kProxyThreads` and the cache budgets were each chosen against a measurement
and written down with the measurement beside them; a control offering somebody
a worse answer than the measured one is a control that only creates bad
sessions. The ones worth exposing are the ones where there is no right answer —
durations, the autosave interval, the label palette — and those are exactly the
ones Premiere exposes.

**The label colours are deliberately not editable**, which is where this parts
company with Premiere. A clip stores its label as a hex rather than as a
position in the palette, so recolouring Violet would leave every clip already
wearing it holding the old colour — a palette and a timeline that disagree.
Premiere can offer it because its labels are stored by position and follow.
Changing that here means changing what a project file holds.

**Drop-frame turned up a bug beneath it.** The frame index was counted at the
nominal rate, so 29.97 skipped a timecode number about every thousand frames
and the readout stuttered while the picture did not. The index comes from the
actual rate now — which means non-drop at 29.97 correctly falls behind the
clock, about 3.6 seconds an hour, and an hour of sequence reads 00:59:56:12.

**Keyboard shortcuts are the largest single item in this section** and probably
larger than the rest of it together: a command table with stable names, a
binding store, a resolver that runs before the widget tree, a conflict check,
and a panel to edit it in. The bindings are already two tables of structs, which
is the right starting shape, but they are `constexpr` and matched by hand in two
places. Worth its own section rather than a row in this one.

### 5.6 The rows that are deliberately not built

A row closed with a reason is worth more than a row left open, so each of these
says what was weighed rather than quietly staying blank. All four follow from
the same rule the section was run on: **expose what has no right answer, leave
what was measured.**

**Memory / RAM reserved — won't do.** The caches have fixed budgets, and every
one of them was chosen against a measurement written down beside it in the code.
A control here cannot offer a *better* answer than the measured one; it can only
offer a worse one, and the sessions it creates are the bad kind — a preview that
stutters for a reason nobody can see, set months ago. Premiere exposes this
because Premiere shares a machine with After Effects and Media Encoder and has
to be told how to divide it. Nothing here does.

**Media cache location — this was declined and then built**, and the reasoning
that declined it was sound at the time and wrong about the size of the prize.
The row was closed as "there is no disk cache to point anywhere, and a control
over the location of a thing that does not exist is worse than no control".
That is still the right rule; what was missing was any measurement of what the
absence cost. See §5.8.

**Appearance brightness — won't do.** Premiere's slider works because its
appearance is one generated palette with a lightness parameter. There are four
*themes* here, and they are not recolours: `xp` has bevels, `aero` has glass,
each owns its own metrics, and the whole widget layer is built so that a theme
can change chrome rather than colours. A brightness slider means themes
generated rather than declared — a different design for the theme layer, not a
control on top of it — and it would buy a dimmer Luna, which nobody has asked
for. Somebody who wants a lighter interface wants a light theme, which is a new
theme and a much smaller piece of work.

**Scratch disks — not yet, and worth doing when there is a second thing to
place.** Proxies have a folder, on their own page, because proxies are the one
thing this writes that can be large and can want to be elsewhere. A "Scratch
Disks" page today would be that one row moved and given a grander name. The
moment a second kind of file needs a home — a disk cache, rendered previews —
the two belong together and this becomes worth building.

### 5.7 What closing the section turned up

Two things, and the second is the reason the driving step is not optional.

**A preference read once has to say so.** The renderer choice takes effect when
the application restarts, because the device is made at startup and everything
that draws — the compositor, each window's swapchain, Skia's context — is built
on it. Nothing can move those to another adapter while they are open. So the
page says which renderer was *chosen* and which one is *in use*, and those
disagree until a restart. That is worth more than it sounds: a machine quietly
running on WARP because no adapter would have it is exactly the machine somebody
opens this page to ask about, and now it answers.

**Looping never restarted when the range ran to the end of the sequence**, and
postroll is what made that reachable. Found by driving, invisible to the tests,
and two faults deep:

- The player stops itself at the end — reaching it clears `running` — and
  `advance_playback` guarded on "is the player playing". So it returned at its
  first line at exactly the moment there was something left to decide, and the
  loop-round it would have done sat unreachable behind that guard. The frame
  loop had the *same* guard and would go back to blocking on its message queue,
  so nothing even woke to notice. There is a comment beside the export case in
  that loop describing this identical shape; it had not been applied here.
- Underneath it, `Player::seek` only cleared the at-the-end flag on the render
  thread when the flush landed, while `Player::play` sends a finished player
  back to zero. Looping does exactly `seek` then `play`, so the seek would have
  been discarded — a bug that could never be observed while the first one kept
  the code from running at all.

Both are fixed, and the second has a test. Measured on screen by sampling the
playhead's position off the picture every 20 ms: with the rolls at zero a loop
over a range marked 4–6 s runs 3.99 → 5.97 and wraps every 2.0 s; with 1.5 s of
preroll and 2 s of postroll the same range runs 2.46 → 7.99 and wraps every
5.5 s, which is the marked span widened by exactly what was asked for.

### 5.8 The media cache, and what measuring it changed

Asked whether a media cache would smooth playback. The answer is **no** — and
saying so first is what made the rest worth doing.

**Playback is not what this touches.** Nothing on the playback path reads a
waveform or a filmstrip. What makes playback hitch is video decode: the reverse
stalls are group-of-pictures bound, and forward ran at about 40 fps on 4K60 —
which turned out to be a millisecond of timestamp rounding rather than the
upload it was blamed on here (handoff §9). Decoded frames already stay on the
card. Either way it is a different subsystem.

**Editing is, and the cost was far worse than this page had guessed.** Importing
the reference ten-minute capture — 1.5 GB, four audio streams — and dropping it
on the timeline took **over eleven minutes** of reading before the waveforms and
filmstrips were done, with a core and a half busy throughout. Every reopen paid
it again.

Almost all of that turned out to be waste rather than work, and it is worth
recording what it was:

- **Every packet in the container was demuxed and thrown away.** `read_audio`
  asked `av_read_frame` for everything and kept the one stream it wanted, which
  sounds free and is not — the demuxer still parses what it hands over, and on
  this material that is 4K HEVC interleaved with the audio. `AVDISCARD_ALL` on
  the streams not being read makes the container skip them outright.
- **The file was read once per audio stream.** Four streams meant four full
  passes over 1.5 GB to draw four lines. `media::extract_waveforms` reads them
  all in one pass, and `WaveformCache` batches every queued stream of the same
  file into one job.

Measured after those two, on the same capture: one waveform **0.7 s**, all four
together **1.4 s**. It had been minutes.

What was left is the filmstrip, and that is genuinely expensive: it seeks and
decodes per frame, about **0.47 s each** on this footage, 300 frames for a
ten-minute source. Seeking is the right access pattern for a handful of frames
spread across a long file, and it is what a cache is for.

`app::media_cache` is that cache — Premiere's Media Cache, under
`%LOCALAPPDATA%` by default and movable and emptiable from Preferences ▸ Media
Cache. Measured on the same capture, doing what dropping it on the timeline
does (four waveforms and a twenty-second stretch of filmstrip):

| | |
|---|---|
| no cache at all | 13.5 s |
| cold cache, filling it | 13.6 s |
| **warm cache** | **0.0 s** |
| stored | 2.18 MB |

Filling it costs nothing measurable, which is the property that makes it safe to
have on always.

Four things in it are load bearing:

- **The key is the path, the size and the modification time together**, rather
  than the path with the other two checked on read. A miss costs a decode; a
  false hit hands somebody a waveform belonging to footage they have replaced,
  and there is no version of that which is worth the saving.
- **A stretch is the unit, not a frame.** Filing frames under which step of a
  fixed grid they fell nearest lost one whenever two landed in the same step —
  which a short stretch does immediately, because a minimum frame count makes it
  sample more finely than the grid. The extractor spreads *n* frames evenly
  across whatever range it is given, so where they land is a property of the
  range: nothing smaller than the range identifies them. Caught by a test that
  asked for six frames back and got five.
- **Everything is refused rather than trusted on the way in.** Every length read
  is a number out of a file that a crash or a disk error can have made anything,
  and believing one is how a cache miss stops being a slow session and becomes
  an allocation the size of the number. A truncated or unrecognised entry is a
  miss.
- **Nothing here can fail an edit.** Every read and every write is allowed to
  fail silently: a cache that cannot be read is a slow session, and one that
  cannot be written is a slow session next time. Neither is worth a dialog.

**The icon view, found while measuring this and fixed after.**
`request_pool_pictures` had a comment promising "only a short stretch of each
source" above a line asking for `0.0, media.duration` — the whole file, for
every source, at the timeline's own fineness. Forty-eight seeks per source, and
forty sources in a pool.

The cost was the smaller half. **Coverage recorded that a stretch had been taken
and not how finely it had been taken**, so forty-eight frames spread across ten
minutes marked the whole file as covered — and every clip of that source on the
timeline was then stuck with one frame every twelve seconds for the rest of the
session, because nothing would ever ask again. Silent, and produced by opening
a panel.

A `Span` now carries the seconds between its frames, `missing` only counts a
stretch that reaches at least as finely as the one being asked for, and `cover`
merges only stretches of matching fineness — two finenesses over the same
seconds are two different facts and neither may be folded into the other's
claim. The pool asks for twelve frames across whatever length the source is, so
a ten-minute capture and a ten-second one cost the same.

The tile's *appearance* has not been checked on screen — the desktop was not
available — so what is verified is the arithmetic, the worker end to end against
real footage, and that a coarse pass no longer stops a finer one. Whether a
dozen frames still feels like enough to scrub across is a question for eyes, and
it stays on the handoff's waiting list.

---

## 6. Audio

### 6.1 The mixer, which is the whole panel and not a row of numbers

Asked for directly, with a screenshot of Premiere's Audio Clip Mixer to build
against. Worth writing out in full, because what is here now is not a smaller
version of Premiere's mixer — it is a different thing that happens to set the
same two values.

**What is here now.** `build_track_strip` gives each audio track two rows: a
**Volume** label with a numeric dB field, and a **Balance** label with a numeric
field from -100 to 100. The master is a horizontal slider with a meter under it.
The whole lot is a single vertical column, master first, tracks below.

**What Premiere has**, top to bottom in each strip:

| | Premiere | Here | Size |
|---|---|---|---|
| ~~Strips side by side, one per track~~ | horizontal, scrolling when narrow | **done** — a horizontal `ScrollView` | — |
| ~~Master at the right-hand end~~ | yes | **done** | — |
| ~~Pan as a **rotary knob**~~ | with L and R either side | **done** — `ui::PanKnob` | — |
| ~~A **vertical fader**~~ | with a dB scale printed down its left | **done** — `ui::Fader`, master included | — |
| ~~A **meter per track**, beside its fader~~ | with its own scale down its right | **done** — `AudioMixer::track_levels` | — |
| ~~Numeric gain readout under the fader~~ | yes | **done** — follows every pixel of the drag | — |
| ~~**M / S** buttons~~ | mute, solo | **done**, in the strip as well as on the track head | — |
| **R** (record-arm) | arms a track for recording an input onto it | **won't do** — this application has no audio capture at all | — |
| ~~Automation mode~~ | Off / Read / Write / Latch / Touch | **done** — all five, recording a pass and committing it as one edit | — |
| ~~Automation on the **master**~~ | the master strip has a mode too | **done** — the same five, one level up | — |
| ~~Track name at the foot~~ | `A1` and `Audio 1` | **done** — the same name the timeline shows | — |

**The reason it looks like this is not a good one.** The comment on
`build_track_strip` says numbers rather than faders because "the panel is as
likely to be a narrow column as a whole region, and a row of strips needs a
width it cannot count on", and `make_audio_panel` stacks the master for the same
reason. That is a description of a layout problem, not a reason to build a
different control — and Premiere has exactly the same problem and answers it by
**scrolling horizontally**, which is visible in the screenshot: the strips run
off the right-hand edge of the panel and the master sits at the end of them. A
`ScrollView` on the horizontal axis is the whole of the fix, and the application
already has one.

**A fader is not a number in a different costume.** A mix is set by ear, with a
hand on the control, watching a meter next to it. That is why every mixer ever
built puts a long throw beside a moving meter: the gesture is continuous, the
feedback is continuous, and the value is the least interesting part. A numeric
field can only be typed into or dragged as a scrub, and neither is riding a
level. The meter is half of it and there is currently no per-track meter at all,
so there is nothing to set a track fader *against*.

**What each piece needs.**

- **Horizontal strips with a scroll.** The arrangement, and it is what makes
  everything below it fit. Master last, as Premiere has it.
- ~~**`ui::Fader`**~~ — **built.** A vertical throw with the dB scale printed
  beside it, values in decibels with the travel linear in them, so the marks
  are evenly spaced and honest. Not `Slider` stood on its end: a slider is a
  value between two ends and its look says nothing about what the numbers mean,
  while a fader is *read against a scale* — knowing you are 3 dB down is the
  whole of what the control is for. `y_of` is public so the marks and the
  thumb cannot drift apart, which is the fault every scaled control has. The
  arrows move by a decibel and the page keys by six, because those are levels
  somebody means rather than fractions of a travel whose length depends on how
  the panel was docked.
- **`ui::PanKnob`** — a rotary, dragged vertically, with L and R either side.
  The value already exists (`Track::pan`); this is a way of setting it.
- ~~**Per-track metering**~~ — **done**, and it was publishing rather than
  computing: every voice is already summed into a block that carries the
  track's clip gain, fades, panner and effect chain, so a lane's own sum was
  one addition away. `AudioMixer` keeps a meter per lane beside the master's,
  `Player::track_levels` reads it, and each strip polls its own at frame rate
  exactly as the master's does.

  Three decisions worth keeping. The lanes are metered **before** the master
  fader, unlike the mix — a track meter answers "what is this track putting
  out", and strips that all dropped together when the master moved would be
  several meters showing one thing. A lane's level is the **sum of every clip
  playing on it at once**, not any one voice's, which is why the block is
  accumulated across the voice loop and metered after it. And the whole set is
  published under one lock with the master's, so no strip can show a level from
  this block beside a neighbour still showing the last.

  A lane with nothing on it reads as silence rather than being absent, so a
  caller never has to ask whether a track has an entry before drawing one.

- ~~**A track fader cannot be set by ear.**~~ **Done.** `set_track_gain` is a
  live trim the mixing thread reads, applied exactly where the track's own gain
  already sits in the chain. Kept as a *ratio* against the gain the mix was
  built with rather than replacing it, so a mixer nobody has touched multiplies
  by exactly one and an export is bit-for-bit what it always was — there is a
  test asserting two mixers produce identical samples when one has had its
  fader set to the value it already had.

**Driving it found the one fault none of this had a test for.** The plan
numbers audio tracks 0, 1, 2 and skips the video ones; everything else counts
tracks as the project lists them. The two agree exactly when a project is all
audio — which every test here was — so the strip asked for lane 1 in a V1+A1
project and got silence, and the meter simply stayed dark. The public API now
takes the project's numbering and maps inside, and there is a test with a video
track above an audio one, which is what every real project looks like and what
none of the tests had.

Two more came from the same session, both invisible to a test:

- **The throw was linear in decibels**, so unity sat nine tenths of the way up
  and the few decibels either side of it — where mixing happens — were squeezed
  into the last centimetre, while half the travel covered "very quiet" to
  "silent". It is tapered now, unity three quarters up, half way reading about
  -12, which is where a console puts it. A test can ask where a level sits and
  gets a consistent answer either way; what it cannot ask is whether the answer
  leaves you anywhere to put your hand.
- **The scale printed all nine marks whatever the height**, so on a strip two
  hundred pixels tall the top six ran together into one unreadable row. Marks
  that will not fit are skipped now, and unity is always drawn.
- ~~**M / S in the strip.**~~ **Done.** They are on the track head too, and the
  old note calling a second pair "two controls for one flag" was wrong about
  what a mixer is for: it is one flag reachable from both of the places you are
  when you want it. Premiere has both because mixing and editing are two
  activities, and leaving one to reach the other is the thing a mixer exists to
  avoid. **R** is record-arm for automation and waits on it — a button that
  arms nothing is worse than no button.
- ~~**Automation modes**~~ — **done**, all five. Only two of them change what is
  *heard*: Off ignores the curve and Read follows it. The other three follow it
  too and differ only in when moving the fader starts writing a new one, which
  is a question about the gesture rather than about the mix — Write records the
  whole pass whether or not anybody is holding the fader, Latch starts at the
  first touch and runs to the end, Touch records only while it is held.

  **A pass is held and committed once.** One keyframe per displayed frame means
  sixty a second; applying them as they arrived would put sixty entries a
  second in the undo stack and make taking a pass back a matter of holding
  Ctrl+Z down. `write_track_gain_pass` lays the whole thing in as one edit, and
  it **replaces only the span it covers** — punching in through one passage
  must not disturb what was set either side of it.

  Driven: A1 set to Write, the sequence played, the fader ridden down, and the
  saved file holds 102 keyframes from t=0 at unity to t=3.37 at 0.574. That is
  the shape of a hand, which is the only thing that could have confirmed it.

- **Record-arm (R) — won't do.** In Premiere, R arms a track for recording an
  *input* onto it — a voiceover through a microphone. This application has no
  audio capture at all, and §5 already declined Premiere's Capture and Device
  Control panes on exactly those grounds. A button that arms nothing is worse
  than no button. It is not the automation switch it looks like: the automation
  modes are the dropdown above it, and they are built.

- ~~**The master has no automation**~~ — **done**, the same five modes one level
  up. A programme that has to come down under a voiceover comes down on the
  master, so the strip needs it as much as any track does. `lay_pass_over` is
  shared between them rather than written twice: punching in means the same
  thing wherever the fader is, and two copies of that rule would be one copy
  and a future disagreement.

  The curve wins over the live fader when there is one, exactly as a track's
  does. An automated fader is being *played back*, and letting the atomic the
  interface writes override it would mean the last number typed silently
  flattening somebody's pass.

**Section 6.1 is closed.**

### 6.2 Submixes, sends, Essential Sound and loudness

Audited by walking the source rather than this page, which is the only way that
has ever produced a right answer here.

#### What is here now

| | Where it is now |
|---|---|
| Eight audio effects | `audio::audio_effect_defs` — high-pass, low-pass, bass, treble, compressor, EQ band, notch, gain |
| An effect chain with animatable parameters | `audio::EffectChain`, retuned on a timeline-aligned grid |
| A clip's own fader, panner and fades | `Clip::gain`, `pan`, `gain_keyframes`, `fade_in`/`fade_out` |
| A track's fader and panner, now automatable | `Track::gain`, `pan`, and the keyframe lists added in §6.1 |
| Metering per track and on the master | `AudioMixer::track_levels` / `levels` — peak, RMS, hold, over |
| A master limiter | `audio::Limiter`, fixed settings, always on |
| Stems on export | `ExportSettings::separate_audio` — one stream per track that has anything on it |

#### The finding that matters more than any row below

**There is no track-level audio processing at all.** `Clip` carries
`audio_effects`; `Track` carries a gain, a pan and now two keyframe lists, and
nothing else. The master carries a limiter that is hard-wired — no chain, no
settings, no way to reach it.

Every row in this subsection sits on top of that. A submix is a track whose
input is other tracks and whose point is that you can put a compressor across
the group. A send is a route into such a track. Essential Sound writes
processing onto a *role*, which is a group. Loudness normalisation across a
programme is a measurement plus a gain applied somewhere above the clips. None
of them mean anything while the only place an effect can go is on one clip.

So the order was fixed, and it was not the order the rows are listed in. **The
first two steps are done:**

1. ~~**An effect stack on a `Track`**~~ — done. The mix loop was restructured
   rather than extended: voices used to add straight into the mix with a copy
   kept per lane as a meter tap, which cannot work once a track has a stack,
   because the stack has to run on what the lane came to *before* it reaches
   the mix. The lane block is the signal path now, and the meter reads it after
   the chain — which is what a strip should show.
2. ~~**An effect stack on the master**~~ — done, after its fader and before the
   limiter. The limiter stays last: it catches whatever the rest of the chain
   produced, and a stack after it would process something already squashed.
3. ~~**A reverb and a delay**~~ — done. The catalogue had eight effects and
   every one was a filter or a gain; nothing in it had a delay line, which made
   a reverb the most-missed effect in its own right and made sends a route to
   nowhere.
4. ~~**Channel mapping**~~ — done, and it was the row a real shoot hits
   soonest.
5. ~~**Loudness**~~ — done, measurement and normalisation both.

6. ~~**Submixes, sends and roles**~~ — done, in one piece, because they are
   one piece. Premiere's roles largely exist to drive submixes: Dialogue goes
   to a dialogue bus, Music to a music bus, and the processing is on the bus
   rather than on each clip. Roles first would have been a label with nothing
   behind it; sends first would have been a route with nothing to route to,
   which is why the reverb came before them.

   A submix is a flag on an audio track rather than a third `TrackKind`. It
   already has everything a bus needs — a fader, a panner, a stack, a meter,
   automation and a strip in the mixer — and the one thing it does not have is
   clips. `track_indices_of_kind` refuses to hand one to a placement, which is
   the funnel every placement goes through, so a clip cannot be dropped on a
   bus and then heard from a lane with no source of its own.

   **The mixer runs its lanes in feed order now**, which is what
   `core::bus_routes` is for. Running a bus before what feeds it does not fail;
   it plays the previous block through this block's compressor, one buffer
   late, which sounds like a bad plugin rather than like a bug. Loops cannot be
   built through the interface — `can_route` asks the question from the far
   end — and a file that contains one is read as feeding the master.

   **The track fader moved with it.** `plan_audio` folded a track's gain into
   every clip on it, which put the fader ahead of each clip's own effects and
   left a pre-fader send with no pre-fader signal to take. It is applied to the
   lane now, after the lane's stack, where Premiere has it. `clip_gain_at` is
   the half without it.

**Section 6.2 is closed.**

#### What Premiere has that we do not

| | Premiere | Here | Size |
|---|---|---|---|
| ~~Track effects~~ | a stack per track, pre-fader | **done** — the lane's own chain, reachable from an fx button on the strip | — |
| ~~Master effects~~ | a stack on the master | **done** — after the fader, before the limiter | — |
| ~~Submix tracks~~ | a bus fed by other tracks, with its own stack and fader | **done** — an output dropdown on every strip, and a bus strip of its own | — |
| ~~Sends~~ | per track, pre or post fader, with a level | **done** — the strip's snd button; post-fader by default, as Premiere is | — |
| ~~A reverb~~ | Studio Reverb, Convolution Reverb | **done** — Schroeder/Freeverb, size, damping and mix | — |
| ~~A delay~~ | yes | **done** — time, feedback and mix | — |
| ~~Essential Sound roles~~ | Dialogue / Music / SFX / Ambience on a clip | **done** — `Clip::role`, set from the clip menu | — |
| ~~Role presets~~ | one press applies a set of effects for that role | **done** — this catalogue's nearest equivalents; appends what is missing rather than replacing a stack | — |
| ~~Ducking~~ | music follows dialogue automatically | **done** — Premiere's amount, fade and position, written as clip keyframes | — |
| ~~Loudness measurement~~ | LUFS to ITU-R BS.1770 | **done** — `audio::LoudnessMeter`, K-weighted and twice gated | — |
| ~~Loudness normalisation~~ | measure and match to a target | **done** — Project ▸ Normalise Loudness | — |
| ~~Channel mapping~~ | which source channel feeds which output | **done** — Premiere's Audio Channels, on the clip menu | — |

**A reverb is the row worth noticing.** The catalogue is eight effects and
every one of them is a filter or a gain — there is nothing with a delay line in
it. That matters twice over: it is the most-missed audio effect on its own, and
it is the reason sends exist. Building sends first would give a route to
nowhere.

**Loudness is two things and only one is hard.** Normalising to a target is
arithmetic once the number exists. Getting the number means K-weighting, mean
square over 400 ms blocks, and the two-stage gate BS.1770 specifies — a real
piece of DSP, but a well-specified one, and `audio::Meter` is already the right
shape to hang it beside: it walks blocks, keeps per-channel state, and publishes
a reading under a lock.

**Channel mapping was not on the list and should be.** Every source is treated
as stereo. A camera recording a lapel mic on channel one and a shotgun on
channel two is completely ordinary, and today both are heard at once with no way
to choose. That is arguably worth more than any row above it and is a smaller
piece of work than most.

#### Nothing here was declined

Every row was built, in the order argued for above rather than the order they
are listed in: the track stack first, because four of these rows were made of
it, and the routing last, because it needed the rest.

---

## 7. Playback and render engine

Written on 2026-10-23 from a code review against the question the owner
actually asked: why does playback and scrubbing *feel* worse than Premiere when
the renderer measures as keeping up? The answer is not throughput. It is where
the work happens and what waits for it.

| | Premiere | Here | Size |
|---|---|---|---|
| Rendering off the interface thread | the playback engine renders ahead on worker threads; the interface picks up finished frames | **done** — `app::PreviewEngine`, a render thread per monitor with a queue of its own on the card; see below | — |
| Frames in flight on the card | yes | **deferred, with a measurement** — each compositor has its own command list and waits only on its own fence; the drains themselves measured at a millisecond or two, see below | — |
| Editing while it plays | playback carries on through an edit | **mostly** — the row was stale: `rebuild_playback` resumes after an edit with a gap of about 25 ms. Seamless means replanning the mixer while its device runs | machinery |
| Export pipelined | render, convert and encode overlap; the card feeds the hardware encoder | **yes** — converted to NV12 on the card, a third of the readback, encoded on a thread of its own with up to three frames on their way back, and a 4K HEVC frame split across both NVENC engines: 4K60 at **4.4× realtime**, from 0.62×; see §7.2 | done |
| Precision on the way out | 10-bit and float paths to the encoder | **half** — the card converts from the float scene straight to P010 as well as NV12, tested to a code; the writer still takes 8 bits only, which is the 10-bit encoder §8 needs | wiring |
| A large pool is cheap until looked at | yes | **yes** — forty 4K sources: 231 MB in the list, 307 MB in icons once the strips are in. The 1.36 GB was not there to find; what was, was 200 MB the driver kept per decoding device after decoding, now given back when a device goes quiet; see §7.3 | done |
| Filmstrips for a folder of rushes | seconds | **yes** — 0.74 s a source, flat across forty, from 11.6 s rising to 14: every tile frame went back to the same keyframe and decoded the same pictures again, in software. Now on the card, seeking only forward; see §7.3 | done |
| Nested and retimed audio | streamed | **yes** — made ten seconds of timeline at a time around the playhead, with a 50 ms crossfade where pieces meet; Play on a 140-second clip at half speed went from 8.1 s to 0.28 s; see §7.4 | done |
| Undo in a long session | cheap per step | **yes** — measured, it was not harmless: a hundred steps of a ten-thousand-clip project held 1.5 GB. Each snapshot now shares every track that did not change with the one beside it: 76 MB, and an edit 3.1 ms rather than 8.6; see §7.5 | done |
| Crossing a cut | the next clip is read ahead | **done** — the far side of a cut is read on a thread of its own, on its own decoding device; see below | — |
| Reverse and J/K/L at 1× | smooth on intra and short-GOP media; long-GOP reverse is costly in Premiere too | **yes** — nothing over 50 ms within one clip or across fifty one-second cuts of a 4.17 s-GOP capture, which was 34 pictures a second and a worst gap of 364 ms; a third decoder per source, at the owner's choice; see §7.1 | done |
| Painting a long timeline | full rate | **yes** — it was never the painting: 0.3 ms to refresh and 3.5–7 ms to paint at 4K (`--benchmark <project>`). Playback set the playhead to the *nearest* frame while deciding a frame was new by truncating, so one frame in four was drawn twice and the next never; see §7.1 | done |

**The plan is to fix the shape rather than shave the costs.** A ring of
command allocators so the card holds frames in flight; a playback engine that
owns the renderer on its own thread, renders ahead of the audio clock into a
ring of display textures, and coalesces scrub requests so the newest wins; an
edit hot-swapping the snapshot that engine reads instead of stopping it; and an
export whose three stages overlap, with the colour conversion done on the card
into NV12 or P010. The two drains are the prerequisite for everything after
them — the handoff's §7B explains why moving the wait alone hangs.

**What it is measured against**, on this machine (RTX 5070 Ti):

| Measure | Target |
|---|---|
| 4K60 HEVC playback, no effects and with four | 60 distinct frames a second, p99 gap ≤ 1.5 frames |
| The ten-minute MKV, crossing fifty cuts | no gap over two frames |
| Reverse and J/K/L at 1× | no stall over 50 ms |
| Scrub, pointer to picture | under 50 ms typically |
| Interface input during playback or a seek | never blocked by decoding |
| Export, a plain 4K60 cut to HEVC on NVENC | at least 4× realtime (about 2× now) |
| 10-bit HDR export | works, at least 2× realtime |
| A forty-source pool | under 600 MB; filmstrips under two seconds a source, and flat — **307 MB and 0.74 s, flat** |

Each target is to be met with a number in this section beside it, taken the way
the handoff's playback measurements were — from outside the process, with
`play_project` as the control — rather than declared.

### 7.1 Measured so far

Two tools, both reproducible. `render_frame <project> <t> <out.png> <frames>`
walks frames through the bare renderer and prints percentiles;
`CUTLINE_REALTIME=1` paces it to the project's rate — flat out, anything
working ahead of the playhead gets a fraction of the time it has in real
playback — and `CUTLINE_FRAME_LOG=<file>` writes every frame's cost. The
running application is measured by sampling the program monitor and the
timecode off the screen together, ~83 times a second; the timecode counts the
interface's own paints, so a frozen window shows as a frozen timecode.

| | before | now | target |
|---|---|---|---|
| Plain 4K60, the application | 56.9/s | **56.8/s** picture, 60.0/s interface, nothing over 50 ms | 60/s |
| Sixteen blurs on 4K60, interface rate | 12.0/s, worst 183 ms | **59.2/s, worst 36 ms** | never blocked |
| Sixteen blurs, picture | 10.0/s | 9.4/s — the card's own limit, 78 ms a frame | — |
| Fifty one-second cuts, bare renderer, paced | p95 29.5, worst 421 ms | **p95 4.1, worst 7.6 ms** | ≤ 2 frames |
| Fifty cuts, the application | worst 842 ms, 14 over 50 ms | worst 62 ms, 4 over 50 ms | ≤ 2 frames |
| Fifty cuts, the application, frames shown | 40.6/s picture, 43.3/s timecode, worst 61 ms | **50.4/s picture, 55.8/s timecode, worst 37 ms**, nothing over 50 ms | 60/s |

The fifty-cut rows were first put down to the interface painting a
hundred-clip timeline too slowly. Measured, the painting was cheap — 0.3 ms
to bring the timeline up to date and 3.5 to 7 ms to paint it at 4K — and the
window was painting 93 times a second. What it painted was the fault: the
playhead was set by rounding to the nearest frame, and a new frame was
detected by truncating, so a turn late in one frame rounded up into the next
and the turn after drew that same frame again. One frame in four never
reached the screen, on any timeline. Whether a turn lands late in a frame
depends on what else the loop is doing, so it came and went: the plain 4K60
row above measured 60 once, and the same build measured between 22 and 45
while this was being chased. Fixed, it measures 60 timecodes and 56.8
pictures a second.

Tying the window's paints to the display — a frame-latency waitable object,
one paint per refresh, the playhead decided just before — was built and
measured as well, and made no difference on screen once the rounding was
fixed. It is not in.

What is left on the fifty cuts is the picture trailing the timecode, 50 to
56: frames the render thread has not finished when the interface wants them.

**Reverse, J at 1×**, from the end of each fixture.

**The picture probe was too small, and is not any more.** The sampler watched
a 64-pixel square in the middle of the monitor, and a frame whose middle
happened not to change from the one before counted as no new frame at all. On
this footage that turned out to be most of the "gaps over 50 ms" left on the
cuts: the interface had taken a new picture every frame, and the screen had
shown it. The probe is a 600 by 300 region now. Figures above this paragraph
were taken with the small one and are low on the picture side; the timecode
column was never affected.

| | before | now |
|---|---|---|
| One 4K60 clip, backwards | 52.5/s, worst 73 ms, 2 over 50 | **54.0/s, worst 42 ms, none over 50** |
| Fifty one-second cuts, backwards | 34.2/s, worst 364 ms, 9 over 50 | **54.0/s, worst 43 ms, none over 50** |
| Fifty cuts, bare renderer, paced, 48 s backwards | — | p99 1.5 ms; two slow frames, both at the start |
| One 4K60 clip, forwards | — | **59.6/s, worst 31 ms** |
| Fifty cuts, forwards | — | **52.0/s picture, 56.0/s timecode, worst 48 ms** |

What it took, in order:

- **Backwards, a cut lands on the *end* of the earlier clip**, as likely as not
  the middle of a group of pictures, and nothing prepared it: the cut reader
  looked only ahead of forward play. It looks behind as well, a second rather
  than half of one, and builds a run that ends on the earlier clip's last
  frame.
- **The serving path's wait for a reader** assumed every job answered half a
  second from its start. A backwards run ends at its cut, so the very frame it
  was built for fell outside the window and was sought from scratch. Each job
  now says which span it answers.
- **A third decoder per source**, at the owner's choice, with its own reader
  thread and its own decoding device. With two, the run before the one in hand
  and the far side of the next cut both wanted the second decoder at once, and
  whichever went second arrived late. It costs another surface pool — about
  450 MB of video memory at 4K — once a source has a cut read.
- **The run before the one in hand moved to its reader thread** too. In slices
  of the render thread's turns it made whole stretches of reverse cost 25 to
  37 ms a frame.
- **No run past the start of a clip.** Reverse never reaches it before cutting
  away, and asking for it held the second decoder and shared the card with
  the cut being read beside it. That decoder now spends the time building the
  run before the next cut's run instead, so both are in hand at the cut.
- **The monitor's engine renders ahead in a shuttle's direction**, where it
  rendered a shuttle a frame at a time on demand. And the shuttle rounds the
  playhead onto the frame grid rather than truncating it.

**Scrubbing**, pointer to picture: the pointer held on the ruler and moved in
jumps every 300 ms, timing each jump to the monitor's last change. The
timecode field is timed beside it, since it moves the moment the playhead
does, and so measures the interface's own share.

| | timecode | picture |
|---|---|---|
| One clip, steps of 0.2 s | 24 ms | **median 43 ms, worst 55** |
| Fifty cuts, jumps of 0.6 s | 24 ms | median 67 ms, p90 237, worst 249 |

Short steps meet the target. Jumps on the long-GOP capture do not: each lands
somewhere new in a 4.17 s group of pictures, and the exact frame is up to 250
decodes from the keyframe before it. Showing the keyframe at once and the
exact frame when it is ready was offered, and the owner chose the exact frame
always: a scrub never shows a picture that is not the one under the pointer,
and on long-GOP footage a big jump is allowed to take a quarter of a second.
These were taken with the small picture probe, which can only make them look
slower.

**The render thread was not enough on its own.** With the work moved off the
interface thread the interface still painted twelve times a second under a
heavy stack, because its paint queued *on the card* behind 78 ms of
compositing: one queue. A render thread's queue of its own, at a lower
priority than the interface's, is what took it to 59.

**Crossing cuts was four faults, not one.** The far side of a cut was read in
twelve-millisecond slices of the render thread; it is read on a reader thread
now. But half the cuts were never read at all, because the direction of play
was taken from the *source* — a cut back into the same capture moves the
source backwards while the sequence plays forwards — in three separate places.
And the reader and the serving decoder shared one Direct3D 11 device, so the
serving decoder's copy queued behind the reader's whole group of pictures, 28
ms a frame while a run was read.

**The drains were measured before being removed, and were not removed.**
`render_frame` with the display pass included: 2.1 ms a frame with no effects,
78 ms with sixteen per-pixel passes, before and after a command list per
compositor. The first is fast enough not to matter and the second is the
card's own work. The drains also guard a real hazard — a decoder's shared
texture is overwritten by its next frame — so they stay until a measurement
says they cost something.

### 7.2 Export

`export_project <project> <out.mp4> --hevc --hardware`, 4K60, on this machine.
The figures in the table above the change were measured first; the plan's
"about 2× realtime" was for a smaller canvas and was never true at 4K.

| | before | now |
|---|---|---|
| Plain 4K60, HEVC on NVENC | 37.8 fps, **0.62×** | 265.7 fps, **4.43×** |
| Plain 4K60, H.264 on NVENC | — | 151.5 fps, 2.53× |
| Fifty one-second cuts, HEVC | 38.2 fps, 0.64× | 164.0 fps, 2.73× |

Where a 4K frame went, before: decoding and compositing 2.7 ms, waiting for
the card and reading back 33 MB of RGBA 8.3 ms, `sws_scale` and the encoder
14.1 ms, each in turn. Three changes, each measured:

- **Converted on the card.** Two passes from the linear scene write NV12 —
  BT.709, studio range, chroma averaged over each four pixels in the display
  encoding as `sws_scale` did it — or P010 at ten bits. 12 MB comes back rather
  than 33, and the processor converts nothing. Tested against `read_back`'s
  own picture to within a code at both depths. 0.62× to 2.34×.
- **Overlapped.** The card's conversion and copy are left running; an
  encoding thread collects each frame from one of three readback slots and
  encodes it while the next is decoded. The slots are what bound how far
  rendering runs ahead.
- **Split encoding.** With both of those in, NVENC was the limit at 5.3 ms a 4K
  frame. HEVC at 4K and up now asks for the frame to be split across the
  card's two encoders: 2.1 ms. At the same quality setting the file came out
  3.5% larger and 0.03 dB lower in PSNR against the source; preset p3 split was
  faster still and 12% larger, and was not taken. A card with one encoder
  refuses the split, and the encoder is then opened without it rather than
  passed over for software.

H.264 has no split mode, so it stops at 2.5×. The fifty cuts are bound by
decoding: every clip starts in the middle of a 4.17 s group of pictures, so
the file needs far more frames decoded than it has frames.

Handing NVENC the card's own textures instead of a copy in system memory was
the plan's third step. Copying a frame out and back in is now 1 ms of a 3.8 ms
frame, and is not the limit; it waits for a measurement that says otherwise.

### 7.3 A large pool

Forty copies of the eight-second 4K60 HEVC capture in one pool. Memory is the
process's private bytes, taken from outside it; filmstrip times are from a
throwaway test driving `ThumbnailCache` with no cache directory, because the
on-disk cache makes a second run in the app meaningless.

| | before | now |
|---|---|---|
| Empty project | 230 MB | 247 MB |
| Forty sources, list view | 231 MB | 231 MB |
| Forty sources, icon view, strips done | ~400 MB in software; 600 MB on the card before the trim | **307 MB** |
| Filmstrip, one source | 11.6 s, rising to 14 by the twentieth | **0.74 s**, flat |
| All forty | 512 s | 30 s |

The 1.36 GB from the earlier drive did not reproduce: holding forty sources
costs nothing until they are looked at, and the list view measured the same as
an empty project. What was found instead:

- **The extractor decoded the same pictures over and over.** It sought for
  every tile frame, and this capture has keyframes at 0 and 4.17 s, so twelve
  frames meant going back to one of two keyframes twelve times — about 1,380
  frames decoded from a source of 480, on two software threads. It now seeks
  only when the seek lands ahead of where decoding has got to, which the
  container's index answers (`VideoDecoder::keyframe_at_or_before`): 3.8 s in
  software. And it decodes on the card through `VideoDecoder`, on a fourth
  shared device of its own so a strip never queues in front of a frame being
  played, bringing back only the dozen frames it keeps: 0.74 s, and no
  processor time. The pictures are identical to software's, tested, and each
  is the frame a fresh seek to it would have found.
- **The slowdown was not an accumulation.** It was the software decode sharing
  the machine; with the decode on the card the time is flat from the first
  source to the fortieth.
- **Each decoding device kept about 200 MB after its decoder closed.** Taken
  apart with DXGI's per-process figures: the 272 MB of surfaces go back to the
  card, but the driver grows a store of its own in shared system memory while
  decoding and keeps it for the device. The devices live for the session, so
  that was 200 MB each for good — the pool sat at 600 MB with its strips done,
  and playback's three devices kept theirs the same way. `IDXGIDevice3::Trim`
  gives it back. It costs 50 ms, so it runs on a thread of its own, and only
  once a device has had nothing on it for two seconds, so a run of filmstrips
  or a cut that reopens on the same device never pays for it. A plain close
  stays at 1.8 ms.

While a strip is being taken the process is at about 600 MB, which is one
open 4K hardware decoder. That is the cost of decoding, not of the pool, and
it goes when the decoding does.

### 7.4 Nested and retimed audio

A nest's sound was its sequence mixed end to end, and a retimed clip's was
its whole span reversed and stretched, both before the mixer came back — so
before Play could start. Measured with a 140-second tone, `create` as playback
calls it:

| | before | now |
|---|---|---|
| Clip at half speed | 8,062 ms | **279 ms** |
| Nest | 309 ms | **72 ms** |
| Reversed clip | 126 ms | **15 ms** |

The stretch is what costs: about 30 ms for each second it makes, so a
ten-minute capture at half speed would have waited over half a minute.

Both are now made the way a long source is read: in pieces of ten seconds of
timeline, the one under the playhead before `create` returns and the rest by
the reader thread, ahead of the playhead before behind it and each published
as soon as it is made. An export makes them as it reaches them. Each piece is
started half a second early so the stretch and a nest's own effects have
settled, and two pieces meet in a 50 ms crossfade at the end of the earlier
one, because two stretches of the same audio started in different places do
not line up to the sample — a test that fails without the fade checks the
join is not a step. A clip of ten seconds or less is one piece, made exactly
as before. A nest's own mixer now reads as it mixes rather than decoding
every clip inside it first, and still reports a missing file.

On the reference capture at half speed through a playback mixer: 0.26 s to
start at the beginning and 0.44 s ten minutes in, and played through three
joins with sound wherever the capture has it.

### 7.5 Undo at size

Measured on a synthetic long-form project — three sequences of sixteen tracks,
forty sources, an effect with keyframes on every picture clip and a volume
curve on every sound clip — at a hundred undo steps, each edit moving one clip:

| | 1,008 clips | 10,080 clips |
|---|---|---|
| History held, before | 158 MB | 1,506 MB |
| History held, now | **12 MB** | **76 MB** |
| An edit, before | 0.64 ms | 8.6 ms |
| An edit, now | 0.18 ms | 3.1 ms |
| Undo or redo, now | 0.37 ms | 5.4 ms |

Every snapshot was a whole copy, though an edit nearly always changes one or
two tracks of dozens. A snapshot now keeps its tracks apart from the rest of
the project and shares each one that is the same as in the snapshot beside it,
matched by sequence and track id so an inserted or reordered track does not
stop the rest being found; a test walks a run of edits that add, swap and
remove tracks and sequences back and forth and checks every state comes back
exactly. The session also moves the outgoing project into the history rather
than copying it, which is most of the edit's saving. Undo is slower than it
was — the project is put back together from shared tracks rather than handed
back — and at 5 ms on the largest project is still a third of a frame.

Copy-on-write tracks in the model itself would also make the edit's own copy
cheap, but every operation in the core writes through plain references, and
at 3 ms an edit on ten thousand clips there is nothing yet for it to buy.

## 8. Colour pipeline: 10-bit and HDR

`architecture.md` deferred HDR past v1 because no footage on hand was HDR. The
owner has now put it in scope, in full: HDR footage is possible and Premiere's
behaviour is the bar. The deferral did its job — the colour layer is
parameterised by transfer and primaries and the compositor is already linear
float — so this is new curves, a tone mapper and plumbing, not a restructure.

| | Premiere | Here | Size |
|---|---|---|---|
| 10-bit decode | P010 kept at 10 bits | **yes** — P010 from the card's decoder and from memory, and planar 10- and 12-bit, sampled at sixteen bits with studio range expanded in codes; a code eight bits cannot hold comes back exact; see §8.1 | done |
| PQ and HLG transfer curves | yes | **yes** — both ways, with HLG's system gamma at a nominal 1,000 nits, so its reference white lands on 203 nits | done |
| Rec.2020 primaries | yes | **yes** — converted to and from Rec.709 in linear light | done |
| A sequence colour space | Rec.709, Rec.2100 PQ, Rec.2100 HLG | **yes** — the three, with HDR graphics white (100, 203, 300 nits) and Auto Tone Map Media, in Project Settings and in the file; see §8.1 | done |
| HDR preview | on an HDR display, tone-mapped otherwise | **yes** — a window on an HDR display shows each monitor's picture at its real brightness, the interface around it exactly as before at the desktop's SDR white; on an SDR display an HDR sequence is tone mapped. Each window follows the display it is on; see §8.3 | done |
| Scopes in nits | HDR ranges on the waveform | **yes** — an HDR sequence's scopes measure its signal in its own PQ or HLG code values rather than the tone-mapped picture, and the waveform and parade are ruled at 100, 203, 1,000 and 4,000 nits; driven, the PQ test patches sit on the 203 and 1,000 lines | done |
| HDR10 export | HEVC Main10 with mastering and light-level metadata | **yes** — an HDR sequence writes HEVC Main 10, BT.2100 PQ or HLG, with the mastering display and light levels in the container and the bitstream; 4K60 at **2.35× realtime**. Any sequence can be written in any of the three spaces from the Export dialog. This build's x265 is 8-bit, so ten bits needs a hardware encoder; see §8.2 | done |

The fixtures will have to be generated rather than captured: every file the
owner has is still SDR, and a test that waits for one would wait for ever.

### 8.1 Colour management

As Premiere does it. A sequence has a working space -- Rec.709, Rec.2100 PQ
or Rec.2100 HLG -- and the scene is linear light in its primaries with 1.0 at
graphics white: SDR white in Rec.709, and 203 nits by default in HDR, which is
where BT.2408 and Premiere both put it. Every source is brought into the
sequence's coded space as it is read, before its effects, so every layer's
effects work on the same kind of numbers: SDR in an HDR sequence is mapped up
so its white is graphics white, and HDR in an SDR sequence is tone mapped
down. A title, a matte or a nest is an SDR Rec.709 source like any other.
SDR footage in an SDR sequence takes none of this, and comes out bit for bit
as it did.

Tone mapping works on luminance so a colour keeps its hue. Nothing moves below
three quarters of SDR white; above it an extended Reinhard shoulder takes
1,000 nits to exactly white, with the knee's slope unchanged so there is no
visible bend. Premiere does not publish its curve, so this one is chosen to
behave the way its Auto Tone Map Media is described: highlights kept apart,
nothing below them disturbed.

The tests render committed 64x32 lossless fixtures with exact 10-bit codes
and check against values worked out from the standards, not from what the
renderer produced: PQ through a PQ sequence comes back as the same codes;
HLG's 75% reference white lands on the PQ code for 203 nits; an SDR grey in a
PQ sequence is that grey's share of 203 nits; a white matte is 203 nits, or
100 when graphics white is set to 100; and in an SDR sequence 1,000 nits is
white while 203 nits sits below it, unless tone mapping is off, when both
clip. On screen, a PQ sequence on this SDR display shows the two patches as
241 and 255, and a Rec.709 one with tone mapping off as 255 and 255.

A nest is an SDR source when its sequence is SDR; an HDR one keeps its
highlights, see §8.3.

### 8.2 HDR export

An export is written in the sequence's own space unless the Export dialog says
otherwise, so an HDR sequence makes an HDR file without being asked: HEVC
Main 10, Rec.2020, PQ or HLG, studio range, and for PQ a P3 mastering display
at 1,000 nits with MaxCLL 1,000 and MaxFALL 400 -- Premiere's defaults are the
same shape, and the light levels cannot be measured before the header is
written without a second pass. An HDR sequence written as Rec.709 is tone
mapped as the preview is; an SDR one written as HDR has its white at
graphics white. HDR is HEVC only, as in Premiere; the dialog switches to it
and says so rather than failing.

Boiler, 4K60, in a PQ sequence, through NVENC:

| | |
|---|---|
| Split across both encoders | 3.78× realtime, metadata in the container only |
| **Unsplit, as shipped** | **2.35× realtime**, metadata in the container and the bitstream |

Split-frame encoding drops the mastering display and light-level SEI from the
bitstream, which is where most televisions read them, so a file carrying
HDR10 metadata is encoded unsplit. Found by stripping the container from an
export and probing the raw stream; the same NVENC writes them unsplit, and the
stock ffmpeg's does too. 2.35× still clears the 2× target for HDR.

The x265 in this build is 8-bit, so a machine without a 10-bit hardware HEVC
encoder is told it cannot write ten bits rather than given eight.

### 8.3 HDR preview

The interface is drawn by Skia, which blends in sRGB-coded values, and every
theme was tuned that way. Moving the whole window into linear scRGB would have
changed every anti-aliased edge and every pane of Aero glass on the one
display it matters on. So the interface is still drawn exactly as before, into
an ordinary surface; on an HDR display that surface is put into a sixteen-bit
scRGB swapchain at the desktop's SDR white -- the "SDR content brightness"
Windows sets, read per display -- which is what Windows does with any other
window there. A monitor leaves a hole where its picture goes, overlays still
drawn over it, and the picture goes underneath at its own brightness from a
float copy the compositor makes beside the 8-bit one: an HDR sequence at real
nits, an SDR one at SDR white so it matches the chrome around it.

Each window follows its display. A window on an SDR display, or every window
on a machine without one, takes exactly the path it always did; moving one
across switches it, and so does turning HDR on, which is looked for every two
seconds because Windows sends nothing a window hears. The HDR copy is only
made while some window can show it.

Checked on the owner's machine: the primary display is HDR (1,499 nits peak)
and the other two SDR. On the SDR display the interface captures as its
original colours exactly and an HDR sequence's 203-nit patch as 241, the
tone mapping; moved back, the window returns to HDR. A screen capture of an
HDR desktop is not the HDR picture -- Windows takes scRGB's 1.0, 80 nits, as
white -- so the interface captures 2.98 times brighter in linear light, which
is the desktop's SDR white of 238 nits over 80 and is what puts it at the same
brightness as every other window on the display itself. The scRGB values the
monitors are sent are tested against arithmetic.

An HDR sequence nested in another keeps its highlights: it is taken out of
its renderer from the float scRGB copy rather than the 8-bit one, and the
outer sequence brings it into its own space as it would any HDR source --
kept in an HDR sequence, tone mapped in an SDR one. Tested both ways with the
PQ patches nested: 573 and 723 come through, and in SDR 1,000 nits is white
with 203 below it.

A source can also be told what it is. Interpret Footage has Premiere's Color
Space Override -- Rec. 709, Rec. 2100 PQ or HLG -- for footage tagged wrongly
or not at all, and every source records its probed depth and colour space, so
a 10-bit source's decoder pool is sized at the sixteen bits a sample it takes
rather than the eight it was assumed to.

## 9. Interface and themes

The owner's words were that it "feels like a first draft", and that is not one
row. It is four things at once — visual polish, interaction feel, layout and
workflow, and dialogs and menus — and it is the reason this section comes
straight after the engine, before any new panel is built on top of what is
there now.

| | Premiere | Here | Size |
|---|---|---|---|
| Built-in workspaces | Editing, Color, Audio, Graphics, Effects, Assembly, laid out as Premiere does | **mostly** — Editing, Assembly, Colour, Effects and Audio, each arranged after Adobe's description of it: Editing has Source and Program side by side with Effect Controls and the mixer behind the source; Assembly gives the project the room; Colour puts the scopes beside the picture and the controls in a group of their own where Lumetri Color goes; Effects gives Effect Controls the room and the Effects panel a group; Audio puts the mixer in front. Graphics waits for the Essential Graphics panel it is built round (Phase 3). `--check` now lays out every built-in in every theme, which is how two of them were found leaving the timeline too narrow for its toolbar | wiring |
| A panel menu on every panel | the ≡ in each header | **done** — a ≡ beside the title of the tab showing, as Premiere draws it, and the same menu from a right-click on any tab. Every panel's starts with Premiere's shared entries: Close Panel, Undock Panel, Close Other Panels in Group, and Panel Group Settings (Close, Undock and Maximise the group), greyed where they have nothing to do. Then the panel's own: the project's New items, Import, Link Media and List or Icon; the Program Monitor's marks, Loop, Playback Resolution and Export Frame; the source's marks; the timeline's tracks, Snap in Timeline and Zoom to Sequence; the scopes' four kinds; the Effects panel's New Custom Bin. Each runs what the panel's own button or key runs. Menus grew what this needed: rules between groups, greyed rows, a column of shortcuts read from the key tables themselves, ticks, submenus that open beside their row (leftwards at the screen's edge) and work from Right, Left and Enter, and a letter jumping to its row. Driving found an undocked Program Monitor empty until the playhead moved — a new window makes the panel afresh and the preview only sends a better picture — so each monitor now starts with the last one it was given | control |
| Every command in the menu bar | yes | **done** — Premiere's bar in Premiere's order: File, Edit, Clip, Sequence, Markers, View, Window, Help. Every editing command is on it with its key written beside it, read from the same tables the keys are handled from; the view's own keys (play, J K L, zoom, the tools, the themes, snapping) are on it too, so nothing is reachable only from a key or a button. Each menu is built as it drops, so greying and ticks say what is true then. Preferences moved under Edit and Project Settings under File, where Premiere keeps them; the workspaces and Reset to Saved Layout are under Window. `--check` opens every menu and points at every submenu in every theme | wiring |
| Consistent context menus | one look, greyed rather than hidden, the same rows as the menu bar | **done** — every right-click is built from the same menu rows as the bar. The timeline's is the clipboard, the cuts and then the Clip menu's own rows, built by the same function; the project panel's has rules between its groups, Label as a submenu, and greys what cannot apply; the track header groups its switches, its heights and its tracks; an effect card's offers everything its own controls do — on and off, reset, move up and down, clear — and Copy, with the moves greyed at the ends of the stack. The plain lists left are pickers (the effects to add, the New and Track buttons), not context menus. Driving found the clip menu running off the bottom of the window: a popup too tall for either side of its anchor now moves up until it fits, as Windows moves one | wiring |
| History panel | the undo list, clickable | **done** — a History panel tabbed with the project in every workspace: New/Open and then every step by name, the current one lit and the ones undone faded but clickable, so a click goes back or forward any number of steps at once. Its panel menu has Step Backward, Step Forward and Clear History. Every step is named: a command passes Premiere's name for itself (Add Edit, Ripple Delete), and every other edit — well over a hundred places apply them — is named by comparing the project before and after (Move, Trim, Razor, Add Effect, Mute Track, Import, Label...), with a step keeping its name as it is undone and redone. Driven: an Add Edit and a Clear, then New/Open and back to the cut | control |
| Motion | hover and press fades, panel transitions, smooth zoom | **done** — `ui::Animator`, one per window, timing each widget's clocks on channels from one list. Controls fade between rest, hover and pressed at the theme's pace; the timeline's zoom (keys, Ctrl+wheel and fit) glides in proportion, holding still the point it zooms about all the way, and presses on the way build on where it is going; a scroll view's wheel glides and its notches add up; popups fade in. Each theme sets the pace: Luna snaps its views and fades its menus as XP did, Aero takes a quarter of a second, Phosphor does neither. Widgets with something on its way are ticked once a frame before layout, so nothing moves during drawing. Panels maximise and switch tabs at once, because Premiere's do. Driving found the zoom never moving on screen while every test passed: the timeline's hover fade and its zoom were on one clock, and drawing put the zoom back to its start each frame — the tests never drew between frames, and now one does | machinery |
| Drag feedback | one look for every drag | **done** — two forms, drawn by one function each: a drop onto something that is there (a panel to dock by, a clip to take an effect, a bin or an effects folder to file into) is the accent washed over it and drawn round it; something a drop would make (a clip from the pool, a transition on a cut) is a faded ghost of what it will be with the same accent outline, and a clip's leading edge in the accent. They used to disagree about whether there was a wash, how thick the line was and whether its corners were round. The timeline's drop target is drawn over the clip's picture now, where it was under its filmstrip. Driven with a pool entry held over V1 | wiring |
| Focus and keyboard navigation | visible focus everywhere | **done** — once Tab has moved the keyboard, the control it is on shows it, and a click puts the cue away again, as Windows does: a dotted rectangle just inside the control in Luna and Aero (left off a text field, whose caret already says so), and an accent ring just outside it in Slate and Phosphor, clipped to whatever clips the control. Every popup takes the keyboard when it opens and gives it back when it closes, so a menu answers the arrows and Enter and the timeline keeps its keys afterwards. The menu bar works from the keyboard: Alt underlines the titles' letters, Alt alone or F10 goes to the bar, Alt and a letter opens that menu on its first entry, Left and Right move between menus open or closed, and a choice leaves the bar. Driven in Luna and Slate, and to Preferences without the mouse | wiring |
| Dialogs | one consistent framework: button order, Enter and Esc, validation, remembered sizes | **done** — `ui::Dialog`: a title, rows whose labels line up, an error line, and OK then Cancel at the bottom right with OK as the default; Enter accepts, Escape cancels, a new dialog starts in its first field and Tab goes round it. A wrong answer keeps the dialog open and says why, where most used to close having done nothing. Every dialog is on it, the windowed ones (Project Settings, Preferences, Export) included: those remember the size they were left at, Escape closes them wherever focus is, and Enter accepts from a field. Export's button starts a render and leaves the window open to show it. `--check` now walks the Export window and tests both axes against each window's own frame, which is how it found Export's buttons sitting below its bottom edge | machinery |
| Icons | one set, one grid | **done** — one family of line drawings with one stroke and one reach, both from the type size, and put on the display's pixel grid as they are drawn: whole-pixel strokes, centred on a pixel for an odd width and on an edge for an even one, at every scale. The tool cursors are drawn from the same marks at the display's scale (32 pixels at 100%, 40 at 125%, measured from the cursor Windows was showing) | control |
| Per-monitor DPI | sharp on every display, and the right size as a window moves between them | **done** — the process is per-monitor aware (v2). Each window lays its tree out in units and scales it as it draws, so a 125% display gets the interface at its own resolution where Windows used to stretch a 96 DPI bitmap over it; input is divided by the same scale, and a window dragged across displays takes the size Windows suggests and re-lays itself out. Dialog sizes are remembered in units. The painter puts every rectangle, frame and straight rule on the pixel grid, so edges laid out at whole units are still whole pixels at 125% — tested by rendering at 1, 1.25, 1.5 and 2 and counting part-lit pixels, and driven by dragging the editor onto the 125% display: 1280x800 became 1600x1000 and clicks landed where they were aimed | machinery |
| Spacing and type | a type ramp and a spacing grid | **done** — each theme names its steps: tight, normal and group spacing, and tiny, small, body and heading type. A box takes a step rather than a number, so it changes with the theme; the literal spacings in the application are gone. A heading is bold at the heading size (a step up in Vista and Slate, body size in XP and Phosphor, as their periods did) with group spacing above it, so a page reads as sections before rows. Settings pages and dialogs are `ui::Form`s, whose labels share one column with the controls starting beside it — before, the controls were sprung to the right edge and a wide page had a gap down its middle. `--check` also checks that the text across every row sits on one line, and fails when it does not; it finds none now, and was shown to catch thirty-six rows a theme when form rows were deliberately top-aligned | machinery |

**Luna must feel like XP and Aero like Vista.** Not recolours — the
architecture already promised themes that change chrome — but the things that
made those systems recognisable:

| | XP / Vista | Here | Size |
|---|---|---|---|
| Faithful controls | Luna's rounded buttons with the orange hover rim, blue-highlighted menus, gripped scrollbars, green block progress; Vista's glossy buttons with a blue glow, the shining progress bar | **done** — Luna's are measured pixel by pixel from captures of XP itself (`cutline --gallery` draws every control in every state at 96 DPI to compare against them): the buttons' navy hairline with the orange band under the pointer and the blue one on the default button; the check box and radio with XP's green tick and bead; the trackbar's pointed thumb, green at rest and orange under the pointer, over a channel that is never filled; the progress bar's green blocks; the tabs' orange line along the top of the one showing and the one under the pointer, which stands taller; the caption buttons, blue squares and a red close with XP's heavy white glyphs; the scroll bars' lavender thumb with its grip, over a track lit from one side (without XP's arrow buttons, which Premiere's scroll bars do not have either); the menus' seventeen-pixel rows, nine-pixel rules in XP's grey and two pixels of white inside the edge; the list headings' beige with shaded bottom rows and divided columns; and the dropdown's arrow on a lavender button. Luna's tooltip was already XP's. Aero is measured from captures of Vista the same way: glass on the frame only, and inside it Vista's light grey face and white pages; buttons, tabs, scroll thumbs and the progress bar in two tones with a hard step across the middle, grey at rest and pale blue under the pointer, the default button ringed in cyan and pulsing towards a bluer glow every two seconds; check boxes and radios with a pale ring round a lit well and a beaded dot; text fields darker along the top; the trackbar's small pointed thumb; Explorer's list headings; Vista's menus with their rounded blue highlight; drop lists drawn as buttons with a small black triangle; and a light crossing the progress bar now and then | control, per control |
| Period fonts | Tahoma and Trebuchet for XP, Segoe UI for Vista | **half** — each theme names its face and the painter measures and draws in it: Tahoma for Luna, Segoe UI for Aero and Slate, Consolas for Phosphor, from the system. Luna's sizes are measured from XP (Tahoma at 8 points without smoothing, Trebuchet bold for captions, smoothed in grey as XP's Standard setting smoothed large type); Aero's wait for the Vista captures | wiring |
| A themed window frame | XP's blue caption and round red close; Vista's glass | **done** — a custom frame (`WM_NCCALCSIZE`). Luna's caption is XP's: its banded blue, rounded only at the top, the title in Trebuchet with a hard navy shadow, and XP's caption buttons. Aero's is real glass: the windows are drawn through the compositor with their alpha intact and asked for Windows 11's acrylic, so the desktop and the windows behind show blurred through the title bar's tint, with the caption buttons hanging from its top edge in Vista's outlined glyphs. Acrylic is softer than Vista's glass, and on a system with no backdrop to give the bar falls back to its own tint over the window | control |
| Animation | hover glows fading in and out | **half** — every control fades at its theme's pace (see Motion above); the glow itself is drawn with the Aero controls | machinery |
| Sounds | the system's own event sounds | **done** — off unless Preferences ▸ Appearance ▸ "Play Windows' sounds" is ticked. The interface says what happened -- a menu opening, a command chosen from it, an answer a dialog will not take, an export finishing or failing -- and the application plays the person's own Windows scheme for it by alias, with no fallback: an event the scheme leaves silent stays silent, and nothing Microsoft recorded is shipped. A dropdown's list is silent, as Windows' combo boxes were | wiring |

Vista's real blur-behind no longer exists on Windows 11. The closest honest
answer is the DWM system backdrop for the frame and a blur drawn in Skia
inside the window, and the row will say so rather than claim the original.
Fonts are used from the system and never bundled, for the same reason the
sounds are not.

## 10. Keyboard shortcuts

Pulled out of §5 for being larger than the rest of it together, and first of
the feature sections because the menus and the panel menus in §9 both name
commands.

| | Premiere | Here | Size |
|---|---|---|---|
| A command table with stable names | yes | **done** — `editor/shortcuts`: every action a key can run, seventy of them, with a stable id ("tool.razor"), its label and heading, the editing command it runs where it is one, and a scope saying when it is offered: always (undo, save), on a bare key before the focused control but never while typing (the tools, J/K/L, Space), or only once the focused control has declined it (the arrows, the marks). One resolver in the window procedure replaces the two tables and the dozen keys it tested one by one, and every menu row and tooltip reads its key from the keymap, so neither can claim a key that does something else. Making it found three keys that never worked: Alt+Left and Alt+Right never nudged, because Windows sends Alt chords as system keys and nothing listened; Space with the Source Monitor in hand played the sequence instead; and F1–F12, Insert, `;`, `/`, `[` and `]` were translated as no key at all | machinery |
| Rebinding, saved | per person, carried between machines | **done** — `%APPDATA%\Cutline\keys.json`, holding the preset and only what was changed, so a preset that gains a key in a later version gives it to everybody who did not change that action. A file from a newer build costs an older one only what it cannot read. Driven: Razor given F5, closed, F5 picked the razor, and after a restart F5 still did | machinery |
| Conflict check | yes | **done** — a chord runs one action only; giving it to another takes it away, and the window says which ("Ctrl+K was Add Edit, and now runs …") | wiring |
| Presets | Premiere's default and others | **done** — Premiere Pro, and Avid Media Composer's splice-in and overwrite on V and B, lift and extract on Z and X, Add Edit on H, marks on E and R as well as I and O, G to clear them. Choosing one, or Reset to Preset, starts from it again | wiring |
| The visual keyboard panel | yes | **done** — Edit ▸ Keyboard Shortcuts (Ctrl+Alt+K), modal as Premiere's is: a drawn keyboard with each bound key tinted -- the accent for what works everywhere, green for what a panel is offered -- and the command's name on it; Ctrl, Shift and Alt on the drawing show their layer; a key clicked shows what it runs or that it is free. Below it every command, searchable by name, heading or key, with its keys, Add (press the key; Esc leaves it) and Clear. Changes save as they are made | control |

## 11. Export presets and queue

| | Premiere | Here | Size |
|---|---|---|---|
| Presets, built in and saved | yes | **none** — one dialog, codec and quality not remembered | wiring |
| Match Source | yes | **none** | wiring |
| A queue running in the background | Media Encoder | **none** — one export at a time, owning the window | machinery |
| HEVC 10-bit and an intermediate (ProRes or DNxHR) | yes | H.264 and HEVC at 8 bits | wiring, after §7 |
| Smart rendering | untouched spans copied rather than re-encoded | **none** | machinery |

## 12. Titles and Essential Graphics

| | Premiere | Here | Size |
|---|---|---|---|
| Layered graphics | text, shapes and images in one clip | one text layer per title | machinery |
| Styles and templates | saved and reapplied | **none** | wiring |
| Responsive pinning | layers follow each other | **none** | machinery |
| Animated in and out | presets | keyframes only | wiring |
| Drop Shadow, Track Matte Key | yes | **none** — both need a pass to read a second texture, which the pass chain cannot yet | machinery |

## 13. Lumetri Color

| | Premiere | Here | Size |
|---|---|---|---|
| Basic correction | white balance, tone | Exposure, Levels, Colour Balance and friends, as separate effects | control |
| Creative | look LUT, faded film, vibrance | **none** | wiring |
| Curves | RGB and hue curves | **none** | control |
| Colour wheels | shadows, midtones, highlights | **none** | control |
| HSL secondary | yes | **none** | machinery |
| `.cube` LUTs | loaded into a 3D texture | **none** | machinery |
| One panel | yes | **none** — and it is to be built HDR-aware from §8 rather than retrofitted | machinery |

## Found by the 2026-10 review

**MP3 did not import — fixed.** The cause was the one suspected: a song with
ID3 cover art carries the sleeve as a picture stream marked
`AV_DISPOSITION_ATTACHED_PIC`, nothing under `src/media` checked for it, and so
`has_video()` said yes. The song became a video file — sent to the picture
decoder, given a filmstrip of one frame — and the sound never arrived as sound.
A plain mp3 with no art always worked, which is why it read as intermittent.
M4A and FLAC carry art the same way and failed the same way.
`detail::is_footage` is now the one rule, used by the probe and by every place
that picks a stream to decode.

`tests/media/formats_test.cpp` opens a committed fixture of each kind the
import dialog offers — mp3 with and without art, m4a, flac, ogg, opus, 24-bit
and float wav, aiff, ProRes, VP9, 10-bit HEVC, gif, jpeg and webp — so "it
imports" is a measured claim. The fixtures are generated by
`tests/media/fixtures/make.sh`, because this project's own FFmpeg has no mp3
encoder to write one with.

**Driving it found a second fault beside it.** An audio-only source in the
source monitor drew an empty panel and went on drawing one: the monitor took
whatever peaks the cache had when it opened — none, for a source just opened —
and the arrival that followed was passed to the timeline and not to it. Driven
since: an mp3 with art opens as a waveform, and a double-click puts it on A1.

---

## Found by audit, listed nowhere else

Walking the spec's own parity checklist and Premiere's menus against the source,
rather than against this page. Everything the checklist names is present — every
effect, every audio effect, all four transitions, all four scopes, all five
tools, both panners, the whole data model — with these exceptions, none of which
had been written down anywhere:

| | Premiere | Here | Size |
|---|---|---|---|
| ~~Separate audio on export~~ | one stream per track, or a mix | **done** — a third choice beside Stereo and Mono | — |
| ~~Scale to frame size~~ | right-click a clip, and an import default | **the row was wrong** — see below. Fit and Fill are both on the clip menu now | — |
| Frame hold / freeze frame | a still from one frame, in place | **done** — held at the playhead, picture only | — |
| ~~Interpret footage~~ | override a source's frame rate, alpha, channels | **done** for the rate; alpha does not exist in this pipeline and channels already had a home — see below | — |
| ~~Paste attributes~~ | pick which properties travel | **done** — nine groups, Ctrl+Alt+V, and Premiere's keyframe stretch | — |
| Preview render bar | red/yellow over the ruler, Enter renders it | none, and nothing caches a rendered span | machinery |
| Safe margins | title-safe and action-safe overlays | none | control |
| ~~Monitor zoom level~~ | Fit, 10%…400%, and scroll | **done** — on both monitors, with the wheel and the middle button to pan | — |
| Display mode | composite, alpha, and the scopes, over the picture | the Scopes panel instead, docked | control |
| ~~Maximise a panel~~ | `~` over any panel | **done** — the panel under the pointer, and again to put it back | — |

**The three monitor rows are §3's last row**, moved here rather than left there:
they belong to the program monitor as much as to the source one, and doing them
in one place is doing them once. Of the three, monitor zoom is the one with
daily use — checking focus, or the edge of a mask — and it is **done**, on both
monitors, from one widget. Safe margins are for broadcast delivery, which is not
what this is being used for; display mode largely repeats the Scopes panel,
which is already dockable and can therefore sit *beside* the picture rather than
over it.

**Zoom turned out to be four lines of geometry and one decision.** Everything
drawn over the picture — the transform box, its handles, every mask outline, the
hit test that finds them — is already placed against `MonitorView::picture()`,
so putting the zoom *inside* that one function carried the whole overlay with it
and nothing else had to learn that zoom exists. That is the shape the widget
already had, not a lucky accident: the reason the overlay was written against
the picture rectangle is that it is the only thing that knows where the picture
is.

**The percentages are measured against the canvas, not against the frame that
turned up.** This is the trap, and it is invisible until somebody changes the
preview quality: the picture arriving at the monitor is a half- or quarter-scale
render at anything below Full, so measuring 100% against *its* pixels would make
the number mean one thing on Full and another on Half. The monitor is told the
sequence size now — `set_canvas_size` rather than only `set_canvas_aspect` — and
one told only the shape steps its levels around Fit rather than lying about
pixels.

**Panning is the middle button and the wheel, and not a left drag.** Premiere
puts scroll bars down two edges of a zoomed monitor. The reason it needs them is
the reason a left drag cannot pan here either: on the program monitor the body
of a selected layer *is* its move handle, so a picture that panned on a left
drag would swallow both gestures, and on the source monitor it would swallow the
drag onto the timeline. The middle button collides with nothing, the wheel is
the gesture the row above asked for, and neither costs two edges of the picture.

**The source monitor's zoom sits on its header row**, beside the name of what is
showing, rather than down with the transport where the program monitor keeps
its own. That row is a third the width of the program monitor's and has already
been cut down twice to fit; `--check` returned 5 clipped and 5 squeezed in every
theme the moment a seventh control went on it. The header holds one control and
the full width of the panel, and "what is showing" and "how closely you are
looking at it" are the same kind of statement about it.

The first is the only one the *spec* asks for: §18's export dialog says
"Audio (mix/separate)", and §11 defines separate as one stream per track. The
rest are Premiere's rather than the spec's, which is why they are listed here
rather than counted against parity.

**Scale to frame size was already the behaviour**, and this page said otherwise
for weeks. Scale here is stored *relative to the aspect-fit size*, so 1 means
"as large as it goes without distortion" and a 4K clip in a 1080p sequence
arrives fitted rather than cropped. Premiere's command exists because Premiere's
default is native pixels. Reading the row rather than the code is what kept it
open; `core::natural_size` says so in its first sentence.

What was genuinely missing is the other half — **filling** a frame the footage
is the wrong shape for, which is what anybody with black bars actually wants —
and both are on the clip menu now, because neither is much use without the other
to get back to. Filling scales both axes by the same factor, so it crops rather
than stretches, and it goes through the same setter the inspector's rows use, so
an animated scale takes a keyframe instead of quietly losing its animation.

**Interpret Footage is one number, and the row asked for three things of which
only one exists here.** Decoded video in this application is NV12 or YUV 4:2:0
and carries no alpha whatsoever — the only thing in the pipeline with an alpha
channel is a rasterised title — so "ignore alpha" and "invert alpha" would be
switches on something that is not there. Channel layout is `Clip::channel_map`,
built some time ago and per *clip*, which is where a camera with a lapel on one
input and a shotgun on the other actually needs deciding. What was genuinely
missing is the frame rate, and it is the third anybody reaches for.

**It is not the same thing as a retime, and that is why both exist.** Conforming
60 to 24 makes the source two and a half times longer and every frame of it a
real one. A 40% retime of the same clip has to invent the frames in between or
show each one two and a half times. They look alike in the timeline and do not
look alike on screen. The other difference is where each lives: a retime is a
property of one clip, a conform is a property of the *source*, so every clip cut
from it is conformed including the ones placed tomorrow.

**The whole feature is one factor applied at two boundaries.** `conform_speed`
is `assumed ÷ file` — 0.4 for 60 shown at 24 — and it is multiplied in at
exactly two places: where the renderer asks the decoder for a frame, and where
the mixer plans a range to read. Everything above those two lines stays in
source seconds and does not know the feature exists. `Media::duration` stays the
*conformed* length, which is what all two dozen of its readers already mean by
it, and the file's own length is kept beside it only while a conform is in force
— so clearing one gives back the length the file has rather than a value two
roundings away from it.

**Clips already cut keep their frames, and the sequence ripples to fit.** A clip
on the first two seconds of 60 fps footage is on frames 0 to 120; at 24 those
frames end at five seconds, so that is where its range has to end. Leaving the
range alone would have every existing clip jump to different footage, which is
not what changing a playback rate means. Every clip that changed length then
moves what follows it, on every track a sync lock holds together — the same rule
the retime ripple uses, and now literally the same code: `ripple_after` was
lifted out of `set_clips_speed`, because two operations that change how long
clips are without moving them have the same problem afterwards and one of them
would eventually get the edge cases wrong.

**A conform drops the pitch, and a retime does not.** This is the one place the
sound had to gain a new path rather than reuse one. A speed change is stretched
with the pitch held, which is what `atempo` does and what anybody retiming a
shot expects. A conform is a change of *timebase* — the file is being replayed
on a different clock — so 60 shown at 24 sounds like tape run slow, because that
is what it is. Correcting it there would hide the one cue that says the conform
took effect. `audio::resample_by` is the tape half, and the two are kept as
separate numbers on the plan precisely so they can be heard differently.

**The evidence is a rendered frame, not the arithmetic.** Every layer under the
renderer can be right about the ratio while the renderer still asks the decoder
for a time in the wrong units, and nothing but pixels can tell. The test renders
real footage at four seconds conformed, renders it plainly at 1.6 seconds, and
requires the two to be the same image — it fails if the multiply is removed.

**Paste Attributes is a question, and that is the whole of its design.** Which
properties travel has no default worth guessing: "all of them" is the plain
paste it exists to be narrower than, and anything else would be the command
deciding for somebody what they meant. So `Command::PasteAttributes` is the one
command whose `run` *refuses* — it has a name, a key, a menu entry and a
`can_run` that greys all three, and the layer that can put a dialogue up catches
it before dispatch. The alternative was a second route in through the menu,
which is how a key and a menu entry come to do slightly different things.

**The groups are groups because a person thinks in them.** Motion is seven
numbers and the keyframes on all of them, and it is one tick: moving a shot and
leaving its animation behind gives you a clip that jumps back where it started
the instant it plays. Opacity carries the fades, because Premiere writes a video
fade *as* opacity keyframes and splitting them here would make the same-named
box mean something different.

**Each half takes from its own kind.** A shot on this timeline is two linked
clips, so copying one copies both and selecting three more selects six. The
picture groups reach the video halves and the sound groups reach the audio ones,
which is what makes "make these three shots like that one" a single gesture. A
group whose kind is not on the clipboard changes nothing rather than clearing
anything — copying a video-only clip and asking for Volume must not silence
what it lands on — and the box shows those groups greyed rather than hiding
them, because a missing half reads as a bug where a greyed one says why.

**Scale Attributes Time is Premiere's, and it is one multiplication** — keyframe
times are clip-relative, so zero is the head of the clip whatever the clip is.
The easing handles are offsets in the same units and are stretched with them; a
scaled animation that kept the handles of the length it was drawn at reads as a
different move rather than as the same one made slower. Off by default, as it is
there.

**Building this found that no dialogue had ever been laid out.** `--check` walks
the widget tree from the host's root, and a popup is not in it — so six boxes of
controls, several of them holding the widest rows in the application, were being
measured in no theme at all. The colour picker and the Window menu were each
added to the check by hand after something went wrong inside one of them. The
rest are in now: speed and duration, paste attributes, the marker box, audio
channels, loudness and Fit Clip, the last of which had to have four disagreeing
marks and a targeted track set up before it would open at all. 2811 widgets
became 2888.

**The render bar** is the opposite —
nothing in the application caches a rendered span, so it is a subsystem rather
than a control, and it is only worth building if scrubbing a heavy stack ever
becomes too slow to work with.

---

## Sections still to do

The rest of the application, in the order it seems worth walking:

- ~~**Project panel**~~ — audited, and written up as section 4 above.
- ~~**Application settings**~~ — audited, written up as section 5, and **closed**:
  every row is built or declined with a reason, bar keyboard customisation,
  which was pulled out into a section of its own below.
- ~~**Audio**~~ — audited, written up as section 6 above, and **closed**: both
  halves, every row. §6.2 was right that nothing in it could be started before
  a track carried an effect stack, and the routing that four of those rows were
  waiting for came last.
- ~~**Titles and graphics**, **Colour**, **Export**~~ — audited on 2026-10-23
  and written up as §11–§13 above.

**The order, chosen by the owner on 2026-10-23**: the engine (§7, with §8's
colour pipeline beside it), then the interface and themes (§9), then keyboard
shortcuts (§10), export presets and queue (§11), titles (§12) and Lumetri
(§13). Engine first because playback and scrubbing are what hurt in daily use;
the interface before new panels so that the new panels are built on what it
becomes rather than polished twice.
- ~~**Sequences**~~ — **done**. `core::Sequence` holds one cut and `Project`
  holds a list of them with the index of the open one; the pool and the bins
  stay project-wide, because importing a clip once and cutting it into three
  sequences is what a pool is for. A tab strip above the tracks switches
  between them, and the pool's menu offers New Sequence and New Sequence From
  Clip. What is left of it is nesting, which is §2.3's row rather than this one.

  Three things are worth knowing before touching it. The open sequence is a
  *list and an index*, not an open one held apart from parked ones: parking the
  closed ones needs the field list written twice and kept in step by hand, and
  loses where the open one sits in tab order. Switching is **not an edit** — it
  does not go through `apply`, does not enter the undo stack and does not mark
  the document modified, because a history that interleaves "I looked at the
  other cut" with real edits cannot be walked backwards. And the playhead is
  remembered per sequence in the *session* rather than on the project, so
  coming back to a cut finds it where you left it without every playhead move
  being a change to the document.

  The file format did not change for a project with one sequence. The first is
  written where a project's own fields have always been, so every file written
  before this reads back as it was and an older build can still open one
  written now; only a second sequence adds anything.
- **Keyboard customisation** — pulled out of section 5, where it was the single
  largest row, and written up as §10.
- **Everything else** — workspaces beyond four and the undo history panel,
  both now in §9. This used to
  begin with "markers with durations and comments", which §2.4 has had done for
  some time; a section-to-do list is exactly where a finished row survives
  longest, because nobody rereads the bottom of the page.
