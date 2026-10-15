/**
 * MPX Color Palette — single source of truth
 * Import this into tailwind.config.js and anywhere else colors are needed.
 */

export const colors = {
  // Semantic names kept for every existing call site; the VALUES are now the
  // CSS variables from app.css, so inline styles follow the theme too.
  // (Tailwind 3 emits `background-color: var(--x)` for these -- opacity
  // modifiers like bg-mpx-primary/50 will not work, and nothing uses them.)
  mpx: {
    primary: "var(--accent)",
    secondary: "var(--accent-line)",
    bg_white: "var(--bg)",
    bg_grey: "var(--surface-3)",
    menu_white: "var(--surface-1)",
    menu_grey: "var(--surface-2)",
    menu_blue: "var(--info)",
    menu_green: "var(--ok)",
    text: "var(--text-1)",
    text_neutral: "var(--text-3)",
    action_green: "var(--ok)",
    action_red: "var(--danger)",
    action_purple: "#A476ED",
    action_blue: "var(--info)"
  },

  // ── Skill type colors ────────────────────────────────────────
  //
  // One colour per kind of skill, so the list is scannable without reading
  // any labels. Deliberately far apart in hue AND in lightness, so they stay
  // distinguishable for the ~8% of men with red-green colour blindness --
  // green and orange are the risky pair, which is why the chips carry text
  // as well and colour is never the only signal. Muted a step from the
  // originals so they sit inside the neutral palette instead of on top of it.
  skillType: {
    capability: "#E39A5A",  // orange -- AI skills: sensors, movement, reasoning
    awa:        "#5FB574",  // green  -- Web skills: browse and shop on websites
    wasm:       "#9C86DE",  // purple -- Motion skills: run on the robot itself
    type4:      "#6FA3D8",  // blue   -- reserved
  },
};

/**
 * Returns the color for a given skill type string (case-insensitive).
 * Falls back to a neutral gray for unknown types.
 * @param {string} skillType - e.g. "awa", "wasm"
 * @returns {string} hex color
 */
export function skillTypeColor(skillType) {
  const key = (skillType || "").toLowerCase();
  return colors.skillType[key] || "var(--text-3)";
}

/**
 * Human-readable label for a skill type.
 * @param {string} skillType
 * @returns {string}
 */
export function skillTypeLabel(skillType) {
  const key = (skillType || "").toLowerCase();
  // "awa" used to be labelled "AISkill", which was wrong and became actively
  // confusing once real AI skills existed: AWA drives a web browser and has no
  // access to a sensor, a servo or a model. Labels now say what the skill
  // actually does, in the words an owner would use.
  const labels = {
    capability: "AI",
    awa: "Web",
    wasm: "Motion",
    type4: "TYPE4",
  };
  return labels[key] || skillType?.toUpperCase() || "?";
}

/**
 * One line explaining what a skill type can do, for the owner rather than the
 * developer. Shown under the filter tabs.
 */
export function skillTypeBlurb(skillType) {
  const key = (skillType || "").toLowerCase();
  const blurbs = {
    capability: "Senses, thinks and moves — runs in the cloud, acts on the robot.",
    awa: "Browses websites for you, like shopping and price checks.",
    wasm: "Runs directly on the robot. Movement and gaits, no internet needed.",
  };
  return blurbs[key] || "";
}

/**
 * Plain-English name for a capability a skill has asked for.
 *
 * The manifest says "sense:activity"; an owner deciding whether to trust a
 * third-party skill needs "Read motion sensors". This is the phone-app
 * permission prompt, and it is the only place most owners will ever see what
 * a skill is actually allowed to touch.
 */
export function capabilityLabel(entry) {
  const ns = String(entry || "").split(":")[0].toLowerCase();
  const labels = {
    sense: "Read motion sensors",
    robot: "Move the robot",
    vision: "Use the camera",
    voice: "Listen and speak",
    brain: "Use AI reasoning",
    store: "Remember things",
    web: "Browse websites",
  };
  return labels[ns] || ns;
}

/**
 * Capabilities an owner should think twice about granting.
 *
 * Movement can hurt a bystander; camera and microphone are recording in
 * someone's home; AI reasoning costs money. These get a warning treatment
 * rather than being listed as flatly as "remember things".
 */
export function capabilityIsSensitive(entry) {
  const ns = String(entry || "").split(":")[0].toLowerCase();
  return ["robot", "vision", "voice", "brain"].includes(ns);
}
