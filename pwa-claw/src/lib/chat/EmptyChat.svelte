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
    background: var(--page);
  }

  .center-area {
    flex: 1;
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    padding: 24px 16px;
    gap: 8px;
    max-width: var(--page-max);
    margin: 0 auto;
    width: 100%;
  }

  .pupper-badge {
    width: clamp(72px, 18vmin, 96px);
    height: clamp(72px, 18vmin, 96px);
    border-radius: 24px;
    background: var(--mpx-yellow);
    display: grid;
    place-items: center;
    margin-bottom: 8px;
    transition: filter var(--dur-slow);
  }
  .pupper-badge.asleep { filter: saturate(0.6); }

  .tagline { font-size: 22px; font-weight: 800; letter-spacing: -0.01em; color: var(--ink); text-align: center; }
  .subtitle { font-size: 14px; color: var(--sub); text-align: center; line-height: 1.45; max-width: 340px; }

  .suggestions {
    margin-top: 16px;
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 8px;
    width: 100%;
  }
  @media (max-width: 360px) { .suggestions { grid-template-columns: 1fr; } }

  .chip {
    display: flex;
    align-items: center;
    gap: 10px;
    min-height: 56px;
    padding: 10px 14px;
    border: 1px solid var(--line);
    border-radius: 18px;
    background: #fff;
    color: var(--ink);
    font-size: 14px;
    font-weight: 700;
    line-height: 1.3;
    text-align: left;
    cursor: pointer;
    transition: background var(--dur-fast), transform var(--dur-fast);
  }
  .chip:active:not(:disabled) { transform: scale(0.97); background: var(--mpx-yellow); }
  .chip:focus-visible { outline: 3px solid var(--ink); outline-offset: 2px; }
  .chip:disabled { opacity: 0.5; cursor: default; }
</style>
