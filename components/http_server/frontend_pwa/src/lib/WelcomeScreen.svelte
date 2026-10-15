<script>
  import Wordmark from "./Wordmark.svelte";
  import Eyes from "./Eyes.svelte";
  import WiFiSetup from "./WiFiSetup.svelte";
  import APModeConfig from "./APModeConfig.svelte";
  import HomeScreen from "./HomeScreen.svelte";

  let { network } = $props();


  /** Current view: "welcome" | "wifi" | "ap" | "home" */
  let view = $state("welcome");

  // ── Derived network state ──────────────────────────────────
  let isStaConnected = $derived(network?.staState === "connected");
  let displayIp = $derived(network?.apIp || "192.168.2.1");
  let staLabel = $derived(
    isStaConnected ? (network?.staSsid || "Connected")
    : network?.staState === "connecting" ? "Connecting…"
    : "OFF"
  );

  function onNavigate(next) {
    view = next;
  }

  function goToWifi() {
    view = "wifi";
  }

  function goToAP() {
    view = "ap";
  }
</script>

<div class="welcome-root">
  {#if view === "home"}
    <HomeScreen {network} />
  {:else if view === "wifi"}
    <WiFiSetup {onNavigate} {network} />
  {:else if view === "ap"}
    <APModeConfig {onNavigate} {network} />
  {:else}
    <!-- ═══ WELCOME VIEW ═══ -->
    <div class="welcome-view">
      <!-- Header -->
      <div class="welcome-header">
        <Wordmark height={22} />
      </div>

      <!-- Your Pupper Card -->
      <div class="pupper-card shell">
        <div class="core pupper-core">
        <div class="pupper-card-top">
          <div class="pupper-avatar">
            <span class="eyes-blink pupper-eyes"><Eyes size={40} /></span>
          </div>
          <div class="pupper-info">
            <span class="pupper-label">Your Pupper</span>
            <span class="pupper-nickname">My MPX Dog</span>
          </div>
        </div>
        <div class="pupper-ip">
          <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
            <circle cx="12" cy="12" r="10"/>
            <line x1="2" y1="12" x2="22" y2="12"/>
            <path d="M12 2a15.3 15.3 0 0 1 4 10 15.3 15.3 0 0 1-4 10 15.3 15.3 0 0 1-4-10 15.3 15.3 0 0 1 4-10z"/>
          </svg>
          <span>IP: {displayIp}</span>
        </div>
        </div>
      </div>

      <!-- Connection Section -->
      <div class="connection-section">
        <h2 class="section-title">Connection</h2>

        <div class="connection-list">
          <!-- LAN -->
          <div class="conn-item">
            <div class="conn-left">
              <div class="conn-icon conn-icon-lan">
                <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
                  <rect x="2" y="2" width="20" height="8" rx="2" ry="2"/>
                  <rect x="2" y="14" width="20" height="8" rx="2" ry="2"/>
                  <line x1="6" y1="6" x2="6.01" y2="6"/>
                  <line x1="6" y1="18" x2="6.01" y2="18"/>
                </svg>
              </div>
              <div class="conn-text">
                <span class="conn-name">LAN</span>
                <span class="conn-detail">Connected</span>
              </div>
            </div>
            <div class="conn-status">
              <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="var(--ok)" stroke-width="3" stroke-linecap="round" stroke-linejoin="round">
                <path d="M5 13l4 4L19 7"/>
              </svg>
            </div>
          </div>

          <!-- Wi-Fi -->
          <button class="conn-item conn-clickable" onclick={goToWifi}>
            <div class="conn-left">
              <div class="conn-icon conn-icon-wifi">
                <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
                  <path d="M5 12.55a11 11 0 0 1 14.08 0"/>
                  <path d="M1.42 9a16 16 0 0 1 21.16 0"/>
                  <path d="M8.53 16.11a6 6 0 0 1 6.95 0"/>
                  <circle cx="12" cy="20" r="1" fill="currentColor"/>
                </svg>
              </div>
              <div class="conn-text">
                <span class="conn-name">Wi‑Fi</span>
                <span class="conn-detail">{staLabel}</span>
              </div>
            </div>
            <div class="conn-arrow">
              <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="var(--text-3)" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
                <path d="M9 18l6-6-6-6"/>
              </svg>
            </div>
          </button>

          <!-- AP Mode -->
          <button class="conn-item conn-clickable" onclick={goToAP}>
            <div class="conn-left">
              <div class="conn-icon conn-icon-ap">
                <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
                  <path d="M12 2a10 10 0 0 1 10 10"/>
                  <path d="M12 6a6 6 0 0 1 6 6"/>
                  <path d="M12 10a2 2 0 0 1 2 2"/>
                  <circle cx="12" cy="20" r="1.5" fill="currentColor"/>
                </svg>
              </div>
              <div class="conn-text">
                <span class="conn-name">AP Mode</span>
                <span class="conn-detail">{network?.apSsid || "MPX-Dog"} · {displayIp}</span>
              </div>
            </div>
            <div class="conn-arrow">
              <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="var(--text-3)" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
                <path d="M9 18l6-6-6-6"/>
              </svg>
            </div>
          </button>
        </div>
      </div>

      <!-- Start Button -->
      <button class="start-btn md-btn md-btn-primary" onclick={() => view = "home"}>
        Let's go
        <span class="md-btn-icon"><svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><path d="M5 12h14M13 5l7 7-7 7"/></svg></span>
      </button>
    </div>
  {/if}
</div>

<style>
  .welcome-root {
    position: absolute;
    inset: 0;
    background: var(--bg);
    overflow: hidden;
  }
  .welcome-view {
    height: 100%;
    display: flex;
    flex-direction: column;
    padding: max(20px, env(safe-area-inset-top)) 20px max(20px, env(safe-area-inset-bottom));
    gap: 20px;
    overflow-y: auto;
  }
  .welcome-header {
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 6px 2px;
    color: var(--text-1);
  }

  /* ── The robot ──────────────────────────────────────────────────────── */
  .pupper-card { padding: 6px; }
  .pupper-core { padding: 18px; }
  .pupper-card-top {
    display: flex;
    align-items: center;
    gap: 14px;
  }
  .pupper-avatar {
    width: 60px;
    height: 60px;
    border-radius: 18px;
    background: radial-gradient(circle at 50% 30%, rgba(255,255,255,0.10), transparent 60%), var(--surface-2);
    border: 1px solid var(--line);
    box-shadow: inset 0 1px 0 var(--highlight);
    color: var(--accent);
    display: flex;
    align-items: center;
    justify-content: center;
    flex-shrink: 0;
  }
  .pupper-info { display: flex; flex-direction: column; gap: 2px; min-width: 0; }
  .pupper-label {
    font-size: 12px;
    font-weight: 600;
    letter-spacing: 0.04em;
    text-transform: uppercase;
    color: var(--text-3);
  }
  .pupper-nickname {
    font-size: 22px;
    font-weight: 700;
    letter-spacing: -0.02em;
    color: var(--text-1);
  }
  .pupper-eyes { display: inline-flex; filter: drop-shadow(0 0 8px rgba(255, 230, 5, 0.5)); }
  .pupper-ip {
    margin-top: 16px;
    padding-top: 14px;
    border-top: 1px solid var(--line);
    display: flex;
    align-items: center;
    gap: 8px;
    font-family: var(--font-mono);
    font-size: 13px;
    color: var(--text-2);
  }

  /* ── Connection ─────────────────────────────────────────────────────── */
  .connection-section { display: flex; flex-direction: column; gap: 10px; }
  .section-title {
    font-size: 12px;
    font-weight: 600;
    letter-spacing: 0.04em;
    text-transform: uppercase;
    color: var(--text-3);
    padding: 0 2px;
  }
  .connection-list {
    background: var(--surface-1);
    border: 1px solid var(--line);
    border-radius: var(--r-lg);
    box-shadow: inset 0 1px 0 var(--highlight), var(--shadow-card);
    overflow: hidden;
  }
  .conn-item {
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 14px 16px;
    width: 100%;
    background: none;
    border: none;
    text-align: left;
    color: var(--text-1);
  }
  .conn-item + .conn-item { border-top: 1px solid var(--line); }
  .conn-clickable { cursor: pointer; transition: background-color 0.12s; }
  .conn-clickable:active { background: var(--surface-2); }
  .conn-left { display: flex; align-items: center; gap: 14px; min-width: 0; }
  .conn-icon {
    width: 40px;
    height: 40px;
    border-radius: 13px;
    background: var(--surface-2);
    color: var(--text-2);
    display: flex;
    align-items: center;
    justify-content: center;
    flex-shrink: 0;
  }
  .conn-text { display: flex; flex-direction: column; gap: 2px; min-width: 0; }
  .conn-name { font-size: 15px; font-weight: 600; }
  .conn-detail {
    font-size: 13px;
    color: var(--text-2);
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .conn-status, .conn-arrow { display: flex; align-items: center; flex-shrink: 0; }

  .start-btn {
    margin-top: auto;
    width: 100%;
    height: 56px;
    font-size: 17px;
  }
</style>
