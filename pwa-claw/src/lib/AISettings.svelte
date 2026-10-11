<script>
  /**
   * AI & Voice — where the robot's AI comes from (firmware: GET/POST /v1/ai).
   *
   *   mangdang  MangDang's cloud AI for chat and voice. Nothing to set up.
   *   own       The owner's own LLM account. The key is stored on the robot
   *             and used from there; for voice, MangDang's server only turns
   *             speech into text and text into speech.
   *   off       No AI: no chat, no voice, the microphone never opens.
   *
   * Two firmwares answer this page:
   *
   *   mpv2-claw   offers all three modes and restarts after saving. Its GET
   *               reply has no "modes" key.
   *   mpx-dog     (local-claw) offers only what it has -- "modes" lists them
   *               ("own" once the on-device agent is in the build) -- and
   *               applies a change live, so POST answers {"restarting":false}
   *               and we simply reload. Its GET also carries "agent"
   *               {available, running}; in "own" mode only the chat page's
   *               "Pupper" brain uses the key -- voice turns are still
   *               answered by MangDang's server for now, which the mode card
   *               says when the robot reports an agent. It also has GET/POST
   *               /v1/voice (status, volume, hands-free, music), which this
   *               page shows when the robot has it.
   *
   * The robot never sends the key back — only whether one is saved and its
   * last four characters. Leaving the key field empty keeps the saved one.
   */
  let { onBack } = $props();

  // Provider presets — the same values ESP-Claw's own settings page uses.
  const PRESETS = [
    { id: "anthropic", label: "Anthropic (Claude)", backend: "anthropic_compatible",
      base_url: "https://api.anthropic.com/v1", model: "claude-sonnet-4-6",
      auth_type: "none", max_tokens_field: "max_tokens" },
    { id: "openai", label: "OpenAI", backend: "openai_compatible",
      base_url: "https://api.openai.com/v1", model: "gpt-5.4",
      auth_type: "bearer", max_tokens_field: "max_completion_tokens" },
    { id: "deepseek", label: "DeepSeek", backend: "openai_compatible",
      base_url: "https://api.deepseek.com", model: "deepseek-v4-pro",
      auth_type: "bearer", max_tokens_field: "max_completion_tokens" },
    { id: "custom_openai", label: "Other (OpenAI-compatible)", backend: "openai_compatible",
      base_url: "", model: "", auth_type: "bearer", max_tokens_field: "max_tokens" },
    { id: "custom_anthropic", label: "Other (Anthropic-compatible)", backend: "anthropic_compatible",
      base_url: "", model: "", auth_type: "none", max_tokens_field: "max_tokens" },
  ];

  const ALL_MODES = [
    { id: "mangdang", title: "MangDang AI", tag: "Default",
      desc: "Ready to go. Chat and voice use MangDang's cloud AI. Nothing to set up." },
    { id: "own", title: "My own AI account", tag: "Your key",
      desc: "Chat and voice answers come from your own API key. The key stays on the robot — MangDang only turns speech into text and back." },
    { id: "off", title: "AI off", tag: "Private",
      desc: "No chat, no voice, and the microphone never turns on. Walking, controls and the Skill Store still work." },
  ];

  // Music genres cap_music knows (SomaFM); anything else is sent as typed
  // (a stream URL also works).
  const GENRES = ["chill", "jazz", "lounge", "ambient", "electronic", "pop", "indie", "rock", "folk", "soul", "reggae", "80s", "70s"];

  let loading = $state(true);
  let loadError = $state("");
  let offered = $state(null);              // GET /v1/ai "modes", or null when the firmware offers all three
  let mode = $state("mangdang");           // what is selected on screen
  let savedMode = $state("mangdang");      // what the robot has stored
  let runningMode = $state("mangdang");    // what this boot is running
  let livesApply = $state(false);          // true once a POST answered restarting:false
  let presetId = $state("anthropic");
  let baseUrl = $state("");
  let model = $state("");
  let apiKey = $state("");
  let keySet = $state(false);
  let keyHint = $state("");
  let showKey = $state(false);
  let saving = $state(false);
  let error = $state("");
  let warning = $state("");
  let restarting = $state(false);

  // /v1/voice (mpx-dog only). null = the robot does not have it.
  let voice = $state(null);
  let voiceBusy = $state(false);
  let voiceError = $state("");
  let musicPick = $state("jazz");
  let musicCustom = $state("");
  let volumeDraft = $state(null);          // slider value while dragging
  let voiceTimer = null;

  // mpx-dog's "own" is narrower than mpv2-claw's for now: the key serves the
  // chat page's "Pupper" brain, voice still goes through MangDang.
  const OWN_DESC_MPX =
    "Chat with Pupper (the robot's own agent) uses your API key, which stays on the robot. Voice is still answered by MangDang's AI for now.";
  let agentInfo = $state(null);            // GET /v1/ai "agent" (mpx-dog only)
  let modes = $derived.by(() => {
    const list = offered ? ALL_MODES.filter((m) => offered.includes(m.id)) : ALL_MODES;
    if (!agentInfo) return list;
    return list.map((m) => (m.id === "own" ? { ...m, desc: OWN_DESC_MPX } : m));
  });
  let preset = $derived(PRESETS.find((p) => p.id === presetId) ?? PRESETS[0]);
  let canSave = $derived(
    !saving && !restarting &&
    (mode !== "own" || (baseUrl.trim().startsWith("http") && model.trim() && (apiKey.trim() || keySet)))
  );
  // Re-saving "own" is allowed (a new key or model); the other two only on a change.
  let changed = $derived(mode !== savedMode || mode === "own");
  let providerHost = $derived.by(() => {
    try { return new URL(baseUrl).host || "your provider"; } catch { return "your provider"; }
  });
  let volume = $derived(volumeDraft ?? voice?.volume ?? 0);
  let handsFreeOn = $derived(!!voice && voice.hands_free !== "off");
  let voiceLine = $derived.by(() => {
    if (!voice) return "";
    if (voice.pi_in_control) return "Paused while the Raspberry Pi is in control.";
    if (voice.talking) return "Talking to MangDang…";
    if (voice.playing_reply) return "Pupper is answering.";
    if (voice.hands_free === "listening") return "Listening for “Hi ESP”.";
    if (voice.hands_free === "off") return "Hands-free listening is off.";
    return `Hands-free: ${voice.hands_free}.`;
  });

  function presetFor(own) {
    const byUrl = PRESETS.find((p) => p.base_url && p.base_url === own.base_url);
    if (byUrl) return byUrl.id;
    return own.backend === "anthropic_compatible" ? "custom_anthropic" : "custom_openai";
  }

  async function load() {
    loading = true;
    loadError = "";
    try {
      const r = await fetch("/v1/ai", { cache: "no-store" });
      if (!r.ok) throw new Error("HTTP " + r.status);
      const d = await r.json();
      offered = Array.isArray(d.modes) && d.modes.length ? d.modes : null;
      agentInfo = d.agent && typeof d.agent === "object" ? d.agent : null;
      mode = savedMode = d.mode || "mangdang";
      runningMode = d.running_mode || savedMode;
      const own = d.own || {};
      keySet = !!own.key_set;
      keyHint = own.key_hint || "";
      if (own.base_url && own.backend) {
        presetId = presetFor(own);
        baseUrl = own.base_url;
        model = own.model || "";
      } else {
        choosePreset("anthropic");
      }
    } catch (e) {
      loadError = "Could not read the robot's AI settings. Is it online?";
    }
    loading = false;
    loadVoice();
  }

  // Voice status is optional: mpv2-claw has no /v1/voice, and mpx-dog built
  // without CONFIG_APP_VOICE_ENABLE answers 404. Either way the card stays hidden.
  async function loadVoice() {
    try {
      const r = await fetch("/v1/voice", { cache: "no-store" });
      if (!r.ok) { voice = null; return; }
      const d = await r.json();
      voice = d && d.available ? d : null;
    } catch {
      voice = null;
    }
  }

  async function postVoice(body) {
    voiceError = "";
    voiceBusy = true;
    try {
      const r = await fetch("/v1/voice", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(body),
      });
      const d = await r.json().catch(() => ({}));
      if (!r.ok) throw new Error(d.error || "HTTP " + r.status);
      if (d && d.available !== undefined) voice = d.available ? d : null;
    } catch (e) {
      voiceError = e.message || "The robot did not accept that.";
    }
    voiceBusy = false;
  }

  function commitVolume() {
    if (volumeDraft === null) return;
    const v = volumeDraft;
    volumeDraft = null;
    postVoice({ volume: v });
  }

  function playMusic() {
    const what = musicPick === "custom" ? musicCustom.trim() : musicPick;
    if (!what) return;
    postVoice({ music: what });
  }

  function choosePreset(id) {
    presetId = id;
    const p = PRESETS.find((x) => x.id === id);
    if (!p) return;
    baseUrl = p.base_url;
    model = p.model;
  }

  async function save() {
    error = "";
    warning = "";
    saving = true;
    const body = { mode };
    if (mode === "own") {
      Object.assign(body, {
        backend: preset.backend,
        base_url: baseUrl.trim(),
        model: model.trim(),
        auth_type: preset.auth_type,
        max_tokens_field: preset.max_tokens_field,
        api_key: apiKey.trim(),
      });
    }
    try {
      const r = await fetch("/v1/ai", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(body),
      });
      const d = await r.json().catch(() => ({}));
      if (!r.ok || !d.ok) throw new Error(d.error || "HTTP " + r.status);
      apiKey = "";
      if (d.restarting === false) {
        // mpx-dog: applied live. Reload the page's view of the robot.
        livesApply = true;
        warning = d.warning || "";
        await load();
      } else {
        restarting = true;
        waitForRobot();
      }
    } catch (e) {
      error = e.message || "Could not save.";
    }
    saving = false;
  }

  // The robot restarts after saving; wait for it to answer again.
  async function waitForRobot() {
    await new Promise((res) => setTimeout(res, 4000));
    for (let i = 0; i < 40; i++) {
      try {
        const r = await fetch("/v1/ai", { cache: "no-store" });
        if (r.ok) { restarting = false; await load(); return; }
      } catch { /* still restarting */ }
      await new Promise((res) => setTimeout(res, 1500));
    }
    restarting = false;
    error = "The robot has not come back yet. Refresh this page in a moment.";
  }

  $effect(() => {
    load();
    // Keep the voice card current while the page is open (link/hands-free
    // state changes on its own as the robot listens and talks).
    voiceTimer = setInterval(() => { if (voice && !voiceBusy) loadVoice(); }, 3000);
    return () => clearInterval(voiceTimer);
  });
</script>

<div class="page">
  <header class="bar">
    <button type="button" class="round" onclick={() => onBack?.()} aria-label="Back to settings">
      <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M15 6l-6 6 6 6"/></svg>
    </button>
    <h1>AI &amp; Voice</h1>
    <span class="spacer"></span>
  </header>

  <div class="scroll">
  <div class="inner">
    {#if loading}
      <div class="mode sk" aria-hidden="true"></div>
      <div class="mode sk" aria-hidden="true"></div>
      <div class="mode sk" aria-hidden="true"></div>
    {:else if loadError}
      <div class="empty">
        <p class="empty-title">Can't read the AI settings</p>
        <p class="empty-desc">{loadError}</p>
        <button type="button" class="pill dark" onclick={load}>Try again</button>
      </div>
    {:else if restarting}
      <div class="restart" role="status">
        <span class="spin"></span>
        <p><b>Restarting the robot…</b><br/>It will be back in about 20 seconds.</p>
      </div>
    {:else}
      <h2 class="section">Where Pupper's AI comes from</h2>

      <div class="modes" role="radiogroup" aria-label="AI mode">
        {#each modes as m}
          <button type="button" class="mode" class:selected={mode === m.id} role="radio" aria-checked={mode === m.id}
                  onclick={() => { mode = m.id; error = ""; }}>
            <span class="dot" aria-hidden="true"></span>
            <span class="mode-text">
              <span class="mode-title">{m.title} <span class="tag">{m.tag}</span></span>
              <span class="mode-desc">{m.desc}</span>
            </span>
          </button>
        {/each}
      </div>
      {#if offered && !offered.includes("own")}
        <p class="note">Using your own AI account is not in this robot's firmware yet.</p>
      {/if}

      {#if mode === "own"}
        <section class="own">
          <label class="field">
            <span>Provider</span>
            <select value={presetId} onchange={(e) => choosePreset(e.currentTarget.value)}>
              {#each PRESETS as p}<option value={p.id}>{p.label}</option>{/each}
            </select>
          </label>
          <label class="field">
            <span>Model</span>
            <input type="text" bind:value={model} placeholder="model name" autocomplete="off" spellcheck="false" />
          </label>
          <label class="field">
            <span>API base URL</span>
            <input type="url" bind:value={baseUrl} placeholder="https://…" autocomplete="off" spellcheck="false" />
          </label>
          <label class="field">
            <span>API key</span>
            <div class="key-row">
              <input type={showKey ? "text" : "password"} bind:value={apiKey} autocomplete="off" spellcheck="false"
                     placeholder={keySet ? `Saved (${keyHint}) — leave empty to keep` : "Paste your API key"} />
              <button class="pill" type="button" onclick={() => (showKey = !showKey)}>{showKey ? "Hide" : "Show"}</button>
            </div>
          </label>
          <p class="note">The key is saved on the robot and only sent to {providerHost}. The robot never shows it again.</p>
        </section>
      {/if}

      {#if runningMode !== savedMode}
        <p class="note">Saved as “{savedMode}”, still running “{runningMode}” until the robot restarts.</p>
      {/if}
      {#if warning}<p class="note">{warning}</p>{/if}
      {#if error}<p class="note bad">{error}</p>{/if}

      <button type="button" class="primary" disabled={!canSave || !changed} onclick={save}>
        {saving ? "Saving…" : (offered || livesApply) ? "Save" : "Save & restart robot"}
      </button>
      {#if offered || livesApply}
        <p class="note">Applied straight away — no restart.</p>
      {:else}
        <p class="note">The robot restarts to apply this (about 20 seconds).</p>
      {/if}

      {#if voice}
        <h2 class="section">Voice</h2>
        <section class="voice">
          <p class="voice-line">{voiceLine}</p>
          <dl class="facts">
            <dt>Link</dt><dd>{voice.link}</dd>
            <dt>Gateway</dt><dd>{voice.provisioned ? "Paired" : "Not paired yet (pairs on first wake word)"}</dd>
            <dt>Microphone</dt><dd>{voice.mic ? (voice.afe?.ready ? "Ready" : "Starting…") : "Not detected"}</dd>
            <dt>Speaker</dt><dd>{voice.speaker ? "Ready" : "Not detected"}</dd>
          </dl>

          <label class="field">
            <span>Volume <em>{volume}</em></span>
            <input type="range" min="0" max="100" step="5" value={volume}
                   disabled={voiceBusy}
                   oninput={(e) => (volumeDraft = Number(e.currentTarget.value))}
                   onchange={commitVolume} />
          </label>

          <div class="row">
            <button class="pill" type="button" disabled={voiceBusy || voice.pi_in_control}
                    onclick={() => postVoice({ hands_free: !handsFreeOn })}>
              {handsFreeOn ? "Stop listening" : "Start listening"}
            </button>
            {#if !voice.provisioned}
              <button class="pill" type="button" disabled={voiceBusy}
                      onclick={() => postVoice({ provision: true })}>Pair with MangDang now</button>
            {/if}
          </div>

          <div class="music">
            <span class="field-label">Music {voice.music?.active ? `— playing ${voice.music.title || "…"}` : ""}</span>
            <div class="row">
              <select bind:value={musicPick} disabled={voiceBusy}>
                {#each GENRES as g}<option value={g}>{g}</option>{/each}
                <option value="custom">Stream URL…</option>
              </select>
              {#if musicPick === "custom"}
                <input type="url" bind:value={musicCustom} placeholder="https://…" autocomplete="off" spellcheck="false" />
              {/if}
              <button class="pill dark" type="button" disabled={voiceBusy} onclick={playMusic}>Play</button>
              {#if voice.music?.active}
                <button class="pill" type="button" disabled={voiceBusy} onclick={() => postVoice({ music: "stop" })}>Stop</button>
              {/if}
            </div>
            <p class="note">You can also just ask: “Hi ESP, play some jazz.”</p>
          </div>

          {#if voiceError}<p class="note bad">{voiceError}</p>{/if}
        </section>
      {/if}
    {/if}
  </div>
  </div>
</div>

<style>
  .page { position: absolute; inset: 0; display: flex; flex-direction: column; background: var(--page); color: var(--ink); }
  .bar {
    flex-shrink: 0; height: calc(64px + var(--safe-top)); padding: calc(10px + var(--safe-top)) 16px 0;
    display: flex; align-items: center; justify-content: space-between;
    max-width: var(--page-max); width: 100%; margin: 0 auto;
  }
  .bar h1 { font-size: 18px; font-weight: 800; }
  .spacer { width: 44px; }
  .round {
    width: 44px; height: 44px; border-radius: 22px; border: none; background: var(--ground); color: var(--ink);
    display: grid; place-items: center; cursor: pointer;
  }

  .scroll { flex: 1; min-height: 0; overflow-y: auto; }
  .inner { max-width: var(--page-max); margin: 0 auto; padding: 8px 16px calc(28px + var(--safe-bottom)); display: flex; flex-direction: column; gap: 12px; }
  .section { margin: 8px 4px 0; font-size: 12px; font-weight: 800; letter-spacing: 0.12em; text-transform: uppercase; color: var(--sub); }

  .modes { display: flex; flex-direction: column; gap: 10px; }
  .mode {
    display: flex; align-items: flex-start; gap: 12px; text-align: left;
    padding: 14px 16px; border-radius: 20px; cursor: pointer;
    background: #fff; border: 1.5px solid var(--line); color: var(--ink);
    transition: background var(--dur-fast), border-color var(--dur-fast);
  }
  .mode.selected { border-color: var(--ink); background: #FFF6B0; }
  .mode.sk { height: 84px; border-color: var(--line); background: var(--ground); cursor: default; }
  .dot {
    flex: none; width: 22px; height: 22px; margin-top: 1px; border-radius: 50%;
    border: 2px solid #A39F96; background: #fff;
  }
  .mode.selected .dot { border-color: var(--ink); box-shadow: inset 0 0 0 4px #fff; background: var(--ink); }
  .mode-text { display: flex; flex-direction: column; gap: 4px; min-width: 0; }
  .mode-title { font-size: 16px; font-weight: 700; }
  .tag {
    display: inline-block; margin-left: 6px; padding: 2px 8px; border-radius: 8px;
    background: var(--ground); color: #4F4C46; font-size: 11px; font-weight: 700; vertical-align: 2px;
  }
  .mode.selected .tag { background: var(--mpx-yellow); color: var(--ink); }
  .mode-desc { font-size: 13px; color: var(--sub); line-height: 1.45; }

  .own, .voice {
    display: flex; flex-direction: column; gap: 12px;
    padding: 16px; border-radius: 22px; background: #fff; border: 1px solid var(--line);
  }
  .voice-line { font-size: 15px; font-weight: 700; }
  .facts {
    display: grid; grid-template-columns: auto 1fr; gap: 6px 12px; margin: 0;
    background: var(--ground); border-radius: 14px; padding: 10px 12px; font-size: 13px;
  }
  .facts dt { font-size: 11px; font-weight: 700; letter-spacing: 0.1em; text-transform: uppercase; color: var(--sub); align-self: center; }
  .facts dd { margin: 0; }
  .field { display: flex; flex-direction: column; gap: 6px; }
  .field > span, .field-label { font-size: 11px; font-weight: 700; letter-spacing: 0.1em; text-transform: uppercase; color: var(--sub); padding: 0 4px; }
  .field em { font-style: normal; font-weight: 700; color: var(--ink); margin-left: 6px; letter-spacing: 0; }
  .field input, .field select, .music select, .music input {
    font: inherit; font-size: 16px; height: 48px;
    padding: 0 14px; border-radius: 14px; border: none;
    background: var(--ground); color: var(--ink); min-width: 0; width: 100%; outline: none;
  }
  .field input:focus-visible, .field select:focus-visible, .music select:focus-visible, .music input:focus-visible { outline: 3px solid var(--ink); outline-offset: 2px; }
  .field input[type="range"] { height: auto; padding: 0; background: none; accent-color: var(--ink); }
  .key-row { display: flex; gap: 8px; align-items: center; }
  .key-row input { flex: 1; }
  .row { display: flex; gap: 8px; flex-wrap: wrap; align-items: center; }
  .row select, .row input { flex: 1; width: auto; }
  .music { display: flex; flex-direction: column; gap: 8px; }

  .note { margin: 0; font-size: 13px; color: var(--sub); line-height: 1.45; padding: 0 4px; }
  .note.bad { color: #9B2C20; font-weight: 600; }

  .primary {
    margin-top: 4px; height: 56px; border: none; border-radius: 28px; cursor: pointer;
    background: var(--ink); color: #fff; font-size: 16px; font-weight: 700;
    transition: transform var(--dur-fast), opacity var(--dur-fast);
  }
  .primary:active:not(:disabled) { transform: scale(0.98); }
  .primary:disabled { opacity: 0.35; cursor: default; }
  .pill {
    height: 40px; padding: 0 16px; border-radius: 20px; border: none; flex-shrink: 0;
    background: var(--ground); color: var(--ink); font-size: 14px; font-weight: 700; cursor: pointer;
  }
  .pill.dark { background: var(--ink); color: #fff; }
  .pill:disabled { opacity: 0.4; cursor: default; }
  button:focus-visible { outline: 3px solid var(--ink); outline-offset: 2px; }

  .empty { background: var(--ground); border-radius: 20px; padding: 28px 20px; display: flex; flex-direction: column; align-items: center; gap: 8px; text-align: center; }
  .empty-title { font-size: 17px; font-weight: 800; }
  .empty-desc { font-size: 14px; line-height: 1.45; color: var(--sub); max-width: 300px; }
  .empty .pill { margin-top: 6px; }

  .restart { display: flex; align-items: center; gap: 14px; padding: 20px; border-radius: 20px; background: var(--ground); }
  .restart p { margin: 0; font-size: 14px; line-height: 1.5; }
  .spin {
    width: 26px; height: 26px; border-radius: 50%; flex: none;
    border: 3px solid var(--hair); border-top-color: var(--ink);
    animation: spin 0.8s linear infinite;
  }
  @keyframes spin { to { transform: rotate(360deg); } }
</style>
