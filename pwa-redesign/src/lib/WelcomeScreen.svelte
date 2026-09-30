<script>
  import { colors } from "./colors.js";
  import WiFiSetup from "./WiFiSetup.svelte";
  import APModeConfig from "./APModeConfig.svelte";
  import HomeScreen from "./HomeScreen.svelte";
  import PupperEyes from "./PupperEyes.svelte";

  let { network } = $props();

  const YELLOW = colors.mpx.primary;

  /** Current view: "welcome" | "wifi" | "ap" | "home" */
  let view = $state("welcome");

  // ── Derived network state ──────────────────────────────────
  let isStaConnected = $derived(network?.staState === "connected");
  let linkState = $derived(network?.linkState ?? "checking");
  let online = $derived(linkState === "online");

  let displayIp = $derived(
    network?.connectionMode === "sta" && network?.staIp ? network.staIp
    : network?.apIp || "192.168.2.1"
  );

  let staLabel = $derived(
    isStaConnected ? (network?.staSsid || "Connected")
    : network?.staState === "connecting" ? "Connecting…"
    : network?.staState === "failed" ? "Connection failed"
    : "Not connected"
  );

  let linkLabel = $derived(
    online ? `Online${network?.latencyMs != null ? ` · ${network.latencyMs} ms` : ""}`
    : linkState === "offline" ? "Unreachable"
    : "Checking…"
  );

  let modeLabel = $derived(
    network?.connectionMode === "ap" ? "Direct (AP)"
    : network?.connectionMode === "sta" ? "Wi‑Fi (STA)"
    : network?.connectionMode === "dev" ? "Dev proxy"
    : "—"
  );

  function onNavigate(next) { view = next; }
</script>

<div class="welcome-root" style="--brand-yellow: {YELLOW}">
  {#if view === "home"}
    <HomeScreen {network} />
  {:else if view === "wifi"}
    <WiFiSetup {onNavigate} {network} />
  {:else if view === "ap"}
    <APModeConfig {onNavigate} {network} />
  {:else}
    <!-- ═══ WELCOME VIEW ═══ -->
    <div class="welcome-view">
      <div class="welcome-inner">
        <!-- Header -->
        <header class="welcome-header">
          <img class="brand-title" src="/md.svg" alt="MangDang" />
          <div class="link-chip" class:online class:offline={linkState === "offline"}>
            <span class="link-dot"></span>
            {online ? "Connected" : linkState === "offline" ? "Offline" : "Connecting"}
          </div>
        </header>

        <div class="welcome-grid">
          <!-- ── Left column: pupper card + start ── -->
          <section class="col col-hero">
            <div class="pupper-card">
              <div class="pupper-card-top">
                <div class="pupper-avatar">
                  <PupperEyes awake={online} size="100%" />
                </div>
                <div class="pupper-info">
                  <span class="pupper-label">Your Pupper</span>
                  <span class="pupper-nickname">My MPX Dog</span>
                  <span class="pupper-mode">{modeLabel}</span>
                </div>
              </div>

              <div class="pupper-meta">
                <div class="meta-item">
                  <span class="meta-key">IP</span>
                  <span class="meta-val mono">{displayIp}</span>
                </div>
                <div class="meta-item">
                  <span class="meta-key">Hotspot</span>
                  <span class="meta-val">{network?.apSsid || "MPX-Dog"}</span>
                </div>
              </div>
            </div>

            <button class="start-btn" onclick={() => (view = "home")}>
              <span>{online ? "Start" : "Continue offline"}</span>
              <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.6" stroke-linecap="round" stroke-linejoin="round">
                <path d="M5 12h14M13 5l7 7-7 7"/>
              </svg>
            </button>
            {#if !online}
              <p class="offline-note">
                The robot isn't answering right now. Controls will work as soon as it's back.
              </p>
            {/if}
          </section>

          <!-- ── Right column: connection ── -->
          <section class="col col-conn">
            <h2 class="section-title">Connection</h2>

            <div class="connection-list">
              <!-- Robot link -->
              <div class="conn-item">
                <div class="conn-left">
                  <div class="conn-icon" class:ok={online} class:bad={linkState === "offline"}>
                    <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
                      <rect x="2" y="2" width="20" height="8" rx="2" ry="2"/>
                      <rect x="2" y="14" width="20" height="8" rx="2" ry="2"/>
                      <line x1="6" y1="6" x2="6.01" y2="6"/>
                      <line x1="6" y1="18" x2="6.01" y2="18"/>
                    </svg>
                  </div>
                  <div class="conn-text">
                    <span class="conn-name">Robot link</span>
                    <span class="conn-detail">{linkLabel}</span>
                  </div>
                </div>
                <div class="conn-status">
                  {#if online}
                    <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="var(--mpx-ok)" stroke-width="3" stroke-linecap="round" stroke-linejoin="round">
                      <path d="M5 13l4 4L19 7"/>
                    </svg>
                  {:else if linkState === "offline"}
                    <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="var(--mpx-err)" stroke-width="3" stroke-linecap="round" stroke-linejoin="round">
                      <line x1="18" y1="6" x2="6" y2="18"/><line x1="6" y1="6" x2="18" y2="18"/>
                    </svg>
                  {:else}
                    <span class="spinner"></span>
                  {/if}
                </div>
              </div>

              <!-- Wi-Fi -->
              <button class="conn-item conn-clickable" onclick={() => (view = "wifi")}>
                <div class="conn-left">
                  <div class="conn-icon" class:ok={isStaConnected}>
                    <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
                      <path d="M5 12.55a11 11 0 0 1 14.08 0"/>
                      <path d="M1.42 9a16 16 0 0 1 21.16 0"/>
                      <path d="M8.53 16.11a6 6 0 0 1 6.95 0"/>
                      <circle cx="12" cy="20" r="1" fill="currentColor"/>
                    </svg>
                  </div>
                  <div class="conn-text">
                    <span class="conn-name">Wi‑Fi</span>
                    <span class="conn-detail">{staLabel}{isStaConnected && network?.staIp ? ` · ${network.staIp}` : ""}</span>
                  </div>
                </div>
                <div class="conn-arrow">
                  <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
                    <path d="M9 18l6-6-6-6"/>
                  </svg>
                </div>
              </button>

              <!-- AP Mode -->
              <button class="conn-item conn-clickable" onclick={() => (view = "ap")}>
                <div class="conn-left">
                  <div class="conn-icon">
                    <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
                      <path d="M12 2a10 10 0 0 1 10 10"/>
                      <path d="M12 6a6 6 0 0 1 6 6"/>
                      <path d="M12 10a2 2 0 0 1 2 2"/>
                      <circle cx="12" cy="20" r="1.5" fill="currentColor"/>
                    </svg>
                  </div>
                  <div class="conn-text">
                    <span class="conn-name">Hotspot (AP)</span>
                    <span class="conn-detail">{network?.apSsid || "MPX-Dog"} · {network?.apIp || "192.168.2.1"}</span>
                  </div>
                </div>
                <div class="conn-arrow">
                  <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
                    <path d="M9 18l6-6-6-6"/>
                  </svg>
                </div>
              </button>
            </div>
          </section>
        </div>
      </div>
    </div>
  {/if}
</div>

<style>
  .welcome-root {
    position: absolute;
    inset: 0;
    background: var(--mpx-surface);
    display: flex;
    flex-direction: column;
    animation: welcome-in var(--dur-slow) var(--ease-out);
  }
  @keyframes welcome-in { from { opacity: 0; } to { opacity: 1; } }

  .welcome-view {
    height: 100%;
    overflow-y: auto;
    -webkit-overflow-scrolling: touch;
    padding: calc(var(--safe-top) + 12px) clamp(16px, 4vw, 28px) calc(var(--safe-bottom) + 20px);
  }

  .welcome-inner {
    max-width: var(--page-max);
    margin: 0 auto;
    min-height: 100%;
    display: flex;
    flex-direction: column;
  }

  /* ── Header ─────────────────────────────── */
  .welcome-header {
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 8px 0 18px;
  }
  .brand-title { width: clamp(120px, 34vw, 150px); height: auto; display: block; }

  .link-chip {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    padding: 5px 11px 5px 8px;
    border-radius: var(--r-pill);
    background: var(--mpx-surface-2);
    color: var(--mpx-muted);
    font-size: 0.7rem;
    font-weight: 700;
  }
  .link-dot { width: 7px; height: 7px; border-radius: 50%; background: var(--mpx-muted-2); animation: dot-pulse 1.4s infinite; }
  .link-chip.online { color: #15803d; background: #ecfdf3; }
  .link-chip.online .link-dot { background: var(--mpx-ok); animation: none; }
  .link-chip.offline { color: #b42318; background: #fef3f2; }
  .link-chip.offline .link-dot { background: var(--mpx-err); animation: none; }
  @keyframes dot-pulse { 0%,100% { opacity: 1; } 50% { opacity: 0.35; } }

  /* ── Grid ───────────────────────────────── */
  .welcome-grid {
    display: flex;
    flex-direction: column;
    gap: 22px;
    flex: 1;
  }
  .col { display: flex; flex-direction: column; gap: 14px; min-width: 0; }
  .col-hero { order: 1; }
  .col-conn { order: 2; }

  /* Push the start button to the bottom on tall phones */
  .col-hero .start-btn { margin-top: auto; }

  @media (min-width: 820px) {
    .welcome-inner { max-width: 960px; }
    .welcome-grid {
      display: grid;
      grid-template-columns: 1.1fr 1fr;
      gap: 32px;
      align-items: start;
    }
    .col-hero .start-btn { margin-top: 4px; }
  }

  /* ── Pupper Card ────────────────────────── */
  .pupper-card {
    position: relative;
    background: var(--brand-yellow);
    border-radius: var(--r-xl);
    padding: clamp(18px, 4vw, 26px);
    display: flex;
    flex-direction: column;
    gap: 18px;
    overflow: hidden;
    box-shadow: var(--shadow-card);
  }
  .pupper-card::after {
    content: '';
    position: absolute;
    inset: 0;
    background: radial-gradient(ellipse 70% 60% at 85% 10%, rgba(255,255,255,0.45), transparent 60%);
    pointer-events: none;
  }
  .pupper-card-top {
    display: flex;
    align-items: center;
    gap: 16px;
    position: relative;
    z-index: 1;
  }
  .pupper-avatar {
    flex-shrink: 0;
    width: clamp(56px, 14vw, 72px);
    height: clamp(56px, 14vw, 72px);
    border-radius: var(--r-lg);
    background: rgba(0, 0, 0, 0.08);
    display: grid;
    place-items: center;
    padding: 12px;
  }
  .pupper-info { display: flex; flex-direction: column; gap: 2px; min-width: 0; }
  .pupper-label {
    font-size: 0.68rem;
    font-weight: 800;
    color: rgba(0,0,0,0.55);
    text-transform: uppercase;
    letter-spacing: 0.08em;
  }
  .pupper-nickname { font-size: clamp(1.2rem, 4.5vw, 1.45rem); font-weight: 800; color: var(--mpx-ink); line-height: 1.15; }
  .pupper-mode { font-size: 0.78rem; font-weight: 600; color: rgba(0,0,0,0.6); }

  .pupper-meta {
    position: relative;
    z-index: 1;
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 8px;
  }
  .meta-item {
    background: rgba(0, 0, 0, 0.08);
    border-radius: var(--r-md);
    padding: 10px 12px;
    display: flex;
    flex-direction: column;
    gap: 2px;
    min-width: 0;
  }
  .meta-key { font-size: 0.64rem; font-weight: 800; text-transform: uppercase; letter-spacing: 0.08em; color: rgba(0,0,0,0.5); }
  .meta-val { font-size: 0.86rem; font-weight: 700; color: var(--mpx-ink); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  .mono { font-family: 'JetBrains Mono', ui-monospace, monospace; font-weight: 600; }

  /* ── Connection Section ─────────────────── */
  .section-title {
    font-size: 0.72rem;
    font-weight: 800;
    color: var(--mpx-muted);
    text-transform: uppercase;
    letter-spacing: 0.08em;
    padding-left: 4px;
  }

  .connection-list {
    display: flex;
    flex-direction: column;
    background: var(--mpx-surface-2);
    border-radius: var(--r-lg);
    overflow: hidden;
  }
  .conn-item {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 12px;
    padding: 14px 16px;
    background: var(--mpx-surface-2);
    transition: background var(--dur-fast);
  }
  .conn-item + .conn-item { border-top: 1px solid rgba(0,0,0,0.05); }
  .conn-clickable {
    cursor: pointer;
    border: none;
    text-align: left;
    width: 100%;
  }
  .conn-clickable:hover { background: #efefef; }
  .conn-clickable:active { background: #e6e6e6; }

  .conn-left { display: flex; align-items: center; gap: 12px; min-width: 0; }
  .conn-icon {
    width: 38px;
    height: 38px;
    border-radius: var(--r-sm);
    display: grid;
    place-items: center;
    flex-shrink: 0;
    background: rgba(0,0,0,0.07);
    color: var(--mpx-ink);
    transition: background var(--dur-med), color var(--dur-med);
  }
  .conn-icon.ok { background: var(--brand-yellow); }
  .conn-icon.bad { background: #fde8e8; color: #b42318; }

  .conn-text { display: flex; flex-direction: column; gap: 2px; min-width: 0; }
  .conn-name { font-size: 0.95rem; font-weight: 700; color: var(--mpx-ink); }
  .conn-detail { font-size: 0.78rem; color: var(--mpx-muted); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }

  .conn-status, .conn-arrow { display: flex; align-items: center; color: var(--mpx-muted-2); flex-shrink: 0; }

  .spinner {
    width: 14px; height: 14px;
    border-radius: 50%;
    border: 2px solid rgba(0,0,0,0.12);
    border-top-color: var(--mpx-ink);
    animation: spin 0.8s linear infinite;
  }
  @keyframes spin { to { transform: rotate(360deg); } }

  /* ── Start Button ───────────────────────── */
  .start-btn {
    width: 100%;
    padding: 16px 20px;
    border: none;
    border-radius: var(--r-lg);
    background: var(--mpx-ink);
    color: #fff;
    font-size: 1.02rem;
    font-weight: 800;
    cursor: pointer;
    display: flex;
    align-items: center;
    justify-content: space-between;
    transition: transform var(--dur-fast) var(--ease-out), box-shadow var(--dur-fast);
    box-shadow: 0 8px 20px rgba(0,0,0,0.18);
  }
  .start-btn:hover { box-shadow: 0 10px 26px rgba(0,0,0,0.24); }
  .start-btn:active { transform: scale(0.985); }

  .offline-note {
    font-size: 0.76rem;
    color: var(--mpx-muted);
    line-height: 1.45;
    text-align: center;
    padding: 0 8px;
  }
</style>
