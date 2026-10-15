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

  server: {
    host: "0.0.0.0",
    port: 5173,
    strictPort: true,
    // Forward robot API + WebSocket calls to a live robot during development.
    // Override the target with:  ROBOT_HOST=192.168.x.x npm run dev
    proxy: {
      "/v1": {
        target: `http://${process.env.ROBOT_HOST || "192.168.1.102"}`,
        changeOrigin: true,
        ws: true,
      },
    },
  },

  preview: {
    host: "0.0.0.0",
    port: 8080,
    strictPort: true,
  },
});
