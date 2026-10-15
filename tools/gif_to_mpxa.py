#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 MangDang
# SPDX-License-Identifier: Apache-2.0
"""Convert a GIF into an MPXA animation for the robot's panel.

Why this exists
---------------
A GIF on an ESP32 is expensive in the one place this board is short: internal
RAM and CPU. LVGL's built-in decoder holds a full ARGB8888 canvas -- 307 KB for
a 320x240 panel -- and runs an LZW decode for every frame, forever, for as long
as the animation is on screen. That is a lot to pay for something whose frames
are usually nearly identical to each other.

MPXA does the expensive thinking here, on a desktop, once:

  * pixels are stored as RGB565, the panel's own format, so the player never
    converts anything;
  * consecutive identical frames are merged into one frame with a longer
    delay -- animations are full of these and a GIF stores every one;
  * each frame records only the rectangle that actually changed since the
    previous one, so a blink stores two small eye-sized patches rather than
    two full screens;
  * that rectangle is run-length encoded, which flat colour compresses to
    almost nothing.

The player needs one W*H RGB565 buffer (150 KB for 320x240, and it can live in
PSRAM) plus a few hundred bytes of state. Decoding a frame is a memcpy loop --
no LZW, no palette lookup, no format conversion.

Usage
-----
    python tools/gif_to_mpxa.py eyes.gif
    python tools/gif_to_mpxa.py eyes.gif -o components/.../anim/eyes.mpxa
    python tools/gif_to_mpxa.py *.gif --outdir fatfs_image/system/anim
    python tools/gif_to_mpxa.py big.gif --width 320 --height 240

Run it with no output path at all to size an animation before committing
to it.

File format (MPXA v1, all little-endian)
----------------------------------------
Header, 16 bytes::

    0   4   magic       "MPXA"
    4   1   version     1
    5   1   pixfmt      0 = RGB565 little-endian
    6   2   width
    8   2   height
    10  2   frame_count
    12  2   loop_count  0 = loop forever
    14  2   reserved    0

Frame table, ``frame_count`` entries of 12 bytes, immediately after the
header::

    0   4   offset      from the start of the file to this frame's payload
    4   4   size        payload length in bytes
    8   2   delay_ms    how long this frame is shown
    10  2   reserved    0

Frame payload::

    0   2   x
    2   2   y
    4   2   w
    6   2   h
    8   ..  RLE stream covering exactly w*h pixels, row-major

The RLE stream is a sequence of tokens::

    token & 0x80 == 0    literal: (token + 1) pixels follow, 2 bytes each
    token & 0x80 != 0    run:     ((token & 0x7F) + 1) copies of the next pixel

So a run or a literal covers 1..128 pixels. Frame 0 always covers the whole
image, so a player can start from a buffer of any contents.
"""

import argparse
import os
import struct
import sys

try:
    from PIL import Image, ImageSequence
except ImportError:
    sys.exit("Pillow is required:  pip install Pillow")

MAGIC = b"MPXA"
VERSION = 1
PIXFMT_RGB565 = 0
HEADER_SIZE = 16
FRAME_ENTRY_SIZE = 12
MAX_RUN = 128


def rgb565(r, g, b):
    """Pack 8-8-8 into RGB565. Truncation, not rounding: rounding can push a
    value past full white and wrap the field."""
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def load_frames(path, width, height, fit):
    """Composite every GIF frame to RGB at the requested size.

    Pillow's seek() applies the GIF's own disposal method, so frames that are
    stored as partial updates come back already composited against what was
    underneath -- which is what we want, since MPXA computes its own deltas.
    """
    im = Image.open(path)
    frames = []
    for frame in ImageSequence.Iterator(im):
        delay = frame.info.get("duration", 0) or 100
        rgb = frame.convert("RGB")
        if width and height and rgb.size != (width, height):
            rgb = resize(rgb, width, height, fit)
        frames.append((rgb, delay))
    return frames


def resize(img, width, height, fit):
    if fit == "stretch":
        return img.resize((width, height), Image.LANCZOS)

    src_w, src_h = img.size
    scale = min(width / src_w, height / src_h) if fit == "contain" \
        else max(width / src_w, height / src_h)
    new = img.resize((max(1, round(src_w * scale)), max(1, round(src_h * scale))),
                     Image.LANCZOS)
    # Centre on a black field, cropping whatever "cover" pushed outside it.
    canvas = Image.new("RGB", (width, height), (0, 0, 0))
    canvas.paste(new, ((width - new.size[0]) // 2, (height - new.size[1]) // 2))
    return canvas


def to_565_rows(img):
    """One flat list of RGB565 values, row-major."""
    w, h = img.size
    data = img.tobytes()
    out = [0] * (w * h)
    for i in range(w * h):
        j = i * 3
        out[i] = rgb565(data[j], data[j + 1], data[j + 2])
    return out


def build_smoothing_map(all_frames, tol):
    """Collapse RGB565 colours that are too close to tell apart.

    GIF encoders dither, and an authoring tool will happily emit two yellows
    one step apart in red. On screen that is one yellow. To run-length encoding
    it is noise that shatters every run: a row of this file's eyes has five
    visible bands and eighty-two colour changes, which is why an unsmoothed
    encode of two rectangles came out at 213 KB.

    The map is built once across every frame, not per frame. A per-frame map
    could assign the same pixel to different canonical colours in consecutive
    frames and invent changes where the animation has none -- which would
    defeat both the duplicate merging and the dirty rectangles.
    """
    histogram = {}
    for pixels in all_frames:
        for value in pixels:
            histogram[value] = histogram.get(value, 0) + 1

    if tol <= 0 or len(histogram) > 4096:
        return None, len(histogram)

    def split(v):
        return (v >> 11, (v >> 5) & 0x3F, v & 0x1F)

    mapping = {}
    # Most common first, so the colour that dominates the image is the one
    # that survives and its neighbours collapse into it.
    for value, _ in sorted(histogram.items(), key=lambda kv: -kv[1]):
        if value in mapping:
            continue
        mapping[value] = value
        vr, vg, vb = split(value)
        for other in histogram:
            if other in mapping:
                continue
            orr, og, ob = split(other)
            if abs(orr - vr) <= tol and abs(og - vg) <= tol * 2 and abs(ob - vb) <= tol:
                mapping[other] = value

    merged = len(histogram) - len(set(mapping.values()))
    return (mapping if merged else None), len(histogram)


def dirty_rect(prev, cur, width, height):
    """Smallest rectangle containing every pixel that changed.

    None means nothing changed at all, which is how duplicate frames are
    detected -- they are the common case in hand-made animations, where a pose
    is held for several frames and the GIF stores each one in full.
    """
    if prev is None:
        return (0, 0, width, height)

    top = None
    bottom = None
    left = width
    right = -1
    for y in range(height):
        row = y * width
        changed = False
        for x in range(width):
            if prev[row + x] != cur[row + x]:
                changed = True
                if x < left:
                    left = x
                if x > right:
                    right = x
        if changed:
            if top is None:
                top = y
            bottom = y
    if top is None:
        return None
    return (left, top, right - left + 1, bottom - top + 1)


def rle_encode(pixels, width, rect):
    """RLE one rectangle out of a full-frame pixel list."""
    x, y, w, h = rect
    out = bytearray()

    for row in range(h):
        base = (y + row) * width + x
        i = 0
        while i < w:
            # How far does the current pixel repeat?
            value = pixels[base + i]
            run = 1
            while run < w - i and run < MAX_RUN and pixels[base + i + run] == value:
                run += 1

            if run >= 2:
                out.append(0x80 | (run - 1))
                out += struct.pack("<H", value)
                i += run
                continue

            # No run here: gather literals up to the next run of 3 or more.
            # Two-pixel runs are not worth breaking a literal for -- the token
            # they would save costs more than it gains.
            start = i
            while i < w and (i - start) < MAX_RUN:
                v = pixels[base + i]
                ahead = 1
                while ahead < 3 and i + ahead < w and pixels[base + i + ahead] == v:
                    ahead += 1
                if ahead >= 3:
                    break
                i += 1
            count = i - start
            out.append(count - 1)
            for k in range(count):
                out += struct.pack("<H", pixels[base + k + start])
    return bytes(out)


def convert(path, out_path, width, height, fit, loop, min_delay, tol, quiet):
    frames = load_frames(path, width, height, fit)
    if not frames:
        raise SystemExit(f"{path}: no frames")

    w, h = frames[0][0].size
    src_count = len(frames)

    pixel_frames = [to_565_rows(img) for img, _ in frames]
    smoothing, distinct = build_smoothing_map(pixel_frames, tol)
    if smoothing:
        pixel_frames = [[smoothing[v] for v in frame] for frame in pixel_frames]

    # Encode, merging any frame that changed nothing into its predecessor's
    # delay. That merge is where most of the saving comes from on hand-drawn
    # animations: a 3.5 s loop at 25 fps is 88 stored frames and often fewer
    # than 15 distinct ones.
    payloads = []
    delays = []
    prev = None
    for cur, (_, delay) in zip(pixel_frames, frames):
        rect = dirty_rect(prev, cur, w, h)
        if rect is None and payloads:
            delays[-1] += delay
            continue
        if rect is None:
            rect = (0, 0, w, h)
        body = struct.pack("<HHHH", *rect) + rle_encode(cur, w, rect)
        payloads.append(body)
        delays.append(delay)
        prev = cur

    delays = [max(d, min_delay) for d in delays]
    if len(delays) > 0xFFFF:
        raise SystemExit(f"{path}: {len(delays)} frames, more than the format's 65535")

    table_size = FRAME_ENTRY_SIZE * len(payloads)
    offset = HEADER_SIZE + table_size

    blob = bytearray()
    blob += struct.pack("<4sBBHHHHH", MAGIC, VERSION, PIXFMT_RGB565,
                        w, h, len(payloads), loop, 0)
    for body, delay in zip(payloads, delays):
        blob += struct.pack("<IIHH", offset, len(body), min(delay, 0xFFFF), 0)
        offset += len(body)
    for body in payloads:
        blob += body

    raw = w * h * 2 * len(payloads)
    if not quiet:
        largest = max(len(b) for b in payloads)
        print(f"{os.path.basename(path)}")
        print(f"  {w}x{h}, {src_count} GIF frames -> {len(payloads)} stored"
              f" ({src_count - len(payloads)} duplicates merged)")
        if smoothing:
            kept = len(set(smoothing.values()))
            print(f"  {distinct} colours -> {kept} after smoothing"
                  f" (dither the panel cannot show)")
        else:
            print(f"  {distinct} colours, no smoothing applied")
        print(f"  {len(blob):,} bytes"
              f"  ({raw:,} raw, {100 - len(blob) * 100 // max(raw, 1)}% smaller)")
        print(f"  largest frame {largest:,} B"
              f"   player buffer {w * h * 2:,} B")

    if out_path:
        with open(out_path, "wb") as f:
            f.write(blob)
        if not quiet:
            print(f"  -> {out_path}")
    return blob


def main():
    ap = argparse.ArgumentParser(
        description="Convert GIFs to MPXA animations for the robot panel.",
        epilog="With no -o/--outdir this only reports what the file would cost.")
    ap.add_argument("inputs", nargs="+", help="GIF file(s)")
    ap.add_argument("-o", "--output", help="output path (single input only)")
    ap.add_argument("--outdir", help="write <name>.mpxa into this directory")
    ap.add_argument("--width", type=int, help="resize width (needs --height)")
    ap.add_argument("--height", type=int, help="resize height (needs --width)")
    ap.add_argument("--fit", choices=("contain", "cover", "stretch"),
                    default="contain",
                    help="how to resize: contain letterboxes, cover crops, "
                         "stretch distorts (default: contain)")
    ap.add_argument("--loop", type=int, default=0,
                    help="loop count, 0 = forever (default: 0)")
    ap.add_argument("--min-delay", type=int, default=20,
                    help="floor for a frame's delay in ms (default: 20). GIFs "
                         "often claim 0, which browsers read as 100 and a "
                         "microcontroller would read as 'as fast as possible'")
    ap.add_argument("--tolerance", type=int, default=1,
                    help="collapse RGB565 colours this far apart into one "
                         "(default: 1). This is what makes RLE work on "
                         "dithered GIFs; 0 disables it")
    ap.add_argument("-q", "--quiet", action="store_true")
    args = ap.parse_args()

    if bool(args.width) != bool(args.height):
        ap.error("--width and --height must be given together")
    if args.output and len(args.inputs) > 1:
        ap.error("-o takes a single input; use --outdir for several")
    if args.outdir:
        os.makedirs(args.outdir, exist_ok=True)

    for path in args.inputs:
        out = args.output
        if not out and args.outdir:
            stem = os.path.splitext(os.path.basename(path))[0]
            out = os.path.join(args.outdir, stem + ".mpxa")
        convert(path, out, args.width, args.height, args.fit,
                args.loop, args.min_delay, args.tolerance, args.quiet)


if __name__ == "__main__":
    main()
