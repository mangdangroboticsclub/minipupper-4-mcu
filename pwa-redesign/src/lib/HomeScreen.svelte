<script>
  import { colors } from "./colors.js";
  import Controller from "./Controller.svelte";
  import ActionIcon from "./ActionIcon.svelte";
  import FileViewer from "./FileViewer.svelte";
  import UploadView from "./UploadView.svelte";
  import Calibration from "./Calibration.svelte";
  import WiFiSetup from "./WiFiSetup.svelte";
  import APModeConfig from "./APModeConfig.svelte";
  import ActionAssignment from "./ActionAssignment.svelte";
  import AddActionView from "./AddActionView.svelte";
  import ChatView from "./chat/ChatView.svelte";
  import SkillsMarketplace from "./SkillsMarketplace.svelte";
  import SkillsManagement from "./SkillsManagement.svelte";
  import PupperEyes from "./PupperEyes.svelte";

  let { network } = $props();

  const YELLOW = colors.mpx.primary;

  let activeTab = $state("home");

  let isFullscreen = $state(false);
  let isEditMode = $state(false);
  let showActionAssignment = $state(false);
  let actionAssignments = $state({});

  // Lock body scroll when fullscreen
  $effect(() => {
    document.body.style.overflow = isFullscreen ? "hidden" : "";
    return () => { document.body.style.overflow = ""; };
  });

  let activeView = $state("main"); // "main" | "upload"

  // ── Connection status (from app.svelte) ────────────────────
  let linkState = $derived(network?.linkState ?? "checking");
  let online = $derived(linkState === "online");
  let connectionMode = $derived(network?.connectionMode ?? "sta");
  let showConnDetails = $state(false);

  let connLabel = $derived(
    online ? "Online" : linkState === "offline" ? "Offline" : "Connecting"
  );
  let connSub = $derived.by(() => {
    if (!online) return null;
    if (connectionMode === "ap") return "AP";
    if (connectionMode === "sta") return network?.staSsid || "Wi‑Fi";
    if (connectionMode === "dev") return "Dev";
    return null;
  });
  let connIp = $derived(
    connectionMode === "sta" && network?.staIp ? network.staIp
    : connectionMode === "dev" ? (network?.staIp || network?.apIp || "—")
    : network?.apIp || "—"
  );
  function fmtAgo(ts) {
    if (!ts) return "never";
    const s = Math.round((Date.now() - ts) / 1000);
    if (s < 5) return "just now";
    if (s < 60) return `${s}s ago`;
    const m = Math.round(s / 60);
    return m < 60 ? `${m}m ago` : `${Math.round(m / 60)}h ago`;
  }

  let showAllActions = $state(false);
  let showAddAction = $state(false);
  let customActions = $state([]);
  let pinnedActions = $state([]);
  let showGaitSection = $state(false);
  let longPressTarget = $state(null);
  let showAllAdjust = $state(false);
  let selectedAdjust = $state(0);
  let showCalibration = $state(false);
  let showWiFi = $state(false);
  let showAPConfig = $state(false);
  let showFiles = $state(false);
  let adjustDraggingId = $state(null);
  let adjustDragBarEl = $state(null);

  let adjustProps = $state([
    // Each entry has a `key` mapping to the /v1/robot/config API parameter name
    { label: 'Period', key: 'period',    value: 80,  min: 0,   max: 200, unit: 'ms',  isCenter: false },
    { label: 'Height', key: 'height',    value: 70,  min: 0,   max: 150, unit: 'mm',  isCenter: false },
    { label: 'Lift',   key: 'up_height', value: 10,  min: 0,   max: 50,  unit: 'mm',  isCenter: false },
    { label: 'Stride', key: 'stride',    value: 10,  min: 0,   max: 50,  unit: 'mm',  isCenter: false },
    { label: 'Tilt',   key: 'tilt',      value: 0,   min: -30, max: 30,  unit: '°',   isCenter: true },
  ]);

  const adjustPerPage = 3;
  function adjustTotalPages() {
    return Math.ceil(adjustProps.length / adjustPerPage);
  }

  // ── Robot config API ────────────────────────────────────────
  async function fetchConfig() {
    try {
      const res = await fetch("/v1/robot/status");
      if (res.ok) {
        const data = await res.json();
        const cfg = data.config || {};
        for (const prop of adjustProps) {
          if (cfg[prop.key] !== undefined) prop.value = cfg[prop.key];
        }
      }
    } catch { /* robot may be unreachable — keep defaults */ }
  }

  async function saveConfig() {
    const body = {};
    for (const prop of adjustProps) body[prop.key] = prop.value;
    try {
      await fetch("/v1/robot/config", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(body),
      });
    } catch { /* ignore transient errors */ }
  }

  // ── localStorage persistence ──────────────────────────────
  const STORAGE_KEY_ASSIGNMENTS = "mpx_action_assignments";
  const STORAGE_KEY_CUSTOM = "mpx_custom_actions";
  const STORAGE_KEY_PINNED = "mpx_pinned_actions";

  $effect(() => {
    try { const s = localStorage.getItem(STORAGE_KEY_ASSIGNMENTS); if (s) actionAssignments = JSON.parse(s); } catch {}
    try { const s = localStorage.getItem(STORAGE_KEY_CUSTOM); if (s) customActions = JSON.parse(s); } catch {}
    try { const s = localStorage.getItem(STORAGE_KEY_PINNED); if (s) pinnedActions = JSON.parse(s); } catch {}
  });

  $effect(() => {
    if (typeof window === "undefined") return;
    const a = JSON.stringify(actionAssignments);
    const c = JSON.stringify(customActions);
    const p = JSON.stringify(pinnedActions);
    try { localStorage.setItem(STORAGE_KEY_ASSIGNMENTS, a); } catch {}
    try { localStorage.setItem(STORAGE_KEY_CUSTOM, c); } catch {}
    try { localStorage.setItem(STORAGE_KEY_PINNED, p); } catch {}
  });

  $effect(() => { fetchConfig(); });

  const actions = [
    // ── Movement gaits ──
    { id: "advance",  label: "Forward",      emoji: "🚶", color: colors.mpx.action_green, category: "gait" },
    { id: "back",     label: "Backward",     emoji: "🔙", color: colors.mpx.action_green, category: "gait" },
    { id: "left",     label: "Strafe Left",  emoji: "◀️",  color: colors.mpx.action_green, category: "gait" },
    { id: "right",    label: "Strafe Right", emoji: "▶️",  color: colors.mpx.action_green, category: "gait" },
    { id: "turnL",    label: "Turn Left",    emoji: "↺",  color: "#0E8C8C",              category: "gait" },
    { id: "turnR",    label: "Turn Right",   emoji: "↻",  color: "#0E8C8C",              category: "gait" },
    { id: "stanford", label: "Trot",         emoji: "🐕", color: "#14A37F",              category: "gait" },
    { id: "step",     label: "Step",         emoji: "🦶", color: "#14A37F",              category: "gait" },
    { id: "testspeed",label: "Speed Test",   emoji: "⚡",  color: "#D14949",              category: "gait" },
    { id: "jump",     label: "Jump",         emoji: "🤸", color: "#D97A29",              category: "gait" },
    { id: "jumpfwd",  label: "Jump Fwd",     emoji: "🏃", color: "#C4901A",              category: "gait" },
    // ── Leg lifts ──
    { id: "flegR",    label: "Lift FR",      emoji: "🦵", color: "#5FAD41",              category: "gait" },
    { id: "flegL",    label: "Lift FL",      emoji: "🦵", color: "#5FAD41",              category: "gait" },
    { id: "blegR",    label: "Lift RR",      emoji: "🦵", color: "#5FAD41",              category: "gait" },
    { id: "blegL",    label: "Lift RL",      emoji: "🦵", color: "#5FAD41",              category: "gait" },
    // ── Height ──
    { id: "heightup",   label: "Height Up",   emoji: "⬆️",  color: "#D96098",              category: "gait" },
    { id: "heightdown", label: "Height Down", emoji: "⬇️",  color: "#D96098",              category: "gait" },
    // ── Diagonal moves ──
    { id: "moveLF", label: "Diag FL", emoji: "↗️",  color: "#4A8BC2",              category: "gait" },
    { id: "moveRF", label: "Diag FR", emoji: "↖️",  color: "#4A8BC2",              category: "gait" },
    { id: "moveLB", label: "Diag BL", emoji: "↘️",  color: "#4A8BC2",              category: "gait" },
    { id: "moveRB", label: "Diag BR", emoji: "↙️",  color: "#4A8BC2",              category: "gait" },
    // ── Skills (all built-in actions are gaits) ──
    { id: "sit",      label: "Sit",          emoji: "🪑", color: colors.mpx.action_blue,   category: "gait" },
    { id: "stretch",  label: "Stretch",      emoji: "🧘", color: "#D96098",               category: "gait" },
    { id: "twerk",    label: "Twerk",        emoji: "💃", color: colors.mpx.action_purple, category: "gait" },
    { id: "roll",     label: "Roll",         emoji: "🔄", color: "#14A37F",               category: "gait" },
    { id: "pitch",    label: "Pitch",        emoji: "📐", color: "#14A37F",               category: "gait" },
    { id: "balance",  label: "Balance",      emoji: "⚖️",  color: "#14A37F",               category: "gait" },
    { id: "init",     label: "Init",         emoji: "🏁", color: "#7C7F7C",               category: "gait" },
    { id: "none",     label: "Stop",         emoji: "⏹️",  color: "#D14949",               category: "gait" },
    { id: "frontkick",label: "Front Kick",   emoji: "🦶", color: "#C44569",               category: "gait" },
    { id: "wiggle",   label: "Wiggle",       emoji: "🐕", color: "#C44569",               category: "gait" },
    { id: "wiggleL",  label: "Wiggle ◀",     emoji: "◀️",  color: "#C44569",               category: "gait" },
    { id: "wiggleR",  label: "Wiggle ▶",     emoji: "▶️",  color: "#C44569",               category: "gait" },
    { id: "buttshrug",  label: "Butt Shrug",   emoji: "🍑", color: "#C44569",             category: "gait" },
    { id: "buttshrugL", label: "Shrug ◀",      emoji: "◀️",  color: "#C44569",             category: "gait" },
    { id: "buttshrugR", label: "Shrug ▶",      emoji: "▶️",  color: "#C44569",             category: "gait" },
    { id: "bowback",  label: "Bow",          emoji: "🙇", color: "#AD5FBF",               category: "gait" },
    { id: "bodycycle",label: "Body Circle",  emoji: "🔄", color: "#AD5FBF",               category: "gait" },
    { id: "headellipse",label: "Head Circle", emoji: "🔄", color: "#AD5FBF",              category: "gait" },
  ];

  const tabs = [
    { id: "home",     label: "Home" },
    { id: "skills",   label: "Skills" },
    { id: "store",    label: "Store" },
    { id: "chat",     label: "Chat" },
    { id: "settings", label: "Settings" },
  ];

  // ── Adjust slider interaction ──
  function adjustPct(prop) {
    return ((prop.value - prop.min) / (prop.max - prop.min)) * 100;
  }
  function adjustSliderValueFromPointer(prop, clientX) {
    if (!adjustDragBarEl) return prop.value;
    const rect = adjustDragBarEl.getBoundingClientRect();
    const pct = (clientX - rect.left) / rect.width;
    return Math.round(prop.min + pct * (prop.max - prop.min));
  }
  function adjustStartDrag(e, propIdx) {
    const prop = adjustProps[propIdx];
    if (!prop) return;
    const handle = e.currentTarget;
    handle.setPointerCapture(e.pointerId);
    adjustDragBarEl = handle.parentElement;
    adjustDraggingId = propIdx;
    const val = adjustSliderValueFromPointer(prop, e.clientX);
    adjustProps[propIdx].value = Math.max(prop.min, Math.min(prop.max, val));
  }
  function adjustOnDragMove(e) {
    if (adjustDraggingId === null) return;
    const prop = adjustProps[adjustDraggingId];
    if (!prop) return;
    const val = adjustSliderValueFromPointer(prop, e.clientX);
    adjustProps[adjustDraggingId].value = Math.max(prop.min, Math.min(prop.max, val));
  }
  function adjustEndDrag() {
    adjustDraggingId = null;
    adjustDragBarEl = null;
    saveConfig();
  }
  function adjustStep(propIdx, delta) {
    const prop = adjustProps[propIdx];
    if (!prop) return;
    adjustProps[propIdx].value = Math.max(prop.min, Math.min(prop.max, prop.value + delta));
    saveConfig();
  }
  function adjustCurrentPage() {
    return Math.floor(selectedAdjust / adjustPerPage);
  }

  let allActions = $derived([...actions, ...customActions.map(c => ({ ...c, category: c.category || "custom" }))]);
  let gaitActions = $derived(allActions.filter(a => a.category === "gait"));
  let skillActions = $derived(allActions.filter(a => a.category !== "gait"));

  function handleAddAction(newAction) {
    customActions = [...customActions, newAction];
  }

  async function sendGait(mode) {
    try {
      await fetch("/v1/robot/gait", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ mode }),
      });
    } catch { /* ignore */ }
  }

  function togglePin(actionId) {
    if (pinnedActions.includes(actionId)) {
      pinnedActions = pinnedActions.filter(id => id !== actionId);
    } else {
      pinnedActions = [...pinnedActions, actionId];
    }
  }
  function isPinned(actionId) { return pinnedActions.includes(actionId); }

  let pinnedActionObjects = $derived(
    pinnedActions.map(id => allActions.find(a => a.id === id)).filter(Boolean)
  );
  const pinnedPerPage = 8;
  let pinnedTotalPages = $derived(Math.max(1, Math.ceil(pinnedActionObjects.length / pinnedPerPage)));

  // ── Fullscreen API ──────────────────────────────────────
  async function toggleFullscreen() {
    if (isFullscreen) {
      isFullscreen = false;
      try { if (document.fullscreenElement) await document.exitFullscreen(); } catch {}
    } else {
      isFullscreen = true;
      try { await document.documentElement.requestFullscreen(); } catch { /* CSS fullscreen fallback */ }
    }
  }

  $effect(() => {
    function onFsChange() {
      if (!document.fullscreenElement && isFullscreen) isFullscreen = false;
    }
    document.addEventListener('fullscreenchange', onFsChange);
    return () => document.removeEventListener('fullscreenchange', onFsChange);
  });

  function selectTab(id) {
    activeTab = id;
    showConnDetails = false;
  }
</script>

<div class="home-root" style="--yellow: {YELLOW}">
  <!-- ═══ Top Header ═══ -->
  <header class="top-header" class:hidden={isFullscreen}>
    <div class="header-left">
      <!-- Connection status pill -->
      <button
        class="conn-pill"
        class:online
        class:offline={linkState === "offline"}
        class:checking={linkState === "checking"}
        class:open={showConnDetails}
        onclick={() => (showConnDetails = !showConnDetails)}
        aria-expanded={showConnDetails}
        aria-label="Robot connection: {connLabel}"
      >
        <span class="conn-dot"></span>
        <span class="conn-label">{connLabel}</span>
        {#if connSub}
          <span class="conn-sub">{connSub}</span>
        {/if}
      </button>
    </div>

    <div class="header-center">
      <img class="header-logo" src="/md.svg" alt="MangDang" />
    </div>

    <div class="header-right">
      <span class="ws-indicator" class:on={network?.wsConnected} title={network?.wsConnected ? "Agent channel open" : "Agent channel closed"} aria-hidden="true">
        <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round">
          <path d="M21 15a2 2 0 0 1-2 2H7l-4 4V5a2 2 0 0 1 2-2h14a2 2 0 0 1 2 2z"/>
        </svg>
      </span>
    </div>

    <!-- Connection details popover -->
    {#if showConnDetails}
      <div class="conn-scrim" role="presentation" onclick={() => (showConnDetails = false)}></div>
      <div class="conn-popover" role="dialog" aria-label="Connection details">
        <div class="conn-pop-head">
          <span class="conn-dot big" class:online class:offline={linkState === "offline"} class:checking={linkState === "checking"}></span>
          <div>
            <div class="conn-pop-title">{online ? "Robot online" : linkState === "offline" ? "Robot unreachable" : "Looking for robot…"}</div>
            <div class="conn-pop-sub">{online ? "All controls are live." : linkState === "offline" ? "Check that you're on the robot's network." : "Waiting for the first response."}</div>
          </div>
        </div>
        <dl class="conn-kv">
          <dt>Mode</dt><dd>{connectionMode === "ap" ? "Direct hotspot (AP)" : connectionMode === "sta" ? `Wi‑Fi · ${network?.staSsid || "—"}` : connectionMode === "dev" ? "Dev proxy" : "—"}</dd>
          <dt>Robot IP</dt><dd class="mono">{connIp}</dd>
          <dt>Latency</dt><dd>{network?.latencyMs != null ? `${network.latencyMs} ms` : "—"}</dd>
          <dt>Agent channel</dt><dd>{network?.wsConnected ? "Open" : "Closed"}</dd>
          <dt>Last seen</dt><dd>{fmtAgo(network?.lastSeenAt)}</dd>
        </dl>
      </div>
    {/if}
  </header>

  <!-- ═══ Content Area ═══ -->
  <main class="content" class:fullscreen={isFullscreen}>
    {#if activeTab === "home" && showAddAction}
      <AddActionView
        onSave={handleAddAction}
        onBack={() => { showAddAction = false; }}
      />
    {:else if activeTab === "home" && showActionAssignment}
      <ActionAssignment
        assignments={actionAssignments}
        customActions={customActions}
        onAddAction={() => { showAddAction = true; }}
        onDeleteAction={(actionId) => {
          customActions = customActions.filter(c => c.id !== actionId);
          pinnedActions = pinnedActions.filter(id => id !== actionId);
        }}
        onSave={(updated) => { actionAssignments = updated; }}
        onBack={() => { showActionAssignment = false; isEditMode = false; }}
      />
    {:else if activeTab === "home" && showAllActions}
      <!-- ═══ Full-page All Actions View ═══ -->
      <div class="sub-page">
        <header class="sub-header">
          <button class="back-btn" aria-label="Back" onclick={() => showAllActions = false}>
            <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
              <line x1="19" y1="12" x2="5" y2="12"/><polyline points="12 19 5 12 12 5"/>
            </svg>
          </button>
          <h2 class="sub-title">All Actions</h2>
          <div style="width:32px"></div>
        </header>
        <div class="sub-scroll">
          {#if gaitActions.length > 0}
            <div class="all-actions-category">
              <button class="all-actions-cat-header" onclick={() => showGaitSection = !showGaitSection} aria-label="Toggle gait section">
                <h3 class="all-actions-cat-title">Gait <span class="cat-count">{gaitActions.length}</span></h3>
                <svg class="collapse-chevron" class:collapsed={!showGaitSection} width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
                  <polyline points="6 9 12 15 18 9"/>
                </svg>
              </button>
              {#if showGaitSection}
              <div class="actions-full-grid">
                {#each gaitActions as action}
                  <div class="action-grid-item all-action-item" style="--action-color: {action.color}" role="button" tabindex="0">
                    <div class="action-grid-icon"><ActionIcon id={action.id} emoji={action.emoji} /></div>
                    <span class="action-grid-label">{action.label}</span>
                    <button
                      class="pin-trigger-btn"
                      class:pinned={isPinned(action.id)}
                      onclick={(e) => { e.stopPropagation(); togglePin(action.id); }}
                      aria-label={isPinned(action.id) ? `Unpin ${action.label}` : `Pin ${action.label}`}
                    >
                      <svg width="12" height="12" viewBox="0 0 24 24" fill={isPinned(action.id) ? 'currentColor' : 'none'} stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
                        <line x1="12" y1="17" x2="12" y2="22"/>
                        <path d="M5 17h14v-2.5c0-.83-.67-1.5-1.5-1.5h-11c-.83 0-1.5.67-1.5 1.5V17z"/>
                        <path d="M12 14V7l-2.33-4.67h4.66L12 7z"/>
                      </svg>
                    </button>
                  </div>
                {/each}
              </div>
              {/if}
            </div>
          {/if}

          {#if skillActions.length > 0}
            <div class="all-actions-category">
              <h3 class="all-actions-cat-title" style="padding: 8px 16px;">Custom <span class="cat-count">{skillActions.length}</span></h3>
              <div class="actions-full-grid">
                {#each skillActions as action}
                  <div class="action-grid-item all-action-item" style="--action-color: {action.color}" role="button" tabindex="0">
                    <div class="action-grid-icon"><ActionIcon id={action.id} emoji={action.emoji} /></div>
                    <span class="action-grid-label">{action.label}</span>
                    <button
                      class="pin-trigger-btn"
                      class:pinned={isPinned(action.id)}
                      onclick={(e) => { e.stopPropagation(); togglePin(action.id); }}
                      aria-label={isPinned(action.id) ? `Unpin ${action.label}` : `Pin ${action.label}`}
                    >
                      <svg width="12" height="12" viewBox="0 0 24 24" fill={isPinned(action.id) ? 'currentColor' : 'none'} stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
                        <line x1="12" y1="17" x2="12" y2="22"/>
                        <path d="M5 17h14v-2.5c0-.83-.67-1.5-1.5-1.5h-11c-.83 0-1.5.67-1.5 1.5V17z"/>
                        <path d="M12 14V7l-2.33-4.67h4.66L12 7z"/>
                      </svg>
                    </button>
                    <button
                      class="delete-action-btn"
                      onclick={(e) => {
                        e.stopPropagation();
                        customActions = customActions.filter(c => c.id !== action.id);
                        pinnedActions = pinnedActions.filter(id => id !== action.id);
                      }}
                      aria-label="Delete {action.label}"
                    >
                      <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="white" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
                        <polyline points="3 6 5 6 21 6"/>
                        <path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2"/>
                      </svg>
                    </button>
                  </div>
                {/each}
              </div>
            </div>
          {/if}

          <div class="all-actions-add-wrap">
            <button class="action-grid-item add-item" onclick={() => { showAllActions = false; showAddAction = true; }}>
              <div class="action-grid-icon add-icon-box">
                <svg width="28" height="28" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
                  <line x1="12" y1="5" x2="12" y2="19"/><line x1="5" y1="12" x2="19" y2="12"/>
                </svg>
              </div>
              <span class="action-grid-label">Add</span>
            </button>
          </div>
        </div>
      </div>
    {:else if activeTab === "home" && showAllAdjust}
      <!-- ═══ Full-page All Adjust View ═══ -->
      <div class="sub-page">
        <header class="sub-header">
          <button class="back-btn" aria-label="Back" onclick={() => showAllAdjust = false}>
            <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
              <line x1="19" y1="12" x2="5" y2="12"/><polyline points="12 19 5 12 12 5"/>
            </svg>
          </button>
          <h2 class="sub-title">All Adjust</h2>
          <div style="width:32px"></div>
        </header>
        <div class="sub-scroll all-adjust-list">
          {#each adjustProps as prop, idx}
            <div class="all-adjust-item">
              <div class="all-adjust-item-header">
                <span class="all-adjust-item-label">{prop.label}</span>
                <span class="all-adjust-item-value">{prop.value}{prop.unit}</span>
              </div>
              <div class="adjust-slider-controls">
                <button class="adjust-step-btn" aria-label="Decrease {prop.label}" onclick={() => adjustStep(idx, -1)}>
                  <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round"><line x1="5" y1="12" x2="19" y2="12"/></svg>
                </button>
                <div class="adjust-slider-bar-wrap">
                  <div class="adjust-slider-bar">
                    <div class="adjust-slider-fill" style="width: {adjustPct(prop)}%"></div>
                    <div class="adjust-slider-handle current" style="left: {adjustPct(prop)}%"
                      role="slider" tabindex="0"
                      aria-label={prop.label}
                      aria-valuemin={prop.min}
                      aria-valuemax={prop.max}
                      aria-valuenow={prop.value}
                      onpointerdown={(e) => adjustStartDrag(e, idx)}
                      onpointermove={adjustOnDragMove}
                      onpointerup={adjustEndDrag}
                      onpointercancel={adjustEndDrag}
                    ></div>
                  </div>
                </div>
                <button class="adjust-step-btn" aria-label="Increase {prop.label}" onclick={() => adjustStep(idx, 1)}>
                  <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round"><line x1="12" y1="5" x2="12" y2="19"/><line x1="5" y1="12" x2="19" y2="12"/></svg>
                </button>
              </div>
            </div>
          {/each}
        </div>
      </div>
    {:else if activeTab === "home"}
      <div class="home-content" class:fullscreen={isFullscreen}>
        <!-- ═══ Controller Panel ═══ -->
        <div
          class="controller-panel"
          class:fullscreen={isFullscreen}
          class:offline={!online}
        >
          <div class="controller-stage">
            <Controller
              assignments={actionAssignments}
              catalog={allActions}
              {online}
              fullscreen={isFullscreen}
              onToggleFullscreen={toggleFullscreen}
              onEdit={() => { isEditMode = true; showActionAssignment = true; }}
            />
          </div>

          {#if !online}
            <div class="offline-banner" role="status">
              <span class="conn-dot offline"></span>
              {linkState === "offline" ? "Robot unreachable — commands won't be delivered" : "Connecting to robot…"}
            </div>
          {/if}
        </div>

        <!-- ═══ Scrollable cards ═══ -->
        <div class="home-content-scroll">
          <!-- Quick Actions -->
          <div class="section-card">
            <div class="section-card-header">
              <h2 class="section-card-title">Quick Actions</h2>
              <button class="see-all-btn" onclick={() => showAllActions = true}>See all</button>
            </div>
            <div class="action-grid-scroll-wrap">
              {#if pinnedActionObjects.length === 0}
                <button class="no-pinned-hint" onclick={() => showAllActions = true}>
                  <span class="hint-plus">+</span>
                  <span class="hint-text">Pin your favourite actions from <b>See all</b> for one-tap access.</span>
                </button>
              {:else}
              <div class="action-grid-scroll">
                {#each { length: pinnedTotalPages } as _, pageIdx}
                  <div class="action-grid-page">
                    {#each pinnedActionObjects.slice(pageIdx * pinnedPerPage, pageIdx * pinnedPerPage + pinnedPerPage) as action}
                      <div
                        class="action-grid-item pinned-item"
                        class:show-trash={longPressTarget === action.id}
                        style="--action-color: {action.color}"
                        role="button"
                        tabindex="0"
                        onclick={() => sendGait(action.id)}
                        onpointerdown={(e) => {
                          const timer = setTimeout(() => { longPressTarget = action.id; }, 600);
                          e.currentTarget._longPressTimer = timer;
                        }}
                        onpointerup={(e) => { clearTimeout(e.currentTarget._longPressTimer); longPressTarget = null; }}
                        onpointerleave={(e) => { clearTimeout(e.currentTarget._longPressTimer); longPressTarget = null; }}
                        onpointercancel={(e) => { clearTimeout(e.currentTarget._longPressTimer); longPressTarget = null; }}
                      >
                        <div class="action-grid-icon"><ActionIcon id={action.id} emoji={action.emoji} /></div>
                        <span class="action-grid-label">{action.label}</span>
                        <button
                          class="unpin-trash-btn"
                          onclick={(e) => { e.stopPropagation(); togglePin(action.id); }}
                          aria-label="Remove {action.label}"
                        >
                          <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="white" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
                            <polyline points="3 6 5 6 21 6"/>
                            <path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2"/>
                          </svg>
                        </button>
                      </div>
                    {/each}
                  </div>
                {/each}
              </div>
              {#if pinnedTotalPages > 1}
              <div class="page-dots">
                {#each { length: pinnedTotalPages } as _, i}
                  <span class="page-dot" class:active={i === 0}></span>
                {/each}
              </div>
              {/if}
              {/if}
            </div>
          </div>

          <!-- Adjust -->
          <div class="section-card">
            <div class="section-card-header">
              <h2 class="section-card-title">Adjust</h2>
              <button class="see-all-btn" onclick={() => showAllAdjust = true}>See all</button>
            </div>

            <div class="adjust-scroll-wrap">
              <div class="adjust-scroll">
                {#each { length: adjustTotalPages() } as _, pageIdx}
                  <div class="adjust-page">
                    {#each adjustProps.slice(pageIdx * adjustPerPage, pageIdx * adjustPerPage + adjustPerPage) as prop, i}
                      {@const idx = pageIdx * adjustPerPage + i}
                      <button
                        class="adjust-selector-btn"
                        class:active={selectedAdjust === idx}
                        onclick={() => selectedAdjust = idx}
                      >
                        <div class="adjust-selector-circle" class:active={selectedAdjust === idx}>
                          <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
                            {#if prop.key === "period"}
                              <circle cx="12" cy="12" r="10"/><polyline points="12 6 12 12 16 14"/>
                            {:else if prop.key === "height"}
                              <line x1="12" y1="5" x2="12" y2="19"/><polyline points="9 8 12 5 15 8"/><polyline points="9 16 12 19 15 16"/>
                            {:else if prop.key === "up_height"}
                              <line x1="12" y1="5" x2="12" y2="19"/><polyline points="9 8 12 5 15 8"/>
                            {:else if prop.key === "stride"}
                              <polyline points="17 8 21 12 17 16"/><polyline points="7 8 3 12 7 16"/><line x1="21" y1="12" x2="3" y2="12"/>
                            {:else}
                              <polyline points="22 12 18 12 15 21 9 3 6 12 2 12"/>
                            {/if}
                          </svg>
                        </div>
                        <span class="adjust-selector-label">{prop.label}</span>
                        <span class="adjust-selector-val">{prop.value}{prop.unit}</span>
                      </button>
                    {/each}
                  </div>
                {/each}
              </div>
              {#if adjustTotalPages() > 1}
              <div class="page-dots">
                {#each { length: adjustTotalPages() } as _, i}
                  <span class="page-dot" class:active={i === adjustCurrentPage()}></span>
                {/each}
              </div>
              {/if}
            </div>

            <!-- Slider for selected property -->
            <div class="adjust-slider-section">
              <div class="adjust-slider-header">
                <span class="adjust-slider-prop-label">{adjustProps[selectedAdjust].label}</span>
                <span class="adjust-slider-value">{adjustProps[selectedAdjust].value}<small>{adjustProps[selectedAdjust].unit}</small></span>
              </div>

              <div class="adjust-slider-controls">
                <button class="adjust-step-btn" aria-label="Decrease" onclick={() => adjustStep(selectedAdjust, -1)}>
                  <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round"><line x1="5" y1="12" x2="19" y2="12"/></svg>
                </button>

                <div class="adjust-slider-bar-wrap">
                  <div class="adjust-slider-bar">
                    <div class="adjust-slider-fill" style="width: {adjustPct(adjustProps[selectedAdjust])}%"></div>
                    <div class="adjust-slider-handle current" style="left: {adjustPct(adjustProps[selectedAdjust])}%"
                      role="slider" tabindex="0"
                      aria-label={adjustProps[selectedAdjust].label}
                      aria-valuemin={adjustProps[selectedAdjust].min}
                      aria-valuemax={adjustProps[selectedAdjust].max}
                      aria-valuenow={adjustProps[selectedAdjust].value}
                      onpointerdown={(e) => adjustStartDrag(e, selectedAdjust)}
                      onpointermove={adjustOnDragMove}
                      onpointerup={adjustEndDrag}
                      onpointercancel={adjustEndDrag}
                    ></div>
                  </div>
                </div>

                <button class="adjust-step-btn" aria-label="Increase" onclick={() => adjustStep(selectedAdjust, 1)}>
                  <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round"><line x1="12" y1="5" x2="12" y2="19"/><line x1="5" y1="12" x2="19" y2="12"/></svg>
                </button>
              </div>
            </div>
          </div>

          <div class="content-spacer"></div>
        </div>
      </div>
    {:else if activeTab === "skills"}
      <SkillsManagement embedded onNavigate={(to) => { if (to === "store") activeTab = "store"; }} />
    {:else if activeTab === "store"}
      <SkillsMarketplace embedded onNavigate={(to) => { if (to === "skills") activeTab = "skills"; }} />
    {:else if activeTab === "chat"}
      <ChatView navigate={(to) => { if (to === "home") activeTab = "home"; }} {network} />
    {:else if activeTab === "settings"}
      {#if showWiFi}
        <WiFiSetup onNavigate={() => showWiFi = false} {network} />
      {:else if showAPConfig}
        <APModeConfig onNavigate={() => showAPConfig = false} {network} />
      {:else if showCalibration}
        <Calibration onBack={() => showCalibration = false} />
      {:else if showFiles}
        {#if activeView === "upload"}
          <div class="upload-page"><div class="upload-inner"><UploadView onBack={() => activeView = "main"} /></div></div>
        {:else}
          <FileViewer onNavigate={(view) => { if (view === "upload") activeView = "upload"; else if (view === "home") showFiles = false; }} />
        {/if}
      {:else}
        <div class="settings-page">
          <header class="settings-header">
            <h2 class="settings-title">Settings</h2>
          </header>
          <div class="settings-scroll">
            <div class="settings-group-label">Network</div>
            <div class="settings-list">
              <button class="settings-item" onclick={() => showWiFi = true}>
                <div class="settings-item-icon">
                  <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
                    <path d="M5 12.55a11 11 0 0 1 14.08 0"/><path d="M1.42 9a16 16 0 0 1 21.16 0"/><path d="M8.53 16.11a6 6 0 0 1 6.95 0"/><circle cx="12" cy="20" r="1" fill="currentColor"/>
                  </svg>
                </div>
                <div class="settings-item-content">
                  <span class="settings-item-label">Wi‑Fi</span>
                  <span class="settings-item-desc">{network?.staState === "connected" ? `Connected to ${network?.staSsid}` : "Join a wireless network"}</span>
                </div>
                <svg class="settings-chevron" width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><polyline points="9 18 15 12 9 6"/></svg>
              </button>
              <button class="settings-item" onclick={() => showAPConfig = true}>
                <div class="settings-item-icon">
                  <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
                    <path d="M12 2a10 10 0 0 1 10 10"/><path d="M12 6a6 6 0 0 1 6 6"/><path d="M12 10a2 2 0 0 1 2 2"/><circle cx="12" cy="20" r="1.5" fill="currentColor"/>
                  </svg>
                </div>
                <div class="settings-item-content">
                  <span class="settings-item-label">Hotspot (AP)</span>
                  <span class="settings-item-desc">{network?.apSsid || "MPX-Dog"} · {network?.apIp || "192.168.2.1"}</span>
                </div>
                <svg class="settings-chevron" width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><polyline points="9 18 15 12 9 6"/></svg>
              </button>
            </div>

            <div class="settings-group-label">Robot</div>
            <div class="settings-list">
              <button class="settings-item" onclick={() => showCalibration = true}>
                <div class="settings-item-icon">
                  <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
                    <circle cx="12" cy="12" r="3"/><path d="M12 2v4M12 18v4M2 12h4M18 12h4"/><path d="M4.9 4.9l2.8 2.8M16.3 16.3l2.8 2.8M4.9 19.1l2.8-2.8M16.3 7.7l2.8-2.8"/>
                  </svg>
                </div>
                <div class="settings-item-content">
                  <span class="settings-item-label">Calibration</span>
                  <span class="settings-item-desc">Fine-tune each leg's servo offsets</span>
                </div>
                <svg class="settings-chevron" width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><polyline points="9 18 15 12 9 6"/></svg>
              </button>
              <button class="settings-item" onclick={() => { showFiles = true; activeView = "main"; }}>
                <div class="settings-item-icon">
                  <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
                    <path d="M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z"/>
                  </svg>
                </div>
                <div class="settings-item-content">
                  <span class="settings-item-label">Files</span>
                  <span class="settings-item-desc">Browse and upload files on the robot</span>
                </div>
                <svg class="settings-chevron" width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><polyline points="9 18 15 12 9 6"/></svg>
              </button>
            </div>

            <div class="settings-footer">
              <span>MPX‑Dog PWA</span>
              <span class="mono">{connIp}</span>
            </div>
          </div>
        </div>
      {/if}
    {/if}
  </main>

  <!-- ═══ Bottom Navigation ═══ -->
  <nav class="bottom-nav" class:hidden={isFullscreen} aria-label="Primary">
    {#each tabs as tab}
      <button
        class="nav-item"
        class:active={activeTab === tab.id}
        onclick={() => selectTab(tab.id)}
        aria-current={activeTab === tab.id ? "page" : undefined}
      >
        <div class="nav-icon">
          {#if tab.id === "home"}
            <span class="nav-eyes"><PupperEyes awake size="22px" color="currentColor" accent={activeTab === "home" ? YELLOW : "#fff"} blink={activeTab === "home"} /></span>
          {:else if tab.id === "skills"}
            <!-- Skills: four tiles, one filled -->
            <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
              <rect x="3" y="3" width="7" height="7" rx="2"/><rect x="14" y="3" width="7" height="7" rx="2"/><rect x="3" y="14" width="7" height="7" rx="2"/>
              <path d="M17.5 14v7M14 17.5h7"/>
            </svg>
          {:else if tab.id === "store"}
            <!-- Store: shopping bag -->
            <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
              <path d="M6 2L3 6v14a2 2 0 0 0 2 2h14a2 2 0 0 0 2-2V6l-3-4z"/><line x1="3" y1="6" x2="21" y2="6"/><path d="M16 10a4 4 0 0 1-8 0"/>
            </svg>
          {:else if tab.id === "chat"}
            <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
              <path d="M21 15a2 2 0 0 1-2 2H7l-4 4V5a2 2 0 0 1 2-2h14a2 2 0 0 1 2 2z"/>
            </svg>
          {:else if tab.id === "settings"}
            <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
              <circle cx="12" cy="12" r="3"/>
              <path d="M19.4 15a1.65 1.65 0 0 0 .33 1.82l.06.06a2 2 0 0 1-2.83 2.83l-.06-.06a1.65 1.65 0 0 0-1.82-.33 1.65 1.65 0 0 0-1 1.51V21a2 2 0 0 1-4 0v-.09A1.65 1.65 0 0 0 9 19.4a1.65 1.65 0 0 0-1.82.33l-.06.06a2 2 0 0 1-2.83-2.83l.06-.06A1.65 1.65 0 0 0 4.68 15a1.65 1.65 0 0 0-1.51-1H3a2 2 0 0 1 0-4h.09A1.65 1.65 0 0 0 4.6 9a1.65 1.65 0 0 0-.33-1.82l-.06-.06a2 2 0 0 1 2.83-2.83l.06.06A1.65 1.65 0 0 0 9 4.68a1.65 1.65 0 0 0 1-1.51V3a2 2 0 0 1 4 0v.09a1.65 1.65 0 0 0 1 1.51 1.65 1.65 0 0 0 1.82-.33l.06-.06a2 2 0 0 1 2.83 2.83l-.06.06A1.65 1.65 0 0 0 19.4 9a1.65 1.65 0 0 0 1.51 1H21a2 2 0 0 1 0 4h-.09a1.65 1.65 0 0 0-1.51 1z"/>
            </svg>
          {/if}
        </div>
        <span class="nav-label">{tab.label}</span>
      </button>
    {/each}
  </nav>
</div>

<style>
  .home-root {
    position: absolute;
    inset: 0;
    background: var(--mpx-surface);
    display: flex;
    flex-direction: column;
    animation: home-in var(--dur-slow) var(--ease-out);
  }
  @keyframes home-in { from { opacity: 0; } to { opacity: 1; } }

  .mono { font-family: 'JetBrains Mono', ui-monospace, monospace; }

  /* ════════════════════════════════════════
     TOP HEADER
     ════════════════════════════════════════ */
  .top-header {
    position: relative;
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: calc(var(--safe-top) + 8px) 12px 8px;
    background: var(--yellow);
    flex-shrink: 0;
    z-index: 30;
    min-height: 48px;
  }
  .top-header.hidden { display: none; }

  .header-left, .header-right { display: flex; align-items: center; gap: 6px; min-width: 72px; }
  .header-right { justify-content: flex-end; }
  .header-center { position: absolute; left: 50%; top: 50%; transform: translate(-50%, -50%); padding-top: var(--safe-top); }
  .header-logo { height: 18px; width: auto; display: block; }

  /* ── Connection pill ──────────────────── */
  .conn-pill {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    padding: 5px 10px 5px 8px;
    border-radius: var(--r-pill);
    border: none;
    background: rgba(0, 0, 0, 0.1);
    color: var(--mpx-ink);
    font-size: 0.7rem;
    font-weight: 800;
    letter-spacing: 0.02em;
    cursor: pointer;
    max-width: 46vw;
    transition: background var(--dur-fast), transform var(--dur-fast);
  }
  .conn-pill:active { transform: scale(0.97); }
  .conn-pill.open { background: rgba(0, 0, 0, 0.18); }
  .conn-pill.offline { background: #000; color: #fff; }

  .conn-dot {
    width: 8px; height: 8px;
    border-radius: 50%;
    background: var(--mpx-muted-2);
    flex-shrink: 0;
    box-shadow: 0 0 0 0 rgba(34,197,94,0.5);
  }
  .conn-dot.big { width: 12px; height: 12px; }
  .online .conn-dot, .conn-dot.online { background: var(--mpx-ok); animation: ping 2.4s ease-out infinite; }
  .offline .conn-dot, .conn-dot.offline { background: var(--mpx-err); }
  .checking .conn-dot, .conn-dot.checking { background: var(--mpx-warn); animation: dot-pulse 1.2s ease-in-out infinite; }
  @keyframes ping {
    0% { box-shadow: 0 0 0 0 rgba(34,197,94,0.45); }
    70% { box-shadow: 0 0 0 6px rgba(34,197,94,0); }
    100% { box-shadow: 0 0 0 0 rgba(34,197,94,0); }
  }
  @keyframes dot-pulse { 0%,100% { opacity: 1; } 50% { opacity: 0.3; } }

  .conn-label { white-space: nowrap; }
  .conn-sub {
    font-weight: 600;
    opacity: 0.7;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
    padding-left: 6px;
    border-left: 1px solid rgba(0,0,0,0.2);
  }
  .conn-pill.offline .conn-sub { border-color: rgba(255,255,255,0.3); }

  .ws-indicator {
    width: 28px; height: 28px;
    border-radius: 50%;
    display: grid; place-items: center;
    color: rgba(0,0,0,0.3);
    transition: color var(--dur-med), background var(--dur-med);
  }
  .ws-indicator.on { color: var(--mpx-ink); background: rgba(0,0,0,0.08); }

  .conn-scrim { position: fixed; inset: 0; z-index: 25; }
  .conn-popover {
    position: absolute;
    top: calc(100% + 6px);
    left: 10px;
    width: min(320px, calc(100vw - 20px));
    background: var(--mpx-surface);
    border-radius: var(--r-lg);
    box-shadow: var(--shadow-float);
    padding: 14px;
    z-index: 26;
    animation: pop-in var(--dur-med) var(--ease-spring);
    transform-origin: top left;
  }
  @keyframes pop-in { from { opacity: 0; transform: scale(0.94) translateY(-4px); } to { opacity: 1; transform: none; } }
  .conn-pop-head { display: flex; align-items: center; gap: 12px; margin-bottom: 12px; }
  .conn-pop-title { font-size: 0.92rem; font-weight: 800; color: var(--mpx-ink); }
  .conn-pop-sub { font-size: 0.74rem; color: var(--mpx-muted); margin-top: 1px; }
  .conn-kv {
    display: grid;
    grid-template-columns: auto 1fr;
    gap: 6px 14px;
    font-size: 0.78rem;
    background: var(--mpx-surface-2);
    border-radius: var(--r-md);
    padding: 10px 12px;
  }
  .conn-kv dt { color: var(--mpx-muted); font-weight: 600; }
  .conn-kv dd { color: var(--mpx-ink); font-weight: 600; text-align: right; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }

  /* ════════════════════════════════════════
     CONTENT
     ════════════════════════════════════════ */
  .content {
    flex: 1;
    min-height: 0;
    overflow-y: auto;
    overflow-x: hidden;
    display: flex;
    flex-direction: column;
    position: relative;
    container-type: size;
  }
  .content.fullscreen { overflow: hidden; }

  .home-content {
    display: flex;
    flex-direction: column;
    flex: 1;
    min-height: 0;
  }

  .home-content-scroll {
    padding: 12px 14px calc(16px + var(--safe-bottom));
    display: flex;
    flex-direction: column;
    gap: 12px;
    flex-shrink: 0;
  }

  /* ── Wide layouts: controller left, cards right ── */
  @media (min-width: 900px) and (min-aspect-ratio: 5/4) {
    .home-content {
      flex-direction: row;
      height: 100%;
      overflow: hidden;
    }
    .controller-panel {
      flex: 1 1 auto;
      min-height: 0 !important;
      height: 100%;
    }
    .home-content-scroll {
      width: clamp(320px, 32vw, 420px);
      flex-shrink: 0;
      overflow-y: auto;
      height: 100%;
      border-left: 1px solid var(--mpx-line);
      background: var(--mpx-surface);
    }
  }

  /* ════════════════════════════════════════
     CONTROLLER PANEL
     ════════════════════════════════════════ */
  .controller-panel {
    position: relative;
    flex: 0 0 auto;
    /* exactly the visible content area (fixed, so the pad is fitted *into*
       it rather than growing it); cards scroll in from below */
    height: calc(100dvh - 140px);
    /* In tall viewports a full-height pad would leave a dead band between
       the actions and the sticks, so cap the panel at what the stacked pad
       needs: sticks are 42% of the width, the action diamond is 3 buttons
       tall, and bumpers / labels / gaps / paddings add ~242px. Cards peek
       in below. (Mirrors the budget in Controller.svelte.) */
    height: min(100cqh, calc(42cqw + 3 * clamp(44px, 14cqw, 68px) + 242px));
    display: flex;
    flex-direction: column;
    padding: 12px 14px 14px;
    background: var(--yellow);
    overflow: hidden;
  }
  @media (min-width: 900px) and (min-aspect-ratio: 5/4) {
    /* row layout: the pad has no intrinsic size, so grow to fill */
    .controller-panel { flex: 1 1 auto; min-width: 0; height: 100%; }
  }

  /* radial highlight */
  .controller-panel::after {
    content: '';
    position: absolute;
    inset: 0;
    background: radial-gradient(ellipse 85% 60% at 50% 35%, rgba(255,255,255,0.28) 0%, rgba(255,255,255,0.08) 40%, transparent 70%);
    pointer-events: none;
  }

  .controller-stage {
    position: relative;
    flex: 1;
    min-height: 0;
    display: flex;
    z-index: 1;
  }

  .controller-panel.fullscreen {
    position: fixed;
    inset: 0;
    z-index: 100;
    height: 100dvh;
    padding: calc(var(--safe-top) + 10px) 14px calc(var(--safe-bottom) + 14px);
  }

  .offline-banner {
    position: absolute;
    left: 50%;
    bottom: 12px;
    transform: translateX(-50%);
    z-index: 21;
    display: inline-flex;
    align-items: center;
    gap: 8px;
    padding: 7px 12px;
    border-radius: var(--r-pill);
    background: rgba(0,0,0,0.82);
    color: #fff;
    font-size: 0.72rem;
    font-weight: 700;
    white-space: nowrap;
    max-width: calc(100% - 28px);
    overflow: hidden;
    text-overflow: ellipsis;
    -webkit-backdrop-filter: blur(6px);
    backdrop-filter: blur(6px);
    animation: pop-in var(--dur-med) var(--ease-spring);
  }

  /* ════════════════════════════════════════
     SECTION CARDS
     ════════════════════════════════════════ */
  .section-card {
    background: var(--mpx-surface-2);
    border-radius: var(--r-lg);
    padding: 16px;
    flex-shrink: 0;
  }
  .section-card-header {
    display: flex;
    align-items: center;
    justify-content: space-between;
    margin-bottom: 12px;
  }
  .section-card-title { font-size: 0.98rem; font-weight: 800; color: var(--mpx-ink); }
  .see-all-btn {
    border: none;
    background: rgba(0,0,0,0.05);
    color: var(--mpx-ink);
    font-size: 0.74rem;
    font-weight: 700;
    cursor: pointer;
    padding: 5px 10px;
    border-radius: var(--r-pill);
    transition: background var(--dur-fast);
  }
  .see-all-btn:hover { background: rgba(0,0,0,0.1); }

  /* ── Action Grid (swipeable) ───────────── */
  .action-grid-scroll-wrap { display: flex; flex-direction: column; gap: 10px; }
  .action-grid-scroll {
    display: flex;
    overflow-x: auto;
    scroll-snap-type: x mandatory;
    -webkit-overflow-scrolling: touch;
    scrollbar-width: none;
    margin: -4px 0;
    padding: 4px 0;
  }
  .action-grid-scroll::-webkit-scrollbar { display: none; }
  .action-grid-page {
    display: grid;
    grid-template-columns: repeat(4, 1fr);
    gap: 12px;
    flex: 0 0 100%;
    scroll-snap-align: start;
  }

  .action-grid-item {
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: 6px;
    border: none;
    background: transparent;
    cursor: pointer;
    padding: 4px 0;
    transition: transform var(--dur-fast) var(--ease-out);
    position: relative;
  }
  .action-grid-item:active { transform: scale(0.92); }
  .action-grid-icon {
    width: clamp(52px, 13vw, 64px);
    aspect-ratio: 1;
    border-radius: var(--r-md);
    background: var(--action-color);
    display: grid; place-items: center;
    color: #fff;
    box-shadow: inset 0 -3px 0 rgba(0,0,0,0.12);
    transition: box-shadow var(--dur-fast);
  }
  .action-grid-item:hover .action-grid-icon { box-shadow: inset 0 -3px 0 rgba(0,0,0,0.12), 0 4px 12px rgba(0,0,0,0.16); }
  .action-grid-icon :global(.action-emoji) { font-size: 1.8rem; }
  .action-grid-label {
    font-size: 0.68rem;
    font-weight: 700;
    color: var(--mpx-ink);
    text-align: center;
    line-height: 1.2;
    max-width: 100%;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }

  .unpin-trash-btn {
    position: absolute;
    top: -6px; right: -6px;
    width: 24px; height: 24px;
    border-radius: 50%;
    background: #d14949;
    border: 2px solid #fff;
    display: none; place-items: center;
    cursor: pointer;
    z-index: 5;
    box-shadow: 0 2px 6px rgba(0,0,0,0.2);
  }
  .pinned-item.show-trash .unpin-trash-btn { display: grid; }

  .no-pinned-hint {
    width: 100%;
    display: flex;
    align-items: center;
    gap: 12px;
    text-align: left;
    font-size: 0.78rem;
    color: var(--mpx-muted);
    padding: 12px;
    border: 1.5px dashed var(--mpx-muted-2);
    border-radius: var(--r-md);
    background: transparent;
    cursor: pointer;
    line-height: 1.4;
  }
  .no-pinned-hint b { color: var(--mpx-ink); }
  .hint-text { flex: 1; min-width: 0; line-height: 1.4; }
  .hint-plus {
    width: 36px; height: 36px;
    border-radius: var(--r-sm);
    background: var(--yellow);
    color: var(--mpx-ink);
    display: grid; place-items: center;
    font-size: 1.3rem; font-weight: 800;
    flex-shrink: 0;
  }

  /* ── All Actions: pin & delete buttons ── */
  .pin-trigger-btn {
    position: absolute;
    top: 0; left: 2px;
    width: 22px; height: 22px;
    border-radius: 50%;
    background: rgba(0,0,0,0.45);
    border: none;
    display: grid; place-items: center;
    cursor: pointer;
    color: #fff;
    opacity: 0.7;
    transition: opacity var(--dur-fast), background var(--dur-fast);
    padding: 0;
  }
  .pin-trigger-btn.pinned { opacity: 1; background: var(--yellow); color: var(--mpx-ink); }
  .all-action-item:hover .pin-trigger-btn { opacity: 1; }

  .delete-action-btn {
    position: absolute;
    top: -4px; right: -4px;
    width: 20px; height: 20px;
    border-radius: 50%;
    background: #d14949;
    border: 1.5px solid #fff;
    display: grid; place-items: center;
    cursor: pointer;
    opacity: 0;
    transition: opacity var(--dur-fast);
    padding: 0;
    box-shadow: 0 2px 4px rgba(0,0,0,0.2);
  }
  .all-action-item:hover .delete-action-btn { opacity: 1; }

  .all-actions-cat-header {
    display: flex; align-items: center; justify-content: space-between;
    width: 100%;
    border: none; background: none;
    cursor: pointer;
    padding: 8px 16px;
    margin: 6px 0 4px;
    border-radius: var(--r-sm);
  }
  .all-actions-cat-header:hover { background: rgba(0,0,0,0.04); }
  .collapse-chevron { color: var(--mpx-muted); transition: transform var(--dur-med); }
  .collapse-chevron.collapsed { transform: rotate(-90deg); }
  .all-actions-cat-title {
    font-size: 0.72rem; font-weight: 800; color: var(--mpx-muted);
    text-transform: uppercase; letter-spacing: 0.08em;
    display: flex; align-items: center; gap: 8px;
  }
  .cat-count { background: var(--mpx-surface-3); border-radius: var(--r-pill); padding: 1px 7px; font-size: 0.66rem; }
  .all-actions-category { margin-bottom: 8px; }
  .all-actions-add-wrap { padding: 0 16px; margin-top: 8px; }
  .actions-full-grid {
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(78px, 1fr));
    gap: 14px 10px;
    padding: 4px 12px;
  }
  .add-icon-box { background: transparent !important; border: 2.5px dashed var(--mpx-muted-2); color: var(--mpx-muted-2); box-shadow: none !important; }

  .page-dots { display: flex; justify-content: center; gap: 6px; }
  .page-dot { width: 6px; height: 6px; border-radius: 50%; background: #ddd; transition: background var(--dur-med); }
  .page-dot.active { background: var(--yellow); }

  /* ── Sub pages ─────────────────────────── */
  .sub-page {
    flex: 1;
    display: flex;
    flex-direction: column;
    background: var(--mpx-surface);
    min-height: 0;
  }
  .sub-header {
    display: flex; align-items: center; justify-content: space-between;
    padding: 12px 14px;
    flex-shrink: 0;
    border-bottom: 1px solid var(--mpx-line);
  }
  .sub-title { font-size: 1.05rem; font-weight: 800; color: var(--mpx-ink); }
  .sub-scroll { flex: 1; overflow-y: auto; padding: 8px 0 calc(24px + var(--safe-bottom)); }
  .back-btn {
    width: 32px; height: 32px;
    display: grid; place-items: center;
    border: none;
    background: var(--mpx-surface-2);
    border-radius: 50%;
    cursor: pointer;
    color: var(--mpx-ink);
    flex-shrink: 0;
    transition: background var(--dur-fast);
  }
  .back-btn:hover { background: var(--mpx-surface-3); }

  /* ── Adjust Selectors ─────────────────── */
  .adjust-scroll-wrap { display: flex; flex-direction: column; gap: 10px; margin-bottom: 14px; }
  .adjust-scroll {
    display: flex;
    overflow-x: auto;
    scroll-snap-type: x mandatory;
    -webkit-overflow-scrolling: touch;
    scrollbar-width: none;
    margin: -4px 0;
    padding: 4px 0;
  }
  .adjust-scroll::-webkit-scrollbar { display: none; }
  .adjust-page {
    display: grid;
    grid-template-columns: repeat(3, 1fr);
    flex: 0 0 100%;
    scroll-snap-align: start;
    gap: 8px;
  }
  .adjust-selector-btn {
    display: flex; flex-direction: column; align-items: center; gap: 6px;
    border: none; background: transparent;
    cursor: pointer;
    padding: 8px 4px;
    border-radius: var(--r-md);
    transition: transform var(--dur-fast), background var(--dur-fast);
  }
  .adjust-selector-btn:active { transform: scale(0.95); }
  .adjust-selector-btn.active { background: rgba(0,0,0,0.04); }
  .adjust-selector-circle {
    width: 50px; height: 50px;
    border-radius: 50%;
    background: var(--mpx-surface-3);
    display: grid; place-items: center;
    color: var(--mpx-muted);
    transition: background var(--dur-med), color var(--dur-med), box-shadow var(--dur-med);
  }
  .adjust-selector-circle.active { background: var(--yellow); color: var(--mpx-ink); box-shadow: 0 4px 12px rgba(0,0,0,0.14); }
  .adjust-selector-label { font-size: 0.7rem; font-weight: 700; color: var(--mpx-ink); }
  .adjust-selector-val { font-size: 0.66rem; font-weight: 600; color: var(--mpx-muted); font-variant-numeric: tabular-nums; }

  .adjust-slider-section { display: flex; flex-direction: column; gap: 8px; }
  .adjust-slider-header { display: flex; align-items: baseline; justify-content: space-between; padding: 0 2px; }
  .adjust-slider-prop-label { font-size: 0.84rem; font-weight: 700; color: var(--mpx-ink); }
  .adjust-slider-value { font-size: 1.05rem; font-weight: 800; color: var(--mpx-ink); font-variant-numeric: tabular-nums; }
  .adjust-slider-value small { font-size: 0.7rem; font-weight: 700; color: var(--mpx-muted); margin-left: 2px; }

  .adjust-slider-controls { display: flex; align-items: center; gap: 8px; }
  .adjust-step-btn {
    width: 34px; height: 34px;
    border-radius: var(--r-sm);
    border: none;
    background: var(--mpx-surface);
    box-shadow: 0 1px 2px rgba(0,0,0,0.08);
    display: grid; place-items: center;
    cursor: pointer;
    color: var(--mpx-ink);
    flex-shrink: 0;
    transition: background var(--dur-fast), transform var(--dur-fast);
  }
  .adjust-step-btn:hover { background: var(--mpx-surface-2); }
  .adjust-step-btn:active { transform: scale(0.92); }

  .adjust-slider-bar-wrap { flex: 1; padding: 14px 0; }
  .adjust-slider-bar { position: relative; width: 100%; height: 6px; border-radius: 4px; background: #ddd; }
  .adjust-slider-fill { position: absolute; top: 0; height: 100%; border-radius: 4px; background: var(--yellow); pointer-events: none; }
  .adjust-slider-handle {
    position: absolute;
    top: 50%;
    width: 24px; height: 24px;
    border-radius: 50%;
    background: var(--mpx-ink);
    border: 3px solid #fff;
    box-shadow: 0 2px 6px rgba(0,0,0,0.25);
    transform: translate(-50%, -50%);
    z-index: 2;
    touch-action: none;
    cursor: grab;
  }
  .adjust-slider-handle:active { cursor: grabbing; transform: translate(-50%, -50%) scale(1.1); }

  .all-adjust-list { display: flex; flex-direction: column; gap: 12px; padding: 12px 14px calc(24px + var(--safe-bottom)); }
  .all-adjust-item { background: var(--mpx-surface-2); border-radius: var(--r-lg); padding: 14px 16px; }
  .all-adjust-item-header { display: flex; align-items: baseline; justify-content: space-between; margin-bottom: 6px; }
  .all-adjust-item-label { font-size: 0.9rem; font-weight: 700; color: var(--mpx-ink); }
  .all-adjust-item-value { font-size: 0.95rem; font-weight: 800; color: var(--mpx-ink); font-variant-numeric: tabular-nums; }

  .content-spacer { height: 4px; flex-shrink: 0; }

  /* ════════════════════════════════════════
     BOTTOM NAV
     ════════════════════════════════════════ */
  .bottom-nav {
    display: flex;
    align-items: stretch;
    background: var(--mpx-surface);
    border-top: 1px solid var(--mpx-line);
    padding: 6px 4px calc(6px + var(--safe-bottom));
    flex-shrink: 0;
    z-index: 30;
  }
  .bottom-nav.hidden { display: none; }

  .nav-item {
    flex: 1;
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: 3px;
    padding: 4px 0 2px;
    border: none;
    background: transparent;
    cursor: pointer;
    color: var(--mpx-muted);
    transition: color var(--dur-fast);
    min-width: 0;
  }
  .nav-item.active { color: var(--mpx-ink); }

  .nav-icon {
    width: 46px;
    height: 28px;
    border-radius: var(--r-pill);
    display: grid;
    place-items: center;
    transition: background var(--dur-med) var(--ease-out), transform var(--dur-med) var(--ease-spring);
  }
  .nav-item.active .nav-icon { background: var(--yellow); }
  .nav-item:active .nav-icon { transform: scale(0.92); }
  .nav-eyes { display: grid; place-items: center; width: 22px; height: 16px; overflow: hidden; }
  .nav-eyes :global(svg) { width: 22px; }

  .nav-label {
    font-size: 0.62rem;
    font-weight: 700;
    letter-spacing: 0.02em;
    white-space: nowrap;
  }

  /* On wide screens move the nav to a compact rail height */
  @media (min-width: 900px) {
    .bottom-nav { padding: 4px 24px calc(4px + var(--safe-bottom)); justify-content: center; gap: 24px; }
    .nav-item { flex: 0 0 96px; }
  }

  /* ════════════════════════════════════════
     UPLOAD PAGE
     ════════════════════════════════════════ */
  .upload-page, .upload-inner { flex: 1; display: flex; flex-direction: column; min-height: 0; }

  /* ════════════════════════════════════════
     SETTINGS PAGE
     ════════════════════════════════════════ */
  .settings-page {
    flex: 1;
    background: var(--mpx-surface-2);
    display: flex;
    flex-direction: column;
    min-height: 0;
  }
  .settings-header {
    display: flex; align-items: center; justify-content: center;
    padding: 12px 14px;
    background: var(--yellow);
    flex-shrink: 0;
  }
  .settings-title { font-size: 1.02rem; font-weight: 800; color: var(--mpx-ink); }
  .settings-scroll {
    flex: 1;
    overflow-y: auto;
    padding: 6px 14px calc(20px + var(--safe-bottom));
    max-width: var(--page-max);
    width: 100%;
    margin: 0 auto;
  }
  .settings-group-label {
    font-size: 0.68rem; font-weight: 800; color: var(--mpx-muted);
    text-transform: uppercase; letter-spacing: 0.08em;
    padding: 14px 6px 6px;
  }
  .settings-list {
    display: flex; flex-direction: column;
    background: var(--mpx-surface);
    border-radius: var(--r-lg);
    overflow: hidden;
    box-shadow: var(--shadow-card);
  }
  .settings-item {
    display: flex; align-items: center; gap: 14px;
    width: 100%;
    padding: 14px 16px;
    border: none;
    background: var(--mpx-surface);
    cursor: pointer;
    text-align: left;
    transition: background var(--dur-fast);
  }
  .settings-item + .settings-item { border-top: 1px solid var(--mpx-line); }
  .settings-item:hover { background: #fafafa; }
  .settings-item:active { background: var(--mpx-surface-2); }
  .settings-item-icon {
    width: 42px; height: 42px;
    border-radius: var(--r-sm);
    background: var(--yellow);
    display: grid; place-items: center;
    color: var(--mpx-ink);
    flex-shrink: 0;
  }
  .settings-item-content { flex: 1; display: flex; flex-direction: column; gap: 2px; min-width: 0; }
  .settings-item-label { font-size: 0.94rem; font-weight: 700; color: var(--mpx-ink); }
  .settings-item-desc { font-size: 0.74rem; font-weight: 500; color: var(--mpx-muted); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  .settings-chevron { color: var(--mpx-muted-2); flex-shrink: 0; }
  .settings-footer {
    display: flex; justify-content: space-between;
    font-size: 0.68rem; color: var(--mpx-muted-2);
    padding: 18px 6px 0;
  }
</style>
