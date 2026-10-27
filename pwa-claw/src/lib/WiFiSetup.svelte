<script>
  import WifiPicker from "./WifiPicker.svelte";

  /** @type {{ onNavigate?: (view: string) => void, network?: object }} */
  let { onNavigate, network } = $props();

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

  // Hidden-network form
  let showManual = $state(false);
  let inputSsid = $state("");
  let inputPassword = $state("");
  let inputIdentity = $state("");   // university / work (enterprise) networks only
  let connecting = $state(false);
  let reconnecting = $state(false);
  let connectError = $state("");

  let pollTimer = null;

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
        error = `The robot answered with an error (${res.status}).`;
      }
    } catch {
      if (!reconnecting) error = "Couldn't reach the robot.";
    }
    loading = false;
  }

  function startReconnectPoller() {
    clearTimeout(pollTimer);
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

  async function doConnect(e) {
    e?.preventDefault();
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
        body: JSON.stringify(inputIdentity.trim()
          ? { ssid, password: inputPassword, identity: inputIdentity.trim() }
          : { ssid, password: inputPassword }),
      });
      requestSent = true;
      if (res.ok) {
        staState = "connecting";
        staSsid = ssid;
        reconnecting = true;
        startReconnectPoller();
      } else {
        const text = await res.text();
        connectError = text || `Couldn't start joining (${res.status}).`;
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

  function cancelReconnect() {
    clearTimeout(pollTimer);
    reconnecting = false;
    connecting = false;
    fetchStatus();
  }

  async function doDisconnect() {
    try {
      await fetch("/v1/wifi/disconnect", { method: "POST" });
      staState = "disconnected";
      staSsid = "";
      staIp = "";
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
    } catch (e) {
      console.error("Forget failed:", e);
    }
  }

  function goBack() {
    onNavigate?.("welcome");
  }

  // ── Init ───────────────────────────────────────────────────
  $effect(() => {
    fetchStatus();
    return () => clearTimeout(pollTimer);
  });

  const connected = $derived(staState === "connected");
  const statusLine = $derived(
    connected ? staIp
    : staState === "connecting" ? `Joining ${staSsid || "Wi‑Fi"}…`
    : staState === "failed" ? `Couldn't join ${staSsid || "the network"}`
    : "Pick a network below"
  );
</script>

<div class="page">
  <header class="bar">
    <button type="button" class="round" onclick={goBack} aria-label="Back to settings">
      <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M15 6l-6 6 6 6"/></svg>
    </button>
    <h1>ESP Wi‑Fi</h1>
    <button type="button" class="round" onclick={fetchStatus} aria-label="Refresh">
      <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M20 11a8 8 0 1 0-2.3 5.7"/><path d="M20 4v7h-7"/></svg>
    </button>
  </header>

  <div class="scroll">
    <div class="inner">
      {#if reconnecting}
        <section class="card center" role="status">
          <span class="big-ico pulse" aria-hidden="true">
            <svg width="26" height="26" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" aria-hidden="true"><path d="M2 8.5a15 15 0 0 1 20 0M5 12a10 10 0 0 1 14 0M8.5 15.5a5 5 0 0 1 7 0"/><circle cx="12" cy="19" r="1.2" fill="currentColor"/></svg>
          </span>
          <h2 class="card-title">Joining {staSsid}</h2>
          <p class="desc">The robot is restarting its network. If this page stops answering, join the <b>{apSsid}</b> hotspot again.</p>
          <button type="button" class="pill" onclick={cancelReconnect}>Cancel</button>
        </section>
      {:else if loading && !staSsid}
        <div class="card sk" aria-hidden="true"><span class="ico"></span><span class="sk-line"></span></div>
      {:else if error}
        <section class="card center">
          <h2 class="card-title">Can't read the Wi‑Fi status</h2>
          <p class="desc">{error}</p>
          <button type="button" class="pill dark" onclick={fetchStatus}>Try again</button>
        </section>
      {:else}
        <section class="card">
          <div class="card-head">
            <span class="ico" class:on={connected}>
              <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" aria-hidden="true"><path d="M2 8.5a15 15 0 0 1 20 0M5 12a10 10 0 0 1 14 0M8.5 15.5a5 5 0 0 1 7 0"/><circle cx="12" cy="19" r="1.2" fill="currentColor"/></svg>
            </span>
            <span class="txt">
              <span class="t">{connected ? staSsid : staState === "connecting" ? staSsid : "Not on Wi‑Fi"}</span>
              <span class="d" class:mono={connected} class:err={staState === "failed"}>{statusLine}</span>
            </span>
            {#if connected}<span class="status">On</span>{/if}
          </div>
          {#if staState !== "disconnected"}
            <div class="acts">
              <button type="button" class="pill" onclick={doDisconnect}>Disconnect</button>
              <button type="button" class="pill danger" onclick={doForget}>Forget network</button>
            </div>
          {/if}
        </section>

        <div class="facts">
          <div class="fact"><span class="fact-k">Hotspot</span><span class="fact-v">{apSsid}</span></div>
          <div class="fact"><span class="fact-k">Hotspot IP</span><span class="fact-v">{apIp}</span></div>
        </div>
      {/if}

      {#if !reconnecting}
        <h2 class="section">Networks nearby</h2>
        <div class="card">
          <WifiPicker {network} onJoined={fetchStatus} />
        </div>

        <div class="group">
          <button type="button" class="row" onclick={() => (showManual = !showManual)} aria-expanded={showManual}>
            <span class="txt">
              <span class="t">Join a hidden network</span>
              <span class="d">Type the name and password yourself</span>
            </span>
            <svg class="chev" class:down={showManual} width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M9 6l6 6-6 6"/></svg>
          </button>
          {#if showManual}
            <form class="manual" onsubmit={doConnect}>
              <label class="field">
                <span class="field-k">Network name</span>
                <input type="text" bind:value={inputSsid} placeholder="SSID" autocomplete="off" />
              </label>
              <label class="field">
                <span class="field-k">Username (university / work Wi‑Fi only)</span>
                <input type="text" bind:value={inputIdentity} placeholder="e.g. name@hku.hk — empty for home Wi‑Fi" autocomplete="off" autocapitalize="off" spellcheck="false" />
              </label>
              <label class="field">
                <span class="field-k">Password</span>
                <input type="password" bind:value={inputPassword} placeholder="Leave empty if open" />
              </label>
              {#if connectError}<p class="note err" role="alert">{connectError}</p>{/if}
              <button type="submit" class="primary" disabled={connecting || !inputSsid.trim()}>
                {connecting ? "Joining…" : "Join"}
              </button>
            </form>
          {/if}
        </div>
      {/if}
    </div>
  </div>
</div>

<style>
  .page { position: absolute; inset: 0; display: flex; flex-direction: column; background: var(--page); color: var(--ink); }
  .bar {
    flex-shrink: 0; height: calc(64px + var(--safe-top)); padding: calc(10px + var(--safe-top)) 16px 0;
    display: flex; align-items: center; justify-content: space-between;
    max-width: var(--page-max); width: 100%; margin: 0 auto;
  }
  .bar h1 { font-size: 18px; font-weight: 800; }
  .round {
    width: 44px; height: 44px; border-radius: 22px; border: none; background: var(--ground); color: var(--ink);
    display: grid; place-items: center; cursor: pointer;
  }

  .scroll { flex: 1; min-height: 0; overflow-y: auto; }
  .inner { max-width: var(--page-max); margin: 0 auto; padding: 8px 16px calc(24px + var(--safe-bottom)); display: flex; flex-direction: column; gap: 12px; }

  .card { background: #fff; border: 1px solid var(--line); border-radius: 22px; padding: 16px; display: flex; flex-direction: column; gap: 12px; }
  .card.center { align-items: center; text-align: center; padding: 24px 20px; gap: 8px; }
  .card-head { display: flex; align-items: center; gap: 12px; }
  .card-title { font-size: 17px; font-weight: 800; }
  .desc { font-size: 14px; line-height: 1.45; color: var(--sub); max-width: 320px; }
  .desc b { color: var(--ink); }
  .ico { width: 40px; height: 40px; border-radius: 12px; flex-shrink: 0; display: grid; place-items: center; background: var(--mpx-yellow); color: var(--ink); }
  .ico.on { background: var(--ink); color: var(--mpx-yellow); }
  .big-ico { width: 56px; height: 56px; border-radius: 16px; display: grid; place-items: center; background: var(--mpx-yellow); margin-bottom: 4px; }
  .pulse { animation: pulse 1.4s ease-in-out infinite; }
  @keyframes pulse { 0%, 100% { opacity: 1; } 50% { opacity: 0.45; } }
  .txt { flex: 1; min-width: 0; display: flex; flex-direction: column; gap: 2px; }
  .t { font-size: 17px; font-weight: 700; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  .d { font-size: 13px; color: var(--sub); overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  .d.err { color: #9B2C20; font-weight: 600; }
  .mono { font-family: var(--mono); }
  .status { display: flex; align-items: center; gap: 6px; font-size: 13px; font-weight: 600; color: #1E7B34; }
  .status::before { content: ""; width: 8px; height: 8px; border-radius: 4px; background: #23A047; }
  .acts { display: flex; gap: 8px; flex-wrap: wrap; }

  .pill { height: 40px; border-radius: 20px; border: none; background: var(--ground); padding: 0 16px; font-size: 14px; font-weight: 700; color: var(--ink); cursor: pointer; }
  .pill.dark { background: var(--ink); color: #fff; }
  .pill.danger { color: #9B2C20; }

  .facts { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 8px; }
  .fact { background: var(--ground); border-radius: 14px; padding: 9px 12px; display: flex; flex-direction: column; gap: 2px; min-width: 0; }
  .fact-k { font-size: 11px; font-weight: 700; letter-spacing: 0.1em; text-transform: uppercase; color: var(--sub); }
  .fact-v { font-family: var(--mono); font-size: 14px; font-weight: 500; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }

  .section { margin: 8px 4px 0; font-size: 12px; font-weight: 800; letter-spacing: 0.12em; text-transform: uppercase; color: var(--sub); }

  .group { background: #fff; border: 1px solid var(--line); border-radius: 20px; overflow: hidden; }
  .row { width: 100%; border: none; background: transparent; color: var(--ink); display: flex; align-items: center; gap: 14px; padding: 14px 16px; text-align: left; cursor: pointer; }
  .row .t { font-size: 16px; }
  .chev { color: #A39F96; flex-shrink: 0; transition: transform var(--dur-fast); }
  .chev.down { transform: rotate(90deg); }
  .manual { padding: 0 16px 16px; display: flex; flex-direction: column; gap: 10px; }
  .field { display: flex; flex-direction: column; gap: 4px; background: var(--ground); border-radius: 16px; padding: 8px 14px; }
  .field:focus-within { outline: 3px solid var(--ink); outline-offset: 2px; }
  .field-k { font-size: 11px; font-weight: 700; letter-spacing: 0.1em; text-transform: uppercase; color: var(--sub); }
  .field input { border: none; background: transparent; font-size: 16px; color: var(--ink); outline: none; padding: 2px 0; }
  .field input::placeholder { color: var(--sub); }
  .note { font-size: 13px; background: var(--ground); border-radius: 14px; padding: 10px 12px; }
  .note.err { color: #9B2C20; font-weight: 600; }
  .primary { height: 52px; border-radius: 26px; border: none; background: var(--ink); color: #fff; font-size: 16px; font-weight: 700; cursor: pointer; }
  .primary:disabled { opacity: 0.35; cursor: default; }

  .sk { flex-direction: row; align-items: center; height: 74px; }
  .sk .ico, .sk-line { background: var(--hair); }
  .sk-line { height: 14px; width: 50%; border-radius: 7px; }

  button:focus-visible { outline: 3px solid var(--ink); outline-offset: 2px; }
  .row:focus-visible { outline-offset: -3px; }
</style>
