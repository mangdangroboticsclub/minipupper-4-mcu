<script>
  import { colors, skillTypeColor, skillTypeLabel } from "./colors.js";
  import { detectCurrency, setPreferredCurrency } from "./currency.js";
  import {
    listSkills,
    getSkillManifest,
    listRobotSkills,
    assignSkill,
    removeSkill,
    deploySkill,
    createCheckout,
    getOrder,
  } from "./marketplaceApi.js";

  /**
   * @prop embedded — rendered as a bottom-nav tab (no back button in browse view)
   * @prop onNavigate — ("back" | "skills") => void
   */
  let { onNavigate, embedded = false } = $props();

  const YELLOW = colors.mpx.primary;

  // ── Known skill types for filter chips ────────────────────────
  const SKILL_TYPES = [
    { key: "awa",  label: "AISkill" },
    { key: "wasm", label: "MoveSkill" },
  ];

  // Currencies whose minor units are not 2. Mirrors the gateway's fx module;
  // display_price is already quantized server-side — this only affects how
  // many digits we *show*.
  const MINOR_DIGITS = {
    // zero-decimal
    BIF: 0, CLP: 0, DJF: 0, GNF: 0, IDR: 0, ISK: 0, JPY: 0, KMF: 0, KRW: 0,
    LAK: 0, MGA: 0, PYG: 0, RWF: 0, UGX: 0, UYI: 0, VND: 0, VUV: 0, XAF: 0,
    XOF: 0, XPF: 0,
    // three-decimal
    BHD: 3, IQD: 3, JOD: 3, KWD: 3, LYD: 3, OMR: 3, TND: 3,
  };

  const DISPLAY_CURRENCIES = ["USD", "HKD", "CNY", "EUR", "GBP", "JPY", "SGD"];

  // ── State ────────────────────────────────────────────────────
  let marketplaceSkills = $state([]);
  let robotSkills = $state([]);
  let loading = $state(true);
  let error = $state("");
  let displayCurrency = $state(detectCurrency());

  // Search & filter
  let searchQuery = $state("");
  let activeFilters = $state([]); // array of skill type keys e.g. ["awa", "wasm"]

  // Detail view
  let detailSkill = $state(null);
  let detailManifest = $state(null);
  let detailLoading = $state(false);

  // Action in flight
  let actionInFlight = $state(null);

  // ── Derived: filtered skills ────────────────────────────────
  let filteredSkills = $derived.by(() => {
    let skills = marketplaceSkills;

    if (searchQuery.trim()) {
      const q = searchQuery.toLowerCase();
      skills = skills.filter(s =>
        (s.title || "").toLowerCase().includes(q) ||
        (s.skill_type || "").toLowerCase().includes(q) ||
        (s.description || "").toLowerCase().includes(q)
      );
    }

    if (activeFilters.length > 0) {
      skills = skills.filter(s =>
        activeFilters.includes((s.skill_type || "").toLowerCase())
      );
    }

    return skills;
  });

  let isBrowsingAll = $derived(!searchQuery.trim() && activeFilters.length === 0);

  // Featured: first skill the user doesn't own yet (falls back to first skill)
  let featured = $derived.by(() => {
    if (!isBrowsingAll || marketplaceSkills.length === 0) return null;
    return marketplaceSkills.find((s) => !isAssigned(s.id)) ?? marketplaceSkills[0];
  });

  let ownedCount = $derived(marketplaceSkills.filter((s) => isAssigned(s.id)).length);

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

  async function onCurrencyChange(code) {
    setPreferredCurrency(code);
    displayCurrency = code;
    await fetchMarketplace();
  }

  $effect(() => { refreshAll(); });

  // ── Helpers ──────────────────────────────────────────────────
  function isAssigned(skillId) {
    return robotSkills.some(s => s.skill_id === skillId);
  }

  /**
   * Extract the author/developer name from available data.
   *   1. `detailManifest.author`
   *   2. Developer namespace before `~` in the skill id, title-cased
   *   3. "Unknown"
   */
  function extractAuthor(skill) {
    if (detailManifest?.author && detailSkill?.id === skill?.id) return detailManifest.author;
    if (skill?.id) {
      const parts = skill.id.split("~");
      if (parts.length >= 2 && parts[0]) {
        return parts[0].replace(/_/g, " ").replace(/\b\w/g, (c) => c.toUpperCase());
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
  async function handleSubscribe(skillId) {
    actionInFlight = skillId;
    try {
      await assignSkill(skillId);
      await fetchRobotSkills();
    } catch (e) {
      console.error("Subscribe failed:", e);
    }
    actionInFlight = null;
  }

  async function handleRefund(skillId) {
    actionInFlight = skillId;
    try {
      await removeSkill(skillId);
      await fetchRobotSkills();
    } catch (e) {
      console.error("Refund failed:", e);
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

  /**
   * Format a price for display, e.g. "$4.99", "HK$39.13", "¥500".
   */
  function formatPrice(price) {
    if (!price) return null;
    let amount = price.amount;
    if (typeof price.amount === "string") amount = parseFloat(price.amount);
    if (Number.isNaN(amount)) return price.currency || null;
    try {
      return new Intl.NumberFormat(undefined, {
        style: "currency",
        currency: price.currency,
        minimumFractionDigits: MINOR_DIGITS[price.currency] ?? 2,
        maximumFractionDigits: MINOR_DIGITS[price.currency] ?? 2,
      }).format(amount);
    } catch {
      const prefix = price.currency === "USD" ? "$" : `${price.currency} `;
      return `${prefix}${amount.toFixed(2)}`;
    }
  }

  // ── Pricing from the gateway ──────────────────────────────────
  // Canonical:  price: { amount, currency }   (what is charged)
  // Display:    display_price: { amount, currency }  (quote, optional)
  function skillPrice(skill) {
    return skill?.display_price ?? skill?.price ?? null;
  }
  function priceLabel(skill) {
    const p = skillPrice(skill);
    if (!p || p.amount <= 0) return "Free";
    return formatPrice(p);
  }
  function isFree(skill) { return priceLabel(skill) === "Free"; }

  function canonicalCurrency(skill) {
    return skill?.price?.currency ?? "USD";
  }

  // ── Checkout state (link-out flow — card entry happens on HTTPS) ─
  let checkoutSkill = $state(null);        // skill being purchased
  let checkoutStage = $state("creating");  // creating | awaiting | paid | error
  let checkoutUrl = $state("");
  let checkoutError = $state("");
  let checkoutOrderId = $state("");
  let checkoutPollTimer = null;
  let copied = $state(false);

  function openCheckout(skill) {
    checkoutSkill = skill;
    checkoutStage = "creating";
    checkoutUrl = "";
    checkoutError = "";
    checkoutOrderId = "";
    copied = false;
    handleCheckout(skill);
  }

  function closeCheckout() {
    stopCheckoutPoll();
    checkoutSkill = null;
    checkoutStage = "creating";
  }

  function stopCheckoutPoll() {
    if (checkoutPollTimer) {
      clearInterval(checkoutPollTimer);
      checkoutPollTimer = null;
    }
  }

  /**
   * Start a purchase. Creates an order + hosted checkout session on the
   * gateway, then either finishes immediately (free skill / mock
   * auto-approve) or hands the buyer an HTTPS checkout link to open in
   * their browser (the PWA is HTTP, so card entry happens off-device).
   */
  async function handleCheckout(skill) {
    checkoutStage = "creating";
    checkoutError = "";
    try {
      const data = await createCheckout(skill.id);
      checkoutOrderId = data.order_id || "";
      checkoutUrl = data.checkout_url || "";

      if (data.status === "paid" || data.status === "fulfilled") {
        await finishPurchase(skill);
        return;
      }

      try {
        await navigator.clipboard.writeText(checkoutUrl);
        copied = true;
      } catch { /* clipboard unavailable — user can still copy manually */ }

      checkoutStage = "awaiting";
      startCheckoutPoll(skill);
    } catch (e) {
      console.error("Checkout failed:", e);
      checkoutStage = "error";
      checkoutError = e.message || "Checkout failed";
    }
  }

  function startCheckoutPoll(skill) {
    stopCheckoutPoll();
    checkoutPollTimer = setInterval(async () => {
      try {
        const order = await getOrder(checkoutOrderId);
        if (order.status === "paid" || order.status === "fulfilled") {
          await finishPurchase(skill);
        } else if (order.status === "failed" || order.status === "expired") {
          stopCheckoutPoll();
          checkoutStage = "error";
          checkoutError = `Order ${order.status} — please try again.`;
        }
      } catch { /* transient network hiccup — keep polling */ }
    }, 2500);
  }

  async function finishPurchase(skill) {
    stopCheckoutPoll();
    checkoutStage = "paid";
    await fetchRobotSkills();
    if ((skill.skill_type || "").toLowerCase() === "wasm") {
      try {
        await deploySkill(skill.id);
      } catch (e) {
        console.error("Deploy failed:", e);
      }
    }
  }

  async function copyCheckoutUrl() {
    try {
      await navigator.clipboard.writeText(checkoutUrl);
      copied = true;
      setTimeout(() => (copied = false), 1800);
    } catch { /* ignore */ }
  }

  function openCheckoutUrl() {
    window.open(checkoutUrl, "_blank");
  }

  function initial(skill) {
    return (skill?.title || "?").trim().charAt(0).toUpperCase();
  }
</script>

<div class="marketplace-root" class:embedded style="--yellow: {YELLOW}">
  <!-- Header -->
  <header class="mp-header">
    {#if detailSkill || !embedded}
      <button class="icon-btn" onclick={() => detailSkill ? closeDetail() : onNavigate?.("back")} aria-label="Back">
        <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
          <line x1="19" y1="12" x2="5" y2="12"/><polyline points="12 19 5 12 12 5"/>
        </svg>
      </button>
    {:else}
      <div class="icon-btn spacer"></div>
    {/if}

    <h2 class="mp-title">{detailSkill ? detailSkill.title : "Skill Store"}</h2>

    {#if !detailSkill}
      <select
        class="mp-currency-select"
        value={displayCurrency}
        onchange={(event) => onCurrencyChange(event.currentTarget.value)}
        aria-label="Display currency"
      >
        {#each DISPLAY_CURRENCIES as currency}
          <option value={currency}>{currency}</option>
        {/each}
      </select>
      <button class="icon-btn" class:spinning={loading} onclick={refreshAll} aria-label="Refresh" disabled={loading}>
        <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
          <polyline points="23 4 23 10 17 10"/><path d="M20.49 15a9 9 0 1 1-2.12-9.36L23 10"/>
        </svg>
      </button>
    {:else}
      <div class="icon-btn spacer"></div>
    {/if}
  </header>

  <!-- ═══ DETAIL VIEW ═══ -->
  {#if detailSkill}
    <div class="mp-detail">
      <div class="mp-detail-inner">
        {#if detailLoading}
          <div class="mp-detail-hero">
            <div class="skeleton" style="width:72px;height:72px;border-radius:var(--r-lg)"></div>
            <div class="sk-lines"><div class="skeleton sk-line"></div><div class="skeleton sk-line short"></div></div>
          </div>
          <div class="skeleton" style="height:56px;border-radius:var(--r-md)"></div>
          <div class="skeleton" style="height:90px;border-radius:var(--r-md)"></div>
        {:else if detailManifest}
          <!-- Hero -->
          <div class="mp-detail-hero" style="--tone: {skillTypeColor(detailSkill.skill_type)}">
            {#if detailManifest.icon_url}
              <img src={detailManifest.icon_url} alt={detailManifest.name} class="mp-detail-icon" />
            {:else}
              <div class="mp-detail-icon-placeholder">
                <span>{initial(detailSkill)}</span>
              </div>
            {/if}
            <div class="mp-detail-hero-info">
              <h3 class="mp-detail-name">{detailManifest.name || detailSkill.title}</h3>
              <p class="mp-detail-author">by {extractAuthor(detailSkill)} · v{detailManifest.version || detailSkill.current_version || "1.0"}</p>
              <div class="mp-detail-badges">
                <span class="mp-type-badge" style="--badge-color: {skillTypeColor(detailSkill.skill_type)}">
                  {skillTypeLabel(detailSkill.skill_type)}
                </span>
                {#if isAssigned(detailSkill.id)}
                  <span class="owned-badge">
                    <svg width="11" height="11" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="3.5" stroke-linecap="round" stroke-linejoin="round"><polyline points="4 12.5 10 18 20 6"/></svg>
                    Owned
                  </span>
                {/if}
              </div>
            </div>
          </div>

          <!-- Price + action -->
          <div class="mp-detail-price-row">
            <div class="mp-detail-price-col">
              <span class="mp-detail-price-label">{isFree(detailSkill) ? "Price" : `Price · ${displayCurrency}`}</span>
              <span class="mp-detail-price-value">{priceLabel(detailSkill)}</span>
            </div>
            {#if isAssigned(detailSkill.id)}
              <button
                class="mp-action-btn mp-action-refund"
                disabled={actionInFlight === detailSkill.id}
                onclick={() => handleRefund(detailSkill.id)}
              >
                {actionInFlight === detailSkill.id ? "Processing…" : "Refund"}
              </button>
            {:else}
              <button class="mp-action-btn mp-action-subscribe" onclick={() => openCheckout(detailSkill)}>
                {isFree(detailSkill) ? "Get" : `Buy ${priceLabel(detailSkill)}`}
              </button>
            {/if}
          </div>

          <!-- Description -->
          {#if detailManifest.description}
            <p class="mp-detail-desc">{detailManifest.description}</p>
          {/if}

          <!-- Capabilities -->
          {#if detailManifest.capabilities?.length}
            <div class="mp-detail-section">
              <h4 class="mp-detail-section-title">Capabilities</h4>
              <div class="mp-detail-chips">
                {#each detailManifest.capabilities as cap}
                  <span class="mp-chip">{cap}</span>
                {/each}
              </div>
            </div>
          {/if}

          <!-- Tags -->
          {#if detailManifest.tags?.length}
            <div class="mp-detail-section">
              <h4 class="mp-detail-section-title">Tags</h4>
              <div class="mp-detail-chips">
                {#each detailManifest.tags as tag}
                  <span class="mp-chip mp-chip-tag">#{tag}</span>
                {/each}
              </div>
            </div>
          {/if}

          <!-- README -->
          {#if detailManifest.readme}
            <div class="mp-detail-section">
              <h4 class="mp-detail-section-title">About this skill</h4>
              <div class="mp-readme">
                <p class="mp-readme-text">{detailManifest.readme}</p>
              </div>
            </div>
          {/if}
        {:else}
          <div class="mp-empty-state">
            <div class="empty-glyph err">!</div>
            <p class="mp-empty-title">Couldn't load details</p>
            <p class="mp-empty-desc">The store didn't return a manifest for this skill.</p>
            <button class="mp-retry-btn" onclick={() => openDetail(detailSkill)}>Try again</button>
          </div>
        {/if}
      </div>
    </div>

  {:else}
    <!-- ═══ BROWSE VIEW ═══ -->
    <div class="mp-toolbar">
      <div class="mp-toolbar-inner">
        <div class="mp-search-box">
          <svg class="mp-search-icon" width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
            <circle cx="11" cy="11" r="8"/><line x1="21" y1="21" x2="16.65" y2="16.65"/>
          </svg>
          <input type="search" class="mp-search-input" placeholder="Search skills…" bind:value={searchQuery} />
          {#if searchQuery}
            <button class="mp-search-clear" onclick={() => searchQuery = ""} aria-label="Clear search">
              <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
                <line x1="18" y1="6" x2="6" y2="18"/><line x1="6" y1="6" x2="18" y2="18"/>
              </svg>
            </button>
          {/if}
        </div>

        <div class="mp-filters">
          <button class="mp-filter-chip" class:active={activeFilters.length === 0} onclick={() => activeFilters = []}>All</button>
          {#each SKILL_TYPES as type}
            <button
              class="mp-filter-chip"
              class:active={activeFilters.includes(type.key)}
              style="--chip-color: {skillTypeColor(type.key)}"
              onclick={() => toggleFilter(type.key)}
            >
              <span class="mp-filter-dot" style="background: {skillTypeColor(type.key)}"></span>
              {type.label}
            </button>
          {/each}
        </div>
      </div>
    </div>

    <!-- Content -->
    <div class="mp-content">
      <div class="mp-content-inner">
        {#if loading}
          <div class="skeleton hero-skeleton"></div>
          <div class="mp-grid">
            {#each [0,1,2,3] as _}<div class="skeleton card-skeleton"></div>{/each}
          </div>
        {:else if error}
          <div class="mp-empty-state">
            <div class="empty-glyph err">!</div>
            <p class="mp-empty-title">Store unavailable</p>
            <p class="mp-empty-desc">{error}</p>
            <button class="mp-retry-btn" onclick={fetchMarketplace}>Retry</button>
          </div>
        {:else if filteredSkills.length === 0}
          <div class="mp-empty-state">
            <div class="empty-glyph">
              <svg width="30" height="30" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
                <path d="M6 2 3 6v14a2 2 0 0 0 2 2h14a2 2 0 0 0 2-2V6l-3-4Z"/><path d="M3 6h18"/><path d="M16 10a4 4 0 0 1-8 0"/>
              </svg>
            </div>
            {#if !isBrowsingAll}
              <p class="mp-empty-title">No matches</p>
              <p class="mp-empty-desc">Try a different search or clear the filters.</p>
              <button class="mp-retry-btn" onclick={() => { searchQuery = ""; activeFilters = []; }}>Clear filters</button>
            {:else}
              <p class="mp-empty-title">Shelves are empty</p>
              <p class="mp-empty-desc">New skills land here as developers publish them. Check back soon.</p>
            {/if}
          </div>
        {:else}
          <!-- Featured hero -->
          {#if featured}
            <button class="mp-hero" style="--tone: {skillTypeColor(featured.skill_type)}" onclick={() => openDetail(featured)}>
              <div class="mp-hero-bg" aria-hidden="true"></div>
              <div class="mp-hero-body">
                <span class="mp-hero-kicker">Featured</span>
                <h3 class="mp-hero-title">{featured.title}</h3>
                {#if featured.description}
                  <p class="mp-hero-desc">{featured.description}</p>
                {/if}
                <div class="mp-hero-row">
                  <span class="mp-type-badge dark" style="--badge-color: {skillTypeColor(featured.skill_type)}">{skillTypeLabel(featured.skill_type)}</span>
                  <span class="mp-hero-price">{isAssigned(featured.id) ? "Owned" : priceLabel(featured)}</span>
                </div>
              </div>
              <div class="mp-hero-glyph" aria-hidden="true">{initial(featured)}</div>
            </button>
          {/if}

          <div class="mp-section-head">
            <h3 class="mp-section-title">{isBrowsingAll ? "All skills" : "Results"}</h3>
            <span class="mp-section-count">{filteredSkills.length} {filteredSkills.length === 1 ? "skill" : "skills"}{isBrowsingAll && ownedCount ? ` · ${ownedCount} owned` : ""}</span>
          </div>

          <div class="mp-grid">
            {#each filteredSkills as skill (skill.id)}
              {@const owned = isAssigned(skill.id)}
              <article class="mp-card" class:owned style="--tone: {skillTypeColor(skill.skill_type)}">
                <button class="mp-card-main" onclick={() => openDetail(skill)}>
                  <div class="mp-card-top">
                    <div class="mp-card-icon"><span>{initial(skill)}</span></div>
                    {#if owned}
                      <span class="owned-badge" title="In your library">
                        <svg width="11" height="11" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="3.5" stroke-linecap="round" stroke-linejoin="round"><polyline points="4 12.5 10 18 20 6"/></svg>
                      </span>
                    {:else}
                      <span class="price-pill" class:free={isFree(skill)}>{priceLabel(skill)}</span>
                    {/if}
                  </div>

                  <h3 class="mp-card-title">{skill.title}</h3>
                  <p class="mp-card-author">{extractAuthor(skill)}</p>
                  {#if skill.description}
                    <p class="mp-card-desc">{skill.description}</p>
                  {/if}

                  <div class="mp-card-foot">
                    <span class="mp-type-badge" style="--badge-color: {skillTypeColor(skill.skill_type)}">{skillTypeLabel(skill.skill_type)}</span>
                    <span class="mp-card-version">v{skill.current_version || "1.0"}</span>
                  </div>
                </button>

                <div class="mp-card-actions">
                  {#if owned}
                    <button
                      class="mp-card-btn mp-card-btn-refund"
                      disabled={actionInFlight === skill.id}
                      onclick={() => handleRefund(skill.id)}
                    >
                      {actionInFlight === skill.id ? "…" : "Refund"}
                    </button>
                  {:else}
                    <button class="mp-card-btn mp-card-btn-sub" onclick={() => openCheckout(skill)}>
                      {isFree(skill) ? "Get" : "Buy"}
                    </button>
                  {/if}
                </div>
              </article>
            {/each}
          </div>

          {#if ownedCount > 0}
            <button class="library-link" onclick={() => onNavigate?.("skills")}>
              <span>Manage your {ownedCount} owned {ownedCount === 1 ? "skill" : "skills"}</span>
              <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><path d="M9 18l6-6-6-6"/></svg>
            </button>
          {/if}
        {/if}
      </div>
    </div>
  {/if}

  <!-- ═══ CHECKOUT SHEET (link-out flow — card entry happens on HTTPS) ═══ -->
  {#if checkoutSkill}
    <div class="pay-scrim" onclick={closeCheckout} role="presentation"></div>
    <div class="pay-sheet" role="dialog" aria-modal="true" aria-label="Checkout">
      <div class="pay-grab"></div>

      {#if checkoutStage === "paid"}
        <div class="pay-success">
          <div class="pay-check">
            <svg width="38" height="38" viewBox="0 0 24 24" fill="none" stroke="#fff" stroke-width="3" stroke-linecap="round" stroke-linejoin="round">
              <polyline points="4 12.5 10 18 20 6"/>
            </svg>
          </div>
          <h3 class="pay-success-title">{isFree(checkoutSkill) ? "Added to your library" : "Payment successful"}</h3>
          <p class="pay-success-sub">{checkoutSkill.title} is ready for your Pupper.</p>
          <div class="pay-receipt">
            <div class="pay-receipt-row"><span>Item</span><b>{checkoutSkill.title}</b></div>
            {#if checkoutOrderId}<div class="pay-receipt-row"><span>Order ID</span><b>{checkoutOrderId}</b></div>{/if}
            <div class="pay-receipt-row"><span>Paid</span><b>{priceLabel(checkoutSkill)}</b></div>
            <div class="pay-receipt-row"><span>Status</span><b class="pay-ok">Installed ✓</b></div>
          </div>
          <div class="pay-link-actions">
            <button class="pay-confirm" onclick={() => { closeCheckout(); onNavigate?.("skills"); }}>Open My Skills</button>
            <button class="pay-done" onclick={closeCheckout}>Keep browsing</button>
          </div>
        </div>

      {:else if checkoutStage === "error"}
        <h3 class="pay-title">Checkout failed</h3>
        <p class="pay-sub">{checkoutError || "Something went wrong."}</p>
        <button class="pay-confirm" onclick={() => handleCheckout(checkoutSkill)}>Try again</button>
        <button class="pay-done" onclick={closeCheckout}>Close</button>

      {:else}
        <h3 class="pay-title">Checkout</h3>
        <p class="pay-sub">Payment happens on a secure page — open the link in your browser.</p>

        <div class="pay-order">
          <div class="pay-order-icon" style="background: {skillTypeColor(checkoutSkill.skill_type)}">
            {initial(checkoutSkill)}
          </div>
          <div class="pay-order-info">
            <span class="pay-order-name">{checkoutSkill.title}</span>
            <span class="pay-order-type">{skillTypeLabel(checkoutSkill.skill_type)}</span>
          </div>
          <span class="pay-order-amt">{priceLabel(checkoutSkill)}</span>
        </div>
        {#if !isFree(checkoutSkill)}
          <p class="mp-pay-note">
            ≈ {priceLabel(checkoutSkill)} — charged in {canonicalCurrency(checkoutSkill)}.
            Final amount shown by your bank may differ slightly.
          </p>
        {/if}

        {#if checkoutStage === "creating"}
          <div class="pay-await">
            <span class="pay-spinner"></span>
            <p>Creating secure checkout…</p>
          </div>
        {:else}
          <div class="pay-link-box">
            <span class="pay-link-label">Checkout link</span>
            <span class="pay-link-url">{checkoutUrl}</span>
          </div>
          <div class="pay-link-actions">
            <button class="pay-confirm" onclick={copyCheckoutUrl}>
              <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><rect x="9" y="9" width="13" height="13" rx="2"/><path d="M5 15H4a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2h9a2 2 0 0 1 2 2v1"/></svg>
              {copied ? "Copied" : "Copy link"}
            </button>
            <button class="pay-confirm pay-open" onclick={openCheckoutUrl}>
              Open in browser
              <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round"><path d="M7 17 17 7M8 7h9v9"/></svg>
            </button>
          </div>
          <p class="pay-secure"><span class="pay-dot"></span>Waiting for payment — this screen updates automatically.</p>
        {/if}
      {/if}
    </div>
  {/if}
</div>

<style>
  .marketplace-root {
    position: absolute;
    inset: 0;
    background: var(--mpx-surface-2);
    display: flex;
    flex-direction: column;
  }
  .marketplace-root.embedded { position: relative; flex: 1; min-height: 0; }

  /* ── Header ────────────────────────────── */
  .mp-header {
    display: flex;
    align-items: center;
    gap: 8px;
    padding: 10px 12px;
    background: var(--yellow);
    flex-shrink: 0;
    z-index: 2;
  }
  .icon-btn {
    width: 34px; height: 34px;
    display: grid; place-items: center;
    background: none; border: none;
    cursor: pointer;
    color: var(--mpx-ink);
    border-radius: 50%;
    flex-shrink: 0;
    transition: background var(--dur-fast);
  }
  .icon-btn:hover { background: rgba(0,0,0,0.08); }
  .icon-btn:disabled { opacity: 0.6; cursor: default; }
  .icon-btn.spacer { visibility: hidden; }
  .icon-btn.spinning svg { animation: spin 0.9s linear infinite; }
  @keyframes spin { to { transform: rotate(360deg); } }

  .mp-title {
    flex: 1;
    font-size: 1.02rem;
    font-weight: 800;
    color: var(--mpx-ink);
    text-align: center;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }

  .mp-currency-select {
    height: 30px;
    padding: 0 6px;
    border: 1.5px solid rgba(0, 0, 0, 0.35);
    border-radius: var(--r-sm);
    background: rgba(255, 255, 255, 0.55);
    color: var(--mpx-ink);
    font-size: 0.72rem;
    font-weight: 800;
    cursor: pointer;
  }
  .mp-currency-select:focus-visible { outline: 2px solid var(--mpx-ink); }

  /* ── Toolbar (search + filters) ─────────── */
  .mp-toolbar {
    background: var(--yellow);
    flex-shrink: 0;
    padding-bottom: 12px;
    border-radius: 0 0 var(--r-xl) var(--r-xl);
  }
  .mp-toolbar-inner {
    max-width: var(--page-max);
    margin: 0 auto;
    padding: 0 14px;
    display: flex;
    flex-direction: column;
    gap: 10px;
  }
  @media (min-width: 900px) { .mp-toolbar-inner, .mp-content-inner, .mp-detail-inner { max-width: 900px; } }

  .mp-search-box {
    display: flex;
    align-items: center;
    gap: 8px;
    background: var(--mpx-surface);
    border-radius: var(--r-md);
    padding: 0 12px;
    height: 44px;
    box-shadow: var(--shadow-card);
    color: var(--mpx-muted);
  }
  .mp-search-box:focus-within { outline: 2px solid var(--mpx-ink); outline-offset: -2px; }
  .mp-search-icon { flex-shrink: 0; }
  .mp-search-input {
    flex: 1;
    min-width: 0;
    border: none;
    background: transparent;
    font-size: 0.92rem;
    color: var(--mpx-ink);
    outline: none;
  }
  .mp-search-input::placeholder { color: var(--mpx-muted); }
  .mp-search-input::-webkit-search-cancel-button { display: none; }
  .mp-search-clear {
    display: grid; place-items: center;
    background: none; border: none;
    cursor: pointer;
    color: var(--mpx-muted);
    padding: 2px;
    border-radius: 50%;
  }

  .mp-filters {
    display: flex;
    align-items: center;
    gap: 8px;
    overflow-x: auto;
    scrollbar-width: none;
  }
  .mp-filters::-webkit-scrollbar { display: none; }

  .mp-filter-chip {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    height: 32px;
    padding: 0 13px;
    border-radius: var(--r-pill);
    border: 1.5px solid rgba(0,0,0,0.2);
    background: transparent;
    font-size: 0.78rem;
    font-weight: 700;
    color: var(--mpx-ink);
    cursor: pointer;
    white-space: nowrap;
    transition: background var(--dur-fast), border-color var(--dur-fast), color var(--dur-fast);
  }
  .mp-filter-chip.active {
    background: var(--mpx-ink);
    border-color: var(--mpx-ink);
    color: var(--yellow);
  }
  .mp-filter-dot { width: 8px; height: 8px; border-radius: 50%; flex-shrink: 0; }

  /* ── Content ───────────────────────────── */
  .mp-content { flex: 1; overflow-y: auto; -webkit-overflow-scrolling: touch; }
  .mp-content-inner {
    max-width: var(--page-max);
    margin: 0 auto;
    padding: 14px 14px calc(24px + var(--safe-bottom));
    display: flex;
    flex-direction: column;
    gap: 14px;
  }

  /* ── Featured hero ─────────────────────── */
  .mp-hero {
    position: relative;
    overflow: hidden;
    display: flex;
    align-items: stretch;
    text-align: left;
    border: none;
    padding: 0;
    border-radius: var(--r-xl);
    background: var(--mpx-ink);
    color: #fff;
    min-height: 150px;
    cursor: pointer;
    box-shadow: var(--shadow-float);
    transition: transform var(--dur-fast);
  }
  .mp-hero:active { transform: scale(0.99); }
  .mp-hero-bg {
    position: absolute; inset: 0;
    background:
      radial-gradient(120% 120% at 100% 0%, color-mix(in srgb, var(--tone) 60%, transparent) 0%, transparent 55%),
      radial-gradient(80% 80% at 0% 100%, rgba(255,230,5,0.25) 0%, transparent 60%);
  }
  .mp-hero-body {
    position: relative;
    flex: 1;
    display: flex;
    flex-direction: column;
    gap: 6px;
    padding: 18px 18px 16px;
    min-width: 0;
  }
  .mp-hero-kicker { font-size: 0.64rem; font-weight: 800; letter-spacing: 0.12em; text-transform: uppercase; color: var(--yellow); }
  .mp-hero-title { font-size: 1.25rem; font-weight: 800; line-height: 1.15; }
  .mp-hero-desc {
    font-size: 0.8rem; color: rgba(255,255,255,0.75); line-height: 1.45;
    display: -webkit-box; -webkit-line-clamp: 2; -webkit-box-orient: vertical; overflow: hidden;
  }
  .mp-hero-row { display: flex; align-items: center; gap: 10px; margin-top: auto; padding-top: 6px; }
  .mp-hero-price { font-size: 0.92rem; font-weight: 800; color: var(--yellow); }
  .mp-hero-glyph {
    position: relative;
    align-self: center;
    margin-right: 18px;
    width: 76px; height: 76px;
    border-radius: var(--r-lg);
    background: color-mix(in srgb, var(--tone) 85%, #000);
    display: grid; place-items: center;
    font-size: 2rem; font-weight: 800;
    color: #fff;
    flex-shrink: 0;
    box-shadow: inset 0 0 0 1px rgba(255,255,255,0.15);
  }
  @media (max-width: 360px) { .mp-hero-glyph { display: none; } }

  .mp-section-head { display: flex; align-items: baseline; justify-content: space-between; padding: 2px 4px 0; }
  .mp-section-title { font-size: 0.86rem; font-weight: 800; color: var(--mpx-ink); }
  .mp-section-count { font-size: 0.7rem; color: var(--mpx-muted); font-weight: 600; }

  /* ── Grid ──────────────────────────────── */
  .mp-grid {
    display: grid;
    grid-template-columns: repeat(2, minmax(0, 1fr));
    gap: 10px;
  }
  @media (min-width: 640px) { .mp-grid { grid-template-columns: repeat(3, minmax(0, 1fr)); } }
  @media (min-width: 1100px) { .mp-grid { grid-template-columns: repeat(4, minmax(0, 1fr)); } }

  /* ── Card ──────────────────────────────── */
  .mp-card {
    background: var(--mpx-surface);
    border-radius: var(--r-lg);
    overflow: hidden;
    display: flex;
    flex-direction: column;
    box-shadow: var(--shadow-card);
    transition: transform var(--dur-fast), box-shadow var(--dur-fast);
  }
  .mp-card:hover { box-shadow: var(--shadow-float); }
  .mp-card.owned { outline: 2px solid var(--yellow); outline-offset: -2px; }

  .mp-card-main {
    flex: 1;
    display: flex;
    flex-direction: column;
    padding: 12px;
    gap: 5px;
    background: none;
    border: none;
    cursor: pointer;
    text-align: left;
    width: 100%;
  }

  .mp-card-top { display: flex; align-items: flex-start; justify-content: space-between; gap: 6px; margin-bottom: 4px; }

  .mp-card-icon {
    width: 40px; height: 40px;
    border-radius: var(--r-md);
    display: grid; place-items: center;
    font-weight: 800; font-size: 1.05rem;
    color: var(--tone);
    background: color-mix(in srgb, var(--tone) 16%, #fff);
    flex-shrink: 0;
  }

  .price-pill {
    font-size: 0.68rem; font-weight: 800;
    padding: 3px 8px;
    border-radius: var(--r-pill);
    background: var(--mpx-ink);
    color: #fff;
    white-space: nowrap;
  }
  .price-pill.free { background: var(--yellow); color: var(--mpx-ink); }

  .owned-badge {
    display: inline-flex; align-items: center; gap: 4px;
    height: 22px;
    padding: 0 7px;
    border-radius: var(--r-pill);
    background: var(--yellow);
    color: var(--mpx-ink);
    font-size: 0.64rem; font-weight: 800;
  }

  .mp-type-badge {
    display: inline-block;
    padding: 2px 8px;
    border-radius: var(--r-pill);
    font-size: 0.6rem;
    font-weight: 800;
    letter-spacing: 0.03em;
    color: var(--badge-color);
    background: color-mix(in srgb, var(--badge-color) 14%, #fff);
    white-space: nowrap;
  }
  .mp-type-badge.dark { background: rgba(255,255,255,0.14); color: #fff; }

  .mp-card-title {
    font-size: 0.88rem; font-weight: 700; color: var(--mpx-ink); line-height: 1.3;
    display: -webkit-box; -webkit-line-clamp: 2; -webkit-box-orient: vertical; overflow: hidden;
  }
  .mp-card-author { font-size: 0.68rem; color: var(--mpx-muted); }
  .mp-card-desc {
    font-size: 0.72rem; color: #777; line-height: 1.4;
    display: -webkit-box; -webkit-line-clamp: 2; -webkit-box-orient: vertical; overflow: hidden;
  }
  .mp-card-foot { margin-top: auto; padding-top: 6px; display: flex; align-items: center; justify-content: space-between; gap: 6px; }
  .mp-card-version { font-size: 0.64rem; color: var(--mpx-muted-2); font-weight: 600; }

  .mp-card-actions { padding: 0 12px 12px; }
  .mp-card-btn {
    width: 100%;
    height: 34px;
    border-radius: var(--r-sm);
    border: none;
    font-size: 0.78rem;
    font-weight: 800;
    cursor: pointer;
    transition: opacity var(--dur-fast), transform var(--dur-fast);
  }
  .mp-card-btn:active:not(:disabled) { transform: scale(0.97); }
  .mp-card-btn:disabled { opacity: 0.5; cursor: default; }
  .mp-card-btn-sub { background: var(--mpx-ink); color: var(--yellow); }
  .mp-card-btn-refund { background: var(--mpx-surface-2); color: #b42318; }

  .library-link {
    display: flex; align-items: center; justify-content: space-between;
    padding: 14px 16px;
    border: none;
    border-radius: var(--r-lg);
    background: var(--mpx-surface);
    color: var(--mpx-ink);
    font-size: 0.84rem; font-weight: 700;
    cursor: pointer;
    box-shadow: var(--shadow-card);
  }

  /* ── Empty / error ─────────────────────── */
  .mp-empty-state {
    display: flex; flex-direction: column; align-items: center;
    margin-top: 8vh;
    text-align: center;
    gap: 8px;
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
  .mp-empty-title { font-size: 1.02rem; font-weight: 800; color: var(--mpx-ink); }
  .mp-empty-desc { font-size: 0.8rem; color: var(--mpx-muted); max-width: 280px; line-height: 1.5; }
  .mp-retry-btn {
    margin-top: 8px;
    padding: 10px 18px;
    border-radius: var(--r-md);
    border: none;
    background: var(--mpx-ink);
    color: #fff;
    font-size: 0.82rem; font-weight: 700;
    cursor: pointer;
  }

  /* ── Skeleton ──────────────────────────── */
  .skeleton {
    background: linear-gradient(90deg, #ececec 25%, #f7f7f7 50%, #ececec 75%);
    background-size: 200% 100%;
    animation: shimmer 1.4s linear infinite;
    border-radius: var(--r-sm);
  }
  @keyframes shimmer { from { background-position: 200% 0; } to { background-position: -200% 0; } }
  .hero-skeleton { height: 150px; border-radius: var(--r-xl); }
  .card-skeleton { height: 190px; border-radius: var(--r-lg); }
  .sk-lines { flex: 1; display: flex; flex-direction: column; gap: 8px; }
  .sk-line { height: 14px; width: 70%; }
  .sk-line.short { width: 40%; height: 10px; }

  /* ═══ DETAIL VIEW ═══ */
  .mp-detail { flex: 1; overflow-y: auto; -webkit-overflow-scrolling: touch; }
  .mp-detail-inner {
    max-width: var(--page-max);
    margin: 0 auto;
    padding: 16px 14px calc(24px + var(--safe-bottom));
    display: flex;
    flex-direction: column;
    gap: 16px;
  }

  .mp-detail-hero { display: flex; align-items: center; gap: 14px; }
  .mp-detail-icon { width: 72px; height: 72px; border-radius: var(--r-lg); object-fit: cover; }
  .mp-detail-icon-placeholder {
    width: 72px; height: 72px;
    border-radius: var(--r-lg);
    display: grid; place-items: center;
    font-size: 1.8rem; font-weight: 800;
    color: var(--tone);
    background: color-mix(in srgb, var(--tone) 16%, #fff);
    flex-shrink: 0;
  }
  .mp-detail-hero-info { display: flex; flex-direction: column; gap: 4px; min-width: 0; }
  .mp-detail-name { font-size: 1.2rem; font-weight: 800; color: var(--mpx-ink); line-height: 1.2; }
  .mp-detail-author { font-size: 0.78rem; color: var(--mpx-muted); }
  .mp-detail-badges { display: flex; align-items: center; gap: 6px; margin-top: 2px; }

  .mp-detail-price-row {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 12px;
    padding: 12px 14px;
    background: var(--mpx-surface);
    border-radius: var(--r-lg);
    box-shadow: var(--shadow-card);
  }
  .mp-detail-price-col { display: flex; flex-direction: column; gap: 2px; }
  .mp-detail-price-label { font-size: 0.66rem; color: var(--mpx-muted); font-weight: 700; text-transform: uppercase; letter-spacing: 0.06em; }
  .mp-detail-price-value { font-size: 1.25rem; font-weight: 800; color: var(--mpx-ink); }

  .mp-action-btn {
    min-width: 120px;
    height: 44px;
    padding: 0 18px;
    border-radius: var(--r-md);
    border: none;
    font-size: 0.9rem;
    font-weight: 800;
    cursor: pointer;
    transition: opacity var(--dur-fast), transform var(--dur-fast);
    white-space: nowrap;
  }
  .mp-action-btn:active:not(:disabled) { transform: scale(0.97); }
  .mp-action-btn:disabled { opacity: 0.5; cursor: default; }
  .mp-action-subscribe { background: var(--mpx-ink); color: var(--yellow); }
  .mp-action-refund { background: var(--mpx-surface-2); color: #b42318; }

  .mp-detail-desc { font-size: 0.88rem; color: #333; line-height: 1.55; }
  .mp-detail-section { display: flex; flex-direction: column; gap: 8px; }
  .mp-detail-section-title {
    font-size: 0.68rem; font-weight: 800; color: var(--mpx-muted);
    text-transform: uppercase; letter-spacing: 0.08em;
  }
  .mp-detail-chips { display: flex; flex-wrap: wrap; gap: 6px; }
  .mp-chip {
    padding: 6px 12px;
    border-radius: var(--r-pill);
    font-size: 0.75rem; font-weight: 600;
    background: var(--mpx-yellow-tint);
    color: #6f6400;
  }
  .mp-chip-tag { background: var(--mpx-surface-3); color: #555; }

  .mp-readme { background: var(--mpx-surface); border-radius: var(--r-lg); padding: 14px; box-shadow: var(--shadow-card); }
  .mp-readme-text { font-size: 0.82rem; color: #444; line-height: 1.6; white-space: pre-wrap; }

  /* ═══ CHECKOUT SHEET ═══ */
  .pay-scrim {
    position: absolute; inset: 0;
    background: rgba(0, 0, 0, 0.45);
    backdrop-filter: blur(2px);
    z-index: 10;
    animation: pay-fade var(--dur-med) ease;
  }
  @keyframes pay-fade { from { opacity: 0; } to { opacity: 1; } }

  .pay-sheet {
    position: absolute;
    left: 0; right: 0; bottom: 0;
    z-index: 11;
    background: #fff;
    border-radius: var(--r-xl) var(--r-xl) 0 0;
    padding: 8px 20px calc(22px + var(--safe-bottom));
    max-height: 92%;
    overflow-y: auto;
    animation: pay-slide var(--dur-slow) var(--ease-spring);
    box-shadow: var(--shadow-float);
    max-width: 560px;
    margin: 0 auto;
  }
  @keyframes pay-slide { from { transform: translateY(100%); } to { transform: translateY(0); } }

  .pay-grab { width: 40px; height: 5px; border-radius: 3px; background: #dcdcdc; margin: 6px auto 14px; }
  .pay-title { font-size: 1.15rem; font-weight: 800; color: var(--mpx-ink); }
  .pay-sub { font-size: 0.8rem; color: var(--mpx-muted); margin: 2px 0 16px; }

  .pay-order {
    display: flex; align-items: center; gap: 12px;
    background: var(--mpx-surface-2);
    border-radius: var(--r-md);
    padding: 12px;
    margin-bottom: 12px;
  }
  .pay-order-icon {
    width: 42px; height: 42px;
    border-radius: var(--r-sm);
    flex-shrink: 0;
    display: grid; place-items: center;
    color: #fff; font-weight: 800; font-size: 1.1rem;
  }
  .pay-order-info { display: flex; flex-direction: column; gap: 2px; flex: 1; min-width: 0; }
  .pay-order-name { font-size: 0.9rem; font-weight: 700; color: var(--mpx-ink); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  .pay-order-type { font-size: 0.72rem; color: var(--mpx-muted); }
  .pay-order-amt { font-size: 1.05rem; font-weight: 800; color: var(--mpx-ink); }
  .mp-pay-note { font-size: 0.72rem; color: var(--mpx-muted); margin: -4px 0 12px; line-height: 1.4; }

  .pay-confirm {
    flex: 1;
    width: 100%;
    margin-top: 8px;
    height: 48px;
    border-radius: var(--r-md);
    border: none;
    background: var(--mpx-ink);
    color: #fff;
    font-size: 0.9rem; font-weight: 700;
    cursor: pointer;
    display: inline-flex; align-items: center; justify-content: center; gap: 8px;
  }
  .pay-confirm:disabled { opacity: 0.9; cursor: default; }
  .pay-open { background: var(--yellow); color: var(--mpx-ink); }

  .pay-spinner {
    width: 18px; height: 18px;
    border-radius: 50%;
    border: 2.5px solid rgba(0, 0, 0, 0.15);
    border-top-color: var(--mpx-ink);
    animation: spin 0.7s linear infinite;
  }
  .pay-secure { display: flex; align-items: center; justify-content: center; gap: 6px; font-size: 0.72rem; color: var(--mpx-muted); margin-top: 12px; }
  .pay-dot { width: 8px; height: 8px; border-radius: 50%; background: var(--mpx-ok); animation: pulse 1.4s ease-in-out infinite; }
  @keyframes pulse { 0%, 100% { opacity: 1; transform: scale(1); } 50% { opacity: 0.5; transform: scale(0.8); } }

  .pay-success { text-align: center; padding: 10px 0 4px; }
  .pay-check {
    width: 74px; height: 74px;
    border-radius: 50%;
    background: var(--mpx-ok);
    margin: 8px auto 16px;
    display: grid; place-items: center;
    animation: pay-pop 0.4s cubic-bezier(.18, 1.5, .4, 1);
  }
  @keyframes pay-pop { from { transform: scale(0); } to { transform: scale(1); } }
  .pay-success-title { font-size: 1.15rem; font-weight: 800; color: var(--mpx-ink); }
  .pay-success-sub { font-size: 0.82rem; color: var(--mpx-muted); margin: 6px 0 0; }
  .pay-receipt { background: var(--mpx-surface-2); border-radius: var(--r-md); padding: 12px 14px; margin: 16px 0 6px; text-align: left; }
  .pay-receipt-row { display: flex; justify-content: space-between; gap: 12px; font-size: 0.8rem; color: #555; padding: 4px 0; }
  .pay-receipt-row b { color: var(--mpx-ink); font-weight: 700; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  .pay-ok { color: #15803d !important; }
  .pay-done {
    flex: 1;
    width: 100%;
    margin-top: 8px;
    height: 48px;
    border-radius: var(--r-md);
    border: none;
    background: var(--yellow);
    color: var(--mpx-ink);
    font-size: 0.9rem; font-weight: 700;
    cursor: pointer;
  }

  .pay-await {
    display: flex; flex-direction: column; align-items: center;
    gap: 10px;
    padding: 18px 0 6px;
    color: var(--mpx-muted);
    font-size: 0.82rem;
  }
  .pay-link-box {
    display: flex; flex-direction: column; gap: 4px;
    background: var(--mpx-surface-2);
    border-radius: var(--r-md);
    padding: 12px 14px;
    margin-bottom: 4px;
  }
  .pay-link-label { font-size: 0.66rem; font-weight: 700; color: var(--mpx-muted); text-transform: uppercase; letter-spacing: 0.06em; }
  .pay-link-url { font-size: 0.78rem; color: #333; word-break: break-all; line-height: 1.4; font-family: ui-monospace, Menlo, Consolas, monospace; }
  .pay-link-actions { display: flex; gap: 8px; }
</style>
