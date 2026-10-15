import { mount } from "svelte";
import App from "./app.svelte";
import "./app.css";
import { applyTheme } from "./lib/theme.js";

// Before the first paint, so the page never flashes the wrong theme.
applyTheme();

const app = mount(App, {
  target: document.getElementById("app"),
});

export default app;

/* ── Service Worker Registration (PWA) ───────────────────────
 *
 * Registering is the easy half. The half that actually matters is noticing
 * when a NEW service worker has taken over and reloading so the running page
 * is not the old build talking to new firmware.
 *
 * Without this, flashing the robot required clearing browsing data before the
 * updated interface appeared. The service worker was serving a cached /m.js
 * forever; see public/sw.js for the cache-first bug that caused it.
 */
if ("serviceWorker" in navigator) {
  window.addEventListener("load", async () => {
    try {
      const registration = await navigator.serviceWorker.register("/sw.js", {
        scope: "/",
        // Never let the browser serve sw.js itself from its HTTP cache. A
        // stale worker script cannot be replaced by a newer one it never sees.
        updateViaCache: "none",
      });

      console.log("[sw] registered:", registration.scope);

      // ── Reload once when a new worker takes control ──────────────
      //
      // Guarded: skipWaiting() in the worker fires controllerchange, and
      // without the flag a page can reload, activate again, and loop.
      let reloading = false;
      navigator.serviceWorker.addEventListener("controllerchange", () => {
        if (reloading) return;
        reloading = true;
        console.log("[sw] new version active — reloading");
        window.location.reload();
      });

      // Ask the browser to re-check sw.js now rather than on its own
      // schedule, so a robot flashed while the tab was open updates on the
      // next visit instead of up to 24 hours later.
      registration.update().catch(() => {});

      // And check again whenever the app is brought back to the foreground.
      // On a phone the tab is rarely closed — it is backgrounded — so this is
      // the moment most updates will actually be noticed.
      document.addEventListener("visibilitychange", () => {
        if (document.visibilityState === "visible") {
          registration.update().catch(() => {});
        }
      });
    } catch (err) {
      // Not fatal: without a service worker the app simply loses offline
      // support and always fetches from the robot, which is correct anyway.
      console.warn("[sw] registration failed:", err.message);
    }
  });
}
