<script>
  import WelcomeScreen from "./WelcomeScreen.svelte";
  import Wordmark from "./Wordmark.svelte";
  import Eyes from "./Eyes.svelte";

  let { network } = $props();

  /** "locked" → "unlocked" → "final" */
  let phase = $state("locked");
  let dragProgress = $state(0);
  let isDragging = $state(false);


  /* ── Pointer handlers ─────────────────── */
  function handlePointerDown(e) {
    if (isUnlocked) return;
    isDragging = true;
    e.preventDefault();
    e.currentTarget.setPointerCapture(e.pointerId);
  }

  function handlePointerMove(e) {
    if (!isDragging) return;
    const rect = e.currentTarget.getBoundingClientRect();
    const progress = Math.max(0, Math.min(1, (e.clientX - rect.left) / rect.width));
    dragProgress = progress;
  }

  function handlePointerUp() {
    if (!isDragging) return;
    isDragging = false;

    if (dragProgress >= 0.85) {
      phase = "unlocked";
      setTimeout(() => {
        phase = "final";
      }, 1200);
    } else {
      dragProgress = 0;
    }
  }

  /* ── Derived values ───────────────────── */
  const isUnlocked = $derived(phase === "unlocked" || phase === "final");
  const isFinal = $derived(phase === "final");

  const lockIconRotation = $derived(dragProgress * -360);
  const handleX = $derived(`${dragProgress * 100}%`);
  const sliderFill = $derived(dragProgress * 100);
</script>

<div class="root" class:final={isFinal}>
  <div class="screen" class:exit={isFinal}>
    <div class="center-group">
      <div class="eyes-stage">
        <div class="bloom"></div>
        <div class="eyes eyes-blink" class:closed={isUnlocked}>
          <Eyes open={!isUnlocked} size={160} />
        </div>
      </div>
      <div class="brand">
        <Wordmark height={30} class="wordmark" />
        <p class="product">Mini Pupper</p>
      </div>
    </div>

    <div class="slider-section">
      <div class="slider-shell">
        <div
          class="slider-track"
          class:unlocked={isUnlocked}
          role="slider"
          tabindex="0"
          aria-label="Slide to unlock"
          aria-valuenow={Math.round(dragProgress * 100)}
          onpointerdown={handlePointerDown}
          onpointermove={handlePointerMove}
          onpointerup={handlePointerUp}
          onpointerleave={handlePointerUp}
        >
          <div class="slider-fill" style="width: calc(30px + (100% - 30px) * {isUnlocked ? 1 : dragProgress})"></div>
          <span class="slider-hint" style="opacity: {isUnlocked ? 0 : 1 - dragProgress * 1.6}">
            <span class="shimmer">Slide to wake</span>
          </span>
          <div class="slider-handle" class:dragging={isDragging} class:done={isUnlocked}
               style="left: calc(4px + (100% - 60px) * {isUnlocked ? 1 : dragProgress})">
            {#if isUnlocked}
              <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><path d="M5 13l4 4L19 7"/></svg>
            {:else}
              <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.25" stroke-linecap="round" stroke-linejoin="round"><path d="M5 12h14M13 5l7 7-7 7"/></svg>
            {/if}
          </div>
        </div>
      </div>
      <p class="fineprint">MangDang Robotics</p>
    </div>
  </div>

  {#if isFinal}
    <WelcomeScreen {network} />
  {/if}
</div>

<style>
  .root {
    position: relative;
    width: 100%;
    height: 100dvh;
    background: var(--bg);
    background-image: radial-gradient(90% 55% at 50% 38%, rgba(255, 230, 5, 0.10), transparent 60%);
    overflow: hidden;
  }

  .screen {
    position: absolute;
    inset: 0;
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: space-between;
    padding: max(48px, env(safe-area-inset-top)) 22px max(22px, env(safe-area-inset-bottom));
    transition: opacity 0.5s var(--ease), transform 0.5s var(--ease);
  }
  .screen.exit {
    opacity: 0;
    transform: scale(0.98) translateY(-8px);
    pointer-events: none;
  }

  .center-group {
    flex: 1;
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    gap: 34px;
  }

  /* The eyes float in a soft bloom of their own light. */
  .eyes-stage {
    position: relative;
    width: 240px;
    height: 200px;
    display: flex;
    align-items: center;
    justify-content: center;
  }
  .bloom {
    position: absolute;
    inset: 0;
    border-radius: 50%;
    background: radial-gradient(closest-side, rgba(255, 230, 5, 0.28), rgba(255, 230, 5, 0.06) 55%, transparent 75%);
    filter: blur(6px);
    animation: bloom 4.6s var(--ease) infinite;
  }
  @keyframes bloom {
    0%, 100% { transform: scale(1);    opacity: 0.9; }
    50%      { transform: scale(1.06); opacity: 1;   }
  }
  .eyes {
    position: relative;
    color: var(--accent);
    display: flex;
    align-items: center;
    height: 112px;
    filter: drop-shadow(0 0 18px rgba(255, 230, 5, 0.45));
    transition: transform 0.3s var(--ease);
  }
  .eyes.closed { animation: none; }

  .brand {
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: 10px;
    color: var(--text-1);
  }
  .product {
    font-size: 15px;
    font-weight: 500;
    letter-spacing: 0.14em;
    text-transform: uppercase;
    color: var(--text-3);
  }

  /* ── Slider ─────────────────────────────────────────────────────────── */
  .slider-section {
    width: 100%;
    max-width: 360px;
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: 16px;
  }
  .slider-shell {
    width: 100%;
    padding: 5px;
    border-radius: var(--r-pill);
    background: var(--shell);
    border: 1px solid var(--line);
    box-shadow: var(--shadow-card);
  }
  .slider-track {
    position: relative;
    height: 60px;
    border-radius: var(--r-pill);
    background: var(--surface-1);
    box-shadow: inset 0 1px 0 var(--highlight);
    overflow: hidden;
    cursor: grab;
    touch-action: none;
    user-select: none;
  }
  .slider-track:active { cursor: grabbing; }
  .slider-fill {
    position: absolute;
    inset: 0 auto 0 0;
    border-radius: var(--r-pill);
    background: var(--accent-soft);
    transition: width 0.05s linear;
  }
  .slider-track.unlocked .slider-fill { background: var(--accent); transition: width 0.3s var(--ease); }
  .slider-hint {
    position: absolute;
    inset: 0;
    display: flex;
    align-items: center;
    justify-content: center;
    padding-left: 48px;
    font-size: 15px;
    font-weight: 500;
    color: var(--text-2);
    pointer-events: none;
    transition: opacity 0.15s;
  }
  .shimmer {
    background: linear-gradient(90deg, var(--text-2) 0%, var(--text-2) 40%, var(--text-1) 50%, var(--text-2) 60%, var(--text-2) 100%);
    background-size: 220% 100%;
    -webkit-background-clip: text;
    background-clip: text;
    color: transparent;
    animation: shimmer 2.6s linear infinite;
  }
  @keyframes shimmer { from { background-position: 120% 0; } to { background-position: -120% 0; } }
  .slider-handle {
    position: absolute;
    top: 4px;
    bottom: 4px;
    aspect-ratio: 1;
    border-radius: 50%;
    background: var(--accent);
    color: var(--accent-ink);
    display: flex;
    align-items: center;
    justify-content: center;
    box-shadow: 0 6px 18px rgba(0, 0, 0, 0.4), 0 0 0 0 rgba(255, 230, 5, 0);
    transition: left 0.2s var(--ease), box-shadow 0.2s var(--ease);
  }
  .slider-handle.dragging { transition: none; box-shadow: 0 6px 18px rgba(0, 0, 0, 0.4), var(--accent-glow); }
  .slider-handle.done { background: var(--accent-ink); color: var(--accent); transition: left 0.3s var(--ease); }
  .slider-handle svg { width: 22px; height: 22px; }

  .fineprint {
    font-size: 11px;
    letter-spacing: 0.12em;
    text-transform: uppercase;
    color: var(--text-3);
  }
</style>
