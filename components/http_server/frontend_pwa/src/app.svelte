<script>
  import { colors } from "./lib/colors.js";
  import LockScreen from "./lib/LockScreen.svelte";

  const YELLOW = colors.mpx.primary;

  // ── Network status (polled from robot) ──────────────────────
  let apIp = $state("192.168.2.1");
  let apSsid = $state("MPX-Dog");
  let staState = $state("disconnected");
  let staSsid = $state("");
  let staIp = $state("");
  let networkLoaded = $state(false);

  let pollTimer;

  async function pollNetworkStatus() {
    try {
      const res = await fetch("/v1/wifi/status");
      if (res.ok) {
        const data = await res.json();
        apIp = data.ap?.ip ?? "192.168.2.1";
        apSsid = data.ap?.ssid ?? "MPX-Dog";
        staState = data.sta?.state ?? "disconnected";
        staSsid = data.sta?.ssid ?? "";
        staIp = data.sta?.ip ?? "";
        networkLoaded = true;
      }
    } catch {
      // Robot unreachable — keep last known state
    }
  }

  // ── Global permission WebSocket ─────────────────────────────
  // Listens for openclaw_action events (file_write, wasm_run, etc.)
  // and shows a modal so the user can approve/deny from any screen.
  const PERMISSION_WS_URL = `ws://${location.host}/v1/chat/ui`;

  /**
   * The socket handle is a PLAIN `let`, not `$state`, and this is the whole
   * point rather than an oversight.
   *
   * It used to be `$state(null)`, and `connectPermissionWs()` read it on its
   * first line — inside the `$effect` below. That made the effect depend on
   * it, so the socket's own `onopen` assignment invalidated the effect that
   * had just created the socket: cleanup closed it, `onclose` nulled the
   * handle, the effect re-ran, opened another one, and round it went. Each
   * turn of the loop also fired an immediate /v1/wifi/status, which is why the
   * robot logged that endpoint every ~60 ms and then ran out of sockets
   * entirely ("error in accept (23)", OPEN SOCKETS = 16/16). Nothing renders
   * from this handle, so it has no business being reactive.
   *
   * Same shape as ChatView's socket, which was hardened for this already.
   */
  let permissionWs = null;
  let permReconnect = null;   // single-flight: never stack pending reconnects
  let permStopped = false;    // set on teardown so a close does not reconnect
  let pendingActions = $state([]);

  function schedulePermReconnect() {
    if (permReconnect || permStopped) return;
    permReconnect = setTimeout(() => {
      permReconnect = null;
      // A hidden tab holds a socket the robot only has five of; it will
      // reconnect when the screen comes back.
      if (!permStopped && document.visibilityState !== "hidden") connectPermissionWs();
    }, 3000);
  }

  function connectPermissionWs() {
    if (permStopped) return;
    // CONNECTING counts: without it, a reconnect fired while the previous
    // handshake was still in flight opened a second socket for the same slot.
    if (permissionWs && (permissionWs.readyState === WebSocket.OPEN ||
                         permissionWs.readyState === WebSocket.CONNECTING)) return;

    if (permissionWs) {
      permissionWs.onopen = permissionWs.onclose =
        permissionWs.onerror = permissionWs.onmessage = null;
      try { permissionWs.close(); } catch { /* already gone */ }
      permissionWs = null;
    }

    try {
      const ws = new WebSocket(PERMISSION_WS_URL);
      permissionWs = ws;   // claim the slot now, so a re-entrant call is guarded
      ws.onmessage = (event) => {
        if (ws !== permissionWs) return;
        try {
          const data = JSON.parse(event.data);
          if (data.type === "openclaw_action" && data.action_id) {
            if (data.status === "pending") {
              pendingActions = [...pendingActions, {
                id: data.action_id,
                type: data.action_type || "unknown",
                description: data.description || "",
              }];
            } else {
              pendingActions = pendingActions.filter((a) => a.id !== data.action_id);
            }
          }
        } catch { /* ignore malformed JSON */ }
      };
      ws.onclose = () => {
        if (ws !== permissionWs) return;   // a replaced socket closing: not ours
        permissionWs = null;
        schedulePermReconnect();
      };
      ws.onerror = () => { if (ws === permissionWs) { try { ws.close(); } catch {} } };
    } catch { schedulePermReconnect(); }
  }

  function disconnectPermissionWs() {
    permStopped = true;
    if (permReconnect) { clearTimeout(permReconnect); permReconnect = null; }
    if (permissionWs) {
      permissionWs.onopen = permissionWs.onclose =
        permissionWs.onerror = permissionWs.onmessage = null;
      try { permissionWs.close(); } catch { /* already gone */ }
      permissionWs = null;
    }
  }

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

  // ── Poll on mount, refresh every 15 s ───────────────────────
  // Deliberately reads NO reactive state. An $effect re-runs whenever anything
  // it touched changes, and everything in here — a poll, an interval, a socket
  // — is a side effect that must happen exactly once per mount. Reading a
  // single `$state` in this block is enough to turn it into a request loop.
  $effect(() => {
    permStopped = false;
    pollNetworkStatus();
    pollTimer = setInterval(pollNetworkStatus, 15_000);
    connectPermissionWs();
    return () => {
      clearInterval(pollTimer);
      disconnectPermissionWs();
    };
  });

  // ── Lending the socket out ──────────────────────────────────
  // The robot's HTTP server allows five open sockets and LRU-purges the
  // oldest idle one when a sixth arrives (main/network/http_server.cc). This
  // permanent WebSocket is one of those five, all session long.
  //
  // Servo Studio is the one screen where that matters: it polls twice a
  // second, reads live values eight times a second, and every write it sends
  // has to survive the trip. When a purge lands on a keep-alive socket the
  // browser was about to reuse, that write dies before it leaves the phone.
  //
  // So Studio asks for the socket back on the way in and returns it on the
  // way out. Nothing needs the permission channel while you are tuning gains,
  // and any action raised meanwhile is still pending when the socket comes
  // back — the robot re-announces it.
  $effect(() => {
    const lend   = () => disconnectPermissionWs();
    const takeBack = () => { permStopped = false; connectPermissionWs(); };
    window.addEventListener("mpx:yield-socket",  lend);
    window.addEventListener("mpx:resume-socket", takeBack);
    return () => {
      window.removeEventListener("mpx:yield-socket",  lend);
      window.removeEventListener("mpx:resume-socket", takeBack);
    };
  });

  /* ── What the dialog says ────────────────────────────────────
   * "Permission Required / The robot is requesting access to perform an
   * action / 🔧 file write / ✅ Approve" described the plumbing, not the
   * decision. Someone who has just tapped Download on a skill is being asked
   * a question they already know the answer to, in words that make it sound
   * like something has gone wrong.
   *
   * The robot distinguishes an install from an ordinary write, so this can
   * ask the actual question and label the button with the actual verb — the
   * one thing that reliably tells a person what a button will do. */
  function permCopy(action) {
    switch (action?.type) {
    case "skill_install":
      return {
        title: "Install this skill?",
        subtitle: "It will be added to this robot and appear under Skills.",
        label: "Skill install",
        foot: "Skills run in a sandbox: they can move the robot, but cannot "
            + "reach your network or the rest of its storage.",
        confirm: "Install",
      };
    case "file_delete":
      return {
        title: "Delete this file?",
        subtitle: "This removes it from the robot's storage.",
        label: "File delete",
        foot: "This cannot be undone from here.",
        confirm: "Delete",
      };
    default:
      return {
        title: "Save this file?",
        subtitle: "Something running on the robot wants to write to its storage.",
        label: (action?.type || "action").replace(/_/g, " "),
        foot: "Only allow this if you started it.",
        confirm: "Allow",
      };
    }
  }

  // ── Pack props for children ─────────────────────────────────
  let network = $derived({ apIp, apSsid, staState, staSsid, staIp, networkLoaded });
</script>

<LockScreen {network} />

<!-- ═══ Global Permission Modal Overlay ═══ -->
{#if pendingActions.length > 0}
  <!-- svelte-ignore a11y_click_events_have_key_events -->
  <!-- svelte-ignore a11y_no_static_element_interactions -->
  <div
    class="perm-overlay"
    onclick={() => {}}
  >
    <div
      class="perm-modal"
      style="--yellow: {YELLOW}"
      onclick={(e) => e.stopPropagation()}
    >
      <h3 class="perm-title">{permCopy(pendingActions[0]).title}</h3>
      <p class="perm-subtitle">{permCopy(pendingActions[0]).subtitle}</p>

      {#each pendingActions as action}
        <div class="perm-action-card">
          <div class="perm-action-header">
            <span class="perm-action-type">{permCopy(action).label}</span>
          </div>
          <p class="perm-action-desc">{action.description}</p>
        </div>
      {/each}

      <p class="perm-foot">{permCopy(pendingActions[0]).foot}</p>

      <div class="perm-buttons">
        <button
          class="perm-btn perm-btn-deny"
          onclick={() => respondPermission(pendingActions[0].id, false)}
        >Cancel</button>
        <button
          class="perm-btn perm-btn-approve"
          onclick={() => respondPermission(pendingActions[0].id, true)}
        >{permCopy(pendingActions[0]).confirm}</button>
      </div>
    </div>
  </div>
{/if}

<style>
  /* ── Permission Modal Overlay ──────────── */
  .perm-overlay {
    position: fixed;
    inset: 0;
    z-index: 100;
    background: var(--surface-3);
    display: flex;
    align-items: center;
    justify-content: center;
    padding: 20px;
  }

  .perm-modal {
    width: 100%;
    max-width: 320px;
    background: var(--surface-1);
    border-radius: 20px;
    padding: 24px 20px 20px;
    box-shadow: 0 20px 60px rgba(0, 0, 0, 0.3);
  }

  .perm-title {
    font-size: 1.05rem;
    font-weight: 700;
    color: var(--text-1);
    margin-bottom: 2px;
  }

  .perm-subtitle {
    font-size: 0.78rem;
    color: var(--text-3);
    margin-bottom: 16px;
  }

  .perm-action-card {
    background: var(--surface-2);
    border-radius: 12px;
    padding: 14px;
    margin-bottom: 14px;
  }

  .perm-action-header {
    display: flex;
    align-items: center;
    gap: 8px;
    margin-bottom: 4px;
  }

  /* The footnote that says what the robot will and will not let a skill do.
     Small, but it is the sentence that makes Install a considered tap rather
     than a reflex. */
  .perm-foot {
    font-size: 0.72rem;
    line-height: 1.45;
    color: #7a7a7a;
    margin: 10px 2px 16px;
  }

  .perm-action-type {
    font-size: 0.85rem;
    font-weight: 600;
    color: var(--text-1);
    text-transform: capitalize;
  }

  .perm-action-desc {
    font-size: 0.75rem;
    color: var(--text-3);
    margin-left: 28px;
    line-height: 1.4;
  }

  .perm-buttons {
    display: flex;
    gap: 10px;
  }

  .perm-btn {
    flex: 1;
    padding: 12px 0;
    border-radius: 12px;
    border: none;
    font-size: 0.85rem;
    font-weight: 600;
    cursor: pointer;
    text-align: center;
    transition: filter 0.15s;
  }

  .perm-btn-deny {
    background: var(--surface-2);
    color: var(--danger);
    border: 1.5px solid var(--danger);
  }
  .perm-btn-deny:active { filter: brightness(0.95); }

  .perm-btn-approve {
    background: var(--accent);
    color: var(--accent-ink);
  }
  .perm-btn-approve:active { filter: brightness(0.9); }
</style>
