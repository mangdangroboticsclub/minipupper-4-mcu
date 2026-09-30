<script>
  import { colors } from "./colors.js";

  let { onBack } = $props();

  const YELLOW = colors.mpx.primary;

  let selectedLeg = $state(null); // null | 'fl' | 'fr' | 'rl' | 'rr'
  let loading = $state(true);
  let saveState = $state("idle"); // idle | saving | saved | error
  let saveTimer = null;

  // Top-view layout: shoulder anchor on the body edge + mirroring per side.
  //   sx = -1 mirrors to the right side, sy = -1 makes the leg bend forward.
  const legs = [
    { id: "fl", label: "FL", name: "Front left",  x: 108, y: 112, sx:  1, sy: -1, lx: 26,  ly: 22  },
    { id: "fr", label: "FR", name: "Front right", x: 212, y: 112, sx: -1, sy: -1, lx: 294, ly: 22  },
    { id: "rl", label: "RL", name: "Rear left",   x: 108, y: 238, sx:  1, sy:  1, lx: 26,  ly: 338 },
    { id: "rr", label: "RR", name: "Rear right",  x: 212, y: 238, sx: -1, sy:  1, lx: 294, ly: 338 },
  ];

  // Servo index base for each leg: FL=1, FR=4, RL=7, RR=10
  const legServoBase = { fl: 0, fr: 3, rl: 6, rr: 9 };

  const ZERO = () => ({ shoulder: 0, hip: 0, knee: 0 });
  let calValues = $state({ fl: ZERO(), fr: ZERO(), rl: ZERO(), rr: ZERO() });

  const calProps = [
    { id: "shoulder", label: "Shoulder", hint: "Swings the whole leg in/out", min: -50, max: 50, step: 1 },
    { id: "hip",      label: "Hip",      hint: "Upper leg forward/back",       min: -50, max: 50, step: 1 },
    { id: "knee",     label: "Knee",     hint: "Lower leg bend",               min: -50, max: 50, step: 1 },
  ];

  let currentLegValues = $derived(selectedLeg ? calValues[selectedLeg] : null);
  let selectedLegMeta = $derived(legs.find((l) => l.id === selectedLeg) ?? null);
  let hasAnyOffset = $derived(
    Object.values(calValues).some((v) => v.shoulder !== 0 || v.hip !== 0 || v.knee !== 0),
  );

  // ── Active joint feedback: which joint is moving and which way ─
  // { leg, joint, dir: 1 | -1 } — cleared shortly after the last change.
  let activeJoint = $state(null);
  let activeTimer = null;
  function flashJoint(leg, joint, dir) {
    activeJoint = { leg, joint, dir };
    clearTimeout(activeTimer);
    activeTimer = setTimeout(() => (activeJoint = null), 700);
  }
  function isActive(leg, joint) {
    return activeJoint && activeJoint.leg === leg && activeJoint.joint === joint;
  }

  // Offset (−50..50) → visual rotation of that segment in degrees.
  const DEG_PER_UNIT = 0.45;
  function rot(leg, joint) {
    return (calValues[leg]?.[joint] ?? 0) * DEG_PER_UNIT;
  }

  // ── API: Load offsets from robot ────────────────────────────
  async function loadOffsets() {
    loading = true;
    try {
      const res = await fetch("/v1/robot/status");
      if (res.ok) {
        const data = await res.json();
        if (data.offsets && data.offsets.length === 12) {
          const o = data.offsets;
          calValues = {
            fl: { shoulder: o[0] || 0, hip: o[1]  || 0, knee: o[2]  || 0 },
            fr: { shoulder: o[3] || 0, hip: o[4]  || 0, knee: o[5]  || 0 },
            rl: { shoulder: o[6] || 0, hip: o[7]  || 0, knee: o[8]  || 0 },
            rr: { shoulder: o[9] || 0, hip: o[10] || 0, knee: o[11] || 0 },
          };
        }
      }
    } catch { /* use defaults */ }
    loading = false;
  }

  // ── API: Update a single servo offset ───────────────────────
  async function updateOffset(servoIndex, offset) {
    saveState = "saving";
    try {
      const res = await fetch("/v1/robot/calibrate", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ servo: servoIndex, offset }),
      });
      saveState = res.ok ? "saved" : "error";
    } catch {
      saveState = "error";
    }
    clearTimeout(saveTimer);
    saveTimer = setTimeout(() => (saveState = "idle"), 1400);
  }

  // ── API: Reset all offsets ──────────────────────────────────
  async function resetOffsets() {
    saveState = "saving";
    try {
      const res = await fetch("/v1/robot/calibrate/reset", { method: "POST" });
      calValues = { fl: ZERO(), fr: ZERO(), rl: ZERO(), rr: ZERO() };
      saveState = res.ok ? "saved" : "error";
    } catch {
      saveState = "error";
    }
    clearTimeout(saveTimer);
    saveTimer = setTimeout(() => (saveState = "idle"), 1400);
  }

  // ── Slider drag state ──
  let draggingProp = $state(null);
  let dragBarEl = $state(null);

  function selectLeg(id) {
    selectedLeg = selectedLeg === id ? null : id;
  }

  function resetLeg() {
    if (!selectedLeg) return;
    calValues[selectedLeg] = ZERO();
    const base = legServoBase[selectedLeg];
    for (let i = 0; i < 3; i++) updateOffset(base + i + 1, 0);
  }

  function setJoint(propId, newVal, { commit = true } = {}) {
    if (!selectedLeg) return;
    const prop = calProps.find((p) => p.id === propId);
    if (!prop) return;
    const cur = calValues[selectedLeg][propId];
    const clamped = Math.max(prop.min, Math.min(prop.max, newVal));
    if (clamped !== cur) flashJoint(selectedLeg, propId, clamped > cur ? 1 : -1);
    calValues[selectedLeg][propId] = clamped;
    if (commit) {
      const base = legServoBase[selectedLeg];
      const jointIdx = calProps.findIndex((p) => p.id === propId);
      updateOffset(base + jointIdx + 1, clamped);
    }
  }

  function adjustProp(id, delta) {
    if (!selectedLeg) return;
    setJoint(id, calValues[selectedLeg][id] + delta);
  }

  function sliderValueFromPointer(prop, clientX) {
    if (!dragBarEl) return 0;
    const rect = dragBarEl.getBoundingClientRect();
    const pct = (clientX - rect.left) / rect.width;
    const range = prop.max - prop.min;
    return Math.round(prop.min + pct * range);
  }

  function startDrag(e, propId) {
    if (!selectedLeg) return;
    const prop = calProps.find((p) => p.id === propId);
    if (!prop) return;
    const handle = e.currentTarget;
    handle.setPointerCapture(e.pointerId);
    dragBarEl = handle.closest(".cal-slider-bar");
    draggingProp = propId;
    setJoint(propId, sliderValueFromPointer(prop, e.clientX), { commit: false });
  }

  function onDragMove(e) {
    if (!draggingProp || !selectedLeg) return;
    const prop = calProps.find((p) => p.id === draggingProp);
    if (!prop) return;
    setJoint(draggingProp, sliderValueFromPointer(prop, e.clientX), { commit: false });
  }

  function endDrag() {
    if (draggingProp && selectedLeg) {
      const base = legServoBase[selectedLeg];
      const jointIdx = calProps.findIndex((p) => p.id === draggingProp);
      updateOffset(base + jointIdx + 1, calValues[selectedLeg][draggingProp]);
    }
    draggingProp = null;
    dragBarEl = null;
  }

  function onSliderKey(e, propId) {
    const prop = calProps.find((p) => p.id === propId);
    if (!prop) return;
    if (e.key === "ArrowLeft" || e.key === "ArrowDown") { e.preventDefault(); adjustProp(propId, -prop.step); }
    else if (e.key === "ArrowRight" || e.key === "ArrowUp") { e.preventDefault(); adjustProp(propId, prop.step); }
    else if (e.key === "Home") { e.preventDefault(); setJoint(propId, prop.min); }
    else if (e.key === "End") { e.preventDefault(); setJoint(propId, prop.max); }
    else if (e.key === "0") { e.preventDefault(); setJoint(propId, 0); }
  }

  function goBack() {
    if (selectedLeg) selectedLeg = null;
    else onBack?.();
  }

  $effect(() => { loadOffsets(); });
  $effect(() => () => { clearTimeout(activeTimer); clearTimeout(saveTimer); });
</script>

<div class="calibration-root" style="--yellow: {YELLOW}">
  <!-- ═══ Header ═══ -->
  <header class="cal-header">
    <button class="icon-btn" onclick={goBack} aria-label="Go back">
      <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
        <line x1="19" y1="12" x2="5" y2="12"/><polyline points="12 19 5 12 12 5"/>
      </svg>
    </button>
    <h2 class="cal-title">Calibration</h2>
    <span class="save-chip" data-state={saveState} aria-live="polite">
      {#if saveState === "saving"}<span class="mini-spin"></span>Saving
      {:else if saveState === "saved"}Saved
      {:else if saveState === "error"}Not saved
      {:else if loading}<span class="mini-spin"></span>Reading
      {:else}Live{/if}
    </span>
  </header>

  <div class="cal-body">
    <!-- ═══ Diagram ═══ -->
    <section class="cal-diagram" aria-label="Robot top view">
      <svg viewBox="0 0 320 360" class="cal-svg" class:loading role="img" aria-label="Mini Pupper seen from above">
        <defs>
          <marker id="cal-arrow" viewBox="0 0 10 10" refX="7" refY="5" markerWidth="5" markerHeight="5" orient="auto-start-reverse">
            <path d="M0 0 L10 5 L0 10 z" fill="currentColor"/>
          </marker>
        </defs>

        <!-- Body -->
        <g class="body">
          <rect x="112" y="64" width="96" height="222" rx="36" class="shell"/>
          <rect x="124" y="76" width="72" height="198" rx="28" class="panel"/>
          <!-- eyes (front) -->
          <g class="eyes">
            <rect x="142" y="94" width="13" height="26" rx="5"/>
            <rect x="165" y="94" width="13" height="26" rx="5"/>
          </g>
          <!-- front marker -->
          <path d="M152 70 L160 62 L168 70" class="front-mark"/>
          <!-- vent lines -->
          <g class="vents">
            <rect x="146" y="180" width="28" height="3" rx="1.5"/>
            <rect x="146" y="190" width="28" height="3" rx="1.5"/>
            <rect x="146" y="200" width="28" height="3" rx="1.5"/>
          </g>
        </g>

        <!-- Legs -->
        {#each legs as leg (leg.id)}
          {@const sel = selectedLeg === leg.id}
          <g
            class="leg"
            class:selected={sel}
            class:dimmed={selectedLeg && !sel}
            transform="translate({leg.x} {leg.y}) scale({leg.sx} {leg.sy})"
            role="button"
            tabindex="0"
            aria-label="Select {leg.name} leg"
            aria-pressed={sel}
            onclick={() => selectLeg(leg.id)}
            onkeydown={(e) => (e.key === "Enter" || e.key === " ") && (e.preventDefault(), selectLeg(leg.id))}
          >
            <!-- hit area -->
            <rect x="-70" y="-14" width="86" height="88" fill="transparent" />

            <!-- shoulder rotation -->
            <g class="seg-root" style="transform: rotate({rot(leg.id, 'shoulder')}deg)">
              <line x1="0" y1="0" x2="-32" y2="8" class="bone"/>

              <!-- hip rotation -->
              <g style="transform: translate(-32px, 8px) rotate({rot(leg.id, 'hip')}deg)">
                <line x1="0" y1="0" x2="-12" y2="34" class="bone"/>

                <!-- knee rotation -->
                <g style="transform: translate(-12px, 34px) rotate({rot(leg.id, 'knee')}deg)">
                  <line x1="0" y1="0" x2="12" y2="20" class="bone thin"/>
                  <circle cx="12" cy="20" r="5.5" class="foot"/>
                  <!-- knee joint -->
                  <circle cx="0" cy="0" r="7.5" class="joint" class:active={isActive(leg.id, 'knee')}/>
                  {#if isActive(leg.id, "knee")}
                    <circle cx="0" cy="0" r="7.5" class="ring"/>
                    <path d={activeJoint.dir > 0 ? "M -9 -15 A 17.5 17.5 0 0 1 15 -9" : "M 15 -9 A 17.5 17.5 0 0 0 -9 -15"} class="dir-arrow" marker-end="url(#cal-arrow)"/>
                  {/if}
                </g>

                <!-- hip joint -->
                <circle cx="0" cy="0" r="8" class="joint" class:active={isActive(leg.id, 'hip')}/>
                {#if isActive(leg.id, "hip")}
                  <circle cx="0" cy="0" r="8" class="ring"/>
                  <path d={activeJoint.dir > 0 ? "M -9 -15 A 17.5 17.5 0 0 1 15 -9" : "M 15 -9 A 17.5 17.5 0 0 0 -9 -15"} class="dir-arrow" marker-end="url(#cal-arrow)"/>
                {/if}
              </g>

              <!-- shoulder joint -->
              <circle cx="0" cy="0" r="9" class="joint shoulder" class:active={isActive(leg.id, 'shoulder')}/>
              {#if isActive(leg.id, "shoulder")}
                <circle cx="0" cy="0" r="9" class="ring"/>
                <path d={activeJoint.dir > 0 ? "M -10 -17 A 19.5 19.5 0 0 1 17 -10" : "M 17 -10 A 19.5 19.5 0 0 0 -10 -17"} class="dir-arrow" marker-end="url(#cal-arrow)"/>
              {/if}
            </g>
          </g>

          <!-- label (un-mirrored) -->
          <g class="leg-label" class:selected={sel} class:dimmed={selectedLeg && !sel} transform="translate({leg.lx} {leg.ly})" pointer-events="none">
            <rect x="-18" y="-12" width="36" height="24" rx="12"/>
            <text x="0" y="4.5" text-anchor="middle">{leg.label}</text>
          </g>
        {/each}
      </svg>

      <p class="cal-hint">
        {#if loading}
          Reading current offsets from Pupper…
        {:else if selectedLegMeta}
          Adjusting <b>{selectedLegMeta.name}</b>. Move a slider — the joint lights up and rotates the way the servo moves.
        {:else}
          Tap a leg to start. Offsets are saved to Pupper as you go.
        {/if}
      </p>
    </section>

    <!-- ═══ Controls ═══ -->
    <section class="cal-panel">
      <!-- Leg chips -->
      <div class="cal-leg-selector" role="tablist" aria-label="Legs">
        {#each legs as leg}
          <button
            class="cal-leg-btn"
            class:selected={selectedLeg === leg.id}
            class:touched={calValues[leg.id].shoulder || calValues[leg.id].hip || calValues[leg.id].knee}
            onclick={() => selectLeg(leg.id)}
            role="tab"
            aria-selected={selectedLeg === leg.id}
            aria-label="Select {leg.name} leg"
          >
            <span class="leg-btn-code">{leg.label}</span>
            <span class="leg-btn-name">{leg.name}</span>
          </button>
        {/each}
      </div>

      {#if selectedLeg && currentLegValues}
        <div class="cal-controls">
          <div class="cal-controls-header">
            <span class="cal-leg-label">{selectedLegMeta.name}</span>
            <button class="text-btn" onclick={resetLeg}>Reset leg</button>
          </div>

          {#each calProps as prop, jointIdx}
            {@const val = currentLegValues[prop.id]}
            {@const active = isActive(selectedLeg, prop.id)}
            <div class="cal-control-row" class:active>
              <div class="cal-control-row-header">
                <span class="joint-dot" class:active></span>
                <span class="cal-control-label">{prop.label}</span>
                <span class="cal-control-hint">{prop.hint}</span>
                <span class="cal-control-value" class:nonzero={val !== 0}>
                  {#if active}
                    <span class="dir-badge" data-dir={activeJoint.dir > 0 ? "up" : "down"} aria-hidden="true">{activeJoint.dir > 0 ? "▲" : "▼"}</span>
                  {/if}
                  {val > 0 ? `+${val}` : val}
                </span>
              </div>
              <div class="cal-slider-row">
                <button class="cal-step-btn" onclick={() => adjustProp(prop.id, -prop.step)} aria-label="Decrease {prop.label}">
                  <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="3" stroke-linecap="round"><line x1="5" y1="12" x2="19" y2="12"/></svg>
                </button>
                <div class="cal-slider-bar">
                  <div class="cal-slider-track"></div>
                  <div class="cal-slider-center"></div>
                  <div class="cal-slider-fill" style="left: {val < 0 ? 50 + val : 50}%; width: {Math.abs(val)}%"></div>
                  <div
                    class="cal-slider-handle"
                    class:cal-slider-dragging={draggingProp === prop.id}
                    style="left: {50 + val}%"
                    role="slider"
                    tabindex="0"
                    aria-label="{prop.label} offset for {selectedLegMeta.name}"
                    aria-valuemin={prop.min}
                    aria-valuemax={prop.max}
                    aria-valuenow={val}
                    onpointerdown={(e) => startDrag(e, prop.id)}
                    onpointermove={onDragMove}
                    onpointerup={endDrag}
                    onpointercancel={endDrag}
                    onkeydown={(e) => onSliderKey(e, prop.id)}
                  ></div>
                </div>
                <button class="cal-step-btn" onclick={() => adjustProp(prop.id, prop.step)} aria-label="Increase {prop.label}">
                  <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="3" stroke-linecap="round"><line x1="12" y1="5" x2="12" y2="19"/><line x1="5" y1="12" x2="19" y2="12"/></svg>
                </button>
              </div>
              <div class="servo-id">Servo {legServoBase[selectedLeg] + jointIdx + 1}</div>
            </div>
          {/each}
        </div>
      {:else}
        <div class="cal-empty">
          <p class="cal-empty-title">Pick a leg</p>
          <p class="cal-empty-desc">Choose from the diagram or the chips above. Each leg has three servos: shoulder, hip and knee.</p>
        </div>
      {/if}

      <div class="cal-footer">
        <button class="danger-btn" onclick={resetOffsets} disabled={!hasAnyOffset && !loading}>
          Reset all offsets
        </button>
      </div>
    </section>
  </div>
</div>

<style>
  .calibration-root {
    flex: 1;
    min-height: 0;
    background: var(--mpx-surface-2);
    display: flex;
    flex-direction: column;
  }

  /* ── Header ────────────────────────────── */
  .cal-header {
    display: flex;
    align-items: center;
    gap: 10px;
    padding: 10px 12px;
    background: var(--yellow);
    flex-shrink: 0;
  }
  .icon-btn {
    width: 34px; height: 34px;
    display: grid; place-items: center;
    border: none; background: none;
    border-radius: 50%;
    cursor: pointer;
    color: var(--mpx-ink);
  }
  .icon-btn:hover { background: rgba(0,0,0,0.08); }
  .cal-title { flex: 1; font-size: 1.02rem; font-weight: 800; color: var(--mpx-ink); text-align: center; }

  .save-chip {
    display: inline-flex; align-items: center; gap: 6px;
    height: 26px; padding: 0 10px;
    border-radius: var(--r-pill);
    background: rgba(0,0,0,0.1);
    color: var(--mpx-ink);
    font-size: 0.66rem; font-weight: 800;
    min-width: 62px; justify-content: center;
    transition: background var(--dur-fast), color var(--dur-fast);
  }
  .save-chip[data-state="saved"] { background: var(--mpx-ink); color: var(--yellow); }
  .save-chip[data-state="error"] { background: #b42318; color: #fff; }
  .mini-spin {
    width: 10px; height: 10px; border-radius: 50%;
    border: 2px solid rgba(0,0,0,0.2); border-top-color: currentColor;
    animation: spin 0.8s linear infinite;
  }
  @keyframes spin { to { transform: rotate(360deg); } }

  /* ── Layout ────────────────────────────── */
  .cal-body {
    flex: 1;
    min-height: 0;
    display: flex;
    flex-direction: column;
    overflow-y: auto;
  }
  .cal-diagram {
    flex: 0 0 auto;
    display: flex;
    flex-direction: column;
    align-items: center;
    padding: 6px 14px 0;
  }
  .cal-svg {
    /* Keep the pupper a sensible size on phones: never taller than ~38% of
       the viewport, never wider than the column. */
    width: min(100%, 380px, calc(38dvh * 320 / 360));
    height: auto;
    aspect-ratio: 320 / 360;
    transition: opacity var(--dur-med);
  }
  .cal-svg.loading { opacity: 0.55; }

  .cal-panel {
    flex-shrink: 0;
    display: flex;
    flex-direction: column;
    gap: 10px;
    padding: 6px 14px calc(14px + var(--safe-bottom));
    max-width: var(--page-max);
    width: 100%;
    margin: 0 auto;
  }

  /* Wide / landscape: diagram left, controls right */
  @media (min-width: 760px) and (min-aspect-ratio: 1/1) {
    .cal-body { flex-direction: row; align-items: stretch; overflow: hidden; }
    .cal-diagram { flex: 1 1 50%; min-width: 0; justify-content: center; padding: 14px; overflow: hidden; }
    .cal-svg { width: auto; height: calc(100% - 60px); max-width: 100%; }
    .cal-panel {
      flex: 0 0 clamp(340px, 42vw, 460px);
      max-width: none;
      margin: 0;
      overflow-y: auto;
      padding: 14px 14px calc(14px + var(--safe-bottom));
      background: var(--mpx-surface);
      border-left: 1px solid var(--mpx-line);
    }
  }

  /* ── SVG art ───────────────────────────── */
  .body .shell { fill: var(--mpx-ink); }
  .body .panel { fill: var(--yellow); }
  .body .eyes rect { fill: var(--mpx-ink); transform-box: fill-box; transform-origin: center; animation: blink 5.2s infinite; }
  .body .eyes rect:nth-child(2) { animation-delay: 0.05s; }
  @keyframes blink { 0%, 92%, 100% { transform: scaleY(1); } 95% { transform: scaleY(0.08); } }
  .body .front-mark { fill: none; stroke: var(--yellow); stroke-width: 3; stroke-linecap: round; stroke-linejoin: round; }
  .body .vents rect { fill: rgba(0,0,0,0.18); }

  .leg { cursor: pointer; outline: none; transition: opacity var(--dur-med); }
  .leg:focus-visible .joint.shoulder { stroke: var(--mpx-info); stroke-width: 3; }
  .leg.dimmed { opacity: 0.45; }
  .leg .bone { stroke: var(--mpx-muted-2); stroke-width: 10; stroke-linecap: round; transition: stroke var(--dur-med); }
  .leg .bone.thin { stroke-width: 8; }
  .leg .joint { fill: var(--mpx-muted); stroke: #fff; stroke-width: 2.5; transition: fill var(--dur-med); }
  .leg .foot { fill: var(--mpx-muted); transition: fill var(--dur-med); }
  .leg:hover .bone { stroke: #a9a9a9; }
  .leg.selected .bone { stroke: var(--mpx-ink); }
  .leg.selected .joint { fill: var(--yellow); stroke: var(--mpx-ink); }
  .leg.selected .foot { fill: var(--mpx-ink); }
  .leg .joint.active { fill: var(--mpx-ink); stroke: var(--yellow); }

  /* segments rotate around their own pivot */
  .seg-root, .seg-root g { transform-box: view-box; transition: transform 220ms var(--ease-spring); }

  .ring {
    fill: none; stroke: var(--yellow); stroke-width: 3;
    transform-box: fill-box; transform-origin: center;
    animation: ring 0.7s ease-out infinite;
    pointer-events: none;
  }
  @keyframes ring { from { transform: scale(1); opacity: 0.9; } to { transform: scale(2.4); opacity: 0; } }
  .dir-arrow {
    fill: none; stroke: var(--mpx-ink); color: var(--mpx-ink); stroke-width: 3; stroke-linecap: round;
    animation: arrow-in var(--dur-fast) var(--ease-out);
    pointer-events: none;
  }
  @keyframes arrow-in { from { opacity: 0; } to { opacity: 1; } }

  .leg-label rect { fill: var(--mpx-surface-3); transition: fill var(--dur-med); }
  .leg-label text { font-size: 12px; font-weight: 800; fill: var(--mpx-muted); font-family: inherit; transition: fill var(--dur-med); }
  .leg-label.selected rect { fill: var(--mpx-ink); }
  .leg-label.selected text { fill: var(--yellow); }
  .leg-label.dimmed { opacity: 0.5; }

  .cal-hint { font-size: 0.74rem; color: var(--mpx-muted); text-align: center; line-height: 1.45; max-width: 360px; padding: 6px 0 4px; }
  .cal-hint b { color: var(--mpx-ink); font-weight: 800; }

  /* ── Leg chips ─────────────────────────── */
  .cal-leg-selector { display: grid; grid-template-columns: repeat(4, 1fr); gap: 6px; }
  .cal-leg-btn {
    position: relative;
    display: flex; flex-direction: column; align-items: center; gap: 1px;
    padding: 8px 4px 7px;
    border-radius: var(--r-md);
    border: 1.5px solid var(--mpx-line);
    background: var(--mpx-surface);
    color: var(--mpx-ink-soft);
    cursor: pointer;
    transition: background var(--dur-fast), border-color var(--dur-fast), transform var(--dur-fast);
  }
  .cal-leg-btn:active { transform: scale(0.97); }
  .cal-leg-btn.selected { background: var(--mpx-ink); border-color: var(--mpx-ink); color: var(--yellow); }
  .cal-leg-btn.touched::after {
    content: ""; position: absolute; top: 6px; right: 6px;
    width: 6px; height: 6px; border-radius: 50%; background: var(--yellow);
  }
  .cal-leg-btn.selected.touched::after { background: var(--yellow); box-shadow: 0 0 0 2px var(--mpx-ink); }
  .leg-btn-code { font-size: 0.82rem; font-weight: 800; }
  .leg-btn-name { font-size: 0.58rem; font-weight: 600; opacity: 0.7; white-space: nowrap; }

  /* ── Controls ──────────────────────────── */
  .cal-controls {
    display: flex; flex-direction: column; gap: 8px;
    background: var(--mpx-surface);
    border-radius: var(--r-lg);
    padding: 12px 14px 10px;
    box-shadow: var(--shadow-card);
  }
  @media (min-width: 760px) and (min-aspect-ratio: 1/1) { .cal-controls { box-shadow: none; padding: 4px 0 0; } }
  .cal-controls-header { display: flex; align-items: center; justify-content: space-between; }
  .cal-leg-label { font-size: 0.8rem; font-weight: 800; color: var(--mpx-ink); }
  .text-btn {
    border: none; background: none;
    font-size: 0.72rem; font-weight: 700; color: var(--mpx-muted);
    cursor: pointer; padding: 4px 6px; border-radius: var(--r-sm);
  }
  .text-btn:hover { color: var(--mpx-ink); background: var(--mpx-surface-2); }

  .cal-control-row {
    display: flex; flex-direction: column; gap: 4px;
    padding: 8px 10px 6px;
    border-radius: var(--r-md);
    border: 1.5px solid transparent;
    transition: border-color var(--dur-fast), background var(--dur-fast);
  }
  .cal-control-row.active { border-color: var(--yellow); background: #fffef2; }
  .cal-control-row-header { display: flex; align-items: center; gap: 8px; }
  .joint-dot { width: 9px; height: 9px; border-radius: 50%; background: var(--mpx-muted-2); flex-shrink: 0; transition: background var(--dur-fast); }
  .joint-dot.active { background: var(--mpx-ink); box-shadow: 0 0 0 3px var(--yellow); }
  .cal-control-label { font-size: 0.8rem; font-weight: 800; color: var(--mpx-ink); }
  .cal-control-hint { flex: 1; font-size: 0.66rem; color: var(--mpx-muted); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  .cal-control-value {
    display: inline-flex; align-items: center; gap: 4px;
    min-width: 44px; justify-content: flex-end;
    font-size: 0.86rem; font-weight: 800; color: var(--mpx-muted);
    font-variant-numeric: tabular-nums;
  }
  .cal-control-value.nonzero { color: var(--mpx-ink); }
  .dir-badge { font-size: 0.6rem; color: var(--mpx-ink); animation: nudge 0.5s var(--ease-spring); }
  .dir-badge[data-dir="down"] { color: var(--mpx-info); }
  @keyframes nudge { from { transform: translateY(-3px); opacity: 0; } to { transform: none; opacity: 1; } }

  .cal-slider-row { display: flex; align-items: center; gap: 8px; }
  .cal-step-btn {
    width: 32px; height: 32px;
    border-radius: var(--r-sm);
    border: none;
    background: var(--mpx-surface-2);
    display: grid; place-items: center;
    cursor: pointer;
    color: var(--mpx-ink);
    flex-shrink: 0;
    transition: background var(--dur-fast), transform var(--dur-fast);
  }
  .cal-step-btn:hover { background: var(--mpx-surface-3); }
  .cal-step-btn:active { transform: scale(0.94); background: var(--yellow); }

  .cal-slider-bar { position: relative; flex: 1; height: 32px; }
  .cal-slider-track { position: absolute; left: 0; right: 0; top: 50%; height: 5px; margin-top: -2.5px; border-radius: 3px; background: var(--mpx-surface-3); }
  .cal-slider-center { position: absolute; left: 50%; top: 50%; width: 2px; height: 12px; margin: -6px 0 0 -1px; background: var(--mpx-muted-2); border-radius: 1px; }
  .cal-slider-fill { position: absolute; top: 50%; height: 5px; margin-top: -2.5px; border-radius: 3px; background: var(--mpx-ink); pointer-events: none; }
  .cal-slider-handle {
    position: absolute;
    top: 50%;
    width: 24px; height: 24px;
    border-radius: 50%;
    background: var(--yellow);
    border: 3px solid var(--mpx-ink);
    box-shadow: var(--shadow-card);
    transform: translate(-50%, -50%);
    z-index: 2;
    cursor: grab;
    touch-action: none;
    transition: box-shadow var(--dur-fast), transform var(--dur-fast);
  }
  .cal-slider-handle:focus-visible { outline: 3px solid var(--mpx-info); outline-offset: 2px; }
  .cal-slider-handle.cal-slider-dragging { cursor: grabbing; transform: translate(-50%, -50%) scale(1.15); box-shadow: var(--shadow-float); }
  .servo-id { font-size: 0.6rem; color: var(--mpx-muted-2); font-weight: 700; text-transform: uppercase; letter-spacing: 0.06em; padding-left: 40px; }

  .cal-empty {
    background: var(--mpx-surface);
    border-radius: var(--r-lg);
    padding: 18px 16px;
    text-align: center;
    box-shadow: var(--shadow-card);
  }
  .cal-empty-title { font-size: 0.92rem; font-weight: 800; color: var(--mpx-ink); }
  .cal-empty-desc { font-size: 0.76rem; color: var(--mpx-muted); line-height: 1.5; margin-top: 4px; }

  .cal-footer { display: flex; justify-content: center; padding-top: 2px; }
  .danger-btn {
    border: none;
    background: none;
    color: #b42318;
    font-size: 0.74rem; font-weight: 700;
    padding: 8px 12px;
    border-radius: var(--r-sm);
    cursor: pointer;
  }
  .danger-btn:hover:not(:disabled) { background: #fde8e8; }
  .danger-btn:disabled { color: var(--mpx-muted-2); cursor: default; }

  @media (prefers-reduced-motion: reduce) {
    .body .eyes rect, .ring { animation: none; }
  }
</style>
