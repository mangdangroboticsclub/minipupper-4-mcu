<script>
  /**
   * MangDang Servo Studio: in-app tuning for the four AT32 driver boards.
   *
   * White, in the app's own look (Claude Design "MangDang Servo Studio"):
   *   phone  servo list -> servo page -> edit sheet / graph / pot calibration
   *   PC     header tools, servo list | settings table or graph | servo card
   *          and editor, pot calibration as a dialog
   * Functions follow minipupperesp's wizard.html; it all runs on the existing
   * /v1/studio/* API in components/http_server/http_server_mpx_studio_api.c.
   */
  let { onBack } = $props();

  /* ── Model ─────────────────────────────────────────────────── */
  /* Mirror of the AT32 sms_config table. onlyMode: shown only in that
     control_mode (0 CURRENT, 1 PWM). d: the gain this robot ships with, in
     AT32 board units. */
  const PARAMS = [
    { a: 0,  n: "Reverse position sensor", k: "reverse_position_sensor", bool: ["normal", "reversed"],
      desc: "Invert the position sensor direction." },
    { a: 1,  n: "Min position ADC", k: "min_position_adc", min: 0, max: 4096, step: 1,
      desc: "ADC reading at the low end of travel. Pot calibration sets this for you." },
    { a: 2,  n: "Max position ADC", k: "max_position_adc", min: 0, max: 4096, step: 1,
      desc: "ADC reading at the high end of travel. Pot calibration sets this for you." },
    { a: 3,  n: "Range position", k: "range_position_deg", min: 0, max: 360, step: 1, unit: "°",
      desc: "Mechanical range of the output shaft between min and max ADC." },
    { a: 4,  n: "Reverse motor", k: "reverse_motor", bool: ["normal", "reversed"],
      desc: "Invert the motor drive direction." },
    { a: 5,  n: "Kp position", k: "kp_position", min: 0, max: 1000, step: 1, d: 65,
      desc: "Position loop proportional gain: the stiffness. Higher holds harder; too high and it shakes." },
    { a: 6,  n: "Kd position", k: "kd_position", min: 0, max: 20000, step: 10, d: 800,
      desc: "Position loop derivative gain: the damping. Higher settles faster with less overshoot; too high and it buzzes." },
    { a: 7,  n: "Kp current", k: "kp_current", min: 0, max: 0.01, step: 0.0001, d: 0.0006, onlyMode: 0,
      desc: "Current loop proportional gain (CURRENT mode only)." },
    { a: 8,  n: "Kff current", k: "kff_current", min: 0, max: 0.01, step: 0.00001, d: 0.00022, onlyMode: 0,
      desc: "Current loop feed-forward: PWM duty per mA of current setpoint (CURRENT mode only)." },
    { a: 9,  n: "Max PWM duty", k: "max_pwm_duty_cycle", min: 0, max: 1, step: 0.01,
      desc: "PWM duty limit, 0 to 1 (1 = full battery voltage)." },
    { a: 10, n: "Ki position", k: "ki_position", min: 0, max: 5000, step: 10, onlyMode: 1,
      desc: "Position loop integral gain (PWM mode only). 0 = off." },
    { a: 11, n: "Control mode", k: "control_mode", bool: ["CURRENT mode", "PWM mode"],
      desc: "CURRENT mode (0): position PD, then the current loop, the original algorithm. PWM mode (1): position PID straight to PWM, quiet. Switches inside the AT32, no reflash." },
    { a: 12, n: "PWM scale", k: "pwm_scale", min: 0, max: 0.01, step: 0.00001, onlyMode: 1,
      desc: "PWM duty per unit of position-PID output (0.001 means PID output 1000 = 100 % PWM). Stiffness = Kp position x PWM scale (PWM mode only)." },
  ];
  const TUNED = PARAMS.filter((p) => p.d !== undefined);

  /* Live keys exactly as /v1/studio/live sends them. */
  const LIVE = [
    { k: "now_deg", a: 100, n: "Present position", u: "°", dp: 1 },
    { k: "set_deg", a: 101, n: "Goal position", u: "°", dp: 1 },
    { k: "err_deg", a: 102, n: "Position error", u: "°", dp: 1 },
    { k: "now_ma",  a: 103, n: "Present current", u: "mA", dp: 0 },
    { k: "cap_ma",  a: 104, n: "Current limit", u: "mA", dp: 0 },
    { k: "set_ma",  a: 105, n: "Current setpoint", u: "mA", dp: 0 },
    { k: "err_ma",  a: 106, n: "Current error", u: "mA", dp: 0 },
    { k: "duty",    a: 107, n: "PWM duty", u: "%", dp: 1, scale: 100 },
    { k: "pos_adc", a: 108, n: "Position ADC (raw)", u: "", dp: 0 },
    { k: "cur_adc", a: 109, n: "Current ADC (raw)", u: "", dp: 0 },
    { k: "mode",    a: 110, n: "Operating mode", u: "", dp: 0 },
    { k: "loop",    a: 111, n: "Loop counter", u: "", dp: 0 },
    { k: "temp_c",  a: 112, n: "Motor temperature", u: "°C", dp: 1 },
  ];
  const MODES = ["IDLE", "POSITION", "TORQUE", "IK"];

  /* Board connector per leg, as printed on the robot (driver_board.c). */
  const LEGS = [
    { name: "Front right", cn: "CN4", ids: [1, 2, 3] },
    { name: "Front left",  cn: "CN5", ids: [4, 5, 6] },
    { name: "Rear right",  cn: "CN3", ids: [7, 8, 9] },
    { name: "Rear left",   cn: "CN6", ids: [10, 11, 12] },
  ];
  const JOINT = ["Hip", "Thigh", "Calf"];
  const legOf = (id) => LEGS[Math.floor((id - 1) / 3)];
  const servoName = (id) => `${legOf(id).name} ${JOINT[(id - 1) % 3].toLowerCase()}`;
  const pad3 = (n) => String(n).padStart(3, "0");

  /* ── Small helpers ─────────────────────────────────────────── */
  const isNum = (v) => typeof v === "number" && isFinite(v);
  /* Board values arrive as floats (0.0006 -> 0.000600000028); six
     significant digits gives back what was written. */
  const fmtV = (v) => (v === "mixed" ? "mixed" : isNum(v) ? String(+v.toPrecision(6)) : "—");
  function actText(P, v) {
    if (v === "mixed") return "differs per servo";
    if (!isNum(v)) return v === null ? "no reply" : "";
    if (P.bool) return P.bool[v >= 0.5 ? 1 : 0];
    if (P.unit) return fmtV(v) + P.unit;
    return "";
  }
  function liveText(L, d) {
    const v = d ? d[L.k] : undefined;
    if (!isNum(v)) return "—";
    if (L.k === "mode") return MODES[v] ?? String(v);
    const s = (L.scale ? v * L.scale : v).toFixed(L.dp).replace(/^-(0(\.0+)?)$/, "$1");
    return L.u ? `${s} ${L.u}`.replace(" °", "°") : s;
  }
  const asText = (x) => (x === null || x === undefined ? "" : String(x).trim());

  /* ── State ─────────────────────────────────────────────────── */
  let wide = $state(false);
  let online = $state(true);
  let cli = $state(false);             // studio mode on = "Connected", gait paused
  let found = $state(null);            // ids that answered the last scan, or null
  let sel = $state(0);                 // 0 none, -1 all servos, 1..12 one servo
  let view = $state("list");           // phone: list | servo | graph
  let tab = $state("table");           // PC middle: table | graph
  let cal = $state(false);             // pot calibration open
  let vals = $state({});               // addr -> number | null | "mixed"
  let loading = $state("");
  let live = $state(null);
  let poll = $state(true);
  let editP = $state(null);            // address being edited
  let draft = $state("");
  let edMsg = $state(""), edBad = $state(false);
  let resetArm = $state(false), tunedArm = $state(false);

  const cur = $derived(sel > 0 ? sel : 0);
  const allMode = $derived(sel === -1);
  const ctlMode = $derived(isNum(vals[11]) ? (vals[11] >= 0.5 ? 1 : 0) : null);
  const shownParams = $derived(PARAMS.filter((P) =>
    P.onlyMode === undefined || ctlMode === null || ctlMode === P.onlyMode));
  const answering = $derived(found ? found.length : null);
  const torqueOn = $derived(live && (live.mode === 1 || live.mode === 2));

  $effect(() => {
    const mq = window.matchMedia("(min-width: 1000px)");
    const f = () => (wide = mq.matches);
    f();
    mq.addEventListener("change", f);
    return () => mq.removeEventListener("change", f);
  });

  /* ── Toast + activity ──────────────────────────────────────── */
  let toastMsg = $state(""), toastBad = $state(false), toastTimer;
  let logLines = $state([]);
  let logOpen = $state(false);
  function toast(m, bad = false) {
    toastMsg = m; toastBad = bad;
    const t = new Date().toLocaleTimeString([], { hour12: false });
    logLines = [{ t, m, bad }, ...logLines].slice(0, 40);
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => (toastMsg = ""), 2600);
  }

  /* ── One request at a time ─────────────────────────────────────
   * The robot's HTTP server has five sockets and LRU-purges idle ones. The
   * app shell already holds some, so this screen never runs two requests at
   * once (one socket, never racing its own poll) and retries a network-level
   * failure exactly once, because a purged keep-alive socket fails instantly
   * and the retry opens a fresh one. Polls are not retried and never hold
   * up a write. */
  let chain = Promise.resolve();
  let inflight = 0;
  const isPoll = (p) => p.startsWith("/v1/studio/live") || p === "/v1/studio/mode";

  async function rawGet(path) {
    const r = await fetch(path, { cache: "no-store" });
    if (!r.ok) throw new Error("HTTP " + r.status);
    return r.json();
  }
  function jget(path) {
    const pl = isPoll(path);
    if (!pl) inflight++;
    const run = chain.then(async () => {
      try {
        return await rawGet(path);
      } catch (e) {
        if (pl) throw e;
        await new Promise((r) => setTimeout(r, 120));
        return rawGet(path);
      }
    }).finally(() => { if (!pl) inflight--; });
    chain = run.catch(() => {});
    return run;
  }

  /* Lend the app shell's permission WebSocket back to the robot while this
     screen is open: it is one of the five sockets and worth nothing here. */
  $effect(() => {
    window.dispatchEvent(new Event("mpx:yield-socket"));
    return () => window.dispatchEvent(new Event("mpx:resume-socket"));
  });

  /* ── Connection ────────────────────────────────────────────── */
  async function pollMode() {
    if (inflight) return;
    try {
      const d = await jget("/v1/studio/mode");
      online = true;
      if (d.studio !== cli) {
        cli = d.studio;
        if (cli && found === null) scan(true);
      }
    } catch { online = false; }
  }
  $effect(() => {
    pollMode();
    const t = setInterval(pollMode, 1500);
    return () => clearInterval(t);
  });

  // Never leave the gait parked because the screen closed.
  $effect(() => () => {
    if (swRun) { try { fetch(`/v1/studio/direct?id=${cur}&m=0&deg=0&cur=0`); } catch {} }
    if (cli && navigator.sendBeacon) navigator.sendBeacon("/v1/studio/mode?on=0");
  });

  async function toggleConn() {
    try {
      const d = await jget("/v1/studio/mode?on=" + (cli ? 0 : 1));
      cli = d.studio;
      toast(cli ? "Connected, gait paused" : "Disconnected, gait resumed");
      if (cli) { await scan(true); reread(); }
      else { live = null; grun = false; }
    } catch { toast("Could not change mode", true); }
  }

  async function scan(quiet = false) {
    if (!cli) return toast("Turn on Connect first", true);
    try {
      const d = await jget("/v1/studio/scan");
      if (!d.ok) return toast(d.err || "Scan failed", true);
      found = d.found;
      if (!quiet || d.found.length !== 12)
        toast(`${d.found.length} of 12 servos answered`, d.found.length !== 12);
    } catch { toast("Scan failed", true); }
  }

  /* ── Selection + reading ───────────────────────────────────── */
  function openServo(id) {
    if (cur !== id) gReset();
    sel = id; editP = null; resetArm = false; tunedArm = false;
    live = null; vals = {};
    if (!wide) { view = "servo"; scrollTop(); }
    if (cli) readOne();
  }
  function openAll() {
    sel = -1; editP = null; resetArm = false; tunedArm = false;
    live = null; vals = {}; grun = false;
    if (wide && tab === "graph") tab = "table";
    if (!wide) { view = "servo"; scrollTop(); }
    if (cli) readAll();
  }
  function step(dir) {
    const base = cur || 1;
    openServo(((base - 1 + dir + 12) % 12) + 1);
  }
  let rootEl = $state(null);
  const scrollTop = () => rootEl && rootEl.scrollTo({ top: 0 });

  function reread() { if (allMode) readAll(); else if (cur) readOne(); }

  async function readOne() {
    if (!cli || !cur) return;
    const id = cur;
    loading = "Reading…";
    try {
      const d = await jget("/v1/studio/dump?id=" + id);
      if (id !== cur) return;
      if (!d.ok) { vals = {}; toast(d.err || "Read failed", true); return; }
      const v = {};
      for (const p of d.params) v[p.p] = isNum(p.v) ? p.v : null;
      vals = v;
    } catch (e) { toast("Read failed: " + (e?.message || e), true); }
    finally { if (id === cur) loading = ""; }
  }

  /* All servos: read each one that answers and show a value only where they
     agree, so a write here never hides a servo that was set differently. */
  async function readAll() {
    if (!cli) return;
    const ids = found && found.length ? found : [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12];
    const merged = {};
    let n = 0;
    for (const id of ids) {
      if (!allMode) return;
      loading = `Reading ${++n} of ${ids.length}…`;
      try {
        const d = await jget("/v1/studio/dump?id=" + id);
        if (!d.ok) continue;
        for (const p of d.params) {
          if (!isNum(p.v)) continue;
          const v = +p.v.toPrecision(6);
          if (!(p.p in merged)) merged[p.p] = v;
          else if (merged[p.p] !== v) merged[p.p] = "mixed";
        }
      } catch { /* one servo missing is fine */ }
    }
    if (allMode) { vals = merged; loading = ""; }
  }

  /* ── Editing ───────────────────────────────────────────────── */
  function openEdit(a) {
    if (!cli) return toast("Turn on Connect first", true);
    if (!cur && !allMode) return;
    editP = a;
    const v = vals[a];
    draft = isNum(v) ? fmtV(v) : "";
    edMsg = ""; edBad = false;
  }
  function closeEdit() { editP = null; }
  function nudge(P, dir) {
    let v = parseFloat(draft);
    if (!isFinite(v)) v = isNum(vals[P.a]) ? vals[P.a] : P.min;
    v = Math.min(P.max, Math.max(P.min, v + dir * P.step));
    const dp = Math.max(0, -Math.floor(Math.log10(P.step) + 1e-9));
    draft = String(+v.toFixed(dp));
  }

  async function writeParam(a, text) {
    const P = PARAMS[a];
    const v = parseFloat(asText(text));
    if (!isFinite(v)) { edMsg = "Enter a number first"; edBad = true; return false; }
    if (!P.bool && (v < P.min || v > P.max)) {
      edMsg = `Out of range (${P.min} to ${P.max})`; edBad = true; return false;
    }
    if (!cli) { edMsg = "Turn on Connect first; the gait still owns the bus"; edBad = true; return false; }
    edMsg = allMode ? "Writing to every servo…" : "Writing…"; edBad = false;
    try {
      const enc = encodeURIComponent(asText(text));
      const d = allMode
        ? await jget(`/v1/studio/setall?p=${a}&v=${enc}`)
        : await jget(`/v1/studio/set?id=${cur}&p=${a}&v=${enc}`);
      if (!d.ok) { edMsg = "Not written: " + (d.err || "no servo answered"); edBad = true; return false; }
      if (allMode) {
        vals[a] = +Number(d.v).toPrecision(6);
        const miss = d.fail && d.fail.length ? `, no reply from ${d.fail.join(", ")}` : "";
        edMsg = `Written to ${d.n} servos${miss}. In RAM until you save.`;
        edBad = !!miss;
        toast(`${P.n} = ${fmtV(d.v)} on ${d.n} servos`, !!miss);
      } else {
        vals[a] = d.v;
        edMsg = `Board reads back ${fmtV(d.v)}. In RAM until you save.`;
        toast(`${P.n} = ${fmtV(d.v)}`);
      }
      return true;
    } catch (e) {
      edMsg = "Write failed: " + (e?.message || e); edBad = true;
      toast("Write failed", true);
      return false;
    }
  }

  async function writeTuned() {
    if (!tunedArm) { tunedArm = true; return; }
    tunedArm = false;
    let bad = 0;
    for (const P of TUNED) if (!(await writeParam(P.a, String(P.d)))) bad++;
    toast(bad ? `${bad} gain(s) not written` : "Tuned gains written, in RAM until you save", !!bad);
  }

  async function saveFlash(all) {
    if (!cli) return toast("Turn on Connect first", true);
    try {
      const d = await jget("/v1/studio/save?id=" + (all ? 0 : cur));
      toast(d.ok ? (all ? "All four boards saved to flash" : "Saved to flash") : (d.err || "Save failed"), !d.ok);
    } catch { toast("Save failed", true); }
  }

  async function factoryReset() {
    if (!cli) return toast("Turn on Connect first", true);
    if (!resetArm) { resetArm = true; return; }
    resetArm = false;
    try {
      const d = await jget("/v1/studio/restore?id=" + (allMode ? 0 : cur));
      toast(d.ok ? "Factory defaults in RAM, save to keep" : (d.err || "Reset failed"), !d.ok);
      if (d.ok) reread();
    } catch { toast("Reset failed", true); }
  }

  async function toggleTorque() {
    if (!cli || !cur) return toast("Connect and pick a servo first", true);
    try {
      if (torqueOn) {
        const d = await jget(`/v1/studio/direct?id=${cur}&m=0&deg=0&cur=0`);
        toast(d.ok ? "Motor off" : (d.err || "Refused"), !d.ok);
        if (d.ok && live) live = { ...live, mode: 0 };
      } else {
        const known = live && isNum(live.now_deg);
        const deg = known ? live.now_deg.toFixed(1) : "135";
        const d = await jget(`/v1/studio/direct?id=${cur}&m=1&deg=${deg}&cur=200`);
        toast(d.ok ? `Holding ${deg}°` : (d.err || "Refused"), !d.ok);
        if (d.ok && live) live = { ...live, mode: 1 };
      }
    } catch { toast("Command failed", true); }
  }

  /* ── Live poll: 2.5 Hz, full rate while graphing or calibrating ── */
  let liveBusy = false, liveMiss = 0, tick = 0;
  $effect(() => {
    const t = setInterval(() => {
      tick++;
      const fast = grun || swRun || cal;
      if (fast || (poll && tick % 4 === 0)) tickLive();
    }, 100);
    return () => clearInterval(t);
  });
  async function tickLive() {
    if (!cli || !cur || liveBusy || inflight) return;
    liveBusy = true;
    const id = cur;
    try {
      const d = await jget("/v1/studio/live?id=" + id);
      if (id !== cur) return;
      if (!d.ok) { if (++liveMiss >= 5) { liveMiss = 0; poll = false; toast(d.err || "Live read failed", true); } return; }
      liveMiss = 0;
      live = d;
      gPush(d);
      calSample(d);
    } catch {
      if (++liveMiss >= 5) { liveMiss = 0; poll = false; grun = false; toast("Live read failed", true); }
    } finally { liveBusy = false; }
  }

  /* ── Graph ─────────────────────────────────────────────────── */
  const TRACES = [
    { k: "now_deg", n: "Position",     c: "#111111", dash: "",    ax: 0 },
    { k: "set_deg", n: "Goal",         c: "#A39F96", dash: "5 5", ax: 0 },
    { k: "err_deg", n: "Error",        c: "#C2410C", dash: "",    ax: 0 },
    { k: "now_ma",  n: "Current",      c: "#2563EB", dash: "",    ax: 1 },
    { k: "cap_ma",  n: "Current cap",  c: "#2563EB", dash: "5 5", ax: 1 },
    { k: "duty",    n: "PWM duty ×10", c: "#15803D", dash: "",    ax: 1, scale: 1000 },
  ];
  let tOn = $state([true, true, false, true, false, false]);
  let grun = $state(false);
  let gbuf = [];                        // plain on purpose; gver triggers redraws
  let gver = $state(0), gt0 = 0;
  let ginfo = $state("");
  let gw = $state(340);
  const GWIN = 15000, GMAX = 6000;
  let gsvg = $state(null);

  function gStartStop() {
    if (grun) { grun = false; return; }
    if (!cli || !cur) return toast("Connect and pick a servo first", true);
    if (!gbuf.length) gt0 = Date.now();
    grun = true;
  }
  function gReset() { gbuf = []; gt0 = Date.now(); ginfo = ""; gver++; }
  function gPush(d) {
    if (!grun) return;
    const pt = { t: Date.now() - gt0 };
    for (const T of TRACES) if (isNum(d[T.k])) pt[T.k] = d[T.k];
    gbuf.push(pt);
    if (gbuf.length > GMAX) gbuf.shift();
    const inWin = gbuf.filter((p) => p.t >= pt.t - GWIN).length;
    ginfo = `Recording · ${gbuf.length} samples · ${(inWin / Math.min(GWIN, Math.max(pt.t, 1)) * 1000).toFixed(0)} Hz`;
    gver++;
  }
  function niceScale(mn, mx, parts) {
    if (!isFinite(mn) || !isFinite(mx)) { mn = 0; mx = 1; }
    if (mn === mx) { mn -= 1; mx += 1; }
    const span = mx - mn;
    let st = Math.pow(10, Math.floor(Math.log10(span / parts)));
    const e = span / parts / st;
    st *= e >= 5 ? 5 : e >= 2 ? 2 : 1;
    return { min: Math.floor(mn / st) * st, max: Math.ceil(mx / st) * st, step: st };
  }
  const gh = $derived(wide ? 430 : 300);
  const gm = $derived.by(() => {
    gver;
    const W = Math.max(200, gw), H = gh;
    const L = 40, R = 44, T = 18, B = 22, pw = W - L - R, ph = H - T - B;
    const out = { W, H, L, R, T, B, grid: [], xt: [], paths: [], empty: !gbuf.length };
    if (!gbuf.length) return out;
    const tmax = gbuf[gbuf.length - 1].t, tmin = Math.max(0, tmax - GWIN);
    const win = gbuf.filter((p) => p.t >= tmin);
    const ax = [{ mn: Infinity, mx: -Infinity }, { mn: Infinity, mx: -Infinity }];
    TRACES.forEach((Tr, i) => {
      if (!tOn[i]) return;
      for (const p of win) {
        if (p[Tr.k] === undefined) continue;
        const v = p[Tr.k] * (Tr.scale || 1), a = ax[Tr.ax];
        if (v < a.mn) a.mn = v; if (v > a.mx) a.mx = v;
      }
    });
    if (!isFinite(ax[0].mn)) { ax[0].mn = 0; ax[0].mx = 270; }
    if (!isFinite(ax[1].mn)) { ax[1].mn = 0; ax[1].mx = 250; }
    const s0 = niceScale(ax[0].mn, ax[0].mx, 5), s1 = niceScale(ax[1].mn, ax[1].mx, 5);
    const Y0 = (v) => T + ph - (v - s0.min) / (s0.max - s0.min) * ph;
    const Y1 = (v) => T + ph - (v - s1.min) / (s1.max - s1.min) * ph;
    const X = (t) => L + (t - tmin) / Math.max(1, tmax - tmin) * pw;
    const n = Math.round((s0.max - s0.min) / s0.step);
    for (let i = 0; i <= n; i++) {
      const y = T + ph - (i / n) * ph;
      const r = s1.min + (i / n) * (s1.max - s1.min);
      out.grid.push({ y: y.toFixed(1), l: String(+(s0.min + i * s0.step).toFixed(4)), r: String(+r.toFixed(1)) });
    }
    TRACES.forEach((Tr, i) => {
      if (!tOn[i]) return;
      let d = "", started = false;
      for (const p of win) {
        if (p[Tr.k] === undefined) { started = false; continue; }
        const v = p[Tr.k] * (Tr.scale || 1);
        const y = Tr.ax ? Y1(v) : Y0(v);
        d += (started ? "L" : "M") + X(p.t).toFixed(1) + "," + y.toFixed(1);
        started = true;
      }
      out.paths.push({ d, c: Tr.c, dash: Tr.dash, w: i === 0 ? 2.4 : 2 });
    });
    out.from = `${(tmin / 1000).toFixed(1)} s`;
    out.to = `${(tmax / 1000).toFixed(1)} s`;
    return out;
  });

  function download(name, href) {
    const l = document.createElement("a");
    l.download = name; l.href = href; l.click();
  }
  function gCapture() {
    if (!gsvg) return;
    const xml = new XMLSerializer().serializeToString(gsvg);
    const img = new Image();
    img.onload = () => {
      const c = document.createElement("canvas"), s = 2;
      c.width = gm.W * s; c.height = gm.H * s;
      const x = c.getContext("2d");
      x.fillStyle = "#FFFFFF"; x.fillRect(0, 0, c.width, c.height);
      x.drawImage(img, 0, 0, c.width, c.height);
      download(`servo${pad3(cur)}-graph.png`, c.toDataURL("image/png"));
      toast("Graph picture saved");
    };
    img.src = "data:image/svg+xml;charset=utf-8," + encodeURIComponent(xml);
  }
  function gSaveCsv() {
    if (!gbuf.length) return toast("Nothing recorded yet", true);
    const cols = ["t_ms", ...TRACES.map((T) => T.k)];
    let s = cols.join(",") + "\n";
    for (const p of gbuf) s += cols.map((k) => { const v = k === "t_ms" ? p.t : p[k]; return v === undefined ? "" : v; }).join(",") + "\n";
    download(`servo${pad3(cur)}-graph.csv`, URL.createObjectURL(new Blob([s], { type: "text/csv" })));
    toast(gbuf.length + " samples saved");
  }
  let csvIn = $state(null);
  function gLoadCsv(ev) {
    const f = ev.currentTarget.files[0];
    if (!f) return;
    const rd = new FileReader();
    rd.onload = () => {
      const lines = String(rd.result).split(/\r?\n/), hdr = lines[0].split(",");
      const buf = [];
      for (let i = 1; i < lines.length; i++) {
        if (!lines[i]) continue;
        const cel = lines[i].split(","), pt = {};
        hdr.forEach((h, j) => { if (cel[j] !== "" && cel[j] !== undefined) pt[h === "t_ms" ? "t" : h] = parseFloat(cel[j]); });
        if (pt.t !== undefined) buf.push(pt);
      }
      grun = false; gbuf = buf; gver++;
      ginfo = `Loaded ${buf.length} samples from ${f.name}`;
    };
    rd.readAsText(f);
    ev.currentTarget.value = "";
  }

  /* ── Pot calibration (minipupperesp wizard) ────────────────── */
  let cA = $state("0"), cAadc = $state(""), cB = $state("90"), cBadc = $state("");
  let cCpd = $state(""), cLo = $state(""), cHi = $state(""), cMg = $state("20");
  let cTab = $state("auto"), cCur = $state("120"), cDur = $state("14");
  let calAdc = $state(null), calMsg = $state(""), calBad = $state(false);
  let recOn = $state(false), recMin = null, recMax = null;
  let swRun = $state(false), swData = $state.raw([]), swStats = $state("");
  let swStop = null, swE = 0;
  const SW_WRAP = 700;

  function openCal() {
    if (!cli) return toast("Turn on Connect first", true);
    if (!cur) return toast("Pick one servo first", true);
    if (isNum(vals[1]) && !cLo) cLo = String(Math.round(vals[1]));
    if (isNum(vals[2]) && !cHi) cHi = String(Math.round(vals[2]));
    if (isNum(vals[1]) && isNum(vals[2]) && vals[3] && !cCpd)
      cCpd = ((vals[2] - vals[1]) / vals[3]).toFixed(2);
    calMsg = ""; calBad = false;
    cal = true;
    if (!wide) scrollTop();
  }
  function closeCal() {
    if (swRun) swFinish("closed");
    recOn = false;
    cal = false;
  }
  function calSample(d) {
    if (!cal || !isNum(d.pos_adc)) return;
    calAdc = d.pos_adc;
    if (recOn) {
      if (recMin === null || calAdc < recMin) recMin = calAdc;
      if (recMax === null || calAdc > recMax) recMax = calAdc;
      cLo = String(Math.round(recMin)); cHi = String(Math.round(recMax));
    }
    if (swRun) swData = [...swData, { t: Date.now() - swT0, a: d.pos_adc }];
  }
  let swT0 = 0;
  const grab = (which) => {
    if (calAdc === null) return toast("No reading yet", true);
    if (which === "A") cAadc = String(Math.round(calAdc)); else cBadc = String(Math.round(calAdc));
  };
  function calCpd() {
    const a = parseFloat(cA), b = parseFloat(cB), aa = parseFloat(cAadc), ba = parseFloat(cBadc);
    if (![a, b, aa, ba].every(isFinite) || a === b) {
      calMsg = "Need two different angles and an ADC reading for each"; calBad = true; return;
    }
    const c = Math.abs(ba - aa) / Math.abs(b - a);
    cCpd = c.toFixed(2);
    calMsg = `${c.toFixed(2)} counts per degree, full electrical travel ${(4096 / c).toFixed(1)}°`; calBad = false;
  }
  function calRec() {
    recOn = !recOn;
    if (recOn) { recMin = recMax = null; calMsg = "Recording: turn the horn through its whole range"; calBad = false; }
    else calMsg = `Captured ${cLo} to ${cHi} counts`;
  }
  async function motorOff() {
    if (!cur) return;
    try { await jget(`/v1/studio/direct?id=${cur}&m=0&deg=0&cur=0`); toast(`Servo ${pad3(cur)} motor off`); }
    catch { toast("Command failed", true); }
  }
  const calc = $derived.by(() => {
    const cpd = parseFloat(cCpd);
    let lo = parseFloat(cLo), hi = parseFloat(cHi), m = parseFloat(cMg);
    if (!isFinite(m)) m = 0;
    if (!(cpd > 0) || !isFinite(lo) || !isFinite(hi)) return null;
    if (lo > hi) [lo, hi] = [hi, lo];
    const r = { cpd, lo, hi, m, minCfg: lo + m, maxCfg: hi - m };
    if (r.maxCfg - r.minCfg < 1) return null;
    r.range = (r.maxCfg - r.minCfg) / cpd;
    r.deadLo = lo / cpd; r.deadHi = (4096 - hi) / cpd;
    r.full = 4096 / cpd; r.usable = (hi - lo) / cpd;
    return r;
  });
  async function calApply(andSave) {
    const r = calc;
    if (!r) { calMsg = "Nothing to write yet: fill in counts per degree and both stops"; calBad = true; return; }
    calMsg = "Writing…"; calBad = false;
    try {
      for (const [p, v] of [[1, Math.round(r.minCfg)], [2, Math.round(r.maxCfg)], [3, +r.range.toFixed(1)]]) {
        const d = await jget(`/v1/studio/set?id=${cur}&p=${p}&v=${v}`);
        if (!d.ok) throw new Error(d.err || "no reply");
        vals[p] = d.v;
      }
      if (andSave) {
        const d = await jget("/v1/studio/save?id=" + cur);
        if (!d.ok) throw new Error(d.err || "save failed");
      }
      calMsg = andSave ? "Written and saved to flash" : "Written to RAM. Save to flash to keep it after power-off.";
      toast(andSave ? "Calibration saved" : "Calibration written");
    } catch (e) { calMsg = "Write failed: " + (e?.message || e); calBad = true; }
  }

  async function swStart() {
    if (swRun) return swFinish("stopped");
    if (!cur) return;
    const mA = Math.round(parseFloat(cCur) || 120);
    const dur = (parseFloat(cDur) || 14) * 1000;
    swData = []; swE = 0; swStats = ""; calMsg = ""; calBad = false;
    try {
      const d = await jget(`/v1/studio/direct?id=${cur}&m=2&cur=${mA}`);
      if (!d.ok) { calMsg = "Servo refused torque mode: " + (d.err || ""); calBad = true; return; }
    } catch { calMsg = "Could not start the sweep"; calBad = true; return; }
    swT0 = Date.now(); swRun = true;
    swStop = setTimeout(() => swFinish("timeout"), dur);
  }
  function swFinish(why) {
    swRun = false;
    clearTimeout(swStop);
    jget(`/v1/studio/direct?id=${cur}&m=0&deg=0&cur=0`).catch(() => {});
    swAnalyse(why);
  }
  /* median of the per-sample rates, ignoring wrap jumps */
  function swSlope(d, a, b) {
    const r = [];
    for (let i = a + 1; i <= b; i++) {
      const dt = d[i].t - d[i - 1].t, da = d[i].a - d[i - 1].a;
      if (dt <= 0 || Math.abs(da) > SW_WRAP) continue;
      r.push(da / dt);
    }
    if (r.length < 5) return 0;
    r.sort((x, y) => x - y);
    return r[Math.floor(r.length / 2)];
  }
  /* One wrap-to-wrap window is exactly 360°, so the share of it the ADC
     spends ramping is the electrical travel and the rest is the dead zone. */
  function swAnalyse(why) {
    const d = swData;
    if (d.length < 20) { calMsg = `Sweep too short (${why}), nothing to analyse`; calBad = true; return; }
    const wraps = [];
    for (let i = 1; i < d.length; i++) if (Math.abs(d[i].a - d[i - 1].a) > SW_WRAP) wraps.push(i);
    let mn = Infinity, mx = -Infinity;
    for (const p of d) { if (p.a < mn) mn = p.a; if (p.a > mx) mx = p.a; }
    cLo = String(Math.round(mn)); cHi = String(Math.round(mx));
    let bad = true;
    if (wraps.length >= 2) {
      const a = wraps[wraps.length - 2], b = wraps[wraps.length - 1];
      const tRev = d[b].t - d[a].t, nRev = b - a;
      const slope = swSlope(d, a, b);
      let hiS = -Infinity, loS = Infinity;
      for (let i = a; i <= b; i++) { if (d[i].a > hiS) hiS = d[i].a; if (d[i].a < loS) loS = d[i].a; }
      const span = hiS - loS;
      if (tRev > 400 && Math.abs(slope) > 1e-4 && span > 200) {
        const frac = Math.min(1, span / Math.abs(slope) / tRev);
        const E = 360 * frac;
        swE = E;
        cCpd = (span / E).toFixed(2);
        swStats = `${wraps.length - 1} turn${wraps.length > 2 ? "s" : ""} seen · travel ${E.toFixed(1)}° · dead zone ${(360 - E).toFixed(1)}°`;
        calMsg = nRev < 40 ? `Only ${nRev} samples in that turn; lower the spin current for a better number` : "";
        bad = nRev < 40;
      } else calMsg = "Could not measure the ramp rate; lower the spin current and retry";
    } else if (wraps.length === 1) {
      calMsg = "Only one wrap seen; raise the timeout so at least two full turns are logged";
    } else {
      calMsg = `No full turn seen. Stops captured (${Math.round(mn)} to ${Math.round(mx)}), but counts per degree needs step 1.`;
    }
    if (!swStats) swStats = `${d.length} samples · ${wraps.length} wrap(s) · ${why}`;
    calBad = bad && !!calMsg;
  }
  const swPath = $derived.by(() => {
    const d = swData;
    if (d.length < 2) return "";
    const W = 356, H = 110, t1 = d[d.length - 1].t || 1;
    let s = "";
    for (let i = 0; i < d.length; i++) {
      const brk = i === 0 || Math.abs(d[i].a - d[i - 1].a) > SW_WRAP;
      s += (brk ? "M" : "L") + (d[i].t / t1 * W).toFixed(1) + "," + (H - 4 - d[i].a / 4096 * (H - 8)).toFixed(1);
    }
    return s;
  });
  const pct = (v) => (Math.max(0, Math.min(4096, v)) / 4096 * 100).toFixed(2) + "%";

  /* ── Derived bits for the cards ────────────────────────────── */
  const gauge = $derived(live && isNum(live.now_deg) ? live.now_deg : null);
  const curPct = $derived(live && isNum(live.now_ma) && live.cap_ma > 0 ? Math.min(100, Math.abs(live.now_ma) / live.cap_ma * 100) : 0);
  const dutyPct = $derived(live && isNum(live.duty) ? Math.min(100, Math.abs(live.duty) * 100) : 0);
  const modeLabel = $derived(ctlMode === 1 ? "PWM mode" : ctlMode === 0 ? "CURRENT mode" : "");
  const chip = $derived(
    !online ? { t: "Robot offline", k: "bad" }
    : !cli ? { t: "Not connected", k: "off" }
    : answering === null ? { t: "Connected", k: "ok" }
    : { t: `${answering} of 12 answering`, k: answering === 12 ? "ok" : "warn" });
  const editing = $derived(editP === null ? null : PARAMS[editP]);
</script>

<!-- ═════════════════════════ snippets ═════════════════════════ -->

{#snippet ico(d, s = 20)}
  <svg width={s} height={s} viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.3" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path {d}></path></svg>
{/snippet}

{#snippet sw(on, label, onclick, disabled = false)}
  <button type="button" role="switch" aria-checked={on} aria-label={label} class="switch" class:on {onclick} {disabled}><span class="knob"></span></button>
{/snippet}

{#snippet servoRows(compact)}
  {#each LEGS as leg}
    <section class="legsec">
      <h2>{leg.name} <span class="mono cn">· {leg.cn}</span></h2>
      <div class="group">
        {#each leg.ids as id, j}
          {@const miss = found && !found.includes(id)}
          <button type="button" class="srow" class:compact class:sel={sel === id} class:miss
                  aria-current={sel === id} onclick={() => openServo(id)}>
            <span class="sid mono">{pad3(id)}</span>
            <span class="sname">{JOINT[j]}</span>
            {#if miss}<span class="snote">no reply</span>{/if}
            {#if !compact}<span class="chev">{@render ico("M9 6l6 6-6 6", 16)}</span>{/if}
          </button>
        {/each}
      </div>
    </section>
  {/each}
{/snippet}

{#snippet allRow(compact)}
  <button type="button" class="allrow" class:compact class:sel={allMode} onclick={openAll}>
    <span class="allbadge">ALL</span>
    <span class="txt">
      <span class="t">All servos</span>
      <span class="d">{compact ? "writes go to every board" : "A setting you change goes to every board"}</span>
    </span>
    {#if !compact}<span class="chev">{@render ico("M9 6l6 6-6 6", 16)}</span>{/if}
  </button>
{/snippet}

{#snippet gaugeSvg(w)}
  {@const fr = gauge === null ? 0 : Math.max(0, Math.min(1, gauge / 270))}
  <svg width={w} height={w * 0.9} viewBox="0 0 120 108" aria-label={gauge === null ? "Position unknown" : `Position ${gauge.toFixed(1)} degrees of 270`}>
    <path d="M 26.06 93.94 A 48 48 0 1 1 93.94 93.94" fill="none" stroke="#F3F3F1" stroke-width="10" stroke-linecap="round"></path>
    {#if gauge !== null}
      <path d="M 26.06 93.94 A 48 48 0 1 1 93.94 93.94" fill="none" stroke="#111111" stroke-width="10" stroke-linecap="round" stroke-dasharray="{(226.2 * fr).toFixed(1)} 226.2"></path>
    {/if}
    <text x="60" y="60" text-anchor="middle" fill="#111111" font-size="20" font-weight="600" font-family="JetBrains Mono, monospace">{gauge === null ? "—" : gauge.toFixed(1)}</text>
    <text x="60" y="74" text-anchor="middle" fill="#6B6862" font-size="8.5" letter-spacing="1.5" font-family="Inter, sans-serif">DEGREES</text>
    <text x="25" y="106" text-anchor="middle" fill="#A39F96" font-size="8" font-family="JetBrains Mono, monospace">0</text>
    <text x="95" y="106" text-anchor="middle" fill="#A39F96" font-size="8" font-family="JetBrains Mono, monospace">270</text>
  </svg>
{/snippet}

{#snippet meters()}
  <div class="meter">
    <div class="mrow"><span>Current</span><span class="mono">{live && isNum(live.now_ma) ? live.now_ma.toFixed(0) : "—"} / {live && isNum(live.cap_ma) ? live.cap_ma.toFixed(0) : "—"} mA</span></div>
    <div class="track"><div class="fill" style="width:{curPct}%"></div></div>
  </div>
  <div class="meter">
    <div class="mrow"><span>PWM duty</span><span class="mono">{live && isNum(live.duty) ? (live.duty * 100).toFixed(1) : "—"} %</span></div>
    <div class="track"><div class="fill" style="width:{dutyPct}%"></div></div>
  </div>
{/snippet}

{#snippet liveCard(compact)}
  <div class="card livecard" class:compact>
    <div class="lc-top">
      {@render gaugeSvg(compact ? 112 : 124)}
      <div class="lc-sw">
        <div class="swrow">
          <span class="swtxt"><span class="t2">Torque</span><span class="d2">{torqueOn ? "holding its position" : "motor off"}</span></span>
          {@render sw(!!torqueOn, "Torque", toggleTorque, !cli)}
        </div>
        <div class="swrow">
          <span class="swtxt"><span class="t2">Live poll</span><span class="d2">{poll ? "readings refresh" : "paused"}</span></span>
          {@render sw(poll, "Live poll", () => (poll = !poll))}
        </div>
      </div>
    </div>
    {@render meters()}
  </div>
{/snippet}

{#snippet configRows(table)}
  {#each shownParams as P (P.a)}
    {@const v = vals[P.a]}
    <button type="button" class="prow" class:table class:sel={editP === P.a} onclick={() => openEdit(P.a)}>
      <span class="addr mono" class:on={editP === P.a}>{P.a}</span>
      <span class="pname"><span class="pn">{P.n}</span><span class="pk mono">{P.k}</span></span>
      {#if table}
        <span class="pv mono">{fmtV(v)}</span>
        <span class="pact">{actText(P, v)}</span>
      {:else}
        <span class="pvcol"><span class="pv mono">{fmtV(v)}</span><span class="pact">{actText(P, v)}</span></span>
      {/if}
    </button>
  {/each}
{/snippet}

{#snippet modeNote()}
  {#if ctlMode === 0}
    <p class="note">Ki position and PWM scale are PWM-mode settings, hidden in CURRENT mode.</p>
  {:else if ctlMode === 1}
    <p class="note">Kp current and Kff current are CURRENT-mode settings, hidden in PWM mode.</p>
  {/if}
{/snippet}

{#snippet tunedRow()}
  <div class="tuned">
    <span class="d">Shipped gains: Kp {TUNED[0].d} · Kd {TUNED[1].d} · Kp current {TUNED[2].d} · Kff current {TUNED[3].d}</span>
    <button type="button" class="pill ghost sm" class:arm={tunedArm} onclick={writeTuned} disabled={!cli}>
      {tunedArm ? "Tap again to write" : "Write tuned gains"}
    </button>
  </div>
{/snippet}

{#snippet saveRow(small)}
  <div class="two">
    <button type="button" class="pill line danger" class:sm={small} class:arm={resetArm} onclick={factoryReset} disabled={!cli}>
      {resetArm ? (allMode ? "Reset all 4 boards?" : "Reset this board (3 servos)?") : "Factory reset"}
    </button>
    <button type="button" class="pill dark" class:sm={small} onclick={() => saveFlash(allMode)} disabled={!cli}>Save to flash</button>
  </div>
{/snippet}

{#snippet editor(inSheet)}
  {@const P = editing}
  {@const v = vals[P.a]}
  <div class="ed-head">
    <span class="ed-addr mono">{P.a}</span>
    <div class="ed-ttl">
      <h2 class="ed-h">{P.n}</h2>
      <span class="mono ed-k">{P.k} · {allMode ? "all servos" : "servo " + pad3(cur)}</span>
    </div>
    <button type="button" class="round sm" aria-label="Close" onclick={closeEdit}>{@render ico("M6 6l12 12M18 6L6 18", 16)}</button>
  </div>
  <p class="ed-desc">{P.desc}</p>
  {#if P.bool}
    <div class="group">
      {#each P.bool as label, i}
        {@const on = asText(draft) === String(i)}
        <button type="button" class="opt" class:sel={on} onclick={() => (draft = String(i))}>
          <span class="radio"><span></span></span>
          <span class="mono">{i}</span><span>{label}</span>
          {#if isNum(v) && (v >= 0.5 ? 1 : 0) === i}<span class="now">now</span>{/if}
        </button>
      {/each}
    </div>
  {:else}
    <div class="facts">
      <div><span>On the servo</span><b class="mono">{fmtV(v)}</b></div>
      <div><span>Range</span><b class="mono">{P.min}–{P.max}</b></div>
      <div><span>Step</span><b class="mono">{P.step}</b></div>
    </div>
    <div class="stepper">
      <button type="button" class="round big" aria-label="Down one step" onclick={() => nudge(P, -1)}>−</button>
      <input class="valin mono" aria-label="New value" inputmode="decimal" bind:value={draft}
             onkeydown={(e) => e.key === "Enter" && writeParam(P.a, draft)} />
      <button type="button" class="round big" aria-label="Up one step" onclick={() => nudge(P, 1)}>+</button>
    </div>
    {@const rv = isFinite(parseFloat(draft)) ? Math.min(P.max, Math.max(P.min, parseFloat(draft))) : P.min}
    <div class="rangewrap" style="--p:{((rv - P.min) / (P.max - P.min) * 100).toFixed(2)}%">
      <input type="range" class="range" min={P.min} max={P.max} step={P.step}
             value={isFinite(parseFloat(draft)) ? parseFloat(draft) : P.min}
             oninput={(e) => (draft = e.currentTarget.value)} aria-label="{P.n} slider" />
      <div class="rlab mono"><span>{P.min}</span><span>{P.max}</span></div>
    </div>
  {/if}
  <button type="button" class="pill dark wide" onclick={() => writeParam(P.a, draft)} disabled={asText(draft) === ""}>
    {asText(draft) === "" ? "Set" : `Set ${asText(draft)}`}{allMode ? " on all servos" : ""}
  </button>
  <p class="msg" class:bad={edBad}>{edMsg || "Set writes to RAM only. Save to flash keeps it after power-off."}</p>
{/snippet}

{#snippet graphPanel()}
  <div class="traces">
    {#each TRACES as T, i}
      <button type="button" class="trace" class:on={tOn[i]} aria-pressed={tOn[i]} onclick={() => (tOn[i] = !tOn[i])}>
        <span class="tsw" style="border-top: 3px {T.dash ? 'dashed' : 'solid'} {T.c}"></span>{T.n}
        {#if wide}<span class="tax">{T.ax ? "right" : "left"}</span>{/if}
      </button>
    {/each}
  </div>
  <div class="gbox" bind:clientWidth={gw}>
    <svg bind:this={gsvg} xmlns="http://www.w3.org/2000/svg" width={gm.W} height={gm.H} viewBox="0 0 {gm.W} {gm.H}" aria-label="Live servo graph" font-family="JetBrains Mono, monospace">
      {#if gm.empty}
        <text x={gm.W / 2} y={gm.H / 2} text-anchor="middle" font-size="13" fill="#A39F96" font-family="Inter, sans-serif">Press Start to record live servo data</text>
      {:else}
        {#each gm.grid as g}
          <line x1={gm.L} y1={g.y} x2={gm.W - gm.R} y2={g.y} stroke="#EFECE4"></line>
          <text x={gm.L - 6} y={+g.y + 3.5} text-anchor="end" font-size="10" fill="#6B6862">{g.l}</text>
          <text x={gm.W - gm.R + 6} y={+g.y + 3.5} font-size="10" fill="#2563EB">{g.r}</text>
        {/each}
        <text x={gm.L} y="10" font-size="10" fill="#6B6862">deg</text>
        <text x={gm.W - gm.R} y="10" text-anchor="end" font-size="10" fill="#2563EB">mA</text>
        {#each gm.paths as p}
          <path d={p.d} fill="none" stroke={p.c} stroke-width={p.w} stroke-dasharray={p.dash || undefined} stroke-linejoin="round"></path>
        {/each}
        <text x={gm.L} y={gm.H - 4} font-size="10" fill="#6B6862">{gm.from}</text>
        <text x={gm.W - gm.R} y={gm.H - 4} text-anchor="end" font-size="10" fill="#6B6862">{gm.to}</text>
      {/if}
    </svg>
  </div>
{/snippet}

{#snippet graphButtons()}
  <button type="button" class="pill {grun ? 'dark' : 'line'} gbtn" onclick={gStartStop}>
    {#if grun}<span class="stopsq"></span>Stop{:else}<span class="playtri"></span>Start{/if}
  </button>
  <button type="button" class="pill line gbtn" onclick={gReset}>Reset</button>
  <button type="button" class="pill {wide ? 'line' : 'soft'} gbtn" onclick={gCapture} disabled={gm.empty}>Capture</button>
  <button type="button" class="pill {wide ? 'line' : 'soft'} gbtn" onclick={gSaveCsv}>Save CSV</button>
  <button type="button" class="pill {wide ? 'line' : 'soft'} gbtn" onclick={() => csvIn && csvIn.click()}>Load CSV</button>
{/snippet}

{#snippet calNum(label, get, set, mode = "decimal")}
  <label class="fld">{label}
    <input class="mono" inputmode={mode} value={get()} oninput={(e) => set(e.currentTarget.value)} />
  </label>
{/snippet}

{#snippet calBody()}
  <div class="calgrid">
    <section class="card calsec">
      <div class="calh"><span class="num">1</span><h3>Scale · counts per degree</h3></div>
      <div class="refrow">
        {@render calNum("Reference A angle", () => cA, (v) => (cA = v))}
        {@render calNum("ADC at A", () => cAadc, (v) => (cAadc = v), "numeric")}
        <button type="button" class="pill line cap" onclick={() => grab("A")}>Capture</button>
      </div>
      <div class="refrow">
        {@render calNum("Reference B angle", () => cB, (v) => (cB = v))}
        {@render calNum("ADC at B", () => cBadc, (v) => (cBadc = v), "numeric")}
        <button type="button" class="pill line cap" onclick={() => grab("B")}>Capture</button>
      </div>
      <div class="cpdrow">
        {@render calNum("Counts / degree", () => cCpd, (v) => (cCpd = v))}
        <button type="button" class="pill soft cap" onclick={calCpd}>Compute from A &amp; B</button>
      </div>
      <p class="note">Park the horn at a measured angle and capture, turn it a known amount and capture again. Measure once per pot model; it holds for every servo of that model.</p>
    </section>

    <section class="card calsec">
      <div class="calh"><span class="num">2</span><h3>Travel · auto or manual</h3></div>
      <div class="seg" role="tablist" aria-label="Travel method">
        <button type="button" role="tab" aria-selected={cTab === "auto"} class:on={cTab === "auto"} onclick={() => (cTab = "auto")}>Auto sweep</button>
        <button type="button" role="tab" aria-selected={cTab === "manual"} class:on={cTab === "manual"} onclick={() => { if (swRun) swFinish("stopped"); cTab = "manual"; }}>Manual</button>
      </div>
      {#if cTab === "auto"}
        <div class="warn"><b>Remove the leg or horn first.</b> Auto sweep spins the shaft round and round in torque mode. It stops at the timeout, and Close always stops it.</div>
        <div class="pair">
          {@render calNum("Spin current (mA)", () => cCur, (v) => (cCur = v), "numeric")}
          {@render calNum("Timeout (s)", () => cDur, (v) => (cDur = v), "numeric")}
        </div>
        <div class="twoone">
          <button type="button" class="pill dark" class:rec={swRun} onclick={swStart}>{swRun ? "Stop sweep" : "Auto sweep"}</button>
          <button type="button" class="pill line" onclick={motorOff}>Motor off</button>
        </div>
        <svg class="sweep" viewBox="0 0 356 110" preserveAspectRatio="none" aria-label="Position ADC during the sweep">
          {#if swPath}
            <path d={swPath} fill="none" stroke="#111111" stroke-width="2" stroke-linejoin="round" vector-effect="non-scaling-stroke"></path>
          {:else}
            <text x="178" y="59" text-anchor="middle" font-size="11" fill="#A39F96" font-family="Inter, sans-serif">Press Auto sweep to log the shaft turning</text>
          {/if}
        </svg>
        <span class="mono stat">{swRun ? `Sampling · ${swData.length} samples · ADC ${calAdc === null ? "—" : Math.round(calAdc)}` : swStats || "One wrap-to-wrap turn is exactly 360°, so no protractor is needed."}</span>
      {:else}
        <div class="manual">
          <div class="adcnow"><span class="d2">Live ADC</span><b class="mono">{calAdc === null ? "—" : Math.round(calAdc)}</b></div>
          <button type="button" class="pill {recOn ? 'dark rec' : 'line'}" onclick={calRec}>{recOn ? "Stop recording" : "Record sweep"}</button>
          <button type="button" class="pill line" onclick={motorOff}>Motor off</button>
        </div>
        <p class="note">Press Motor off, then Record sweep, turn the horn by hand through its whole range, and press again to stop. The lowest and highest counts are captured for you.</p>
      {/if}
      <div class="three">
        {@render calNum("Stop A (low)", () => cLo, (v) => (cLo = v), "numeric")}
        {@render calNum("Stop B (high)", () => cHi, (v) => (cHi = v), "numeric")}
        {@render calNum("Margin", () => cMg, (v) => (cMg = v), "numeric")}
      </div>
    </section>

    <section class="card calsec">
      <div class="calh"><span class="num">3</span><h3>Result</h3></div>
      <div class="rbar">
        {#if calc}
          <div class="seg-mgn" style="left:{pct(calc.lo)};width:calc({pct(calc.hi)} - {pct(calc.lo)})"></div>
          <div class="seg-use" style="left:{pct(calc.minCfg)};width:calc({pct(calc.maxCfg)} - {pct(calc.minCfg)})"></div>
        {/if}
        {#if calAdc !== null}<div class="rcur" style="left:{pct(calAdc)}"></div>{/if}
      </div>
      <div class="rlab mono"><span>0</span><span>2048</span><span>4096</span></div>
      <div class="facts">
        <div><span>Min ADC</span><b class="mono">{calc ? Math.round(calc.minCfg) : "—"}</b></div>
        <div><span>Max ADC</span><b class="mono">{calc ? Math.round(calc.maxCfg) : "—"}</b></div>
        <div><span>Range</span><b class="mono">{calc ? calc.range.toFixed(1) + "°" : "—"}</b></div>
      </div>
      {#if calc}
        <span class="mono stat">full {calc.full.toFixed(1)}° · usable {calc.usable.toFixed(1)}° · dead {calc.deadLo.toFixed(1)}° / {calc.deadHi.toFixed(1)}°</span>
      {/if}
      <button type="button" class="pill dark" onclick={() => calApply(true)}>Write + save to flash</button>
      <button type="button" class="pill line" onclick={() => calApply(false)}>Write to servo only</button>
      {#if calMsg}<p class="msg" class:bad={calBad}>{calMsg}</p>{/if}
    </section>
  </div>
{/snippet}

{#snippet activity()}
  <section class="actsec">
    <button type="button" class="acthead" onclick={() => (logOpen = !logOpen)} aria-expanded={logOpen}>
      <span>Activity</span><span class="mono">{logLines.length || ""}</span>
      <span class="chev" class:open={logOpen}>{@render ico("M9 6l6 6-6 6", 14)}</span>
    </button>
    {#if logOpen}
      <div class="group logs">
        {#if !logLines.length}<div class="logl d">Nothing yet.</div>{/if}
        {#each logLines as l}
          <div class="logl" class:bad={l.bad}><span class="mono lt">{l.t}</span><span>{l.m}</span></div>
        {/each}
      </div>
    {/if}
  </section>
{/snippet}

{#snippet notConnected()}
  <div class="card empty">
    <b>{online ? "Not connected" : "Robot offline"}</b>
    <span>{online ? "Turn on Connect to pause the gait and read the servos." : "Waiting for the robot to answer."}</span>
    {#if online}<button type="button" class="pill dark" onclick={toggleConn}>Connect</button>{/if}
  </div>
{/snippet}

<!-- ═════════════════════════ page ═════════════════════════ -->

<input bind:this={csvIn} type="file" accept=".csv,text/csv" hidden onchange={gLoadCsv} />

<div class="studio" class:wide bind:this={rootEl}>
{#if wide}
  <!-- ══════════════ PC ══════════════ -->
  <header class="pchead">
    <button type="button" class="pill soft backpill" onclick={onBack}>{@render ico("M15 6l-6 6 6 6", 18)}Settings</button>
    <div class="pctitle">
      <h1>Servo Studio</h1>
      <span>Mini Pupper · 4 AT32 driver boards · SPI2</span>
    </div>
    <span class="chip {chip.k}"><span class="cdot"></span>{cli ? "Connected · gait paused · " + (answering === null ? "scanning" : chip.t) : chip.t}</span>
    <span class="grow"></span>
    <div class="tools">
      <span class="connpill"><span>Connect</span>{@render sw(cli, "Connect", toggleConn, !online)}</span>
      <button type="button" class="pill line" onclick={() => scan()} disabled={!cli}>Scan</button>
      <button type="button" class="pill line" onclick={reread} disabled={!cli || !sel}>Refresh</button>
      <div class="seg tabs" role="tablist" aria-label="Middle view">
        <button type="button" role="tab" aria-selected={tab === "table"} class:on={tab === "table"} onclick={() => (tab = "table")}>Settings</button>
        <button type="button" role="tab" aria-selected={tab === "graph"} class:on={tab === "graph"} onclick={() => (tab = "graph")} disabled={!cur}>Graph</button>
      </div>
      <button type="button" class="pill line" onclick={openCal} disabled={!cli || !cur}>Calibrate</button>
      <button type="button" class="pill line" onclick={() => saveFlash(false)} disabled={!cli || !cur}>Save</button>
      <button type="button" class="pill dark" onclick={() => saveFlash(true)} disabled={!cli}>Save all</button>
    </div>
  </header>

  <div class="pcbody">
    <nav class="pcnav" aria-label="Servos">
      {@render allRow(true)}
      {@render servoRows(true)}
      {@render activity()}
    </nav>

    <main class="pcmain">
      {#if !cli}
        {@render notConnected()}
      {:else if !sel}
        <div class="card empty"><b>Pick a servo</b><span>Choose one on the left, or All servos to change every board at once.</span></div>
      {:else if tab === "graph" && cur}
        <div class="card gcard">
          <div class="gtools">{@render graphButtons()}<span class="grow"></span><span class="mono stat">{ginfo || "last 15 s"}</span></div>
          {@render graphPanel()}
        </div>
      {:else}
        <div class="group ptable">
          <div class="thead"><span>Addr</span><span>Parameter</span><span class="r">Value</span><span>Actual</span></div>
          <div class="tsec">Configuration · stored {allMode ? "on every servo" : "per servo"}{#if loading}<span class="ld">{loading}</span>{/if}</div>
          {@render configRows(true)}
          {#if !allMode}
            <div class="tsec">Live telemetry · read-only</div>
            {#each LIVE as L}
              <div class="prow table ro">
                <span class="addr mono">{L.a}</span>
                <span class="pname"><span class="pn">{L.n}</span><span class="pk mono">{L.k}</span></span>
                <span class="pv mono">{live && isNum(live[L.k]) ? (L.k === "mode" ? live[L.k] : liveText({ ...L, u: "" }, live)) : "—"}</span>
                <span class="pact">{liveText(L, live)}</span>
              </div>
            {/each}
          {/if}
        </div>
        {@render modeNote()}
        {@render tunedRow()}
      {/if}
    </main>

    <aside class="pcaside">
      {#if cli && sel}
        <section class="card scard">
          <div class="shead">
            <span class="sbadge mono" class:all={allMode}>{allMode ? "ALL" : pad3(cur)}</span>
            <span class="txt"><span class="t">{allMode ? "All servos" : servoName(cur)}</span>
              <span class="d">{allMode ? "Writes go to every board" : `Board ${legOf(cur).cn}${modeLabel ? " · " + modeLabel : ""}`}</span></span>
          </div>
          {#if !allMode}
            <div class="lc-top">
              {@render gaugeSvg(112)}
              <div class="lc-sw">
                <div class="swrow"><span class="swtxt"><span class="t2">Torque</span><span class="d2">{torqueOn ? "holding position" : "motor off"}</span></span>{@render sw(!!torqueOn, "Torque", toggleTorque)}</div>
                <div class="swrow"><span class="swtxt"><span class="t2">Live poll</span></span>{@render sw(poll, "Live poll", () => (poll = !poll))}</div>
              </div>
            </div>
            {@render meters()}
            <div class="mini">
              {#each [["Position", "now_deg"], ["Goal", "set_deg"], ["Pos error", "err_deg"], ["Current", "now_ma"], ["Current cap", "cap_ma"], ["Temperature", "temp_c"], ["Mode", "mode"]] as [label, k]}
                <div><span>{label}</span><b class="mono">{liveText(LIVE.find((L) => L.k === k), live)}</b></div>
              {/each}
            </div>
          {/if}
          {@render saveRow(true)}
        </section>
        {#if editing}
          <section class="card edcard">{@render editor(false)}</section>
        {:else}
          <p class="note center">Click a setting in the table to change it.</p>
        {/if}
      {/if}
    </aside>
  </div>

  {#if cal}
    <!-- svelte-ignore a11y_click_events_have_key_events -->
    <!-- svelte-ignore a11y_no_static_element_interactions -->
    <div class="scrim" onclick={(e) => e.target === e.currentTarget && closeCal()}>
      <div class="caldlg" role="dialog" aria-label="Pot calibration">
        <div class="calhead">
          <div class="txt"><h2 class="dlgh">Pot calibration</h2><span class="d">Servo {pad3(cur)} · {servoName(cur).toLowerCase()}</span></div>
          <button type="button" class="pill soft" onclick={closeCal}>Close</button>
        </div>
        {@render calBody()}
      </div>
    </div>
  {/if}

{:else}
  <!-- ══════════════ Phone ══════════════ -->
  <div class="inner">
  {#if cal && cur}
    <div class="ptop">
      <button type="button" class="round" aria-label="Back to servo {pad3(cur)}" onclick={closeCal}>{@render ico("M15 6l-6 6 6 6", 18)}</button>
      <div class="txt"><h1 class="h22">Pot calibration</h1><span class="d">Servo {pad3(cur)} · {servoName(cur).toLowerCase()}</span></div>
    </div>
    {@render calBody()}

  {:else if view === "graph" && cur}
    <div class="ptop">
      <button type="button" class="round" aria-label="Back to servo {pad3(cur)}" onclick={() => (view = "servo")}>{@render ico("M15 6l-6 6 6 6", 18)}</button>
      <div class="txt"><h1 class="h22">Graph</h1><span class="d">Servo {pad3(cur)} · {servoName(cur).toLowerCase()}</span></div>
    </div>
    <div class="card gcard">{@render graphPanel()}</div>
    <span class="mono stat center">{ginfo || "Last 15 seconds"}</span>
    <div class="gbtns">{@render graphButtons()}</div>

  {:else if view === "servo" && sel}
    <div class="ptop between">
      <button type="button" class="round" aria-label="Back to servos" onclick={() => { view = "list"; editP = null; }}>{@render ico("M15 6l-6 6 6 6", 18)}</button>
      {#if !allMode}
        <div class="navbtns">
          <button type="button" class="round" aria-label="Previous servo" onclick={() => step(-1)}>{@render ico("M18 15l-6-6-6 6", 18)}</button>
          <button type="button" class="round" aria-label="Next servo" onclick={() => step(1)}>{@render ico("M6 9l6 6 6-6", 18)}</button>
        </div>
      {/if}
    </div>
    <div class="shero">
      <span class="sbadge big mono" class:all={allMode}>{allMode ? "ALL" : pad3(cur)}</span>
      <div class="txt">
        <h1 class="h28">{allMode ? "All servos" : servoName(cur)}</h1>
        <span class="d">{allMode ? "A setting you change goes to every board" : `Board ${legOf(cur).cn}${modeLabel ? " · " + modeLabel : ""}`}</span>
      </div>
    </div>

    {#if !cli}
      {@render notConnected()}
    {:else}
      {#if !allMode}
        {@render liveCard(false)}
        <div class="two">
          <button type="button" class="tile wide2" onclick={() => { view = "graph"; scrollTop(); }}>{@render ico("M3 3v18h18M7 15l4-5 3 3 5-7")}Graph</button>
          <button type="button" class="tile wide2" onclick={openCal}>{@render ico("M3 17l14-14 4 4L7 21H3zM14 6l4 4M9 11l2 2M6 14l2 2")}Calibrate</button>
        </div>
      {/if}

      <section>
        <h2>Configuration · stored {allMode ? "on every servo" : "per servo"}{#if loading}<span class="ld">{loading}</span>{/if}</h2>
        <div class="group">{@render configRows(false)}</div>
        {@render modeNote()}
        {@render tunedRow()}
      </section>

      {#if !allMode}
        <section>
          <h2>Live telemetry · read-only</h2>
          <div class="group">
            {#each LIVE as L}
              <div class="lrow"><span class="laddr mono">{L.a}</span><span class="ln">{L.n}</span><span class="lv mono">{liveText(L, live)}</span></div>
            {/each}
          </div>
        </section>
      {/if}

      {@render saveRow(false)}
      <p class="note center">A setting you change lives in RAM until you save it.</p>
    {/if}
    {@render activity()}

  {:else}
    <div class="ptop between">
      <button type="button" class="pill soft backpill" onclick={onBack}>{@render ico("M15 6l-6 6 6 6", 18)}Settings</button>
      <span class="chip {chip.k}"><span class="cdot"></span>{chip.t}</span>
    </div>
    <div class="hero">
      <h1>Servo Studio</h1>
      <p>Mini Pupper · 4 AT32 driver boards · SPI2</p>
    </div>

    <div class="card connrow">
      <span class="ico dark">{@render ico("M9 7V3M15 7V3M7 7h10v4a5 5 0 0 1-10 0zM12 16v5")}</span>
      <span class="txt"><span class="t">{cli ? "Connected" : "Connect"}</span>
        <span class="d">{cli ? "Gait paused while you work" : online ? "Pauses the gait so you can edit servos" : "Robot is not answering"}</span></span>
      {@render sw(cli, "Connect", toggleConn, !online)}
    </div>

    <div class="tools3">
      <button type="button" class="tile" onclick={() => scan()} disabled={!cli}>{@render ico("M11 4a7 7 0 1 0 0 14 7 7 0 0 0 0-14zM20 20l-4-4")}Scan</button>
      <button type="button" class="tile" onclick={() => { if (cli) { scan(true); } }} disabled={!cli}>{@render ico("M20 11a8 8 0 1 0-2.3 5.7M20 4v7h-7")}Refresh</button>
      <button type="button" class="tile" onclick={() => saveFlash(true)} disabled={!cli}>{@render ico("M5 3h11l3 3v15H5zM8 3v5h7V3M8 21v-7h8v7")}Save all</button>
    </div>

    {@render allRow(false)}
    {@render servoRows(false)}
    {@render activity()}
  {/if}
  </div>

  {#if editing && view === "servo" && !cal}
    <!-- svelte-ignore a11y_click_events_have_key_events -->
    <!-- svelte-ignore a11y_no_static_element_interactions -->
    <div class="scrim" onclick={(e) => e.target === e.currentTarget && closeEdit()}>
      <div class="sheet" role="dialog" aria-label="Edit {editing.n}">
        <div class="grab"></div>
        {@render editor(true)}
      </div>
    </div>
  {/if}
{/if}

  {#if toastMsg}<div class="toast" class:bad={toastBad} role="status">{toastMsg}</div>{/if}
</div>

<style>
  .studio {
    position: fixed; inset: 0; z-index: 60; overflow-y: auto; -webkit-overflow-scrolling: touch;
    background: var(--page, #fff); color: var(--ink, #111);
    font-family: Inter, "SF Pro Display", -apple-system, "Segoe UI", sans-serif;
    --warn-ink: #8A4100; --danger: #A1311F; --sel: #FFFBDA;
  }
  button { font: inherit; color: inherit; cursor: pointer; }
  button:disabled { cursor: not-allowed; opacity: 0.45; }
  .mono { font-family: var(--mono); }
  .grow { flex: 1; }
  .center { text-align: center; }
  h1, h2, h3, p { margin: 0; }

  /* ── phone frame ── */
  .inner { max-width: var(--page-max, 560px); margin: 0 auto; box-sizing: border-box;
    padding: calc(20px + var(--safe-top, 0px)) 16px calc(32px + var(--safe-bottom, 0px));
    display: flex; flex-direction: column; gap: 18px; }
  .ptop { display: flex; align-items: center; gap: 12px; }
  .ptop.between { justify-content: space-between; }
  .navbtns { display: flex; gap: 8px; }
  .hero { display: flex; flex-direction: column; gap: 4px; padding: 0 4px; }
  .hero h1 { font-size: 34px; font-weight: 800; letter-spacing: -0.02em; }
  .hero p { font-size: 14px; color: var(--sub); }
  .h22 { font-size: 22px; font-weight: 800; }
  .h28 { font-size: 28px; font-weight: 800; letter-spacing: -0.02em; }
  section { display: flex; flex-direction: column; gap: 8px; }
  h2 { margin: 0 6px; font-size: 12px; font-weight: 800; letter-spacing: 0.12em; text-transform: uppercase; color: var(--sub); }
  h2 .cn { font-weight: 500; letter-spacing: 0; text-transform: none; }
  .ld { margin-left: 8px; font-weight: 600; letter-spacing: 0; text-transform: none; color: var(--sub); }

  .round { width: 44px; height: 44px; border-radius: 22px; border: none; background: var(--ground); display: grid; place-items: center; flex-shrink: 0; }
  .round.sm { width: 40px; height: 40px; border-radius: 20px; }
  .round.big { width: 56px; height: 56px; border-radius: 28px; font-size: 28px; font-weight: 600; }

  .pill { height: 44px; padding: 0 16px; border-radius: 22px; border: none; font-size: 14px; font-weight: 700; display: inline-flex; align-items: center; justify-content: center; gap: 6px; white-space: nowrap; }
  .pill.sm { height: 40px; font-size: 13px; border-radius: 20px; }
  .pill.soft { background: var(--ground); }
  .pill.line { background: #fff; border: 1px solid var(--line); }
  .pill.dark { background: var(--ink); color: #fff; }
  .pill.ghost { background: transparent; border: 1px solid var(--line); height: 36px; padding: 0 12px; font-size: 12px; }
  .pill.danger { color: var(--danger); }
  .pill.arm { background: var(--danger); color: #fff; border-color: var(--danger); }
  .pill.rec { background: var(--danger); }
  .pill.wide { width: 100%; height: 52px; border-radius: 26px; font-size: 16px; }
  .backpill { padding: 0 16px 0 12px; }

  .chip { display: inline-flex; align-items: center; gap: 8px; height: 32px; padding: 0 12px; border-radius: 16px; font-size: 13px; font-weight: 700; white-space: nowrap; }
  .cdot { width: 8px; height: 8px; border-radius: 4px; }
  .chip.ok { background: #E7F4EA; color: #1E6B30; } .chip.ok .cdot { background: #23A047; }
  .chip.warn { background: #FFF3E6; color: var(--warn-ink); } .chip.warn .cdot { background: #E07B00; }
  .chip.off { background: var(--ground); color: var(--sub); } .chip.off .cdot { background: #A39F96; }
  .chip.bad { background: #FBEAE7; color: var(--danger); } .chip.bad .cdot { background: var(--danger); }

  .card { border: 1px solid var(--line); border-radius: 20px; background: #fff; }
  .group { border: 1px solid var(--line); border-radius: 20px; overflow: hidden; background: #fff; }
  .group > * + * { border-top: 1px solid var(--hair-2); }
  .txt { flex: 1; min-width: 0; display: flex; flex-direction: column; gap: 2px; text-align: left; }
  .t { font-size: 16px; font-weight: 700; }
  .d { font-size: 13px; color: var(--sub); }
  .note { margin: 0 6px; font-size: 13px; line-height: 1.45; color: var(--sub); }
  .msg { margin: 0; font-size: 13px; color: var(--sub); text-align: center; }
  .msg.bad { color: var(--danger); font-weight: 600; }
  .stat { font-size: 12px; color: var(--sub); }

  .connrow { display: flex; align-items: center; gap: 14px; padding: 14px 16px; }
  .ico { width: 38px; height: 38px; border-radius: 11px; display: grid; place-items: center; flex-shrink: 0; }
  .ico.dark { background: var(--ink); color: var(--mpx-yellow); }

  .switch { width: 52px; height: 32px; border-radius: 16px; border: none; background: var(--hair); padding: 3px; display: flex; justify-content: flex-start; flex-shrink: 0; transition: background var(--dur-fast, 140ms); }
  .switch.on { background: var(--ink); justify-content: flex-end; }
  .knob { width: 26px; height: 26px; border-radius: 13px; background: #fff; box-shadow: 0 1px 3px rgba(0, 0, 0, 0.2); }
  .switch.on .knob { background: var(--mpx-yellow); box-shadow: none; }

  .tools3 { display: grid; grid-template-columns: repeat(3, minmax(0, 1fr)); gap: 8px; }
  .tile { height: 64px; border: none; border-radius: 16px; background: var(--ground); font-size: 13px; font-weight: 700; display: flex; flex-direction: column; align-items: center; justify-content: center; gap: 4px; }
  .tile.wide2 { height: 56px; flex-direction: row; gap: 8px; font-size: 14px; }

  .allrow { width: 100%; border: 1px solid var(--line); border-radius: 20px; background: #fff; display: flex; align-items: center; gap: 12px; padding: 12px 16px; text-align: left; }
  .allrow.compact { border-radius: 16px; padding: 8px 12px; gap: 10px; }
  .allrow.compact .t { font-size: 14px; } .allrow.compact .d { font-size: 11px; }
  .allrow.sel { background: var(--sel); }
  .allbadge { width: 38px; height: 38px; border-radius: 11px; background: var(--mpx-yellow); font-size: 11px; font-weight: 800; letter-spacing: 0.04em; display: grid; place-items: center; flex-shrink: 0; }
  .allrow.compact .allbadge { width: 40px; height: 32px; border-radius: 9px; font-size: 10px; }
  .chev { color: #A39F96; display: grid; place-items: center; transition: transform var(--dur-fast, 140ms); }
  .chev.open { transform: rotate(90deg); }

  .srow { width: 100%; border: none; background: #fff; display: flex; align-items: center; gap: 12px; padding: 12px 16px; text-align: left; }
  .srow.compact { padding: 7px 12px; gap: 10px; }
  .srow.sel { background: var(--sel); }
  .srow.miss { opacity: 0.5; }
  .sid { width: 44px; height: 38px; border-radius: 11px; background: var(--ground); font-size: 14px; font-weight: 600; display: grid; place-items: center; flex-shrink: 0; }
  .srow.compact .sid { width: 40px; height: 30px; border-radius: 9px; font-size: 12px; }
  .srow.sel .sid { background: var(--mpx-yellow); }
  .sname { flex: 1; font-size: 16px; font-weight: 700; }
  .srow.compact .sname { font-size: 14px; }
  .snote { font-size: 13px; font-weight: 600; color: var(--warn-ink); }
  .srow.compact .snote { font-size: 11px; }

  .shero { display: flex; align-items: center; gap: 14px; padding: 0 4px; }
  .sbadge { width: 48px; height: 40px; border-radius: 12px; background: var(--mpx-yellow); font-size: 15px; font-weight: 600; display: grid; place-items: center; flex-shrink: 0; }
  .sbadge.big { width: 60px; height: 56px; border-radius: 16px; font-size: 20px; }
  .sbadge.all { font-family: inherit; font-weight: 800; font-size: 12px; letter-spacing: 0.04em; }

  .livecard { padding: 14px 16px; display: flex; flex-direction: column; gap: 12px; }
  .lc-top { display: flex; align-items: center; gap: 14px; }
  .lc-sw { flex: 1; display: flex; flex-direction: column; gap: 12px; }
  .swrow { display: flex; align-items: center; gap: 10px; }
  .swtxt { flex: 1; display: flex; flex-direction: column; gap: 1px; }
  .t2 { font-size: 15px; font-weight: 700; }
  .d2 { font-size: 12px; color: var(--sub); }
  .meter { display: flex; flex-direction: column; gap: 6px; }
  .mrow { display: flex; justify-content: space-between; font-size: 13px; }
  .mrow span:first-child { font-weight: 700; }
  .track { height: 8px; border-radius: 4px; background: var(--ground); overflow: hidden; }
  .fill { height: 8px; border-radius: 4px; background: var(--ink); transition: width 0.2s; }

  .two { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 8px; }
  .two .pill { height: 52px; border-radius: 26px; font-size: 15px; }
  .two .pill.sm { height: 40px; border-radius: 20px; font-size: 13px; }

  .prow { width: 100%; border: none; background: #fff; display: flex; align-items: center; gap: 12px; padding: 10px 14px; text-align: left; }
  .prow.sel { background: var(--sel); }
  .addr { width: 36px; height: 30px; border-radius: 9px; background: var(--ground); font-size: 12px; font-weight: 600; display: grid; place-items: center; flex-shrink: 0; }
  .addr.on { background: var(--mpx-yellow); }
  .pname { flex: 1; min-width: 0; display: flex; flex-direction: column; gap: 1px; }
  .pn { font-size: 15px; font-weight: 700; }
  .pk { font-size: 11px; color: var(--sub); overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  .pvcol { display: flex; flex-direction: column; align-items: flex-end; gap: 1px; }
  .pv { font-size: 15px; font-weight: 600; }
  .pact { font-size: 11px; color: var(--sub); }

  .lrow { display: flex; align-items: center; gap: 12px; padding: 10px 14px; }
  .laddr { width: 36px; height: 26px; border-radius: 8px; background: var(--ground); font-size: 11px; font-weight: 600; color: #4F4C46; display: grid; place-items: center; flex-shrink: 0; }
  .ln { flex: 1; font-size: 14px; }
  .lv { font-size: 14px; font-weight: 600; }

  .tuned { display: flex; align-items: center; gap: 10px; flex-wrap: wrap; padding: 0 6px; }
  .tuned .d { flex: 1; min-width: 180px; font-size: 12px; }

  .empty { padding: 28px 20px; display: flex; flex-direction: column; align-items: center; gap: 8px; text-align: center; }
  .empty b { font-size: 17px; }
  .empty span { font-size: 14px; color: var(--sub); max-width: 340px; }
  .empty .pill { margin-top: 6px; }

  /* ── edit sheet / editor ── */
  .scrim { position: fixed; inset: 0; z-index: 70; background: rgba(17, 17, 17, 0.42); display: flex; align-items: flex-end; justify-content: center; }
  .sheet { width: 100%; max-width: var(--page-max, 560px); max-height: 92vh; overflow-y: auto; box-sizing: border-box; background: #fff; border-radius: 28px 28px 0 0; padding: 10px 16px calc(28px + var(--safe-bottom, 0px)); display: flex; flex-direction: column; gap: 16px; }
  .grab { width: 40px; height: 5px; border-radius: 3px; background: var(--hair); align-self: center; }
  .ed-head { display: flex; align-items: flex-start; gap: 12px; }
  .ed-addr { width: 40px; height: 34px; border-radius: 10px; background: var(--mpx-yellow); font-size: 14px; font-weight: 600; display: grid; place-items: center; flex-shrink: 0; }
  .ed-ttl { flex: 1; min-width: 0; display: flex; flex-direction: column; gap: 2px; }
  .ed-h { margin: 0; font-size: 22px; font-weight: 800; letter-spacing: -0.01em; text-transform: none; color: var(--ink); }
  .ed-k { font-size: 12px; color: var(--sub); }
  .ed-desc { font-size: 14px; line-height: 1.45; color: #4F4C46; }
  .facts { display: grid; grid-template-columns: repeat(3, minmax(0, 1fr)); gap: 8px; }
  .facts > div { background: var(--ground); border-radius: 14px; padding: 8px 12px; display: flex; flex-direction: column; gap: 1px; min-width: 0; }
  .facts span { font-size: 11px; color: var(--sub); font-weight: 600; }
  .facts b { font-size: 16px; font-weight: 600; overflow: hidden; text-overflow: ellipsis; }
  .stepper { display: flex; align-items: center; gap: 12px; }
  .valin { flex: 1; min-width: 0; height: 56px; box-sizing: border-box; border-radius: 16px; border: 2px solid var(--ink); background: #fff; font-size: 26px; font-weight: 600; color: var(--ink); text-align: center; outline: none; }
  .rangewrap { display: flex; flex-direction: column; gap: 4px; }
  .range { -webkit-appearance: none; appearance: none; width: 100%; height: 28px; background: transparent; margin: 0; }
  .range::-webkit-slider-runnable-track { height: 6px; border-radius: 3px; background: linear-gradient(to right, var(--ink) var(--p, 0%), var(--ground) var(--p, 0%)); }
  .range::-moz-range-track { height: 6px; border-radius: 3px; background: var(--ground); }
  .range::-moz-range-progress { height: 6px; border-radius: 3px; background: var(--ink); }
  .range::-webkit-slider-thumb { -webkit-appearance: none; width: 28px; height: 28px; margin-top: -11px; border-radius: 14px; background: var(--mpx-yellow); border: 3px solid var(--ink); box-sizing: border-box; }
  .range::-moz-range-thumb { width: 22px; height: 22px; border-radius: 14px; background: var(--mpx-yellow); border: 3px solid var(--ink); }
  .rlab { display: flex; justify-content: space-between; font-size: 11px; color: var(--sub); }
  .opt { width: 100%; border: none; background: #fff; display: flex; align-items: center; gap: 12px; padding: 14px 16px; text-align: left; font-size: 15px; font-weight: 600; }
  .opt.sel { background: var(--sel); }
  .radio { width: 22px; height: 22px; border-radius: 11px; border: 2px solid #C9C5BB; box-sizing: border-box; display: grid; place-items: center; flex-shrink: 0; }
  .radio span { width: 10px; height: 10px; border-radius: 5px; }
  .opt.sel .radio { border-color: var(--ink); }
  .opt.sel .radio span { background: var(--ink); }
  .now { margin-left: auto; font-size: 12px; color: var(--sub); font-weight: 600; }

  /* ── graph ── */
  .gcard { padding: 12px 8px 8px; display: flex; flex-direction: column; gap: 12px; }
  .traces { display: flex; flex-wrap: wrap; gap: 6px; padding: 0 4px; }
  .trace { height: 36px; padding: 0 12px; border-radius: 18px; border: 1px solid var(--ground); background: var(--ground); font-size: 12px; font-weight: 700; display: flex; align-items: center; gap: 6px; opacity: 0.6; }
  .trace.on { background: #fff; border-color: var(--ink); opacity: 1; }
  .tsw { width: 14px; height: 0; }
  .tax { font-weight: 500; color: var(--sub); }
  .gbox { width: 100%; overflow: hidden; }
  .gbox svg { display: block; }
  .gbtns { display: grid; grid-template-columns: repeat(6, minmax(0, 1fr)); gap: 8px; }
  .gbtns .gbtn:nth-child(1), .gbtns .gbtn:nth-child(2) { grid-column: span 3; height: 48px; border-radius: 24px; }
  .gbtns .gbtn:nth-child(n+3) { grid-column: span 2; height: 44px; font-size: 13px; padding: 0 6px; }
  .stopsq { width: 10px; height: 10px; border-radius: 2px; background: var(--mpx-yellow); }
  .playtri { width: 0; height: 0; border-left: 9px solid var(--ink); border-top: 6px solid transparent; border-bottom: 6px solid transparent; }
  .gtools { display: flex; flex-wrap: wrap; align-items: center; gap: 8px; padding: 2px 8px 0; }
  .gtools .gbtn { height: 40px; font-size: 13px; padding: 0 14px; }

  /* ── calibration ── */
  .calgrid { display: flex; flex-direction: column; gap: 18px; }
  .calsec { padding: 16px; display: flex; flex-direction: column; gap: 12px; }
  .calh { display: flex; align-items: center; gap: 10px; }
  .calh h3 { font-size: 17px; font-weight: 800; }
  .num { width: 28px; height: 28px; border-radius: 14px; background: var(--ink); color: var(--mpx-yellow); font-size: 14px; font-weight: 800; display: grid; place-items: center; flex-shrink: 0; }
  .fld { display: flex; flex-direction: column; gap: 4px; font-size: 12px; font-weight: 600; color: var(--sub); min-width: 0; }
  .fld input { height: 44px; width: 100%; box-sizing: border-box; border-radius: 12px; border: none; background: var(--ground); padding: 0 10px; font-size: 15px; color: var(--ink); outline: none; }
  .fld input:focus { box-shadow: inset 0 0 0 2px var(--ink); }
  .refrow { display: grid; grid-template-columns: minmax(0, 1fr) minmax(0, 1fr) 92px; gap: 8px; align-items: end; }
  .cpdrow, .pair { display: grid; grid-template-columns: minmax(0, 1fr) minmax(0, 1fr); gap: 8px; align-items: end; }
  .three { display: grid; grid-template-columns: repeat(3, minmax(0, 1fr)); gap: 8px; }
  .twoone { display: grid; grid-template-columns: 2fr 1fr; gap: 8px; }
  .twoone .pill { height: 48px; border-radius: 24px; }
  .cap { height: 44px; font-size: 13px; padding: 0 10px; }
  .seg { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 4px; background: var(--hair); border-radius: 14px; padding: 4px; }
  .seg button { height: 40px; border: none; border-radius: 10px; background: transparent; color: #4F4C46; font-size: 14px; font-weight: 700; }
  .seg button.on { background: #fff; color: var(--ink); box-shadow: 0 1px 3px rgba(0, 0, 0, 0.12); }
  .warn { background: #FFF3E6; border-radius: 14px; padding: 10px 12px; font-size: 13px; line-height: 1.45; color: #7A3A00; }
  .sweep { width: 100%; height: 110px; border-radius: 12px; background: var(--ground); display: block; }
  .manual { display: grid; grid-template-columns: 1fr auto auto; gap: 8px; align-items: center; }
  .adcnow { display: flex; flex-direction: column; }
  .adcnow b { font-size: 22px; font-weight: 600; }
  .rbar { position: relative; height: 22px; border-radius: 6px; background: var(--ground); overflow: hidden; }
  .seg-mgn { position: absolute; top: 0; bottom: 0; background: #A39F96; }
  .seg-use { position: absolute; top: 0; bottom: 0; background: var(--ink); }
  .rcur { position: absolute; top: 0; bottom: 0; width: 2px; margin-left: -1px; background: var(--mpx-yellow); box-shadow: 0 0 0 1px var(--ink); }
  .calsec > .pill { height: 52px; border-radius: 26px; font-size: 16px; }
  .calsec > .pill.line { height: 48px; border-radius: 24px; font-size: 14px; }

  /* ── activity ── */
  .actsec { gap: 8px; }
  .acthead { display: flex; align-items: center; gap: 8px; border: none; background: none; padding: 0 6px; font-size: 12px; font-weight: 800; letter-spacing: 0.12em; text-transform: uppercase; color: var(--sub); }
  .acthead .mono { letter-spacing: 0; }
  .logs { max-height: 220px; overflow-y: auto; }
  .logl { display: flex; gap: 10px; padding: 8px 14px; font-size: 13px; }
  .logl.bad { color: var(--danger); }
  .lt { color: var(--sub); flex: none; font-size: 12px; }

  .toast { position: fixed; left: 50%; bottom: calc(24px + var(--safe-bottom, 0px)); transform: translateX(-50%); z-index: 80;
    background: var(--ink); color: #fff; padding: 12px 20px; border-radius: 999px; font-size: 14px; font-weight: 600;
    box-shadow: 0 10px 30px rgba(0, 0, 0, 0.18); max-width: 88%; text-align: center; }
  .toast.bad { background: var(--danger); }

  /* ══ PC ══ */
  .pchead { background: #fff; border-bottom: 1px solid var(--line); padding: 14px 28px; display: flex; flex-wrap: wrap; align-items: center; gap: 12px; }
  .pctitle { display: flex; flex-direction: column; gap: 1px; margin: 0 8px 0 4px; }
  .pctitle h1 { font-size: 22px; font-weight: 800; letter-spacing: -0.02em; }
  .pctitle span { font-size: 12px; color: var(--sub); }
  .tools { display: flex; flex-wrap: wrap; align-items: center; gap: 8px; }
  .connpill { display: flex; align-items: center; gap: 10px; height: 44px; padding: 0 6px 0 14px; border-radius: 22px; background: var(--ground); font-size: 14px; font-weight: 700; }
  .seg.tabs { display: flex; }
  .seg.tabs button { height: 36px; padding: 0 16px; font-size: 13px; color: var(--ink); }
  .pcbody { padding: 22px 28px 28px; display: flex; gap: 22px; align-items: flex-start; }
  .pcnav { flex: 0 0 250px; display: flex; flex-direction: column; gap: 14px; }
  .pcnav .legsec { gap: 6px; }
  .pcnav h2 { font-size: 11px; }
  .pcnav .group { border-radius: 16px; }
  .pcmain { flex: 1 1 520px; min-width: 0; display: flex; flex-direction: column; gap: 14px; }
  .pcaside { flex: 0 0 330px; display: flex; flex-direction: column; gap: 14px; position: sticky; top: 16px; max-height: calc(100vh - 32px); overflow-y: auto; }

  .ptable .thead, .prow.table { display: grid; grid-template-columns: 64px minmax(0, 1fr) 120px 170px; gap: 12px; align-items: center; }
  .ptable .thead { padding: 10px 16px; background: var(--ground); font-size: 11px; font-weight: 700; letter-spacing: 0.06em; text-transform: uppercase; color: var(--sub); }
  .thead .r, .prow.table .pv { text-align: right; }
  .tsec { padding: 12px 16px 6px; font-size: 12px; font-weight: 800; letter-spacing: 0.1em; text-transform: uppercase; color: var(--sub); }
  .prow.table { padding: 7px 16px; }
  .prow.table .addr { width: 40px; height: 26px; border-radius: 8px; }
  .prow.table .pn { font-size: 14px; }
  .prow.table .pv { font-size: 14px; }
  .prow.table .pact { font-size: 13px; }
  .prow.ro { cursor: default; }
  .prow.ro .pn { font-weight: 500; }
  .ptable .tsec + .prow { border-top: none; }

  .scard { padding: 14px; display: flex; flex-direction: column; gap: 12px; }
  .shead { display: flex; align-items: center; gap: 10px; }
  .shead .t { font-size: 17px; font-weight: 800; }
  .shead .d { font-size: 12px; }
  .scard .lc-sw { gap: 10px; }
  .scard .t2 { font-size: 14px; }
  .scard .d2 { font-size: 11px; }
  .scard .switch { width: 48px; height: 30px; border-radius: 15px; }
  .scard .knob { width: 24px; height: 24px; border-radius: 12px; }
  .scard .mrow { font-size: 12px; }
  .mini { border-top: 1px solid var(--hair-2); padding-top: 4px; }
  .mini div { display: flex; justify-content: space-between; padding: 4px 0; font-size: 13px; }
  .mini span { color: #4F4C46; }
  .mini b { font-weight: 600; }
  .edcard { border: 2px solid var(--ink); padding: 14px; display: flex; flex-direction: column; gap: 12px; }
  .edcard .ed-h { font-size: 18px; }
  .edcard .ed-desc { font-size: 13px; }
  .edcard .round.big { width: 44px; height: 44px; font-size: 22px; }
  .edcard .valin { height: 44px; font-size: 18px; border-radius: 12px; }
  .edcard .pill.wide { height: 44px; font-size: 14px; }
  .edcard .facts b { font-size: 14px; }
  .wide .scrim { align-items: flex-start; overflow-y: auto; padding: 60px 0; }
  .caldlg { width: calc(100% - 240px); max-width: 1200px; background: #fff; border-radius: 28px; padding: 24px 28px 28px; display: flex; flex-direction: column; gap: 18px; }
  .calhead { display: flex; align-items: center; gap: 12px; }
  .dlgh { font-size: 26px; font-weight: 800; letter-spacing: -0.02em; text-transform: none; color: var(--ink); margin: 0; }
  .calhead .d { font-size: 14px; }
  .caldlg .calgrid { display: grid; grid-template-columns: repeat(3, minmax(0, 1fr)); gap: 16px; align-items: start; }
  .caldlg .refrow { grid-template-columns: minmax(0, 1fr) minmax(0, 1fr) 84px; }
  @media (max-width: 1240px) { .caldlg { width: calc(100% - 64px); } .pcaside { flex-basis: 300px; } .ptable .thead, .prow.table { grid-template-columns: 52px minmax(0, 1fr) 96px 130px; } }
</style>
