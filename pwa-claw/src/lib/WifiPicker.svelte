<script>
  /**
   * WifiPicker — the networks the robot can hear, and joining one.
   *
   * Used on the Connect page (first-time setup) and in the Home Wi-Fi sheet.
   * GET /v1/wifi/scan lists networks; POST /v1/wifi/connect starts a join and
   * /v1/wifi/status is polled until the robot is on (or gives up).
   *
   * @prop network   app-wide network state (staState, staSsid, staIp, ...)
   * @prop onJoined  (ssid, ip) => void — called once the robot is connected
   */
  let { network, onJoined } = $props();

  let nets = $state([]);
  let scanning = $state(false);
  let scanError = $state("");
  let picked = $state(null);       // ssid whose password row is open
  let password = $state("");
  let showPw = $state(false);
  let joining = $state(null);      // ssid being joined
  let joinError = $state("");
  let pwInput = $state(null);

  let scanTimer = null;
  let pollTimer = null;

  async function scan(attempt = 0) {
    scanning = true;
    scanError = "";
    clearTimeout(scanTimer);
    try {
      const res = await fetch("/v1/wifi/scan", { cache: "no-store" });
      const data = await res.json();
      if (data.ok) {
        nets = data.networks || [];
        scanning = false;
        return;
      }
      // The radio refuses to scan while it is in the middle of joining.
      if (data.error === "busy" && attempt < 4) {
        scanTimer = setTimeout(() => scan(attempt + 1), 1500);
        return;
      }
      scanError = "The robot couldn't look for networks right now.";
    } catch {
      scanError = "Couldn't reach the robot.";
    }
    scanning = false;
  }

  function pick(n) {
    joinError = "";
    if (!n.secure) { join(n.ssid, ""); return; }
    if (picked === n.ssid) { picked = null; return; }
    picked = n.ssid;
    password = "";
    showPw = false;
    queueMicrotask(() => pwInput?.focus());
  }

  async function join(ssid, pw) {
    joining = ssid;
    joinError = "";
    picked = null;
    try {
      const res = await fetch("/v1/wifi/connect", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ ssid, password: pw }),
      });
      const data = await res.json().catch(() => ({}));
      if (!res.ok || data.ok === false) throw new Error(data.error || `HTTP ${res.status}`);
    } catch (e) {
      joining = null;
      joinError = `Couldn't start joining ${ssid}.`;
      return;
    }
    const started = Date.now();
    const poll = async () => {
      try {
        const r = await fetch("/v1/wifi/status", { cache: "no-store" });
        const s = await r.json();
        if (s.sta?.state === "connected" && s.sta?.ssid === ssid && s.sta?.ip) {
          joining = null;
          onJoined?.(ssid, s.sta.ip);
          return;
        }
        if (s.sta?.error && Date.now() - started > 4000) {
          joining = null;
          joinError = /auth|password|4way|handshake/i.test(s.sta.error)
            ? `Wrong password for ${ssid}.`
            : `Couldn't join ${ssid} (${s.sta.error}).`;
          return;
        }
      } catch { /* the phone may hop networks for a moment — keep asking */ }
      if (Date.now() - started > 25000) {
        joining = null;
        joinError = `${ssid} didn't answer. Check the password and try again.`;
        return;
      }
      pollTimer = setTimeout(poll, 1000);
    };
    pollTimer = setTimeout(poll, 1200);
  }

  function submit(e, ssid) {
    e.preventDefault();
    if (password.length > 0 && password.length < 8) {
      joinError = "Wi‑Fi passwords are at least 8 characters.";
      return;
    }
    join(ssid, password);
  }

  $effect(() => {
    scan();
    return () => { clearTimeout(scanTimer); clearTimeout(pollTimer); };
  });

  const current = $derived(network?.staState === "connected" ? network?.staSsid : "");
</script>

<div class="picker">
  <div class="head">
    <span class="label">{scanning ? "Looking for networks…" : nets.length ? "Networks the robot can hear" : "No networks found"}</span>
    <button type="button" class="rescan" class:spin={scanning} onclick={() => scan()} disabled={scanning || !!joining} aria-label="Look for networks again">
      <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.3" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M20 12a8 8 0 1 1-2.3-5.6"/><path d="M20 4v4h-4"/></svg>
    </button>
  </div>

  {#if scanError}
    <p class="note err" role="alert">{scanError}</p>
  {/if}

  {#if nets.length}
    <ul class="list" aria-label="Wi‑Fi networks">
      {#each nets as n (n.ssid)}
        {@const isCur = current === n.ssid}
        <li class:open={picked === n.ssid}>
          <button type="button" class="row" onclick={() => pick(n)} disabled={!!joining || isCur} aria-expanded={n.secure ? picked === n.ssid : undefined}>
            <svg class="bars" width="20" height="20" viewBox="0 0 24 24" fill="none" stroke-width="2.4" stroke-linecap="round" aria-label="Signal {n.bars} of 3">
              <path d="M2 8.5a15 15 0 0 1 20 0" class:dim={n.bars < 3}/>
              <path d="M5 12a10 10 0 0 1 14 0" class:dim={n.bars < 2}/>
              <path d="M8.5 15.5a5 5 0 0 1 7 0"/>
              <circle cx="12" cy="19" r="1.3" class="dot"/>
            </svg>
            <span class="ssid">{n.ssid}</span>
            {#if joining === n.ssid}
              <span class="state">Joining…</span><span class="spinner" aria-hidden="true"></span>
            {:else if isCur}
              <span class="state ok">Connected</span>
            {:else if n.secure}
              <svg class="lock" width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round" aria-label="Needs a password"><rect x="5" y="11" width="14" height="10" rx="2"/><path d="M8 11V8a4 4 0 0 1 8 0v3"/></svg>
            {/if}
          </button>
          {#if picked === n.ssid}
            <form class="pw" onsubmit={(e) => submit(e, n.ssid)}>
              <label class="sr" for="wifi-pw-{n.ssid}">Password for {n.ssid}</label>
              <div class="pw-box">
                <input
                  id="wifi-pw-{n.ssid}"
                  bind:this={pwInput}
                  bind:value={password}
                  type={showPw ? "text" : "password"}
                  placeholder="Password"
                  autocomplete="off"
                  autocapitalize="off"
                  spellcheck="false"
                />
                <button type="button" class="eye" onclick={() => (showPw = !showPw)} aria-label={showPw ? "Hide password" : "Show password"}>
                  {#if showPw}
                    <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M3 3l18 18"/><path d="M10.6 6.1A9.8 9.8 0 0 1 12 6c5 0 9 6 9 6a17 17 0 0 1-2.6 3.3M6.6 6.6C4.3 8.1 3 12 3 12s4 6 9 6a8.6 8.6 0 0 0 4-1"/></svg>
                  {:else}
                    <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M3 12s4-6 9-6 9 6 9 6-4 6-9 6-9-6-9-6z"/><circle cx="12" cy="12" r="2.5"/></svg>
                  {/if}
                </button>
              </div>
              <button type="submit" class="join">Join</button>
            </form>
          {/if}
        </li>
      {/each}
    </ul>
  {:else if scanning}
    <ul class="list skeleton" aria-hidden="true">
      {#each [0, 1, 2] as _}<li><div class="row"><span class="sk"></span></div></li>{/each}
    </ul>
  {/if}

  {#if joinError}
    <p class="note err" role="alert">{joinError}</p>
  {/if}
</div>

<style>
  .picker { display: flex; flex-direction: column; gap: 10px; }
  .head { display: flex; align-items: center; justify-content: space-between; gap: 8px; }
  .label { font-size: 13px; font-weight: 600; color: var(--sub); }
  .rescan {
    width: 40px; height: 40px; border-radius: 20px; border: none;
    background: var(--ground); color: var(--ink);
    display: grid; place-items: center; cursor: pointer; flex-shrink: 0;
  }
  .rescan:disabled { opacity: 0.6; cursor: default; }
  .rescan.spin svg { animation: spin 0.9s linear infinite; }
  @keyframes spin { to { transform: rotate(360deg); } }

  .list { list-style: none; background: var(--ground); border-radius: 16px; overflow: hidden; }
  .list li + li { border-top: 1px solid #E2DDD1; }
  .row {
    width: 100%; min-height: 50px; border: none; background: transparent;
    display: flex; align-items: center; gap: 12px; padding: 0 14px;
    text-align: left; cursor: pointer; color: var(--ink);
  }
  .row:disabled { cursor: default; }
  .row:focus-visible { outline: 3px solid var(--ink); outline-offset: -3px; }
  .bars { stroke: var(--ink); flex-shrink: 0; }
  .bars .dim { stroke: #C9C4B8; }
  .bars .dot { fill: var(--ink); stroke: none; }
  .ssid { flex: 1; min-width: 0; font-size: 15px; font-weight: 600; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  .lock { color: var(--sub); flex-shrink: 0; }
  .state { font-size: 13px; font-weight: 600; color: var(--sub); }
  .state.ok { color: #1E7B34; }

  .pw { display: flex; gap: 8px; padding: 0 10px 10px; }
  .pw-box { flex: 1; min-width: 0; position: relative; }
  .pw-box input {
    width: 100%; height: 46px; border-radius: 12px; border: 1.5px solid #D9D4C7;
    background: #fff; padding: 0 44px 0 12px; font-size: 16px; color: var(--ink);
  }
  .pw-box input:focus-visible { outline: none; border-color: var(--ink); }
  .eye {
    position: absolute; right: 2px; top: 1px; width: 44px; height: 44px;
    border: none; background: transparent; color: var(--sub);
    display: grid; place-items: center; cursor: pointer;
  }
  .join {
    height: 46px; border-radius: 12px; border: none; background: var(--ink); color: #fff;
    padding: 0 18px; font-size: 15px; font-weight: 700; cursor: pointer;
  }
  .join:focus-visible { outline: 3px solid var(--mpx-yellow); outline-offset: 2px; }

  .note { font-size: 14px; line-height: 1.4; color: #4F4C46; background: var(--ground); border-radius: 12px; padding: 10px 12px; }
  .note.err { color: #9B2C20; background: #FBEAE7; }

  .spinner {
    width: 16px; height: 16px; border-radius: 50%; flex-shrink: 0;
    border: 2.4px solid #D9D4C7; border-top-color: var(--ink);
    animation: spin 0.8s linear infinite;
  }
  .skeleton .sk { display: block; height: 14px; width: 55%; border-radius: 7px; background: #E6E2D8; }
  .sr { position: absolute; width: 1px; height: 1px; overflow: hidden; clip: rect(0 0 0 0); white-space: nowrap; }
</style>
