<script>
  let { navigate } = $props();

  // ── State ──────────────────────────────────────────────────
  let status = $state(null);
  let activeMode = $state("none");

  // Config (loaded from robot, editable via sliders)
  let period = $state(80);
  let height = $state(70);
  let upHeight = $state(10);
  let stride = $state(10);
  let tilt = $state(10);
  let sgSpeed = $state(50);   // Stanford walk / diagonal speed (mm/s)

  // Offsets (loaded from robot)
  let offsets = $state(Array(12).fill(0));

  // Loading states
  let loading = $state(true);
  let statusError = $state("");

  // ── API helpers ────────────────────────────────────────────

  async function fetchStatus() {
    try {
      const res = await fetch("/v1/robot/status");
      if (res.ok) {
        const data = await res.json();
        activeMode = data.mode || "none";
        period = data.config?.period ?? period;
        height = data.config?.height ?? height;
        upHeight = data.config?.up_height ?? upHeight;
        stride = data.config?.stride ?? stride;
        tilt = data.config?.tilt ?? tilt;
        sgSpeed = data.config?.sg_speed ?? sgSpeed;
        if (data.offsets) offsets = data.offsets;
        statusError = "";
      } else {
        statusError = `Status fetch failed (${res.status})`;
      }
    } catch (e) {
      statusError = `Cannot reach robot: ${e.message}`;
    }
    loading = false;
  }

  async function sendGait(mode) {
    activeMode = mode;
    try {
      await fetch("/v1/robot/gait", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ mode }),
      });
    } catch (e) {
      console.error("Gait command failed:", e);
    }
  }

  async function updateConfig() {
    try {
      await fetch("/v1/robot/config", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ period, height, up_height: upHeight, stride, tilt, sg_speed: sgSpeed }),
      });
    } catch (e) {
      console.error("Config update failed:", e);
    }
  }

  async function updateOffset(servo, offset) {
    try {
      await fetch("/v1/robot/calibrate", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ servo, offset }),
      });
    } catch (e) {
      console.error("Calibrate failed:", e);
    }
  }

  // ── Parameter helpers ──────────────────────────────────────
  function adj(param, delta) {
    if (param === "period")   { period   = Math.max(30, Math.min(1000, period + delta)); }
    if (param === "height")   { height   = Math.max(30, Math.min(200, height + delta)); }
    if (param === "upHeight") { upHeight = Math.max(0, Math.min(50, upHeight + delta)); }
    if (param === "stride")   { stride   = Math.max(0, Math.min(60, stride + delta)); }
    if (param === "tilt")     { tilt     = Math.max(0, Math.min(60, tilt + delta)); }
    if (param === "sgSpeed")  { sgSpeed  = Math.max(10, Math.min(200, sgSpeed + delta)); }
    updateConfig();
  }

  // ── Joystick (mini_pupper_web_controller style) ─────────────
  // Left pad: forward/strafe.  Right pad: turn.  Values -1..1 are
  // sent to /v1/robot/joy ~10x/s while a pad is touched; touching a
  // pad auto-starts the Stanford walk, releasing steps in place.
  let joyF = 0, joyS = 0, joyT = 0;          // current stick values
  let joyKnobL = $state({ x: 0, y: 0 });     // knob positions (-1..1) for display
  let joyKnobR = $state({ x: 0 });
  let joyTimer = null;

  async function sendJoy() {
    try {
      await fetch("/v1/robot/joy", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ f: joyF, s: joyS, t: joyT }),
      });
    } catch (e) { /* transient — keep trying while pad is held */ }
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
      sendJoy();               // final zeros -> step in place
    }
  }

  // Normalised -1..1 position of the pointer inside the pad element.
  function padPos(e, el) {
    const r = el.getBoundingClientRect();
    let x = ((e.clientX - r.left) / r.width) * 2 - 1;
    let y = ((e.clientY - r.top) / r.height) * 2 - 1;
    x = Math.max(-1, Math.min(1, x));
    y = Math.max(-1, Math.min(1, y));
    return { x, y };
  }

  function padLeftMove(e) {
    if (e.buttons === 0 && e.type === "pointermove") return;
    const p = padPos(e, e.currentTarget);
    joyF = -p.y;               // pad up    = forward (+)
    joyS = -p.x;               // pad left  = strafe left (+)
    joyKnobL = { x: p.x, y: p.y };
    joyStartTimer();
    e.currentTarget.setPointerCapture?.(e.pointerId);
  }

  function padLeftEnd() {
    joyF = 0; joyS = 0;
    joyKnobL = { x: 0, y: 0 };
    joyStopIfIdle();
    if (joyTimer) sendJoy();   // right pad may still be active
  }

  function padRightMove(e) {
    if (e.buttons === 0 && e.type === "pointermove") return;
    const p = padPos(e, e.currentTarget);
    joyT = -p.x;               // pad left = turn left (+)
    joyKnobR = { x: p.x };
    joyStartTimer();
    e.currentTarget.setPointerCapture?.(e.pointerId);
  }

  function padRightEnd() {
    joyT = 0;
    joyKnobR = { x: 0 };
    joyStopIfIdle();
    if (joyTimer) sendJoy();
  }

  // ── Offset helpers ─────────────────────────────────────────
  function adjOffset(idx, delta) {
    offsets[idx] = Math.round((offsets[idx] + delta) * 10) / 10;
    updateOffset(idx + 1, offsets[idx]);
  }

  async function resetOffsets() {
    try {
      await fetch("/v1/robot/calibrate/reset", { method: "POST" });
      offsets = Array(12).fill(0);
    } catch (e) {
      console.error("Reset offsets failed:", e);
    }
  }

  // ── Diagnostics ───────────────────────────────────────────
  let diagResult = $state("");
  let diagRunning = $state(false);

  async function diagPing(servoId) {
    diagRunning = true;
    diagResult = `Pinging servo ${servoId}...`;
    try {
      const res = await fetch("/v1/robot/diagnostic/ping", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ servo: servoId }),
      });
      const data = await res.json();
      if (data.ok) {
        diagResult = `✅ Servo ${servoId} is responding`;
      } else {
        diagResult = `❌ Servo ${servoId} did NOT respond (err=${data.error})`;
      }
    } catch (e) {
      diagResult = `❌ Ping failed: ${e.message}`;
    }
    diagRunning = false;
  }

  async function diagSweep(servoId) {
    diagRunning = true;
    diagResult = `Sweeping servo ${servoId}...`;
    try {
      const res = await fetch("/v1/robot/diagnostic/sweep", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ servo: servoId }),
      });
      if (res.ok) {
        diagResult = `✅ Servo ${servoId} sweep complete`;
      } else {
        diagResult = `❌ Sweep failed`;
      }
    } catch (e) {
      diagResult = `❌ Sweep failed: ${e.message}`;
    }
    diagRunning = false;
  }

  // ── Init ───────────────────────────────────────────────────
  $effect(() => {
    fetchStatus();
    return () => {              // stop the joystick sender on unmount
      if (joyTimer) { clearInterval(joyTimer); joyTimer = null; }
    };
  });

  // ── Button config ──────────────────────────────────────────
  const gaitActions = {
    advance:  { label: "▲",  mode: "advance",  cls: "bg-blue-600 active:bg-blue-700" },
    back:     { label: "▼",  mode: "back",     cls: "bg-blue-600 active:bg-blue-700" },
    left:     { label: "◀",  mode: "left",     cls: "bg-blue-600 active:bg-blue-700" },
    right:    { label: "▶",  mode: "right",    cls: "bg-blue-600 active:bg-blue-700" },
    turnL:    { label: "↺",  mode: "turnL",    cls: "bg-cyan-700 active:bg-cyan-800" },
    turnR:    { label: "↻",  mode: "turnR",    cls: "bg-cyan-700 active:bg-cyan-800" },
    step:     { label: "Step",  mode: "step",  cls: "bg-emerald-700 active:bg-emerald-800" },
    twerk:    { label: "💃",   mode: "twerk",  cls: "bg-purple-700 active:bg-purple-800" },
    jump:     { label: "⬆ Jump", mode: "jump", cls: "bg-orange-700 active:bg-orange-800" },
    jumpfwd:  { label: "↗ Fwd",  mode: "jumpfwd", cls: "bg-amber-700 active:bg-amber-800" },
    init:     { label: "Init",   mode: "init", cls: "bg-gray-600 active:bg-gray-700" },
    roll:     { label: "Roll",   mode: "roll", cls: "bg-teal-700 active:bg-teal-800" },
    pitch:    { label: "Pitch",  mode: "pitch", cls: "bg-teal-700 active:bg-teal-800" },
    testspeed:{ label: "Test",   mode: "testspeed", cls: "bg-red-700 active:bg-red-800" },
  };
</script>

<div class="flex flex-col h-full">
  <!-- Header -->
  <header class="flex items-center gap-3 px-4 py-3 bg-mpx-surface border-b border-mpx-muted/20 shrink-0">
    <button onclick={() => navigate("home")}
            class="text-lg hover:text-mpx-orange transition-colors cursor-pointer">‹</button>
    <h2 class="font-semibold">Robot Control</h2>
    <button onclick={fetchStatus}
            class="ml-auto text-xs text-mpx-muted hover:text-mpx-text transition-colors cursor-pointer">
      ↻ Refresh
    </button>
  </header>

  <div class="flex-1 overflow-y-auto px-4 py-3 space-y-4">
    {#if loading}
      <p class="text-center text-mpx-muted text-sm mt-8">Connecting to robot…</p>
    {:else if statusError}
      <p class="text-center text-red-400 text-sm mt-8">{statusError}</p>
      <div class="text-center">
        <button onclick={fetchStatus}
                class="rounded-lg bg-mpx-orange px-6 py-2 text-sm text-white cursor-pointer">
          Retry
        </button>
      </div>
    {:else}

      <!-- ═══════════════════════════════════════════════════════
           D-Pad + Rotation Controls
           ═══════════════════════════════════════════════════════ -->
      <div class="rounded-xl bg-mpx-surface border border-mpx-muted/10 px-4 py-5">
        <p class="text-xs text-mpx-muted mb-3 uppercase tracking-wide font-semibold">Movement</p>

        <!-- D-Pad grid -->
        <div class="flex flex-col items-center gap-1.5 mb-4">
          <div class="flex gap-1.5">
            <div class="w-14"></div>
            <button onclick={() => sendGait("advance")}
                    class="w-14 h-14 rounded-xl text-xl font-bold text-white
                           {gaitActions.advance.cls}
                           {activeMode === 'advance' ? 'ring-2 ring-white/60' : ''}
                           transition-all cursor-pointer active:scale-95">
              ▲
            </button>
            <div class="w-14"></div>
          </div>
          <div class="flex gap-1.5">
            <button onclick={() => sendGait("left")}
                    class="w-14 h-14 rounded-xl text-xl font-bold text-white
                           {gaitActions.left.cls}
                           {activeMode === 'left' ? 'ring-2 ring-white/60' : ''}
                           transition-all cursor-pointer active:scale-95">
              ◀
            </button>
            <button onclick={() => sendGait("none")}
                    class="w-14 h-14 rounded-xl text-sm font-bold text-white bg-red-600
                           hover:bg-red-700 active:bg-red-800
                           transition-all cursor-pointer active:scale-95">
              ■
            </button>
            <button onclick={() => sendGait("right")}
                    class="w-14 h-14 rounded-xl text-xl font-bold text-white
                           {gaitActions.right.cls}
                           {activeMode === 'right' ? 'ring-2 ring-white/60' : ''}
                           transition-all cursor-pointer active:scale-95">
              ▶
            </button>
          </div>
          <div class="flex gap-1.5">
            <div class="w-14"></div>
            <button onclick={() => sendGait("back")}
                    class="w-14 h-14 rounded-xl text-xl font-bold text-white
                           {gaitActions.back.cls}
                           {activeMode === 'back' ? 'ring-2 ring-white/60' : ''}
                           transition-all cursor-pointer active:scale-95">
              ▼
            </button>
            <div class="w-14"></div>
          </div>
        </div>

        <!-- Stanford Walk (exact StanfordQuadruped trot gait) -->
        <div class="flex justify-center mb-3">
          <button onclick={() => sendGait("stanford")}
                  class="flex-1 max-w-60 rounded-xl py-2.5 text-sm font-bold text-white
                         bg-teal-600 hover:bg-teal-700 active:bg-teal-800
                         {activeMode === 'stanford' ? 'ring-2 ring-white/60' : ''}
                         transition-all cursor-pointer active:scale-95">
            🐕 Stanford Walk
          </button>
        </div>

        <!-- Rotation row -->
        <div class="flex justify-center gap-3">
          <button onclick={() => sendGait("turnL")}
                  class="flex-1 max-w-28 rounded-xl py-2.5 text-sm font-bold text-white
                         {gaitActions.turnL.cls}
                         {activeMode === 'turnL' ? 'ring-2 ring-white/60' : ''}
                         transition-all cursor-pointer active:scale-95">
            ↺ Turn L
          </button>
          <button onclick={() => sendGait("turnR")}
                  class="flex-1 max-w-28 rounded-xl py-2.5 text-sm font-bold text-white
                         {gaitActions.turnR.cls}
                         {activeMode === 'turnR' ? 'ring-2 ring-white/60' : ''}
                         transition-all cursor-pointer active:scale-95">
            Turn R ↻
          </button>
        </div>
      </div>

      <!-- ═══════════════════════════════════════════════════════
           Joystick (Stanford walk) — like the minipupperesp
           web controller: left pad drive/strafe, right pad turn.
           ═══════════════════════════════════════════════════════ -->
      <div class="rounded-xl bg-mpx-surface border border-mpx-muted/10 px-4 py-4">
        <p class="text-xs text-mpx-muted mb-1 uppercase tracking-wide font-semibold">Joystick</p>
        <p class="text-[11px] text-mpx-muted mb-3">
          Touch to trot (Stanford gait, {sgSpeed} mm/s max) — release to step in place
        </p>
        <div class="flex justify-center gap-4 select-none touch-none">
          <!-- Left pad: forward / strafe -->
          <div class="relative w-36 h-36 rounded-2xl bg-mpx-bg border border-mpx-muted/20
                      touch-none cursor-pointer"
               onpointerdown={padLeftMove}
               onpointermove={padLeftMove}
               onpointerup={padLeftEnd}
               onpointercancel={padLeftEnd}
               onlostpointercapture={padLeftEnd}>
            <span class="absolute top-1 left-1/2 -translate-x-1/2 text-[10px] text-mpx-muted">▲ fwd</span>
            <span class="absolute bottom-1 left-1/2 -translate-x-1/2 text-[10px] text-mpx-muted">▼ back</span>
            <span class="absolute left-1.5 top-1/2 -translate-y-1/2 text-[10px] text-mpx-muted">◀</span>
            <span class="absolute right-1.5 top-1/2 -translate-y-1/2 text-[10px] text-mpx-muted">▶</span>
            <div class="absolute w-9 h-9 rounded-full bg-teal-500/90 shadow pointer-events-none"
                 style="left: calc(50% + {joyKnobL.x * 36}px - 18px);
                        top:  calc(50% + {joyKnobL.y * 36}px - 18px);"></div>
          </div>
          <!-- Right pad: turn -->
          <div class="relative w-36 h-36 rounded-2xl bg-mpx-bg border border-mpx-muted/20
                      touch-none cursor-pointer"
               onpointerdown={padRightMove}
               onpointermove={padRightMove}
               onpointerup={padRightEnd}
               onpointercancel={padRightEnd}
               onlostpointercapture={padRightEnd}>
            <span class="absolute left-1.5 top-1/2 -translate-y-1/2 text-[10px] text-mpx-muted">↺ L</span>
            <span class="absolute right-1.5 top-1/2 -translate-y-1/2 text-[10px] text-mpx-muted">R ↻</span>
            <div class="absolute w-9 h-9 rounded-full bg-cyan-500/90 shadow pointer-events-none"
                 style="left: calc(50% + {joyKnobR.x * 36}px - 18px);
                        top:  calc(50% - 18px);"></div>
          </div>
        </div>
      </div>

      <!-- ═══════════════════════════════════════════════════════
           Special Moves
           ═══════════════════════════════════════════════════════ -->
      <div class="rounded-xl bg-mpx-surface border border-mpx-muted/10 px-4 py-4">
        <p class="text-xs text-mpx-muted mb-3 uppercase tracking-wide font-semibold">Special Moves</p>
        <div class="flex flex-wrap gap-2">
          <button onclick={() => sendGait("step")}
                  class="px-4 py-2 rounded-lg text-xs font-bold text-white
                         {gaitActions.step.cls}
                         {activeMode === 'step' ? 'ring-2 ring-white/60' : ''}
                         transition-all cursor-pointer">
            {gaitActions.step.label}
          </button>
          <button onclick={() => sendGait("twerk")}
                  class="px-4 py-2 rounded-lg text-xs font-bold text-white
                         {gaitActions.twerk.cls}
                         {activeMode === 'twerk' ? 'ring-2 ring-white/60' : ''}
                         transition-all cursor-pointer">
            {gaitActions.twerk.label}
          </button>
          <button onclick={() => sendGait("jump")}
                  class="px-4 py-2 rounded-lg text-xs font-bold text-white
                         {gaitActions.jump.cls}
                         {activeMode === 'jump' ? 'ring-2 ring-white/60' : ''}
                         transition-all cursor-pointer">
            {gaitActions.jump.label}
          </button>
          <button onclick={() => sendGait("jumpfwd")}
                  class="px-4 py-2 rounded-lg text-xs font-bold text-white
                         {gaitActions.jumpfwd.cls}
                         {activeMode === 'jumpfwd' ? 'ring-2 ring-white/60' : ''}
                         transition-all cursor-pointer">
            {gaitActions.jumpfwd.label}
          </button>
          <button onclick={() => sendGait("roll")}
                  class="px-4 py-2 rounded-lg text-xs font-bold text-white
                         {gaitActions.roll.cls}
                         {activeMode === 'roll' ? 'ring-2 ring-white/60' : ''}
                         transition-all cursor-pointer">
            {gaitActions.roll.label}
          </button>
          <button onclick={() => sendGait("pitch")}
                  class="px-4 py-2 rounded-lg text-xs font-bold text-white
                         {gaitActions.pitch.cls}
                         {activeMode === 'pitch' ? 'ring-2 ring-white/60' : ''}
                         transition-all cursor-pointer">
            {gaitActions.pitch.label}
          </button>
          <button onclick={() => sendGait("init")}
                  class="px-4 py-2 rounded-lg text-xs font-bold text-white
                         {gaitActions.init.cls}
                         {activeMode === 'init' ? 'ring-2 ring-white/60' : ''}
                         transition-all cursor-pointer">
            {gaitActions.init.label}
          </button>
          <button onclick={() => sendGait("stretch")}
                  class="px-4 py-2 rounded-lg text-xs font-bold text-white
                         bg-pink-700 hover:bg-pink-800 active:bg-pink-900
                         {activeMode === 'stretch' ? 'ring-2 ring-white/60' : ''}
                         transition-all cursor-pointer">
            Stretch
          </button>
          <button onclick={() => sendGait("testspeed")}
                  class="px-4 py-2 rounded-lg text-xs font-bold text-white
                         {gaitActions.testspeed.cls}
                         {activeMode === 'testspeed' ? 'ring-2 ring-white/60' : ''}
                         transition-all cursor-pointer">
            {gaitActions.testspeed.label}
          </button>
        </div>
      </div>

      <!-- ═══════════════════════════════════════════════════════
           Look / Head Poses (imported from StanfordQuadruped)
           ═══════════════════════════════════════════════════════ -->
      <div class="rounded-xl bg-mpx-surface border border-mpx-muted/10 px-4 py-4">
        <p class="text-xs text-mpx-muted mb-3 uppercase tracking-wide font-semibold">Look / Head</p>
        <!-- 3x3 aiming grid -->
        <div class="grid grid-cols-3 gap-1.5 max-w-44 mx-auto">
          {#each [
            { m: "lookul",   l: "↖" }, { m: "lookup",   l: "↑" }, { m: "lookur",   l: "↗" },
            { m: "lookleft", l: "←" }, { m: "init",     l: "•" }, { m: "lookright",l: "→" },
            { m: "lookll",   l: "↙" }, { m: "lookdown", l: "↓" }, { m: "looklr",   l: "↘" },
          ] as b}
            <button onclick={() => sendGait(b.m)}
                    class="h-12 rounded-lg text-lg font-bold text-white
                           bg-indigo-700 hover:bg-indigo-800 active:bg-indigo-900
                           {activeMode === b.m ? 'ring-2 ring-white/60' : ''}
                           transition-all cursor-pointer active:scale-95">
              {b.l}
            </button>
          {/each}
        </div>
      </div>

      <!-- ═══════════════════════════════════════════════════════
           Imported Moves (lifts / height / choreography)
           ═══════════════════════════════════════════════════════ -->
      <div class="rounded-xl bg-mpx-surface border border-mpx-muted/10 px-4 py-4">
        <p class="text-xs text-mpx-muted mb-3 uppercase tracking-wide font-semibold">Imported Moves</p>
        <div class="flex flex-wrap gap-2">
          {#each [
            { m: "flegR",      l: "Lift FR",   c: "bg-lime-700 hover:bg-lime-800 active:bg-lime-900" },
            { m: "flegL",      l: "Lift FL",   c: "bg-lime-700 hover:bg-lime-800 active:bg-lime-900" },
            { m: "blegR",      l: "Lift RR",   c: "bg-lime-700 hover:bg-lime-800 active:bg-lime-900" },
            { m: "blegL",      l: "Lift RL",   c: "bg-lime-700 hover:bg-lime-800 active:bg-lime-900" },
            { m: "heightup",   l: "Height ▲",  c: "bg-pink-700 hover:bg-pink-800 active:bg-pink-900" },
            { m: "heightdown", l: "Height ▼",  c: "bg-pink-700 hover:bg-pink-800 active:bg-pink-900" },
            { m: "balance",    l: "Balance",   c: "bg-teal-700 hover:bg-teal-800 active:bg-teal-900" },
            { m: "frontkick",  l: "🐴 Front Kick", c: "bg-rose-700 hover:bg-rose-800 active:bg-rose-900" },
            { m: "wiggle",     l: "🐕 Wiggle",     c: "bg-rose-700 hover:bg-rose-800 active:bg-rose-900" },
            { m: "buttshrug",  l: "Butt Shrug",   c: "bg-rose-700 hover:bg-rose-800 active:bg-rose-900" },
            { m: "wiggleL",    l: "Wiggle ◀",     c: "bg-rose-600 hover:bg-rose-700 active:bg-rose-800" },
            { m: "wiggleR",    l: "▶ Wiggle",     c: "bg-rose-600 hover:bg-rose-700 active:bg-rose-800" },
            { m: "buttshrugL", l: "Shrug ◀",      c: "bg-rose-600 hover:bg-rose-700 active:bg-rose-800" },
            { m: "buttshrugR", l: "▶ Shrug",      c: "bg-rose-600 hover:bg-rose-700 active:bg-rose-800" },
            { m: "bowback",    l: "Bow",       c: "bg-fuchsia-700 hover:bg-fuchsia-800 active:bg-fuchsia-900" },
            { m: "bodycycle",  l: "Body Circle", c: "bg-fuchsia-700 hover:bg-fuchsia-800 active:bg-fuchsia-900" },
            { m: "headellipse",l: "Head Ellipse",c: "bg-fuchsia-700 hover:bg-fuchsia-800 active:bg-fuchsia-900" },
            { m: "moveLF",     l: "◤ L-Fwd",   c: "bg-blue-600 hover:bg-blue-700 active:bg-blue-800" },
            { m: "moveRF",     l: "R-Fwd ◥",   c: "bg-blue-600 hover:bg-blue-700 active:bg-blue-800" },
            { m: "moveLB",     l: "◣ L-Back",  c: "bg-blue-600 hover:bg-blue-700 active:bg-blue-800" },
            { m: "moveRB",     l: "R-Back ◢",  c: "bg-blue-600 hover:bg-blue-700 active:bg-blue-800" },
          ] as b}
            <button onclick={() => sendGait(b.m)}
                    class="px-4 py-2 rounded-lg text-xs font-bold text-white
                           {b.c}
                           {activeMode === b.m ? 'ring-2 ring-white/60' : ''}
                           transition-all cursor-pointer">
              {b.l}
            </button>
          {/each}
        </div>
      </div>

      <!-- ═══════════════════════════════════════════════════════
           Parameter Sliders
           ═══════════════════════════════════════════════════════ -->
      <div class="rounded-xl bg-mpx-surface border border-mpx-muted/10 px-4 py-4">
        <p class="text-xs text-mpx-muted mb-3 uppercase tracking-wide font-semibold">Parameters</p>
        <div class="space-y-3">
          {#each [
            { label: "Period (ms)",  key: "period",   val: period,   min: 30,  max: 1000, step: 5 },
            { label: "Height (mm)",  key: "height",   val: height,   min: 30,  max: 200,  step: 5 },
            { label: "Lift (mm)",    key: "upHeight", val: upHeight,  min: 0,   max: 50,   step: 2 },
            { label: "Stride (mm)",  key: "stride",   val: stride,   min: 0,   max: 60,   step: 2 },
            { label: "Tilt (°)",     key: "tilt",     val: tilt,     min: 0,   max: 60,   step: 2 },
            { label: "Walk speed (mm/s)", key: "sgSpeed", val: sgSpeed, min: 10, max: 200, step: 5 },
          ] as param}
            <div>
              <div class="flex justify-between text-xs mb-1">
                <span class="text-mpx-text">{param.label}</span>
                <span class="text-mpx-muted">{param.val}</span>
              </div>
              <div class="flex items-center gap-2">
                <button onclick={() => adj(param.key, -param.step)}
                        class="w-7 h-7 rounded-lg bg-mpx-bg border border-mpx-muted/20
                               text-sm text-mpx-muted hover:text-mpx-text
                               transition-colors cursor-pointer">−</button>
                <input
                  type="range"
                  min={param.min}
                  max={param.max}
                  step={param.step}
                  value={param.val}
                  oninput={(e) => {
                    const v = parseInt(e.target.value);
                    if (param.key === "period") period = v;
                    else if (param.key === "height") height = v;
                    else if (param.key === "upHeight") upHeight = v;
                    else if (param.key === "stride") stride = v;
                    else if (param.key === "tilt") tilt = v;
                    else if (param.key === "sgSpeed") sgSpeed = v;
                  }}
                  onchange={updateConfig}
                  class="flex-1 accent-mpx-orange h-1.5 rounded-full
                         [&::-webkit-slider-thumb]:appearance-none
                         [&::-webkit-slider-thumb]:w-4
                         [&::-webkit-slider-thumb]:h-4
                         [&::-webkit-slider-thumb]:rounded-full
                         [&::-webkit-slider-thumb]:bg-mpx-orange
                         [&::-webkit-slider-thumb]:cursor-pointer"
                />
                <button onclick={() => adj(param.key, param.step)}
                        class="w-7 h-7 rounded-lg bg-mpx-bg border border-mpx-muted/20
                               text-sm text-mpx-muted hover:text-mpx-text
                               transition-colors cursor-pointer">+</button>
              </div>
            </div>
          {/each}
        </div>
      </div>

      <!-- ═══════════════════════════════════════════════════════
           Servo Calibration
           ═══════════════════════════════════════════════════════ -->
      <div class="rounded-xl bg-mpx-surface border border-mpx-muted/10 px-4 py-4">
        <div class="flex items-center justify-between mb-3">
          <p class="text-xs text-mpx-muted uppercase tracking-wide font-semibold">Servo Calibration</p>
          <button onclick={resetOffsets}
                  class="text-xs text-red-400 hover:text-red-300 cursor-pointer">
            Reset All
          </button>
        </div>

        <p class="text-xs text-mpx-muted/60 mb-3">Press <strong>Init</strong> first, then adjust offsets</p>

        <div class="grid grid-cols-2 gap-2">
          {#each [
            { leg: "Front Right", ids: [1, 2, 3], color: "bg-red-900/20 border-red-900/30" },
            { leg: "Front Left",  ids: [4, 5, 6], color: "bg-blue-900/20 border-blue-900/30" },
            { leg: "Rear Right",  ids: [7, 8, 9], color: "bg-amber-900/20 border-amber-900/30" },
            { leg: "Rear Left",   ids: [10, 11, 12], color: "bg-green-900/20 border-green-900/30" },
          ] as group}
            <div class="rounded-lg {group.color} border px-3 py-2">
              <p class="text-xs font-medium text-mpx-text mb-1.5">{group.leg}</p>
              {#each group.ids as id}
                <div class="flex items-center justify-between gap-1 py-0.5">
                  <span class="text-xs text-mpx-muted w-5">S{id}</span>
                  <button onclick={() => adjOffset(id - 1, -1)}
                          class="w-6 h-6 rounded bg-black/30 text-xs text-mpx-muted
                                 hover:text-mpx-text cursor-pointer">−</button>
                  <span class="text-xs text-mpx-text w-8 text-center font-mono">
                    {offsets[id - 1]?.toFixed(1) ?? "0.0"}
                  </span>
                  <button onclick={() => adjOffset(id - 1, 1)}
                          class="w-6 h-6 rounded bg-black/30 text-xs text-mpx-muted
                                 hover:text-mpx-text cursor-pointer">+</button>
                </div>
              {/each}
            </div>
          {/each}
        </div>
      </div>

      <!-- Status indicator -->
      <div class="text-center text-xs text-mpx-muted pb-2">
        Active mode: <span class="font-mono text-mpx-orange">{activeMode}</span>
      </div>

      <!-- ═══════════════════════════════════════════════════════
           Diagnostics
           ═══════════════════════════════════════════════════════ -->
      <details class="rounded-xl bg-mpx-surface border border-mpx-muted/10 px-4 py-3">
        <summary class="text-xs text-mpx-muted uppercase tracking-wide font-semibold cursor-pointer select-none">
          🔧 Diagnostics
        </summary>
        <div class="mt-3 space-y-3">
          <p class="text-xs text-mpx-muted/60">
            Ping a servo to verify the bus is working. Sweep moves it ±45°.
          </p>

          <div class="flex flex-wrap gap-1.5">
            {#each [1,2,3,4,5,6,7,8,9,10,11,12] as id}
              <button onclick={() => diagPing(id)}
                      disabled={diagRunning}
                      class="w-9 h-9 rounded-lg text-xs font-mono font-bold
                             bg-mpx-bg border border-mpx-muted/20
                             hover:border-mpx-orange/50
                             transition-colors cursor-pointer
                             {diagRunning ? 'opacity-40' : ''}">
                {id}
              </button>
            {/each}
          </div>

          <div class="flex gap-2">
            <button onclick={() => diagSweep(1)}
                    disabled={diagRunning}
                    class="flex-1 rounded-lg bg-amber-700 px-3 py-2 text-xs text-white
                           hover:bg-amber-800 transition-colors cursor-pointer
                           {diagRunning ? 'opacity-50' : ''}">
              Sweep Servo 1
            </button>
            <button onclick={() => diagSweep(4)}
                    disabled={diagRunning}
                    class="flex-1 rounded-lg bg-amber-700 px-3 py-2 text-xs text-white
                           hover:bg-amber-800 transition-colors cursor-pointer
                           {diagRunning ? 'opacity-50' : ''}">
              Sweep Servo 4
            </button>
            <button onclick={() => diagSweep(7)}
                    disabled={diagRunning}
                    class="flex-1 rounded-lg bg-amber-700 px-3 py-2 text-xs text-white
                           hover:bg-amber-800 transition-colors cursor-pointer
                           {diagRunning ? 'opacity-50' : ''}">
              Sweep Servo 7
            </button>
            <button onclick={() => diagSweep(10)}
                    disabled={diagRunning}
                    class="flex-1 rounded-lg bg-amber-700 px-3 py-2 text-xs text-white
                           hover:bg-amber-800 transition-colors cursor-pointer
                           {diagRunning ? 'opacity-50' : ''}">
              Sweep Servo 10
            </button>
          </div>

          {#if diagResult}
            <div class="rounded-lg bg-black/40 border border-mpx-muted/20 px-3 py-2">
              <pre class="text-xs text-green-400 font-mono whitespace-pre-wrap">{diagResult}</pre>
            </div>
          {/if}
        </div>
      </details>

    {/if}
  </div>
</div>
