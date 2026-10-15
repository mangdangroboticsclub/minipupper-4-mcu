/* MPX-Dog Service Worker
 *
 * ── THE BUG THIS REPLACES ──────────────────────────────────────────────────
 *
 * The previous version was cache-first with no revalidation:
 *
 *     const cached = await caches.match(request);
 *     if (cached) return cached;          // forever
 *
 * Once /m.js was cached it was served for the rest of the installation's life.
 * The request never reached the network, so the firmware's correct ETag and
 * Cache-Control: no-cache headers were never consulted. Flashing new firmware
 * changed the bytes on the device and changed nothing in the browser, and the
 * only fix was clearing browsing data — which works only because it deletes
 * this cache.
 *
 * The cache name made it worse: a hardcoded "mpx-dog-v2" that no build step
 * ever bumped, so the activate handler's cleanup (delete every cache whose
 * name !== CACHE) never deleted anything.
 *
 * ── THE STRATEGY NOW ───────────────────────────────────────────────────────
 *
 * Different content gets different treatment, because "cache everything the
 * same way" is what caused the problem:
 *
 *   APP SHELL (index.html, m.js, index.css)  →  NETWORK-FIRST
 *       This is the code. It must be correct before it is fast. The robot is
 *       on the same LAN and the firmware sends an ETag, so the usual cost is
 *       a 304 with an empty body — a few milliseconds. Cache is the fallback
 *       when the robot is unreachable, which keeps the app usable offline.
 *
 *   OTHER ASSETS (icons, manifest)           →  STALE-WHILE-REVALIDATE
 *       Served instantly from cache, refreshed in the background. These
 *       change rarely and a one-load delay in noticing is harmless.
 *
 *   API (/v1/*) and WebSockets               →  NETWORK ONLY
 *       Never cached. A cached robot status is a lie.
 *
 * Correctness no longer depends on anyone remembering to bump a version
 * string. CACHE_VERSION below exists only to garbage-collect old caches; if it
 * is never touched again, the app still updates correctly.
 */

const CACHE_VERSION = "v3";
const CACHE = `mpx-dog-${CACHE_VERSION}`;

/**
 * The application itself. Anything here is checked against the network on
 * every load, so a firmware update takes effect on the next page load.
 */
const APP_SHELL = new Set(["/", "/index.html", "/m.js", "/index.css"]);

/** Warmed on install so the very first offline load works. */
const PRECACHE_URLS = [
  "/",
  "/index.html",
  "/m.js",
  "/index.css",
  "/manifest.json",
  "/eye-open.svg",
];

/* ── Install ─────────────────────────────────────────────────────────────
 *
 * Individually rather than cache.addAll(): addAll is atomic, so one missing
 * icon fails the whole install and leaves the app with no offline copy at all.
 */
self.addEventListener("install", (event) => {
  event.waitUntil(
    (async () => {
      const cache = await caches.open(CACHE);
      for (const url of PRECACHE_URLS) {
        try {
          await cache.add(new Request(url, { cache: "reload" }));
        } catch {
          console.warn("[sw] could not precache", url);
        }
      }
    })(),
  );
  // Take over immediately. Waiting for every tab to close means a user who
  // never fully quits the app never receives an update.
  self.skipWaiting();
});

/* ── Activate ──────────────────────────────────────────────────────────── */
self.addEventListener("activate", (event) => {
  event.waitUntil(
    (async () => {
      const keys = await caches.keys();
      await Promise.all(
        keys.filter((k) => k !== CACHE).map((k) => caches.delete(k)),
      );
      await self.clients.claim();
    })(),
  );
});

/**
 * Fetch from the network and update the cache. Returns null if unreachable.
 */
async function networkThenCache(request) {
  try {
    const response = await fetch(request);
    if (response && response.ok) {
      const cache = await caches.open(CACHE);
      cache.put(request, response.clone());
    }
    return response;
  } catch {
    return null;
  }
}

/* ── Fetch ─────────────────────────────────────────────────────────────── */
self.addEventListener("fetch", (event) => {
  const { request } = event;

  // Only GET is cacheable. A POST that went through this path would be
  // replayed from cache, which for /v1/skills would silently repeat a write.
  if (request.method !== "GET") return;

  let url;
  try {
    url = new URL(request.url);
  } catch {
    return;
  }

  // Never touch anything from another origin — the marketplace proxy and any
  // CDN request must go straight to the network.
  if (url.origin !== self.location.origin) return;

  // Live data: never cached.
  if (url.pathname.startsWith("/v1/") || url.pathname.startsWith("/studio")) {
    return;
  }

  const isShell = APP_SHELL.has(url.pathname) || request.mode === "navigate";

  if (isShell) {
    // ── NETWORK-FIRST ────────────────────────────────────────────────
    event.respondWith(
      (async () => {
        const fresh = await networkThenCache(request);
        if (fresh) return fresh;

        // Offline. Serve the cached copy; for a navigation, fall back to the
        // cached shell so a deep link still boots the app.
        const cached =
          (await caches.match(request)) ||
          (request.mode === "navigate" ? await caches.match("/index.html") : null);

        return (
          cached ||
          new Response("Robot unreachable", {
            status: 503,
            headers: { "Content-Type": "text/plain" },
          })
        );
      })(),
    );
    return;
  }

  // ── STALE-WHILE-REVALIDATE ─────────────────────────────────────────
  event.respondWith(
    (async () => {
      const cached = await caches.match(request);
      // Kick off the refresh either way; do not await it when we have a hit.
      const refresh = networkThenCache(request);
      if (cached) {
        event.waitUntil(refresh);
        return cached;
      }
      const fresh = await refresh;
      return (
        fresh ||
        new Response("Robot unreachable", {
          status: 503,
          headers: { "Content-Type": "text/plain" },
        })
      );
    })(),
  );
});

/* ── Manual update trigger ───────────────────────────────────────────────
 *
 * Lets the app force a check ("Check for updates" in settings) without the
 * user having to know what a service worker is.
 */
self.addEventListener("message", (event) => {
  if (event.data === "SKIP_WAITING") self.skipWaiting();
});
