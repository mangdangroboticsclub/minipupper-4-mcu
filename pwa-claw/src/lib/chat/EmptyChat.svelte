<script>
  import PupperEyes from "../PupperEyes.svelte";

  /**
   * @prop online    — agent channel connected
   * @prop onSuggest — (text) => void, fills/sends a starter prompt
   */
  let { online = true, onSuggest } = $props();

  const SUGGESTIONS = [
    { icon: "🐾", text: "Do a little dance" },
    { icon: "👋", text: "Wave hello and sit down" },
    { icon: "🔍", text: "What skills do you have?" },
    { icon: "🛠️", text: "Write a Lua script that makes you nod twice" },
  ];
</script>

<div class="empty-chat">
  <div class="center-area">
    <div class="pupper-badge" class:asleep={!online}>
      <PupperEyes awake={online} size="58%" mood="happy" />
    </div>

    <h2 class="tagline">{online ? "Pupper is listening" : "Waking Pupper up…"}</h2>
    <p class="subtitle">
      {#if online}
        Tell it what to do in plain words. It plans the steps, writes the
        robot commands, and asks before touching files.
      {:else}
        One moment. You can type as soon as Pupper is awake.
      {/if}
    </p>

    <div class="suggestions" aria-label="Suggested prompts">
      {#each SUGGESTIONS as s}
        <button class="chip" onclick={() => onSuggest?.(s.text)} disabled={!online}>
          <span aria-hidden="true">{s.icon}</span>
          <span>{s.text}</span>
        </button>
      {/each}
    </div>
  </div>
</div>

<style>
  .empty-chat {
    display: flex;
    flex-direction: column;
    flex: 1;
    min-height: 0;
    overflow-y: auto;
    background: #fff;
  }

  .center-area {
    flex: 1;
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    padding: 24px;
    gap: 10px;
    max-width: 480px;
    margin: 0 auto;
    width: 100%;
  }

  .pupper-badge {
    width: clamp(72px, 18vmin, 104px);
    height: clamp(72px, 18vmin, 104px);
    border-radius: var(--r-lg);
    background: var(--mpx-yellow);
    display: grid;
    place-items: center;
    box-shadow: var(--shadow-card);
    margin-bottom: 6px;
    transition: filter var(--dur-slow);
  }
  .pupper-badge.asleep { filter: saturate(0.6); }

  .tagline { font-size: 1.05rem; font-weight: 800; color: var(--mpx-ink); text-align: center; }
  .subtitle { font-size: 0.8rem; color: var(--mpx-muted); text-align: center; line-height: 1.5; max-width: 340px; }

  .suggestions {
    margin-top: 14px;
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 8px;
    width: 100%;
  }
  @media (max-width: 360px) { .suggestions { grid-template-columns: 1fr; } }

  .chip {
    display: flex;
    align-items: center;
    gap: 8px;
    padding: 11px 12px;
    border: 1px solid var(--mpx-line);
    border-radius: var(--r-md);
    background: var(--mpx-surface);
    color: var(--mpx-ink-soft);
    font-size: 0.78rem;
    font-weight: 600;
    text-align: left;
    cursor: pointer;
    transition: border-color var(--dur-fast), transform var(--dur-fast), background var(--dur-fast);
  }
  .chip:hover:not(:disabled) { border-color: var(--mpx-yellow); background: #fffef2; }
  .chip:active:not(:disabled) { transform: scale(0.98); }
  .chip:disabled { opacity: 0.5; cursor: default; }
</style>
