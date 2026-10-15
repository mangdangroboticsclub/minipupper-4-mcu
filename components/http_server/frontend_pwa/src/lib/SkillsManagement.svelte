<script>
  import { colors, skillTypeColor, skillTypeLabel } from "./colors.js";
  import {
    listRobotSkills,
    toggleSkill,
    deploySkill,
    enqueueLua,
  } from "./marketplaceApi.js";

  let { onNavigate } = $props();

  const YELLOW = colors.mpx.primary;

  // ── State ───────────────────────────────────────────────────
  let skills = $state([]);
  let loading = $state(true);
  let error = $state("");
  let actionInFlight = $state(null); // skill_id being acted on, or "deploy:ID" / "delete:ID"

  // ── Deployed-skill tracking (persisted to localStorage) ─────
  // The robot API doesn't return a `deployed` flag, so we track it
  // client-side. Each entry stores the file paths written during deploy
  // so we can delete them from LittleFS later.
  // Shape: { [skillId]: string[] }  — skill_id → array of file paths
  const DEPLOYED_STORAGE_KEY = "mpx_deployed_skills";

  /** @type {Record<string, string[]>} */
  let deployedSkills = $state({});

  function loadDeployedState() {
    try {
      const raw = localStorage.getItem(DEPLOYED_STORAGE_KEY);
      if (raw) {
        deployedSkills = JSON.parse(raw);
      }
    } catch { /* ignore */ }
  }

  function saveDeployedState() {
    try {
      localStorage.setItem(DEPLOYED_STORAGE_KEY, JSON.stringify(deployedSkills));
    } catch { /* ignore */ }
  }

  // Load persisted state on mount
  $effect(() => { loadDeployedState(); });

  // ── Fetch ────────────────────────────────────────────────────
  async function fetchSkills() {
    loading = true;
    error = "";
    try {
      skills = await listRobotSkills();
    } catch (e) {
      error = e.message || "Failed to load skills";
      skills = [];
    }
    loading = false;
  }

  $effect(() => { fetchSkills(); });

  // ── Helpers ──────────────────────────────────────────────────
  function isWasm(skill) {
    return (skill.skill_type || "").toLowerCase() === "wasm";
  }

  /**
   * Extract file paths from Lua deploy scripts.
   * Looks for file_write("/path", ...) calls in the script text.
   */
  function extractFilePaths(scripts) {
    const paths = [];
    for (const script of scripts) {
      const re = /file_write\s*\(\s*["']([^"']+)["']/g;
      let match;
      while ((match = re.exec(script)) !== null) {
        paths.push(match[1]);
      }
    }
    return paths;
  }

  // ── Toggle (non-WASM skills only) ────────────────────────────
  async function handleToggle(skillId, enabled) {
    actionInFlight = skillId;
    try {
      await toggleSkill(skillId, enabled);
      await fetchSkills();
    } catch (e) {
      console.error("Toggle failed:", e);
    }
    actionInFlight = null;
  }

  // ── Deploy WASM skill to MPX ─────────────────────────────────
  async function handleDeploy(skill) {
    const key = "deploy:" + skill.skill_id;
    actionInFlight = key;
    try {
      const data = await deploySkill(skill.skill_id);
      const allScripts = [];
      if (data.skills && data.skills.length > 0) {
        for (const s of data.skills) {
          for (const cmd of s.commands || []) {
            allScripts.push(cmd.script);
            await enqueueLua(cmd.script);
          }
        }
      }
      // Extract file paths written by the deploy scripts
      const filePaths = extractFilePaths(allScripts);
      // Mark as deployed with file paths for later deletion
      deployedSkills = { ...deployedSkills, [skill.skill_id]: filePaths };
      saveDeployedState();
      await fetchSkills();
    } catch (e) {
      console.error("Deploy failed:", e);
    }
    actionInFlight = null;
  }

  // ── Delete deployed WASM files from LittleFS ─────────────────
  // Does NOT unsubscribe/refund — only removes the files.
  async function handleDelete(skill) {
    const key = "delete:" + skill.skill_id;
    actionInFlight = key;
    try {
      const filePaths = deployedSkills[skill.skill_id] || [];
      // Delete each deployed file from LittleFS
      for (const filePath of filePaths) {
        await fetch("/v1/fs/delete", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ path: filePath }),
        });
      }
      // Remove from deployed tracking
      const next = { ...deployedSkills };
      delete next[skill.skill_id];
      deployedSkills = next;
      saveDeployedState();
      await fetchSkills();
    } catch (e) {
      console.error("Delete failed:", e);
    }
    actionInFlight = null;
  }

  // ── Determine button state for WASM skills ───────────────────
  function isDeployed(skill) {
    return skill.skill_id in deployedSkills;
  }
</script>

<div class="skills-mgmt-root" style="--yellow: {YELLOW}">
  <!-- Header -->
  <header class="mgmt-header">
    <button class="back-btn" onclick={() => onNavigate("back")} aria-label="Back">
      <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
        <line x1="19" y1="12" x2="5" y2="12"/>
        <polyline points="12 19 5 12 12 5"/>
      </svg>
    </button>
    <h2 class="mgmt-title">Skill Management</h2>
    <button class="refresh-btn" onclick={fetchSkills} aria-label="Refresh">
      <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
        <polyline points="23 4 23 10 17 10"/>
        <path d="M20.49 15a9 9 0 1 1-2.12-9.36L23 10"/>
      </svg>
    </button>
  </header>

  <!-- Content -->
  <div class="mgmt-content">
    {#if loading}
      <p class="mgmt-empty">Loading skills…</p>
    {:else if error}
      <div class="mgmt-error">
        <p>{error}</p>
        <button class="mgmt-retry-btn" onclick={fetchSkills}>Retry</button>
      </div>
    {:else if skills.length === 0}
      <div class="mgmt-empty-state">
        <div class="mgmt-empty-icon">📦</div>
        <p class="mgmt-empty-title">No Skills Yet</p>
        <p class="mgmt-empty-desc">
          Browse the Skill Store to find and subscribe to skills for your MPX Dog.
        </p>
      </div>
    {:else}
      <div class="mgmt-list">
        {#each skills as skill (skill.skill_id)}
          <div class="mgmt-card">
            <!-- Skill info -->
            <div class="mgmt-card-left">
              <div class="mgmt-card-icon">
                <span>{skill.title?.charAt(0) || "⚡"}</span>
              </div>
              <div class="mgmt-card-info">
                <div class="mgmt-card-title-row">
                  <span class="mgmt-card-title">{skill.title || "Unknown Skill"}</span>
                  <span
                    class="mgmt-type-badge"
                    style="--badge-color: {skillTypeColor(skill.skill_type)}"
                  >
                    {skillTypeLabel(skill.skill_type)}
                  </span>
                </div>
                <span class="mgmt-card-version">v{skill.current_version || "1.0"}</span>
              </div>
            </div>

            <!-- Actions -->
            <div class="mgmt-card-actions">
              {#if isWasm(skill)}
                <!-- WASM: Download / Delete button (no toggle) -->
                {#if isDeployed(skill)}
                  <button
                    class="mgmt-btn mgmt-btn-delete"
                    disabled={actionInFlight === "delete:" + skill.skill_id}
                    onclick={() => handleDelete(skill)}
                  >
                    {actionInFlight === "delete:" + skill.skill_id ? "…" : "Delete from MPX"}
                  </button>
                {:else}
                  <button
                    class="mgmt-btn mgmt-btn-download"
                    disabled={actionInFlight === "deploy:" + skill.skill_id}
                    onclick={() => handleDeploy(skill)}
                  >
                    {actionInFlight === "deploy:" + skill.skill_id ? "…" : "Download to MPX"}
                  </button>
                {/if}
              {:else}
                <!-- Non-WASM (AWA): toggle switch -->
                <label class="mgmt-toggle" class:disabled={actionInFlight === skill.skill_id}>
                  <input
                    type="checkbox"
                    checked={skill.enabled}
                    onchange={() => handleToggle(skill.skill_id, !skill.enabled)}
                    disabled={actionInFlight === skill.skill_id}
                  />
                  <span class="mgmt-toggle-track"></span>
                  <span class="mgmt-toggle-thumb"></span>
                </label>
              {/if}
            </div>
          </div>
        {/each}
      </div>
    {/if}
  </div>
</div>

<style>
  .skills-mgmt-root {
    position: absolute;
    inset: 0;
    background: var(--surface-1);
    display: flex;
    flex-direction: column;
  }

  /* ── Header ────────────────────────────── */
  .mgmt-header {
    background: var(--bg);
    border-bottom: 1px solid var(--line);
    display: flex;
    align-items: center;
    gap: 10px;
    padding: 10px 14px;
    flex-shrink: 0;
  }

  .back-btn {
    display: flex;
    align-items: center;
    justify-content: center;
    background: none;
    border: none;
    cursor: pointer;
    color: var(--text-1);
    padding: 4px;
    border-radius: 8px;
  }
  .back-btn:active { background: var(--surface-2); }

  .mgmt-title {
    flex: 1;
    font-size: 1.05rem;
    font-weight: 700;
    color: var(--text-1);
    text-align: center;
  }

  .refresh-btn {
    display: flex;
    align-items: center;
    justify-content: center;
    background: none;
    border: none;
    cursor: pointer;
    color: var(--text-1);
    padding: 4px;
    border-radius: 8px;
  }
  .refresh-btn:active { background: var(--surface-2); }

  /* ── Content ───────────────────────────── */
  .mgmt-content {
    flex: 1;
    overflow-y: auto;
    padding: 16px 14px;
  }

  .mgmt-empty {
    text-align: center;
    color: var(--text-3);
    font-size: 0.9rem;
    margin-top: 40px;
  }

  .mgmt-error {
    text-align: center;
    color: var(--danger);
    font-size: 0.85rem;
    margin-top: 40px;
  }

  .mgmt-retry-btn {
    margin-top: 10px;
    padding: 6px 16px;
    border-radius: 8px;
    border: 1px solid var(--line);
    background: var(--surface-1);
    font-size: 0.8rem;
    cursor: pointer;
    color: var(--text-1);
  }
  .mgmt-retry-btn:active { background: var(--surface-2); }

  /* ── Empty state ───────────────────────── */
  .mgmt-empty-state {
    display: flex;
    flex-direction: column;
    align-items: center;
    margin-top: 48px;
    text-align: center;
    gap: 8px;
  }
  .mgmt-empty-icon {
    font-size: 3rem;
    margin-bottom: 4px;
  }
  .mgmt-empty-title {
    font-size: 1rem;
    font-weight: 600;
    color: var(--text-1);
  }
  .mgmt-empty-desc {
    font-size: 0.8rem;
    color: var(--text-3);
    max-width: 240px;
    line-height: 1.4;
  }

  /* ── List ──────────────────────────────── */
  .mgmt-list {
    display: flex;
    flex-direction: column;
    gap: 10px;
  }

  /* ── Card ──────────────────────────────── */
  .mgmt-card {
    display: flex;
    align-items: center;
    justify-content: space-between;
    background: var(--surface-2);
    border: 1px solid var(--line);
    border-radius: 14px;
    padding: 14px;
    gap: 12px;
  }

  .mgmt-card-left {
    display: flex;
    align-items: center;
    gap: 10px;
    min-width: 0;
    flex: 1;
  }

  .mgmt-card-icon {
    width: 40px;
    height: 40px;
    border-radius: 10px;
    background: var(--yellow);
    display: flex;
    align-items: center;
    justify-content: center;
    font-weight: 700;
    font-size: 1.1rem;
    color: var(--text-1);
    flex-shrink: 0;
  }

  .mgmt-card-info {
    display: flex;
    flex-direction: column;
    gap: 2px;
    min-width: 0;
    flex: 1;
  }

  .mgmt-card-title-row {
    display: flex;
    align-items: center;
    gap: 6px;
  }

  .mgmt-card-title {
    font-size: 0.9rem;
    font-weight: 600;
    color: var(--text-1);
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }

  .mgmt-type-badge {
    display: inline-block;
    padding: 2px 7px;
    border-radius: 999px;
    font-size: 0.6rem;
    font-weight: 700;
    letter-spacing: 0.03em;
    color: var(--solid-ink);
    background: var(--badge-color);
    flex-shrink: 0;
  }

  .mgmt-card-version {
    font-size: 0.75rem;
    color: var(--text-3);
  }

  /* ── Actions ───────────────────────────── */
  .mgmt-card-actions {
    flex-shrink: 0;
    display: flex;
    align-items: center;
  }

  /* Download / Delete buttons */
  .mgmt-btn {
    padding: 8px 14px;
    border-radius: 10px;
    border: none;
    font-size: 0.78rem;
    font-weight: 600;
    cursor: pointer;
    transition: opacity 0.15s;
    white-space: nowrap;
  }
  .mgmt-btn:disabled { opacity: 0.5; cursor: default; }

  .mgmt-btn-download {
    background: var(--accent);
    color: var(--accent-ink);
  }
  .mgmt-btn-download:active:not(:disabled) { filter: brightness(0.92); }

  .mgmt-btn-delete {
    background: var(--danger);
    color: var(--solid-ink);
  }
  .mgmt-btn-delete:active:not(:disabled) { filter: brightness(0.9); }

  /* ── Toggle Switch ─────────────────────── */
  .mgmt-toggle {
    position: relative;
    display: inline-flex;
    align-items: center;
    cursor: pointer;
    width: 44px;
    height: 26px;
  }
  .mgmt-toggle.disabled { opacity: 0.5; cursor: default; }

  .mgmt-toggle input {
    position: absolute;
    opacity: 0;
    width: 0;
    height: 0;
  }

  .mgmt-toggle-track {
    position: absolute;
    inset: 0;
    border-radius: 999px;
    background: #d1d1d1;
    transition: background 0.2s;
  }
  .mgmt-toggle input:checked ~ .mgmt-toggle-track {
    background: var(--yellow);
  }

  .mgmt-toggle-thumb {
    position: absolute;
    top: 3px;
    left: 3px;
    width: 20px;
    height: 20px;
    border-radius: 50%;
    background: var(--surface-1);
    box-shadow: 0 1px 3px rgba(0,0,0,0.15);
    transition: transform 0.2s;
  }
  .mgmt-toggle input:checked ~ .mgmt-toggle-thumb {
    transform: translateX(18px);
  }
</style>
