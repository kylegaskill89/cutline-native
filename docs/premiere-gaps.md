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
| Source patch (V1/A1 indicators) | drag to choose which track receives | not separately — targeting does both jobs while there is one source | control |
| Track targeting for keyboard edits | per track, toggled | **done** — a T in every header, and what insert and overwrite aim at | — |
| Insert (`,`) and Overwrite (`.`) | from the source monitor at the playhead | **done** — from the pool's selection, on Premiere's own keys | — |
| Three- and four-point editing | in/out on source and sequence | the sequence half only; there is no source monitor to mark | machinery |
| Sync lock | which tracks ripple together | **done** — on by default, on the header's own menu | — |
| Mute / solo / lock / hide | yes | **done** | — |

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
| Nesting | a sequence inside a sequence | none | machinery |
| Multi-camera | none | none | machinery |

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
| Display mode, safe margins, zoom | yes | none — and not this section's, since it is both monitors' | audit |

**Section 3 is closed but for the last row, which is deferred rather than
done.** Display mode, safe margins and monitor zoom belong to the program
monitor as much as to this one, so building them into the source monitor alone
would be building them twice; they are carried in *Found by audit* below, as
**Safe margins**, **Monitor zoom level** and **Display mode**, and that list is
where they will be picked up.

Worth saying plainly what was weighed, so the deferral is a decision rather than
a thing that quietly did not happen. Safe margins are title-safe and action-safe
overlays for broadcast delivery, which is not what this application is being
used for. Display mode — composite, alpha, and the scopes — largely repeats what
the Scopes panel already offers, and offers it as a dockable panel that can sit
beside the picture rather than over it. Monitor zoom is the one of the three
with daily use, and it is the one to do first when they are picked up.

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
| New sequence from a clip | drag onto "New Item", or the menu | **needs sequences**, see below | model |

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

**New sequence from a clip needs something this project does not have yet: more
than one sequence.** A `Project` holds its tracks directly — there is no
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
| Keyboard shortcuts | fully editable, with presets | `kApplicationKeys` and `kTransportKeys`, fixed | **machinery** |
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
stalls are group-of-pictures bound and forward runs at about 40 fps on 4K60
because every frame is uploaded from system memory. The win there is keeping
hardware-decoded frames on the GPU, which is a different subsystem.

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

**Still open, and found while measuring this.** `request_pool_pictures` in the
composition root has a comment promising "only a short stretch of each source"
above a line asking for `0.0, media.duration` — the whole file, for every source
in the pool, in the icon view. With forty sources that is precisely the hundred
seconds of processor time the comment warns against. It is not fixed here
because fixing it properly means deciding what a tile ought to scrub across, and
the cache makes the second visit free rather than the first. It is also exactly
the case on the handoff's waiting-to-be-driven list — nobody has ever watched
the icon view against a large pool.

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
| Interpret footage | override a source's frame rate, alpha, channels | none — a source is what it says it is | model |
| Paste attributes | pick which properties travel | the effect stack only, whole | control |
| Preview render bar | red/yellow over the ruler, Enter renders it | none, and nothing caches a rendered span | machinery |
| Safe margins | title-safe and action-safe overlays | none | control |
| Monitor zoom level | Fit, 10%…400%, and scroll | letterboxed fit only | control |
| Display mode | composite, alpha, and the scopes, over the picture | the Scopes panel instead, docked | control |
| ~~Maximise a panel~~ | `~` over any panel | **done** — the panel under the pointer, and again to put it back | — |

**The three monitor rows are §3's last row**, moved here rather than left there:
they belong to the program monitor as much as to the source one, and doing them
in one place is doing them once. Of the three, monitor zoom is the one with
daily use — checking focus, or the edge of a mask — and is the one to start
with. Safe margins are for broadcast delivery, which is not what this is being
used for; display mode largely repeats the Scopes panel, which is already
dockable and can therefore sit *beside* the picture rather than over it.

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

- ~~**Project panel**~~ — audited, and written up as section 4 above.
- ~~**Application settings**~~ — audited, written up as section 5, and **closed**:
  every row is built or declined with a reason, bar keyboard customisation,
  which was pulled out into a section of its own below.
- **Audio** — the Audio Track Mixer, submixes, sends, the essential sound panel,
  loudness normalisation.
- **Titles and graphics** — the Essential Graphics panel, layered graphics,
  responsive design, styles.
- **Colour** — the Lumetri Color panel and its scopes workflow.
- **Export** — presets, queue, and the media encoder relationship.
- **Sequences** — more than one per project, which section 4 ran into and
  section 5 did not need. A list of them, which one is open, and a tab strip.
- **Keyboard customisation** — pulled out of section 5, where it was the single
  largest row: a command table, a binding store, a conflict check and a panel.
- **Everything else** — markers with durations and comments, workspaces beyond
  four, undo history panel.
