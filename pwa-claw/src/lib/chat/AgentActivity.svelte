<script>
  /**
   * AgentActivity — a compact timeline of the agent's reasoning/working
   * steps for one turn. Consecutive `step` frames are grouped by the
   * parent into a single activity block.
   *
   * @prop steps   — [{ text, seq, total, ts }]
   * @prop active  — true while the agent is still working on this turn
   */
  let { steps = [], active = false } = $props();

  let total = $derived(steps.reduce((m, s) => Math.max(m, s.total || 0), 0) || steps.length);
  let last = $derived(steps[steps.length - 1]);
  let open = $state(true);

  // Auto-collapse once the turn finishes, but let the user re-open.
  let wasActive = false;
  $effect(() => {
    if (wasActive && !active) open = false;
    wasActive = active;
  });

  function fmtTime(ts) {
    if (!ts) return "";
    const d = ts < 100000000000 ? new Date(ts * 1000) : new Date(ts);
    return d.toLocaleTimeString([], { hour: "2-digit", minute: "2-digit", second: "2-digit" });
  }
</script>

<div class="activity" class:active class:open>
  <button class="activity-head" onclick={() => (open = !open)} aria-expanded={open}>
    <span class="head-icon" aria-hidden="true">
      {#if active}
        <span class="orbit"><span></span><span></span><span></span></span>
      {:else}
        <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="3" stroke-linecap="round" stroke-linejoin="round"><polyline points="4 12.5 10 18 20 6"/></svg>
      {/if}
    </span>
    <span class="head-text">
      {#if active}
        <span class="head-title">Working{last ? `: ${last.text}` : "…"}</span>
      {:else}
        <span class="head-title">Worked through {steps.length} {steps.length === 1 ? "step" : "steps"}</span>
      {/if}
    </span>
    <span class="head-count">{steps.length}{total ? `/${total}` : ""}</span>
    <svg class="chev" width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><polyline points="6 9 12 15 18 9"/></svg>
  </button>

  {#if open}
    <ol class="steps">
      {#each steps as step, i (step.seq ?? i)}
        {@const isLast = i === steps.length - 1}
        <li class="step" class:current={active && isLast} class:done={!active || !isLast}>
          <span class="rail" aria-hidden="true">
            <span class="node"></span>
            {#if !isLast}<span class="line"></span>{/if}
          </span>
          <div class="step-body">
            <div class="step-row">
              <span class="step-seq">Step {step.seq ?? i + 1}{step.total ? ` of ${step.total}` : ""}</span>
              {#if step.ts}<span class="step-time">{fmtTime(step.ts)}</span>{/if}
            </div>
            <p class="step-text">{step.text}</p>
          </div>
        </li>
      {/each}
    </ol>
  {/if}
</div>

<style>
  .activity {
    margin: 2px 16px;
    max-width: 85%;
    border-radius: var(--r-md);
    background: var(--mpx-surface);
    border: 1px solid var(--mpx-line);
    overflow: hidden;
    transition: border-color var(--dur-med);
  }
  .activity.active { border-color: color-mix(in srgb, var(--mpx-yellow) 70%, #fff); box-shadow: 0 0 0 3px var(--mpx-yellow-tint); }

  .activity-head {
    width: 100%;
    display: flex;
    align-items: center;
    gap: 8px;
    padding: 9px 12px;
    background: none;
    border: none;
    cursor: pointer;
    text-align: left;
    color: var(--mpx-ink-soft);
  }
  .head-icon {
    width: 22px; height: 22px;
    border-radius: 50%;
    display: grid; place-items: center;
    background: var(--mpx-yellow);
    color: var(--mpx-ink);
    flex-shrink: 0;
  }
  .activity:not(.active) .head-icon { background: #ecfdf3; color: #15803d; }

  .head-text { flex: 1; min-width: 0; display: flex; }
  .head-title {
    font-size: 0.76rem; font-weight: 700;
    white-space: nowrap; overflow: hidden; text-overflow: ellipsis;
  }
  .head-count { font-size: 0.66rem; font-weight: 700; color: var(--mpx-muted); font-variant-numeric: tabular-nums; }
  .chev { color: var(--mpx-muted-2); transition: transform var(--dur-med) var(--ease-spring); flex-shrink: 0; }
  .activity.open .chev { transform: rotate(180deg); }

  /* orbiting dots while active */
  .orbit { position: relative; width: 14px; height: 14px; display: block; animation: spin 1.4s linear infinite; }
  .orbit span {
    position: absolute; width: 4px; height: 4px; border-radius: 50%; background: var(--mpx-ink);
    top: 0; left: 50%; margin-left: -2px; transform-origin: 2px 7px;
  }
  .orbit span:nth-child(2) { transform: rotate(120deg); opacity: 0.6; }
  .orbit span:nth-child(3) { transform: rotate(240deg); opacity: 0.3; }
  @keyframes spin { to { transform: rotate(360deg); } }

  .steps {
    list-style: none;
    margin: 0;
    padding: 2px 12px 10px 12px;
    display: flex;
    flex-direction: column;
  }
  .step { display: flex; gap: 10px; }
  .rail { display: flex; flex-direction: column; align-items: center; width: 14px; flex-shrink: 0; }
  .node {
    width: 10px; height: 10px; border-radius: 50%;
    margin-top: 5px;
    background: var(--mpx-surface-3);
    border: 2px solid var(--mpx-muted-2);
    box-sizing: border-box;
    flex-shrink: 0;
  }
  .step.done .node { background: var(--mpx-ink); border-color: var(--mpx-ink); }
  .step.current .node {
    background: var(--mpx-yellow);
    border-color: var(--mpx-yellow);
    animation: pulse 1.2s ease-in-out infinite;
  }
  @keyframes pulse {
    0%, 100% { box-shadow: 0 0 0 0 rgba(255, 230, 5, 0.6); }
    50% { box-shadow: 0 0 0 5px rgba(255, 230, 5, 0); }
  }
  .line { flex: 1; width: 2px; background: var(--mpx-line); margin: 2px 0; min-height: 8px; }
  .step.done .line { background: color-mix(in srgb, var(--mpx-ink) 25%, #fff); }

  .step-body { flex: 1; min-width: 0; padding-bottom: 8px; }
  .step:last-child .step-body { padding-bottom: 0; }
  .step-row { display: flex; align-items: baseline; gap: 8px; }
  .step-seq { font-size: 0.62rem; font-weight: 800; text-transform: uppercase; letter-spacing: 0.06em; color: var(--mpx-muted); }
  .step.current .step-seq { color: #8a7c00; }
  .step-time { font-size: 0.6rem; color: var(--mpx-muted-2); margin-left: auto; font-variant-numeric: tabular-nums; }
  .step-text { font-size: 0.8rem; color: var(--mpx-ink-soft); line-height: 1.4; margin-top: 1px; }
  .step.current .step-text { color: var(--mpx-ink); font-weight: 600; }
</style>
