<script>
  /**
   * The Store — browse and install skills.
   *
   * ── WHAT THIS REPLACES ───────────────────────────────────────────────────
   *
   * The old Store tab listed the skills you already owned with a toggle each.
   * That is a library, not a shop: there was no way to see a skill you did not
   * have, and no way to get one.
   *
   * ── WHERE THE RANKING COMES FROM ─────────────────────────────────────────
   *
   * The gateway decides. GET /v1/marketplace/storefront returns featured,
   * sections and counts already ordered, and states its rule in `ranking`
   * (today: newest-by-publish-date). Doing it server-side means every client
   * agrees on what "Featured" means, and the day install counts exist nothing
   * in this file has to change.
   *
   * Nothing here invents a rating or an install count. The metadata shown is
   * what the platform genuinely knows — maker, version, how many permissions
   * a skill asks for, and whether this robot can run it at all.
   */
  import { skillTypeColor, skillTypeLabel } from "./colors.js";
  import { getStorefront, getStorefrontSkill, getConfig } from "./marketplaceApi.js";
  import InstallSheet from "./InstallSheet.svelte";

  /**
   * The robot's own UUID, needed so the store knows what is already installed
   * and what this hardware can run.
   *
   * Resolved here rather than taken as a prop: the app talks to "the robot it
   * is served from" and never names it, so no parent has this to give. It
   * comes from /v1/gateway/config, which the firmware answers with its own
   * CONFIG_APP_ROBOT_UUID.
   */
  let uuid = $state("");

  const YELLOW = "#FFE605";

  let loading = $state(true);
  let error = $state("");
  let store = $state(null);
  let typeFilter = $state("all");

  /** The skill whose install sheet is open, or null. */
  let pending = $state(null);
  let pendingBusy = $state(false);

  /** Set after a successful install, so the card can say so immediately. */
  let justInstalled = $state(new Set());

  const TYPES = [
    ["all", "All"],
    ["CAPABILITY", "AI"],
    ["AWA", "Web"],
    ["WASM", "Motion"],
  ];

  async function load() {
    loading = true;
    error = "";
    try {
      store = await getStorefront(uuid, typeFilter);
    } catch (err) {
      // The robot proxies this to the gateway over its STA interface, so the
      // usual cause is the robot being on its own hotspot with no route out —
      // worth saying, because "failed to load" sends people to the wrong place.
      error = err.message || "Could not reach the store";
      store = null;
    }
    loading = false;
  }

  async function setFilter(t) {
    if (typeFilter === t) return;
    typeFilter = t;
    await load();
  }

  /**
   * Open the permission sheet. Fetches the detail record first so the sheet
   * shows the readme and the full permission list rather than the summary the
   * card carries.
   */
  async function openInstall(skill) {
    pendingBusy = true;
    try {
      pending = await getStorefrontSkill(skill.skill_id, uuid);
    } catch {
      // Falling back to the card's own data rather than blocking the install:
      // the permission list is present on both, and it is the part that
      // matters. Only the readme is lost.
      pending = skill;
    }
    pendingBusy = false;
  }

  function onInstalled(result) {
    if (pending) justInstalled = new Set([...justInstalled, pending.skill_id]);
    pending = null;
    // Reload so counts, featured and ownership are the gateway's view again
    // rather than this component's guess.
    load();
    if (result && result.enabled === false) {
      error = "Installed, but could not switch it on. Turn it on in Library.";
    }
  }

  function isInstalled(skill) {
    return skill.owned || justInstalled.has(skill.skill_id);
  }

  $effect(() => {
    (async () => {
      if (!uuid) {
        // Without the UUID the store still renders — it just cannot tell what
        // is owned. Better a slightly wrong store than no store.
        try {
          const cfg = await getConfig();
          if (cfg?.robot_uuid) uuid = cfg.robot_uuid;
        } catch {
          /* fall through with no uuid */
        }
      }
      await load();
    })();
  });
</script>

<div class="st-root" style="--yellow: {YELLOW}">

  <div class="st-head">
    <h2 class="st-title">Store</h2>
    {#if store}
      <div class="st-robot">
        <span class="st-dot"></span>
        <span>{uuid || "this robot"}</span>
      </div>
    {/if}
  </div>

  <div class="st-filters" role="tablist" aria-label="Filter by skill type">
    {#each TYPES as [key, label] (key)}
      {#if key === "all" || (store?.counts?.[key] ?? 0) > 0}
        <button
          class="st-filter"
          class:st-filter-on={typeFilter === key}
          style="--badge: {key === 'all' ? '#6f6f6f' : skillTypeColor(key)}"
          role="tab"
          aria-selected={typeFilter === key}
          onclick={() => setFilter(key)}
        >
          {#if key !== "all"}<span class="st-dot-sm"></span>{/if}
          {label}
          {#if store?.counts}
            <span class="st-count">{store.counts[key === "all" ? "all" : key] ?? 0}</span>
          {/if}
        </button>
      {/if}
    {/each}
  </div>

  <div class="st-body">

    {#if loading && !store}
      <p class="st-wait">Loading the store…</p>

    {:else if error && !store}
      <div class="st-empty">
        <p class="st-empty-title">Store unavailable</p>
        <p class="st-empty-desc">
          The robot could not reach the marketplace. It needs to be on your
          Wi-Fi, not just its own hotspot.
        </p>
        <button class="st-btn st-btn-primary" onclick={load}>Try again</button>
      </div>

    {:else if store}

      {#if error}
        <p class="st-inline-error">{error}</p>
      {/if}

      <!-- ── Featured ─────────────────────────────────────────
           One skill, one pitch. The gateway picks it: newest that this robot
           can run and does not already own. -->
      {#if store.featured}
        <div class="st-hero">
          <div class="st-hero-grid"></div>
          <svg class="st-hero-art" width="150" height="106" viewBox="0 0 150 106" fill="none" aria-hidden="true">
            <g stroke="#F4A261" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round">
              <rect x="34" y="34" width="62" height="27" rx="9"></rect>
              <path d="M96 40l16-9 12 3v13l-12 4-16-2"></path>
              <path d="M112 31l3-9M120 31l6-7"></path>
              <circle cx="115" cy="41" r="2.4" fill="#F4A261" stroke="none"></circle>
              <path d="M44 61l-8 16 5 13"></path><path d="M58 61l6 15-3 14"></path>
              <path d="M78 61l-7 16 6 13"></path><path d="M92 61l7 14-4 15"></path>
              <path d="M34 45l-9-6"></path>
            </g>
          </svg>

          <div class="st-hero-body">
            <div class="st-hero-tags">
              <span class="st-hero-badge">Featured</span>
              <span class="st-hero-type" style="color: {skillTypeColor(store.featured.skill_type)}">
                {skillTypeLabel(store.featured.skill_type)} skill
              </span>
            </div>

            <h3 class="st-hero-title">{store.featured.title}</h3>
            {#if store.featured.readme}
              <p class="st-hero-desc">{store.featured.readme}</p>
            {/if}

            <div class="st-hero-actions">
              {#if isInstalled(store.featured)}
                <span class="st-hero-installed">Installed</span>
              {:else}
                <button class="st-hero-btn" disabled={pendingBusy}
                        onclick={() => openInstall(store.featured)}>Install</button>
              {/if}
              <div class="st-hero-meta">
                <span class="st-hero-pub">{store.featured.publisher}</span>
                <span class="st-hero-sub">
                  v{store.featured.current_version}
                  {#if store.featured.permission_count}
                    · {store.featured.permission_count} permission{store.featured.permission_count === 1 ? "" : "s"}
                  {/if}
                </span>
              </div>
            </div>
          </div>
        </div>
      {/if}

      <!-- ── Categories ───────────────────────────────────── -->
      {#if typeFilter === "all" && store.counts}
        <h4 class="st-kicker">Categories</h4>
        <div class="st-cats">
          {#each [["CAPABILITY", "AI"], ["AWA", "Web"], ["WASM", "Motion"]] as [key, label] (key)}
            <button class="st-cat" onclick={() => setFilter(key)}>
              <span class="st-cat-swatch" style="--badge: {skillTypeColor(key)}"></span>
              <span class="st-cat-name">{label}</span>
              <span class="st-cat-count">{store.counts[key] ?? 0} skills</span>
            </button>
          {/each}
        </div>
      {/if}

      <!-- ── Sections ─────────────────────────────────────── -->
      {#each store.sections as section (section.key)}
        <div class="st-section-head">
          <h3 class="st-section-title">{section.title}</h3>
        </div>

        <div class="st-list">
          {#each section.skills as skill (skill.skill_id)}
            <article class="st-card" class:st-card-blocked={!skill.runnable}>
              <div class="st-card-avatar" style="--badge: {skillTypeColor(skill.skill_type)}">
                {(skill.title || "?").charAt(0).toUpperCase()}
              </div>

              <div class="st-card-body">
                <div class="st-card-title-row">
                  <span class="st-card-title">{skill.title}</span>
                  {#if skill.is_new && skill.runnable}
                    <span class="st-new">New</span>
                  {/if}
                </div>

                {#if skill.runnable}
                  <div class="st-card-meta">
                    <span>{skill.publisher}</span>
                    <span class="st-sep"></span>
                    <span>
                      {skill.permission_count} permission{skill.permission_count === 1 ? "" : "s"}
                    </span>
                  </div>
                {:else}
                  <!-- Shown, not hidden. "Where did my skill go" is worse than
                       an explanation. -->
                  <div class="st-card-blocked-note">
                    Needs {skill.missing_capabilities.join(", ")} — not fitted on this robot
                  </div>
                {/if}
              </div>

              {#if isInstalled(skill)}
                <span class="st-get st-get-owned">Owned</span>
              {:else if !skill.runnable}
                <span class="st-get st-get-off">Get</span>
              {:else}
                <button class="st-get st-get-go" disabled={pendingBusy}
                        onclick={() => openInstall(skill)}>Get</button>
              {/if}
            </article>
          {/each}
        </div>
      {/each}

      {#if store.sections.length === 0 && !store.featured}
        <div class="st-empty">
          <p class="st-empty-title">Nothing here yet</p>
          <p class="st-empty-desc">
            No {typeFilter === "all" ? "" : skillTypeLabel(typeFilter) + " "}skills
            have been published to the marketplace.
          </p>
        </div>
      {/if}

      <!-- The store states its own ordering rule rather than implying an
           editorial process that does not exist. -->
      {#if store.ranking === "newest-by-publish-date"}
        <p class="st-ranking">Ordered by publish date — newest first.</p>
      {/if}

      <div class="st-tail"></div>
    {/if}
  </div>

  {#if pending}
    <InstallSheet
      skill={pending}
      onclose={() => (pending = null)}
      oninstalled={onInstalled}
    />
  {/if}
</div>

<style>
  .st-root {
    flex: 1;
    min-height: 0;
    background: var(--surface-2);
    display: flex;
    flex-direction: column;
    overflow: hidden;
    position: relative;
  }

  .st-head {
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 14px 16px 0;
  }

  .st-title { margin: 0; font-size: 1.3rem; font-weight: 800; letter-spacing: -0.025em; }

  .st-robot {
    display: flex;
    align-items: center;
    gap: 7px;
    padding: 5px 10px 5px 8px;
    background: var(--surface-1);
    border: 1.5px solid var(--line);
    border-radius: 999px;
    font-size: 0.68rem;
    font-weight: 700;
    color: #4a4a4a;
  }
  .st-dot { width: 7px; height: 7px; border-radius: 50%; background: var(--ok); }

  .st-filters {
    display: flex;
    gap: 6px;
    padding: 12px 16px 10px;
    overflow-x: auto;
    scrollbar-width: none;
  }
  .st-filters::-webkit-scrollbar { display: none; }

  .st-filter {
    flex-shrink: 0;
    display: inline-flex;
    align-items: center;
    gap: 5px;
    padding: 5px 11px;
    border: 1.5px solid var(--line);
    border-radius: 999px;
    background: var(--surface-1);
    color: var(--text-2);
    font-size: 0.72rem;
    font-weight: 700;
    cursor: pointer;
  }
  .st-filter-on {
    border-color: var(--badge);
    color: var(--text-1);
    background: color-mix(in srgb, var(--badge) 14%, var(--surface-1));
  }
  .st-dot-sm { width: 7px; height: 7px; border-radius: 50%; background: var(--badge); }
  .st-count { font-size: 0.62rem; font-weight: 800; color: var(--text-3); }
  .st-filter-on .st-count { color: var(--badge); }

  .st-body {
    flex: 1;
    min-height: 0;
    overflow-y: auto;
    -webkit-overflow-scrolling: touch;
    padding: 2px 16px 0;
  }

  .st-wait { margin: 20px 0; font-size: 0.8rem; color: var(--text-3); text-align: center; }

  .st-inline-error {
    margin: 0 0 12px;
    padding: 10px 12px;
    border-radius: 10px;
    background: #fdecec;
    color: #9c3535;
    font-size: 0.75rem;
    line-height: 1.4;
  }

  /* ── Featured ─────────────────────────────────── */
  .st-hero {
    position: relative;
    overflow: hidden;
    background: var(--solid);
    border-radius: 20px;
    padding: 20px;
  }

  /* Depth from a technical grid rather than a gradient. */
  .st-hero-grid {
    position: absolute;
    inset: 0;
    opacity: 0.5;
    background-image: linear-gradient(rgba(255, 255, 255, 0.045) 1px, transparent 1px),
      linear-gradient(90deg, rgba(255, 255, 255, 0.045) 1px, transparent 1px);
    background-size: 26px 26px;
  }

  .st-hero-art { position: absolute; right: 8px; bottom: 4px; opacity: 0.5; }
  .st-hero-body { position: relative; }

  .st-hero-tags { display: flex; align-items: center; gap: 7px; }

  .st-hero-badge {
    padding: 3px 9px;
    border-radius: 6px;
    background: var(--accent);
    color: var(--accent-ink);
    font-size: 0.56rem;
    font-weight: 800;
    letter-spacing: 0.08em;
    text-transform: uppercase;
  }

  .st-hero-type {
    font-size: 0.62rem;
    font-weight: 700;
    letter-spacing: 0.06em;
    text-transform: uppercase;
  }

  .st-hero-title {
    margin: 13px 0 0;
    font-size: 1.6rem;
    font-weight: 800;
    color: var(--solid-ink);
    letter-spacing: -0.035em;
    line-height: 1.08;
  }

  .st-hero-desc {
    margin: 9px 0 0;
    font-size: 0.81rem;
    line-height: 1.5;
    color: var(--solid-ink);
    max-width: 236px;
  }

  .st-hero-actions { display: flex; align-items: center; gap: 11px; margin-top: 18px; }

  .st-hero-btn {
    padding: 10px 20px;
    border: none;
    border-radius: 10px;
    background: var(--accent);
    color: var(--accent-ink);
    font-size: 0.82rem;
    font-weight: 800;
    cursor: pointer;
  }
  .st-hero-btn:disabled { opacity: 0.6; cursor: default; }

  .st-hero-installed {
    padding: 10px 20px;
    border-radius: 10px;
    background: var(--surface-2);
    color: var(--solid-ink);
    font-size: 0.82rem;
    font-weight: 800;
  }

  .st-hero-meta { display: flex; flex-direction: column; gap: 2px; min-width: 0; }
  .st-hero-pub { font-size: 0.72rem; font-weight: 700; color: var(--solid-ink); }
  .st-hero-sub { font-size: 0.65rem; color: var(--solid-ink); }

  /* ── Categories ───────────────────────────────── */
  .st-kicker {
    margin: 24px 0 11px;
    font-size: 0.7rem;
    font-weight: 800;
    letter-spacing: 0.1em;
    text-transform: uppercase;
    color: var(--text-3);
  }

  .st-cats { display: flex; gap: 9px; }

  .st-cat {
    flex: 1;
    display: flex;
    flex-direction: column;
    align-items: flex-start;
    gap: 2px;
    background: var(--surface-1);
    border: 1.5px solid var(--line);
    border-radius: 14px;
    padding: 13px 12px 12px;
    cursor: pointer;
    text-align: left;
  }

  .st-cat-swatch {
    width: 20px;
    height: 20px;
    border-radius: 7px;
    background: var(--badge);
    margin-bottom: 8px;
  }

  .st-cat-name { font-size: 0.88rem; font-weight: 800; letter-spacing: -0.01em; }
  .st-cat-count { font-size: 0.67rem; color: var(--text-3); font-weight: 600; }

  /* ── Sections ─────────────────────────────────── */
  .st-section-head { margin: 24px 0 11px; }
  .st-section-title { margin: 0; font-size: 1rem; font-weight: 800; letter-spacing: -0.02em; }

  .st-list { display: flex; flex-direction: column; gap: 8px; }

  .st-card {
    display: flex;
    align-items: center;
    gap: 12px;
    background: var(--surface-1);
    border: 1.5px solid var(--line);
    border-radius: 14px;
    padding: 13px;
  }
  .st-card-blocked { background: var(--surface-2); }

  .st-card-avatar {
    width: 46px;
    height: 46px;
    flex-shrink: 0;
    border-radius: 13px;
    background: var(--badge, var(--surface-3));
    color: var(--solid-ink);
    display: flex;
    align-items: center;
    justify-content: center;
    font-size: 1.05rem;
    font-weight: 800;
  }
  .st-card-blocked .st-card-avatar { opacity: 0.45; }

  .st-card-body { min-width: 0; flex: 1; }
  .st-card-title-row { display: flex; align-items: center; gap: 6px; }

  .st-card-title {
    font-size: 0.88rem;
    font-weight: 700;
    letter-spacing: -0.012em;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .st-card-blocked .st-card-title { color: #8f8f8f; }

  .st-new {
    flex-shrink: 0;
    padding: 1px 6px;
    border-radius: 4px;
    background: #fff8cc;
    border: 1px solid #ecdc7a;
    color: #7a6b00;
    font-size: 0.55rem;
    font-weight: 800;
    letter-spacing: 0.05em;
    text-transform: uppercase;
  }

  .st-card-meta {
    display: flex;
    align-items: center;
    gap: 6px;
    margin-top: 4px;
    font-size: 0.7rem;
    color: var(--text-3);
    font-weight: 600;
  }
  .st-sep { width: 3px; height: 3px; border-radius: 50%; background: #cfcfcf; }

  .st-card-blocked-note {
    margin-top: 4px;
    font-size: 0.7rem;
    color: #a06a2c;
    line-height: 1.35;
  }

  .st-get {
    flex-shrink: 0;
    padding: 8px 16px;
    border: none;
    border-radius: 999px;
    font-size: 0.75rem;
    font-weight: 800;
  }
  .st-get-go { background: var(--accent); color: var(--accent-ink); cursor: pointer; }
  .st-get-go:disabled { opacity: 0.5; cursor: default; }
  .st-get-owned { background: var(--surface-2); color: var(--text-2); }
  .st-get-off { background: var(--surface-2); color: #b4b4b4; }

  /* ── Empty and footer ─────────────────────────── */
  .st-empty { padding: 34px 20px; text-align: center; }
  .st-empty-title { margin: 0; font-size: 0.92rem; font-weight: 800; }
  .st-empty-desc {
    margin: 7px 0 15px;
    font-size: 0.78rem;
    line-height: 1.45;
    color: var(--text-3);
  }

  .st-btn {
    padding: 8px 16px;
    border: none;
    border-radius: 9px;
    font-size: 0.78rem;
    font-weight: 800;
    cursor: pointer;
  }
  .st-btn-primary { background: var(--accent); color: var(--accent-ink); }

  .st-ranking {
    margin: 20px 0 0;
    font-size: 0.68rem;
    color: var(--text-3);
    text-align: center;
  }

  .st-tail { height: 20px; }

  @media (min-width: 700px) {
    .st-list {
      display: grid;
      grid-template-columns: repeat(auto-fill, minmax(300px, 1fr));
      gap: 10px;
    }
  }
</style>
