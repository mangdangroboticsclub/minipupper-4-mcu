<script>
  /**
   * PermissionCard — a request from the agent to perform a privileged
   * action on the robot (file write / delete, running a skill, …).
   *
   * Used both by the global modal (app.svelte) and inline in the chat.
   */
  let {
    action,          // { id, type, description, receivedAt? }
    queued = 0,      // how many more requests are waiting behind this one
    compact = false, // inline (chat) variant
    onRespond,       // (actionId, approved) => void
  } = $props();

  const META = {
    file_write:  { label: "Write file",   tone: "#0357B7", hint: "The agent wants to save a file on the robot." },
    file_delete: { label: "Delete file",  tone: "#ED7676", hint: "The agent wants to remove a file from the robot." },
    wasm_run:    { label: "Run skill",    tone: "#A476ED", hint: "The agent wants to execute a Move Skill." },
    lua_run:     { label: "Run script",   tone: "#6AAE6C", hint: "The agent wants to run a Lua script." },
  };

  let meta = $derived(META[action?.type] ?? {
    label: (action?.type || "action").replace(/_/g, " "),
    tone: "#969494",
    hint: "The agent is requesting access to perform an action.",
  });

  // Countdown — firmware times out permission requests after 60 s.
  const TIMEOUT_MS = 60_000;
  let now = $state(Date.now());
  $effect(() => {
    const id = setInterval(() => (now = Date.now()), 500);
    return () => clearInterval(id);
  });
  let remaining = $derived(
    action?.receivedAt ? Math.max(0, TIMEOUT_MS - (now - action.receivedAt)) : null,
  );
  let remainingPct = $derived(remaining === null ? 100 : (remaining / TIMEOUT_MS) * 100);
</script>

<div class="perm-card" class:compact style="--tone: {meta.tone}">
  <div class="perm-head">
    <div class="perm-icon" aria-hidden="true">
      {#if action?.type === "file_write"}
        <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round">
          <path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z"/><polyline points="14 2 14 8 20 8"/><path d="M12 18v-6"/><path d="M9 15l3 3 3-3"/>
        </svg>
      {:else if action?.type === "file_delete"}
        <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round">
          <polyline points="3 6 5 6 21 6"/><path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2"/>
        </svg>
      {:else if action?.type === "wasm_run" || action?.type === "lua_run"}
        <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round">
          <polygon points="6 3 20 12 6 21 6 3"/>
        </svg>
      {:else}
        <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round">
          <path d="M12 22s8-4 8-10V5l-8-3-8 3v7c0 6 8 10 8 10z"/>
        </svg>
      {/if}
    </div>
    <div class="perm-titles">
      <span class="perm-kicker">Permission required</span>
      <h3 class="perm-title">{meta.label}</h3>
    </div>
    {#if queued > 0}
      <span class="perm-queue" title="{queued} more waiting">+{queued}</span>
    {/if}
  </div>

  <p class="perm-hint">{meta.hint}</p>

  {#if action?.description}
    <div class="perm-desc">
      <code>{action.description}</code>
    </div>
  {/if}

  {#if remaining !== null}
    <div class="perm-timer" aria-hidden="true">
      <div class="perm-timer-fill" style="width: {remainingPct}%"></div>
    </div>
  {/if}

  <div class="perm-buttons">
    <button class="perm-btn perm-btn-deny" onclick={() => onRespond?.(action.id, false)}>
      Deny
    </button>
    <button class="perm-btn perm-btn-approve" onclick={() => onRespond?.(action.id, true)}>
      Approve
      {#if remaining !== null}
        <span class="perm-countdown">{Math.ceil(remaining / 1000)}s</span>
      {/if}
    </button>
  </div>
</div>

<style>
  .perm-card {
    background: var(--mpx-surface);
    border-radius: var(--r-xl);
    padding: 20px 18px 16px;
    box-shadow: var(--shadow-float);
    display: flex;
    flex-direction: column;
    gap: 12px;
  }
  .perm-card.compact {
    box-shadow: var(--shadow-card);
    border: 1px solid var(--mpx-line);
    border-radius: var(--r-lg);
    padding: 14px 14px 12px;
    gap: 10px;
  }

  .perm-head {
    display: flex;
    align-items: center;
    gap: 12px;
  }
  .perm-icon {
    width: 44px;
    height: 44px;
    border-radius: var(--r-md);
    background: color-mix(in srgb, var(--tone) 14%, #fff);
    color: var(--tone);
    display: grid;
    place-items: center;
    flex-shrink: 0;
  }
  .compact .perm-icon { width: 38px; height: 38px; }

  .perm-titles { display: flex; flex-direction: column; gap: 1px; min-width: 0; flex: 1; }
  .perm-kicker {
    font-size: 0.66rem;
    font-weight: 700;
    letter-spacing: 0.08em;
    text-transform: uppercase;
    color: var(--mpx-muted);
  }
  .perm-title {
    font-size: 1.05rem;
    font-weight: 800;
    color: var(--mpx-ink);
    text-transform: capitalize;
    line-height: 1.2;
  }
  .compact .perm-title { font-size: 0.95rem; }

  .perm-queue {
    font-size: 0.7rem;
    font-weight: 700;
    color: var(--mpx-ink);
    background: var(--mpx-yellow);
    border-radius: var(--r-pill);
    padding: 3px 8px;
  }

  .perm-hint {
    font-size: 0.8rem;
    color: var(--mpx-muted);
    line-height: 1.4;
  }
  .compact .perm-hint { display: none; }

  .perm-desc {
    background: var(--mpx-surface-2);
    border-radius: var(--r-md);
    padding: 10px 12px;
    border-left: 3px solid var(--tone);
  }
  .perm-desc code {
    font-family: 'JetBrains Mono', 'Fira Code', ui-monospace, monospace;
    font-size: 0.76rem;
    color: var(--mpx-ink-soft);
    word-break: break-word;
    white-space: pre-wrap;
  }

  .perm-timer {
    height: 3px;
    border-radius: 2px;
    background: var(--mpx-surface-3);
    overflow: hidden;
  }
  .perm-timer-fill {
    height: 100%;
    background: var(--tone);
    transition: width 0.5s linear;
  }

  .perm-buttons { display: flex; gap: 10px; }
  .perm-btn {
    flex: 1;
    padding: 13px 0;
    border-radius: var(--r-md);
    border: none;
    font-size: 0.9rem;
    font-weight: 700;
    cursor: pointer;
    display: inline-flex;
    align-items: center;
    justify-content: center;
    gap: 8px;
    transition: transform var(--dur-fast) var(--ease-out), filter var(--dur-fast);
  }
  .compact .perm-btn { padding: 10px 0; font-size: 0.82rem; }
  .perm-btn:active { transform: scale(0.97); }
  .perm-btn-deny {
    background: var(--mpx-surface-2);
    color: var(--mpx-ink);
  }
  .perm-btn-approve {
    background: var(--mpx-yellow);
    color: var(--mpx-ink);
  }
  .perm-btn-approve:active { filter: brightness(0.94); }
  .perm-countdown {
    font-size: 0.7rem;
    font-weight: 700;
    background: rgba(0, 0, 0, 0.12);
    padding: 2px 7px;
    border-radius: var(--r-pill);
    font-variant-numeric: tabular-nums;
  }
</style>
