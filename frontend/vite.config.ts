import { defineConfig } from "vitest/config";

// The server embeds exactly four files with fixed names (no hash), so the build writes
// index.html, assets/app.js, assets/app.css and assets/logo.webp. The content policy of the
// server allows no inline script or style, so the module-preload polyfill (inline code) is off
// and every image is a file (assetsInlineLimit 0).
export default defineConfig({
  esbuild: { jsx: "automatic", jsxImportSource: "preact" },
  build: {
    outDir: "../build/frontend/dist",
    emptyOutDir: true,
    assetsInlineLimit: 0,
    cssCodeSplit: false,
    sourcemap: false,
    modulePreload: { polyfill: false },
    rollupOptions: {
      output: {
        entryFileNames: "assets/app.js",
        chunkFileNames: "assets/[name].js",
        assetFileNames: (asset) =>
          (asset.names ?? []).some((name) => name.endsWith(".css"))
            ? "assets/app.css"
            : "assets/[name][extname]",
      },
    },
  },
  test: {
    environment: "node",
    include: ["src/**/*.test.ts"],
  },
});
