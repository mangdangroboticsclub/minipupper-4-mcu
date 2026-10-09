<script>
  import { colors } from "./colors.js";

  let { onNavigate } = $props();

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
  <header class="fv-header" style="background: {YELLOW}">
    <div class="fv-header-row">
      <button class="fv-back-btn" onclick={() => { if (currentDir !== "/") { goUp(); } else if (onNavigate) onNavigate("home"); }} aria-label="Go back">
        <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
          <line x1="19" y1="12" x2="5" y2="12"/>
          <polyline points="12 19 5 12 12 5"/>
        </svg>
      </button>
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
    background: #f5f5f5;
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

  .fv-back-btn {
    width: 30px;
    height: 30px;
    display: flex;
    align-items: center;
    justify-content: center;
    border: none;
    background: rgba(0,0,0,0.12);
    border-radius: 50%;
    cursor: pointer;
    color: #000;
    flex-shrink: 0;
  }
  .fv-back-btn:hover {
    background: rgba(0,0,0,0.2);
  }

  .fv-title {
    font-size: 1.05rem;
    font-weight: 800;
    color: #000;
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
    background: rgba(0, 0, 0, 0.15);
    color: #000;
    font-size: 0.7rem;
    font-weight: 700;
    cursor: pointer;
    transition: background 0.15s;
    line-height: 1.6;
  }
  .fv-pill-btn:hover {
    background: rgba(0, 0, 0, 0.25);
  }

  .fv-hidden-toggle {
    font-size: 0.65rem;
    white-space: nowrap;
  }
  .fv-hidden-active {
    background: rgba(0,0,0,0.25);
  }

  /* ── Search ── */
  .fv-search-wrap {
    display: flex;
    align-items: center;
    gap: 6px;
    background: rgba(255,255,255,0.5);
    border-radius: 8px;
    padding: 6px 10px;
  }
  .fv-search-icon {
    flex-shrink: 0;
    color: #666;
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
    color: #888;
  }

  /* ═══ Storage Bar ═══ */
  .fv-storage-bar {
    padding: 8px 14px;
    background: #eee;
    flex-shrink: 0;
  }

  .fv-storage-row {
    display: flex;
    justify-content: space-between;
    font-size: 0.7rem;
    color: #666;
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
    background: #d9d9d9;
    overflow: hidden;
  }

  .fv-storage-fill {
    height: 100%;
    border-radius: 4px;
    background: var(--bar-bg, #FFE605);
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
    background: #d9d9d9;
    border-radius: 4px;
  }

  .fv-status {
    text-align: center;
    color: #999;
    font-size: 0.85rem;
    margin-top: 24px;
  }

  /* ── Directory Item ── */
  .fv-dir-item {
    display: flex;
    align-items: center;
    gap: 10px;
    padding: 10px 12px;
    background: #fff;
    border-radius: 12px;
    border: none;
    cursor: pointer;
    transition: background 0.15s;
    text-align: left;
    width: 100%;
  }
  .fv-dir-item:hover {
    background: #f0f0f0;
  }

  .fv-dir-icon {
    font-size: 1.1rem;
    flex-shrink: 0;
  }

  .fv-dir-name {
    font-size: 0.85rem;
    font-weight: 600;
    color: #000;
  }

  /* ── File Item ── */
  .fv-file-item {
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 10px 12px;
    background: #fff;
    border-radius: 12px;
    transition: background 0.15s;
  }
  .fv-file-item:hover {
    background: #f0f0f0;
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
    color: #000;
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
    color: #999;
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
    background: #f0f0f0;
    border-radius: 8px;
    cursor: pointer;
    font-size: 0.75rem;
    transition: background 0.15s;
    color: #666;
  }
  .fv-action-btn:hover {
    background: #e0e0e0;
  }

  .fv-view-btn {
    background: #eef;
    color: #44a;
  }
  .fv-view-btn:hover {
    background: #dde;
  }

  .fv-run-btn {
    color: #fff;
  }
  .fv-run-lua {
    background: #818cf8;
  }
  .fv-run-lua:hover {
    background: #6366f1;
  }
  .fv-run-wasm {
    background: var(--bar-bg, #FFE605);
    color: #000;
  }
  .fv-run-wasm:hover {
    filter: brightness(0.9);
  }

  .fv-del-btn {
    color: #ef4444;
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
    border-top: 1px solid #e0e0e0;
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
    background: rgba(0,0,0,0.3);
  }

  .fv-viewer-path {
    font-size: 0.7rem;
    color: #aaa;
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
    color: #aaa;
    cursor: pointer;
    font-size: 0.8rem;
    border-radius: 4px;
    flex-shrink: 0;
  }
  .fv-viewer-close:hover {
    background: rgba(255,255,255,0.1);
    color: #fff;
  }

  .fv-viewer-content {
    flex: 1;
    overflow-y: auto;
    padding: 10px 14px;
    font-size: 0.75rem;
    font-family: 'JetBrains Mono', 'Fira Code', monospace;
    color: #4ade80;
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
    color: #000;
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
