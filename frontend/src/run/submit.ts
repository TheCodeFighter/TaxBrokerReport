import type { ApiClient } from "../api/client";
import type { MessageParams } from "../i18n/messages";
import { MAX_FILE_BYTES, MAX_FILES } from "./limits";
import { orderedBrokerFiles, type RunInputs } from "./inputs";
import { buildRunRequest } from "./request";

/** The part of the result JSON (schema 2) that the input screen shows. */
export interface RunResult {
  status: string;
  runRevision: number;
  reports: Record<string, { status: string }>;
  diagnostics: { severity: string }[];
}

/** A rate or reference file that the server could not read (the 422 `inputFault`). */
export interface InputFault {
  kind: string;
  messageKey: string;
  messageParams: MessageParams;
  source?: { file?: string; row?: number };
}

export type SubmitOutcome =
  | { kind: "result"; result: RunResult }
  /** A 400: the stable `code`, and for `bad_taxpayer` the `field` of the taxpayer object. */
  | { kind: "refused"; code: string; field?: string }
  /** A file over the limit (checked before sending: `fileName`) or a 413 of the server. */
  | { kind: "tooLarge"; fileName?: string }
  | { kind: "inputFault"; fault: InputFault }
  /** Any other answer, for example a 500 of the server. */
  | { kind: "unexpected" };

function allFiles(inputs: RunInputs): File[] {
  return [...orderedBrokerFiles(inputs).map((entry) => entry.file), ...inputs.rateFiles, ...inputs.referenceFiles];
}

async function readJson(response: Response): Promise<Record<string, unknown>> {
  try {
    return (await response.json()) as Record<string, unknown>;
  } catch {
    return {};
  }
}

/**
 * Sends the run and sorts the answer. The count and size of the files are checked first, so a
 * refused large body never reaches the network. A lost session (401, no server) throws
 * `SessionEndedError` from the client; the page already shows its screen.
 */
export async function submitRun(apiFetch: ApiClient["apiFetch"], inputs: RunInputs): Promise<SubmitOutcome> {
  const files = allFiles(inputs);

  if (files.length > MAX_FILES) {
    return { kind: "refused", code: "too_many_files" };
  }

  const tooLarge = files.find((file) => file.size > MAX_FILE_BYTES);

  if (tooLarge !== undefined) {
    return { kind: "tooLarge", fileName: tooLarge.name };
  }

  const response = await apiFetch("/api/run", { method: "POST", body: buildRunRequest(inputs) });

  if (response.status === 200) {
    return { kind: "result", result: (await response.json()) as RunResult };
  }

  const body = await readJson(response);

  if (response.status === 413) {
    return { kind: "tooLarge" };
  }

  if (response.status === 422 && typeof body.inputFault === "object" && body.inputFault !== null) {
    return { kind: "inputFault", fault: body.inputFault as InputFault };
  }

  if (response.status === 400 && typeof body.error === "string") {
    return {
      kind: "refused",
      code: body.error,
      ...(typeof body.field === "string" ? { field: body.field } : {}),
    };
  }

  return { kind: "unexpected" };
}
