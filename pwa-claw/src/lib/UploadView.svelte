<script>
  let { onBack } = $props();

  let dragging = $state(false);
  let uploading = $state(false);
  let status = $state(null);          // { tone: "ok" | "err", text }
  let selectedFile = $state(null);
  let inputEl = $state(null);

  function onDragOver(e) {
    e.preventDefault();
    dragging = true;
  }

  function onDragLeave() {
    dragging = false;
  }

  function onDrop(e) {
    e.preventDefault();
    dragging = false;
    if (e.dataTransfer.files.length > 0) {
      selectedFile = e.dataTransfer.files[0];
      status = null;
    }
  }

  function onFileSelect(e) {
    if (e.target.files.length > 0) {
      selectedFile = e.target.files[0];
      status = null;
    }
  }

  async function upload() {
    if (!selectedFile) return;
    uploading = true;
    status = null;

    try {
      const name = encodeURIComponent(selectedFile.name);
      const res = await fetch(`/v1/skills/upload?name=${name}`, {
        method: "POST",
        body: await selectedFile.arrayBuffer(),
      });

      if (res.ok) {
        status = { tone: "ok", text: `${selectedFile.name} is on the robot` };
        selectedFile = null;
      } else {
        const err = await res.text();
        status = { tone: "err", text: `Upload failed: ${err}` };
      }
    } catch (e) {
      status = { tone: "err", text: `Couldn't reach the robot: ${e.message}` };
    }
    uploading = false;
  }

  function formatSize(bytes) {
    if (bytes < 1024) return `${bytes} B`;
    return `${(bytes / 1024).toFixed(1)} KB`;
  }
</script>

<div class="page">
  <header class="bar">
    <button type="button" class="round" onclick={onBack} aria-label="Back to files">
      <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M15 6l-6 6 6 6"/></svg>
    </button>
    <h1>Upload skill</h1>
    <span class="spacer"></span>
  </header>

  <div class="scroll">
    <div class="inner">
      <button
        type="button"
        class="drop"
        class:dragging
        class:picked={!!selectedFile}
        ondragover={onDragOver}
        ondragleave={onDragLeave}
        ondrop={onDrop}
        onclick={() => inputEl?.click()}
      >
        <span class="ico" aria-hidden="true">
          {#if selectedFile}
            <svg width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M4 18c3-8 5-12 8-12s5 4 8 12"/><circle cx="12" cy="6" r="2"/></svg>
          {:else}
            <svg width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><path d="M12 16V4M6 10l6-6 6 6"/><path d="M4 20h16"/></svg>
          {/if}
        </span>
        {#if selectedFile}
          <span class="drop-title">{selectedFile.name}</span>
          <span class="drop-desc">{formatSize(selectedFile.size)} · tap to pick another</span>
        {:else}
          <span class="drop-title">{dragging ? "Drop it here" : "Pick a .wasm file"}</span>
          <span class="drop-desc">Tap to browse, or drag a file here. Up to 128 KB.</span>
        {/if}
      </button>
      <input bind:this={inputEl} type="file" accept=".wasm" onchange={onFileSelect} class="hidden" tabindex="-1" aria-hidden="true" />

      {#if status}
        <p class="note" class:err={status.tone === "err"} role="status">{status.text}</p>
      {/if}

      <button type="button" class="primary" onclick={upload} disabled={!selectedFile || uploading}>
        {uploading ? "Uploading…" : "Upload to the robot"}
      </button>
    </div>
  </div>
</div>

<style>
  .page { position: absolute; inset: 0; display: flex; flex-direction: column; background: var(--page); color: var(--ink); }
  .bar {
    flex-shrink: 0; height: calc(64px + var(--safe-top)); padding: calc(10px + var(--safe-top)) 16px 0;
    display: flex; align-items: center; justify-content: space-between;
    max-width: var(--page-max); width: 100%; margin: 0 auto;
  }
  .bar h1 { font-size: 18px; font-weight: 800; }
  .spacer { width: 44px; }
  .round {
    width: 44px; height: 44px; border-radius: 22px; border: none; background: var(--ground); color: var(--ink);
    display: grid; place-items: center; cursor: pointer;
  }

  .scroll { flex: 1; min-height: 0; overflow-y: auto; }
  .inner { max-width: var(--page-max); margin: 0 auto; padding: 8px 16px calc(24px + var(--safe-bottom)); display: flex; flex-direction: column; gap: 12px; }

  .drop {
    width: 100%; min-height: 200px; border: 1.5px dashed #CFC9BB; border-radius: 22px; background: #fff; color: var(--ink);
    display: flex; flex-direction: column; align-items: center; justify-content: center; gap: 6px;
    padding: 28px 20px; text-align: center; cursor: pointer;
    transition: background var(--dur-fast), border-color var(--dur-fast);
  }
  .drop.dragging { border-color: var(--ink); background: #FFF6B0; }
  .drop.picked { border-style: solid; border-color: var(--line); }
  .ico { width: 52px; height: 52px; border-radius: 15px; display: grid; place-items: center; background: var(--ground); margin-bottom: 6px; }
  .drop.picked .ico { background: #CFE3FF; }
  .drop-title { font-size: 17px; font-weight: 800; word-break: break-all; }
  .drop-desc { font-size: 14px; line-height: 1.45; color: var(--sub); max-width: 300px; }
  .hidden { display: none; }

  .note { font-size: 13px; color: #1E7B34; font-weight: 600; background: var(--ground); border-radius: 14px; padding: 10px 12px; }
  .note.err { color: #9B2C20; }

  .primary {
    height: 56px; border-radius: 28px; border: none; background: var(--ink); color: #fff;
    font-size: 16px; font-weight: 700; cursor: pointer; transition: transform var(--dur-fast), opacity var(--dur-fast);
  }
  .primary:active:not(:disabled) { transform: scale(0.98); }
  .primary:disabled { opacity: 0.35; cursor: default; }
  button:focus-visible { outline: 3px solid var(--ink); outline-offset: 2px; }
</style>
