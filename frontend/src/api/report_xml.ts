import type { ApiClient } from "./client";
import { fileNameOf, type SavedFile } from "./saved_file";

/** The answer of the server: the XML file, or the reason there is none. */
export type ReportXmlAnswer = { file: SavedFile } | { problem: "stale" | "failed" };

/**
 * Asks the server for the XML of one form of the shown run. A 409 means the run changed since the
 * result was shown; any other refusal is a failure. A lost session still throws from `apiFetch`,
 * so the page ends it. The file keeps the name of the server, or `<form>.xml` without one.
 */
export async function loadReportXml(
  apiFetch: ApiClient["apiFetch"],
  key: string,
  form: string,
  revision: number,
): Promise<ReportXmlAnswer> {
  const response = await apiFetch(`/api/run/reports/${key}/xml?revision=${revision}`);

  if (response.status === 409) {
    return { problem: "stale" };
  }

  if (!response.ok) {
    return { problem: "failed" };
  }

  return { file: { blob: await response.blob(), fileName: fileNameOf(response, `${form}.xml`) } };
}
