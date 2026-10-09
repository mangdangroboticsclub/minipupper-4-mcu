<script>
  import { marked } from "marked";
  import PermissionCard from "../PermissionCard.svelte";

  let { msg, isFirst = false, isLastInGroup = false, respondPermission, busy = false } = $props();

  /* ── Markdown renderer ─────────────────── */
  const renderer = new marked.Renderer();
  renderer.link = ({ href, text }) =>
    `<a href="${href}" target="_blank" rel="noopener noreferrer" class="msg-link">${text}</a>`;
  marked.setOptions({ renderer, breaks: true, gfm: true });

  function renderMarkdown(text) {
    if (!text) return "";
    const raw = marked.parse(text, { async: false });
    return raw
      .replace(/<script[\s\S]*?<\/script>/gi, "")
      .replace(/\son\w+\s*=\s*["']?[^"'\s>]+["']?/gi, "");
  }

  function fmtTime(ts) {
    if (!ts) return "";
    const d = ts < 100000000000 ? new Date(ts * 1000) : new Date(ts);
    return d.toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" });
  }

  let isUser = $derived(msg.role === "user");
  let isBot = $derived(msg.role === "bot");
  let isSystem = $derived(msg.role === "system");
  let isStep = $derived(msg.role === "step");

  // ── Permission / action-result system messages ─────────────
  // New-format messages carry { kind, actionType, description, status }.
  // Legacy messages only have { text, actionId, actionType, pending }.
  let permAction = $derived(
    isSystem && msg.actionId && msg.pending
      ? {
          id: msg.actionId,
          type: msg.actionType,
          description: msg.description ?? (msg.text || "").replace(/^🔧 OpenClaw requests:\s*/, ""),
          receivedAt: msg.receivedAt,
        }
      : null,
  );
  let actionResult = $derived(
    isSystem && !msg.pending && (msg.kind === "action_result" || msg.kind === "permission")
      ? {
          type: msg.actionType,
          description: msg.description ?? "",
          status: msg.status || "completed",
        }
      : null,
  );

  const ACTION_LABEL = {
    file_write: "Write file",
    file_delete: "Delete file",
    wasm_run: "Run skill",
    lua_run: "Run script",
  };
  function actionLabel(type) {
    return ACTION_LABEL[type] ?? (type || "action").replace(/_/g, " ");
  }
  const STATUS_LABEL = { approved: "Approved", denied: "Denied", timeout: "Timed out", completed: "Done", error: "Failed" };

  // ── Bot: merge commands with their results ─────────────────
  let robotActions = $derived.by(() => {
    if (!isBot) return [];
    const cmds = msg.commands || [];
    const results = msg.commandResults || [];
    const used = new Set();
    const rows = cmds.map((cmd, i) => {
      let rIdx = results.findIndex((r, j) => !used.has(j) && r.script === cmd.script);
      if (rIdx === -1 && results[i] && !used.has(i)) rIdx = i;
      const result = rIdx >= 0 ? results[rIdx] : null;
      if (rIdx >= 0) used.add(rIdx);
      return {
        type: cmd.type || "lua",
        script: cmd.script,
        output: result?.output ?? "",
        // No result yet: the newest reply is presumably still executing;
        // older replies without a result never reported back.
        status: result ? result.status : busy ? "running" : "unknown",
      };
    });
    // Any results that didn't match a listed command
    results.forEach((r, j) => {
      if (!used.has(j)) rows.push({ type: "lua", script: r.script, output: r.output ?? "", status: r.status });
    });
    return rows;
  });
  let actionSummary = $derived.by(() => {
    const n = robotActions.length;
    if (!n) return "";
    const done = robotActions.filter((a) => a.status === "completed").length;
    const failed = robotActions.filter((a) => a.status === "error" || a.status === "timeout").length;
    const running = robotActions.filter((a) => a.status === "running").length;
    if (running) return `${done}/${n} done · ${running} running`;
    if (failed) return `${done}/${n} done · ${failed} failed`;
    if (done === n) return `${n} ${n === 1 ? "action" : "actions"} completed`;
    if (done > 0) return `${done}/${n} done`;
    return `${n} ${n === 1 ? "command" : "commands"} sent`;
  });
  let actionsOpen = $state(false);
  let hasProblem = $derived(robotActions.some((a) => a.status === "error" || a.status === "timeout"));
  let hasRunning = $derived(robotActions.some((a) => a.status === "running"));

  // Timeline helpers for user messages
  let history = $derived(msg.history || []);
  let lastEntry = $derived(history.length > 0 ? history[history.length - 1] : null);

  function stageLabel(e) {
    switch (e?.stage) {
      case "sending": return "Sending…";
      case "sent": return "Sent";
      case "session_reset": return "Session ready";
      case "relayed": return "Relayed to cloud";
      case "processing": return "AI processing…";
      case "step": return `Step ${e.seq}/${e.total}: ${e.text}`;
      case "completed": return "Complete";
      default: return e?.stage || "";
    }
  }
</script>

{#if isStep}
  <!-- Fallback for a lone step (normally grouped by AgentActivity) -->
  <div class="msg-row msg-row-step">
    <div class="step-bubble">
      <span class="step-number">{msg.seq}</span>
      <p class="step-text">{msg.text}</p>
    </div>
  </div>

{:else if isSystem}
  <div class="msg-row msg-row-center">
    {#if permAction}
      <div class="perm-wrap">
        <PermissionCard compact action={permAction} onRespond={respondPermission} />
      </div>
    {:else if actionResult}
      <div class="action-chip" data-status={actionResult.status}>
        <span class="action-chip-icon" aria-hidden="true">
          {#if actionResult.status === "approved" || actionResult.status === "completed"}
            <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="3" stroke-linecap="round" stroke-linejoin="round"><polyline points="4 12.5 10 18 20 6"/></svg>
          {:else if actionResult.status === "denied" || actionResult.status === "error"}
            <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="3" stroke-linecap="round" stroke-linejoin="round"><path d="M18 6 6 18M6 6l12 12"/></svg>
          {:else}
            <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="9"/><path d="M12 7v5l3 2"/></svg>
          {/if}
        </span>
        <span class="action-chip-text">
          <b>{STATUS_LABEL[actionResult.status] ?? actionResult.status}</b>
          · {actionLabel(actionResult.type)}
          {#if actionResult.description}<code>{actionResult.description}</code>{/if}
        </span>
      </div>
    {:else}
      <div class="system-bubble"><p>{msg.text}</p></div>
    {/if}
  </div>

{:else if isUser}
  <div class="msg-row msg-row-user">
    <div class="user-wrapper">
      <div class="user-bubble">
        <p class="user-text">{msg.text}</p>
        <div class="user-meta">
          <span class="msg-time">{fmtTime(msg.ts)}</span>
          {#if msg.status === "sending" || msg.status?.startsWith("step_")}
            <span class="sending-dot"></span>
          {:else if msg.status === "completed"}
            <svg width="11" height="11" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="3" stroke-linecap="round" stroke-linejoin="round" class="check-icon"><polyline points="4 12.5 10 18 20 6"/></svg>
          {/if}
        </div>
      </div>
      {#if lastEntry && lastEntry.stage !== "completed" && lastEntry.stage !== "step"}
        <div class="timeline-single">
          <span class="dot" class:live={lastEntry.stage === "sending" || lastEntry.stage === "processing"}></span>
          <span>{stageLabel(lastEntry)}</span>
        </div>
      {/if}
    </div>
  </div>

{:else if isBot}
  <div class="msg-row msg-row-bot">
    <div class="bot-wrapper">
      <div class="bot-bubble">
        <div class="bot-content markdown-body">
          {@html renderMarkdown(msg.text)}
        </div>
        {#if msg.ts}
          <div class="bot-meta"><span class="msg-time">{fmtTime(msg.ts)}</span></div>
        {/if}
      </div>

      {#if robotActions.length}
        <div class="robot-actions" class:problem={hasProblem} class:running={hasRunning}>
          <button class="ra-head" onclick={() => (actionsOpen = !actionsOpen)} aria-expanded={actionsOpen}>
            <span class="ra-icon" aria-hidden="true">
              <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round">
                <rect x="4" y="7" width="16" height="12" rx="3"/><path d="M12 3v4M8 12h.01M16 12h.01M9 16h6"/>
              </svg>
            </span>
            <span class="ra-title">Robot actions</span>
            <span class="ra-summary">{actionSummary}</span>
            <svg class="chev" class:open={actionsOpen} width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><polyline points="6 9 12 15 18 9"/></svg>
          </button>

          <!-- status strip: one segment per action -->
          <div class="ra-strip" aria-hidden="true">
            {#each robotActions as a}
              <span class="seg" data-status={a.status}></span>
            {/each}
          </div>

          {#if actionsOpen}
            <ol class="ra-list">
              {#each robotActions as a, i}
                <li class="ra-item" data-status={a.status}>
                  <div class="ra-item-head">
                    <span class="ra-num">#{i + 1}</span>
                    <span class="ra-type">{a.type}</span>
                    <span class="ra-status">
                      {#if a.status === "running"}<span class="mini-spin"></span>Running
                      {:else if a.status === "unknown"}Sent
                      {:else if a.status === "completed"}Done
                      {:else if a.status === "error"}Failed
                      {:else if a.status === "timeout"}Timed out
                      {:else}{a.status}{/if}
                    </span>
                  </div>
                  <pre class="ra-script">{a.script}</pre>
                  {#if a.output}
                    <pre class="ra-output">{a.output}</pre>
                  {/if}
                </li>
              {/each}
            </ol>
          {/if}
        </div>
      {/if}
    </div>
  </div>
{/if}

<style>
  .msg-row { display: flex; padding: 2px 16px; }
  .msg-row-step { justify-content: flex-start; }
  .msg-row-center { justify-content: center; }
  .msg-row-user { justify-content: flex-end; }
  .msg-row-bot { justify-content: flex-start; }

  /* ── Lone step ─────────────────────────── */
  .step-bubble {
    max-width: 85%;
    display: flex; align-items: center; gap: 8px;
    border-radius: var(--r-md);
    padding: 8px 12px;
    background: var(--mpx-surface);
    border: 1px solid var(--mpx-line);
  }
  .step-number {
    width: 20px; height: 20px; border-radius: 50%;
    background: var(--mpx-yellow);
    display: grid; place-items: center;
    font-size: 0.65rem; font-weight: 800; color: var(--mpx-ink);
    flex-shrink: 0;
  }
  .step-text { font-size: 0.8rem; color: var(--mpx-ink-soft); }

  /* ── System ────────────────────────────── */
  .system-bubble {
    max-width: 90%;
    border-radius: var(--r-md);
    padding: 8px 14px;
    background: var(--mpx-surface);
    border: 1px solid var(--mpx-line);
    text-align: center;
    font-size: 0.75rem;
    color: var(--mpx-muted);
  }
  .perm-wrap { width: 100%; max-width: 420px; }

  .action-chip {
    display: inline-flex; align-items: center; gap: 8px;
    max-width: 92%;
    padding: 6px 12px 6px 8px;
    border-radius: var(--r-pill);
    background: var(--mpx-surface);
    border: 1px solid var(--mpx-line);
    font-size: 0.72rem;
    color: var(--mpx-ink-soft);
  }
  .action-chip-icon {
    width: 20px; height: 20px; border-radius: 50%;
    display: grid; place-items: center;
    background: var(--mpx-surface-3); color: var(--mpx-muted);
    flex-shrink: 0;
  }
  .action-chip[data-status="approved"] .action-chip-icon,
  .action-chip[data-status="completed"] .action-chip-icon { background: #dcfce7; color: #15803d; }
  .action-chip[data-status="denied"] .action-chip-icon,
  .action-chip[data-status="error"] .action-chip-icon { background: #fee2e2; color: #b91c1c; }
  .action-chip[data-status="timeout"] .action-chip-icon { background: #fef3c7; color: #b45309; }
  .action-chip-text { min-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  .action-chip-text b { font-weight: 800; color: var(--mpx-ink); }
  .action-chip-text code {
    margin-left: 6px;
    font-family: ui-monospace, Menlo, Consolas, monospace;
    font-size: 0.68rem;
    background: var(--mpx-surface-2);
    padding: 1px 6px; border-radius: 4px;
  }

  /* ── User bubble ───────────────────────── */
  .user-wrapper { display: flex; flex-direction: column; align-items: flex-end; max-width: 80%; }
  .user-bubble {
    background: var(--mpx-yellow);
    border-radius: 18px 18px 6px 18px;
    padding: 10px 14px;
  }
  .user-text { font-size: 0.88rem; color: var(--mpx-ink); line-height: 1.4; word-wrap: break-word; }
  .user-meta { display: flex; align-items: center; justify-content: flex-end; gap: 5px; margin-top: 4px; }
  .msg-time { font-size: 0.6rem; color: rgba(0, 0, 0, 0.4); font-variant-numeric: tabular-nums; }
  .sending-dot { width: 6px; height: 6px; border-radius: 50%; background: rgba(0, 0, 0, 0.35); animation: blink 1.2s infinite; }
  @keyframes blink { 0%, 100% { opacity: 1; } 50% { opacity: 0.3; } }
  .check-icon { color: rgba(0, 0, 0, 0.5); }

  .timeline-single { display: flex; align-items: center; gap: 5px; font-size: 0.66rem; color: var(--mpx-muted); margin-top: 3px; padding-right: 4px; }
  .dot { width: 6px; height: 6px; border-radius: 50%; background: var(--mpx-muted-2); }
  .dot.live { background: var(--mpx-yellow-deep); animation: blink 1.2s infinite; }

  /* ── Bot bubble ────────────────────────── */
  .bot-wrapper { max-width: 85%; display: flex; flex-direction: column; gap: 6px; min-width: 0; }
  .bot-bubble {
    background: var(--mpx-surface);
    border: 1px solid var(--mpx-line);
    border-radius: 18px 18px 18px 6px;
    padding: 12px 14px;
    box-shadow: var(--shadow-card);
  }
  .bot-content { font-size: 0.88rem; color: var(--mpx-ink-soft); line-height: 1.5; word-break: break-word; }
  .bot-meta { display: flex; margin-top: 6px; }
  .bot-meta .msg-time { color: rgba(0, 0, 0, 0.3); }

  /* ── Robot actions panel ───────────────── */
  .robot-actions {
    border-radius: var(--r-md);
    background: var(--mpx-ink);
    color: #fff;
    overflow: hidden;
  }
  .ra-head {
    width: 100%;
    display: flex; align-items: center; gap: 8px;
    padding: 9px 12px 7px;
    background: none; border: none; cursor: pointer;
    color: inherit; text-align: left;
  }
  .ra-icon {
    width: 22px; height: 22px; border-radius: 6px;
    background: var(--mpx-yellow); color: var(--mpx-ink);
    display: grid; place-items: center; flex-shrink: 0;
  }
  .ra-title { font-size: 0.76rem; font-weight: 800; }
  .ra-summary { flex: 1; font-size: 0.68rem; color: rgba(255,255,255,0.6); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  .chev { color: rgba(255,255,255,0.5); transition: transform var(--dur-med) var(--ease-spring); }
  .chev.open { transform: rotate(180deg); }

  .ra-strip { display: flex; gap: 3px; padding: 0 12px 9px; }
  .seg { flex: 1; height: 4px; border-radius: 2px; background: rgba(255,255,255,0.18); }
  .seg[data-status="completed"] { background: var(--mpx-ok); }
  .seg[data-status="error"] { background: var(--mpx-err); }
  .seg[data-status="timeout"] { background: #f59e0b; }
  .seg[data-status="running"] { background: var(--mpx-yellow); animation: blink 1s infinite; }

  .ra-list { list-style: none; margin: 0; padding: 0 8px 8px; display: flex; flex-direction: column; gap: 6px; }
  .ra-item {
    border-radius: var(--r-sm);
    background: rgba(255,255,255,0.06);
    padding: 8px 10px;
    border-left: 3px solid rgba(255,255,255,0.2);
  }
  .ra-item[data-status="completed"] { border-left-color: var(--mpx-ok); }
  .ra-item[data-status="error"] { border-left-color: var(--mpx-err); }
  .ra-item[data-status="timeout"] { border-left-color: #f59e0b; }
  .ra-item[data-status="running"] { border-left-color: var(--mpx-yellow); }
  .ra-item-head { display: flex; align-items: center; gap: 8px; margin-bottom: 4px; }
  .ra-num { font-size: 0.62rem; color: rgba(255,255,255,0.45); font-weight: 700; }
  .ra-type { font-size: 0.58rem; font-weight: 800; text-transform: uppercase; letter-spacing: 0.06em; padding: 1px 6px; border-radius: 4px; background: rgba(255,230,5,0.2); color: var(--mpx-yellow); }
  .ra-status { margin-left: auto; font-size: 0.64rem; font-weight: 700; display: inline-flex; align-items: center; gap: 5px; color: rgba(255,255,255,0.75); }
  .ra-item[data-status="completed"] .ra-status { color: var(--mpx-ok); }
  .ra-item[data-status="error"] .ra-status { color: var(--mpx-err); }
  .ra-item[data-status="timeout"] .ra-status { color: #f59e0b; }
  .ra-item[data-status="running"] .ra-status { color: var(--mpx-yellow); }
  .mini-spin { width: 10px; height: 10px; border-radius: 50%; border: 2px solid rgba(255,230,5,0.3); border-top-color: var(--mpx-yellow); animation: spin 0.8s linear infinite; }
  @keyframes spin { to { transform: rotate(360deg); } }

  .ra-script, .ra-output {
    font-family: ui-monospace, "JetBrains Mono", Menlo, Consolas, monospace;
    font-size: 0.68rem;
    white-space: pre-wrap;
    word-break: break-word;
    overflow-x: auto;
    margin: 0;
    line-height: 1.45;
  }
  .ra-script { color: rgba(255,255,255,0.85); }
  .ra-output {
    margin-top: 6px;
    padding: 6px 8px;
    border-radius: 6px;
    background: rgba(0,0,0,0.35);
    color: rgba(255,255,255,0.7);
  }
  .ra-item[data-status="error"] .ra-output { color: #fecaca; }

  /* ── Markdown ──────────────────────────── */
  :global(.msg-link) { color: #8a7c00; text-decoration: underline; }
  :global(.bot-content.markdown-body p) { margin: 0.4em 0; }
  :global(.bot-content.markdown-body p:first-child) { margin-top: 0; }
  :global(.bot-content.markdown-body p:last-child) { margin-bottom: 0; }
  :global(.bot-content.markdown-body ul),
  :global(.bot-content.markdown-body ol) { padding-left: 1.2em; margin: 0.3em 0; }
  :global(.bot-content.markdown-body code) { background: #f0f0f0; padding: 1px 4px; border-radius: 3px; font-size: 0.8em; }
  :global(.bot-content.markdown-body pre) { background: #f5f5f5; padding: 8px 10px; border-radius: 6px; font-size: 0.75rem; overflow-x: auto; }
  :global(.bot-content.markdown-body pre code) { background: none; padding: 0; }
  :global(.bot-content.markdown-body blockquote) { border-left: 3px solid #FFE605; padding-left: 10px; margin: 0.4em 0; color: #888; }
  :global(.bot-content.markdown-body table) { font-size: 0.75rem; border-collapse: collapse; }
  :global(.bot-content.markdown-body th),
  :global(.bot-content.markdown-body td) { border: 1px solid #e8e8e8; padding: 4px 8px; }
</style>
