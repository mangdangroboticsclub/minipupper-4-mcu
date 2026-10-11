/**
 * MPX Color Palette — single source of truth
 * Import this into tailwind.config.js and anywhere else colors are needed.
 */

export const colors = {
  mpx: {
    primary: "#FFE605",
    secondary: "#A49714",
    bg_white: "#E8E8E8",
    bg_grey: "#949393",
    menu_white: "#FFFFFF",
    menu_grey: "#D9D9D9",
    menu_blue: "#0357B7",
    menu_green: "#1FBD00",
    text: "#000000",
    text_neutral: "#969494",
    action_green: "#6AAE6C",
    action_red: "#ED7676",
    action_purple: "#A476ED",
    action_blue: "#89B0DB"
  },

  // ── Skill type colors (4 slots: AWA, WASM, + 2 future types) ──
  skillType: {
    awa:  "#6AAE6C",   // green — AWA skills (Lua-based)
    wasm: "#A476ED",   // purple — WASM skills
    type3: "#F4A261",  // orange — reserved for future skill type
    type4: "#89B0DB",  // blue — reserved for future skill type
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
  return colors.skillType[key] || "#949393";
}

/**
 * Human-readable label for a skill type.
 * @param {string} skillType
 * @returns {string}
 */
export function skillTypeLabel(skillType) {
  const key = (skillType || "").toLowerCase();
  const labels = {
    awa: "AISkill",
    wasm: "MoveSkill",
    type3: "TYPE3",
    type4: "TYPE4",
  };
  return labels[key] || skillType?.toUpperCase() || "?";
}
