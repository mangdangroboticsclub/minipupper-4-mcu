<script>
  import { marked } from "marked";
  import Eyes from "../Eyes.svelte";

  let { msg, isFirst = false, isLastInGroup = false, respondPermission } = $props();

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
    const ms = ts < 100000000000 ? ts * 1000 : ts;
    if (ms < 978307200000) return "";   /* robot clock not set yet: no time beats a wrong one */
    return new Date(ms).toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" });
  }

  const STAGE_TEXT = {
    sending: "Sending…",
    sent: "Sent",
    relayed: "Relayed to cloud",
    processing: "Thinking…",
    completed: "Done",
  };
  function stageText(entry) {
    if (!entry) return "";
    if (entry.stage === "step") return `Step ${entry.seq}/${entry.total} · ${entry.text}`;
    return STAGE_TEXT[entry.stage] || entry.stage;
  }

  let isUser = $derived(msg.role === "user");
  let isBot = $derived(msg.role === "bot");
  let isSystem = $derived(msg.role === "system");
  let isStep = $derived(msg.role === "step");

  // Timeline helpers for user messages
  let history = $derived(msg.history || []);
  let lastEntry = $derived(history.length > 0 ? history[history.length - 1] : null);
  let hasMultipleEntries = $derived(history.length > 1);
</script>

{#if isStep}
  <!-- Step: one line of what the robot is doing, indented under its avatar -->
  <div class="msg-row msg-row-step">
    <div class="step-line">
      <span class="step-dot"></span>
      <span class="step-label">Step {msg.seq}/{msg.total}</span>
      <span class="step-text">{msg.text}</span>
    </div>
  </div>
{:else if isSystem}
  <div class="msg-row msg-row-center">
    {#if msg.pending && msg.actionId}
      <div class="action-card">
        <p class="action-text">{msg.text}</p>
        <div class="action-btns">
          <button class="approve-btn" onclick={() => respondPermission(msg.actionId, true)}>Approve</button>
          <button class="deny-btn" onclick={() => respondPermission(msg.actionId, false)}>Deny</button>
        </div>
      </div>
    {:else}
      <div class="system-pill">{msg.text}</div>
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
            <svg class="check-icon" width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="3" stroke-linecap="round" stroke-linejoin="round"><polyline points="4 12.5 10 18 20 6"/></svg>
          {/if}
        </div>
      </div>
      {#if lastEntry && lastEntry.stage !== "step" && lastEntry.stage !== "completed"}
        <!-- Delivery state only. Once the robot starts working, its steps
             are rows of their own below, and the reply is the completion. -->
        <div class="timeline">
          <div class="timeline-single">
            <span class="dot dot-live"></span>
            <span>{stageText(lastEntry)}</span>
          </div>
        </div>
      {/if}
    </div>
  </div>
{:else if isBot}
  <div class="msg-row msg-row-bot">
    <span class="bot-avatar eyes-blink" class:bot-avatar-hidden={!isLastInGroup}><Eyes size={13} /></span>
    <div class="bot-wrapper">
      <div class="bot-bubble">
        <div class="bot-content markdown-body">
          {@html renderMarkdown(msg.text)}
        </div>
      </div>
      <div class="bot-meta">
        {#if fmtTime(msg.ts)}<span class="msg-time">{fmtTime(msg.ts)}</span>{/if}
        {#if msg.commands?.length}<span class="cmd-badge">{msg.commands.length} Lua</span>{/if}
      </div>
      {#if msg.commands?.length}
        <details class="cmd-details">
          <summary class="cmd-summary">
            <svg width="10" height="10" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="3" stroke-linecap="round" stroke-linejoin="round"><polyline points="6 9 12 15 18 9"/></svg>
            {msg.commands.length} Lua {msg.commands.length === 1 ? "command" : "commands"}
          </summary>
          <div class="cmd-list">
            {#each msg.commands as cmd, i}
              <div class="cmd-item">
                <div class="cmd-item-header">
                  <span class="cmd-item-num">#{i + 1}</span>
                  <span class="cmd-item-type">{cmd.type}</span>
                </div>
                <pre class="cmd-item-script">{cmd.script}</pre>
              </div>
            {/each}
          </div>
        </details>
      {/if}
      {#if msg.commandResults?.length}
        <div class="cmd-results">
          {#each msg.commandResults as result}
            <div class="cmd-result" class:completed={result.status === 'completed'} class:error={result.status === 'error'} class:timeout={result.status === 'timeout'}>
              <div class="cmd-result-header">
                <span class="cmd-status">
                  {#if result.status === "completed"}✓{:else if result.status === "error"}✗{:else if result.status === "timeout"}⏱{:else}ℹ{/if}
                </span>
                <span class="cmd-status-label">{result.status}</span>
              </div>
              {#if result.output}<pre class="cmd-output">{result.output}</pre>{/if}
            </div>
          {/each}
        </div>
      {/if}
    </div>
  </div>
{/if}

<style>
  .msg-row { display: flex; }
  .msg-row-step   { justify-content: flex-start; padding-left: 40px; }
  .msg-row-center { justify-content: center; }
  .msg-row-user   { justify-content: flex-end; }
  .msg-row-bot    { justify-content: flex-start; align-items: flex-start; gap: 10px; }

  /* ── Step line ───────────────────────── */
  .step-line {
    display: inline-flex;
    align-items: baseline;
    gap: 8px;
    max-width: 100%;
    font-size: 12.5px;
    color: var(--text-2);
    padding: 2px 0;
    line-height: 1.35;
  }
  .step-dot {
    width: 7px; height: 7px; border-radius: 50%;
    background: var(--accent);
    box-shadow: 0 0 0 3px var(--accent-soft);
    flex-shrink: 0;
  }
  .step-label {
    font-size: 10.5px;
    font-weight: 700;
    letter-spacing: 0.06em;
    text-transform: uppercase;
    color: var(--text-3);
    flex-shrink: 0;
  }
  .step-text {
    min-width: 0;
    display: -webkit-box;
    -webkit-line-clamp: 2;
    -webkit-box-orient: vertical;
    overflow: hidden;
    overflow-wrap: anywhere;
  }
  .step-dot { align-self: center; }

  /* ── System ─────────────────────────── */
  .system-pill {
    max-width: 90%;
    font-size: 12px;
    color: var(--text-3);
    padding: 5px 12px;
    border-radius: var(--r-pill);
    background: var(--surface-1);
    border: 1px solid var(--line);
    text-align: center;
  }
  .action-card {
    width: min(100%, 420px);
    padding: 14px 16px;
    border-radius: var(--r-md);
    background: var(--surface-1);
    border: 1px solid var(--accent-line);
    box-shadow: inset 0 1px 0 var(--highlight), 0 0 0 4px var(--accent-soft);
  }
  .action-text { font-size: 13.5px; color: var(--text-1); line-height: 1.45; }
  .action-btns { display: flex; gap: 8px; margin-top: 12px; }
  .approve-btn, .deny-btn {
    flex: 1;
    height: 36px;
    border-radius: var(--r-pill);
    font-family: inherit;
    font-size: 13px;
    font-weight: 700;
    cursor: pointer;
    transition: transform 200ms var(--ease), filter 200ms var(--ease);
  }
  .approve-btn { border: none; background: var(--ok); color: #06210f; }
  .deny-btn { border: 1px solid var(--line-strong); background: transparent; color: var(--text-2); }
  .approve-btn:active, .deny-btn:active { transform: scale(0.97); }

  /* ── User bubble ─────────────────────── */
  .user-wrapper {
    display: flex;
    flex-direction: column;
    align-items: flex-end;
    max-width: min(82%, 560px);
  }
  .user-bubble {
    background: linear-gradient(180deg, #FFEC3F, var(--accent));
    color: var(--accent-ink);
    border-radius: 20px 20px 6px 20px;
    padding: 11px 15px 8px;
    box-shadow: 0 10px 26px rgba(255, 230, 5, 0.14), inset 0 1px 0 rgba(255, 255, 255, 0.45);
  }
  .user-text {
    font-size: 15px;
    font-weight: 500;
    line-height: 1.4;
    overflow-wrap: anywhere;
  }
  .user-meta {
    display: flex;
    align-items: center;
    justify-content: flex-end;
    gap: 5px;
    margin-top: 5px;
    color: rgba(20, 20, 20, 0.55);
  }
  .user-meta .msg-time { color: inherit; }
  .msg-time { font-size: 10.5px; font-weight: 600; letter-spacing: 0.02em; color: var(--text-3); font-variant-numeric: tabular-nums; }
  .sending-dot {
    width: 6px; height: 6px; border-radius: 50%;
    background: rgba(20, 20, 20, 0.55);
    animation: pulse 1.2s infinite;
  }
  @keyframes pulse { 0%, 100% { opacity: 1; } 50% { opacity: 0.3; } }
  .check-icon { color: rgba(20, 20, 20, 0.7); }

  /* ── Timeline under the user bubble ─── */
  .timeline { margin-top: 5px; align-self: flex-end; max-width: 100%; }
  .timeline-single, .timeline-summary, .timeline-entry {
    display: flex;
    align-items: center;
    gap: 6px;
    font-size: 11.5px;
    color: var(--text-3);
    min-width: 0;
  }
  .timeline-single > span:last-child, .timeline-current {
    overflow: hidden; text-overflow: ellipsis; white-space: nowrap; max-width: 60vw;
  }
  .timeline-summary { list-style: none; cursor: pointer; justify-content: flex-end; }
  .timeline-summary::-webkit-details-marker { display: none; }
  .timeline-summary::marker { display: none; content: ""; }
  .timeline-expand {
    display: inline-flex; align-items: center; gap: 3px;
    padding: 1px 7px; border-radius: var(--r-pill);
    background: var(--surface-2); border: 1px solid var(--line);
    font-size: 10.5px; font-weight: 700; color: var(--text-2);
    transition: transform 300ms var(--ease);
  }
  .timeline-details[open] .timeline-expand svg { transform: rotate(180deg); }
  .timeline-entries { margin-top: 4px; display: flex; flex-direction: column; gap: 3px; align-items: flex-end; }

  .dot { width: 6px; height: 6px; border-radius: 50%; flex-shrink: 0; background: var(--text-3); }
  .dot-live { background: var(--accent); box-shadow: 0 0 0 3px var(--accent-soft); animation: pulse 1.2s infinite; }
  .dot-done { background: var(--ok); }
  .dot-dim { opacity: 0.5; }

  /* ── Bot bubble ──────────────────────── */
  .bot-avatar {
    width: 30px;
    height: 30px;
    border-radius: 10px;
    display: inline-flex;
    align-items: center;
    justify-content: center;
    background: var(--surface-1);
    border: 1px solid var(--line);
    box-shadow: inset 0 1px 0 var(--highlight), 0 0 18px rgba(255, 230, 5, 0.16);
    flex-shrink: 0;
    margin-top: 2px;
  }
  .bot-avatar-hidden { visibility: hidden; }
  .bot-wrapper { max-width: min(86%, 620px); min-width: 0; }
  .bot-bubble {
    background: var(--surface-1);
    border: 1px solid var(--line);
    border-radius: 20px 20px 20px 6px;
    padding: 12px 16px;
    box-shadow: inset 0 1px 0 var(--highlight), var(--shadow-card);
  }
  .bot-content { font-size: 15px; color: var(--text-1); line-height: 1.5; overflow-wrap: anywhere; }
  .bot-meta { display: flex; align-items: center; gap: 8px; margin: 5px 0 0 4px; min-height: 14px; }
  .cmd-badge {
    font-size: 10px; font-weight: 700; letter-spacing: 0.06em; text-transform: uppercase;
    padding: 1px 7px; border-radius: var(--r-pill);
    background: var(--surface-2); color: var(--text-3); border: 1px solid var(--line);
  }

  /* ── Expandable commands ─────────────── */
  .cmd-details { margin: 4px 0 0 4px; }
  .cmd-summary {
    display: inline-flex; align-items: center; gap: 5px;
    font-size: 11.5px; font-weight: 600; color: var(--text-3);
    cursor: pointer; list-style: none;
  }
  .cmd-summary::-webkit-details-marker { display: none; }
  .cmd-summary::marker { display: none; content: ""; }
  .cmd-summary:hover { color: var(--text-1); }
  .cmd-details[open] .cmd-summary svg { transform: rotate(180deg); }
  .cmd-list { margin-top: 6px; display: flex; flex-direction: column; gap: 6px; }
  .cmd-item {
    border-radius: var(--r-sm);
    padding: 8px 10px;
    background: var(--surface-1);
    border: 1px solid var(--line);
    border-left: 3px solid var(--accent-line);
  }
  .cmd-item-header { display: flex; align-items: center; gap: 6px; }
  .cmd-item-num { font-size: 10.5px; color: var(--text-3); font-weight: 600; }
  .cmd-item-type {
    font-size: 9.5px; padding: 1px 6px; border-radius: var(--r-pill);
    background: var(--accent-soft); color: var(--accent-ui);
    text-transform: uppercase; font-weight: 700; letter-spacing: 0.06em;
  }
  .cmd-item-script {
    font-size: 11.5px; color: var(--text-2);
    font-family: ui-monospace, "JetBrains Mono", "SF Mono", Menlo, monospace;
    overflow-x: auto; white-space: pre-wrap; margin-top: 4px;
  }

  /* ── Command results ─────────────────── */
  .cmd-results { margin: 6px 0 0 4px; display: flex; flex-direction: column; gap: 4px; }
  .cmd-result {
    border-radius: var(--r-sm);
    padding: 8px 12px;
    background: var(--surface-1);
    border: 1px solid var(--line);
    border-left: 3px solid var(--line-strong);
  }
  .cmd-result.completed { border-left-color: var(--ok); }
  .cmd-result.error     { border-left-color: var(--danger); }
  .cmd-result.timeout   { border-left-color: var(--warn); }
  .cmd-result-header { display: flex; align-items: center; gap: 6px; }
  .cmd-status { font-size: 12px; font-weight: 700; color: var(--text-2); }
  .cmd-result.completed .cmd-status { color: var(--ok); }
  .cmd-result.error .cmd-status { color: var(--danger); }
  .cmd-result.timeout .cmd-status { color: var(--warn); }
  .cmd-status-label {
    font-size: 9.5px; padding: 1px 6px; border-radius: var(--r-pill);
    text-transform: uppercase; font-weight: 700; letter-spacing: 0.06em;
    background: var(--surface-2); color: var(--text-2); border: 1px solid var(--line);
  }
  .cmd-output {
    font-family: ui-monospace, "JetBrains Mono", "SF Mono", Menlo, monospace;
    font-size: 11.5px; color: var(--text-2); margin-top: 4px;
    white-space: pre-wrap; overflow-x: auto;
  }

  :global(.msg-link) { color: var(--accent-ui); text-decoration: underline; text-underline-offset: 2px; }
  :global(.bot-content.markdown-body p) { margin: 0.45em 0; }
  :global(.bot-content.markdown-body p:first-child) { margin-top: 0; }
  :global(.bot-content.markdown-body p:last-child) { margin-bottom: 0; }
  :global(.bot-content.markdown-body ul), :global(.bot-content.markdown-body ol) { padding-left: 1.2em; margin: 0.3em 0; }
  :global(.bot-content.markdown-body code) {
    background: var(--surface-2); border: 1px solid var(--line);
    padding: 1px 5px; border-radius: 6px; font-size: 0.85em;
  }
  :global(.bot-content.markdown-body pre) {
    background: var(--surface-2); border: 1px solid var(--line);
    padding: 10px 12px; border-radius: var(--r-sm); font-size: 12.5px; overflow-x: auto;
  }
  :global(.bot-content.markdown-body pre code) { background: none; border: none; padding: 0; }
  :global(.bot-content.markdown-body blockquote) {
    border-left: 3px solid var(--accent-line); padding-left: 10px; margin: 0.4em 0; color: var(--text-2);
  }
  :global(.bot-content.markdown-body table) { font-size: 12.5px; border-collapse: collapse; }
  :global(.bot-content.markdown-body th), :global(.bot-content.markdown-body td) { border: 1px solid var(--line); padding: 4px 8px; }
</style>
