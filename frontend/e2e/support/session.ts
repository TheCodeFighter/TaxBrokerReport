import { mkdir } from "node:fs/promises";
import { join } from "node:path";

import type { Browser, BrowserContext } from "playwright-core";
import { afterAll, afterEach, beforeAll, beforeEach } from "vitest";

import { launchBrowser } from "./browser";
import { loginContext } from "./report_page";
import { startServer, type RunningServer, type StartOptions } from "./server";

/** What a scenario works with: its own server, and a browser profile that is logged in to it. */
export interface Session {
  browser: Browser;
  server: RunningServer;
  context: BrowserContext;
}

/**
 * Registers the hooks of a test file: one Chromium for the file, and for every scenario a new
 * server with its own empty folders and a new login. The server keeps the answers of a session
 * for the next run, so a scenario that starts from a new session starts from a clean state.
 * `options.native` starts the server like a menu start (it stops when the last page closes).
 * A failed scenario leaves a screenshot of each open page in `TBR_E2E_SCREENSHOTS`, if set.
 */
export function useSession(options: StartOptions = { native: false }): Session {
  const session = {} as Session;

  beforeAll(async () => {
    session.browser = await launchBrowser();
  });

  afterAll(async () => {
    // Unset when Chromium failed to start: the start error must stay visible.
    await session.browser?.close();
  });

  beforeEach(async () => {
    session.server = await startServer(options);
    session.context = await loginContext(session.browser, session.server);
  });

  afterEach(async ({ task }) => {
    const folder = process.env.TBR_E2E_SCREENSHOTS;

    // After a failed start the fields can still be unset; the start error must stay visible, and
    // a failed screenshot must not leave the server running.
    try {
      if (task.result?.state === "fail" && folder !== undefined) {
        await mkdir(folder, { recursive: true });

        const pages = session.context?.pages() ?? [];

        for (const [index, page] of pages.entries()) {
          const name = task.name.replace(/[^A-Za-z0-9]+/g, "_").slice(0, 60);

          await page.screenshot({ path: join(folder, `${name}_${index}.png`), fullPage: true });
        }
      }
    } finally {
      await session.context?.close();
      await session.server?.stop();
    }
  });

  return session;
}
