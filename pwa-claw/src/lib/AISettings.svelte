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

<div class="ai-root">
  <header class="ai-header">
    <button class="icon-btn" onclick={() => onBack?.()} aria-label="Go back">
      <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
        <line x1="19" y1="12" x2="5" y2="12"/><polyline points="12 19 5 12 12 5"/>
      </svg>
    </button>
    <h2 class="ai-title">AI &amp; Voice</h2>
    <span class="spacer"></span>
  </header>

  <div class="ai-body">
    {#if loading}
      <p class="note">Reading the robot's AI settings…</p>
    {:else if loadError}
      <p class="note bad">{loadError}</p>
      <button class="ghost" onclick={load}>Try again</button>
    {:else if restarting}
      <div class="restart">
        <span class="spin"></span>
        <p><b>Restarting the robot…</b><br/>It will be back in about 20 seconds.</p>
      </div>
    {:else}
      <p class="lead">Choose where Pupper's AI comes from.</p>

      <div class="modes" role="radiogroup" aria-label="AI mode">
        {#each modes as m}
          <button class="mode" class:selected={mode === m.id} role="radio" aria-checked={mode === m.id}
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
              <button class="ghost small" type="button" onclick={() => (showKey = !showKey)}>{showKey ? "Hide" : "Show"}</button>
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

      <button class="primary" disabled={!canSave || !changed} onclick={save}>
        {saving ? "Saving…" : (offered || livesApply) ? "Save" : "Save & restart robot"}
      </button>
      {#if offered || livesApply}
        <p class="note">Applied straight away — no restart.</p>
      {:else}
        <p class="note">The robot restarts to apply this (about 20 seconds).</p>
      {/if}

      {#if voice}
        <section class="voice">
          <h3 class="voice-title">Voice</h3>
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
            <button class="ghost small" type="button" disabled={voiceBusy || voice.pi_in_control}
                    onclick={() => postVoice({ hands_free: !handsFreeOn })}>
              {handsFreeOn ? "Stop listening" : "Start listening"}
            </button>
            {#if !voice.provisioned}
              <button class="ghost small" type="button" disabled={voiceBusy}
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
              <button class="ghost small" type="button" disabled={voiceBusy} onclick={playMusic}>Play</button>
              {#if voice.music?.active}
                <button class="ghost small" type="button" disabled={voiceBusy} onclick={() => postVoice({ music: "stop" })}>Stop</button>
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

<style>
  .ai-root {
    flex: 1;
    min-height: 0;
    background: var(--mpx-surface-2);
    display: flex;
    flex-direction: column;
  }
  .ai-header {
    display: flex; align-items: center; gap: 10px;
    padding: 10px 12px;
    background: var(--yellow, #FFE605);
    flex-shrink: 0;
  }
  .icon-btn {
    width: 34px; height: 34px; display: grid; place-items: center;
    border: none; background: none; border-radius: 50%; cursor: pointer; color: var(--mpx-ink);
  }
  .icon-btn:hover { background: rgba(0,0,0,0.08); }
  .ai-title { flex: 1; font-size: 1.02rem; font-weight: 800; color: var(--mpx-ink); text-align: center; }
  .spacer { width: 34px; }

  .ai-body {
    flex: 1; min-height: 0; overflow-y: auto;
    padding: 16px 16px 32px;
    display: flex; flex-direction: column; gap: 12px;
    max-width: 560px; width: 100%; margin: 0 auto; box-sizing: border-box;
  }
  .lead { margin: 0; font-weight: 700; color: var(--mpx-ink); }

  .modes { display: flex; flex-direction: column; gap: 10px; }
  .mode {
    display: flex; align-items: flex-start; gap: 12px; text-align: left;
    padding: 14px; border-radius: 14px; cursor: pointer;
    background: var(--mpx-surface); border: 2px solid var(--mpx-line);
    color: var(--mpx-ink); font: inherit;
  }
  .mode.selected { border-color: var(--mpx-ink); }
  .dot {
    flex: none; width: 20px; height: 20px; margin-top: 2px; border-radius: 50%;
    border: 2px solid var(--mpx-muted); background: var(--mpx-surface);
  }
  .mode.selected .dot { border-color: var(--mpx-ink); box-shadow: inset 0 0 0 4px var(--mpx-surface); background: var(--mpx-ink); }
  .mode-text { display: flex; flex-direction: column; gap: 4px; min-width: 0; }
  .mode-title { font-weight: 800; }
  .tag {
    display: inline-block; margin-left: 6px; padding: 1px 8px; border-radius: 999px;
    background: var(--yellow, #FFE605); font-size: 0.68rem; font-weight: 800; vertical-align: 2px;
  }
  .mode-desc { font-size: 0.86rem; color: var(--mpx-ink-soft, #2B2B2B); line-height: 1.35; }

  .own, .voice {
    display: flex; flex-direction: column; gap: 12px;
    padding: 14px; border-radius: 14px; background: var(--mpx-surface); border: 1px solid var(--mpx-line);
  }
  .voice { margin-top: 8px; }
  .voice-title { margin: 0; font-size: 0.95rem; font-weight: 800; color: var(--mpx-ink); }
  .voice-line { margin: 0; font-size: 0.86rem; color: var(--mpx-ink-soft, #2B2B2B); }
  .facts {
    display: grid; grid-template-columns: auto 1fr; gap: 4px 12px; margin: 0;
    font-size: 0.8rem; color: var(--mpx-ink);
  }
  .facts dt { font-weight: 700; color: var(--mpx-muted); }
  .facts dd { margin: 0; }
  .field { display: flex; flex-direction: column; gap: 6px; font-size: 0.8rem; font-weight: 700; color: var(--mpx-ink); }
  .field em { font-style: normal; font-weight: 500; color: var(--mpx-muted); margin-left: 6px; }
  .field-label { font-size: 0.8rem; font-weight: 700; color: var(--mpx-ink); }
  .field input, .field select, .music select, .music input {
    font: inherit; font-weight: 500; font-size: 0.95rem;
    padding: 10px 12px; border-radius: 10px; border: 1px solid var(--mpx-muted-2, #BDBDBD);
    background: var(--mpx-surface); color: var(--mpx-ink); min-width: 0; width: 100%; box-sizing: border-box;
  }
  .field input[type="range"] { padding: 0; border: none; accent-color: var(--mpx-ink); }
  .key-row { display: flex; gap: 8px; }
  .key-row input { flex: 1; }
  .row { display: flex; gap: 8px; flex-wrap: wrap; align-items: center; }
  .row select, .row input { flex: 1; width: auto; }
  .music { display: flex; flex-direction: column; gap: 8px; }

  .note { margin: 0; font-size: 0.8rem; color: var(--mpx-muted); line-height: 1.35; }
  .note.bad { color: #b42318; font-weight: 700; }

  .primary {
    margin-top: 4px; padding: 14px; border: none; border-radius: 999px; cursor: pointer;
    background: var(--mpx-ink); color: var(--yellow, #FFE605); font: inherit; font-weight: 800; font-size: 1rem;
  }
  .primary:disabled { opacity: 0.4; cursor: not-allowed; }
  .ghost {
    padding: 10px 14px; border-radius: 999px; border: 1px solid var(--mpx-ink);
    background: none; color: var(--mpx-ink); font: inherit; font-weight: 700; cursor: pointer;
  }
  .ghost:disabled { opacity: 0.4; cursor: not-allowed; }
  .ghost.small { padding: 8px 12px; font-size: 0.8rem; }

  .restart { display: flex; align-items: center; gap: 14px; padding: 24px 4px; color: var(--mpx-ink); }
  .restart p { margin: 0; line-height: 1.5; }
  .spin {
    width: 26px; height: 26px; border-radius: 50%; flex: none;
    border: 3px solid rgba(0,0,0,0.12); border-top-color: var(--mpx-ink);
    animation: spin 0.8s linear infinite;
  }
  @keyframes spin { to { transform: rotate(360deg); } }
</style>
