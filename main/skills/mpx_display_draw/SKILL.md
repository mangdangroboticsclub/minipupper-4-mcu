---
{
  "name": "mpx_display_draw",
  "description": "Draw anything on the robot's 320x240 screen (an animal, an object, a symbol, a scene) with display_draw, when the built-in faces are not enough.",
  "author": "MangDang",
  "metadata": {
    "cap_groups": [
      "cap_display"
    ],
    "manage_mode": "readonly"
  }
}
---

# Drawing on this robot's screen

`display_draw` paints a picture you describe as a list of shapes. Anything you
can build from rectangles, circles, ovals, lines, triangles, arcs and text, you
can put on the robot's face: a crab, a heart, a sun, a clock, a score.

## First: you usually should not

`display_show_emotion` already animates the eyes (`happy`, `sad`, `love`,
`surprised`, ...). It is instant, it animates smoothly, and it is what the
robot should wear. If someone says "look sad", use that. Draw only when the
request is genuinely not an expression: an animal, an object, a symbol,
information (a number, a name, a timer value), or something the person
describes in detail.

## The panel

- **320 wide, 240 tall**, landscape. **(0,0) is the top left**, x grows right,
  y grows down. The centre is (160,120).
- No touch: nothing you draw can be pressed.
- Seen from across a room on a small robot: big shapes, bold colours, few words.
- The resting face is yellow `#FFE631` eyes on black; matching that palette
  makes a drawing feel like the robot's own.

## The call

```json
{
  "background": "black",
  "seconds": 20,
  "shapes": [ { "type": "circle", "x": 160, "y": 120, "r": 50, "color": "red" } ]
}
```

- `background`: colour of the whole screen (default black).
- `seconds`: how long the drawing stays before the face comes back (default
  20, max 600). `0` keeps it until `display_clear`, an emotion or a text.
- `shapes`: painted in order, later ones on top. Up to ~200 primitives.

Colours: `black white red green blue yellow orange purple pink cyan grey brown
navy lime magenta gold silver darkred skyblue beige`, or `#rrggbb`.

| type | fields |
|---|---|
| `rect` | `x`,`y` (top left), `w`,`h`, `radius` (rounded corners) |
| `circle` | `x`,`y` (centre), `r` |
| `oval` | `x`,`y` (centre), `rx`,`ry` |
| `line` / `polyline` | `points`: `[[x,y],[x,y],...]`, `width` (default 3) |
| `triangle` | `points`: three `[x,y]` |
| `polygon` | `points`: 3+ `[x,y]`, filled. **Convex only** -- split a concave shape (a claw, a star) into triangles or several convex polygons |
| `arc` | `x`,`y` (centre), `r`, `start`,`end` in degrees (0 = 3 o'clock, clockwise, so 0..180 is the lower half), `width` |
| `text` | `x`,`y` (centre of the text), `text`, `size`: `small`, `medium`, `large` |

Every shape takes `color`. `rect`, `circle` and `oval` also take `fill: false`
(outline only), `border` (outline colour) and `width` (outline thickness).

## Example: a crab

```json
{
  "background": "black",
  "seconds": 30,
  "shapes": [
    {"type":"line","points":[[110,150],[70,190]],"color":"#E0401C","width":8},
    {"type":"line","points":[[120,160],[90,205]],"color":"#E0401C","width":8},
    {"type":"line","points":[[210,150],[250,190]],"color":"#E0401C","width":8},
    {"type":"line","points":[[200,160],[230,205]],"color":"#E0401C","width":8},
    {"type":"line","points":[[115,120],[75,85]],"color":"#E0401C","width":10},
    {"type":"line","points":[[205,120],[245,85]],"color":"#E0401C","width":10},
    {"type":"circle","x":65,"y":65,"r":28,"color":"#E0401C"},
    {"type":"triangle","points":[[60,40],[90,55],[70,70]],"color":"black"},
    {"type":"circle","x":255,"y":65,"r":28,"color":"#E0401C"},
    {"type":"triangle","points":[[260,40],[230,55],[250,70]],"color":"black"},
    {"type":"oval","x":160,"y":140,"rx":75,"ry":50,"color":"#FF5A2A"},
    {"type":"line","points":[[140,100],[135,75]],"color":"#E0401C","width":5},
    {"type":"line","points":[[180,100],[185,75]],"color":"#E0401C","width":5},
    {"type":"circle","x":135,"y":70,"r":11,"color":"white"},
    {"type":"circle","x":185,"y":70,"r":11,"color":"white"},
    {"type":"circle","x":137,"y":72,"r":5,"color":"black"},
    {"type":"circle","x":187,"y":72,"r":5,"color":"black"},
    {"type":"arc","x":160,"y":145,"r":22,"start":20,"end":160,"color":"black","width":4}
  ]
}
```

## Things that will bite you

- **Order matters**: background parts first, details last. A body drawn after
  the eyes hides them.
- **Stay on the panel**: keep x in 0..319 and y in 0..239. Off-panel parts are
  cut, not moved.
- **Text does not wrap.** A few words per line at `large`; split longer text
  into several `text` shapes on different `y`.
- **One call is one picture.** To change it, call `display_draw` again with the
  whole picture; it is not an edit of the last one.
- **Do not leave a drawing up forever.** When it has said what it had to say,
  the face is what the robot should wear: let `seconds` run out or call
  `display_clear`.
