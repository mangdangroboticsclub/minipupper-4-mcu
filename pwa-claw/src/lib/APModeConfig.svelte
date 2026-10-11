<script>
  /** @type {{ onNavigate?: (view: string) => void, network?: object }} */
  let { onNavigate, network } = $props();

  let ssid = $state(network?.apSsid || "MPX-Dog");
  let password = $state("");
  let showPassword = $state(false);
  let saving = $state(false);
  let error = $state("");

  /** After a successful save the ESP restarts. Track which stage we're in. */
  let phase = $state("form"); // "form" | "restarting" | "reconnect"

  // Show current AP info
  let apIp = $derived(network?.apIp || "192.168.2.1");

  async function saveConfig(e) {
    e?.preventDefault();
    if (!ssid.trim()) return;
    if (password.length > 0 && password.length < 8) {
      error = "Hotspot passwords are at least 8 characters.";
      return;
    }
    saving = true;
    error = "";
    try {
      const res = await fetch("/v1/wifi/ap-config", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ ssid: ssid.trim(), password }),
      });
      if (res.ok) {
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

<div class="page">
  <header class="bar">
    {#if phase === "form"}
      <button type="button" class="round" onclick={goBack} aria-label="Back to settings">
        <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M15 6l-6 6 6 6"/></svg>
      </button>
    {:else}
      <span class="spacer"></span>
    {/if}
    <h1>ESP hotspot</h1>
    <span class="spacer"></span>
  </header>

  <div class="scroll">
    <div class="inner">
      {#if phase === "restarting"}
        <section class="card center" role="status">
          <span class="big-ico pulse" aria-hidden="true">
            <svg width="26" height="26" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round"><path d="M20 11a8 8 0 1 0-2.3 5.7"/><path d="M20 4v7h-7"/></svg>
          </span>
          <h2 class="card-title">Saving and restarting</h2>
          <p class="desc">The robot is saving the new hotspot settings and restarting its network.</p>
          <div class="loading"><div></div></div>
        </section>

      {:else if phase === "reconnect"}
        <section class="card center">
          <span class="big-ico" aria-hidden="true">
            <svg width="26" height="26" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" aria-hidden="true"><circle cx="12" cy="12" r="2"/><path d="M8 8a6 6 0 0 0 0 8M16 8a6 6 0 0 1 0 8M5 5a10 10 0 0 0 0 14M19 5a10 10 0 0 1 0 14"/></svg>
          </span>
          <h2 class="card-title">Join the new hotspot</h2>
          <p class="desc">The robot restarted with the new settings, so this device has lost it for now.</p>
        </section>

        <ol class="steps">
          <li>
            <span class="num">1</span>
            <span>Open this device's <b>Wi‑Fi settings</b> and join <b>{ssid || "MPX-Dog"}</b>{password ? " with the password you set." : ". It has no password."}</span>
          </li>
          <li>
            <span class="num">2</span>
            <span>Come back here and tap the button below.</span>
          </li>
        </ol>

        <button type="button" class="primary" onclick={onReconnected}>I'm connected</button>

      {:else}
        <p class="lead">The hotspot is the robot's own Wi‑Fi. Your phone joins it directly when there's no other network around.</p>

        <div class="facts">
          <div class="fact"><span class="fact-k">Robot IP</span><span class="fact-v">{apIp}</span></div>
        </div>

        <form class="form" onsubmit={saveConfig}>
          <label class="field">
            <span class="field-k">Hotspot name</span>
            <input type="text" bind:value={ssid} placeholder="MPX-Dog" autocomplete="off" />
          </label>

          <label class="field">
            <span class="field-k">Password</span>
            <span class="pw">
              <input type={showPassword ? "text" : "password"} bind:value={password} placeholder="At least 8 characters, or empty" />
              <button type="button" class="eye" onclick={() => (showPassword = !showPassword)} aria-label={showPassword ? "Hide password" : "Show password"}>
                {#if showPassword}
                  <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M10.6 5.1A10.4 10.4 0 0 1 12 5c6.4 0 10 7 10 7a17.6 17.6 0 0 1-3.2 4.1M6.6 6.6C3.8 8.4 2 12 2 12s3.6 7 10 7a9.7 9.7 0 0 0 5.4-1.6"/><path d="M3 3l18 18M9.9 9.9a3 3 0 0 0 4.2 4.2"/></svg>
                {:else}
                  <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M2 12s3.6-7 10-7 10 7 10 7-3.6 7-10 7S2 12 2 12z"/><circle cx="12" cy="12" r="3"/></svg>
                {/if}
              </button>
            </span>
          </label>

          <p class="hint">Saving restarts the robot's network. You'll need to join the hotspot again afterwards.</p>

          {#if error}<p class="note err" role="alert">{error}</p>{/if}

          <button type="submit" class="primary" disabled={saving || !ssid.trim()}>
            {saving ? "Saving…" : "Save hotspot"}
          </button>
        </form>
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
  .spacer { width: 44px; }
  .round {
    width: 44px; height: 44px; border-radius: 22px; border: none; background: var(--ground); color: var(--ink);
    display: grid; place-items: center; cursor: pointer;
  }

  .scroll { flex: 1; min-height: 0; overflow-y: auto; }
  .inner { max-width: var(--page-max); margin: 0 auto; padding: 8px 16px calc(24px + var(--safe-bottom)); display: flex; flex-direction: column; gap: 12px; }

  .lead { font-size: 14px; line-height: 1.45; color: var(--sub); padding: 0 4px; }
  .hint { font-size: 13px; line-height: 1.45; color: var(--sub); padding: 0 4px; }

  .card { background: #fff; border: 1px solid var(--line); border-radius: 22px; padding: 24px 20px; display: flex; flex-direction: column; align-items: center; text-align: center; gap: 8px; }
  .card-title { font-size: 17px; font-weight: 800; }
  .desc { font-size: 14px; line-height: 1.45; color: var(--sub); max-width: 320px; }
  .big-ico { width: 56px; height: 56px; border-radius: 16px; display: grid; place-items: center; background: var(--mpx-yellow); margin-bottom: 4px; }
  .pulse { animation: pulse 1.4s ease-in-out infinite; }
  @keyframes pulse { 0%, 100% { opacity: 1; } 50% { opacity: 0.45; } }
  .loading { width: 200px; height: 6px; border-radius: 3px; background: var(--hair); overflow: hidden; margin-top: 8px; }
  .loading div { width: 30%; height: 100%; border-radius: 3px; background: var(--ink); animation: slide 1.5s ease infinite; }
  @keyframes slide { from { transform: translateX(-100%); } to { transform: translateX(400%); } }

  .steps { list-style: none; display: flex; flex-direction: column; gap: 8px; }
  .steps li { display: flex; align-items: flex-start; gap: 12px; background: var(--ground); border-radius: 16px; padding: 12px 14px; font-size: 14px; line-height: 1.45; }
  .num { width: 26px; height: 26px; border-radius: 13px; background: var(--ink); color: var(--mpx-yellow); display: grid; place-items: center; font-size: 13px; font-weight: 800; flex-shrink: 0; }

  .facts { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 8px; }
  .fact { background: var(--ground); border-radius: 14px; padding: 9px 12px; display: flex; flex-direction: column; gap: 2px; min-width: 0; }
  .fact-k, .field-k { font-size: 11px; font-weight: 700; letter-spacing: 0.1em; text-transform: uppercase; color: var(--sub); }
  .fact-v { font-family: var(--mono); font-size: 14px; font-weight: 500; }

  .form { display: flex; flex-direction: column; gap: 10px; }
  .field { display: flex; flex-direction: column; gap: 4px; background: var(--ground); border-radius: 16px; padding: 8px 14px; }
  .field:focus-within { outline: 3px solid var(--ink); outline-offset: 2px; }
  .field input { flex: 1; min-width: 0; border: none; background: transparent; font-size: 16px; color: var(--ink); outline: none; padding: 2px 0; }
  .field input::placeholder { color: var(--sub); }
  .pw { display: flex; align-items: center; gap: 8px; }
  .eye { width: 32px; height: 32px; border-radius: 16px; border: none; background: transparent; color: var(--sub); display: grid; place-items: center; cursor: pointer; flex-shrink: 0; }

  .note { font-size: 13px; background: var(--ground); border-radius: 14px; padding: 10px 12px; }
  .note.err { color: #9B2C20; font-weight: 600; }
  .primary {
    margin-top: 4px; height: 56px; border-radius: 28px; border: none; background: var(--ink); color: #fff;
    font-size: 16px; font-weight: 700; cursor: pointer; transition: transform var(--dur-fast), opacity var(--dur-fast);
  }
  .primary:active:not(:disabled) { transform: scale(0.98); }
  .primary:disabled { opacity: 0.35; cursor: default; }
  button:focus-visible { outline: 3px solid var(--ink); outline-offset: 2px; }
</style>
