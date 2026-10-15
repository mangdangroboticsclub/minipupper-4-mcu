/**
 * Marketplace API — communicates with the marketplace Gateway via
 * the robot's proxy endpoints (/v1/marketplace/*).
 *
 * The robot forwards these requests to the Gateway at
 * APP_CHAT_SERVER_IP:APP_CHAT_SERVER_PORT on its STA interface.
 */

import { detectCurrency } from "./currency.js";

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
 * GET /v1/marketplace/skills?currency=…  →  GET /v1/skills?currency=…
 */
export async function listSkills() {
  const qs = `?currency=${encodeURIComponent(detectCurrency())}`;
  const res = await fetch(`/v1/marketplace/skills${qs}`);
  if (!res.ok) throw new Error(`Failed to list skills: ${res.status}`);
  return res.json();
}

/**
 * Get details on a single skill.
 * GET /v1/marketplace/skills/{skillId}?currency=…
 *   → GET /v1/skills/{skillId}?currency=…
 */
export async function getSkill(skillId) {
  const qs = `?currency=${encodeURIComponent(detectCurrency())}`;
  const res = await fetch(`/v1/marketplace/skills/${encodeURIComponent(skillId)}${qs}`);
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

// ── Checkout & Orders ─────────────────────────────────────────

/**
 * Create a checkout order + hosted HTTPS checkout URL for a skill on
 * this robot. The robot injects its own UUID into the path.
 * POST /v1/marketplace/robot/checkout  →  POST /v1/robots/{uuid}/checkout
 * Returns { order_id, checkout_url, status, ... }.
 */
export async function createCheckout(skillId) {
  const res = await fetch("/v1/marketplace/robot/checkout", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ skill_id: skillId }),
  });
  if (!res.ok) throw new Error(`Failed to create checkout: ${res.status}`);
  return res.json();
}

/**
 * Poll a checkout order's status.
 * GET /v1/marketplace/orders/{orderId}  →  GET /v1/orders/{orderId}
 */
export async function getOrder(orderId) {
  const res = await fetch(`/v1/marketplace/orders/${encodeURIComponent(orderId)}`);
  if (!res.ok) throw new Error(`Failed to get order: ${res.status}`);
  return res.json();
}

/**
 * List this robot's orders (purchase history).
 * GET /v1/marketplace/robot/orders  →  GET /v1/robots/{uuid}/orders
 */
export async function listOrders() {
  const res = await fetch("/v1/marketplace/robot/orders");
  if (!res.ok) throw new Error(`Failed to list orders: ${res.status}`);
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

export { getConfig };
