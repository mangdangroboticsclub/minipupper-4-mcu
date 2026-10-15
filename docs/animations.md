# Putting a GIF on the robot's screen

`tools/gif_to_mpxa.py` converts a GIF into an **MPXA** file: a compact
animation the firmware can play straight onto the panel.

## The short version

```powershell
pip install Pillow

# See what it would cost, without writing anything
python tools\gif_to_mpxa.py my_animation.gif

# Convert it into the system partition image
python tools\gif_to_mpxa.py my_animation.gif --outdir fatfs_image\system\anim

idf.py build flash
```

The animation is then on the robot at `/system/anim/my_animation.mpxa`.

A GIF that is not already 320x240 needs resizing, or it will be rejected:

```powershell
python tools\gif_to_mpxa.py big.gif --width 320 --height 240 --outdir fatfs_image\system\anim
```

`--fit contain` (the default) letterboxes on black and keeps the proportions,
`--fit cover` fills the panel and crops the overflow, `--fit stretch` distorts.

## Why not just play the GIF

LVGL can decode GIFs, and on a desktop that would be the obvious answer. On
this board it costs the two things that are actually scarce:

- a full ARGB8888 canvas, **307 KB** for a 320x240 panel, held for as long as
  the animation is on screen;
- an LZW decode of every frame, forever, competing with the agent and the web
  server for CPU.

MPXA moves that work to your desktop and does it once. The player needs one
RGB565 buffer the size of the panel (150 KB, and it can sit in PSRAM) and
decodes a frame with a memcpy loop -- no LZW, no palette lookup, and no pixel
format conversion, because the file is already in the panel's own format.

## What the converter does, and what to expect

Run on the reference eyes animation:

```
320x240, 88 GIF frames -> 14 stored (74 duplicates merged)
32 colours -> 17 after smoothing (dither the panel cannot show)
25,641 bytes  (2,150,400 raw, 99% smaller)
largest frame 3,957 B   player buffer 153,600 B
```

Three things earn that:

**Duplicate frames are merged.** A GIF stores every frame even when nothing
moved. That 3.5 s loop is 88 frames and 14 distinct ones; the other 74 become
longer delays on the frames they repeat. Timing is preserved exactly.

**Only the changed rectangle is stored.** After frame 0, a frame records the
smallest rectangle that differs from the one before it. A blink stores two
eye-sized patches, not two screens.

**That rectangle is run-length encoded**, which is close to free on flat
colour.

### Colour smoothing is the one that surprises people

The reference GIF is dithered between two yellows that differ by one step in
red. On the panel that is one yellow. To run-length encoding it is noise: a row
of the eyes has five visible bands and **eighty-two** colour changes. Encoded
as-is, the file came out at 213 KB. Collapsing colours the panel cannot tell
apart brought it to 25 KB -- an 8x difference, from a distinction nobody can
see.

That pass is on by default (`--tolerance 1`). Turn it off with `--tolerance 0`
if you are converting a photograph or a smooth gradient and see banding; raise
it to 2 or 3 if a flat-colour animation is still coming out large.

The map is computed once across every frame, never per frame. A per-frame map
could assign the same pixel to different colours in consecutive frames and
invent changes where the animation has none, which would defeat both the
duplicate merging and the dirty rectangles.

## Sizing an animation before you commit to it

Run the converter with no output path. It prints the numbers and writes
nothing. Worth doing before adding anything long: the system partition is
2600 KB and already holds the fonts, the built-in skills and both web UIs.
`idf.py build` will fail loudly if the image no longer fits, but it is nicer to
know first.

Rough guide, for a 320x240 panel:

| Content | Expect |
|---|---|
| Flat-colour animation (eyes, simple shapes) | 20-60 KB |
| Cartoon with a few dozen colours | 100-400 KB |
| Video-like footage, photographic gradients | 1 MB+, will not fit |

If something comes out far larger than you expected, check the colour line in
the report first. A high count after smoothing usually means dithering, and a
higher `--tolerance` is the fix.

## The file format

Documented in full at the top of `tools/gif_to_mpxa.py`, so the format and the
tool that writes it cannot drift apart. In short: a 16-byte header, a frame
table of offset/size/delay, then per-frame `x/y/w/h` plus an RLE stream. All
little-endian. Frame 0 always covers the whole image, so a player can start
from a buffer containing anything.

## Status

The converter and the format are done and verified -- a decoder built
independently from the spec above reproduces all 88 source frames of the
reference GIF exactly, with the loop timing preserved.

**The firmware-side player is not written yet.** Converting a GIF today puts a
correct `.mpxa` on the robot that nothing reads. The player is roughly 150
lines against `mpx_anim_play("/system/anim/name.mpxa")`, plus a
`display_play_animation` tool so the agent can trigger one from chat.

Note that the robot's resting face is *drawn*, not an animation -- see
`components/cap_display/src/cap_display_face.c`. It costs no assets and can
change expression on demand, which a canned animation cannot. MPXA is for
things drawing cannot do: a logo, a boot sequence, a character with real
artwork behind it.
