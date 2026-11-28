import type { ApiClient } from "../api/client";

/** A new random id for this page load: 32 hex characters from the browser's random source. */
export function makeTabId(randomValues: (bytes: Uint8Array) => Uint8Array): string {
  const bytes = randomValues(new Uint8Array(16));

  return Array.from(bytes, (byte) => byte.toString(16).padStart(2, "0")).join("");
}

export interface TabReporter {
  /** Says the page is open (after the login, and when the page is shown again). */
  open(): void;
  /** Says the page is closed (the page is hidden for good). The call survives the unload. */
  close(): void;
}

/**
 * Tells the server when this page is open or closed (`POST /api/tab`), so the server can stop
 * when the last page is closed. It uses `send`, not `apiFetch`: a lost server or session here
 * changes nothing on the page, and the message of a closing page has no one to read an error.
 */
export function createTabReporter(send: ApiClient["send"], id: string): TabReporter {
  const report = (open: boolean) => {
    send("/api/tab", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ id, open }),
      keepalive: true,
    }).catch(() => undefined);
  };

  return { open: () => report(true), close: () => report(false) };
}
