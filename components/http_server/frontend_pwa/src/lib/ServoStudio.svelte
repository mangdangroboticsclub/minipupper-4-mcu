<script>
  /**
   * MangDang Servo Studio — in-app servo tuning for the AT32 driver boards.
   *
   * Same workbench as the standalone page at /studio: servo tree on the left,
   * parameter table + live scope in the middle, direct control + live readouts
   * on the right. Collapses to a single column on a phone.
   *
   * Talks to the /v1/studio/* API in main/network/http_server.cc. Kept in the
   * black/yellow instrument look rather than the app's light theme — this
   * screen drives motors directly, and looking different is the point.
   */
  let { onBack } = $props();

  /* ── Model ─────────────────────────────────────────────────── */
  const PARAMS = [
    { a: 0, n: "Reverse position sensor", h: "0 or 1",               cal: true,  s: 1 },
    { a: 1, n: "Min position ADC",        h: "raw sensor floor",     cal: true,  s: 1 },
    { a: 2, n: "Max position ADC",        h: "raw sensor ceiling",   cal: true,  s: 1 },
    { a: 3, n: "Range position",          h: "degrees",              cal: true,  s: 1 },
    { a: 4, n: "Reverse motor",           h: "0 or 1",               cal: true,  s: 1 },
    { a: 5, n: "Kp position",             h: "position P gain",      cal: false, s: 0.01,    d: 65 },
    { a: 6, n: "Kd position",             h: "position D gain",      cal: false, s: 0.01,    d: 800 },
    { a: 7, n: "Kp current",              h: "current P gain",       cal: false, s: 0.0001,  d: 0.0006 },
    { a: 8, n: "Kff current",             h: "current feed-forward", cal: false, s: 0.00001, d: 0.00022 },
    { a: 9, n: "Max PWM duty",            h: "0 to 1",               cal: false, s: 0.01 },
  ];
  const CAL   = PARAMS.filter((p) => p.cal);
  const GAINS = PARAMS.filter((p) => !p.cal);

  /* The tuned gains this robot ships with. Only the four control gains have a
     default: calibration is per-servo (each board's own sensor range) and Max
     PWM duty is a safety ceiling you set deliberately, so neither has one
     right answer to restore. `d` is in AT32 board units — the same units the
     table edits and /v1/studio/set writes — NOT MuJoCo N·m/rad. */
  const DEFAULTS = PARAMS.filter((p) => p.d !== undefined);
  /* Written as decimals, not %g of a float: 0.00022 must reach the board as
     0.00022, and toString() on the number gives exactly that. */
  const dtxt = (p) => String(p.d);

  const LEGS = [
    { t: "Front Right", ids: [1, 2, 3] },  { t: "Front Left", ids: [4, 5, 6] },
    { t: "Rear Right",  ids: [7, 8, 9] },  { t: "Rear Left",  ids: [10, 11, 12] },
  ];
  const JOINT = ["Abduction", "Thigh", "Calf"];
  const MODES = ["idle", "position", "torque"];

  const LIVE_ROWS = [
    ["now_deg", "Position", "°", 1],    ["set_deg", "Goal", "°", 1],
    ["err_deg", "Pos error", "°", 2],   ["now_ma", "Current", "mA", 0],
    ["cap_ma", "Current cap", "mA", 0], ["duty", "PWM duty", "", 3],
    ["temp_c", "NTC temp", "°C", 1],    ["mode", "Mode", "", 0],
  ];

  /* ── State ─────────────────────────────────────────────────── */
  let cli     = $state(false);   // studio mode / "connected"
  let online  = $state(false);
  let cur     = $state(0);
  let servos  = $state([]);
  let gone    = $state(new Set());
  let vals    = $state({});      // addr -> editable string
  let actual  = $state({});      // addr -> value read back from the board
  let loading = $state(false);
  let live    = $state(null);
  let poll    = $state(false);
  let showGraph = $state(false);
  let grun    = $state(false);
  let ginfo   = $state("idle");

  let saOpen = $state(false);
  let saTick = $state({}), saVal = $state({}), saRes = $state({});
  let saSave = $state(false), saBusy = $state("");
  for (const p of PARAMS) { saTick[p.a] = !p.cal; saVal[p.a] = ""; saRes[p.a] = "—"; }

  let deg = $state(135);
  let cap = $state(200);

  let toastMsg = $state(""), toastBad = $state(false);
  let toastTimer;

  /* A toast is gone in 2.6 s, which is no use when you are trying to work out
     whether a write landed. Every toast is also appended here, so the answer
     is still on screen when you look for it. Newest first, capped — this is a
     "what just happened" panel, not a record. */
  let logLines = $state([]);
  let logOpen  = $state(true);
  function logAdd(m, bad) {
    const t = new Date().toLocaleTimeString([], { hour12: false });
    logLines = [{ t, m, bad }, ...logLines].slice(0, 40);
  }

  function toast(m, bad = false) {
    toastMsg = m; toastBad = bad;
    logAdd(m, bad);
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => (toastMsg = ""), 2600);
  }

  /* ── One request at a time ─────────────────────────────────────
   * WHY THIS EXISTS, AND WHY THE STANDALONE /studio PAGE DOES NOT NEED IT.
   *
   * The robot's HTTP server runs with max_open_sockets = 5 and
   * lru_purge_enable (main/network/http_server.cc). The /studio page is
   * effectively the only client on that budget, so it can fire requests and
   * forget about them.
   *
   * The PWA is not alone. The app shell holds a PERMANENT WebSocket on
   * /v1/chat/ui, polls /v1/wifi/status, and leaves keep-alive sockets behind
   * from loading its own assets. Add this screen's 600 ms status poll, its
   * 140 ms live poll and a write on top and the cap is reached. httpd then
   * LRU-purges the oldest IDLE session — very often a keep-alive socket the
   * browser is about to reuse. The reused socket dies on send and fetch()
   * rejects before anything reaches the robot.
   *
   * That is the "sometimes it works, mostly it does not" bug. Nothing was
   * wrong with the write, the driver board or the SPI bus: the request never
   * left the phone. It is also why Factory reset seemed fine — it is the one
   * button pressed rarely enough to usually land on a live socket.
   *
   * Two rules fix it without touching the firmware:
   *   1. One request in flight at a time, so this screen never needs more
   *      than one socket and never races its own poll.
   *   2. A purged keep-alive socket fails instantly and the next attempt
   *      opens a fresh one — so retry a network-level failure exactly once.
   *
   * Polls are deliberately NOT retried and do not hold the link: a dropped
   * refresh costs nothing, and 600 ms later there is another one.
   */
  let chain     = Promise.resolve();
  let inflight  = 0;                // > 0 => a real operation owns the link
  const isPoll  = (p) => p.startsWith("/v1/studio/status") ||
                         p.startsWith("/v1/studio/live");

  async function rawGet(path) {
    const r = await fetch(path, { cache: "no-store" });
    if (!r.ok) throw new Error("HTTP " + r.status);
    return r.json();
  }

  function jget(path) {
    const poll_ = isPoll(path);
    if (!poll_) inflight++;
    const run = chain.then(async () => {
      try {
        return await rawGet(path);
      } catch (e) {
        if (poll_) throw e;
        await new Promise((r) => setTimeout(r, 120));
        return rawGet(path);        // second attempt gets a fresh socket
      }
    }).finally(() => { if (!poll_) inflight--; });
    chain = run.catch(() => {});    // one failure must not poison the queue
    return run;
  }

  /* The app shell's permission WebSocket is one of the robot's five sockets,
     held for the whole session. It is worth nothing on this screen and costs
     a fifth of the budget, so borrow it while Studio is open and hand it
     straight back on the way out. Reads no state, so it runs once per mount. */
  $effect(() => {
    window.dispatchEvent(new Event("mpx:yield-socket"));
    return () => window.dispatchEvent(new Event("mpx:resume-socket"));
  });

  const fmt = (v, d = 1) =>
    (v === null || v === undefined || Number.isNaN(v)) ? "—" : Number(v).toFixed(d);
  const pad = (n) => String(n).padStart(2, "0");

  const curLabel = $derived.by(() => {
    for (const leg of LEGS) {
      const j = leg.ids.indexOf(cur);
      if (j >= 0) return `servo ${cur} · ${leg.t} ${JOINT[j]}`;
    }
    return "no servo selected";
  });
  const has = $derived(cli && cur > 0);
  const servoAt = (id) => servos.find((s) => s.id === id);

  /* ── Status poll ───────────────────────────────────────────── */
  async function pollStatus() {
    /* A pending write matters more than a fresher temperature reading, and a
       poll queued in front of it only delays it. */
    if (inflight) return;
    try {
      const d = await jget("/v1/studio/status");
      servos = d.servos; cli = d.studio; online = true;
    } catch { online = false; }
  }
  $effect(() => {
    pollStatus();
    const t = setInterval(pollStatus, 600);
    return () => clearInterval(t);
  });

  // Never strand the robot with its gait parked because the screen closed.
  $effect(() => () => {
    if (cli && navigator.sendBeacon) navigator.sendBeacon("/v1/studio/mode?on=0");
  });

  /* ── Actions ───────────────────────────────────────────────── */
  async function toggleConn() {
    try {
      const d = await jget("/v1/studio/mode?on=" + (cli ? 0 : 1));
      cli = d.studio;
      toast(cli ? "connected — gait parked" : "disconnected — gait resumed");
      if (cli && cur) refreshSel(); else stopPoll();
    } catch { toast("could not change mode", true); }
  }

  async function scan() {
    if (!cli) return toast("press Connect first", true);
    try {
      const d = await jget("/v1/studio/scan");
      if (!d.ok) return toast(d.err || "scan failed", true);
      const f = new Set(d.found);
      gone = new Set([1,2,3,4,5,6,7,8,9,10,11,12].filter((i) => !f.has(i)));
      toast(d.found.length + " of 12 servos answered", d.found.length !== 12);
    } catch { toast("scan failed", true); }
  }

  function selServo(id) {
    cur = id;
    gReset();
    if (cli) refreshSel();
  }

  async function refreshSel() {
    if (!cli || !cur) return;
    loading = true;
    try {
      const d = await jget("/v1/studio/dump?id=" + cur);
      if (!d.ok) { actual = {}; vals = {}; toast(d.err || "read failed", true); return; }
      const a = {}, v = {};
      for (const p of d.params) {
        a[p.p] = p.v;
        v[p.p] = (p.v === null || p.v === undefined) ? "" : String(p.v);
      }
      actual = a; vals = v;
    } catch { toast("read failed", true); }
    finally { loading = false; }
  }

  /* The value boxes are <input type="number">, and Svelte's bind:value COERCES
     those to a Number. So the same slot holds a string when it was filled in
     from a board read (refreshSel writes String(p.v)) and a number the instant
     you type in it.

     Calling .trim() on that number threw a TypeError before the try block —
     no request, no toast, not even a line in the Activity panel. That is the
     "I press Set and nothing happens" bug, and it explains its shape exactly:
     Reload, Tuned defaults and Factory Reset all worked because none of them
     read this field, and a row you had not typed into still held a string.
     Reverse motor was just the row you happened to edit.

     Read it as text and this stops mattering. */
  const asText = (x) =>
    (x === null || x === undefined || Number.isNaN(x)) ? "" : String(x).trim();

  async function applyParam(a) {
    /* scan() and saveFlash() both check this and applyParam did not, so a
       write with the gait still running produced a bare "write refused" from
       the firmware with nothing saying what to do about it. */
    if (!cli) return toast("press Connect first — the gait still owns the bus", true);
    const v = asText(vals[a]);
    if (v === "") return toast("enter a value first", true);
    try {
      const d = await jget(`/v1/studio/set?id=${cur}&p=${a}&v=${encodeURIComponent(v)}`);
      if (!d.ok) return toast(d.err || "write refused", true);
      actual[a] = d.v; vals[a] = String(d.v);
      toast(PARAMS[a].n + " → " + d.v);
    } catch (e) {
      /* Report the real reason. A bare "write failed" sent me hunting the SPI
         bus for a bug that was in this function. */
      toast("write failed — " + (e?.message || e), true);
    }
  }

  async function saveFlash(all = false) {
    if (!cli) return toast("press Connect first", true);
    try {
      const d = await jget("/v1/studio/save?id=" + (all ? 0 : cur));
      toast(d.ok ? (all ? "all four boards saved" : "saved to board flash")
                 : (d.err || "save failed"), !d.ok);
    } catch { toast("save failed", true); }
  }

  async function factoryReset() {
    if (!cli) return toast("press Connect first — the gait still owns the bus", true);
    try {
      const d = await jget("/v1/studio/restore?id=" + cur);
      toast(d.ok ? "factory defaults in RAM — save to keep" : (d.err || "restore failed"), !d.ok);
      if (d.ok) refreshSel();
    } catch { toast("restore failed", true); }
  }

  async function direct(mode) {
    if (!cli) return toast("press Connect first — the gait still owns the bus", true);
    try {
      const d = await jget(`/v1/studio/direct?id=${cur}&m=${mode}&deg=${deg}&cur=${cap || 200}`);
      toast(d.ok ? (mode ? `holding ${Number(deg).toFixed(1)}°` : "motor off")
                 : (d.err || "refused"), !d.ok);
    } catch { toast("command failed", true); }
  }

  /* ── Live poll (also feeds the scope) ──────────────────────── */
  let pollTimer = null;
  function stopPoll() { poll = false; clearInterval(pollTimer); pollTimer = null; }
  function togglePoll() {
    if (poll) return stopPoll();
    poll = true;
    tickPoll();
    pollTimer = setInterval(tickPoll, 140);
  }
  /* A transient drop used to stop the scope dead. Now three consecutive
     failures do — one lost frame at 140 ms is not worth ending the trace. */
  let liveMiss = 0;
  async function tickPoll() {
    if (!poll || !cur || inflight) return;
    try {
      const d = await jget("/v1/studio/live?id=" + cur);
      if (!d.ok) { stopPoll(); return toast(d.err || "live read failed", true); }
      live = d;
      liveMiss = 0;
      gPush(d);
    } catch {
      if (++liveMiss >= 3) { liveMiss = 0; stopPoll(); toast("live read failed", true); }
    }
  }

  /* ── Scope ─────────────────────────────────────────────────────
   * Two time-aligned plots rather than one dual-axis plot: degrees and
   * milliamps share no scale, so overlaying them on two y-axes would invent a
   * correlation that is not in the data. One measured series per pane (yellow)
   * plus its commanded value as a dashed reference.
   * ──────────────────────────────────────────────────────────── */
  const GWIN = 20000, GMAX = 4000;
  let gbuf = [];                       // plain array on purpose — not reactive
  let gt0 = 0, gHover = null;
  let cvA = $state(null), cvB = $state(null), tipEl = $state(null);
  let tipHtml = $state(""), tipShow = $state(false), tipX = $state(0), tipY = $state(0);

  function gStartStop() {
    if (grun) { grun = false; return; }
    if (!cli || !cur) return toast("connect and pick a servo first", true);
    if (!poll) togglePoll();
    if (!gbuf.length) gt0 = Date.now();
    grun = true;
  }
  function gReset() { gbuf = []; ginfo = "idle"; gDraw(); }

  function gPush(d) {
    if (!grun) return;
    const p = { t: Date.now() - gt0 };
    for (const k of ["now_deg","set_deg","err_deg","now_ma","cap_ma","duty","temp_c"])
      if (d[k] !== undefined) p[k] = d[k];
    gbuf.push(p);
    if (gbuf.length > GMAX) gbuf.shift();
    ginfo = `servo ${cur} · ${gbuf.length} samples · ${(p.t / 1000).toFixed(1)} s`;
    gDraw();
  }

  function gSize() {
    for (const c of [cvA, cvB]) {
      if (!c) continue;
      const r = c.getBoundingClientRect(), dpr = window.devicePixelRatio || 1;
      c.width = Math.max(80, Math.round(r.width * dpr));
      c.height = Math.round(150 * dpr);
      c.getContext("2d").setTransform(dpr, 0, 0, dpr, 0, 0);
    }
  }
  function niceStep(span) {
    const raw = span / 4, p = Math.pow(10, Math.floor(Math.log10(raw))), n = raw / p;
    return (n <= 1 ? 1 : n <= 2 ? 2 : n <= 5 ? 5 : 10) * p;
  }

  function drawPlot(c, keyMain, keyRef, unit, digits) {
    if (!c) return;
    const x = c.getContext("2d"), dpr = window.devicePixelRatio || 1;
    const W = c.width / dpr, H = c.height / dpr;
    const L = 52, R = 12, T = 10, B = 20, pw = W - L - R, ph = H - T - B;
    x.clearRect(0, 0, W, H);
    if (pw < 40 || ph < 30) return;

    if (!gbuf.length) {
      x.fillStyle = "#5A5A5A"; x.font = "12px Inter,sans-serif";
      x.textAlign = "center"; x.textBaseline = "middle";
      x.fillText("Press Start to record", W / 2, H / 2);
      return;
    }

    const tmax = gbuf[gbuf.length - 1].t, tmin = Math.max(0, tmax - GWIN);
    const win = gbuf.filter((p) => p.t >= tmin);

    let mn = Infinity, mx = -Infinity;
    for (const p of win) for (const k of [keyMain, keyRef]) {
      const v = p[k];
      if (v !== undefined && isFinite(v)) { if (v < mn) mn = v; if (v > mx) mx = v; }
    }
    if (!isFinite(mn)) { mn = 0; mx = 1; }
    if (mn === mx) { mn -= 1; mx += 1; }
    const padY = (mx - mn) * 0.12; mn -= padY; mx += padY;

    const X = (t) => L + (t - tmin) / Math.max(1, tmax - tmin) * pw;
    const Y = (v) => T + ph - (v - mn) / (mx - mn) * ph;

    // recessive grid — solid hairlines, one shade off the surface
    const step = niceStep(mx - mn);
    x.strokeStyle = "#242424"; x.lineWidth = 1;
    x.fillStyle = "#5A5A5A";
    x.font = '10px "JetBrains Mono",ui-monospace,monospace';
    x.textAlign = "right"; x.textBaseline = "middle";
    for (let v = Math.ceil(mn / step) * step; v <= mx; v += step) {
      const y = Math.round(Y(v)) + 0.5;
      x.beginPath(); x.moveTo(L, y); x.lineTo(L + pw, y); x.stroke();
      x.fillText(v.toFixed(digits), L - 8, y);
    }
    x.textAlign = "center"; x.textBaseline = "top";
    for (let s = Math.ceil(tmin / 5000) * 5000; s <= tmax; s += 5000)
      x.fillText((s / 1000).toFixed(0) + "s", X(s), T + ph + 5);
    x.textAlign = "left"; x.fillText(unit, L, T - 2);

    const line = (key, dash, color) => {
      x.save(); x.setLineDash(dash); x.strokeStyle = color; x.lineWidth = 2;
      x.lineJoin = "round"; x.lineCap = "round"; x.beginPath();
      let started = false;
      for (const p of win) {
        const v = p[key];
        if (v === undefined || !isFinite(v)) { started = false; continue; }
        const px = X(p.t), py = Y(v);
        if (!started) { x.moveTo(px, py); started = true; } else x.lineTo(px, py);
      }
      x.stroke(); x.restore();
    };
    line(keyRef, [4, 4], "#8C8C8C");   // reference underneath
    line(keyMain, [], "#FFE605");      // measured on top

    for (let i = win.length - 1; i >= 0; i--) {
      const v = win[i][keyMain];
      if (v === undefined || !isFinite(v)) continue;
      x.fillStyle = "#FFE605";
      x.beginPath(); x.arc(X(win[i].t), Y(v), 3, 0, 6.284); x.fill();
      break;
    }
    if (gHover) {
      const px = X(gHover.t);
      if (px >= L && px <= L + pw) {
        x.strokeStyle = "#4A4A4A"; x.lineWidth = 1; x.setLineDash([2, 3]);
        x.beginPath(); x.moveTo(px, T); x.lineTo(px, T + ph); x.stroke(); x.setLineDash([]);
      }
    }
  }

  function gDraw() {
    if (!showGraph) return;
    if (cvA && !cvA.width) gSize();
    drawPlot(cvA, "now_deg", "set_deg", "deg", 1);
    drawPlot(cvB, "now_ma", "cap_ma", "mA", 0);
  }

  $effect(() => {
    if (!showGraph || !cvA) return;
    gSize(); gDraw();
    const onResize = () => { gSize(); gDraw(); };
    window.addEventListener("resize", onResize);
    return () => window.removeEventListener("resize", onResize);
  });

  function onHover(ev, which) {
    if (!gbuf.length || !tipEl) return;
    const c = which === "A" ? cvA : cvB;
    const r = c.getBoundingClientRect(), dpr = window.devicePixelRatio || 1;
    const W = c.width / dpr, L = 52, R = 12, pw = W - L - R;
    const tmax = gbuf[gbuf.length - 1].t, tmin = Math.max(0, tmax - GWIN);
    const t = tmin + Math.min(1, Math.max(0, (ev.clientX - r.left - L) / pw)) * (tmax - tmin);
    let best = null, bd = Infinity;
    for (const p of gbuf) { const d = Math.abs(p.t - t); if (d < bd) { bd = d; best = p; } }
    gHover = best;
    // Anchor to the panel, not to whichever canvas fired the event.
    const wrap = tipEl.parentElement.getBoundingClientRect();
    tipShow = true;
    tipX = Math.max(6, Math.min(wrap.width - 178, ev.clientX - wrap.left + 14));
    tipY = Math.max(6, Math.min(wrap.height - 68, ev.clientY - wrap.top - 64));
    tipHtml = `${(best.t / 1000).toFixed(2)} s<br><b>${fmt(best.now_deg)}°</b> / goal ${fmt(best.set_deg)}°` +
              `<br><b>${fmt(best.now_ma, 0)} mA</b> / cap ${fmt(best.cap_ma, 0)} mA`;
    gDraw();
  }
  function offHover() { gHover = null; tipShow = false; gDraw(); }

  function capturePng() {
    if (!cvA || !cvB) return;
    const out = document.createElement("canvas");
    out.width = Math.max(cvA.width, cvB.width); out.height = cvA.height + cvB.height;
    const o = out.getContext("2d");
    o.fillStyle = "#0E0E0E"; o.fillRect(0, 0, out.width, out.height);
    o.drawImage(cvA, 0, 0); o.drawImage(cvB, 0, cvA.height);
    const l = document.createElement("a");
    l.download = `servo${cur}-trace.png`; l.href = out.toDataURL("image/png"); l.click();
    toast("PNG saved");
  }
  function saveCsv() {
    if (!gbuf.length) return toast("nothing recorded yet", true);
    const cols = ["t","now_deg","set_deg","err_deg","now_ma","cap_ma","duty","temp_c"];
    let csv = cols.join(",") + "\n";
    for (const p of gbuf) csv += cols.map((k) => p[k] === undefined ? "" : p[k]).join(",") + "\n";
    const l = document.createElement("a");
    l.download = `servo${cur}-trace.csv`;
    l.href = URL.createObjectURL(new Blob([csv], { type: "text/csv" })); l.click();
    toast(gbuf.length + " samples saved");
  }

  /* ── Set all servos ────────────────────────────────────────── */
  async function saFromCur() {
    if (!cur) return toast("pick a servo first", true);
    try {
      const d = await jget("/v1/studio/dump?id=" + cur);
      if (!d.ok) return toast(d.err || "read failed", true);
      for (const p of d.params) if (p.v !== null && p.v !== undefined) saVal[p.p] = String(p.v);
      toast("loaded servo " + cur);
    } catch { toast("read failed", true); }
  }
  /* Load the shipped gains into the set-all form, and tick exactly those four.
     Deliberately unticks everything else — including the calibration rows — so
     a stray value left in the Min/Max ADC boxes from a previous "From selected"
     cannot ride along and rewrite every board's sensor mapping. */
  function saDefaults() {
    for (const p of PARAMS) saTick[p.a] = false;
    for (const p of DEFAULTS) { saVal[p.a] = dtxt(p); saTick[p.a] = true; }
    toast("tuned gains loaded — press Apply");
  }

  /* Same values, one servo: fills the boxes without writing, so you can see
     what is about to change before pressing Set. */
  function curDefaults() {
    if (!cur) return toast("pick a servo first", true);
    for (const p of DEFAULTS) vals[p.a] = dtxt(p);
    toast("tuned gains filled in — press Set on each row");
  }

  async function saApply() {
    if (!cli) return toast("press Connect first — the gait still owns the bus", true);
    /* Same number-input coercion as applyParam: .trim() on a typed-in value
       threw here too, and this one took the whole Set All panel with it. */
    const todo = PARAMS.filter((p) => saTick[p.a] && asText(saVal[p.a]) !== "");
    if (!todo.length) return toast("tick a parameter and give it a value", true);
    let bad = 0;
    for (let i = 0; i < todo.length; i++) {
      const p = todo[i], last = i === todo.length - 1;
      saBusy = `Applying ${i + 1}/${todo.length}…`;
      saRes[p.a] = "…";
      try {
        const d = await jget(`/v1/studio/setall?p=${p.a}` +
                             `&v=${encodeURIComponent(asText(saVal[p.a]))}` +
                             `&save=${saSave && last ? 1 : 0}`);
        if (d.ok) {
          saRes[p.a] = d.fail?.length ? "fail " + d.fail.join(",") : `${d.n}/12`;
          if (d.fail?.length) bad++;
        } else { saRes[p.a] = "refused"; bad++; }
      } catch { saRes[p.a] = "error"; bad++; }
    }
    saBusy = "";
    toast(bad ? `${bad} parameter(s) had failures` : `applied to all 12${saSave ? " and saved" : ""}`, !!bad);
    if (cur && cli) refreshSel();
  }
</script>

<div class="studio">

  <!-- ══ Top bar ══════════════════════════════════════════════ -->
  <div class="bar">
    <button class="back" onclick={onBack} aria-label="Back">
      <svg width="19" height="19" viewBox="0 0 24 24" fill="none" stroke="currentColor"
           stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
        <polyline points="15 18 9 12 15 6"/>
      </svg>
    </button>
    <div class="brand">
      <h1>MangDang <em>Servo Studio</em></h1>
      <p>Mini Pupper · AT32 driver boards</p>
    </div>

    <div class="tools">
      <div class="btngrp">
        <button class="b" class:on={cli} onclick={toggleConn}>{cli ? "Disconnect" : "Connect"}</button>
        <button class="b out" onclick={scan}>Scan</button>
        <button class="b out" onclick={refreshSel}>Refresh</button>
      </div>
      <span class="sep"></span>
      <button class="b" class:on={showGraph} class:out={!showGraph}
              onclick={() => showGraph = !showGraph}>Graph</button>
      <span class="sep"></span>
      <button class="b out" onclick={() => cli ? (saOpen = true) : toast("press Connect first", true)}>Set All Servos</button>
      <span class="sep"></span>
      <div class="btngrp">
        <button class="b out" onclick={() => saveFlash(false)} disabled={!has}>Save</button>
        <button class="b out" onclick={() => saveFlash(true)}>Save All</button>
      </div>
    </div>

    <div class="banner">
      <i class="dot" class:on={online}></i>
      {online ? (cli ? "studio mode — gait parked" : "telemetry only — press Connect") : "no connection"}
    </div>
  </div>

  <!-- ══ Workbench ════════════════════════════════════════════ -->
  <div class="main">

    <!-- ── Servo tree ── -->
    <div class="tree">
      {#each LEGS as leg}
        <div class="leg">
          <div class="lbl">{leg.t}</div>
          <div class="rows">
          {#each leg.ids as id, j}
            {@const s = servoAt(id)}
            <button class="srv" class:on={cur === id} class:gone={gone.has(id)}
                    onclick={() => selServo(id)}>
              <span class="sid">{pad(id)}</span>
              <span class="snm">{JOINT[j]}</span>
              <span class="sdg" class:hot={s && s.c !== null && s.c >= 55}>
                {s ? fmt(s.deg) + "°" : "—"}{s && s.c !== null ? "  " + fmt(s.c, 0) + "°C" : ""}
              </span>
            </button>
          {/each}
          </div>
        </div>
      {/each}
    </div>

    <!-- ── Parameters + scope ── -->
    <div class="mid">
      <div class="card">
        <div class="hd">Parameters <span class="sp">{curLabel}</span></div>
        <div class="bd">
          <table>
            <thead><tr>
              <th class="addr">Addr</th><th>Parameter</th>
              <th class="n">Value</th><th class="n act">Actual</th><th class="n" style="width:58px"></th>
            </tr></thead>
            <tbody>
              {#if !cli}
                <tr><td colspan="5" class="empty">Press <b>Connect</b>, then pick a servo.</td></tr>
              {:else if !cur}
                <tr><td colspan="5" class="empty">Pick a servo from the list.</td></tr>
              {:else if loading}
                <tr><td colspan="5" class="empty">Reading…</td></tr>
              {:else if !Object.keys(actual).length}
                <tr><td colspan="5" class="empty">No answer from that servo.</td></tr>
              {:else}
                {#each [["Calibration", CAL], ["Control gains", GAINS]] as group}
                  <tr class="grp"><td colspan="5">{group[0]}</td></tr>
                  {#each group[1] as p}
                    <tr>
                      <td class="addr">{pad(p.a)}</td>
                      <td>
                        <div class="pn">{p.n}</div>
                        <div class="ph">
                          {p.h}<span class="phact"> · actual {actual[p.a] ?? "—"}</span>
                          {#if p.d !== undefined}<span class="phdef"> · default {p.d}</span>{/if}
                        </div>
                      </td>
                      <td class="n"><input type="number" step={p.s} bind:value={vals[p.a]} /></td>
                      <td class="n act">{actual[p.a] ?? "—"}</td>
                      <td class="n"><button class="b sm on" onclick={() => applyParam(p.a)}>Set</button></td>
                    </tr>
                  {/each}
                {/each}
              {/if}
            </tbody>
          </table>
          <div class="stack">
            <button class="b out" onclick={refreshSel} disabled={!has}>Reload</button>
            <button class="b out" onclick={curDefaults} disabled={!has}>★ Tuned defaults</button>
            <button class="b on" onclick={() => saveFlash(false)} disabled={!has}>Save to Flash</button>
            <button class="b warn" onclick={factoryReset} disabled={!has}>Factory Reset</button>
          </div>
          <p class="note">
            <b>Set</b> writes to the board's RAM only — use <b>Save to Flash</b> to keep values after power-off.
          </p>
        </div>
      </div>

      {#if showGraph}
        <div class="card gpanel">
          <div class="hd">Live scope <span class="sp">{ginfo}</span></div>
          <div class="gtools">
            <button class="b" class:on={!grun} class:warn={grun} onclick={gStartStop}>
              {grun ? "■ Stop" : "▶ Start"}
            </button>
            <button class="b out" onclick={gReset}>↺ Reset</button>
            <button class="b out" onclick={capturePng}>Capture PNG</button>
            <button class="b out" onclick={saveCsv}>Save CSV</button>
            <span class="sp">20 s window</span>
          </div>
          <div class="plot">
            <h4>Angle <span class="k"><i></i>measured</span><span class="k"><i class="ref"></i>goal (reference)</span></h4>
            <canvas bind:this={cvA} onmousemove={(e) => onHover(e, "A")} onmouseleave={offHover}></canvas>
          </div>
          <div class="plot last">
            <h4>Current <span class="k"><i></i>measured</span><span class="k"><i class="ref"></i>cap (reference)</span></h4>
            <canvas bind:this={cvB} onmousemove={(e) => onHover(e, "B")} onmouseleave={offHover}></canvas>
          </div>
          <div class="tip" bind:this={tipEl} class:show={tipShow}
               style="left:{tipX}px;top:{tipY}px">{@html tipHtml}</div>
        </div>
      {/if}
    </div>

    <!-- ── Direct control + live ── -->
    <div class="right">
      <div class="card">
        <div class="hd">Degrees</div>
        <div class="bd">
          <div class="big">{Number(deg).toFixed(1)}<small>°</small></div>
          <input class="slider" type="range" min="0" max="270" step="0.5" bind:value={deg} />
          <div class="scale"><span>0</span><span>135 centre</span><span>270</span></div>
          <div class="frow">
            <span>Current cap</span>
            <input type="number" min="0" max="1500" step="10" bind:value={cap} />
            <span class="u">mA</span>
          </div>
          <div class="stack">
            <button class="b on" onclick={() => direct(1)} disabled={!has}>Torque</button>
            <button class="b out" onclick={() => direct(0)} disabled={!has}>Motor off</button>
          </div>
          <p class="note">
            Raw AT32 angle — 135° is mechanical centre. This bypasses the gait and its IK limits, so start with a low cap.
          </p>
        </div>
      </div>

      <div class="card">
        <div class="hd">Live <span class="sp">
          <button class="b sm" class:on={poll} class:out={!poll} onclick={togglePoll} disabled={!has}>
            {poll ? "Stop" : "Poll"}
          </button>
        </span></div>
        <div class="bd">
          <dl class="live">
            {#if live}
              {#each LIVE_ROWS as row}
                {#if row[0] in live}
                  <dt>{row[1]}</dt>
                  <dd>{row[0] === "mode" ? (MODES[live[row[0]]] ?? live[row[0]]) : fmt(live[row[0]], row[3])}{#if row[2]}<small>{row[2]}</small>{/if}</dd>
                {/if}
              {/each}
            {:else}
              <dt>—</dt><dd>no data</dd>
            {/if}
          </dl>
        </div>
      </div>
    </div>
  </div>

  <!-- ══ Set-all modal ════════════════════════════════════════ -->
  {#if saOpen}
    <!-- svelte-ignore a11y_click_events_have_key_events -->
    <!-- svelte-ignore a11y_no_static_element_interactions -->
    <div class="saback" onclick={(e) => { if (e.target === e.currentTarget) saOpen = false; }}>
      <div class="samodal">
        <div class="satop">
          <div>
            <h3>Set All Servos</h3>
            <p>write the ticked parameters to all 12 servos at once</p>
          </div>
          <button class="x" onclick={() => saOpen = false} aria-label="Close">×</button>
        </div>
        <div class="sabody">
          <div class="stack">
            <button class="b out sm" onclick={() => PARAMS.forEach((p) => saTick[p.a] = true)}>Tick all</button>
            <button class="b out sm" onclick={() => PARAMS.forEach((p) => saTick[p.a] = false)}>Untick all</button>
            <button class="b out sm" onclick={() => PARAMS.forEach((p) => saTick[p.a] = !p.cal)}>Gains only</button>
            <button class="b out sm" onclick={saFromCur}>↻ From selected</button>
            <button class="b sm on" onclick={saDefaults}>★ Tuned defaults</button>
          </div>
          <p class="note">
            <b>★ Tuned defaults</b> loads the gains this robot ships with —
            Kp&nbsp;{PARAMS[5].d} · Kd&nbsp;{PARAMS[6].d} ·
            Kp&nbsp;current&nbsp;{PARAMS[7].d} · Kff&nbsp;current&nbsp;{PARAMS[8].d}
            — and ticks only those four. Nothing is written until you press Apply.
          </p>
          <table>
            <thead><tr>
              <th style="width:28px"></th><th class="addr">Addr</th><th>Parameter</th>
              <th class="n">Value</th><th class="n">Result</th>
            </tr></thead>
            <tbody>
              {#each [["Calibration — per servo", CAL], ["Control gains", GAINS]] as group}
                <tr class="grp"><td colspan="5">{group[0]}</td></tr>
                {#each group[1] as p}
                  <tr>
                    <td><input class="chk" type="checkbox" bind:checked={saTick[p.a]} /></td>
                    <td class="addr">{pad(p.a)}</td>
                    <td><div class="pn">{p.n}</div><div class="ph">{p.h}</div></td>
                    <td class="n"><input type="number" step={p.s} bind:value={saVal[p.a]} /></td>
                    <td class="n act">{saRes[p.a]}</td>
                  </tr>
                {/each}
              {/each}
            </tbody>
          </table>
          <p class="note">
            Calibration values are per-servo and are <b>unticked by default</b>.
            Ticking them overwrites each servo's own calibration.
          </p>
        </div>
        <div class="safoot">
          <button class="b on" onclick={saApply} disabled={!!saBusy}>{saBusy || "Apply to all 12"}</button>
          <label class="savelbl"><input class="chk" type="checkbox" bind:checked={saSave} /> save to flash when done</label>
        </div>
      </div>
    </div>
  {/if}

  <div class="logwrap" class:open={logOpen}>
    <button class="loghead" onclick={() => (logOpen = !logOpen)}>
      <span>Activity</span>
      <span class="logcount">{logLines.length ? logLines.length : ""}</span>
      <span class="logchev">{logOpen ? "\u25be" : "\u25b4"}</span>
    </button>
    {#if logOpen}
      <div class="logbody">
        {#if logLines.length === 0}
          <div class="logempty">Nothing yet. Connect, pick a servo, then Set a value.</div>
        {:else}
          {#each logLines as l}
            <div class="logline" class:bad={l.bad}>
              <span class="logt">{l.t}</span><span>{l.m}</span>
            </div>
          {/each}
        {/if}
      </div>
    {/if}
  </div>

  {#if toastMsg}<div class="toast" class:bad={toastBad}>{toastMsg}</div>{/if}
</div>

<style>
  /* Palette is black + yellow only. State is carried by yellow plus a word,
     never by colour alone — which also keeps it colour-blind safe. */
  .studio{
    /* Mapped onto the app tokens (app.css) so the studio follows the theme;
       the names below are the ones the rest of this stylesheet uses. */
    --y: var(--accent); --y2: var(--accent-line); --y-wash: var(--accent-soft);
    --ink: var(--bg); --pane: var(--surface-1); --well: var(--surface-2); --sunk: var(--bg);
    --line2: var(--line-strong);
    --mute: var(--text-2); --dim: var(--text-3);
    --mono: var(--font-mono);
    position: absolute;inset: 0;display: flex;flex-direction: column;
    background: var(--ink);color: var(--text-1);font-size: 13px;line-height: 1.45;
    font-family: Inter,"SF Pro Display",-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;
  }
  button{cursor: pointer;border: none;background: none;color: inherit;font: inherit}
  button:disabled{cursor: not-allowed;opacity: .3}

  /* ── Top bar ── */
  .bar{
    flex: none;display: flex;align-items: center;gap: 9px;flex-wrap: wrap;
    padding: 10px 14px;background: var(--pane);border-bottom: 1px solid var(--line);
  }
  .back{
    width: 32px;height: 32px;border-radius: 9px;border: 1px solid var(--line2);background: var(--well);
    display: grid;place-items: center;flex: none;
  }
  .brand{margin-right: 4px;min-width: 0}
  .brand h1{font-size: 13.5px;font-weight: 750;letter-spacing: -.015em;white-space: nowrap}
  .brand h1 em{font-style: normal;color: var(--y)}
  .brand p{font-size: 8.5px;font-weight: 700;letter-spacing: .19em;text-transform: uppercase;color: var(--dim)}
  .btngrp{display: flex;gap: 6px}
  .sep{width: 1px;height: 20px;background: var(--line2)}
  .banner{margin-left: auto;font-size: 11px;color: var(--mute);display: flex;align-items: center;gap: 7px;white-space: nowrap}
  .dot{width: 7px;height: 7px;border-radius: 50%;background: var(--dim);flex: none}
  .dot.on{background: var(--y);box-shadow: 0 0 0 3px var(--y-wash)}

  /* ── Buttons ── */
  .b{
    display: inline-flex;align-items: center;justify-content: center;padding: 7px 12px;border-radius: 9px;
    background: var(--well);border: 1px solid var(--line2);font-size: 12px;font-weight: 650;white-space: nowrap;
    transition: background .12s,border-color .12s,color .12s;
  }
  .b.on{background: var(--y);color: var(--accent-ink);border-color: var(--y)}
  .b.out{background: transparent;color: var(--mute)}
  .b.warn{background: transparent;color: var(--y2);border-color: rgba(164,151,20,.5)}
  .b.sm{padding: 5px 9px;font-size: 11px;border-radius: 7px}
  .stack{display: flex;gap: 7px;flex-wrap: wrap;margin-top: 12px}

  /* ── Workbench ── */
  .main{flex: 1;display: grid;grid-template-columns: 212px minmax(0,1fr) 262px;min-height: 0}
  .tree,.mid,.right{min-height: 0;overflow-y: auto;-webkit-overflow-scrolling: touch}
  .tree{border-right: 1px solid var(--line);background: var(--pane);padding: 12px 10px 24px}
  .mid{padding: 12px 14px 24px;display: flex;flex-direction: column;gap: 12px}
  .right{border-left: 1px solid var(--line);background: var(--pane);padding: 12px 12px 24px;display: flex;flex-direction: column;gap: 12px}
  /* Stacked layout. grid-auto-rows must be min-content: the grid inherits a
     fixed height from the flex parent, so stretched rows would each take 1/3 of
     the viewport while their content spilled out and overlapped the next
     section. */
  @media(max-width:1000px){
    .main{grid-template-columns: minmax(0,1fr);grid-auto-rows: min-content;
      align-content: start;overflow-y: auto}
    .tree,.mid,.right{overflow: visible;min-height: auto}
    .tree,.right{border: none;border-top: 1px solid var(--line)}
  }


  .lbl{font-size: 8.5px;font-weight: 800;letter-spacing: .18em;text-transform: uppercase;color: var(--dim);
    display: flex;align-items: center;gap: 7px;margin-bottom: 7px}
  .lbl::after{content: "";flex: 1;height: 1px;background: var(--line)}

  /* ── Tree ── */
  .leg{margin-bottom: 12px}
  .srv{
    width: 100%;display: grid;grid-template-columns: 22px 1fr auto;align-items: center;gap: 8px;
    padding: 7px 9px;border-radius: 8px;border: 1px solid transparent;text-align: left;margin-bottom: 3px;
  }
  .srv.on{background: var(--y-wash);border-color: var(--y)}
  .srv.on .sid{background: var(--y);color: var(--accent-ink)}
  .srv.on .snm{color: var(--solid-ink)}
  .srv.on .sdg{color: var(--y)}
  .srv.gone{opacity: .32}
  .sid{width: 22px;height: 22px;border-radius: 6px;background: var(--well);color: var(--mute);
    display: grid;place-items: center;font-size: 10px;font-weight: 800;font-family: var(--mono)}
  .snm{font-size: 11.5px;font-weight: 600;color: var(--mute);white-space: nowrap;overflow: hidden;text-overflow: ellipsis}
  .sdg{font-size: 10.5px;font-family: var(--mono);color: var(--dim);white-space: nowrap}
  .sdg.hot{color: var(--y);font-weight: 700}

  /* ── Cards & tables ── */
  .card{background: var(--pane);border: 1px solid var(--line);border-radius: 12px;overflow: hidden}
  .hd{padding: 11px 14px;border-bottom: 1px solid var(--line);display: flex;align-items: center;gap: 9px;
    font-size: 9px;font-weight: 800;letter-spacing: .18em;text-transform: uppercase;color: var(--mute)}
  .hd::before{content: "";width: 3px;height: 11px;background: var(--y);border-radius: 2px;flex: none}
  .hd .sp{margin-left: auto;letter-spacing: 0;text-transform: none;font-weight: 600;font-size: 11px;color: var(--dim)}
  .bd{padding: 4px 14px 14px}

  table{width: 100%;border-collapse: collapse}
  th{font-size: 8.5px;font-weight: 800;letter-spacing: .16em;text-transform: uppercase;color: var(--dim);
    text-align: left;padding: 9px 7px 7px}
  th.n,td.n{text-align: right}
  td{padding: 4px 7px;border-top: 1px solid var(--line);font-size: 12.5px}
  .addr{font-family: var(--mono);font-size: 11px;color: var(--dim);width: 36px}
  .pn{font-weight: 650}
  .ph{font-size: 10.5px;color: var(--dim)}
  .act{font-family: var(--mono);font-size: 12px;color: var(--y2);width: 74px}
  .grp td{font-size: 8.5px;font-weight: 800;letter-spacing: .16em;text-transform: uppercase;color: var(--y2);
    padding: 11px 7px 5px;background: var(--surface-2)}
  .empty{padding: 24px 8px;text-align: center;color: var(--dim);font-size: 12px;border-top: none}
  input[type=number]{
    width: 88px;padding: 6px 9px;border-radius: 8px;background: var(--sunk);border: 1px solid var(--line2);
    color: var(--solid-ink);font-size: 12.5px;text-align: right;font-family: var(--mono);font-variant-numeric: tabular-nums;
  }
  input[type=number]:focus{outline: none;border-color: var(--y)}
  .chk{width: 15px;height: 15px;accent-color: var(--y)}
  .note{font-size: 11px;color: var(--dim);line-height: 1.5;margin-top: 11px}
  .note b{color: var(--mute);font-weight: 650}

  /* ── Scope ── */
  .gpanel{position: relative}
  .gtools{display: flex;gap: 6px;flex-wrap: wrap;align-items: center;padding: 12px 14px 10px}
  .gtools .sp{margin-left: auto;font-size: 11px;color: var(--dim);font-family: var(--mono)}
  .plot{padding: 0 8px 6px}
  .plot.last{padding-bottom: 12px}
  .plot h4{font-size: 8.5px;font-weight: 800;letter-spacing: .16em;text-transform: uppercase;color: var(--dim);
    padding: 2px 6px 5px;display: flex;gap: 10px;align-items: baseline;flex-wrap: wrap}
  .k{display: inline-flex;align-items: center;gap: 5px;font-weight: 700;letter-spacing: .02em;
    text-transform: none;font-size: 10px;color: var(--mute)}
  .k i{width: 14px;height: 0;border-top: 2px solid var(--y);display: inline-block}
  .k i.ref{border-top: 2px dashed var(--mute)}
  canvas{display: block;width: 100%;height: 150px;border-radius: 9px;background: var(--sunk)}
  .tip{
    position: absolute;pointer-events: none;display: none;z-index: 8;
    background: var(--solid);border: 1px solid var(--line2);border-radius: 8px;padding: 7px 10px;
    font-family: var(--mono);font-size: 11px;white-space: nowrap;
  }
  .tip.show{display: block}

  /* ── Right rail ── */
  .big{font-size: 38px;font-weight: 750;letter-spacing: -.04em;text-align: center;color: var(--y);
    font-family: var(--mono);font-variant-numeric: tabular-nums;margin: 4px 0 2px}
  .big small{font-size: 15px;color: var(--mute);font-weight: 600}
  .slider{-webkit-appearance: none;appearance: none;width: 100%;height: 4px;border-radius: 99px;
    background: var(--line2);margin: 12px 0 6px;outline: none}
  .slider::-webkit-slider-thumb{-webkit-appearance: none;width: 22px;height: 22px;border-radius: 50%;
    background: var(--y);border: 3px solid var(--ink);box-shadow: 0 0 0 1px var(--y)}
  .slider::-moz-range-thumb{width: 18px;height: 18px;border-radius: 50%;background: var(--y);border: 3px solid var(--ink)}
  .scale{display: flex;justify-content: space-between;font-size: 10px;color: var(--dim);font-weight: 650}
  .frow{display: flex;align-items: center;gap: 9px;margin-top: 12px;font-size: 11.5px;font-weight: 650}
  .frow input{width: 72px}
  .frow .u{color: var(--mute);font-weight: 600}

  dl.live{display: grid;grid-template-columns: 1fr auto;gap: 0;margin: 0}
  dl.live dt{font-size: 11px;color: var(--mute);padding: 6px 0;border-top: 1px solid var(--line)}
  dl.live dd{font-size: 12.5px;font-family: var(--mono);text-align: right;padding: 6px 0;border-top: 1px solid var(--line);margin: 0}
  dl.live dd small{color: var(--dim);margin-left: 3px;font-size: 10px}
  dl.live dt:first-of-type,dl.live dd:first-of-type{border-top: none}

  /* ── Modal ── */
  .saback{position: absolute;inset: 0;background: var(--surface-3);display: flex;z-index: 50;
    align-items: center;justify-content: center;padding: 18px}
  .samodal{width: 100%;max-width: 560px;max-height: 88%;display: flex;flex-direction: column;
    background: var(--pane);border: 1px solid var(--line2);border-radius: 14px;overflow: hidden}
  .satop{padding: 14px 16px;border-bottom: 1px solid var(--line);display: flex;align-items: center;gap: 10px}
  .satop h3{font-size: 13px;font-weight: 750}
  .satop p{font-size: 11px;color: var(--dim)}
  .satop .x{margin-left: auto;font-size: 20px;color: var(--mute);line-height: 1}
  .sabody{overflow-y: auto;padding: 0 16px 12px}
  .safoot{padding: 12px 16px;border-top: 1px solid var(--line);display: flex;gap: 9px;align-items: center;flex-wrap: wrap}
  .savelbl{display: flex;align-items: center;gap: 7px;font-size: 11.5px;color: var(--mute);font-weight: 650}

  /* ── Toast ── */
  .toast{
    /* fixed, not absolute: .studio is a flex column that can extend past the
       viewport, so an absolutely-positioned toast ends up below the fold —
       which reads as "there was no message at all". The standalone studio
       page uses fixed for the same reason. */
    position:fixed;left: 50%;bottom: 20px;transform: translateX(-50%);
    background: var(--y);color: var(--accent-ink);padding: 10px 20px;border-radius: 999px;
    font-size: 12.5px;font-weight: 700;box-shadow: 0 10px 34px rgba(0,0,0,.7);
    max-width: 90%;text-align: center;z-index: 60;
  }
  .toast.bad{background: var(--solid);color: var(--y);border: 1px solid var(--y)}

  /* Activity log — docked bottom-right, collapsible, never covers the table. */
  .logwrap{
    position: fixed;right: 14px;bottom: 14px;width: min(340px,calc(100vw - 28px));
    background: var(--pane);border: 1px solid var(--line);border-radius: 12px;
    box-shadow: 0 12px 40px rgba(0,0,0,.6);z-index: 55;overflow: hidden;
  }
  .loghead{
    display: flex;align-items: center;gap: 8px;width: 100%;
    background: var(--well);border: 0;border-bottom: 1px solid var(--line);
    color: var(--mute);font: 600 11.5px/1 var(--mono);letter-spacing: .08em;
    text-transform: uppercase;padding: 9px 12px;cursor: pointer;
  }
  .loghead span:first-child{flex: 1;text-align: left}
  .logcount{color: var(--y)}
  .logchev{color: var(--dim)}
  .logbody{max-height: 168px;overflow: auto;padding: 6px 0}
  .logline{
    display: flex;gap: 9px;padding: 4px 12px;
    font: 12px/1.45 var(--mono);color: var(--text-3);
  }
  .logline.bad{color: var(--y)}
  .logt{color: var(--dim);flex: none}
  .logempty{padding: 10px 12px;color: var(--dim);font-size: 12px}

  /* ── Phone ── */
  .tools{display: flex;align-items: center;gap: 9px;flex-wrap: wrap}
  .rows{display: contents}
  .phact{display: none}
  /* Dim, not yellow: the shipped value is reference text, and yellow in this
     screen means "live" or "acting". */
  .phdef{color: var(--dim);margin-left: 4px}

  @media(max-width:640px){
    .bar{gap: 7px;padding: 9px 12px}
    .brand{margin-right: auto}
    .brand h1{font-size: 12.5px}
    /* One swipeable strip instead of three stacked rows of buttons. */
    .tools{order: 3;width: 100%;flex-wrap: nowrap;overflow-x: auto;gap: 6px;
      padding-bottom: 2px;-webkit-overflow-scrolling: touch;scrollbar-width: none}
    .tools::-webkit-scrollbar{display: none}
    .tools .b{flex: none}
    .sep{display: none}
    .banner{order: 2;margin-left: 0;font-size: 10.5px}

    /* Twelve full-width rows is a long scroll before you reach anything.
       Tile each leg 3-across so the whole robot fits in four short blocks. */
    .rows{display: grid;grid-template-columns: repeat(3,minmax(0,1fr));gap: 6px}
    .srv{grid-template-columns: 1fr;gap: 1px;padding: 8px 9px;margin-bottom: 0;
      background: var(--sunk);border-color: var(--line)}
    .sid{width: 20px;height: 18px;border-radius: 5px;font-size: 9.5px}
    .snm{font-size: 10px}
    .sdg{font-size: 11px;color: var(--solid-ink)}
    .tree{padding: 12px 12px 16px}
    .mid,.right{padding-left: 12px;padding-right: 12px}

    /* Reclaim the width the Actual column was using; it moves under the name. */
    .act{display: none}
    .phact{display: inline;color: var(--y2);margin-left: 4px}
    .addr{width: 28px;font-size: 10px}
    .bd{padding: 4px 10px 14px}
    input[type=number]{width: 74px;padding: 6px 8px}
    td{padding: 5px 4px}
    .big{font-size: 34px}
  }
</style>
