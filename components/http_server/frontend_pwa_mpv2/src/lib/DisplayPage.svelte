<script>
  /**
   * Display — what the robot's screen shows: its eyes, or a GIF animation
   * stored on the robot (<storage>/display/*.gif).
   *
   * Turning "Play animation" on replaces the eyes with the chosen GIF; off
   * gives the screen back to the eyes. "Loop it" keeps the animation up (and
   * the robot brings it back after a restart); "Play once" shows it one time
   * and then the eyes return by themselves.
   *
   * Talks to /v1/display (http_server_mpx_display_api.c).
   */
  let { onBack } = $props();

  let st = $state(null);          // GET /v1/display
  let pick = $state("");          // the selected animation id
  let loop = $state(true);
  let busy = $state(false);
  let msg = $state("");
  let msgBad = $state(false);
  let confirmDel = $state("");
  let thumbsUpTo = $state(0);     // thumbnails load one at a time (the robot serves 5 sockets)
  let fileInput;

  const on = $derived(!!st?.on);
  const anims = $derived(st?.anims || []);
  const current = $derived(anims.find((a) => a.id === pick) || null);
  const usedPct = $derived(st?.storage?.total ? Math.min(100, (st.storage.used / st.storage.total) * 100) : 0);

  function say(m, bad = false) { msg = m; msgBad = bad; }
  const mb = (b) => (b / 1048576).toFixed(1);
  const kb = (b) => Math.round(b / 1024) + " KB";

  function apply(j) {
    st = j;
    loop = j.loop ?? true;
    if (!pick || !j.anims?.some((a) => a.id === pick)) {
      pick = j.id && j.anims?.some((a) => a.id === j.id) ? j.id : (j.anims?.[0]?.id || "");
    }
  }

  async function load() {
    try {
      const r = await fetch("/v1/display", { cache: "no-store" });
      if (!r.ok) throw new Error("HTTP " + r.status);
      apply(await r.json());
      say("");
    } catch {
      say("Could not reach the robot's display settings.", true);
    }
  }
  $effect(() => { load(); });
  // A play-once animation hands the screen back to the eyes by itself;
  // check now and then so the switch follows it.
  $effect(() => {
    if (!on || loop) return;
    const t = setInterval(load, 2000);
    return () => clearInterval(t);
  });

  async function post(body) {
    busy = true;
    try {
      const r = await fetch("/v1/display", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(body),
      });
      if (!r.ok) {
        say((await r.text()) || "The robot said no.", true);
      } else {
        apply(await r.json());
        say("");
      }
    } catch {
      say("The robot did not answer.", true);
    }
    busy = false;
  }

  function toggle() {
    if (busy) return;
    if (on) post({ on: false });
    else if (pick) post({ on: true, id: pick, loop });
    else say("Add a GIF first.", true);
  }
  function choose(id) {
    pick = id;
    confirmDel = "";
    if (on) post({ on: true, id, loop });
  }
  function setLoop(v) {
    loop = v;
    if (on && pick) post({ on: true, id: pick, loop: v });
  }

  // ── Adding a GIF ──
  function idFromName(name) {
    const base = name.replace(/\.gif$/i, "").toLowerCase().replace(/[^a-z0-9_-]+/g, "-").replace(/^-+|-+$/g, "");
    return (base || "animation").slice(0, 24);
  }
  async function gifSize(file) {
    const b = new Uint8Array(await file.slice(0, 10).arrayBuffer());
    const sig = String.fromCharCode(...b.slice(0, 6));
    if (sig !== "GIF87a" && sig !== "GIF89a") return null;
    return { w: b[6] | (b[7] << 8), h: b[8] | (b[9] << 8) };
  }
  async function onFile(e) {
    const file = e.currentTarget.files?.[0];
    e.currentTarget.value = "";
    if (!file) return;
    const max = st?.max_bytes || 600 * 1024;
    const dim = await gifSize(file);
    if (!dim) return say("That file is not a GIF.", true);
    if (dim.w > (st?.max_w || 320) || dim.h > (st?.max_h || 240))
      return say(`It is ${dim.w} × ${dim.h}. The screen is 320 × 240, so make it that size or smaller.`, true);
    if (file.size > max) return say(`It is ${kb(file.size)}. Keep it under ${kb(max)}.`, true);
    let id = idFromName(file.name);
    if (["blink", "smile", "sleepy"].includes(id)) id = id + "-2";
    busy = true;
    say(`Uploading ${file.name}…`);
    try {
      const r = await fetch(`/v1/display/gif?id=${encodeURIComponent(id)}`, {
        method: "POST",
        headers: { "Content-Type": "image/gif" },
        body: file,
      });
      if (!r.ok) say((await r.text()) || "Upload failed.", true);
      else { apply(await r.json()); pick = id; say(`${file.name} added.`); }
    } catch {
      say("Upload failed. Is the robot still connected?", true);
    }
    busy = false;
  }
  async function remove(id) {
    if (confirmDel !== id) { confirmDel = id; return; }
    confirmDel = "";
    busy = true;
    try {
      const r = await fetch(`/v1/display/gif?id=${encodeURIComponent(id)}`, { method: "DELETE" });
      if (r.ok) { apply(await r.json()); if (pick === id) pick = anims[0]?.id || ""; say("Removed."); }
      else say((await r.text()) || "Could not remove it.", true);
    } catch { say("Could not remove it.", true); }
    busy = false;
  }
</script>

<div class="page">
  <div class="inner">
    <div class="top">
      <button type="button" class="round" onclick={onBack} aria-label="Back to settings">
        <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M15 6l-6 6 6 6"/></svg>
      </button>
    </div>
    <h1>Display</h1>

    <div class="preview-wrap">
      <div class="bezel">
        <div class="screen" class:anim={on && current}>
          {#if on && current}
            <img class="gif" src={`/v1/display/gif?id=${encodeURIComponent(current.id)}`} alt={`${current.name} playing on the robot`} />
          {:else}
            <span class="eye l"></span><span class="eye r"></span>
          {/if}
        </div>
        <span class="badge">{on ? "Animation" : "Eyes"}</span>
      </div>
      <p class="caption">What the robot's screen shows right now</p>
    </div>

    <section>
      <div class="group">
        <div class="row">
          <span class="ico dark">
            <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><rect x="3" y="5" width="18" height="14" rx="2"/><path d="M10 9.5l5 2.5-5 2.5z" fill="currentColor"/></svg>
          </span>
          <span class="txt">
            <span class="t">Play animation</span>
            <span class="d">{on ? "Eyes are paused while it plays" : "Off · the robot shows its eyes"}</span>
          </span>
          <button type="button" class="switch" class:on role="switch" aria-checked={on} aria-label="Play animation" disabled={busy || !st} onclick={toggle}>
            <span class="knob"></span>
          </button>
        </div>
      </div>
    </section>

    <section>
      <h2>Animations</h2>
      <div class="group" role="radiogroup" aria-label="Animation to play">
        {#each anims as a, i (a.id)}
          <div class="pickrow" class:sel={pick === a.id}>
            <button type="button" class="pick" role="radio" aria-checked={pick === a.id} disabled={busy} onclick={() => choose(a.id)}>
              <span class="thumb">
                {#if i <= thumbsUpTo}
                  <img src={`/v1/display/gif?id=${encodeURIComponent(a.id)}`} alt="" loading="lazy"
                       onload={() => (thumbsUpTo = Math.max(thumbsUpTo, i + 1))}
                       onerror={() => (thumbsUpTo = Math.max(thumbsUpTo, i + 1))} />
                {/if}
              </span>
              <span class="txt">
                <span class="t">{a.name}</span>
                <span class="d">{a.w} × {a.h} · {kb(a.bytes)}{a.builtin ? " · built in" : ""}</span>
              </span>
              <span class="radio" aria-hidden="true"><span></span></span>
            </button>
            {#if !a.builtin}
              <button type="button" class="del" class:confirm={confirmDel === a.id} disabled={busy}
                      aria-label={confirmDel === a.id ? `Confirm removing ${a.name}` : `Remove ${a.name}`}
                      onclick={() => remove(a.id)}>
                {#if confirmDel === a.id}Remove{:else}
                  <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M4 7h16M10 11v6M14 11v6M6 7l1 13h10l1-13M9 7V4h6v3"/></svg>
                {/if}
              </button>
            {/if}
          </div>
        {/each}
        <button type="button" class="pick add" disabled={busy} onclick={() => fileInput?.click()}>
          <span class="thumb dashed">
            <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" aria-hidden="true"><path d="M12 5v14M5 12h14"/></svg>
          </span>
          <span class="txt">
            <span class="t">Add a GIF</span>
            <span class="d">320 × 240 or smaller, under {kb(st?.max_bytes || 614400)}</span>
          </span>
        </button>
        <input bind:this={fileInput} class="sr" type="file" accept="image/gif,.gif" onchange={onFile} tabindex="-1" aria-hidden="true" />
      </div>
    </section>

    <section>
      <h2>When it ends</h2>
      <div class="seg" role="radiogroup" aria-label="When the animation ends">
        <button type="button" role="radio" aria-checked={loop} class:on={loop} disabled={busy} onclick={() => setLoop(true)}>Loop it</button>
        <button type="button" role="radio" aria-checked={!loop} class:on={!loop} disabled={busy} onclick={() => setLoop(false)}>Play once, then eyes</button>
      </div>
    </section>

    {#if msg}<p class="msg" class:bad={msgBad} role="status">{msg}</p>{/if}

    {#if st?.storage}
      <div class="store">
        <div class="store-row"><span>Robot storage</span><span class="mono">{mb(st.storage.used)} of {mb(st.storage.total)} MB</span></div>
        <div class="bar"><div style="width: {usedPct}%"></div></div>
      </div>
    {/if}
  </div>
</div>

<style>
  .page { position: absolute; inset: 0; overflow-y: auto; background: var(--page); }
  .inner { max-width: var(--page-max); margin: 0 auto; padding: calc(20px + var(--safe-top)) 16px calc(28px + var(--safe-bottom)); display: flex; flex-direction: column; gap: 18px; }
  .top { display: flex; }
  .round { width: 44px; height: 44px; border-radius: 22px; border: none; background: var(--ground); color: var(--ink); display: grid; place-items: center; cursor: pointer; }
  h1 { margin: 0 4px; font-size: 34px; font-weight: 800; letter-spacing: -0.02em; }
  section { display: flex; flex-direction: column; gap: 8px; }
  h2 { margin: 0 6px; font-size: 12px; font-weight: 800; letter-spacing: 0.12em; text-transform: uppercase; color: var(--sub); }

  .preview-wrap { display: flex; flex-direction: column; gap: 8px; }
  .bezel { position: relative; }
  .screen { position: relative; width: 100%; aspect-ratio: 4 / 3; border-radius: 20px; background: #000; overflow: hidden; }
  .screen.anim { background: transparent; }
  .gif { width: 100%; height: 100%; object-fit: contain; display: block; image-rendering: auto; }
  .eye { position: absolute; top: 26%; width: 18.4%; height: 47.5%; border-radius: 8px; background: #FFE631; animation: blink 4.2s infinite; transform-origin: center; }
  .eye.l { left: 16.5%; }
  .eye.r { right: 16.5%; }
  @keyframes blink { 0%, 92%, 100% { transform: scaleY(1); } 95% { transform: scaleY(0.06); } }
  .badge { position: absolute; right: 10px; top: 10px; height: 24px; padding: 0 10px; border-radius: 12px; background: rgba(0,0,0,0.55); color: #fff; font-size: 11px; font-weight: 700; display: flex; align-items: center; }
  .caption { margin: 0 6px; font-size: 13px; color: var(--sub); text-align: center; }

  .group { background: #fff; border: 1px solid var(--line); border-radius: 20px; overflow: hidden; }
  .group > * + * { border-top: 1px solid var(--hair-2); }
  .row { display: flex; align-items: center; gap: 14px; padding: 14px 16px; }
  .ico { width: 38px; height: 38px; border-radius: 11px; background: var(--mpx-yellow); color: var(--ink); display: grid; place-items: center; flex-shrink: 0; }
  .ico.dark { background: var(--ink); color: var(--mpx-yellow); }
  .txt { flex: 1; min-width: 0; display: flex; flex-direction: column; gap: 2px; text-align: left; }
  .t { font-size: 16px; font-weight: 700; color: var(--ink); }
  .d { font-size: 13px; color: var(--sub); overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }

  .switch { width: 52px; height: 32px; border-radius: 16px; border: none; background: #E6E2D8; padding: 3px; display: flex; justify-content: flex-start; cursor: pointer; flex-shrink: 0; transition: background var(--dur-fast); }
  .switch.on { background: var(--ink); justify-content: flex-end; }
  .knob { width: 26px; height: 26px; border-radius: 13px; background: #fff; box-shadow: 0 1px 3px rgba(0,0,0,0.2); }
  .switch.on .knob { background: var(--mpx-yellow); }
  .switch:disabled { opacity: 0.6; }

  .pickrow { display: flex; align-items: center; }
  .pickrow.sel { background: #FFFBDA; }
  .pick { flex: 1; min-width: 0; border: none; background: transparent; font: inherit; color: var(--ink); display: flex; align-items: center; gap: 12px; padding: 10px 16px; cursor: pointer; }
  .pick.add { width: 100%; padding: 12px 16px; }
  .thumb { width: 64px; height: 48px; border-radius: 10px; background: #000; overflow: hidden; flex-shrink: 0; display: grid; place-items: center; }
  .thumb img { width: 100%; height: 100%; object-fit: cover; }
  .thumb.dashed { background: transparent; border: 2px dashed #D9D5CB; box-sizing: border-box; color: var(--ink); }
  .radio { width: 22px; height: 22px; border-radius: 11px; border: 2px solid #C9C5BB; box-sizing: border-box; display: grid; place-items: center; flex-shrink: 0; }
  .radio span { width: 10px; height: 10px; border-radius: 5px; }
  .pickrow.sel .radio { border-color: var(--ink); }
  .pickrow.sel .radio span { background: var(--ink); }
  .del { height: 40px; min-width: 40px; margin-right: 10px; padding: 0 10px; border: none; border-radius: 20px; background: transparent; color: #A1311F; font: inherit; font-size: 13px; font-weight: 700; display: grid; place-items: center; cursor: pointer; }
  .del.confirm { background: #A1311F; color: #fff; }

  .seg { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 4px; background: var(--hair); border-radius: 14px; padding: 4px; }
  .seg button { height: 40px; border: none; border-radius: 10px; background: transparent; color: #4F4C46; font-size: 13px; font-weight: 700; cursor: pointer; }
  .seg button.on { background: #fff; color: var(--ink); box-shadow: 0 1px 3px rgba(0, 0, 0, 0.12); }
  .seg button:disabled, .pick:disabled { opacity: 0.6; }

  .msg { margin: 0 6px; font-size: 13px; color: var(--sub); }
  .msg.bad { color: #A1311F; font-weight: 600; }
  .store { display: flex; flex-direction: column; gap: 6px; padding: 0 6px; }
  .store-row { display: flex; justify-content: space-between; font-size: 12px; color: var(--sub); }
  .mono { font-family: var(--mono); }
  .bar { height: 6px; border-radius: 3px; background: var(--ground); overflow: hidden; }
  .bar div { height: 6px; background: var(--ink); }

  button:focus-visible { outline: 3px solid var(--ink); outline-offset: 2px; }
  .sr { position: absolute; width: 1px; height: 1px; overflow: hidden; clip: rect(0 0 0 0); white-space: nowrap; }
</style>
