<script>
  import { colors } from "./lib/colors.js";
  import LockScreen from "./lib/LockScreen.svelte";
  import PermissionCard from "./lib/PermissionCard.svelte";

  const YELLOW = colors.mpx.primary;

  // ── Network status (polled from robot) ──────────────────────
  let apIp = $state("192.168.2.1");
  let apSsid = $state("MPX-Dog");
  let staState = $state("disconnected");
  let staSsid = $state("");
  let staIp = $state("");
  let piIp = $state("");
  // mpx-dog: only firmware with the Pi/ROS link reports a `pi` object in
  // /v1/wifi/status. Settings hides the Raspberry Pi section without it.
  let hasPi = $state(false);
  let networkLoaded = $state(false);

  // ── Robot link health ───────────────────────────────────────
  // "online"   → last poll succeeded recently
  // "offline"  → poll failed (unreachable / timed out)
  // "checking" → first poll still in flight
  let linkState = $state("checking");
  let latencyMs = $state(null);
  let lastSeenAt = $state(null);
  let failStreak = 0;

  const POLL_ONLINE_MS = 5_000;
  const POLL_OFFLINE_MS = 2_500;
  const FETCH_TIMEOUT_MS = 4_000;

  let pollTimer = null;

  async function pollNetworkStatus() {
    const ctrl = new AbortController();
    const to = setTimeout(() => ctrl.abort(), FETCH_TIMEOUT_MS);
    const t0 = performance.now();
    try {
      const res = await fetch("/v1/wifi/status", { signal: ctrl.signal, cache: "no-store" });
      if (!res.ok) throw new Error(`HTTP ${res.status}`);
      const data = await res.json();
      apIp = data.ap?.ip ?? "192.168.2.1";
      apSsid = data.ap?.ssid ?? "MPX-Dog";
      staState = data.sta?.state ?? "disconnected";
      staSsid = data.sta?.ssid ?? "";
      staIp = data.sta?.ip ?? "";
      piIp = data.pi?.ip ?? "";
      hasPi = data.pi !== undefined && data.pi !== null;
      networkLoaded = true;
      latencyMs = Math.round(performance.now() - t0);
      lastSeenAt = Date.now();
      failStreak = 0;
      linkState = "online";
    } catch {
      failStreak++;
      // Tolerate a single dropped poll before flipping to offline.
      if (failStreak >= 2 || linkState === "checking") {
        linkState = "offline";
        latencyMs = null;
      }
    } finally {
      clearTimeout(to);
    }
    schedulePoll();
  }

  function schedulePoll() {
    clearTimeout(pollTimer);
    pollTimer = setTimeout(
      pollNetworkStatus,
      linkState === "online" ? POLL_ONLINE_MS : POLL_OFFLINE_MS,
    );
  }

  // ── Connection mode ─────────────────────────────────────────
  // Decide how *this* browser reaches the robot by matching the page
  // host against the robot's interface IPs (falls back to hostname
  // heuristics during development behind a proxy).
  let connectionMode = $derived.by(() => {
    if (typeof window === "undefined") return "sta";
    const host = window.location.hostname;
    if (host === apIp) return "ap";
    if (staIp && host === staIp) return "sta";
    if (host === "192.168.2.1") return "ap";
    if (host === "localhost" || host === "127.0.0.1") return "dev";
    return staState === "connected" ? "sta" : "ap";
  });

  // ── Global permission WebSocket ─────────────────────────────
  // Listens for openclaw_action events (file_write, wasm_run, etc.)
  // and shows a modal so the user can approve/deny from any screen.
  const PERMISSION_WS_URL = `ws://${location.host}/v1/chat/ui`;

  let permissionWs = $state(null);
  let wsConnected = $state(false);
  let pendingActions = $state([]);
  let wsRetryTimer = null;
  let wsLent = false;   // Servo Studio has borrowed the socket (see below)

  function connectPermissionWs() {
    if (permissionWs && permissionWs.readyState === WebSocket.OPEN) return;

    try {
      const ws = new WebSocket(PERMISSION_WS_URL);
      ws.onopen = () => { permissionWs = ws; wsConnected = true; };
      ws.onmessage = (event) => {
        try {
          const data = JSON.parse(event.data);
          if (data.type === "openclaw_action" && data.action_id) {
            if (data.status === "pending") {
              if (!pendingActions.some((a) => a.id === data.action_id)) {
                pendingActions = [...pendingActions, {
                  id: data.action_id,
                  type: data.action_type || "unknown",
                  description: data.description || "",
                  receivedAt: Date.now(),
                }];
              }
            } else {
              pendingActions = pendingActions.filter((a) => a.id !== data.action_id);
            }
          }
        } catch { /* ignore malformed JSON */ }
      };
      ws.onclose = () => {
        permissionWs = null;
        wsConnected = false;
        clearTimeout(wsRetryTimer);
        if (!wsLent) wsRetryTimer = setTimeout(connectPermissionWs, 3000);
      };
      ws.onerror = () => { ws.close(); };
    } catch { /* ignore */ }
  }

  // ── Lending the socket out ──────────────────────────────────
  // The robot's HTTP server has few sockets and LRU-purges the oldest idle
  // one. Servo Studio polls fast and every write must land, so it asks for
  // this permanent WebSocket back on the way in and returns it on the way out
  // (window events "mpx:yield-socket" / "mpx:resume-socket").
  $effect(() => {
    const lend = () => {
      wsLent = true;
      clearTimeout(wsRetryTimer);
      if (permissionWs) permissionWs.close();
    };
    const takeBack = () => { wsLent = false; connectPermissionWs(); };
    window.addEventListener("mpx:yield-socket", lend);
    window.addEventListener("mpx:resume-socket", takeBack);
    return () => {
      window.removeEventListener("mpx:yield-socket", lend);
      window.removeEventListener("mpx:resume-socket", takeBack);
    };
  });

  function respondPermission(actionId, approved) {
    if (permissionWs && permissionWs.readyState === WebSocket.OPEN) {
      permissionWs.send(JSON.stringify({
        type: "permission_response",
        action_id: actionId,
        approved,
        ts: Math.floor(Date.now() / 1000),
      }));
    }
    pendingActions = pendingActions.filter((a) => a.id !== actionId);
  }

  // ── Lifecycle ───────────────────────────────────────────────
  $effect(() => {
    pollNetworkStatus();
    connectPermissionWs();

    // Re-poll immediately when the tab becomes visible again.
    const onVis = () => {
      if (document.visibilityState === "visible") {
        clearTimeout(pollTimer);
        pollNetworkStatus();
      }
    };
    document.addEventListener("visibilitychange", onVis);

    return () => {
      clearTimeout(pollTimer);
      clearTimeout(wsRetryTimer);
      document.removeEventListener("visibilitychange", onVis);
      if (permissionWs) permissionWs.close();
    };
  });

  // ── Pack props for children ─────────────────────────────────
  let network = $derived({
    apIp, apSsid, staState, staSsid, staIp, piIp, hasPi, networkLoaded,
    linkState, latencyMs, lastSeenAt, connectionMode, wsConnected,
    online: linkState === "online",
  });
</script>

<LockScreen {network} />

<!-- ═══ Global Permission Modal Overlay ═══ -->
{#if pendingActions.length > 0}
  <div class="perm-overlay" role="dialog" aria-modal="true" aria-label="Permission required" style="--yellow: {YELLOW}">
    <div class="perm-modal">
      <PermissionCard
        action={pendingActions[0]}
        queued={pendingActions.length - 1}
        onRespond={respondPermission}
      />
    </div>
  </div>
{/if}

<style>
  .perm-overlay {
    position: fixed;
    inset: 0;
    z-index: 100;
    background: rgba(0, 0, 0, 0.55);
    -webkit-backdrop-filter: blur(6px);
    backdrop-filter: blur(6px);
    display: flex;
    align-items: flex-end;
    justify-content: center;
    padding: 20px;
    padding-bottom: calc(20px + var(--safe-bottom));
    animation: perm-fade var(--dur-med) var(--ease-out);
  }
  @media (min-width: 640px) {
    .perm-overlay { align-items: center; }
  }
  @keyframes perm-fade { from { opacity: 0; } to { opacity: 1; } }

  .perm-modal {
    width: 100%;
    max-width: 380px;
    animation: perm-rise var(--dur-slow) var(--ease-spring);
  }
  @keyframes perm-rise {
    from { transform: translateY(24px) scale(0.98); opacity: 0; }
    to   { transform: translateY(0) scale(1); opacity: 1; }
  }
</style>
