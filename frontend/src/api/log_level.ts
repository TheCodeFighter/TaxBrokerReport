import type { ApiClient } from "./client";

/** The four levels of the log that the user can choose; `error` is the default. */
export type LogLevel = "error" | "warn" | "info" | "debug";

export const LOG_LEVELS: readonly LogLevel[] = ["error", "warn", "info", "debug"];

const JSON_HEADERS = { "Content-Type": "application/json" };

/** The level of an answer `{"level":"info"}`, or nothing when the answer is not that. */
async function levelOf(response: Response): Promise<LogLevel | undefined> {
  if (!response.ok) {
    return undefined;
  }

  try {
    const body = (await response.json()) as { level?: unknown };

    return LOG_LEVELS.find((level) => level === body.level);
  } catch {
    return undefined;
  }
}

/**
 * The level the server logs at now, or nothing when the answer is not the expected one. A lost
 * session still throws from `apiFetch`, so the page ends it.
 */
export async function loadLogLevel(apiFetch: ApiClient["apiFetch"]): Promise<LogLevel | undefined> {
  return levelOf(await apiFetch("/api/log-level"));
}

/** Sets the level for the rest of the server session; gives the level the server now has, or nothing when it refused. */
export async function changeLogLevel(
  apiFetch: ApiClient["apiFetch"],
  level: LogLevel,
): Promise<LogLevel | undefined> {
  return levelOf(
    await apiFetch("/api/log-level", { method: "POST", headers: JSON_HEADERS, body: JSON.stringify({ level }) }),
  );
}
