/**
 * Marketplace API — communicates with the marketplace Gateway via
 * the robot's proxy endpoints (/v1/marketplace/*).
 *
 * The robot forwards these requests to the Gateway at
 * APP_CHAT_SERVER_IP:APP_CHAT_SERVER_PORT on its STA interface.
 */

let config = null;
let configPromise = null;

/**
 * Fetch the gateway config from the robot.
 * Caches the result so it's only fetched once.
 */
async function getConfig() {
  if (config) return config;
  if (configPromise) return configPromise;

  configPromise = (async () => {
    try {
      const res = await fetch("/v1/gateway/config");
      if (res.ok) {
        config = await res.json();
        return config;
      }
    } catch (e) {
      console.warn("marketplaceApi: failed to fetch config", e);
    }
    return null;
  })();

  return configPromise;
}

// ── Skill Discovery ───────────────────────────────────────────

/**
 * List all skills available in the marketplace.
 * GET /v1/marketplace/skills  →  GET /v1/skills
 */
export async function listSkills() {
  const res = await fetch("/v1/marketplace/skills");
  if (!res.ok) throw new Error(`Failed to list skills: ${res.status}`);
  return res.json();
}

/**
 * Get details on a single skill.
 * GET /v1/marketplace/skills/{skillId}  →  GET /v1/skills/{skillId}
 */
export async function getSkill(skillId) {
  const res = await fetch(`/v1/marketplace/skills/${encodeURIComponent(skillId)}`);
  if (!res.ok) throw new Error(`Failed to get skill: ${res.status}`);
  return res.json();
}

/**
 * Fetch the manifest (README, screenshots, capabilities) for a skill.
 * GET /v1/marketplace/skills/{skillId}/manifest  →  GET /v1/skills/{skillId}/manifest
 */
export async function getSkillManifest(skillId) {
  const res = await fetch(`/v1/marketplace/skills/${encodeURIComponent(skillId)}/manifest`);
  if (!res.ok) throw new Error(`Failed to get manifest: ${res.status}`);
  return res.json();
}

// ── Robot Skill Assignment ─────────────────────────────────────

/**
 * List all skills assigned to this robot (with enabled/disabled state).
 * GET /v1/marketplace/robot/skills  →  GET /v1/robots/{uuid}/skills
 */
export async function listRobotSkills() {
  const res = await fetch("/v1/marketplace/robot/skills");
  if (!res.ok) throw new Error(`Failed to list robot skills: ${res.status}`);
  return res.json();
}

/**
 * Assign (subscribe to) a skill on this robot.
 * POST /v1/marketplace/robot/skills  →  POST /v1/robots/{uuid}/skills
 */
export async function assignSkill(skillId) {
  const res = await fetch("/v1/marketplace/robot/skills", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ skill_id: skillId }),
  });
  if (!res.ok) throw new Error(`Failed to assign skill: ${res.status}`);
  return res.json();
}

/**
 * Toggle a skill on or off for this robot.
 * PATCH /v1/marketplace/robot/skills/{skillId}
 *   → PATCH /v1/robots/{uuid}/skills/{skillId}
 */
export async function toggleSkill(skillId, enabled) {
  const res = await fetch(`/v1/marketplace/robot/skills/${encodeURIComponent(skillId)}`, {
    method: "PATCH",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ enabled }),
  });
  if (!res.ok) throw new Error(`Failed to toggle skill: ${res.status}`);
  return res.json();
}

/**
 * Deploy a WASM skill to the robot.
 * POST /v1/marketplace/robot/deploy
 *   → POST /v1/robots/{uuid}/deploy
 *
 * Returns the deploy response containing skills[] with commands[].
 * Each command has a .script (Lua code) to execute on the robot.
 */
export async function deploySkill(skillId) {
  const res = await fetch("/v1/marketplace/robot/deploy", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ skill_id: skillId }),
  });
  if (!res.ok) throw new Error(`Failed to deploy skill: ${res.status}`);
  return res.json();
}

/**
 * Enqueue a Lua script for async execution (non-blocking).
 * POST /v1/lua/enqueue  →  {"script":"..."}
 * Returns { ok: true } immediately; the script runs in the background.
 */
export async function enqueueLua(script) {
  const res = await fetch("/v1/lua/enqueue", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ script }),
  });
  if (!res.ok) throw new Error(`Lua enqueue failed: ${res.status}`);
  return res.json();
}

/**
 * Remove (refund/uninstall) a skill from this robot.
 * DELETE /v1/marketplace/robot/skills/{skillId}
 *   → DELETE /v1/robots/{uuid}/skills/{skillId}
 */
export async function removeSkill(skillId) {
  const res = await fetch(`/v1/marketplace/robot/skills/${encodeURIComponent(skillId)}`, {
    method: "DELETE",
  });
  if (!res.ok) throw new Error(`Failed to remove skill: ${res.status}`);
  return res.json();
}

// ── Storefront ────────────────────────────────────────────────

/**
 * The whole Store screen in one request.
 *
 * GET /v1/marketplace/storefront  →  GET /v1/storefront
 *
 * Returns { featured, sections, counts, provides, ranking }. The gateway does
 * the ranking so every client agrees on what "Featured" and "New" mean, and
 * states its rule in `ranking` rather than leaving clients to guess.
 *
 * robotUuid matters: without it nothing knows what is already installed or
 * what this robot's hardware can actually run.
 */
export async function getStorefront(robotUuid, type) {
  const params = new URLSearchParams();
  if (robotUuid) params.set("robot_uuid", robotUuid);
  if (type && type !== "all") params.set("type", type);
  const qs = params.toString();

  const res = await fetch(`/v1/marketplace/storefront${qs ? "?" + qs : ""}`);
  if (!res.ok) throw new Error(`Failed to load the store: ${res.status}`);
  return res.json();
}

/**
 * Everything needed to draw the install confirmation for one skill.
 * GET /v1/marketplace/storefront/skills/{id}  →  GET /v1/storefront/skills/{id}
 */
export async function getStorefrontSkill(skillId, robotUuid) {
  const qs = robotUuid ? `?robot_uuid=${encodeURIComponent(robotUuid)}` : "";
  const res = await fetch(
    `/v1/marketplace/storefront/skills/${encodeURIComponent(skillId)}${qs}`,
  );
  if (!res.ok) throw new Error(`Failed to load skill: ${res.status}`);
  return res.json();
}

/**
 * Install a skill: assign it to the robot, then switch it on.
 *
 * Two calls because the gateway keeps ownership and activation separate — you
 * can own a skill and have it off. From the owner's side "Install" means both,
 * so this does both rather than leaving a skill installed-but-inert with no
 * explanation.
 *
 * If the enable half fails the skill IS still assigned, so this reports which
 * half succeeded rather than a bare failure — the caller can tell the user to
 * flip the toggle instead of trying to install again.
 */
export async function installSkill(skillId) {
  await assignSkill(skillId);
  try {
    await toggleSkill(skillId, true);
    return { assigned: true, enabled: true };
  } catch (err) {
    return { assigned: true, enabled: false, error: err.message };
  }
}

export { getConfig };
