import type { ApiClient } from "./client";

/** The answer of `GET /api/profile`; the server leaves out what it does not know. */
export interface ProfileJson {
  /** A profile file exists (also when it cannot be read). */
  saved: boolean;
  /** The place of the file without the user name; left out when it cannot be given. */
  location?: string;
  taxpayer?: Record<string, unknown>;
  /** Keys whose saved value is not valid. */
  invalidFields?: string[];
  /** The file held a key that the application does not know. */
  ignoredKeys?: boolean;
  unreadable?: boolean;
}

/** What `POST /api/profile` did. */
export type SaveProfileOutcome =
  | { kind: "saved" }
  /** A 400: the taxpayer key the server refused, when it names one. */
  | { kind: "refused"; field?: string }
  /** Any other answer, for example a 500: the file could not be written. */
  | { kind: "failed" };

const JSON_HEADERS = { "Content-Type": "application/json" };

/**
 * The saved profile, or nothing when the answer is not the expected one (then the page shows the
 * empty form). A lost session still throws from `apiFetch`, so the page ends it.
 */
export async function loadProfile(apiFetch: ApiClient["apiFetch"]): Promise<ProfileJson | undefined> {
  const response = await apiFetch("/api/profile");

  if (!response.ok) {
    return undefined;
  }

  try {
    const body = (await response.json()) as ProfileJson;

    return typeof body.saved === "boolean" ? body : undefined;
  } catch {
    return undefined;
  }
}

/** Saves the taxpayer object of the form (the one the run sends). */
export async function saveProfile(
  apiFetch: ApiClient["apiFetch"],
  taxpayer: Record<string, string | boolean>,
): Promise<SaveProfileOutcome> {
  const response = await apiFetch("/api/profile", {
    method: "POST",
    headers: JSON_HEADERS,
    body: JSON.stringify(taxpayer),
  });

  if (response.status === 204) {
    return { kind: "saved" };
  }

  if (response.status === 400) {
    try {
      const body = (await response.json()) as { field?: unknown };

      return typeof body.field === "string" ? { kind: "refused", field: body.field } : { kind: "refused" };
    } catch {
      return { kind: "refused" };
    }
  }

  return { kind: "failed" };
}

/** Deletes the saved profile. True when the server did it (also when there was no file). */
export async function deleteProfile(apiFetch: ApiClient["apiFetch"]): Promise<boolean> {
  const response = await apiFetch("/api/profile/delete", { method: "POST", headers: JSON_HEADERS, body: "" });

  return response.status === 200;
}
