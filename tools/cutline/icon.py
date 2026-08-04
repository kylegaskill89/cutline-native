"""Draws the application icon and writes `cutline.ico`.

Kept as a script rather than as a checked-in binary alone, because an icon
nobody can edit is an icon that is wrong forever. Run it from anywhere:

    python tools/cutline/icon.py

The mark is the name. A clip lying along a timeline, broken by the vertical
line of a cut — that is what a cutline *is*, and it is three rectangles, which
is the most that survives being drawn sixteen pixels wide.

Colours are the default theme's, so the icon on the taskbar and the window it
opens are recognisably the same object: Slate's accent for the clip, its panel
background behind, and the cut in white because a cut is the one thing on a
timeline that has to be unmistakable.
"""

from __future__ import annotations

import os

from PIL import Image, ImageDraw

BACKDROP = (28, 33, 43, 255)  # Slate's panel, a shade deeper
CLIP = (76, 154, 255, 255)  # Slate's accent
CLIP_DARK = (44, 104, 190, 255)  # the far side of the cut, so the two read apart
SOUND = (86, 176, 128, 255)  # the timeline's audio green
SOUND_DARK = (52, 118, 86, 255)
CUT = (255, 255, 255, 255)

# Every size Windows asks for. 256 is the one Explorer shows large, 16 the one
# the title bar and the taskbar's small mode show, and the rest are what it
# scales between rather than resampling badly from either end.
SIZES = [256, 128, 64, 48, 32, 24, 16]

# Drawn once at this size and resampled down, which keeps the proportions
# identical at every size — a mark redrawn per size drifts.
CANVAS = 1024


def draw(size: int) -> Image.Image:
    image = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    pen = ImageDraw.Draw(image)

    # A rounded square rather than a bare shape: at small sizes a silhouette
    # with no ground behind it dissolves into whatever is under it.
    inset = size * 0.04
    pen.rounded_rectangle(
        [inset, inset, size - inset - 1, size - inset - 1],
        radius=size * 0.22,
        fill=BACKDROP,
    )

    # Two rows, because one bar is a stripe and two are a *timeline*. Picture
    # above and sound below is the arrangement every editor uses and the one
    # this application draws, so the icon is a small picture of the thing.
    left = size * 0.15
    right = size * 0.85
    radius = size * 0.045

    # The gap between the rows is wider than it looks like it needs to be, and
    # that is the sixteen-pixel size talking: at anything tighter the two bars
    # merge into one and the mark stops being a timeline.
    video_top = size * 0.28
    video_bottom = size * 0.46
    audio_top = size * 0.54
    audio_bottom = size * 0.72

    # The cut, and the hair of space either side of it that makes two pieces
    # look like two pieces rather than like one bar with a line drawn on it.
    cut_x = size * 0.45
    gap = size * 0.028

    pen.rounded_rectangle([left, video_top, cut_x - gap, video_bottom],
                          radius=radius, fill=CLIP)
    pen.rounded_rectangle([cut_x + gap, video_top, right, video_bottom],
                          radius=radius, fill=CLIP_DARK)
    pen.rounded_rectangle([left, audio_top, cut_x - gap, audio_bottom],
                          radius=radius, fill=SOUND)
    pen.rounded_rectangle([cut_x + gap, audio_top, right, audio_bottom],
                          radius=radius, fill=SOUND_DARK)

    # The cut itself, running past both rows, because it belongs to the
    # sequence rather than to either clip it happens to fall on.
    line = max(1.0, size * 0.026)
    pen.rectangle([cut_x - line / 2, size * 0.18, cut_x + line / 2, size * 0.82],
                  fill=CUT)

    return image


def main() -> None:
    master = draw(CANVAS)
    here = os.path.dirname(os.path.abspath(__file__))

    frames = [
        master.resize((size, size), Image.LANCZOS)
        for size in SIZES
        if size != CANVAS
    ]
    frames[0].save(
        os.path.join(here, "cutline.ico"),
        format="ICO",
        sizes=[(size, size) for size in SIZES],
    )

    # A PNG beside it for the readme and the release page, where an .ico is
    # awkward and half the renderers will not show one at all.
    master.resize((512, 512), Image.LANCZOS).save(os.path.join(here, "cutline.png"))
    print("wrote cutline.ico and cutline.png")


if __name__ == "__main__":
    main()
