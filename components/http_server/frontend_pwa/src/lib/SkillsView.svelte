<script>
  /**
   * SkillsView — the robot's skills, as skills.
   *
   * Anything pushed with `mpx-cli deploy` used to surface only as a row in the
   * raw LittleFS file browser: an emoji, a filename, a byte count. You could
   * not tell a skill from a config blob, "Run" popped a browser alert that said
   * "started" whether the module worked or trapped on its first instruction,
   * and marketplace skills lived on a completely separate screen buried in
   * Settings.
   *
   * This screen is the skill list, with the file browser demoted to a segment
   * you can switch to when you actually want to poke at the filesystem.
   *
   * Three sources are merged into one list:
   *   - GET /v1/skills/list   — .wasm / .mpxe at the LittleFS root (the CLI)
   *   - GET /v1/lua/list      — .lua scripts under /lua
   *   - GET /v1/marketplace/robot/skills — skills bought from the marketplace
   *
   * The marketplace call needs the robot's uplink, so it is allowed to fail
   * quietly: no uplink simply means that section is not rendered. A skill you
   * uploaded yourself must never be hidden because the internet is down.
   */
  import {
    colors, skillTypeColor, skillTypeLabel, skillTypeBlurb,
    capabilityLabel, capabilityIsSensitive,
  } from "./colors.js";
  import StoreView from "./StoreView.svelte";
  import FileViewer from "./FileViewer.svelte";
  import {
    listRobotSkills,
    toggleSkill,
    deploySkill,
    enqueueLua,
  } from "./marketplaceApi.js";

  let { onNavigate } = $props();

  const YELLOW = colors.mpx.primary;

  /**
   * Last-known data, kept outside the component so it survives navigating away
   * and back. Coming back to this screen used to blank the list and re-run
   * every request; now the previous answer is on screen in the same frame and
   * is quietly replaced when the fresh one lands.
   *
   * Deliberately module-level and NOT localStorage: it is a render cache for
   * this browsing session, not a record of anything. The robot owns what is
   * installed on it — that mistake was made once already and is not repeated
   * here.
   */
  const cache = { wasm: null, lua: null, fs: null, installed: null, mkt: null };
  const hydrated = cache.wasm !== null;

  /* ── Which tab is showing ────────────────────────────────────
   *
   * Three, and the split is by WHAT YOU DO THERE rather than by what the
   * thing technically is:
   *
   *   skills  what is on the robot and can be run right now
   *   store   what you own from the marketplace, and whether it is downloaded
   *   files   the raw filesystem, for when you need to see the actual bytes
   *
   * The same skill legitimately appears in two of them — owned in Store,
   * present in Skills — and that is the point, not a duplication: those
   * answer different questions ("did I buy it?" and "can I run it?"), and
   * conflating them is what made a fresh download look like a stray file.
   * Every tab carries one line saying what it holds, so nobody has to learn
   * the rule from watching where things land.
   */
  let segment = $state("skills");   // "skills" | "store" | "library" | "files"

  const TAB_BLURB = {
    skills:  "Everything installed on this robot. Tap Run to try one.",
    store:   "Browse the marketplace and install new skills.",
    library: "Skills you own. AI and Web skills run in the cloud; Motion skills download to the robot.",
    files:   "The robot's raw storage. You rarely need this.",
  };

  // ── Local (on-robot) skills ─────────────────────────────────
  let wasmSkills = $state(cache.wasm ?? []);   // [{ name, size }]
  let luaSkills = $state(cache.lua ?? []);     // [{ name }]
  let fsInfo = $state(cache.fs ?? { total: 0, used: 0 });
  let loading = $state(!hydrated);
  let loadError = $state("");

  // ── Marketplace skills ──────────────────────────────────────
  let mktSkills = $state(cache.mkt ?? []);

  /**
   * Store filter: "all" | "capability" | "awa" | "wasm".
   *
   * Added because the list is now long enough that finding your own skill
   * meant scrolling past a dozen others. Filtering by what a skill DOES is
   * the only sort an owner can reason about -- they do not know or care that
   * "AWA" means Playwright.
   */
  let typeFilter = $state("all");
  let mktAvailable = $state(false);
  let mktLoading = $state(false);
  let actionInFlight = $state(null);

  // What the ROBOT records as installed. This used to be a localStorage map
  // built by regex-scraping Lua deploy scripts for file_write("...") calls,
  // which meant: install on your phone and your laptop offered to install
  // again; uninstall from a second device deleted nothing while reporting
  // success; and clearing site data orphaned every installed file with no way
  // to find it. The robot owns its filesystem, so the robot owns the record.
  let installed = $state(cache.installed ?? []);  // [{ skill_id, file, version, title }]

  // ── Run state ───────────────────────────────────────────────
  let running = $state(null);       // { name, elapsedMs } while a skill runs
  let lastRun = $state(null);       // { name, ok, message, durationMs }
  let runError = $state("");
  let statusTimer = null;

  // ── Confirmations (no window.confirm: it blocks the PWA shell) ──
  let confirmDelete = $state(null); // { kind, name, label }

  // ── Helpers ─────────────────────────────────────────────────
  function fmtSize(b) {
    if (b === undefined || b === null) return "";
    return b < 1024 ? b + " B" : (b / 1024).toFixed(1) + " KB";
  }

  /** walk_with_gains.wasm → "Walk With Gains" */
  function prettyName(filename) {
    return filename
      .replace(/\.(wasm|mpxe|lua)$/i, "")
      .replace(/[_-]+/g, " ")
      .replace(/\s+/g, " ")
      .trim()
      .replace(/\b\w/g, (c) => c.toUpperCase()) || filename;
  }

  function initialOf(name) {
    const p = prettyName(name);
    return (p.charAt(0) || "?").toUpperCase();
  }

  // ── Fetch ───────────────────────────────────────────────────
  /**
   * A request that gives up instead of hanging.
   *
   * The robot's httpd answers every request from a *single* task, so one slow
   * handler does not merely delay itself — it delays everything queued behind
   * it. A fetch with no deadline turns one wedged endpoint into a wedged
   * screen, which is exactly how this view came to sit on a spinner.
   */
  async function get(path, ms) {
    const ctl = new AbortController();
    const timer = setTimeout(() => ctl.abort(), ms);
    try {
      return await fetch(path, { signal: ctl.signal });
    } finally {
      clearTimeout(timer);
    }
  }

  /**
   * The critical path: the two lists this screen exists to show, and nothing
   * else. Storage figures, provenance badges and the marketplace all used to
   * be awaited before the first skill could be drawn — so the slowest of five
   * requests, one of which leaves the robot entirely, decided how long you
   * stared at "Loading…".
   */
  async function fetchSkillList() {
    loadError = "";
    try {
      const [sr, lr] = await Promise.all([
        get("/v1/skills/list", 8000),
        get("/v1/lua/list", 8000).catch(() => null),
      ]);

      if (!sr || !sr.ok) throw new Error("robot unreachable");
      wasmSkills = (await sr.json()).map(
        (s) => ({ ...s, name: String(s.name || "").replace(/^\/+/, "") }));

      if (lr && lr.ok) {
        const d = await lr.json();
        // /v1/lua/list answers with a bare array of names.
        const names = Array.isArray(d) ? d : (d.files || d.f || []);
        luaSkills = names
          .map((n) => (typeof n === "string" ? { name: n } : n))
          /* Normalise the leading slash before anything reads the name.
             The firmware used to return "/foo.lua" here and "foo.lua" from
             /v1/skills/list, and every consumer downstream — the _deploy_
             filter, the delete path, the label — quietly assumed the second
             form. Fixed in the firmware too, but stripping it here as well
             means this screen behaves on a robot that has not been reflashed
             yet, which is most of them at any given moment. */
          .map((s) => ({ ...s, name: String(s.name || "").replace(/^\/+/, "") }))
          // The deploy pipeline writes throwaway /lua/_deploy_N.lua files;
          // they are machinery, not something anyone wants to run by hand.
          .filter((s) => s.name && !s.name.startsWith("_deploy_"));
      } else {
        luaSkills = [];
      }
      cache.wasm = wasmSkills;
      cache.lua = luaSkills;
    } catch (e) {
      loadError = e.name === "AbortError"
        ? "The robot did not answer in time"
        : (e.message || "Could not reach the robot");
      wasmSkills = [];
      luaSkills = [];
    }
    loading = false;
  }

  /* Decoration. Costs a full-partition traversal on the robot when its cache
     is cold, so it must never gate the list. */
  async function fetchFsInfo() {
    try {
      const r = await get("/v1/fs/info", 8000);
      if (r.ok) { fsInfo = await r.json(); cache.fs = fsInfo; }
    } catch { /* the storage bar simply does not appear */ }
  }

  async function fetchMarketplace() {
    mktLoading = true;
    try {
      const list = await listRobotSkills();
      mktSkills = Array.isArray(list) ? list : [];
      mktAvailable = true;
      cache.mkt = mktSkills;
    } catch {
      // No uplink, or not signed in. Not an error worth showing here.
      mktSkills = [];
      mktAvailable = false;
    } finally {
      mktLoading = false;
    }
  }

  async function fetchInstalled() {
    try {
      const r = await get("/v1/skills/installed", 8000);
      if (r.ok) {
        const d = await r.json();
        installed = Array.isArray(d.skills) ? d.skills : [];
        cache.installed = installed;
      }
    } catch {
      // Older firmware has no such endpoint. An empty list simply means the
      // provenance badges do not show; the skill list itself is unaffected.
      installed = [];
    }
  }

  /**
   * Paint the list, then fill in the rest around it.
   *
   * The three background calls are deliberately not awaited: each updates its
   * own corner of the screen when it lands, and any of them can fail or time
   * out without the list noticing. They are also fired in the order the eye
   * reaches them.
   */
  async function refresh() {
    loading = !hydrated;    // a warm cache is already on screen; do not blank it
    await fetchSkillList();

    // Chained, not fired together. The robot has 16 LWIP sockets in total and
    // its web server holds at most five; a browser will happily open a fresh
    // connection per parallel request, and every one it closes then sits in
    // TIME_WAIT holding a slot. Three at once is three sockets for no gain —
    // the server answers them one at a time regardless, because it serves
    // every request from a single task.
    fetchInstalled()
      .then(fetchFsInfo)
      .then(fetchMarketplace);
  }

  $effect(() => {
    refresh();
    pollStatus();
    return () => { if (statusTimer) clearTimeout(statusTimer); };
  });

  // ── Run + status polling ────────────────────────────────────
  /**
   * POST /v1/skills/run returns the moment the task is spawned — the outcome
   * lands up to 60 s later. So the result comes from polling
   * GET /v1/skills/status, which reports the running skill and the last one's
   * exit code. Polling is fast while something runs and idles off afterwards,
   * so a parked screen is not hammering an ESP32 once a second forever.
   */
  async function pollStatus() {
    if (statusTimer) { clearTimeout(statusTimer); statusTimer = null; }

    try {
      const r = await fetch("/v1/skills/status");
      if (r.ok) {
        const d = await r.json();
        running = d.running
          ? { name: d.name || "", elapsedMs: d.elapsed_ms || 0 }
          : null;
        lastRun = d.last
          ? {
              name: d.last.name,
              ok: !!d.last.ok,
              message: d.last.message || (d.last.ok ? "ok" : "failed"),
              durationMs: d.last.duration_ms || 0,
            }
          : null;
      }
    } catch { /* robot busy or offline — try again on the next tick */ }

    statusTimer = setTimeout(pollStatus, running ? 500 : 4000);
  }

  async function runSkill(name) {
    runError = "";
    try {
      const r = await fetch("/v1/skills/run", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ skill: name }),
      });

      if (r.status === 409) {
        runError = "Another skill is already running — only one at a time.";
        return;
      }
      if (!r.ok) {
        runError = `Could not start ${name} (HTTP ${r.status})`;
        return;
      }

      // Show the running state immediately rather than waiting up to half a
      // second for the next poll to confirm what we already know.
      running = { name, elapsedMs: 0 };
      lastRun = null;
      pollStatus();
    } catch (e) {
      runError = e.message || "Could not reach the robot";
    }
  }

  async function runLua(name) {
    runError = "";
    try {
      const r = await fetch("/v1/lua/run", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ path: "/lua/" + name }),
      });
      const d = await r.json();
      lastRun = {
        name,
        ok: !!d.ok,
        message: d.ok ? (d.output || "ok") : (d.error || "failed"),
        durationMs: 0,
      };
    } catch (e) {
      runError = e.message || "Could not reach the robot";
    }
  }

  // ── Delete ──────────────────────────────────────────────────
  function askDelete(kind, name) {
    confirmDelete = { kind, name, label: prettyName(name) };
  }

  async function doDelete() {
    if (!confirmDelete) return;
    const { kind, name } = confirmDelete;
    confirmDelete = null;
    actionInFlight = "del:" + name;

    try {
      const path = kind === "lua" ? "/lua/" + name : "/" + name;
      const r = await fetch("/v1/fs/delete", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ path }),
      });
      if (!r.ok) runError = `Delete failed (HTTP ${r.status})`;
      /* Was fetchLocal(), which does not exist — so every delete ended in
         "fetchLocal is not defined" and the list never refreshed, whether or
         not the file actually went. refresh() is the right call anyway: a
         delete changes the storage bar and the provenance badges too. */
      await refresh();
    } catch (e) {
      runError = e.message || "Delete failed";
    }
    actionInFlight = null;
  }

  // ── Marketplace actions (carried over unchanged) ────────────
  function isWasmSkill(skill) {
    return (skill.skill_type || "").toLowerCase() === "wasm";
  }

  function isDeployed(skill) {
    return installed.some((e) => e.skill_id === skill.skill_id);
  }

  async function handleToggle(skillId, enabled) {
    actionInFlight = skillId;
    try {
      await toggleSkill(skillId, enabled);
      await fetchMarketplace();
    } catch (e) {
      runError = e.message || "Toggle failed";
    }
    actionInFlight = null;
  }

  /* Which file did this deploy actually put on the robot?
   *
   * The gateway writes the skill through a Lua script it generates, so the
   * app never sees the filename and the robot never learns which marketplace
   * skill the file belongs to. That missing link is why a download showed up
   * as "gaits.mpxe" instead of its title, why the store never marked it as
   * owned, and why Refund deleted nothing — uninstall looks the filename up
   * in a record that was never written.
   *
   * Rather than parsing the gateway's script or asking it to change, compare
   * the robot's own file list before and after. Whatever appeared is the
   * skill. It needs no cooperation from anyone and cannot drift out of sync
   * with what is actually on flash. */
  async function currentSkillFiles() {
    try {
      const r = await get("/v1/skills/list", 8000);
      if (!r.ok) return null;
      const list = await r.json();
      return new Set(list.map((s) => String(s.name || "").replace(/^\/+/, "")));
    } catch { return null; }
  }

  async function handleDeploy(skill) {
    actionInFlight = "deploy:" + skill.skill_id;
    try {
      const before = await currentSkillFiles();

      const data = await deploySkill(skill.skill_id);
      for (const s of data.skills || []) {
        for (const cmd of s.commands || []) {
          await enqueueLua(cmd.script);
        }
      }

      /* The scripts are queued, not finished — the robot runs them on its own
         worker and one of them waits for you to approve the write. Poll for
         the new file rather than guessing at a delay. */
      let placed = null;
      for (let i = 0; i < 30 && !placed; i++) {
        await new Promise((r) => setTimeout(r, 1000));
        const after = await currentSkillFiles();
        if (!after || !before) break;
        for (const name of after) if (!before.has(name)) { placed = name; break; }
      }

      if (placed) {
        await fetch("/v1/skills/record", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({
            skill_id: skill.skill_id,
            file: placed,
            title: skill.title || skill.name || skill.skill_id,
            version: skill.version || "",
          }),
        }).catch(() => { /* the skill still runs; only the label is lost */ });
      } else {
        runError = "Downloaded, but no new skill file appeared — the write "
                 + "may not have been approved on this device.";
      }

      // No client-side bookkeeping beyond that: ask the robot what it now has.
      await refresh();
    } catch (e) {
      runError = e.message || "Download failed";
    }
    actionInFlight = null;
  }

  async function handleRemove(skill) {
    actionInFlight = "delete:" + skill.skill_id;
    try {
      // One call deletes the file and clears the record together. Doing them
      // separately is how a refunded skill stayed runnable, and how a deleted
      // file left a phantom install behind.
      const r = await fetch("/v1/skills/uninstall", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ skill_id: skill.skill_id }),
      });
      if (!r.ok) runError = `Uninstall failed (HTTP ${r.status})`;
      await refresh();
    } catch (e) {
      runError = e.message || "Uninstall failed";
    }
    actionInFlight = null;
  }

  // ── Derived ─────────────────────────────────────────────────
  let localCount = $derived(wasmSkills.length + luaSkills.length);
  let storeCount = $derived(mktSkills.length);

  /** Normalised type for a skill, lowercased, with a safe fallback. */
  function typeOf(skill) {
    return (skill?.skill_type || "").toLowerCase() || "awa";
  }

  /** How many of each type the owner has, for the filter chips. */
  let typeCounts = $derived.by(() => {
    const counts = { all: mktSkills.length, capability: 0, awa: 0, wasm: 0 };
    for (const s2 of mktSkills) {
      const t = typeOf(s2);
      if (counts[t] !== undefined) counts[t]++;
    }
    return counts;
  });

  /** Display order and headings. AI first: newest, and what people look for. */
  const TYPE_ORDER = ["capability", "awa", "wasm"];

  /**
   * The store list as GROUPS, not one flat list.
   *
   * Sorting alone was not enough — with ten skills and no visual break, a
   * sorted list looks identical to an unsorted one and you still scroll
   * hunting for your own skill. Headings make the structure visible, which
   * was the whole point of sorting in the first place.
   *
   * When a single type is filtered the heading is redundant, so the group
   * renders without one.
   */
  let mktGroups = $derived.by(() => {
    const groups = [];
    for (const type of TYPE_ORDER) {
      if (typeFilter !== "all" && typeFilter !== type) continue;
      const items = mktSkills
        .filter((s2) => typeOf(s2) === type)
        .slice()
        .sort((a, b) => (a.title || "").localeCompare(b.title || ""));
      if (items.length) groups.push({ type, items });
    }
    return groups;
  });

  /** Total shown, for the empty state. */
  let visibleCount = $derived(mktGroups.reduce((n, g) => n + g.items.length, 0));

  /** Capabilities a skill declared, or [] for legacy skills that declared none. */
  function requiresOf(skill) {
    const raw = Array.isArray(skill?.requires) ? skill.requires : [];
    // "sense:activity" and "sense:orientation" are one permission to an owner.
    return [...new Set(raw.map((r) => String(r).split(":")[0]))];
  }

  /** True when the robot cannot provide something this skill needs. */
  function isBlocked(skill) {
    return Array.isArray(skill?.missing_capabilities) && skill.missing_capabilities.length > 0;
  }

  /**
   * Cloud-run skills are enabled with a toggle; on-robot skills are
   * downloaded. Capability and AWA skills both execute in the worker, so
   * neither has a file to push to the robot.
   */
  function isCloudSkill(skill) {
    const t = typeOf(skill);
    return t === "capability" || t === "awa";
  }

  /* The install record for a file, if the robot has one. This is what turns
     "gaits.mpxe" into "01 · Layer 1" and puts a Store badge on it — and it is
     exactly what was missing, because nothing wrote installed.json for a
     marketplace download until now. */
  function recordFor(filename) {
    return installed.find((e) => e.file === filename) || null;
  }
  function displayTitle(filename) {
    return recordFor(filename)?.title || prettyName(filename);
  }
  let usedPct = $derived(
    fsInfo.total ? Math.min(100, (fsInfo.used / fsInfo.total) * 100) : 0
  );
</script>

<div class="sv-root" style="--yellow: {YELLOW}">
  <!-- ═══ Header ═══ -->
  <header class="sv-header">
    <div class="sv-header-row">
      <h2 class="sv-title">Skills</h2>
      <button class="sv-icon-btn" onclick={refresh} aria-label="Refresh">
        <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
          <polyline points="23 4 23 10 17 10"/>
          <path d="M20.49 15a9 9 0 1 1-2.12-9.36L23 10"/>
        </svg>
      </button>
    </div>

    <div class="sv-segmented" role="tablist">
      <button
        class="sv-seg" class:sv-seg-on={segment === "skills"}
        role="tab" aria-selected={segment === "skills"}
        onclick={() => (segment = "skills")}
      >
        On robot{#if localCount}<span class="sv-seg-count">{localCount}</span>{/if}
      </button>
      <button
        class="sv-seg" class:sv-seg-on={segment === "store"}
        role="tab" aria-selected={segment === "store"}
        onclick={() => (segment = "store")}
      >
        Store
      </button>
      <button
        class="sv-seg" class:sv-seg-on={segment === "library"}
        role="tab" aria-selected={segment === "library"}
        onclick={() => (segment = "library")}
      >
        Library{#if storeCount}<span class="sv-seg-count">{storeCount}</span>{/if}
      </button>
      <button
        class="sv-seg" class:sv-seg-on={segment === "files"}
        role="tab" aria-selected={segment === "files"}
        onclick={() => (segment = "files")}
      >
        Files
      </button>
    </div>

    <!-- One line per tab. Cheap, and it removes the guesswork about which of
         the three a given thing lives in. -->
    <p class="sv-blurb">{TAB_BLURB[segment]}</p>
  </header>

  {#if segment === "files"}
    <!-- The raw filesystem browser, unchanged — one level down instead of
         being the only way to see a skill. -->
    <div class="sv-files-host">
      <FileViewer {onNavigate} embedded={true} />
    </div>
  {:else if segment === "skills"}
    <div class="sv-body">
      <!-- ═══ Running / last-result banner ═══ -->
      {#if running}
        <div class="sv-banner sv-banner-run">
          <span class="sv-spinner" aria-hidden="true"></span>
          <div class="sv-banner-text">
            <strong>{prettyName(running.name)}</strong>
            <span>running… {(running.elapsedMs / 1000).toFixed(1)}s</span>
          </div>
        </div>
      {:else if lastRun}
        <div class="sv-banner" class:sv-banner-ok={lastRun.ok} class:sv-banner-bad={!lastRun.ok}>
          <span class="sv-banner-mark">{lastRun.ok ? "✓" : "!"}</span>
          <div class="sv-banner-text">
            <strong>{prettyName(lastRun.name)}</strong>
            <!-- The separator is written as an expression because Svelte
                 trims the literal whitespace that precedes a block. -->
            <span>
              {lastRun.message}{#if lastRun.durationMs}{" · " + (lastRun.durationMs / 1000).toFixed(1) + "s"}{/if}
            </span>
          </div>
          <button class="sv-banner-x" onclick={() => (lastRun = null)} aria-label="Dismiss">✕</button>
        </div>
      {/if}

      {#if runError}
        <div class="sv-banner sv-banner-bad">
          <span class="sv-banner-mark">!</span>
          <div class="sv-banner-text"><span>{runError}</span></div>
          <button class="sv-banner-x" onclick={() => (runError = "")} aria-label="Dismiss">✕</button>
        </div>
      {/if}

      <!-- ═══ Storage ═══ -->
      {#if fsInfo.total}
        <div class="sv-storage">
          <div class="sv-storage-row">
            <span>Storage</span>
            <span>{fmtSize(fsInfo.used)} / {fmtSize(fsInfo.total)}</span>
          </div>
          <div class="sv-storage-track">
            <div class="sv-storage-fill" class:sv-storage-hot={usedPct > 85} style="width:{usedPct}%"></div>
          </div>
        </div>
      {/if}

      <!-- ═══ On this robot ═══ -->
      <section class="sv-section">
        <h3 class="sv-section-title">Ready to run</h3>

        {#if loading}
          <p class="sv-muted">Loading…</p>
        {:else if loadError}
          <div class="sv-empty">
            <p class="sv-empty-title">Robot unreachable</p>
            <p class="sv-empty-desc">{loadError}</p>
            <button class="sv-btn sv-btn-primary" onclick={refresh}>Retry</button>
          </div>
        {:else if localCount === 0}
          <div class="sv-empty">
            <p class="sv-empty-title">Nothing installed yet</p>
            <p class="sv-empty-desc">
              Two ways to fill this up. Open <strong>Store</strong> and download
              something you own — or build your own with the SDK and push it in
              one command:
            </p>
            <code class="sv-code">mpx-cli deploy</code>
            <p class="sv-empty-desc sv-empty-or">The ＋ button uploads a .wasm by hand.</p>
          </div>
        {:else}
          <div class="sv-list">
            {#each wasmSkills as s (s.name)}
              <article class="sv-card" class:sv-card-live={running?.name === s.name}>
                <div class="sv-card-top">
                  <div class="sv-avatar" style="--badge: {skillTypeColor('wasm')}">{initialOf(s.name)}</div>
                  <div class="sv-card-id">
                    <div class="sv-card-title-row">
                      <span class="sv-card-title">{displayTitle(s.name)}</span>
                      <span class="sv-chip" style="--badge: {skillTypeColor('wasm')}">
                        {skillTypeLabel("wasm")}
                      </span>
                      {#if recordFor(s.name)}
                        <span class="sv-chip sv-chip-store">Store</span>
                      {/if}
                    </div>
                    <span class="sv-card-meta">
                      {s.name} · {fmtSize(s.size)}{#if recordFor(s.name)?.version} · v{recordFor(s.name).version}{/if}
                    </span>
                  </div>
                </div>
                <div class="sv-card-actions">
                  <button
                    class="sv-btn sv-btn-primary"
                    disabled={!!running}
                    onclick={() => runSkill(s.name)}
                  >
                    {running?.name === s.name ? "Running…" : "Run"}
                  </button>
                  <button
                    class="sv-btn sv-btn-ghost"
                    disabled={actionInFlight === "del:" + s.name}
                    onclick={() => askDelete("wasm", s.name)}
                  >
                    Delete
                  </button>
                </div>
              </article>
            {/each}

            {#each luaSkills as s (s.name)}
              <article class="sv-card">
                <div class="sv-card-top">
                  <div class="sv-avatar" style="--badge: {skillTypeColor('awa')}">{initialOf(s.name)}</div>
                  <div class="sv-card-id">
                    <div class="sv-card-title-row">
                      <span class="sv-card-title">{prettyName(s.name)}</span>
                      <span class="sv-chip" style="--badge: {skillTypeColor('awa')}">
                        {skillTypeLabel("awa")}
                      </span>
                    </div>
                    <span class="sv-card-meta">{s.name}</span>
                  </div>
                </div>
                <div class="sv-card-actions">
                  <button class="sv-btn sv-btn-primary" onclick={() => runLua(s.name)}>Run</button>
                  <button
                    class="sv-btn sv-btn-ghost"
                    disabled={actionInFlight === "del:" + s.name}
                    onclick={() => askDelete("lua", s.name)}
                  >
                    Delete
                  </button>
                </div>
              </article>
            {/each}
          </div>
        {/if}
      </section>

      <div class="sv-spacer"></div>
    </div>

    <!-- ═══ Upload FAB ═══
         Belongs to this tab only: it uploads a skill you built, which is a
         thing you do TO the robot. There is nothing to upload in the Store. -->
    <button class="sv-fab" onclick={() => onNavigate && onNavigate("upload")} aria-label="Upload a skill">
      <svg width="26" height="26" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
        <line x1="12" y1="5" x2="12" y2="19"/>
        <line x1="5" y1="12" x2="19" y2="12"/>
      </svg>
    </button>

  {:else if segment === "store"}
    <!-- The storefront replaces the old owned-skills list here. Owning and
         browsing are different questions: "what can I get" needs the whole
         catalogue, "what do I have" needs toggles. The owned list now lives
         under the Library segment below. -->
    <StoreView />

  {:else if segment === "library"}
    <!-- ═══ Store ═══════════════════════════════════════════════
         What you OWN, which is a different question from what is on the
         robot. A skill is listed here the moment you subscribe; the button
         says whether it has made the trip to the robot yet. Keeping this
         apart from the run list is the whole reason for the third tab: a
         download used to appear in one flat list with no title and no way to
         tell it apart from a file someone dropped there. -->
    <div class="sv-body">
      {#if mktLoading && !mktSkills.length}
        <section class="sv-section">
          <p class="sv-mkt-wait">Checking your marketplace skills…</p>
        </section>
      {:else if !mktAvailable}
        <div class="sv-empty">
          <p class="sv-empty-title">Store unavailable</p>
          <p class="sv-empty-desc">
            The robot could not reach the marketplace. It needs to be on your
            Wi-Fi, not just its own hotspot.
          </p>
          <button class="sv-btn sv-btn-primary" onclick={refresh}>Try again</button>
        </div>
      {:else if mktSkills.length === 0}
        <div class="sv-empty">
          <p class="sv-empty-title">You do not own any skills yet</p>
          <p class="sv-empty-desc">
            Browse and subscribe in <strong>Settings → Marketplace</strong>.
            Anything you take will show up here, ready to download.
          </p>
        </div>
      {:else}
        <!-- Filter by what a skill DOES. Owners cannot reason about "AWA"
             vs "CAPABILITY", but they can reason about AI / Web / Motion. -->
        <div class="sv-filters" role="tablist" aria-label="Filter skills by type">
          {#each [["all", "All"], ["capability", "AI"], ["awa", "Web"], ["wasm", "Motion"]] as [key, label]}
            {#if key === "all" || typeCounts[key] > 0}
              <button
                class="sv-filter"
                class:sv-filter-on={typeFilter === key}
                style="--badge: {key === 'all' ? '#6f6f6f' : skillTypeColor(key)}"
                role="tab"
                aria-selected={typeFilter === key}
                onclick={() => (typeFilter = key)}
              >
                {#if key !== "all"}<span class="sv-filter-dot"></span>{/if}
                {label}
                <span class="sv-filter-count">{typeCounts[key]}</span>
              </button>
            {/if}
          {/each}
        </div>

        {#if typeFilter !== "all" && skillTypeBlurb(typeFilter)}
          <p class="sv-filter-blurb">{skillTypeBlurb(typeFilter)}</p>
        {/if}

        {#if visibleCount === 0}
          <p class="sv-mkt-wait">No {skillTypeLabel(typeFilter)} skills yet.</p>
        {/if}

        {#each mktGroups as group (group.type)}
        <section class="sv-section">
          <!-- A coloured rule keyed to the type, so the eye can find the
               section without reading the words. -->
          <div class="sv-group-head" style="--badge: {skillTypeColor(group.type)}">
            <span class="sv-group-bar"></span>
            <h3 class="sv-section-title sv-group-title">
              {skillTypeLabel(group.type)} skills
            </h3>
            <span class="sv-group-count">{group.items.length}</span>
          </div>

          <div class="sv-list">
            {#each group.items as skill (skill.skill_id)}
              <article class="sv-card" class:sv-card-blocked={isBlocked(skill)}>
                <div class="sv-card-top">
                  <div class="sv-avatar" style="--badge: {skillTypeColor(skill.skill_type)}">
                    {(skill.title || "?").charAt(0).toUpperCase()}
                  </div>
                  <div class="sv-card-id">
                    <div class="sv-card-title-row">
                      <span class="sv-card-title">{skill.title || "Unknown skill"}</span>
                      <span class="sv-chip" style="--badge: {skillTypeColor(skill.skill_type)}">
                        {skillTypeLabel(skill.skill_type)}
                      </span>
                    </div>
                    <span class="sv-card-meta">
                      v{skill.current_version || "1.0"} ·
                      {#if isCloudSkill(skill)}
                        {skill.enabled ? "on" : "off"}
                      {:else}
                        {isDeployed(skill) ? "on this robot" : "not downloaded yet"}
                      {/if}
                    </span>
                  </div>
                </div>

                <!-- What this skill is allowed to touch.
                     An owner enabling a stranger's skill deserves to see that
                     it can move their robot before they flip the switch, not
                     after. Legacy skills declare nothing, so nothing shows. -->
                {#if requiresOf(skill).length}
                  <div class="sv-perms">
                    {#each requiresOf(skill) as cap}
                      <span class="sv-perm" class:sv-perm-warn={capabilityIsSensitive(cap)}>
                        {capabilityLabel(cap)}
                      </span>
                    {/each}
                  </div>
                {/if}

                {#if isBlocked(skill)}
                  <p class="sv-blocked-note">
                    Needs {skill.missing_capabilities.map(capabilityLabel).join(", ").toLowerCase()} —
                    this robot does not have that yet.
                  </p>
                {/if}

                <div class="sv-card-actions">
                  {#if isWasmSkill(skill)}
                    {#if isDeployed(skill)}
                      <button
                        class="sv-btn sv-btn-ghost"
                        disabled={actionInFlight === "delete:" + skill.skill_id}
                        onclick={() => handleRemove(skill)}
                      >
                        {actionInFlight === "delete:" + skill.skill_id ? "…" : "Remove from robot"}
                      </button>
                    {:else}
                      <button
                        class="sv-btn sv-btn-primary"
                        disabled={actionInFlight === "deploy:" + skill.skill_id}
                        onclick={() => handleDeploy(skill)}
                      >
                        {actionInFlight === "deploy:" + skill.skill_id ? "…" : "Download to robot"}
                      </button>
                    {/if}
                  {:else}
                    <label
                      class="sv-toggle"
                      class:sv-toggle-busy={actionInFlight === skill.skill_id}
                      class:sv-toggle-blocked={isBlocked(skill)}
                    >
                      <input
                        type="checkbox"
                        checked={skill.enabled && !isBlocked(skill)}
                        disabled={actionInFlight === skill.skill_id || isBlocked(skill)}
                        onchange={() => handleToggle(skill.skill_id, !skill.enabled)}
                      />
                      <span class="sv-toggle-track"></span>
                      <span class="sv-toggle-thumb"></span>
                    </label>
                  {/if}
                </div>
              </article>
            {/each}
          </div>
        </section>
        {/each}
      {/if}

      <div class="sv-spacer"></div>
    </div>
  {/if}

  <!-- ═══ Delete confirmation ═══ -->
  {#if confirmDelete}
    <div class="sv-modal-back">
      <div class="sv-modal">
        <h4 class="sv-modal-title">Delete {confirmDelete.label}?</h4>
        <p class="sv-modal-desc">
          <code>{confirmDelete.name}</code> is removed from the robot's storage.
          You can push it again with <code>mpx-cli deploy</code>.
        </p>
        <div class="sv-modal-actions">
          <button class="sv-btn sv-btn-ghost" onclick={() => (confirmDelete = null)}>Cancel</button>
          <button class="sv-btn sv-btn-danger" onclick={doDelete}>Delete</button>
        </div>
      </div>
    </div>
  {/if}
</div>

<style>
  .sv-root {
    flex: 1;
    min-height: 0;
    background: var(--surface-2);
    display: flex;
    flex-direction: column;
    overflow: hidden;
    position: relative;
  }

  /* ── Header ─────────────────────────────────────── */
  .sv-header {
    flex-shrink: 0;
    background: var(--bg);
    padding: 14px 16px 12px;
    border-bottom: 1px solid var(--line);
  }

  .sv-header-row {
    display: flex;
    align-items: center;
    gap: 8px;
    margin-bottom: 9px;
  }

  .sv-title {
    font-size: 24px;
    font-weight: 700;
    letter-spacing: -0.02em;
    color: var(--text-1);
    margin: 0;
  }

  .sv-icon-btn {
    margin-left: auto;
    width: 30px;
    height: 30px;
    display: flex;
    align-items: center;
    justify-content: center;
    border: 1px solid var(--line);
    border-radius: 50%;
    background: var(--surface-1);
    color: var(--text-2);
    cursor: pointer;
  }
  .sv-icon-btn:active { background: var(--surface-2); }

  /* ── Segmented control ──────────────────────────── */
  .sv-segmented {
    display: flex;
    gap: 3px;
    background: var(--surface-2);
    border: 1px solid var(--line);
    border-radius: 999px;
    padding: 3px;
  }

  .sv-seg {
    flex: 1;
    display: flex;
    align-items: center;
    justify-content: center;
    gap: 5px;
    padding: 6px 4px;
    border: none;
    border-radius: 999px;
    background: transparent;
    color: var(--text-2);
    font-size: 12.5px;
    font-weight: 600;
    white-space: nowrap;
    cursor: pointer;
    transition: background 0.15s, color 0.15s;
  }

  .sv-seg-on {
    background: var(--solid);
    color: var(--solid-ink);
  }

  .sv-seg-count {
    display: inline-flex;
    align-items: center;
    justify-content: center;
    min-width: 17px;
    height: 17px;
    padding: 0 4px;
    border-radius: 999px;
    background: var(--surface-3);
    color: inherit;
    font-size: 0.64rem;
    font-weight: 800;
  }
  .sv-seg-on .sv-seg-count {
    background: var(--accent);
    color: var(--accent-ink);
  }

  /* The one-liner under the tabs. Deliberately quiet: it is there for the
     first few times you use the screen, not to compete with the list. */
  .sv-blurb {
    margin: 8px 2px 0;
    font-size: 0.72rem;
    line-height: 1.4;
    color: #8a8a8a;
  }

  /* Marks a skill the robot knows came from the store, as opposed to one you
     pushed yourself. Outlined rather than filled so it reads as provenance
     and not as a second type badge. */
  .sv-chip-store {
    background: transparent;
    border: 1px solid #c9c9c9;
    color: var(--text-2);
  }


  /* ── Type filters ─────────────────────────────────────
     A second row of tabs under the segment control. Visually lighter than
     .sv-seg so the hierarchy stays readable: segment picks the screen, this
     narrows what is on it. */
  .sv-filters {
    display: flex;
    gap: 6px;
    overflow-x: auto;
    -webkit-overflow-scrolling: touch;
    scrollbar-width: none;
    padding: 2px 0 10px;
  }
  .sv-filters::-webkit-scrollbar { display: none; }

  .sv-filter {
    flex-shrink: 0;
    display: inline-flex;
    align-items: center;
    gap: 5px;
    padding: 5px 11px;
    border: 1.5px solid var(--line);
    border-radius: 999px;
    background: var(--surface-1);
    color: var(--text-2);
    font-size: 0.72rem;
    font-weight: 700;
    cursor: pointer;
    transition: border-color 0.15s, color 0.15s, background 0.15s;
  }
  .sv-filter:hover { border-color: #bcbcbc; }

  .sv-filter-on {
    /* Tinted with the type's own colour rather than a single accent, so the
       active filter and the chips on the cards below are visibly the same
       idea. */
    border-color: var(--badge);
    color: var(--text-1);
    background: color-mix(in srgb, var(--badge) 14%, var(--surface-1));
  }

  .sv-filter-dot {
    width: 7px;
    height: 7px;
    border-radius: 50%;
    background: var(--badge);
  }

  .sv-filter-count {
    font-size: 0.62rem;
    font-weight: 800;
    color: var(--text-3);
  }
  .sv-filter-on .sv-filter-count { color: var(--badge); }

  .sv-filter-blurb {
    margin: -4px 2px 10px;
    font-size: 0.7rem;
    line-height: 1.4;
    color: #8a8a8a;
  }

  /* ── Capability permissions ───────────────────────────
     The phone-app permission list. Neutral pills for the harmless ones,
     amber for anything that moves hardware, records, or costs money —
     colour plus a border change, never colour alone. */
  .sv-perms {
    display: flex;
    flex-wrap: wrap;
    gap: 5px;
    margin-top: 9px;
  }

  .sv-perm {
    padding: 3px 8px;
    border-radius: 6px;
    background: var(--surface-2);
    border: 1px solid #e4e4e4;
    color: var(--text-2);
    font-size: 0.63rem;
    font-weight: 600;
    line-height: 1.3;
  }

  .sv-perm-warn {
    background: #fff6e8;
    border-color: #f0c99a;
    color: #8a5a1c;
  }

  /* ── Blocked skills ───────────────────────────────────
     Owned, but this robot lacks the hardware. Shown greyed rather than
     hidden: "where did my skill go" is a worse experience than "here is
     your skill and here is why it cannot run". */
  .sv-card-blocked {
    opacity: 0.72;
    background: var(--surface-2);
  }

  .sv-blocked-note {
    margin: 8px 0 0;
    font-size: 0.67rem;
    line-height: 1.4;
    color: #a06a2c;
  }

  .sv-toggle-blocked {
    opacity: 0.4;
    pointer-events: none;
  }


  /* ── Group headings ───────────────────────────────────
     Sorting alone was invisible with ten skills in one column. A coloured
     rule keyed to the type lets the eye find a section without reading. */
  .sv-group-head {
    display: flex;
    align-items: center;
    gap: 7px;
    margin: 2px 0 8px;
  }

  .sv-group-bar {
    width: 3px;
    height: 13px;
    border-radius: 2px;
    background: var(--badge);
  }

  .sv-group-title { margin: 0; }

  .sv-group-count {
    margin-left: auto;
    font-size: 0.64rem;
    font-weight: 800;
    color: #a8a8a8;
  }

  /* ── Body ───────────────────────────────────────── */
  .sv-body {
    flex: 1;
    min-height: 0;
    overflow-y: auto;
    -webkit-overflow-scrolling: touch;
    padding: 12px 14px 0;
  }

  .sv-files-host {
    flex: 1;
    min-height: 0;
    display: flex;
    flex-direction: column;
    overflow: hidden;
  }

  .sv-spacer { height: 88px; }

  /* ── Banners ────────────────────────────────────── */
  .sv-banner {
    display: flex;
    align-items: center;
    gap: 10px;
    padding: 10px 12px;
    border-radius: 12px;
    background: var(--surface-1);
    border-left: 4px solid var(--line-strong);
    margin-bottom: 10px;
    box-shadow: 0 1px 3px rgba(0, 0, 0, 0.08);
  }

  .sv-banner-run { border-left-color: var(--yellow); }
  .sv-banner-ok  { border-left-color: var(--ok); }
  .sv-banner-bad { border-left-color: var(--danger); }

  .sv-banner-text {
    display: flex;
    flex-direction: column;
    gap: 1px;
    min-width: 0;
    flex: 1;
  }
  .sv-banner-text strong {
    font-size: 0.84rem;
    font-weight: 800;
    color: var(--text-1);
  }
  .sv-banner-text span {
    font-size: 0.74rem;
    color: #6b6b6b;
    overflow-wrap: anywhere;
  }

  .sv-banner-mark {
    display: flex;
    align-items: center;
    justify-content: center;
    width: 22px;
    height: 22px;
    flex-shrink: 0;
    border-radius: 50%;
    font-size: 0.78rem;
    font-weight: 900;
    color: var(--solid-ink);
    background: var(--surface-3);
  }
  .sv-banner-ok  .sv-banner-mark { background: var(--ok); }
  .sv-banner-bad .sv-banner-mark { background: var(--danger); }

  .sv-banner-x {
    border: none;
    background: none;
    color: #9b9b9b;
    font-size: 0.8rem;
    cursor: pointer;
    padding: 4px;
    flex-shrink: 0;
  }

  .sv-spinner {
    width: 18px;
    height: 18px;
    flex-shrink: 0;
    border: 2.5px solid var(--line);
    border-top-color: var(--line-strong);
    border-radius: 50%;
    animation: sv-spin 0.7s linear infinite;
  }
  @keyframes sv-spin { to { transform: rotate(360deg); } }

  /* ── Storage ────────────────────────────────────── */
  .sv-storage {
    background: var(--surface-1);
    border-radius: 12px;
    padding: 9px 12px 11px;
    margin-bottom: 14px;
    box-shadow: 0 1px 3px rgba(0, 0, 0, 0.06);
  }

  .sv-storage-row {
    display: flex;
    justify-content: space-between;
    font-size: 0.72rem;
    font-weight: 700;
    color: #6b6b6b;
    margin-bottom: 6px;
  }

  .sv-storage-track {
    height: 5px;
    border-radius: 999px;
    background: #e4e4e4;
    overflow: hidden;
  }

  .sv-storage-fill {
    height: 100%;
    border-radius: 999px;
    background: var(--solid);
  }
  .sv-storage-hot { background: var(--danger); }

  /* ── Sections ───────────────────────────────────── */
  .sv-section { margin-bottom: 18px; }

  .sv-section-title {
    font-size: 0.68rem;
    font-weight: 800;
    letter-spacing: 0.07em;
    text-transform: uppercase;
    color: var(--text-3);
    margin: 0 0 8px 2px;
  }

  .sv-muted {
    font-size: 0.8rem;
    color: var(--text-3);
    margin: 6px 2px;
  }

  .sv-list {
    display: flex;
    flex-direction: column;
    gap: 9px;
  }

  /* ── Skill card ─────────────────────────────────── */
  .sv-card {
    background: var(--surface-1);
    border-radius: 14px;
    padding: 12px;
    box-shadow: 0 1px 3px rgba(0, 0, 0, 0.08);
    border: 1.5px solid transparent;
  }

  .sv-card-live {
    border-color: var(--yellow);
    box-shadow: 0 0 0 3px rgba(255, 230, 5, 0.25);
  }

  .sv-card-top {
    display: flex;
    align-items: flex-start;
    gap: 10px;
  }

  .sv-avatar {
    width: 38px;
    height: 38px;
    flex-shrink: 0;
    border-radius: 11px;
    background: var(--badge, var(--surface-3));
    color: var(--solid-ink);
    display: flex;
    align-items: center;
    justify-content: center;
    font-size: 1rem;
    font-weight: 800;
  }

  .sv-card-id {
    min-width: 0;
    flex: 1;
    display: flex;
    flex-direction: column;
    gap: 3px;
  }

  .sv-card-title-row {
    display: flex;
    align-items: center;
    gap: 6px;
    flex-wrap: wrap;
  }

  .sv-card-title {
    font-size: 0.9rem;
    font-weight: 800;
    color: var(--text-1);
    overflow-wrap: anywhere;
  }

  .sv-chip {
    flex-shrink: 0;
    padding: 2px 7px;
    border-radius: 999px;
    background: var(--badge, var(--surface-3));
    color: var(--solid-ink);
    font-size: 0.58rem;
    font-weight: 800;
    letter-spacing: 0.04em;
    text-transform: uppercase;
  }

  .sv-card-meta {
    font-size: 0.7rem;
    color: var(--text-3);
    font-family: ui-monospace, SFMono-Regular, Menlo, monospace;
    overflow-wrap: anywhere;
  }

  /* Actions sit on their own row so a long skill name never squeezes the
     buttons to unreadable slivers on a narrow phone. */
  .sv-card-actions {
    display: flex;
    gap: 7px;
    margin-top: 11px;
  }

  .sv-btn {
    flex: 1;
    height: 38px;
    padding: 0 12px;
    border: 1px solid transparent;
    border-radius: var(--r-sm);
    font-size: 13px;
    font-weight: 600;
    cursor: pointer;
    white-space: nowrap;
    transition: transform 80ms ease;
  }
  .sv-btn:active { transform: scale(0.98); }
  .sv-btn:disabled { opacity: 0.45; cursor: default; }

  /* Run is the card's action, but there is one per card: solid, not the
     accent, so a list of skills is not a wall of yellow. The accent is for
     the one thing on the screen that matters most (the add button). */
  .sv-btn-primary { background: var(--solid); color: var(--solid-ink); }
  .sv-btn-ghost   { background: var(--surface-2); color: var(--text-1); border-color: var(--line); }
  .sv-btn-danger  { background: var(--danger-soft); color: var(--danger); }

  /* Placeholder for the one section that waits on the network, not the robot. */
  .sv-mkt-wait {
    margin: 0;
    padding: 13px 15px;
    background: var(--surface-1);
    border-radius: 14px;
    font-size: 0.78rem;
    color: #7b7b7b;
    box-shadow: 0 1px 3px rgba(0, 0, 0, 0.06);
  }

  /* ── Empty state ────────────────────────────────── */
  .sv-empty {
    background: var(--surface-1);
    border-radius: 14px;
    padding: 22px 18px;
    text-align: center;
    box-shadow: 0 1px 3px rgba(0, 0, 0, 0.06);
  }

  .sv-empty-title {
    font-size: 0.92rem;
    font-weight: 800;
    color: var(--text-1);
    margin: 0 0 5px;
  }

  .sv-empty-desc {
    font-size: 0.78rem;
    color: #7b7b7b;
    margin: 0 0 10px;
    line-height: 1.45;
  }
  .sv-empty-or { margin: 10px 0 0; font-size: 0.72rem; }

  .sv-code {
    display: inline-block;
    padding: 7px 13px;
    border-radius: 8px;
    background: var(--accent);
    color: var(--accent-ink);
    font-family: ui-monospace, SFMono-Regular, Menlo, monospace;
    font-size: 0.78rem;
    font-weight: 700;
  }

  /* ── Marketplace toggle ─────────────────────────── */
  .sv-toggle {
    position: relative;
    width: 46px;
    height: 26px;
    margin-left: auto;
    flex-shrink: 0;
    cursor: pointer;
  }
  .sv-toggle-busy { opacity: 0.5; }
  .sv-toggle input {
    position: absolute;
    opacity: 0;
    width: 100%;
    height: 100%;
    margin: 0;
    cursor: pointer;
    z-index: 2;
  }
  .sv-toggle-track {
    position: absolute;
    inset: 0;
    border-radius: 999px;
    background: var(--surface-3);
    transition: background 0.18s;
  }
  .sv-toggle-thumb {
    position: absolute;
    top: 3px;
    left: 3px;
    width: 20px;
    height: 20px;
    border-radius: 50%;
    background: var(--surface-1);
    box-shadow: 0 1px 3px rgba(0, 0, 0, 0.3);
    transition: transform 0.18s;
  }
  .sv-toggle input:checked ~ .sv-toggle-track { background: var(--solid); }
  .sv-toggle input:checked ~ .sv-toggle-thumb { transform: translateX(20px); }

  /* ── FAB ────────────────────────────────────────── */
  .sv-fab {
    position: absolute;
    right: 16px;
    bottom: 18px;
    width: 52px;
    height: 52px;
    border: none;
    border-radius: 50%;
    background: var(--accent);
    color: var(--accent-ink);
    display: flex;
    align-items: center;
    justify-content: center;
    box-shadow: 0 3px 10px rgba(0, 0, 0, 0.25);
    cursor: pointer;
    z-index: 5;
  }
  .sv-fab:active { transform: scale(0.94); }

  /* ── Confirm modal ──────────────────────────────── */
  .sv-modal-back {
    position: absolute;
    inset: 0;
    background: var(--surface-3);
    display: flex;
    align-items: center;
    justify-content: center;
    padding: 22px;
    z-index: 20;
  }

  .sv-modal {
    width: 100%;
    max-width: 330px;
    background: var(--surface-1);
    border-radius: 16px;
    padding: 18px;
  }

  .sv-modal-title {
    font-size: 0.98rem;
    font-weight: 800;
    color: var(--text-1);
    margin: 0 0 7px;
  }

  .sv-modal-desc {
    font-size: 0.78rem;
    color: #6b6b6b;
    line-height: 1.5;
    margin: 0 0 15px;
  }
  .sv-modal-desc code {
    font-family: ui-monospace, SFMono-Regular, Menlo, monospace;
    font-size: 0.74rem;
    background: var(--surface-2);
    padding: 1px 5px;
    border-radius: 4px;
    overflow-wrap: anywhere;
  }

  .sv-modal-actions { display: flex; gap: 8px; }

  /* ── Wider screens: two cards per row ───────────── */
  @media (min-width: 720px) {
    .sv-body { padding: 16px 20px 0; }

    /* Left unconstrained this stretches to the full window width on a
       desktop browser, which reads as a banner rather than a control. */
    .sv-segmented { max-width: 360px; }

    .sv-list {
      display: grid;
      grid-template-columns: repeat(auto-fill, minmax(300px, 1fr));
      gap: 12px;
    }

    .sv-card { display: flex; flex-direction: column; }
    .sv-card-actions { margin-top: auto; padding-top: 11px; }
  }
</style>
