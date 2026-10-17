<script>
  import {
    listSkills,
    listRobotSkills,
    toggleSkill,
    deploySkill,
    enqueueLua,
  } from "./marketplaceApi.js";

  /**
   * Skills: what is on the robot, and what you own but haven't put on it.
   *
   * AI skills (marketplace type "awa") run in the agent and are switched on or
   * off. Motion skills ("wasm") are files installed into the robot's storage.
   * Skill files already on the robot (/v1/skills/registry) show under On robot
   * even when the store can't be reached.
   *
   * @prop onNavigate — ("store") => void
   */
  let { onNavigate } = $props();

  let tab = $state("robot");            // "robot" | "library"
  let owned = $state([]);               // /v1/marketplace/robot/skills
  let catalog = $state({});             // skill id -> { description }
  let registry = $state([]);            // /v1/skills/registry skills
  let running = $state(null);           // name of the skill running now
  let loading = $state(true);
  let storeError = $state(false);
  let busy = $state(null);
  let toast = $state(null);
  let toastTimer = null;

  function say(text, tone = "ok") {
    toast = { text, tone };
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => (toast = null), 2600);
  }

  // Motion skills installed from the store: file paths written on install,
  // so Remove can delete them again.
  const DEPLOYED_KEY = "mpx_deployed_skills";
  let deployed = $state({});
  try { const raw = localStorage.getItem(DEPLOYED_KEY); if (raw) deployed = JSON.parse(raw); } catch {}
  function saveDeployed() { try { localStorage.setItem(DEPLOYED_KEY, JSON.stringify(deployed)); } catch {} }

  async function refresh({ quiet = false } = {}) {
    if (!quiet) loading = true;
    const [ownedRes, catRes, regRes, statRes] = await Promise.allSettled([
      listRobotSkills(),
      listSkills(),
      fetch("/v1/skills/registry", { cache: "no-store" }).then((r) => (r.ok ? r.json() : null)),
      fetch("/v1/skills/status", { cache: "no-store" }).then((r) => (r.ok ? r.json() : null)),
    ]);
    storeError = ownedRes.status !== "fulfilled";
    owned = ownedRes.status === "fulfilled" && Array.isArray(ownedRes.value) ? ownedRes.value : [];
    if (catRes.status === "fulfilled" && Array.isArray(catRes.value)) {
      catalog = Object.fromEntries(catRes.value.map((s) => [s.id, s]));
    }
    registry = regRes.status === "fulfilled" && regRes.value?.skills ? regRes.value.skills : [];
    running = statRes.status === "fulfilled" && statRes.value?.running ? statRes.value.name || "" : null;
    loading = false;
  }
  $effect(() => { refresh(); return () => clearTimeout(toastTimer); });

  const isMotion = (s) => (s.skill_type || "").toLowerCase() === "wasm";
  const tail = (id) => (id || "").split("~").pop();
  const pretty = (slug) => (slug || "").replace(/\.(wasm|mpxe)$/, "").replace(/[_-]+/g, " ").replace(/^\w/, (c) => c.toUpperCase());

  function registryFor(skillId) {
    const t = tail(skillId);
    return registry.find((r) => r.slug === t || (r.file || "").replace(/^\//, "").startsWith(t));
  }

  const items = $derived.by(() => {
    const list = owned.map((s) => {
      const motion = isMotion(s);
      const reg = motion ? registryFor(s.skill_id) : null;
      return {
        key: s.skill_id,
        name: s.title || pretty(tail(s.skill_id)),
        desc: catalog[s.skill_id]?.description || "",
        motion,
        onRobot: motion ? s.skill_id in deployed || !!reg : !!s.enabled,
        runName: reg ? reg.slug || reg.file.replace(/^\//, "") : null,
        src: s,
      };
    });
    // Files on the robot that no owned skill accounts for.
    const claimed = new Set(list.map((i) => i.runName).filter(Boolean));
    for (const r of registry) {
      const name = r.slug || (r.file || "").replace(/^\//, "");
      if (!name || claimed.has(name)) continue;
      list.push({ key: "local:" + name, name: pretty(name), desc: "Installed on the robot", motion: true, onRobot: true, runName: name, src: null });
    }
    return list;
  });
  const onRobot = $derived(items.filter((i) => i.onRobot));
  const library = $derived(items.filter((i) => !i.onRobot));
  const shown = $derived(tab === "robot" ? onRobot : library);

  // ── Actions ──
  async function setEnabled(item, on) {
    busy = item.key;
    try {
      await toggleSkill(item.key, on);
      await refresh({ quiet: true });
      say(`${item.name} ${on ? "is on" : "is off"}`);
    } catch { say("Couldn't change that skill", "err"); }
    busy = null;
  }

  async function install(item) {
    busy = item.key;
    try {
      const data = await deploySkill(item.key);
      const scripts = [];
      for (const s of data.skills || []) for (const cmd of s.commands || []) { scripts.push(cmd.script); await enqueueLua(cmd.script); }
      const paths = [];
      for (const sc of scripts) for (const m of sc.matchAll(/file_write\s*\(\s*["']([^"']+)["']/g)) paths.push(m[1]);
      deployed = { ...deployed, [item.key]: paths };
      saveDeployed();
      await refresh({ quiet: true });
      say(`${item.name} is on the robot`);
    } catch { say("Install failed", "err"); }
    busy = null;
  }

  async function remove(item) {
    busy = item.key;
    try {
      for (const path of deployed[item.key] || []) {
        await fetch("/v1/fs/delete", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ path }) });
      }
      const next = { ...deployed }; delete next[item.key]; deployed = next; saveDeployed();
      await refresh({ quiet: true });
      say(`${item.name} removed from the robot`);
    } catch { say("Couldn't remove it", "err"); }
    busy = null;
  }

  async function run(item) {
    busy = item.key;
    try {
      if (running && running === item.runName) {
        await fetch("/v1/skills/stop", { method: "POST" });
        running = null;
      } else {
        const r = await fetch("/v1/skills/run", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ skill: item.runName }) });
        if (r.status === 409) say("Another skill is running. Stop it first.", "err");
        else running = item.runName;
      }
    } catch { say("Couldn't reach the robot", "err"); }
    busy = null;
  }
</script>

<div class="skills">
  <header class="head">
    <h1>Skills</h1>
    <div class="seg" role="tablist" aria-label="Where">
      <button type="button" role="tab" aria-selected={tab === "robot"} class:on={tab === "robot"} onclick={() => (tab = "robot")}>
        On robot{loading ? "" : ` · ${onRobot.length}`}
      </button>
      <button type="button" role="tab" aria-selected={tab === "library"} class:on={tab === "library"} onclick={() => (tab = "library")}>
        Library{loading ? "" : ` · ${library.length}`}
      </button>
    </div>
  </header>

  <div class="scroll">
    <div class="inner">
      {#if storeError && !loading}
        <p class="note">Couldn't reach the Store, so only skills already on the robot are shown.</p>
      {/if}

      {#if loading}
        {#each [0, 1, 2] as _}<div class="card sk" aria-hidden="true"><span class="sk-ico"></span><span class="sk-line"></span></div>{/each}
      {:else if shown.length === 0}
        <div class="empty">
          <p class="empty-title">{tab === "robot" ? "Nothing on the robot yet" : "Your library is empty"}</p>
          <p class="empty-desc">
            {tab === "robot"
              ? "Turn on an AI skill or install a motion skill from your Library."
              : "Skills you get in the Store land here, ready to put on the robot."}
          </p>
          {#if tab === "robot" && library.length}
            <button type="button" class="ghost" onclick={() => (tab = "library")}>Open Library</button>
          {:else}
            <button type="button" class="ghost" onclick={() => onNavigate?.("store")}>Go to the Store</button>
          {/if}
        </div>
      {:else}
        <ul class="list">
          {#each shown as item (item.key)}
            {@const isRunning = running && running === item.runName}
            <li class="card" class:busy={busy === item.key}>
              <span class="ico" class:motion={item.motion} aria-hidden="true">
                {#if item.motion}
                  <svg width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M4 18c3-8 5-12 8-12s5 4 8 12"/><circle cx="12" cy="6" r="2"/></svg>
                {:else}
                  <svg width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M12 3l1.8 4.6L18.5 9l-4.7 1.4L12 15l-1.8-4.6L5.5 9l4.7-1.4z"/><path d="M18 15l.9 2.1L21 18l-2.1.9L18 21l-.9-2.1L15 18l2.1-.9z"/></svg>
                {/if}
              </span>
              <div class="info">
                <span class="name">{item.name}</span>
                {#if item.desc}<span class="desc">{item.desc}</span>{/if}
                <span class="chip" class:motion={item.motion}>{item.motion ? "Motion skill" : "AI skill"}</span>
              </div>
              <div class="acts">
                {#if tab === "robot"}
                  {#if item.motion}
                    {#if item.runName}
                      <button type="button" class="btn" class:dark={isRunning} disabled={busy === item.key} onclick={() => run(item)} aria-label="{isRunning ? 'Stop' : 'Run'} {item.name}">{isRunning ? "Stop" : "Run"}</button>
                    {/if}
                    {#if item.src && item.key in deployed}
                      <button type="button" class="link" disabled={busy === item.key} onclick={() => remove(item)} aria-label="Remove {item.name} from the robot">Remove</button>
                    {/if}
                  {:else}
                    <button type="button" class="btn dark" disabled={busy === item.key} onclick={() => setEnabled(item, false)} aria-label="Turn off {item.name}">Turn off</button>
                  {/if}
                {:else}
                  <button type="button" class="btn" disabled={busy === item.key} onclick={() => (item.motion ? install(item) : setEnabled(item, true))} aria-label="{item.motion ? 'Install' : 'Turn on'} {item.name}">
                    {busy === item.key ? "…" : item.motion ? "Install" : "Turn on"}
                  </button>
                {/if}
              </div>
            </li>
          {/each}
        </ul>
      {/if}
    </div>
  </div>

  {#if toast}
    <div class="toast" class:err={toast.tone === "err"} role="status">{toast.text}</div>
  {/if}
</div>

<style>
  .skills { position: absolute; inset: 0; display: flex; flex-direction: column; background: var(--page); color: var(--ink); }
  .head { flex-shrink: 0; width: 100%; max-width: var(--page-max); margin: 0 auto; padding: calc(26px + var(--safe-top)) 20px 8px; display: flex; flex-direction: column; gap: 16px; }
  h1 { font-size: 34px; font-weight: 800; letter-spacing: -0.02em; }
  .seg { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); background: var(--hair); border-radius: 16px; padding: 4px; gap: 4px; }
  .seg button { height: 44px; border: none; border-radius: 12px; background: transparent; color: #4F4C46; font-size: 15px; font-weight: 700; cursor: pointer; }
  .seg button.on { background: #fff; color: var(--ink); box-shadow: 0 1px 3px rgba(0, 0, 0, 0.12); }
  .seg button:focus-visible { outline: 3px solid var(--ink); outline-offset: 1px; }

  .scroll { flex: 1; min-height: 0; overflow-y: auto; }
  .inner { max-width: var(--page-max); margin: 0 auto; padding: 8px 20px 20px; display: flex; flex-direction: column; gap: 10px; }
  .note { font-size: 13px; color: #4F4C46; background: var(--ground); border-radius: 14px; padding: 10px 12px; }

  .list { list-style: none; display: flex; flex-direction: column; gap: 10px; }
  .card { background: #fff; border: 1px solid var(--line); border-radius: 20px; padding: 14px; display: flex; align-items: center; gap: 14px; transition: opacity var(--dur-fast); }
  .card.busy { opacity: 0.6; }
  .ico { width: 52px; height: 52px; border-radius: 15px; flex-shrink: 0; display: grid; place-items: center; background: var(--mpx-yellow); color: var(--ink); }
  .ico.motion { background: #CFE3FF; }
  .info { flex: 1; min-width: 0; display: flex; flex-direction: column; gap: 3px; }
  .name { font-size: 16px; font-weight: 700; }
  .desc { font-size: 13px; color: var(--sub); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  .chip { align-self: flex-start; margin-top: 2px; font-size: 11px; font-weight: 700; padding: 3px 8px; border-radius: 8px; background: #FFF6B0; color: #5B4E00; }
  .chip.motion { background: #E3EEFF; color: #1F4F99; }
  .acts { display: flex; flex-direction: column; align-items: flex-end; gap: 2px; flex-shrink: 0; }
  .btn { height: 40px; min-width: 84px; padding: 0 14px; border-radius: 20px; border: none; background: var(--ground); color: var(--ink); font-size: 14px; font-weight: 800; cursor: pointer; }
  .btn.dark { background: var(--ink); color: var(--mpx-yellow); }
  .btn:disabled, .link:disabled { opacity: 0.6; cursor: default; }
  .link { height: 32px; border: none; background: transparent; color: #9B2C20; font-size: 13px; font-weight: 700; cursor: pointer; padding: 0 6px; }
  button:focus-visible { outline: 3px solid var(--ink); outline-offset: 2px; }

  .empty { background: var(--ground); border-radius: 20px; padding: 28px 20px; display: flex; flex-direction: column; align-items: center; gap: 8px; text-align: center; }
  .empty-title { font-size: 17px; font-weight: 800; }
  .empty-desc { font-size: 14px; line-height: 1.45; color: var(--sub); max-width: 300px; }
  .ghost { margin-top: 6px; height: 44px; padding: 0 18px; border-radius: 22px; border: none; background: var(--ink); color: #fff; font-size: 15px; font-weight: 700; cursor: pointer; }

  .sk { height: 80px; }
  .sk-ico { width: 52px; height: 52px; border-radius: 15px; background: var(--hair); }
  .sk-line { height: 14px; width: 50%; border-radius: 7px; background: var(--hair); }

  .toast {
    position: absolute; left: 50%; bottom: 16px; transform: translateX(-50%);
    background: var(--ink); color: #fff; padding: 10px 16px; border-radius: 14px;
    font-size: 14px; font-weight: 600; box-shadow: var(--shadow-float); max-width: calc(100% - 40px);
  }
  .toast.err { background: #9B2C20; }
</style>
