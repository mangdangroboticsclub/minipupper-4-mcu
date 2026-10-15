<script>
  import { colors } from "./colors.js";
  import ActionIcon from "./ActionIcon.svelte";

  let {
    assignments = {},
    onSave,
    onBack,
    customActions = [],
    onAddAction,
    onDeleteAction,
  } = $props();

  const buttonPositions = [
    { id: 'action-top',    label: 'Top',    color: colors.mpx.action_green,  icon: 'triangle' },
    { id: 'action-right',  label: 'Right',  color: colors.mpx.action_red,    icon: 'circle' },
    { id: 'action-bottom', label: 'Bottom', color: colors.mpx.action_blue,   icon: 'pentagon' },
    { id: 'action-left',   label: 'Left',   color: colors.mpx.action_purple, icon: 'square' },
  ];

  // ── Built-in actions with emoji + category ───────────────
  const builtinActions = [
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
    // minipupperesp moves (firmware names; one-shots end by themselves)
    { id: "mate", label: "Mate", emoji: "🐾", color: "#14A37F", category: "gait" },
    { id: "alive", label: "Alive", emoji: "💓", color: "#14A37F", category: "gait" },
    { id: "charshow", label: "Show Off", emoji: "🎭", color: "#AD5FBF", category: "gait" },
    { id: "wakeup", label: "Wake Up", emoji: "😴", color: "#E08A2E", category: "gait" },
    { id: "look", label: "Look Around", emoji: "👀", color: "#E08A2E", category: "gait" },
    { id: "bow", label: "Play Bow", emoji: "🎉", color: "#E08A2E", category: "gait" },
    { id: "trot", label: "Happy Trot", emoji: "🐾", color: "#E08A2E", category: "gait" },
    { id: "chase", label: "Chase Tail", emoji: "🌀", color: "#E08A2E", category: "gait" },
    { id: "dig", label: "Dig", emoji: "⛏️", color: "#E08A2E", category: "gait" },
    { id: "scratch", label: "Scratch", emoji: "🐕", color: "#E08A2E", category: "gait" },
    { id: "shake", label: "Shake Off", emoji: "💦", color: "#E08A2E", category: "gait" },
    { id: "cower", label: "Scared", emoji: "😨", color: "#E08A2E", category: "gait" },
    { id: "paw", label: "Give Paw", emoji: "🤝", color: "#E08A2E", category: "gait" },
    { id: "tripod", label: "Three Legs", emoji: "🦮", color: "#E08A2E", category: "gait" },
    { id: "situp", label: "Sit Up + Clap", emoji: "🙌", color: "#C44569", category: "gait" },
    { id: "rearclap", label: "Rear + Clap", emoji: "👏", color: "#C44569", category: "gait" },
    { id: "surrender", label: "Hands Up", emoji: "🙆", color: "#C44569", category: "gait" },
    { id: "sixseven", label: "6 7", emoji: "🤚", color: "#C44569", category: "gait" },
    { id: "pushup", label: "Push-ups", emoji: "💪", color: "#C44569", category: "gait" },
    { id: "runningman", label: "Running Man", emoji: "🏃", color: "#C44569", category: "gait" },
    { id: "moonwalk", label: "Moonwalk", emoji: "🌙", color: "#C44569", category: "gait" },
    { id: "spin90", label: "Spin 90°", emoji: "🔃", color: "#D97A29", category: "gait" },
    { id: "longjump", label: "Long Jump", emoji: "🦘", color: "#D97A29", category: "gait" },
    { id: "farjump", label: "Far Jump", emoji: "🚀", color: "#D97A29", category: "gait" },
    { id: "backflip", label: "Backflip", emoji: "🤸", color: "#D14949", category: "gait" },
  ];

  // ── Merge built-in + custom actions ──────────────────────
  let allActions = $derived([...builtinActions, ...customActions]);

  // ── State ────────────────────────────────────────────────
  let selectedPosition = $state(buttonPositions[0].id);
  let searchQuery = $state("");
  let showGaitSection = $state(true);

  // Clone assignments for local editing — auto-saves on each change
  let localAssignments = $state({ ...assignments });

  // ── Derived ──────────────────────────────────────────────
  let selectedPos = $derived(
    buttonPositions.find(p => p.id === selectedPosition) ?? buttonPositions[0]
  );

  let filteredActions = $derived.by(() => {
    if (!searchQuery) return allActions;
    const q = searchQuery.toLowerCase();
    return allActions.filter(a =>
      a.label.toLowerCase().includes(q) ||
      a.emoji.includes(q)
    );
  });

  let gaitActions = $derived(filteredActions.filter(a => a.category === "gait"));
  let skillActions = $derived(filteredActions.filter(a => a.category !== "gait"));

  function getAssignedAction(posId) {
    const actionId = localAssignments[posId];
    if (!actionId) return null;
    return allActions.find(a => a.id === actionId) ?? null;
  }

  function selectPosition(posId) {
    selectedPosition = posId;
    searchQuery = "";
  }

  function assignAction(actionId) {
    const updated = { ...localAssignments, [selectedPosition]: actionId };
    localAssignments = updated;
    onSave?.(updated);
  }

  function clearAssignment(posId) {
    const next = { ...localAssignments };
    delete next[posId];
    localAssignments = next;
    onSave?.(next);
  }
</script>

<div class="action-assignment-page">
  <!-- Header bar with back + title -->
  <header class="top-bar" style="background: {colors.mpx.primary}">
    <button class="back-btn" onclick={onBack} aria-label="Back">
      <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
        <line x1="19" y1="12" x2="5" y2="12"/>
        <polyline points="12 19 5 12 12 5"/>
      </svg>
    </button>
    <h1 class="top-bar-title">Button Assignment</h1>
    <div style="width:32px"></div>
  </header>

  <!-- ═══ Action Buttons Bar ═══ -->
  <div class="buttons-bar">
    {#each buttonPositions as pos}
      {@const assigned = getAssignedAction(pos.id)}
      <button
        class="btn-selector"
        class:selected={selectedPosition === pos.id}
        style="--btn-color: {pos.color}"
        onclick={() => selectPosition(pos.id)}
        aria-label="Select {pos.label} button"
      >
        <div class="btn-selector-icon">
          {#if pos.icon === "triangle"}
            <svg viewBox="0 0 50 50">
              <path d="M25 6 L40 34.5 L10 34.5 Z" fill="none" stroke="currentColor" stroke-width="5" stroke-linejoin="round"/>
            </svg>
          {:else if pos.icon === "circle"}
            <svg viewBox="0 0 50 50">
              <circle cx="25" cy="25" r="15" fill="none" stroke="currentColor" stroke-width="5"/>
            </svg>
          {:else if pos.icon === "pentagon"}
            <svg viewBox="0 0 50 50">
              <polygon points="25,10 39.3,20.4 33.8,37.1 16.2,37.1 10.7,20.4" fill="none" stroke="currentColor" stroke-width="5" stroke-linejoin="round"/>
            </svg>
          {:else if pos.icon === "square"}
            <svg viewBox="0 0 50 50">
              <rect x="13" y="13" width="24" height="24" rx="3" fill="none" stroke="currentColor" stroke-width="5"/>
            </svg>
          {/if}
        </div>
        <span class="btn-selector-label" class:unset={!assigned}>
          {assigned ? assigned.label : "—"}
        </span>
      </button>
    {/each}
  </div>

  <!-- ═══ Action Picker ═══ -->
  <div class="picker-section">
    <!-- Search bar -->
    <div class="search-bar">
      <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
        <circle cx="11" cy="11" r="8"/>
        <line x1="21" y1="21" x2="16.65" y2="16.65"/>
      </svg>
      <input
        type="text"
        class="search-input"
        placeholder="Search actions..."
        bind:value={searchQuery}
      />
    </div>

    <!-- Assigning hint -->
    <div class="assign-hint" style="color: {selectedPos.color}">
      Tap an action to assign to <strong>{selectedPos.label}</strong>
      {#if getAssignedAction(selectedPosition)}
        <button class="clear-btn" onclick={() => clearAssignment(selectedPosition)}>
          Clear
        </button>
      {/if}
    </div>

    <!-- ═══ Gait Section (collapsible) ═══ -->
    {#if gaitActions.length > 0}
      <button class="category-header collapsible" onclick={() => showGaitSection = !showGaitSection}>
        <span class="category-label">🚶 Gait</span>
        <svg class="collapse-chevron" class:collapsed={!showGaitSection} width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
          <polyline points="6 9 12 15 18 9"/>
        </svg>
      </button>
      {#if showGaitSection}
      <div class="actions-grid">
        {#each gaitActions as action}
          {@const isAssigned = localAssignments[selectedPosition] === action.id}
          <button
            class="action-item"
            class:selected={isAssigned}
            style="--action-color: {action.color}"
            onclick={() => assignAction(action.id)}
          >
            <div class="action-icon" style="background: {action.color}">
              <ActionIcon id={action.id} emoji={action.emoji} />
            </div>
            <span class="action-label">{action.label}</span>
            {#if isAssigned}
              <div class="action-check" style="background: {action.color}">
                <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="white" stroke-width="4" stroke-linecap="round" stroke-linejoin="round">
                  <polyline points="20 6 9 17 4 12"/>
                </svg>
              </div>
            {/if}
          </button>
        {/each}
      </div>
      {/if}
    {/if}

    <!-- ═══ Skill Section ═══ -->
    {#if skillActions.length > 0}
      <div class="category-header">
        <span class="category-label">🎯 Skill</span>
      </div>
      <div class="actions-grid">
        {#each skillActions as action}
          {@const isAssigned = localAssignments[selectedPosition] === action.id}
          {@const isCustom = action.category === "skill" || action.category === "custom"}
          <div
            class="action-item"
            class:selected={isAssigned}
            style="--action-color: {action.color}"
            role="button"
            tabindex="0"
            onclick={() => assignAction(action.id)}
          >
            <div class="action-icon" style="background: {action.color}">
              <ActionIcon id={action.id} emoji={action.emoji} />
            </div>
            <span class="action-label">{action.label}</span>
            {#if isAssigned}
              <div class="action-check" style="background: {action.color}">
                <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="white" stroke-width="4" stroke-linecap="round" stroke-linejoin="round">
                  <polyline points="20 6 9 17 4 12"/>
                </svg>
              </div>
            {/if}
            {#if isCustom}
            <button
              class="delete-action-item-btn"
              onclick={(e) => {
                e.stopPropagation();
                onDeleteAction?.(action.id);
              }}
              aria-label="Delete {action.label}"
            >
              <svg width="10" height="10" viewBox="0 0 24 24" fill="none" stroke="white" stroke-width="3" stroke-linecap="round" stroke-linejoin="round">
                <polyline points="3 6 5 6 21 6"/>
                <path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2"/>
              </svg>
            </button>
            {/if}
          </div>
        {/each}
      </div>
    {/if}

    <!-- ═══ Add Action Button ═══ -->
    <button class="add-action-btn" onclick={() => onAddAction?.()}>
      <span class="add-icon">➕</span>
      <span class="add-label">Add Custom Action</span>
    </button>

    {#if filteredActions.length === 0}
      <div class="no-results">No actions found</div>
    {/if}
  </div>
</div>

<style>
  .action-assignment-page {
    position: absolute;
    inset: 0;
    background: #fff;
    display: flex;
    flex-direction: column;
    z-index: 50;
  }

  .top-bar {
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 10px 14px;
    flex-shrink: 0;
  }

  .back-btn {
    width: 32px;
    height: 32px;
    display: flex;
    align-items: center;
    justify-content: center;
    border: none;
    background: rgba(0,0,0,0.15);
    border-radius: 50%;
    cursor: pointer;
    color: #000;
    transition: background 0.15s;
  }
  .back-btn:hover { background: rgba(0,0,0,0.25); }

  .top-bar-title {
    font-size: 1rem;
    font-weight: 700;
    color: #000;
    margin: 0;
  }

  /* ════════════════════════════════════════
     BUTTONS BAR
     ════════════════════════════════════════ */
  .buttons-bar {
    display: flex;
    justify-content: center;
    gap: 8px;
    padding: 12px 14px;
    background: #f8f8f8;
    flex-shrink: 0;
  }

  .btn-selector {
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: 4px;
    padding: 8px 10px;
    border: 2px solid transparent;
    border-radius: 12px;
    background: #fff;
    cursor: pointer;
    transition: all 0.15s;
    min-width: 0;
    flex: 1;
    max-width: 80px;
    box-shadow: 0 1px 3px rgba(0,0,0,0.08);
  }
  .btn-selector:hover {
    background: #f5f5f5;
  }
  .btn-selector.selected {
    border-color: var(--btn-color);
    background: #fff;
    box-shadow: 0 0 0 1px var(--btn-color), 0 2px 8px rgba(0,0,0,0.1);
  }

  .btn-selector.selected .btn-selector-icon {
    background: var(--btn-color);
    border-radius: 50%;
    color: #fff;
  }

  .btn-selector-icon {
    width: 36px;
    height: 36px;
    display: flex;
    align-items: center;
    justify-content: center;
    color: var(--btn-color);
    transition: all 0.15s;
  }

  .btn-selector-icon svg {
    width: 65%;
    height: auto;
  }

  .btn-selector-label {
    font-size: 0.65rem;
    font-weight: 600;
    color: #000;
    text-align: center;
    line-height: 1.3;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
    max-width: 100%;
  }
  .btn-selector-label.unset {
    color: #ccc;
  }

  /* ════════════════════════════════════════
     PICKER SECTION
     ════════════════════════════════════════ */
  .picker-section {
    flex: 1;
    display: flex;
    flex-direction: column;
    padding: 12px 14px 24px;
    overflow-y: auto;
  }
  .picker-section::-webkit-scrollbar {
    width: 4px;
  }
  .picker-section::-webkit-scrollbar-thumb {
    background: #ddd;
    border-radius: 4px;
  }

  /* ── Search Bar ───────────────────────── */
  .search-bar {
    display: flex;
    align-items: center;
    gap: 8px;
    padding: 10px 14px;
    border-radius: 24px;
    background: #f5f5f5;
    border: 1.5px solid #e0e0e0;
    margin-bottom: 10px;
    color: #999;
    flex-shrink: 0;
  }
  .search-bar:focus-within {
    border-color: #FFE605;
    background: #fff;
  }

  .search-input {
    flex: 1;
    border: none;
    background: transparent;
    outline: none;
    font-size: 0.85rem;
    color: #000;
  }
  .search-input::placeholder {
    color: #bbb;
  }

  /* ── Assign hint ──────────────────────── */
  .assign-hint {
    display: flex;
    align-items: center;
    gap: 8px;
    font-size: 0.8rem;
    margin-bottom: 8px;
    flex-shrink: 0;
  }
  .assign-hint strong {
    font-weight: 700;
  }

  .clear-btn {
    padding: 2px 10px;
    border: 1.5px solid #ddd;
    border-radius: 12px;
    background: #fff;
    color: #999;
    font-size: 0.7rem;
    font-weight: 600;
    cursor: pointer;
    transition: all 0.15s;
    margin-left: auto;
  }
  .clear-btn:hover {
    border-color: #999;
    color: #666;
  }

  /* ── Category Header ──────────────────── */
  .category-header {
    display: flex;
    align-items: center;
    gap: 6px;
    padding: 8px 2px 6px;
    flex-shrink: 0;
  }
  .category-header.collapsible {
    cursor: pointer;
    border: none;
    background: none;
    width: 100%;
    border-radius: 8px;
    transition: background 0.15s;
  }
  .category-header.collapsible:hover {
    background: rgba(0,0,0,0.04);
  }

  .collapse-chevron {
    color: #888;
    transition: transform 0.2s;
    margin-left: auto;
    flex-shrink: 0;
  }
  .collapse-chevron.collapsed {
    transform: rotate(-90deg);
  }
  .category-label {
    font-size: 0.75rem;
    font-weight: 700;
    color: #888;
    text-transform: uppercase;
    letter-spacing: 0.5px;
  }

  /* ── Actions Grid ─────────────────────── */
  .actions-grid {
    display: grid;
    grid-template-columns: repeat(4, 1fr);
    gap: 10px;
    margin-bottom: 8px;
  }

  .action-item {
    position: relative;
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: 6px;
    padding: 12px 4px 10px;
    border: 2px solid transparent;
    border-radius: 14px;
    background: #fafafa;
    cursor: pointer;
    transition: all 0.15s;
  }
  .action-item:hover {
    background: #f0f0f0;
  }
  .action-item.selected {
    border-color: var(--action-color);
    background: #fff;
  }

  .action-icon {
    width: 44px;
    height: 44px;
    border-radius: 12px;
    display: flex;
    align-items: center;
    justify-content: center;
    color: #fff;
  }
  .action-icon :global(.action-emoji) {
    font-size: 1.7rem;
  }
  .action-label {
    font-size: 0.65rem;
    font-weight: 600;
    color: #000;
    text-align: center;
    line-height: 1.2;
  }

  .action-check {
    position: absolute;
    top: -4px;
    right: -4px;
    width: 18px;
    height: 18px;
    border-radius: 50%;
    display: flex;
    align-items: center;
    justify-content: center;
    box-shadow: 0 1px 3px rgba(0,0,0,0.2);
  }

  .delete-action-item-btn {
    position: absolute;
    top: -6px;
    left: -6px;
    width: 18px;
    height: 18px;
    border-radius: 50%;
    background: #d14949;
    border: 1.5px solid #fff;
    display: flex;
    align-items: center;
    justify-content: center;
    cursor: pointer;
    opacity: 0;
    transition: opacity 0.15s;
    padding: 0;
    box-shadow: 0 2px 4px rgba(0,0,0,0.2);
  }
  .action-item:hover .delete-action-item-btn {
    opacity: 1;
  }

  /* ── Add Action Button ────────────────── */
  .add-action-btn {
    display: flex;
    align-items: center;
    justify-content: center;
    gap: 8px;
    padding: 12px;
    border: 2px dashed #ccc;
    border-radius: 14px;
    background: #fafafa;
    cursor: pointer;
    transition: all 0.15s;
    margin-top: 8px;
    width: 100%;
  }
  .add-action-btn:hover {
    background: #f0f0f0;
    border-color: #FFE605;
  }

  .add-icon {
    font-size: 1.1rem;
  }
  .add-label {
    font-size: 0.8rem;
    font-weight: 600;
    color: #666;
  }

  .no-results {
    grid-column: 1 / -1;
    text-align: center;
    padding: 24px;
    color: #bbb;
    font-size: 0.85rem;
  }
</style>


