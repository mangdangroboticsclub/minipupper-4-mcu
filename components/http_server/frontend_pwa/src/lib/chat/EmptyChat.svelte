<script>
  import Eyes from "../Eyes.svelte";

  let { onSuggest } = $props();

  /* Things the robot can actually do from this box, phrased the way a person
   * would type them. Tapping one fills the composer; nothing is sent. */
  const SUGGESTIONS = [
    { icon: "🎨", text: "Draw a crab on your screen" },
    { icon: "🐾", text: "Wave hello, then sit" },
    { icon: "💡", text: "Change the display to green" },
    { icon: "❓", text: "What can you do?" },
  ];
</script>

<div class="empty-chat">
  <div class="center-area">
    <div class="dog-icon">
      <span class="eyes-blink"><Eyes size={44} /></span>
    </div>
    <p class="eyebrow">Mini Pupper</p>
    <h2 class="tagline">What shall we do today?</h2>
    <p class="subtitle">Talk to your pupper in plain words. It can move, draw on its screen and run skills.</p>

    <div class="suggestions">
      {#each SUGGESTIONS as s}
        <button class="suggestion" onclick={() => onSuggest?.(s.text)}>
          <span class="suggestion-icon">{s.icon}</span>
          <span class="suggestion-text">{s.text}</span>
          <span class="suggestion-arrow">
            <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.6" stroke-linecap="round" stroke-linejoin="round"><line x1="5" y1="12" x2="19" y2="12"/><polyline points="12 5 19 12 12 19"/></svg>
          </span>
        </button>
      {/each}
    </div>
  </div>
</div>

<style>
  .empty-chat {
    display: flex;
    flex-direction: column;
    height: 100%;
    background: transparent;
  }

  .center-area {
    flex: 1;
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    padding: 0 20px 12px;
    gap: 8px;
    max-width: 520px;
    width: 100%;
    margin: 0 auto;
  }

  .dog-icon {
    width: 88px;
    height: 88px;
    border-radius: 28px;
    display: flex;
    align-items: center;
    justify-content: center;
    color: var(--accent);
    background: radial-gradient(circle at 50% 30%, rgba(255,255,255,0.08), transparent 60%), var(--surface-1);
    border: 1px solid var(--line);
    box-shadow: inset 0 1px 0 var(--highlight), var(--shadow-card), 0 0 48px rgba(255, 230, 5, 0.14);
    margin-bottom: 10px;
  }
  .dog-icon :global(svg) { filter: drop-shadow(0 0 10px rgba(255, 230, 5, 0.5)); }

  .eyebrow {
    font-size: 10.5px;
    font-weight: 700;
    letter-spacing: 0.22em;
    text-transform: uppercase;
    color: var(--accent-ui);
  }
  .tagline {
    font-size: 22px;
    font-weight: 700;
    letter-spacing: -0.01em;
    color: var(--text-1);
    text-align: center;
  }
  .subtitle {
    font-size: 13.5px;
    color: var(--text-3);
    text-align: center;
    line-height: 1.5;
    max-width: 320px;
  }

  .suggestions {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 8px;
    width: 100%;
    margin-top: 18px;
  }
  @media (max-width: 380px) { .suggestions { grid-template-columns: 1fr; } }
  .suggestion {
    display: flex;
    align-items: center;
    gap: 10px;
    text-align: left;
    padding: 12px 12px 12px 14px;
    border-radius: var(--r-md);
    background: var(--surface-1);
    border: 1px solid var(--line);
    box-shadow: inset 0 1px 0 var(--highlight);
    color: var(--text-1);
    font-family: inherit;
    font-size: 13.5px;
    font-weight: 500;
    cursor: pointer;
    transition: transform 240ms var(--ease), border-color 240ms var(--ease), background-color 240ms var(--ease);
  }
  .suggestion:hover { border-color: var(--line-strong); background: var(--surface-2); }
  .suggestion:hover .suggestion-arrow { transform: translateX(2px); color: var(--accent-ui); }
  .suggestion:active { transform: scale(0.98); }
  .suggestion-icon { font-size: 16px; line-height: 1; flex-shrink: 0; }
  .suggestion-text { flex: 1; min-width: 0; line-height: 1.3; }
  .suggestion-arrow {
    width: 24px; height: 24px; border-radius: 50%;
    display: inline-flex; align-items: center; justify-content: center;
    background: var(--surface-2); color: var(--text-3);
    flex-shrink: 0;
    transition: transform 240ms var(--ease), color 240ms var(--ease);
  }
</style>
