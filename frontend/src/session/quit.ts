import { SessionEndedError, type ApiClient } from "../api/client";

/** What the quit call did. */
export type QuitOutcome = "stopped" | "failed" | "ended";

/**
 * Asks the server to stop (`POST /api/quit`, no body). "stopped": the server said it is stopping.
 * "failed": it answered something else. "ended": the session ended during the call (a 401, or
 * the server was already gone); the client has already told the page.
 */
export async function requestQuit(apiFetch: ApiClient["apiFetch"]): Promise<QuitOutcome> {
  try {
    const response = await apiFetch("/api/quit", { method: "POST" });

    return response.ok ? "stopped" : "failed";
  } catch (error) {
    if (error instanceof SessionEndedError) {
      return "ended";
    }

    throw error;
  }
}

/**
 * Closes this tab after the server has stopped. A browser only closes a tab that a script
 * opened or that has no history, so this may do nothing; the last screen then tells the user to
 * close the tab.
 */
export function closeThisTab(closeWindow: () => void): void {
  try {
    closeWindow();
  } catch {
    // A refused close is not an error: the last screen stays.
  }
}
