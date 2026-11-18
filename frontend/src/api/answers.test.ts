import { describe, expect, it, vi } from "vitest";

import { deleteDecisions, loadCurrentRun, sendAnswers } from "./answers";

function json(status: number, body: unknown): Response {
  return new Response(JSON.stringify(body), { status, headers: { "Content-Type": "application/json" } });
}

describe("sendAnswers", () => {
  it("posts the revision and the answers and gives the new result", async () => {
    const apiFetch = vi.fn().mockResolvedValue(json(200, { status: "success", runRevision: 3 }));

    const outcome = await sendAnswers(apiFetch, 2, [{ requestId: "request-0001", answer: "no_other_activity" }]);

    expect(apiFetch).toHaveBeenCalledTimes(1);
    expect(apiFetch).toHaveBeenCalledWith("/api/run/answers", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: '{"runRevision":2,"answers":[{"requestId":"request-0001","answer":"no_other_activity"}]}',
    });
    expect(outcome).toEqual({ kind: "result", result: { status: "success", runRevision: 3 } });
  });

  it("posts a ratio answer as an object", async () => {
    const apiFetch = vi.fn().mockResolvedValue(json(200, { status: "success", runRevision: 3 }));

    await sendAnswers(apiFetch, 2, [
      {
        requestId: "request-0001",
        answer: { newShares: 2, oldShares: 1, effectiveDate: "2023-01-15", confirmedUnits: "2.00000000" },
      },
    ]);

    const init = apiFetch.mock.calls[0]?.[1] as { body: string };

    expect(init.body).toBe(
      '{"runRevision":2,"answers":[{"requestId":"request-0001","answer":{"newShares":2,"oldShares":1,"effectiveDate":"2023-01-15","confirmedUnits":"2.00000000"}}]}',
    );
  });

  it("gives every rejection of a 422 with its index and key", async () => {
    const rejections = [
      { index: 0, messageKey: "answer_rejected.ratio_not_positive", messageParams: { requestId: "request-0001", field: "newShares" } },
      { index: 0, messageKey: "answer_rejected.ratio_unchanged", messageParams: { requestId: "request-0001" } },
    ];
    const apiFetch = vi.fn().mockResolvedValue(json(422, { rejections }));

    const outcome = await sendAnswers(apiFetch, 2, [{ requestId: "request-0001", answer: "cancel" }]);

    expect(outcome).toEqual({ kind: "rejected", rejections });
  });

  it("says stale for a stale revision", async () => {
    const rejections = [
      { index: 0, messageKey: "answer_rejected.stale_revision", messageParams: { requestId: "request-0001" } },
    ];
    const apiFetch = vi.fn().mockResolvedValue(json(422, { rejections }));

    expect(await sendAnswers(apiFetch, 2, [{ requestId: "request-0001", answer: "cancel" }])).toEqual({ kind: "stale" });
  });

  it("says stale for a question that is no longer open", async () => {
    const rejections = [
      { index: 0, messageKey: "answer_rejected.unknown_request", messageParams: { requestId: "request-0009" } },
    ];
    const apiFetch = vi.fn().mockResolvedValue(json(422, { rejections }));

    expect(await sendAnswers(apiFetch, 2, [{ requestId: "request-0009", answer: "cancel" }])).toEqual({ kind: "stale" });
  });

  it("says there is no run for a 404", async () => {
    const apiFetch = vi.fn().mockResolvedValue(json(404, { error: "no_run" }));

    expect(await sendAnswers(apiFetch, 2, [{ requestId: "request-0001", answer: "cancel" }])).toEqual({ kind: "noRun" });
  });

  it("says it failed for a 500, a 400 and a 422 without rejections", async () => {
    const answers = [{ requestId: "request-0001", answer: "cancel" }];

    expect(await sendAnswers(vi.fn().mockResolvedValue(json(500, {})), 2, answers)).toEqual({ kind: "failed" });
    expect(await sendAnswers(vi.fn().mockResolvedValue(json(400, { error: "bad_request" })), 2, answers)).toEqual({
      kind: "failed",
    });
    expect(await sendAnswers(vi.fn().mockResolvedValue(json(422, {})), 2, answers)).toEqual({ kind: "failed" });
    expect(await sendAnswers(vi.fn().mockResolvedValue(new Response("not json", { status: 422 })), 2, answers)).toEqual({
      kind: "failed",
    });
  });
});

describe("deleteDecisions", () => {
  it("posts the IDs of the answers to delete", async () => {
    const apiFetch = vi.fn().mockResolvedValue(json(200, { status: "needs_input", runRevision: 4 }));

    const outcome = await deleteDecisions(apiFetch, 3, ["decision-0001", "decision-0002"]);

    expect(apiFetch).toHaveBeenCalledWith("/api/run/decisions/delete", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: '{"runRevision":3,"decisionIds":["decision-0001","decision-0002"]}',
    });
    expect(outcome).toEqual({ kind: "result", result: { status: "needs_input", runRevision: 4 } });
  });

  it("posts all for the delete of every answer", async () => {
    const apiFetch = vi.fn().mockResolvedValue(json(200, { status: "needs_input", runRevision: 4 }));

    await deleteDecisions(apiFetch, 3, "all");

    const init = apiFetch.mock.calls[0]?.[1] as { body: string };

    expect(init.body).toBe('{"runRevision":3,"all":true}');
  });

  it("says stale for a delete of an earlier revision", async () => {
    const rejections = [
      { index: 0, messageKey: "answer_rejected.stale_revision_delete", messageParams: { decisionId: "decision-0001" } },
    ];
    const apiFetch = vi.fn().mockResolvedValue(json(422, { rejections }));

    expect(await deleteDecisions(apiFetch, 3, ["decision-0001"])).toEqual({ kind: "stale" });
  });

  it("says stale for an answer that is not there any more", async () => {
    const rejections = [
      { index: 0, messageKey: "answer_rejected.unknown_decision", messageParams: { decisionId: "decision-0009" } },
    ];
    const apiFetch = vi.fn().mockResolvedValue(json(422, { rejections }));

    expect(await deleteDecisions(apiFetch, 3, ["decision-0009"])).toEqual({ kind: "stale" });
  });

  it("gives the other rejections", async () => {
    const rejections = [
      { index: 0, messageKey: "answer_rejected.duplicate_delete", messageParams: { decisionId: "decision-0001" } },
    ];
    const apiFetch = vi.fn().mockResolvedValue(json(422, { rejections }));

    expect(await deleteDecisions(apiFetch, 3, ["decision-0001", "decision-0001"])).toEqual({
      kind: "rejected",
      rejections,
    });
  });
});

describe("loadCurrentRun", () => {
  it("gives the current result of the server", async () => {
    const apiFetch = vi.fn().mockResolvedValue(json(200, { status: "needs_input", runRevision: 5 }));

    const outcome = await loadCurrentRun(apiFetch);

    expect(apiFetch).toHaveBeenCalledWith("/api/run");
    expect(outcome).toEqual({ kind: "result", result: { status: "needs_input", runRevision: 5 } });
  });

  it("says there is no run for a 404", async () => {
    expect(await loadCurrentRun(vi.fn().mockResolvedValue(json(404, { error: "no_run" })))).toEqual({ kind: "noRun" });
  });
});
