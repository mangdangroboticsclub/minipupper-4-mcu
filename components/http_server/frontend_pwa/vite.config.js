import { defineConfig } from "vite";
import { svelte } from "@sveltejs/vite-plugin-svelte";

export default defineConfig({
  plugins: [svelte()],

  root: "src",
  publicDir: "../public",
  base: "/",

  build: {
    outDir: "../www",
    emptyOutDir: true,
    minify: "terser",
    terserOptions: {
      compress: {
        drop_console: false,
        drop_debugger: true,
      },
    },
    rollupOptions: {
      output: {
        entryFileNames: "m.js",
        chunkFileNames: "m-[hash].js",
        assetFileNames: (assetInfo) => {
          if (assetInfo.name?.endsWith(".css")) return "index.css";
          if (assetInfo.name?.endsWith(".svg")) return "[name][extname]";
          return "[name][extname]";
        },
      },
    },
  },

  // The dev server has no robot behind it, so every /v1/... call would 404 and
  // Servo Studio would sit there saying "offline". Forward the API and the
  // telemetry/chat WebSockets to the real robot instead.
  //
  //   npm run dev                          -> talks to 192.168.2.1 (the AP)
  //   ROBOT=192.168.1.42 npm run dev       -> talks to it over your LAN
  //
  // On Windows cmd use:  set ROBOT=192.168.1.42 && npm run dev
  server: {
    host: "0.0.0.0",
    port: 5173,
    strictPort: true,
    proxy: {
      "/v1": {
        target: `http://${process.env.ROBOT || "192.168.2.1"}`,
        changeOrigin: true,
        ws: true,
      },
      // The standalone tuning page is served by the firmware, not by vite.
      "/studio": {
        target: `http://${process.env.ROBOT || "192.168.2.1"}`,
        changeOrigin: true,
      },
    },
  },

  preview: {
    host: "0.0.0.0",
    port: 8080,
    strictPort: true,
  },
});
