<script>
  let { onSave, onBack } = $props();

  // ── File-system browser state (embedded FileViewer) ──
  let files = $state([]);
  let dirs = $state([]);
  let currentDir = $state("/");
  let loading = $state(true);
  let showHidden = $state(false);
  let searchQuery = $state("");

  function isWasm(n) { return n.endsWith(".wasm") || n.endsWith(".mpxe"); }
  function isLua(n)  { return n.endsWith(".lua"); }
  function isAllowed(n) { return isWasm(n) || isLua(n); }

  function fmtSize(b) {
    return b < 1024 ? b + " B" : (b / 1024).toFixed(1) + " KB";
  }

  async function fetchDir(dir) {
    loading = true;
    try {
      const lr = await fetch("/v1/fs/list?path=" + encodeURIComponent(dir));
      if (lr.ok) { const d = await lr.json(); files = d.f || []; dirs = d.d || []; currentDir = dir; }
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
  const q = $derived(searchQuery.trim().toLowerCase());
  let filteredDirs = $derived(q ? dirs.filter((d) => d.toLowerCase().includes(q)) : dirs);
  let filteredFiles = $derived(
    (q ? files.filter((f) => f.n.toLowerCase().includes(q)) : files).filter((f) => showHidden || isAllowed(f.n))
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

  const pathOf = (n) => (currentDir === "/" ? "" : currentDir) + "/" + n;
  const isSelected = (f) => selectedFile && selectedFile.path === pathOf(f.n);

  function selectFile(f) {
    let fileType = "lua";
    if (isWasm(f.n)) fileType = f.n.endsWith(".mpxe") ? "mpxe" : "wasm";
    selectedFile = {
      path: pathOf(f.n),
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

  const formValid = $derived(!!selectedFile && actionName.trim().length > 0);
</script>

<div class="page">
  <header class="bar">
    <button type="button" class="round" onclick={onBack} aria-label="Back">
      <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M15 6l-6 6 6 6"/></svg>
    </button>
    <h1>Add a move</h1>
    <span class="spacer"></span>
  </header>

  <div class="scroll">
    <div class="inner">
      <!-- ═══ Step 1: Browse & pick a file ═══ -->
      <section>
        <h2>1 · Pick a file</h2>
        <p class="hint">A motion skill (.wasm, .mpxe) or a Lua script on the robot.</p>

        <label class="search">
          <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" aria-hidden="true"><circle cx="11" cy="11" r="7"/><path d="M20 20l-4-4"/></svg>
          <span class="sr">Search files</span>
          <input type="search" bind:value={searchQuery} placeholder="Search files" />
        </label>

        <div class="files">
          <div class="files-head">
            <span class="path">{currentDir}</span>
            <button type="button" class="round sm" class:on={showHidden} onclick={() => (showHidden = !showHidden)} aria-pressed={showHidden} aria-label="Show all files" title="Show all files">
              <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M2 12s3.6-7 10-7 10 7 10 7-3.6 7-10 7S2 12 2 12z"/><circle cx="12" cy="12" r="3"/></svg>
            </button>
            <button type="button" class="round sm" onclick={() => fetchDir(currentDir)} aria-label="Refresh">
              <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M20 11a8 8 0 1 0-2.3 5.7"/><path d="M20 4v7h-7"/></svg>
            </button>
          </div>

          <ul class="list">
            {#if loading}
              <li class="status">Loading…</li>
            {:else}
              {#if currentDir !== "/"}
                <li>
                  <button type="button" class="row" onclick={goUp}>
                    <span class="ico"><svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M9 14l-4-4 4-4"/><path d="M5 10h10a4 4 0 0 1 4 4v4"/></svg></span>
                    <span class="t">Up one folder</span>
                  </button>
                </li>
              {/if}

              {#each filteredDirs as d (d)}
                <li>
                  <button type="button" class="row" onclick={() => goDir(d)}>
                    <span class="ico"><svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"/></svg></span>
                    <span class="t">{d}</span>
                    <svg class="chev" width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M9 6l6 6-6 6"/></svg>
                  </button>
                </li>
              {/each}

              {#each filteredFiles as f (f.n)}
                {@const ok = isAllowed(f.n)}
                {@const sel = isSelected(f)}
                <li>
                  <button type="button" class="row" class:sel disabled={!ok} aria-pressed={sel} onclick={() => selectFile(f)}>
                    <span class="ico" class:motion={isWasm(f.n)} class:lua={isLua(f.n)} aria-hidden="true">
                      {#if isWasm(f.n)}
                        <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M4 18c3-8 5-12 8-12s5 4 8 12"/><circle cx="12" cy="6" r="2"/></svg>
                      {:else if isLua(f.n)}
                        <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><path d="M8 8l-4 4 4 4M16 8l4 4-4 4"/></svg>
                      {:else}
                        <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><path d="M6 3h8l4 4v14H6z"/><path d="M14 3v4h4"/></svg>
                      {/if}
                    </span>
                    <span class="txt">
                      <span class="t">{f.n}</span>
                      <span class="d">{fmtSize(f.s)}</span>
                    </span>
                    {#if sel}
                      <span class="check" aria-hidden="true"><svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="3" stroke-linecap="round" stroke-linejoin="round"><path d="M5 13l4 4L19 7"/></svg></span>
                    {/if}
                  </button>
                </li>
              {/each}

              {#if !filteredDirs.length && !filteredFiles.length && currentDir === "/"}
                <li class="status">{q ? `No file called “${searchQuery}”.` : "No skill files on the robot yet."}</li>
              {/if}
            {/if}
          </ul>
        </div>
      </section>

      <!-- ═══ Step 2: Name ═══ -->
      <section>
        <h2>2 · Name it</h2>
        <label class="field">
          <span class="sr">Move name</span>
          <input type="text" placeholder="e.g. My cool move" bind:value={actionName} maxlength="24" />
          <span class="count">{actionName.length}/24</span>
        </label>
      </section>

      <!-- ═══ Step 3: Pick an emoji ═══ -->
      <section>
        <h2>3 · Pick an icon</h2>
        <p class="hint">Type any emoji, or pick one below.</p>
        <label class="field">
          <span class="sr">Custom emoji</span>
          <input type="text" class="emoji-in" placeholder="Type an emoji" bind:value={customEmojiInput} maxlength="8" />
        </label>
        <div class="emoji-grid">
          {#each emojiSuggestions as emoji}
            <button
              type="button"
              class="emoji"
              class:on={actionEmoji === emoji && !customEmojiInput}
              aria-pressed={actionEmoji === emoji && !customEmojiInput}
              onclick={() => { actionEmoji = emoji; customEmojiInput = ""; }}
            >{emoji}</button>
          {/each}
        </div>
      </section>

      <!-- ═══ Preview ═══ -->
      {#if selectedFile}
        <section>
          <h2>Preview</h2>
          <div class="card">
            <span class="preview-ico" aria-hidden="true">{effectiveEmoji}</span>
            <span class="txt">
              <span class="name">{actionName || "Move name"}</span>
              <span class="d">{selectedFile.name}</span>
            </span>
          </div>
        </section>
      {/if}
    </div>
  </div>

  <div class="foot">
    <button type="button" class="primary" disabled={!formValid} onclick={handleSave}>Save move</button>
  </div>
</div>

<style>
  .page { position: absolute; inset: 0; z-index: 50; display: flex; flex-direction: column; background: var(--page); color: var(--ink); }
  .bar {
    flex-shrink: 0; height: calc(64px + var(--safe-top)); padding: calc(10px + var(--safe-top)) 16px 0;
    display: flex; align-items: center; justify-content: space-between;
    max-width: var(--page-max); width: 100%; margin: 0 auto;
  }
  .bar h1 { font-size: 18px; font-weight: 800; }
  .spacer { width: 44px; }
  .round {
    width: 44px; height: 44px; border-radius: 22px; border: none; background: var(--ground); color: var(--ink);
    display: grid; place-items: center; cursor: pointer; flex-shrink: 0; transition: background var(--dur-fast);
  }
  .round.sm { width: 34px; height: 34px; background: #fff; }
  .round.on { background: var(--mpx-yellow); }

  .scroll { flex: 1; min-height: 0; overflow-y: auto; }
  .inner { max-width: var(--page-max); margin: 0 auto; padding: 8px 16px 20px; display: flex; flex-direction: column; gap: 22px; }
  section { display: flex; flex-direction: column; gap: 8px; }
  h2 { margin: 0 4px; font-size: 12px; font-weight: 800; letter-spacing: 0.12em; text-transform: uppercase; color: var(--sub); }
  .hint { font-size: 13px; color: var(--sub); padding: 0 4px; margin-top: -2px; }

  .search { display: flex; align-items: center; gap: 8px; height: 46px; padding: 0 14px; border-radius: 23px; background: var(--ground); color: var(--sub); }
  .search input { flex: 1; min-width: 0; border: none; background: transparent; font-size: 16px; color: var(--ink); outline: none; }
  .search:focus-within, .field:focus-within { outline: 3px solid var(--ink); outline-offset: 2px; }

  .files { background: #fff; border: 1px solid var(--line); border-radius: 20px; overflow: hidden; }
  .files-head { display: flex; align-items: center; gap: 6px; padding: 8px 8px 8px 16px; background: var(--ground); }
  .path { flex: 1; min-width: 0; font-family: var(--mono); font-size: 13px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  .list { list-style: none; max-height: 300px; overflow-y: auto; }
  .list > * + * { border-top: 1px solid var(--hair-2); }
  .status { padding: 20px 16px; text-align: center; font-size: 14px; color: var(--sub); }
  .row {
    width: 100%; border: none; background: transparent; color: var(--ink);
    display: flex; align-items: center; gap: 12px; padding: 10px 16px; text-align: left; cursor: pointer;
    transition: background var(--dur-fast);
  }
  .row:disabled { opacity: 0.45; cursor: default; }
  .row.sel { background: #FFF6B0; }
  .row:focus-visible { outline: 3px solid var(--ink); outline-offset: -3px; }
  .ico { width: 34px; height: 34px; border-radius: 10px; background: var(--ground); display: grid; place-items: center; flex-shrink: 0; }
  .ico.lua { background: var(--mpx-yellow); }
  .ico.motion { background: #CFE3FF; }
  .txt { flex: 1; min-width: 0; display: flex; flex-direction: column; gap: 1px; }
  .t { flex: 1; min-width: 0; font-size: 15px; font-weight: 700; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  .d { font-size: 13px; color: var(--sub); overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  .chev { color: #A39F96; flex-shrink: 0; }
  .check { width: 26px; height: 26px; border-radius: 13px; background: var(--ink); color: var(--mpx-yellow); display: grid; place-items: center; flex-shrink: 0; }

  .field { display: flex; align-items: center; gap: 8px; height: 50px; padding: 0 16px; border-radius: 16px; background: var(--ground); }
  .field input { flex: 1; min-width: 0; border: none; background: transparent; font-size: 16px; color: var(--ink); outline: none; }
  .field input::placeholder, .search input::placeholder { color: var(--sub); }
  .emoji-in { text-align: center; font-size: 20px !important; }
  .count { font-size: 12px; font-weight: 700; color: var(--sub); font-variant-numeric: tabular-nums; }

  .emoji-grid { display: grid; grid-template-columns: repeat(8, minmax(0, 1fr)); gap: 6px; }
  .emoji {
    aspect-ratio: 1; border: none; border-radius: 12px; background: var(--ground); font-size: 20px; padding: 0;
    display: grid; place-items: center; cursor: pointer; transition: background var(--dur-fast), transform var(--dur-fast);
  }
  .emoji.on { background: var(--mpx-yellow); transform: scale(1.06); }

  .card { background: #fff; border: 1px solid var(--line); border-radius: 20px; padding: 14px; display: flex; align-items: center; gap: 14px; }
  .preview-ico { width: 52px; height: 52px; border-radius: 15px; background: var(--mpx-yellow); display: grid; place-items: center; font-size: 26px; flex-shrink: 0; }
  .name { font-size: 16px; font-weight: 700; }

  .foot { flex-shrink: 0; width: 100%; max-width: var(--page-max); margin: 0 auto; padding: 8px 16px calc(16px + var(--safe-bottom)); }
  .primary {
    width: 100%; height: 56px; border-radius: 28px; border: none; background: var(--ink); color: #fff;
    font-size: 16px; font-weight: 700; cursor: pointer; transition: transform var(--dur-fast), opacity var(--dur-fast);
  }
  .primary:active:not(:disabled) { transform: scale(0.98); }
  .primary:disabled { opacity: 0.35; cursor: default; }
  button:focus-visible { outline: 3px solid var(--ink); outline-offset: 2px; }
  .sr { position: absolute; width: 1px; height: 1px; overflow: hidden; clip: rect(0 0 0 0); white-space: nowrap; }
</style>
