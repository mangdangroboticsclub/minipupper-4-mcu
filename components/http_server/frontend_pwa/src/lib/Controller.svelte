<script>
  import { colors } from "./colors.js";

  const YELLOW = colors.mpx.primary;

  /** Fraction of joystick radius that is a deadzone (0–1) */
  const DEADZONE = 0.18;
  const LOOK_DEADZONE = 0.35;

  let { assignments = {} } = $props();

  // ── Joystick state ──────────────────────────────────────────
  let joystickTopX = $state(0);
  let joystickTopY = $state(0);
  let joystickBotX = $state(0);
  let joystickBotY = $state(0);
  let joystickTopDx = $state(0);
  let joystickTopDy = $state(0);
  let joystickBotDx = $state(0);
  let joystickBotDy = $state(0);
  let activeJoystick = $state(null); // 'top' | 'bottom' | null

  // Joystick geometry (set on pointerdown)
  let jCenterX = $state(0);
  let jCenterY = $state(0);
  let jMaxRadius = $state(0);

  // ── Button press state ──────────────────────────────────────
  let pressedButtons = $state(new Set());

  // ── Robot API: Joystick values & timer ──────────────────────
  let joyF = 0, joyS = 0, joyT = 0;
  let joyTimer = null;

  async function sendJoy() {
    try {
      await fetch("/v1/robot/joy", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ f: joyF, s: joyS, t: joyT }),
      });
    } catch { /* transient — keep trying while pad is held */ }
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
      sendJoy(); // final zeros -> step in place
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

  // ── Look direction (head pose) mapping ──────────────────────
  /** Map joystick normalised (-1..1) position to a look gait command */
  function joystickToLookMode(nx, ny) {
    const dist = Math.sqrt(nx * nx + ny * ny);
    if (dist < LOOK_DEADZONE) return 'init';

    const angle = Math.atan2(ny, nx) * 180 / Math.PI;

    if (angle > -22.5 && angle <= 22.5) return 'lookright';
    if (angle > 22.5 && angle <= 67.5) return 'looklr';
    if (angle > 67.5 && angle <= 112.5) return 'lookdown';
    if (angle > 112.5 && angle <= 157.5) return 'lookll';
    if (angle > 157.5 || angle <= -157.5) return 'lookleft';
    if (angle > -157.5 && angle <= -112.5) return 'lookul';
    if (angle > -112.5 && angle <= -67.5) return 'lookup';
    if (angle > -67.5 && angle <= -22.5) return 'lookur';

    return 'init';
  }

  let lastLookMode = $state('init');

  function sendLookMode(mode) {
    if (mode === lastLookMode) return;
    lastLookMode = mode;
    sendGait(mode);
  }

  // ── Keyboard state ──────────────────────────────────────────
  const keyMap = {
    'ArrowUp':    'dpad-up',
    'ArrowDown':  'dpad-down',
    'ArrowLeft':  'dpad-left',
    'ArrowRight': 'dpad-right',
    'w':          'dpad-up',
    's':          'dpad-down',
    'a':          'dpad-left',
    'd':          'dpad-right',
    'i':          'action-top',
    'l':          'action-right',
    'k':          'action-bottom',
    'j':          'action-left',
    'q':          'shoulder-l',
    'e':          'shoulder-r',
  };

  // ── Joystick helpers ────────────────────────────────────────

  function getJoystickClientPos(e) {
    if (e.touches && e.touches.length > 0) {
      return { x: e.touches[0].clientX, y: e.touches[0].clientY };
    }
    return { x: e.clientX, y: e.clientY };
  }

  function getJoystickRingRect(ringEl) {
    return ringEl.getBoundingClientRect();
  }

  function computeJoystickGeometry(ringEl) {
    const rect = ringEl.getBoundingClientRect();
    jCenterX = rect.left + rect.width / 2;
    jCenterY = rect.top + rect.height / 2;
    // The thumb is ~48% of ring width; so max radius = ring half - thumb half - 2px
    const thumbWidth = rect.width * 0.48;
    jMaxRadius = rect.width / 2 - thumbWidth / 2 - 2;
  }

  function startJoystick(e, which, ringEl) {
    e.preventDefault();
    activeJoystick = which;
    computeJoystickGeometry(ringEl);
    updateJoystick(e);

    // Global listeners
    document.addEventListener('mousemove', onJoystickMove);
    document.addEventListener('mouseup', endJoystick);
    document.addEventListener('touchmove', onJoystickMove, { passive: false });
    document.addEventListener('touchend', endJoystick);
    document.addEventListener('touchcancel', endJoystick);
  }

  function onJoystickMove(e) {
    e.preventDefault();
    if (!activeJoystick) return;
    updateJoystick(e);
  }

  function endJoystick() {
    if (!activeJoystick) return;
    if (activeJoystick === 'top') {
      joystickTopX = 0;
      joystickTopY = 0;
      joystickTopDx = 0;
      joystickTopDy = 0;
      joyF = 0; joyS = 0;
      // Explicitly stop the gait — zero joy values alone don't halt walking
      sendGait('none');
    } else {
      joystickBotX = 0;
      joystickBotY = 0;
      joystickBotDx = 0;
      joystickBotDy = 0;
      // Center the head on release
      sendLookMode('init');
    }
    activeJoystick = null;
    joyStopIfIdle();

    document.removeEventListener('mousemove', onJoystickMove);
    document.removeEventListener('mouseup', endJoystick);
    document.removeEventListener('touchmove', onJoystickMove);
    document.removeEventListener('touchend', endJoystick);
    document.removeEventListener('touchcancel', endJoystick);
  }

  function updateJoystick(e) {
    const pos = getJoystickClientPos(e);
    let dx = pos.x - jCenterX;
    let dy = pos.y - jCenterY;
    const dist = Math.sqrt(dx * dx + dy * dy);

    if (dist > jMaxRadius) {
      dx = (dx / dist) * jMaxRadius;
      dy = (dy / dist) * jMaxRadius;
    }

    let nx = jMaxRadius > 0 ? +(dx / jMaxRadius).toFixed(4) : 0;
    let ny = jMaxRadius > 0 ? +(dy / jMaxRadius).toFixed(4) : 0;

    // ── deadzone: snap small deflections to zero ──────────────
    const dz = activeJoystick === 'top' ? DEADZONE : LOOK_DEADZONE;
    const mag = Math.sqrt(nx * nx + ny * ny);
    if (mag < dz) {
      nx = 0;
      ny = 0;
      dx = 0;
      dy = 0;
    }

    if (activeJoystick === 'top') {
      joystickTopX = nx;
      joystickTopY = ny;
      joystickTopDx = dx;
      joystickTopDy = dy;
      // Left pad: forward/strafe
      joyF = -ny;
      joyS = -nx;
      if (mag < DEADZONE) {
        joyStopIfIdle();
      } else {
        joyStartTimer();
      }
    } else {
      joystickBotX = nx;
      joystickBotY = ny;
      joystickBotDx = dx;
      joystickBotDy = dy;
      // Right pad: head pose (look direction)
      const mode = joystickToLookMode(nx, ny);
      sendLookMode(mode);
    }
  }

  // ── Button handlers ─────────────────────────────────────────

  /** Map controller button IDs to robot gait commands */
  const buttonGaitMap = {
    'dpad-up':    'advance',
    'dpad-down':  'back',
    'dpad-left':  'left',
    'dpad-right': 'right',
    'action-top':    'jump',
    'action-right':  'twerk',
    'action-bottom': 'sit',
    'action-left':   'stretch',
    'shoulder-l': 'turnL',
    'shoulder-r': 'turnR',
  };

  function handlePress(action) {
    if (pressedButtons.has(action)) return;
    pressedButtons.add(action);
    // Check action assignments first, fall back to hardcoded map
    const gait = assignments[action] || buttonGaitMap[action];
    if (gait) sendGait(gait);
  }

  function handleRelease(action) {
    if (!pressedButtons.has(action)) return;
    pressedButtons.delete(action);
    // stop the gait when the button is released (hold-to-action)
    const gait = assignments[action] || buttonGaitMap[action];
    if (gait) sendGait('none');
  }

  function onButtonPointerDown(e, action) {
    e.preventDefault();
    handlePress(action);
    e.currentTarget.setPointerCapture(e.pointerId);
  }

  function onButtonPointerUp(e, action) {
    handleRelease(action);
  }

  function onButtonPointerLeave(e, action) {
    handleRelease(action);
  }

  // ── Keyboard handlers ───────────────────────────────────────

  function onKeyDown(e) {
    const action = keyMap[e.key];
    if (!action) return;
    e.preventDefault();
    handlePress(action);
  }

  function onKeyUp(e) {
    const action = keyMap[e.key];
    if (!action) return;
    e.preventDefault();
    handleRelease(action);
  }


</script>

<svelte:window onkeydown={onKeyDown} onkeyup={onKeyUp} />

<div class="controller-wrapper">
  <!-- ═══ Shoulder Buttons ═══ -->
  <button
    class="shoulder-btn shoulder-left"
    class:pressed={pressedButtons.has('shoulder-l')}
    onpointerdown={(e) => onButtonPointerDown(e, 'shoulder-l')}
    onpointerup={(e) => onButtonPointerUp(e, 'shoulder-l')}
    onpointerleave={(e) => onButtonPointerLeave(e, 'shoulder-l')}
    aria-label="L (shoulder)"
  >
    <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><path d="M3 12a9 9 0 1 0 3-6.7"/><path d="M3 4v5h5"/></svg>
    <span>L</span>
  </button>
  <button
    class="shoulder-btn shoulder-right"
    class:pressed={pressedButtons.has('shoulder-r')}
    onpointerdown={(e) => onButtonPointerDown(e, 'shoulder-r')}
    onpointerup={(e) => onButtonPointerUp(e, 'shoulder-r')}
    onpointerleave={(e) => onButtonPointerLeave(e, 'shoulder-r')}
    aria-label="R (shoulder)"
  >
    <span>R</span>
    <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><path d="M21 12a9 9 0 1 1-3-6.7"/><path d="M21 4v5h-5"/></svg>
  </button>

  <!-- ═══ Controller Body ═══ -->
  <div class="controller-body">
    <!-- Decorative cross backgrounds (behind D-Pad and Action buttons) -->
    <div class="bg-cross dpad-bg-cross">
      <div class="cross-bar cross-bar-h"></div>
      <div class="cross-bar cross-bar-v"></div>
    </div>
    <div class="bg-cross action-bg-cross">
      <div class="cross-bar cross-bar-h"></div>
      <div class="cross-bar cross-bar-v"></div>
    </div>

    <!-- Left Analog Joystick (Top) -->
    <!-- svelte-ignore a11y_interactive_supports_focus a11y_role_has_required_aria_props -->
    <div
      class="joystick-zone joystick-top"
      class:active={activeJoystick === 'top'}
      onpointerdown={(e) => {
        const ring = e.currentTarget.querySelector('.joystick-ring');
        startJoystick(e, 'top', ring);
      }}
      role="slider"
      aria-label="Left joystick"
      aria-valuenow={Math.round((Math.abs(joystickTopX) + Math.abs(joystickTopY)) * 50)}
      aria-valuetext={`x:${joystickTopX.toFixed(2)} y:${joystickTopY.toFixed(2)}`}
    >
      <div class="joystick-inner">
        <div class="joystick-ring">
          <div class="deadzone-indicator" style="--dz-pct:{DEADZONE * 100}%"></div>
          <div
            class="joystick-thumb"
            style="transform: translate(calc(-50% + {joystickTopDx}px), calc(-50% + {joystickTopDy}px))"
          ></div>
        </div>
      </div>
    </div>

    <!-- D-Pad -->
    <div class="dpad">
      <button
        class="dpad-btn"
        class:pressed={pressedButtons.has('dpad-up')}
        onpointerdown={(e) => onButtonPointerDown(e, 'dpad-up')}
        onpointerup={(e) => onButtonPointerUp(e, 'dpad-up')}
        onpointerleave={(e) => onButtonPointerLeave(e, 'dpad-up')}
        aria-label="D-Pad Up"
      >
        <svg viewBox="0 0 24 24">
          <polygon points="12,3 21,20 3,20" fill="currentColor"/>
        </svg>
      </button>
      <button
        class="dpad-btn"
        class:pressed={pressedButtons.has('dpad-left')}
        onpointerdown={(e) => onButtonPointerDown(e, 'dpad-left')}
        onpointerup={(e) => onButtonPointerUp(e, 'dpad-left')}
        onpointerleave={(e) => onButtonPointerLeave(e, 'dpad-left')}
        aria-label="D-Pad Left"
      >
        <svg viewBox="0 0 24 24">
          <polygon points="3,12 20,3 20,21" fill="currentColor"/>
        </svg>
      </button>
      <button
        class="dpad-btn"
        class:pressed={pressedButtons.has('dpad-right')}
        onpointerdown={(e) => onButtonPointerDown(e, 'dpad-right')}
        onpointerup={(e) => onButtonPointerUp(e, 'dpad-right')}
        onpointerleave={(e) => onButtonPointerLeave(e, 'dpad-right')}
        aria-label="D-Pad Right"
      >
        <svg viewBox="0 0 24 24">
          <polygon points="21,12 4,3 4,21" fill="currentColor"/>
        </svg>
      </button>
      <button
        class="dpad-btn"
        class:pressed={pressedButtons.has('dpad-down')}
        onpointerdown={(e) => onButtonPointerDown(e, 'dpad-down')}
        onpointerup={(e) => onButtonPointerUp(e, 'dpad-down')}
        onpointerleave={(e) => onButtonPointerLeave(e, 'dpad-down')}
        aria-label="D-Pad Down"
      >
        <svg viewBox="0 0 24 24">
          <polygon points="12,21 3,4 21,4" fill="currentColor"/>
        </svg>
      </button>
    </div>

    <!-- Action Buttons -->
    <div class="action-btns">
      <button
        class="action-btn"
        class:pressed={pressedButtons.has('action-top')}
        onpointerdown={(e) => onButtonPointerDown(e, 'action-top')}
        onpointerup={(e) => onButtonPointerUp(e, 'action-top')}
        onpointerleave={(e) => onButtonPointerLeave(e, 'action-top')}
        aria-label="Action Top (Triangle)"
      >
        <svg viewBox="0 0 50 50" class="action-svg">
          <path d="M25 6 L40 34.5 L10 34.5 Z" fill="none" stroke="var(--ok)" stroke-width="4" stroke-linejoin="round"/>
        </svg>
      </button>
      <button
        class="action-btn"
        class:pressed={pressedButtons.has('action-right')}
        onpointerdown={(e) => onButtonPointerDown(e, 'action-right')}
        onpointerup={(e) => onButtonPointerUp(e, 'action-right')}
        onpointerleave={(e) => onButtonPointerLeave(e, 'action-right')}
        aria-label="Action Right (Circle)"
      >
        <svg viewBox="0 0 50 50" class="action-svg">
          <circle cx="25" cy="25" r="15" fill="none" stroke="var(--danger)" stroke-width="4"/>
        </svg>
      </button>
      <button
        class="action-btn"
        class:pressed={pressedButtons.has('action-bottom')}
        onpointerdown={(e) => onButtonPointerDown(e, 'action-bottom')}
        onpointerup={(e) => onButtonPointerUp(e, 'action-bottom')}
        onpointerleave={(e) => onButtonPointerLeave(e, 'action-bottom')}
        aria-label="Action Bottom (Pentagon)"
      >
        <svg viewBox="0 0 50 50" class="action-svg">
          <polygon points="25,10 39.3,20.4 33.8,37.1 16.2,37.1 10.7,20.4" fill="none" stroke="var(--info)" stroke-width="4" stroke-linejoin="round"/>
        </svg>
      </button>
      <button
        class="action-btn"
        class:pressed={pressedButtons.has('action-left')}
        onpointerdown={(e) => onButtonPointerDown(e, 'action-left')}
        onpointerup={(e) => onButtonPointerUp(e, 'action-left')}
        onpointerleave={(e) => onButtonPointerLeave(e, 'action-left')}
        aria-label="Action Left (Square)"
      >
        <svg viewBox="0 0 50 50" class="action-svg">
          <rect x="13" y="13" width="24" height="24" rx="3" fill="none" stroke="#9C86DE" stroke-width="4"/>
        </svg>
      </button>
    </div>

    <!-- Right Analog Joystick (Bottom) -->
    <!-- svelte-ignore a11y_interactive_supports_focus a11y_role_has_required_aria_props -->
    <div
      class="joystick-zone joystick-bottom"
      class:active={activeJoystick === 'bottom'}
      onpointerdown={(e) => {
        const ring = e.currentTarget.querySelector('.joystick-ring');
        startJoystick(e, 'bottom', ring);
      }}
      role="slider"
      aria-label="Right joystick"
      aria-valuenow={Math.round((Math.abs(joystickBotX) + Math.abs(joystickBotY)) * 50)}
      aria-valuetext={`x:${joystickBotX.toFixed(2)} y:${joystickBotY.toFixed(2)}`}
    >
      <div class="joystick-inner">
        <div class="joystick-ring">
          <div class="deadzone-indicator" style="--dz-pct:{LOOK_DEADZONE * 100}%"></div>
          <div
            class="joystick-thumb"
            style="transform: translate(calc(-50% + {joystickBotDx}px), calc(-50% + {joystickBotDy}px))"
          ></div>
        </div>
      </div>
    </div>

  </div>
</div>

<style>
  /* ══════════════════════════════════════════════════════════════════
     The gamepad as machined hardware: a bezel (shell) holding a plate
     (core), concave wells for the sticks, keys that sit proud of the
     plate and go down when pressed. Colour means one thing -- yellow is
     a live control. Touch geometry is unchanged from the original.
     ══════════════════════════════════════════════════════════════════ */
  .controller-wrapper {
    position: relative;
    width: 100%;
    flex: 1;
    min-height: 460px;
    max-width: 400px;
    align-self: center;
    container-type: size;
  }

  /* ── Shoulder keys: two tabs rising out of the bezel ─────────────── */
  .shoulder-btn {
    position: absolute;
    top: 1.5%;
    height: 8.5%;
    width: 26%;
    display: flex;
    align-items: center;
    justify-content: center;
    gap: 7px;
    padding: 0 0 4px;
    border: 1px solid var(--line);
    border-bottom: none;
    border-radius: 18px 18px 0 0;
    background: linear-gradient(180deg, var(--surface-3), var(--shell));
    box-shadow: inset 0 1px 0 var(--highlight);
    color: var(--text-2);
    font-weight: 700;
    font-size: 14px;
    letter-spacing: 0.06em;
    cursor: pointer;
    z-index: 4;
    transition: transform 160ms var(--ease), background-color 160ms var(--ease), color 160ms var(--ease);
  }
  .shoulder-btn svg { width: 17px; height: 17px; }
  .shoulder-left  { left: 7%; }
  .shoulder-right { right: 7%; }
  .shoulder-btn.pressed,
  .shoulder-btn:active {
    transform: translateY(2px);
    background: var(--accent);
    border-color: var(--accent);
    color: var(--accent-ink);
    box-shadow: 0 0 18px rgba(255, 230, 5, 0.35), inset 0 1px 0 rgba(255,255,255,0.3);
  }

  /* ── Body: bezel + plate ──────────────────────────────────────────── */
  .controller-body {
    position: absolute;
    inset: 9% 0 0 0;
    background: var(--shell);
    border: 1px solid var(--line);
    border-radius: 30px;
    box-shadow: var(--shadow-card);
  }
  .controller-body::before {
    content: '';
    position: absolute;
    inset: 6px;
    border-radius: 24px;
    background:
      radial-gradient(120% 80% at 50% 0%, rgba(255, 255, 255, 0.05), transparent 60%),
      var(--surface-1);
    box-shadow: inset 0 1px 0 var(--highlight);
  }

  /* The recessed cross behind the d-pad and the buttons. */
  .bg-cross {
    position: absolute;
    pointer-events: none;
    z-index: 0;
  }
  .dpad-bg-cross   { left: 4.05%;  top: 40%; width: 38.71%; aspect-ratio: 162.588 / 166.554; }
  .action-bg-cross { right: 7.14%; top: 40%; width: 38.57%; aspect-ratio: 1; }
  .cross-bar {
    position: absolute;
    border-radius: 18%;
    background: rgba(0, 0, 0, 0.28);
    box-shadow: inset 0 2px 6px rgba(0, 0, 0, 0.45), 0 1px 0 rgba(255, 255, 255, 0.04);
  }
  :global([data-theme="light"]) .cross-bar { background: rgba(17, 17, 20, 0.08); box-shadow: inset 0 2px 6px rgba(17,17,20,0.12), 0 1px 0 rgba(255,255,255,0.8); }
  .cross-bar-h { width: 100%; height: 36%; top: 50%; left: 0; transform: translateY(-50%); }
  .cross-bar-v { width: 36%; height: 100%; top: 0; left: 50%; transform: translateX(-50%); }

  /* ── Sticks: a concave well, a convex thumb ───────────────────────── */
  .joystick-zone {
    position: absolute;
    width: 44%;
    width: min(44%, 29cqh); /* never let the sticks collide with the keys on short phones */
    aspect-ratio: 1;
    border-radius: 50%;
    left: 50%;
    transform: translateX(-50%);
    display: flex;
    align-items: center;
    justify-content: center;
    background:
      radial-gradient(circle at 50% 42%, rgba(0, 0, 0, 0.38), rgba(0, 0, 0, 0) 78%),
      var(--surface-2);
    box-shadow:
      inset 0 8px 18px rgba(0, 0, 0, 0.5),
      inset 0 -1px 0 rgba(255, 255, 255, 0.06),
      0 1px 0 rgba(255, 255, 255, 0.05);
    cursor: pointer;
    touch-action: none;
    z-index: 5;
    transition: box-shadow 260ms var(--ease);
  }
  :global([data-theme="light"]) .joystick-zone {
    background: radial-gradient(circle at 50% 42%, rgba(17,17,20,0.10), rgba(17,17,20,0) 78%), var(--surface-2);
    box-shadow: inset 0 6px 14px rgba(17,17,20,0.14), 0 1px 0 #fff;
  }
  .joystick-zone.active { box-shadow: inset 0 8px 18px rgba(0, 0, 0, 0.5), 0 0 0 2px var(--accent-line), 0 0 30px rgba(255, 230, 5, 0.22); }
  .joystick-top    { top: 11%; }
  .joystick-bottom { bottom: 3.5%; }

  .joystick-inner {
    width: 86%;
    aspect-ratio: 1;
    border-radius: 50%;
    display: flex;
    align-items: center;
    justify-content: center;
    position: relative;
    pointer-events: none;
  }
  /* The ring: a hairline circle with four ticks, like a dial. */
  .joystick-ring {
    position: relative;
    width: 100%;
    aspect-ratio: 1;
    border-radius: 50%;
    border: 1px solid var(--line-strong);
    pointer-events: none;
  }
  .joystick-ring::before,
  .joystick-ring::after {
    content: '';
    position: absolute;
    background: var(--line-strong);
    pointer-events: none;
  }
  .joystick-ring::before { width: 1px; height: 100%; left: 50%; top: 0; transform: translateX(-50%);
    background: linear-gradient(180deg, var(--line-strong) 0 8%, transparent 8% 92%, var(--line-strong) 92%); }
  .joystick-ring::after  { width: 100%; height: 1px; top: 50%; left: 0; transform: translateY(-50%);
    background: linear-gradient(90deg, var(--line-strong) 0 8%, transparent 8% 92%, var(--line-strong) 92%); }

  .deadzone-indicator {
    position: absolute;
    width: calc(var(--dz-pct, 30%) * 2);
    aspect-ratio: 1;
    border-radius: 50%;
    border: 1px dashed var(--line-strong);
    top: 50%;
    left: 50%;
    transform: translate(-50%, -50%);
    pointer-events: none;
    z-index: 1;
    opacity: 0.7;
  }

  .joystick-thumb {
    position: absolute;
    width: 46%;
    aspect-ratio: 1;
    border-radius: 50%;
    background:
      radial-gradient(circle at 35% 28%, rgba(255, 255, 255, 0.16), rgba(255, 255, 255, 0) 58%),
      var(--surface-3);
    border: 1px solid var(--line-strong);
    box-shadow:
      0 10px 22px rgba(0, 0, 0, 0.55),
      inset 0 1px 0 rgba(255, 255, 255, 0.14),
      inset 0 -3px 6px rgba(0, 0, 0, 0.35);
    left: 50%;
    top: 50%;
    transform: translate(-50%, -50%);
    transition: box-shadow 200ms var(--ease);
    will-change: transform;
    pointer-events: none;
  }
  :global([data-theme="light"]) .joystick-thumb {
    background: radial-gradient(circle at 35% 28%, #fff, rgba(255,255,255,0) 60%), var(--surface-3);
    box-shadow: 0 8px 18px rgba(17,17,20,0.22), inset 0 1px 0 #fff, inset 0 -3px 6px rgba(17,17,20,0.10);
  }
  .joystick-thumb::after {
    content: '';
    position: absolute;
    width: 26%;
    aspect-ratio: 1;
    border-radius: 50%;
    background: var(--text-3);
    box-shadow: inset 0 1px 2px rgba(0, 0, 0, 0.4);
    top: 50%;
    left: 50%;
    transform: translate(-50%, -50%);
    transition: background-color 200ms var(--ease), box-shadow 200ms var(--ease);
  }
  .joystick-zone.active .joystick-thumb {
    border-color: var(--accent-line);
    box-shadow: 0 12px 26px rgba(0, 0, 0, 0.6), 0 0 0 2px var(--accent-line), inset 0 1px 0 rgba(255, 255, 255, 0.14), inset 0 -3px 6px rgba(0, 0, 0, 0.35);
  }
  .joystick-zone.active .joystick-thumb::after { background: var(--accent); box-shadow: 0 0 12px rgba(255, 230, 5, 0.7); }

  /* ── D-pad: four keys standing proud of a recess ──────────────────── */
  .dpad {
    position: absolute;
    left: 4.05%;
    top: 40%;
    width: 38.71%;
    aspect-ratio: 162.588 / 166.554;
    display: grid;
    grid-template-columns: repeat(3, 1fr);
    grid-template-rows: repeat(3, 1fr);
    gap: 3%;
    padding: 3%;
  }
  .dpad-btn {
    width: 100%;
    height: 100%;
    border: 1px solid var(--line);
    border-radius: 18%;
    cursor: pointer;
    display: flex;
    align-items: center;
    justify-content: center;
    background: linear-gradient(180deg, var(--surface-3), var(--surface-2));
    box-shadow: var(--shadow-key);
    color: var(--text-1);
    transition: transform 120ms var(--ease), background-color 120ms var(--ease), box-shadow 120ms var(--ease);
  }
  .dpad-btn svg { pointer-events: none; width: 40%; height: auto; }
  .dpad-btn:nth-child(1) { grid-column: 2; grid-row: 1; border-radius: 30% 30% 12% 12%; }
  .dpad-btn:nth-child(2) { grid-column: 1; grid-row: 2; border-radius: 30% 12% 12% 30%; }
  .dpad-btn:nth-child(3) { grid-column: 3; grid-row: 2; border-radius: 12% 30% 30% 12%; }
  .dpad-btn:nth-child(4) { grid-column: 2; grid-row: 3; border-radius: 12% 12% 30% 30%; }
  .dpad-btn.pressed,
  .dpad-btn:active {
    transform: translateY(1px);
    background: var(--accent);
    border-color: var(--accent);
    color: var(--accent-ink);
    box-shadow: 0 0 18px rgba(255, 230, 5, 0.35), inset 0 1px 0 rgba(255, 255, 255, 0.35);
  }

  /* ── Action keys (diamond) ────────────────────────────────────────── */
  .action-btns {
    position: absolute;
    right: 7.14%;
    top: 40%;
    width: 38.57%;
    aspect-ratio: 1;
  }
  .action-btn {
    position: absolute;
    width: 29%;
    aspect-ratio: 1;
    border: 1px solid var(--line);
    border-radius: 50%;
    cursor: pointer;
    display: flex;
    align-items: center;
    justify-content: center;
    background:
      radial-gradient(circle at 40% 30%, rgba(255, 255, 255, 0.10), rgba(255, 255, 255, 0) 60%),
      var(--surface-2);
    box-shadow: var(--shadow-key);
    transition: transform 120ms var(--ease), box-shadow 120ms var(--ease);
  }
  .action-btn svg { pointer-events: none; width: 56%; height: auto; }
  .action-btn:nth-child(1) { top: 0; left: 50%; transform: translateX(-50%); }
  .action-btn:nth-child(2) { top: 50%; right: 0; transform: translateY(-50%); }
  .action-btn:nth-child(3) { bottom: 0; left: 50%; transform: translateX(-50%); }
  .action-btn:nth-child(4) { top: 50%; left: 0; transform: translateY(-50%); }
  .action-btn.pressed,
  .action-btn:active {
    background: var(--surface-3);
    box-shadow: 0 0 0 2px var(--accent-line), 0 0 16px rgba(255, 230, 5, 0.25), inset 0 1px 0 rgba(255,255,255,0.1);
  }
  .action-btn:nth-child(1).pressed, .action-btn:nth-child(1):active { transform: translateX(-50%) translateY(1px); }
  .action-btn:nth-child(2).pressed, .action-btn:nth-child(2):active { transform: translateY(-50%) translateY(1px); }
  .action-btn:nth-child(3).pressed, .action-btn:nth-child(3):active { transform: translateX(-50%) translateY(1px); }
  .action-btn:nth-child(4).pressed, .action-btn:nth-child(4):active { transform: translateY(-50%) translateY(1px); }
</style>
