<script>
  import ActionIcon from "./ActionIcon.svelte";

  /**
   * Every move the robot knows, to run or pin to Quick actions.
   *
   * @prop actions      built-in moves [{ id, label, emoji }]
   * @prop custom       moves the user added [{ id, label, emoji }]
   * @prop pinned       pinned ids
   * @prop onRun        (id) => void
   * @prop onTogglePin  (id) => void
   * @prop onDelete     (id) => void — custom moves only
   * @prop onAdd        () => void
   * @prop onBack       () => void
   */
  let { actions = [], custom = [], pinned = [], onRun, onTogglePin, onDelete, onAdd, onBack } = $props();

  let query = $state("");
  const match = (a) => !query.trim() || a.label.toLowerCase().includes(query.trim().toLowerCase());
  const shownBuiltIn = $derived(actions.filter(match));
  const shownCustom = $derived(custom.filter(match));
</script>

{#snippet tile(a, isCustom)}
  {@const on = pinned.includes(a.id)}
  <li class="tile">
    <button type="button" class="run" onclick={() => onRun?.(a.id)} aria-label="Run {a.label}">
      <span class="ico"><ActionIcon id={a.id} emoji={a.emoji} /></span>
      <span class="lbl">{a.label}</span>
    </button>
    <button type="button" class="pin" class:on onclick={() => onTogglePin?.(a.id)} aria-pressed={on} aria-label={on ? `Unpin ${a.label}` : `Pin ${a.label} to Quick actions`}>
      <svg width="14" height="14" viewBox="0 0 24 24" fill={on ? "currentColor" : "none"} stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M12 17v5"/><path d="M5 17h14v-2.5c0-.8-.7-1.5-1.5-1.5h-11c-.8 0-1.5.7-1.5 1.5z"/><path d="M12 13V7L9.7 2.3h4.6L12 7"/></svg>
    </button>
    {#if isCustom}
      <button type="button" class="del" onclick={() => onDelete?.(a.id)} aria-label="Delete {a.label}">
        <svg width="13" height="13" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" aria-hidden="true"><path d="M6 6l12 12M18 6L6 18"/></svg>
      </button>
    {/if}
  </li>
{/snippet}

<div class="page">
  <header class="bar">
    <button type="button" class="round" onclick={onBack} aria-label="Back">
      <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M15 6l-6 6 6 6"/></svg>
    </button>
    <h1>All moves</h1>
    <button type="button" class="round" onclick={onAdd} aria-label="Add a move">
      <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" aria-hidden="true"><path d="M12 5v14M5 12h14"/></svg>
    </button>
  </header>

  <div class="scroll">
    <div class="inner">
      <label class="search">
        <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" aria-hidden="true"><circle cx="11" cy="11" r="7"/><path d="M20 20l-4-4"/></svg>
        <span class="sr">Search moves</span>
        <input type="search" bind:value={query} placeholder="Search moves" />
      </label>
      <p class="hint">Tap a move to run it. The pin puts it in Quick actions on Home.</p>

      {#if shownCustom.length}
        <h2>Your moves</h2>
        <ul class="grid">{#each shownCustom as a (a.id)}{@render tile(a, true)}{/each}</ul>
      {/if}

      <h2>Built in</h2>
      {#if shownBuiltIn.length}
        <ul class="grid">{#each shownBuiltIn as a (a.id)}{@render tile(a, false)}{/each}</ul>
      {:else}
        <p class="hint">No move called “{query}”.</p>
      {/if}
    </div>
  </div>
</div>

<style>
  .page { position: absolute; inset: 0; z-index: 20; background: var(--page); display: flex; flex-direction: column; }
  .bar { height: 64px; flex-shrink: 0; display: flex; align-items: center; justify-content: space-between; padding: 8px 16px 0; }
  .bar h1 { font-size: 18px; font-weight: 800; }
  .round {
    width: 44px; height: 44px; border-radius: 22px; border: none; background: var(--ground); color: var(--ink);
    display: grid; place-items: center; cursor: pointer;
  }
  .scroll { flex: 1; min-height: 0; overflow-y: auto; }
  .inner { max-width: var(--page-max); margin: 0 auto; padding: 8px 16px calc(24px + var(--safe-bottom)); display: flex; flex-direction: column; gap: 10px; }
  .search { display: flex; align-items: center; gap: 8px; height: 46px; padding: 0 14px; border-radius: 23px; background: var(--ground); color: var(--sub); }
  .search input { flex: 1; min-width: 0; border: none; background: transparent; font-size: 16px; color: var(--ink); outline: none; }
  .search:focus-within { outline: 3px solid var(--ink); outline-offset: 2px; }
  .hint { font-size: 13px; color: var(--sub); padding: 0 4px; }
  h2 { margin: 8px 4px 0; font-size: 12px; font-weight: 800; letter-spacing: 0.12em; text-transform: uppercase; color: var(--sub); }
  .grid { list-style: none; display: grid; grid-template-columns: repeat(auto-fill, minmax(96px, 1fr)); gap: 8px; }
  .tile { position: relative; }
  .run {
    width: 100%; min-height: 96px; border: 1px solid var(--line); border-radius: 18px; background: #fff; color: var(--ink);
    display: flex; flex-direction: column; align-items: center; justify-content: center; gap: 6px;
    padding: 12px 6px; cursor: pointer; transition: transform var(--dur-fast);
  }
  .run:active { transform: scale(0.97); background: var(--mpx-yellow); }
  .ico { font-size: 24px; line-height: 1; }
  .lbl { font-size: 13px; font-weight: 700; text-align: center; line-height: 1.2; }
  .pin, .del {
    position: absolute; top: 4px; width: 32px; height: 32px; border-radius: 16px; border: none;
    background: transparent; color: #A39F96; display: grid; place-items: center; cursor: pointer;
  }
  .pin { right: 4px; }
  .pin.on { color: var(--ink); background: var(--mpx-yellow); }
  .del { left: 4px; color: #B4402F; }
  button:focus-visible { outline: 3px solid var(--ink); outline-offset: 2px; }
  .sr { position: absolute; width: 1px; height: 1px; overflow: hidden; clip: rect(0 0 0 0); white-space: nowrap; }
</style>
