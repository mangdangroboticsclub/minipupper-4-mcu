<script>
  import { colors } from "./colors.js";
  import WelcomeScreen from "./WelcomeScreen.svelte";

  let { network } = $props();

  /** "locked" → "unlocked" → "final" */
  let phase = $state("locked");
  let dragProgress = $state(0);
  let isDragging = $state(false);
  let trackEl = $state(null);

  const YELLOW = colors.mpx.primary; // #FFE605
  const UNLOCK_AT = 0.82;

  /* ── Pointer handlers ─────────────────── */
  function handlePointerDown(e) {
    if (isUnlocked) return;
    isDragging = true;
    e.preventDefault();
    e.currentTarget.setPointerCapture(e.pointerId);
    updateFromPointer(e);
  }

  function updateFromPointer(e) {
    if (!trackEl) return;
    const rect = trackEl.getBoundingClientRect();
    // Keep the handle centre inside the track.
    const handle = rect.height;
    const usable = Math.max(1, rect.width - handle);
    const x = e.clientX - rect.left - handle / 2;
    dragProgress = Math.max(0, Math.min(1, x / usable));
  }

  function handlePointerMove(e) {
    if (!isDragging) return;
    updateFromPointer(e);
  }

  function handlePointerUp() {
    if (!isDragging) return;
    isDragging = false;
    if (dragProgress >= UNLOCK_AT) unlock();
    else dragProgress = 0;
  }

  function handleKey(e) {
    if (isUnlocked) return;
    if (e.key === "Enter" || e.key === " " || e.key === "ArrowRight") {
      e.preventDefault();
      dragProgress = 1;
      unlock();
    }
  }

  function unlock() {
    dragProgress = 1;
    phase = "unlocked";
    if (navigator.vibrate) { try { navigator.vibrate(12); } catch {} }
    setTimeout(() => { phase = "final"; }, 1100);
    // Drop the lock UI from the DOM once its exit animation has played.
    setTimeout(() => { phase = "gone"; }, 1100 + 700);
  }

  /* ── Derived values ───────────────────── */
  const isUnlocked = $derived(phase !== "locked");
  const isFinal = $derived(phase === "final" || phase === "gone");
  const isGone = $derived(phase === "gone");
  const nearUnlock = $derived(dragProgress >= UNLOCK_AT);

</script>

<div class="root" style="--brand-yellow: {YELLOW}" class:final={isFinal}>
  <!-- ═══ LOCKED / UNLOCKED SCREEN ═══ -->
  {#if !isGone}
  <div class="screen" class:exit={isFinal}>
    <div class="center-group" style="--p: {dragProgress}">
      <!-- Eyes -->
      <div class="eyes-area" class:awake={!isUnlocked}>
          <!-- eye-open.svg (locked) -->
          <svg class="eyes eyes-open" viewBox="0 0 181 124" fill="none" aria-hidden="true">
            <g class="eye eye-l">
              <rect x="0.5" y="0.5" width="71" height="119" rx="17.5" fill="black" stroke="black"/>
              <path d="M16.5 110.5C11.9092 114.387 11.23 117.413 9 123H68C64.9899 117.521 63.1846 114.639 58.5 110.5C55.1314 107.523 53.1879 105.634 49 104C42.2693 101.374 34.4575 102.053 27.5 104C22.2014 105.483 20.6992 106.945 16.5 110.5Z" fill="#FCE505" stroke="#FFE605"/>
            </g>
            <g class="eye eye-r">
              <rect x="109.5" y="0.5" width="71" height="119" rx="17.5" fill="black" stroke="black"/>
              <path d="M122.5 110.224C117.909 114.111 117.23 117.137 115 122.724H174C170.99 117.245 169.185 114.363 164.5 110.224C161.131 107.247 159.188 105.358 155 103.724C148.269 101.098 140.457 101.777 133.5 103.724C128.201 105.207 126.699 106.669 122.5 110.224Z" fill="#FFE605" stroke="#FFE605"/>
            </g>
          </svg>
      </div>

      <!-- Brand name -->
      <img class="brand-name" src="/md.svg" alt="MangDang" />

      <!-- Lock icon -->
      <div class="lock-icon-wrapper" class:unlocked={isUnlocked}>
        <svg class="lock-svg" viewBox="0 0 40 44" fill="none" aria-hidden="true">
          <path class="shackle" d="M12 20V13a8 8 0 0 1 16 0v7" stroke="black" stroke-width="4" stroke-linecap="round"/>
          <rect class="lock-body" x="6" y="19" width="28" height="22" rx="6" fill="black"/>
          <circle cx="20" cy="29" r="2.6" fill="var(--brand-yellow)"/>
          <rect x="18.8" y="29" width="2.4" height="6" rx="1.2" fill="var(--brand-yellow)"/>
        </svg>
      </div>
    </div>

    <!-- Bottom: slider -->
    <div class="slider-section">
      <div
        class="slider-track"
        class:unlocked={isUnlocked}
        class:near={nearUnlock && !isUnlocked}
        class:dragging={isDragging}
        role="slider"
        tabindex="0"
        aria-label="Slide to unlock"
        aria-valuemin="0"
        aria-valuemax="100"
        aria-valuenow={Math.round(dragProgress * 100)}
        bind:this={trackEl}
        onpointerdown={handlePointerDown}
        onpointermove={handlePointerMove}
        onpointerup={handlePointerUp}
        onpointercancel={handlePointerUp}
        onkeydown={handleKey}
        style="--p: {dragProgress}"
      >
        <div class="slider-fill"></div>

        <span class="slider-hint" class:hidden={dragProgress > 0.15 || isUnlocked}>
          Slide to unlock
        </span>
        <span class="slider-hint slider-hint-done" class:show={isUnlocked}>
          Welcome back
        </span>

        <div class="slider-handle">
          {#if isUnlocked}
            <svg class="handle-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="3" stroke-linecap="round" stroke-linejoin="round">
              <path d="M5 13l4 4L19 7"/>
            </svg>
          {:else}
            <svg class="handle-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.6" stroke-linecap="round" stroke-linejoin="round">
              <path d="M5 12h14M13 5l7 7-7 7"/>
            </svg>
          {/if}
        </div>
      </div>
    </div>
  </div>
  {/if}

  <!-- ═══ FINAL SCREEN → Welcome / Setup ═══ -->
  {#if isFinal}
    <WelcomeScreen {network} />
  {/if}
</div>

<style>
  /* ── Root ─────────────────────────────── */
  .root {
    position: relative;
    width: 100%;
    height: 100dvh;
    background-color: var(--brand-yellow);
    overflow: hidden;
    transition: background-color 0.8s ease;
  }
  .root.final { background-color: var(--page); }

  /* ── Screen ───────────────────────────── */
  .screen {
    position: absolute;
    inset: 0;
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: space-between;
    padding:
      calc(var(--safe-top) + clamp(14px, 3vh, 28px))
      clamp(20px, 6vw, 48px)
      calc(var(--safe-bottom) + clamp(20px, 5vh, 48px));
    background:
      radial-gradient(ellipse 90% 55% at 50% 30%, rgba(255,255,255,0.28), transparent 70%);
    transition: opacity 0.6s ease, transform 0.6s ease;
  }
  .screen.exit {
    opacity: 0;
    transform: scale(0.96);
    pointer-events: none;
  }

  /* ── Centre group ─────────────────────── */
  .center-group {
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    flex: 1;
    min-height: 0;
    width: 100%;
    /* the whole identity block lifts slightly as you slide */
    transform: translateY(calc(var(--p, 0) * -8px));
    transition: transform 0.12s linear;
  }

  .eyes-area {
    display: flex;
    justify-content: center;
    align-items: flex-end;
    width: clamp(120px, 38vmin, 220px);
    height: clamp(82px, 26vmin, 150px);
  }
  .eyes {
    display: block;
    width: 100%;
    height: auto;
    max-height: 100%;
  }

  /* blink: squash the open eyes from their vertical middle */
  .eyes-area.awake .eye {
    transform-origin: 50% 55%;
    transform-box: fill-box;
    animation: blink 1.9s ease-in-out infinite;
  }
  .eyes-area.awake .eye-r { animation-delay: 0.05s; }
  @keyframes blink {
    0%, 88%, 100% { transform: scaleY(1); }
    94% { transform: scaleY(0.08); }
  }

  /* ── Brand name ───────────────────────── */
  .brand-name {
    width: clamp(140px, 42vmin, 210px);
    height: auto;
    display: block;
    margin-top: clamp(14px, 3.5vh, 28px);
  }

  /* ── Lock icon ────────────────────────── */
  .lock-icon-wrapper {
    margin-top: clamp(12px, 3vh, 22px);
    width: clamp(28px, 6vmin, 36px);
    height: clamp(30px, 6.6vmin, 40px);
    display: grid;
    place-items: center;
    opacity: 0.9;
  }
  .lock-svg { width: 100%; height: 100%; overflow: visible; }
  .shackle {
    transform-box: fill-box;
    transform-origin: 90% 100%;
    /* shackle eases open as you slide */
    transform: rotate(calc(var(--p, 0) * -18deg)) translateY(calc(var(--p, 0) * -2px));
    transition: transform 0.12s linear;
  }
  .lock-icon-wrapper.unlocked .shackle {
    transform: rotate(-38deg) translateY(-4px);
    transition: transform var(--dur-slow) var(--ease-spring);
  }
  .lock-icon-wrapper.unlocked .lock-body {
    animation: lock-pop var(--dur-slow) var(--ease-spring);
  }
  @keyframes lock-pop {
    0% { transform: scale(1); }
    40% { transform: scale(1.12); }
    100% { transform: scale(1); }
  }

  /* ── Slider ───────────────────────────── */
  .slider-section {
    width: 100%;
    display: flex;
    justify-content: center;
    flex-shrink: 0;
  }

  .slider-track {
    --h: clamp(56px, 8.5vmin, 66px);
    position: relative;
    width: min(100%, 380px);
    height: var(--h);
    border-radius: var(--r-pill);
    background: rgba(0, 0, 0, 0.1);
    box-shadow: inset 0 2px 6px rgba(0, 0, 0, 0.08);
    touch-action: none;
    user-select: none;
    -webkit-user-select: none;
    cursor: grab;
    outline: none;
    transition: background-color var(--dur-med) ease, transform var(--dur-fast) ease;
    overflow: hidden;
  }
  .slider-track:focus-visible { box-shadow: 0 0 0 3px rgba(0,0,0,0.35); }
  .slider-track.dragging { cursor: grabbing; }
  .slider-track.near { background: rgba(0, 0, 0, 0.16); }
  .slider-track.unlocked {
    background: #000;
    cursor: default;
  }

  .slider-fill {
    position: absolute;
    left: 0;
    top: 0;
    bottom: 0;
    /* fill to the trailing edge of the handle */
    width: calc(var(--h) + (100% - var(--h)) * var(--p, 0));
    background: rgba(0, 0, 0, 0.14);
    border-radius: var(--r-pill);
    pointer-events: none;
  }
  .slider-track.unlocked .slider-fill { width: 100%; background: transparent; }

  .slider-hint {
    position: absolute;
    inset: 0;
    display: flex;
    align-items: center;
    justify-content: center;
    padding-left: calc(var(--h) * 0.6);
    font-size: 0.92rem;
    font-weight: 700;
    letter-spacing: 0.04em;
    color: rgba(0, 0, 0, 0.55);
    pointer-events: none;
    transition: opacity var(--dur-med) ease;
    /* shimmer */
    background: linear-gradient(90deg, rgba(0,0,0,0.55) 0%, rgba(0,0,0,0.55) 40%, rgba(0,0,0,0.95) 50%, rgba(0,0,0,0.55) 60%, rgba(0,0,0,0.55) 100%);
    background-size: 220% 100%;
    -webkit-background-clip: text;
    background-clip: text;
    -webkit-text-fill-color: transparent;
    animation: shimmer 2.4s linear infinite;
  }
  .slider-hint.hidden { opacity: 0; }
  @keyframes shimmer {
    from { background-position: 120% 0; }
    to   { background-position: -120% 0; }
  }

  .slider-hint-done {
    opacity: 0;
    animation: none;
    -webkit-text-fill-color: currentColor;
    background: none;
    color: #fff;
    padding-left: 0;
    padding-right: calc(var(--h) * 0.6);
  }
  .slider-hint-done.show { opacity: 1; }

  /* ── Handle ───────────────────────────── */
  .slider-handle {
    --size: calc(var(--h) - 10px);
    position: absolute;
    top: 5px;
    left: 5px;
    width: var(--size);
    height: var(--size);
    border-radius: 50%;
    background: #000;
    color: var(--brand-yellow);
    display: grid;
    place-items: center;
    box-shadow: 0 4px 14px rgba(0, 0, 0, 0.25);
    will-change: left, transform;
    z-index: 2;
  }
  /* the handle's left edge is driven by --p over the free track space */
  .slider-track .slider-handle {
    left: calc(5px + var(--p, 0) * (100% - var(--h)));
    transition: left 0.05s linear, transform var(--dur-fast) ease, background var(--dur-med) ease;
  }
  .slider-track:not(.dragging):not(.unlocked) .slider-handle {
    transition: left var(--dur-slow) var(--ease-spring), transform var(--dur-fast) ease;
  }
  .slider-track.dragging .slider-handle { transform: scale(1.06); }
  .slider-track.unlocked .slider-handle {
    background: var(--brand-yellow);
    color: #000;
    animation: handle-pop var(--dur-slow) var(--ease-spring);
  }
  @keyframes handle-pop {
    0% { transform: scale(1); }
    45% { transform: scale(1.18); }
    100% { transform: scale(1); }
  }

  /* idle nudge: gently invite the user to slide */
  .slider-track:not(.dragging):not(.unlocked) .slider-handle .handle-icon {
    animation: nudge 2.4s ease-in-out infinite;
  }
  @keyframes nudge {
    0%, 60%, 100% { transform: translateX(0); }
    75% { transform: translateX(4px); }
    88% { transform: translateX(0); }
  }

  .handle-icon {
    width: 44%;
    height: 44%;
  }

  /* ── Short viewports (landscape phones) ── */
  @media (max-height: 520px) {
    .brand-name { margin-top: 8px; }
    .lock-icon-wrapper { margin-top: 8px; }
    .eyes-area { height: clamp(60px, 22vmin, 110px); }
  }
</style>
