<script>
  import HomeScreen from "./HomeScreen.svelte";
  import WifiPicker from "./WifiPicker.svelte";

  let { network } = $props();

  /** "connect" | "home" */
  let view = $state("connect");
  let changingWifi = $state(false);

  const online = $derived(network?.linkState === "online");
  const staConnected = $derived(network?.staState === "connected");
  const staJoining = $derived(network?.staState === "connecting");
  const espIp = $derived(staConnected && network?.staIp ? network.staIp : network?.apIp || "192.168.2.1");
  const piIp = $derived(network?.piIp || "");
  const showPicker = $derived(!staConnected || changingWifi);

  const wifiSub = $derived(
    staConnected ? `${network?.staSsid} · connected`
    : staJoining ? `Joining ${network?.staSsid || "Wi‑Fi"}…`
    : "Not set up. Pick your network"
  );
</script>

{#if view === "home"}
  <HomeScreen {network} />
{:else}
  <div class="connect">
    <header class="top">
      <img class="logo" src="/md.svg" alt="MangDang" />
    </header>

    <div class="scroll">
      <div class="inner">
        <section class="robot" aria-label="Your robot">
          <div class="robot-eyes" aria-hidden="true"><i></i><i></i></div>
          <div class="robot-text">
            <span class="kicker">Your robot</span>
            <span class="robot-name">Mini Pupper</span>
          </div>
        </section>

        <section class="card" aria-labelledby="wifi-title">
          <div class="card-head">
            <span class="ico" class:on={staConnected}>
              <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" aria-hidden="true"><path d="M2 8.5a15 15 0 0 1 20 0M5 12a10 10 0 0 1 14 0M8.5 15.5a5 5 0 0 1 7 0"/><circle cx="12" cy="19" r="1.2" fill="currentColor"/></svg>
            </span>
            <div class="card-title">
              <h2 id="wifi-title">Wi‑Fi</h2>
              <span class="sub">{wifiSub}</span>
            </div>
            {#if staConnected}
              <button type="button" class="pill-btn" onclick={() => (changingWifi = !changingWifi)} aria-expanded={changingWifi}>
                {changingWifi ? "Done" : "Change"}
              </button>
            {/if}
          </div>
          {#if showPicker}
            <WifiPicker {network} onJoined={() => (changingWifi = false)} />
          {/if}
        </section>

        <h2 class="section-label">Boards</h2>

        <section class="card" aria-label="ESP32 robot controller">
          <div class="card-head">
            <span class="ico on">
              <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><rect x="6" y="6" width="12" height="12" rx="2"/><path d="M9 2v3M15 2v3M9 19v3M15 19v3M2 9h3M2 15h3M19 9h3M19 15h3"/></svg>
            </span>
            <div class="card-title">
              <h3>ESP32</h3>
              <span class="sub">Robot controller</span>
            </div>
            <span class="status" class:ok={online}>{online ? "On" : network?.linkState === "offline" ? "Unreachable" : "Checking…"}</span>
          </div>
          <div class="facts">
            <div class="fact">
              <span class="fact-k">Hotspot</span>
              <span class="fact-v">{network?.apSsid || "—"}</span>
            </div>
            <div class="fact">
              <span class="fact-k">IP</span>
              <span class="fact-v">{espIp}</span>
            </div>
          </div>
        </section>

        {#if piIp}
          <section class="card row" aria-label="Raspberry Pi">
            <span class="ico on">
              <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><rect x="3" y="5" width="18" height="14" rx="2"/><path d="M7 9h4v4H7zM14 9h3M14 13h3"/></svg>
            </span>
            <div class="card-title">
              <h3>Raspberry Pi</h3>
              <span class="sub">ROS 2 · lidar</span>
            </div>
            <div class="fact compact">
              <span class="fact-k">IP</span>
              <span class="fact-v">{piIp}</span>
            </div>
          </section>
        {/if}
      </div>
    </div>

    <div class="foot">
      <button type="button" class="start" onclick={() => (view = "home")}>
        <span>Start</span>
        <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M5 12h14M13 6l6 6-6 6"/></svg>
      </button>
      {#if network?.linkState === "offline"}
        <p class="offline">The robot isn't answering. Controls work again as soon as it's back.</p>
      {/if}
    </div>
  </div>
{/if}

<style>
  .connect {
    position: absolute;
    inset: 0;
    display: flex;
    flex-direction: column;
    background: var(--page);
    color: var(--ink);
    padding-top: var(--safe-top);
    animation: rise var(--dur-slow) var(--ease-out);
  }
  @keyframes rise { from { opacity: 0; transform: translateY(12px); } to { opacity: 1; transform: none; } }

  .top { height: 56px; flex-shrink: 0; display: flex; align-items: center; padding: 0 20px; max-width: var(--page-max); width: 100%; margin: 0 auto; }
  .logo { height: 20px; width: auto; display: block; }

  .scroll { flex: 1; min-height: 0; overflow-y: auto; -webkit-overflow-scrolling: touch; }
  .inner { max-width: var(--page-max); margin: 0 auto; padding: 6px 16px 16px; display: flex; flex-direction: column; gap: 12px; }

  .robot {
    background: var(--mpx-yellow);
    border-radius: 24px;
    padding: 16px 20px;
    display: flex;
    align-items: center;
    gap: 16px;
  }
  .robot-eyes { width: 52px; height: 52px; display: flex; align-items: center; justify-content: center; gap: 6px; flex-shrink: 0; }
  .robot-eyes i {
    width: 14px; height: 30px; border-radius: 5px; background: var(--ink);
    transform-origin: 50% 55%;
    animation: blink 3.6s ease-in-out infinite;
  }
  .robot-eyes i + i { animation-delay: 0.04s; }
  @keyframes blink { 0%, 93%, 100% { transform: scaleY(1); } 96% { transform: scaleY(0.1); } }
  .robot-text { display: flex; flex-direction: column; gap: 1px; }
  .kicker { font-size: 11px; font-weight: 700; letter-spacing: 0.12em; text-transform: uppercase; color: rgba(17, 17, 17, 0.66); }
  .robot-name { font-size: 26px; font-weight: 800; letter-spacing: -0.02em; line-height: 1.15; }

  .card { background: #fff; border: 1px solid var(--line); border-radius: 22px; padding: 16px; display: flex; flex-direction: column; gap: 12px; }
  .card.row { flex-direction: row; align-items: center; }
  .card-head { display: flex; align-items: center; gap: 12px; }
  .ico {
    width: 40px; height: 40px; border-radius: 12px; flex-shrink: 0;
    display: grid; place-items: center;
    background: var(--mpx-yellow); color: var(--ink);
  }
  .ico.on { background: var(--ink); color: var(--mpx-yellow); }
  .card-title { flex: 1; min-width: 0; display: flex; flex-direction: column; }
  .card-title h2, .card-title h3 { font-size: 17px; font-weight: 700; }
  .sub { font-size: 13px; color: var(--sub); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }

  .pill-btn {
    height: 40px; border-radius: 20px; border: none; background: var(--ground);
    padding: 0 16px; font-size: 14px; font-weight: 700; cursor: pointer; color: var(--ink);
  }
  .pill-btn:focus-visible { outline: 3px solid var(--ink); outline-offset: 2px; }

  .status { display: flex; align-items: center; gap: 6px; font-size: 13px; font-weight: 600; color: var(--sub); }
  .status.ok { color: #1E7B34; }
  .status.ok::before { content: ""; width: 8px; height: 8px; border-radius: 4px; background: #23A047; }

  .facts { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 8px; }
  .fact { background: var(--ground); border-radius: 14px; padding: 9px 12px; display: flex; flex-direction: column; gap: 2px; min-width: 0; }
  .fact.compact { flex-shrink: 0; }
  .fact-k { font-size: 11px; font-weight: 700; letter-spacing: 0.1em; text-transform: uppercase; color: var(--sub); }
  .fact-v { font-family: var(--mono); font-size: 14px; font-weight: 500; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }

  .section-label { margin: 6px 4px 0; font-size: 12px; font-weight: 700; letter-spacing: 0.12em; text-transform: uppercase; color: var(--sub); }

  .foot {
    flex-shrink: 0;
    width: 100%;
    max-width: var(--page-max);
    margin: 0 auto;
    padding: 8px 16px calc(18px + var(--safe-bottom));
  }
  .start {
    width: 100%; height: 60px; border-radius: 30px; border: none;
    background: var(--ink); color: #fff;
    display: flex; align-items: center; justify-content: space-between;
    padding: 0 24px; font-size: 18px; font-weight: 700; cursor: pointer;
    transition: transform var(--dur-fast);
  }
  .start svg { color: var(--mpx-yellow); }
  .start:active { transform: scale(0.98); }
  .start:focus-visible { outline: 3px solid var(--mpx-yellow); outline-offset: 3px; }
  .offline { margin-top: 10px; font-size: 13px; color: var(--sub); text-align: center; }
</style>
