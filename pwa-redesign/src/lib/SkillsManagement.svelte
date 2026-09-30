<script>
  import { colors, skillTypeColor, skillTypeLabel } from "./colors.js";
  import {
    listRobotSkills,
    toggleSkill,
    deploySkill,
    enqueueLua,
  } from "./marketplaceApi.js";

  /**
   * @prop embedded — rendered as a bottom-nav tab (no back button)
   * @prop onNavigate — ("back" | "store") => void
   */
  let { onNavigate, embedded = false } = $props();

  const YELLOW = colors.mpx.primary;

  // ── State ───────────────────────────────────────────────────
  let skills = $state([]);
  let loading = $state(true);
  let error = $state("");
  let actionInFlight = $state(null); // skill_id being acted on, or "deploy:ID" / "delete:ID"
  let toast = $state(null);          // { text, tone }
  let toastTimer = null;

  function showToast(text, tone = "ok") {
    toast = { text, tone };
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => (toast = null), 2600);
  }

  // ── Deployed-skill tracking (persisted to localStorage) ─────
  // The robot API doesn't return a `deployed` flag, so we track it
  // client-side. Each entry stores the file paths written during deploy
  // so we can delete them from LittleFS later.
  const DEPLOYED_STORAGE_KEY = "mpx_deployed_skills";

  /** @type {Record<string, string[]>} */
  let deployedSkills = $state({});

  function loadDeployedState() {
    try {
      const raw = localStorage.getItem(DEPLOYED_STORAGE_KEY);
      if (raw) deployedSkills = JSON.parse(raw);
    } catch { /* ignore */ }
  }
  function saveDeployedState() {
    try { localStorage.setItem(DEPLOYED_STORAGE_KEY, JSON.stringify(deployedSkills)); } catch { /* ignore */ }
  }
  $effect(() => { loadDeployedState(); });

  // ── Fetch ────────────────────────────────────────────────────
  async function fetchSkills({ silent = false } = {}) {
    if (!silent) loading = true;
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
  function isWasm(skill) { return (skill.skill_type || "").toLowerCase() === "wasm"; }
  function isDeployed(skill) { return skill.skill_id in deployedSkills; }

  /** Extract file paths from Lua deploy scripts (file_write("/path", …)). */
  function extractFilePaths(scripts) {
    const paths = [];
    for (const script of scripts) {
      const re = /file_write\s*\(\s*["']([^"']+)["']/g;
      let match;
      while ((match = re.exec(script)) !== null) paths.push(match[1]);
    }
    return paths;
  }

  // Group by type, keeping a stable order (AI first, then Move, then others)
  const TYPE_ORDER = ["awa", "wasm"];
  let groups = $derived.by(() => {
    const map = new Map();
    for (const s of skills) {
      const key = (s.skill_type || "other").toLowerCase();
      if (!map.has(key)) map.set(key, []);
      map.get(key).push(s);
    }
    const keys = [...map.keys()].sort((a, b) => {
      const ia = TYPE_ORDER.indexOf(a), ib = TYPE_ORDER.indexOf(b);
      return (ia === -1 ? 99 : ia) - (ib === -1 ? 99 : ib);
    });
    return keys.map((k) => ({ key: k, label: skillTypeLabel(k), color: skillTypeColor(k), items: map.get(k) }));
  });

  let stats = $derived({
    total: skills.length,
    enabled: skills.filter((s) => !isWasm(s) && s.enabled).length,
    onDevice: skills.filter((s) => isWasm(s) && isDeployed(s)).length,
  });

  // ── Toggle (non-WASM skills only) ────────────────────────────
  async function handleToggle(skill, enabled) {
    actionInFlight = skill.skill_id;
    try {
      await toggleSkill(skill.skill_id, enabled);
      await fetchSkills({ silent: true });
      showToast(`${skill.title || "Skill"} ${enabled ? "enabled" : "disabled"}`);
    } catch (e) {
      console.error("Toggle failed:", e);
      showToast("Couldn't update skill", "err");
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
      const filePaths = extractFilePaths(allScripts);
      deployedSkills = { ...deployedSkills, [skill.skill_id]: filePaths };
      saveDeployedState();
      await fetchSkills({ silent: true });
      showToast(`${skill.title || "Skill"} installed on MPX`);
    } catch (e) {
      console.error("Deploy failed:", e);
      showToast("Install failed", "err");
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
      for (const filePath of filePaths) {
        await fetch("/v1/fs/delete", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ path: filePath }),
        });
      }
      const next = { ...deployedSkills };
      delete next[skill.skill_id];
      deployedSkills = next;
      saveDeployedState();
      await fetchSkills({ silent: true });
      showToast(`${skill.title || "Skill"} removed from MPX`);
    } catch (e) {
      console.error("Delete failed:", e);
      showToast("Remove failed", "err");
    }
    actionInFlight = null;
  }

  function initial(skill) {
    return (skill.title || "?").trim().charAt(0).toUpperCase();
  }
</script>

<div class="skills-mgmt-root" class:embedded style="--yellow: {YELLOW}">
  <!-- Header -->
  <header class="mgmt-header">
    {#if !embedded}
      <button class="icon-btn" onclick={() => onNavigate?.("back")} aria-label="Back">
        <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
          <line x1="19" y1="12" x2="5" y2="12"/><polyline points="12 19 5 12 12 5"/>
        </svg>
      </button>
    {:else}
      <div class="icon-btn spacer"></div>
    {/if}
    <h2 class="mgmt-title">My Skills</h2>
    <button class="icon-btn" class:spinning={loading} onclick={() => fetchSkills()} aria-label="Refresh" disabled={loading}>
      <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
        <polyline points="23 4 23 10 17 10"/><path d="M20.49 15a9 9 0 1 1-2.12-9.36L23 10"/>
      </svg>
    </button>
  </header>

  <!-- Content -->
  <div class="mgmt-content">
    <div class="mgmt-inner">
      {#if loading}
        <!-- Skeleton -->
        <div class="stats-row skeleton-row">
          {#each [0,1,2] as _}<div class="stat skeleton"></div>{/each}
        </div>
        <div class="mgmt-list">
          {#each [0,1,2] as _}
            <div class="mgmt-card skeleton-card">
              <div class="skeleton sk-icon"></div>
              <div class="sk-lines"><div class="skeleton sk-line"></div><div class="skeleton sk-line short"></div></div>
              <div class="skeleton sk-pill"></div>
            </div>
          {/each}
        </div>
      {:else if error}
        <div class="mgmt-empty-state">
          <div class="empty-glyph err">!</div>
          <p class="mgmt-empty-title">Couldn't reach the store</p>
          <p class="mgmt-empty-desc">{error}</p>
          <button class="primary-btn" onclick={() => fetchSkills()}>Try again</button>
        </div>
      {:else if skills.length === 0}
        <div class="mgmt-empty-state">
          <div class="empty-glyph">
            <svg width="30" height="30" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
              <rect x="3" y="3" width="7" height="7" rx="2"/><rect x="14" y="3" width="7" height="7" rx="2"/><rect x="3" y="14" width="7" height="7" rx="2"/><path d="M17.5 14v7M14 17.5h7"/>
            </svg>
          </div>
          <p class="mgmt-empty-title">No skills yet</p>
          <p class="mgmt-empty-desc">Skills teach your Pupper new tricks — AI behaviours and motion routines. Grab your first one from the Store.</p>
          <button class="primary-btn" onclick={() => onNavigate?.("store")}>
            Browse the Store
            <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><path d="M5 12h14M13 5l7 7-7 7"/></svg>
          </button>
        </div>
      {:else}
        <!-- Summary -->
        <div class="stats-row">
          <div class="stat">
            <span class="stat-num">{stats.total}</span>
            <span class="stat-key">Owned</span>
          </div>
          <div class="stat">
            <span class="stat-num">{stats.enabled}</span>
            <span class="stat-key">Enabled</span>
          </div>
          <div class="stat">
            <span class="stat-num">{stats.onDevice}</span>
            <span class="stat-key">On device</span>
          </div>
        </div>

        {#each groups as group (group.key)}
          <section class="group">
            <header class="group-head">
              <span class="group-dot" style="background: {group.color}"></span>
              <h3 class="group-title">{group.label}</h3>
              <span class="group-count">{group.items.length}</span>
              <span class="group-hint">
                {group.key === "wasm" ? "Installed to the robot's storage" : group.key === "awa" ? "Runs in the cloud agent" : ""}
              </span>
            </header>

            <div class="mgmt-list">
              {#each group.items as skill (skill.skill_id)}
                {@const busy = actionInFlight === skill.skill_id || actionInFlight === "deploy:" + skill.skill_id || actionInFlight === "delete:" + skill.skill_id}
                <div class="mgmt-card" class:busy style="--tone: {group.color}">
                  <div class="mgmt-card-icon" aria-hidden="true">{initial(skill)}</div>

                  <div class="mgmt-card-info">
                    <div class="mgmt-card-title-row">
                      <span class="mgmt-card-title">{skill.title || "Unknown Skill"}</span>
                    </div>
                    <div class="mgmt-card-meta">
                      <span class="meta-chip">v{skill.current_version || "1.0"}</span>
                      {#if isWasm(skill)}
                        <span class="meta-chip" class:on={isDeployed(skill)}>{isDeployed(skill) ? "On device" : "Not installed"}</span>
                      {:else}
                        <span class="meta-chip" class:on={skill.enabled}>{skill.enabled ? "Enabled" : "Disabled"}</span>
                      {/if}
                    </div>
                  </div>

                  <div class="mgmt-card-actions">
                    {#if isWasm(skill)}
                      {#if isDeployed(skill)}
                        <button class="mgmt-btn mgmt-btn-delete" disabled={busy} onclick={() => handleDelete(skill)} aria-label="Remove {skill.title} from MPX">
                          {#if busy}<span class="spinner"></span>{:else}Remove{/if}
                        </button>
                      {:else}
                        <button class="mgmt-btn mgmt-btn-download" disabled={busy} onclick={() => handleDeploy(skill)} aria-label="Install {skill.title} on MPX">
                          {#if busy}<span class="spinner dark"></span>{:else}
                            <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><path d="M12 3v12"/><path d="M7 10l5 5 5-5"/><path d="M4 21h16"/></svg>
                            Install
                          {/if}
                        </button>
                      {/if}
                    {:else}
                      <label class="mgmt-toggle" class:disabled={busy} aria-label="{skill.enabled ? 'Disable' : 'Enable'} {skill.title}">
                        <input
                          type="checkbox"
                          checked={skill.enabled}
                          onchange={() => handleToggle(skill, !skill.enabled)}
                          disabled={busy}
                        />
                        <span class="mgmt-toggle-track"></span>
                        <span class="mgmt-toggle-thumb"></span>
                      </label>
                    {/if}
                  </div>
                </div>
              {/each}
            </div>
          </section>
        {/each}

        <button class="store-link" onclick={() => onNavigate?.("store")}>
          <span>Find more skills in the Store</span>
          <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><path d="M9 18l6-6-6-6"/></svg>
        </button>
      {/if}
    </div>
  </div>

  {#if toast}
    <div class="toast" class:err={toast.tone === "err"} role="status">{toast.text}</div>
  {/if}
</div>

<style>
  .skills-mgmt-root {
    position: absolute;
    inset: 0;
    background: var(--mpx-surface-2);
    display: flex;
    flex-direction: column;
  }
  .skills-mgmt-root.embedded { position: relative; flex: 1; min-height: 0; }

  /* ── Header ────────────────────────────── */
  .mgmt-header {
    display: flex;
    align-items: center;
    gap: 10px;
    padding: 10px 12px;
    background: var(--yellow);
    flex-shrink: 0;
  }
  .icon-btn {
    width: 34px; height: 34px;
    display: grid; place-items: center;
    background: none; border: none;
    cursor: pointer;
    color: var(--mpx-ink);
    border-radius: 50%;
    transition: background var(--dur-fast);
  }
  .icon-btn:hover { background: rgba(0,0,0,0.08); }
  .icon-btn:disabled { opacity: 0.6; cursor: default; }
  .icon-btn.spacer { visibility: hidden; }
  .icon-btn.spinning svg { animation: spin 0.9s linear infinite; }
  @keyframes spin { to { transform: rotate(360deg); } }

  .mgmt-title { flex: 1; font-size: 1.02rem; font-weight: 800; color: var(--mpx-ink); text-align: center; }

  /* ── Content ───────────────────────────── */
  .mgmt-content { flex: 1; overflow-y: auto; -webkit-overflow-scrolling: touch; }
  .mgmt-inner {
    max-width: var(--page-max);
    margin: 0 auto;
    padding: 14px 14px calc(24px + var(--safe-bottom));
    display: flex;
    flex-direction: column;
    gap: 16px;
  }
  @media (min-width: 900px) { .mgmt-inner { max-width: 820px; } }

  /* ── Stats ─────────────────────────────── */
  .stats-row { display: grid; grid-template-columns: repeat(3, 1fr); gap: 8px; }
  .stat {
    background: var(--mpx-surface);
    border-radius: var(--r-md);
    padding: 12px 12px 10px;
    display: flex; flex-direction: column; gap: 2px;
    box-shadow: var(--shadow-card);
  }
  .stat-num { font-size: 1.35rem; font-weight: 800; color: var(--mpx-ink); line-height: 1; font-variant-numeric: tabular-nums; }
  .stat-key { font-size: 0.66rem; font-weight: 700; color: var(--mpx-muted); text-transform: uppercase; letter-spacing: 0.06em; }

  /* ── Groups ────────────────────────────── */
  .group { display: flex; flex-direction: column; gap: 8px; }
  .group-head { display: flex; align-items: center; gap: 8px; padding: 0 4px; flex-wrap: wrap; }
  .group-dot { width: 9px; height: 9px; border-radius: 50%; }
  .group-title { font-size: 0.8rem; font-weight: 800; color: var(--mpx-ink); }
  .group-count { font-size: 0.66rem; font-weight: 700; color: var(--mpx-muted); background: var(--mpx-surface-3); border-radius: var(--r-pill); padding: 1px 7px; }
  .group-hint { margin-left: auto; font-size: 0.68rem; color: var(--mpx-muted); }

  .mgmt-list { display: flex; flex-direction: column; gap: 8px; }
  @media (min-width: 900px) { .mgmt-list { display: grid; grid-template-columns: 1fr 1fr; } }

  /* ── Card ──────────────────────────────── */
  .mgmt-card {
    display: flex;
    align-items: center;
    gap: 12px;
    background: var(--mpx-surface);
    border-radius: var(--r-lg);
    padding: 12px 12px 12px 14px;
    box-shadow: var(--shadow-card);
    border-left: 4px solid var(--tone, var(--mpx-line));
    transition: opacity var(--dur-med);
  }
  .mgmt-card.busy { opacity: 0.75; }

  .mgmt-card-icon {
    width: 44px; height: 44px;
    border-radius: var(--r-md);
    background: color-mix(in srgb, var(--tone) 16%, #fff);
    color: var(--tone);
    display: grid; place-items: center;
    font-weight: 800; font-size: 1.1rem;
    flex-shrink: 0;
  }
  .mgmt-card-info { display: flex; flex-direction: column; gap: 5px; min-width: 0; flex: 1; }
  .mgmt-card-title-row { display: flex; align-items: center; gap: 6px; }
  .mgmt-card-title { font-size: 0.92rem; font-weight: 700; color: var(--mpx-ink); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  .mgmt-card-meta { display: flex; gap: 6px; flex-wrap: wrap; }
  .meta-chip {
    font-size: 0.64rem; font-weight: 700;
    padding: 2px 7px;
    border-radius: var(--r-pill);
    background: var(--mpx-surface-2);
    color: var(--mpx-muted);
  }
  .meta-chip.on { background: #ecfdf3; color: #15803d; }

  .mgmt-card-actions { flex-shrink: 0; display: flex; align-items: center; }

  .mgmt-btn {
    min-width: 88px;
    height: 36px;
    padding: 0 12px;
    border-radius: var(--r-sm);
    border: none;
    font-size: 0.78rem;
    font-weight: 700;
    cursor: pointer;
    display: inline-flex; align-items: center; justify-content: center; gap: 6px;
    transition: transform var(--dur-fast), filter var(--dur-fast);
    white-space: nowrap;
  }
  .mgmt-btn:active:not(:disabled) { transform: scale(0.96); }
  .mgmt-btn:disabled { cursor: default; }
  .mgmt-btn-download { background: var(--mpx-ink); color: var(--yellow); }
  .mgmt-btn-delete { background: var(--mpx-surface-2); color: #b42318; }

  .spinner {
    width: 14px; height: 14px;
    border-radius: 50%;
    border: 2px solid rgba(0,0,0,0.15);
    border-top-color: #b42318;
    animation: spin 0.8s linear infinite;
  }
  .spinner.dark { border-color: rgba(255,230,5,0.25); border-top-color: var(--yellow); }

  /* ── Toggle Switch ─────────────────────── */
  .mgmt-toggle { position: relative; display: inline-flex; align-items: center; cursor: pointer; width: 48px; height: 28px; }
  .mgmt-toggle.disabled { opacity: 0.5; cursor: default; }
  .mgmt-toggle input { position: absolute; opacity: 0; width: 0; height: 0; }
  .mgmt-toggle-track { position: absolute; inset: 0; border-radius: var(--r-pill); background: #d6d6d6; transition: background var(--dur-med); }
  .mgmt-toggle input:checked ~ .mgmt-toggle-track { background: var(--mpx-ink); }
  .mgmt-toggle-thumb {
    position: absolute; top: 3px; left: 3px;
    width: 22px; height: 22px;
    border-radius: 50%;
    background: #fff;
    box-shadow: 0 1px 3px rgba(0,0,0,0.2);
    transition: transform var(--dur-med) var(--ease-spring), background var(--dur-med);
  }
  .mgmt-toggle input:checked ~ .mgmt-toggle-thumb { transform: translateX(20px); background: var(--yellow); }
  .mgmt-toggle input:focus-visible ~ .mgmt-toggle-track { outline: 2px solid var(--mpx-ink); outline-offset: 2px; }

  /* ── Store link ────────────────────────── */
  .store-link {
    display: flex; align-items: center; justify-content: space-between;
    padding: 14px 16px;
    border: none;
    border-radius: var(--r-lg);
    background: var(--yellow);
    color: var(--mpx-ink);
    font-size: 0.86rem; font-weight: 800;
    cursor: pointer;
    transition: transform var(--dur-fast);
  }
  .store-link:active { transform: scale(0.985); }

  /* ── Empty / error ─────────────────────── */
  .mgmt-empty-state {
    display: flex; flex-direction: column; align-items: center;
    margin-top: 10vh;
    text-align: center;
    gap: 10px;
    padding: 0 12px;
  }
  .empty-glyph {
    width: 72px; height: 72px;
    border-radius: var(--r-lg);
    background: var(--yellow);
    color: var(--mpx-ink);
    display: grid; place-items: center;
    margin-bottom: 6px;
    box-shadow: var(--shadow-card);
  }
  .empty-glyph.err { background: #fde8e8; color: #b42318; font-weight: 800; font-size: 1.6rem; }
  .mgmt-empty-title { font-size: 1.05rem; font-weight: 800; color: var(--mpx-ink); }
  .mgmt-empty-desc { font-size: 0.82rem; color: var(--mpx-muted); max-width: 300px; line-height: 1.5; }
  .primary-btn {
    margin-top: 8px;
    display: inline-flex; align-items: center; gap: 8px;
    padding: 12px 20px;
    border: none;
    border-radius: var(--r-md);
    background: var(--mpx-ink);
    color: #fff;
    font-size: 0.88rem; font-weight: 700;
    cursor: pointer;
  }
  .primary-btn:active { transform: scale(0.97); }

  /* ── Skeleton ──────────────────────────── */
  .skeleton {
    background: linear-gradient(90deg, #ececec 25%, #f6f6f6 50%, #ececec 75%);
    background-size: 200% 100%;
    animation: shimmer 1.4s linear infinite;
    border-radius: var(--r-sm);
  }
  @keyframes shimmer { from { background-position: 200% 0; } to { background-position: -200% 0; } }
  .skeleton-row .stat { height: 58px; padding: 0; box-shadow: none; }
  .skeleton-card { border-left-color: var(--mpx-line); }
  .sk-icon { width: 44px; height: 44px; border-radius: var(--r-md); flex-shrink: 0; }
  .sk-lines { flex: 1; display: flex; flex-direction: column; gap: 8px; }
  .sk-line { height: 12px; width: 70%; }
  .sk-line.short { width: 40%; height: 10px; }
  .sk-pill { width: 48px; height: 28px; border-radius: var(--r-pill); }

  /* ── Toast ─────────────────────────────── */
  .toast {
    position: absolute;
    left: 50%;
    bottom: calc(16px + var(--safe-bottom));
    transform: translateX(-50%);
    background: var(--mpx-ink);
    color: #fff;
    font-size: 0.78rem; font-weight: 700;
    padding: 10px 16px;
    border-radius: var(--r-pill);
    box-shadow: var(--shadow-float);
    white-space: nowrap;
    animation: toast-in var(--dur-med) var(--ease-spring);
    z-index: 5;
  }
  .toast.err { background: #b42318; }
  @keyframes toast-in { from { opacity: 0; transform: translate(-50%, 10px); } to { opacity: 1; transform: translate(-50%, 0); } }
</style>
