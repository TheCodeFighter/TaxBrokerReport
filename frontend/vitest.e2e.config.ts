import { defineConfig } from "vitest/config";

// The browser tests (scripts/e2e.sh). `npm test` uses vite.config.ts and never starts a browser.
export default defineConfig({
  test: {
    environment: "node",
    include: ["e2e/**/*.e2e.ts"],
    testTimeout: 60_000,
    hookTimeout: 30_000,
    // One server and one Chromium per file, one file after the other.
    fileParallelism: false,
  },
});
