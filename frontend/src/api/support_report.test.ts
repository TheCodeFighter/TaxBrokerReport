import { describe, expect, it, vi } from "vitest";

import { loadSupportReport } from "./support_report";

describe("loadSupportReport", () => {
  it("asks once for the report and gives its text and the file name of the server", async () => {
    const apiFetch = vi.fn().mockResolvedValue(
      new Response('{"supportReportVersion":1}\n', {
        status: 200,
        headers: { "Content-Disposition": 'attachment; filename="taxbroker-support-2024.json"' },
      }),
    );

    const answer = await loadSupportReport(apiFetch);

    expect(apiFetch).toHaveBeenCalledTimes(1);
    expect(apiFetch).toHaveBeenCalledWith("/api/run/support-report");
    expect("file" in answer && answer.file.fileName).toBe("taxbroker-support-2024.json");
    expect("file" in answer && (await answer.file.blob.text())).toBe('{"supportReportVersion":1}\n');
  });

  it("uses a fixed file name when the header is missing or is not a plain name", async () => {
    const noHeader = vi.fn().mockResolvedValue(new Response("{}", { status: 200 }));
    const oddName = vi.fn().mockResolvedValue(
      new Response("{}", { status: 200, headers: { "Content-Disposition": 'attachment; filename="../x y.json"' } }),
    );

    const first = await loadSupportReport(noHeader);
    const second = await loadSupportReport(oddName);

    expect("file" in first && first.file.fileName).toBe("taxbroker-support.json");
    expect("file" in second && second.file.fileName).toBe("taxbroker-support.json");
  });

  it("says there is no run for a 404", async () => {
    const apiFetch = vi.fn().mockResolvedValue(new Response('{"error":"no_run"}', { status: 404 }));

    expect(await loadSupportReport(apiFetch)).toEqual({ problem: "no_run" });
  });

  it("says it failed for any other refusal", async () => {
    const apiFetch = vi.fn().mockResolvedValue(new Response("", { status: 500 }));

    expect(await loadSupportReport(apiFetch)).toEqual({ problem: "failed" });
  });
});
