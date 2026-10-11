<script>
  import { detectCurrency, setPreferredCurrency } from "./currency.js";
  import {
    listSkills,
    getSkillManifest,
    listRobotSkills,
    removeSkill,
    deploySkill,
    createCheckout,
    getOrder,
  } from "./marketplaceApi.js";

  /**
   * The Store: AI skills and Motion skills, App Store style.
   * Browse: a featured skill, the top list, and categories from the skills'
   * tags. Detail: the skill's page with Get / Buy. Purchase links out to the
   * gateway's HTTPS checkout and polls the order, as before.
   *
   * @prop onNavigate — ("skills") => void
   */
  let { onNavigate } = $props();

  const MINOR_DIGITS = {
    BIF: 0, CLP: 0, DJF: 0, GNF: 0, IDR: 0, ISK: 0, JPY: 0, KMF: 0, KRW: 0,
    LAK: 0, MGA: 0, PYG: 0, RWF: 0, UGX: 0, UYI: 0, VND: 0, VUV: 0, XAF: 0,
    XOF: 0, XPF: 0,
    BHD: 3, IQD: 3, JOD: 3, KWD: 3, LYD: 3, OMR: 3, TND: 3,
  };
  const DISPLAY_CURRENCIES = ["USD", "HKD", "CNY", "EUR", "GBP", "JPY", "SGD"];

  let skills = $state([]);
  let robotSkills = $state([]);
  let loading = $state(true);
  let error = $state("");
  let displayCurrency = $state(detectCurrency());

  let kind = $state("awa");         // "awa" (AI) | "wasm" (Motion)
  let category = $state("");        // tag filter
  let searching = $state(false);
  let query = $state("");
  let searchInput = $state(null);

  let detailSkill = $state(null);
  let detailManifest = $state(null);
  let detailLoading = $state(false);
  let actionInFlight = $state(null);

  // ── Fetch ──
  async function fetchSkills() {
    loading = true;
    error = "";
    try { skills = await listSkills(); }
    catch (e) { error = e.message || "Failed to load the store"; skills = []; }
    loading = false;
  }
  async function fetchRobotSkills() {
    try { robotSkills = await listRobotSkills(); } catch { /* ignore */ }
  }
  async function refreshAll() { await Promise.all([fetchSkills(), fetchRobotSkills()]); }
  async function onCurrencyChange(code) {
    setPreferredCurrency(code);
    displayCurrency = code;
    await fetchSkills();
  }
  $effect(() => { refreshAll(); });

  // ── Helpers ──
  const typeOf = (s) => ((s?.skill_type || "").toLowerCase() === "wasm" ? "wasm" : "awa");
  const isOwned = (id) => robotSkills.some((s) => s.skill_id === id);
  function author(skill) {
    if (detailManifest?.author && detailSkill?.id === skill?.id) return detailManifest.author;
    const parts = (skill?.id || "").split("~");
    return parts.length >= 2 && parts[0] ? parts[0].replace(/_/g, " ").replace(/\b\w/g, (c) => c.toUpperCase()) : "MangDang";
  }
  function formatPrice(price) {
    if (!price) return null;
    let amount = typeof price.amount === "string" ? parseFloat(price.amount) : price.amount;
    if (Number.isNaN(amount)) return price.currency || null;
    try {
      return new Intl.NumberFormat(undefined, {
        style: "currency", currency: price.currency,
        minimumFractionDigits: MINOR_DIGITS[price.currency] ?? 2,
        maximumFractionDigits: MINOR_DIGITS[price.currency] ?? 2,
      }).format(amount);
    } catch {
      return `${price.currency === "USD" ? "$" : price.currency + " "}${amount.toFixed(2)}`;
    }
  }
  const skillPrice = (s) => s?.display_price ?? s?.price ?? null;
  function priceLabel(s) {
    const p = skillPrice(s);
    if (!p || p.amount <= 0) return "Free";
    return formatPrice(p);
  }
  const isFree = (s) => priceLabel(s) === "Free";
  const canonicalCurrency = (s) => s?.price?.currency ?? "USD";
  const buttonLabel = (s) => (isOwned(s.id) ? "Open" : isFree(s) ? "Get" : priceLabel(s));

  // ── Browse lists ──
  const ofKind = $derived(skills.filter((s) => typeOf(s) === kind));
  const featured = $derived(ofKind.find((s) => !isOwned(s.id)) ?? ofKind[0] ?? null);
  const categories = $derived.by(() => {
    const seen = new Map();
    for (const s of ofKind) for (const t of s.tags || []) seen.set(t, (seen.get(t) || 0) + 1);
    return [...seen.keys()].slice(0, 6);
  });
  const ranked = $derived.by(() => {
    let list = ofKind.filter((s) => s !== featured || category);
    if (category) list = ofKind.filter((s) => (s.tags || []).includes(category));
    return list;
  });
  const results = $derived.by(() => {
    const q = query.trim().toLowerCase();
    if (!q) return [];
    return skills.filter((s) => (s.title || "").toLowerCase().includes(q) || (s.description || "").toLowerCase().includes(q));
  });

  function pickKind(k) { kind = k; category = ""; }
  function openSearch() {
    searching = true;
    queueMicrotask(() => searchInput?.focus());
  }
  function closeSearch() { searching = false; query = ""; }

  function act(s) {
    if (isOwned(s.id)) onNavigate?.("skills");
    else openCheckout(s);
  }

  // ── Detail ──
  async function openDetail(skill) {
    detailSkill = skill;
    detailManifest = null;
    detailLoading = true;
    try { detailManifest = await getSkillManifest(skill.id); } catch { detailManifest = null; }
    detailLoading = false;
  }
  function closeDetail() { detailSkill = null; detailManifest = null; }

  async function handleRefund(id) {
    actionInFlight = id;
    try { await removeSkill(id); await fetchRobotSkills(); } catch (e) { console.error("Refund failed:", e); }
    actionInFlight = null;
  }

  // ── Checkout (link-out: card entry happens on HTTPS) ──
  let checkoutSkill = $state(null);
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
  function closeCheckout() { stopCheckoutPoll(); checkoutSkill = null; checkoutStage = "creating"; }
  function stopCheckoutPoll() { if (checkoutPollTimer) { clearInterval(checkoutPollTimer); checkoutPollTimer = null; } }

  async function handleCheckout(skill) {
    checkoutStage = "creating";
    checkoutError = "";
    try {
      const data = await createCheckout(skill.id);
      checkoutOrderId = data.order_id || "";
      checkoutUrl = data.checkout_url || "";
      if (data.status === "paid" || data.status === "fulfilled") { await finishPurchase(skill); return; }
      try { await navigator.clipboard.writeText(checkoutUrl); copied = true; } catch { /* copy by hand */ }
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
        if (order.status === "paid" || order.status === "fulfilled") await finishPurchase(skill);
        else if (order.status === "failed" || order.status === "expired") {
          stopCheckoutPoll();
          checkoutStage = "error";
          checkoutError = `Order ${order.status}. Please try again.`;
        }
      } catch { /* keep polling */ }
    }, 2500);
  }
  async function finishPurchase(skill) {
    stopCheckoutPoll();
    checkoutStage = "paid";
    await fetchRobotSkills();
    if (typeOf(skill) === "wasm") {
      try { await deploySkill(skill.id); } catch (e) { console.error("Deploy failed:", e); }
    }
  }
  async function copyCheckoutUrl() {
    try { await navigator.clipboard.writeText(checkoutUrl); copied = true; setTimeout(() => (copied = false), 1800); } catch {}
  }
  $effect(() => () => stopCheckoutPoll());
</script>

{#snippet icon(s, size)}
  <span class="app-ico" class:motion={typeOf(s) === "wasm"} style="--s: {size}px" aria-hidden="true">
    <i></i><i></i>
  </span>
{/snippet}

{#snippet getBtn(s, big)}
  <button
    type="button"
    class="get"
    class:big
    class:owned={isOwned(s.id)}
    onclick={(e) => { e.stopPropagation(); act(s); }}
    aria-label="{buttonLabel(s)} {s.title}"
  >{buttonLabel(s)}</button>
{/snippet}

{#snippet row(s, rank)}
  <li class="row">
    <button type="button" class="row-main" onclick={() => openDetail(s)}>
      {#if rank}<span class="rank">{rank}</span>{/if}
      {@render icon(s, 58)}
      <span class="row-txt">
        <span class="row-name">{s.title}</span>
        <span class="row-desc">{s.description || (typeOf(s) === "wasm" ? "Motion skill" : "AI skill")}</span>
      </span>
    </button>
    {@render getBtn(s, false)}
  </li>
{/snippet}

<div class="store">
  {#if detailSkill}
    <!-- ═══ Skill page ═══ -->
    <div class="scroll">
      <div class="inner">
        <div class="topbar">
          <button type="button" class="round" onclick={closeDetail} aria-label="Back to the Store">
            <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M15 6l-6 6 6 6"/></svg>
          </button>
        </div>
        <div class="d-hero">
          {@render icon(detailSkill, 108)}
          <div class="d-txt">
            <h1>{detailManifest?.name || detailSkill.title}</h1>
            <span class="d-sub">{author(detailSkill)} · v{detailManifest?.version || detailSkill.current_version || "1.0"}</span>
            <div class="d-act">
              {@render getBtn(detailSkill, true)}
              <span class="kind-chip" class:motion={typeOf(detailSkill) === "wasm"}>{typeOf(detailSkill) === "wasm" ? "Motion skill" : "AI skill"}</span>
            </div>
          </div>
        </div>

        {#if detailLoading}
          <div class="sk-block" aria-hidden="true"></div>
        {:else}
          {#if detailManifest?.description || detailSkill.description}
            <p class="d-desc">{detailManifest?.description || detailSkill.description}</p>
          {/if}
          {#if detailManifest?.capabilities?.length}
            <section class="d-sec">
              <h2>Uses</h2>
              <div class="chips">{#each detailManifest.capabilities as c}<span class="chip">{c}</span>{/each}</div>
            </section>
          {/if}
          {#if detailManifest?.readme}
            <section class="d-sec">
              <h2>About this skill</h2>
              <p class="readme">{detailManifest.readme}</p>
            </section>
          {/if}
          <section class="d-sec facts">
            <div><span>Price</span><b>{priceLabel(detailSkill)}</b></div>
            <div><span>Version</span><b>{detailManifest?.version || detailSkill.current_version || "1.0"}</b></div>
            <div><span>Runs</span><b>{typeOf(detailSkill) === "wasm" ? "On the robot" : "With the AI"}</b></div>
          </section>
          {#if isOwned(detailSkill.id)}
            <button type="button" class="refund" disabled={actionInFlight === detailSkill.id} onclick={() => handleRefund(detailSkill.id)}>
              {actionInFlight === detailSkill.id ? "Processing…" : "Refund and remove"}
            </button>
          {/if}
        {/if}
      </div>
    </div>
  {:else}
    <!-- ═══ Browse ═══ -->
    <div class="scroll">
      <div class="inner">
        <header class="head">
          {#if searching}
            <label class="search">
              <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" aria-hidden="true"><circle cx="11" cy="11" r="7"/><path d="M20 20l-4-4"/></svg>
              <span class="sr">Search the Store</span>
              <input type="search" bind:this={searchInput} bind:value={query} placeholder="Search skills" />
            </label>
            <button type="button" class="text-btn" onclick={closeSearch}>Cancel</button>
          {:else}
            <h1>Store</h1>
            <button type="button" class="round" onclick={openSearch} aria-label="Search the Store">
              <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" aria-hidden="true"><circle cx="11" cy="11" r="7"/><path d="M20 20l-4-4"/></svg>
            </button>
          {/if}
        </header>

        {#if searching}
          {#if query.trim() && results.length === 0}
            <p class="muted">No skills match “{query}”.</p>
          {:else}
            <ul class="rows">{#each results as s (s.id)}{@render row(s, 0)}{/each}</ul>
          {/if}
        {:else}
          <div class="tabs" role="tablist" aria-label="Kind of skill">
            <button type="button" role="tab" aria-selected={kind === "awa"} class:on={kind === "awa"} onclick={() => pickKind("awa")}>AI skills</button>
            <button type="button" role="tab" aria-selected={kind === "wasm"} class:on={kind === "wasm"} onclick={() => pickKind("wasm")}>Motion skills</button>
          </div>

          {#if loading}
            <div class="sk-hero" aria-hidden="true"></div>
            {#each [0, 1, 2] as _}<div class="sk-row" aria-hidden="true"></div>{/each}
          {:else if error}
            <div class="empty">
              <p class="empty-title">The Store isn't reachable</p>
              <p class="muted">The robot needs to be on Wi‑Fi with internet to open the Store.</p>
              <button type="button" class="dark-btn" onclick={refreshAll}>Try again</button>
            </div>
          {:else if ofKind.length === 0}
            <div class="empty">
              <p class="empty-title">Nothing here yet</p>
              <p class="muted">New {kind === "wasm" ? "motion" : "AI"} skills appear here as they're published.</p>
            </div>
          {:else}
            {#if featured && !category}
              <section class="feat-wrap">
                <span class="kicker">{kind === "wasm" ? "Featured motion skill" : "Featured AI skill"}</span>
                <button type="button" class="feat" class:motion={kind === "wasm"} onclick={() => openDetail(featured)}>
                  <span class="feat-eyes" aria-hidden="true"><i></i><i></i></span>
                  <span class="feat-foot">
                    <span class="feat-txt">
                      <span class="feat-name">{featured.title}</span>
                      {#if featured.description}<span class="feat-desc">{featured.description}</span>{/if}
                    </span>
                  </span>
                </button>
                <div class="feat-btn">{@render getBtn(featured, true)}</div>
              </section>
            {/if}

            <section>
              <div class="sec-head">
                <h2>{category ? category.replace(/^\w/, (c) => c.toUpperCase()) : kind === "wasm" ? "Top motion skills" : "Top AI skills"}</h2>
                {#if category}<button type="button" class="text-btn" onclick={() => (category = "")}>Show all</button>{/if}
              </div>
              <ul class="rows">
                {#each ranked as s, i (s.id)}{@render row(s, i + 1)}{/each}
              </ul>
            </section>

            {#if categories.length > 1 && !category}
              <section>
                <div class="sec-head"><h2>Browse</h2></div>
                <div class="cats">
                  {#each categories as c, i}
                    <button type="button" class="cat" class:tint={i % 3 !== 0} class:motion={kind === "wasm"} onclick={() => (category = c)}>
                      {c.replace(/^\w/, (x) => x.toUpperCase())}
                    </button>
                  {/each}
                </div>
              </section>
            {/if}
          {/if}

          <label class="currency">
            Prices in
            <select value={displayCurrency} onchange={(e) => onCurrencyChange(e.currentTarget.value)}>
              {#each DISPLAY_CURRENCIES as c}<option value={c}>{c}</option>{/each}
            </select>
          </label>
        {/if}
      </div>
    </div>
  {/if}

  <!-- ═══ Checkout sheet ═══ -->
  {#if checkoutSkill}
    <div class="scrim" onclick={closeCheckout} role="presentation"></div>
    <div class="sheet" role="dialog" aria-modal="true" aria-label="Checkout">
      <span class="grab" aria-hidden="true"></span>
      <div class="pay-item">
        {@render icon(checkoutSkill, 52)}
        <span class="pay-txt"><b>{checkoutSkill.title}</b><span>{typeOf(checkoutSkill) === "wasm" ? "Motion skill" : "AI skill"}</span></span>
        <b class="pay-amt">{priceLabel(checkoutSkill)}</b>
      </div>

      {#if checkoutStage === "paid"}
        <div class="done">
          <span class="tick" aria-hidden="true"><svg width="30" height="30" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="3" stroke-linecap="round" stroke-linejoin="round"><path d="M4 12.5l6 5.5L20 6"/></svg></span>
          <h3>{isFree(checkoutSkill) ? "It's yours" : "Payment done"}</h3>
          <p class="muted">{checkoutSkill.title} is in your Library{typeOf(checkoutSkill) === "wasm" ? " and on the robot" : ""}.</p>
          {#if checkoutOrderId}<p class="order">Order {checkoutOrderId}</p>{/if}
        </div>
        <button type="button" class="dark-btn wide" onclick={() => { closeCheckout(); onNavigate?.("skills"); }}>Open Skills</button>
        <button type="button" class="text-btn wide" onclick={closeCheckout}>Keep browsing</button>
      {:else if checkoutStage === "error"}
        <p class="err">{checkoutError || "Something went wrong."}</p>
        <button type="button" class="dark-btn wide" onclick={() => handleCheckout(checkoutSkill)}>Try again</button>
        <button type="button" class="text-btn wide" onclick={closeCheckout}>Close</button>
      {:else if checkoutStage === "creating"}
        <p class="muted center"><span class="spinner" aria-hidden="true"></span>Getting checkout ready…</p>
      {:else}
        {#if !isFree(checkoutSkill)}
          <p class="muted small">Charged in {canonicalCurrency(checkoutSkill)}; your bank may show a slightly different amount.</p>
        {/if}
        <p class="muted">Payment happens on a secure page. Open the link in your browser; this sheet updates when it's paid.</p>
        <div class="link-box"><span>{checkoutUrl}</span></div>
        <div class="pay-btns">
          <button type="button" class="ghost-btn" onclick={copyCheckoutUrl}>{copied ? "Copied" : "Copy link"}</button>
          <button type="button" class="dark-btn" onclick={() => window.open(checkoutUrl, "_blank")}>Open checkout</button>
        </div>
        <p class="waiting"><span class="pulse" aria-hidden="true"></span>Waiting for payment…</p>
      {/if}
    </div>
  {/if}
</div>

<style>
  .store { position: absolute; inset: 0; display: flex; flex-direction: column; background: #fff; color: var(--ink); }
  .scroll { flex: 1; min-height: 0; overflow-y: auto; }
  .inner { max-width: var(--page-max); margin: 0 auto; padding: calc(22px + var(--safe-top)) 20px 28px; display: flex; flex-direction: column; gap: 18px; }

  .head { display: flex; align-items: center; justify-content: space-between; gap: 10px; min-height: 48px; }
  .head h1 { font-size: 34px; font-weight: 800; letter-spacing: -0.02em; }
  .round { width: 44px; height: 44px; border-radius: 22px; border: none; background: var(--ground); color: var(--ink); display: grid; place-items: center; cursor: pointer; flex-shrink: 0; }
  .search { flex: 1; display: flex; align-items: center; gap: 8px; height: 44px; padding: 0 14px; border-radius: 22px; background: var(--ground); color: var(--sub); }
  .search input { flex: 1; min-width: 0; border: none; background: transparent; font-size: 16px; color: var(--ink); outline: none; }
  .search:focus-within { outline: 3px solid var(--ink); outline-offset: 2px; }
  .text-btn { height: 40px; border: none; background: transparent; color: #1F5FBF; font-size: 15px; font-weight: 700; cursor: pointer; padding: 0 6px; }
  .text-btn.wide { width: 100%; }

  .tabs { display: flex; gap: 8px; margin-top: -6px; }
  .tabs button { height: 40px; padding: 0 18px; border-radius: 20px; border: none; background: var(--ground); color: #3F3C37; font-size: 15px; font-weight: 700; cursor: pointer; }
  .tabs button.on { background: var(--ink); color: #fff; }

  .kicker { font-size: 12px; font-weight: 800; letter-spacing: 0.12em; text-transform: uppercase; color: var(--sub); }
  .feat-wrap { position: relative; display: flex; flex-direction: column; gap: 10px; }
  .feat {
    width: 100%; min-height: 230px; border: none; border-radius: 26px; padding: 22px; cursor: pointer; text-align: left;
    background: var(--mpx-yellow); color: var(--ink);
    display: flex; flex-direction: column; justify-content: space-between; gap: 20px;
  }
  .feat.motion { background: #CFE3FF; }
  .feat-eyes { display: flex; gap: 14px; align-items: flex-end; height: 100px; }
  .feat-eyes i { width: 40px; height: 84px; border-radius: 12px; background: var(--ink); transform-origin: 50% 60%; animation: blink 4.2s ease-in-out infinite; }
  .feat-eyes i + i { animation-delay: 0.05s; }
  @keyframes blink { 0%, 94%, 100% { transform: scaleY(1); } 97% { transform: scaleY(0.1); } }
  .feat-foot { display: flex; align-items: flex-end; justify-content: space-between; gap: 12px; padding-right: 96px; }
  .feat-txt { display: flex; flex-direction: column; gap: 4px; }
  .feat-name { font-size: 26px; font-weight: 800; letter-spacing: -0.01em; }
  .feat-desc { font-size: 14px; color: rgba(17, 17, 17, 0.72); }
  .feat-btn { position: absolute; right: 22px; bottom: 22px; }

  .sec-head { display: flex; align-items: baseline; justify-content: space-between; margin-bottom: 2px; }
  .sec-head h2 { font-size: 22px; font-weight: 800; letter-spacing: -0.01em; }

  .rows { list-style: none; }
  .row { display: flex; align-items: center; gap: 12px; border-bottom: 1px solid var(--hair-2); }
  .row-main { flex: 1; min-width: 0; display: flex; align-items: center; gap: 14px; padding: 12px 0; border: none; background: transparent; text-align: left; cursor: pointer; color: var(--ink); }
  .rank { font-size: 15px; font-weight: 800; color: var(--sub); width: 16px; text-align: center; flex-shrink: 0; }
  .row-txt { flex: 1; min-width: 0; display: flex; flex-direction: column; gap: 2px; }
  .row-name { font-size: 16px; font-weight: 700; }
  .row-desc { font-size: 13px; color: var(--sub); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }

  .app-ico {
    width: var(--s); height: var(--s); border-radius: calc(var(--s) * 0.27); flex-shrink: 0;
    background: var(--mpx-yellow); display: flex; align-items: center; justify-content: center; gap: calc(var(--s) * 0.07);
  }
  .app-ico.motion { background: #CFE3FF; }
  .app-ico i { width: calc(var(--s) * 0.12); height: calc(var(--s) * 0.28); border-radius: calc(var(--s) * 0.05); background: var(--ink); }

  .get {
    height: 32px; min-width: 72px; padding: 0 14px; border-radius: 16px; border: none; flex-shrink: 0;
    background: #EDEAF8; color: #1F5FBF; font-size: 14px; font-weight: 800; text-transform: uppercase; cursor: pointer;
  }
  .get.big { height: 36px; min-width: 80px; background: var(--ink); color: #fff; font-size: 15px; }
  .get.owned { background: var(--ground); color: #1F5FBF; }
  .get.big.owned { background: var(--ink); color: #fff; }

  .cats { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 10px; }
  .cat { height: 64px; border-radius: 18px; border: none; display: flex; align-items: flex-end; padding: 12px 14px; font-size: 16px; font-weight: 800; color: var(--ink); background: var(--ground); cursor: pointer; text-align: left; }
  .cat.tint { background: var(--mpx-yellow); }
  .cat.tint.motion { background: #CFE3FF; }

  .currency { align-self: center; display: flex; align-items: center; gap: 6px; font-size: 13px; color: var(--sub); }
  .currency select { border: none; background: var(--ground); border-radius: 10px; height: 32px; padding: 0 8px; font-size: 13px; font-weight: 700; color: var(--ink); }

  .muted { font-size: 14px; line-height: 1.45; color: var(--sub); }
  .muted.small { font-size: 12px; }
  .muted.center { display: flex; align-items: center; justify-content: center; gap: 10px; padding: 18px 0; }
  .empty { background: var(--ground); border-radius: 22px; padding: 28px 20px; display: flex; flex-direction: column; align-items: center; gap: 8px; text-align: center; }
  .empty-title { font-size: 17px; font-weight: 800; }
  .dark-btn { height: 46px; padding: 0 20px; border-radius: 23px; border: none; background: var(--ink); color: #fff; font-size: 15px; font-weight: 700; cursor: pointer; }
  .dark-btn.wide { width: 100%; }
  .ghost-btn { height: 46px; padding: 0 20px; border-radius: 23px; border: none; background: var(--ground); color: var(--ink); font-size: 15px; font-weight: 700; cursor: pointer; }

  .sk-hero { height: 230px; border-radius: 26px; background: var(--ground); }
  .sk-row { height: 70px; border-radius: 16px; background: var(--ground); }
  .sk-block { height: 120px; border-radius: 18px; background: var(--ground); }

  /* detail */
  .topbar { display: flex; }
  .d-hero { display: flex; gap: 18px; align-items: center; }
  .d-txt { display: flex; flex-direction: column; gap: 4px; min-width: 0; }
  .d-txt h1 { font-size: 24px; font-weight: 800; letter-spacing: -0.01em; }
  .d-sub { font-size: 14px; color: var(--sub); }
  .d-act { display: flex; align-items: center; gap: 10px; margin-top: 8px; flex-wrap: wrap; }
  .kind-chip { font-size: 12px; font-weight: 700; padding: 4px 9px; border-radius: 8px; background: #FFF6B0; color: #5B4E00; }
  .kind-chip.motion { background: #E3EEFF; color: #1F4F99; }
  .d-desc { font-size: 16px; line-height: 1.5; }
  .d-sec { display: flex; flex-direction: column; gap: 8px; }
  .d-sec h2 { font-size: 18px; font-weight: 800; }
  .chips { display: flex; flex-wrap: wrap; gap: 6px; }
  .chip { font-size: 13px; font-weight: 600; padding: 6px 10px; border-radius: 10px; background: var(--ground); }
  .readme { font-size: 15px; line-height: 1.55; color: #3F3C37; white-space: pre-wrap; }
  .facts { flex-direction: row; background: var(--ground); border-radius: 18px; padding: 14px; justify-content: space-around; text-align: center; }
  .facts div { display: flex; flex-direction: column; gap: 2px; }
  .facts span { font-size: 11px; font-weight: 700; letter-spacing: 0.08em; text-transform: uppercase; color: var(--sub); }
  .facts b { font-size: 15px; }
  .refund { align-self: center; height: 40px; border: none; background: transparent; color: #9B2C20; font-size: 14px; font-weight: 700; cursor: pointer; }

  /* checkout */
  .scrim { position: fixed; inset: 0; z-index: 60; background: rgba(17, 17, 17, 0.4); }
  .sheet {
    position: fixed; left: 0; right: 0; bottom: 0; z-index: 61; max-width: var(--page-max); margin: 0 auto;
    background: #fff; border-radius: 28px 28px 0 0; padding: 10px 20px calc(20px + var(--safe-bottom));
    display: flex; flex-direction: column; gap: 14px; box-shadow: 0 -12px 40px rgba(0, 0, 0, 0.18);
    animation: up var(--dur-slow) var(--ease-spring);
  }
  @keyframes up { from { transform: translateY(100%); } to { transform: none; } }
  .grab { align-self: center; width: 40px; height: 5px; border-radius: 3px; background: #D9D4C7; }
  .pay-item { display: flex; align-items: center; gap: 12px; }
  .pay-txt { flex: 1; display: flex; flex-direction: column; font-size: 13px; color: var(--sub); }
  .pay-txt b { font-size: 16px; color: var(--ink); }
  .pay-amt { font-size: 17px; }
  .link-box { background: var(--ground); border-radius: 14px; padding: 12px 14px; font-family: var(--mono); font-size: 12px; word-break: break-all; color: #3F3C37; }
  .pay-btns { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; }
  .waiting { display: flex; align-items: center; justify-content: center; gap: 8px; font-size: 13px; color: var(--sub); }
  .pulse { width: 8px; height: 8px; border-radius: 4px; background: #F5B301; animation: pulse 1.2s ease-in-out infinite; }
  @keyframes pulse { 50% { opacity: 0.3; } }
  .done { display: flex; flex-direction: column; align-items: center; gap: 6px; text-align: center; padding: 6px 0; }
  .tick { width: 60px; height: 60px; border-radius: 30px; background: #1E7B34; color: #fff; display: grid; place-items: center; }
  .done h3 { font-size: 20px; font-weight: 800; }
  .order { font-family: var(--mono); font-size: 12px; color: var(--sub); }
  .err { font-size: 14px; color: #9B2C20; background: #FBEAE7; border-radius: 12px; padding: 10px 12px; }
  .spinner { width: 18px; height: 18px; border-radius: 50%; border: 2.4px solid #D9D4C7; border-top-color: var(--ink); animation: spin 0.8s linear infinite; }
  @keyframes spin { to { transform: rotate(360deg); } }

  button:focus-visible, select:focus-visible { outline: 3px solid var(--ink); outline-offset: 2px; }
  .sr { position: absolute; width: 1px; height: 1px; overflow: hidden; clip: rect(0 0 0 0); white-space: nowrap; }
</style>
