<script>
  import { untrack } from "svelte";

  /**
   * System monitor widget: a ring gauge in the Home header (outer ring CPU,
   * inner ring internal RAM) that opens a panel with the detail from
   * GET /v1/system/stats (main/util/sys_monitor.h).
   * @prop online  the robot answered its last status poll
   */
  let { online = true } = $props();

  const POLL_MS = 5_000;          // closed: enough to keep the gauge honest
  const POLL_OPEN_MS = 2_000;     // open: the firmware samples every 2 s
  const FETCH_TIMEOUT_MS = 3_000;
  const WARN_PCT = 75;
  const CRIT_PCT = 90;

  let stats = $state(null);
  let error = $state("");
  let open = $state(false);
  let updatedAt = $state(0);
  let timer = null;

  async function poll() {
    clearTimeout(timer);
    if (online && document.visibilityState === "visible") {
      const ctrl = new AbortController();
      const to = setTimeout(() => ctrl.abort(), FETCH_TIMEOUT_MS);
      try {
        const res = await fetch("/v1/system/stats", { signal: ctrl.signal, cache: "no-store" });
        const data = await res.json();
        if (data.ok) {
          stats = data;
          error = "";
          updatedAt = Date.now();
        } else {
          error = data.error || "unavailable";
        }
      } catch {
        error = "unreachable";
      } finally {
        clearTimeout(to);
      }
    }
    timer = setTimeout(poll, open ? POLL_OPEN_MS : POLL_MS);
  }

  // Runs once. untrack: poll() reads `online` and `open`, and as dependencies
  // they would restart the loop (and fire an extra request) on every change.
  $effect(() => untrack(() => {
    poll();
    const onVis = () => { if (document.visibilityState === "visible") poll(); };
    document.addEventListener("visibilitychange", onVis);
    return () => {
      clearTimeout(timer);
      document.removeEventListener("visibilitychange", onVis);
    };
  }));

  function toggle() {
    open = !open;
    if (open) poll();
  }

  function onKey(e) { if (open && e.key === "Escape") open = false; }

  const usedPct = (m) => (m && m.total ? Math.round(100 * (1 - m.free / m.total)) : 0);
  const cpuMax = $derived(stats?.cpu ? Math.max(...stats.cpu) : null);
  const ramPct = $derived(stats ? usedPct(stats.internal) : null);
  const level = (pct) => (pct == null ? "" : pct >= CRIT_PCT ? "crit" : pct >= WARN_PCT ? "warn" : "");

  function bytes(n) {
    if (n >= 1024 * 1024) return `${(n / 1024 / 1024).toFixed(1)} MB`;
    if (n >= 1024) return `${Math.round(n / 1024)} KB`;
    return `${n} B`;
  }

  function uptime(s) {
    const d = Math.floor(s / 86400), h = Math.floor((s % 86400) / 3600), m = Math.floor((s % 3600) / 60);
    if (d) return `${d}d ${h}h`;
    if (h) return `${h}h ${m}m`;
    return `${m}m ${s % 60}s`;
  }

  // Ring geometry: circumference-based dash for each gauge.
  const R_OUT = 16, R_IN = 11;
  const C_OUT = 2 * Math.PI * R_OUT, C_IN = 2 * Math.PI * R_IN;
  const dash = (pct, c) => `${((pct ?? 0) / 100) * c} ${c}`;

  const label = $derived(
    stats
      ? `System monitor: CPU ${cpuMax ?? "–"}%, internal RAM ${ramPct}% used. Open details`
      : "System monitor. Open details",
  );
</script>

<svelte:window onkeydown={onKey} />

<button type="button" class="gauge" class:stale={!online || (error && !stats)} onclick={toggle}
        aria-haspopup="dialog" aria-expanded={open} aria-label={label}>
  <svg width="40" height="40" viewBox="0 0 40 40" aria-hidden="true">
    <circle class="track" cx="20" cy="20" r={R_OUT} />
    <circle class="track" cx="20" cy="20" r={R_IN} />
    <circle class="arc cpu {level(cpuMax)}" cx="20" cy="20" r={R_OUT} stroke-dasharray={dash(cpuMax, C_OUT)} />
    <circle class="arc ram {level(ramPct)}" cx="20" cy="20" r={R_IN} stroke-dasharray={dash(ramPct, C_IN)} />
  </svg>
  <span class="num">{cpuMax ?? "–"}</span>
</button>

{#if open}
  <div class="scrim" role="presentation" onclick={() => (open = false)}></div>
  <div class="panel" role="dialog" aria-label="System monitor">
    <div class="head">
      <h2>System</h2>
      {#if stats}<span class="sub">up {uptime(stats.uptime_s)}</span>{/if}
    </div>

    {#if !stats}
      <p class="empty">{online ? (error ? `No data: ${error}` : "Loading…") : "Robot offline"}</p>
    {:else}
      <section>
        <h3>CPU <span class="sub">last {(stats.window_ms / 1000).toFixed(1)} s</span></h3>
        {#if stats.cpu}
          {#each stats.cpu as pct, core}
            <div class="row">
              <span class="k">Core {core}</span>
              <span class="bar"><i class={level(pct)} style="width: {pct}%"></i></span>
              <span class="v">{pct}%</span>
            </div>
          {/each}
        {:else}
          <p class="empty">Measuring…</p>
        {/if}
      </section>

      <section>
        <h3>Memory</h3>
        {#each [["Internal", stats.internal], ["PSRAM", stats.psram]] as [name, m]}
          {#if m.total}
            <div class="row">
              <span class="k">{name}</span>
              <span class="bar"><i class={level(usedPct(m))} style="width: {usedPct(m)}%"></i></span>
              <span class="v">{usedPct(m)}%</span>
            </div>
            <p class="detail">{bytes(m.free)} free of {bytes(m.total)} · low {bytes(m.min_free)} · largest {bytes(m.largest)}</p>
          {/if}
        {/each}
      </section>

      <section>
        <h3>Tasks <span class="sub">{stats.tasks.count} running · % of one core</span></h3>
        <ul class="tasks">
          {#each stats.tasks.top as t}
            <li>
              <span class="tname">{t.name}</span>
              <span class="tcore">{t.core < 0 ? "any" : `core ${t.core}`}</span>
              <span class="v">{t.cpu}%</span>
            </li>
          {/each}
        </ul>
        <p class="detail wide" class:alert={stats.tasks.low_stack.stack_free < 512}>
          Least stack left: {stats.tasks.low_stack.name} ({bytes(stats.tasks.low_stack.stack_free)})
        </p>
      </section>

      <p class="foot">
        {#if error}Last update failed ({error}) · {/if}
        updated {new Date(updatedAt).toLocaleTimeString()}
      </p>
    {/if}
  </div>
{/if}

<style>
  .gauge {
    position: relative;
    width: 40px; height: 40px; border-radius: 20px; border: none;
    background: var(--ground); color: var(--ink);
    display: grid; place-items: center; cursor: pointer; padding: 0;
    flex-shrink: 0;
  }
  .gauge:focus-visible { outline: 3px solid var(--ink); outline-offset: 2px; }
  .gauge.stale { opacity: 0.45; }
  .gauge svg { position: absolute; inset: 0; transform: rotate(-90deg); }
  .track { fill: none; stroke: var(--hair); stroke-width: 3.5; }
  .arc { fill: none; stroke-width: 3.5; stroke-linecap: round; transition: stroke-dasharray var(--dur-med) var(--ease-out); }
  .arc.cpu { stroke: var(--ink); }
  .arc.ram { stroke: var(--mpx-info); }
  .arc.warn { stroke: var(--mpx-warn); }
  .arc.crit { stroke: var(--mpx-err); }
  .num { position: relative; font: 700 10px/1 var(--mono); letter-spacing: -0.03em; }

  .scrim { position: fixed; inset: 0; z-index: 50; }
  .panel {
    position: fixed; z-index: 51;
    top: calc(64px + var(--safe-top));
    right: max(12px, calc((100vw - var(--page-max)) / 2 + 12px));
    width: min(320px, calc(100vw - 24px));
    max-height: calc(100dvh - 140px - var(--safe-top));
    overflow-y: auto;
    background: #fff; border: 1px solid var(--line); border-radius: 20px;
    box-shadow: var(--shadow-float);
    padding: 14px 16px 12px;
    display: flex; flex-direction: column; gap: 14px;
    animation: pop var(--dur-med) var(--ease-spring);
    transform-origin: top right;
  }
  @keyframes pop { from { opacity: 0; transform: scale(0.96) translateY(-6px); } to { opacity: 1; transform: none; } }

  .head { display: flex; align-items: baseline; justify-content: space-between; }
  h2 { font-size: 18px; font-weight: 800; letter-spacing: -0.01em; }
  h3 { font-size: 12px; font-weight: 700; text-transform: uppercase; letter-spacing: 0.06em; color: var(--sub); margin-bottom: 6px; display: flex; justify-content: space-between; gap: 8px; }
  h3 .sub { text-transform: none; letter-spacing: 0; font-weight: 500; }
  .sub { font-size: 12px; color: var(--sub); }
  section { display: flex; flex-direction: column; gap: 4px; }

  .row { display: grid; grid-template-columns: 64px 1fr 40px; align-items: center; gap: 8px; font-size: 13px; }
  .k { color: var(--ink); font-weight: 600; }
  .v { font: 600 12px var(--mono); text-align: right; }
  .bar { height: 8px; border-radius: 4px; background: var(--ground); overflow: hidden; }
  .bar i { display: block; height: 100%; border-radius: 4px; background: var(--ink); transition: width var(--dur-med) var(--ease-out); }
  .bar i.warn { background: var(--mpx-warn); }
  .bar i.crit { background: var(--mpx-err); }
  .detail { font-size: 11.5px; color: var(--sub); padding-left: 72px; margin-bottom: 4px; }
  .detail.wide { padding-left: 0; margin-top: 4px; }
  .detail.alert { color: #9B2C20; font-weight: 600; }

  .tasks { list-style: none; display: flex; flex-direction: column; gap: 2px; }
  .tasks li { display: grid; grid-template-columns: 1fr auto 40px; gap: 8px; font-size: 13px; align-items: center; }
  .tname { font-family: var(--mono); font-size: 12px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  .tcore { font-size: 11px; color: var(--sub); }

  .empty { font-size: 13px; color: var(--sub); }
  .foot { font-size: 11px; color: var(--mpx-muted); text-align: right; }
</style>
