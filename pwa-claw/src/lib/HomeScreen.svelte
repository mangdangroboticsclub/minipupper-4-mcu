<script>
  import { colors } from "./colors.js";
  import Controller from "./Controller.svelte";
  import ActionIcon from "./ActionIcon.svelte";
  import ActionAssignment from "./ActionAssignment.svelte";
  import AddActionView from "./AddActionView.svelte";
  import AllActions from "./AllActions.svelte";
  import ChatView from "./chat/ChatView.svelte";
  import SkillsMarketplace from "./SkillsMarketplace.svelte";
  import SkillsManagement from "./SkillsManagement.svelte";
  import SettingsPage from "./SettingsPage.svelte";
  import WifiSheet from "./WifiSheet.svelte";

  let { network } = $props();

  let activeTab = $state("home");
  let showAllActions = $state(false);
  let showAddAction = $state(false);
  let showActionAssignment = $state(false);
  let showWifi = $state(false);

  const online = $derived(network?.linkState === "online");
  const staConnected = $derived(network?.staState === "connected");

  // ── Moves catalogue ─────────────────────────────────────────
  const actions = [
    { id: "advance",  label: "Forward",      emoji: "🚶", color: colors.mpx.action_green, category: "gait" },
    { id: "back",     label: "Backward",     emoji: "🔙", color: colors.mpx.action_green, category: "gait" },
    { id: "left",     label: "Strafe Left",  emoji: "◀️",  color: colors.mpx.action_green, category: "gait" },
    { id: "right",    label: "Strafe Right", emoji: "▶️",  color: colors.mpx.action_green, category: "gait" },
    { id: "turnL",    label: "Turn Left",    emoji: "↺",  color: "#0E8C8C",              category: "gait" },
    { id: "turnR",    label: "Turn Right",   emoji: "↻",  color: "#0E8C8C",              category: "gait" },
    { id: "stanford", label: "Trot",         emoji: "🐕", color: "#14A37F",              category: "gait" },
    { id: "step",     label: "Step",         emoji: "🦶", color: "#14A37F",              category: "gait" },
    { id: "testspeed",label: "Speed Test",   emoji: "⚡",  color: "#D14949",              category: "gait" },
    { id: "jump",     label: "Jump",         emoji: "🤸", color: "#D97A29",              category: "gait" },
    { id: "jumpfwd",  label: "Jump Fwd",     emoji: "🏃", color: "#C4901A",              category: "gait" },
    { id: "flegR",    label: "Lift FR",      emoji: "🦵", color: "#5FAD41",              category: "gait" },
    { id: "flegL",    label: "Lift FL",      emoji: "🦵", color: "#5FAD41",              category: "gait" },
    { id: "blegR",    label: "Lift RR",      emoji: "🦵", color: "#5FAD41",              category: "gait" },
    { id: "blegL",    label: "Lift RL",      emoji: "🦵", color: "#5FAD41",              category: "gait" },
    { id: "heightup",   label: "Height Up",   emoji: "⬆️",  color: "#D96098",              category: "gait" },
    { id: "heightdown", label: "Height Down", emoji: "⬇️",  color: "#D96098",              category: "gait" },
    { id: "moveLF", label: "Diag FL", emoji: "↗️",  color: "#4A8BC2",              category: "gait" },
    { id: "moveRF", label: "Diag FR", emoji: "↖️",  color: "#4A8BC2",              category: "gait" },
    { id: "moveLB", label: "Diag BL", emoji: "↘️",  color: "#4A8BC2",              category: "gait" },
    { id: "moveRB", label: "Diag BR", emoji: "↙️",  color: "#4A8BC2",              category: "gait" },
    { id: "stretch",  label: "Stretch",      emoji: "🧘", color: "#D96098",               category: "gait" },
    { id: "twerk",    label: "Twerk",        emoji: "💃", color: colors.mpx.action_purple, category: "gait" },
    { id: "roll",     label: "Roll",         emoji: "🔄", color: "#14A37F",               category: "gait" },
    { id: "pitch",    label: "Pitch",        emoji: "📐", color: "#14A37F",               category: "gait" },
    { id: "balance",  label: "Balance",      emoji: "⚖️",  color: "#14A37F",               category: "gait" },
    { id: "init",     label: "Init",         emoji: "🏁", color: "#7C7F7C",               category: "gait" },
    { id: "none",     label: "Stop",         emoji: "⏹️",  color: "#D14949",               category: "gait" },
    { id: "frontkick",label: "Front Kick",   emoji: "🦶", color: "#C44569",               category: "gait" },
    { id: "wiggle",   label: "Wiggle",       emoji: "🐕", color: "#C44569",               category: "gait" },
    { id: "wiggleL",  label: "Wiggle ◀",     emoji: "◀️",  color: "#C44569",               category: "gait" },
    { id: "wiggleR",  label: "Wiggle ▶",     emoji: "▶️",  color: "#C44569",               category: "gait" },
    { id: "buttshrug",  label: "Butt Shrug",   emoji: "🍑", color: "#C44569",             category: "gait" },
    { id: "buttshrugL", label: "Shrug ◀",      emoji: "◀️",  color: "#C44569",             category: "gait" },
    { id: "buttshrugR", label: "Shrug ▶",      emoji: "▶️",  color: "#C44569",             category: "gait" },
    { id: "bowback",  label: "Bow",          emoji: "🙇", color: "#AD5FBF",               category: "gait" },
    { id: "bodycycle",label: "Body Circle",  emoji: "🔄", color: "#AD5FBF",               category: "gait" },
    { id: "headellipse",label: "Head Circle", emoji: "🔄", color: "#AD5FBF",              category: "gait" },
    { id: "mate", label: "Mate", emoji: "🐾", color: "#14A37F", category: "gait" },
    { id: "alive", label: "Alive", emoji: "💓", color: "#14A37F", category: "gait" },
    { id: "charshow", label: "Show Off", emoji: "🎭", color: "#AD5FBF", category: "gait" },
    { id: "wakeup", label: "Wake Up", emoji: "😴", color: "#E08A2E", category: "gait" },
    { id: "look", label: "Look Around", emoji: "👀", color: "#E08A2E", category: "gait" },
    { id: "bow", label: "Play Bow", emoji: "🎉", color: "#E08A2E", category: "gait" },
    { id: "trot", label: "Happy Trot", emoji: "🐾", color: "#E08A2E", category: "gait" },
    { id: "chase", label: "Chase Tail", emoji: "🌀", color: "#E08A2E", category: "gait" },
    { id: "dig", label: "Dig", emoji: "⛏️", color: "#E08A2E", category: "gait" },
    { id: "scratch", label: "Scratch", emoji: "🐕", color: "#E08A2E", category: "gait" },
    { id: "shake", label: "Shake Off", emoji: "💦", color: "#E08A2E", category: "gait" },
    { id: "cower", label: "Scared", emoji: "😨", color: "#E08A2E", category: "gait" },
    { id: "paw", label: "Give Paw", emoji: "🤝", color: "#E08A2E", category: "gait" },
    { id: "tripod", label: "Three Legs", emoji: "🦮", color: "#E08A2E", category: "gait" },
    { id: "situp", label: "Sit Up + Clap", emoji: "🙌", color: "#C44569", category: "gait" },
    { id: "rearclap", label: "Rear + Clap", emoji: "👏", color: "#C44569", category: "gait" },
    { id: "surrender", label: "Hands Up", emoji: "🙆", color: "#C44569", category: "gait" },
    { id: "sixseven", label: "6 7", emoji: "🤚", color: "#C44569", category: "gait" },
    { id: "pushup", label: "Push-ups", emoji: "💪", color: "#C44569", category: "gait" },
    { id: "runningman", label: "Running Man", emoji: "🏃", color: "#C44569", category: "gait" },
    { id: "moonwalk", label: "Moonwalk", emoji: "🌙", color: "#C44569", category: "gait" },
    { id: "spin90", label: "Spin 90°", emoji: "🔃", color: "#D97A29", category: "gait" },
    { id: "longjump", label: "Long Jump", emoji: "🦘", color: "#D97A29", category: "gait" },
    { id: "farjump", label: "Far Jump", emoji: "🚀", color: "#D97A29", category: "gait" },
    { id: "backflip", label: "Backflip", emoji: "🤸", color: "#D14949", category: "gait" },
  ];
  const DEFAULT_PINNED = ["paw", "situp", "pushup", "stretch", "bow", "shake", "moonwalk"];

  // ── Saved in the browser ────────────────────────────────────
  const KEY_ASSIGN = "mpx_action_assignments";
  const KEY_CUSTOM = "mpx_custom_actions";
  const KEY_PINNED = "mpx_pinned_actions";
  let actionAssignments = $state({});
  let customActions = $state([]);
  let pinnedActions = $state([]);
  let loaded = false;

  $effect(() => {
    try { const s = localStorage.getItem(KEY_ASSIGN); if (s) actionAssignments = JSON.parse(s); } catch {}
    try { const s = localStorage.getItem(KEY_CUSTOM); if (s) customActions = JSON.parse(s); } catch {}
    try {
      const s = localStorage.getItem(KEY_PINNED);
      pinnedActions = s ? JSON.parse(s) : [...DEFAULT_PINNED];
    } catch { pinnedActions = [...DEFAULT_PINNED]; }
    loaded = true;
  });
  $effect(() => {
    const a = JSON.stringify(actionAssignments);
    const c = JSON.stringify(customActions);
    const p = JSON.stringify(pinnedActions);
    if (!loaded) return;
    try { localStorage.setItem(KEY_ASSIGN, a); } catch {}
    try { localStorage.setItem(KEY_CUSTOM, c); } catch {}
    try { localStorage.setItem(KEY_PINNED, p); } catch {}
  });

  const allActions = $derived([...actions, ...customActions.map((c) => ({ ...c, category: c.category || "custom" }))]);
  const pinnedObjects = $derived(pinnedActions.map((id) => allActions.find((a) => a.id === id)).filter(Boolean));

  function togglePin(id) {
    pinnedActions = pinnedActions.includes(id) ? pinnedActions.filter((x) => x !== id) : [...pinnedActions, id];
  }

  let flash = $state(null);
  async function sendGait(mode) {
    flash = mode;
    setTimeout(() => { if (flash === mode) flash = null; }, 700);
    try {
      await fetch("/v1/robot/gait", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ mode }),
      });
    } catch { /* ignore */ }
  }

  // ── Landscape ───────────────────────────────────────────────
  // The landscape button goes full screen and asks for landscape; turning the
  // phone sideways on Home does the same on its own. Where the browser can't
  // lock the orientation (iOS), the pad is drawn turned on a portrait screen.
  let wideMode = $state(false);
  let phoneSideways = $state(false);
  let viewportPortrait = $state(true);

  $effect(() => {
    const mqSide = matchMedia("(orientation: landscape) and (max-height: 540px)");
    const mqPort = matchMedia("(orientation: portrait)");
    const sync = () => { phoneSideways = mqSide.matches; viewportPortrait = mqPort.matches; };
    sync();
    mqSide.addEventListener("change", sync);
    mqPort.addEventListener("change", sync);
    return () => { mqSide.removeEventListener("change", sync); mqPort.removeEventListener("change", sync); };
  });

  // PC: a big enough window that isn't a phone held sideways gets the
  // landscape pad inline on Home -- no full screen, no rotate button.
  let desktop = $state(false);
  $effect(() => {
    const mq = matchMedia("(min-width: 1000px) and (min-height: 600px)");
    const sync = () => (desktop = mq.matches);
    sync();
    mq.addEventListener("change", sync);
    return () => mq.removeEventListener("change", sync);
  });

  const showWide = $derived(!desktop && activeTab === "home" && !showAllActions && !showAddAction && !showActionAssignment && (wideMode || phoneSideways));
  const rotated = $derived(showWide && viewportPortrait);

  async function enterWide() {
    wideMode = true;
    try { if (!document.fullscreenElement) await document.documentElement.requestFullscreen(); } catch {}
    try { await screen.orientation?.lock?.("landscape"); } catch {}
  }
  async function exitWide() {
    wideMode = false;
    try { screen.orientation?.unlock?.(); } catch {}
    try { if (document.fullscreenElement) await document.exitFullscreen(); } catch {}
  }
  $effect(() => {
    const onFs = () => { if (!document.fullscreenElement && wideMode && !phoneSideways) wideMode = false; };
    document.addEventListener("fullscreenchange", onFs);
    return () => document.removeEventListener("fullscreenchange", onFs);
  });

  // ── Wi-Fi pill: tap, or pull down on it ────────────────────
  let pillStart = null;
  function pillDown(e) { pillStart = e.clientY; }
  function pillMove(e) {
    if (pillStart !== null && e.clientY - pillStart > 24) { pillStart = null; showWifi = true; }
  }
  function pillUp() { pillStart = null; }

  const tabs = [
    { id: "home", label: "Home" },
    { id: "skills", label: "Skills" },
    { id: "store", label: "Store" },
    { id: "chat", label: "Chat" },
    { id: "settings", label: "Settings" },
  ];
  function selectTab(id) {
    activeTab = id;
    showAllActions = false;
    showAddAction = false;
    showActionAssignment = false;
  }
</script>

{#snippet tabIcon(id, on)}
  {#if id === "home"}
    <span class="eyes-ico" aria-hidden="true"><i></i><i></i></span>
  {:else if id === "skills"}
    <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.1" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M13 2L4 14h7l-1 8 9-12h-7z"/></svg>
  {:else if id === "store"}
    <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.1" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M4 9h16l-1 11H5z"/><path d="M9 9V6a3 3 0 0 1 6 0v3"/></svg>
  {:else if id === "chat"}
    <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.1" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M4 5h16v11H9l-5 4z"/></svg>
  {:else}
    <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.1" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><circle cx="12" cy="12" r="3"/><path d="M12 2v3M12 19v3M4.2 4.2l2.1 2.1M17.7 17.7l2.1 2.1M2 12h3M19 12h3M4.2 19.8l2.1-2.1M17.7 6.3l2.1-2.1"/></svg>
  {/if}
{/snippet}

<div class="shell-root">
  <main class="content">
    {#if activeTab === "home"}
      {#if showAddAction}
        <AddActionView onSave={(a) => (customActions = [...customActions, a])} onBack={() => (showAddAction = false)} />
      {:else if showActionAssignment}
        <ActionAssignment
          assignments={actionAssignments}
          {customActions}
          onAddAction={() => (showAddAction = true)}
          onDeleteAction={(id) => { customActions = customActions.filter((c) => c.id !== id); pinnedActions = pinnedActions.filter((x) => x !== id); }}
          onSave={(updated) => (actionAssignments = updated)}
          onBack={() => (showActionAssignment = false)}
        />
      {:else if showAllActions}
        <AllActions
          actions={actions.filter((a) => a.id !== "none")}
          custom={customActions}
          pinned={pinnedActions}
          onRun={sendGait}
          onTogglePin={togglePin}
          onDelete={(id) => { customActions = customActions.filter((c) => c.id !== id); pinnedActions = pinnedActions.filter((x) => x !== id); }}
          onAdd={() => { showAllActions = false; showAddAction = true; }}
          onBack={() => (showAllActions = false)}
        />
      {:else}
        <div class="home" class:desk={desktop}>
          <header class="home-head">
            <button
              type="button"
              class="wifi-pill"
              onclick={() => (showWifi = true)}
              onpointerdown={pillDown}
              onpointermove={pillMove}
              onpointerup={pillUp}
              onpointercancel={pillUp}
              aria-haspopup="dialog"
              aria-label="Robot Wi‑Fi: {staConnected ? network?.staSsid : 'not connected'}. Pull down to pick a network"
            >
              <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" aria-hidden="true"><path d="M2 8.5a15 15 0 0 1 20 0M5 12a10 10 0 0 1 14 0M8.5 15.5a5 5 0 0 1 7 0"/><circle cx="12" cy="19" r="1.2" fill="currentColor"/></svg>
              <span class="wifi-name">{staConnected ? network?.staSsid : "Wi‑Fi"}</span>
              <svg class="caret" width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M6 9l6 6 6-6"/></svg>
            </button>
            <img class="logo" src="/md.svg" alt="MangDang" />
            {#if desktop}<span></span>{:else}
            <button type="button" class="round" onclick={enterWide} aria-label="Landscape controller">
              <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><rect x="3" y="7" width="18" height="10" rx="3"/><path d="M8 10v4M6 12h4M16 11h.01M18 13h.01"/></svg>
            </button>
            {/if}
          </header>

          {#if network?.linkState === "offline"}
            <p class="offline" role="status">Can't reach the robot. The buttons work again as soon as it's back.</p>
          {/if}

          <div class="home-scroll">
            <div class="pad-wrap">
              <Controller
                wide={desktop}
                assignments={actionAssignments}
                catalog={allActions}
                {online}
                onEdit={() => (showActionAssignment = true)}
              />
            </div>

            <section class="quick" aria-labelledby="quick-title">
              <div class="quick-head">
                <h2 id="quick-title">Quick actions</h2>
                <button type="button" class="see-all" onclick={() => (showAllActions = true)}>See all</button>
              </div>
              <ul class="quick-grid">
                {#each pinnedObjects as a (a.id)}
                  <li>
                    <button type="button" class="qa" class:hit={flash === a.id} onclick={() => sendGait(a.id)} aria-label="Run {a.label}">
                      <span class="qa-ico"><ActionIcon id={a.id} emoji={a.emoji} /></span>
                      <span class="qa-lbl">{a.label}</span>
                    </button>
                  </li>
                {/each}
                <li>
                  <button type="button" class="qa add" onclick={() => (showAllActions = true)} aria-label="Pin more moves">
                    <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" aria-hidden="true"><path d="M12 5v14M5 12h14"/></svg>
                    <span class="qa-lbl">Pin</span>
                  </button>
                </li>
              </ul>
            </section>
          </div>
        </div>
      {/if}
    {:else if activeTab === "skills"}
      <SkillsManagement embedded onNavigate={(to) => { if (to === "store") activeTab = "store"; }} />
    {:else if activeTab === "store"}
      <SkillsMarketplace embedded onNavigate={(to) => { if (to === "skills") activeTab = "skills"; }} />
    {:else if activeTab === "chat"}
      <div class="chat-tab">
        <ChatView navigate={(to) => { if (to === "home") activeTab = "home"; }} {network} />
      </div>
    {:else if activeTab === "settings"}
      <SettingsPage {network} />
    {/if}
  </main>

  <nav class="tabs" aria-label="Main">
    {#each tabs as t (t.id)}
      {@const on = activeTab === t.id}
      <button type="button" class="tab" class:on aria-current={on ? "page" : undefined} onclick={() => selectTab(t.id)}>
        <span class="tab-ico">{@render tabIcon(t.id, on)}</span>
        <span>{t.label}</span>
      </button>
    {/each}
  </nav>

  {#if showWide}
    <div class="wide-stage" class:rotated>
      <div class="wide-inner">
        <Controller
          wide
          {rotated}
          assignments={actionAssignments}
          catalog={allActions}
          {online}
          onEdit={() => { exitWide(); showActionAssignment = true; }}
          onExitWide={exitWide}
        />
      </div>
    </div>
  {/if}

  {#if showWifi}
    <WifiSheet {network} onClose={() => (showWifi = false)} />
  {/if}
</div>

<style>
  .shell-root {
    position: absolute;
    inset: 0;
    display: flex;
    flex-direction: column;
    background: var(--page);
    color: var(--ink);
  }
  .content { flex: 1; min-height: 0; position: relative; display: flex; flex-direction: column; overflow: hidden; }

  /* ── Home ── */
  .home { position: absolute; inset: 0; display: flex; flex-direction: column; }
  .home-head {
    flex-shrink: 0;
    height: calc(60px + var(--safe-top));
    padding: calc(8px + var(--safe-top)) 16px 0;
    display: grid;
    grid-template-columns: 1fr auto 1fr;
    align-items: center;
    gap: 8px;
    max-width: var(--page-max);
    width: 100%;
    margin: 0 auto;
  }
  .wifi-pill {
    justify-self: start;
    max-width: 100%;
    height: 40px;
    border-radius: 20px;
    border: none;
    background: var(--ground);
    display: flex; align-items: center; gap: 8px;
    padding: 0 12px 0 10px;
    font-size: 14px; font-weight: 600; color: var(--ink);
    cursor: pointer;
    touch-action: pan-x;
  }
  .wifi-name { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; min-width: 0; }
  .caret { color: var(--sub); flex-shrink: 0; }
  .logo { height: 16px; width: auto; display: block; }
  .round {
    justify-self: end;
    width: 40px; height: 40px; border-radius: 20px; border: none; background: var(--ground); color: var(--ink);
    display: grid; place-items: center; cursor: pointer;
  }
  .wifi-pill:focus-visible, .round:focus-visible, .see-all:focus-visible, .qa:focus-visible, .tab:focus-visible { outline: 3px solid var(--ink); outline-offset: 2px; }
  .offline { margin: 0 auto; max-width: var(--page-max); padding: 0 18px 6px; font-size: 13px; color: #9B2C20; text-align: center; }

  .home-scroll { flex: 1; min-height: 0; overflow-y: auto; display: flex; flex-direction: column; }
  .pad-wrap { flex-shrink: 0; padding: 8px 14px 0; width: 100%; max-width: 460px; margin: 0 auto; }

  .quick {
    flex: 1;
    margin: 12px 14px 12px;
    width: calc(100% - 28px);
    max-width: 432px;
    align-self: center;
    background: #fff;
    border: 1px solid var(--line);
    border-radius: 26px;
    padding: 12px 14px 14px;
    display: flex; flex-direction: column; gap: 10px;
    min-height: 120px;
  }
  .quick-head { display: flex; align-items: center; justify-content: space-between; }
  .quick-head h2 { margin-left: 4px; font-size: 16px; font-weight: 800; }
  .see-all { height: 32px; border-radius: 16px; border: none; background: var(--ground); padding: 0 12px; font-size: 13px; font-weight: 700; cursor: pointer; color: var(--ink); }
  .quick-grid { list-style: none; display: grid; grid-template-columns: repeat(4, minmax(0, 1fr)); gap: 8px; }
  .qa {
    width: 100%; height: 62px; border-radius: 16px; border: none; background: var(--ground); color: var(--ink);
    display: flex; flex-direction: column; align-items: center; justify-content: center; gap: 4px;
    cursor: pointer; transition: background var(--dur-fast), transform var(--dur-fast);
  }
  .qa:active { transform: scale(0.97); }
  .qa.hit { background: var(--mpx-yellow); }
  .qa.add { background: #fff; border: 1.5px dashed #CFC9BB; }
  .qa-ico { font-size: 20px; line-height: 1; }
  .qa-lbl { font-size: 12px; font-weight: 700; max-width: 100%; padding: 0 4px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }

  /* ── PC: the landscape pad inline, quick actions in one row under it ── */
  .home.desk .home-head, .home.desk .offline { max-width: 1080px; padding-left: 24px; padding-right: 24px; }
  .home.desk .home-scroll { align-items: center; padding-bottom: 16px; }
  .home.desk .pad-wrap { max-width: 1080px; padding: 12px 24px 0; height: clamp(420px, 58vh, 560px); }
  .home.desk .quick { flex: 0 0 auto; max-width: 1032px; margin-top: 18px; }
  .home.desk .quick-grid { grid-template-columns: repeat(auto-fill, minmax(112px, 1fr)); }
  .home.desk .qa { height: 72px; }

  /* ── Chat ── */
  .chat-tab { position: absolute; inset: 0; display: flex; flex-direction: column; }

  /* ── Tab bar ── */
  .tabs {
    flex-shrink: 0;
    display: grid;
    grid-template-columns: repeat(5, minmax(0, 1fr));
    background: #fff;
    border-top: 1px solid var(--hair);
    padding: 6px 4px calc(8px + var(--safe-bottom));
  }
  .tab {
    border: none; background: transparent; color: var(--sub);
    display: flex; flex-direction: column; align-items: center; gap: 3px;
    font-size: 11px; font-weight: 600; padding: 4px 0; cursor: pointer;
    min-height: 54px;
  }
  .tab-ico { width: 48px; height: 30px; border-radius: 15px; display: grid; place-items: center; transition: background var(--dur-fast); }
  .tab.on { color: var(--ink); font-weight: 700; }
  .tab.on .tab-ico { background: var(--mpx-yellow); }
  .eyes-ico { display: flex; gap: 4px; }
  .eyes-ico i { width: 6px; height: 12px; border-radius: 2px; background: currentColor; }

  /* ── Landscape stage ── */
  .wide-stage {
    position: fixed;
    inset: 0;
    z-index: 40;
    background: var(--page);
    display: grid;
    place-items: center;
    padding: calc(10px + var(--safe-top)) calc(12px + env(safe-area-inset-right, 0px)) calc(10px + var(--safe-bottom)) calc(12px + env(safe-area-inset-left, 0px));
  }
  .wide-inner { width: 100%; height: 100%; max-width: 980px; }
  /* Portrait screen that can't be locked sideways: draw the pad turned. */
  .wide-stage.rotated {
    inset: auto;
    top: 0;
    left: 100vw;
    width: 100dvh;
    height: 100vw;
    transform-origin: top left;
    transform: rotate(90deg);
  }
</style>
