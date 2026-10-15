<script>
  import { listConversations } from "../conversationStore.js";

  let {
    activeId,
    onSelect,
    onNew,
    onDelete,
    refreshKey = 0,
  } = $props();

  let conversations = $state([]);
  let searchText = $state("");

  function refresh() {
    conversations = listConversations();
  }

  $effect(() => {
    refreshKey;
    refresh();
  });

  /** Group conversations by date category */
  function getDateLabel(ts) {
    const d = new Date(ts);
    const now = new Date();
    const today = new Date(now.getFullYear(), now.getMonth(), now.getDate());
    const dateDay = new Date(d.getFullYear(), d.getMonth(), d.getDate());
    const diffDays = Math.round((today - dateDay) / 86400000);

    if (diffDays === 0) return "Today";
    if (diffDays === 1) return "Yesterday";
    if (diffDays < 7) return "Earlier this week";
    return d.toLocaleDateString([], { month: "short", day: "numeric", year: "numeric" });
  }

  /** Group conversations by category */
  let grouped = $derived.by(() => {
    const list = searchText.trim()
      ? conversations.filter((c) =>
          c.title.toLowerCase().includes(searchText.toLowerCase())
        )
      : conversations;

    const groups = {};
    for (const c of list) {
      const label = getDateLabel(c.updatedAt);
      if (!groups[label]) groups[label] = [];
      groups[label].push(c);
    }
    return groups;
  });

  function handleDelete(id, e) {
    e.stopPropagation();
    import("../conversationStore.js").then(({ deleteConversation }) => {
      deleteConversation(id);
      refresh();
      if (activeId === id) onDelete?.(id);
    });
  }
</script>

<div class="sidebar">
  <!-- Header -->
  <div class="sidebar-header">
    <h3 class="sidebar-title">Chats</h3>
    <button class="new-btn" onclick={onNew} aria-label="New conversation">
      <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
        <line x1="12" y1="5" x2="12" y2="19"/>
        <line x1="5" y1="12" x2="19" y2="12"/>
      </svg>
    </button>
  </div>

  <!-- Search -->
  <div class="search-box">
    <svg class="search-icon" width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
      <circle cx="11" cy="11" r="8"/>
      <line x1="21" y1="21" x2="16.65" y2="16.65"/>
    </svg>
    <input
      bind:value={searchText}
      placeholder="Search chats…"
      class="search-input"
    />
  </div>

  <!-- Conversation list -->
  <div class="convo-list">
    {#if Object.keys(grouped).length === 0}
      <p class="empty-msg">{searchText ? "No matching conversations." : "No conversations yet."}</p>
    {:else}
      {#each Object.entries(grouped) as [label, convos]}
        <div class="group">
          <span class="group-label">{label}</span>
          {#each convos as convo (convo.id)}
            <button
              class="convo-item"
              class:active={convo.id === activeId}
              onclick={() => onSelect?.(convo.id)}
            >
              <div class="convo-content">
                <p class="convo-title">{convo.title}</p>
                {#if convo.messages?.length}
                  <p class="convo-preview">{convo.messages[convo.messages.length - 1]?.text?.slice(0, 80)}</p>
                {/if}
              </div>
              <div class="convo-meta">
                <span class="convo-time">{formatTime(convo.updatedAt)}</span>
                <!-- svelte-ignore a11y_click_events_have_key_events -->
                <span
                  class="delete-btn"
                  onclick={(e) => handleDelete(convo.id, e)}
                  aria-label="Delete"
                >
                  <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
                    <line x1="18" y1="6" x2="6" y2="18"/>
                    <line x1="6" y1="6" x2="18" y2="18"/>
                  </svg>
                </span>
              </div>
            </button>
          {/each}
        </div>
      {/each}
    {/if}
  </div>
</div>

<script context="module">
  function formatTime(ts) {
    const d = new Date(ts);
    const now = new Date();
    const diffMs = now - d;
    const diffDays = Math.floor(diffMs / 86400000);
    if (diffDays === 0) {
      return d.toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" });
    }
    if (diffDays === 1) return "Yesterday";
    if (diffDays < 7) return `${diffDays}d ago`;
    return d.toLocaleDateString([], { month: "short", day: "numeric" });
  }
</script>

<style>
  .sidebar {
    height: 100%;
    display: flex;
    flex-direction: column;
    background: var(--surface-1);
    border-right: 1px solid var(--line);
    padding-top: env(safe-area-inset-top, 0px);
  }
  .sidebar-header {
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 18px 16px 10px;
  }
  .sidebar-title {
    font-size: 20px;
    font-weight: 700;
    letter-spacing: -0.01em;
    color: var(--text-1);
  }
  .new-btn {
    width: 34px; height: 34px; border-radius: 50%;
    border: none;
    background: var(--accent);
    color: var(--accent-ink);
    display: inline-flex; align-items: center; justify-content: center;
    cursor: pointer;
    box-shadow: 0 6px 14px rgba(255, 230, 5, 0.22);
    transition: transform 200ms var(--ease);
  }
  .new-btn:active { transform: scale(0.94); }

  .search-box {
    position: relative;
    margin: 0 12px 10px;
    color: var(--text-3);
  }
  .search-icon { position: absolute; left: 12px; top: 50%; transform: translateY(-50%); pointer-events: none; }
  .search-input {
    width: 100%;
    height: 38px;
    padding: 0 12px 0 34px;
    border-radius: var(--r-pill);
    border: 1px solid var(--line);
    background: var(--surface-2);
    color: var(--text-1);
    font-family: inherit;
    font-size: 13px;
    outline: none;
    transition: border-color 200ms var(--ease);
  }
  .search-input::placeholder { color: var(--text-3); }
  .search-input:focus { border-color: var(--accent-line); }

  .convo-list { flex: 1; overflow-y: auto; padding: 0 8px 16px; }
  .group { margin-bottom: 8px; }
  .group-label {
    display: block;
    padding: 10px 10px 4px;
    font-size: 10.5px;
    font-weight: 700;
    letter-spacing: 0.1em;
    text-transform: uppercase;
    color: var(--text-3);
  }
  .convo-item {
    width: 100%;
    display: flex;
    align-items: flex-start;
    gap: 8px;
    padding: 10px 10px 10px 12px;
    border-radius: var(--r-sm);
    border: 1px solid transparent;
    background: transparent;
    text-align: left;
    cursor: pointer;
    position: relative;
    transition: background-color 200ms var(--ease), border-color 200ms var(--ease);
  }
  .convo-item:hover { background: var(--surface-2); }
  .convo-item.active { background: var(--surface-2); border-color: var(--line); }
  .convo-item.active::before {
    content: "";
    position: absolute;
    left: 0; top: 12px; bottom: 12px;
    width: 3px; border-radius: 3px;
    background: var(--accent);
  }
  .convo-content { flex: 1; min-width: 0; }
  .convo-title {
    font-size: 13.5px;
    font-weight: 600;
    color: var(--text-1);
    overflow: hidden; text-overflow: ellipsis; white-space: nowrap;
  }
  .convo-preview {
    font-size: 12px;
    color: var(--text-3);
    margin-top: 2px;
    overflow: hidden; text-overflow: ellipsis; white-space: nowrap;
  }
  .convo-meta { display: flex; flex-direction: column; align-items: flex-end; gap: 6px; flex-shrink: 0; }
  .convo-time { font-size: 10.5px; color: var(--text-3); font-variant-numeric: tabular-nums; }
  .delete-btn {
    width: 22px; height: 22px; border-radius: 50%;
    display: inline-flex; align-items: center; justify-content: center;
    color: var(--text-3);
    background: var(--surface-3);
    opacity: 0.7;
    transition: color 200ms var(--ease), background-color 200ms var(--ease), opacity 200ms var(--ease);
  }
  .delete-btn:hover { color: var(--danger); opacity: 1; }
  .empty-msg { padding: 24px 12px; text-align: center; font-size: 13px; color: var(--text-3); }
</style>
