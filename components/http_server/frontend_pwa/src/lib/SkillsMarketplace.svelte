<script>
  import { skillTypeLabel } from "./colors.js";
  import Eyes from "./Eyes.svelte";
  import {
    listSkills,
    getSkillManifest,
    listRobotSkills,
    assignSkill,
    removeSkill,
  } from "./marketplaceApi.js";

  let { onNavigate } = $props();

  /* Cover art is the category, drawn once: a paw for skills that move the
   * robot, a spark for behaviours. Inline so the store fetches nothing. */
  const GLYPH_PAW = '<svg viewBox="0 0 24 24" fill="currentColor"><circle cx="6.5" cy="9" r="2.2"/><circle cx="17.5" cy="9" r="2.2"/><circle cx="9.5" cy="5" r="2.2"/><circle cx="14.5" cy="5" r="2.2"/><path d="M12 10.5c-3.2 0-6 2.6-6 5.3 0 1.6 1.2 2.7 2.8 2.7 1.2 0 2-.6 3.2-.6s2 .6 3.2.6c1.6 0 2.8-1.1 2.8-2.7 0-2.7-2.8-5.3-6-5.3z"/></svg>';
  const GLYPH_SPARK = '<svg viewBox="0 0 24 24" fill="currentColor"><path d="M12 2.5l1.9 5.6 5.6 1.9-5.6 1.9L12 17.5l-1.9-5.6L4.5 10l5.6-1.9z"/><path d="M19 15l.9 2.6 2.6.9-2.6.9L19 22l-.9-2.6-2.6-.9 2.6-.9z" opacity=".8"/><path d="M4.5 15.5l.7 1.8 1.8.7-1.8.7-.7 1.8-.7-1.8-1.8-.7 1.8-.7z" opacity=".6"/></svg>';
  const GLYPH_BOX = '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linejoin="round"><path d="M12 3l8 4.5v9L12 21l-8-4.5v-9z"/><path d="M4 7.5l8 4.5 8-4.5M12 12v9"/></svg>';
  /* One vocabulary everywhere: the filter chips, the legend and the pills on
   * the cards all say MoveSkill / AISkill. */
  function typeLabel(type) {
    const t = (type || "").toLowerCase();
    return SKILL_TYPES.find((k) => k.key === t)?.label || skillTypeLabel(type);
  }
  function coverGlyph(type) {
    const t = (type || "").toLowerCase();
    return t === "wasm" ? GLYPH_PAW : t === "awa" ? GLYPH_SPARK : GLYPH_BOX;
  }

  // ── Known skill types for filter chips ────────────────────────
  const SKILL_TYPES = [
    { key: "awa",  label: "AISkill" },
    { key: "wasm", label: "MoveSkill" },
    // { key: "type3", label: "Type 3" },
    // { key: "type4", label: "Type 4" },
  ];

  // ── State ────────────────────────────────────────────────────
  let marketplaceSkills = $state([]);
  let robotSkills = $state([]);
  let loading = $state(true);
  let error = $state("");

  // Search & filter
  let searchQuery = $state("");
  let activeFilters = $state([]); // array of skill type keys e.g. ["awa", "wasm"]

  /* Sorting. A store with no sort reads as a pile; a store with a sort reads
     as a catalogue. Kept as a plain <select> rather than a custom dropdown —
     it is one element, it is keyboard- and screen-reader-correct for free, and
     on a phone the OS picker is better than anything hand-rolled. */
  const SORTS = [
    { key: "name",      label: "Name (A–Z)" },
    { key: "name_desc", label: "Name (Z–A)" },
    { key: "price_asc", label: "Price (low to high)" },
    { key: "price_desc",label: "Price (high to low)" },
    { key: "type",      label: "Type" },
    { key: "owned",     label: "Owned first" },
  ];
  let sortBy = $state("name");

  // Detail view
  let detailSkill = $state(null);
  let detailManifest = $state(null);
  let detailLoading = $state(false);

  // Action in flight
  let actionInFlight = $state(null);
  /* Separate from `error`, which replaces the whole list with a Retry panel.
     A failed subscribe must not hide the catalogue you were looking at. */
  let actionError = $state("");

  /* ── Derived: what the list shows ─────────────────────────────
   * Filter and sort in ONE pass over one array. Nothing intermediate is kept
   * in state, so the only copies alive are the source list and the slice on
   * screen — which matters on a device where the whole app shares the
   * browser with a robot's control page. */
  let filteredSkills = $derived.by(() => {
    let skills = marketplaceSkills;

    // Filter by search
    if (searchQuery.trim()) {
      const q = searchQuery.toLowerCase();
      skills = skills.filter(s =>
        (s.title || "").toLowerCase().includes(q) ||
        (s.skill_type || "").toLowerCase().includes(q) ||
        (s.description || "").toLowerCase().includes(q)
      );
    }

    // Filter by type
    if (activeFilters.length > 0) {
      skills = skills.filter(s =>
        activeFilters.includes((s.skill_type || "").toLowerCase())
      );
    }

    // Sort last, on the already-narrowed set.
    const byName = (a, b) => (a.title || "").localeCompare(b.title || "");
    const sorted = [...skills];
    switch (sortBy) {
    case "name_desc":  sorted.sort((a, b) => byName(b, a)); break;
    case "price_asc":  sorted.sort((a, b) => skillPrice(a) - skillPrice(b) || byName(a, b)); break;
    case "price_desc": sorted.sort((a, b) => skillPrice(b) - skillPrice(a) || byName(a, b)); break;
    case "type":       sorted.sort((a, b) =>
                         (a.skill_type || "").localeCompare(b.skill_type || "") || byName(a, b)); break;
    case "owned":      sorted.sort((a, b) =>
                         (isAssigned(b.id) - isAssigned(a.id)) || byName(a, b)); break;
    default:           sorted.sort(byName);
    }
    return sorted;
  });

  let ownedCount = $derived(
    marketplaceSkills.filter((s) => isAssigned(s.id)).length);

  // ── Fetch ────────────────────────────────────────────────────
  async function fetchMarketplace() {
    loading = true;
    error = "";
    try {
      marketplaceSkills = await listSkills();
    } catch (e) {
      error = e.message || "Failed to load marketplace";
      marketplaceSkills = [];
    }
    loading = false;
  }

  async function fetchRobotSkills() {
    try {
      robotSkills = await listRobotSkills();
    } catch { /* ignore */ }
  }

  async function refreshAll() {
    await Promise.all([fetchMarketplace(), fetchRobotSkills()]);
  }

  $effect(() => { refreshAll(); });

  // ── Helpers ──────────────────────────────────────────────────
  function isAssigned(skillId) {
    return robotSkills.some(s => s.skill_id === skillId);
  }

  /**
   * Extract the author/developer name from available data.
   *
   * Priority:
   *   1. `detailManifest.author` (if manifest was fetched and has it)
   *   2. Parse from skill ID: everything before `~` (e.g. "haris_dev" from "haris_dev~amazon"),
   *      with underscores replaced by spaces and title-cased.
   *   3. Fall back to "Unknown".
   */
  function extractAuthor(skill) {
    // 1. Try manifest
    if (detailManifest?.author) return detailManifest.author;

    // 2. Parse from skill ID (developer namespace before ~)
    if (skill?.id) {
      const parts = skill.id.split("~");
      if (parts.length >= 2 && parts[0]) {
        return parts[0]
          .replace(/_/g, " ")
          .replace(/\b\w/g, (c) => c.toUpperCase());
      }
    }

    return "Unknown";
  }

  function toggleFilter(key) {
    if (activeFilters.includes(key)) {
      activeFilters = activeFilters.filter(f => f !== key);
    } else {
      activeFilters = [...activeFilters, key];
    }
  }

  // ── Subscribe / Refund ────────────────────────────────────────
  /* Both of these used to swallow the failure into console.error. The robot
     was answering 405 to every POST and DELETE here — its handler table had
     filled up and the marketplace's write routes never registered — and the
     screen's only tell was that the button went back to how it looked before.
     A write that did not happen has to say so. */
  async function handleSubscribe(skillId) {
    actionInFlight = skillId;
    actionError = "";
    try {
      await assignSkill(skillId);
      // Only refresh robot skills — marketplace listing hasn't changed
      await fetchRobotSkills();
    } catch (e) {
      console.error("Subscribe failed:", e);
      actionError = `Could not subscribe — ${e.message || e}`;
    }
    actionInFlight = null;
  }

  async function handleRefund(skillId) {
    actionInFlight = skillId;
    actionError = "";
    try {
      await removeSkill(skillId);
      // Only refresh robot skills — marketplace listing hasn't changed
      await fetchRobotSkills();
    } catch (e) {
      console.error("Refund failed:", e);
      actionError = `Could not refund — ${e.message || e}`;
    }
    actionInFlight = null;
  }

  // ── Detail ────────────────────────────────────────────────────
  async function openDetail(skill) {
    detailSkill = skill;
    detailManifest = null;
    detailLoading = true;
    try {
      detailManifest = await getSkillManifest(skill.id);
    } catch {
      detailManifest = null;
    }
    detailLoading = false;
  }

  function closeDetail() {
    detailSkill = null;
    detailManifest = null;
  }

  /*
   * TODO: Checkout flow (future)
   * When skills are monetized, each skill will have a `price` field (e.g. { amount: 4.99, currency: "USD" })
   * and a `checkout_url` field pointing to the payment page.
   *
   * The checkout button should:
   *   1. Call an API endpoint to generate a checkout session: POST /v1/marketplace/skills/{id}/checkout
   *   2. Receive a checkout URL from the response
   *   3. Copy the URL to clipboard using: navigator.clipboard.writeText(url)
   *   4. Show a toast/alert: "Checkout link copied! Open it in your browser to complete payment."
   *
   * Example implementation:
   *
   *   async function handleCheckout(skillId) {
   *     try {
   *       const res = await fetch(`/v1/marketplace/skills/${skillId}/checkout`, { method: "POST" });
   *       const data = await res.json();
   *       await navigator.clipboard.writeText(data.checkout_url);
   *       // Show success toast
   *     } catch (e) {
   *       console.error("Checkout failed:", e);
   *     }
   *   }
   *
   * The checkout button should be placed next to the Subscribe button on the detail page
   * and on the marketplace card, styled as a secondary action.
   */

  /**
   * Placeholder: format a price for display.
   * @param {{ amount: number, currency: string }} price
   * @returns {string} e.g. "$4.99"
   */
  function formatPrice(price) {
    if (!price) return null;
    const symbol = price.currency === "USD" ? "$" : price.currency;
    return `${symbol}${price.amount.toFixed(2)}`;
  }

  // ── Demo pricing + fake checkout ─────────────────────────────
  // NOTE: cosmetic only. The API has no price yet, so prices are derived
  // deterministically from the skill id. Replace skillPrice() and confirmPay()
  // with the real pricing + payment integration when ready.
  const DEMO_PRICES = [0.99, 1.99, 9.99];
  function skillPrice(skill) {
    const id = skill?.id || skill?.title || "";
    let h = 0;
    for (let i = 0; i < id.length; i++) h = (h * 31 + id.charCodeAt(i)) >>> 0;
    return DEMO_PRICES[h % DEMO_PRICES.length];
  }
  function priceLabel(skill) { return `$${skillPrice(skill).toFixed(2)}`; }

  // Payment sheet state
  let payingSkill = $state(null);  // skill being "purchased"
  let payStage = $state("form");   // "form" | "processing" | "success"
  let payOrderId = $state("");
  let payMethod = $state("card");

  function openPay(skill) {
    payingSkill = skill;
    payStage = "form";
    payMethod = "card";
  }
  function closePay() {
    payingSkill = null;
    payStage = "form";
  }
  function confirmPay() {
    if (payStage === "processing") return;
    payStage = "processing";
    const skill = payingSkill;
    // Simulate payment processing, then actually subscribe so the robot demo works.
    setTimeout(async () => {
      try {
        await assignSkill(skill.id);
        await fetchRobotSkills();
      } catch (e) {
        console.error("Subscribe failed:", e);
      }
      payOrderId = "#MD-" + Math.floor(100000 + Math.random() * 899999);
      payStage = "success";
    }, 1900);
  }
</script>

<div class="marketplace-root">
  <!-- Header -->
  <header class="mp-header">
    <button class="back-btn" onclick={() => detailSkill ? closeDetail() : onNavigate("back")} aria-label="Back">
      <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round">
        <line x1="19" y1="12" x2="5" y2="12"/>
        <polyline points="12 19 5 12 12 5"/>
      </svg>
    </button>
    <div class="mp-heading">
      <span class="mp-eyebrow">{detailSkill ? skillTypeLabel(detailSkill.skill_type) : "MangDang marketplace"}</span>
      <h2 class="mp-title">{detailSkill ? detailSkill.title : "Skill Store"}</h2>
    </div>
    {#if !detailSkill}
      <button class="refresh-btn" onclick={refreshAll} aria-label="Refresh">
        <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round">
          <polyline points="23 4 23 10 17 10"/>
          <path d="M20.49 15a9 9 0 1 1-2.12-9.36L23 10"/>
        </svg>
      </button>
    {:else}
      <div style="width:38px"></div>
    {/if}
  </header>

  <!-- ═══ DETAIL VIEW ═══ -->
  {#if detailSkill}
    <div class="mp-detail">
      {#if detailLoading}
        <p class="mp-empty">Loading details…</p>
      {:else if detailManifest}
        <div class="mp-detail-hero shell">
          <div class="core mp-detail-core">
            {#if detailManifest.icon_url}
              <img src={detailManifest.icon_url} alt={detailManifest.name} class="mp-detail-icon" />
            {:else}
              <span class="mp-cover mp-cover-lg mp-cover-{(detailSkill.skill_type || '').toLowerCase()}">
                {@html coverGlyph(detailSkill.skill_type)}
              </span>
            {/if}
            <div class="mp-detail-hero-info">
              <h3 class="mp-detail-name">{detailManifest.name || detailSkill.title}</h3>
              <p class="mp-detail-author">by {extractAuthor(detailSkill)} · v{detailManifest.version || detailSkill.current_version || "1.0"}</p>
              <div class="mp-detail-badges">
                <span class="mp-type-pill mp-type-{(detailSkill.skill_type || '').toLowerCase()}">{typeLabel(detailSkill.skill_type)}</span>
                {#if isAssigned(detailSkill.id)}<span class="mp-owned-pill">In your library</span>{/if}
              </div>
            </div>
          </div>
        </div>

        {#if detailManifest.description}
          <p class="mp-detail-desc">{detailManifest.description}</p>
        {/if}

        {#if detailManifest.capabilities?.length}
          <div class="mp-detail-section">
            <h4 class="mp-detail-section-title">Uses</h4>
            <div class="mp-detail-chips">
              {#each detailManifest.capabilities as cap}<span class="mp-chip">{cap}</span>{/each}
            </div>
          </div>
        {/if}

        {#if detailManifest.tags?.length}
          <div class="mp-detail-section">
            <h4 class="mp-detail-section-title">Tags</h4>
            <div class="mp-detail-chips">
              {#each detailManifest.tags as tag}<span class="mp-chip mp-chip-tag">#{tag}</span>{/each}
            </div>
          </div>
        {/if}

        {#if detailManifest.readme}
          <div class="mp-detail-section">
            <h4 class="mp-detail-section-title">About</h4>
            <div class="mp-readme"><p class="mp-readme-text">{detailManifest.readme}</p></div>
          </div>
        {/if}

        <div class="mp-detail-actions">
          <div class="mp-detail-price">
            <span class="mp-detail-price-label">Price</span>
            <span class="mp-detail-price-value">{priceLabel(detailSkill)}</span>
          </div>
          {#if isAssigned(detailSkill.id)}
            <button
              class="mp-btn mp-btn-ghost mp-btn-lg"
              disabled={actionInFlight === detailSkill.id}
              onclick={() => handleRefund(detailSkill.id)}
            >
              {actionInFlight === detailSkill.id ? "Processing…" : "Refund"}
            </button>
          {:else}
            <button class="mp-btn mp-btn-buy mp-btn-lg" onclick={() => openPay(detailSkill)}>
              Get skill
              <span class="mp-btn-disc">
                <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.8" stroke-linecap="round" stroke-linejoin="round"><line x1="5" y1="12" x2="19" y2="12"/><polyline points="12 5 19 12 12 19"/></svg>
              </span>
            </button>
          {/if}
        </div>
      {:else}
        <p class="mp-empty">Could not load skill details.</p>
      {/if}
    </div>

  {:else}
    <!-- ═══ BROWSE VIEW ═══ -->
    <div class="mp-browse">
      {#if !searchQuery && activeFilters.length === 0 && !loading && !error && marketplaceSkills.length}
        <div class="mp-hero shell">
          <div class="core mp-hero-core">
            <div class="mp-hero-art">
              <span class="eyes-blink"><Eyes size={30} /></span>
            </div>
            <div class="mp-hero-text">
              <h3>Teach your pupper new tricks</h3>
              <p>{marketplaceSkills.length} skills{ownedCount ? ` · ${ownedCount} in your library` : ""}. Bought skills install over Wi‑Fi in seconds.</p>
            </div>
          </div>
        </div>
      {/if}

      <div class="mp-search-box">
        <svg class="mp-search-icon" width="17" height="17" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round">
          <circle cx="11" cy="11" r="8"/>
          <line x1="21" y1="21" x2="16.65" y2="16.65"/>
        </svg>
        <input type="text" class="mp-search-input" placeholder="Search skills…" bind:value={searchQuery} />
        {#if searchQuery}
          <button class="mp-search-clear" onclick={() => searchQuery = ""} aria-label="Clear search">
            <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.6" stroke-linecap="round" stroke-linejoin="round">
              <line x1="18" y1="6" x2="6" y2="18"/><line x1="6" y1="6" x2="18" y2="18"/>
            </svg>
          </button>
        {/if}
      </div>

      <div class="mp-toolbar">
        <div class="mp-chiprow">
          <button class="mp-filter-chip" class:active={activeFilters.length === 0} onclick={() => (activeFilters = [])}>All</button>
          {#each SKILL_TYPES as type}
            <button class="mp-filter-chip" class:active={activeFilters.includes(type.key)} onclick={() => toggleFilter(type.key)}>
              <span class="mp-filter-dot mp-dot-{type.key}"></span>{type.label}
            </button>
          {/each}
        </div>
        <label class="mp-sort">
          <svg width="13" height="13" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round"><line x1="4" y1="6" x2="20" y2="6"/><line x1="7" y1="12" x2="17" y2="12"/><line x1="10" y1="18" x2="14" y2="18"/></svg>
          <select class="mp-sort-select" bind:value={sortBy} aria-label="Sort">
            {#each SORTS as o}<option value={o.key}>{o.label}</option>{/each}
          </select>
        </label>
      </div>

      <p class="mp-legend">
        <b>MoveSkill</b> downloads onto the robot and makes it move ·
        <b>AISkill</b> runs as a behaviour you switch on
      </p>

      {#if actionError}
        <div class="mp-action-error">
          <span>{actionError}</span>
          <button class="mp-action-error-x" onclick={() => actionError = ""}>✕</button>
        </div>
      {/if}

      <div class="mp-content">
        {#if loading}
          <p class="mp-empty">Loading marketplace…</p>
        {:else if error}
          <div class="mp-error">
            <p>{error}</p>
            <button class="mp-btn mp-btn-ghost" onclick={fetchMarketplace}>Retry</button>
          </div>
        {:else if filteredSkills.length === 0}
          <div class="mp-empty-state">
            {#if searchQuery || activeFilters.length > 0}
              <p class="mp-empty-title">No matches</p>
              <p class="mp-empty-desc">Nothing here matches “{searchQuery || SKILL_TYPES.find(t => activeFilters.includes(t.key))?.label}”.</p>
              <button class="mp-btn mp-btn-ghost" onclick={() => { searchQuery = ""; activeFilters = []; }}>Clear filters</button>
            {:else}
              <p class="mp-empty-title">The store is empty</p>
              <p class="mp-empty-desc">No skills have been published yet.</p>
            {/if}
          </div>
        {:else}
          <p class="mp-count">
            <b>{filteredSkills.length}</b> {filteredSkills.length === 1 ? "skill" : "skills"}
            {#if filteredSkills.length !== marketplaceSkills.length}of {marketplaceSkills.length}{/if}
          </p>

          <ul class="mp-grid">
            {#each filteredSkills as skill (skill.id)}
              <li class="mp-card" class:mp-card-owned={isAssigned(skill.id)}>
                <button class="mp-card-main" onclick={() => openDetail(skill)}>
                  <span class="mp-cover mp-cover-{(skill.skill_type || '').toLowerCase()}">
                    {@html coverGlyph(skill.skill_type)}
                  </span>
                  <span class="mp-card-text">
                    <span class="mp-card-title">{skill.title || "Untitled"}</span>
                    <span class="mp-card-sub">{extractAuthor(skill)} · v{skill.current_version || "1.0"}</span>
                    {#if skill.description}<span class="mp-card-desc">{skill.description}</span>{/if}
                  </span>
                </button>
                <div class="mp-card-foot">
                  <span class="mp-type-pill mp-type-{(skill.skill_type || '').toLowerCase()}">{typeLabel(skill.skill_type)}</span>
                  <span class="mp-foot-spacer"></span>
                  {#if isAssigned(skill.id)}
                    <span class="mp-owned-pill">
                      <svg width="11" height="11" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="3" stroke-linecap="round" stroke-linejoin="round"><polyline points="4 12.5 10 18 20 6"/></svg>
                      Owned
                    </span>
                    <button class="mp-btn mp-btn-ghost" disabled={actionInFlight === skill.id} onclick={() => handleRefund(skill.id)}>
                      {actionInFlight === skill.id ? "…" : "Refund"}
                    </button>
                  {:else}
                    <button class="mp-btn mp-btn-buy" onclick={() => openPay(skill)}>
                      Get <span class="mp-btn-price">{priceLabel(skill)}</span>
                    </button>
                  {/if}
                </div>
              </li>
            {/each}
          </ul>
        {/if}
      </div>
    </div>
  {/if}

  <!-- ═══ PAYMENT SHEET (cosmetic demo — wire real payment later) ═══ -->
  {#if payingSkill}
    <div class="pay-scrim" onclick={closePay} role="presentation"></div>
    <div class="pay-sheet">
      <div class="pay-grab"></div>

      {#if payStage === "success"}
        <div class="pay-success">
          <div class="pay-check">
            <svg width="34" height="34" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="3" stroke-linecap="round" stroke-linejoin="round"><polyline points="4 12.5 10 18 20 6"/></svg>
          </div>
          <h3 class="pay-success-title">It's yours</h3>
          <p class="pay-success-sub">{payingSkill.title} is installing on your pupper.</p>
          <div class="pay-receipt">
            <div class="pay-receipt-row"><span>Item</span><b>{payingSkill.title}</b></div>
            <div class="pay-receipt-row"><span>Order</span><b>{payOrderId}</b></div>
            <div class="pay-receipt-row"><span>Paid</span><b>{priceLabel(payingSkill)}</b></div>
            <div class="pay-receipt-row"><span>Status</span><b class="pay-ok">Installed</b></div>
          </div>
          <button class="pay-confirm" onclick={closePay}>Start using it</button>
        </div>
      {:else}
        <h3 class="pay-title">Checkout</h3>
        <p class="pay-sub">One tap, and it's on the robot.</p>

        <div class="pay-order">
          <span class="mp-cover mp-cover-sm mp-cover-{(payingSkill.skill_type || '').toLowerCase()}">{@html coverGlyph(payingSkill.skill_type)}</span>
          <div class="pay-order-info">
            <span class="pay-order-name">{payingSkill.title}</span>
            <span class="pay-order-type">{typeLabel(payingSkill.skill_type)} · by {extractAuthor(payingSkill)}</span>
          </div>
          <span class="pay-order-amt">{priceLabel(payingSkill)}</span>
        </div>

        <div class="pay-methods">
          <button class="pay-method" class:active={payMethod === "card"} onclick={() => payMethod = "card"}>Card</button>
          <button class="pay-method" class:active={payMethod === "apple"} onclick={() => payMethod = "apple"}>Apple Pay</button>
          <button class="pay-method" class:active={payMethod === "gpay"} onclick={() => payMethod = "gpay"}>Google Pay</button>
        </div>

        {#if payMethod === "card"}
          <div class="pay-field">
            <label for="pay-cc">Card number</label>
            <input id="pay-cc" type="text" value="4242 4242 4242 4242" inputmode="numeric" />
          </div>
          <div class="pay-field-row">
            <div class="pay-field"><label for="pay-exp">Expiry</label><input id="pay-exp" type="text" value="09/28" /></div>
            <div class="pay-field"><label for="pay-cvc">CVC</label><input id="pay-cvc" type="text" value="123" inputmode="numeric" /></div>
          </div>
          <div class="pay-field">
            <label for="pay-name">Name on card</label>
            <input id="pay-name" type="text" value="Mang Dang" />
          </div>
        {:else}
          <div class="pay-wallet">
            <span class="pay-wallet-label">{payMethod === "apple" ? "Apple Pay" : "Google Pay"} selected</span>
            <span class="pay-wallet-hint">Confirm with a single tap below.</span>
          </div>
        {/if}

        <button class="pay-confirm" disabled={payStage === "processing"} onclick={confirmPay}>
          {#if payStage === "processing"}
            <span class="pay-spinner"></span> Processing…
          {:else}
            Pay {priceLabel(payingSkill)}
          {/if}
        </button>
        <p class="pay-secure">Payment is handled by the MPX marketplace. You can refund a skill from your library at any time.</p>
      {/if}
    </div>
  {/if}
</div>

<style>
  /* ═══ Frame ═══════════════════════════════════════════════════ */
  .marketplace-root {
    display: flex;
    flex-direction: column;
    height: 100%;
    background: var(--bg);
    position: relative;
  }
  .mp-header {
    display: flex;
    align-items: center;
    gap: 10px;
    padding: 8px 14px 6px;
    flex-shrink: 0;
  }
  .back-btn, .refresh-btn {
    width: 38px; height: 38px; border-radius: 50%;
    border: 1px solid var(--line);
    background: var(--surface-1);
    color: var(--text-2);
    display: inline-flex; align-items: center; justify-content: center;
    cursor: pointer;
    box-shadow: inset 0 1px 0 var(--highlight);
    flex-shrink: 0;
    transition: color 200ms var(--ease), transform 200ms var(--ease);
  }
  .back-btn:hover, .refresh-btn:hover { color: var(--text-1); }
  .back-btn:active, .refresh-btn:active { transform: scale(0.94); }
  .mp-heading { flex: 1; min-width: 0; display: flex; flex-direction: column; }
  .mp-eyebrow {
    font-size: 10px; font-weight: 700; letter-spacing: 0.2em; text-transform: uppercase;
    color: var(--accent-ui);
  }
  .mp-title {
    font-size: 22px; font-weight: 700; letter-spacing: -0.01em; color: var(--text-1);
    line-height: 1.1; overflow: hidden; text-overflow: ellipsis; white-space: nowrap;
  }

  .mp-browse { flex: 1; min-height: 0; overflow-y: auto; padding: 4px 14px 24px; }

  /* ═══ Hero ════════════════════════════════════════════════════ */
  .mp-hero { margin: 6px 0 12px; }
  .mp-hero-core {
    display: flex; align-items: center; gap: 14px;
    padding: 14px 16px 14px 14px;
    background:
      radial-gradient(120% 140% at 0% 0%, rgba(255, 230, 5, 0.16), transparent 55%),
      var(--surface-1);
  }
  .mp-hero-art {
    width: 60px; height: 60px; border-radius: 18px; flex-shrink: 0;
    display: inline-flex; align-items: center; justify-content: center;
    background: var(--surface-2);
    border: 1px solid var(--line);
    box-shadow: inset 0 1px 0 var(--highlight), 0 0 30px rgba(255, 230, 5, 0.18);
  }
  .mp-hero-art :global(svg) { filter: drop-shadow(0 0 8px rgba(255, 230, 5, 0.5)); }
  .mp-hero-text h3 { font-size: 17px; font-weight: 700; color: var(--text-1); letter-spacing: -0.01em; }
  .mp-hero-text p { font-size: 12.5px; color: var(--text-3); margin-top: 3px; line-height: 1.4; }

  /* ═══ Search ══════════════════════════════════════════════════ */
  .mp-search-box {
    position: relative;
    display: flex; align-items: center;
    height: 44px;
    border-radius: var(--r-pill);
    background: var(--surface-1);
    border: 1px solid var(--line);
    box-shadow: inset 0 1px 0 var(--highlight);
    color: var(--text-3);
    padding: 0 8px 0 14px;
    transition: border-color 200ms var(--ease), box-shadow 200ms var(--ease);
  }
  .mp-search-box:focus-within { border-color: var(--accent-line); box-shadow: inset 0 1px 0 var(--highlight), 0 0 0 4px var(--accent-soft); }
  .mp-search-icon { flex-shrink: 0; margin-right: 8px; }
  .mp-search-input {
    flex: 1; min-width: 0; height: 100%;
    border: none; outline: none; background: transparent;
    font-family: inherit; font-size: 14.5px; color: var(--text-1);
  }
  .mp-search-input::placeholder { color: var(--text-3); }
  .mp-search-clear {
    width: 28px; height: 28px; border-radius: 50%; border: none;
    background: var(--surface-3); color: var(--text-2);
    display: inline-flex; align-items: center; justify-content: center; cursor: pointer;
  }

  /* ═══ Toolbar ═════════════════════════════════════════════════ */
  .mp-toolbar { display: flex; align-items: center; gap: 8px; margin-top: 10px; }
  .mp-chiprow { display: flex; gap: 6px; overflow-x: auto; flex: 1; min-width: 0; scrollbar-width: none; padding: 2px 0; }
  .mp-chiprow::-webkit-scrollbar { display: none; }
  .mp-filter-chip {
    display: inline-flex; align-items: center; gap: 6px;
    height: 32px; padding: 0 12px;
    border-radius: var(--r-pill);
    border: 1px solid var(--line);
    background: var(--surface-1);
    color: var(--text-2);
    font-family: inherit; font-size: 12.5px; font-weight: 600;
    white-space: nowrap; cursor: pointer; flex-shrink: 0;
    transition: background-color 200ms var(--ease), color 200ms var(--ease), border-color 200ms var(--ease);
  }
  .mp-filter-chip.active { background: var(--solid); color: var(--solid-ink); border-color: var(--solid); }
  .mp-filter-dot { width: 7px; height: 7px; border-radius: 50%; }
  .mp-dot-wasm { background: var(--accent); }
  .mp-dot-awa  { background: #8E7CFF; }
  .mp-filter-chip.active .mp-filter-dot { box-shadow: 0 0 0 2px rgba(255,255,255,0.25); }
  .mp-sort {
    display: inline-flex; align-items: center; gap: 4px; flex-shrink: 0;
    height: 32px; padding: 0 8px 0 10px;
    border-radius: var(--r-pill);
    border: 1px solid var(--line);
    background: var(--surface-1);
    color: var(--text-3);
  }
  .mp-sort-select {
    border: none; background: transparent; color: var(--text-2);
    font-family: inherit; font-size: 12px; font-weight: 600; outline: none;
    max-width: 92px; text-overflow: ellipsis; cursor: pointer;
  }
  .mp-legend { font-size: 11.5px; color: var(--text-3); line-height: 1.45; margin: 10px 2px 0; }
  .mp-legend b { color: var(--text-2); font-weight: 600; }
  .mp-count { font-size: 11px; font-weight: 600; letter-spacing: 0.06em; text-transform: uppercase; color: var(--text-3); margin: 14px 2px 8px; }
  .mp-count b { color: var(--text-1); font-weight: 700; }

  .mp-action-error {
    display: flex; align-items: center; gap: 8px; margin-top: 10px;
    padding: 10px 12px; border-radius: var(--r-sm);
    background: rgba(255, 99, 99, 0.10); border: 1px solid rgba(255, 99, 99, 0.35);
    color: var(--danger); font-size: 12.5px;
  }
  .mp-action-error span { flex: 1; }
  .mp-action-error-x { border: none; background: transparent; color: inherit; cursor: pointer; font-size: 14px; }

  /* ═══ Cards ═══════════════════════════════════════════════════ */
  .mp-grid { list-style: none; display: grid; grid-template-columns: 1fr; gap: 10px; }
  @media (min-width: 700px) { .mp-grid { grid-template-columns: 1fr 1fr; } }
  .mp-card {
    border-radius: var(--r-lg);
    background: var(--surface-1);
    border: 1px solid var(--line);
    box-shadow: inset 0 1px 0 var(--highlight), var(--shadow-card);
    overflow: hidden;
    transition: transform 240ms var(--ease), border-color 240ms var(--ease);
  }
  .mp-card:hover { border-color: var(--line-strong); }
  .mp-card-owned { border-color: var(--accent-line); }
  .mp-card-main {
    width: 100%; display: flex; align-items: flex-start; gap: 14px;
    padding: 14px 14px 10px;
    border: none; background: transparent; text-align: left; cursor: pointer;
    font-family: inherit; color: inherit;
  }
  .mp-card-main:active { opacity: 0.85; }
  .mp-card-text { flex: 1; min-width: 0; display: flex; flex-direction: column; gap: 2px; }
  .mp-card-title { font-size: 16px; font-weight: 700; color: var(--text-1); letter-spacing: -0.01em; }
  .mp-card-sub { font-size: 12px; color: var(--text-3); }
  .mp-card-desc {
    font-size: 13px; color: var(--text-2); line-height: 1.4; margin-top: 4px;
    display: -webkit-box; -webkit-line-clamp: 2; -webkit-box-orient: vertical; overflow: hidden;
  }
  .mp-card-foot {
    display: flex; align-items: center; gap: 8px;
    padding: 10px 14px 12px;
    border-top: 1px solid var(--line);
  }
  .mp-foot-spacer { flex: 1; }

  /* Cover tiles: the category is the artwork. */
  .mp-cover {
    width: 62px; height: 62px; border-radius: 18px; flex-shrink: 0;
    display: inline-flex; align-items: center; justify-content: center;
    background: linear-gradient(145deg, var(--surface-3), var(--surface-2));
    color: var(--text-1);
    border: 1px solid rgba(255, 255, 255, 0.08);
    box-shadow: inset 0 1px 0 rgba(255, 255, 255, 0.25), 0 8px 20px rgba(0, 0, 0, 0.35);
  }
  .mp-cover :global(svg) { width: 54%; height: 54%; }
  .mp-cover-wasm { background: linear-gradient(145deg, #FFF07A 0%, #FFE605 45%, #F0B400 100%); color: #141414; }
  .mp-cover-awa  { background: linear-gradient(145deg, #A99BFF 0%, #7C6CF6 45%, #4D3FD8 100%); color: #FFFFFF; }
  .mp-cover-lg { width: 84px; height: 84px; border-radius: 24px; }
  .mp-cover-sm { width: 46px; height: 46px; border-radius: 14px; }

  .mp-type-pill {
    display: inline-flex; align-items: center; gap: 5px;
    height: 24px; padding: 0 9px; border-radius: var(--r-pill);
    font-size: 10.5px; font-weight: 700; letter-spacing: 0.06em; text-transform: uppercase;
    background: var(--surface-2); color: var(--text-2); border: 1px solid var(--line);
  }
  .mp-type-wasm { color: var(--accent-ui); border-color: var(--accent-line); background: var(--accent-soft); }
  .mp-type-awa  { color: #B0A4FF; border-color: rgba(142, 124, 255, 0.4); background: rgba(142, 124, 255, 0.12); }
  :global([data-theme="light"]) .mp-type-awa { color: #4D3FD8; }
  .mp-owned-pill {
    display: inline-flex; align-items: center; gap: 4px;
    height: 24px; padding: 0 9px; border-radius: var(--r-pill);
    font-size: 11px; font-weight: 700; color: var(--ok);
    background: rgba(69, 212, 131, 0.12); border: 1px solid rgba(69, 212, 131, 0.35);
  }

  /* Buttons */
  .mp-btn {
    display: inline-flex; align-items: center; justify-content: center; gap: 6px;
    height: 34px; padding: 0 14px;
    border-radius: var(--r-pill);
    font-family: inherit; font-size: 13px; font-weight: 700;
    cursor: pointer; white-space: nowrap;
    transition: transform 200ms var(--ease), background-color 200ms var(--ease), color 200ms var(--ease), border-color 200ms var(--ease);
  }
  .mp-btn:active { transform: scale(0.96); }
  .mp-btn:disabled { opacity: 0.5; cursor: default; }
  .mp-btn-buy { border: none; background: var(--accent); color: var(--accent-ink); box-shadow: 0 6px 16px rgba(255, 230, 5, 0.22); }
  .mp-btn-price { font-weight: 800; }
  .mp-btn-ghost { background: transparent; color: var(--text-2); border: 1px solid var(--line-strong); }
  .mp-btn-ghost:hover { color: var(--text-1); }
  .mp-btn-lg { height: 48px; padding: 0 6px 0 22px; font-size: 15px; flex: 1; gap: 12px; }
  .mp-btn-lg.mp-btn-ghost { padding: 0 22px; }
  .mp-btn-disc {
    width: 34px; height: 34px; border-radius: 50%;
    display: inline-flex; align-items: center; justify-content: center;
    background: rgba(20, 20, 20, 0.14);
  }

  /* ═══ Detail ══════════════════════════════════════════════════ */
  .mp-detail { flex: 1; min-height: 0; overflow-y: auto; padding: 6px 14px 28px; }
  .mp-detail-hero { margin-bottom: 14px; }
  .mp-detail-core {
    display: flex; align-items: center; gap: 16px; padding: 16px;
    background: radial-gradient(120% 140% at 0% 0%, rgba(255, 230, 5, 0.12), transparent 55%), var(--surface-1);
  }
  .mp-detail-icon { width: 84px; height: 84px; border-radius: 24px; object-fit: cover; flex-shrink: 0; }
  .mp-detail-hero-info { min-width: 0; }
  .mp-detail-name { font-size: 20px; font-weight: 700; color: var(--text-1); letter-spacing: -0.01em; }
  .mp-detail-author { font-size: 12.5px; color: var(--text-3); margin-top: 2px; }
  .mp-detail-badges { display: flex; gap: 6px; flex-wrap: wrap; margin-top: 10px; }
  .mp-detail-desc { font-size: 15px; color: var(--text-1); line-height: 1.5; margin: 0 2px 16px; }
  .mp-detail-section { margin-bottom: 16px; }
  .mp-detail-section-title {
    font-size: 11px; font-weight: 700; letter-spacing: 0.1em; text-transform: uppercase;
    color: var(--text-3); margin: 0 2px 8px;
  }
  .mp-detail-chips { display: flex; flex-wrap: wrap; gap: 6px; }
  .mp-chip {
    height: 28px; padding: 0 11px; border-radius: var(--r-pill);
    display: inline-flex; align-items: center;
    background: var(--surface-1); border: 1px solid var(--line);
    font-size: 12.5px; font-weight: 600; color: var(--text-2);
  }
  .mp-chip-tag { color: var(--text-3); font-weight: 500; }
  .mp-readme {
    padding: 14px 16px; border-radius: var(--r-md);
    background: var(--surface-1); border: 1px solid var(--line);
    box-shadow: inset 0 1px 0 var(--highlight);
  }
  .mp-readme-text { font-size: 14px; color: var(--text-2); line-height: 1.55; white-space: pre-wrap; }
  .mp-detail-actions {
    position: sticky; bottom: 0;
    display: flex; align-items: center; gap: 12px;
    margin-top: 8px; padding: 12px;
    border-radius: var(--r-lg);
    background: var(--surface-1);
    border: 1px solid var(--line-strong);
    box-shadow: inset 0 1px 0 var(--highlight), var(--shadow-card);
  }
  .mp-detail-price { display: flex; flex-direction: column; padding-left: 6px; }
  .mp-detail-price-label { font-size: 10.5px; font-weight: 700; letter-spacing: 0.08em; text-transform: uppercase; color: var(--text-3); }
  .mp-detail-price-value { font-size: 22px; font-weight: 800; color: var(--text-1); letter-spacing: -0.02em; }

  /* ═══ States ══════════════════════════════════════════════════ */
  .mp-empty { text-align: center; color: var(--text-3); font-size: 13.5px; padding: 40px 0; }
  .mp-error { text-align: center; padding: 30px 0; color: var(--danger); font-size: 13.5px; display: flex; flex-direction: column; align-items: center; gap: 12px; }
  .mp-empty-state { text-align: center; padding: 40px 12px; display: flex; flex-direction: column; align-items: center; gap: 6px; }
  .mp-empty-title { font-size: 16px; font-weight: 700; color: var(--text-1); }
  .mp-empty-desc { font-size: 13px; color: var(--text-3); margin-bottom: 8px; }

  /* ═══ Payment sheet ═══════════════════════════════════════════ */
  .pay-scrim { position: fixed; inset: 0; background: rgba(0, 0, 0, 0.6); z-index: 60; backdrop-filter: blur(4px); }
  .pay-sheet {
    position: fixed; left: 0; right: 0; bottom: 0; z-index: 61;
    max-width: 560px; margin: 0 auto;
    padding: 10px 18px calc(18px + env(safe-area-inset-bottom, 0px));
    border-radius: 28px 28px 0 0;
    background: var(--surface-1);
    border: 1px solid var(--line-strong); border-bottom: none;
    box-shadow: 0 -24px 60px rgba(0, 0, 0, 0.5);
    animation: sheet-up 480ms var(--ease);
  }
  @keyframes sheet-up { from { transform: translateY(24px); opacity: 0; } to { transform: none; opacity: 1; } }
  .pay-grab { width: 40px; height: 4px; border-radius: 2px; background: var(--line-strong); margin: 0 auto 14px; }
  .pay-title { font-size: 20px; font-weight: 700; color: var(--text-1); letter-spacing: -0.01em; }
  .pay-sub { font-size: 13px; color: var(--text-3); margin: 2px 0 14px; }
  .pay-order {
    display: flex; align-items: center; gap: 12px;
    padding: 12px; border-radius: var(--r-md);
    background: var(--surface-2); border: 1px solid var(--line);
  }
  .pay-order-info { flex: 1; min-width: 0; display: flex; flex-direction: column; }
  .pay-order-name { font-size: 15px; font-weight: 700; color: var(--text-1); }
  .pay-order-type { font-size: 12px; color: var(--text-3); margin-top: 2px; }
  .pay-order-amt { font-size: 18px; font-weight: 800; color: var(--text-1); }
  .pay-methods {
    display: flex; gap: 4px; padding: 4px; margin: 14px 0 12px;
    border-radius: var(--r-pill); background: var(--surface-2); border: 1px solid var(--line);
  }
  .pay-method {
    flex: 1; height: 34px; border: none; border-radius: var(--r-pill);
    background: transparent; color: var(--text-3);
    font-family: inherit; font-size: 13px; font-weight: 600; cursor: pointer;
    transition: background-color 200ms var(--ease), color 200ms var(--ease);
  }
  .pay-method.active { background: var(--solid); color: var(--solid-ink); }
  .pay-field { display: flex; flex-direction: column; gap: 5px; margin-bottom: 10px; flex: 1; }
  .pay-field label { font-size: 11px; font-weight: 700; letter-spacing: 0.06em; text-transform: uppercase; color: var(--text-3); }
  .pay-field input {
    height: 44px; padding: 0 14px; border-radius: var(--r-sm);
    background: var(--surface-2); border: 1px solid var(--line);
    color: var(--text-1); font-family: inherit; font-size: 15px; outline: none;
    font-variant-numeric: tabular-nums;
  }
  .pay-field input:focus { border-color: var(--accent-line); }
  .pay-field-row { display: flex; gap: 10px; }
  .pay-wallet {
    padding: 18px; border-radius: var(--r-md); text-align: center;
    background: var(--surface-2); border: 1px dashed var(--line-strong);
    display: flex; flex-direction: column; gap: 4px; margin-bottom: 10px;
  }
  .pay-wallet-label { font-size: 14px; font-weight: 700; color: var(--text-1); }
  .pay-wallet-hint { font-size: 12.5px; color: var(--text-3); }
  .pay-confirm {
    width: 100%; height: 50px; margin-top: 6px;
    border: none; border-radius: var(--r-pill);
    background: var(--accent); color: var(--accent-ink);
    font-family: inherit; font-size: 15.5px; font-weight: 800; cursor: pointer;
    display: inline-flex; align-items: center; justify-content: center; gap: 8px;
    box-shadow: 0 8px 20px rgba(255, 230, 5, 0.25);
    transition: transform 200ms var(--ease);
  }
  .pay-confirm:active { transform: scale(0.98); }
  .pay-confirm:disabled { opacity: 0.7; cursor: default; }
  .pay-spinner { width: 16px; height: 16px; border-radius: 50%; border: 2px solid rgba(20,20,20,0.25); border-top-color: var(--accent-ink); animation: spin 0.8s linear infinite; }
  @keyframes spin { to { transform: rotate(360deg); } }
  .pay-secure { font-size: 11.5px; color: var(--text-3); text-align: center; margin-top: 12px; line-height: 1.45; }
  .pay-success { display: flex; flex-direction: column; align-items: center; text-align: center; padding-top: 6px; }
  .pay-check {
    width: 72px; height: 72px; border-radius: 50%;
    display: inline-flex; align-items: center; justify-content: center;
    background: var(--ok); color: #06210f;
    box-shadow: 0 0 0 10px rgba(69, 212, 131, 0.14);
    margin-bottom: 14px;
  }
  .pay-success-title { font-size: 20px; font-weight: 700; color: var(--text-1); }
  .pay-success-sub { font-size: 13px; color: var(--text-3); margin: 4px 0 14px; }
  .pay-receipt {
    width: 100%; padding: 4px 14px; border-radius: var(--r-md);
    background: var(--surface-2); border: 1px solid var(--line); margin-bottom: 14px;
  }
  .pay-receipt-row { display: flex; justify-content: space-between; padding: 9px 0; font-size: 13.5px; color: var(--text-3); border-bottom: 1px solid var(--line); }
  .pay-receipt-row:last-child { border-bottom: none; }
  .pay-receipt-row b { color: var(--text-1); font-weight: 600; }
  .pay-ok { color: var(--ok) !important; }
</style>
