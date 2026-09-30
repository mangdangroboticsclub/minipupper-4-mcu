<script>
  import { colors } from "./colors.js";

  /**
   * Controller — a single, fluid gamepad that fills whatever box it is given.
   *
   * Layout is driven purely by CSS container queries:
   *   tall / square  →  [ L-bumper   R-bumper ]   wide  →  [ L-bumper  Actions  R-bumper ]
   *                     [      Actions        ]           [   Move    Actions    Look    ]
   *                     [   Move   |   Look   ]
   *
   * @prop assignments        slot id → gait id (from the action editor)
   * @prop catalog            [{ id, label, emoji, color }] used to name assigned actions
   * @prop online             robot reachable (dims the pad when false)
   * @prop fullscreen         current fullscreen state (for the toggle glyph)
   * @prop onToggleFullscreen () => void
   * @prop onEdit             () => void — open the action editor
   */
  let {
    assignments = {},
    catalog = [],
    online = true,
    fullscreen = false,
    onToggleFullscreen,
    onEdit,
  } = $props();

  /** Fraction of the stick travel that is a deadzone (0–1) */
  const DEADZONE = 0.18;
  const LOOK_DEADZONE = 0.35;

  // The four assignable slots — ids are shared with ActionAssignment.svelte.
  const SLOTS = [
    { id: "action-top",    pos: "top",    fallback: "jump",    color: colors.mpx.action_green,  shape: "triangle" },
    { id: "action-right",  pos: "right",  fallback: "twerk",   color: colors.mpx.action_red,    shape: "circle" },
    { id: "action-left",   pos: "left",   fallback: "stretch", color: colors.mpx.action_purple, shape: "square" },
    { id: "action-bottom", pos: "bottom", fallback: "sit",     color: colors.mpx.action_blue,   shape: "pentagon" },
  ];

  function slotAction(slot) {
    const id = assignments[slot.id] || slot.fallback;
    const meta = catalog.find((a) => a.id === id);
    return { id, label: meta?.label ?? id };
  }

  // ── Robot API: joystick stream ───────────────────────────────
  let joyF = 0, joyS = 0, joyT = 0;
  let joyTimer = null;

  async function sendJoy() {
    try {
      await fetch("/v1/robot/joy", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ f: joyF, s: joyS, t: joyT }),
      });
    } catch { /* transient — keep trying while the stick is held */ }
  }
  function joyStartTimer() {
    if (!joyTimer) {
      sendJoy();
      joyTimer = setInterval(sendJoy, 100);
    }
  }
  function joyStopIfIdle() {
    if (joyF === 0 && joyS === 0 && joyT === 0 && joyTimer) {
      clearInterval(joyTimer);
      joyTimer = null;
      sendJoy(); // final zeros → step in place
    }
  }

  async function sendGait(mode) {
    try {
      await fetch("/v1/robot/gait", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ mode }),
      });
    } catch { /* ignore */ }
  }

  // ── Look direction (head pose) ───────────────────────────────
  function joystickToLookMode(nx, ny) {
    const dist = Math.hypot(nx, ny);
    if (dist < LOOK_DEADZONE) return "init";
    const angle = (Math.atan2(ny, nx) * 180) / Math.PI;
    if (angle > -22.5 && angle <= 22.5) return "lookright";
    if (angle > 22.5 && angle <= 67.5) return "looklr";
    if (angle > 67.5 && angle <= 112.5) return "lookdown";
    if (angle > 112.5 && angle <= 157.5) return "lookll";
    if (angle > 157.5 || angle <= -157.5) return "lookleft";
    if (angle > -157.5 && angle <= -112.5) return "lookul";
    if (angle > -112.5 && angle <= -67.5) return "lookup";
    if (angle > -67.5 && angle <= -22.5) return "lookur";
    return "init";
  }
  let lastLookMode = "init";
  function sendLookMode(mode) {
    if (mode === lastLookMode) return;
    lastLookMode = mode;
    sendGait(mode);
  }

  // ── Sticks ───────────────────────────────────────────────────
  // Each stick tracks its own pointer so Move + Look work together on touch.
  const makeStick = () => ({ nx: 0, ny: 0, dx: 0, dy: 0, active: false, pointerId: null, cx: 0, cy: 0, r: 0 });
  let move = $state(makeStick());
  let look = $state(makeStick());
  const stickOf = (which) => (which === "move" ? move : look);

  function stickDown(e, which) {
    const s = stickOf(which);
    if (s.active) return;
    e.preventDefault();
    const ring = e.currentTarget.querySelector(".stick-ring");
    const rect = ring.getBoundingClientRect();
    s.cx = rect.left + rect.width / 2;
    s.cy = rect.top + rect.height / 2;
    s.r = rect.width * 0.31; // thumb travel radius (thumb is 38% of the ring)
    s.active = true;
    s.pointerId = e.pointerId;
    capture(e.currentTarget, e.pointerId);
    stickUpdate(e, which);
  }
  function stickMove(e, which) {
    const s = stickOf(which);
    if (!s.active || e.pointerId !== s.pointerId) return;
    e.preventDefault();
    stickUpdate(e, which);
  }
  function stickUp(e, which) {
    const s = stickOf(which);
    if (!s.active || e.pointerId !== s.pointerId) return;
    s.active = false;
    s.pointerId = null;
    s.nx = s.ny = s.dx = s.dy = 0;
    if (which === "move") {
      joyF = 0; joyS = 0;
      sendGait("none"); // zero joy alone doesn't halt walking
      joyStopIfIdle();
    } else {
      sendLookMode("init");
    }
  }
  function stickUpdate(e, which) {
    const s = stickOf(which);
    let dx = e.clientX - s.cx;
    let dy = e.clientY - s.cy;
    const dist = Math.hypot(dx, dy);
    if (dist > s.r) { dx = (dx / dist) * s.r; dy = (dy / dist) * s.r; }
    let nx = s.r > 0 ? +(dx / s.r).toFixed(4) : 0;
    let ny = s.r > 0 ? +(dy / s.r).toFixed(4) : 0;
    const dz = which === "move" ? DEADZONE : LOOK_DEADZONE;
    const mag = Math.hypot(nx, ny);
    if (mag < dz) { nx = 0; ny = 0; dx = 0; dy = 0; }
    s.nx = nx; s.ny = ny; s.dx = dx; s.dy = dy;

    if (which === "move") {
      joyF = -ny;
      joyS = -nx;
      if (mag < dz) joyStopIfIdle(); else joyStartTimer();
    } else {
      sendLookMode(joystickToLookMode(nx, ny));
    }
  }

  // Visual helpers
  const mag = (s) => Math.min(1, Math.hypot(s.nx, s.ny));
  /** CSS angle: 0 = up, clockwise */
  const cssAngle = (s) => (Math.atan2(s.dx, -s.dy) * 180) / Math.PI;
  function sector(s) {
    const a = (Math.atan2(-s.ny, s.nx) * 180) / Math.PI; // 0 = right, 90 = up
    return Math.round(((a + 360) % 360) / 45) % 8;          // 0 R,1 UR,2 U,3 UL,4 L,5 DL,6 D,7 DR
  }
  const MOVE_NAMES = ["Right", "Forward · right", "Forward", "Forward · left", "Left", "Back · left", "Back", "Back · right"];
  const LOOK_NAMES = ["right", "up · right", "up", "up · left", "left", "down · left", "down", "down · right"];
  let moveReadout = $derived(mag(move) === 0 ? (move.active ? "Deadzone" : "Drag to walk") : `${MOVE_NAMES[sector(move)]} · ${Math.round(mag(move) * 100)}%`);
  let lookReadout = $derived(mag(look) === 0 ? (look.active ? "Centred" : "Drag to look around") : `Looking ${LOOK_NAMES[sector(look)]}`);
  /** dominant cardinal for rim glyph highlight: "n" | "e" | "s" | "w" | null */
  function cardinal(s) {
    if (mag(s) === 0) return null;
    return Math.abs(s.nx) > Math.abs(s.ny) ? (s.nx > 0 ? "e" : "w") : (s.ny > 0 ? "s" : "n");
  }

  // ── Buttons (hold-to-run) ────────────────────────────────────
  const BUTTON_GAITS = {
    "turn-l": "turnL",
    "turn-r": "turnR",
    "key-up": "advance",
    "key-down": "back",
    "key-left": "left",
    "key-right": "right",
    "action-top": "jump",
    "action-right": "twerk",
    "action-bottom": "sit",
    "action-left": "stretch",
  };
  let pressed = $state(new Set());
  let lastGait = $state(null);

  function gaitFor(id) { return assignments[id] || BUTTON_GAITS[id]; }
  function press(id) {
    if (pressed.has(id)) return;
    pressed = new Set(pressed).add(id);
    const gait = gaitFor(id);
    if (gait) { lastGait = gait; sendGait(gait); }
  }
  function release(id) {
    if (!pressed.has(id)) return;
    const next = new Set(pressed); next.delete(id); pressed = next;
    if (gaitFor(id)) { lastGait = null; sendGait("none"); }
  }
  function stopAll() {
    pressed = new Set();
    lastGait = null;
    joyF = 0; joyS = 0; joyT = 0;
    joyStopIfIdle();
    sendGait("none");
    if (navigator.vibrate) { try { navigator.vibrate(20); } catch {} }
  }
  function centreHead() { lastLookMode = null; sendLookMode("init"); }

  function capture(el, pointerId) {
    try { el.setPointerCapture(pointerId); } catch { /* synthetic / inactive pointer */ }
  }
  function btnDown(e, id) { e.preventDefault(); capture(e.currentTarget, e.pointerId); press(id); }
  function btnUp(e, id) { release(id); }

  // ── Keyboard ─────────────────────────────────────────────────
  const KEYMAP = {
    ArrowUp: "key-up", ArrowDown: "key-down", ArrowLeft: "key-left", ArrowRight: "key-right",
    w: "key-up", s: "key-down", a: "key-left", d: "key-right",
    q: "turn-l", e: "turn-r",
    i: "action-top", l: "action-right", k: "action-bottom", j: "action-left",
  };
  function onKeyDown(e) {
    if (e.target && /^(INPUT|TEXTAREA|SELECT)$/.test(e.target.tagName)) return;
    if (e.key === " " || e.key === "Escape") { e.preventDefault(); stopAll(); return; }
    const id = KEYMAP[e.key];
    if (!id) return;
    e.preventDefault();
    press(id);
  }
  function onKeyUp(e) {
    const id = KEYMAP[e.key];
    if (!id) return;
    e.preventDefault();
    release(id);
  }

  $effect(() => () => { if (joyTimer) clearInterval(joyTimer); });
</script>

<svelte:window onkeydown={onKeyDown} onkeyup={onKeyUp} />

{#snippet stick(which, s, readout, dz)}
  <div
    class="stick"
    class:look-stick={which === "look"}
    class:active={s.active}
    style="--ang: {cssAngle(s)}deg; --mag: {mag(s)}; --dz: {dz}"
    role="slider"
    tabindex="0"
    aria-label="{which === 'move' ? 'Move' : 'Look'} stick"
    aria-valuemin="0"
    aria-valuemax="100"
    aria-valuenow={Math.round(mag(s) * 100)}
    aria-valuetext={readout}
    onpointerdown={(e) => stickDown(e, which)}
    onpointermove={(e) => stickMove(e, which)}
    onpointerup={(e) => stickUp(e, which)}
    onpointercancel={(e) => stickUp(e, which)}
  >
    <div class="stick-ring">
      <div class="beam"></div>
      <span class="glyph n" class:on={cardinal(s) === "n"}><svg viewBox="0 0 24 24"><path d="M12 5l6 8H6z"/></svg></span>
      <span class="glyph e" class:on={cardinal(s) === "e"}><svg viewBox="0 0 24 24"><path d="M19 12l-8 6V6z"/></svg></span>
      <span class="glyph s" class:on={cardinal(s) === "s"}><svg viewBox="0 0 24 24"><path d="M12 19l-6-8h12z"/></svg></span>
      <span class="glyph w" class:on={cardinal(s) === "w"}><svg viewBox="0 0 24 24"><path d="M5 12l8-6v12z"/></svg></span>
      <div class="deadzone"></div>
      <div class="thumb" style="transform: translate(calc(-50% + {s.dx}px), calc(-50% + {s.dy}px))">
        {#if which === "look"}
          <!-- the Look cap is the pupper's face -->
          <span class="thumb-eyes"><i></i><i></i></span>
        {:else}
          <svg class="thumb-mark" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round"><path d="M12 3v18M3 12h18"/><path d="M12 3l-3 3M12 3l3 3M12 21l-3-3M12 21l3-3M3 12l3-3M3 12l3 3M21 12l-3-3M21 12l-3 3"/></svg>
        {/if}
      </div>
    </div>
  </div>
{/snippet}

<div class="pad-box">
<div class="pad" class:offline={!online}>
  <!-- ═══ SHOULDER BUMPERS (turn) ═══ -->
  <button
    class="bumper l"
    class:pressed={pressed.has("turn-l")}
    onpointerdown={(e) => btnDown(e, "turn-l")}
    onpointerup={(e) => btnUp(e, "turn-l")}
    onpointercancel={(e) => btnUp(e, "turn-l")}
    aria-label="Turn left (hold)"
    title="Turn left · Q"
  >
    <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.8" stroke-linecap="round" stroke-linejoin="round"><path d="M3 12a9 9 0 1 0 3-6.7"/><path d="M3 4v5h5"/></svg>
    <span>Turn</span>
  </button>
  <button
    class="bumper r"
    class:pressed={pressed.has("turn-r")}
    onpointerdown={(e) => btnDown(e, "turn-r")}
    onpointerup={(e) => btnUp(e, "turn-r")}
    onpointercancel={(e) => btnUp(e, "turn-r")}
    aria-label="Turn right (hold)"
    title="Turn right · E"
  >
    <span>Turn</span>
    <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.8" stroke-linecap="round" stroke-linejoin="round"><path d="M21 12a9 9 0 1 1-3-6.7"/><path d="M21 4v5h-5"/></svg>
  </button>

  <!-- ═══ MOVE ═══ -->
  <section class="cluster move" aria-label="Move">
    <div class="stick-label">
      <span class="title">Move</span>
    </div>
    {@render stick("move", move, moveReadout, DEADZONE)}
    <span class="readout" class:live={mag(move) > 0}>{moveReadout}</span>
  </section>

  <!-- ═══ ACTIONS (ABXY diamond) ═══ -->
  <section class="actions" aria-label="Actions">
    <div class="tools">
      {#if onEdit && !fullscreen}
        <button class="tool-btn" onclick={onEdit} aria-label="Edit actions" title="Edit actions">
          <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round"><path d="M11 4H4a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h14a2 2 0 0 0 2-2v-7"/><path d="M18.5 2.5a2.121 2.121 0 0 1 3 3L12 15l-4 1 1-4 9.5-9.5z"/></svg>
        </button>
      {/if}
      <button class="stop-btn" onclick={stopAll} aria-label="Stop all movement" title="Stop · Space">
        <span class="stop-square"></span>
        <span class="stop-text">Stop</span>
        {#if lastGait}<span class="stop-status">{lastGait}</span>{/if}
      </button>
      {#if onToggleFullscreen}
        <button class="tool-btn" onclick={onToggleFullscreen} aria-label={fullscreen ? "Exit fullscreen" : "Fullscreen"} title={fullscreen ? "Exit fullscreen" : "Fullscreen"}>
          {#if fullscreen}
            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round"><polyline points="4 14 10 14 10 20"/><polyline points="20 10 14 10 14 4"/><line x1="14" y1="10" x2="21" y2="3"/><line x1="3" y1="21" x2="10" y2="14"/></svg>
          {:else}
            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round"><polyline points="15 3 21 3 21 9"/><polyline points="9 21 3 21 3 15"/><line x1="21" y1="3" x2="14" y2="10"/><line x1="3" y1="21" x2="10" y2="14"/></svg>
          {/if}
        </button>
      {/if}
    </div>

    <div class="diamond">
      {#each SLOTS as slot (slot.id)}
        {@const a = slotAction(slot)}
        <div class="dcell {slot.pos}">
          <button
            class="abtn"
            class:pressed={pressed.has(slot.id)}
            style="--tone: {slot.color}"
            onpointerdown={(e) => btnDown(e, slot.id)}
            onpointerup={(e) => btnUp(e, slot.id)}
            onpointercancel={(e) => btnUp(e, slot.id)}
            aria-label="{a.label} (hold)"
            title={a.label}
          >
            <svg class="face-glyph" viewBox="0 0 24 24" aria-hidden="true">
              {#if slot.shape === "triangle"}
                <path d="M12 4l9 16H3z"/>
              {:else if slot.shape === "circle"}
                <circle cx="12" cy="12" r="8.5"/>
              {:else if slot.shape === "square"}
                <rect x="4" y="4" width="16" height="16" rx="2"/>
              {:else}
                <path d="M12 3l9 6.5-3.4 10.5H6.4L3 9.5z"/>
              {/if}
            </svg>
          </button>
          <span class="albl">{a.label}</span>
        </div>
      {/each}
      <span class="dcentre" aria-hidden="true"></span>
    </div>
  </section>

  <!-- ═══ LOOK ═══ -->
  <section class="cluster look" aria-label="Look">
    <div class="stick-label">
      <span class="title">Look</span>
      <button class="tool-btn mini" onclick={centreHead} aria-label="Centre head" title="Centre head">
        <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.6" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="3"/><path d="M12 2v4M12 18v4M2 12h4M18 12h4"/></svg>
      </button>
    </div>
    {@render stick("look", look, lookReadout, LOOK_DEADZONE)}
    <span class="readout" class:live={mag(look) > 0}>{lookReadout}</span>
  </section>
</div>
</div>

<style>
  /* ════════════════════════════════════════
     LAYOUT — container-query driven
     ════════════════════════════════════════ */
  /* The query container. Everything inside (including the .pad grid's own
     rules and cq* units) resolves against this box, i.e. the pad's own size. */
  .pad-box {
    container-type: size;
    width: 100%;
    height: 100%;
    min-width: 0;
    min-height: 0;
    display: grid;
    align-items: center;
  }

  .pad {
    /* shell */
    --shell: #1c1c1c;
    --shell-2: #2a2a2a;
    --shell-3: #363636;
    --shell-line: rgba(255, 255, 255, 0.07);
    --ink-on-shell: rgba(255, 255, 255, 0.62);
    --ink-on-shell-2: rgba(255, 255, 255, 0.4);

    --gap: clamp(8px, 2.2cqmin, 14px);
    --fs: clamp(0.66rem, 2.6cqmin, 0.8rem);
    --fs-sm: clamp(0.58rem, 2.2cqmin, 0.7rem);
    --cluster-gap: clamp(6px, 1.6cqmin, 10px);
    --label-h: 22px;
    --readout-h: 16px;
    /* label + readout + the two gaps that frame the stick */
    --chrome: calc(var(--label-h) + var(--readout-h) + 2 * var(--cluster-gap));
    --bumper-h: clamp(30px, 7cqmin, 40px);
    --tools-h: 34px;
    --albl: 16px;                                   /* label under an action button */
    --abtn: clamp(44px, 15cqw, 68px);               /* action button diameter */
    --actions-h: calc(var(--tools-h) + var(--gap) + 3 * (var(--abtn) + var(--albl)));
    --pad-p: clamp(10px, 2.6cqmin, 18px);           /* shell padding */
    --inner-h: calc(100cqh - 2 * var(--pad-p));     /* usable height inside the shell */

    width: 100%;
    height: 100%;
    min-height: 0;
    display: grid;
    gap: var(--gap);
    padding: var(--pad-p);
    box-sizing: border-box;
    border-radius: clamp(24px, 9cqmin, 52px);
    background:
      radial-gradient(ellipse 90% 55% at 50% 0%, rgba(255,255,255,0.09), transparent 60%),
      linear-gradient(180deg, var(--shell-2) 0%, var(--shell) 100%);
    box-shadow:
      inset 0 1px 0 rgba(255,255,255,0.12),
      inset 0 -3px 0 rgba(0,0,0,0.55),
      0 14px 30px rgba(0,0,0,0.28);
    /* tall / square: bumpers on top, then the diamond, sticks side by side */
    grid-template-columns: minmax(0, 1fr) minmax(0, 1fr);
    grid-template-rows: auto auto minmax(0, 1fr);
    grid-template-areas:
      "tl      tr"
      "actions actions"
      "move    look";
    /* sticks are width-bound; the height budget is what remains */
    --stick: clamp(88px, min(calc(42cqw - var(--pad-p)), calc(var(--inner-h) - var(--bumper-h) - var(--actions-h) - 2 * var(--gap) - var(--chrome))), 340px);
    user-select: none;
    -webkit-user-select: none;
    transition: opacity var(--dur-med), filter var(--dur-med);
  }
  .pad.offline { opacity: 0.6; filter: saturate(0.5); }

  @container (min-aspect-ratio: 5/4) {
    .pad {
      grid-template-columns: minmax(0, 1fr) auto minmax(0, 1fr);
      grid-template-rows: auto minmax(0, 1fr);
      grid-template-areas:
        "tl   actions tr"
        "move actions look";
      --abtn: clamp(40px, min(7.5cqw, calc((var(--inner-h) - var(--tools-h) - var(--gap) - 3 * var(--albl)) / 3)), 68px);
      --stick: clamp(88px, min(30cqw, calc(var(--inner-h) - var(--bumper-h) - var(--gap) - var(--chrome))), 340px);
      /* keep gamepad proportions on big screens: size the shell to its
         contents (bumpers + the taller of sticks / diamond) and centre it */
      height: min(100%, calc(var(--bumper-h) + var(--gap) + max(var(--stick) + var(--chrome), var(--actions-h)) + 2 * var(--pad-p)));
    }
    .pad > .cluster { justify-content: center; align-self: stretch; }
    .pad > .actions { align-self: center; }
  }

  .bumper.l { grid-area: tl; justify-self: start; }
  .bumper.r { grid-area: tr; justify-self: end; }
  .move { grid-area: move; }
  .look { grid-area: look; }
  .actions { grid-area: actions; justify-self: center; }

  /* ════════════════════════════════════════
     SHARED
     ════════════════════════════════════════ */
  .title {
    font-size: var(--fs);
    font-weight: 800;
    letter-spacing: 0.14em;
    text-transform: uppercase;
    color: var(--ink-on-shell);
  }
  .readout {
    height: var(--readout-h);
    line-height: var(--readout-h);
    font-size: var(--fs-sm);
    font-weight: 700;
    color: var(--ink-on-shell-2);
    white-space: nowrap; overflow: hidden; text-overflow: ellipsis;
    max-width: 100%;
    transition: color var(--dur-fast);
  }
  .readout.live { color: var(--mpx-yellow); }

  .tool-btn {
    width: var(--tools-h); height: var(--tools-h);
    flex-shrink: 0;
    border-radius: 50%;
    border: none;
    background: var(--shell-3);
    color: var(--ink-on-shell);
    box-shadow: inset 0 1px 0 rgba(255,255,255,0.1), 0 2px 0 rgba(0,0,0,0.5);
    display: grid; place-items: center;
    cursor: pointer;
    transition: transform var(--dur-fast), background var(--dur-fast), color var(--dur-fast);
  }
  .tool-btn svg { width: 55%; height: 55%; }
  .tool-btn:hover { color: #fff; }
  .tool-btn:active { transform: translateY(2px); box-shadow: inset 0 1px 0 rgba(255,255,255,0.1); }
  .tool-btn.mini { width: var(--label-h); height: var(--label-h); }

  .stick:focus-visible, .bumper:focus-visible, .abtn:focus-visible, .stop-btn:focus-visible, .tool-btn:focus-visible {
    outline: 3px solid var(--mpx-yellow);
    outline-offset: 3px;
  }

  /* ════════════════════════════════════════
     BUMPERS (turn L / R)
     ════════════════════════════════════════ */
  .bumper {
    height: var(--bumper-h);
    min-width: clamp(76px, 22cqw, 128px);
    padding: 0 clamp(10px, 3cqmin, 16px);
    display: inline-flex; align-items: center; justify-content: center; gap: 6px;
    border: none;
    border-radius: calc(var(--bumper-h) / 2);
    background: linear-gradient(180deg, #4a4a4a, var(--shell-3));
    color: var(--ink-on-shell);
    font-size: var(--fs-sm);
    font-weight: 800;
    letter-spacing: 0.12em;
    text-transform: uppercase;
    box-shadow: inset 0 1px 0 rgba(255,255,255,0.14), 0 3px 0 #0d0d0d;
    cursor: pointer;
    touch-action: none;
    transition: transform var(--dur-fast), background var(--dur-fast), color var(--dur-fast), box-shadow var(--dur-fast);
  }
  .bumper svg { width: calc(var(--bumper-h) * 0.55); height: calc(var(--bumper-h) * 0.55); }
  .bumper.l { border-top-left-radius: calc(var(--bumper-h) * 0.35); }
  .bumper.r { border-top-right-radius: calc(var(--bumper-h) * 0.35); }
  .bumper.pressed, .bumper:active {
    transform: translateY(3px);
    box-shadow: inset 0 1px 0 rgba(255,255,255,0.14), 0 0 0 #0d0d0d;
    background: var(--mpx-yellow);
    color: var(--mpx-ink);
  }

  /* ════════════════════════════════════════
     STICK CLUSTERS
     ════════════════════════════════════════ */
  .cluster {
    min-width: 0; min-height: 0;
    align-self: end; /* stacked: hug the sticks to the bottom (thumb reach) */
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: flex-end;
    gap: var(--cluster-gap);
  }
  .stick-label {
    height: var(--label-h);
    display: flex; align-items: center; justify-content: center; gap: 8px;
  }

  .stick {
    width: var(--stick);
    height: var(--stick);
    flex-shrink: 0;
    border-radius: 50%;
    padding: 5%;
    /* recessed well in the shell */
    background: radial-gradient(circle at 50% 40%, #0a0a0a 0%, #151515 60%, #202020 100%);
    box-shadow:
      inset 0 6px 14px rgba(0,0,0,0.85),
      inset 0 -1px 0 rgba(255,255,255,0.10),
      0 1px 0 rgba(255,255,255,0.06);
    cursor: grab;
    touch-action: none;
    position: relative;
  }
  .stick.active { cursor: grabbing; }

  .stick-ring {
    position: relative;
    width: 100%; height: 100%;
    border-radius: 50%;
    background: radial-gradient(circle at 50% 45%, #2c2c2c 0%, #1f1f1f 60%, #171717 100%);
    box-shadow: inset 0 2px 6px rgba(0,0,0,0.8);
    overflow: hidden;
  }
  /* crosshair */
  .stick-ring::before, .stick-ring::after {
    content: ""; position: absolute; background: rgba(255,255,255,0.06); pointer-events: none;
  }
  .stick-ring::before { left: 50%; top: 0; width: 1px; height: 100%; }
  .stick-ring::after { top: 50%; left: 0; height: 1px; width: 100%; }

  /* direction beam: a ring wedge that swings with the stick */
  .beam {
    position: absolute; inset: 0;
    border-radius: 50%;
    background: conic-gradient(from calc(var(--ang) - 32deg), var(--mpx-yellow) 0deg 64deg, transparent 64deg);
    -webkit-mask: radial-gradient(circle, transparent 62%, #000 63%);
    mask: radial-gradient(circle, transparent 62%, #000 63%);
    opacity: calc(var(--mag) * 0.9);
    transition: opacity var(--dur-fast);
    pointer-events: none;
  }

  .glyph {
    position: absolute;
    width: 14%; height: 14%;
    display: grid; place-items: center;
    color: rgba(255,255,255,0.22);
    transition: color var(--dur-fast), transform var(--dur-fast);
    pointer-events: none;
  }
  .glyph svg { width: 100%; height: 100%; fill: currentColor; }
  .glyph.n { top: 4%; left: 50%; transform: translateX(-50%); }
  .glyph.s { bottom: 4%; left: 50%; transform: translateX(-50%); }
  .glyph.w { left: 4%; top: 50%; transform: translateY(-50%); }
  .glyph.e { right: 4%; top: 50%; transform: translateY(-50%); }
  .glyph.on { color: var(--mpx-ink); }
  .glyph.n.on { transform: translateX(-50%) scale(1.25); }
  .glyph.s.on { transform: translateX(-50%) scale(1.25); }
  .glyph.w.on { transform: translateY(-50%) scale(1.25); }
  .glyph.e.on { transform: translateY(-50%) scale(1.25); }

  .deadzone {
    position: absolute;
    left: 50%; top: 50%;
    /* travel radius is 31% of the ring; deadzone = dz × travel × 2 */
    width: calc(var(--dz) * 62%); height: calc(var(--dz) * 62%);
    transform: translate(-50%, -50%);
    border-radius: 50%;
    border: 1.5px dashed rgba(255,255,255,0.14);
    pointer-events: none;
  }

  /* thumb cap */
  .thumb {
    position: absolute;
    left: 50%; top: 50%;
    width: 38%; height: 38%;
    border-radius: 50%;
    background:
      radial-gradient(circle at 50% 50%, transparent 58%, rgba(0,0,0,0.12) 60%, transparent 63%),
      radial-gradient(circle at 40% 32%, #fff6a8 0%, var(--mpx-yellow) 42%, var(--mpx-yellow-deep) 100%);
    border: 3px solid #0e0e0e;
    box-shadow: 0 5px 12px rgba(0,0,0,0.6), inset 0 1px 0 rgba(255,255,255,0.75), inset 0 -3px 0 rgba(0,0,0,0.18);
    display: grid; place-items: center;
    color: var(--mpx-ink);
    will-change: transform;
    pointer-events: none;
    transition: box-shadow var(--dur-fast);
  }
  .stick:not(.active) .thumb { transition: transform 180ms var(--ease-spring), box-shadow var(--dur-fast); }
  .stick.active .thumb { box-shadow: 0 9px 18px rgba(0,0,0,0.7), inset 0 1px 0 rgba(255,255,255,0.75), inset 0 -3px 0 rgba(0,0,0,0.18); }
  .thumb-mark { width: 46%; height: 46%; opacity: 0.5; }

  .thumb-eyes { display: flex; gap: 14%; width: 56%; justify-content: center; }
  .thumb-eyes i {
    display: block;
    width: 26%; aspect-ratio: 11 / 20;
    border-radius: 999px 999px 40% 40%;
    background: var(--mpx-ink);
    transform-origin: center;
    animation: blink 6s infinite;
  }
  @keyframes blink { 0%, 93%, 100% { transform: scaleY(1); } 96% { transform: scaleY(0.12); } }

  /* ════════════════════════════════════════
     ACTIONS — ABXY diamond with Stop in the middle
     ════════════════════════════════════════ */
  .actions {
    min-width: 0; min-height: 0;
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: var(--gap);
  }
  .tools {
    height: var(--tools-h);
    width: calc(3 * var(--abtn) + 16px);
    display: flex; align-items: center; gap: 8px;
  }
  /* emergency stop — its own wide pill, clearly not a face button */
  .stop-btn {
    flex: 1; min-width: 0;
    height: var(--tools-h);
    display: inline-flex; align-items: center; justify-content: center; gap: 7px;
    padding: 0 10px;
    border: 1.5px solid rgba(237,118,118,0.55);
    border-radius: calc(var(--tools-h) / 2);
    background: linear-gradient(180deg, #2e2e2e, #1a1a1a);
    color: #fff;
    font-size: var(--fs-sm);
    font-weight: 800;
    letter-spacing: 0.14em;
    text-transform: uppercase;
    box-shadow: inset 0 1px 0 rgba(255,255,255,0.08), 0 3px 0 #000;
    cursor: pointer;
    transition: transform var(--dur-fast), box-shadow var(--dur-fast), background var(--dur-fast);
  }
  .stop-btn:active {
    transform: translateY(3px);
    box-shadow: inset 0 1px 0 rgba(255,255,255,0.08), 0 0 0 #000;
    background: var(--mpx-err);
    color: var(--mpx-ink);
  }
  .stop-square {
    width: 10px; height: 10px; flex-shrink: 0;
    border-radius: 2px;
    background: var(--mpx-err);
    box-shadow: 0 0 8px rgba(237,118,118,0.6);
  }
  .stop-btn:active .stop-square { background: var(--mpx-ink); box-shadow: none; }
  .stop-status {
    min-width: 0;
    font-weight: 700;
    letter-spacing: 0.06em;
    color: var(--mpx-yellow);
    white-space: nowrap; overflow: hidden; text-overflow: ellipsis;
  }
  .stop-status::before { content: "·"; margin-right: 7px; color: var(--ink-on-shell-2); }

  .diamond {
    display: grid;
    grid-template-columns: repeat(3, var(--abtn));
    grid-template-rows: repeat(3, calc(var(--abtn) + var(--albl)));
    column-gap: 8px;
    /* faint cross behind the diamond, like a moulded recess */
    background:
      radial-gradient(circle at 50% calc(50% - var(--albl) / 2), rgba(0,0,0,0.35) 0%, rgba(0,0,0,0.25) 52%, transparent 58%);
  }
  .dcell {
    display: flex; flex-direction: column; align-items: center;
    width: var(--abtn);
  }
  .dcell.top    { grid-area: 1 / 2; }
  .dcell.left   { grid-area: 2 / 1; }
  .dcell.right  { grid-area: 2 / 3; }
  .dcell.bottom { grid-area: 3 / 2; }

  .abtn {
    position: relative;
    width: var(--abtn); height: var(--abtn);
    border-radius: 50%;
    border: none;
    display: grid; place-items: center;
    background:
      radial-gradient(circle at 40% 30%, rgba(255,255,255,0.45) 0%, rgba(255,255,255,0) 45%),
      var(--tone);
    color: var(--mpx-ink);
    box-shadow:
      inset 0 -4px 0 rgba(0,0,0,0.22),
      inset 0 1px 0 rgba(255,255,255,0.5),
      0 4px 0 rgba(0,0,0,0.55),
      0 6px 12px rgba(0,0,0,0.35);
    cursor: pointer;
    touch-action: none;
    transition: transform var(--dur-fast), box-shadow var(--dur-fast), filter var(--dur-fast);
  }
  .face-glyph {
    width: 40%; height: 40%;
    fill: rgba(0,0,0,0.62);
    filter: drop-shadow(0 1px 0 rgba(255,255,255,0.35));
    transition: fill var(--dur-fast);
  }
  .abtn.pressed .face-glyph, .abtn:active .face-glyph { fill: var(--mpx-ink); }
  .abtn.pressed, .abtn:active {
    transform: translateY(4px);
    filter: brightness(1.12);
    box-shadow:
      inset 0 -2px 0 rgba(0,0,0,0.22),
      inset 0 1px 0 rgba(255,255,255,0.5),
      0 0 0 rgba(0,0,0,0.55),
      0 0 0 4px color-mix(in srgb, var(--mpx-yellow) 70%, transparent);
  }
  .albl {
    height: var(--albl);
    line-height: var(--albl);
    width: calc(var(--abtn) + 20px);
    text-align: center;
    font-size: var(--fs-sm);
    font-weight: 800;
    color: var(--ink-on-shell);
    white-space: nowrap; overflow: hidden; text-overflow: ellipsis;
  }
  /* moulded dimple in the middle of the diamond */
  .dcentre {
    grid-area: 2 / 2;
    align-self: start;
    justify-self: center;
    margin-top: calc(var(--abtn) * 0.25);
    width: calc(var(--abtn) * 0.5); height: calc(var(--abtn) * 0.5);
    border-radius: 50%;
    background: radial-gradient(circle at 50% 35%, #101010, #1e1e1e);
    box-shadow: inset 0 3px 6px rgba(0,0,0,0.8), 0 1px 0 rgba(255,255,255,0.06);
  }

  @media (prefers-reduced-motion: reduce) {
    .thumb-eyes i { animation: none; }
  }
</style>
