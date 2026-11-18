import { describe, expect, it, vi } from "vitest";

import { cancelDeveloperExport, confirmDeveloperExport, nextDeveloperStep } from "./developer_export";
import type { DecisionRequest } from "./questions";
import type { RunResult } from "./submit";

function json(status: number, body: unknown): Response {
  return new Response(JSON.stringify(body), { status, headers: { "Content-Type": "application/json" } });
}

function step(id: string, number: number, isins: string[]): DecisionRequest {
  return {
    id,
    type: "developer_export_confirmation_required",
    runRevision: 0,
    affectedReports: ["capital_gains"],
    sources: [],
    step: number,
    options: ["confirm"],
    investments: isins.map((isin, index) => ({
      name: `Synthetic Stock ${String.fromCharCode(65 + index)}`,
      isin,
      reasonCodes: [],
    })),
  };
}

function result(runRevision: number, requests: DecisionRequest[], unsafe = false): RunResult {
  return {
    status: "success",
    runRevision,
    reports: { capitalGains: { status: unsafe ? "generated" : "needs_input", unsafe } },
    diagnostics: [],
    excludedItems: [],
    requests,
    decisions: [],
    corrections: [],
    reconciliation: [],
  } as unknown as RunResult;
}

describe("nextDeveloperStep", () => {
  it("gives the open step that lists the same ISINs", () => {
    const next = nextDeveloperStep(result(4, [step("request-0003", 2, ["DE000SYN0001"])]), ["DE000SYN0001"]);

    expect(next?.id).toBe("request-0003");
    expect(next?.step).toBe(2);
  });

  it("gives nothing when the list of ISINs differs", () => {
    const changed = result(4, [step("request-0003", 2, ["DE000SYN0001", "DE000SYN0002"])]);

    expect(nextDeveloperStep(changed, ["DE000SYN0001"])).toBeUndefined();
  });

  it("gives nothing when no confirmation step is open", () => {
    expect(nextDeveloperStep(result(6, [], true), ["DE000SYN0001"])).toBeUndefined();
  });
});

describe("confirmDeveloperExport", () => {
  const first = step("request-0002", 1, ["DE000SYN0001"]);

  it("confirms the three steps one after the other and gives the final result", async () => {
    const final = result(6, [], true);
    const apiFetch = vi
      .fn()
      .mockResolvedValueOnce(json(200, result(4, [step("request-0003", 2, ["DE000SYN0001"])])))
      .mockResolvedValueOnce(json(200, result(5, [step("request-0004", 3, ["DE000SYN0001"])])))
      .mockResolvedValueOnce(json(200, final));

    const outcome = await confirmDeveloperExport(apiFetch, 3, first);

    expect(apiFetch.mock.calls.map((call) => (call[1] as { body: string }).body)).toEqual([
      '{"runRevision":3,"answers":[{"requestId":"request-0002","answer":"confirm"}]}',
      '{"runRevision":4,"answers":[{"requestId":"request-0003","answer":"confirm"}]}',
      '{"runRevision":5,"answers":[{"requestId":"request-0004","answer":"confirm"}]}',
    ]);
    expect(outcome).toEqual({ kind: "result", result: final });
  });

  it("stops after a stale answer and sends no third call", async () => {
    const rejections = [
      { index: 0, messageKey: "answer_rejected.stale_revision", messageParams: { requestId: "request-0003" } },
    ];
    const apiFetch = vi
      .fn()
      .mockResolvedValueOnce(json(200, result(4, [step("request-0003", 2, ["DE000SYN0001"])])))
      .mockResolvedValueOnce(json(422, { rejections }));

    const outcome = await confirmDeveloperExport(apiFetch, 3, first);

    expect(apiFetch).toHaveBeenCalledTimes(2);
    expect(outcome).toEqual({ kind: "stale" });
  });

  it("stops and gives the result when the next step lists other ISINs", async () => {
    const changed = result(4, [step("request-0003", 2, ["DE000SYN0001", "DE000SYN0002"])]);
    const apiFetch = vi.fn().mockResolvedValueOnce(json(200, changed));

    const outcome = await confirmDeveloperExport(apiFetch, 3, first);

    expect(apiFetch).toHaveBeenCalledTimes(1);
    expect(outcome).toEqual({ kind: "result", result: changed });
  });

  it("stops and gives the rejection of the first call", async () => {
    const rejections = [{ index: 0, messageKey: "answer_rejected.unsupported_answer", messageParams: {} }];
    const apiFetch = vi.fn().mockResolvedValueOnce(json(422, { rejections }));

    expect(await confirmDeveloperExport(apiFetch, 3, first)).toEqual({ kind: "rejected", rejections });
    expect(apiFetch).toHaveBeenCalledTimes(1);
  });

  it("continues from step 2 when the page was reloaded in the middle", async () => {
    const second = step("request-0003", 2, ["DE000SYN0001"]);
    const final = result(5, [], true);
    const apiFetch = vi
      .fn()
      .mockResolvedValueOnce(json(200, result(4, [step("request-0004", 3, ["DE000SYN0001"])])))
      .mockResolvedValueOnce(json(200, final));

    const outcome = await confirmDeveloperExport(apiFetch, 3, second);

    expect(apiFetch).toHaveBeenCalledTimes(2);
    expect(outcome).toEqual({ kind: "result", result: final });
  });

  it("never sends more than three confirmations", async () => {
    const third = step("request-0004", 3, ["DE000SYN0001"]);
    const apiFetch = vi.fn().mockResolvedValueOnce(json(200, result(5, [step("request-0005", 4, ["DE000SYN0001"])])));

    await confirmDeveloperExport(apiFetch, 4, third);

    expect(apiFetch).toHaveBeenCalledTimes(1);
  });
});

describe("cancelDeveloperExport", () => {
  it("sends the cancel answer of the open step and nothing else", async () => {
    const apiFetch = vi.fn().mockResolvedValueOnce(json(200, result(4, [])));

    await cancelDeveloperExport(apiFetch, 3, step("request-0002", 1, ["DE000SYN0001"]));

    expect(apiFetch).toHaveBeenCalledTimes(1);
    expect((apiFetch.mock.calls[0]?.[1] as { body: string }).body).toBe(
      '{"runRevision":3,"answers":[{"requestId":"request-0002","answer":"cancel"}]}',
    );
  });
});
