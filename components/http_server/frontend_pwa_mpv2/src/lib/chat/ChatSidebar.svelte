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
    <h3 class="sidebar-title">Conversations</h3>
    <button class="new-btn" onclick={onNew} aria-label="New conversation">
      <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
        <line x1="12" y1="5" x2="12" y2="19"/>
        <line x1="5" y1="12" x2="19" y2="12"/>
      </svg>
    </button>
  </div>

  <!-- Search -->
  <div class="search-box">
    <svg class="search-icon" width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="#999" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
      <circle cx="11" cy="11" r="8"/>
      <line x1="21" y1="21" x2="16.65" y2="16.65"/>
    </svg>
    <input
      bind:value={searchText}
      placeholder="Search chat history…"
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
    width: 280px;
    height: 100%;
    display: flex;
    flex-direction: column;
    background: #fff;
    border-right: 1px solid #e8e8e8;
    flex-shrink: 0;
  }

  .sidebar-header {
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 14px 14px 10px;
  }

  .sidebar-title {
    font-size: 0.85rem;
    font-weight: 700;
    color: #333;
    text-transform: uppercase;
    letter-spacing: 0.04em;
  }

  .new-btn {
    width: 30px;
    height: 30px;
    border-radius: 50%;
    border: none;
    background: #f0f0f0;
    color: #666;
    cursor: pointer;
    display: flex;
    align-items: center;
    justify-content: center;
    transition: background 0.15s;
  }
  .new-btn:hover {
    background: #ffeb3b;
    color: #222;
  }

  .search-box {
    position: relative;
    margin: 0 14px 10px;
  }

  .search-icon {
    position: absolute;
    left: 12px;
    top: 50%;
    transform: translateY(-50%);
    pointer-events: none;
  }

  .search-input {
    width: 100%;
    border: none;
    outline: none;
    background: #f3f3f3;
    border-radius: 100px;
    padding: 10px 14px 10px 36px;
    font-size: 0.8rem;
    color: #333;
  }
  .search-input::placeholder {
    color: #bbb;
  }

  .convo-list {
    flex: 1;
    overflow-y: auto;
    padding: 0 6px 14px;
  }

  .group {
    margin-bottom: 6px;
  }

  .group-label {
    display: block;
    font-size: 0.65rem;
    font-weight: 600;
    color: #999;
    text-transform: uppercase;
    letter-spacing: 0.06em;
    padding: 8px 10px 4px;
  }

  .convo-item {
    display: flex;
    align-items: flex-start;
    justify-content: space-between;
    gap: 6px;
    width: 100%;
    text-align: left;
    background: none;
    border: none;
    border-radius: 8px;
    padding: 10px;
    cursor: pointer;
    transition: background 0.12s;
  }
  .convo-item:hover {
    background: #f5f5f5;
  }
  .convo-item.active {
    background: #fff8e1;
  }

  .convo-content {
    flex: 1;
    min-width: 0;
  }

  .convo-title {
    font-size: 0.8rem;
    font-weight: 600;
    color: #333;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }

  .convo-preview {
    font-size: 0.7rem;
    color: #999;
    margin-top: 2px;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }

  .convo-meta {
    display: flex;
    flex-direction: column;
    align-items: flex-end;
    gap: 4px;
    flex-shrink: 0;
  }

  .convo-time {
    font-size: 0.6rem;
    color: #bbb;
  }

  .delete-btn {
    width: 18px;
    height: 18px;
    border-radius: 50%;
    display: flex;
    align-items: center;
    justify-content: center;
    color: #ccc;
    transition: all 0.12s;
    opacity: 0;
  }
  .convo-item:hover .delete-btn,
  .convo-item.active .delete-btn {
    opacity: 1;
  }
  .delete-btn:hover {
    background: #fee2e2;
    color: #ef4444;
  }

  .empty-msg {
    font-size: 0.75rem;
    color: #bbb;
    text-align: center;
    padding: 40px 14px;
  }
</style>
