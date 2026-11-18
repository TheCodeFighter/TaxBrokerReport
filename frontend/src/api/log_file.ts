import type { ApiClient } from "./client";
import { fileNameOf, type SupportReportFile } from "./support_report";

const FALLBACK_FILE_NAME = "taxbroker-log.txt";

/** The answer of the server: the log as a file, or the reason there is none. */
export type LogFileAnswer = { file: SupportReportFile } | { problem: "no_log" | "failed" };

/**
 * Asks the server for the log of this computer. A 404 means there is no log yet; any other
 * refusal is a failure. A lost session still throws from `apiFetch`, so the page ends it.
 */
export async function loadLogFile(apiFetch: ApiClient["apiFetch"]): Promise<LogFileAnswer> {
  const response = await apiFetch("/api/log");

  if (response.status === 404) {
    return { problem: "no_log" };
  }

  if (!response.ok) {
    return { problem: "failed" };
  }

  return { file: { blob: await response.blob(), fileName: fileNameOf(response, FALLBACK_FILE_NAME) } };
}
