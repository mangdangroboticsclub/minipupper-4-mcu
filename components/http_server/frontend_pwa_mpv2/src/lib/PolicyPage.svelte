<script>
  /**
   * RL walk — the walk policy that runs on the robot's own ESP32-S3
   * (mpv2-uros components/mpx_policy), started and steered from the phone
   * instead of the `policy` command over the USB cable.
   *
   * Talks to /v1/policy (http_server_mpx_robot_api.c). While a run is on, the
   * page sends the stick position every 250 ms; that is also the robot's
   * dead-man: when it stops arriving for 2 s (phone locked, page closed, Wi-Fi
   * gone) the robot ramps to its stand and stops by itself.
   */
  import { onDestroy } from "svelte";

  let { onBack } = $props();

  let st = $state(null);            // GET /v1/policy
  let busy = $state(false);
  let msg = $state("");
  let msgBad = $state(false);
  let test = $state(null);          // { ok, max_err, us_avg, us_max }

  // Start options
  const DURATIONS = [
    { s: 10, label: "10 s" },
    { s: 30, label: "30 s" },
    { s: 120, label: "2 min" },
    { s: 0, label: "Until Stop" },
  ];
  let seconds = $state(10);
  let dry = $state(false);
  let force = $state(false);
  // "forward": one button, a fixed forward speed. "omni": the stick.
  let mode = $state("forward");
  const FWD_SPEEDS = [
    { v: 0.05, label: "Slow" },
    { v: 0.08, label: "Normal" },
    { v: 0.12, label: "Fast" },
  ];
  let fwdSpeed = $state(0.08);
  let ownCal = $state(null);        // null until the first status: then the robot's current choice
  let wantLog = $state(true);
  // Never stays down: a walk that falls gets back up (the get-up policy) and walks on.
  let getupAuto = $state(null);     // null until the first status
  let getupRun = $state(false);     // the run on now is a "Get up" (no walking after it)

  // The log of the last run, fetched when a logged run ends.
  let logged = $state(null);        // { summary, csv }
  let logBusy = $state(false);
  let runMeta = null;               // what the logged run was asked to do

  // The stick: x, y in -1..1 (y up = forward), turn in -1..1 (left = -1)
  let sx = $state(0), sy = $state(0), turn = $state(0);
  let padEl;
  let dragging = false;

  const active = $derived(!!st?.active);
  const running = $derived(st?.phase === "run");
  const otherRobot = $derived(!!st && st.loaded && st.robot && st.robot !== st.this_robot);
  const max = $derived(st?.cmd_max || [0.1, 0.1, 0.5]);
  const omni = $derived(!!st?.omni);

  // The command (m/s, m/s, rad/s): vy and wz positive = left. Walk forward
  // holds one speed; omni follows the stick.
  const cmd = $derived(mode === "forward"
    ? [Math.min(fwdSpeed, max[0]), 0, 0]
    : [
        +(sy * max[0]).toFixed(3),
        omni ? +(-sx * max[1]).toFixed(3) : 0,
        omni ? +(-turn * max[2]).toFixed(3) : 0,
      ]);

  $effect(() => {
    if (ownCal === null && st && st.available !== false) ownCal = !!st.own_calibration;
    if (getupAuto === null && st && st.available !== false && st.getup_ready !== undefined) getupAuto = !!st.getup_auto;
  });
  $effect(() => { if (!active) getupRun = false; });
  const canGetUp = $derived(!!st?.getup_ready);

  function say(m, bad = false) { msg = m; msgBad = bad; }

  const PHASE = {
    idle: "Idle",
    prep: "Getting ready · keep it still",
    ramp_in: "Moving to the stand",
    hold_in: "Holding the stand",
    run: "Walking",
    ramp_out: "Stopping",
    hold_out: "Stopping",
    getup: "Getting up",
  };

  async function call(body) {
    const r = await fetch("/v1/policy", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(body),
    });
    const j = await r.json().catch(() => null);
    if (j && j.available !== undefined) st = j;
    return { r, j };
  }

  async function load() {
    try {
      const r = await fetch("/v1/policy", { cache: "no-store" });
      if (r.status === 404) { st = { available: false }; return; }
      st = await r.json();
    } catch {
      say("Could not reach the robot.", true);
    }
  }
  $effect(() => { load(); });

  // "Starting…" is old news once it walks or stops.
  let lastPhase = "";
  $effect(() => {
    const ph = st?.phase || "";
    if (ph !== lastPhase && (ph === "run" || (lastPhase && ph === "idle")) && !msgBad) msg = "";
    lastPhase = ph;
  });

  // Idle: refresh now and then. Active: the stick loop below keeps st fresh.
  $effect(() => {
    if (active) return;
    const t = setInterval(load, 2000);
    return () => clearInterval(t);
  });

  // While a run is on: the stick every 250 ms (and the robot's dead-man).
  let inFlight = false;
  $effect(() => {
    if (!active) return;
    const t = setInterval(async () => {
      if (inFlight) return;
      inFlight = true;
      try {
        await call({ action: "cmd", vx: cmd[0], vy: cmd[1], wz: cmd[2] });
      } catch {
        say("Lost the robot. It stops by itself within 2 s.", true);
      }
      inFlight = false;
    }, 250);
    return () => clearInterval(t);
  });

  async function start() {
    if (busy) return;
    busy = true;
    test = null;
    logged = null;
    sx = sy = turn = 0;
    runMeta = { mode, speed: fwdSpeed, seconds, dry, ownCal, name: st?.name, robot: st?.robot,
                getupAuto: canGetUp && !!getupAuto };
    try {
      const body = {
        action: "run", seconds, vx: 0, vy: 0, wz: 0, hold: 2, dry, force,
        log: wantLog, own_calibration: !!ownCal,
      };
      if (canGetUp) body.getup_auto = !!getupAuto;
      const { r, j } = await call(body);
      if (!r.ok) runMeta = null;
      if (!r.ok) say(j?.error ? `The robot said no: ${j.error}` : `The robot said no (HTTP ${r.status}).`, true);
      else say(dry ? "Dry run: no servo moves. Keep the robot still for 1 s." : "Starting. Keep the robot still for 1 s.");
    } catch {
      say("The robot did not answer.", true);
    }
    busy = false;
  }

  async function getUp() {
    if (busy) return;
    busy = true;
    test = null;
    logged = null;
    sx = sy = turn = 0;
    runMeta = { mode: "getup", seconds: 0, dry: false, ownCal, name: st?.getup_name, robot: st?.robot };
    try {
      const { r, j } = await call({ action: "getup", force, log: wantLog });
      if (!r.ok) runMeta = null;
      if (!r.ok) say(j?.error ? `The robot said no: ${j.error}` : `The robot said no (HTTP ${r.status}).`, true);
      else { getupRun = true; say("Getting up. Keep clear of the legs."); }
    } catch {
      say("The robot did not answer.", true);
    }
    busy = false;
  }

  async function stop() {
    sx = sy = turn = 0;
    try {
      await call({ action: "stop" });
      say("Stopping: it ramps to the stand, holds, then hands back to the gait.");
    } catch {
      say("The robot did not answer. It stops by itself within 2 s.", true);
    }
  }

  async function runTest() {
    busy = true;
    test = null;
    try {
      const r = await fetch("/v1/policy", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ action: "test" }),
      });
      const j = await r.json().catch(() => null);
      if (r.ok && j) test = j;
      else say(j?.error || "Could not run the self-test.", true);
    } catch {
      say("The robot did not answer.", true);
    }
    busy = false;
  }

  // ── The run log ──
  let wasActive = false;
  $effect(() => {
    const a = active;
    if (wasActive && !a && runMeta && wantLog) fetchLog();
    wasActive = a;
  });

  async function fetchLog() {
    logBusy = true;
    try {
      const r = await fetch("/v1/policy/log", { cache: "no-store" });
      if (!r.ok) throw new Error();
      const csv = await r.text();
      logged = { csv, summary: summarize(csv, runMeta, st) };
    } catch {
      say("Could not read the run log.", true);
    }
    logBusy = false;
  }

  function stats(v) {
    if (!v.length) return { mean: NaN, max: NaN, p99: NaN, min: NaN };
    const s = [...v].sort((a, b) => a - b);
    return {
      mean: v.reduce((a, b) => a + b, 0) / v.length,
      max: s[s.length - 1],
      min: s[0],
      p99: s[Math.min(s.length - 1, Math.floor(s.length * 0.99))],
    };
  }

  function summarize(csv, meta, status) {
    const lines = csv.trim().split(/\r?\n/);
    const head = lines.shift().split(",");
    const col = (n) => head.indexOf(n);
    const rows = lines.map((l) => l.split(","));
    const num = (r, n) => parseFloat(r[col(n)]);
    const walking = rows.filter((r) => r[col("phase")] === "run");
    const gettingUp = rows.filter((r) => r[col("phase")] === "getup");
    const holding = rows.filter((r) => r[col("phase")] === "hold_in");
    const pick = (rs, n, skipFirst = false) => rs.slice(skipFirst ? 1 : 0).map((r) => num(r, n)).filter((x) => Number.isFinite(x));
    const ms = (x) => (Number.isFinite(x) ? (x / 1000).toFixed(1) : "—");
    const d1 = (x) => (Number.isFinite(x) ? x.toFixed(1) : "—");
    const dt = stats(pick(walking, "dt_us", true));
    const tick = stats(pick(rows, "tick_us"));
    const net = stats(pick(walking, "network_us"));
    const ex = stats(pick(rows, "exchange_us"));
    const late = rows.filter((r) => r[col("late")] === "1").length;
    const lateWalk = walking.filter((r) => r[col("late")] === "1").length;
    const legs = stats(pick(walking, "fresh_legs"));
    const trkW = stats(pick(walking, "track_mean_deg"));
    const trkWmax = stats(pick(walking, "track_max_deg"));
    const trkH = stats(pick(holding, "track_mean_deg"));
    const tilt = stats(pick(walking, "tilt_deg"));
    const temp = stats(pick(rows, "temp_c"));
    const cur = stats(pick(rows, "current_ma"));
    const vx = stats(pick(walking, "vx")), vy = stats(pick(walking, "vy")), wz = stats(pick(walking, "wz"));
    const what = meta?.mode === "getup" ? "get up" : meta?.mode === "forward" ? `walk forward ${meta.speed} m/s` : "omni (stick)";
    const out = [
      `RL walk log · ${meta?.name || status?.name} (for ${meta?.robot || status?.robot}) · ${meta?.ownCal ? "own calibration" : "policy's calibration"} · ${what} · ${meta?.seconds ? meta.seconds + " s" : "until Stop"}${meta?.dry ? " · DRY" : ""}`,
      `ended: ${status?.last_stop || "?"}`,
      `steps logged ${rows.length} (walking ${walking.length})${status?.log_dropped ? `, ${status.log_dropped} not logged` : ""}; steps with a late tick: ${late} (walking ${lateWalk})`,
      `step period walking: mean ${ms(dt.mean)} ms, p99 ${ms(dt.p99)}, max ${ms(dt.max)}   [20 ms]`,
      `tick: mean ${ms(tick.mean)} ms, p99 ${ms(tick.p99)}, max ${ms(tick.max)}   [a tick is 5 ms]`,
      `network: mean ${ms(net.mean)} ms, max ${ms(net.max)} · servo exchange: mean ${ms(ex.mean)} ms, max ${ms(ex.max)}`,
      meta?.dry ? "servos: dry run, not used" :
      `legs answering while walking: min ${Number.isFinite(legs.min) ? legs.min : "—"}/4 · tracking |target - measured| walking: mean ${d1(trkW.mean)}°, p99 ${d1(trkWmax.p99)}°, max ${d1(trkWmax.max)}° · standing: mean ${d1(trkH.mean)}°`,
      `tilt walking: mean ${d1(tilt.mean)}°, max ${d1(tilt.max)}° · hottest servo ${d1(temp.max)} °C · max current ${Number.isFinite(cur.max) ? cur.max : "—"} mA`,
      `command walking: vx ${vx.mean?.toFixed?.(3) ?? "—"} (max ${vx.max ?? "—"}), vy max ${vy.max ?? "—"}, wz max ${wz.max ?? "—"}`,
    ];
    if (gettingUp.length || meta?.getupAuto || meta?.mode === "getup") {
      const gcur = stats(pick(gettingUp, "current_ma"));
      out.push(`getting up: ${status?.getups ?? 0} time(s), stood ${status?.getups_ok ?? 0} · ${(gettingUp.length * 0.02).toFixed(1)} s in all · max current ${Number.isFinite(gcur.max) ? gcur.max : "—"} mA${meta?.getupAuto ? " · never stays down ON" : ""}`);
    }
    return out.join("\n");
  }

  // The robot's page is plain http, where navigator.clipboard does not exist:
  // fall back to the old select-and-copy.
  async function copy(text, what) {
    try {
      if (navigator.clipboard && window.isSecureContext) {
        await navigator.clipboard.writeText(text);
      } else {
        const ta = document.createElement("textarea");
        ta.value = text;
        ta.setAttribute("readonly", "");
        ta.style.position = "fixed";
        ta.style.opacity = "0";
        document.body.appendChild(ta);
        ta.select();
        ta.setSelectionRange(0, text.length);
        const ok = document.execCommand("copy");
        document.body.removeChild(ta);
        if (!ok) throw new Error();
      }
      say(`${what} copied.`);
    } catch {
      say("Could not copy here: select the text and copy it by hand.", true);
    }
  }

  function download() {
    const blob = new Blob([logged.csv], { type: "text/csv" });
    const a = document.createElement("a");
    a.href = URL.createObjectURL(blob);
    a.download = `rl_walk_log_${new Date().toISOString().slice(0, 19).replace(/[:T]/g, "-")}.csv`;
    a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 2000);
  }

  // Leaving the page stops the run (the dead-man would anyway).
  onDestroy(() => {
    if (active) fetch("/v1/policy", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ action: "stop" }) }).catch(() => {});
  });

  // ── The stick ──
  function setFromPointer(e) {
    const r = padEl.getBoundingClientRect();
    let x = ((e.clientX - r.left) / r.width) * 2 - 1;
    let y = -(((e.clientY - r.top) / r.height) * 2 - 1);
    const n = Math.hypot(x, y);
    if (n > 1) { x /= n; y /= n; }
    sx = Math.abs(x) < 0.08 ? 0 : x;
    sy = Math.abs(y) < 0.08 ? 0 : y;
  }
  function padDown(e) {
    if (!running) return;
    dragging = true;
    padEl.setPointerCapture(e.pointerId);
    setFromPointer(e);
  }
  function padMove(e) { if (dragging) setFromPointer(e); }
  function padUp() { dragging = false; sx = 0; sy = 0; }
  function turnUp() { turn = 0; }

  const fmt = (v, d = 2) => (v === null || v === undefined || Number.isNaN(v) ? "—" : Number(v).toFixed(d));
</script>

<div class="page">
  <div class="inner">
    <div class="top">
      <button type="button" class="round" onclick={onBack} aria-label="Back to settings">
        <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M15 6l-6 6 6 6"/></svg>
      </button>
    </div>
    <h1>RL walk</h1>

    {#if st && st.available === false}
      <p class="msg bad">This firmware has no RL policy. It needs mpv2-uros.</p>
    {:else if st}
      <section>
        <div class="group">
          <div class="row">
            <span class="dot" class:on={active} class:walk={running}></span>
            <span class="txt">
              <span class="t">{PHASE[st.phase] || st.state}{st.dry && active ? " · dry" : ""}</span>
              <span class="d">
                {getupRun ? `${st.getup_name} · the get-up policy` : st.loaded ? `${st.name} · for ${st.robot}` : "No policy loaded"}
                {st.own_calibration ? " · your calibration" : " · the policy's calibration"}
              </span>
            </span>
          </div>
          {#if active}
            <div class="row stats">
              <span>Tilt <b>{fmt(st.tilt_deg, 1)}°</b></span>
              <span>Hottest servo <b>{fmt(st.temp_max_c, 0)} °C</b></span>
              <span>Steps <b>{st.steps}</b></span>
              {#if st.getups}<span>Got up <b>{st.getups_ok}</b> of <b>{st.getups}</b></span>{/if}
            </div>
          {:else if st.steps || st.last_stop}
            <div class="row stats">
              <span>Last run <b>{st.steps}</b> steps, <b>{st.late_steps}</b> late</span>
              <span>Slowest tick <b>{st.tick_us_max}</b> µs</span>
              {#if st.last_stop}<span class="wide">Ended: {st.last_stop}</span>{/if}
            </div>
          {/if}
        </div>
      </section>

      {#if logBusy}
        <p class="msg">Reading the run log…</p>
      {:else if logged && !active}
        <section>
          <h2>Run log</h2>
          <pre class="log">{logged.summary}</pre>
          <div class="logbtns">
            <button type="button" class="act" onclick={() => copy(logged.summary, "Summary")}>Copy summary</button>
            <button type="button" class="act ghost" onclick={download}>Download full log (CSV)</button>
          </div>
          <p class="note">Send the summary (or the CSV file) to check the timing, the servo tracking and the tilt.</p>
        </section>
      {/if}

      {#if active && getupRun}
        <section>
          <h2>{st.phase === "getup" ? "Getting up" : "Wait"}</h2>
          <div class="drive">
            <p class="big">{st.phase === "getup" ? "Rolling back onto its feet…" : PHASE[st.phase] || st.state}</p>
          </div>
        </section>
        <button type="button" class="stop" onclick={stop}>Stop</button>
      {:else if active && mode === "forward"}
        <section>
          <h2>{running ? "Walking forward" : st.phase === "getup" ? "It fell: getting up" : "Wait for it to stand"}</h2>
          <div class="drive" class:off={!running}>
            <p class="big">{running ? `${cmd[0]} m/s forward` : st.phase === "getup" ? "Getting up…" : "Standing up…"}</p>
            <div class="seg three" role="radiogroup" aria-label="Speed">
              {#each FWD_SPEEDS as f}
                <button type="button" role="radio" aria-checked={fwdSpeed === f.v} class:on={fwdSpeed === f.v} onclick={() => (fwdSpeed = f.v)}>{f.label}</button>
              {/each}
            </div>
          </div>
        </section>
        <button type="button" class="stop" onclick={stop}>Stop</button>
      {:else if active}
        <section>
          <h2>{running ? "Drive" : st.phase === "getup" ? "It fell: getting up" : "Wait for it to stand"}</h2>
          <div class="drive" class:off={!running}>
            <div class="pad" bind:this={padEl} role="application" aria-label="Walk direction: drag"
                 onpointerdown={padDown} onpointermove={padMove} onpointerup={padUp} onpointercancel={padUp}>
              <span class="ring"></span>
              <span class="knob" style="transform: translate({sx * 70}px, {-sy * 70}px)"></span>
              <span class="lbl f">Forward</span>
              {#if omni}<span class="lbl l">Left</span><span class="lbl r">Right</span>{/if}
            </div>
            {#if omni}
              <label class="turn">
                <span>Turn {turn < 0 ? "left" : turn > 0 ? "right" : ""}</span>
                <input type="range" min="-1" max="1" step="0.05" bind:value={turn} disabled={!running}
                       onpointerup={turnUp} onpointercancel={turnUp} ontouchend={turnUp} aria-label="Turn" />
              </label>
            {/if}
            <p class="cmd mono">vx {fmt(cmd[0], 3)} · vy {fmt(cmd[1], 3)} m/s · wz {fmt(cmd[2], 2)} rad/s</p>
          </div>
        </section>
        <button type="button" class="stop" onclick={stop}>Stop</button>
      {:else}
        <section>
          <h2>Mode</h2>
          <div class="seg two" role="radiogroup" aria-label="Mode">
            <button type="button" role="radio" aria-checked={mode === "forward"} class:on={mode === "forward"} onclick={() => (mode = "forward")}>Walk forward</button>
            <button type="button" role="radio" aria-checked={mode === "omni"} class:on={mode === "omni"} onclick={() => (mode = "omni")}>Omni (stick)</button>
          </div>
          {#if mode === "forward"}
            <div class="seg three" role="radiogroup" aria-label="Forward speed">
              {#each FWD_SPEEDS as f}
                <button type="button" role="radio" aria-checked={fwdSpeed === f.v} class:on={fwdSpeed === f.v} onclick={() => (fwdSpeed = f.v)}>{f.label} · {f.v}</button>
              {/each}
            </div>
          {/if}
        </section>

        <section>
          <h2>Run for</h2>
          <div class="seg" role="radiogroup" aria-label="Run for">
            {#each DURATIONS as d}
              <button type="button" role="radio" aria-checked={seconds === d.s} class:on={seconds === d.s} onclick={() => (seconds = d.s)}>{d.label}</button>
            {/each}
          </div>
        </section>

        <section>
          <div class="group">
            <div class="row">
              <span class="txt">
                <span class="t">Dry run</span>
                <span class="d">No servo moves: checks the IMU, the network and the timing</span>
              </span>
              <button type="button" class="switch" class:on={dry} role="switch" aria-checked={dry} aria-label="Dry run" onclick={() => (dry = !dry)}><span class="knob2"></span></button>
            </div>
            <div class="row">
              <span class="txt">
                <span class="t">Use my calibration</span>
                <span class="d">{ownCal ? "Your robot's own offsets (Settings → Calibration)" : "The offsets saved in the policy (the robot it was trained on)"}</span>
              </span>
              <button type="button" class="switch" class:on={ownCal} role="switch" aria-checked={!!ownCal} aria-label="Use my calibration" onclick={() => (ownCal = !ownCal)}><span class="knob2"></span></button>
            </div>
            <div class="row">
              <span class="txt">
                <span class="t">Record a log</span>
                <span class="d">Every step's timing, servo tracking and tilt, shown when the run ends</span>
              </span>
              <button type="button" class="switch" class:on={wantLog} role="switch" aria-checked={wantLog} aria-label="Record a log" onclick={() => (wantLog = !wantLog)}><span class="knob2"></span></button>
            </div>
            {#if canGetUp}
              <div class="row">
                <span class="txt">
                  <span class="t">Never stays down</span>
                  <span class="d">If it falls, it gets back up and walks on (up to 3 times). Off: it goes limp.</span>
                </span>
                <button type="button" class="switch" class:on={getupAuto} role="switch" aria-checked={!!getupAuto} aria-label="Never stays down" onclick={() => (getupAuto = !getupAuto)}><span class="knob2"></span></button>
              </div>
            {/if}
            {#if otherRobot}
              <div class="row">
                <span class="txt">
                  <span class="t">Run a policy for {st.robot}</span>
                  <span class="d">It was trained for another robot. On a stand only.</span>
                </span>
                <button type="button" class="switch" class:on={force} role="switch" aria-checked={force} aria-label="Allow a policy for another robot" onclick={() => (force = !force)}><span class="knob2"></span></button>
              </div>
            {/if}
          </div>
        </section>

        <button type="button" class="go" disabled={busy || !st.loaded} onclick={start}>{mode === "forward" ? `Start · walk forward ${fwdSpeed} m/s` : "Start"}</button>
        <p class="note">First runs: robot on a stand, feet off the ground. It stands up first (about 3 s), then walks where you drag. It stops by itself on a big tilt, a hot servo, or if this page stops talking to it for 2 s.</p>

        {#if canGetUp}
          <section>
            <h2>Fallen over?</h2>
            <button type="button" class="go ghost" disabled={busy} onclick={getUp}>Get up</button>
            <p class="note">It rolls back onto its feet from its back or a side, then stands. Clear space around it; it stops after 10 s if it cannot.</p>
          </section>
        {/if}

        <section>
          <div class="group">
            <button type="button" class="row btn" disabled={busy} onclick={runTest}>
              <span class="txt">
                <span class="t">Self-test</span>
                <span class="d">
                  {#if test}
                    {test.ok ? "OK" : "FAILED"} · error {Number(test.max_err).toExponential(1)} · {test.us_avg} µs per step (worst {test.us_max})
                  {:else}
                    The robot's network against training's own answers
                  {/if}
                </span>
              </span>
            </button>
          </div>
        </section>
      {/if}

      {#if msg}<p class="msg" class:bad={msgBad} role="status">{msg}</p>{/if}

    {:else}
      <p class="msg">Loading…</p>
    {/if}
  </div>
</div>

<style>
  .page { position: absolute; inset: 0; overflow-y: auto; background: var(--page); }
  .inner { max-width: var(--page-max); margin: 0 auto; padding: calc(20px + var(--safe-top)) 16px calc(28px + var(--safe-bottom)); display: flex; flex-direction: column; gap: 18px; }
  .top { display: flex; }
  .round { width: 44px; height: 44px; border-radius: 22px; border: none; background: var(--ground); color: var(--ink); display: grid; place-items: center; cursor: pointer; }
  h1 { margin: 0 4px; font-size: 34px; font-weight: 800; letter-spacing: -0.02em; }
  section { display: flex; flex-direction: column; gap: 8px; }
  h2 { margin: 0 6px; font-size: 12px; font-weight: 800; letter-spacing: 0.12em; text-transform: uppercase; color: var(--sub); }

  .group { background: #fff; border: 1px solid var(--line); border-radius: 20px; overflow: hidden; }
  .group > * + * { border-top: 1px solid var(--hair-2); }
  .row { display: flex; align-items: center; gap: 14px; padding: 14px 16px; width: 100%; box-sizing: border-box; }
  .row.btn { border: none; background: transparent; font: inherit; color: var(--ink); cursor: pointer; text-align: left; }
  .row.btn:disabled { opacity: 0.6; }
  .row.stats { flex-wrap: wrap; gap: 6px 16px; font-size: 13px; color: var(--sub); }
  .row.stats b { color: var(--ink); }
  .wide { flex-basis: 100%; }
  .txt { flex: 1; min-width: 0; display: flex; flex-direction: column; gap: 2px; text-align: left; }
  .t { font-size: 16px; font-weight: 700; color: var(--ink); }
  .d { font-size: 13px; color: var(--sub); }
  .dot { width: 14px; height: 14px; border-radius: 7px; background: #C9C5BB; flex-shrink: 0; }
  .dot.on { background: var(--mpx-yellow); box-shadow: 0 0 0 4px #FFF4B0; }
  .dot.walk { background: #1E7B34; box-shadow: 0 0 0 4px #CDEBD4; }

  .seg { display: grid; grid-template-columns: repeat(4, minmax(0, 1fr)); gap: 4px; background: var(--hair); border-radius: 14px; padding: 4px; }
  .seg button { height: 40px; border: none; border-radius: 10px; background: transparent; color: #4F4C46; font-size: 13px; font-weight: 700; cursor: pointer; }
  .seg button.on { background: #fff; color: var(--ink); box-shadow: 0 1px 3px rgba(0, 0, 0, 0.12); }

  .switch { width: 52px; height: 32px; border-radius: 16px; border: none; background: #E6E2D8; padding: 3px; display: flex; justify-content: flex-start; cursor: pointer; flex-shrink: 0; }
  .switch.on { background: var(--ink); justify-content: flex-end; }
  .knob2 { width: 26px; height: 26px; border-radius: 13px; background: #fff; box-shadow: 0 1px 3px rgba(0,0,0,0.2); }
  .switch.on .knob2 { background: var(--mpx-yellow); }

  .go, .stop { height: 56px; border-radius: 28px; border: none; font-size: 17px; font-weight: 800; cursor: pointer; }
  .go { background: var(--ink); color: #fff; }
  .go:disabled { opacity: 0.5; }
  .go.ghost { background: var(--hair); color: var(--ink); width: 100%; }
  .stop { background: #B3261E; color: #fff; position: sticky; bottom: calc(12px + var(--safe-bottom)); box-shadow: 0 6px 18px rgba(179, 38, 30, 0.35); }

  .drive { display: flex; flex-direction: column; align-items: center; gap: 14px; background: #fff; border: 1px solid var(--line); border-radius: 20px; padding: 18px 16px; }
  .drive.off { opacity: 0.5; }
  .pad { position: relative; width: 200px; height: 200px; border-radius: 100px; background: var(--hair); touch-action: none; user-select: none; display: grid; place-items: center; }
  .pad:focus, .pad:focus-visible { outline: none; }
  .ring { position: absolute; inset: 30px; border-radius: 50%; border: 2px dashed #D9D5CB; }
  .knob { width: 60px; height: 60px; border-radius: 30px; background: var(--ink); box-shadow: 0 4px 10px rgba(0,0,0,0.25); transition: transform 60ms linear; }
  .lbl { position: absolute; font-size: 11px; font-weight: 700; color: var(--sub); }
  .lbl.f { top: 8px; }
  .lbl.l { left: 10px; }
  .lbl.r { right: 10px; }
  .turn { width: 100%; display: flex; flex-direction: column; gap: 6px; font-size: 13px; font-weight: 700; color: var(--sub); }
  .turn input { width: 100%; accent-color: var(--ink); }
  .cmd { margin: 0; font-size: 12px; color: var(--sub); }
  .mono { font-family: var(--mono); }

  .seg.two { grid-template-columns: repeat(2, minmax(0, 1fr)); }
  .seg.three { grid-template-columns: repeat(3, minmax(0, 1fr)); width: 100%; box-sizing: border-box; }
  .big { margin: 0; font-size: 22px; font-weight: 800; color: var(--ink); }
  .log { margin: 0; padding: 14px; border-radius: 16px; background: #1E1D1A; color: #F2EFE6; font-family: var(--mono); font-size: 11.5px; line-height: 1.5; white-space: pre-wrap; word-break: break-word; user-select: text; }
  .logbtns { display: flex; gap: 8px; flex-wrap: wrap; }
  .act { height: 44px; border-radius: 22px; border: none; background: var(--ink); color: #fff; padding: 0 18px; font-size: 14px; font-weight: 700; cursor: pointer; }
  .act.ghost { background: var(--hair); color: var(--ink); }
  .note { margin: 0 6px; font-size: 13px; line-height: 1.45; color: var(--sub); }
  .msg { margin: 0 6px; font-size: 13px; color: var(--sub); }
  .msg.bad { color: #A1311F; font-weight: 600; }
  button:focus-visible { outline: 3px solid var(--ink); outline-offset: 2px; }
</style>
