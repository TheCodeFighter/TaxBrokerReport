import { describe, expect, it } from "vitest";

import type { MessageCatalogue } from "../i18n/messages";
import { refusedText, runErrorText } from "./run_errors";

const catalogue: MessageCatalogue = {
  messages: {
    "input_file.too_large": {
      en: "The file is too large.",
      sl: "Datoteka je prevelika.",
      params: [],
    },
  },
  terms: {},
};

describe("refusedText", () => {
  it("has a text for the codes the form can cause", () => {
    expect(refusedText("no_broker_file")).toBe("runErrorNoBrokerFile");
    expect(refusedText("too_many_files")).toBe("runErrorTooManyFiles");
    expect(refusedText("unsupported_broker")).toBe("runErrorBroker");
  });

  it("gives the general text for every other code, also for a name of Object", () => {
    expect(refusedText("bad_multipart")).toBe("runErrorGeneral");
    expect(refusedText("toString")).toBe("runErrorGeneral");
  });
});

describe("runErrorText", () => {
  const fault = { kind: "ecb_rate_file", messageKey: "input_file.too_large", messageParams: {} };

  it("names the file and the row of a rate file the server could not read", () => {
    const outcome = { kind: "inputFault" as const, fault: { ...fault, source: { file: "rates.csv", row: 3 } } };

    expect(runErrorText(outcome, "en", catalogue).text).toBe(
      "A file cannot be read: rates.csv, row 3. The file is too large.",
    );
    expect(runErrorText(outcome, "sl", catalogue).text).toBe(
      "Datoteke ni mogoče prebrati: rates.csv, vrstica 3. Datoteka je prevelika.",
    );
  });

  it("names only the file when the fault has no row", () => {
    const outcome = { kind: "inputFault" as const, fault: { ...fault, source: { file: "rates.csv" } } };

    expect(runErrorText(outcome, "en", catalogue).text).toBe(
      "A file cannot be read: rates.csv. The file is too large.",
    );
  });

  it("leaves out the name when the fault has no source", () => {
    const outcome = { kind: "inputFault" as const, fault };

    expect(runErrorText(outcome, "en", catalogue).text).toBe("A file cannot be read. The file is too large.");
  });

  it("marks the input of a refused taxpayer field", () => {
    const outcome = { kind: "refused" as const, code: "bad_taxpayer", field: "taxNumber" };

    expect(runErrorText(outcome, "en", undefined)).toEqual({
      text: "Enter the eight digits of the tax number.",
      field: { field: "taxNumber", text: "Enter the eight digits of the tax number." },
    });
  });

  it("gives the limit of 32 files and of 100 MB", () => {
    expect(runErrorText({ kind: "refused", code: "too_many_files" }, "en", undefined).text).toBe(
      "There are too many files. At most 32 at a time.",
    );
    expect(runErrorText({ kind: "tooLarge" }, "en", undefined).text).toBe(
      "A file is too large. One file can be at most 100 MB.",
    );
    expect(runErrorText({ kind: "tooLarge", fileName: "big.csv" }, "en", undefined).text).toBe(
      "The file big.csv is too large. One file can be at most 100 MB.",
    );
  });

  it("has a plain text for an unexpected answer", () => {
    expect(runErrorText({ kind: "unexpected" }, "en", undefined).text).toBe(
      "The server gave an unexpected answer. Try again.",
    );
  });
});
