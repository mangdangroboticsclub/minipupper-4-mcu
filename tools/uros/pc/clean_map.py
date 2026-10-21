#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Tidy a map saved with map_saver_cli: a cleaner picture for RViz and a video,
and the same room for Nav2.

    python3 $UROS/pc/clean_map.py /mnt/c/esp/maps/room_v2.yaml
    # -> room_v2_clean.yaml + room_v2_clean.pgm next to it; navigate on that:
    ros2 launch $UROS/pc/uros_nav.launch.py map:=/mnt/c/esp/maps/room_v2_clean.yaml

What it does (the original is not touched):
  - lone wall dots (a lidar hit with no other wall within 2 cells) become free
    floor; a dotted wall (hits a cell or two apart) is kept
  - stray floor dots out in the unknown (single rays past a gap) become unknown
  - one-cell gaps in a dotted wall are closed, so walls read as solid lines
  - small unknown holes inside the room (cells the lidar never hit, under
    furniture, between scans) become floor, up to --hole m^2 each
  - with --thicken: walls one cell thick grow by a cell into the floor beside
    them, so they read as solid lines (for a 2.5 cm map; at 5 cm it eats floor)
  - the unknown border is cropped to --margin m around the room
  - --erase x0,y0,x1,y1 (m, map frame; as many as needed) makes a box floor:
    something that was in the room while mapping and is gone (a bag, a foot).
    RViz's "Publish Point" tool shows a spot's x, y.

A wall the robot really has to avoid is not removed: only dots with no other
wall within two cells. Check the result in RViz before navigating on it.
"""
import argparse
import os
from collections import deque

import numpy as np
import yaml

FREE, OCC, UNK = 254, 0, 205          # map_saver_cli's trinary values


def read_pgm(path):
    with open(path, 'rb') as f:
        data = f.read()
    fields, i = [], 0
    while len(fields) < 4:            # magic, width, height, maxval; '#' comments between
        while data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b'#':
            while data[i:i + 1] not in (b'\n', b''):
                i += 1
            continue
        j = i
        while not data[j:j + 1].isspace():
            j += 1
        fields.append(data[i:j])
        i = j
    if fields[0] != b'P5':
        raise SystemExit('%s: not a binary PGM (P5)' % path)
    w, h, maxval = int(fields[1]), int(fields[2]), int(fields[3])
    if maxval > 255:
        raise SystemExit('%s: 16-bit PGM not supported' % path)
    i += 1                            # the one whitespace after maxval
    return np.frombuffer(data[i:i + w * h], dtype=np.uint8).reshape(h, w).copy()


def write_pgm(path, img, res):
    with open(path, 'wb') as f:
        f.write(b'P5\n# CREATOR: clean_map.py %.3f m/pix\n%d %d\n255\n' % (res, img.shape[1], img.shape[0]))
        f.write(img.astype(np.uint8).tobytes())


def neighbours(mask, r=1):
    """How many of the cells within r around each cell (the 8 for r=1, 24 for r=2) are set."""
    m = np.pad(mask.astype(np.int16), r)
    h, w = mask.shape
    span = range(-r, r + 1)
    return sum(m[r + dy:r + dy + h, r + dx:r + dx + w] for dy in span for dx in span if dy or dx)


def fill_holes(cls, max_cells):
    """Unknown pockets not touching the edge of the image and at most max_cells big -> free."""
    h, w = cls.shape
    seen = np.zeros_like(cls, dtype=bool)
    filled = 0
    for y0, x0 in zip(*np.nonzero(cls == UNK)):
        if seen[y0, x0]:
            continue
        comp, edge, q = [], False, deque([(y0, x0)])
        seen[y0, x0] = True
        while q:
            y, x = q.popleft()
            comp.append((y, x))
            if y in (0, h - 1) or x in (0, w - 1):
                edge = True
            for ny, nx in ((y - 1, x), (y + 1, x), (y, x - 1), (y, x + 1)):
                if 0 <= ny < h and 0 <= nx < w and not seen[ny, nx] and cls[ny, nx] == UNK:
                    seen[ny, nx] = True
                    q.append((ny, nx))
        if not edge and len(comp) <= max_cells:
            ys, xs = zip(*comp)
            cls[list(ys), list(xs)] = FREE
            filled += 1
    return filled


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('map', help="the map's .yaml (map_saver_cli)")
    ap.add_argument('--out', default='', help='output .yaml (default: <name>_clean.yaml)')
    ap.add_argument('--hole', type=float, default=0.04, help='largest unknown hole to fill, m^2 (default 0.04)')
    ap.add_argument('--margin', type=float, default=0.25, help='unknown border kept around the room, m')
    ap.add_argument('--erase', action='append', default=[], metavar='X0,Y0,X1,Y1',
                    help='make this box (m, map frame) free floor; repeat for more')
    ap.add_argument('--thicken', action='store_true', help='grow one-cell walls by a cell (2.5 cm maps)')
    a = ap.parse_args()

    with open(a.map) as f:
        meta = yaml.safe_load(f)
    img_path = meta['image']
    if not os.path.isabs(img_path):
        img_path = os.path.join(os.path.dirname(os.path.abspath(a.map)), img_path)
    raw = read_pgm(img_path)
    res = float(meta['resolution'])
    occ_t, free_t = float(meta.get('occupied_thresh', 0.65)), float(meta.get('free_thresh', 0.25))
    p = raw.astype(np.float32) / 255.0 if meta.get('negate', 0) else (255.0 - raw) / 255.0
    cls = np.full(raw.shape, UNK, np.uint8)
    cls[p > occ_t] = OCC
    cls[p < free_t] = FREE
    cls[raw == UNK] = UNK             # map_saver's unknown grey (with free_thresh 0.25 it would read as free)
    before = {k: int((cls == v).sum()) for k, v in (('wall', OCC), ('free', FREE), ('unknown', UNK))}

    ox, oy = float(meta['origin'][0]), float(meta['origin'][1])
    h, w = cls.shape
    erased = 0
    cx = ox + (np.arange(w) + 0.5) * res                  # cell centres; row 0 is the top (largest y)
    cy = oy + (h - 1 - np.arange(h) + 0.5) * res
    for box in a.erase:
        x0, y0, x1, y1 = (float(v) for v in box.split(','))
        sel = ((cy[:, None] >= min(y0, y1)) & (cy[:, None] <= max(y0, y1)) &
               (cx[None, :] >= min(x0, x1)) & (cx[None, :] <= max(x0, x1)))
        erased += int((sel & (cls != FREE)).sum())
        cls[sel] = FREE

    occ = cls == OCC
    lone = occ & (neighbours(occ, 2) == 0) & (neighbours(cls == FREE) > 0)    # no wall within 2 cells
    cls[lone] = FREE
    free = cls == FREE
    stray = free & (neighbours(free) <= 1)
    cls[stray] = UNK
    holes = fill_holes(cls, int(a.hole / (res * res)))
    # one-cell gaps in a dotted wall (wall on both sides, across or diagonally) -> wall
    o = np.pad(cls == OCC, 1)
    h0, w0 = cls.shape
    at = lambda dy, dx: o[1 + dy:1 + dy + h0, 1 + dx:1 + dx + w0]
    gap = (cls != OCC) & ((at(0, -1) & at(0, 1)) | (at(-1, 0) & at(1, 0)) |
                          (at(-1, -1) & at(1, 1)) | (at(-1, 1) & at(1, -1)))
    cls[gap] = OCC
    thick = 0
    if a.thicken:
        occ = cls == OCC
        # a wall cell with at most two wall neighbours is part of a thin line: grow it into the
        # free cells beside it (never into unknown, so the room does not grow)
        thin = occ & (neighbours(occ) == 2)       # inside a thin line; not a dot or a stub (those stay as they are)
        grow = (neighbours(thin) > 0) & (cls == FREE)
        cls[grow] = OCC
        thick = int(grow.sum())

    known = np.argwhere(cls != UNK)
    if len(known) == 0:
        raise SystemExit('the map has no known cells')
    m = int(round(a.margin / res))
    r0, c0 = (int(v) for v in np.maximum(known.min(0) - m, 0))
    r1, c1 = (int(v) for v in np.minimum(known.max(0) + m + 1, (h, w)))
    out_img = cls[r0:r1, c0:c1]
    yaw = float(meta['origin'][2]) if len(meta['origin']) > 2 else 0.0
    # image row 0 is the top (largest y); the origin is the bottom-left corner
    origin = [round(ox + c0 * res, 6), round(oy + (h - r1) * res, 6), yaw]

    out = a.out or os.path.splitext(os.path.abspath(a.map))[0] + '_clean.yaml'
    pgm = os.path.splitext(out)[0] + '.pgm'
    write_pgm(pgm, out_img, res)
    meta_out = dict(meta, image=os.path.basename(pgm), origin=origin, mode='trinary', negate=0,
                    occupied_thresh=0.65, free_thresh=0.196)
    with open(out, 'w') as f:
        yaml.safe_dump(meta_out, f, sort_keys=False, default_flow_style=None)

    print('%s: %dx%d cells at %.3f m (%.2f x %.2f m)' % (os.path.basename(a.map), w, h, res, w * res, h * res))
    print('  walls %(wall)d, free %(free)d, unknown %(unknown)d' % before)
    if a.erase:
        print('  erased %d cells in %d box(es)' % (erased, len(a.erase)))
    print('  removed %d lone wall dots and %d stray floor dots, filled %d holes, closed %d wall gaps, '
          'thickened %d wall cells' % (int(lone.sum()), int(stray.sum()), holes, int(gap.sum()), thick))
    print('wrote %s (%dx%d, %.2f x %.2f m)' % (out, out_img.shape[1], out_img.shape[0],
                                               out_img.shape[1] * res, out_img.shape[0] * res))


if __name__ == '__main__':
    main()
