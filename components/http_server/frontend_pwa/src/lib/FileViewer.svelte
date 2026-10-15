<script>
  import { colors } from "./colors.js";

  // `embedded` is set when this renders inside the Skills screen's Files
  // segment, which already supplies the yellow header and the title.
  let { onNavigate, embedded = false } = $props();

  const YELLOW = colors.mpx.primary;

  let files = $state([]);
  let dirs = $state([]);
  let currentDir = $state("/");
  let fsInfo = $state({ total: 0, used: 0 });
  let loading = $state(true);
  let deleting = $state(null);
  let sudoMode = $state(false);
  let showHidden = $state(false);
  let fileContent = $state(null);
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
    fileContent = null;
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

  async function viewFile(p) {
    try {
      const r = await fetch("/v1/fs/read?path=" + encodeURIComponent(p));
      if (r.ok) { const d = await r.json(); if (d.ok) fileContent = { path: d.p, content: d.c }; }
    } catch { fileContent = null; }
  }

  async function delFile(n) {
    if (!confirm("Delete " + n + "?")) return;
    deleting = n;
    try {
      const rel = currentDir === "/" ? "" : currentDir;
      const r = await fetch("/v1/fs/delete", {
        method: "POST", headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ path: rel + "/" + n, sudo: sudoMode }),
      });
      if (!r.ok) alert("Delete failed: " + (await r.text()));
      else await fetchDir(currentDir);
    } catch (e) { alert("Error: " + e.message); }
    deleting = null;
  }

  async function runWasm(n) {
    const p = (currentDir === "/" ? "" : currentDir) + "/" + n;
    try {
      const r = await fetch("/v1/skills/run", { method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ skill: p }) });
      if (r.ok) { const d = await r.json(); alert("⚡ " + n + "\n" + (d.output || "OK")); }
      else alert("❌ " + n + " (" + r.status + ")");
    } catch (e) { alert("❌ " + e.message); }
  }

  async function runLua(n) {
    const p = (currentDir === "/" ? "" : currentDir) + "/" + n;
    try {
      const r = await fetch("/v1/lua/run", { method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ path: p }) });
      const d = await r.json();
      if (d.ok) alert("🌙 " + n + "\n" + (d.output || "(no output)"));
      else alert("❌ " + n + ": " + (d.error || "failed"));
    } catch (e) { alert("❌ " + e.message); }
  }

  function goDir(d) { fileContent = null; fetchDir(currentDir === "/" ? "/" + d : currentDir + "/" + d); }
  function goUp() {
    fileContent = null;
    if (currentDir === "/") return;
    const p = currentDir.split("/").filter(Boolean); p.pop();
    fetchDir(p.length === 0 ? "/" : "/" + p.join("/"));
  }

  function goUpload() {
    if (onNavigate) onNavigate("upload");
  }

  $effect(() => { fetchDir("/"); });

  // ── Filtered files based on search ──
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
</script>

<div class="fv-root">
  <!-- ═══ Header ═══ -->
  <header class="fv-header" class:fv-header-embedded={embedded}
          style={embedded ? "" : `background: ${YELLOW}`}>
    <!-- When embedded under the Skills screen's own yellow header, the title
         row would be a second "Files" heading directly beneath the segmented
         control. The actions move into the search row instead. -->
    {#if !embedded}
      <div class="fv-header-row">
        <h2 class="fv-title">Files</h2>
        <div class="fv-header-actions">
          <button class="fv-pill-btn fv-hidden-toggle"
                  class:fv-hidden-active={showHidden}
                  onclick={() => { showHidden = !showHidden; }}>
            {showHidden ? '👁 Hidden' : '👁‍🗨'}
          </button>
          <button class="fv-pill-btn" onclick={() => fetchDir(currentDir)}>↻</button>
        </div>
      </div>
    {/if}

    <!-- Search bar -->
    <div class="fv-search-wrap">
      <svg class="fv-search-icon" width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
        <circle cx="11" cy="11" r="8"/>
        <line x1="21" y1="21" x2="16.65" y2="16.65"/>
      </svg>
      <input
        class="fv-search-input"
        type="text"
        placeholder="Search files…"
        bind:value={searchQuery}
      />
      {#if embedded}
        <button class="fv-pill-btn fv-hidden-toggle"
                class:fv-hidden-active={showHidden}
                onclick={() => { showHidden = !showHidden; }}>
          {showHidden ? '👁 Hidden' : '👁‍🗨'}
        </button>
        <button class="fv-pill-btn" onclick={() => fetchDir(currentDir)}>↻</button>
      {/if}
    </div>
  </header>

  <!-- ═══ Storage Bar ═══ -->
  <div class="fv-storage-bar" style="--bar-bg: {YELLOW}">
    <div class="fv-storage-row">
      <span class="fv-storage-path">📁 {currentDir === "/" ? "/" : currentDir}</span>
      <span class="fv-storage-pct">{fsInfo.total ? ((fsInfo.used / fsInfo.total) * 100).toFixed(0) : 0}%</span>
    </div>
    <div class="fv-storage-track">
      <div class="fv-storage-fill" style="width:{fsInfo.total ? ((fsInfo.used / fsInfo.total) * 100) : 0}%"></div>
    </div>
  </div>

  <!-- ═══ File List ═══ -->
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
          <div class="fv-file-item" class:fv-dimmed={!ok}>
            <div class="fv-file-info">
              <span class="fv-file-icon">{icon(f.n)}</span>
              <div class="fv-file-meta">
                <span class="fv-file-name">{f.n}{#if f.r}<span class="fv-lock-indicator">🔒</span>{/if}</span>
                <span class="fv-file-size">{fmtSize(f.s)}</span>
              </div>
            </div>
            <div class="fv-file-actions">
              {#if isLua(f.n)}
                <button class="fv-action-btn fv-view-btn" onclick={() => viewFile((currentDir === "/" ? "" : currentDir) + "/" + f.n)} title="View">📖</button>
                <button class="fv-action-btn fv-run-btn fv-run-lua" onclick={() => runLua(f.n)} title="Run">▶</button>
              {:else if isWasm(f.n)}
                <button class="fv-action-btn fv-run-btn fv-run-wasm" onclick={() => runWasm(f.n)} title="Run">⚡</button>
              {/if}
              {#if !f.r && ok}
                <button class="fv-action-btn fv-del-btn" onclick={() => delFile(f.n)} disabled={deleting === f.n} title="Delete">✕</button>
              {/if}
            </div>
          </div>
        {/if}
      {/each}
    {/if}
  </div>

  <!-- ═══ File Content Viewer ═══ -->
  {#if fileContent}
    <div class="fv-viewer-panel">
      <div class="fv-viewer-header">
        <span class="fv-viewer-path">{fileContent.path}</span>
        <button class="fv-viewer-close" onclick={() => { fileContent = null; }}>✕</button>
      </div>
      <pre class="fv-viewer-content">{fileContent.content || "(empty)"}</pre>
    </div>
  {/if}

  <!-- ═══ Add Button ═══ -->
  <button class="fv-add-btn" style="background: {YELLOW}" onclick={goUpload} aria-label="Upload file">
    <svg width="28" height="28" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
      <line x1="12" y1="5" x2="12" y2="19"/>
      <line x1="5" y1="12" x2="19" y2="12"/>
    </svg>
  </button>
</div>

<style>
  .fv-root {
    flex: 1;
    background: var(--surface-2);
    display: flex;
    flex-direction: column;
    overflow: hidden;
  }

  /* ═══ Header ═══ */
  .fv-header {
    flex-shrink: 0;
    padding: 10px 14px 8px;
  }

  .fv-header-row {
    display: flex;
    align-items: center;
    gap: 8px;
    margin-bottom: 8px;
  }

  .fv-title {
    font-size: 1.05rem;
    font-weight: 800;
    color: var(--text-1);
    margin: 0;
  }

  .fv-header-actions {
    display: flex;
    align-items: center;
    gap: 4px;
    margin-left: auto;
  }

  .fv-pill-btn {
    padding: 2px 8px;
    border: none;
    border-radius: 999px;
    background: var(--surface-3);
    color: var(--text-1);
    font-size: 0.7rem;
    font-weight: 700;
    cursor: pointer;
    transition: background 0.15s;
    line-height: 1.6;
  }
  .fv-pill-btn:hover {
    background: var(--surface-3);
  }

  .fv-hidden-toggle {
    font-size: 0.65rem;
    white-space: nowrap;
  }
  .fv-hidden-active {
    background: var(--surface-3);
  }

  /* ── Search ── */
  .fv-search-wrap {
    display: flex;
    align-items: center;
    gap: 6px;
    background: var(--surface-1);
    border-radius: 8px;
    padding: 6px 10px;
  }

  /* Embedded in the Skills screen: the yellow band and the title belong to
     the parent, so this header is just the search row on the page ground. */
  .fv-header-embedded {
    background: var(--surface-2);
    padding: 10px 14px 4px;
  }
  .fv-header-embedded .fv-search-wrap {
    background: var(--surface-1);
    box-shadow: 0 1px 3px rgba(0, 0, 0, 0.07);
  }
  .fv-header-embedded .fv-pill-btn {
    background: var(--surface-2);
    flex-shrink: 0;
  }
  .fv-search-icon {
    flex-shrink: 0;
    color: var(--text-2);
  }
  .fv-search-input {
    flex: 1;
    border: none;
    background: transparent;
    outline: none;
    font-size: 0.8rem;
    color: var(--text-1);
    font-family: inherit;
  }
  .fv-search-input::placeholder {
    color: var(--text-3);
  }

  /* ═══ Storage Bar ═══ */
  .fv-storage-bar {
    padding: 8px 14px;
    background: var(--surface-2);
    flex-shrink: 0;
  }

  .fv-storage-row {
    display: flex;
    justify-content: space-between;
    font-size: 0.7rem;
    color: var(--text-2);
    margin-bottom: 4px;
  }

  .fv-storage-path {
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }

  .fv-storage-pct {
    flex-shrink: 0;
    margin-left: 8px;
  }

  .fv-storage-track {
    height: 6px;
    border-radius: 4px;
    background: var(--surface-3);
    overflow: hidden;
  }

  .fv-storage-fill {
    height: 100%;
    border-radius: 4px;
    background: var(--bar-bg, var(--accent));
    transition: width 0.3s;
  }

  /* ═══ File List ═══ */
  .fv-list {
    flex: 1;
    overflow-y: auto;
    padding: 8px 14px;
    display: flex;
    flex-direction: column;
    gap: 6px;
  }
  .fv-list::-webkit-scrollbar {
    width: 4px;
  }
  .fv-list::-webkit-scrollbar-thumb {
    background: var(--surface-3);
    border-radius: 4px;
  }

  .fv-status {
    text-align: center;
    color: var(--text-3);
    font-size: 0.85rem;
    margin-top: 24px;
  }

  /* ── Directory Item ── */
  .fv-dir-item {
    display: flex;
    align-items: center;
    gap: 10px;
    padding: 10px 12px;
    background: var(--surface-1);
    border-radius: 12px;
    border: none;
    cursor: pointer;
    transition: background 0.15s;
    text-align: left;
    width: 100%;
  }
  .fv-dir-item:hover {
    background: var(--surface-2);
  }

  .fv-dir-icon {
    font-size: 1.1rem;
    flex-shrink: 0;
  }

  .fv-dir-name {
    font-size: 0.85rem;
    font-weight: 600;
    color: var(--text-1);
  }

  /* ── File Item ── */
  .fv-file-item {
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 10px 12px;
    background: var(--surface-1);
    border-radius: 12px;
    transition: background 0.15s;
  }
  .fv-file-item:hover {
    background: var(--surface-2);
  }
  .fv-dimmed {
    opacity: 0.45;
  }

  .fv-file-info {
    display: flex;
    align-items: center;
    gap: 10px;
    min-width: 0;
    flex: 1;
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
    font-size: 0.85rem;
    font-weight: 600;
    color: var(--text-1);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }

  .fv-lock-indicator {
    font-size: 0.6rem;
    margin-left: 2px;
  }

  .fv-file-size {
    display: block;
    font-size: 0.7rem;
    color: var(--text-3);
    margin-top: 1px;
  }

  .fv-file-actions {
    display: flex;
    align-items: center;
    gap: 4px;
    flex-shrink: 0;
    margin-left: 8px;
  }

  .fv-action-btn {
    width: 28px;
    height: 28px;
    display: flex;
    align-items: center;
    justify-content: center;
    border: none;
    background: var(--surface-2);
    border-radius: 8px;
    cursor: pointer;
    font-size: 0.75rem;
    transition: background 0.15s;
    color: var(--text-2);
  }
  .fv-action-btn:hover {
    background: var(--surface-3);
  }

  .fv-view-btn {
    background: #eef;
    color: #44a;
  }
  .fv-view-btn:hover {
    background: #dde;
  }

  .fv-run-btn {
    color: var(--solid-ink);
  }
  .fv-run-lua {
    background: #818cf8;
  }
  .fv-run-lua:hover {
    background: #6366f1;
  }
  .fv-run-wasm {
    background: var(--bar-bg, var(--accent));
    color: var(--text-1);
  }
  .fv-run-wasm:hover {
    filter: brightness(0.9);
  }

  .fv-del-btn {
    color: var(--danger);
    background: #fef2f2;
  }
  .fv-del-btn:hover {
    background: #fecaca;
  }
  .fv-del-btn:disabled {
    opacity: 0.5;
    cursor: default;
  }

  /* ═══ File Content Viewer ═══ */
  .fv-viewer-panel {
    border-top: 1px solid var(--line);
    background: #1a1a2e;
    flex-shrink: 0;
    max-height: 40%;
    display: flex;
    flex-direction: column;
  }

  .fv-viewer-header {
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 8px 14px;
    background: var(--surface-3);
  }

  .fv-viewer-path {
    font-size: 0.7rem;
    color: var(--text-3);
    font-family: monospace;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }

  .fv-viewer-close {
    width: 24px;
    height: 24px;
    display: flex;
    align-items: center;
    justify-content: center;
    border: none;
    background: transparent;
    color: var(--text-3);
    cursor: pointer;
    font-size: 0.8rem;
    border-radius: 4px;
    flex-shrink: 0;
  }
  .fv-viewer-close:hover {
    background: var(--surface-2);
    color: var(--solid-ink);
  }

  .fv-viewer-content {
    flex: 1;
    overflow-y: auto;
    padding: 10px 14px;
    font-size: 0.75rem;
    font-family: 'JetBrains Mono', 'Fira Code', monospace;
    color: var(--ok);
    white-space: pre-wrap;
    margin: 0;
    line-height: 1.5;
  }

  /* ═══ Add Button ═══ */
  .fv-add-btn {
    position: absolute;
    bottom: 76px;
    right: 20px;
    width: 52px;
    height: 52px;
    border-radius: 50%;
    border: none;
    display: flex;
    align-items: center;
    justify-content: center;
    cursor: pointer;
    color: var(--text-1);
    box-shadow: 0 3px 12px rgba(0,0,0,0.25);
    transition: transform 0.15s, box-shadow 0.15s;
    z-index: 10;
    pointer-events: auto;
  }
  .fv-add-btn:active {
    transform: scale(0.92);
  }
  .fv-add-btn:hover {
    box-shadow: 0 5px 16px rgba(0,0,0,0.3);
  }
</style>
