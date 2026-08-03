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
| Pen / free-draw path | yes | **done** — corners in a buffer, even-odd fill, a handle on each | — |
| Dragging the shape on the monitor | yes | **done**, and it writes a keyframe when the number is animated | — |
| Animating a mask | keyframed path, and tracking | **done** for every number it has; a path is the part that is missing | — |
| Tracking | per-frame analysis | none, and belongs with neither of the above | machinery |

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
4. **An effect applies to one clip, not the selection.** The double-click and
   the drop both take the whole selection now; what is still true is narrower —
   nothing else that edits a stack does. Worth re-checking rather than trusted.
5. ~~**Effects cannot be reordered by dragging.**~~ **Done.** The up and down
   buttons stay; `ui::GrabRow` adds the drag, with an insertion line across the
   top of the card it would land above.
6. ~~**Only the whole stack can be copied.**~~ **Done** — right-click a card's
   header. It fills the same clipboard, so a paste still *replaces*; copying one
   effect to *add* to another stack is a different operation and does not exist.
7. **Effect Controls never says which clip it is showing.** Premiere titles the
   panel with the clip and the sequence. With one clip selected it is obvious;
   with a timeline full of similar takes it is not.

**Already listed, and now closed.** Nothing carried forward from this section
is still open. Everything that was listed is done: paired X/Y, a visible reset per row,
resetting a whole effect, greying a governed property, the anchor point, both
panners, audio-effect keyframes, bezier handles, effects as passes, masks and
dragging them on the picture, nested folders, dropping an effect on the picture,
named presets, user bins, a free-drawn mask path, and Effect Controls naming the
clip it is showing — which turned out to have been done already and listed
anyway.

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
of it.

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
| Ripple trim (edge drag that closes the gap) | yes | **done** — a tool, on Premiere's B | — |
| Rolling edit (both sides of a cut at once) | yes | **done** — a tool, on Premiere's N | — |
| Trim to playhead (`Q` / `W`) | yes | none | wiring |
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

### 2.3 Clips on the timeline

| | Premiere | Here | Size |
|---|---|---|---|
| Drag from the pool onto a track | yes | **done**, at the point of release | — |
| Drag a clip to move it | along the track **and between tracks** | **done** — the second half was missing and the row said otherwise | — |
| Copy and paste clips | Ctrl+C / Ctrl+V, and paste-insert | **done** — copy, cut, paste and paste-insert | — |
| Duplicate (alt-drag a copy) | yes | none | wiring |
| Label colours | eight, set per clip and per bin | none | model + control |
| Enable / disable a clip | yes | **done** — on the clip menu, ticked, and a switched-off clip is drawn as one | — |
| Speed / duration dialogue | a box with both and a ripple option | Speed and Reverse in the inspector | control |
| Right-click a clip | the menu most edits are actually reached from | **done** — and it offers only what would do something | — |
| Nesting | a sequence inside a sequence | none | machinery |
| Multi-camera | none | none | machinery |

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
| Tooltips | yes | none anywhere | control |

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

What is left in this section is **tooltips** — there are none anywhere in the
application, and several controls are icons with no words at all.

### 2.4 Reading the timeline

| | Premiere | Here | Size |
|---|---|---|---|
| Waveforms and filmstrips on clips | yes | **done**, cached and drawn on workers | — |
| Volume rubber band | yes | **done**, with keyframes | — |
| Keyframe marks on a clip | yes | **done** | — |
| Snapping | yes | **done** | — |
| Zoom, and zoom to fit | yes | **done** | — |
| Track height, per track | dragged, and expand/collapse all | **done** — dragged on the line under a header, double-click puts it back | — |
| Markers with duration and comment | yes | a name and a colour, no duration | model + control |
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
| Frame hold / freeze frame | a still from one frame, in place | none | model |
| Interpret footage | override a source's frame rate, alpha, channels | none — a source is what it says it is | model |
| Paste attributes | pick which properties travel | the effect stack only, whole | control |
| Preview render bar | red/yellow over the ruler, Enter renders it | none, and nothing caches a rendered span | machinery |
| Safe margins | title-safe and action-safe overlays | none | control |
| Monitor zoom level | Fit, 10%…400%, and scroll | letterboxed fit only | control |
| Maximise a panel | `~` over any panel | none | control |

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

**The render bar** is the opposite —
nothing in the application caches a rendered span, so it is a subsystem rather
than a control, and it is only worth building if scrubbing a heavy stack ever
becomes too slow to work with.

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
