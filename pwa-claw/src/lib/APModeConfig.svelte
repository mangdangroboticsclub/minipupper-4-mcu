<script>
  import { colors } from "./colors.js";

  /** @type {{ onNavigate?: (view: string) => void, network?: object }} */
  let { onNavigate, network } = $props();

  const YELLOW = colors.mpx.primary;

  let ssid = $state(network?.apSsid || "MPX-Dog");
  let password = $state("");
  let showPassword = $state(false);
  let saving = $state(false);
  let saved = $state(false);
  let error = $state("");

  /** After a successful save the ESP restarts. Track which stage we're in. */
  let phase = $state("form"); // "form" | "restarting" | "reconnect"

  // Show current AP info
  let apIp = $derived(network?.apIp || "192.168.2.1");

  async function saveConfig() {
    if (!ssid.trim()) return;
    saving = true;
    error = "";
    try {
      const res = await fetch("/v1/wifi/ap-config", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ ssid: ssid.trim(), password }),
      });
      if (res.ok) {
        saved = true;
        // ESP will restart — show the restarting screen
        phase = "restarting";
      } else {
        const text = await res.text();
        error = text || `Save failed (${res.status})`;
      }
    } catch (e) {
      // If the connection drops mid-restart, that's expected — move to reconnect
      phase = "reconnect";
    }
    saving = false;
  }

  /** Called when the user confirms they've reconnected. */
  function onReconnected() {
    onNavigate?.("welcome");
  }

  function goBack() {
    onNavigate?.("welcome");
  }

  // After a short delay, move from "restarting" to "reconnect" (the ESP should
  // be down by then and the fetch would have failed or hung).
  $effect(() => {
    if (phase === "restarting") {
      const t = setTimeout(() => { phase = "reconnect"; }, 3000);
      return () => clearTimeout(t);
    }
  });
</script>

<div class="ap-root">
  {#if phase === "restarting"}
    <!-- ═══ RESTARTING VIEW ═══ -->
    <div class="restart-view">
      <div class="restart-icon">🔄</div>
      <h2 class="restart-title">Applying Configuration</h2>
      <p class="restart-desc">
        The robot is saving your new AP settings and restarting…
      </p>
      <div class="restart-bar-track">
        <div class="restart-bar-fill"></div>
      </div>
    </div>

  {:else if phase === "reconnect"}
    <!-- ═══ RECONNECT VIEW ═══ -->
    <div class="reconnect-view">
      <div class="reconnect-icon">📶</div>
      <h2 class="reconnect-title">Reconnect Required</h2>
      <p class="reconnect-desc">
        Your robot has restarted with the new AP settings.
      </p>

      <div class="reconnect-step-list">
        <div class="reconnect-step">
          <span class="step-num">1</span>
          <span class="step-text">
            Open your device's <strong>Wi‑Fi settings</strong> and connect to
            <strong>{ssid || "MPX-Dog"}</strong>
            {#if password}
              using the password you set.
            {:else}
              (no password required).
            {/if}
          </span>
        </div>
        <div class="reconnect-step">
          <span class="step-num">2</span>
          <span class="step-text">
            Come back and <strong>refresh this page</strong>, or tap the button below.
          </span>
        </div>
      </div>

      <button class="reconnect-btn" style="background: {YELLOW}" onclick={onReconnected}>
        I'm connected — Continue
      </button>
    </div>

  {:else}
    <!-- ═══ FORM VIEW ═══ -->
    <!-- Header -->
    <div class="header">
      <button class="back-btn" onclick={goBack} aria-label="Back">
        <svg width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
          <path d="M19 12H5M12 19l-7-7 7-7"/>
        </svg>
      </button>
      <h1 class="header-title">AP Mode</h1>
    </div>

    <p class="desc">Configure the Access Point mode for your Pupper's direct hotspot connection.</p>

    <!-- Current AP info -->
    <div class="ap-info">
      <span class="ap-info-label">Current IP:</span>
      <span class="ap-info-value">{apIp}</span>
    </div>

    <!-- ═══ SSID Field ═══ -->
    <div class="field">
      <label class="field-label" for="ap-ssid">SSID</label>
      <input
        id="ap-ssid"
        class="field-input"
        type="text"
        bind:value={ssid}
        placeholder="Enter AP name"
      />
    </div>

    <!-- ═══ Password Field ═══ -->
    <div class="field">
      <label class="field-label" for="ap-password">Password</label>
      <div class="password-wrapper">
        <input
          id="ap-password"
          class="field-input"
          type={showPassword ? "text" : "password"}
          bind:value={password}
          placeholder="At least 8 characters"
        />
        <button
          class="toggle-vis"
          onclick={() => showPassword = !showPassword}
          aria-label={showPassword ? "Hide password" : "Show password"}
        >
          {#if showPassword}
            <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
              <path d="M17.94 17.94A10.07 10.07 0 0 1 12 20c-7 0-11-8-11-8a18.45 18.45 0 0 1 5.06-5.94M9.9 4.24A9.12 9.12 0 0 1 12 4c7 0 11 8 11 8a18.5 18.5 0 0 1-2.16 3.19M14.12 14.12a3 3 0 1 1-4.24-4.24"/>
              <line x1="1" y1="1" x2="23" y2="23"/>
            </svg>
          {:else}
            <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
              <path d="M1 12s4-8 11-8 11 8 11 8-4 8-11 8-11-8-11-8z"/>
              <circle cx="12" cy="12" r="3"/>
            </svg>
          {/if}
        </button>
      </div>
    </div>

    <!-- ═══ Save Button ═══ -->
    <button
      class="save-btn"
      onclick={saveConfig}
      disabled={saving || !ssid.trim()}
    >
      {#if saving}
        <span class="spinner"></span>
      {:else}
        Save Configuration
      {/if}
    </button>

    {#if error}
      <p class="ap-error">{error}</p>
    {/if}
  {/if}
</div>

<style>
  .ap-root {
    position: absolute;
    inset: 0;
    background: #fff;
    display: flex;
    flex-direction: column;
    padding: 0 20px;
    overflow-y: auto;
  }

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

  .desc {
    font-size: 0.85rem;
    color: #666;
    line-height: 1.5;
    margin: 8px 0 20px;
  }

  .field {
    display: flex;
    flex-direction: column;
    gap: 6px;
    margin-bottom: 16px;
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

  .password-wrapper {
    position: relative;
  }

  .toggle-vis {
    position: absolute;
    right: 10px;
    top: 50%;
    transform: translateY(-50%);
    background: none;
    border: none;
    color: #999;
    cursor: pointer;
    padding: 4px;
    display: flex;
  }

  .save-btn {
    width: 100%;
    padding: 14px;
    border: none;
    border-radius: 12px;
    background: #000;
    color: #fff;
    font-size: 1rem;
    font-weight: 600;
    cursor: pointer;
    transition: background 0.15s;
    margin-top: 12px;
    display: flex;
    align-items: center;
    justify-content: center;
    gap: 8px;
  }
  .save-btn:hover {
    background: #222;
  }
  .save-btn:disabled {
    background: #ccc;
    cursor: not-allowed;
  }

  .saved-text {
    color: #4caf50;
  }

  .spinner {
    width: 20px;
    height: 20px;
    border: 2.5px solid rgba(255,255,255,0.4);
    border-top-color: #fff;
    border-radius: 50%;
    animation: spin 0.7s linear infinite;
    display: inline-block;
  }

  @keyframes spin {
    to { transform: rotate(360deg); }
  }

  .ap-info {
    display: flex;
    align-items: center;
    gap: 8px;
    background: #f5f5f5;
    border-radius: 10px;
    padding: 10px 14px;
    margin-bottom: 16px;
  }

  .ap-info-label {
    font-size: 0.8rem;
    font-weight: 600;
    color: #666;
  }

  .ap-info-value {
    font-size: 0.85rem;
    font-weight: 500;
    color: #000;
    font-family: monospace;
  }

  .ap-error {
    margin-top: 12px;
    font-size: 0.85rem;
    color: #d00;
    text-align: center;
  }

  /* ── Restart View ──────────────────────── */
  .restart-view {
    flex: 1;
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    text-align: center;
    gap: 16px;
    padding: 40px 20px;
  }

  .restart-icon {
    font-size: 3rem;
    animation: pulse 1.5s infinite;
  }

  .restart-title {
    font-size: 1.2rem;
    font-weight: 700;
    color: #000;
  }

  .restart-desc {
    font-size: 0.9rem;
    color: #666;
    line-height: 1.5;
  }

  .restart-bar-track {
    width: 200px;
    height: 4px;
    border-radius: 4px;
    background: #eee;
    overflow: hidden;
  }

  .restart-bar-fill {
    height: 100%;
    width: 30%;
    border-radius: 4px;
    background: #000;
    animation: loadingBar 1.5s ease infinite;
  }

  @keyframes loadingBar {
    0%   { transform: translateX(-100%); }
    100% { transform: translateX(400%); }
  }

  @keyframes pulse {
    0%, 100% { opacity: 1; }
    50%      { opacity: 0.4; }
  }

  /* ── Reconnect View ────────────────────── */
  .reconnect-view {
    flex: 1;
    display: flex;
    flex-direction: column;
    align-items: center;
    text-align: center;
    padding: 30px 20px 24px;
    gap: 14px;
  }

  .reconnect-icon {
    font-size: 3rem;
  }

  .reconnect-title {
    font-size: 1.2rem;
    font-weight: 700;
    color: #000;
  }

  .reconnect-desc {
    font-size: 0.9rem;
    color: #666;
    line-height: 1.5;
  }

  .reconnect-step-list {
    display: flex;
    flex-direction: column;
    gap: 14px;
    width: 100%;
    margin-top: 8px;
    text-align: left;
  }

  .reconnect-step {
    display: flex;
    align-items: flex-start;
    gap: 12px;
  }

  .step-num {
    width: 26px;
    height: 26px;
    border-radius: 50%;
    background: #000;
    color: #fff;
    font-size: 0.8rem;
    font-weight: 700;
    display: flex;
    align-items: center;
    justify-content: center;
    flex-shrink: 0;
  }

  .step-text {
    font-size: 0.85rem;
    color: #333;
    line-height: 1.5;
    padding-top: 3px;
  }

  .reconnect-btn {
    width: 100%;
    padding: 14px;
    border: none;
    border-radius: 12px;
    color: #000;
    font-size: 1rem;
    font-weight: 700;
    cursor: pointer;
    transition: transform 0.1s;
    margin-top: auto;
  }
  .reconnect-btn:active {
    transform: scale(0.98);
  }
</style>
