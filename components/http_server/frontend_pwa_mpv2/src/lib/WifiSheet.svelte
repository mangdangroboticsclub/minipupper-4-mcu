<script>
  import WifiPicker from "./WifiPicker.svelte";

  /**
   * The Robot Wi-Fi sheet: pulled down from the Wi-Fi pill on Home.
   * @prop network  app-wide network state
   * @prop onClose  () => void
   */
  let { network, onClose } = $props();

  const staConnected = $derived(network?.staState === "connected");

  // Swipe the sheet back up to close it.
  let drag = $state(0);
  let start = null;
  function down(e) { start = e.clientY; }
  function move(e) { if (start !== null) drag = Math.min(0, e.clientY - start); }
  function up() {
    if (start === null) return;
    if (drag < -60) onClose?.();
    drag = 0;
    start = null;
  }

  function onKey(e) { if (e.key === "Escape") onClose?.(); }
</script>

<svelte:window onkeydown={onKey} />

<div class="scrim" role="presentation" onclick={onClose}></div>
<div class="sheet" role="dialog" aria-modal="true" aria-labelledby="wifi-sheet-title" style="transform: translateY({drag}px)">
  <div class="body">
    <div class="head">
      <h2 id="wifi-sheet-title">Robot Wi‑Fi</h2>
      <button type="button" class="close" onclick={onClose} aria-label="Close">
        <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" aria-hidden="true"><path d="M6 6l12 12M18 6L6 18"/></svg>
      </button>
    </div>
    <div class="now">
      <span class="ico" class:on={staConnected}>
        <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" aria-hidden="true"><path d="M2 8.5a15 15 0 0 1 20 0M5 12a10 10 0 0 1 14 0M8.5 15.5a5 5 0 0 1 7 0"/><circle cx="12" cy="19" r="1.2" fill="currentColor"/></svg>
      </span>
      <span class="now-txt">
        {#if staConnected}
          <b>{network?.staSsid}</b><span class="mono">{network?.staIp}</span>
        {:else}
          <b>Not on Wi‑Fi</b><span>Pick a network below</span>
        {/if}
      </span>
    </div>
    <WifiPicker {network} />
  </div>
  <div
    class="grab"
    role="presentation"
    onpointerdown={down}
    onpointermove={move}
    onpointerup={up}
    onpointercancel={up}
  ><span></span></div>
</div>

<style>
  .scrim { position: fixed; inset: 0; z-index: 60; background: rgba(17, 17, 17, 0.35); animation: fade var(--dur-med) var(--ease-out); }
  .sheet {
    position: fixed; left: 0; right: 0; top: 0; z-index: 61;
    max-width: var(--page-max); margin: 0 auto;
    max-height: 86dvh;
    display: flex; flex-direction: column;
    background: #fff;
    border-radius: 0 0 28px 28px;
    box-shadow: 0 18px 40px rgba(0, 0, 0, 0.2);
    padding-top: var(--safe-top);
    animation: drop var(--dur-slow) var(--ease-spring);
    transition: transform 120ms;
  }
  @keyframes drop { from { transform: translateY(-100%); } to { transform: translateY(0); } }
  @keyframes fade { from { opacity: 0; } to { opacity: 1; } }
  .body { flex: 1; min-height: 0; overflow-y: auto; padding: 14px 16px 6px; display: flex; flex-direction: column; gap: 14px; }
  .head { display: flex; align-items: center; justify-content: space-between; }
  .head h2 { font-size: 22px; font-weight: 800; letter-spacing: -0.01em; }
  .close { width: 44px; height: 44px; border-radius: 22px; border: none; background: var(--ground); color: var(--ink); display: grid; place-items: center; cursor: pointer; }
  .close:focus-visible { outline: 3px solid var(--ink); outline-offset: 2px; }
  .now { display: flex; align-items: center; gap: 12px; background: var(--ground); border-radius: 16px; padding: 12px 14px; }
  .ico { width: 38px; height: 38px; border-radius: 11px; background: var(--mpx-yellow); color: var(--ink); display: grid; place-items: center; flex-shrink: 0; }
  .ico.on { background: var(--ink); color: var(--mpx-yellow); }
  .now-txt { display: flex; flex-direction: column; gap: 1px; font-size: 13px; color: var(--sub); min-width: 0; }
  .now-txt b { font-size: 16px; color: var(--ink); }
  .mono { font-family: var(--mono); }
  .grab { height: 28px; flex-shrink: 0; display: grid; place-items: center; touch-action: none; cursor: grab; }
  .grab span { width: 40px; height: 5px; border-radius: 3px; background: #D9D4C7; }
</style>
