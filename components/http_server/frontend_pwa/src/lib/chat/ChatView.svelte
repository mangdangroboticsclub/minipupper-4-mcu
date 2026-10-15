<script>
  import { onMount } from "svelte";
  import { marked } from "marked";
  import ChatSidebar from "./ChatSidebar.svelte";
  import ChatMessage from "./ChatMessage.svelte";
  import EmptyChat from "./EmptyChat.svelte";
  import Eyes from "../Eyes.svelte";
  import {
    saveConversation,
    getConversation,
    generateUUID,
  } from "../conversationStore.js";

  let { navigate } = $props();

  // ── UUID helper (matches original) ─────────
  function makeUUID() {
    if (typeof crypto !== "undefined" && typeof crypto.randomUUID === "function") {
      return crypto.randomUUID();
    }
    return "10000000-1000-4000-8000-100000000000".replace(/[018]/g, (c) =>
      (c ^ (crypto.getRandomValues(new Uint8Array(1))[0] & (15 >> (c / 4)))).toString(16)
    );
  }

  // ── Session ID persistence ─────────────────
  const SESSION_STORAGE_KEY = "mpx_active_session_id";

  function restoreSessionId() {
    try {
      const saved = localStorage.getItem(SESSION_STORAGE_KEY);
      if (saved) return saved;
    } catch {}
    return makeUUID();
  }

  // ── State ──────────────────────────────────
  let ws = null;
  let reconnectTimer = null;   // single-flight reconnect guard (prevents socket storms)
  let connected = $state(false);
  let sending = $state(false);
  let sessionId = $state(restoreSessionId());
  let messages = $state([]);
  let inputText = $state("");
  let showSidebar = $state(false);
  let sidebarRefreshKey = $state(0);
  let sessionAckBadge = $state(false);
  let scrollAnchor = $state(null);
  let inputEl = $state(null);
  let pendingActions = $state([]);

  // Live timer for response counter
  let now = $state(Date.now());
  let lastSentTs = $state(null);
  $effect(() => {
    const id = setInterval(() => { now = Date.now(); }, 200);
    return () => clearInterval(id);
  });

  // Accumulator for chunked chat_reply reassembly
  let pendingChunks = {};

  // ── Markdown ───────────────────────────────
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

  // ── Timestamp helpers ─────────────────────
  function normalizeTs(ts) { return ts < 100000000000 ? ts * 1000 : ts; }

  /* The robot stamps its frames from its own clock, which is 1970 until NTP
   * has run -- and NTP needs the Wi-Fi the user may be here to fix. A stamp
   * from before 2001 is that clock, not a time, so the phone's clock is used
   * instead and the "Jan 1, 1970" date pill never appears. */
  const TS_FLOOR_MS = 978307200000;
  function validTs(ts) { return typeof ts === "number" && ts > 0 && normalizeTs(ts) > TS_FLOOR_MS; }
  function stampFor(ts) { return validTs(ts) ? ts : Date.now(); }

  function formatTimestamp(ts) {
    if (!ts) return "";
    const d = new Date(normalizeTs(ts));
    return d.toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" });
  }

  function getMsgTs(msg) {
    if (validTs(msg.ts)) return msg.ts;
    if (msg.history && msg.history.length > 0 && validTs(msg.history[0].ts)) return msg.history[0].ts;
    return null;
  }

  function isNewDay(a, b) {
    const tsA = getMsgTs(a);
    const tsB = getMsgTs(b);
    if (!tsA || !tsB) return false;
    const dA = new Date(normalizeTs(tsA));
    const dB = new Date(normalizeTs(tsB));
    return dA.getDate() !== dB.getDate() || dA.getMonth() !== dB.getMonth() || dA.getFullYear() !== dB.getFullYear();
  }

  function findPrevTimestampMsg(msgs, fromIdx) {
    for (let i = fromIdx; i >= 0; i--) {
      if (getMsgTs(msgs[i])) return msgs[i];
    }
    return msgs[fromIdx];
  }

  function formatDateLabel(msg) {
    const ts = getMsgTs(msg);
    if (!ts) return "";
    const d = new Date(normalizeTs(ts));
    const now = new Date();
    const today = new Date(now.getFullYear(), now.getMonth(), now.getDate());
    const dateDay = new Date(d.getFullYear(), d.getMonth(), d.getDate());
    const diffDays = Math.round((today - dateDay) / 86400000);
    if (diffDays === 0) return "Today";
    if (diffDays === 1) return "Yesterday";
    if (diffDays < 7) {
      const days = ["Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"];
      return days[d.getDay()];
    }
    return d.toLocaleDateString([], { month: "short", day: "numeric", year: "numeric" });
  }

  // ── Auto-save ──────────────────────────────
  let debounceTimer;
  function persist() {
    clearTimeout(debounceTimer);
    debounceTimer = setTimeout(() => {
      if (messages.length > 0) {
        saveConversation(sessionId, { messages });
        sidebarRefreshKey++;
      }
    }, 500);
  }

  $effect(() => {
    const msgs = messages;
    if (msgs.length > 0) persist();
  });

  $effect(() => {
    try { localStorage.setItem(SESSION_STORAGE_KEY, sessionId); } catch {}
  });

  // ── Conversation actions ──────────────────
  function selectConversation(id) {
    const convo = getConversation(id);
    if (!convo) return;
    if (messages.length > 0) saveConversation(sessionId, { messages });
    sidebarRefreshKey++;
    sessionId = convo.id;
    messages = convo.messages || [];
    sending = false;
    if (window.innerWidth < 768) showSidebar = false;
    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({
        type: "session_reset",
        session_id: sessionId,
        ts: Math.floor(Date.now() / 1000),
      }));
    }
  }

  function newConversation() {
    if (messages.length > 0) saveConversation(sessionId, { messages });
    sidebarRefreshKey++;
    const newId = makeUUID();
    sessionId = newId;
    messages = [];
    sending = false;
    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({
        type: "session_reset",
        session_id: newId,
        ts: Math.floor(Date.now() / 1000),
      }));
    }
    inputEl?.focus();
  }

  function handleDelete(id) {
    if (id === sessionId) {
      const newId = makeUUID();
      sessionId = newId;
      messages = [];
      sending = false;
      if (ws && ws.readyState === WebSocket.OPEN) {
        ws.send(JSON.stringify({
          type: "session_reset",
          session_id: newId,
          ts: Math.floor(Date.now() / 1000),
        }));
      }
    }
  }

  // ── Permission response ────────────────────
  function respondPermission(actionId, approved) {
    if (!ws || ws.readyState !== WebSocket.OPEN) return;
    ws.send(JSON.stringify({
      type: "permission_response",
      action_id: actionId,
      approved,
      ts: Math.floor(Date.now() / 1000),
    }));
    pendingActions = pendingActions.filter((a) => a.id !== actionId);
    // Update the matching system message
    for (let i = messages.length - 1; i >= 0; i--) {
      if (messages[i].actionId === actionId) {
        const updated = { ...messages[i] };
        updated.pending = false;
        const prefix = approved ? "✅ Approved: " : "⛔ Denied: ";
        updated.text = prefix + messages[i].text.replace("🔧 OpenClaw requests: ", "");
        messages[i] = updated;
        break;
      }
    }
  }

  // ═══════════════════════════════════════════
  //  WebSocket — ported from working Chat.svelte
  // ═══════════════════════════════════════════
  const WS_URL = `ws://${location.host}/v1/chat/ui`;

  function scheduleReconnect() {
    // Single-flight: never stack more than one pending reconnect.
    if (reconnectTimer) return;
    reconnectTimer = setTimeout(() => {
      reconnectTimer = null;
      if (document.visibilityState !== "hidden") connect();
    }, 3000);
  }

  function connect() {
    // Don't open a second socket if one is already open OR still connecting.
    if (ws && (ws.readyState === WebSocket.OPEN || ws.readyState === WebSocket.CONNECTING)) return;

    // Make sure any previous socket is fully torn down before opening a new one.
    if (ws) {
      ws.onopen = ws.onclose = ws.onerror = ws.onmessage = null;
      try { ws.close(); } catch {}
      ws = null;
    }

    const socket = new WebSocket(WS_URL);
    ws = socket;   // claim the slot immediately so re-entrant calls are guarded

    socket.onopen = () => {
      if (socket !== ws) return;        // stale socket — ignore
      connected = true;
      socket.send(JSON.stringify({
        type: "session_reset",
        session_id: sessionId,
        ts: Math.floor(Date.now() / 1000),
        pwa_build: "stormfix-v14",
      }));
    };

    socket.onclose = () => {
      if (socket !== ws) return;        // a stale/replaced socket closing — do NOT reconnect
      connected = false;
      ws = null;
      scheduleReconnect();
    };

    socket.onerror = () => {
      if (socket === ws) connected = false;
    };

    socket.onmessage = (event) => {
      try {
        const data = JSON.parse(event.data);
        handleMessage(data);
      } catch (e) {
        console.warn("chat WS: unparseable frame dropped", e, event.data);
      }
    };
  }

  function disconnect() {
    if (reconnectTimer) { clearTimeout(reconnectTimer); reconnectTimer = null; }
    if (ws) {
      ws.onopen = ws.onclose = ws.onerror = ws.onmessage = null;
      try { ws.close(); } catch {}
      ws = null;
    }
    connected = false;
  }

  function handleMessage(data) {
    // ── Chunked chat_reply reassembly (array-based like original) ──
    if (data.type === "chat_reply_chunk") {
      const sid = data.session_id;
      if (!pendingChunks[sid]) {
        pendingChunks[sid] = {
          parts: new Array(data.total),
          total: data.total,
          count: 0,
        };
      }
      const acc = pendingChunks[sid];
      if (acc.parts[data.seq] === undefined) {
        acc.parts[data.seq] = data.text;
        acc.count++;
      }
      if (acc.count < acc.total) return;
      const fullText = acc.parts.join("");
      delete pendingChunks[sid];
      handleMessage({ type: "chat_reply", text: fullText, commands: [], session_id: sid });
      return;
    }

    // ── openclaw_action ──
    if (data.type === "openclaw_action") {
      const action = {
        id: data.action_id || "",
        type: data.action_type || "unknown",
        description: data.description || "",
        status: data.status || "completed",
      };
      if (action.status === "pending" && action.id) {
        pendingActions = [...pendingActions, action];
      } else if (action.id) {
        pendingActions = pendingActions.filter((a) => a.id !== action.id);
        const icon = action.status === "approved" ? "✅" : action.status === "denied" ? "⛔" : action.status === "timeout" ? "⏱️" : "ℹ️";
        messages = [...messages, {
          role: "system",
          text: `${icon} OpenClaw action: ${action.type} — ${action.description} [${action.status}]`,
        }];
      }
      if (action.status === "pending") {
        messages = [...messages, {
          role: "system",
          text: `🔧 OpenClaw requests: ${action.description}`,
          actionId: action.id,
          actionType: action.type,
          pending: true,
        }];
      }
      sending = false;
      return;
    }

    // ── step ──
    if (data.type === "step") {
      // Update the most recent user message's history
      const lastIdx = messages.length - 1;
      if (lastIdx >= 0 && messages[lastIdx].role === "user") {
        const updated = { ...messages[lastIdx] };
        if (!updated.history) updated.history = [];
        updated.status = `step_${data.seq}_of_${data.total}`;
        updated.history = [
          ...updated.history,
          { stage: "step", ts: stampFor(data.ts), text: data.text, seq: data.seq, total: data.total },
        ];
        messages[lastIdx] = updated;
      }
      // Also append as a visible step message
      messages = [...messages, {
        role: "step",
        text: data.text,
        seq: data.seq,
        total: data.total,
        ts: stampFor(data.ts),
      }];
      return;
    }

    // ── command_result ──
    if (data.type === "command_result") {
      for (let i = messages.length - 1; i >= 0; i--) {
        if (messages[i].role === "bot") {
          const updated = { ...messages[i] };
          if (!updated.commandResults) updated.commandResults = [];
          updated.commandResults = [...updated.commandResults, {
            script: data.script,
            output: data.output,
            status: data.status,
          }];
          messages[i] = updated;
          break;
        }
      }
      return;
    }

    // ── chat_reply ──
    if (data.type === "chat_reply") {
      if (data.text && data.text.includes("Session reset acknowledged")) {
        sessionAckBadge = true;
        setTimeout(() => { sessionAckBadge = false; }, 3000);
        sending = false;
        return;
      }
      // Mark last user message as completed
      const lastIdx = messages.length - 1;
      if (lastIdx >= 0 && messages[lastIdx].role === "user") {
        const updated = { ...messages[lastIdx] };
        updated.status = "completed";
        if (!updated.history) updated.history = [];
        updated.history = [...updated.history, { stage: "completed", ts: stampFor(data.ts) }];
        updated.reply = data;
        messages[lastIdx] = updated;
      }
      // Dedupe — skip if last bot message has identical text
      const lastMsg = messages.length > 0 ? messages[messages.length - 1] : null;
      const newMsg = {
        role: "bot",
        text: data.text,
        commands: data.commands || [],
        ts: stampFor(data.ts),
      };
      if (lastMsg && lastMsg.role === "bot" && lastMsg.text === data.text) {
        messages = [...messages.slice(0, -1), newMsg];
      } else {
        messages = [...messages, newMsg];
      }
      sending = false;
      return;
    }

    // ── ack ──
    if (data.type === "ack") {
      const lastIdx = messages.length - 1;
      if (lastIdx >= 0 && messages[lastIdx].role === "user") {
        const updated = { ...messages[lastIdx] };
        updated.status = data.status || "sent";
        if (!updated.history) updated.history = [];
        updated.history = [...updated.history, { stage: data.status, ts: Date.now() }];
        messages[lastIdx] = updated;
      }
      return;
    }

    // ── error ──
    if (data.type === "error" && data.text) {
      messages = [...messages, { role: "system", text: `⚠️ ${data.text}` }];
      sending = false;
      return;
    }
  }

  // ── Send message ──────────────────────────
  function sendMessage(text) {
    if (!text.trim() || sending || !ws || ws.readyState !== WebSocket.OPEN) return;

    const nowTs = Date.now();
    messages = [...messages, {
      role: "user",
      text: text.trim(),
      status: "sending",
      history: [{ stage: "sending", ts: nowTs }],
      ts: nowTs,
    }];
    inputText = "";
    sending = true;
    lastSentTs = nowTs;

    ws.send(JSON.stringify({
      type: "user_chat_input",
      text: text.trim(),
      session_id: sessionId,
      ts: Math.floor(nowTs / 1000),
    }));
  }

  // ── Lifecycle ──────────────────────────────
  onMount(() => {
    const convo = getConversation(sessionId);
    if (convo && convo.messages.length > 0) {
      messages = convo.messages;
    }
    connect();
    return () => disconnect();
  });

  // ── Auto-scroll ────────────────────────────
  $effect(() => {
    if (messages.length > 0 && scrollAnchor) {
      requestAnimationFrame(() => {
        scrollAnchor.scrollIntoView({ behavior: "smooth", block: "end" });
      });
    }
  });

  // ── Auto-focus input ──────────────────────
  $effect(() => {
    if (!sending && inputEl && connected) {
      inputEl.focus();
    }
  });

  /* A suggestion chip on the empty screen fills the box rather than sending:
   * the person still gets to read what they are about to say to a robot. */
  function useSuggestion(text) {
    inputText = text;
    inputEl?.focus();
  }

  let statusLabel = $derived(sending ? "Thinking" : connected ? "Online" : "Offline");
</script>

<div class="chat-root">
  <!-- Sidebar (slide animation) -->
  <div class="sidebar-backdrop" class:visible={showSidebar} onclick={() => (showSidebar = false)}></div>
  <div class="sidebar-panel" class:open={showSidebar}>
    <ChatSidebar
      activeId={sessionId}
      refreshKey={sidebarRefreshKey}
      onSelect={selectConversation}
      onNew={newConversation}
      onDelete={handleDelete}
    />
  </div>

  <!-- Main chat area -->
  <div class="chat-main">
    <!-- Top bar -->
    <div class="chat-topbar">
      <div class="topbar-inner">
        <button
          class="tb-btn"
          class:active={showSidebar}
          onclick={() => (showSidebar = !showSidebar)}
          aria-label="Conversation history"
          title="Conversation history"
        >
          <svg width="17" height="17" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
            <circle cx="12" cy="12" r="10"/>
            <polyline points="12 6 12 12 16 14"/>
          </svg>
          <span>History</span>
        </button>
        <button class="tb-btn" onclick={newConversation} aria-label="New conversation" title="New conversation">
          <svg width="17" height="17" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round">
            <line x1="12" y1="5" x2="12" y2="19"/>
            <line x1="5" y1="12" x2="19" y2="12"/>
          </svg>
          <span>New</span>
        </button>

        <span class="status-pill" class:online={connected && !sending} class:busy={sending} class:offline={!connected}>
          {#if sessionAckBadge}
            <span class="status-dot"></span>Session reset
          {:else}
            <span class="status-dot"></span>{statusLabel}
          {/if}
        </span>
      </div>
    </div>

    <!-- Messages or Empty state -->
    {#if messages.length === 0 && !sending}
      <EmptyChat onSuggest={useSuggestion} />
    {:else}
      <div class="messages-area">
        <div class="messages-scroll">
          {#each messages as msg, i}
            {#if formatDateLabel(msg) && (i === 0 || (i > 0 && isNewDay(findPrevTimestampMsg(messages, i - 1), msg)))}
              <div class="date-delineator"><span>{formatDateLabel(msg)}</span></div>
            {/if}
            <ChatMessage
              {msg}
              isFirst={i === 0}
              isLastInGroup={i === messages.length - 1 || messages[i + 1]?.role !== msg.role}
              {respondPermission}
            />
          {/each}

          {#if sending && lastSentTs}
            <div class="thinking">
              <span class="thinking-avatar eyes-blink"><Eyes size={13} /></span>
              <span class="thinking-dots"><i></i><i></i><i></i></span>
              <span class="thinking-timer">{Math.floor((now - lastSentTs) / 1000)}s</span>
            </div>
          {/if}

          <div bind:this={scrollAnchor}></div>
        </div>
      </div>
    {/if}

    <!-- Composer -->
    <div class="composer">
      <div class="composer-shell" class:disabled={!connected}>
        <input
          bind:this={inputEl}
          bind:value={inputText}
          onkeydown={(e) => e.key === "Enter" && sendMessage(inputText)}
          placeholder={connected ? "Ask Mini Pupper…" : "Reconnecting…"}
          disabled={sending || !connected}
          class="chat-input"
        />
        <button
          class="send-btn"
          onclick={() => sendMessage(inputText)}
          disabled={!inputText.trim() || sending || !connected}
          aria-label="Send"
        >
          {#if sending}
            <span class="spinner"></span>
          {:else}
            <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.6" stroke-linecap="round" stroke-linejoin="round">
              <line x1="12" y1="19" x2="12" y2="5"/>
              <polyline points="5 12 12 5 19 12"/>
            </svg>
          {/if}
        </button>
      </div>
      <p class="composer-hint">Mini Pupper can move, draw on its screen and run skills from chat.</p>
    </div>
  </div>
</div>

<style>
  .chat-root {
    display: flex;
    height: 100%;
    background: var(--bg);
    position: relative;
  }

  /* ── Sidebar (slide) ──────────────────── */
  .sidebar-backdrop {
    position: fixed;
    inset: 0;
    background: rgba(0, 0, 0, 0.55);
    z-index: 40;
    opacity: 0;
    pointer-events: none;
    transition: opacity 320ms var(--ease);
  }
  .sidebar-backdrop.visible { opacity: 1; pointer-events: auto; }

  .sidebar-panel {
    position: fixed;
    top: 0;
    left: 0;
    bottom: 0;
    width: min(300px, 86vw);
    z-index: 50;
    transform: translateX(-104%);
    transition: transform 420ms var(--ease);
    box-shadow: 24px 0 60px rgba(0, 0, 0, 0.45);
  }
  .sidebar-panel.open { transform: translateX(0); }

  /* ── Main chat area ──────────────────── */
  .chat-main {
    flex: 1;
    display: flex;
    flex-direction: column;
    min-width: 0;
  }

  /* ── Top bar ──────────────────────────── */
  .chat-topbar {
    flex-shrink: 0;
    padding: 8px 12px 6px;
  }
  .topbar-inner {
    max-width: 760px;
    margin: 0 auto;
    display: flex;
    align-items: center;
    gap: 6px;
  }
  .tb-btn {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    height: 34px;
    padding: 0 12px 0 10px;
    border: 1px solid var(--line);
    border-radius: var(--r-pill);
    background: var(--surface-1);
    color: var(--text-2);
    font-size: 12.5px;
    font-weight: 600;
    cursor: pointer;
    box-shadow: inset 0 1px 0 var(--highlight);
    transition: color 200ms var(--ease), border-color 200ms var(--ease), background-color 200ms var(--ease), transform 200ms var(--ease);
  }
  .tb-btn:hover { color: var(--text-1); border-color: var(--line-strong); }
  .tb-btn:active { transform: scale(0.97); }
  .tb-btn.active { color: var(--accent-ui); border-color: var(--accent-line); background: var(--accent-soft); }

  .status-pill {
    margin-left: auto;
    display: inline-flex;
    align-items: center;
    gap: 7px;
    height: 30px;
    padding: 0 12px 0 10px;
    border-radius: var(--r-pill);
    background: var(--surface-1);
    border: 1px solid var(--line);
    font-size: 11.5px;
    font-weight: 600;
    letter-spacing: 0.01em;
    color: var(--text-2);
  }
  .status-dot { width: 7px; height: 7px; border-radius: 50%; background: var(--text-3); }
  .status-pill.online .status-dot { background: var(--ok); box-shadow: 0 0 0 3px rgba(69, 212, 131, 0.18); }
  .status-pill.busy .status-dot { background: var(--accent); box-shadow: 0 0 0 3px var(--accent-soft); animation: status-pulse 1.1s ease-in-out infinite; }
  .status-pill.offline { color: var(--danger); }
  .status-pill.offline .status-dot { background: var(--danger); }
  @keyframes status-pulse { 0%, 100% { opacity: 1; } 50% { opacity: 0.35; } }

  /* ── Date delineator ──────────────────── */
  .date-delineator {
    display: flex;
    justify-content: center;
    margin: 6px 0 2px;
  }
  .date-delineator span {
    font-size: 10.5px;
    font-weight: 600;
    letter-spacing: 0.08em;
    text-transform: uppercase;
    color: var(--text-3);
    padding: 4px 10px;
    border-radius: var(--r-pill);
    background: var(--surface-1);
    border: 1px solid var(--line);
  }

  /* ── Messages ─────────────────────────── */
  .messages-area {
    flex: 1;
    overflow-y: auto;
    min-height: 0;
    overscroll-behavior: contain;
  }
  .messages-scroll {
    max-width: 760px;
    margin: 0 auto;
    padding: 6px 14px 18px;
    display: flex;
    flex-direction: column;
    gap: 10px;
  }

  /* ── Thinking ─────────────────────────── */
  .thinking {
    display: flex;
    align-items: center;
    gap: 10px;
    padding-left: 2px;
  }
  .thinking-avatar {
    width: 30px;
    height: 30px;
    border-radius: 10px;
    display: inline-flex;
    align-items: center;
    justify-content: center;
    background: var(--surface-1);
    border: 1px solid var(--line);
    box-shadow: inset 0 1px 0 var(--highlight), 0 0 18px rgba(255, 230, 5, 0.18);
    flex-shrink: 0;
  }
  .thinking-dots {
    display: inline-flex;
    align-items: center;
    gap: 5px;
    height: 34px;
    padding: 0 14px;
    border-radius: 18px 18px 18px 6px;
    background: var(--surface-1);
    border: 1px solid var(--line);
  }
  .thinking-dots i {
    width: 6px; height: 6px; border-radius: 50%;
    background: var(--text-3);
    animation: dot-bounce 1.2s var(--ease) infinite;
  }
  .thinking-dots i:nth-child(2) { animation-delay: 0.15s; }
  .thinking-dots i:nth-child(3) { animation-delay: 0.30s; }
  @keyframes dot-bounce {
    0%, 60%, 100% { transform: translateY(0); opacity: 0.45; }
    30% { transform: translateY(-4px); opacity: 1; background: var(--accent); }
  }
  .thinking-timer {
    font-size: 11px;
    font-weight: 600;
    color: var(--text-3);
    font-variant-numeric: tabular-nums;
  }

  /* ── Composer ────────────────────────── */
  .composer {
    flex-shrink: 0;
    padding: 6px 14px 10px;
    background: linear-gradient(180deg, transparent, var(--bg) 40%);
  }
  .composer-shell {
    max-width: 760px;
    margin: 0 auto;
    display: flex;
    align-items: center;
    gap: 8px;
    padding: 5px 5px 5px 18px;
    border-radius: 28px;
    background: var(--surface-1);
    border: 1px solid var(--line-strong);
    box-shadow: inset 0 1px 0 var(--highlight), var(--shadow-card);
    transition: border-color 200ms var(--ease), box-shadow 200ms var(--ease);
  }
  .composer-shell:focus-within {
    border-color: var(--accent-line);
    box-shadow: inset 0 1px 0 var(--highlight), 0 0 0 4px var(--accent-soft), var(--shadow-card);
  }
  .composer-shell.disabled { opacity: 0.7; }

  .chat-input {
    flex: 1;
    border: none;
    outline: none;
    background: transparent;
    font-family: inherit;
    font-size: 15px;
    color: var(--text-1);
    padding: 10px 0;
    min-width: 0;
  }
  .chat-input::placeholder { color: var(--text-3); }
  .chat-input:disabled { opacity: 0.6; }

  .send-btn {
    width: 42px;
    height: 42px;
    border-radius: 50%;
    border: none;
    background: var(--accent);
    color: var(--accent-ink);
    display: inline-flex;
    align-items: center;
    justify-content: center;
    cursor: pointer;
    flex-shrink: 0;
    box-shadow: 0 6px 16px rgba(255, 230, 5, 0.25);
    transition: transform 200ms var(--ease), background-color 200ms var(--ease), box-shadow 200ms var(--ease), color 200ms var(--ease);
  }
  .send-btn:hover:not(:disabled) { transform: translateY(-1px); }
  .send-btn:active:not(:disabled) { transform: scale(0.95); }
  .send-btn:disabled {
    background: var(--surface-3);
    color: var(--text-3);
    box-shadow: none;
    cursor: default;
  }
  .composer-hint {
    max-width: 760px;
    margin: 8px auto 0;
    text-align: center;
    font-size: 11px;
    color: var(--text-3);
  }

  .spinner {
    width: 16px;
    height: 16px;
    border: 2px solid var(--line-strong);
    border-top-color: var(--accent);
    border-radius: 50%;
    animation: spin 0.8s linear infinite;
  }
  @keyframes spin { to { transform: rotate(360deg); } }
</style>
