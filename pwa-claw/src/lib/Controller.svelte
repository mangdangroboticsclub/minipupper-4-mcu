<script>
  import { colors } from "./colors.js";

  /**
   * Controller — the yellow gamepad on Home.
   *
   * Portrait ("tall"): shoulder Turn buttons + Power on top, the body d-pad and
   * the four move buttons in the middle, Move / Look sticks underneath, like
   * holding a PS4 pad upright. Landscape ("wide", mp4 style): d-pad left, the
   * sticks in the middle under Power, move buttons right, Turn on the shoulders.
   * While Power is off, no control sends anything to the robot.
   *
   * @prop assignments   slot id → gait id (from the action editor)
   * @prop catalog       [{ id, label }] used to name assigned actions
   * @prop online        robot reachable (dims the pad when false)
   * @prop wide          landscape layout
   * @prop onEdit        () => void — open the button editor
   * @prop onExitWide    () => void — shown in landscape to go back to portrait
   * @prop rotated       the pad is drawn turned 90° clockwise on a portrait
   *                     screen, so finger movement is turned back before use
   */
  let {
    assignments = {},
    catalog = [],
    online = true,
    wide = false,
    rotated = false,
    onEdit,
    onExitWide,
  } = $props();

  const DEADZONE = 0.18;
  const LOOK_DEADZONE = 0.35;

  // The four assignable face buttons — ids are shared with ActionAssignment.
  const SLOTS = [
    { id: "action-top",    pos: "top",    fallback: "jump",    tone: "#14907F", shape: "triangle" },
    { id: "action-left",   pos: "left",   fallback: "stretch", tone: "#C2338A", shape: "square" },
    { id: "action-right",  pos: "right",  fallback: "twerk",   tone: "#D83A34", shape: "circle" },
    { id: "action-bottom", pos: "bottom", fallback: "situp",   tone: "#2D6BCB", shape: "pentagon" },
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

  // Analog look (firmware /v1/robot/look): the head glides toward yaw/pitch and
  // back to the stand when centred. Falls back to the 8 discrete look gaits on
  // firmware without the endpoint.
  const LOOK_YAW_MAX = 30, LOOK_PITCH_MAX = 20;
  let lookPadOk = true;
  let lookPending = null, lookTimer = null, lookLastSent = "";
  function lookPadSend() {
    lookTimer = null;
    if (!lookPending) return;
    const { yaw, pitch } = lookPending;
    lookPending = null;
    const key = `${yaw},${pitch}`;
    if (key === lookLastSent) return;
    lookLastSent = key;
    fetch(`/v1/robot/look?yaw=${yaw}&pitch=${pitch}`)
      .then((r) => { if (r.status === 404) lookPadOk = false; })
      .catch(() => {});
    lookTimer = setTimeout(lookPadSend, 80);
  }
  function sendLook(nx, ny) {
    if (!lookPadOk) { sendLookMode(joystickToLookMode(nx, ny)); return; }
    const m = Math.hypot(nx, ny);
    const k = m > 0 ? Math.min(1, (m - LOOK_DEADZONE) / (1 - LOOK_DEADZONE)) / m : 0;
    lookPending = {
      yaw: Math.round(nx * k * LOOK_YAW_MAX),          // stick right -> head right
      pitch: Math.round(-ny * k * LOOK_PITCH_MAX),     // stick up -> nose up
    };
    if (!lookTimer) lookPadSend();
  }

  // ── Sticks ───────────────────────────────────────────────────
  // Each stick tracks its own pointer so Move + Look work together on touch.
  const makeStick = () => ({ nx: 0, ny: 0, dx: 0, dy: 0, active: false, pointerId: null, cx: 0, cy: 0, r: 0 });
  let move = $state(makeStick());
  let look = $state(makeStick());
  const stickOf = (which) => (which === "move" ? move : look);

  // The right stick turns the robot on the spot (minipupperesp's right pad)
  // or moves the head. Tap its caption to switch; the choice is remembered.
  const RIGHT_MODE_KEY = "mpx.rightStick";
  function loadRightMode() {
    try { return localStorage.getItem(RIGHT_MODE_KEY) === "look" ? "look" : "turn"; } catch { return "turn"; }
  }
  let rightMode = $state(loadRightMode());
  function toggleRightMode() {
    if (look.active) return;
    if (rightMode === "look") sendLook(0, 0);
    else { joyT = 0; joyStopIfIdle(); }
    rightMode = rightMode === "turn" ? "look" : "turn";
    try { localStorage.setItem(RIGHT_MODE_KEY, rightMode); } catch { /* private mode */ }
  }
  const turning = () => rightMode === "turn" && look.active;

  function stickDown(e, which) {
    const s = stickOf(which);
    if (!powered || s.active) return;
    e.preventDefault();
    const rect = e.currentTarget.getBoundingClientRect();
    s.cx = rect.left + rect.width / 2;
    s.cy = rect.top + rect.height / 2;
    s.r = rect.width * 0.32;    // knob travel: the knob is half the well
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
      if (!turning()) sendGait("none"); // zero joy alone doesn't halt walking
      joyStopIfIdle();
    } else if (rightMode === "turn") {
      joyT = 0;
      if (!move.active) sendGait("none");
      joyStopIfIdle();
    } else {
      sendLook(0, 0);
    }
  }
  function stickUpdate(e, which) {
    const s = stickOf(which);
    const sx = e.clientX - s.cx;
    const sy = e.clientY - s.cy;
    // Turned 90° clockwise, the pad's right is the screen's down.
    let dx = rotated ? sy : sx;
    let dy = rotated ? -sx : sy;
    const turnStick = which === "look" && rightMode === "turn";
    if (turnStick) dy = 0;      // turning uses the left-right axis only
    const dist = Math.hypot(dx, dy);
    if (dist > s.r) { dx = (dx / dist) * s.r; dy = (dy / dist) * s.r; }
    let nx = s.r > 0 ? +(dx / s.r).toFixed(4) : 0;
    let ny = s.r > 0 ? +(dy / s.r).toFixed(4) : 0;
    const dz = which === "move" || turnStick ? DEADZONE : LOOK_DEADZONE;
    const mag = Math.hypot(nx, ny);
    // The knob follows the finger even inside the deadzone; only the command
    // is held at zero there.
    s.dx = dx; s.dy = dy;
    if (mag < dz) { nx = 0; ny = 0; }
    s.nx = nx; s.ny = ny;

    if (which === "move") {
      joyF = -ny;
      joyS = -nx;
      if (mag < dz) joyStopIfIdle(); else joyStartTimer();
    } else if (turnStick) {
      // Stick right -> turn right (firmware: t > 0 turns left). Past the
      // deadzone the turn rate runs 0.1..1, so a small push is a slow turn
      // (and is above the firmware's 0.08 start threshold).
      const a = Math.abs(nx);
      const t = a < dz ? 0 : Math.sign(nx) * Math.min(1, 0.1 + 0.9 * (a - dz) / (1 - dz));
      joyT = t === 0 ? 0 : -(+t.toFixed(3));
      if (joyT === 0) joyStopIfIdle(); else joyStartTimer();
    } else {
      sendLook(nx, ny);
    }
  }

  // ── Buttons (hold-to-run) ────────────────────────────────────
  const BUTTON_GAITS = {
    "turn-l": "turnL",
    "turn-r": "turnR",
    // keyboard: arrows / WASD walk
    "key-up": "advance",
    "key-down": "back",
    "key-left": "left",
    "key-right": "right",
    // d-pad: body height and side steps
    "dpad-up": "heightup",
    "dpad-down": "heightdown",
    "dpad-left": "left",
    "dpad-right": "right",
    "action-top": "jump",
    "action-right": "twerk",
    "action-bottom": "situp",
    "action-left": "stretch",
  };
  let pressed = $state(new Set());
  let lastLabel = $state("");

  function gaitFor(id) { return assignments[id] || BUTTON_GAITS[id]; }

  // Which gaits keep running until stopped. One-shot moves end by themselves,
  // so releasing the button must not abort them with "none".
  let continuousGaits = null;
  fetch("/v1/robot/moves")
    .then((r) => (r.ok ? r.json() : null))
    .then((j) => {
      if (j && Array.isArray(j.moves))
        continuousGaits = new Set(j.moves.filter((m) => m.continuous).map((m) => m.name));
    })
    .catch(() => {});
  const isContinuous = (g) => !continuousGaits || continuousGaits.has(g);

  function press(id) {
    if (!powered || pressed.has(id)) return;
    pressed = new Set(pressed).add(id);
    const gait = gaitFor(id);
    if (gait) {
      sendGait(gait);
      const slot = SLOTS.find((s) => s.id === id);
      if (slot) lastLabel = slotAction(slot).label;
    }
    if (navigator.vibrate) { try { navigator.vibrate(8); } catch {} }
  }
  function release(id) {
    if (!pressed.has(id)) return;
    const next = new Set(pressed); next.delete(id); pressed = next;
    const gait = gaitFor(id);
    if (gait && isContinuous(gait)) sendGait("none");
    if (SLOTS.some((s) => s.id === id)) setTimeout(() => { if (!SLOTS.some((s) => pressed.has(s.id))) lastLabel = ""; }, 900);
  }
  function stopAll() {
    pressed = new Set();
    lastLabel = "";
    joyF = 0; joyS = 0; joyT = 0;
    joyStopIfIdle();
    sendGait("none");
    if (navigator.vibrate) { try { navigator.vibrate(20); } catch {} }
  }

  // Power gates every control. Off by default so a stray touch on a freshly
  // opened page cannot move the robot.
  let powered = $state(false);
  function togglePower() {
    if (powered) {
      move = makeStick();
      look = makeStick();
      stopAll();
      sendLook(0, 0);
    } else if (navigator.vibrate) {
      try { navigator.vibrate(12); } catch {}
    }
    powered = !powered;
  }

  function capture(el, pointerId) {
    try { el.setPointerCapture(pointerId); } catch { /* synthetic / inactive pointer */ }
  }
  function btnDown(e, id) { e.preventDefault(); capture(e.currentTarget, e.pointerId); press(id); }
  function btnUp(e, id) { release(id); }
  function btnKey(e, id) {
    if (e.key !== "Enter" && e.key !== " ") return;
    e.preventDefault();
    e.stopPropagation();
    if (e.type === "keydown") press(id); else release(id);
  }

  // ── Keyboard ─────────────────────────────────────────────────
  const KEYMAP = {
    ArrowUp: "key-up", ArrowDown: "key-down", ArrowLeft: "key-left", ArrowRight: "key-right",
    w: "key-up", s: "key-down", a: "key-left", d: "key-right",
    q: "turn-l", e: "turn-r",
    r: "dpad-up", f: "dpad-down",
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

  const DPAD = [
    { id: "dpad-up",    pos: "up",    label: "Body up",    path: "M12 6l7 10H5z" },
    { id: "dpad-left",  pos: "left",  label: "Step left",  path: "M6 12l10-7v14z" },
    { id: "dpad-right", pos: "right", label: "Step right", path: "M18 12L8 5v14z" },
    { id: "dpad-down",  pos: "down",  label: "Body down",  path: "M12 18L5 8h14z" },
  ];
</script>

<svelte:window onkeydown={onKeyDown} onkeyup={onKeyUp} />

{#snippet stick(which, s)}
  <div class="stick-wrap">
    <div
      class="well"
      class:active={s.active}
      role="slider"
      tabindex="0"
      aria-label={which === "move" ? "Move stick: drag to walk" : rightMode === "turn" ? "Turn stick: drag left or right to turn" : "Look stick: drag to look around"}
      aria-valuemin="0"
      aria-valuemax="100"
      aria-valuenow={Math.round(Math.min(1, Math.hypot(s.nx, s.ny)) * 100)}
      onpointerdown={(e) => stickDown(e, which)}
      onpointermove={(e) => stickMove(e, which)}
      onpointerup={(e) => stickUp(e, which)}
      onpointercancel={(e) => stickUp(e, which)}
    >
      <span class="well-ring" aria-hidden="true"></span>
      <span class="knob" class:held={s.active} style="transform: translate({s.dx}px, {s.dy}px)" aria-hidden="true">
        {#if which === "look" && rightMode === "turn"}
          <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><path d="M4 12a8 8 0 0 1 14-5.3M20 12a8 8 0 0 1-14 5.3"/><path d="M18 3v4h-4M6 21v-4h4"/></svg>
        {:else if which === "look"}
          <span class="knob-eyes"><i></i><i></i></span>
        {:else}
          <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><path d="M12 3v18M3 12h18M12 3l-3 3M12 3l3 3M12 21l-3-3M12 21l3-3M3 12l3-3M3 12l3 3M21 12l-3-3M21 12l-3 3"/></svg>
        {/if}
      </span>
    </div>
    {#if which === "move"}
      <span class="cap">Move</span>
    {:else}
      <button
        type="button"
        class="cap cap-toggle"
        onclick={toggleRightMode}
        aria-label={rightMode === "turn" ? "Right stick turns the robot. Switch to look" : "Right stick moves the head. Switch to turn"}
        title="Tap to switch Turn / Look"
      >
        {rightMode === "turn" ? "Turn" : "Look"}
        <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.6" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M7 7h12l-3-3M17 17H5l3 3"/></svg>
      </button>
    {/if}
  </div>
{/snippet}

{#snippet dpad()}
  <div class="cluster">
    <div class="dpad">
      {#each DPAD as k (k.id)}
        <button
          type="button"
          class="dkey {k.pos}"
          class:pressed={pressed.has(k.id)}
          aria-label="{k.label} (hold)"
          onpointerdown={(e) => btnDown(e, k.id)}
          onpointerup={(e) => btnUp(e, k.id)}
          onpointercancel={(e) => btnUp(e, k.id)}
          onkeydown={(e) => btnKey(e, k.id)}
          onkeyup={(e) => btnKey(e, k.id)}
        >
          <svg viewBox="0 0 24 24" aria-hidden="true"><path d={k.path}/></svg>
        </button>
      {/each}
      <span class="dhub" aria-hidden="true"></span>
    </div>
    <span class="cap soft">Body</span>
  </div>
{/snippet}

{#snippet faces()}
  <div class="cluster">
    <div class="faces">
      {#each SLOTS as slot (slot.id)}
        {@const a = slotAction(slot)}
        <button
          type="button"
          class="fbtn {slot.pos}"
          class:pressed={pressed.has(slot.id)}
          style="--tone: {slot.tone}"
          aria-label="{a.label} (hold)"
          title={a.label}
          onpointerdown={(e) => btnDown(e, slot.id)}
          onpointerup={(e) => btnUp(e, slot.id)}
          onpointercancel={(e) => btnUp(e, slot.id)}
          onkeydown={(e) => btnKey(e, slot.id)}
          onkeyup={(e) => btnKey(e, slot.id)}
        >
          <svg viewBox="0 0 24 24" aria-hidden="true">
            {#if slot.shape === "triangle"}<path d="M12 4l9 16H3z"/>
            {:else if slot.shape === "circle"}<circle cx="12" cy="12" r="8"/>
            {:else if slot.shape === "square"}<rect x="4.5" y="4.5" width="15" height="15" rx="1"/>
            {:else}<path d="M12 3l9 6.5-3.4 10.5H6.4L3 9.5z"/>{/if}
          </svg>
        </button>
      {/each}
    </div>
    <span class="cap soft" aria-live="polite">{lastLabel || "Moves"}</span>
  </div>
{/snippet}

{#snippet tools()}
  <div class="tools">
    {#if wide && onExitWide}
      <button type="button" class="tool" onclick={onExitWide} aria-label="Back to portrait">
        <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M15 6l-6 6 6 6"/></svg>
      </button>
    {/if}
    {#if onEdit}
      <button type="button" class="tool" onclick={onEdit} aria-label="Edit buttons">
        <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M4 20h4L19 9l-4-4L4 16z"/></svg>
      </button>
    {/if}
    <button
      type="button"
      class="power"
      class:on={powered}
      onclick={togglePower}
      aria-pressed={powered}
      aria-label={powered ? "Power on: controls enabled. Tap to stop and disable" : "Power off: controls disabled. Tap to enable"}
      title={powered ? "Power on" : "Power off"}
    >
      <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.6" stroke-linecap="round" aria-hidden="true"><path d="M12 3v8"/><path d="M6.3 7.2a7.5 7.5 0 1 0 11.4 0"/></svg>
    </button>
  </div>
{/snippet}

{#snippet shoulder(side)}
  {@const id = side === "l" ? "turn-l" : "turn-r"}
  <button
    type="button"
    class="shoulder {side}"
    class:pressed={pressed.has(id)}
    aria-label={side === "l" ? "Turn left (hold)" : "Turn right (hold)"}
    onpointerdown={(e) => btnDown(e, id)}
    onpointerup={(e) => btnUp(e, id)}
    onpointercancel={(e) => btnUp(e, id)}
    onkeydown={(e) => btnKey(e, id)}
    onkeyup={(e) => btnKey(e, id)}
  >
    {#if side === "l"}
      <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M4 12a8 8 0 1 0 2.3-5.6"/><path d="M4 4v4h4"/></svg>
      <span>L1</span>
    {:else}
      <span>R1</span>
      <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M20 12a8 8 0 1 1-2.3-5.6"/><path d="M20 4v4h-4"/></svg>
    {/if}
  </button>
{/snippet}

<div class="box" class:wide>
  <section class="shell" class:offline={!online} class:unpowered={!powered} aria-label="Controller">
    {#if wide}
      <div class="row-top">
        {@render shoulder("l")}
        {@render shoulder("r")}
      </div>
      <div class="body">
        {@render dpad()}
        <div class="centre">
          {@render tools()}
          <div class="sticks">
            {@render stick("move", move)}
            {@render stick("look", look)}
          </div>
        </div>
        {@render faces()}
      </div>
    {:else}
      <div class="row-top">
        {@render shoulder("l")}
        {@render tools()}
        {@render shoulder("r")}
      </div>
      <div class="row-mid">
        {@render dpad()}
        {@render faces()}
      </div>
      <div class="sticks">
        {@render stick("move", move)}
        {@render stick("look", look)}
      </div>
    {/if}
  </section>
</div>

<style>
  /* Sizes follow the controller's own width (portrait) or height (landscape),
     so the pad fits any phone without media queries. */
  .box { container-type: inline-size; width: 100%; }
  .box.wide { container-type: size; height: 100%; display: grid; align-items: center; }

  .shell {
    --btn: clamp(42px, 13.6cqw, 60px);
    --stick: clamp(118px, 42cqw, 200px);
    --side: clamp(12px, 4.6cqw, 20px);
    position: relative;
    border-radius: 40px;
    padding: 0 var(--side) 18px;
    background:
      radial-gradient(120% 70% at 50% 0%, rgba(255, 255, 255, 0.38), transparent 60%),
      linear-gradient(180deg, #FFE24A 0%, #FFD31A 55%, #F2C200 100%);
    box-shadow:
      inset 0 2px 0 rgba(255, 255, 255, 0.55),
      inset 0 -6px 0 rgba(160, 120, 0, 0.25),
      0 14px 30px rgba(120, 90, 0, 0.18);
    display: flex;
    flex-direction: column;
    user-select: none;
    -webkit-user-select: none;
    transition: opacity var(--dur-med), filter var(--dur-med);
  }
  .shell.offline { opacity: 0.6; filter: saturate(0.4); }

  /* ── portrait ── */
  .row-top { display: flex; align-items: flex-start; justify-content: space-between; margin: 0 calc(-1 * var(--side)); }
  .row-mid { display: flex; align-items: center; justify-content: space-between; margin-top: clamp(10px, 3.4cqw, 18px); }
  .sticks { display: flex; justify-content: space-between; align-items: flex-end; margin-top: clamp(14px, 5cqw, 24px); }

  /* ── landscape ── */
  .wide .shell {
    --btn: clamp(40px, 14cqh, 64px);
    --stick: clamp(110px, 42cqh, 210px);
    --side: clamp(14px, 3cqw, 28px);
    height: min(100%, 520px);
    border-radius: 44px;
    padding-bottom: clamp(10px, 4cqh, 22px);
  }
  .wide .row-top { margin-bottom: 0; }
  .wide .body {
    flex: 1;
    min-height: 0;
    display: grid;
    grid-template-columns: auto 1fr auto;
    align-items: center;
    gap: clamp(10px, 3cqw, 32px);
  }
  .wide .centre { display: flex; flex-direction: column; align-items: center; gap: clamp(8px, 3cqh, 18px); min-width: 0; }
  .wide .sticks { margin-top: 0; justify-content: center; gap: clamp(14px, 5cqw, 56px); }
  .wide .tools { padding-top: 0; }

  .cluster { display: flex; flex-direction: column; align-items: center; gap: 6px; }
  .cap { font-size: 12px; font-weight: 800; letter-spacing: 0.14em; text-transform: uppercase; color: var(--ink); }
  .cap.soft { font-size: 11px; color: rgba(17, 17, 17, 0.62); max-width: calc(var(--btn) * 3); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }

  /* ── shoulders ── */
  .shoulder {
    height: clamp(52px, 16cqw, 64px);
    min-width: clamp(86px, 26cqw, 120px);
    white-space: nowrap;
    border: none;
    display: flex; align-items: center; gap: 8px;
    padding: 0 clamp(12px, 4.4cqw, 18px);
    font-size: 14px; font-weight: 800; letter-spacing: 0.06em; color: var(--ink);
    background: linear-gradient(180deg, #F4C800, #E2B400);
    box-shadow: inset 0 -3px 0 rgba(110, 80, 0, 0.25);
    cursor: pointer;
    touch-action: none;
    transition: transform var(--dur-fast), filter var(--dur-fast);
  }
  .shoulder svg { width: 24px; height: 24px; flex-shrink: 0; }
  .shoulder.l { border-radius: 40px 0 28px 0; justify-content: flex-start; }
  .shoulder.r { border-radius: 0 40px 0 28px; justify-content: flex-end; }
  .wide .shoulder.l { border-radius: 44px 0 28px 0; }
  .wide .shoulder.r { border-radius: 0 44px 0 28px; }
  .shoulder.pressed { transform: translateY(2px); filter: brightness(0.92); }

  /* ── tools ── */
  .tools { display: flex; gap: 8px; padding-top: 12px; }
  .tool {
    width: 44px; height: 44px; border-radius: 22px; border: none;
    background: rgba(17, 17, 17, 0.08); color: var(--ink);
    display: grid; place-items: center; cursor: pointer;
  }
  .tool svg { width: 18px; height: 18px; }
  .power {
    width: 44px; height: 44px; border-radius: 50%; border: none;
    background: var(--ink); color: #FF5A4F;
    display: grid; place-items: center;
    cursor: pointer;
    transition: color var(--dur-fast), box-shadow var(--dur-fast);
  }
  .power svg { width: 22px; height: 22px; }
  .power.on { color: #2ECC71; box-shadow: 0 0 0 3px rgba(46, 204, 113, 0.45), 0 0 14px rgba(46, 204, 113, 0.55); }
  .power:active { transform: translateY(1px); }

  .unpowered :is(.dpad, .faces, .well, .shoulder) {
    opacity: 0.45;
    filter: saturate(0.3);
    cursor: not-allowed;
    transition: opacity var(--dur-med), filter var(--dur-med);
  }

  /* ── d-pad ── */
  .dpad {
    position: relative;
    display: grid;
    grid-template-columns: repeat(3, var(--btn));
    grid-template-rows: repeat(3, var(--btn));
    grid-template-areas: ". up ." "left hub right" ". down .";
  }
  .dkey {
    border: none; background: #FFFCF0; color: var(--ink);
    box-shadow: 0 3px 0 rgba(130, 95, 0, 0.35);
    display: grid; place-items: center; cursor: pointer; touch-action: none;
    transition: transform 60ms, box-shadow 60ms;
  }
  .dkey svg { width: 36%; height: 36%; fill: currentColor; }
  .dkey.up { grid-area: up; border-radius: 12px 12px 4px 4px; }
  .dkey.down { grid-area: down; border-radius: 4px 4px 12px 12px; }
  .dkey.left { grid-area: left; border-radius: 12px 4px 4px 12px; }
  .dkey.right { grid-area: right; border-radius: 4px 12px 12px 4px; }
  .dhub { grid-area: hub; background: #FFFCF0; }
  .dkey.pressed { transform: translateY(3px); box-shadow: 0 0 0 rgba(130, 95, 0, 0.35); background: #fff; }

  /* ── face buttons ── */
  .faces {
    display: grid;
    grid-template-columns: repeat(3, var(--btn));
    grid-template-rows: repeat(3, var(--btn));
    grid-template-areas: ". top ." "left . right" ". bottom .";
    place-items: center;
  }
  .fbtn {
    width: var(--btn); height: var(--btn); border-radius: 50%; border: none;
    background: #FFFCF0;
    box-shadow: 0 3px 0 rgba(130, 95, 0, 0.35);
    display: grid; place-items: center; cursor: pointer; touch-action: none;
    transition: transform 60ms, box-shadow 60ms, background 60ms;
  }
  .fbtn svg { width: 42%; height: 42%; fill: none; stroke: var(--tone); stroke-width: 2.6; stroke-linejoin: round; }
  .fbtn.top { grid-area: top; }
  .fbtn.left { grid-area: left; }
  .fbtn.right { grid-area: right; }
  .fbtn.bottom { grid-area: bottom; }
  .fbtn.pressed { transform: translateY(3px); box-shadow: 0 0 0 transparent; background: var(--tone); }
  .fbtn.pressed svg { stroke: #fff; }

  /* ── sticks ── */
  .stick-wrap { display: flex; flex-direction: column; align-items: center; gap: 10px; }
  .well {
    position: relative;
    width: var(--stick); height: var(--stick);
    border-radius: 50%;
    background: #26262A;
    box-shadow: inset 0 6px 14px rgba(0, 0, 0, 0.55), 0 2px 0 rgba(255, 255, 255, 0.45);
    display: grid; place-items: center;
    touch-action: none;
    cursor: grab;
  }
  .well.active { cursor: grabbing; }
  .well-ring { position: absolute; inset: 9%; border-radius: 50%; border: 1.5px dashed rgba(255, 255, 255, 0.14); pointer-events: none; }
  .knob {
    width: 50%; height: 50%; border-radius: 50%;
    background: radial-gradient(circle at 40% 32%, #FFFFFF 0%, #ECEAE4 60%, #D3D0C8 100%);
    box-shadow: 0 6px 12px rgba(0, 0, 0, 0.45), inset 0 -3px 0 rgba(0, 0, 0, 0.12);
    display: grid; place-items: center; color: #6B6862;
    pointer-events: none;
    transition: transform 160ms cubic-bezier(0.3, 1.5, 0.5, 1);
  }
  .knob.held { transition: none; }
  .knob svg { width: 34%; height: 34%; }
  .cap-toggle {
    display: inline-flex; align-items: center; gap: 5px;
    padding: 3px 9px; margin: -3px 0;
    border: 1.5px solid rgba(17, 17, 17, 0.22); border-radius: 999px;
    background: transparent; font-family: inherit; cursor: pointer;
  }
  .cap-toggle svg { width: 11px; height: 11px; }
  .cap-toggle:active { background: rgba(17, 17, 17, 0.08); }
  .knob-eyes { display: flex; gap: 10%; width: 32%; justify-content: center; }
  .knob-eyes i { width: 9px; height: 16px; border-radius: 3px; background: var(--ink); }

  button:focus-visible, .well:focus-visible { outline: 3px solid var(--ink); outline-offset: 3px; }
</style>
