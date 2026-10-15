<script>
  import { colors } from "./colors.js";

  const YELLOW = colors.mpx.primary;

  /** Fraction of joystick radius that is a deadzone (0–1) */
  const DEADZONE = 0.18;
  const LOOK_DEADZONE = 0.35;

  let { assignments = {} } = $props();

  // ── Joystick state ──────────────────────────────────────────
  let joystickLX = $state(0);
  let joystickLY = $state(0);
  let joystickRx = $state(0);
  let joystickRy = $state(0);
  let joystickLDx = $state(0);
  let joystickLDy = $state(0);
  let joystickRDx = $state(0);
  let joystickRDy = $state(0);
  let activeJoystick = $state(null); // 'left' | 'right' | null

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
    } catch {}
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
      sendJoy();
    }
  }

  async function sendGait(mode) {
    try {
      await fetch("/v1/robot/gait", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ mode }),
      });
    } catch {}
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

  function computeJoystickGeometry(ringEl) {
    const rect = ringEl.getBoundingClientRect();
    jCenterX = rect.left + rect.width / 2;
    jCenterY = rect.top + rect.height / 2;
    const thumbWidth = rect.width * 0.48;
    jMaxRadius = rect.width / 2 - thumbWidth / 2 - 2;
  }

  function startJoystick(e, which, ringEl) {
    e.preventDefault();
    activeJoystick = which;
    computeJoystickGeometry(ringEl);
    updateJoystick(e);

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
    if (activeJoystick === 'left') {
      joystickLX = 0; joystickLY = 0;
      joystickLDx = 0; joystickLDy = 0;
      joyF = 0; joyS = 0;
      // Explicitly stop the gait — zero joy values alone don't halt walking
      sendGait('none');
    } else {
      joystickRx = 0; joystickRy = 0;
      joystickRDx = 0; joystickRDy = 0;
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
    const dz = activeJoystick === 'left' ? DEADZONE : LOOK_DEADZONE;
    const mag = Math.sqrt(nx * nx + ny * ny);
    if (mag < dz) {
      nx = 0;
      ny = 0;
      dx = 0;
      dy = 0;
    }

    if (activeJoystick === 'left') {
      joystickLX = nx; joystickLY = ny;
      joystickLDx = dx; joystickLDy = dy;
      joyF = -ny;
      joyS = -nx;
      if (mag < DEADZONE) {
        joyStopIfIdle();
      } else {
        joyStartTimer();
      }
    } else {
      joystickRx = nx; joystickRy = ny;
      joystickRDx = dx; joystickRDy = dy;
      // Right pad: head pose (look direction)
      const mode = joystickToLookMode(nx, ny);
      sendLookMode(mode);
    }
  }

  // ── Button handlers ─────────────────────────────────────────

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

<div class="landscape-wrapper">
  <div class="landscape-inner">
  <!-- ════════════════════════════════════════
       SVG VISUAL LAYER (background)
       ════════════════════════════════════════ -->
  <svg class="bg-svg" width="100%" height="100%" viewBox="0 0 881 457" preserveAspectRatio="xMidYMid meet" fill="none" xmlns="http://www.w3.org/2000/svg">
    <!-- Main Controller Area -->
    <rect x="0.5" y="58.5" width="880" height="398" rx="48" fill="var(--shell)" stroke="var(--line)"/>
    <rect x="7" y="65" width="867" height="385" rx="42" fill="url(#lgPlate)"/>
    <rect x="7.5" y="65.5" width="866" height="384" rx="42" fill="none" stroke="url(#lgEdge)"/>


    <!-- Left Shoulder Button -->
    <path d="M113.845 5.10119L73.0001 58H264L223.155 5.10119C219.381 1.55387 216.571 0.165127 208.762 0.106473H128.238C121.51 -0.426678 118.513 1.01283 113.845 5.10119Z" fill="url(#lgKey)" stroke="var(--line)"/>
    <path d="M148.13 36.2655C148.18 33.1479 148.893 30.0766 150.22 27.2553C151.548 24.4337 153.46 21.9257 155.83 19.8993C158.2 17.873 160.975 16.374 163.968 15.5012C166.962 14.6283 170.109 14.4016 173.197 14.8367C176.284 15.2718 179.245 16.358 181.881 18.0239C184.517 19.6899 186.769 21.8982 188.487 24.5006C190.205 27.1029 191.35 30.041 191.846 33.1194C192.342 36.1981 192.179 39.3478 191.366 42.3584L190.323 46.2197L186.026 45.0593C187.917 41.5017 188.631 37.2913 187.766 33.0351C185.785 23.2934 176.282 17.0018 166.541 18.9823C166.048 19.0825 165.563 19.2034 165.089 19.3412L165.088 19.3405C165.061 19.3483 165.034 19.3578 165.007 19.3658C156.149 21.9828 150.613 30.9872 152.488 40.2077C152.497 40.2511 152.506 40.2944 152.515 40.3376L152.064 40.3297L148.065 40.2658L148.13 36.2655Z" fill="var(--text-2)"/>
    <path d="M157.202 34.8114L149.803 41.3882L143.321 35.535" stroke="var(--text-2)" stroke-width="4" stroke-linecap="round"/>
    <!-- Right Shoulder Button -->
    <path d="M687.845 6.10116L647 59H838L797.155 6.10116C793.381 2.55384 790.571 1.1651 782.762 1.10644H702.238C695.51 0.573291 692.513 2.0128 687.845 6.10116Z" fill="url(#lgKey)" stroke="var(--line)"/>
    <path d="M762.995 38.3744C763.114 35.2587 762.569 32.1533 761.396 29.2641C760.224 26.3747 758.45 23.7666 756.194 21.6147C753.937 19.4628 751.248 17.8156 748.306 16.7816C745.364 15.7477 742.234 15.3507 739.127 15.6176C736.02 15.8847 733.005 16.8086 730.283 18.3292C727.56 19.8498 725.191 21.9326 723.335 24.438C721.478 26.9433 720.176 29.815 719.513 32.862C718.851 35.9093 718.843 39.0631 719.492 42.1134L720.324 46.0256L724.677 45.1C722.982 41.4451 722.497 37.2021 723.592 32.9991C726.098 23.3791 735.928 17.6122 745.548 20.1181C746.035 20.2449 746.512 20.3919 746.979 20.5552L746.98 20.5546C747.006 20.5639 747.032 20.5747 747.059 20.5842C755.762 23.6778 760.801 32.9692 758.429 42.0744C758.418 42.1173 758.406 42.16 758.395 42.2027L758.846 42.2192L762.843 42.3723L762.995 38.3744Z" fill="var(--text-2)"/>
    <path d="M754.015 36.4305L761.046 43.3989L767.836 37.9058" stroke="var(--text-2)" stroke-width="4" stroke-linecap="round"/>
    <!-- Left Joystick -->
    <circle cx="296.5" cy="317.5" r="114.5" fill="url(#lgWell)" stroke="var(--line)"/>
    <circle cx="296" cy="317" r="97" fill="none" stroke="var(--line-strong)"/>
    <!-- Right Joystick -->
    <circle cx="565.5" cy="315.5" r="114.5" fill="url(#lgWell)" stroke="var(--line)"/>
    <circle cx="565" cy="315" r="97" fill="none" stroke="var(--line-strong)"/>
    <!-- Action Buttons bg -->
    <path d="M671 199.781C671 182.325 685.151 168.175 702.607 168.175L729.937 168.175L729.937 139.012C729.937 121.332 744.269 107 761.948 107L765.928 107C783.607 107 797.939 121.332 797.94 139.012L797.94 168.175L825.269 168.175C842.725 168.175 856.876 182.325 856.876 199.781L856.876 203.71C856.876 221.166 842.725 235.317 825.269 235.317L797.94 235.317L797.94 262.987C797.94 280.667 783.607 295 765.928 295L761.948 295C744.269 295 729.937 280.667 729.937 262.987L729.937 235.317L702.607 235.317C685.151 235.317 671 221.166 671 203.71L671 199.781Z" fill="url(#lgRecess)" stroke="var(--line)"/>
    <!-- Triangle Button -->
    <circle cx="763.407" cy="143.114" r="26.5537" fill="url(#lgKey)" stroke="var(--line)"/>
    <path d="M762.634 128.5C762.995 127.875 763.863 127.836 764.287 128.383L764.366 128.5L776.924 150.25C777.309 150.917 776.827 151.75 776.058 151.75H750.942C750.221 151.75 749.753 151.018 750.015 150.377L750.076 150.25L762.634 128.5Z" stroke="var(--ok)" stroke-width="4"/>
    <!-- Circle Button -->
    <circle cx="819.701" cy="201.532" r="26.5537" fill="url(#lgKey)" stroke="var(--line)"/>
    <circle cx="819.5" cy="201.5" r="16.5" stroke="var(--danger)" stroke-width="4"/>
    <!-- Pentagon Button -->
    <circle cx="763.407" cy="259.95" r="26.5537" fill="url(#lgKey)" stroke="var(--line)"/>
    <path d="M762.203 246.842C763.269 246.045 764.732 246.045 765.797 246.842L776.129 254.568C777.15 255.332 777.576 256.658 777.192 257.874L773.189 270.561C772.795 271.809 771.636 272.658 770.327 272.658H757.673C756.364 272.658 755.206 271.809 754.812 270.561L750.808 257.874C750.424 256.658 750.85 255.332 751.871 254.568L762.203 246.842Z" stroke="var(--info)" stroke-width="4"/>
    <!-- Square Button -->
    <circle cx="706.051" cy="200.469" r="26.5537" fill="url(#lgKey)" stroke="var(--line)"/>
    <rect x="693" y="188" width="26" height="26" rx="2" stroke="#9C86DE" stroke-width="4"/>
    <!-- Dpad bg -->
    <path d="M41.0001 166.334C41.0001 163.573 43.2387 161.334 46.0001 161.334L96.4874 161.334L96.4884 108C96.4885 105.239 98.727 103 101.488 103L155.512 103C158.273 103 160.512 105.239 160.512 108L160.512 161.334L211 161.334C213.761 161.334 216 163.573 216 166.334L216 220.358C216 223.12 213.761 225.358 211 225.358L160.512 225.358L160.512 277.269C160.512 280.03 158.273 282.269 155.512 282.269L101.487 282.269C98.7262 282.268 96.4875 280.03 96.4874 277.269L96.4874 225.358L46.0001 225.357C43.2387 225.357 41.0001 223.119 41.0001 220.357L41.0001 166.334Z" fill="url(#lgRecess)" stroke="var(--line)"/>
    <!-- Dpad Up -->
    <rect x="105.024" y="108.691" width="48.374" height="48.374" rx="6" fill="url(#lgKey)" stroke="var(--line)"/>
    <path d="M127.689 124.124C128.088 123.571 128.912 123.571 129.311 124.124L138.181 136.415C138.659 137.076 138.186 138 137.37 138H119.63C118.814 138 118.341 137.076 118.819 136.415L127.689 124.124Z" fill="var(--text-1)"/>
    <!-- Dpad Down -->
    <rect x="103.602" y="226.781" width="48.374" height="48.374" rx="6" fill="url(#lgKey)" stroke="var(--line)"/>
    <path d="M129.311 259.876C128.912 260.429 128.088 260.429 127.689 259.876L118.819 247.585C118.341 246.924 118.814 246 119.63 246L137.37 246C138.186 246 138.659 246.924 138.181 247.585L129.311 259.876Z" fill="var(--text-1)"/>
    <!-- Dpad Right -->
    <rect x="161.935" y="168.447" width="48.374" height="48.374" rx="6" fill="url(#lgKey)" stroke="var(--line)"/>
    <path d="M194.376 192.189C194.93 192.588 194.93 193.412 194.376 193.811L182.085 202.681C181.424 203.159 180.5 202.686 180.5 201.87L180.5 184.13C180.5 183.314 181.424 182.841 182.085 183.319L194.376 192.189Z" fill="var(--text-1)"/>
    <!-- Dpad Left -->
    <rect x="46.6912" y="168.447" width="48.374" height="48.374" rx="6" fill="url(#lgKey)" stroke="var(--line)"/>
    <path d="M62.1237 193.311C61.5706 192.912 61.5706 192.088 62.1237 191.689L74.4149 182.819C75.0762 182.341 76.0001 182.814 76.0001 183.63L76.0001 201.37C76.0001 202.186 75.0762 202.659 74.4149 202.181L62.1237 193.311Z" fill="var(--text-1)"/>
    <defs>
      <linearGradient id="lgPlate" x1="0" y1="65" x2="0" y2="450" gradientUnits="userSpaceOnUse">
        <stop offset="0" stop-color="var(--surface-2)"/>
        <stop offset="0.35" stop-color="var(--surface-1)"/>
      </linearGradient>
      <linearGradient id="lgEdge" x1="0" y1="65" x2="0" y2="200" gradientUnits="userSpaceOnUse">
        <stop offset="0" stop-color="var(--highlight)"/>
        <stop offset="1" stop-color="transparent"/>
      </linearGradient>
      <radialGradient id="lgWell" cx="0.5" cy="0.42" r="0.6">
        <stop offset="0" stop-color="var(--bg)"/>
        <stop offset="1" stop-color="var(--surface-2)"/>
      </radialGradient>
      <linearGradient id="lgKey" x1="0" y1="0" x2="0" y2="1">
        <stop offset="0" stop-color="var(--surface-3)"/>
        <stop offset="1" stop-color="var(--surface-2)"/>
      </linearGradient>
      <linearGradient id="lgRecess" x1="0" y1="0" x2="0" y2="1">
        <stop offset="0" stop-color="var(--bg)"/>
        <stop offset="1" stop-color="var(--surface-1)"/>
      </linearGradient>
      <radialGradient id="lp0" cx="0" cy="0" r="1" gradientUnits="userSpaceOnUse" gradientTransform="translate(390.5 282.5) rotate(1.14959) scale(598.12 1362.18)">
        <stop offset="0.225962" stop-color="#FFE605"/>
        <stop offset="1" stop-color="#FDFDF6"/>
      </radialGradient>
      <radialGradient id="lp1" cx="0" cy="0" r="1" gradientUnits="userSpaceOnUse" gradientTransform="translate(296.5 317.5) rotate(90) scale(114.5)">
        <stop offset="0.754808" stop-color="#FFFCE4"/>
        <stop offset="1" stop-color="#DEC900" stop-opacity="0.6"/>
      </radialGradient>
      <radialGradient id="lp2" cx="0" cy="0" r="1" gradientUnits="userSpaceOnUse" gradientTransform="translate(297.391 317.391) rotate(90) scale(52.3908)">
        <stop stop-color="#919191"/>
        <stop offset="1" stop-color="white"/>
      </radialGradient>
      <linearGradient id="lp3" x1="297.863" y1="277.272" x2="297.863" y2="356.566" gradientUnits="userSpaceOnUse">
        <stop offset="0.519231" stop-color="#F3F1F1"/>
        <stop offset="1" stop-color="#B8B3B3"/>
      </linearGradient>
      <radialGradient id="lp4" cx="0" cy="0" r="1" gradientUnits="userSpaceOnUse" gradientTransform="translate(565.5 315.5) rotate(90) scale(114.5)">
        <stop offset="0.754808" stop-color="#FFFCE4"/>
        <stop offset="1" stop-color="#DEC900" stop-opacity="0.6"/>
      </radialGradient>
      <radialGradient id="lp5" cx="0" cy="0" r="1" gradientUnits="userSpaceOnUse" gradientTransform="translate(566.391 315.391) rotate(90) scale(52.3908)">
        <stop stop-color="#919191"/>
        <stop offset="1" stop-color="white"/>
      </radialGradient>
      <linearGradient id="lp6" x1="566.863" y1="275.272" x2="566.863" y2="354.566" gradientUnits="userSpaceOnUse">
        <stop offset="0.519231" stop-color="#F3F1F1"/>
        <stop offset="1" stop-color="#B8B3B3"/>
      </linearGradient>
      <radialGradient id="lp7" cx="0" cy="0" r="1" gradientUnits="userSpaceOnUse" gradientTransform="translate(763.938 201) rotate(90) scale(94 92.938)">
        <stop offset="0.524038" stop-color="#CDBA01"/>
        <stop offset="1" stop-color="#B8A701"/>
      </radialGradient>
      <radialGradient id="lp8" cx="0" cy="0" r="1" gradientUnits="userSpaceOnUse" gradientTransform="translate(128.5 192.634) rotate(90) scale(89.6343 87.5)">
        <stop offset="0.524038" stop-color="#CDBA01"/>
        <stop offset="1" stop-color="#C5B301"/>
      </radialGradient>
    </defs>
  </svg>

  <!-- ════════════════════════════════════════
       INTERACTIVE OVERLAY
       ════════════════════════════════════════ -->
  <div class="overlay">

    <!-- ═══ Left Shoulder Button ═══ -->
    <button
      class="shoulder-btn shoulder-l"
      class:pressed={pressedButtons.has('shoulder-l')}
      onpointerdown={(e) => onButtonPointerDown(e, 'shoulder-l')}
      onpointerup={(e) => onButtonPointerUp(e, 'shoulder-l')}
      onpointerleave={(e) => onButtonPointerLeave(e, 'shoulder-l')}
      aria-label="L (shoulder)"
    ></button>

    <!-- ═══ Right Shoulder Button ═══ -->
    <button
      class="shoulder-btn shoulder-r"
      class:pressed={pressedButtons.has('shoulder-r')}
      onpointerdown={(e) => onButtonPointerDown(e, 'shoulder-r')}
      onpointerup={(e) => onButtonPointerUp(e, 'shoulder-r')}
      onpointerleave={(e) => onButtonPointerLeave(e, 'shoulder-r')}
      aria-label="R (shoulder)"
    ></button>

    <!-- ═══ Left Joystick ═══ -->
    <!-- svelte-ignore a11y_interactive_supports_focus a11y_role_has_required_aria_props -->
    <div
      class="joystick-zone joy-left"
      class:active={activeJoystick === 'left'}
      onpointerdown={(e) => {
        const ring = e.currentTarget.querySelector('.joy-ring');
        startJoystick(e, 'left', ring);
      }}
      role="slider"
      aria-label="Left joystick"
      aria-valuenow={Math.round((Math.abs(joystickLX) + Math.abs(joystickLY)) * 50)}
      aria-valuetext={`x:${joystickLX.toFixed(2)} y:${joystickLY.toFixed(2)}`}
    >
      <div class="joy-inner">
        <div class="joy-ring">
          <div class="deadzone-indicator" style="--dz-pct:{DEADZONE * 100}%"></div>
          <div
            class="joy-thumb"
            style="transform: translate(calc(-50% + {joystickLDx}px), calc(-50% + {joystickLDy}px))"
          ></div>
        </div>
      </div>
    </div>

    <!-- ═══ Right Joystick ═══ -->
    <!-- svelte-ignore a11y_interactive_supports_focus a11y_role_has_required_aria_props -->
    <div
      class="joystick-zone joy-right"
      class:active={activeJoystick === 'right'}
      onpointerdown={(e) => {
        const ring = e.currentTarget.querySelector('.joy-ring');
        startJoystick(e, 'right', ring);
      }}
      role="slider"
      aria-label="Right joystick"
      aria-valuenow={Math.round((Math.abs(joystickRx) + Math.abs(joystickRy)) * 50)}
      aria-valuetext={`x:${joystickRx.toFixed(2)} y:${joystickRy.toFixed(2)}`}
    >
      <div class="joy-inner">
        <div class="joy-ring">
          <div class="deadzone-indicator" style="--dz-pct:{LOOK_DEADZONE * 100}%"></div>
          <div
            class="joy-thumb"
            style="transform: translate(calc(-50% + {joystickRDx}px), calc(-50% + {joystickRDy}px))"
          ></div>
        </div>
      </div>
    </div>

    <!-- ═══ D-Pad ═══ -->
    <div class="dpad">
      <button
        class="dpad-btn"
        class:pressed={pressedButtons.has('dpad-up')}
        onpointerdown={(e) => onButtonPointerDown(e, 'dpad-up')}
        onpointerup={(e) => onButtonPointerUp(e, 'dpad-up')}
        onpointerleave={(e) => onButtonPointerLeave(e, 'dpad-up')}
        aria-label="D-Pad Up"
      ></button>
      <button
        class="dpad-btn"
        class:pressed={pressedButtons.has('dpad-left')}
        onpointerdown={(e) => onButtonPointerDown(e, 'dpad-left')}
        onpointerup={(e) => onButtonPointerUp(e, 'dpad-left')}
        onpointerleave={(e) => onButtonPointerLeave(e, 'dpad-left')}
        aria-label="D-Pad Left"
      ></button>
      <button
        class="dpad-btn"
        class:pressed={pressedButtons.has('dpad-right')}
        onpointerdown={(e) => onButtonPointerDown(e, 'dpad-right')}
        onpointerup={(e) => onButtonPointerUp(e, 'dpad-right')}
        onpointerleave={(e) => onButtonPointerLeave(e, 'dpad-right')}
        aria-label="D-Pad Right"
      ></button>
      <button
        class="dpad-btn"
        class:pressed={pressedButtons.has('dpad-down')}
        onpointerdown={(e) => onButtonPointerDown(e, 'dpad-down')}
        onpointerup={(e) => onButtonPointerUp(e, 'dpad-down')}
        onpointerleave={(e) => onButtonPointerLeave(e, 'dpad-down')}
        aria-label="D-Pad Down"
      ></button>
    </div>

    <!-- ═══ Action Buttons ═══ -->
    <div class="action-btns">
      <button
        class="action-btn"
        class:pressed={pressedButtons.has('action-top')}
        onpointerdown={(e) => onButtonPointerDown(e, 'action-top')}
        onpointerup={(e) => onButtonPointerUp(e, 'action-top')}
        onpointerleave={(e) => onButtonPointerLeave(e, 'action-top')}
        aria-label="Action Top (Triangle)"
      ></button>
      <button
        class="action-btn"
        class:pressed={pressedButtons.has('action-right')}
        onpointerdown={(e) => onButtonPointerDown(e, 'action-right')}
        onpointerup={(e) => onButtonPointerUp(e, 'action-right')}
        onpointerleave={(e) => onButtonPointerLeave(e, 'action-right')}
        aria-label="Action Right (Circle)"
      ></button>
      <button
        class="action-btn"
        class:pressed={pressedButtons.has('action-bottom')}
        onpointerdown={(e) => onButtonPointerDown(e, 'action-bottom')}
        onpointerup={(e) => onButtonPointerUp(e, 'action-bottom')}
        onpointerleave={(e) => onButtonPointerLeave(e, 'action-bottom')}
        aria-label="Action Bottom (Pentagon)"
      ></button>
      <button
        class="action-btn"
        class:pressed={pressedButtons.has('action-left')}
        onpointerdown={(e) => onButtonPointerDown(e, 'action-left')}
        onpointerup={(e) => onButtonPointerUp(e, 'action-left')}
        onpointerleave={(e) => onButtonPointerLeave(e, 'action-left')}
        aria-label="Action Left (Square)"
      ></button>
    </div>

  </div>
  </div>
</div>

<style>
  .landscape-wrapper {
    position: relative;
    flex: 1;
    width: 100%;
    display: flex;
    align-items: center;
    justify-content: center;
    overflow: hidden;
  }

  .landscape-inner {
    position: relative;
    width: var(--inner-w, 100%);
    height: var(--inner-h, auto);
    aspect-ratio: 881 / 457;
    flex-shrink: 0;
  }

  .bg-svg {
    position: absolute;
    inset: 0;
    display: block;
    pointer-events: none;
  }

  /* ════════════════════════════════════════
     INTERACTIVE OVERLAY
     ════════════════════════════════════════ */
  .overlay {
    position: absolute;
    inset: 0;
  }

  /* ════════════════════════════════════════
     SHOULDER BUTTONS
     ════════════════════════════════════════ */
  .shoulder-btn {
    position: absolute;
    border: none;
    background: transparent;
    cursor: pointer;
    padding: 0;
    transition: transform 0.08s ease;
    z-index: 10;
    border-radius: 6px;
  }
  .shoulder-btn.pressed,
  .shoulder-btn:active {
    background: var(--surface-2);
    transform: translateY(3px);
  }

  .shoulder-l {
    left: 8.3%;
    top: 0;
    width: 22%;
    height: 14%;
  }

  .shoulder-r {
    right: 5%;
    top: 0;
    width: 22%;
    height: 14%;
  }

  /* ════════════════════════════════════════
     ANALOG JOYSTICKS
     ════════════════════════════════════════ */
  .joystick-zone {
    position: absolute;
    width: 26%;
    aspect-ratio: 1;
    border-radius: 50%;
    left: 50%;
    display: flex;
    align-items: center;
    justify-content: center;
    cursor: pointer;
    touch-action: none;
    z-index: 5;
  }

  .joy-left {
    left: 20.66%;
    top: 44.4%;
  }

  .joy-right {
    left: 51.2%;
    top: 44%;
  }

  .joy-inner {
    width: 95.7%;
    aspect-ratio: 1;
    border-radius: 50%;
    background: transparent;
    display: flex;
    align-items: center;
    justify-content: center;
    position: relative;
    pointer-events: none;
  }

  .joy-inner::after {
    content: '';
    position: absolute;
    width: 102%;
    height: 102%;
    border-radius: 50%;
    background: transparent;
    top: -1%;
    left: -0.5%;
    z-index: -1;
    pointer-events: none;
  }

  .joy-ring {
    position: relative;
    width: 80%;
    aspect-ratio: 1;
    border-radius: 50%;
    background: var(--surface-2);
    border: 1px solid var(--line);
    pointer-events: none;
    overflow: hidden;
  }

  .joy-ring::before,
  .joy-ring::after {
    content: '';
    position: absolute;
    background: var(--surface-2);
    pointer-events: none;
  }
  .joy-ring::before {
    width: 1px;
    height: 100%;
    left: 50%;
    top: 0;
    transform: translateX(-50%);
  }
  .joy-ring::after {
    width: 100%;
    height: 1px;
    top: 50%;
    left: 0;
    transform: translateY(-50%);
  }

  /* ── deadzone ring ─────────────────────── */
  .deadzone-indicator {
    position: absolute;
    width: calc(var(--dz-pct, 30%) * 2);
    aspect-ratio: 1;
    border-radius: 50%;
    border: 1.5px dashed var(--line-strong);
    background: var(--surface-2);
    top: 50%;
    left: 50%;
    transform: translate(-50%, -50%);
    pointer-events: none;
    z-index: 1;
  }

  .joy-thumb {
    position: absolute;
    width: 79%;
    aspect-ratio: 1;
    border-radius: 50%;
    background: var(--surface-3);
    border: 1px solid var(--line-strong);
    box-shadow: 0 6px 16px rgba(0, 0, 0, 0.35);
    left: 50%;
    top: 50%;
    transform: translate(-50%, -50%);
    transition: box-shadow 0.1s ease;
    will-change: transform;
    pointer-events: none;
  }

  .joy-thumb::after {
    content: '';
    position: absolute;
    width: 30%;
    aspect-ratio: 1;
    border-radius: 50%;
    background: var(--text-3);
    top: 50%;
    left: 50%;
    transform: translate(-50%, -50%);
  }

  .joystick-zone.active .joy-thumb {
    border-color: var(--accent);
  }

  /* ════════════════════════════════════════
     D-PAD
     ════════════════════════════════════════ */
  .dpad {
    position: absolute;
    left: 4.7%;
    top: 22.5%;
    width: 20%;
    aspect-ratio: 175 / 180;
    display: grid;
    grid-template-columns: repeat(3, 1fr);
    grid-template-rows: repeat(3, 1fr);
    gap: 4%;
  }

  .dpad-btn {
    width: 100%;
    height: 100%;
    border: none;
    border-radius: 16%;
    cursor: pointer;
    display: flex;
    align-items: center;
    justify-content: center;
    transition: all 0.08s ease;
    background: transparent;
    color: var(--text-1);
  }

  .dpad-btn:nth-child(1) { grid-column: 2; grid-row: 1; }
  .dpad-btn:nth-child(2) { grid-column: 1; grid-row: 2; }
  .dpad-btn:nth-child(3) { grid-column: 3; grid-row: 2; }
  .dpad-btn:nth-child(4) { grid-column: 2; grid-row: 3; }

  .dpad-btn.pressed,
  .dpad-btn:active {
    background: var(--accent);
    transform: scale(0.88);
  }

  /* ════════════════════════════════════════
     ACTION BUTTONS
     ════════════════════════════════════════ */
  .action-btns {
    position: absolute;
    right: 2.8%;
    top: 23.4%;
    width: 21%;
    aspect-ratio: 1;
  }

  .action-btn {
    position: absolute;
    width: 30%;
    aspect-ratio: 1;
    border: none;
    border-radius: 50%;
    cursor: pointer;
    display: flex;
    align-items: center;
    justify-content: center;
    transition: all 0.08s ease;
    background: transparent;
    color: var(--text-1);
  }

  .action-btn:nth-child(1) { top: 0; left: 50%; transform: translateX(-50%); }
  .action-btn:nth-child(2) { top: 50%; right: 0; transform: translateY(-50%); }
  .action-btn:nth-child(3) { bottom: 0; left: 50%; transform: translateX(-50%); }
  .action-btn:nth-child(4) { top: 50%; left: 0; transform: translateY(-50%); }

  .action-btn.pressed,
  .action-btn:active {
    background: var(--surface-2);
    transform: scale(0.88);
  }
  .action-btn:nth-child(1).pressed,
  .action-btn:nth-child(1):active {
    transform: translateX(-50%) scale(0.88);
  }
  .action-btn:nth-child(2).pressed,
  .action-btn:nth-child(2):active {
    transform: translateY(-50%) scale(0.88);
  }
  .action-btn:nth-child(3).pressed,
  .action-btn:nth-child(3):active {
    transform: translateX(-50%) scale(0.88);
  }
  .action-btn:nth-child(4).pressed,
  .action-btn:nth-child(4):active {
    transform: translateY(-50%) scale(0.88);
  }
</style>
