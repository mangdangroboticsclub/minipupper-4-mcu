/**
 * Theme -- dark (default), light, or follow the OS.
 *
 * Applied as data-theme on <html>, which is what app.css keys its tokens on.
 * Stored in localStorage; "system" stores nothing and listens to the OS.
 */
const KEY = "mpx.theme";
const media = typeof matchMedia === "function" ? matchMedia("(prefers-color-scheme: light)") : null;

export function getThemePreference() {
  try { return localStorage.getItem(KEY) || "dark"; } catch { return "dark"; }
}

function resolve(pref) {
  if (pref === "system") return media && media.matches ? "light" : "dark";
  return pref === "light" ? "light" : "dark";
}

export function applyTheme(pref = getThemePreference()) {
  document.documentElement.dataset.theme = resolve(pref);
  const meta = document.querySelector('meta[name="theme-color"]');
  if (meta) meta.content = resolve(pref) === "light" ? "#F2F2F4" : "#0E0E10";
}

export function setThemePreference(pref) {
  try {
    if (pref === "dark") localStorage.removeItem(KEY); else localStorage.setItem(KEY, pref);
  } catch { /* private mode: still apply for this session */ }
  applyTheme(pref);
}

if (media) media.addEventListener("change", () => { if (getThemePreference() === "system") applyTheme("system"); });
