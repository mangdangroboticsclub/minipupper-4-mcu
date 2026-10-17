<script>
  import WiFiSetup from "./WiFiSetup.svelte";
  import APModeConfig from "./APModeConfig.svelte";
  import Calibration from "./Calibration.svelte";
  import AISettings from "./AISettings.svelte";
  import ServoStudio from "./ServoStudio.svelte";
  import FileViewer from "./FileViewer.svelte";
  import UploadView from "./UploadView.svelte";
  import DisplayPage from "./DisplayPage.svelte";
  import RLWalkPage from "./RLWalkPage.svelte";

  let { network } = $props();

  /** null | "wifi" | "hotspot" | "ai" | "calibration" | "studio" | "display" | "rl" | "files" | "upload" */
  let page = $state(null);

  const staConnected = $derived(network?.staState === "connected");
  const piIp = $derived(network?.piIp || "");
  // mpx-dog: the Pi link, on-device AI and the LCD are later porting phases.
  // Each section shows only once its firmware endpoint answers, so this one
  // app build runs against both the phase-1 firmware and the full one.
  const hasPi = $derived(!!network?.hasPi);
  let hasAi = $state(false);
  let hasDisplay = $state(false);

  // ── AI mode label ──
  let aiMode = $state("");
  $effect(() => {
    fetch("/v1/ai", { cache: "no-store" })
      .then((r) => (r.ok ? r.json() : null))
      .then((j) => { if (j) { hasAi = true; aiMode = j.running_mode || j.mode || ""; } })
      .catch(() => {});
  });
  const aiLabel = $derived(
    aiMode === "mangdang" ? "MangDang AI"
    : aiMode === "own" ? "Your own AI account"
    : aiMode === "off" ? "AI off"
    : "Chat and voice"
  );

  // ── Agent permissions (/v1/agent/permissions): "Allow writes (YOLO)" ──
  let allowWrites = $state(null);   // null until the firmware answers (older builds: no row)
  let permsBusy = $state(false);
  $effect(() => {
    fetch("/v1/agent/permissions", { cache: "no-store" })
      .then((r) => (r.ok ? r.json() : null))
      .then((j) => { if (j && typeof j.allow_writes === "boolean") allowWrites = j.allow_writes; })
      .catch(() => {});
  });

  async function toggleAllowWrites() {
    if (allowWrites === null || permsBusy) return;
    const next = !allowWrites;
    if (next && !confirm(
      "Allow writes without asking?\n\nScripts and skills started by the agent will save and delete files on the robot with no permission prompt. Turn this off again to be asked first."
    )) return;
    permsBusy = true;
    try {
      const r = await fetch("/v1/agent/permissions", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ allow_writes: next }),
      });
      if (r.ok) allowWrites = (await r.json()).allow_writes;
    } catch { /* keep the old state */ }
    permsBusy = false;
  }

  // ── Display (/v1/display): eyes or an animation ──
  let display = $state(null);
  $effect(() => {
    if (page !== null) return;           // refresh when coming back from the Display page
    fetch("/v1/display", { cache: "no-store" })
      .then((r) => (r.ok ? r.json() : null))
      .then((j) => { display = j; if (j) hasDisplay = true; })
      .catch(() => {});
  });
  const displayLabel = $derived.by(() => {
    if (!display?.on) return "Eyes · or play an animation";
    const a = display.anims?.find((x) => x.id === display.id);
    return `Playing ${a?.name || display.id}${display.loop ? "" : " once"}`;
  });

  // ── RL walk on the ESP32-S3 (/v1/policy) and its ROS 2 card (/v1/uros) ──
  let policy = $state(null);
  $effect(() => {
    if (page !== null) return;           // refresh when coming back from the RL page
    fetch("/v1/policy", { cache: "no-store" })
      .then((r) => (r.ok ? r.json() : null))
      .then((j) => { policy = j; })
      .catch(() => {});
  });
  const policyLabel = $derived.by(() => {
    if (!policy?.loaded) return "No policy in this firmware";
    if (policy.active) return policy.phase === "run" ? "Walking" : "Running";
    return `${policy.name} · runs on the robot itself`;
  });

  // ── Rest when idle (/v1/robot/power) ──
  const REST_CHOICES = [
    { s: 120, label: "2 min" },
    { s: 300, label: "5 min" },
    { s: 600, label: "10 min" },
    { s: 1800, label: "30 min" },
    { s: 0, label: "Never" },
  ];
  let power = $state(null);       // { rest_after_s, state, resting }
  let restOpen = $state(false);
  let restBusy = $state(false);

  async function loadPower() {
    try {
      const r = await fetch("/v1/robot/power", { cache: "no-store" });
      if (r.ok) power = await r.json();
    } catch { /* older firmware: no rest setting */ }
  }
  async function postPower(body) {
    restBusy = true;
    try {
      const r = await fetch("/v1/robot/power", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(body),
      });
      if (r.ok) power = await r.json();
    } catch { /* ignore */ }
    restBusy = false;
  }
  $effect(() => { loadPower(); });

  const restLabel = $derived.by(() => {
    if (!power) return "Lie down and switch the servos off when unused";
    const s = power.rest_after_s;
    const when = s === 0 ? "Never rests" : `After ${s >= 60 ? Math.round(s / 60) + " min" : s + " s"} unused`;
    return power.resting ? `${when} · resting now` : when;
  });

  // ── Shut down / restart the Pi (/v1/pi/power -> tools/pi/pi_power.py) ──
  // phase: idle | confirm-shutdown | confirm-reboot | sending | done | error
  let piPower = $state({ phase: "idle", action: "", msg: "", gone: false });
  let piPowerOpen = $state(false);

  const PI_POWER_ERRORS = {
    pi_not_seen: "The robot hasn't heard from the Pi, so it doesn't know its address. Is pi_announce running?",
    pi_unreachable: "Couldn't reach the Pi. Is pi_power installed on it, and are the robot and the Pi on the same Wi‑Fi?",
    bad_token: "The Pi refused: the token doesn't match (APP_PI_POWER_TOKEN in the firmware vs /etc/default/pi-power on the Pi).",
    pi_refused: "The Pi refused the request.",
    pi_link_disabled: "This firmware was built without the Pi link.",
  };

  async function sendPiPower(action) {
    piPower = { phase: "sending", action, msg: "", gone: false };
    try {
      const r = await fetch("/v1/pi/power", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ action }),
      });
      if (r.status === 404) {
        piPower = { phase: "error", action, msg: "This firmware has no Pi power control yet. Update the robot.", gone: false };
        return;
      }
      const j = await r.json().catch(() => ({}));
      if (j.ok) {
        piPower = { phase: "done", action, msg: "", gone: false };
      } else {
        piPower = { phase: "error", action, msg: PI_POWER_ERRORS[j.error] || `Failed (${j.error || "HTTP " + r.status}).`, gone: false };
      }
    } catch {
      piPower = { phase: "error", action, msg: "Couldn't reach the robot.", gone: false };
    }
  }

  // After the request, watch the Pi's address: it disappears a few seconds
  // after pi_announce / the bridge stop (HOST_INFO goes stale after 6 s).
  $effect(() => {
    if (piPower.phase !== "done") return;
    if (!piIp && !piPower.gone) {
      piPower.gone = true;
    } else if (piIp && piPower.gone && piPower.action === "reboot") {
      piPower = { phase: "idle", action: "", msg: "", gone: false };   // back up
      piPowerOpen = false;
    }
  });

  const piPowerLabel = $derived.by(() => {
    const { phase, action, gone } = piPower;
    if (phase === "sending") return action === "reboot" ? "Asking the Pi to restart…" : "Asking the Pi to shut down…";
    if (phase === "done" && action === "reboot") return gone ? "Restarting…" : "Restart requested";
    if (phase === "done") return gone ? "Shut down · wait for the green light to stop" : "Shutting down…";
    if (phase === "error") return "Didn't work · tap for details";
    return "Switch the Pi off safely before unplugging it";
  });

  function back() {
    page = page === "upload" ? "files" : null;
  }
</script>

{#snippet row(opts)}
  <button type="button" class="row" onclick={opts.onclick} aria-expanded={opts.expanded}>
    <span class="ico" class:dark={opts.dark}>{@render opts.icon()}</span>
    <span class="txt">
      <span class="t">{opts.title}</span>
      <span class="d" class:ok={opts.ok}>{opts.desc}</span>
    </span>
    {#if opts.chevron !== false}
      <svg class="chev" class:down={opts.expanded} width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M9 6l6 6-6 6"/></svg>
    {/if}
  </button>
{/snippet}

{#snippet iWifi()}<svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" aria-hidden="true"><path d="M2 8.5a15 15 0 0 1 20 0M5 12a10 10 0 0 1 14 0M8.5 15.5a5 5 0 0 1 7 0"/><circle cx="12" cy="19" r="1.2" fill="currentColor"/></svg>{/snippet}
{#snippet iHotspot()}<svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" aria-hidden="true"><circle cx="12" cy="12" r="2"/><path d="M8 8a6 6 0 0 0 0 8M16 8a6 6 0 0 1 0 8M5 5a10 10 0 0 0 0 14M19 5a10 10 0 0 1 0 14"/></svg>{/snippet}
{#snippet iPi()}<svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><rect x="3" y="5" width="18" height="14" rx="2"/><path d="M7 9h4v4H7zM14 9h3M14 13h3"/></svg>{/snippet}
{#snippet iPower()}<svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" aria-hidden="true"><path d="M12 3v9"/><path d="M6.3 7.2a8 8 0 1 0 11.4 0"/></svg>{/snippet}
{#snippet iAi()}<svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" aria-hidden="true"><rect x="9" y="3" width="6" height="11" rx="3"/><path d="M5 11a7 7 0 0 0 14 0M12 18v3"/></svg>{/snippet}
{#snippet iCal()}<svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M5 20l4-9 5 3 5-9"/><circle cx="9" cy="11" r="1.6" fill="currentColor"/><circle cx="14" cy="14" r="1.6" fill="currentColor"/></svg>{/snippet}
{#snippet iStudio()}<svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" aria-hidden="true"><path d="M6 4v16M12 4v16M18 4v16"/><path d="M4 9h4M10 14h4M16 7h4" stroke-width="2.6"/></svg>{/snippet}
{#snippet iRest()}<svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M20 14.5A8 8 0 1 1 9.5 4a6.5 6.5 0 0 0 10.5 10.5z"/></svg>{/snippet}
{#snippet iDisplay()}<svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><rect x="3" y="5" width="18" height="14" rx="2"/><path d="M8.5 10v4M15.5 10v4"/></svg>{/snippet}
{#snippet iRl()}<svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><circle cx="6" cy="6" r="2.2"/><circle cx="18" cy="6" r="2.2"/><circle cx="12" cy="18" r="2.2"/><path d="M8 7.5l3 8.5M16 7.5l-3 8.5M8.2 6h7.6"/></svg>{/snippet}
{#snippet iUnlock()}<svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><rect x="5" y="11" width="14" height="10" rx="2"/><path d="M8 11V7a4 4 0 0 1 7.5-2"/></svg>{/snippet}
{#snippet iFiles()}<svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"/></svg>{/snippet}

{#if page === "wifi"}
  <WiFiSetup onNavigate={back} {network} />
{:else if page === "hotspot"}
  <APModeConfig onNavigate={back} {network} />
{:else if page === "ai"}
  <AISettings onBack={back} />
{:else if page === "calibration"}
  <Calibration onBack={back} />
{:else if page === "studio"}
  <ServoStudio onBack={back} />
{:else if page === "display"}
  <DisplayPage onBack={back} />
{:else if page === "rl"}
  <RLWalkPage onBack={back} />
{:else if page === "upload"}
  <UploadView onBack={back} />
{:else if page === "files"}
  <FileViewer onNavigate={(v) => { if (v === "upload") page = "upload"; else page = null; }} />
{:else}
  <div class="settings">
    <div class="inner">
      <h1>Settings</h1>

      <section>
        <h2>ESP32 · robot controller</h2>
        <div class="group">
          {@render row({ icon: iWifi, title: "ESP Wi‑Fi", desc: staConnected ? `${network?.staSsid} · ${network?.staIp}` : "Not connected · join a network", onclick: () => (page = "wifi") })}
          {@render row({ icon: iHotspot, title: "ESP hotspot", desc: `${network?.apSsid || "—"} · ${network?.apIp || "—"}`, onclick: () => (page = "hotspot") })}
        </div>
      </section>

      {#if hasPi}
        <section>
          <h2>Raspberry Pi</h2>
          <div class="group">
            <div class="row static">
              <span class="ico dark">{@render iPi()}</span>
              <span class="txt">
                <span class="t">Pi link</span>
                {#if piIp}
                  <span class="d ok">Connected · <span class="mono">{piIp}</span></span>
                {:else}
                  <span class="d">Not connected. Run mp4_link.py on the Pi</span>
                {/if}
              </span>
            </div>
            {#if piIp || piPower.phase !== "idle"}
              {@render row({ icon: iPower, dark: true, title: "Shut down Pi", desc: piPowerLabel, expanded: piPowerOpen, onclick: () => (piPowerOpen = !piPowerOpen) })}
              {#if piPowerOpen}
                <div class="rest">
                  {#if piPower.phase === "confirm-shutdown" || piPower.phase === "confirm-reboot"}
                    {@const reboot = piPower.phase === "confirm-reboot"}
                    <p class="note strong">{reboot ? "Restart the Raspberry Pi?" : "Shut down the Raspberry Pi?"}</p>
                    <p class="note">ROS, the RL policy and anything else running on the Pi will stop. If the Pi is driving the robot, the robot stops and stands.</p>
                    <div class="rest-actions">
                      <button type="button" class="act" class:danger={!reboot} onclick={() => sendPiPower(reboot ? "reboot" : "shutdown")}>{reboot ? "Yes, restart" : "Yes, shut down"}</button>
                      <button type="button" class="act ghost" onclick={() => (piPower = { phase: "idle", action: "", msg: "", gone: false })}>Cancel</button>
                    </div>
                  {:else if piPower.phase === "sending"}
                    <p class="note">Asking the Pi…</p>
                  {:else if piPower.phase === "done" && piPower.action === "shutdown"}
                    {#if piPower.gone}
                      <p class="note strong ok">The Pi has gone offline.</p>
                      <p class="note">Wait until its green activity light stops flashing (about 10–20 s more). Then it's safe to switch off or unplug it.</p>
                    {:else}
                      <p class="note">The Pi is shutting down. Don't unplug it yet…</p>
                    {/if}
                  {:else if piPower.phase === "done"}
                    <p class="note">The Pi is restarting. It will be back here in about a minute.</p>
                  {:else}
                    {#if piPower.phase === "error"}
                      <p class="note err">{piPower.msg}</p>
                    {/if}
                    <div class="rest-actions">
                      <button type="button" class="act danger" disabled={!piIp} onclick={() => (piPower = { phase: "confirm-shutdown", action: "", msg: "", gone: false })}>Shut down</button>
                      <button type="button" class="act ghost" disabled={!piIp} onclick={() => (piPower = { phase: "confirm-reboot", action: "", msg: "", gone: false })}>Restart</button>
                    </div>
                    <p class="note">Needs the pi_power service on the Pi (tools/pi/README.md).</p>
                  {/if}
                </div>
              {/if}
            {/if}
          </div>
        </section>
      {/if}

      {#if hasAi || allowWrites !== null}
        <section>
          <h2>AI</h2>
          <div class="group">
            {#if hasAi}
              {@render row({ icon: iAi, title: "AI & Voice", desc: aiLabel, onclick: () => (page = "ai") })}
            {/if}
            {#if allowWrites !== null}
              <button
                type="button"
                class="row"
                role="switch"
                aria-checked={allowWrites}
                disabled={permsBusy}
                onclick={toggleAllowWrites}
              >
                <span class="ico" class:warn={allowWrites}>{@render iUnlock()}</span>
                <span class="txt">
                  <span class="t">Allow writes (YOLO)</span>
                  <span class="d" class:warn={allowWrites}>
                    {allowWrites ? "On · files are saved and deleted without asking" : "Off · ask before saving or deleting files"}
                  </span>
                </span>
                <span class="switch" class:on={allowWrites} aria-hidden="true"><span class="knob"></span></span>
              </button>
            {/if}
          </div>
        </section>
      {/if}

      <section>
        <h2>Robot</h2>
        <div class="group">
          {#if hasDisplay}
            {@render row({ icon: iDisplay, title: "Display", desc: displayLabel, onclick: () => (page = "display") })}
          {/if}
          {#if policy?.available}
            {@render row({ icon: iRl, title: "RL walk", desc: policyLabel, onclick: () => (page = "rl") })}
          {/if}
          {@render row({ icon: iCal, title: "Calibration", desc: "Line up each leg in 3D", onclick: () => (page = "calibration") })}
          {@render row({ icon: iStudio, title: "MangDang Servo Studio", desc: "Gains on the driver boards", onclick: () => (page = "studio") })}
          {#if power}
            {@render row({ icon: iRest, title: "Rest when idle", desc: restLabel, expanded: restOpen, onclick: () => (restOpen = !restOpen) })}
            {#if restOpen}
              <div class="rest">
                <div class="seg" role="radiogroup" aria-label="Rest after">
                  {#each REST_CHOICES as c}
                    <button
                      type="button"
                      role="radio"
                      aria-checked={power.rest_after_s === c.s}
                      class:on={power.rest_after_s === c.s}
                      disabled={restBusy}
                      onclick={() => postPower({ rest_after_s: c.s })}
                    >{c.label}</button>
                  {/each}
                </div>
                <div class="rest-actions">
                  {#if power.resting}
                    <button type="button" class="act" disabled={restBusy} onclick={() => postPower({ action: "wake" })}>Stand up now</button>
                  {:else}
                    <button type="button" class="act" disabled={restBusy} onclick={() => postPower({ action: "rest" })}>Rest now</button>
                  {/if}
                </div>
                <p class="note">Resting lowers the body and switches the servos off to save the battery. Any move or stick wakes it.</p>
              </div>
            {/if}
          {/if}
        </div>
      </section>

      <section>
        <h2>Advanced</h2>
        <div class="group">
          {@render row({ icon: iFiles, title: "Files on the robot", desc: "Browse, upload and run skill files", onclick: () => (page = "files") })}
        </div>
      </section>
    </div>
  </div>
{/if}

<style>
  .settings { position: absolute; inset: 0; overflow-y: auto; background: var(--page); }
  .inner { max-width: var(--page-max); margin: 0 auto; padding: calc(26px + var(--safe-top)) 16px 24px; display: flex; flex-direction: column; gap: 20px; }
  h1 { margin: 0 4px; font-size: 34px; font-weight: 800; letter-spacing: -0.02em; }
  section { display: flex; flex-direction: column; gap: 8px; }
  h2 { margin: 0 6px; font-size: 12px; font-weight: 800; letter-spacing: 0.12em; text-transform: uppercase; color: var(--sub); }
  .group { background: #fff; border: 1px solid var(--line); border-radius: 20px; overflow: hidden; }
  .group > * + * { border-top: 1px solid var(--hair-2); }
  .row {
    width: 100%; border: none; background: transparent; color: var(--ink);
    display: flex; align-items: center; gap: 14px; padding: 14px 16px; text-align: left; cursor: pointer;
  }
  .row.static { cursor: default; }
  .row:focus-visible { outline: 3px solid var(--ink); outline-offset: -3px; }
  .ico { width: 38px; height: 38px; border-radius: 11px; background: var(--mpx-yellow); color: var(--ink); display: grid; place-items: center; flex-shrink: 0; }
  .ico.dark { background: var(--ink); color: var(--mpx-yellow); }
  .txt { flex: 1; min-width: 0; display: flex; flex-direction: column; gap: 2px; }
  .t { font-size: 16px; font-weight: 700; }
  .d { font-size: 13px; color: var(--sub); overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  .d.ok { color: #1E7B34; font-weight: 600; }
  .mono { font-family: var(--mono); color: var(--sub); font-weight: 500; }
  .chev { color: #A39F96; flex-shrink: 0; transition: transform var(--dur-fast); }
  .chev.down { transform: rotate(90deg); }
  .row:disabled { cursor: default; opacity: 0.7; }
  .ico.warn { background: #F6C9C2; color: #9B2C20; }
  .d.warn { color: #9B2C20; font-weight: 600; }
  .switch { width: 46px; height: 28px; border-radius: 14px; background: var(--hair); flex-shrink: 0; position: relative; transition: background var(--dur-fast); }
  .switch .knob { position: absolute; top: 3px; left: 3px; width: 22px; height: 22px; border-radius: 11px; background: #fff; box-shadow: 0 1px 3px rgba(0, 0, 0, 0.2); transition: transform var(--dur-fast); }
  .switch.on { background: #9B2C20; }
  .switch.on .knob { transform: translateX(18px); }

  .rest { padding: 4px 16px 16px; display: flex; flex-direction: column; gap: 12px; border-top: none !important; }
  .seg { display: grid; grid-template-columns: repeat(5, minmax(0, 1fr)); gap: 4px; background: var(--hair); border-radius: 14px; padding: 4px; }
  .seg button {
    height: 40px; border: none; border-radius: 10px; background: transparent; color: #4F4C46;
    font-size: 13px; font-weight: 700; cursor: pointer;
  }
  .seg button.on { background: #fff; color: var(--ink); box-shadow: 0 1px 3px rgba(0, 0, 0, 0.12); }
  .seg button:focus-visible { outline: 3px solid var(--ink); outline-offset: 1px; }
  .rest-actions { display: flex; }
  .act { height: 44px; border-radius: 22px; border: none; background: var(--ink); color: #fff; padding: 0 18px; font-size: 14px; font-weight: 700; cursor: pointer; }
  .act:disabled, .seg button:disabled { opacity: 0.6; }
  .note { font-size: 13px; line-height: 1.45; color: var(--sub); }
  .note.strong { color: var(--ink); font-weight: 700; font-size: 14px; }
  .note.ok { color: #1E7B34; }
  .note.err { color: #B3261E; font-weight: 600; }
  .rest-actions { gap: 8px; }
  .act.danger { background: #B3261E; }
  .act.ghost { background: var(--hair); color: var(--ink); }</style>
