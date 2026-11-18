import { describe, expect, it, vi } from "vitest";

import { createApiClient, SessionEndedError, type FetchFunction } from "../api/client";
import { emptyInputs } from "./inputs";
import { MAX_FILE_BYTES } from "./limits";
import { submitRun } from "./submit";

function json(status: number, body: unknown): Response {
  return new Response(JSON.stringify(body), { status, headers: { "Content-Type": "application/json" } });
}

function inputs() {
  const prepared = emptyInputs(new Date(2026, 10, 16));

  prepared.brokerFiles.trade_republic = [new File(["a"], "a.csv")];
  prepared.taxpayer.taxNumber = "12345679";

  return prepared;
}

function clientFor(fetchFunction: ReturnType<typeof vi.fn>, onEnded = () => undefined) {
  return createApiClient(fetchFunction as unknown as FetchFunction, onEnded);
}

describe("submitRun", () => {
  it("returns the result of a 200", async () => {
    const result = { status: "success", runRevision: 1, reports: {}, diagnostics: [], excludedItems: [] };
    const fetchFunction = vi.fn().mockResolvedValue(json(200, result));

    await expect(submitRun(clientFor(fetchFunction).apiFetch, inputs())).resolves.toEqual({
      kind: "result",
      result,
    });
  });

  it("sends a FormData with the client header and no Content-Type of its own", async () => {
    const fetchFunction = vi.fn().mockResolvedValue(json(200, { reports: {}, diagnostics: [], excludedItems: [] }));

    await submitRun(clientFor(fetchFunction).apiFetch, inputs());

    const [path, init] = fetchFunction.mock.calls[0] as [string, RequestInit];

    expect(path).toBe("/api/run");
    expect(init.method).toBe("POST");
    expect(init.body).toBeInstanceOf(FormData);
    expect(init.headers).toEqual({ "X-TaxBroker": "1" });
  });

  it("gives the code of a refused request", async () => {
    const fetchFunction = vi.fn().mockResolvedValue(json(400, { error: "no_broker_file" }));

    await expect(submitRun(clientFor(fetchFunction).apiFetch, inputs())).resolves.toEqual({
      kind: "refused",
      code: "no_broker_file",
    });
  });

  it("gives the field of a refused taxpayer", async () => {
    const fetchFunction = vi.fn().mockResolvedValue(json(400, { error: "bad_taxpayer", field: "birthDate" }));

    await expect(submitRun(clientFor(fetchFunction).apiFetch, inputs())).resolves.toEqual({
      kind: "refused",
      code: "bad_taxpayer",
      field: "birthDate",
    });
  });

  it("sorts a 413 as too large", async () => {
    const fetchFunction = vi.fn().mockResolvedValue(json(413, { error: "file_too_large" }));

    await expect(submitRun(clientFor(fetchFunction).apiFetch, inputs())).resolves.toEqual({ kind: "tooLarge" });
  });

  it("gives the input fault of a 422 with its file and row", async () => {
    const fault = {
      kind: "ecb_rate_file",
      messageKey: "input_file.too_large",
      messageParams: {},
      source: { file: "rates.csv", row: 3 },
    };
    const fetchFunction = vi.fn().mockResolvedValue(json(422, { inputFault: fault }));

    await expect(submitRun(clientFor(fetchFunction).apiFetch, inputs())).resolves.toEqual({
      kind: "inputFault",
      fault,
    });
  });

  it("gives unexpected for a 500 without a body", async () => {
    const fetchFunction = vi.fn().mockResolvedValue(new Response(null, { status: 500 }));

    await expect(submitRun(clientFor(fetchFunction).apiFetch, inputs())).resolves.toEqual({ kind: "unexpected" });
  });

  it("refuses a file over 100 MiB before it sends anything", async () => {
    const fetchFunction = vi.fn();
    const prepared = inputs();
    const big = new File(["a"], "big.csv");

    Object.defineProperty(big, "size", { value: MAX_FILE_BYTES + 1 });
    prepared.brokerFiles.trade_republic = [big];

    await expect(submitRun(clientFor(fetchFunction).apiFetch, prepared)).resolves.toEqual({
      kind: "tooLarge",
      fileName: "big.csv",
    });
    expect(fetchFunction).not.toHaveBeenCalled();
  });

  it("accepts a file of exactly 100 MiB", async () => {
    const fetchFunction = vi.fn().mockResolvedValue(json(200, { reports: {}, diagnostics: [], excludedItems: [] }));
    const prepared = inputs();
    const exact = new File(["a"], "exact.csv");

    Object.defineProperty(exact, "size", { value: MAX_FILE_BYTES });
    prepared.brokerFiles.trade_republic = [exact];

    await submitRun(clientFor(fetchFunction).apiFetch, prepared);

    expect(fetchFunction).toHaveBeenCalledTimes(1);
  });

  it("refuses 33 files before it sends anything", async () => {
    const fetchFunction = vi.fn();
    const prepared = inputs();

    prepared.brokerFiles.trade_republic = Array.from({ length: 33 }, (_unused, index) => new File(["a"], `${index}.csv`));

    await expect(submitRun(clientFor(fetchFunction).apiFetch, prepared)).resolves.toEqual({
      kind: "refused",
      code: "too_many_files",
    });
    expect(fetchFunction).not.toHaveBeenCalled();
  });

  it("accepts 32 files", async () => {
    const fetchFunction = vi.fn().mockResolvedValue(json(200, { reports: {}, diagnostics: [], excludedItems: [] }));
    const prepared = inputs();

    prepared.brokerFiles.trade_republic = Array.from({ length: 32 }, (_unused, index) => new File(["a"], `${index}.csv`));

    await submitRun(clientFor(fetchFunction).apiFetch, prepared);

    expect(fetchFunction).toHaveBeenCalledTimes(1);
  });

  it("throws SessionEndedError, not a run error, when the server cannot be reached", async () => {
    const fetchFunction = vi.fn().mockRejectedValue(new TypeError("network error"));
    const onEnded = vi.fn();

    await expect(submitRun(clientFor(fetchFunction, onEnded).apiFetch, inputs())).rejects.toBeInstanceOf(
      SessionEndedError,
    );
    expect(onEnded).toHaveBeenCalledTimes(1);
  });

  it("sends only the run when the details are not to be saved", async () => {
    const fetchFunction = vi.fn().mockResolvedValue(json(200, { reports: {}, diagnostics: [], excludedItems: [] }));

    const outcome = await submitRun(clientFor(fetchFunction).apiFetch, inputs(), false);

    expect(fetchFunction.mock.calls.map((call) => call[0])).toEqual(["/api/run"]);
    expect(outcome).toEqual({ kind: "result", result: { reports: {}, diagnostics: [], excludedItems: [] } });
  });

  it("never saves the developer mode with the details", async () => {
    const fetchFunction = vi
      .fn()
      .mockResolvedValueOnce(new Response(null, { status: 204 }))
      .mockResolvedValueOnce(json(200, { reports: {}, diagnostics: [], excludedItems: [] }));
    const developer = inputs();

    developer.developerMode = true;
    await submitRun(clientFor(fetchFunction).apiFetch, developer, true);

    const [, profileInit] = fetchFunction.mock.calls[0] as [string, RequestInit];
    const [, runInit] = fetchFunction.mock.calls[1] as [string, RequestInit];

    expect(profileInit.body).toBe('{"taxNumber":"12345679","isResident":true}');
    expect(JSON.parse((runInit.body as FormData).get("request") as string).developerMode).toBe(true);
  });

  it("saves the details first and then sends the run", async () => {
    const fetchFunction = vi
      .fn()
      .mockResolvedValueOnce(new Response(null, { status: 204 }))
      .mockResolvedValueOnce(json(200, { reports: {}, diagnostics: [], excludedItems: [] }));

    const outcome = await submitRun(clientFor(fetchFunction).apiFetch, inputs(), true);

    expect(fetchFunction.mock.calls.map((call) => call[0])).toEqual(["/api/profile", "/api/run"]);

    const [, profileInit] = fetchFunction.mock.calls[0] as [string, RequestInit];

    // The taxpayer object of the run: the tax number and the residence.
    expect(profileInit.body).toBe('{"taxNumber":"12345679","isResident":true}');
    expect(outcome).toEqual({ kind: "result", result: { reports: {}, diagnostics: [], excludedItems: [] }, profile: "saved" });
  });

  it("stops before the run when the server refuses a taxpayer field", async () => {
    const fetchFunction = vi.fn().mockResolvedValueOnce(json(400, { error: "bad_taxpayer", field: "taxNumber" }));

    await expect(submitRun(clientFor(fetchFunction).apiFetch, inputs(), true)).resolves.toEqual({
      kind: "refused",
      code: "bad_taxpayer",
      field: "taxNumber",
    });
    expect(fetchFunction.mock.calls.map((call) => call[0])).toEqual(["/api/profile"]);
  });

  it("still sends the run when the details could not be saved", async () => {
    const fetchFunction = vi
      .fn()
      .mockResolvedValueOnce(json(500, { error: "profile_not_saved" }))
      .mockResolvedValueOnce(json(200, { reports: {}, diagnostics: [], excludedItems: [] }));

    const outcome = await submitRun(clientFor(fetchFunction).apiFetch, inputs(), true);

    expect(fetchFunction.mock.calls.map((call) => call[0])).toEqual(["/api/profile", "/api/run"]);
    expect(outcome).toEqual({ kind: "result", result: { reports: {}, diagnostics: [], excludedItems: [] }, profile: "not_saved" });
  });

  it("keeps the saved outcome when the run is refused after the save", async () => {
    const inputFault = { kind: "rate_file", messageKey: "k", messageParams: {} };
    const fetchFunction = vi
      .fn()
      .mockResolvedValueOnce(new Response(null, { status: 204 }))
      .mockResolvedValueOnce(json(422, { inputFault }));

    const outcome = await submitRun(clientFor(fetchFunction).apiFetch, inputs(), true);

    expect(outcome).toEqual({ kind: "inputFault", fault: inputFault, profile: "saved" });
  });

  it("keeps the not-saved outcome when the run is refused after a failed save", async () => {
    const fetchFunction = vi
      .fn()
      .mockResolvedValueOnce(json(500, { error: "profile_not_saved" }))
      .mockResolvedValueOnce(json(400, { error: "bad_year" }));

    const outcome = await submitRun(clientFor(fetchFunction).apiFetch, inputs(), true);

    expect(outcome).toEqual({ kind: "refused", code: "bad_year", profile: "not_saved" });
  });

  it("saves nothing when a file is over the limit", async () => {
    const fetchFunction = vi.fn();
    const prepared = inputs();
    const big = new File(["a"], "big.csv");

    Object.defineProperty(big, "size", { value: MAX_FILE_BYTES + 1 });
    prepared.brokerFiles.trade_republic = [big];

    await submitRun(clientFor(fetchFunction).apiFetch, prepared, true);

    expect(fetchFunction).not.toHaveBeenCalled();
  });
});
