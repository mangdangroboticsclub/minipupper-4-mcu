<script>
  import { colors } from "./colors.js";

  /** @type {{ onNavigate?: (view: string) => void, network?: object }} */
  let { onNavigate, network } = $props();

  const YELLOW = colors.mpx.primary;

  // ── State ──────────────────────────────────────────────────
  let loading = $state(true);
  let error = $state("");

  // AP info
  let apSsid = $state(network?.apSsid || "MPX-Dog");
  let apIp = $state(network?.apIp || "192.168.2.1");

  // STA info
  let staState = $state(network?.staState || "disconnected");
  let staSsid = $state(network?.staSsid || "");
  let staIp = $state(network?.staIp || "");

  // Connect form
  let inputSsid = $state("");
  let inputPassword = $state("");
  let connecting = $state(false);
  let reconnecting = $state(false);
  let connectError = $state("");

  // Poll timer for reconnection
  let pollTimer = $state(null);

  // ── Derived ─────────────────────────────────────────────────
  let wifiOn = $derived(staState !== "disconnected");
  let showConnectForm = $state(false);

  // ── API helpers ────────────────────────────────────────────

  async function fetchStatus() {
    loading = true;
    error = "";
    try {
      const res = await fetch("/v1/wifi/status");
      if (res.ok) {
        const data = await res.json();
        apSsid = data.ap?.ssid ?? "MPX-Dog";
        apIp = data.ap?.ip ?? "192.168.2.1";
        staState = data.sta?.state ?? "disconnected";
        staSsid = data.sta?.ssid ?? "";
        staIp = data.sta?.ip ?? "";
        if (reconnecting) reconnecting = false;
      } else if (!reconnecting) {
        error = `Status fetch failed (${res.status})`;
      }
    } catch (e) {
      if (!reconnecting) error = `Cannot reach robot: ${e.message}`;
    }
    loading = false;
  }

  function startReconnectPoller() {
    if (pollTimer) clearTimeout(pollTimer);
    async function poll() {
      try {
        const res = await fetch("/v1/wifi/status");
        if (res.ok) {
          const data = await res.json();
          const state = data.sta?.state ?? "disconnected";
          staState = state;
          staSsid = data.sta?.ssid ?? staSsid;
          staIp = data.sta?.ip ?? "";
          if (state === "connected" || state === "failed" || state === "disconnected") {
            reconnecting = false;
            connecting = false;
            return;
          }
        }
      } catch {}
      pollTimer = setTimeout(poll, 2000);
    }
    poll();
  }

  async function doConnect() {
    const ssid = inputSsid.trim();
    if (!ssid) return;
    connecting = true;
    reconnecting = false;
    connectError = "";
    let requestSent = false;
    try {
      const res = await fetch("/v1/wifi/connect", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ ssid, password: inputPassword }),
      });
      requestSent = true;
      if (res.ok) {
        staState = "connecting";
        staSsid = ssid;
        reconnecting = true;
        startReconnectPoller();
      } else {
        const text = await res.text();
        connectError = text || `Connect failed (${res.status})`;
        connecting = false;
      }
    } catch (e) {
      if (!requestSent) {
        staState = "connecting";
        staSsid = ssid;
        reconnecting = true;
        startReconnectPoller();
      } else {
        connectError = `Connection lost: ${e.message}`;
        connecting = false;
      }
    }
  }

  async function doDisconnect() {
    try {
      await fetch("/v1/wifi/disconnect", { method: "POST" });
      staState = "disconnected";
      staSsid = "";
      staIp = "";
      showConnectForm = false;
    } catch (e) {
      console.error("Disconnect failed:", e);
    }
  }

  async function doForget() {
    try {
      await fetch("/v1/wifi/forget", { method: "POST" });
      staState = "disconnected";
      staSsid = "";
      staIp = "";
      showConnectForm = false;
    } catch (e) {
      console.error("Forget failed:", e);
    }
  }

  function toggleWifi() {
    if (wifiOn) {
      doDisconnect();
    } else {
      showConnectForm = true;
    }
  }

  function goBack() {
    onNavigate?.("welcome");
  }

  // ── Init ───────────────────────────────────────────────────
  $effect(() => {
    fetchStatus();
    return () => { if (pollTimer) clearTimeout(pollTimer); };
  });
</script>

<div class="wifi-root">
  <!-- ═══ Header ═══ -->
  <div class="header">
    <button class="back-btn" onclick={goBack} aria-label="Back">
      <svg width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
        <path d="M19 12H5M12 19l-7-7 7-7"/>
      </svg>
    </button>
    <h1 class="header-title">WiFi</h1>
    <button onclick={fetchStatus} class="refresh-btn" aria-label="Refresh">↻</button>
  </div>

  <!-- ═══ Loading ═══ -->
  {#if loading && !reconnecting}
    <div class="scanning">
      <span class="spinner"></span>
      <span>Loading status…</span>
    </div>
  {:else if reconnecting}
    <!-- ═══ Reconfiguring ═══ -->
    <div class="reconnecting">
      <div class="recon-icon">📡</div>
      <h3 class="recon-title">Reconfiguring Network</h3>
      <p class="recon-desc">
        The robot is restarting its network to connect to <strong>{staSsid}</strong>.
      </p>
      <p class="recon-hint">
        Your device may have disconnected. Reconnect to <strong>MPX-Dog</strong> WiFi if needed.
      </p>
      <button class="cancel-btn" onclick={() => { if (pollTimer) clearTimeout(pollTimer); reconnecting = false; connecting = false; fetchStatus(); }}>
        Cancel
      </button>
    </div>
  {:else if error}
    <div class="scanning" style="color: #d00">{error}</div>
    <button class="retry-btn" onclick={fetchStatus}>Retry</button>
  {:else}
    <!-- ═══ AP Info ═══ -->
    <div class="info-card">
      <div class="info-row">
        <span class="info-label">🔵 AP Mode</span>
        <span class="info-value">{apSsid} · {apIp}</span>
      </div>
    </div>

    <!-- ═══ STA Status ═══ -->
    <div class="info-card">
      <div class="info-row">
        <span class="info-label">📡 Station Mode</span>
        <span class="info-value">
          {#if staState === "connected"}
            <span class="status-dot status-ok"></span> {staSsid} · {staIp}
          {:else if staState === "connecting"}
            <span class="status-dot status-busy"></span> Connecting to {staSsid}…
          {:else if staState === "failed"}
            <span class="status-dot status-err"></span> Failed
          {:else}
            <span class="status-dot status-off"></span> Disconnected
          {/if}
        </span>
      </div>
    </div>

    <!-- ═══ Toggle & Actions ═══ -->
    <div class="wifi-toggle-card">
      <div class="toggle-row">
        <div class="toggle-label">
          <svg class="toggle-icon" width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
            <path d="M5 12.55a11 11 0 0 1 14.08 0"/>
            <path d="M1.42 9a16 16 0 0 1 21.16 0"/>
            <path d="M8.53 16.11a6 6 0 0 1 6.95 0"/>
            <circle cx="12" cy="20" r="1" fill="currentColor"/>
          </svg>
          <span>Wi‑Fi</span>
        </div>
        <button
          class="toggle-switch"
          class:active={wifiOn}
          onclick={toggleWifi}
          role="switch"
          aria-checked={wifiOn}
          aria-label={wifiOn ? "Disable Wi-Fi" : "Enable Wi-Fi"}
        >
          <span class="toggle-knob"></span>
        </button>
      </div>
    </div>

    {#if wifiOn}
      <button class="action-btn" onclick={doForget}>Forget Network</button>
    {/if}

    <!-- ═══ Connect Form ═══ -->
    {#if showConnectForm || !wifiOn}
      <div class="connect-form">
        <p class="form-hint">Connect to a Wi‑Fi network to give your robot internet access.</p>

        <div class="field">
          <label class="field-label" for="wifi-ssid">Network Name (SSID)</label>
          <input
            id="wifi-ssid"
            class="field-input"
            type="text"
            bind:value={inputSsid}
            placeholder="Enter network name"
          />
        </div>

        <div class="field">
          <label class="field-label" for="wifi-password">Password</label>
          <input
            id="wifi-password"
            class="field-input"
            type="password"
            bind:value={inputPassword}
            placeholder="Enter password"
          />
        </div>

        <button
          class="join-btn"
          onclick={doConnect}
          disabled={connecting || !inputSsid.trim()}
        >
          {#if connecting}
            <span class="spinner"></span>
          {:else}
            Connect
          {/if}
        </button>
      </div>
    {/if}

    <!-- ═══ Error Toast ═══ -->
    {#if connectError}
      <div class="error-toast">
        <span>{connectError}</span>
        <button class="error-ok" onclick={() => connectError = ""}>Ok</button>
      </div>
    {/if}
  {/if}
</div>

<style>
  .wifi-root {
    position: absolute;
    inset: 0;
    background: #fff;
    display: flex;
    flex-direction: column;
    padding: 0 20px;
    overflow-y: auto;
  }

  /* ── Header ─────────────────────────────── */
  .header {
    display: flex;
    align-items: center;
    gap: 8px;
    padding: 16px 0 8px;
  }

  .back-btn {
    width: 36px;
    height: 36px;
    border: none;
    background: transparent;
    border-radius: 50%;
    display: flex;
    align-items: center;
    justify-content: center;
    cursor: pointer;
    color: #000;
    flex-shrink: 0;
  }
  .back-btn:hover {
    background: rgba(0,0,0,0.05);
  }

  .header-title {
    font-size: 1.25rem;
    font-weight: 700;
    color: #000;
  }

  /* ── Toggle Card ────────────────────────── */
  .wifi-toggle-card {
    background: #f5f5f5;
    border-radius: 14px;
    padding: 12px 16px;
    margin: 8px 0 12px;
  }

  .toggle-row {
    display: flex;
    align-items: center;
    justify-content: space-between;
  }

  .toggle-label {
    display: flex;
    align-items: center;
    gap: 10px;
    font-size: 1rem;
    font-weight: 600;
    color: #000;
  }

  .toggle-icon {
    color: #000;
  }

  .toggle-switch {
    width: 48px;
    height: 28px;
    border-radius: 14px;
    border: none;
    background: #ccc;
    cursor: pointer;
    position: relative;
    transition: background 0.2s ease;
    padding: 0;
  }
  .toggle-switch.active {
    background: #000;
  }

  .toggle-knob {
    position: absolute;
    top: 3px;
    left: 3px;
    width: 22px;
    height: 22px;
    border-radius: 50%;
    background: #fff;
    box-shadow: 0 1px 3px rgba(0,0,0,0.2);
    transition: transform 0.2s ease;
  }
  .toggle-switch.active .toggle-knob {
    transform: translateX(20px);
  }

  /* ── Scanning ───────────────────────────── */
  .scanning {
    display: flex;
    align-items: center;
    gap: 10px;
    padding: 20px 0;
    font-size: 0.9rem;
    color: #666;
  }

  .spinner {
    width: 18px;
    height: 18px;
    border: 2.5px solid #ccc;
    border-top-color: #000;
    border-radius: 50%;
    animation: spin 0.7s linear infinite;
    display: inline-block;
  }

  @keyframes spin {
    to { transform: rotate(360deg); }
  }

  /* ── Section Label ──────────────────────── */
  .section-label {
    font-size: 0.85rem;
    font-weight: 700;
    color: #000;
    margin: 12px 0 8px;
  }

  /* ── Network List ───────────────────────── */
  .network-list {
    display: flex;
    flex-direction: column;
    gap: 2px;
  }

  .network-item {
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 14px 16px;
    background: #f5f5f5;
    border: none;
    border-radius: 12px;
    cursor: pointer;
    text-align: left;
    transition: background 0.15s;
  }
  .network-item:hover {
    background: #eee;
  }
  .network-item:active {
    background: #e5e5e5;
  }

  .net-info {
    display: flex;
    align-items: center;
    gap: 10px;
  }

  .net-icon {
    color: #000;
    flex-shrink: 0;
  }

  .net-ssid {
    font-size: 1rem;
    font-weight: 500;
    color: #000;
  }

  .net-meta {
    display: flex;
    align-items: center;
    gap: 8px;
  }

  .lock-icon {
    color: #999;
  }

  .signal-bars {
    display: flex;
    align-items: flex-end;
    gap: 2px;
    height: 14px;
  }

  .signal-bar {
    width: 4px;
    border-radius: 2px;
    background: #ddd;
  }
  .signal-bar:nth-child(1) { height: 5px; }
  .signal-bar:nth-child(2) { height: 8px; }
  .signal-bar:nth-child(3) { height: 11px; }
  .signal-bar:nth-child(4) { height: 14px; }
  .signal-bar.active {
    background: #000;
  }

  /* ── Connect Form ───────────────────────── */
  .connect-form {
    margin-top: 8px;
    display: flex;
    flex-direction: column;
    gap: 14px;
  }

  .form-hint {
    font-size: 0.85rem;
    color: #666;
    line-height: 1.5;
  }

  .field {
    display: flex;
    flex-direction: column;
    gap: 6px;
  }

  .field-label {
    font-size: 0.85rem;
    font-weight: 600;
    color: #000;
  }

  .field-input {
    width: 100%;
    padding: 12px 14px;
    border: 1.5px solid #ddd;
    border-radius: 10px;
    font-size: 1rem;
    color: #000;
    background: #fff;
    outline: none;
    transition: border-color 0.15s;
  }
  .field-input:focus {
    border-color: #000;
  }
  .field-input::placeholder {
    color: #bbb;
  }

  .join-btn {
    align-self: flex-end;
    width: 48px;
    height: 48px;
    border-radius: 50%;
    border: none;
    background: #000;
    color: #fff;
    display: flex;
    align-items: center;
    justify-content: center;
    cursor: pointer;
    transition: background 0.15s, transform 0.1s;
    margin-top: 8px;
  }
  .join-btn:hover {
    background: #222;
  }
  .join-btn:active {
    transform: scale(0.95);
  }
  .join-btn:disabled {
    background: #ccc;
    cursor: not-allowed;
  }

  /* ── Error Toast ────────────────────────── */
  .error-toast {
    position: fixed;
    bottom: 24px;
    left: 20px;
    right: 20px;
    background: #fff;
    border-radius: 14px;
    padding: 16px 20px;
    display: flex;
    align-items: center;
    justify-content: space-between;
    box-shadow: 0 4px 24px rgba(0,0,0,0.15);
    font-size: 0.9rem;
    color: #000;
    z-index: 100;
    animation: slideUp 0.3s ease;
  }

  @keyframes slideUp {
    from { transform: translateY(20px); opacity: 0; }
    to { transform: translateY(0); opacity: 1; }
  }

  .error-ok {
    padding: 6px 16px;
    border: none;
    border-radius: 8px;
    background: #000;
    color: #fff;
    font-weight: 600;
    font-size: 0.85rem;
    cursor: pointer;
  }

  /* ── Status dots ────────────────────────── */
  .status-dot {
    display: inline-block;
    width: 8px;
    height: 8px;
    border-radius: 50%;
    margin-right: 4px;
  }
  .status-ok { background: #22c55e; }
  .status-busy { background: #eab308; animation: pulse 1s infinite; }
  .status-err { background: #ef4444; }
  .status-off { background: #999; }

  @keyframes pulse {
    0%, 100% { opacity: 1; }
    50% { opacity: 0.4; }
  }

  /* ── Info cards ─────────────────────────── */
  .info-card {
    background: #f5f5f5;
    border-radius: 14px;
    padding: 12px 16px;
    margin: 4px 0;
  }

  .info-row {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 8px;
  }

  .info-label {
    font-size: 0.85rem;
    font-weight: 600;
    color: #000;
    flex-shrink: 0;
  }

  .info-value {
    font-size: 0.8rem;
    color: #666;
    text-align: right;
    word-break: break-all;
  }

  /* ── Refresh / Retry / Action buttons ──── */
  .refresh-btn {
    background: none;
    border: none;
    font-size: 1.2rem;
    cursor: pointer;
    color: #666;
    padding: 4px 8px;
    border-radius: 6px;
  }
  .refresh-btn:hover { background: #eee; }

  .retry-btn {
    display: block;
    margin: 8px auto;
    padding: 8px 24px;
    border: none;
    border-radius: 10px;
    background: #000;
    color: #fff;
    font-weight: 600;
    cursor: pointer;
  }

  .action-btn {
    display: block;
    width: 100%;
    padding: 10px;
    margin-top: 6px;
    border: none;
    border-radius: 10px;
    background: #f5f5f5;
    color: #d00;
    font-weight: 600;
    font-size: 0.9rem;
    cursor: pointer;
  }
  .action-btn:hover { background: #fee; }

  .cancel-btn {
    display: block;
    margin: 8px auto;
    padding: 6px 20px;
    border: 1px solid #ccc;
    border-radius: 8px;
    background: #fff;
    color: #666;
    font-size: 0.85rem;
    cursor: pointer;
  }

  /* ── Reconnecting state ─────────────────── */
  .reconnecting {
    text-align: center;
    padding: 30px 16px;
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: 12px;
  }

  .recon-icon {
    font-size: 2.5rem;
    animation: pulse 1.5s infinite;
  }

  .recon-title {
    font-size: 1.1rem;
    font-weight: 700;
    color: #000;
  }

  .recon-desc {
    font-size: 0.85rem;
    color: #666;
    line-height: 1.5;
  }

  .recon-hint {
    font-size: 0.8rem;
    color: #999;
    background: #f9f9f9;
    padding: 10px 14px;
    border-radius: 10px;
    line-height: 1.5;
  }

  /* ── Join button (wider) ────────────────── */
  .join-btn {
    align-self: stretch;
    width: auto;
    height: auto;
    padding: 12px;
    border-radius: 12px;
    border: none;
    background: #000;
    color: #fff;
    font-weight: 700;
    font-size: 1rem;
    display: flex;
    align-items: center;
    justify-content: center;
    gap: 8px;
    cursor: pointer;
    transition: background 0.15s, transform 0.1s;
    margin-top: 8px;
  }
  .join-btn:hover {
    background: #222;
  }
  .join-btn:active {
    transform: scale(0.98);
  }
  .join-btn:disabled {
    background: #ccc;
    cursor: not-allowed;
  }
</style>
