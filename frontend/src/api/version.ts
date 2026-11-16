import type { ApiClient } from "./client";

/**
 * The version of the running server (`GET /api/version`), or nothing when the answer is not
 * the expected one. A lost session still throws from `apiFetch`, so the page ends it.
 */
export async function loadVersion(apiFetch: ApiClient["apiFetch"]): Promise<string | undefined> {
  const response = await apiFetch("/api/version");

  if (!response.ok) {
    return undefined;
  }

  try {
    const body = (await response.json()) as { version?: unknown };

    return typeof body.version === "string" && body.version !== "" ? body.version : undefined;
  } catch {
    return undefined;
  }
}
