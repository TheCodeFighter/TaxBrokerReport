import type { ApiClient } from "./client";
import { fileNameOf, type SavedFile } from "./saved_file";

const FALLBACK_FILE_NAME = "taxbroker-support.json";

/** The answer of the server: the file, or the reason there is none. */
export type SupportReportAnswer = { file: SavedFile } | { problem: "no_run" | "failed" };

/**
 * Asks the server for the support report of the run. A 404 means no run exists yet; any other
 * refusal is a failure. A lost session still throws from `apiFetch`, so the page ends it.
 */
export async function loadSupportReport(apiFetch: ApiClient["apiFetch"]): Promise<SupportReportAnswer> {
  const response = await apiFetch("/api/run/support-report");

  if (response.status === 404) {
    return { problem: "no_run" };
  }

  if (!response.ok) {
    return { problem: "failed" };
  }

  return { file: { blob: await response.blob(), fileName: fileNameOf(response, FALLBACK_FILE_NAME) } };
}
