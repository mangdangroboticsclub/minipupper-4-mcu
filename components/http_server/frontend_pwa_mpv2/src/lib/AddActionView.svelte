<script>
  import { colors } from "./colors.js";

  let { onSave, onBack } = $props();

  const YELLOW = colors.mpx.primary;

  // ── File-system browser state (embedded FileViewer) ──
  let files = $state([]);
  let dirs = $state([]);
  let currentDir = $state("/");
  let fsInfo = $state({ total: 0, used: 0 });
  let loading = $state(true);
  let showHidden = $state(false);
  let searchQuery = $state("");

  function isWasm(n) { return n.endsWith(".wasm") || n.endsWith(".mpxe"); }
  function isLua(n)  { return n.endsWith(".lua"); }
  function isAllowed(n) { return isWasm(n) || isLua(n); }

  function icon(n) {
    if (isWasm(n)) return n.endsWith(".mpxe") ? "🧩" : "⚡";
    if (isLua(n))  return "🌙";
    if (n.endsWith(".gz")) return "📦";
    if (n.endsWith(".json")) return "📋";
    if (n.endsWith(".md")) return "📝";
    return "📄";
  }

  function fmtSize(b) {
    return b < 1024 ? b + " B" : (b / 1024).toFixed(1) + " KB";
  }

  async function fetchDir(dir) {
    loading = true;
    try {
      const [lr, ir] = await Promise.all([
        fetch("/v1/fs/list?path=" + encodeURIComponent(dir)),
        fetch("/v1/fs/info"),
      ]);
      if (lr.ok) { const d = await lr.json(); files = d.f || []; dirs = d.d || []; currentDir = dir; }
      if (ir.ok) fsInfo = await ir.json();
    } catch {
      files = [];
      dirs = [];
    }
    loading = false;
  }

  function goDir(d) { fetchDir(currentDir === "/" ? "/" + d : currentDir + "/" + d); }

  function goUp() {
    if (currentDir === "/") return;
    const p = currentDir.split("/").filter(Boolean); p.pop();
    fetchDir(p.length === 0 ? "/" : "/" + p.join("/"));
  }

  $effect(() => { fetchDir("/"); });

  // ── Filtering ──
  let filteredDirs = $derived(
    searchQuery
      ? dirs.filter(d => d.toLowerCase().includes(searchQuery.toLowerCase()))
      : dirs
  );
  let filteredFiles = $derived(
    searchQuery
      ? files.filter(f => f.n.toLowerCase().includes(searchQuery.toLowerCase()))
      : files
  );

  // ── Selection state ──
  let selectedFile = $state(null);
  let actionName = $state("");
  let actionEmoji = $state("");
  let customEmojiInput = $state("");

  const emojiSuggestions = [
    "🤖", "⚡", "🔥", "💨", "🌟", "🎯", "🎪", "🎭",
    "🦿", "🦾", "⚙️", "🔧", "🛠️", "🧠", "👀", "🦅",
    "🐉", "🦎", "🐢", "🐇", "🦊", "🐺", "🦁", "🐯",
    "🤿", "🏋️", "🤺", "🏃", "🧗", "🤹", "🎨", "🎵",
  ];

  let effectiveEmoji = $derived(customEmojiInput || actionEmoji || "⚙️");

  function selectFile(f) {
    const dirPath = currentDir === "/" ? "" : currentDir;
    let fileType = "lua";
    if (isWasm(f.n)) fileType = f.n.endsWith(".mpxe") ? "mpxe" : "wasm";
    selectedFile = {
      path: dirPath + "/" + f.n,
      name: f.n,
      type: fileType,
    };
    // Auto-suggest a name from the file name
    if (!actionName) {
      actionName = f.n.replace(/\.(wasm|lua)$/, "");
      actionName = actionName.charAt(0).toUpperCase() + actionName.slice(1);
    }
  }

  function handleSave() {
    if (!selectedFile || !actionName.trim()) return;

    const newAction = {
      id: "custom_" + Date.now(),
      label: actionName.trim(),
      emoji: effectiveEmoji,
      color: "#888",
      category: "skill",
      filePath: selectedFile.path,
      fileType: selectedFile.type,
    };

    onSave?.(newAction);
    onBack?.();
  }

  function isFormValid() {
    return selectedFile && actionName.trim().length > 0;
  }
</script>

<div class="add-action-page">
  <!-- Header -->
  <header class="top-bar" style="background: {YELLOW}">
    <button class="back-btn" onclick={onBack} aria-label="Back">
      <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
        <line x1="19" y1="12" x2="5" y2="12"/>
        <polyline points="12 19 5 12 12 5"/>
      </svg>
    </button>
    <h1 class="top-bar-title">Add Action</h1>
    <div style="width:32px"></div>
  </header>

  <div class="form-scroll">
    <!-- ═══ Step 1: Browse & pick a file ═══ -->
    <section class="form-section">
      <h2 class="section-title">1. Browse &amp; select a file</h2>
      <p class="section-desc">Navigate to a .wasm, .mpxe, or .lua file on the robot.</p>

      <!-- Search bar -->
      <div class="fv-search-wrap">
        <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
          <circle cx="11" cy="11" r="8"/>
          <line x1="21" y1="21" x2="16.65" y2="16.65"/>
        </svg>
        <input
          class="fv-search-input"
          type="text"
          placeholder="Search files…"
          bind:value={searchQuery}
        />
      </div>

      <!-- Storage bar -->
      <div class="fv-storage-bar" style="--bar-bg: {YELLOW}">
        <div class="fv-storage-row">
          <span class="fv-storage-path">📁 {currentDir === "/" ? "/" : currentDir}</span>
          <span class="fv-storage-actions">
            <button class="fv-pill-btn fv-hidden-toggle"
                    class:fv-hidden-active={showHidden}
                    onclick={() => { showHidden = !showHidden; }}>
              {showHidden ? '👁 Hidden' : '👁‍🗨'}
            </button>
            <button class="fv-pill-btn" onclick={() => fetchDir(currentDir)}>↻</button>
          </span>
        </div>
        <div class="fv-storage-track">
          <div class="fv-storage-fill" style="width:{fsInfo.total ? ((fsInfo.used / fsInfo.total) * 100) : 0}%"></div>
        </div>
        <span class="fv-storage-pct">{fsInfo.total ? ((fsInfo.used / fsInfo.total) * 100).toFixed(0) : 0}% used</span>
      </div>

      <!-- File / dir listing -->
      <div class="fv-list">
        {#if loading}
          <p class="fv-status">Loading…</p>
        {:else}
          {#if currentDir !== "/"}
            <button class="fv-dir-item" onclick={goUp}>
              <span class="fv-dir-icon">📂</span>
              <span class="fv-dir-name">..</span>
            </button>
          {/if}

          {#each filteredDirs as d}
            <button class="fv-dir-item" onclick={() => goDir(d)}>
              <span class="fv-dir-icon">📁</span>
              <span class="fv-dir-name">{d}</span>
            </button>
          {/each}

          {#each filteredFiles as f}
            {@const ok = isAllowed(f.n)}
            {#if showHidden || ok}
              <button
                class="fv-file-item"
                class:fv-dimmed={!ok}
                class:fv-file-selected={selectedFile && selectedFile.name === f.n && (currentDir === "/" ? "" : currentDir) + "/" + f.n === selectedFile.path}
                onclick={() => ok ? selectFile(f) : undefined}
                disabled={!ok}
              >
                <span class="fv-file-icon">{icon(f.n)}</span>
                <div class="fv-file-meta">
                  <span class="fv-file-name">{f.n}{#if f.r}<span class="fv-lock-indicator">🔒</span>{/if}</span>
                  <span class="fv-file-size">{fmtSize(f.s)}</span>
                </div>
                {#if selectedFile && selectedFile.name === f.n && (currentDir === "/" ? "" : currentDir) + "/" + f.n === selectedFile.path}
                  <span class="fv-file-check">✓</span>
                {/if}
              </button>
            {/if}
          {/each}
        {/if}
      </div>
    </section>

    <!-- ═══ Step 2: Name ═══ -->
    <section class="form-section">
      <h2 class="section-title">2. Give it a name</h2>
      <input
        type="text"
        class="name-input"
        placeholder="e.g. My Cool Action"
        bind:value={actionName}
        maxlength="24"
      />
      <span class="char-count">{actionName.length}/24</span>
    </section>

    <!-- ═══ Step 3: Pick an emoji ═══ -->
    <section class="form-section">
      <h2 class="section-title">3. Pick an emoji</h2>
      <p class="section-desc">Type any emoji below, or pick one from the grid.</p>

      <input
        type="text"
        class="emoji-text-input"
        placeholder="e.g. 🎉 or any text…"
        bind:value={customEmojiInput}
        maxlength="8"
      />

      <div class="emoji-grid">
        {#each emojiSuggestions as emoji}
          <button
            class="emoji-item"
            class:selected={actionEmoji === emoji && !customEmojiInput}
            onclick={() => { actionEmoji = emoji; customEmojiInput = ""; }}
          >
            {emoji}
          </button>
        {/each}
      </div>
      <div class="emoji-preview">
        Selected: <span class="emoji-preview-icon">{effectiveEmoji}</span>
      </div>
    </section>

    <!-- ═══ Preview ═══ -->
    {#if selectedFile}
      <section class="form-section preview-section">
        <h2 class="section-title">Preview</h2>
        <div class="preview-card">
          <div class="preview-icon" style="background: {YELLOW}">
            <span class="preview-emoji">{effectiveEmoji}</span>
          </div>
          <div class="preview-info">
            <span class="preview-name">{actionName || "Action Name"}</span>
            <span class="preview-file">{selectedFile.name}</span>
          </div>
        </div>
      </section>
    {/if}
  </div>

  <!-- ═══ Save Button ═══ -->
  <div class="footer-bar">
    <button
      class="save-btn"
      style="background: {YELLOW}"
      disabled={!isFormValid()}
      onclick={handleSave}
    >
      Save Action
    </button>
  </div>
</div>

<style>
  .add-action-page {
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

  .form-scroll {
    flex: 1;
    overflow-y: auto;
    padding: 16px 14px;
  }
  .form-scroll::-webkit-scrollbar {
    width: 4px;
  }
  .form-scroll::-webkit-scrollbar-thumb {
    background: #ddd;
    border-radius: 4px;
  }

  /* ── Sections ── */
  .form-section {
    margin-bottom: 20px;
  }

  .section-title {
    font-size: 0.85rem;
    font-weight: 700;
    color: #000;
    margin: 0 0 4px;
  }

  .section-desc {
    font-size: 0.75rem;
    color: #999;
    margin: 0 0 10px;
  }

  /* ═══ FileViewer-inspired styles ═══ */

  /* ── Search ── */
  .fv-search-wrap {
    display: flex;
    align-items: center;
    gap: 6px;
    padding: 8px 12px;
    border-radius: 12px;
    background: #f5f5f5;
    border: 1.5px solid #e0e0e0;
    margin-bottom: 8px;
    color: #999;
  }
  .fv-search-wrap:focus-within {
    border-color: #FFE605;
    background: #fff;
  }

  .fv-search-input {
    flex: 1;
    border: none;
    background: transparent;
    outline: none;
    font-size: 0.8rem;
    color: #000;
    font-family: inherit;
  }
  .fv-search-input::placeholder {
    color: #bbb;
  }

  /* ── Storage bar ── */
  .fv-storage-bar {
    padding: 8px 12px;
    background: #f5f5f5;
    border-radius: 10px;
    margin-bottom: 10px;
  }

  .fv-storage-row {
    display: flex;
    justify-content: space-between;
    align-items: center;
    font-size: 0.7rem;
    color: #666;
    margin-bottom: 4px;
  }

  .fv-storage-path {
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    font-size: 0.7rem;
    font-weight: 600;
    color: #444;
  }

  .fv-storage-actions {
    display: flex;
    align-items: center;
    gap: 4px;
    flex-shrink: 0;
    margin-left: 8px;
  }

  .fv-pill-btn {
    padding: 2px 8px;
    border: none;
    border-radius: 999px;
    background: rgba(0, 0, 0, 0.12);
    color: #000;
    font-size: 0.65rem;
    font-weight: 700;
    cursor: pointer;
    transition: background 0.15s;
    line-height: 1.6;
  }
  .fv-pill-btn:hover {
    background: rgba(0, 0, 0, 0.22);
  }

  .fv-hidden-toggle {
    font-size: 0.6rem;
    white-space: nowrap;
  }
  .fv-hidden-active {
    background: rgba(0,0,0,0.22);
  }

  .fv-storage-track {
    height: 5px;
    border-radius: 3px;
    background: #ddd;
    overflow: hidden;
    margin-bottom: 3px;
  }

  .fv-storage-fill {
    height: 100%;
    border-radius: 3px;
    background: var(--bar-bg, #FFE605);
    transition: width 0.3s;
  }

  .fv-storage-pct {
    display: block;
    font-size: 0.6rem;
    color: #999;
    text-align: right;
  }

  /* ── File / dir list ── */
  .fv-list {
    display: flex;
    flex-direction: column;
    gap: 5px;
    max-height: 240px;
    overflow-y: auto;
  }
  .fv-list::-webkit-scrollbar {
    width: 3px;
  }
  .fv-list::-webkit-scrollbar-thumb {
    background: #ddd;
    border-radius: 4px;
  }

  .fv-status {
    text-align: center;
    color: #999;
    font-size: 0.8rem;
    padding: 20px;
  }

  .fv-dir-item {
    display: flex;
    align-items: center;
    gap: 10px;
    padding: 9px 12px;
    border-radius: 10px;
    border: 2px solid transparent;
    background: #fafafa;
    cursor: pointer;
    transition: all 0.15s;
    text-align: left;
    width: 100%;
  }
  .fv-dir-item:hover {
    background: #f0f0f0;
  }

  .fv-dir-icon {
    font-size: 1rem;
    flex-shrink: 0;
  }

  .fv-dir-name {
    font-size: 0.8rem;
    font-weight: 600;
    color: #000;
  }

  .fv-file-item {
    display: flex;
    align-items: center;
    gap: 10px;
    padding: 9px 12px;
    border-radius: 10px;
    border: 2px solid transparent;
    background: #fafafa;
    cursor: pointer;
    transition: all 0.15s;
    text-align: left;
    width: 100%;
  }
  .fv-file-item:hover {
    background: #f0f0f0;
  }
  .fv-file-item:disabled {
    cursor: default;
    opacity: 0.5;
  }
  .fv-file-item:disabled:hover {
    background: #fafafa;
  }
  .fv-file-item.fv-file-selected {
    border-color: #FFE605;
    background: #fffef0;
  }

  .fv-dimmed {
    opacity: 0.45;
  }

  .fv-file-icon {
    font-size: 1.1rem;
    flex-shrink: 0;
  }

  .fv-file-meta {
    min-width: 0;
    flex: 1;
  }

  .fv-file-name {
    display: block;
    font-size: 0.8rem;
    font-weight: 600;
    color: #000;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }

  .fv-lock-indicator {
    font-size: 0.55rem;
    margin-left: 2px;
  }

  .fv-file-size {
    display: block;
    font-size: 0.65rem;
    color: #999;
    margin-top: 1px;
  }

  .fv-file-check {
    margin-left: auto;
    color: #FFE605;
    font-weight: 700;
    font-size: 1rem;
    flex-shrink: 0;
  }

  /* ── Name input ── */
  .name-input {
    width: 100%;
    padding: 12px 14px;
    border: 2px solid #e0e0e0;
    border-radius: 12px;
    font-size: 0.9rem;
    outline: none;
    transition: border-color 0.15s;
    box-sizing: border-box;
    font-family: inherit;
  }
  .name-input:focus {
    border-color: #FFE605;
  }

  .char-count {
    display: block;
    text-align: right;
    font-size: 0.65rem;
    color: #bbb;
    margin-top: 4px;
  }

  /* ── Emoji text input ── */
  .emoji-text-input {
    width: 100%;
    padding: 10px 14px;
    border: 2px solid #e0e0e0;
    border-radius: 12px;
    font-size: 1.2rem;
    outline: none;
    margin-bottom: 10px;
    box-sizing: border-box;
    font-family: inherit;
    text-align: center;
  }
  .emoji-text-input:focus {
    border-color: #FFE605;
  }

  /* ── Emoji picker ── */
  .emoji-grid {
    display: grid;
    grid-template-columns: repeat(8, 1fr);
    gap: 6px;
  }

  .emoji-item {
    width: 100%;
    aspect-ratio: 1;
    display: flex;
    align-items: center;
    justify-content: center;
    border: 2px solid transparent;
    border-radius: 10px;
    background: #f5f5f5;
    cursor: pointer;
    font-size: 1.2rem;
    transition: all 0.15s;
    padding: 0;
  }
  .emoji-item:hover {
    background: #eee;
  }
  .emoji-item.selected {
    border-color: #FFE605;
    background: #fff;
    transform: scale(1.1);
  }

  .emoji-preview {
    font-size: 0.8rem;
    color: #666;
    margin-top: 8px;
    display: flex;
    align-items: center;
    gap: 6px;
  }

  .emoji-preview-icon {
    font-size: 1.5rem;
  }

  /* ── Preview ── */
  .preview-section {
    margin-bottom: 0;
  }

  .preview-card {
    display: flex;
    align-items: center;
    gap: 14px;
    padding: 14px;
    border-radius: 14px;
    background: #fafafa;
    border: 2px solid #eee;
  }

  .preview-icon {
    width: 52px;
    height: 52px;
    border-radius: 14px;
    display: flex;
    align-items: center;
    justify-content: center;
    font-size: 1.6rem;
    flex-shrink: 0;
  }

  .preview-info {
    display: flex;
    flex-direction: column;
    gap: 2px;
  }

  .preview-name {
    font-size: 0.95rem;
    font-weight: 700;
    color: #000;
  }

  .preview-file {
    font-size: 0.7rem;
    color: #999;
  }

  /* ── Footer ── */
  .footer-bar {
    padding: 12px 14px;
    border-top: 1px solid #eee;
    flex-shrink: 0;
  }

  .save-btn {
    width: 100%;
    padding: 14px;
    border: none;
    border-radius: 14px;
    font-size: 1rem;
    font-weight: 700;
    color: #000;
    cursor: pointer;
    transition: opacity 0.15s;
  }
  .save-btn:disabled {
    opacity: 0.4;
    cursor: default;
  }
  .save-btn:not(:disabled):hover {
    opacity: 0.9;
  }
</style>
