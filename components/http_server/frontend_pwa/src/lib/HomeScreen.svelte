<script>
  import { colors } from "./colors.js";
  import Controller from "./Controller.svelte";
  import ControllerLandscape from "./ControllerLandscape.svelte";
  import ActionIcon from "./ActionIcon.svelte";
  import SkillsView from "./SkillsView.svelte";
  import UploadView from "./UploadView.svelte";
  import Calibration from "./Calibration.svelte";
  import WiFiSetup from "./WiFiSetup.svelte";
  import APModeConfig from "./APModeConfig.svelte";

  let { network } = $props();
  import ActionAssignment from "./ActionAssignment.svelte";
  import AddActionView from "./AddActionView.svelte";
  import ChatView from "./chat/ChatView.svelte";
  import SkillsMarketplace from "./SkillsMarketplace.svelte";
  import ServoStudio from "./ServoStudio.svelte";
  import Wordmark from "./Wordmark.svelte";
  import Eyes from "./Eyes.svelte";
  import { getThemePreference, setThemePreference } from "./theme.js";

  const YELLOW = colors.mpx.primary;

  let activeTab = $state("home");
  let themePref = $state(getThemePreference());
  function chooseTheme(pref) { themePref = pref; setThemePreference(pref); }
  let useLandscape = $state(false);
  let panelEl = $state(null);

  const INNER_RATIO = 881 / 457; // landscape inner aspect ratio

  function setInnerSizing() {
    if (!panelEl) return;
    const { width, height } = panelEl.getBoundingClientRect();
    const panelRatio = width / height;
    if (panelRatio > INNER_RATIO) {
      panelEl.style.setProperty('--inner-w', 'auto');
      panelEl.style.setProperty('--inner-h', '100%');
    } else {
      panelEl.style.setProperty('--inner-w', '100%');
      panelEl.style.setProperty('--inner-h', 'auto');
    }
  }

  $effect(() => {
    if (!panelEl) return;
    const ro = new ResizeObserver(([entry]) => {
      const { width, height } = entry.contentRect;
      useLandscape = width >= height * 1.3;
      setInnerSizing();
    });
    ro.observe(panelEl);
    return () => ro.disconnect();
  });

  // Re-run sizing immediately whenever the controller switches
  $effect(() => {
    if (!panelEl) return;
    useLandscape; // track changes
    setInnerSizing();
  });
  let isFullscreen = $state(false);
  let isEditMode = $state(false);
  let showActionAssignment = $state(false);
  let actionAssignments = $state({});

  // Lock body scroll when fullscreen
  $effect(() => {
    if (isFullscreen) {
      document.body.style.overflow = 'hidden';
    } else {
      document.body.style.overflow = '';
    }
    return () => { document.body.style.overflow = ''; };
  });

  let activeView = $state("main"); // "main" | "upload"

  // ── Connection mode detection ──────────────────────────────
  /** "ap" if the page is loaded via 192.168.2.1, otherwise "sta". */
  let connectionMode = $derived(
    typeof window !== "undefined" && window.location.hostname === "192.168.2.1"
      ? "ap"
      : "sta"
  );

  let showAllActions = $state(false);
  let showAddAction = $state(false);
  let customActions = $state([]);
  let pinnedActions = $state([]);
  let showGaitSection = $state(false);
  let longPressTarget = $state(null);
  let showAllAdjust = $state(false);
  let selectedAdjust = $state(0);
  let showCalibration = $state(false);
  let showStudio = $state(false);
  let showWiFi = $state(false);
  let showAPConfig = $state(false);
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
  let configLoading = $state(false);
  /** The gait the robot reports it is in ("stanford", "step", ...). */
  let robotMode = $state("");

  async function fetchConfig() {
    configLoading = true;
    try {
      const res = await fetch("/v1/robot/status");
      if (res.ok) {
        const data = await res.json();
        if (typeof data.mode === "string") robotMode = data.mode;
        const cfg = data.config || {};
        for (const prop of adjustProps) {
          if (cfg[prop.key] !== undefined) {
            prop.value = cfg[prop.key];
          }
        }
      }
    } catch { /* robot may be unreachable — keep defaults */ }
    configLoading = false;
  }

  async function saveConfig() {
    const body = {};
    for (const prop of adjustProps) {
      body[prop.key] = prop.value;
    }
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

  // Load persisted state on mount
  $effect(() => {
    try {
      const saved = localStorage.getItem(STORAGE_KEY_ASSIGNMENTS);
      if (saved) actionAssignments = JSON.parse(saved);
    } catch {}
    try {
      const saved = localStorage.getItem(STORAGE_KEY_CUSTOM);
      if (saved) customActions = JSON.parse(saved);
    } catch {}
    try {
      const saved = localStorage.getItem(STORAGE_KEY_PINNED);
      if (saved) pinnedActions = JSON.parse(saved);
    } catch {}
  });

  // Persist on change
  $effect(() => {
    if (typeof window === "undefined") return;
    // track changes
    const a = JSON.stringify(actionAssignments);
    const c = JSON.stringify(customActions);
    const p = JSON.stringify(pinnedActions);
    try { localStorage.setItem(STORAGE_KEY_ASSIGNMENTS, a); } catch {}
    try { localStorage.setItem(STORAGE_KEY_CUSTOM, c); } catch {}
    try { localStorage.setItem(STORAGE_KEY_PINNED, p); } catch {}
  });

  $effect(() => {
    fetchConfig();
  });

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

  // The second tab used to be the raw file browser, which is where every
  // skill pushed with `mpx-cli deploy` ended up looking like a stray blob.
  // It is now the Skills screen; the file browser lives inside it as a
  // segment, so nothing was lost.
  /* The store is a destination, not a setting.
   *
   * It used to be a row inside Settings — three taps deep, next to Wi-Fi and
   * calibration, which is where you put a configuration screen and not where
   * anyone looks to browse. Somewhere you go to spend money belongs in the
   * navigation. Placed after Skills because the pair reads in the order you
   * use them: find something, then run it. */
  const tabs = [
    { id: "home",     label: "Home",     icon: "home" },
    { id: "skills",   label: "Skills",   icon: "skills" },
    { id: "store",    label: "Store",    icon: "store" },
    { id: "chat",     label: "Chat",     icon: "chat" },
    { id: "settings", label: "Settings", icon: "settings" },
  ];

  // ── Adjust slider interaction ──
  function adjustPct(prop) {
    return ((prop.value - prop.min) / (prop.max - prop.min)) * 100;
  }
  function adjustNeutralPct(prop) {
    if (prop.isCenter) {
      return ((0 - prop.min) / (prop.max - prop.min)) * 100;
    }
    return 0;
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

  const actionsPerPage = 8;
  function totalPages() {
    return Math.ceil((actions.length + customActions.length + 1) / actionsPerPage);
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

  function isPinned(actionId) {
    return pinnedActions.includes(actionId);
  }

  // Get pinned action objects
  let pinnedActionObjects = $derived(
    pinnedActions.map(id => allActions.find(a => a.id === id)).filter(Boolean)
  );

  const pinnedPerPage = 8;
  let pinnedTotalPages = $derived(Math.max(1, Math.ceil(pinnedActionObjects.length / pinnedPerPage)));

  // ── Fullscreen API ──────────────────────────────────────
  async function toggleFullscreen() {
    if (isFullscreen) {
      // Exit fullscreen
      isFullscreen = false;
      try {
        if (document.fullscreenElement) {
          await document.exitFullscreen();
        }
      } catch {}
    } else {
      // Enter fullscreen using the Fullscreen API
      isFullscreen = true;
      try {
        await document.documentElement.requestFullscreen();
      } catch {
        // Fallback: just use CSS fullscreen if API fails
      }
    }
  }

  // Listen for fullscreen exit via Esc
  $effect(() => {
    function onFsChange() {
      if (!document.fullscreenElement && isFullscreen) {
        isFullscreen = false;
      }
    }
    document.addEventListener('fullscreenchange', onFsChange);
    return () => document.removeEventListener('fullscreenchange', onFsChange);
  });

  function selectTab(id) {
    activeTab = id;
  }
</script>

<div class="home-root">
  <!-- ═══ Top Header: the robot, and how you are reaching it ═══ -->
  <header class="top-header">
    <div class="header-left">
      <span class="header-eyes eyes-blink"><Eyes size={26} /></span>
      <div class="header-text">
        <span class="header-title">Mini Pupper</span>
        <span class="header-sub">
          <span class="conn-mode-dot" class:mode-ap={connectionMode === "ap"} class:mode-sta={connectionMode === "sta"}></span>
          {connectionMode === "ap" ? "Direct link" : (network?.staSsid || "Wi‑Fi")}{robotMode ? ` · ${robotMode}` : ""}
        </span>
      </div>
    </div>

    <div class="header-right">
      <Wordmark height={14} class="header-mark" />
    </div>
  </header>

  <!-- ═══ Content Area ═══ -->
  <main class="content">
    {#if activeTab === "home" && showAddAction}
      <AddActionView
        onSave={handleAddAction}
        onBack={() => { showAddAction = false; }}
      />
    {:else if activeTab === "home" && showActionAssignment}
      <!-- ═══ Action Assignment Sub-screen ═══ -->
      <ActionAssignment
        assignments={actionAssignments}
        customActions={customActions}
        onAddAction={() => { showAddAction = true; }}
        onDeleteAction={(actionId) => {
          customActions = customActions.filter(c => c.id !== actionId);
          pinnedActions = pinnedActions.filter(id => id !== actionId);
        }}
        onSave={(updated) => {
          actionAssignments = updated;
        }}
        onBack={() => {
          showActionAssignment = false;
          isEditMode = false;
        }}
      />
    {:else if activeTab === "home" && showAllActions}
      <!-- ═══ Full-page All Actions View ═══ -->
      <div class="all-actions-page" style="--yellow: {YELLOW}">
        <header class="all-actions-header">
          <button class="back-btn" aria-label="Back" onclick={() => showAllActions = false}>
            <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
              <line x1="19" y1="12" x2="5" y2="12"/>
              <polyline points="12 19 5 12 12 5"/>
            </svg>
          </button>
          <h2 class="all-actions-title">All Actions</h2>
          <div style="width:32px"></div>
        </header>
        <div class="all-actions-scroll">
          <!-- ═══ Gait Section (collapsible) ═══ -->
          {#if gaitActions.length > 0}
            <div class="all-actions-category">
              <button class="all-actions-cat-header" onclick={() => showGaitSection = !showGaitSection} aria-label="Toggle gait section">
                <h3 class="all-actions-cat-title">🚶 Gait</h3>
                <svg class="collapse-chevron" class:collapsed={!showGaitSection} width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
                  <polyline points="6 9 12 15 18 9"/>
                </svg>
              </button>
              {#if showGaitSection}
              <div class="actions-full-grid">
                {#each gaitActions as action}
                  <div class="action-grid-item all-action-item" style="--action-color: {action.color}" role="button" tabindex="0">
                    <div class="action-grid-icon">
                      <ActionIcon id={action.id} emoji={action.emoji} />
                    </div>
                    <span class="action-grid-label">{action.label}</span>
                    <!-- Pin button -->
                    <button
                      class="pin-trigger-btn"
                      class:pinned={isPinned(action.id)}
                      onclick={(e) => { e.stopPropagation(); togglePin(action.id); }}
                      aria-label={isPinned(action.id) ? 'Unpin {action.label}' : 'Pin {action.label}'}
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

          <!-- ═══ Skill Section ═══ -->
          {#if skillActions.length > 0}
            <div class="all-actions-category">
              <h3 class="all-actions-cat-title">🎯 Skill</h3>
              <div class="actions-full-grid">
                {#each skillActions as action}
                  <div class="action-grid-item all-action-item" style="--action-color: {action.color}" role="button" tabindex="0">
                    <div class="action-grid-icon">
                      <ActionIcon id={action.id} emoji={action.emoji} />
                    </div>
                    <span class="action-grid-label">{action.label}</span>
                    <!-- Pin button -->
                    <button
                      class="pin-trigger-btn"
                      class:pinned={isPinned(action.id)}
                      onclick={(e) => { e.stopPropagation(); togglePin(action.id); }}
                      aria-label={isPinned(action.id) ? 'Unpin {action.label}' : 'Pin {action.label}'}
                    >
                      <svg width="12" height="12" viewBox="0 0 24 24" fill={isPinned(action.id) ? 'currentColor' : 'none'} stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
                        <line x1="12" y1="17" x2="12" y2="22"/>
                        <path d="M5 17h14v-2.5c0-.83-.67-1.5-1.5-1.5h-11c-.83 0-1.5.67-1.5 1.5V17z"/>
                        <path d="M12 14V7l-2.33-4.67h4.66L12 7z"/>
                      </svg>
                    </button>
                    {#if action.category === "skill" || action.category === "custom"}
                    <!-- Delete custom action -->
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
                    {/if}
                  </div>
                {/each}
              </div>
            </div>
          {/if}

          <!-- Add button -->
          <div class="all-actions-add-wrap">
            <button class="action-grid-item add-item" onclick={() => { showAllActions = false; showAddAction = true; }}>
              <div class="action-grid-icon add-icon-box">
                <svg width="28" height="28" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
                  <line x1="12" y1="5" x2="12" y2="19"/>
                  <line x1="5" y1="12" x2="19" y2="12"/>
                </svg>
              </div>
              <span class="action-grid-label">Add</span>
            </button>
          </div>
        </div>
      </div>
    {:else if activeTab === "home" && showAllAdjust}
      <!-- ═══ Full-page All Adjust View ═══ -->
      <div class="all-adjust-page" style="--yellow: {YELLOW}">
        <header class="all-adjust-header">
          <button class="back-btn" aria-label="Back" onclick={() => showAllAdjust = false}>
            <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
              <line x1="19" y1="12" x2="5" y2="12"/>
              <polyline points="12 19 5 12 12 5"/>
            </svg>
          </button>
          <h2 class="all-adjust-title">All Adjust</h2>
          <div style="width:32px"></div>
        </header>
        <div class="all-adjust-list">
          {#each adjustProps as prop, idx}
            <div class="all-adjust-item">
              <div class="all-adjust-item-header">
                <span class="all-adjust-item-label">{prop.label}</span>
                <span class="all-adjust-item-value">{prop.value}{prop.unit}</span>
              </div>
              <div class="all-adjust-slider-row">
                <button class="adjust-step-btn" aria-label="Decrease {prop.label}" onclick={() => adjustStep(idx, -1)}>
                  <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
                    <line x1="5" y1="12" x2="19" y2="12"/>
                  </svg>
                </button>
                <div class="all-adjust-slider-container">
                  <div class="all-adjust-slider-bar">
                    <div
                      class="all-adjust-slider-fill"
                      style="width: {adjustPct(prop)}%"
                    ></div>
                    <div class="all-adjust-slider-handle current" style="left: {adjustPct(prop)}%"
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
                  <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
                    <line x1="12" y1="5" x2="12" y2="19"/>
                    <line x1="5" y1="12" x2="19" y2="12"/>
                  </svg>
                </button>
              </div>
            </div>
          {/each}
        </div>
      </div>
    {:else if activeTab === "home"}
      <div class="home-content" style="--thumb-color: {YELLOW}; --yellow: {YELLOW}">
        <!-- ═══ Controller Panel ═══ -->
        <div
          class="controller-panel"
          class:landscape={useLandscape}
          class:fullscreen={isFullscreen}
          bind:this={panelEl}
        >
          {#if useLandscape}
            <ControllerLandscape assignments={actionAssignments} />
          {:else}
            <Controller assignments={actionAssignments} />
          {/if}

          <!-- ═══ Overlay Control Buttons ═══ -->
          <div class="ctrl-overlay-btns">
            <button
              class="ctrl-overlay-btn"
              onclick={toggleFullscreen}
              aria-label={isFullscreen ? 'Minimize' : 'Fullscreen'}
              title={isFullscreen ? 'Minimize' : 'Fullscreen'}
            >
              {#if isFullscreen}
                <!-- Minimize icon -->
                <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
                  <polyline points="4 14 10 14 10 20"/>
                  <polyline points="20 10 14 10 14 4"/>
                  <line x1="14" y1="10" x2="21" y2="3"/>
                  <line x1="3" y1="21" x2="10" y2="14"/>
                </svg>
              {:else}
                <!-- Fullscreen icon -->
                <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
                  <polyline points="15 3 21 3 21 9"/>
                  <polyline points="9 21 3 21 3 15"/>
                  <line x1="21" y1="3" x2="14" y2="10"/>
                  <line x1="3" y1="21" x2="10" y2="14"/>
                </svg>
              {/if}
            </button>

            {#if !isFullscreen}
            <button
              class="ctrl-overlay-btn"
              class:active={isEditMode}
              onclick={() => {
                isEditMode = !isEditMode;
                if (isEditMode) showActionAssignment = true;
              }}
              aria-label="Edit actions"
              title="Edit actions"
            >
              <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
                <path d="M11 4H4a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h14a2 2 0 0 0 2-2v-7"/>
                <path d="M18.5 2.5a2.121 2.121 0 0 1 3 3L12 15l-4 1 1-4 9.5-9.5z"/>
              </svg>
            </button>
            {/if}
          </div>
        </div>

        <!-- ═══ Scrollable cards below ═══ -->
        <div class="home-content-scroll">

        <!-- ═══ Actions Card (Pinned) ═══ -->
        <div class="section-card">
          <div class="section-card-header">
            <h2 class="section-card-title">Quick Actions</h2>
            <button class="see-all-btn" onclick={() => showAllActions = true}>See all</button>
          </div>
          <div class="action-grid-scroll-wrap">
            {#if pinnedActionObjects.length === 0}
              <p class="no-pinned-hint">Long-press an action in "See all" to pin it here.</p>
            {:else}
            <div class="action-grid-scroll">
              {#each { length: pinnedTotalPages } as _, pageIdx}
                  <div class="action-grid-page">
                    {#each pinnedActionObjects.slice(pageIdx * pinnedPerPage, pageIdx * pinnedPerPage + pinnedPerPage) as action}
                      <div
                        class="action-grid-item pinned-item"
                        class:show-trash={longPressTarget === action.id}
                        style="--action-color: {action.color}; position: relative;"
                        role="button"
                        tabindex="0"
                        onclick={() => sendGait(action.id)}
                        onpointerdown={(e) => {
                          const timer = setTimeout(() => {
                            longPressTarget = action.id;
                          }, 600);
                          e.currentTarget._longPressTimer = timer;
                        }}
                        onpointerup={(e) => {
                          clearTimeout(e.currentTarget._longPressTimer);
                          longPressTarget = null;
                        }}
                        onpointerleave={(e) => {
                          clearTimeout(e.currentTarget._longPressTimer);
                          longPressTarget = null;
                        }}
                        onpointercancel={(e) => {
                          clearTimeout(e.currentTarget._longPressTimer);
                          longPressTarget = null;
                        }}
                      >
                        <div class="action-grid-icon">
                          <ActionIcon id={action.id} emoji={action.emoji} />
                        </div>
                        <span class="action-grid-label">{action.label}</span>
                        <!-- Trash button (shown on long-press) -->
                        <button
                          class="unpin-trash-btn"
                          onclick={(e) => {
                            e.stopPropagation();
                            togglePin(action.id);
                          }}
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
            <!-- Page dots -->
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

        <!-- ═══ Adjust Card (swipeable) ═══ -->
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
                    <button
                      class="adjust-selector-btn"
                      class:active={selectedAdjust === pageIdx * adjustPerPage + i}
                      onclick={() => selectedAdjust = pageIdx * adjustPerPage + i}
                    >
                      <div class="adjust-selector-circle" class:active={selectedAdjust === pageIdx * adjustPerPage + i} style="--sel-color: {YELLOW}">
                        {#if (pageIdx * adjustPerPage + i) === 0}
                          <svg width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
                            <circle cx="12" cy="12" r="10"/>
                            <polyline points="12 6 12 12 16 14"/>
                          </svg>
                        {:else if (pageIdx * adjustPerPage + i) === 1}
                          <svg width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
                            <line x1="12" y1="5" x2="12" y2="19"/>
                            <polyline points="9 8 12 5 15 8"/>
                            <polyline points="9 16 12 19 15 16"/>
                          </svg>
                        {:else if (pageIdx * adjustPerPage + i) === 2}
                          <svg width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
                            <line x1="12" y1="5" x2="12" y2="19"/>
                            <polyline points="9 8 12 5 15 8"/>
                          </svg>
                        {:else if (pageIdx * adjustPerPage + i) === 3}
                          <svg width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
                            <polyline points="17 8 21 12 17 16"/>
                            <polyline points="7 8 3 12 7 16"/>
                            <line x1="21" y1="12" x2="3" y2="12"/>
                          </svg>
                        {:else if (pageIdx * adjustPerPage + i) === 4}
                          <!-- Tilt: incline/level icon -->
                          <svg width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
                            <polyline points="22 12 18 12 15 21 9 3 6 12 2 12"/>
                          </svg>
                        {/if}
                      </div>
                      <span class="adjust-selector-label">{prop.label}</span>
                    </button>
                  {/each}
                </div>
              {/each}
            </div>
            <!-- Page dots -->
            <div class="page-dots">
              {#each { length: adjustTotalPages() } as _, i}
                <span class="page-dot" class:active={i === adjustCurrentPage()}></span>
              {/each}
            </div>
          </div>

          <!-- Slider for selected property -->
          <div class="adjust-slider-section">
            <div class="adjust-slider-header">
              <span class="adjust-slider-prop-label">{adjustProps[selectedAdjust].label}</span>
              <span class="adjust-slider-value">{adjustProps[selectedAdjust].value}{adjustProps[selectedAdjust].unit}</span>
            </div>

            <div class="adjust-slider-controls">
              <button class="adjust-step-btn" aria-label="Decrease" onclick={() => adjustStep(selectedAdjust, -1)}>
                <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
                  <line x1="5" y1="12" x2="19" y2="12"/>
                </svg>
              </button>

              <div class="adjust-slider-bar-wrap">
                <div class="adjust-slider-bar">
                  <div
                    class="adjust-slider-fill"
                    style="width: {adjustPct(adjustProps[selectedAdjust])}%"
                  ></div>
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
                <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
                  <line x1="12" y1="5" x2="12" y2="19"/>
                  <line x1="5" y1="12" x2="19" y2="12"/>
                </svg>
              </button>
            </div>
          </div>
        </div>

        <!-- bottom spacer for safe area -->
        <div class="content-spacer"></div>
      </div>
      </div>
    {:else if activeTab === "skills"}
      {#if activeView === "upload"}
        <!-- ═══ Upload View ═══ -->
        <div class="upload-page">
          <div class="upload-inner">
            <UploadView onBack={() => activeView = "main"} />
          </div>
        </div>
      {:else}
        <SkillsView onNavigate={(view) => { if (view === "upload") activeView = "upload"; }} />
      {/if}
    {:else if activeTab === "store"}
      <SkillsMarketplace onNavigate={() => (activeTab = "skills")} />
    {:else if activeTab === "chat"}
      <ChatView navigate={(to) => { if (to === "home") activeTab = "home"; }} />
    {:else if activeTab === "settings"}
      {#if showWiFi}
        <WiFiSetup onNavigate={() => showWiFi = false} {network} />
      {:else if showAPConfig}
        <APModeConfig onNavigate={() => showAPConfig = false} {network} />
      {:else if showCalibration}
        <Calibration onBack={() => showCalibration = false} />
      {:else if showStudio}
        <ServoStudio onBack={() => showStudio = false} />
      {:else}
        <div class="settings-page">
          <header class="settings-header">
            <h2 class="settings-title">Settings</h2>
          </header>
          <div class="settings-list">
            <div class="settings-group-label">Appearance</div>
            <div class="theme-row" role="radiogroup" aria-label="Theme">
              {#each [["dark", "Dark"], ["light", "Light"], ["system", "System"]] as [id, label]}
                <button class="theme-opt" class:active={themePref === id} role="radio" aria-checked={themePref === id} onclick={() => chooseTheme(id)}>{label}</button>
              {/each}
            </div>
            <div class="settings-group-label">Robot</div>
            <button class="settings-item" onclick={() => showWiFi = true}>
              <div class="settings-item-icon">
                <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
                  <path d="M5 12.55a11 11 0 0 1 14.08 0"/>
                  <path d="M1.42 9a16 16 0 0 1 21.16 0"/>
                  <path d="M8.53 16.11a6 6 0 0 1 6.95 0"/>
                  <circle cx="12" cy="20" r="1" fill="currentColor"/>
                </svg>
              </div>
              <div class="settings-item-content">
                <span class="settings-item-label">Wi‑Fi</span>
                <span class="settings-item-desc">Join or configure wireless network</span>
              </div>
              <svg class="settings-chevron" width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
                <polyline points="9 18 15 12 9 6"/>
              </svg>
            </button>
            <button class="settings-item" onclick={() => showAPConfig = true}>
              <div class="settings-item-icon">
                <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
                  <path d="M12 2a10 10 0 0 1 10 10"/>
                  <path d="M12 6a6 6 0 0 1 6 6"/>
                  <path d="M12 10a2 2 0 0 1 2 2"/>
                  <circle cx="12" cy="20" r="1.5" fill="currentColor"/>
                </svg>
              </div>
              <div class="settings-item-content">
                <span class="settings-item-label">AP Mode</span>
                <span class="settings-item-desc">Configure hotspot access point</span>
              </div>
              <svg class="settings-chevron" width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
                <polyline points="9 18 15 12 9 6"/>
              </svg>
            </button>
            <button class="settings-item" onclick={() => showCalibration = true}>
              <div class="settings-item-icon">
                <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
                  <circle cx="12" cy="12" r="3"/>
                  <path d="M19.4 15a1.65 1.65 0 0 0 .33 1.82l.06.06a2 2 0 0 1-2.83 2.83l-.06-.06a1.65 1.65 0 0 0-1.82-.33 1.65 1.65 0 0 0-1 1.51V21a2 2 0 0 1-4 0v-.09A1.65 1.65 0 0 0 9 19.4a1.65 1.65 0 0 0-1.82.33l-.06.06a2 2 0 0 1-2.83-2.83l.06-.06A1.65 1.65 0 0 0 4.68 15a1.65 1.65 0 0 0-1.51-1H3a2 2 0 0 1 0-4h.09A1.65 1.65 0 0 0 4.6 9a1.65 1.65 0 0 0-.33-1.82l-.06-.06a2 2 0 0 1 2.83-2.83l.06.06A1.65 1.65 0 0 0 9 4.68a1.65 1.65 0 0 0 1-1.51V3a2 2 0 0 1 4 0v.09a1.65 1.65 0 0 0 1 1.51 1.65 1.65 0 0 0 1.82-.33l.06-.06a2 2 0 0 1 2.83 2.83l-.06.06A1.65 1.65 0 0 0 19.4 9a1.65 1.65 0 0 0 1.51 1H21a2 2 0 0 1 0 4h-.09a1.65 1.65 0 0 0-1.51 1z"/>
                </svg>
              </div>
              <div class="settings-item-content">
                <span class="settings-item-label">Calibration</span>
                <span class="settings-item-desc">Adjust leg positions and offsets</span>
              </div>
              <svg class="settings-chevron" width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
                <polyline points="9 18 15 12 9 6"/>
              </svg>
            </button>
            <button class="settings-item" onclick={() => showStudio = true}>
              <div class="settings-item-icon">
                <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
                  <line x1="4" y1="21" x2="4" y2="14"/><line x1="4" y1="10" x2="4" y2="3"/>
                  <line x1="12" y1="21" x2="12" y2="12"/><line x1="12" y1="8" x2="12" y2="3"/>
                  <line x1="20" y1="21" x2="20" y2="16"/><line x1="20" y1="12" x2="20" y2="3"/>
                  <line x1="1" y1="14" x2="7" y2="14"/><line x1="9" y1="8" x2="15" y2="8"/>
                  <line x1="17" y1="16" x2="23" y2="16"/>
                </svg>
              </div>
              <div class="settings-item-content">
                <span class="settings-item-label">Servo Studio</span>
                <span class="settings-item-desc">Tune Kp / Kd gains on the driver boards</span>
              </div>
              <svg class="settings-chevron" width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
                <polyline points="9 18 15 12 9 6"/>
              </svg>
            </button>
            <!-- The Skill Store row lived here. It is a bottom-nav tab now:
                 two doors to one screen is how people end up unsure which one
                 they are looking at. -->
            <!-- "Skill Management" used to live here. It listed only
                 marketplace skills, while skills pushed from mpx-cli were
                 invisible to it — two half-lists on two screens. Both are now
                 on the Skills tab, so this entry would only be a second way
                 into a screen that is already one tap away. -->
          </div>
        </div>
      {/if}
    {/if}
  </main>

  <!-- ═══ Bottom Navigation ═══ -->
  <nav class="bottom-nav">
    {#each tabs as tab}
      <button
        class="nav-item"
        class:active={activeTab === tab.id}
        onclick={() => selectTab(tab.id)}
      >
        <div class="nav-icon">
          {#if tab.id === "home"}
            <span class="nav-eyes" aria-hidden="true"><Eyes size={26} /></span>

          {:else if tab.id === "skills"}
            <!-- Lightning bolt — a skill is something that runs, not a folder -->
            <svg width="26" height="26" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round">
              <polygon points="13 2 3 14 12 14 11 22 21 10 12 10 13 2"/>
            </svg>
          {:else if tab.id === "store"}
            <!-- Shopfront awning. A bag or a cart would read as a checkout;
                 this reads as a place you browse. -->
            <svg width="26" height="26" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round">
              <path d="M3 9l1.5-5h15L21 9"/>
              <path d="M4 9v10a1 1 0 0 0 1 1h14a1 1 0 0 0 1-1V9"/>
              <path d="M3 9h18"/>
              <path d="M9 20v-6h6v6"/>
            </svg>
          {:else if tab.id === "chat"}
            <!-- Chat bubble -->
            <svg width="26" height="26" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round">
              <path d="M21 15a2 2 0 0 1-2 2H7l-4 4V5a2 2 0 0 1 2-2h14a2 2 0 0 1 2 2z"/>
            </svg>
          {:else if tab.id === "settings"}
            <!-- Gear icon -->
            <svg width="26" height="26" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round">
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
    background: var(--bg);
    display: flex;
    flex-direction: column;
  }

  /* ════════════════════════════════════════
     TOP HEADER
     ════════════════════════════════════════ */
  .top-header {
    display: flex;
    align-items: center;
    justify-content: space-between;
    height: 64px;
    padding: 0 18px 0 16px;
    padding-top: env(safe-area-inset-top, 0px);
    flex-shrink: 0;
    color: var(--text-1);
  }
  .header-left { display: flex; align-items: center; gap: 12px; min-width: 0; }
  .header-right { display: flex; align-items: center; color: var(--text-3); }
  .header-eyes {
    display: inline-flex;
    width: 40px;
    height: 40px;
    align-items: center;
    justify-content: center;
    border-radius: 13px;
    color: var(--accent);
    background: var(--surface-1);
    border: 1px solid var(--line);
    box-shadow: inset 0 1px 0 var(--highlight);
    filter: drop-shadow(0 0 6px rgba(255, 230, 5, 0.35));
  }
  .header-text { display: flex; flex-direction: column; gap: 1px; min-width: 0; }
  .header-title {
    font-size: 17px;
    font-weight: 700;
    letter-spacing: -0.01em;
    line-height: 1.15;
  }
  .header-sub {
    display: flex;
    align-items: center;
    gap: 6px;
    font-size: 12.5px;
    color: var(--text-2);
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .conn-mode-dot {
    width: 7px;
    height: 7px;
    border-radius: 50%;
    background: var(--text-3);
    flex-shrink: 0;
  }
  .conn-mode-dot.mode-ap  { background: var(--ok);  box-shadow: 0 0 8px var(--ok); }
  .conn-mode-dot.mode-sta { background: var(--ok);  box-shadow: 0 0 8px var(--ok); }

  /* ════════════════════════════════════════
     CONTENT
     ════════════════════════════════════════ */
  .content {
    flex: 1;
    overflow-y: auto;
    display: flex;
    flex-direction: column;
  }

  .home-content {
    display: flex;
    flex-direction: column;
    flex: 1;
    min-height: 0;
  }

  .home-content-scroll {
    padding: 8px 16px 28px;
    display: flex;
    flex-direction: column;
    gap: 12px;
    flex-shrink: 0;
  }

  /* ════════════════════════════════════════
     CONTROLLER PANEL
     ════════════════════════════════════════ */
  .controller-panel {
    flex: 1;
    min-height: calc(100dvh - 150px);
    display: flex;
    flex-direction: column;
    padding: 6px 16px 10px;
    gap: 0;
    position: relative;
    overflow: hidden;
    background: transparent;
  }

  /* ── Fullscreen mode ──────────────────── */
  .controller-panel.fullscreen {
    position: fixed;
    inset: 0;
    z-index: 100;
    padding: 14px;
    border-radius: 0;
    background: var(--bg);
  }

  /* ── Overlay Control Buttons ──────────── */
  .ctrl-overlay-btns {
    position: absolute;
    top: 8px;
    left: 50%;
    transform: translateX(-50%);
    display: flex;
    gap: 10px;
    z-index: 20;
  }

  .ctrl-overlay-btn {
    width: 34px;
    height: 34px;
    border-radius: 50%;
    border: 1px solid var(--line);
    background: var(--surface-1);
    color: var(--text-2);
    display: flex;
    align-items: center;
    justify-content: center;
    cursor: pointer;
    transition: background 0.15s, transform 0.1s;
  }
  .ctrl-overlay-btn:active {
    transform: scale(0.9);
  }
  .ctrl-overlay-btn.active {
    background: var(--accent);
    border-color: var(--accent);
    color: var(--accent-ink);
  }

  /* ════════════════════════════════════════
     SECTION CARDS
     ════════════════════════════════════════ */
  .section-card {
    background: var(--surface-1);
    border: 1px solid var(--line);
    border-radius: var(--r-lg);
    padding: 16px;
    flex-shrink: 0;
    box-shadow: inset 0 1px 0 var(--highlight), var(--shadow-card);
  }

  .section-card-header {
    display: flex;
    align-items: center;
    justify-content: space-between;
    margin-bottom: 12px;
  }

  .section-card-title {
    font-size: 16px;
    font-weight: 700;
    letter-spacing: -0.01em;
    color: var(--text-1);
  }

  .see-all-btn {
    border: none;
    background: none;
    color: var(--text-2);
    font-size: 13px;
    font-weight: 600;
    cursor: pointer;
    padding: 4px 8px;
    border-radius: 8px;
    transition: background 0.15s;
  }
  .see-all-btn:active {
    background: var(--surface-2);
  }

  /* ── Action Grid (swipeable) ───────────── */
  .action-grid-scroll-wrap {
    display: flex;
    flex-direction: column;
    gap: 10px;
  }

  .action-grid-scroll {
    display: flex;
    overflow-x: auto;
    scroll-snap-type: x mandatory;
    -webkit-overflow-scrolling: touch;
    scrollbar-width: none;
    gap: 0;
    margin: -4px 0;
    padding: 4px 0;
  }
  .action-grid-scroll::-webkit-scrollbar {
    display: none;
  }

  .action-grid-page {
    display: grid;
    grid-template-columns: repeat(4, 1fr);
    gap: 12px;
    flex: 0 0 100%;
    scroll-snap-align: start;
  }

  /* ── Action Grid Item ──────────────────── */
  .action-grid-item {
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: 6px;
    border: none;
    background: transparent;
    cursor: pointer;
    padding: 4px 0;
    transition: transform 0.1s;
  }
  .action-grid-item:active {
    transform: scale(0.92);
  }

  .action-grid-icon {
    width: 58px;
    height: 58px;
    border-radius: 14px;
    background: var(--action-color);
    display: flex;
    align-items: center;
    justify-content: center;
    color: var(--solid-ink);
    border: 1px solid var(--line);
  }

  .action-grid-icon :global(.action-emoji) {
    font-size: 2rem;
  }

  .action-grid-label {
    font-size: 0.7rem;
    font-weight: 600;
    color: var(--text-1);
    text-align: center;
    line-height: 1.2;
  }

  /* ── Pinned item trash button ─────────── */
  .pinned-item {
    position: relative;
  }

  .unpin-trash-btn {
    position: absolute;
    top: -6px;
    right: -6px;
    width: 24px;
    height: 24px;
    border-radius: 50%;
    background: var(--danger);
    border: 2px solid var(--line);
    display: none;
    align-items: center;
    justify-content: center;
    cursor: pointer;
    z-index: 5;
    box-shadow: 0 2px 6px rgba(0,0,0,0.2);
    transition: transform 0.1s;
  }
  .pinned-item.show-trash .unpin-trash-btn {
    display: flex;
  }
  .unpin-trash-btn:active {
    transform: scale(0.85);
  }

  .no-pinned-hint {
    font-size: 0.8rem;
    color: var(--text-3);
    text-align: center;
    padding: 16px 8px;
    font-style: italic;
  }

  /* ── All Actions: pin & delete buttons ── */
  .all-action-item {
    position: relative;
  }

  .pin-trigger-btn {
    position: absolute;
    top: 2px;
    left: 2px;
    width: 20px;
    height: 20px;
    border-radius: 50%;
    background: var(--surface-3);
    border: none;
    display: flex;
    align-items: center;
    justify-content: center;
    cursor: pointer;
    color: var(--solid-ink);
    opacity: 0.6;
    transition: opacity 0.15s, background 0.15s;
    padding: 0;
  }
  .pin-trigger-btn.pinned {
    opacity: 1;
    background: var(--accent);
    color: var(--accent-ink);
  }
  .all-action-item:hover .pin-trigger-btn {
    opacity: 1;
  }

  .delete-action-btn {
    position: absolute;
    top: -4px;
    right: -4px;
    width: 20px;
    height: 20px;
    border-radius: 50%;
    background: var(--danger);
    border: 1.5px solid var(--line);
    display: flex;
    align-items: center;
    justify-content: center;
    cursor: pointer;
    opacity: 0;
    transition: opacity 0.15s;
    padding: 0;
    box-shadow: 0 2px 4px rgba(0,0,0,0.2);
  }
  .all-action-item:hover .delete-action-btn {
    opacity: 1;
  }

  /* ── Collapsible Gait Header ───────────── */
  .all-actions-cat-header {
    display: flex;
    align-items: center;
    justify-content: space-between;
    width: 100%;
    border: none;
    background: none;
    cursor: pointer;
    padding: 8px 16px;
    margin: 12px 0 8px;
    transition: background 0.15s;
    border-radius: 8px;
  }
  .all-actions-cat-header:hover {
    background: var(--surface-2);
  }

  .collapse-chevron {
    color: var(--text-3);
    transition: transform 0.2s;
    flex-shrink: 0;
  }
  .collapse-chevron.collapsed {
    transform: rotate(-90deg);
  }

  /* ── Add Item (dashed box) ─────────────── */
  .add-icon-box {
    background: transparent !important;
    border: 2.5px dashed var(--line-strong);
    color: var(--text-3);
  }

  /* ── Page Dots ─────────────────────────── */
  .page-dots {
    display: flex;
    justify-content: center;
    gap: 6px;
  }

  .page-dot {
    width: 6px;
    height: 6px;
    border-radius: 50%;
    background: var(--surface-3);
    transition: background 0.2s;
  }
  .page-dot.active {
    background: var(--yellow, var(--accent));
  }

  /* ── All Actions Page (full screen) ────── */
  .all-actions-page {
    flex: 1;
    display: flex;
    flex-direction: column;
    padding: 16px 14px 24px;
    overflow-y: auto;
    background: var(--surface-1);
  }
  .all-actions-page::-webkit-scrollbar {
    width: 4px;
  }
  .all-actions-page::-webkit-scrollbar-thumb {
    background: var(--surface-3);
    border-radius: 4px;
  }

  .all-actions-header {
    display: flex;
    align-items: center;
    justify-content: space-between;
    margin-bottom: 20px;
    flex-shrink: 0;
  }

  .all-actions-title {
    font-size: 1.1rem;
    font-weight: 700;
    color: var(--text-1);
    margin: 0;
  }

  .all-actions-scroll {
    flex: 1;
    overflow-y: auto;
    padding: 8px 0 24px;
  }
  .all-actions-scroll::-webkit-scrollbar {
    width: 4px;
  }
  .all-actions-scroll::-webkit-scrollbar-thumb {
    background: var(--surface-3);
    border-radius: 4px;
  }

  .all-actions-category {
    margin-bottom: 8px;
  }

  .all-actions-cat-title {
    font-size: 0.75rem;
    font-weight: 700;
    color: var(--text-3);
    text-transform: uppercase;
    letter-spacing: 0.5px;
    margin: 0;
    padding: 0;
  }

  .all-actions-add-wrap {
    padding: 0 16px;
    margin-top: 8px;
  }

  .actions-full-grid {
    display: grid;
    grid-template-columns: repeat(4, 1fr);
    gap: 14px;
    padding: 4px 0;
    flex-shrink: 0;
  }

  /* ── Back Button ───────────────────────── */
  .back-btn {
    width: 32px;
    height: 32px;
    display: flex;
    align-items: center;
    justify-content: center;
    border: none;
    background: var(--surface-2);
    border-radius: 50%;
    cursor: pointer;
    color: var(--text-1);
    flex-shrink: 0;
    transition: background 0.15s;
  }
  .back-btn:hover {
    background: var(--surface-2);
  }

  /* ── Adjust Selectors (swipeable) ─────── */
  .adjust-scroll-wrap {
    display: flex;
    flex-direction: column;
    gap: 10px;
    margin-bottom: 16px;
  }

  .adjust-scroll {
    display: flex;
    overflow-x: auto;
    scroll-snap-type: x mandatory;
    -webkit-overflow-scrolling: touch;
    scrollbar-width: none;
    gap: 0;
    margin: -4px 0;
    padding: 4px 0;
  }
  .adjust-scroll::-webkit-scrollbar {
    display: none;
  }

  .adjust-page {
    display: flex;
    justify-content: space-around;
    flex: 0 0 100%;
    scroll-snap-align: start;
    gap: 8px;
  }

  .adjust-selector-btn {
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: 6px;
    border: none;
    background: transparent;
    cursor: pointer;
    padding: 0;
    transition: transform 0.1s;
  }
  .adjust-selector-btn:active {
    transform: scale(0.92);
  }

  .adjust-selector-circle {
    width: 52px;
    height: 52px;
    border-radius: 50%;
    background: var(--surface-2);
    display: flex;
    align-items: center;
    justify-content: center;
    color: var(--text-3);
    transition: background 0.2s, color 0.2s, box-shadow 0.2s;
  }
  .adjust-selector-circle.active {
    background: var(--sel-color, var(--accent));
    color: var(--text-1);
    box-shadow: 0 3px 10px rgba(0,0,0,0.15);
  }

  .adjust-selector-label {
    font-size: 0.7rem;
    font-weight: 600;
    color: var(--text-1);
    text-align: center;
  }

  /* ── Adjust Slider Section ─────────────── */
  .adjust-slider-section {
    display: flex;
    flex-direction: column;
    gap: 10px;
  }

  .adjust-slider-header {
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 0 2px;
  }

  .adjust-slider-prop-label {
    font-size: 0.85rem;
    font-weight: 600;
    color: var(--text-1);
  }

  .adjust-slider-value {
    font-size: 0.85rem;
    font-weight: 700;
    color: var(--text-1);
    font-variant-numeric: tabular-nums;
  }

  .adjust-slider-controls {
    display: flex;
    align-items: center;
    gap: 8px;
  }

  .adjust-step-btn {
    width: 32px;
    height: 32px;
    border-radius: 10px;
    border: 1.5px solid var(--line);
    background: var(--surface-1);
    display: flex;
    align-items: center;
    justify-content: center;
    cursor: pointer;
    color: var(--text-2);
    flex-shrink: 0;
    transition: background 0.15s, border-color 0.15s;
  }
  .adjust-step-btn:hover {
    background: var(--surface-2);
    border-color: var(--line-strong);
  }

  .adjust-slider-bar-wrap {
    flex: 1;
    padding: 16px 0;
  }

  .adjust-slider-bar {
    position: relative;
    width: 100%;
    height: 5px;
    border-radius: 4px;
    background: var(--surface-3);
  }

  .adjust-slider-fill {
    position: absolute;
    top: 0;
    height: 100%;
    border-radius: 4px;
    background: var(--yellow, var(--accent));
    pointer-events: none;
  }

  .adjust-slider-handle {
    position: absolute;
    top: 50%;
    width: 20px;
    height: 20px;
    border-radius: 50%;
    background: var(--yellow, var(--accent));
    border: 3px solid var(--line);
    box-shadow: 0 1px 4px rgba(0,0,0,0.25);
    transform: translate(-50%, -50%);
    pointer-events: none;
    z-index: 1;
  }
  .adjust-slider-handle.current {
    z-index: 2;
    pointer-events: auto;
    touch-action: none;
    cursor: grab;
  }
  .adjust-slider-handle.current:active {
    cursor: grabbing;
  }

  /* ── All Adjust Page (full screen) ─────── */
  .all-adjust-page {
    flex: 1;
    display: flex;
    flex-direction: column;
    padding: 16px 14px 24px;
    overflow-y: auto;
    background: var(--surface-1);
  }
  .all-adjust-page::-webkit-scrollbar {
    width: 4px;
  }
  .all-adjust-page::-webkit-scrollbar-thumb {
    background: var(--surface-3);
    border-radius: 4px;
  }

  .all-adjust-header {
    display: flex;
    align-items: center;
    justify-content: space-between;
    margin-bottom: 20px;
    flex-shrink: 0;
  }

  .all-adjust-title {
    font-size: 1.1rem;
    font-weight: 700;
    color: var(--text-1);
    margin: 0;
  }

  .all-adjust-list {
    display: flex;
    flex-direction: column;
    gap: 18px;
    flex-shrink: 0;
  }

  .all-adjust-item {
    background: var(--surface-2);
    border-radius: 14px;
    padding: 14px 16px;
  }

  .all-adjust-item-header {
    display: flex;
    align-items: center;
    justify-content: space-between;
    margin-bottom: 10px;
  }

  .all-adjust-item-label {
    font-size: 0.9rem;
    font-weight: 600;
    color: var(--text-1);
  }

  .all-adjust-item-value {
    font-size: 0.85rem;
    font-weight: 700;
    color: var(--text-1);
    font-variant-numeric: tabular-nums;
  }

  .all-adjust-slider-row {
    display: flex;
    align-items: center;
    gap: 8px;
    padding: 4px 0;
  }

  .all-adjust-slider-container {
    flex: 1;
    padding: 10px 0 4px;
  }

  .all-adjust-slider-bar {
    position: relative;
    width: 100%;
    height: 6px;
    border-radius: 4px;
    background: var(--surface-3);
  }

  .all-adjust-slider-fill {
    position: absolute;
    top: 0;
    height: 100%;
    border-radius: 4px;
    background: var(--yellow, var(--accent));
    pointer-events: none;
  }

  .all-adjust-slider-handle {
    position: absolute;
    top: 50%;
    width: 18px;
    height: 18px;
    border-radius: 50%;
    background: var(--yellow, var(--accent));
    border: 3px solid var(--line);
    box-shadow: 0 1px 4px rgba(0,0,0,0.25);
    transform: translate(-50%, -50%);
    pointer-events: none;
    z-index: 1;
  }
  .all-adjust-slider-handle.current {
    z-index: 2;
    pointer-events: auto;
    touch-action: none;
    cursor: grab;
  }
  .all-adjust-slider-handle.current:active {
    cursor: grabbing;
  }

  .content-spacer {
    height: 8px;
    flex-shrink: 0;
  }

  /* ════════════════════════════════════════
     BOTTOM NAV
     ════════════════════════════════════════ */
  /* A dock, not a bar: floating a little above the bottom edge, glass over
     whatever scrolls beneath it. The active item gets the accent and a
     small indicator; everything else stays quiet. */
  .bottom-nav {
    position: relative;
    display: flex;
    align-items: stretch;
    margin: 0 auto max(10px, env(safe-area-inset-bottom, 10px));
    width: calc(100% - 24px);
    max-width: 560px;   /* a dock, not a bar: on a tablet or laptop it stays hand-sized */
    padding: 6px;
    border-radius: 24px;
    background: rgba(26, 26, 30, 0.82);
    border: 1px solid var(--line-strong);
    box-shadow: inset 0 1px 0 var(--highlight), 0 12px 32px rgba(0, 0, 0, 0.45);
    backdrop-filter: blur(18px) saturate(140%);
    -webkit-backdrop-filter: blur(18px) saturate(140%);
    flex-shrink: 0;
    z-index: 30;
  }
  :global([data-theme="light"]) .bottom-nav {
    background: rgba(255, 255, 255, 0.84);
    box-shadow: inset 0 1px 0 #fff, 0 12px 32px rgba(17, 17, 20, 0.14);
  }
  .nav-eyes {
    display: flex;
    align-items: center;
    justify-content: center;
    width: 26px;
    height: 26px;
    color: currentColor;
    transition: color 0.2s var(--ease), filter 0.2s var(--ease);
  }
  .nav-item.active .nav-eyes { color: var(--accent-ui); filter: drop-shadow(0 0 6px rgba(255, 230, 5, 0.5)); }

  .nav-item {
    flex: 1;
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: 3px;
    padding: 8px 0 7px;
    border: none;
    border-radius: 18px;
    background: transparent;
    cursor: pointer;
    color: var(--text-3);
    transition: color 0.2s var(--ease), background-color 0.2s var(--ease), transform 0.2s var(--ease);
  }
  .nav-item:active { transform: scale(0.94); }
  .nav-item.active {
    color: var(--text-1);
    background: var(--surface-2);
    box-shadow: inset 0 1px 0 var(--highlight);
  }
  .nav-item.active .nav-icon svg { stroke: var(--accent-ui); filter: drop-shadow(0 0 6px rgba(255, 230, 5, 0.4)); }

  .nav-icon {
    display: flex;
    align-items: center;
    justify-content: center;
    height: 26px;
  }
  .nav-icon svg { width: 24px; height: 24px; }

  .nav-label {
    font-size: 11px;
    font-weight: 600;
    letter-spacing: 0.01em;
    max-width: 100%;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }

  /* ════════════════════════════════════════
     UPLOAD PAGE
     ════════════════════════════════════════ */
  .upload-page {
    flex: 1;
    display: flex;
    flex-direction: column;
  }

  .upload-inner {
    flex: 1;
    display: flex;
    flex-direction: column;
  }

  /* ════════════════════════════════════════
     SETTINGS PAGE
     ════════════════════════════════════════ */
  .settings-page {
    flex: 1;
    background: var(--bg);
    display: flex;
    flex-direction: column;
  }

  .settings-header {
    display: flex;
    align-items: center;
    padding: 18px 16px 6px;
    flex-shrink: 0;
  }

  .settings-title {
    font-size: 24px;
    font-weight: 700;
    letter-spacing: -0.02em;
    color: var(--text-1);
    margin: 0;
  }

  .settings-list {
    padding: 8px 16px 24px;
    display: flex;
    flex-direction: column;
    gap: 8px;
    flex-shrink: 0;
  }
  .settings-group-label {
    font-size: 12px;
    font-weight: 600;
    letter-spacing: 0.04em;
    text-transform: uppercase;
    color: var(--text-3);
    padding: 14px 2px 4px;
  }
  .theme-row {
    display: grid;
    grid-template-columns: repeat(3, 1fr);
    gap: 4px;
    padding: 4px;
    background: var(--surface-2);
    border: 1px solid var(--line);
    border-radius: var(--r-sm);
  }
  .theme-opt {
    height: 36px;
    border: none;
    border-radius: 7px;
    background: transparent;
    color: var(--text-2);
    font-size: 14px;
    font-weight: 600;
    cursor: pointer;
    transition: background 0.15s, color 0.15s;
  }
  .theme-opt.active {
    background: var(--surface-1);
    color: var(--text-1);
    box-shadow: 0 1px 2px rgba(0, 0, 0, 0.25);
  }

  .settings-item {
    display: flex;
    align-items: center;
    gap: 14px;
    width: 100%;
    padding: 14px 16px;
    border: 1px solid var(--line);
    background: var(--surface-1);
    border-radius: var(--r-md);
    cursor: pointer;
    text-align: left;
    transition: background 0.15s;
    box-shadow: var(--shadow-card);
  }
  .settings-item:active {
    background: var(--surface-2);
  }

  .settings-item-icon {
    width: 40px;
    height: 40px;
    border-radius: 10px;
    background: var(--surface-2);
    display: flex;
    align-items: center;
    justify-content: center;
    color: var(--text-2);
    flex-shrink: 0;
  }

  .settings-item-content {
    flex: 1;
    display: flex;
    flex-direction: column;
    gap: 2px;
  }

  .settings-item-label {
    font-size: 15px;
    font-weight: 600;
    color: var(--text-1);
  }

  .settings-item-desc {
    font-size: 13px;
    font-weight: 400;
    color: var(--text-2);
  }

  .settings-chevron {
    color: var(--text-3);
    flex-shrink: 0;
  }
</style>
