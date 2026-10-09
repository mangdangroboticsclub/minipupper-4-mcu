<script>
  import { colors } from "./colors.js";

  let { onBack } = $props();

  const YELLOW = colors.mpx.primary;

  let dragging = $state(false);
  let uploading = $state(false);
  let uploadStatus = $state("");
  let selectedFile = $state(null);
  let dragRef = $state(null);

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
    }
  }

  function onFileSelect(e) {
    if (e.target.files.length > 0) {
      selectedFile = e.target.files[0];
    }
  }

  async function upload() {
    if (!selectedFile) return;
    uploading = true;
    uploadStatus = "Uploading…";

    try {
      const name = encodeURIComponent(selectedFile.name);
      const res = await fetch(`/v1/skills/upload?name=${name}`, {
        method: "POST",
        body: await selectedFile.arrayBuffer(),
      });

      if (res.ok) {
        uploadStatus = `✅ ${selectedFile.name} uploaded successfully`;
        selectedFile = null;
      } else {
        const err = await res.text();
        uploadStatus = `❌ Upload failed: ${err}`;
      }
    } catch (e) {
      uploadStatus = `❌ Error: ${e.message}`;
    }
    uploading = false;
  }

  function formatSize(bytes) {
    if (bytes < 1024) return `${bytes} B`;
    return `${(bytes / 1024).toFixed(1)} KB`;
  }
</script>

<div class="upload-root">
  <header class="upload-header" style="background: {YELLOW}">
    <button class="upload-back-btn" onclick={onBack} aria-label="Go back">
      <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
        <line x1="19" y1="12" x2="5" y2="12"/>
        <polyline points="12 19 5 12 12 5"/>
      </svg>
    </button>
    <h2 class="upload-title">Upload Skill</h2>
  </header>

  <div class="upload-body">
    <div
      bind:this={dragRef}
      ondragover={onDragOver}
      ondragleave={onDragLeave}
      ondrop={onDrop}
      class="upload-dropzone"
      class:upload-dragging={dragging}
      style="--drop-border: {YELLOW}"
      onclick={() => dragRef?.querySelector("input").click()}
      role="button"
      tabindex="0"
      onkeydown={(e) => e.key === "Enter" && dragRef?.querySelector("input").click()}
    >
      <input type="file" accept=".wasm" onchange={onFileSelect} class="upload-hidden-input" />
      {#if selectedFile}
        <span class="upload-drop-icon">📄</span>
        <p class="upload-file-name">{selectedFile.name}</p>
        <p class="upload-file-size">{formatSize(selectedFile.size)}</p>
      {:else}
        <span class="upload-drop-icon">📤</span>
        <p class="upload-drop-text">
          {dragging ? "Drop it here!" : "Tap to browse for a .wasm file"}
        </p>
        <p class="upload-drop-hint">Max file size: 128 KB</p>
      {/if}
    </div>

    {#if selectedFile}
      <button class="upload-btn" onclick={upload} disabled={uploading}
              style="background: {YELLOW}"
              class:upload-disabled={uploading}>
        {uploading ? "Uploading…" : "Upload to Robot"}
      </button>
    {/if}

    {#if uploadStatus}
      <p class="upload-status" class:upload-success={uploadStatus.includes('✅')} class:upload-error={!uploadStatus.includes('✅')}>
        {uploadStatus}
      </p>
    {/if}
  </div>
</div>

<style>
  .upload-root {
    flex: 1;
    background: #f5f5f5;
    display: flex;
    flex-direction: column;
  }

  .upload-header {
    display: flex;
    align-items: center;
    gap: 10px;
    padding: 12px 14px;
    flex-shrink: 0;
  }

  .upload-back-btn {
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
  .upload-back-btn:hover {
    background: rgba(0,0,0,0.2);
  }

  .upload-title {
    font-size: 1.05rem;
    font-weight: 800;
    color: #000;
    margin: 0;
  }

  .upload-body {
    flex: 1;
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    gap: 20px;
    padding: 24px 20px;
  }

  .upload-dropzone {
    width: 100%;
    max-width: 320px;
    border: 2.5px dashed #ccc;
    border-radius: 16px;
    padding: 32px 20px;
    text-align: center;
    cursor: pointer;
    transition: border-color 0.2s, background 0.2s;
  }
  .upload-dragging {
    border-color: var(--drop-border, #FFE605);
    background: rgba(255,230,5,0.06);
  }
  .upload-dropzone:hover {
    border-color: #999;
  }

  .upload-hidden-input {
    display: none;
  }

  .upload-drop-icon {
    font-size: 2.5rem;
    display: block;
    margin-bottom: 8px;
  }

  .upload-drop-text {
    font-size: 0.9rem;
    font-weight: 600;
    color: #333;
    margin: 0;
  }

  .upload-drop-hint {
    font-size: 0.75rem;
    color: #999;
    margin: 4px 0 0;
  }

  .upload-file-name {
    font-size: 0.9rem;
    font-weight: 600;
    color: #000;
    margin: 0;
  }

  .upload-file-size {
    font-size: 0.75rem;
    color: #999;
    margin: 2px 0 0;
  }

  .upload-btn {
    border: none;
    border-radius: 12px;
    padding: 12px 32px;
    font-size: 0.9rem;
    font-weight: 700;
    color: #000;
    cursor: pointer;
    transition: filter 0.15s;
  }
  .upload-btn:hover {
    filter: brightness(0.9);
  }
  .upload-disabled {
    opacity: 0.5;
  }

  .upload-status {
    font-size: 0.8rem;
    text-align: center;
    max-width: 300px;
    margin: 0;
  }
  .upload-success {
    color: #22c55e;
  }
  .upload-error {
    color: #ef4444;
  }
</style>
