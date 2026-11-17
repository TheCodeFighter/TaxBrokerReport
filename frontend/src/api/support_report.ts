import type { ApiClient } from "./client";

/** The support report as a file the user can save. */
export interface SupportReportFile {
  blob: Blob;
  fileName: string;
}

const FALLBACK_FILE_NAME = "taxbroker-support.json";

/** The file name of `attachment; filename="taxbroker-support-2024.json"`, or the fallback name. */
function fileNameOf(response: Response): string {
  const header = response.headers.get("Content-Disposition") ?? "";
  const match = /filename="([A-Za-z0-9._-]+)"/.exec(header);

  return match?.[1] ?? FALLBACK_FILE_NAME;
}

/** The answer of the server: the file, or the reason there is none. */
export type SupportReportAnswer = { file: SupportReportFile } | { problem: "no_run" | "failed" };

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

  return { file: { blob: await response.blob(), fileName: fileNameOf(response) } };
}

/** Saves the file through a temporary link: the browser shows its own save dialog or folder. */
export function saveFile(file: SupportReportFile): void {
  const url = URL.createObjectURL(file.blob);
  const link = document.createElement("a");

  link.href = url;
  link.download = file.fileName;
  document.body.appendChild(link);
  link.click();
  link.remove();
  URL.revokeObjectURL(url);
}
