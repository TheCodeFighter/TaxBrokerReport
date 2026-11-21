import { chromium, type Browser, type BrowserContext } from "playwright-core";

/** Chromium of the e2e image (the Alpine package). */
const CHROMIUM_PATH = "/usr/bin/chromium-browser";

/**
 * Starts Chromium. `--no-sandbox` is needed because the container has no user namespaces for the
 * sandbox; the browser only opens the local test server and nothing else.
 */
export function launchBrowser(): Promise<Browser> {
  return chromium.launch({
    executablePath: CHROMIUM_PATH,
    args: ["--no-sandbox", "--disable-dev-shm-usage"],
  });
}

/** A fresh browser profile that accepts downloads and runs in the default (Slovenian) language. */
export function newContext(browser: Browser): Promise<BrowserContext> {
  return browser.newContext({ acceptDownloads: true, locale: "sl-SI" });
}
