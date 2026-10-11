<script>
  let { onNavigate } = $props();

  let files = $state([]);
  let dirs = $state([]);
  let currentDir = $state("/");
  let fsInfo = $state({ total: 0, used: 0 });
  let loading = $state(true);
  let busy = $state(null);
  let sudoMode = $state(false);
  let showHidden = $state(false);
  let sheet = $state(null);           // { title, body } — file contents or run output
  let searchQuery = $state("");
  let toast = $state(null);
  let toastTimer = null;

  function say(text, tone = "ok") {
    toast = { text, tone };
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => (toast = null), 2600);
  }

  function isWasm(n) { return n.endsWith(".wasm") || n.endsWith(".mpxe"); }
  function isLua(n)  { return n.endsWith(".lua"); }
  function isAllowed(n) { return isWasm(n) || isLua(n); }

  function kind(n) {
    if (isWasm(n)) return "Motion skill";
    if (isLua(n)) return "Lua script";
    return "";
  }

  function fmtSize(b) {
    return b < 1024 ? b + " B" : (b / 1024).toFixed(1) + " KB";
  }

  const pathOf = (n) => (currentDir === "/" ? "" : currentDir) + "/" + n;

  async function fetchDir(dir) {
    loading = true;
    sheet = null;
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

  async function viewFile(n) {
    try {
      const r = await fetch("/v1/fs/read?path=" + encodeURIComponent(pathOf(n)));
      if (r.ok) { const d = await r.json(); if (d.ok) sheet = { title: d.p, body: d.c || "(empty)" }; }
      else say("Couldn't open " + n, "err");
    } catch { say("Couldn't reach the robot", "err"); }
  }

  async function delFile(n) {
    if (!confirm("Delete " + n + "?")) return;
    busy = n;
    try {
      const r = await fetch("/v1/fs/delete", {
        method: "POST", headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ path: pathOf(n), sudo: sudoMode }),
      });
      if (!r.ok) say("Delete failed: " + (await r.text()), "err");
      else { await fetchDir(currentDir); say(n + " deleted"); }
    } catch { say("Couldn't reach the robot", "err"); }
    busy = null;
  }

  async function runWasm(n) {
    busy = n;
    try {
      const r = await fetch("/v1/skills/run", { method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ skill: pathOf(n) }) });
      if (r.ok) { const d = await r.json(); sheet = { title: "Ran " + n, body: d.output || "OK" }; }
      else if (r.status === 409) say("Another skill is running. Stop it first.", "err");
      else say(n + " didn't run (" + r.status + ")", "err");
    } catch { say("Couldn't reach the robot", "err"); }
    busy = null;
  }

  async function runLua(n) {
    busy = n;
    try {
      const r = await fetch("/v1/lua/run", { method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ path: pathOf(n) }) });
      const d = await r.json();
      if (d.ok) sheet = { title: "Ran " + n, body: d.output || "(no output)" };
      else say(n + ": " + (d.error || "failed"), "err");
    } catch { say("Couldn't reach the robot", "err"); }
    busy = null;
  }

  function goDir(d) { sheet = null; fetchDir(currentDir === "/" ? "/" + d : currentDir + "/" + d); }
  function goUp() {
    sheet = null;
    if (currentDir === "/") return;
    const p = currentDir.split("/").filter(Boolean); p.pop();
    fetchDir(p.length === 0 ? "/" : "/" + p.join("/"));
  }
  function back() {
    if (currentDir !== "/") goUp();
    else onNavigate?.("home");
  }

  $effect(() => { fetchDir("/"); return () => clearTimeout(toastTimer); });

  const q = $derived(searchQuery.trim().toLowerCase());
  let filteredDirs = $derived(q ? dirs.filter((d) => d.toLowerCase().includes(q)) : dirs);
  let filteredFiles = $derived(
    (q ? files.filter((f) => f.n.toLowerCase().includes(q)) : files).filter((f) => showHidden || isAllowed(f.n))
  );
  const usedPct = $derived(fsInfo.total ? Math.round((fsInfo.used / fsInfo.total) * 100) : 0);
  const isEmpty = $derived(currentDir === "/" && filteredDirs.length === 0 && filteredFiles.length === 0);
</script>

{#snippet chev()}
  <svg class="chev" width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M9 6l6 6-6 6"/></svg>
{/snippet}
{#snippet folderIcon()}
  <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"/></svg>
{/snippet}

<div class="page">
  <header class="bar">
    <button type="button" class="round" onclick={back} aria-label={currentDir === "/" ? "Back to settings" : "Up one folder"}>
      <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M15 6l-6 6 6 6"/></svg>
    </button>
    <h1>Files</h1>
    <div class="bar-acts">
      <button type="button" class="round" class:on={showHidden} onclick={() => (showHidden = !showHidden)} aria-pressed={showHidden} aria-label="Show all files" title="Show all files">
        {#if showHidden}
          <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M2 12s3.6-7 10-7 10 7 10 7-3.6 7-10 7S2 12 2 12z"/><circle cx="12" cy="12" r="3"/></svg>
        {:else}
          <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M10.6 5.1A10.4 10.4 0 0 1 12 5c6.4 0 10 7 10 7a17.6 17.6 0 0 1-3.2 4.1M6.6 6.6C3.8 8.4 2 12 2 12s3.6 7 10 7a9.7 9.7 0 0 0 5.4-1.6"/><path d="M3 3l18 18M9.9 9.9a3 3 0 0 0 4.2 4.2"/></svg>
        {/if}
      </button>
      <button type="button" class="round" onclick={() => fetchDir(currentDir)} aria-label="Refresh">
        <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M20 11a8 8 0 1 0-2.3 5.7"/><path d="M20 4v7h-7"/></svg>
      </button>
    </div>
  </header>

  <div class="scroll">
    <div class="inner">
      <label class="search">
        <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" aria-hidden="true"><circle cx="11" cy="11" r="7"/><path d="M20 20l-4-4"/></svg>
        <span class="sr">Search files</span>
        <input type="search" bind:value={searchQuery} placeholder="Search files" />
      </label>

      <div class="storage">
        <div class="storage-row">
          <span class="path">{currentDir}</span>
          <span class="pct">{usedPct}% used</span>
        </div>
        <div class="track"><div style="width: {usedPct}%"></div></div>
      </div>

      {#if loading}
        <div class="group" aria-hidden="true">
          {#each [0, 1, 2] as _}<div class="row sk"><span class="ico"></span><span class="sk-line"></span></div>{/each}
        </div>
      {:else if isEmpty}
        <div class="empty">
          <p class="empty-title">{q ? "No matching files" : "No skill files yet"}</p>
          <p class="empty-desc">{q ? `Nothing here is called “${searchQuery}”.` : "Upload a motion skill, or let the agent write a Lua script."}</p>
        </div>
      {:else}
        <ul class="group">
          {#if currentDir !== "/"}
            <li>
              <button type="button" class="row" onclick={goUp}>
                <span class="ico">
                  <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M9 14l-4-4 4-4"/><path d="M5 10h10a4 4 0 0 1 4 4v4"/></svg>
                </span>
                <span class="txt"><span class="t">Up one folder</span></span>
              </button>
            </li>
          {/if}

          {#each filteredDirs as d (d)}
            <li>
              <button type="button" class="row" onclick={() => goDir(d)}>
                <span class="ico">{@render folderIcon()}</span>
                <span class="txt"><span class="t">{d}</span></span>
                {@render chev()}
              </button>
            </li>
          {/each}

          {#each filteredFiles as f (f.n)}
            {@const ok = isAllowed(f.n)}
            <li class="row file" class:dim={!ok} class:busy={busy === f.n}>
              <span class="ico" class:motion={isWasm(f.n)} class:lua={isLua(f.n)} aria-hidden="true">
                {#if isWasm(f.n)}
                  <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M4 18c3-8 5-12 8-12s5 4 8 12"/><circle cx="12" cy="6" r="2"/></svg>
                {:else if isLua(f.n)}
                  <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><path d="M8 8l-4 4 4 4M16 8l4 4-4 4"/></svg>
                {:else}
                  <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><path d="M6 3h8l4 4v14H6z"/><path d="M14 3v4h4"/></svg>
                {/if}
              </span>
              <span class="txt">
                <span class="t">
                  {f.n}
                  {#if f.r}
                    <svg class="lock" width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round" aria-label="Read-only"><rect x="5" y="11" width="14" height="10" rx="2"/><path d="M8 11V7a4 4 0 0 1 8 0v4"/></svg>
                  {/if}
                </span>
                <span class="d">{fmtSize(f.s)}{kind(f.n) ? ` · ${kind(f.n)}` : ""}</span>
              </span>
              {#if ok}
                <span class="acts">
                  <button type="button" class="btn" disabled={busy === f.n} onclick={() => (isLua(f.n) ? runLua(f.n) : runWasm(f.n))} aria-label="Run {f.n}">Run</button>
                  <span class="links">
                    {#if isLua(f.n)}
                      <button type="button" class="link" onclick={() => viewFile(f.n)} aria-label="View {f.n}">View</button>
                    {/if}
                    {#if !f.r}
                      <button type="button" class="link del" disabled={busy === f.n} onclick={() => delFile(f.n)} aria-label="Delete {f.n}">Delete</button>
                    {/if}
                  </span>
                </span>
              {/if}
            </li>
          {/each}
        </ul>
      {/if}

      <button type="button" class="primary" onclick={() => onNavigate?.("upload")}>
        <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M12 16V4M6 10l6-6 6 6"/><path d="M4 20h16"/></svg>
        <span>Upload a skill file</span>
      </button>
    </div>
  </div>

  {#if sheet}
    <div class="sheet" role="dialog" aria-label={sheet.title}>
      <div class="sheet-head">
        <span class="sheet-title">{sheet.title}</span>
        <button type="button" class="round sm" onclick={() => (sheet = null)} aria-label="Close">
          <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" aria-hidden="true"><path d="M6 6l12 12M18 6L6 18"/></svg>
        </button>
      </div>
      <pre class="sheet-body">{sheet.body}</pre>
    </div>
  {/if}

  {#if toast}
    <div class="toast" class:err={toast.tone === "err"} role="status">{toast.text}</div>
  {/if}
</div>

<style>
  .page { position: absolute; inset: 0; display: flex; flex-direction: column; background: var(--page); color: var(--ink); }
  .bar {
    flex-shrink: 0; height: calc(64px + var(--safe-top)); padding: calc(10px + var(--safe-top)) 16px 0;
    display: grid; grid-template-columns: 1fr auto 1fr; align-items: center; gap: 8px;
    max-width: var(--page-max); width: 100%; margin: 0 auto;
  }
  .bar h1 { font-size: 18px; font-weight: 800; }
  .bar-acts { justify-self: end; display: flex; gap: 8px; }
  .round {
    width: 44px; height: 44px; border-radius: 22px; border: none; background: var(--ground); color: var(--ink);
    display: grid; place-items: center; cursor: pointer; transition: background var(--dur-fast);
  }
  .round.on { background: var(--mpx-yellow); }
  .round.sm { width: 36px; height: 36px; }

  .scroll { flex: 1; min-height: 0; overflow-y: auto; }
  .inner { max-width: var(--page-max); margin: 0 auto; padding: 8px 16px calc(24px + var(--safe-bottom)); display: flex; flex-direction: column; gap: 10px; }

  .search { display: flex; align-items: center; gap: 8px; height: 46px; padding: 0 14px; border-radius: 23px; background: var(--ground); color: var(--sub); }
  .search input { flex: 1; min-width: 0; border: none; background: transparent; font-size: 16px; color: var(--ink); outline: none; }
  .search:focus-within { outline: 3px solid var(--ink); outline-offset: 2px; }

  .storage { background: var(--ground); border-radius: 14px; padding: 10px 12px; display: flex; flex-direction: column; gap: 8px; }
  .storage-row { display: flex; justify-content: space-between; gap: 8px; font-size: 13px; }
  .path { font-family: var(--mono); color: var(--ink); overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  .pct { flex-shrink: 0; font-weight: 700; color: var(--sub); }
  .track { height: 6px; border-radius: 3px; background: var(--hair); overflow: hidden; }
  .track > div { height: 100%; border-radius: 3px; background: var(--ink); transition: width var(--dur-med); }

  .group { list-style: none; background: #fff; border: 1px solid var(--line); border-radius: 20px; overflow: hidden; }
  .group > * + * { border-top: 1px solid var(--hair-2); }
  .row {
    width: 100%; border: none; background: transparent; color: var(--ink);
    display: flex; align-items: center; gap: 14px; padding: 12px 16px; text-align: left; cursor: pointer;
  }
  .row.file { cursor: default; transition: opacity var(--dur-fast); }
  .row.dim { opacity: 0.5; }
  .row.busy { opacity: 0.6; }
  .ico { width: 38px; height: 38px; border-radius: 11px; background: var(--ground); color: var(--ink); display: grid; place-items: center; flex-shrink: 0; }
  .ico.lua { background: var(--mpx-yellow); }
  .ico.motion { background: #CFE3FF; }
  .txt { flex: 1; min-width: 0; display: flex; flex-direction: column; gap: 2px; }
  .t { font-size: 16px; font-weight: 700; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  .lock { color: var(--sub); margin-left: 4px; vertical-align: -1px; }
  .d { font-size: 13px; color: var(--sub); }
  .chev { color: #A39F96; flex-shrink: 0; }

  .acts { display: flex; flex-direction: column; align-items: flex-end; gap: 2px; flex-shrink: 0; }
  .links { display: flex; }
  .btn { height: 36px; min-width: 64px; padding: 0 14px; border-radius: 18px; border: none; background: var(--ink); color: var(--mpx-yellow); font-size: 14px; font-weight: 800; cursor: pointer; }
  .link { height: 28px; border: none; background: transparent; color: var(--ink); font-size: 13px; font-weight: 700; cursor: pointer; padding: 0 6px; }
  .link.del { color: #9B2C20; }
  .btn:disabled, .link:disabled { opacity: 0.6; cursor: default; }
  button:focus-visible { outline: 3px solid var(--ink); outline-offset: 2px; }
  .row:focus-visible { outline-offset: -3px; }

  .sk { height: 63px; }
  .sk .ico, .sk-line { background: var(--hair); }
  .sk-line { height: 14px; width: 50%; border-radius: 7px; }

  .empty { background: var(--ground); border-radius: 20px; padding: 28px 20px; display: flex; flex-direction: column; align-items: center; gap: 8px; text-align: center; }
  .empty-title { font-size: 17px; font-weight: 800; }
  .empty-desc { font-size: 14px; line-height: 1.45; color: var(--sub); max-width: 300px; }

  .primary {
    margin-top: 6px; height: 56px; border-radius: 28px; border: none; background: var(--ink); color: #fff;
    display: flex; align-items: center; justify-content: center; gap: 10px;
    font-size: 16px; font-weight: 700; cursor: pointer; transition: transform var(--dur-fast);
  }
  .primary svg { color: var(--mpx-yellow); }
  .primary:active { transform: scale(0.98); }

  .sheet {
    position: absolute; left: 0; right: 0; bottom: 0; z-index: 30;
    max-width: var(--page-max); margin: 0 auto; max-height: 55%;
    background: #fff; border-radius: 28px 28px 0 0; box-shadow: var(--shadow-float);
    padding: 14px 16px calc(16px + var(--safe-bottom)); display: flex; flex-direction: column; gap: 10px;
    animation: up var(--dur-med) var(--ease-out);
  }
  @keyframes up { from { transform: translateY(24px); opacity: 0; } to { transform: none; opacity: 1; } }
  .sheet-head { display: flex; align-items: center; justify-content: space-between; gap: 12px; }
  .sheet-title { font-family: var(--mono); font-size: 13px; font-weight: 600; color: var(--sub); overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  .sheet-body {
    flex: 1; min-height: 0; overflow: auto; margin: 0; padding: 12px 14px; border-radius: 14px;
    background: var(--ground); font-family: var(--mono); font-size: 13px; line-height: 1.5; white-space: pre-wrap; color: var(--ink);
  }

  .toast {
    position: absolute; left: 50%; bottom: 16px; transform: translateX(-50%); z-index: 40;
    background: var(--ink); color: #fff; padding: 10px 16px; border-radius: 14px;
    font-size: 14px; font-weight: 600; box-shadow: var(--shadow-float); max-width: calc(100% - 40px);
  }
  .toast.err { background: #9B2C20; }
  .sr { position: absolute; width: 1px; height: 1px; overflow: hidden; clip: rect(0 0 0 0); white-space: nowrap; }
</style>
