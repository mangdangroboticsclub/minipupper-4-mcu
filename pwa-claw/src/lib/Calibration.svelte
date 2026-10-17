<script>
  let { onBack } = $props();

  // Servo index base for each leg. THESE MUST MATCH THE FIRMWARE: robot.h lays
  // the twelve servos out as FR=1-3, FL=4-6, RR=7-9, RL=10-12, so offsets[0..2]
  // are the FRONT RIGHT leg. (A swapped map once labelled every leg with its
  // neighbour's name; fix labels here, never the loom.)
  const legServoBase = { fr: 0, fl: 3, rr: 6, rl: 9 };

  // Shown front-left first, as you'd read the robot from above.
  const legs = [
    { id: "fl", label: "FL", name: "Front left" },
    { id: "fr", label: "FR", name: "Front right" },
    { id: "rl", label: "BL", name: "Back left" },
    { id: "rr", label: "BR", name: "Back right" },
  ];

  // Joint order within a leg matches the firmware: base+1 abduction, +2 hip, +3 knee.
  const calProps = [
    { id: "abd",  label: "Abduction", hint: "Swings the whole leg in/out" },
    { id: "hip",  label: "Hip",       hint: "Upper leg forward/back" },
    { id: "knee", label: "Knee",      hint: "Lower leg bend" },
  ];
  // ±90°: a quarter turn either way. Was ±50, which some servos needed more than;
  // the firmware stores any value (no clamp there).
  const MIN = -90, MAX = 90;

  const ZERO = () => ({ abd: 0, hip: 0, knee: 0 });
  let calValues = $state({ fl: ZERO(), fr: ZERO(), rl: ZERO(), rr: ZERO() });
  let selectedLeg = $state("fl");
  let selectedJoint = $state("hip");
  let loading = $state(true);
  let saveState = $state("idle"); // idle | saving | saved | error
  let saveTimer = null;

  const legMeta = $derived(legs.find((l) => l.id === selectedLeg));
  const hasAnyOffset = $derived(Object.values(calValues).some((v) => v.abd || v.hip || v.knee));
  const touched = (id) => { const v = calValues[id]; return !!(v.abd || v.hip || v.knee); };

  // ── 3D view (three.js loads only when this page opens) ──
  let canvasEl = $state(null);
  let view = null;
  let view3d = $state("loading"); // loading | ready | failed

  $effect(() => {
    if (!canvasEl) return;
    let cancelled = false;
    import("./pupper3d.js")
      .then(({ createPupperView }) => createPupperView(canvasEl, "/pupper_model.json", {
        onPickLeg: (leg) => selectLeg(leg),
      }))
      .then((v) => {
        if (cancelled) { v.dispose(); return; }
        view = v;
        view.setOffsets($state.snapshot(calValues));
        view.select(selectedLeg, selectedJoint);
        view3d = "ready";
      })
      .catch((e) => { console.warn("3D view unavailable:", e); view3d = "failed"; });
    return () => { cancelled = true; view?.dispose(); view = null; };
  });

  // Keep the model in step with the numbers.
  $effect(() => {
    const snap = $state.snapshot(calValues);
    view?.setOffsets(snap);
  });

  // ── API ──
  async function loadOffsets() {
    loading = true;
    try {
      const res = await fetch("/v1/robot/status");
      if (res.ok) {
        const data = await res.json();
        const o = data.offsets;
        if (Array.isArray(o) && o.length === 12) {
          calValues = {
            fr: { abd: o[0] || 0, hip: o[1] || 0, knee: o[2] || 0 },
            fl: { abd: o[3] || 0, hip: o[4] || 0, knee: o[5] || 0 },
            rr: { abd: o[6] || 0, hip: o[7] || 0, knee: o[8] || 0 },
            rl: { abd: o[9] || 0, hip: o[10] || 0, knee: o[11] || 0 },
          };
        }
      }
    } catch { /* keep zeros */ }
    loading = false;
  }

  function settle(state) {
    saveState = state;
    clearTimeout(saveTimer);
    saveTimer = setTimeout(() => (saveState = "idle"), 1400);
  }

  async function updateOffset(servoIndex, offset) {
    saveState = "saving";
    try {
      const res = await fetch("/v1/robot/calibrate", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ servo: servoIndex, offset }),
      });
      settle(res.ok ? "saved" : "error");
    } catch { settle("error"); }
  }

  async function resetOffsets() {
    saveState = "saving";
    try {
      const res = await fetch("/v1/robot/calibrate/reset", { method: "POST" });
      calValues = { fl: ZERO(), fr: ZERO(), rl: ZERO(), rr: ZERO() };
      settle(res.ok ? "saved" : "error");
    } catch { settle("error"); }
  }

  // ── Editing ──
  function selectLeg(id) {
    if (id === selectedLeg) return;
    selectedLeg = id;
    view?.select(id, selectedJoint, { swing: true });
  }
  function selectJoint(joint) {
    selectedJoint = joint;
    view?.select(selectedLeg, joint);
  }

  function servoOf(joint) {
    return legServoBase[selectedLeg] + calProps.findIndex((p) => p.id === joint) + 1;
  }

  function setJoint(joint, value, { commit = true } = {}) {
    const v = Math.max(MIN, Math.min(MAX, Math.round(value)));
    if (selectedJoint !== joint) selectJoint(joint);
    calValues[selectedLeg][joint] = v;
    if (commit) updateOffset(servoOf(joint), v);
  }
  const step = (joint, d) => setJoint(joint, calValues[selectedLeg][joint] + d);

  function resetLeg() {
    calValues[selectedLeg] = ZERO();
    for (const p of calProps) updateOffset(servoOf(p.id), 0);
  }

  // Slider drag: values follow the finger; the robot is told on release.
  let dragJoint = null;
  function valueAt(e) {
    const r = e.currentTarget.getBoundingClientRect();
    return MIN + ((e.clientX - r.left) / r.width) * (MAX - MIN);
  }
  function sliderDown(e, joint) {
    try { e.currentTarget.setPointerCapture(e.pointerId); } catch {}
    dragJoint = joint;
    setJoint(joint, valueAt(e), { commit: false });
  }
  function sliderMove(e, joint) {
    if (dragJoint === joint) setJoint(joint, valueAt(e), { commit: false });
  }
  function sliderUp(joint) {
    if (dragJoint !== joint) return;
    dragJoint = null;
    updateOffset(servoOf(joint), calValues[selectedLeg][joint]);
  }
  function sliderKey(e, joint) {
    const keys = { ArrowLeft: -1, ArrowDown: -1, ArrowRight: 1, ArrowUp: 1 };
    if (keys[e.key]) { e.preventDefault(); step(joint, keys[e.key]); }
    else if (e.key === "Home") { e.preventDefault(); setJoint(joint, MIN); }
    else if (e.key === "End") { e.preventDefault(); setJoint(joint, MAX); }
    else if (e.key === "0") { e.preventDefault(); setJoint(joint, 0); }
  }

  const pct = (v) => ((v - MIN) / (MAX - MIN)) * 100;
  const fmt = (v) => (v > 0 ? `+${v}°` : `${v}°`);
  const chip = $derived(
    saveState === "saving" ? "Saving" : saveState === "saved" ? "Saved" : saveState === "error" ? "Not saved" : loading ? "Reading" : "Live"
  );

  $effect(() => { loadOffsets(); return () => clearTimeout(saveTimer); });
</script>

<div class="cal">
  <header class="bar">
    <button type="button" class="round" onclick={() => onBack?.()} aria-label="Back to settings">
      <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M15 6l-6 6 6 6"/></svg>
    </button>
    <h1>Calibration</h1>
    <span class="status" data-state={saveState} role="status">{chip}</span>
  </header>

  <div class="scroll">
    <div class="inner">
      <div class="stage">
        <canvas bind:this={canvasEl} class="view" aria-label="3D Mini Pupper. Drag to turn it, tap a leg to pick it."></canvas>
        <span class="tag">{legMeta.label} · {calProps.find((p) => p.id === selectedJoint).label.toLowerCase()}</span>
        {#if view3d === "loading"}
          <span class="overlay">Loading the 3D model…</span>
        {:else if view3d === "failed"}
          <span class="overlay">The 3D view isn't available in this browser. The controls below still move the robot.</span>
        {:else}
          <span class="hint">Drag to turn</span>
        {/if}
      </div>

      <div class="legs" role="tablist" aria-label="Legs">
        {#each legs as leg (leg.id)}
          <button
            type="button"
            role="tab"
            class="leg"
            class:on={selectedLeg === leg.id}
            aria-selected={selectedLeg === leg.id}
            aria-label="{leg.name} leg"
            onclick={() => selectLeg(leg.id)}
          >
            <b>{leg.label}</b>
            <span>{leg.name}</span>
            {#if touched(leg.id)}<i class="dot" aria-hidden="true"></i>{/if}
          </button>
        {/each}
      </div>

      <section class="joints" aria-label="{legMeta.name} offsets">
        <div class="joints-head">
          <h2>{legMeta.name}</h2>
          <button type="button" class="link" onclick={resetLeg}>Reset leg</button>
        </div>
        {#each calProps as p (p.id)}
          {@const v = calValues[selectedLeg][p.id]}
          {@const on = selectedJoint === p.id}
          <div class="joint" class:on>
            <button type="button" class="joint-head" onclick={() => selectJoint(p.id)} aria-pressed={on}>
              <span class="jdot" aria-hidden="true"></span>
              <b>{p.label}</b>
              <span class="jhint">Servo {servoOf(p.id)} · {p.hint}</span>
              <span class="jval" class:zero={v === 0}>{fmt(v)}</span>
            </button>
            <div class="ctl">
              <button type="button" class="step" onclick={() => step(p.id, -1)} aria-label="Decrease {p.label}">
                <svg width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="3" stroke-linecap="round" aria-hidden="true"><path d="M5 12h14"/></svg>
              </button>
              <div
                class="slider"
                role="slider"
                tabindex="0"
                aria-label="{p.label} offset for {legMeta.name}"
                aria-valuemin={MIN}
                aria-valuemax={MAX}
                aria-valuenow={v}
                aria-valuetext={fmt(v)}
                onpointerdown={(e) => sliderDown(e, p.id)}
                onpointermove={(e) => sliderMove(e, p.id)}
                onpointerup={() => sliderUp(p.id)}
                onpointercancel={() => sliderUp(p.id)}
                onkeydown={(e) => sliderKey(e, p.id)}
              >
                <span class="track" aria-hidden="true"></span>
                <span class="mid" aria-hidden="true"></span>
                <span class="fill" style="left: {Math.min(50, pct(v))}%; width: {Math.abs(pct(v) - 50)}%" aria-hidden="true"></span>
                <span class="handle" style="left: {pct(v)}%" aria-hidden="true"></span>
              </div>
              <button type="button" class="step" onclick={() => step(p.id, 1)} aria-label="Increase {p.label}">
                <svg width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="3" stroke-linecap="round" aria-hidden="true"><path d="M12 5v14M5 12h14"/></svg>
              </button>
            </div>
          </div>
        {/each}
      </section>

      <button type="button" class="reset-all" onclick={resetOffsets} disabled={!hasAnyOffset && !loading}>Reset all offsets</button>
    </div>
  </div>
</div>

<style>
  .cal { position: absolute; inset: 0; z-index: 5; display: flex; flex-direction: column; background: var(--page); color: var(--ink); }
  .bar { flex-shrink: 0; height: calc(64px + var(--safe-top)); padding: calc(10px + var(--safe-top)) 16px 0; display: flex; align-items: center; justify-content: space-between; max-width: var(--page-max); width: 100%; margin: 0 auto; }
  .bar h1 { font-size: 18px; font-weight: 800; }
  .round { width: 44px; height: 44px; border-radius: 22px; border: none; background: var(--ground); color: var(--ink); display: grid; place-items: center; cursor: pointer; }
  .status { height: 32px; min-width: 64px; border-radius: 16px; display: grid; place-items: center; padding: 0 12px; font-size: 13px; font-weight: 800; background: var(--hair); }
  .status[data-state="saved"] { background: var(--ink); color: var(--mpx-yellow); }
  .status[data-state="error"] { background: #9B2C20; color: #fff; }

  .scroll { flex: 1; min-height: 0; overflow-y: auto; }
  .inner { max-width: var(--page-max); margin: 0 auto; padding: 10px 16px calc(20px + var(--safe-bottom)); display: flex; flex-direction: column; gap: 10px; }

  .stage { position: relative; height: clamp(210px, 32vh, 340px); border-radius: 28px; background: #fff; border: 1px solid var(--line); overflow: hidden; flex-shrink: 0; }
  .view { width: 100%; height: 100%; display: block; touch-action: none; cursor: grab; }
  .tag { position: absolute; left: 16px; top: 14px; font-size: 12px; font-weight: 800; letter-spacing: 0.1em; text-transform: uppercase; background: var(--mpx-yellow); padding: 5px 10px; border-radius: 10px; pointer-events: none; }
  .hint { position: absolute; right: 16px; bottom: 12px; font-size: 12px; color: var(--sub); pointer-events: none; }
  .overlay { position: absolute; inset: 0; display: grid; place-items: center; padding: 24px; text-align: center; font-size: 14px; color: var(--sub); pointer-events: none; }

  .legs { display: grid; grid-template-columns: repeat(4, minmax(0, 1fr)); gap: 8px; }
  .leg {
    position: relative; height: 58px; border-radius: 16px; border: 1.5px solid #E2DDD1; background: #fff; color: var(--ink);
    display: flex; flex-direction: column; align-items: center; justify-content: center; gap: 1px; cursor: pointer;
  }
  .leg b { font-size: 17px; }
  .leg span { font-size: 11px; font-weight: 600; opacity: 0.75; white-space: nowrap; }
  .leg.on { background: var(--ink); border-color: var(--ink); color: var(--mpx-yellow); }
  .dot { position: absolute; top: 7px; right: 7px; width: 7px; height: 7px; border-radius: 4px; background: #FFC400; }
  .leg.on .dot { box-shadow: 0 0 0 2px var(--ink); }

  .joints { background: #fff; border: 1px solid var(--line); border-radius: 22px; padding: 14px 14px 8px; display: flex; flex-direction: column; }
  .joints-head { display: flex; align-items: center; justify-content: space-between; padding: 0 2px 6px; }
  .joints-head h2 { font-size: 16px; font-weight: 800; }
  .link { height: 36px; border: none; background: transparent; padding: 0 8px; font-size: 14px; font-weight: 700; color: #4F4C46; text-decoration: underline; cursor: pointer; }
  .joint { display: flex; flex-direction: column; gap: 4px; padding: 8px 6px 6px; border-radius: 16px; }
  .joint.on { background: #FFFBDB; }
  .joint-head { border: none; background: transparent; padding: 0; display: flex; align-items: center; gap: 8px; width: 100%; text-align: left; cursor: pointer; color: var(--ink); }
  .jdot { width: 10px; height: 10px; border-radius: 5px; background: #D9D4C7; flex-shrink: 0; }
  .joint.on .jdot { background: #FFC400; box-shadow: 0 0 0 3px rgba(255, 196, 0, 0.3); }
  .joint-head b { font-size: 15px; }
  .jhint { flex: 1; min-width: 0; font-size: 12px; color: var(--sub); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  .jval { font-family: var(--mono); font-size: 18px; font-weight: 500; min-width: 56px; text-align: right; }
  .jval.zero { color: #8A867D; }
  .ctl { display: flex; align-items: center; gap: 12px; }
  .step { width: 56px; height: 56px; border-radius: 18px; border: none; background: var(--ground); color: var(--ink); flex-shrink: 0; display: grid; place-items: center; cursor: pointer; transition: transform 60ms; }
  .step:active { transform: scale(0.95); background: var(--mpx-yellow); }
  .slider { flex: 1; height: 56px; position: relative; cursor: pointer; touch-action: none; }
  .track { position: absolute; left: 0; right: 0; top: 25px; height: 6px; border-radius: 3px; background: var(--hair); }
  .mid { position: absolute; left: 50%; top: 18px; width: 2px; height: 20px; margin-left: -1px; background: #B5AFA2; }
  .fill { position: absolute; top: 25px; height: 6px; border-radius: 3px; background: var(--ink); }
  .handle { position: absolute; top: 14px; width: 28px; height: 28px; margin-left: -14px; border-radius: 14px; background: #fff; border: 2.5px solid var(--ink); box-sizing: border-box; box-shadow: 0 2px 6px rgba(0, 0, 0, 0.18); }
  .joint.on .handle { background: var(--mpx-yellow); }

  .reset-all { height: 48px; border-radius: 24px; border: 1.5px solid #E2B4AE; background: #fff; color: #A12A1E; font-size: 15px; font-weight: 700; cursor: pointer; }
  .reset-all:disabled { opacity: 0.5; cursor: default; }
  button:focus-visible, .slider:focus-visible { outline: 3px solid var(--ink); outline-offset: 2px; }
</style>
