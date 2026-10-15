<script>
  /**
   * Install confirmation — the permission review.
   *
   * ── WHY THIS EXISTS ──────────────────────────────────────────────────────
   *
   * Installing assigns a skill that can move a machine near a person. Firing
   * POST /v1/robots/{uuid}/skills straight from a card tap means an owner
   * grants that without ever being shown it.
   *
   * So permissions are written as what the skill can DO, not as capability
   * names, and anything that touches hardware or spends money carries the
   * consequence on the line below it.
   *
   * Cancel is a full-weight button, not a grey ghost. If refusing is harder
   * than accepting, the sheet is theatre rather than consent.
   */
  import { skillTypeColor, skillTypeLabel } from "./colors.js";
  import { installSkill } from "./marketplaceApi.js";

  let { skill, onclose, oninstalled } = $props();

  let busy = $state(false);
  let error = $state("");

  /**
   * What each capability lets a skill do, in the owner's words.
   *
   * `detail` is the consequence, not a restatement of the title — "can walk
   * without asking again" is the part someone needs before agreeing, and it
   * is the part a capability name never conveys.
   *
   * `weight` marks the ones worth hesitating over: hardware that moves,
   * sensors that record, and anything that spends the owner's allowance.
   */
  const PERMISSION = {
    sense: {
      title: "Read motion sensors",
      detail: "Knows when the robot is moved, tilted or picked up",
      weight: "normal",
    },
    robot: {
      title: "Move the robot",
      detail: "Can walk, turn and drive the legs without asking again",
      weight: "physical",
    },
    vision: {
      title: "Use the camera",
      detail: "Can see and identify what is in front of the robot",
      weight: "physical",
    },
    voice: {
      title: "Listen and speak",
      detail: "Can hear what is said nearby and talk back",
      weight: "physical",
    },
    brain: {
      title: "Use AI reasoning",
      detail: "Sends what it sees to the AI, which uses your allowance",
      weight: "costs",
    },
    store: {
      title: "Remember things",
      detail: "Keeps notes between runs, on this robot only",
      weight: "normal",
    },
    web: {
      title: "Browse websites",
      detail: "Opens pages and reads them on your behalf",
      weight: "normal",
    },
  };

  const LABEL = { physical: "Physical", costs: "Costs" };

  let permissions = $derived(
    (skill?.permissions ?? []).map((key) => ({
      key,
      ...(PERMISSION[key] ?? {
        title: key,
        // An unknown capability is more alarming than a known one, not less:
        // it means this app is older than the skill it is being asked to
        // trust. Say so rather than rendering a bare namespace.
        detail: "This app does not recognise this permission — update the robot",
        weight: "physical",
      }),
    })),
  );

  async function confirm() {
    busy = true;
    error = "";
    try {
      const result = await installSkill(skill.skill_id);
      oninstalled?.(result);
    } catch (err) {
      error = err.message || "Install failed";
      busy = false;
    }
  }
</script>

<!-- Scrim. Tapping it cancels — the same escape a hardware back button gives. -->
<div
  class="is-scrim"
  role="button"
  tabindex="0"
  aria-label="Cancel"
  onclick={() => !busy && onclose?.()}
  onkeydown={(e) => e.key === "Escape" && !busy && onclose?.()}
></div>

<div class="is-sheet" role="dialog" aria-modal="true" aria-label="Confirm install">
  <div class="is-grip"></div>

  <div class="is-head">
    <div class="is-avatar" style="--badge: {skillTypeColor(skill.skill_type)}">
      {(skill.title || "?").charAt(0).toUpperCase()}
    </div>
    <div class="is-id">
      <div class="is-title-row">
        <h2 class="is-title">{skill.title}</h2>
        <span class="is-chip" style="--badge: {skillTypeColor(skill.skill_type)}">
          {skillTypeLabel(skill.skill_type)}
        </span>
      </div>
      <div class="is-meta">{skill.publisher} · v{skill.current_version}</div>
    </div>
  </div>

  {#if skill.readme}
    <p class="is-desc">{skill.readme}</p>
  {/if}

  {#if permissions.length}
    <div class="is-perm-head">This skill will be able to</div>
    <div class="is-perms">
      {#each permissions as p (p.key)}
        <div class="is-perm">
          <div class="is-perm-icon" class:is-perm-icon-warn={p.weight !== "normal"}>
            <svg width="15" height="15" viewBox="0 0 24 24" fill="none"
                 stroke={p.weight === "normal" ? "#7a7a7a" : "#C4802F"}
                 stroke-width="2.1" stroke-linecap="round" stroke-linejoin="round">
              {#if p.key === "robot"}
                <path d="M5 19V10M10 19V5M15 19v-6M20 19v-3"></path>
              {:else if p.key === "vision"}
                <path d="M3.5 8.5h4L9 6.5h6L16.5 8.5h4v10h-17z"></path>
                <circle cx="12" cy="13" r="3.2"></circle>
              {:else if p.key === "voice"}
                <path d="M12 3.5a2.6 2.6 0 0 1 2.6 2.6v5a2.6 2.6 0 0 1-5.2 0v-5A2.6 2.6 0 0 1 12 3.5z"></path>
                <path d="M6.5 11.5a5.5 5.5 0 0 0 11 0M12 17v3.5"></path>
              {:else if p.key === "brain"}
                <path d="M12 3.5a4.2 4.2 0 0 1 4.2 4.2v.9a4.2 4.2 0 0 1-1.6 3.3v1.7a2.6 2.6 0 0 1-5.2 0v-1.7A4.2 4.2 0 0 1 7.8 8.6v-.9A4.2 4.2 0 0 1 12 3.5z"></path>
                <path d="M9.8 17.6h4.4"></path>
              {:else if p.key === "store"}
                <path d="M4.5 6.5h15v12h-15z"></path><path d="M4.5 10.5h15M9 6.5v4"></path>
              {:else if p.key === "web"}
                <circle cx="12" cy="12" r="8.6"></circle>
                <path d="M3.4 12h17.2M12 3.4c2.4 2.6 2.4 14.6 0 17.2M12 3.4c-2.4 2.6-2.4 14.6 0 17.2"></path>
              {:else}
                <path d="M12 3v4M12 17v4M3 12h4M17 12h4"></path>
                <circle cx="12" cy="12" r="3.4"></circle>
              {/if}
            </svg>
          </div>
          <div class="is-perm-body">
            <div class="is-perm-title-row">
              <span class="is-perm-title">{p.title}</span>
              {#if p.weight !== "normal"}
                <span class="is-perm-flag">{LABEL[p.weight]}</span>
              {/if}
            </div>
            <div class="is-perm-detail" class:is-perm-detail-warn={p.weight !== "normal"}>
              {p.detail}
            </div>
          </div>
        </div>
      {/each}
    </div>
  {:else}
    <p class="is-noperm">
      This skill declares no permissions. It cannot read the robot's sensors or
      move it.
    </p>
  {/if}

  <!-- What the platform does and does not check. Uncomfortable and true:
       there is no review pipeline, and implying one would be worse. -->
  <div class="is-note">
    <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="#9a9a9a" stroke-width="2.1" stroke-linecap="round">
      <circle cx="12" cy="12" r="9"></circle><path d="M12 11v5M12 7.6v.5"></path>
    </svg>
    <p>
      Published by <strong>{skill.publisher}</strong>, who is not verified by MPX.
      It can only do what is listed above — you can turn it off at any time.
    </p>
  </div>

  {#if error}
    <p class="is-error">{error}</p>
  {/if}

  <div class="is-actions">
    <button class="is-btn is-btn-cancel" disabled={busy} onclick={() => onclose?.()}>
      Cancel
    </button>
    <button class="is-btn is-btn-go" disabled={busy} onclick={confirm}>
      {busy ? "Installing…" : "Install"}
    </button>
  </div>
</div>

<style>
  .is-scrim {
    position: absolute;
    inset: 0;
    background: var(--surface-3);
    border: none;
    z-index: 40;
  }

  .is-sheet {
    position: absolute;
    left: 0;
    right: 0;
    bottom: 0;
    z-index: 41;
    background: var(--surface-1);
    border-radius: 22px 22px 0 0;
    padding: 10px 20px 22px;
    box-shadow: 0 -8px 40px rgba(0, 0, 0, 0.22);
    max-height: 88%;
    overflow-y: auto;
    -webkit-overflow-scrolling: touch;
  }

  .is-grip {
    width: 38px;
    height: 4px;
    border-radius: 999px;
    background: var(--surface-3);
    margin: 0 auto 18px;
  }

  .is-head { display: flex; align-items: center; gap: 13px; }

  .is-avatar {
    width: 52px;
    height: 52px;
    flex-shrink: 0;
    border-radius: 15px;
    background: var(--badge, var(--surface-3));
    color: var(--solid-ink);
    display: flex;
    align-items: center;
    justify-content: center;
    font-size: 1.25rem;
    font-weight: 800;
  }

  .is-id { min-width: 0; flex: 1; }
  .is-title-row { display: flex; align-items: center; gap: 7px; }

  .is-title {
    margin: 0;
    font-size: 1.15rem;
    font-weight: 800;
    letter-spacing: -0.025em;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }

  .is-chip {
    flex-shrink: 0;
    padding: 2px 7px;
    border-radius: 999px;
    background: var(--badge, var(--surface-3));
    color: var(--solid-ink);
    font-size: 0.56rem;
    font-weight: 800;
    letter-spacing: 0.05em;
    text-transform: uppercase;
  }

  .is-meta { font-size: 0.74rem; color: var(--text-3); margin-top: 3px; }

  .is-desc {
    margin: 15px 0 0;
    font-size: 0.81rem;
    line-height: 1.5;
    color: #4f4f4f;
  }

  .is-perm-head {
    margin-top: 20px;
    font-size: 0.7rem;
    font-weight: 800;
    letter-spacing: 0.09em;
    text-transform: uppercase;
    color: var(--text-3);
  }

  .is-perms { display: flex; flex-direction: column; gap: 14px; margin-top: 13px; }
  .is-perm { display: flex; gap: 12px; align-items: flex-start; }

  .is-perm-icon {
    width: 30px;
    height: 30px;
    flex-shrink: 0;
    border-radius: 9px;
    background: var(--surface-2);
    display: flex;
    align-items: center;
    justify-content: center;
  }

  /* Colour is never the only signal — the border changes too. */
  .is-perm-icon-warn { background: #fff6e8; border: 1px solid #f0c99a; }

  .is-perm-body { flex: 1; padding-top: 1px; min-width: 0; }
  .is-perm-title-row { display: flex; align-items: center; gap: 6px; flex-wrap: wrap; }
  .is-perm-title { font-size: 0.83rem; font-weight: 700; }

  .is-perm-flag {
    padding: 1px 6px;
    border-radius: 4px;
    background: #fff6e8;
    border: 1px solid #f0c99a;
    color: #8a5a1c;
    font-size: 0.56rem;
    font-weight: 800;
    letter-spacing: 0.04em;
    text-transform: uppercase;
  }

  .is-perm-detail {
    font-size: 0.72rem;
    color: var(--text-3);
    line-height: 1.4;
    margin-top: 2px;
  }
  .is-perm-detail-warn { color: #8a5a1c; }

  .is-noperm {
    margin: 18px 0 0;
    font-size: 0.78rem;
    line-height: 1.45;
    color: var(--text-2);
  }

  .is-note {
    display: flex;
    gap: 9px;
    margin-top: 20px;
    padding: 11px 12px;
    background: var(--surface-2);
    border-radius: 11px;
  }
  .is-note svg { flex-shrink: 0; margin-top: 1px; }
  .is-note p { margin: 0; font-size: 0.71rem; line-height: 1.45; color: #7a7a7a; }
  .is-note strong { color: #5a5a5a; }

  .is-error {
    margin: 13px 0 0;
    padding: 10px 12px;
    border-radius: 10px;
    background: #fdecec;
    color: #9c3535;
    font-size: 0.75rem;
    line-height: 1.4;
  }

  .is-actions { display: flex; gap: 9px; margin-top: 20px; }

  .is-btn {
    padding: 14px;
    border: none;
    border-radius: 12px;
    font-size: 0.85rem;
    font-weight: 800;
    cursor: pointer;
  }
  .is-btn:disabled { opacity: 0.5; cursor: default; }

  /* Refusing must be as easy as accepting. */
  .is-btn-cancel { flex: 1; background: var(--surface-2); color: #3a3a3a; }
  .is-btn-go { flex: 1.4; background: var(--solid); color: var(--accent); }
</style>
